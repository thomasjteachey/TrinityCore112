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

#include "Item.h"
#include "ItemTemplate.h"
#include "Miscellaneous/Transmogrification.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <vector>

namespace
{
using namespace Trinity::Transmog;

constexpr uint32 ACTION_MAIN       = GOSSIP_ACTION_INFO_DEF + 1;
constexpr uint32 ACTION_TOGGLE     = GOSSIP_ACTION_INFO_DEF + 2;
constexpr uint32 ACTION_REMOVE_ALL = GOSSIP_ACTION_INFO_DEF + 3;
constexpr uint32 ACTION_CLOSE      = GOSSIP_ACTION_INFO_DEF + 4;

// The gossip action is the only state that survives a menu round trip, so slot and
// page are encoded into it rather than kept in a server-side map (maps update in
// parallel; per-player script state would need locking to be safe).
constexpr uint32 PAGES_PER_SLOT    = 100;
constexpr uint32 ACTION_SLOT_BASE  = GOSSIP_ACTION_INFO_DEF + 100;                                     // + slot * PAGES_PER_SLOT + page
constexpr uint32 ACTION_CLEAR_BASE = ACTION_SLOT_BASE + EQUIPMENT_SLOT_END * PAGES_PER_SLOT + 100;     // + slot
// The chosen appearance travels as its item entry rather than a list index, so a
// bag change between opening the menu and confirming cannot shift the selection.
constexpr uint32 ENTRIES_PER_SLOT  = 1000000;
constexpr uint32 ACTION_PICK_BASE  = ACTION_CLEAR_BASE + EQUIPMENT_SLOT_END + 100;                      // + slot * ENTRIES_PER_SLOT + item entry

// Leaves room for clear/prev/next/back and stays clear of GOSSIP_MAX_MENU_ITEMS (32).
constexpr uint32 SOURCES_PER_PAGE  = 26;

char const* GetSlotName(uint8 slot)
{
    switch (slot)
    {
        case EQUIPMENT_SLOT_HEAD:      return "Head";
        case EQUIPMENT_SLOT_SHOULDERS: return "Shoulders";
        case EQUIPMENT_SLOT_BODY:      return "Shirt";
        case EQUIPMENT_SLOT_CHEST:     return "Chest";
        case EQUIPMENT_SLOT_WAIST:     return "Waist";
        case EQUIPMENT_SLOT_LEGS:      return "Legs";
        case EQUIPMENT_SLOT_FEET:      return "Feet";
        case EQUIPMENT_SLOT_WRISTS:    return "Wrists";
        case EQUIPMENT_SLOT_HANDS:     return "Hands";
        case EQUIPMENT_SLOT_BACK:      return "Back";
        case EQUIPMENT_SLOT_MAINHAND:  return "Main hand";
        case EQUIPMENT_SLOT_OFFHAND:   return "Off hand";
        case EQUIPMENT_SLOT_RANGED:    return "Ranged";
        case EQUIPMENT_SLOT_TABARD:    return "Tabard";
        default:                       return "Unknown";
    }
}

void Notify(Player* player, char const* text)
{
    if (WorldSession* session = player ? player->GetSession() : nullptr)
        session->SendNotification("%s", text);
}

// What the player currently sees in this slot, for the menu line.
std::string DescribeSlot(Player* player, uint8 slot)
{
    Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item)
        return "";

    if (uint32 const fakeEntry = player->GetTransmogEntry(item->GetGUID()))
        if (ItemTemplate const* fake = sObjectMgr->GetItemTemplate(fakeEntry))
            return GetColoredName(fake);

    return GetColoredName(item->GetTemplate());
}

void SendMainMenu(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);

    AddGossipItemFor(player, GOSSIP_ICON_CHAT,
        player->IsTransmogEnabled() ? "Transmog display: |cff20ff20ON|r (click to turn off)"
                                    : "Transmog display: |cffff2020OFF|r (click to turn on)",
        GOSSIP_SENDER_MAIN, ACTION_TOGGLE);

    if (player->IsTransmogEnabled())
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (!IsTransmogSlot(slot))
                continue;

            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;

            AddGossipItemFor(player, GOSSIP_ICON_VENDOR,
                Trinity::StringFormat("{}: {}", GetSlotName(slot), DescribeSlot(player, slot)),
                GOSSIP_SENDER_MAIN, ACTION_SLOT_BASE + slot * PAGES_PER_SLOT);
        }

        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Remove all transmogrifications",
            GOSSIP_SENDER_MAIN, ACTION_REMOVE_ALL,
            "Remove every appearance from your gear?", 0, false);
    }
    else
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "|cff808080Turn the display on to change your appearance.|r", GOSSIP_SENDER_MAIN, ACTION_MAIN);

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Nevermind", GOSSIP_SENDER_MAIN, ACTION_CLOSE);
    SendGossipMenuFor(player, player->GetGossipTextId(creature), creature);
}

