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
#include "ObjectGuid.h"
#include <algorithm>

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

    // The chest is deliberately left ownerless above so it outlives the corpse.
    // That also removes GameObject::IsAlwaysVisibleFor's owner escape, which is
    // what had been masking the following.
    //
    // WorldObject::SummonGameObject only calls SetSpawnedByDefault(false) when
    // the summoner is NOT a player (Object.cpp) - and this summoner is the dying
    // player, so the flag keeps its constructor value of true. Combined with a
    // non-zero respawn delay, all three clauses of GameObject::isSpawned() are
    // then false for the object's entire life:
    //
    //     m_respawnDelayTime == 0                        -> false (it is 3600)
    //     m_respawnTime > 0 && !m_spawnedByDefault        -> false (flag is true)
    //     m_respawnTime == 0 && m_spawnedByDefault        -> false (timer is set)
    //
    // So the chest is invisible to every client (isSpawned -> IsInvisibleDueToDespawn
    // -> CanSeeOrDetect) and rejected by every bot search: FindNearestGameObject
    // takes spawnedOnly = true by default, and three further call sites test
    // isSpawned() explicitly. That is why bots walked straight past death chests.
    //
    // The flag also inverts the timer's meaning. GameObject::Update treats the
    // expiry as a DESPAWN only when !m_spawnedByDefault ("Despawn timer" ->
    // GO_JUST_DEACTIVATED); left true it falls through to the RESPAWN path, so
    // the chest would wink into existence after an hour and then stay forever.
    //
    // Order matters: clear the flag first, because SetRespawnTime only publishes
    // the visibility update when it sees !m_spawnedByDefault.
    bool const spawnedBefore = chest->isSpawned();
    bool const byDefaultBefore = chest->isSpawnedByDefault();

    chest->SetSpawnedByDefault(false);
    chest->SetRespawnTime(int32(_despawnTime.count()));

    // Settles, from the running server rather than by argument, whether the
    // above was actually needed: if spawnedBefore is false then a summoned
    // chest really was unfindable by every isSpawned()-filtered search (which
    // includes FindNearestGameObject's default spawnedOnly = true, the call the
    // bots use). If it is true, this whole block is unnecessary and should go.
    TC_LOG_INFO("playerbots.pve",
        "CustomLootChests: chest {} for {} - isSpawned {} -> {}, spawnedByDefault {} -> {}, respawnDelay {}s.",
        _chestEntry, _player->GetName(), spawnedBefore, chest->isSpawned(),
        byDefaultBefore, chest->isSpawnedByDefault(), chest->GetRespawnDelay());

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
    return chest;
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
