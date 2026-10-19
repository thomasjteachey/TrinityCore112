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

#include "ScriptMgr.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "World.h"
#include <algorithm>
#include <cmath>

using namespace Trinity::ChatCommands;

class bgqueue_commandscript : public CommandScript
{
public:
    bgqueue_commandscript() : CommandScript("bgqueue_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "bgqueue", HandleBgQueueCommand, rbac::RBAC_PERM_COMMAND_LFG_QUEUE, Console::Yes },
            { "bghonor", HandleBgHonorCommand, rbac::RBAC_PERM_COMMAND_LFG_QUEUE, Console::Yes },
        };

        return commandTable;
    }

    static bool HandleBgQueueCommand(ChatHandler* handler)
    {
        bool hasQueuedPlayers = false;

        for (uint32 queueTypeId = BATTLEGROUND_QUEUE_AV; queueTypeId < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueTypeId)
        {
            BattlegroundQueueTypeId bgQueueTypeId = BattlegroundQueueTypeId(queueTypeId);
            BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
            if (queue.m_QueuedPlayers.empty())
                continue;

            hasQueuedPlayers = true;

            std::string queueName;
            if (uint8 arenaType = BattlegroundMgr::BGArenaType(bgQueueTypeId))
                queueName = Trinity::StringFormat("Arena {}v{}", arenaType, arenaType);
            else
            {
                BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
                if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId))
                    queueName = bgTemplate->GetName();
                else
                    queueName = Trinity::StringFormat("Battleground type {}", uint32(bgTypeId));
            }

            handler->PSendSysMessage("%s queue (%zu players):", queueName.c_str(), queue.m_QueuedPlayers.size());

            for (BattlegroundQueue::QueuedPlayersMap::value_type const& queuedPlayer : queue.m_QueuedPlayers)
            {
                std::string playerName;

                if (Player const* player = ObjectAccessor::FindConnectedPlayer(queuedPlayer.first))
                    playerName = player->GetName();
                else if (!sCharacterCache->GetCharacterNameByGuid(queuedPlayer.first, playerName))
                    playerName = "<unknown>";

                char const* factionName = "Unknown";
                char const* queueMode = "Unrated";

                if (GroupQueueInfo const* groupInfo = queuedPlayer.second.GroupInfo)
                {
                    factionName = groupInfo->Team == HORDE ? "Horde" : "Alliance";
                    queueMode = groupInfo->IsRated ? "Rated" : "Unrated";
                }

                handler->PSendSysMessage(" - %s (%s, %s)", playerName.c_str(), factionName, queueMode);
            }
        }

        if (!hasQueuedPlayers)
            handler->SendSysMessage("No players are currently queued for battlegrounds or arenas.");

        return true;
    }

    // Every kind of match on record: its median length, the median honor its
    // winners and losers earn in play, the end-of-match payout that evens it
    // out to the realm's honor per hour, and the hourly rate a player of it
    // actually gets (Centurion.Battleground.HourlyHonor). Honor is shown
    // before any battleground's own adjustment (Tanaris and the other custom
    // maps trim theirs) and before it is turned into experience below the cap.
    static bool HandleBgHonorCommand(ChatHandler* handler)
    {
        bool const enabled = sWorld->getBoolConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ENABLE);
        uint32 const minSamples = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_MIN_SAMPLES);
        uint32 const honorPerHour = sBattlegroundMgr->GetHonorPerHour();
        bool const autoRate = !sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_RATE);

        std::string rateSource = "configured";
        if (autoRate)
        {
            uint32 const anchorType = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ANCHOR_TYPE);
            uint32 const anchorArenaType = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ANCHOR_ARENA_TYPE);
            if (!anchorType)
                rateSource = "what battlegrounds pay today";
            else
            {
                std::string anchorName = Trinity::StringFormat("type {}", anchorType);
                if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundTypeId(anchorType)))
                    anchorName = bgTemplate->GetName();
                if (anchorArenaType)
                    anchorName += Trinity::StringFormat(" {}v{}", anchorArenaType, anchorArenaType);
                rateSource = "what " + anchorName + " pays today";
            }
        }

        handler->PSendSysMessage("Hourly match honor is %s. Rate: %s (%s), %u matches needed per kind.",
            enabled ? "ON" : "OFF",
            honorPerHour ? Trinity::StringFormat("{} honor/hour", honorPerHour).c_str() : "not enough data",
            rateSource.c_str(), minSamples);

        std::vector<BattlegroundMgr::MatchDurationStats> stats = sBattlegroundMgr->GetMatchDurationStats();
        if (stats.empty())
        {
            handler->SendSysMessage("No matches have been recorded yet.");
            return true;
        }

        std::sort(stats.begin(), stats.end(), [](BattlegroundMgr::MatchDurationStats const& a, BattlegroundMgr::MatchDurationStats const& b)
        {
            if (a.Arena != b.Arena)
                return !a.Arena;
            return a.Key < b.Key;
        });

        for (BattlegroundMgr::MatchDurationStats const& stat : stats)
        {
            std::string name;
            if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundTypeId(stat.Key.TypeId)))
                name = bgTemplate->GetName();
            else
                name = Trinity::StringFormat("Type {}", stat.Key.TypeId);
            if (stat.Key.ArenaType)
                name += Trinity::StringFormat(" {}v{}", uint32(stat.Key.ArenaType), uint32(stat.Key.ArenaType));

            bool const balanced = enabled && honorPerHour && stat.Samples >= minSamples;
            BattlegroundMgr::HourlyHonorPayout const payout = balanced ?
                sBattlegroundMgr->HourlyHonorPayoutFor(stat, honorPerHour) : BattlegroundMgr::GetFlatPayout(stat.Arena);

            auto perHour = [&stat](uint32 earned, uint32 paid)
            {
                return stat.MedianSeconds ? uint32(uint64(earned + paid) * HOUR / stat.MedianSeconds) : 0u;
            };

            handler->PSendSysMessage(" - %s %u-%u: %u matches, median %u:%02u, in play %u/%u, payout %u/%u%s = %u/%u per hour",
                name.c_str(), uint32(stat.Key.MinLevel), uint32(stat.MaxLevel), stat.Samples,
                stat.MedianSeconds / MINUTE, stat.MedianSeconds % MINUTE,
                stat.MedianWinHonor, stat.MedianLossHonor, payout.Winner, payout.Loser,
                balanced ? "" : (stat.Samples < minSamples ? " (flat, too few)" : " (flat)"),
                perHour(stat.MedianWinHonor, payout.Winner), perHour(stat.MedianLossHonor, payout.Loser));
        }

        handler->SendSysMessage("Pairs are win/loss.");
        return true;
    }
};

void AddSC_bgqueue_commandscript()
{
    new bgqueue_commandscript();
}
