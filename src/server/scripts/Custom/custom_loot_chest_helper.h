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
#include <vector>

namespace CustomLootChests
{
class PlayerChestBuilder
{
public:
    PlayerChestBuilder(Player* player, uint32 chestEntry, Seconds despawnTime);

    bool HasLoot() const { return !_items.empty(); }
    void AddStackableItem(uint32 itemId, uint32 count);
    void AddItem(Item* item);

    GameObject* Summon() const;

private:
    LootItem CreateLootItem(uint32 itemId, uint8 count, uint32 randomSuffix, int32 randomPropertyId) const;

    Player* _player = nullptr;
    uint32 _chestEntry = 0;
    Seconds _despawnTime = Seconds(0);
    mutable std::vector<LootItem> _items;
};
}

#endif // CUSTOM_LOOT_CHEST_HELPER_H
