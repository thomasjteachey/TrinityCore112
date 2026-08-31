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

#include "custom_loot_chest_helper.h"

#include "Bag.h"
#include "Item.h"
#include "Log.h"
#include "GameTime.h"
#include "ObjectGuid.h"
#include <algorithm>
#include <mutex>
#include <vector>

namespace CustomLootChests
{
PlayerChestBuilder::PlayerChestBuilder(Player* player, uint32 chestEntry, Seconds despawnTime)
    : _player(player), _chestEntry(chestEntry), _despawnTime(despawnTime)
{
}

LootItem PlayerChestBuilder::CreateLootItem(uint32 itemId, uint8 count, uint32 randomSuffix, int32 randomPropertyId) const
{
    LootItem lootItem;
    lootItem.itemid = itemId;
    lootItem.itemIndex = static_cast<uint32>(_items.size());
    lootItem.count = count;
    lootItem.randomSuffix = randomSuffix;
    lootItem.randomPropertyId = randomPropertyId;
    lootItem.freeforall = false;
    lootItem.follow_loot_rules = false;
    return lootItem;
}

void PlayerChestBuilder::AddStackableItem(uint32 itemId, uint32 count)
{
    if (!itemId || !count)
        return;

    // First, try to top off any existing stacks of the same item instead of
    // creating additional entries in the loot list.
    for (LootItem& item : _items)
    {
        if (item.itemid != itemId || item.count >= 255)
            continue;

        uint32 const room = 255 - item.count;
        uint32 const toAdd = std::min<uint32>(count, room);
        item.count += toAdd;
        count -= toAdd;

        if (!count)
            return;
    }

    while (count && _items.size() < MAX_NR_LOOT_ITEMS)
    {
        uint8 const stack = static_cast<uint8>(std::min<uint32>(count, 255));
        _items.push_back(CreateLootItem(itemId, stack, 0, 0));
        count -= stack;
    }
}

void PlayerChestBuilder::AddItem(Item* item)
{
    if (!item || _items.size() >= MAX_NR_LOOT_ITEMS)
        return;

    _items.push_back(CreateLootItem(item->GetEntry(), item->GetCount(), item->GetItemSuffixFactor(), item->GetItemRandomPropertyId()));
}

GameObject* PlayerChestBuilder::Summon() const
{
    if (!_player || !_chestEntry || _items.empty())
        return nullptr;

    GameObject* chest = _player->SummonGameObject(_chestEntry, _player->GetPosition(), QuaternionData(), _despawnTime, GO_SUMMON_TIMED_DESPAWN);
    if (!chest)
    {
        TC_LOG_WARN("playerbots.pve", "CustomLootChests: failed to summon chest {} for player {} ({})", _chestEntry, _player->GetName(), _player->GetGUID().ToString());
        return nullptr;
    }

    _player->RemoveGameObject(chest, false);
    chest->SetOwnerGUID(ObjectGuid::Empty);

    // NOTE: an earlier version forced SetSpawnedByDefault(false) here, on the
    // reasoning that a player-summoned GameObject keeps m_spawnedByDefault true
    // and is therefore never isSpawned(). A probe on the live realm disproved
    // it outright - "isSpawned true -> true, spawnedByDefault false -> false" -
    // so the chest was always spawned and always findable, and the block was
    // doing nothing. Removed rather than left in as decoration.
    //
    // Bots ignoring chests was never a visibility problem: it was priority.
    // The chest lookup sat behind an errand scan that fires once every fifteen
    // seconds and only while the bot is idle, so a bot always found a fight or
    // an errand first.

    Loot& loot = chest->loot;
    loot.clear();
    loot.loot_type = LOOT_CORPSE;
    loot.lootOwnerGUID.Clear();

    for (LootItem& item : _items)
    {
        item.itemIndex = static_cast<uint32>(loot.items.size());
        loot.items.push_back(item);
        ++loot.unlootedCount;
    }

    chest->SetLootRecipient(nullptr);
    chest->SetLootState(GO_READY);
    chest->SetGoState(GO_STATE_READY);
    chest->ForceValuesUpdateAtIndex(GAMEOBJECT_DYNAMIC);
    chest->ForceValuesUpdateAtIndex(GAMEOBJECT_FLAGS);

    // Tell the registry where it is, so nothing ever has to search for it.
    RegisterChest(chest, _despawnTime);
    return chest;
}

namespace
{
    std::mutex g_ChestRegistryLock;
    std::vector<ChestLocation> g_ChestRegistry;
}

void RegisterChest(GameObject* chest, Seconds despawnTime)
{
    if (!chest)
        return;

    ChestLocation record;
    record.Guid = chest->GetGUID();
    record.Entry = chest->GetEntry();
    record.MapId = chest->GetMapId();
    record.X = chest->GetPositionX();
    record.Y = chest->GetPositionY();
    record.Z = chest->GetPositionZ();
    record.ExpiresAt = GameTime::GetGameTime() + despawnTime.count();

    std::lock_guard<std::mutex> guard(g_ChestRegistryLock);
    g_ChestRegistry.push_back(record);
}

void ForgetChest(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(g_ChestRegistryLock);
    g_ChestRegistry.erase(std::remove_if(g_ChestRegistry.begin(), g_ChestRegistry.end(),
        [guid](ChestLocation const& record) { return record.Guid == guid; }), g_ChestRegistry.end());
}

bool FindNearestChest(uint32 entry, uint32 mapId, float x, float y, float z, float maxDistance, ObjectGuid& outGuid)
{
    time_t const now = GameTime::GetGameTime();
    float const limitSq = maxDistance * maxDistance;
    float bestSq = limitSq;
    bool found = false;

    std::lock_guard<std::mutex> guard(g_ChestRegistryLock);

    // Drop anything past its own despawn deadline while we are here; the list
    // stays a handful of entries, so this never becomes the expensive part.
    g_ChestRegistry.erase(std::remove_if(g_ChestRegistry.begin(), g_ChestRegistry.end(),
        [now](ChestLocation const& record) { return record.ExpiresAt <= now; }), g_ChestRegistry.end());

    for (ChestLocation const& record : g_ChestRegistry)
    {
        if (record.Entry != entry || record.MapId != mapId)
            continue;

        float const dx = record.X - x;
        float const dy = record.Y - y;
        float const dz = record.Z - z;
        float const distSq = dx * dx + dy * dy + dz * dz;
        if (distSq > bestSq)
            continue;

        bestSq = distSq;
        outGuid = record.Guid;
        found = true;
    }

    return found;
}

void CollectItemsWithQuality(Player* player, ItemQualities quality, PlayerChestBuilder& chest, std::vector<ItemLocation>& removedItems,
    std::unordered_set<uint32> const& excludedEntries)
{
    if (!player)
        return;

    auto const tryStoreItem = [&](Item* item, uint8 bag, uint8 slot)
    {
        if (!item)
            return;

        if (ItemTemplate const* proto = item->GetTemplate())
        {
            if (proto->Quality == quality && !excludedEntries.count(item->GetEntry()))
            {
                chest.AddItem(item);
                removedItems.push_back({ bag, slot });
            }
        }
    };

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        tryStoreItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        tryStoreItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        if (Bag* bag = player->GetBagByPos(bagSlot))
        {
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                tryStoreItem(bag->GetItemByPos(slot), bagSlot, slot);
        }
    }
}
}
