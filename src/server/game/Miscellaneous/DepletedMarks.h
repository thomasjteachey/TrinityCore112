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
#include <span>

class Player;

namespace Trinity::Custom
{
    inline constexpr uint32 DEPLETED_MARK_CONVERSION_COST = 3;
    inline constexpr std::size_t MAX_DEPLETED_MARK_ENTRIES = 32;

    // The Legionnaire Mark of Honor and its depleted class/race variants are
    // item ids chosen per realm (Centurion.Marks.*). Legionnaire+ and
    // Barracks+ keep the stock ids L+ reused (20558, 20559-20575); Centurion
    // points at its own copies, because there those stock ids are B+'s
    // battleground marks, Hallow's End masks and Black Whelp Tunic.
    // Loaded from World::LoadConfigSettings, so `.reload config` applies it.
    void LoadMarkConfig();
    uint32 GetRestoredMarkEntry();
    std::span<uint32 const> GetDepletedMarkEntries();

    uint32 GetTotalDepletedMarkCount(Player const* player, bool includeBank = false);
    bool HasEnoughDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank = false);
    bool ConsumeDepletedMarks(Player* player, uint32 amount);
    uint32 GetTotalIneligibleDepletedMarkCount(Player const* player, bool includeBank = false);
    bool HasEnoughIneligibleDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank = false);
    bool ConsumeIneligibleDepletedMarks(Player* player, uint32 amount);
    uint32 GetTotalEligibleDepletedMarkCount(Player const* player, bool includeBank = false);
    bool HasEnoughEligibleDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank = false);
    bool ConsumeEligibleDepletedMarks(Player* player, uint32 amount);
    uint32 GetOwnedEligibleDepletedMarkEntry(Player const* player);
    uint32 GetDepletedMarkEntryForPlayer(Player const* player);
}

#endif // TRINITYCORE_DEPLETED_MARKS_H

