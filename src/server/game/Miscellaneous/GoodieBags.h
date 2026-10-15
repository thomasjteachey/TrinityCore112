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

#ifndef TRINITYCORE_GOODIE_BAGS_H
#define TRINITYCORE_GOODIE_BAGS_H

#include "Define.h"

class Creature;
class InstanceMap;
class Unit;

// The goodie bags (Centurion.GoodieBag.*). Every character at MinLevel or
// above who is in a dungeon or raid when its last boss dies gets one: a
// random piece of that instance's boss loot, some gold and Centurion Marks
// of Honor.
//
// The gold and the marks are the bag's own item_template / item_loot_template
// rows, so they are data. The piece is not, because it depends on where the
// bag came from: it is rolled here when the bag is handed out and stored with
// the bag (item_loot_items), the way TrinityCore keeps a half-looted
// container, so opening it now or after a relog shows the same loot.
namespace GoodieBags
{
    // Centurion.GoodieBag.*. World::LoadConfigSettings, so `.reload config`
    // applies it too.
    TC_GAME_API void LoadConfig();

    // Which boss loot tables each instance's pieces are rolled from. Startup,
    // after the creature spawns, the instance encounters and the loot tables.
    TC_GAME_API void LoadPools();

    // The instance's final encounter is done: InstanceScript::UpdateEncounterState
    // found the encounter's lastEncounterDungeon. `source` is the boss, when
    // the credit came from a kill.
    TC_GAME_API void OnInstanceFinished(InstanceMap* instance, Unit const* source);

    // Every creature a player kills. Finishes the instance for the bosses
    // named in Centurion.GoodieBag.FinalBosses - content whose encounters
    // DungeonEncounter.dbc does not know (the 40-player Onyxia and Naxxramas).
    TC_GAME_API void OnCreatureKilled(Creature* victim);
}

#endif
