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

#include "Miscellaneous/DepletedMarks.h"
#include "Entities/Player/Player.h"
#include "ObjectMgr.h"
#include <algorithm>

namespace Trinity::Custom
{
namespace
{
bool MaskMatches(uint32 allowedMask, uint32 playerMask)
{
    return allowedMask == 0 || allowedMask == uint32(-1) || (allowedMask & playerMask);
}

uint8 GetSpecificityScore(uint32 allowedMask, uint32 playerMask)
{
    if (allowedMask == 0 || allowedMask == uint32(-1))
        return 0;

    if (allowedMask == playerMask)
        return 2;

    if (allowedMask & playerMask)
        return 1;

    return 0;
}

bool IsTemplateEligibleForPlayer(ItemTemplate const* itemTemplate, uint32 classMask, uint32 raceMask)
{
    return MaskMatches(itemTemplate->AllowableClass, classMask) && MaskMatches(itemTemplate->AllowableRace, raceMask);
}
}

uint32 GetTotalDepletedMarkCount(Player const* player, bool includeBank)
{
    if (!player)
        return 0;

    uint32 total = 0;
    for (uint32 entry : DepletedMarkEntries)
        total += player->GetItemCount(entry, includeBank);

    return total;
}

bool HasEnoughDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank)
{
    return GetTotalDepletedMarkCount(player, includeBank) >= requiredCount;
}

bool ConsumeDepletedMarks(Player* player, uint32 amount)
{
    if (!player || amount == 0)
        return false;

    if (!HasEnoughDepletedMarks(player, amount, false))
        return false;

    for (uint32 entry : DepletedMarkEntries)
    {
        if (amount == 0)
            break;

        uint32 available = player->GetItemCount(entry);
        if (!available)
            continue;

        uint32 toRemove = std::min(amount, available);
        player->DestroyItemCount(entry, toRemove, true);
        amount -= toRemove;
    }

    return amount == 0;
}

uint32 GetTotalIneligibleDepletedMarkCount(Player const* player, bool includeBank)
{
    if (!player)
        return 0;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    uint32 total = 0;
    for (uint32 entry : DepletedMarkEntries)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        total += player->GetItemCount(entry, includeBank);
    }

    return total;
}

bool HasEnoughIneligibleDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank)
{
    return GetTotalIneligibleDepletedMarkCount(player, includeBank) >= requiredCount;
}

bool ConsumeIneligibleDepletedMarks(Player* player, uint32 amount)
{
    if (!player || amount == 0)
        return false;

    if (!HasEnoughIneligibleDepletedMarks(player, amount, false))
        return false;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    for (uint32 entry : DepletedMarkEntries)
    {
        if (amount == 0)
            break;

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        uint32 available = player->GetItemCount(entry);
        if (!available)
            continue;

        uint32 toRemove = std::min(amount, available);
        player->DestroyItemCount(entry, toRemove, true);
        amount -= toRemove;
    }

    return amount == 0;
}

uint32 GetDepletedMarkEntryForPlayer(Player const* player)
{
    if (!player)
        return 0;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    uint32 bestEntry = 0;
    uint8 bestScore = 0;

    for (uint32 entry : DepletedMarkEntries)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (!IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        uint8 const classScore = GetSpecificityScore(itemTemplate->AllowableClass, classMask);
        uint8 const raceScore = GetSpecificityScore(itemTemplate->AllowableRace, raceMask);
        uint8 const score = classScore * 3 + raceScore;

        if (score > bestScore)
        {
            bestScore = score;
            bestEntry = entry;
        }
    }

    return bestEntry;
}
}

