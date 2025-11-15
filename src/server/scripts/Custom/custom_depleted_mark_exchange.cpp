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
#include "Item.h"
#include "Miscellaneous/DepletedMarks.h"
#include "Player.h"

class item_depleted_mark_converter : public ItemScript
{
public:
    item_depleted_mark_converter() : ItemScript("item_depleted_mark_converter") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& /*targets*/) override
    {
        if (!player || !item)
            return false;

        uint32 const rewardEntry = Trinity::Custom::GetDepletedMarkEntryForPlayer(player);
        if (!rewardEntry)
        {
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, item, nullptr);
            return false;
        }

        if (!Trinity::Custom::HasEnoughIneligibleDepletedMarks(player, Trinity::Custom::DEPLETED_MARK_CONVERSION_COST))
        {
            player->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, item, nullptr);
            return false;
        }

        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, rewardEntry, 1) != EQUIP_ERR_OK)
        {
            player->SendEquipError(EQUIP_ERR_INVENTORY_FULL, nullptr, nullptr, rewardEntry);
            return false;
        }

        if (!Trinity::Custom::ConsumeIneligibleDepletedMarks(player, Trinity::Custom::DEPLETED_MARK_CONVERSION_COST))
        {
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, item, nullptr);
            return false;
        }

        if (Item* newItem = player->StoreNewItem(dest, rewardEntry, true))
            player->SendNewItem(newItem, 1, true, false);

        return true;
    }
};

void AddSC_custom_depleted_mark_exchange()
{
    new item_depleted_mark_converter();
}
