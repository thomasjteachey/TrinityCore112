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

#ifndef TRINITY_TICK_STATS_H
#define TRINITY_TICK_STATS_H

#include "Define.h"

#include <vector>

// Rolling world-tick and per-map update timings, read by the .tick GM command.
//
// `.server info` only ever shows ONE tick, and the world tick is the slowest
// map (MapManager::Update waits for every scheduled map), so a single number
// cannot say whether the realm is steadily slow or which map is holding it
// back. This keeps a few seconds of both.
namespace TickStats
{
struct WorldWindow
{
    uint32 ticks = 0;
    uint32 spanMs = 0;
    uint32 lastMs = 0;
    uint32 avgMs = 0;
    uint32 p95Ms = 0;
    uint32 maxMs = 0;

    // Mean time per tick spent in each World::Update phase over the same ticks.
    uint32 sessionsMs = 0;
    uint32 mapsMs = 0;
    uint32 battlegroundsMs = 0;
    uint32 scriptsMs = 0;
};

struct MapTiming
{
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 updates = 0;
    uint32 lastMs = 0;
    uint32 avgMs = 0;
    uint32 maxMs = 0;
};

// World thread, once per World::Update. diffMs is the tick the governor and
// `.server info` read; the phases are the work measured inside it.
TC_GAME_API void RecordWorldTick(uint32 diffMs, uint32 sessionsMs, uint32 mapsMs, uint32 battlegroundsMs, uint32 scriptsMs);

// Any map thread, once per Map::Update of a map, instance or lobby sub-map.
TC_GAME_API void RecordMapUpdate(uint32 mapId, uint32 instanceId, uint32 elapsedMs);

// World thread only (commands and world scripts run there).
TC_GAME_API WorldWindow GetWorldWindow(uint32 windowMs);

// Maps updated within the window, slowest average first.
TC_GAME_API std::vector<MapTiming> GetMapTimings(uint32 windowMs);
}

#endif
