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

#include "BattlegroundCustomArena.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"

std::unordered_map<uint32, std::vector<BattlegroundCustomArenaObject>> BattlegroundCustomArena::_objectStore;

BattlegroundCustomArena::BattlegroundCustomArena() : _doorCount(0), _buffCount(0)
{
    // BgObjects cannot be sized here: the type id is assigned by SetTypeID()
    // after construction, so which arena this is -- and therefore how many gates
    // it has -- is not known yet. SetupBattleground() sizes it instead, and is
    // always called on the instance after the type id is in place.
}

void BattlegroundCustomArena::LoadObjects()
{
    uint32 oldMSTime = getMSTime();

    _objectStore.clear();

    //                                                   0          1        2  3  4  5            6          7          8          9          10
    QueryResult result = WorldDatabase.Query("SELECT BgTypeId, GoEntry, X, Y, Z, Orientation, Rotation0, Rotation1, Rotation2, Rotation3, ObjectType "
                                             "FROM battleground_custom_arena_object ORDER BY BgTypeId, ObjectType, Id");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 custom arena objects. DB table `battleground_custom_arena_object` is empty.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 bgTypeId = fields[0].GetUInt32();
        if (!IsDataDrivenArena(BattlegroundTypeId(bgTypeId)))
        {
            TC_LOG_ERROR("sql.sql", "Table `battleground_custom_arena_object` has a row for BgTypeId {}, which is not a data-driven arena. Skipped.", bgTypeId);
            continue;
        }

        BattlegroundCustomArenaObject obj;
        obj.GoEntry     = fields[1].GetUInt32();
        obj.X           = fields[2].GetFloat();
        obj.Y           = fields[3].GetFloat();
        obj.Z           = fields[4].GetFloat();
        obj.Orientation = fields[5].GetFloat();
        obj.Rot0        = fields[6].GetFloat();
        obj.Rot1        = fields[7].GetFloat();
        obj.Rot2        = fields[8].GetFloat();
        obj.Rot3        = fields[9].GetFloat();
        obj.Type        = fields[10].GetUInt8();

        if (obj.Type != CUSTOM_ARENA_OBJECT_DOOR && obj.Type != CUSTOM_ARENA_OBJECT_BUFF)
        {
            TC_LOG_ERROR("sql.sql", "Table `battleground_custom_arena_object` row for BgTypeId {} has unknown ObjectType {}. Skipped.", bgTypeId, uint32(obj.Type));
            continue;
        }

        if (!sObjectMgr->GetGameObjectTemplate(obj.GoEntry))
        {
            TC_LOG_ERROR("sql.sql", "Table `battleground_custom_arena_object` row for BgTypeId {} uses GoEntry {}, which is not in `gameobject_template`. Skipped.", bgTypeId, obj.GoEntry);
            continue;
        }

        _objectStore[bgTypeId].push_back(obj);
        ++count;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} custom arena objects for {} arenas in {} ms",
        count, uint32(_objectStore.size()), GetMSTimeDiffToNow(oldMSTime));
}

std::vector<BattlegroundCustomArenaObject> const* BattlegroundCustomArena::GetObjects(BattlegroundTypeId bgTypeId)
{
    auto itr = _objectStore.find(uint32(bgTypeId));
    return itr != _objectStore.end() ? &itr->second : nullptr;
}

bool BattlegroundCustomArena::SetupBattleground()
{
    // GetTypeID(true) rather than GetTypeID(): a player who queued for All
    // Arenas has m_TypeID == BATTLEGROUND_AA, and only the random type id says
    // which arena was actually rolled.
    BattlegroundTypeId const bgTypeId = GetTypeID(true);

    std::vector<BattlegroundCustomArenaObject> const* objects = GetObjects(bgTypeId);
    if (!objects || objects->empty())
    {
        // Deliberately not a failure. An arena with no gate rows is playable --
        // it just starts open -- and refusing to create the battleground would
        // turn a missing row into a queue that pops and then breaks.
        TC_LOG_DEBUG("bg.battleground", "BattlegroundCustomArena: no objects configured for bgTypeId {}; running without gates.", uint32(bgTypeId));
        _doorCount = 0;
        _buffCount = 0;
        BgObjects.resize(0);
        return true;
    }

    BgObjects.resize(objects->size());

    _doorCount = 0;
    _buffCount = 0;

    // The query orders by ObjectType, so all doors precede all buffs and the
    // open/close loops below can walk contiguous index ranges.
    uint32 index = 0;
    for (BattlegroundCustomArenaObject const& obj : *objects)
    {
        if (!AddObject(index, obj.GoEntry, obj.X, obj.Y, obj.Z, obj.Orientation,
                       obj.Rot0, obj.Rot1, obj.Rot2, obj.Rot3,
                       obj.Type == CUSTOM_ARENA_OBJECT_DOOR ? RESPAWN_IMMEDIATELY : 120))
        {
            TC_LOG_ERROR("sql.sql", "BattlegroundCustomArena: failed to spawn object entry {} for bgTypeId {}.", obj.GoEntry, uint32(bgTypeId));
            return false;
        }

        if (obj.Type == CUSTOM_ARENA_OBJECT_DOOR)
            ++_doorCount;
        else
            ++_buffCount;

        ++index;
    }

    return true;
}

void BattlegroundCustomArena::StartingEventCloseDoors()
{
    for (uint32 i = 0; i < _doorCount; ++i)
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);

    for (uint32 i = _doorCount; i < _doorCount + _buffCount; ++i)
        SpawnBGObject(i, RESPAWN_ONE_DAY);
}

void BattlegroundCustomArena::StartingEventOpenDoors()
{
    for (uint32 i = 0; i < _doorCount; ++i)
        DoorOpen(i);

    for (uint32 i = _doorCount; i < _doorCount + _buffCount; ++i)
        SpawnBGObject(i, 60);
}

void BattlegroundCustomArena::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    // Turns on the "N players remaining" readout in the arena frame. Without it
    // the client receives the alive counts that Arena::FillInitialWorldStates
    // sends but never displays them.
    packet.Worldstates.emplace_back(3610, 1); // ARENA_WORLD_STATE_ALIVE_PLAYERS_SHOW

    Arena::FillInitialWorldStates(packet);
}
