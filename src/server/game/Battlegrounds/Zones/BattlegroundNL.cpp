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

#include "BattlegroundNL.h"
#include "GameObject.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"

BattlegroundNL::BattlegroundNL()
{
    BgObjects.resize(BG_NL_OBJECT_MAX);
}

void BattlegroundNL::StartingEventCloseDoors()
{
    SpawnBGObject(BG_NL_OBJECT_DOOR_1, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_NL_OBJECT_DOOR_2, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_NL_OBJECT_WALL_1, RESPAWN_IMMEDIATELY);

    // Hide the start object and Shadow Sight until the arena begins.
    SpawnBGObject(BG_NL_OBJECT_START_OBJECT, RESPAWN_ONE_DAY);

    SpawnBGObject(BG_NL_OBJECT_BUFF_1, RESPAWN_ONE_DAY);
    SpawnBGObject(BG_NL_OBJECT_BUFF_2, RESPAWN_ONE_DAY);

    SetNefarianDoorClosed(BG_NL_OBJECT_DOOR_1);
    SetNefarianDoorClosed(BG_NL_OBJECT_DOOR_2);
    ApplyNonInteractableObjectFlags();
}

void BattlegroundNL::StartingEventOpenDoors()
{
    // BWL portcullis state is visually inverted versus the stock arena door helpers:
    // in the source BWL spawn, state 1 is open and player use toggles it shut.
    SetNefarianDoorOpen(BG_NL_OBJECT_DOOR_1);
    SetNefarianDoorOpen(BG_NL_OBJECT_DOOR_2);

    SpawnBGObject(BG_NL_OBJECT_START_OBJECT, RESPAWN_IMMEDIATELY);

    SpawnBGObject(BG_NL_OBJECT_BUFF_1, 60);
    SpawnBGObject(BG_NL_OBJECT_BUFF_2, 60);

    ApplyNonInteractableObjectFlags();
}

void BattlegroundNL::HandleAreaTrigger(Player* player, uint32 trigger)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    Battleground::HandleAreaTrigger(player, trigger);
}

void BattlegroundNL::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(3002, 1); // BATTLEGROUND_NEFARIAN_ARENA_SHOW

    Arena::FillInitialWorldStates(packet);
}

bool BattlegroundNL::SetupBattleground()
{
    // Gates use the exact rotations from the original BWL gameobjects:
    // 75162 and 2136042.
    if (!AddObject(BG_NL_OBJECT_DOOR_1, BG_NL_OBJECT_TYPE_DOOR,
            -7488.169922f, -1150.540039f, 476.712006f, 0.610865f,
            0.0f, 0.0f, 0.300706f, 0.953717f,
            RESPAWN_IMMEDIATELY, GO_STATE_ACTIVE)
        || !AddObject(BG_NL_OBJECT_DOOR_2, BG_NL_OBJECT_TYPE_DOOR,
            -7591.849121f, -1203.417725f, 476.799896f, 2.171660f,
            -0.0f, -0.0f, -0.884691f, -0.466177f,
            RESPAWN_IMMEDIATELY, GO_STATE_ACTIVE)
        // Permanent wall/blocker, from original BWL gameobject 75164.
        || !AddObject(BG_NL_OBJECT_WALL_1, BG_NL_OBJECT_TYPE_WALL,
            -7464.000000f, -1103.550049f, 480.029999f, 0.610865f,
            0.0f, 0.0f, 0.300706f, 0.953717f,
            RESPAWN_IMMEDIATELY, GO_STATE_READY)
        // Start object, from requested BWL gameobject 35829.
        || !AddObject(BG_NL_OBJECT_START_OBJECT, BG_NL_OBJECT_TYPE_START,
            -7587.760000f, -1261.430000f, 482.000000f, 0.577301f,
            0.0f, 0.0f, 0.284659f, 0.958629f,
            RESPAWN_ONE_DAY)
        // Shadow Sight objects.
        || !AddObject(BG_NL_OBJECT_BUFF_1, BG_NL_OBJECT_TYPE_BUFF_1,
            -7571.823242f, -1250.127808f, 476.800140f, 0.628354f,
            0.0f, 0.0f, 0.309034f, 0.951051f,
            120)
        || !AddObject(BG_NL_OBJECT_BUFF_2, BG_NL_OBJECT_TYPE_BUFF_2,
            -7483.253418f, -1187.455200f, 476.799866f, 3.766021f,
            0.0f, 0.0f, 0.951656f, -0.307167f,
            120))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundNL::SetupBattleground: failed to spawn one or more Nefarian's Arena objects.");
        return false;
    }

    SetNefarianDoorClosed(BG_NL_OBJECT_DOOR_1);
    SetNefarianDoorClosed(BG_NL_OBJECT_DOOR_2);
    ApplyNonInteractableObjectFlags();
    return true;
}

void BattlegroundNL::ApplyNonInteractableObjectFlags()
{
    if (GameObject* door = GetBGObject(BG_NL_OBJECT_DOOR_1, false))
        door->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* door = GetBGObject(BG_NL_OBJECT_DOOR_2, false))
        door->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* wall = GetBGObject(BG_NL_OBJECT_WALL_1, false))
        wall->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* startObject = GetBGObject(BG_NL_OBJECT_START_OBJECT, false))
        startObject->SetFlag(GO_FLAG_NOT_SELECTABLE);
}

void BattlegroundNL::SetNefarianDoorClosed(uint32 type)
{
    if (GameObject* door = GetBGObject(type, false))
    {
        door->SetLootState(GO_READY);
        door->SetGoState(GO_STATE_READY);
    }
}

void BattlegroundNL::SetNefarianDoorOpen(uint32 type)
{
    if (GameObject* door = GetBGObject(type, false))
    {
        door->SetLootState(GO_ACTIVATED);
        door->SetGoState(GO_STATE_ACTIVE);
    }
}
