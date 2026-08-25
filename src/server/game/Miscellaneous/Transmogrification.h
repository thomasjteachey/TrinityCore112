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

#ifndef TRINITYCORE_TRANSMOGRIFICATION_H
#define TRINITYCORE_TRANSMOGRIFICATION_H

#include "Define.h"
#include <string>
#include <vector>

class Item;
class Player;
struct ItemTemplate;

namespace Trinity::Transmog
{
    // Why a source item was rejected. Used to give the player a useful message
    // instead of a silent "no".
    enum class Result : uint8
    {
        Ok,
        Disabled,
        BadSlot,
        NoTargetItem,
        NoSourceItem,
        SameAppearance,
        ClassMismatch,       // armor onto weapon and the like
        SlotMismatch,        // 2H onto 1H, ranged onto melee, ...
        ArmorTypeMismatch,   // plate onto cloth
        WeaponTypeMismatch,  // axe onto sword
        NoDisplay,
        QualityNotAllowed,
        NotUsable,           // the player could never wear the source item
        NotEnoughTokens
    };

    [[nodiscard]] bool IsEnabled();

    // Equipment slots that can carry an appearance (everything visible: no neck,
    // rings or trinkets).
    [[nodiscard]] bool IsTransmogSlot(uint8 slot);

    // Can `target` (an item equipped by `player`) take `source`'s appearance?
    [[nodiscard]] Result CanTransmogrify(Player const* player, Item const* target, ItemTemplate const* source);

    // Every item in the player's bags/bank whose appearance could be applied to
    // whatever is currently equipped in `slot`. One entry per item template, so
    // duplicates of the same item do not spam the menu.
    void CollectSourcesForSlot(Player const* player, uint8 slot, std::vector<ItemTemplate const*>& out);

    [[nodiscard]] uint32 GetTokenEntry();
    [[nodiscard]] uint32 GetTokenCost();
    [[nodiscard]] std::string GetTokenName();
    [[nodiscard]] bool HasEnoughTokens(Player const* player);
    bool TakeTokens(Player* player);

    [[nodiscard]] char const* GetResultText(Result result);

    // Item name wrapped in the client's quality colour code, for gossip lines.
    [[nodiscard]] std::string GetColoredName(ItemTemplate const* proto);
}

#endif // TRINITYCORE_TRANSMOGRIFICATION_H
