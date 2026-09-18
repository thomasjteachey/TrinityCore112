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

// World-mode vs tournament-mode characters: the per-player upkeep and the GM
// commands. The rules themselves are enforced in core, where each interaction
// happens, through Miscellaneous/TournamentMode.h.
//
//   .tournament info [$player]
//   .tournament set $player world|tournament
//   .tournament queue $player on|off
//   .tournament item $item        - the world/tournament pair an item belongs to
//   .tournament reloaditems       - re-read `item_tournament_link` and the loadout data
//   .tournament loadout [$player] [restore] - the battleground loadout a character
//                                   is wearing, and a way to hand its gear back

#include "AccountBankMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "custom_bounty.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "Miscellaneous/TournamentMode.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "Util.h"
#include "WorldSession.h"

using namespace Trinity::ChatCommands;

class tournament_mode_player_script : public PlayerScript
{
public:
    tournament_mode_player_script() : PlayerScript("tournament_mode_player_script") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        Tournament::ApplyCharacterKit(player);

        // A character that logged out, was disconnected or was cut off by a
        // restart in the middle of a tournament match is still wearing the
        // loadout. Give it its own gear back before it does anything with it.
        Tournament::RestoreLoadoutAfterLogin(player);
    }

    // Confinement is checked continuously rather than on zone change alone, so
    // it also catches a login outside the grounds, a `.reload config` that
    // shrank the zone list and any teleport that slipped past the checks. The
    // test is two set lookups and returns at once for everyone else.
    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        // The reagent waiver (PvP for every character, always for tournament
        // characters) has to be visible to the client, which checks reagents
        // before it sends a cast. Follow it as it comes and goes - battleground
        // or arena entry and exit, duel start and end, a mode change, and
        // InitStatsForLevel zeroing the field on a level-up. Two field reads when
        // nothing changed.
        // The battleground loadout's last step: a client that was mid-world-port
        // when its own gear came back is told again, now that it has arrived.
        // One relaxed atomic read when nobody is waiting.
        Tournament::RefreshLoadoutVisuals(player);

        bool const waived = Tournament::HasReagentWaiver(player);
        bool const shown = player->GetUInt32Value(PLAYER_NO_REAGENT_COST_1) == 0xFFFFFFFF;
        if (waived != shown)
            player->UpdateNoReagentCostMask();

        // The tournament phase follows the mode through `.tournament set`,
        // `.reload config` and anything else that recomputed the phase without
        // Player::SetPhaseMask's help. One mask test when nothing changed.
        if (Tournament::IsPhaseStale(player))
            Tournament::RefreshPhase(player);

        if (!Tournament::HasConfinement() || !player->HasTournamentModeFlag())
            return;

        WorldSession const* session = player->GetSession();
        if (!session || session->PlayerLoading() || player->IsBeingTeleported())
            return;

        Tournament::EnforceConfinement(player);
    }

    // Bots ignore tournament characters, so neither side can pull the other into
    // a party - which is also what keeps companion bots away from them.
    bool OnCanGroupInvite(Player* player, std::string& memberName) override
    {
        if (!Tournament::IsEnabled() || !player)
            return true;

        Player* target = ObjectAccessor::FindPlayerByName(memberName);
        if (!target)
            return true;

        bool const playerIsBot = playerbot::IsManagedRandomBot(player);
        if (playerIsBot == playerbot::IsManagedRandomBot(target))
            return true;

        if (!Tournament::IsTournamentCharacter(playerIsBot ? target : player))
            return true;

        if (!playerIsBot)
            ChatHandler(player->GetSession()).PSendSysMessage("%s does not group with tournament characters.", target->GetName().c_str());
        return false;
    }
};

class tournament_mode_commandscript : public CommandScript
{
public:
    tournament_mode_commandscript() : CommandScript("tournament_mode_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable tournamentTable =
        {
            { "info",        HandleInfo,        rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "set",         HandleSet,         rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "queue",       HandleQueue,       rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "item",        HandleItem,        rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "reloaditems", HandleReloadItems, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "loadout",     HandleLoadout,     rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "tournament", tournamentTable },
        };
        return commandTable;
    }

