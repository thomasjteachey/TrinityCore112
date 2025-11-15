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

#ifndef TRINITYCORE_DEPLETED_MARKS_H
#define TRINITYCORE_DEPLETED_MARKS_H

#include "Define.h"
#include <array>

class Player;

namespace Trinity::Custom
{
    inline constexpr uint32 ITEM_RESTORED_MARK_OF_HONOR = 20558;
    inline constexpr uint32 DEPLETED_MARK_FIRST_ENTRY    = 20559;
    inline constexpr uint32 DEPLETED_MARK_LAST_ENTRY     = 20575;
    inline constexpr uint32 DEPLETED_MARK_CONVERSION_COST = 3;

    inline constexpr std::array<uint32, DEPLETED_MARK_LAST_ENTRY - DEPLETED_MARK_FIRST_ENTRY + 1> DepletedMarkEntries =
    {
        20559, 20560, 20561, 20562, 20563, 20564, 20565, 20566, 20567,
        20568, 20569, 20570, 20571, 20572, 20573, 20574, 20575
    };

    uint32 GetTotalDepletedMarkCount(Player const* player, bool includeBank = false);
    bool HasEnoughDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank = false);
    bool ConsumeDepletedMarks(Player* player, uint32 amount);
    uint32 GetTotalIneligibleDepletedMarkCount(Player const* player, bool includeBank = false);
    bool HasEnoughIneligibleDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank = false);
    bool ConsumeIneligibleDepletedMarks(Player* player, uint32 amount);
    uint32 GetDepletedMarkEntryForPlayer(Player const* player);
}

#endif // TRINITYCORE_DEPLETED_MARKS_H

