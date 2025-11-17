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

#include "Group.h"
#include "GroupReference.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "WorldSession.h"

#include "mod_pvpve_dungeon.h"

#include <vector>

namespace
{
constexpr uint32 ACTION_QUEUE = GOSSIP_ACTION_INFO_DEF + 1;
constexpr uint32 ACTION_INVADE = GOSSIP_ACTION_INFO_DEF + 2;
constexpr uint32 ACTION_CLOSE = GOSSIP_ACTION_INFO_DEF + 3;
constexpr uint32 STOCKADES_TEMPLATE_ID = 1;

void SendQueueError(Player* player, char const* text)
{
    if (!player)
        return;

    CloseGossipMenuFor(player);
    if (WorldSession* session = player->GetSession())
        session->SendNotification("%s", text);
}

void BroadcastToGroup(Group* group, char const* text)
{
    if (!group || !text)
        return;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* member = ref->GetSource())
            if (WorldSession* session = member->GetSession())
                session->SendNotification("%s", text);
}
}

class npc_pvpve_dungeon_queue : public CreatureScript
{
public:
    npc_pvpve_dungeon_queue() : CreatureScript("npc_pvpve_dungeon_queue") { }

    struct npc_pvpve_dungeon_queueAI : public ScriptedAI
    {
        npc_pvpve_dungeon_queueAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            if (!player)
                return false;

            ClearGossipMenuFor(player);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Queue for Stockades PvPvE", GOSSIP_SENDER_MAIN, ACTION_QUEUE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Invade someone's Stockades run", GOSSIP_SENDER_MAIN, ACTION_INVADE,
                "Enter the name of a player who is already inside the Stockades PvPvE instance:", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Maybe later", GOSSIP_SENDER_MAIN, ACTION_CLOSE);
            SendGossipMenuFor(player, player->GetGossipTextId(me), me);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            if (!player)
                return false;

            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            switch (action)
            {
                case ACTION_QUEUE:
                    HandleQueue(player, 0);
                    return true;
                case ACTION_INVADE:
                    SendQueueError(player, "You must enter a player name to invade their run.");
                    return true;
                case ACTION_CLOSE:
                    CloseGossipMenuFor(player);
                    return true;
                default:
                    break;
            }

            return false;
        }

        bool OnGossipSelectCode(Player* player, uint32 /*menuId*/, uint32 gossipListId, char const* code) override
        {
            if (!player)
                return false;

            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            switch (action)
            {
                case ACTION_INVADE:
                    HandleInvade(player, code ? code : "");
                    return true;
                default:
                    break;
            }

            return false;
        }

    private:
        void HandleQueue(Player* player, uint64 preferredRunId)
        {
            if (!player)
                return;

            DungeonTemplate const* dungeonTemplate = PvpveDungeonMgr::instance()->GetDungeonTemplate(STOCKADES_TEMPLATE_ID);
            if (!dungeonTemplate || !dungeonTemplate->Enabled)
            {
                SendQueueError(player, "The Stockades PvPvE queue is currently unavailable.");
                return;
            }

            if (preferredRunId)
            {
                PvpveDungeonRun* preferredRun = PvpveDungeonMgr::instance()->GetRun(preferredRunId);
                if (!preferredRun || preferredRun->TemplateId != STOCKADES_TEMPLATE_ID)
                {
                    SendQueueError(player, "That run is no longer available for invasion.");
                    return;
                }

                if (preferredRun->Completed)
                {
                    SendQueueError(player, "That run has already finished.");
                    return;
                }

                if (dungeonTemplate->MaxTeams && preferredRun->Teams.size() >= dungeonTemplate->MaxTeams)
                {
                    SendQueueError(player, "That run already has the maximum number of teams.");
                    return;
                }
            }

            Group* group = player->GetGroup();
            std::vector<ObjectGuid> memberGuids;
            memberGuids.reserve(group ? group->GetMembersCount() : 1);

            if (!group)
            {
                if (dungeonTemplate->MinPlayers > 1)
                {
                    SendQueueError(player, "You must be in a party to queue for this event.");
                    return;
                }

                memberGuids.push_back(player->GetGUID());
            }
            else
            {
                if (!group->IsLeader(player->GetGUID()))
                {
                    SendQueueError(player, "Only the party leader can queue for the Stockades PvPvE event.");
                    return;
                }

                uint32 const memberCount = group->GetMembersCount();
                if (dungeonTemplate->MinPlayers && memberCount < dungeonTemplate->MinPlayers)
                {
                    SendQueueError(player, "Your party does not meet the minimum size for this event.");
                    return;
                }

                if (dungeonTemplate->MaxPlayers && memberCount > dungeonTemplate->MaxPlayers)
                {
                    SendQueueError(player, "Your party exceeds the maximum size for this event.");
                    return;
                }

                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (!member || !member->IsInWorld())
                    {
                        SendQueueError(player, "All party members must be online to join the queue.");
                        return;
                    }

                    if (!member->IsAlive())
                    {
                        SendQueueError(player, "All party members must be alive to join the queue.");
                        return;
                    }

                    if (member->GetMap() && member->GetMap()->Instanceable())
                    {
                        SendQueueError(player, "Party members must leave instances before queueing.");
                        return;
                    }

                    memberGuids.push_back(member->GetGUID());
                }

                if (memberGuids.size() != memberCount)
                {
                    SendQueueError(player, "Unable to determine party members. Please try again.");
                    return;
                }
            }

            if (!PvpveDungeonMgr::instance()->QueueTeam(STOCKADES_TEMPLATE_ID, memberGuids, preferredRunId))
            {
                SendQueueError(player, "Failed to join the queue. Please try again shortly.");
                return;
            }

            char const* successText = preferredRunId ? "You are preparing to invade an active Stockades PvPvE run!" :
                "You have joined the Stockades PvPvE queue!";

            if (group)
                BroadcastToGroup(group, successText);
            else if (WorldSession* session = player->GetSession())
                session->SendNotification("%s", successText);

            CloseGossipMenuFor(player);
        }

        void HandleInvade(Player* player, std::string targetName)
        {
            if (!player)
                return;

            auto firstNonSpace = targetName.find_first_not_of(" \t\r\n");
            if (firstNonSpace == std::string::npos)
            {
                SendQueueError(player, "You must enter a player name to invade their run.");
                return;
            }

            auto lastNonSpace = targetName.find_last_not_of(" \t\r\n");
            targetName = targetName.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1);

            if (!normalizePlayerName(targetName))
            {
                SendQueueError(player, "That is not a valid player name.");
                return;
            }

            Player* target = ObjectAccessor::FindPlayerByName(targetName);
            if (!target)
            {
                SendQueueError(player, "That player is not currently online.");
                return;
            }

            if (target == player)
            {
                SendQueueError(player, "You cannot invade your own run.");
                return;
            }

            PvpveDungeonRun* targetRun = PvpveDungeonMgr::instance()->GetRunForPlayer(target->GetGUID());
            if (!targetRun || targetRun->TemplateId != STOCKADES_TEMPLATE_ID)
            {
                SendQueueError(player, "Your target is not inside an active Stockades PvPvE run.");
                return;
            }

            HandleQueue(player, targetRun->Id);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_pvpve_dungeon_queueAI(creature);
    }
};

void AddSC_npc_pvpve_dungeon_queue()
{
    new npc_pvpve_dungeon_queue();
}
