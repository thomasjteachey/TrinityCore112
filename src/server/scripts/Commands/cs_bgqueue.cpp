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
                queueName = Trinity::StringFormat("Arena %uv%u", arenaType, arenaType);
            else
            {
                BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
                if (Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId))
                    queueName = bgTemplate->GetName();
                else
                    queueName = Trinity::StringFormat("Battleground type %u", bgTypeId);
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
};

void AddSC_bgqueue_commandscript()
{
    new bgqueue_commandscript();
}
