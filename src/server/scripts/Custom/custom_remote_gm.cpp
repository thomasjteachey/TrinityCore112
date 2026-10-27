/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Remote GM commands: what a GM does in game to the player they have selected,
// done instead to a player named on the command line, so the console and SOAP
// (which have no selection) can do it too (owner, 2026-10-27).
//
//   .remote auras <name>                   the player's auras, passives left out
//   .remote unaura <name> <spell|debuffs>  remove one spell's auras, or every debuff
//   .remote aura <name> <spell>            apply an aura (online only)
//   .remote cooldown <name> [spell]        clear one cooldown, or all of them
//
// Online, each acts on the player in the world. Console and SOAP commands run on
// the world thread after the map threads have finished (World::Update), which is
// what makes that safe; the same command typed in game would run on the GM's
// map thread and reach into another map's player, so these are console-only.
// Offline, auras and cooldowns are read from and deleted in character_aura and
// character_spell_cooldown, which the player loads at the next login.
//
// "debuffs" leaves passives and equipment auras alone: those come back only with
// a relog or a re-equip, and nobody asking for a debuff to go wants them gone.

#include "Chat.h"
#include "ChatCommand.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <set>
#include <string>
#include <vector>

using namespace Trinity::ChatCommands;

namespace
{
    // "71h 40m left", "12m 5s left", "permanent".
    std::string Remaining(int32 ms)
    {
        if (ms < 0)
            return "permanent";

        uint32 const s = uint32(ms) / IN_MILLISECONDS;
        if (s >= HOUR)
            return Trinity::StringFormat("{}h {}m left", s / HOUR, (s % HOUR) / MINUTE);
        if (s >= MINUTE)
            return Trinity::StringFormat("{}m {}s left", s / MINUTE, s % MINUTE);
        return Trinity::StringFormat("{}s left", s);
    }

    std::string SpellLabel(ChatHandler* handler, uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        char const* name = info ? info->SpellName[handler->GetSessionDbcLocale()] : nullptr;
        return Trinity::StringFormat("{} {}", spellId, (name && *name) ? name : "(unknown spell)");
    }

    std::string Who(PlayerIdentifier const& player)
    {
        return Trinity::StringFormat("{} ({})", player.GetName(), player.IsConnected() ? "online" : "offline");
    }

    bool Fail(ChatHandler* handler, std::string const& text)
    {
        handler->SendSysMessage(text);
        handler->SetSentErrorMessage(true);
        return false;
    }

    bool ConsoleOnly(ChatHandler* handler)
    {
        if (handler->IsConsole())
            return true;
        return Fail(handler, "The .remote commands are for the console and SOAP. In game, select the player and use .unaura, .aura or .cooldown.");
    }
}

class custom_remote_gm_commands : public CommandScript
{
public:
    custom_remote_gm_commands() : CommandScript("custom_remote_gm_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable remoteTable =
        {
            { "auras",    HandleAuras,    rbac::RBAC_PERM_COMMAND_GM,       Console::Yes },
            { "unaura",   HandleUnaura,   rbac::RBAC_PERM_COMMAND_UNAURA,   Console::Yes },
            { "aura",     HandleAura,     rbac::RBAC_PERM_COMMAND_AURA,     Console::Yes },
            { "cooldown", HandleCooldown, rbac::RBAC_PERM_COMMAND_COOLDOWN, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "remote", remoteTable },
        };
        return commandTable;
    }

    static bool HandleAuras(ChatHandler* handler, PlayerIdentifier player)
    {
        if (!ConsoleOnly(handler))
            return false;

        std::vector<std::string> lines;
        if (Player* target = player.GetConnectedPlayer())
        {
            for (auto const& [spellId, app] : target->GetAppliedAuras())
            {
                Aura const* aura = app->GetBase();
                if (aura->IsPassive())
                    continue;

                std::string line = Trinity::StringFormat("  {} - {}", SpellLabel(handler, spellId), app->IsPositive() ? "buff" : "debuff");
                if (aura->GetStackAmount() > 1)
                    line += Trinity::StringFormat(" x{}", aura->GetStackAmount());
                if (!aura->GetCastItemGUID().IsEmpty())
                    line += ", from an item";
                line += ", " + Remaining(aura->GetDuration());
                lines.push_back(std::move(line));
            }
        }
        else if (QueryResult result = CharacterDatabase.Query(Trinity::StringFormat(
                     "SELECT spell, stackCount, remainTime, itemGuid FROM character_aura WHERE guid = {}",
                     player.GetGUID().GetCounter()).c_str()))
        {
            do
            {
                Field* f = result->Fetch();
                uint32 const spellId = f[0].GetUInt32();
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);

                std::string line = Trinity::StringFormat("  {} - {}", SpellLabel(handler, spellId), (info && info->IsPositive()) ? "buff" : "debuff");
                if (f[1].GetUInt8() > 1)
                    line += Trinity::StringFormat(" x{}", f[1].GetUInt8());
                if (f[3].GetUInt64() != 0)
                    line += ", from an item";
                line += ", " + Remaining(f[2].GetInt32());
                lines.push_back(std::move(line));
            } while (result->NextRow());
        }