    static bool HandleInfo(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);
        if (!target)
            return false;

        bool tournament = false;
        bool queue = false;
        Player const* online = target->GetConnectedPlayer();
        if (online)
        {
            tournament = online->HasTournamentModeFlag();
            queue = online->HasTournamentQueueFlag();
        }
        else if (QueryResult result = CharacterDatabase.PQuery("SELECT extra_flags FROM characters WHERE guid = {}", target->GetGUID().GetCounter()))
        {
            uint16 const flags = (*result)[0].GetUInt16();
            tournament = (flags & PLAYER_EXTRA_TOURNAMENT_MODE) != 0;
            queue = (flags & PLAYER_EXTRA_TOURNAMENT_QUEUE) != 0;
        }
        else
        {
            handler->PSendSysMessage("tournament: no character named %s.", target->GetName().c_str());
            return true;
        }

        // The stored opt-in stops deciding anything once the level rule takes
        // over, so say which of the two put them in the tournament queue.
        char const* queueNote = "";
        if (!tournament)
        {
            if (online && Tournament::QueuesInTournamentPool(online))
                queueNote = Tournament::GetQueueLockReason(online) == Tournament::QUEUE_LOCK_FORCED
                    ? ", held in the tournament queue by its level"
                    : ", opted into the tournament queue";
            else if (!online && queue)
                queueNote = ", opted into the tournament queue";
        }

