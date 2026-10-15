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

// .info - the one chat command every player may type.
//
// Every other command is GM-only (ChatHandler::_ParseCommands turns a player
// account away before the command table is even searched), so this is let
// through by name there AND by its permission, RBAC_PERM_COMMAND_INFO, which
// the player role holds. Taking either away closes it again.
//
// It answers what the client cannot: how the realm is running, when things
// reset, and the realm's own rules as they apply to this character right now -
// the honor cap (nothing in the client shows one), honor paid as experience
// below the cap, War Mode's pause, bounty, contract and challenge modes.
// Always about the one typing it; a GM looking at somebody else has .pinfo.

#include "ChallengeModes.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "custom_barracks_hardcore.h"
#include "custom_bounty.h"
#include "custom_notoriety.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GitRevision.h"
#include "ItemTemplate.h"
#include "Map.h"
#include "MapManager.h"
#include "Miscellaneous/Surnames.h"
#include "Miscellaneous/TournamentMode.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TickStats.h"
#include "UpdateTime.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <ctime>
#include <string>
#include <vector>

using namespace Trinity::ChatCommands;

namespace
{
    // Two units at most: "2d 5h", "3h 12m", "4m". Seconds only under a minute.
    std::string Span(uint64 seconds)
    {
        uint64 const days = seconds / DAY;
        uint64 const hours = (seconds % DAY) / HOUR;
        uint64 const minutes = (seconds % HOUR) / MINUTE;
        if (days)
            return Trinity::StringFormat("{}d {}h", days, hours);
        if (hours)
            return Trinity::StringFormat("{}h {}m", hours, minutes);
        if (minutes)
            return Trinity::StringFormat("{}m", minutes);
        return Trinity::StringFormat("{}s", seconds);
    }

    // "x1", "x0.75", "x1.5".
    std::string Rate(float rate)
    {
        std::string text = Trinity::StringFormat("{:.2f}", rate);
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.')
            text.pop_back();
        return "x" + text;
    }

    char const* AreaName(uint32 areaId, LocaleConstant locale)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        if (!area)
            return nullptr;