        handler->SendSysMessage(Trinity::StringFormat("{}: {} aura(s), passives not shown.", Who(player), lines.size()));
        for (std::string const& line : lines)
            handler->SendSysMessage(line);
        return true;
    }

    static bool HandleUnaura(ChatHandler* handler, PlayerIdentifier player, Variant<SpellInfo const*, EXACT_SEQUENCE("debuffs")> what)
    {
        if (!ConsoleOnly(handler))
            return false;

        bool const debuffs = what.holds_alternative<EXACT_SEQUENCE("debuffs")>();
        uint32 onlySpell = 0;
        if (!debuffs)
        {
            SpellInfo const* spell = what.get<SpellInfo const*>();
            if (!spell)
                return Fail(handler, "No such spell.");
            onlySpell = spell->Id;
        }

        std::set<uint32> removed;
        if (Player* target = player.GetConnectedPlayer())
        {
            // Collected first: removing one aura can remove or apply others,
            // which would invalidate an iterator over the map being walked.
            for (auto const& [spellId, app] : target->GetAppliedAuras())
            {
                Aura const* aura = app->GetBase();
                bool const wanted = debuffs
                    ? (!app->IsPositive() && !aura->IsPassive() && aura->GetCastItemGUID().IsEmpty())
                    : spellId == onlySpell;
                if (wanted)
                    removed.insert(spellId);
            }

            for (uint32 spellId : removed)
                target->RemoveAurasDueToSpell(spellId);
        }
        else
        {
            ObjectGuid::LowType const guid = player.GetGUID().GetCounter();
            if (QueryResult result = CharacterDatabase.Query(Trinity::StringFormat(
                    "SELECT spell, itemGuid FROM character_aura WHERE guid = {}", guid).c_str()))
            {
                do
                {
                    Field* f = result->Fetch();
                    uint32 const spellId = f[0].GetUInt32();
                    if (debuffs)
                    {
                        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                        if (!info || info->IsPositive() || f[1].GetUInt64() != 0)
                            continue;
                    }
                    else if (spellId != onlySpell)
                        continue;
                    removed.insert(spellId);
                } while (result->NextRow());
            }

            for (uint32 spellId : removed)
                CharacterDatabase.DirectExecute(Trinity::StringFormat(
                    "DELETE FROM character_aura WHERE guid = {} AND spell = {}", guid, spellId).c_str());
        }

        if (removed.empty())
        {
            handler->SendSysMessage(Trinity::StringFormat("{}: nothing to remove.", Who(player)));
            return true;
        }

        handler->SendSysMessage(Trinity::StringFormat("{}: removed {} spell(s):", Who(player), removed.size()));
        for (uint32 spellId : removed)
            handler->SendSysMessage("  " + SpellLabel(handler, spellId));
        return true;
    }

    static bool HandleAura(ChatHandler* handler, PlayerIdentifier player, SpellInfo const* spell)
    {
        if (!ConsoleOnly(handler))
            return false;
        if (!spell)
            return Fail(handler, "No such spell.");

        Player* target = player.GetConnectedPlayer();
        if (!target)
            return Fail(handler, Trinity::StringFormat("{} is offline; an aura can only be applied to a player in the world.", player.GetName()));

        AuraCreateInfo createInfo(spell, MAX_EFFECT_MASK, target);
        createInfo.SetCaster(target);
        Aura::TryRefreshStackOrCreate(createInfo);

        handler->SendSysMessage(Trinity::StringFormat("{}: applied {}.", Who(player), SpellLabel(handler, spell->Id)));
        return true;
    }

    static bool HandleCooldown(ChatHandler* handler, PlayerIdentifier player, Optional<SpellInfo const*> spell)
    {
        if (!ConsoleOnly(handler))
            return false;

        SpellInfo const* only = spell ? *spell : nullptr;
        if (spell && !only)
            return Fail(handler, "No such spell.");

        if (Player* target = player.GetConnectedPlayer())
        {
            if (only)
                target->GetSpellHistory()->ResetCooldown(only->Id, true);
            else
                target->GetSpellHistory()->ResetAllCooldowns();
        }
        else
        {
            std::string sql = Trinity::StringFormat("DELETE FROM character_spell_cooldown WHERE guid = {}", player.GetGUID().GetCounter());
            if (only)
                sql += Trinity::StringFormat(" AND spell = {}", only->Id);
            CharacterDatabase.DirectExecute(sql.c_str());
        }

        handler->SendSysMessage(Trinity::StringFormat("{}: cleared {}.", Who(player),
            only ? "the cooldown of " + SpellLabel(handler, only->Id) : std::string("every cooldown")));
        return true;
    }
};

void AddSC_custom_remote_gm()
{
    new custom_remote_gm_commands();
}
