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

#ifndef __BATTLEGROUNDCUSTOMARENA_H
#define __BATTLEGROUNDCUSTOMARENA_H

#include "Arena.h"

#include <unordered_map>
#include <vector>

// One class for every ported arena.
//
// BattlegroundTV and BattlegroundTTP are the older shape: a .h/.cpp pair whose
// entire content is four AddObject calls with the gate and buff coordinates
// baked in. Fourteen more copies of that would be fourteen more files differing
// only in twelve numbers each, and every coordinate correction would mean a
// recompile and a Jenkins run.
//
// So the gates and buffs live in `battleground_custom_arena_object` instead and
// this class spawns whatever that table lists for its type id. Consequences
// worth knowing:
//
//   * A coordinate fix is an UPDATE plus `.reload battleground_template`. No
//     rebuild. That matters because the positions were measured from the client
//     terrain rather than surveyed in-game, so some will need nudging.
//   * The shadowsight buff positions can be left out entirely today and filled
//     in later by inserting rows -- an arena with no buff rows simply has no
//     buffs and still works.
//   * An arena with no rows at all still loads and is playable; it just has no
//     gates. Doing nothing is the right failure mode for a missing row here,
//     rather than refusing to create the battleground.

enum BattlegroundCustomArenaObjectType : uint8
{
    CUSTOM_ARENA_OBJECT_DOOR    = 0,
    CUSTOM_ARENA_OBJECT_BUFF    = 1
};

struct BattlegroundCustomArenaObject
{
    uint32 GoEntry;
    float  X;
    float  Y;
    float  Z;
    float  Orientation;
    float  Rot0;
    float  Rot1;
    float  Rot2;
    float  Rot3;
    uint8  Type;
};

class BattlegroundCustomArena : public Arena
{
    public:
        BattlegroundCustomArena();

        void StartingEventCloseDoors() override;
        void StartingEventOpenDoors() override;

        bool SetupBattleground() override;
        void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;

        // Loaded once from the world DB and shared by every instance. Rebuilt by
        // `.reload battleground_template`; running matches keep the objects they
        // already spawned and pick the new layout up next match.
        static void LoadObjects();
        static std::vector<BattlegroundCustomArenaObject> const* GetObjects(BattlegroundTypeId bgTypeId);

    private:
        // Indices into BgObjects are assigned in load order, with all doors
        // before all buffs so the open/close loops stay contiguous.
        uint32 _doorCount;
        uint32 _buffCount;

        static std::unordered_map<uint32 /*bgTypeId*/, std::vector<BattlegroundCustomArenaObject>> _objectStore;
};

#endif // __BATTLEGROUNDCUSTOMARENA_H
