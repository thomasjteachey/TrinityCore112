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

// Mok'gora, the duel to the death - the script half.
//
// All of the rules live in game/Miscellaneous/Mokgora.cpp, because four core
// call sites need them (the chat handler, the damage clamp, the kill path and
// the logout). What is left here is everything that can only be reached from a
// script: the duel hooks, login and delete, the per-second sweep of expired
// challenges, and the one fact game/ cannot work out for itself - which of
// these characters is a playerbot.

#include "Chat.h"
#include "Miscellaneous/Mokgora.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "WorldSession.h"

#include <cmath>

#include "custom_barracks_hardcore.h"

using namespace Trinity::ChatCommands;

namespace
{
    // game/ has no way to recognise a bot - the account set is loaded by the
    // Hardcore ruleset, over here. Mok'gora asks this before it lets anyone be
    // challenged, because a bot cannot read a warning box and cannot consent to
    // dying in one.
    bool IsPlayerbotForMokgora(Player const* player)
    {
        return BarracksHardcore::IsPlayerbot(player);
    }
}

// Coward!'s stat reduction, turned from a percentage into a flat number.
//
// Only a FLAT SPELL_AURA_MOD_STAT writes UNIT_FIELD_NEGSTAT, and the character
// sheet prints a stat in red only when that field is negative
// (Unit::UpdateStatBuffMod feeds it; PaperDollFrame_SetStat reads it). A
// percentage aura - 80 MOD_PERCENT_STAT, which Resurrection Sickness uses, or
// 137 MOD_TOTAL_STAT_PERCENTAGE - only ever MULTIPLIES that field, and
// multiplying zero leaves zero. So a percentage debuff shrinks the green number
// and can never redden it, res sickness included.
//
// The percentage still lives in the DBC, as those effects' base points: this
// reads the -20 that arrives in `amount` AS a percent and replaces it with what
// that percent is worth on this character. One source of truth, and no config
// key to drift out of step with the spell data.
//
// Snapshot, deliberately: canBeRecalculated is left alone but the value is
// taken once, when the brand lands. Three days of gear changes will drift it,
// which is the price of the sheet turning red at all.
class spell_mokgora_coward : public AuraScript
{
    PrepareAuraScript(spell_mokgora_coward);

    void CalcFlatStat(AuraEffect const* aurEff, int32& amount, bool& /*canBeRecalculated*/)
    {
        // Registered for every effect, so ignore the ones that are not stats -
        // the same script covers the damage and resistance effects, which are
        // honest percentages and want no help.
        if (aurEff->GetAuraType() != SPELL_AURA_MOD_STAT)
            return;

        Unit* owner = GetUnitOwner();
        if (!owner)
            return;

        int32 const miscValue = aurEff->GetMiscValue();
        if (miscValue < 0 || miscValue >= MAX_STATS)
            return;

        float const percent = float(amount);                 // the DBC's -20
        float const current = owner->GetStat(Stats(miscValue));

        amount = int32(std::lround(current * percent / 100.0f));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_mokgora_coward::CalcFlatStat, EFFECT_ALL, SPELL_AURA_ANY);
    }
};

class custom_mokgora_world : public WorldScript
{
public:
    custom_mokgora_world() : WorldScript("custom_mokgora_world") { }

    void OnStartup() override
    {
        Mokgora::SetBotPredicate(&IsPlayerbotForMokgora);
    }

    // Challenges lapse on a clock, and a clock needs a tick. A second's
    // granularity is finer than anything the feature promises - the box counts
    // down in whole seconds - so the sweep is cheap and almost always finds an
    // empty list.
    void OnUpdate(uint32 diff) override
    {
        _sinceSweep += diff;
        if (_sinceSweep < 1000)
            return;

        _sinceSweep = 0;
        Mokgora::Update();
    }

private:
    uint32 _sinceSweep = 0;
};

class custom_mokgora_player : public PlayerScript
{
public:
    custom_mokgora_player() : PlayerScript("custom_mokgora_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        Mokgora::OnLogin(player);
    }

    // The countdown has run out and the two of them are free to swing.
    void OnDuelStart(Player* first, Player* second) override
    {
        Mokgora::OnDuelStart(first, second);
    }

    void OnDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        Mokgora::OnDuelEnd(winner, loser, type);
    }
};

// Why did that challenge not go through?
//
// Every gate in Mokgora::WhyNot answers with the one line the player needs and
// nothing else, which is right for them and useless for working out why a whole
// zone cannot fight. This prints the same answer for an arbitrary pair, so a GM
// can stand somewhere and ask about two people who are not themselves.
class custom_mokgora_commands : public CommandScript
{
public:
    custom_mokgora_commands() : CommandScript("custom_mokgora_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable mokgoraTable =
        {
            { "why", HandleMokgoraWhy, rbac::RBAC_PERM_COMMAND_GM, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "mokgora", mokgoraTable },
        };
        return commandTable;
    }

    // .mokgora why <challenger> <target>  - or one name, against yourself.
    static bool HandleMokgoraWhy(ChatHandler* handler, std::string first, Optional<std::string> second)
    {
        Player* challenger = nullptr;
        Player* target = nullptr;

        if (second && !second->empty())
        {
            challenger = ObjectAccessor::FindPlayerByName(first);
            target = ObjectAccessor::FindPlayerByName(*second);
        }
        else
        {
            challenger = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            target = ObjectAccessor::FindPlayerByName(first);
        }

        if (!challenger || !target)
        {
            handler->SendSysMessage("mokgora: one of those names is not online.");
            return true;
        }

        handler->PSendSysMessage("mokgora: enabled %s, challenge range %u yd, ring %u yd, coward spell %u.",
            Mokgora::IsEnabled() ? "yes" : "NO",
            uint32(Mokgora::ChallengeRange()),
            uint32(Mokgora::BoundsYards()),
            Mokgora::CowardSpell());

        handler->PSendSysMessage("  %s -> %s: %s",
            challenger->GetName().c_str(), target->GetName().c_str(),
            Mokgora::Explain(challenger, target).c_str());

        handler->PSendSysMessage("  %s: in a Mok'gora %s, offer pending %s | %s: in a Mok'gora %s, offer pending %s",
            challenger->GetName().c_str(),
            Mokgora::IsMokgoraDuel(challenger) ? "yes" : "no",
            Mokgora::HasPendingOffer(challenger) ? "yes" : "no",
            target->GetName().c_str(),
            Mokgora::IsMokgoraDuel(target) ? "yes" : "no",
            Mokgora::HasPendingOffer(target) ? "yes" : "no");

        return true;
    }
};

void AddSC_custom_mokgora()
{
    RegisterSpellScript(spell_mokgora_coward);
    new custom_mokgora_world();
    new custom_mokgora_player();
    new custom_mokgora_commands();
}