void SendSlotMenu(Player* player, Creature* creature, uint8 slot, uint32 page)
{
    Item* target = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!target)
    {
        Notify(player, GetResultText(Result::NoTargetItem));
        SendMainMenu(player, creature);
        return;
    }

    std::vector<ItemTemplate const*> sources;
    CollectSourcesForSlot(player, slot, sources);

    uint32 const pageCount = std::max<uint32>(1, (uint32(sources.size()) + SOURCES_PER_PAGE - 1) / SOURCES_PER_PAGE);
    if (page >= pageCount)
        page = pageCount - 1;

    ClearGossipMenuFor(player);

    uint32 const first = page * SOURCES_PER_PAGE;
    uint32 const last = std::min<uint32>(first + SOURCES_PER_PAGE, uint32(sources.size()));

    for (uint32 i = first; i < last; ++i)
    {
        ItemTemplate const* source = sources[i];
        std::string const confirm = GetTokenCost()
            ? Trinity::StringFormat("Make your {} look like {}?\n\nCost: {} x {}",
                GetSlotName(slot), source->Name1, GetTokenCost(), GetTokenName())
            : Trinity::StringFormat("Make your {} look like {}?", GetSlotName(slot), source->Name1);

        AddGossipItemFor(player, GOSSIP_ICON_VENDOR, GetColoredName(source), GOSSIP_SENDER_MAIN,
            ACTION_PICK_BASE + slot * ENTRIES_PER_SLOT + source->ItemId, confirm, 0, false);
    }

    if (sources.empty())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|cff808080No suitable appearances in your bags or bank.|r",
            GOSSIP_SENDER_MAIN, ACTION_SLOT_BASE + slot * PAGES_PER_SLOT + page);

    if (page > 0)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<- Previous page",
            GOSSIP_SENDER_MAIN, ACTION_SLOT_BASE + slot * PAGES_PER_SLOT + (page - 1));

    if (page + 1 < pageCount)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next page ->",
            GOSSIP_SENDER_MAIN, ACTION_SLOT_BASE + slot * PAGES_PER_SLOT + (page + 1));

    if (player->GetTransmogEntry(target->GetGUID()))
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Restore the original appearance",
            GOSSIP_SENDER_MAIN, ACTION_CLEAR_BASE + slot);

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<- Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, player->GetGossipTextId(creature), creature);
}

void HandlePick(Player* player, Creature* creature, uint8 slot, uint32 sourceEntry)
{
    Item* target = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!target)
    {
        Notify(player, GetResultText(Result::NoTargetItem));
        SendMainMenu(player, creature);
        return;
    }

    // Everything is re-checked against the player as they are right now: the menu
    // was built some time ago and the client can send any option back.
    ItemTemplate const* source = sObjectMgr->GetItemTemplate(sourceEntry);
    if (!source || !player->GetItemCount(sourceEntry, true))
    {
        Notify(player, GetResultText(Result::NoSourceItem));
        SendSlotMenu(player, creature, slot, 0);
        return;
    }

    Result const result = CanTransmogrify(player, target, source);
    if (result != Result::Ok)
    {
        Notify(player, GetResultText(result));
        SendSlotMenu(player, creature, slot, 0);
        return;
    }

    if (!TakeTokens(player))
    {
        Notify(player, GetResultText(Result::NotEnoughTokens));
        SendSlotMenu(player, creature, slot, 0);
        return;
    }

    player->SetTransmog(target, source->ItemId);
    Notify(player, "Your gear has been transmogrified.");
    SendMainMenu(player, creature);
}
}

class npc_transmogrifier : public CreatureScript
{
public:
    npc_transmogrifier() : CreatureScript("npc_transmogrifier") { }

    struct npc_transmogrifierAI : public ScriptedAI
    {
        npc_transmogrifierAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            if (!player)
                return false;

            if (!Trinity::Transmog::IsEnabled())
            {
                Notify(player, Trinity::Transmog::GetResultText(Trinity::Transmog::Result::Disabled));
                CloseGossipMenuFor(player);
                return true;
            }

            SendMainMenu(player, me);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            if (!player)
                return false;

            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);

            if (!Trinity::Transmog::IsEnabled())
            {
                CloseGossipMenuFor(player);
                return true;
            }

            switch (action)
            {
                case ACTION_CLOSE:
                    CloseGossipMenuFor(player);
                    return true;
                case ACTION_MAIN:
                    SendMainMenu(player, me);
                    return true;
                case ACTION_TOGGLE:
                    player->SetTransmogEnabled(!player->IsTransmogEnabled());
                    Notify(player, player->IsTransmogEnabled()
                        ? "Transmogrification is now visible to you."
                        : "You and everyone around you now see your real gear.");
                    SendMainMenu(player, me);
                    return true;
                case ACTION_REMOVE_ALL:
                {
                    uint32 const removed = player->RemoveAllTransmogs();
                    Notify(player, removed ? "Your gear looks like itself again." : "You had nothing transmogrified.");
                    SendMainMenu(player, me);
                    return true;
                }
                default:
                    break;
            }

            if (action >= ACTION_PICK_BASE)
            {
                uint32 const offset = action - ACTION_PICK_BASE;
                uint8 const slot = uint8(offset / ENTRIES_PER_SLOT);
                if (Trinity::Transmog::IsTransmogSlot(slot))
                    HandlePick(player, me, slot, offset % ENTRIES_PER_SLOT);
                else
                    SendMainMenu(player, me);
                return true;
            }

            if (action >= ACTION_CLEAR_BASE)
            {
                uint8 const slot = uint8(action - ACTION_CLEAR_BASE);
                if (Trinity::Transmog::IsTransmogSlot(slot))
                    if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        player->RemoveTransmog(item);

                SendMainMenu(player, me);
                return true;
            }

            if (action >= ACTION_SLOT_BASE)
            {
                uint32 const offset = action - ACTION_SLOT_BASE;
                uint8 const slot = uint8(offset / PAGES_PER_SLOT);
                if (Trinity::Transmog::IsTransmogSlot(slot))
                    SendSlotMenu(player, me, slot, offset % PAGES_PER_SLOT);
                else
                    SendMainMenu(player, me);
                return true;
            }

            return false;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_transmogrifierAI(creature);
    }
};

void AddSC_npc_transmogrifier()
{
    new npc_transmogrifier();
}
