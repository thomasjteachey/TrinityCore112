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

#ifndef CUSTOM_LOOT_CHEST_HELPER_H
#define CUSTOM_LOOT_CHEST_HELPER_H

#include "Duration.h"
#include "GameObject.h"
#include "Loot.h"
#include "Player.h"
#include "SharedDefines.h"
#include <unordered_set>
#include <vector>

namespace CustomLootChests
{
struct ItemLocation
{
    uint8 Bag = INVENTORY_SLOT_BAG_0;
    uint8 Slot = 0;
};

class PlayerChestBuilder
{
public:
    PlayerChestBuilder(Player* player, uint32 chestEntry, Seconds despawnTime);

    bool HasLoot() const { return !_items.empty() || _money > 0; }
    void AddStackableItem(uint32 itemId, uint32 count);
    void AddItem(Item* item);
    // Coin in the chest, in copper. A chest carrying only money is still
    // worth summoning - a bounty payout usually has nothing else in it.
    void AddMoney(uint32 copper) { _money += copper; }
    uint32 GetMoney() const { return _money; }

    GameObject* Summon() const;

private:
    LootItem CreateLootItem(uint32 itemId, uint8 count, uint32 randomSuffix, int32 randomPropertyId) const;

    Player* _player = nullptr;
    uint32 _chestEntry = 0;
    Seconds _despawnTime = Seconds(0);
    uint32 _money = 0;
    mutable std::vector<LootItem> _items;
};

void CollectItemsWithQuality(Player* player, ItemQualities quality, PlayerChestBuilder& chest, std::vector<ItemLocation>& removedItems,
    std::unordered_set<uint32> const& excludedEntries = {});

// ---------------------------------------------------------------------------
// Live chest registry.
//
// Every death chest in the world was created by code in this file, so nothing
// has to go looking for one. Bots previously ran a grid search on a cadence to
// notice chests near them, which is real work on the map update thread for an
// answer we already knew at summon time. Chests announce themselves here
// instead, and a lookup is a linear pass over a handful of records.
//
// Entries are pruned by their own despawn deadline; a caller that resolves a
// guid to nothing should call ForgetChest so a looted or despawned chest stops
// being offered.
// ---------------------------------------------------------------------------
struct ChestLocation
{
    ObjectGuid Guid;
    uint32 Entry = 0;
    uint32 MapId = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    time_t ExpiresAt = 0;
};

void RegisterChest(GameObject* chest, Seconds despawnTime);
void ForgetChest(ObjectGuid guid);

// Nearest live chest on this map within maxDistance, or false. Does not touch
// the grid: the answer comes from the registry.
// Entry is matched exactly: more than one system builds chests through
// PlayerChestBuilder (hardcore death loot, Dire Maul beads), and they must not
// be offered to each other's seekers.
bool FindNearestChest(uint32 entry, uint32 mapId, float x, float y, float z, float maxDistance, ObjectGuid& outGuid);
}

#endif // CUSTOM_LOOT_CHEST_HELPER_H