        char const* name = area->AreaName[locale];
        if (!name || !*name)
            name = area->AreaName[LOCALE_enUS];
        return (name && *name) ? name : nullptr;
    }

    struct Headcount
    {
        uint32 players = 0;
        uint32 bots = 0;
    };

    // Counted off the maps rather than the session list: the bot fleet plays on
    // socketless sessions that sWorld does not keep. A GM who has gone invisible
    // is not on the realm as far as players are told. World thread only, which
    // is where chat commands run (CMSG_MESSAGECHAT is PROCESS_THREADUNSAFE).
    Headcount CountHeads()
    {
        Headcount count;
        sMapMgr->DoForAllMaps([&count](Map* map)
        {
            Map::PlayerList const& players = map->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            {
                Player* player = itr->GetSource();
                WorldSession* session = player ? player->GetSession() : nullptr;
                if (!session)
                    continue;

                if (session->IsVirtualSession() || BarracksHardcore::IsPlayerbot(player))
                    ++count.bots;
                else if (session->GetSecurity() > SEC_PLAYER && !player->isGMVisible())
                    continue;
                else
                    ++count.players;
            }
        });
        return count;
    }

    void SendRealm(ChatHandler* handler, WorldSession* session)
    {
        std::string date = GitRevision::GetDate();
        if (date.size() > 10)
            date.resize(10);

        handler->SendSysMessage(Trinity::StringFormat("|cffffd000{}|r - {}level cap {}. Build {} ({}).",
            realm.Name, sWorld->IsPvPRealm() ? "PvP realm, " : "",
            sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), GitRevision::GetHash(), date));

        time_t const now = GameTime::GetGameTime();
        tm local{};
        localtime_r(&now, &local);
        char clock[32] = {};
        strftime(clock, sizeof(clock), "%a %H:%M %Z", &local);

        Headcount const heads = CountHeads();
        handler->SendSysMessage(Trinity::StringFormat("Online: {} {} and {} bots. Up {}. Server time {}.",
            heads.players, heads.players == 1 ? "player" : "players", heads.bots, Span(GameTime::GetUptime()), clock));

        // The minute's average rather than one tick: a single tick is noise, and
        // the world tick is the slowest map, so one bad battleground spikes it.
        TickStats::WorldWindow const tick = TickStats::GetWorldWindow(MINUTE * IN_MILLISECONDS);
        uint32 const tickMs = tick.ticks ? tick.avgMs : sWorldUpdateTime.GetLastUpdateTime();
        handler->SendSysMessage(Trinity::StringFormat("Server tick {} ms (last minute). Your latency {} ms.",
            tickMs, session->GetLatency()));

        time_t const daily = sWorld->GetNextDailyQuestsResetTime();
        time_t const weekly = sWorld->GetNextWeeklyQuestsResetTime();
        if (daily > now && weekly > now)
            handler->SendSysMessage(Trinity::StringFormat("Daily quests reset in {}, weekly quests in {}.", Span(daily - now), Span(weekly - now)));
        else if (daily > now)
            handler->SendSysMessage(Trinity::StringFormat("Daily quests reset in {}.", Span(daily - now)));

        float const killXp = sWorld->getRate(RATE_XP_KILL);
        float const questXp = sWorld->getRate(RATE_XP_QUEST);
        std::string const xp = killXp == questXp
            ? Rate(killXp)
            : Trinity::StringFormat("{} (kills), {} (quests)", Rate(killXp), Rate(questXp));
        handler->SendSysMessage(Trinity::StringFormat("Rates: experience {}, honor {}, reputation {}, loot {}, gold {}.",
            xp, Rate(sWorld->getRate(RATE_HONOR)), Rate(sWorld->getRate(RATE_REPUTATION_GAIN)),
            Rate(sWorld->getRate(RATE_DROP_ITEM_RARE)), Rate(sWorld->getRate(RATE_DROP_MONEY))));

        if (sWorld->IsShuttingDown())
            handler->SendSysMessage(Trinity::StringFormat("|cffff6060The server goes down in {}.|r", Span(sWorld->GetShutDownTimeLeft())));
    }

    // Which battleground pool this character queues into, as the Battlegrounds
    // tab's locked toggle would explain it.
    std::string QueueNote(Player* player)
    {
        if (!Tournament::QueuesInTournamentPool(player))
            return "battlegrounds: world queue";

        if (Tournament::GetQueueLockReason(player) == Tournament::QUEUE_LOCK_FORCED)
            return Trinity::StringFormat("battlegrounds: tournament queue (every world character from level {})", Tournament::GetQueueMinLevel());
        return "battlegrounds: tournament queue (you opted in)";
    }

    void SendExperience(ChatHandler* handler, Player* player)
    {
        uint32 const cap = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        if (player->GetLevel() >= cap)
            return;

        // Player::RewardHonor keeps the honor of anyone who stopped their own
        // experience, because for them there is nothing to convert it into.
        bool const xpOff = player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
        if (xpOff)
            handler->SendSysMessage("Experience: turned off.");
        else if (player->IsWarModeXpCapped())
            handler->SendSysMessage("Experience: none here - War Mode is on and you are at this zone's top level.");
        else if (uint32 const nextLevelXp = player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP))
        {
            uint32 const rested = uint32(player->GetRestBonus() * 100.0f / nextLevelXp);
            if (rested)
                handler->SendSysMessage(Trinity::StringFormat("Rested: {}% of a level.", rested));
        }

        if (sWorld->getBoolConfig(CONFIG_CENTURION_BG_XP_INSTEAD_OF_HONOR))
            handler->SendSysMessage(xpOff
                ? Trinity::StringFormat("Below level {} honor is paid as experience; with yours off you keep the honor.", cap)
                : Trinity::StringFormat("Below level {} honor is paid as experience instead.", cap));
    }

    void SendHonor(ChatHandler* handler, Player* player)
    {
        handler->SendSysMessage(Trinity::StringFormat("Honor {} of {}. Arena points {}.",
            player->GetHonorPoints(), player->GetMaxHonorPoints(), player->GetArenaPoints()));

        // The ladder is a tournament character's; a world character has a cap
        // of its own and nothing to climb.
        if (Tournament::GetWorldMaxHonorPoints(player))
            return;

        uint32 gateSpellId = 0;
        uint32 gateQuestId = 0;
        uint32 const next = player->GetNextHonorCapStep(&gateSpellId, &gateQuestId);
        if (!next)
        {
            handler->SendSysMessage("That is the top of the honor cap ladder.");
            return;
        }

        if (Quest const* gate = gateQuestId ? sObjectMgr->GetQuestTemplate(gateQuestId) : nullptr)
        {
            handler->SendSysMessage(Trinity::StringFormat("Completing \"{}\" raises the cap to {}.", gate->GetTitle(), next));
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                uint32 const itemId = gate->RequiredItemId[i];
                uint32 const needed = gate->RequiredItemCount[i];
                ItemTemplate const* item = itemId && needed ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
                if (item)
                    handler->SendSysMessage(Trinity::StringFormat("  You hold {} of the {} {} it asks for.",
                        player->GetItemCount(itemId, true), needed, item->Name1));
            }
        }
        else if (SpellInfo const* spell = gateSpellId ? sSpellMgr->GetSpellInfo(gateSpellId) : nullptr)
            handler->SendSysMessage(Trinity::StringFormat("Learning {} raises the cap to {}.", spell->SpellName[handler->GetSessionDbcLocale()], next));
        else
            handler->SendSysMessage(Trinity::StringFormat("The next step of the honor cap holds {}.", next));
    }

    void SendWorldRules(ChatHandler* handler, Player* player)
    {
        LocaleConstant const locale = handler->GetSessionDbcLocale();

        if (!BarracksHardcore::IsWarModeOptedIn(player))
            handler->SendSysMessage("War Mode: off.");
        else if (player->IsWarModePaused())
            handler->SendSysMessage("War Mode: on, but paused here - this zone is below your level, so you can neither harm nor help other players.");
        else
            handler->SendSysMessage("War Mode: on.");

        if (Bounty::Enabled())
            if (uint32 const stacks = Bounty::GetStacks(player))
                handler->SendSysMessage(Trinity::StringFormat("Bounty: {} {} on your head{}.", stacks, stacks == 1 ? "stack" : "stacks",
                    Bounty::IsHuntedRelentlessly(stacks) ? ", and the bots will not stop coming" : ""));

        if (Notoriety::Enabled() && Notoriety::HasLiveContract(player->GetGUID()))
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(Notoriety::QuestId());
            std::string line = quest ? Trinity::StringFormat("Contract: \"{}\"", quest->GetTitle()) : std::string("Contract: carried");

            uint32 mapId = 0;
            uint32 zoneId = 0;
            Position rendezvous;
            if (Notoriety::GetRendezvous(player->GetGUID(), mapId, zoneId, rendezvous))
                if (char const* zone = AreaName(zoneId, locale))
                    line += Trinity::StringFormat(" - your fence waits in {}", zone);
            handler->SendSysMessage(line + ".");
        }

        if (sChallengeModes->Enabled())
        {
            std::string modes;
            for (uint8 i = SETTING_HARDCORE; i <= SETTING_IRON_MAN; ++i)
            {
                ChallengeModeSettings const setting = ChallengeModeSettings(i);
                if (!sChallengeModes->IsEnabledForPlayer(setting, player))
                    continue;

                if (!modes.empty())
                    modes += ", ";
                modes += ChallengeModes::GetDisplayName(setting);
                if (setting == SETTING_HARDCORE && sChallengeModes->IsEnabledForPlayer(HARDCORE_DEAD, player))
                    modes += " (fallen)";
            }
            if (!modes.empty())
                handler->SendSysMessage(Trinity::StringFormat("Challenge modes: {}.", modes));
        }
    }

    void SendCharacter(ChatHandler* handler, Player* player)
    {
        LocaleConstant const locale = handler->GetSessionDbcLocale();
        std::string const name = Surnames::Decorated(player->GetGUID(), player->GetName());
        bool const tournament = Tournament::IsEnabled() && Tournament::IsTournamentCharacter(player);

        if (!Tournament::IsEnabled())
            handler->SendSysMessage(Trinity::StringFormat("|cffffd000{}|r", name));
        else if (tournament)
            handler->SendSysMessage(Trinity::StringFormat("|cffffd000{}|r - tournament character.", name));
        else
            handler->SendSysMessage(Trinity::StringFormat("|cffffd000{}|r - world character; {}.", name, QueueNote(player)));

        handler->SendSysMessage(Trinity::StringFormat("Played {}, {} of it at this level.",
            Span(player->GetTotalPlayedTime()), Span(player->GetLevelPlayedTime())));

        SendExperience(handler, player);
        SendHonor(handler, player);

        // Bounties, contracts, challenge modes and War Mode are world-character
        // affairs; a tournament character is outside all of them.
        if (!tournament)
            SendWorldRules(handler, player);

        std::string where;
        if (char const* zone = AreaName(player->GetZoneId(), locale))
            where = zone;
        if (player->GetAreaId() != player->GetZoneId())
            if (char const* area = AreaName(player->GetAreaId(), locale))
                where += where.empty() ? std::string(area) : Trinity::StringFormat(", {}", area);
        if (where.empty())
            if (MapEntry const* map = sMapStore.LookupEntry(player->GetMapId()))
                where = map->MapName[locale];

        handler->SendSysMessage(Trinity::StringFormat("Location: {} - map {} at {:.1f}, {:.1f}, {:.1f}.",
            where.empty() ? "unknown" : where, player->GetMapId(),
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()));
    }
}

class custom_info_commandscript : public CommandScript
{
public:
    custom_info_commandscript() : CommandScript("custom_info_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "info", HandleInfo, rbac::RBAC_PERM_COMMAND_INFO, Console::No },
        };
        return commandTable;
    }

    static bool HandleInfo(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player)
            return false;

        SendRealm(handler, session);
        SendCharacter(handler, player);
        return true;
    }
};

void AddSC_custom_info_command()
{
    new custom_info_commandscript();
}
