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

#ifndef TRINITYCORE_BATTLEGROUND_SPOILS_H
#define TRINITYCORE_BATTLEGROUND_SPOILS_H

#include "Define.h"

class Player;
struct Loot;

// The battleground spoils chests (Centurion.Battleground.Spoils.*). The chest
// is handed out by Battleground::AwardSpoilsChest; what is IN it is decided
// here, when it is opened.
//
// The loot tables roll a green from the bracket's pool, which alone would hand
// a mage plate. So once the chest's loot is rolled, every green in it is
// swapped for one from the same pool that fits the opener: something their
// class wears (their best armour type) or wields, within a few levels of them,
// whose stats - or random suffix, re-rolled until it fits - are ones the class
// uses.
namespace BattlegroundSpoils
{
    // Reads each chest's pool (item_loot_template -> reference_loot_template).
    // Startup, after the loot tables.
    TC_GAME_API void LoadPools();

    // Called from Player::SendLoot on first opening, before the loot is stored.
    TC_GAME_API void TailorChestLoot(Loot& loot, uint32 chestEntry, Player const* opener);
}

#endif
