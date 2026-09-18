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

#ifndef VANILLA_RAIDS_H
#define VANILLA_RAIDS_H

#include "Define.h"
#include "SharedDefines.h"

class Map;
class Player;
class WorldObject;

// Vanilla (patch 1.12 / level 60) Naxxramas and Onyxia, living alongside the
// Wrath level 80 versions of the same two maps rather than replacing them.
//
// Both raids ride on raid difficulty 2 (RAID_DIFFICULTY_10MAN_HEROIC), a slot
// neither map uses in Wrath: Naxxramas has no heroic mode and Onyxia's Lair
// only ships 10- and 25-player normal. So difficulty 0 and 1 stay exactly as
// they are - the level 80 raids are reachable at all times - and difficulty 2
// is the 40-player level 60 version, with its own creature entries, its own
// spawns (spawnMask bit 4) and its own scripts.
//
// Every runtime behaviour below is off unless VanillaRaids.Enable is set, so
// the whole feature can be taken back out of the realm with one config line.
//
// Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0);
// Naxxramas 40 scripts and data originally by Sogladev.
namespace VanillaRaids
{
    inline constexpr char const* LogFilter = "module.vanillaraids";

    // The difficulty the 40-player versions live on.
    inline constexpr Difficulty VanillaRaidDifficulty = RAID_DIFFICULTY_10MAN_HEROIC;

    inline constexpr uint32 MAP_NAXXRAMAS      = 533;
    inline constexpr uint32 MAP_ONYXIAS_LAIR   = 249;

    // Vanilla Naxxramas attunement chain (Argent Dawn). Any one of the three
    // rewarded means attuned - they are the three faction-specific versions.
    inline constexpr uint32 QUEST_NAXX40_ATTUNEMENT_1 = 9121;
    inline constexpr uint32 QUEST_NAXX40_ATTUNEMENT_2 = 9122;
    inline constexpr uint32 QUEST_NAXX40_ATTUNEMENT_3 = 9123;
    // Flag quest handed out on first entry through the Stratholme side.
    inline constexpr uint32 QUEST_NAXX40_ENTRANCE_FLAG = 9378;

    // WorldSafeLocs row for the vanilla Naxxramas graveyard. The zone's own
    // entry belongs to the Wrath wing and would send a level 60 raid to the
    // wrong side of the necropolis.
    inline constexpr uint32 NAXX40_GRAVEYARD = 909;

    void LoadConfig();

    // Master gate. False means nothing in this namespace does anything and the
    // 40-player scripts refuse to run, leaving the level 80 raids untouched.
    bool Enabled();
    bool Naxx40Enabled();
    bool Onyxia40Enabled();

    // Entry rules, each independently switchable.
    bool AttunementRequired();
    bool StratholmeEntranceRequired();
    uint8 MaxEntryLevel();

    // "Doable" toggles from the upstream module: they soften four encounters
    // that a short or undergeared raid cannot otherwise clear. Off by default,
    // which is the authentic 1.12 tuning.
    bool NerfFourHorsemen();
    bool NerfPatchwerk();
    bool NerfRazuvious();
    bool NerfGluth();

    // True only when this map instance is the 40-player version of that raid,
    // i.e. the master gate is on, the per-raid gate is on, and the map is
    // running at VanillaRaidDifficulty.
    bool IsNaxx40Map(Map const* map);
    bool IsOnyxia40Map(Map const* map);
    // Either of the above - for scripts shared between both raids.
    bool IsVanillaRaidMap(Map const* map);

    // Convenience for creature AI: the map the object is standing in.
    bool IsNaxx40(WorldObject const* object);
    bool IsOnyxia40(WorldObject const* object);

    bool IsAttuned(Player const* player);
    // Playerbots are exempt from the entry checks - they cannot pick up an
    // attunement quest, and a raid of clones would never form otherwise.
    bool IsBotAccount(Player const* player);
}

#endif // VANILLA_RAIDS_H