        handler->PSendSysMessage("%s: %s character%s%s.", target->GetName().c_str(),
            tournament ? "tournament" : "world", queueNote,
            Tournament::IsEnabled() ? "" : " (Centurion.Tournament.Enable is off: no rules apply)");
        return true;
    }

    static bool HandleSet(ChatHandler* handler, PlayerIdentifier target, std::string_view modeArg)
    {
        bool tournament;
        if (StringEqualI(modeArg, "tournament"))
            tournament = true;
        else if (StringEqualI(modeArg, "world"))
            tournament = false;
        else
        {
            handler->SendSysMessage("tournament: expected world or tournament.");
            return true;
        }

        ObjectGuid const guid = target.GetGUID();
        if (Player* player = target.GetConnectedPlayer())
        {
            // The two banks share one session slot; flipping the flag under an
            // open bank would save it into the other bank's rows.
            if (AccountBank::IsAccountBankOpen(player))
                AccountBank::CloseAccountBank(player);

            player->SetTournamentModeFlag(tournament);
            if (tournament)
            {
                player->SetTournamentQueueFlag(false);
                // Bounties are a world-character affair; one carried across
                // would sit on a character nothing can hunt or pay out.
                Bounty::ClearBounty(player);
            }
            player->SaveToDB();

            if (tournament)
                Tournament::ApplyCharacterKit(player);
            Tournament::RefreshPhase(player);
        }
        else
        {
            uint32 const clear = PLAYER_EXTRA_TOURNAMENT_MODE | PLAYER_EXTRA_TOURNAMENT_QUEUE;
            CharacterDatabase.PExecute("UPDATE characters SET extra_flags = (extra_flags & {}) | {} WHERE guid = {}",
                uint32(0xFFFF & ~clear), tournament ? uint32(PLAYER_EXTRA_TOURNAMENT_MODE) : 0u, guid.GetCounter());
        }

        sCharacterCache->UpdateCharacterTournamentMode(guid, tournament);
        handler->PSendSysMessage("%s is now a %s character%s.", target.GetName().c_str(), tournament ? "tournament" : "world",
            target.IsConnected() ? "" : " (applies at their next login)");
        return true;
    }

    static bool HandleQueue(ChatHandler* handler, PlayerIdentifier target, bool on)
    {
        ObjectGuid const guid = target.GetGUID();
        if (Player* player = target.GetConnectedPlayer())
        {
            if (player->HasTournamentModeFlag())
            {
                handler->PSendSysMessage("%s is a tournament character and always queues with tournament characters.", target.GetName().c_str());
                return true;
            }

            player->SetTournamentQueueFlag(on);
            player->SaveToDB();
            Tournament::SendQueueState(player);

            // Set it anyway - it is what they go back to if the level rule is
            // ever lifted - but do not let a GM think it changed their queue.
            if (Tournament::GetQueueLockReason(player) == Tournament::QUEUE_LOCK_FORCED)
                handler->PSendSysMessage("%s is level %u and queues with tournament characters either way "
                    "(Centurion.Tournament.ForceQueueAtMinLevel); the flag is kept for if that rule is lifted.",
                    target.GetName().c_str(), player->GetLevel());
        }
        else
            CharacterDatabase.PExecute("UPDATE characters SET extra_flags = (extra_flags & {}) | {} WHERE guid = {}",
                uint32(0xFFFF & ~uint32(PLAYER_EXTRA_TOURNAMENT_QUEUE)), on ? uint32(PLAYER_EXTRA_TOURNAMENT_QUEUE) : 0u, guid.GetCounter());

        handler->PSendSysMessage("%s: tournament queue %s.", target.GetName().c_str(), on ? "on" : "off");
        return true;
    }

    // Reads the link both ways, so an item link or an id from either side
    // answers the same question: which item is this one in the other mode?
    static bool HandleItem(ChatHandler* handler, ItemTemplate const* item)
    {
        Optional<Tournament::ItemLink> const link = Tournament::GetItemLink(item->ItemId);
        if (!link)
        {
            handler->PSendSysMessage("%s (%u): a world-mode item with no tournament copy (%u tournament item(s) known).",
                item->Name1.c_str(), item->ItemId, Tournament::GetItemLinkCount());
            return true;
        }

        ItemTemplate const* tournamentItem = sObjectMgr->GetItemTemplate(link->TournamentEntry);
        ItemTemplate const* worldItem = link->WorldEntry ? sObjectMgr->GetItemTemplate(link->WorldEntry) : nullptr;

        if (!worldItem)
        {
            handler->PSendSysMessage("%s (%u): a tournament-only item - nothing stands for it in world mode.",
                tournamentItem ? tournamentItem->Name1.c_str() : "?", link->TournamentEntry);
            return true;
        }

        handler->PSendSysMessage("world %u \"%s\" (item level %u)", worldItem->ItemId, worldItem->Name1.c_str(), worldItem->ItemLevel);
        handler->PSendSysMessage("  <-> tournament %u \"%s\" (item level %u)%s", link->TournamentEntry,
            tournamentItem ? tournamentItem->Name1.c_str() : "?", tournamentItem ? tournamentItem->ItemLevel : 0,
            link->Crunched ? ", crunched" : "");
        return true;
    }

    static bool HandleReloadItems(ChatHandler* handler)
    {
        Tournament::LoadItemLinks();
        Tournament::LoadLoadoutData();
        handler->PSendSysMessage("`item_tournament_link` and `tournament_loadout_template` reloaded: %u tournament item(s) known.",
            Tournament::GetItemLinkCount());
        return true;
    }

    // What a character is fighting in, and the way out when something went
    // wrong: `restore` hands its own gear back on the spot.
    static bool HandleLoadout(ChatHandler* handler, Optional<PlayerIdentifier> target, Optional<std::string_view> action)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);
        if (!target)
            return false;

        Player* player = target->GetConnectedPlayer();
        if (!player)
        {
            handler->PSendSysMessage("%s is offline; the loadout is handed back at their next login.", target->GetName().c_str());
            return true;
        }

        bool const wearing = Tournament::HasBattlegroundLoadout(player);
        if (action && StringEqualI(*action, "restore"))
        {
            Tournament::RestoreBattlegroundLoadout(player);
            handler->PSendSysMessage("%s: loadout restored%s.", player->GetName().c_str(), wearing ? "" : " (nothing was recorded)");
            return true;
        }

        handler->PSendSysMessage("%s: %s a tournament loadout. Template pieces for class %u: %s.",
            player->GetName().c_str(), wearing ? "wearing" : "not wearing", uint32(player->GetClass()),
            Tournament::GetLoadoutTemplateItem(player->GetClass(), EQUIPMENT_SLOT_CHEST) ? "loaded" : "none");
        return true;
    }
};

void AddSC_custom_tournament_mode()
{
    new tournament_mode_player_script();
    new tournament_mode_commandscript();
}
