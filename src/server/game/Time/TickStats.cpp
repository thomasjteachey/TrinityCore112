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

#include "TickStats.h"

#include "Timer.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>

namespace
{
struct WorldSample
{
    uint32 atMs;
    uint32 diffMs;
    uint32 sessionsMs;
    uint32 mapsMs;
    uint32 battlegroundsMs;
    uint32 scriptsMs;
};

// 4096 ticks is ~2.5 minutes at a 40 ms tick; an idle realm ticking every
// millisecond covers only a few seconds, which is all anyone needs there.
constexpr size_t WorldRingSize = 4096;
std::array<WorldSample, WorldRingSize> g_WorldRing{};
size_t g_WorldNext = 0;
size_t g_WorldCount = 0;

struct MapSample
{
    uint32 atMs;
    uint32 elapsedMs;
};

constexpr size_t MapRingSize = 512;

struct MapRing
{
    std::array<MapSample, MapRingSize> samples{};
    size_t next = 0;
    size_t count = 0;
    uint32 lastSeenMs = 0;
};

// Map threads write concurrently, so the per-map rings sit behind one mutex.
// One lock per map update is noise next to the update itself.
std::mutex g_MapLock;
std::unordered_map<uint64, MapRing> g_MapRings;

// Maps unloaded (instances closed, battlegrounds ended) drop out after this.
constexpr uint32 MapForgetMs = 60 * 1000;

uint64 MapKey(uint32 mapId, uint32 instanceId)
{
    return (uint64(mapId) << 32) | instanceId;
}

uint32 Percentile95(std::vector<uint32>& values)
{
    if (values.empty())
        return 0;

    size_t const index = std::min(values.size() - 1, values.size() * 95 / 100);
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}
}

namespace TickStats
{
void RecordWorldTick(uint32 diffMs, uint32 sessionsMs, uint32 mapsMs, uint32 battlegroundsMs, uint32 scriptsMs)
{
    g_WorldRing[g_WorldNext] = { getMSTime(), diffMs, sessionsMs, mapsMs, battlegroundsMs, scriptsMs };
    g_WorldNext = (g_WorldNext + 1) % WorldRingSize;
    if (g_WorldCount < WorldRingSize)
        ++g_WorldCount;
}

void RecordMapUpdate(uint32 mapId, uint32 instanceId, uint32 elapsedMs)
{
    uint32 const nowMs = getMSTime();

    std::lock_guard<std::mutex> guard(g_MapLock);
    MapRing& ring = g_MapRings[MapKey(mapId, instanceId)];
    ring.samples[ring.next] = { nowMs, elapsedMs };
    ring.next = (ring.next + 1) % MapRingSize;
    if (ring.count < MapRingSize)
        ++ring.count;
    ring.lastSeenMs = nowMs;
}

WorldWindow GetWorldWindow(uint32 windowMs)
{
    WorldWindow window;
    if (!g_WorldCount)
        return window;

    uint32 const nowMs = getMSTime();
    std::vector<uint32> diffs;
    diffs.reserve(g_WorldCount);

    uint64 diffSum = 0, sessionsSum = 0, mapsSum = 0, battlegroundsSum = 0, scriptsSum = 0;
    uint32 oldestMs = nowMs;
    for (size_t i = 0; i < g_WorldCount; ++i)
    {
        WorldSample const& sample = g_WorldRing[(g_WorldNext + WorldRingSize - 1 - i) % WorldRingSize];
        if (getMSTimeDiff(sample.atMs, nowMs) > windowMs)
            break;

        if (i == 0)
            window.lastMs = sample.diffMs;

        diffs.push_back(sample.diffMs);
        diffSum += sample.diffMs;
        sessionsSum += sample.sessionsMs;
        mapsSum += sample.mapsMs;
        battlegroundsSum += sample.battlegroundsMs;
        scriptsSum += sample.scriptsMs;
        window.maxMs = std::max(window.maxMs, sample.diffMs);
        oldestMs = sample.atMs;
    }

    window.ticks = uint32(diffs.size());
    if (!window.ticks)
        return window;

    window.spanMs = getMSTimeDiff(oldestMs, nowMs);
    window.avgMs = uint32(diffSum / window.ticks);
    window.sessionsMs = uint32(sessionsSum / window.ticks);
    window.mapsMs = uint32(mapsSum / window.ticks);
    window.battlegroundsMs = uint32(battlegroundsSum / window.ticks);
    window.scriptsMs = uint32(scriptsSum / window.ticks);
    window.p95Ms = Percentile95(diffs);
    return window;
}

std::vector<MapTiming> GetMapTimings(uint32 windowMs)
{
    uint32 const nowMs = getMSTime();
    std::vector<MapTiming> timings;

    std::lock_guard<std::mutex> guard(g_MapLock);
    for (auto itr = g_MapRings.begin(); itr != g_MapRings.end();)
    {
        MapRing const& ring = itr->second;
        if (getMSTimeDiff(ring.lastSeenMs, nowMs) > MapForgetMs)
        {
            itr = g_MapRings.erase(itr);
            continue;
        }

        MapTiming timing;
        timing.mapId = uint32(itr->first >> 32);
        timing.instanceId = uint32(itr->first & 0xFFFFFFFF);

        uint64 sum = 0;
        for (size_t i = 0; i < ring.count; ++i)
        {
            MapSample const& sample = ring.samples[(ring.next + MapRingSize - 1 - i) % MapRingSize];
            if (getMSTimeDiff(sample.atMs, nowMs) > windowMs)
                break;

            if (i == 0)
                timing.lastMs = sample.elapsedMs;

            ++timing.updates;
            sum += sample.elapsedMs;
            timing.maxMs = std::max(timing.maxMs, sample.elapsedMs);
        }

        if (timing.updates)
        {
            timing.avgMs = uint32(sum / timing.updates);
            timings.push_back(timing);
        }
        ++itr;
    }

    std::sort(timings.begin(), timings.end(), [](MapTiming const& left, MapTiming const& right)
    {
        return left.avgMs != right.avgMs ? left.avgMs > right.avgMs : left.maxMs > right.maxMs;
    });
    return timings;
}
}
