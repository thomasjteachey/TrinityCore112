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
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include "mod_pvpve_dungeon.h"

#include <vector>

namespace
{
constexpr uint32 ACTION_QUEUE = GOSSIP_ACTION_INFO_DEF + 1;
constexpr uint32 ACTION_CLOSE = GOSSIP_ACTION_INFO_DEF + 2;
constexpr uint32 STOCKADES_TEMPLATE_ID = 1;

void SendLeaderError(Player* player, char const* text)
{
    if (!player)
        return;

    player->PlayerTalkClass->SendCloseGossip();
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

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!player || !creature)
            return false;

        player->PlayerTalkClass->ClearMenus();
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Queue for Stockades PvPvE", GOSSIP_SENDER_MAIN, ACTION_QUEUE);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Maybe later", GOSSIP_SENDER_MAIN, ACTION_CLOSE);
        player->SendGossipMenu(player->GetGossipTextId(creature), creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (sender != GOSSIP_SENDER_MAIN)
            return false;

        switch (action)
        {
            case ACTION_QUEUE:
                HandleQueue(player);
                return true;
            case ACTION_CLOSE:
                player->PlayerTalkClass->SendCloseGossip();
                return true;
            default:
                break;
        }

        return false;
    }

private:
    void HandleQueue(Player* player)
    {
        if (!player)
            return;

        Group* group = player->GetGroup();
        if (!group)
        {
            SendLeaderError(player, "You must be in a party to queue for the Stockades PvPvE event.");
            return;
        }

        if (!group->IsLeader(player->GetGUID()))
        {
            SendLeaderError(player, "Only the party leader can queue for the Stockades PvPvE event.");
            return;
        }

        if (group->GetMembersCount() != 2)
        {
            SendLeaderError(player, "Exactly two party members are required for this queue.");
            return;
        }

        std::vector<ObjectGuid> memberGuids;
        memberGuids.reserve(2);

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld())
            {
                SendLeaderError(player, "All party members must be online to join the queue.");
                return;
            }

            if (!member->IsAlive())
            {
                SendLeaderError(player, "All party members must be alive to join the queue.");
                return;
            }

            if (member->GetMap() && member->GetMap()->Instanceable())
            {
                SendLeaderError(player, "Party members must leave instances before queueing.");
                return;
            }

            memberGuids.push_back(member->GetGUID());
        }

        if (memberGuids.size() != 2)
        {
            SendLeaderError(player, "Unable to determine party members. Please try again.");
            return;
        }

        if (!PvpveDungeonMgr::instance()->QueueTeam(STOCKADES_TEMPLATE_ID, memberGuids))
        {
            SendLeaderError(player, "Failed to join the queue. Please try again shortly.");
            return;
        }

        BroadcastToGroup(group, "You have joined the Stockades PvPvE queue!");
        player->PlayerTalkClass->SendCloseGossip();
    }
};

void AddSC_npc_pvpve_dungeon_queue()
{
    new npc_pvpve_dungeon_queue();
}
