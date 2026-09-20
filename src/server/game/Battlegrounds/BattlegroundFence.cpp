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

#include "BattlegroundFence.h"
#include "Battleground.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "Map.h"
#include "VMapFactory.h"
#include "VMapManager2.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    // Heights above the arena floor to probe at. A single knee-height ray is
    // stopped by the first railing or step it meets, so the outer wall is
    // whichever of these reaches furthest.
    constexpr float ProbeHeights[] = { 1.5f, 6.0f, 14.0f };

    // The probe never looks further than this from the centre, whatever the
    // start separation suggests, so a fence build stays bounded on a map where
    // the rays find nothing at all.
    constexpr float MinProbeDistance = 60.0f;
    constexpr float MaxProbeDistance = 400.0f;

    // A ray that gets this close to the probe limit is treated as having found
    // no wall: it left the arena rather than bounding it.
    constexpr float ProbeMissFraction = 0.95f;

    // Below this share of sectors finding a wall, the shape is not an
    // enclosure and the fence refuses to be used. Open battlegrounds land here.
    constexpr float MinimumEnclosedFraction = 0.60f;

    // A sector that found no wall is a doorway looking out; give it the 75th
    // percentile plus a little, which seals it without knowing where it is.
    constexpr float DoorwaySealFactor = 1.10f;

    // A sector that DID find a wall keeps what it measured. These two only
    // fence off nonsense - a ray stopped by something up against the centre, or
    // one that escaped through a gap and struck scenery far outside. The floor
    // used to be 0.80 of the reference, which quietly overrode real
    // measurements: where the Imperial Arena's wall stands 55 yd out the fence
    // was raised to 55 and sat LOOSER than the wall it was meant to trace, so a
    // bot a yard past it still read as inside.
    constexpr float MeasuredFloorFactor = 0.50f;
    constexpr float MeasuredCeilingFactor = 1.35f;

    // How far past the measured enclosure a team start may sit before the
    // measurement is judged to be of something other than the boundary.
    constexpr float MaxStartReachFactor = 1.40f;

    constexpr float SectorArc = 2.0f * float(M_PI) / float(BattlegroundFence::SectorCount);

    std::mutex g_FenceCacheLock;
    std::unordered_map<uint64, BattlegroundFence::Fence> g_FenceCache;

    uint64 MakeFenceKey(Battleground const* bg)
    {
        // Map id alone would be enough for every arena on this realm, but a
        // battleground type that ever shares a map with another would then
        // silently inherit the wrong centre. Both, and the question never
        // comes up.
        return (uint64(bg->GetMapId()) << 32) | uint64(bg->GetTypeID());
    }

    float NormalizeAngle(float angle)
    {
        while (angle < 0.0f)
            angle += 2.0f * float(M_PI);
        while (angle >= 2.0f * float(M_PI))
            angle -= 2.0f * float(M_PI);
        return angle;
    }

    // Furthest wall this direction finds, or 0 when every height ran to the
    // probe limit without hitting anything.
    float ProbeSectorRadius(uint32 mapId, float centreX, float centreY, float floorZ, float angle, float probeDistance)
    {
        VMAP::VMapManager2* vmap = VMAP::VMapFactory::createOrGetVMapManager();
        if (!vmap)
            return 0.0f;

        float const dirX = std::cos(angle);
        float const dirY = std::sin(angle);
        float const missThreshold = probeDistance * ProbeMissFraction;

        float best = 0.0f;
        for (float height : ProbeHeights)
        {
            float const startZ = floorZ + height;
            float const endX = centreX + dirX * probeDistance;
            float const endY = centreY + dirY * probeDistance;

            float hitX = endX;
            float hitY = endY;
            float hitZ = startZ;

            // modifyDist 0 keeps the reported point on the surface that was
            // struck. Anything negative would pull the fence inward by that
            // much in every direction, for no benefit here.
            if (!vmap->getObjectHitPos(mapId, centreX, centreY, startZ, endX, endY, startZ, hitX, hitY, hitZ, 0.0f))
                continue;

            float const dx = hitX - centreX;
            float const dy = hitY - centreY;
            float const distance = std::sqrt(dx * dx + dy * dy);
            if (distance >= missThreshold)
                continue;

            best = std::max(best, distance);
        }

        return best;
    }

    float PercentileOf(std::vector<float> sorted, float percentile)
    {
        if (sorted.empty())
            return 0.0f;

        std::sort(sorted.begin(), sorted.end());
        std::size_t const index = std::min(sorted.size() - 1,
            std::size_t(percentile * float(sorted.size())));
        return sorted[index];
    }

    BattlegroundFence::Fence BuildFence(Battleground const* bg)
    {
        BattlegroundFence::Fence fence;

        Map const* map = bg->FindBgMap();
        if (!map)
            return fence;

        Position const* allianceStart = bg->GetTeamStartPosition(TEAM_ALLIANCE);
        Position const* hordeStart = bg->GetTeamStartPosition(TEAM_HORDE);
        if (!allianceStart || !hordeStart)
            return fence;

        float const startSpan = allianceStart->GetExactDist(hordeStart);
        if (startSpan < 1.0f)
            return fence;

        fence.CentreX = (allianceStart->GetPositionX() + hordeStart->GetPositionX()) * 0.5f;
        fence.CentreY = (allianceStart->GetPositionY() + hordeStart->GetPositionY()) * 0.5f;

        // The midpoint of two start positions is not guaranteed to be standable
        // - in a stacked arena it can be mid-air. Search downward from above
        // the higher start for the surface the probe should sit on, and fall
        // back to the start height when the map has nothing there.
        float const searchFrom = std::max(allianceStart->GetPositionZ(), hordeStart->GetPositionZ()) + 10.0f;
        float floorZ = map->GetHeight(fence.CentreX, fence.CentreY, searchFrom, true, 200.0f);
        if (floorZ <= INVALID_HEIGHT)
            floorZ = (allianceStart->GetPositionZ() + hordeStart->GetPositionZ()) * 0.5f;
        fence.CentreZ = floorZ;

        float const probeDistance = std::clamp(startSpan * 1.5f, MinProbeDistance, MaxProbeDistance);

        // The rays read the static vmap tree, which only holds the models of
        // grids the map has loaded, so a fence built too early would measure a
        // half-present arena and then be cached in that shape. Every caller
        // reaches here with both teams standing in their pens, which are at
        // most a couple of hundred yards apart on a 533-yard grid, so the
        // arena's own grids are always in. Gates do not enter into it either
        // way: a gate is a gameobject, and no gameobject is in this tree.
        std::vector<float> hits;
        hits.reserve(BattlegroundFence::SectorCount);

        std::array<float, BattlegroundFence::SectorCount> raw = {};
        for (uint32 sector = 0; sector < BattlegroundFence::SectorCount; ++sector)
        {
            float const angle = float(sector) * SectorArc;
            raw[sector] = ProbeSectorRadius(map->GetId(), fence.CentreX, fence.CentreY, floorZ, angle, probeDistance);
            if (raw[sector] > 0.0f)
                hits.push_back(raw[sector]);
        }

        float const enclosedFraction = float(hits.size()) / float(BattlegroundFence::SectorCount);
        if (enclosedFraction < MinimumEnclosedFraction)
        {
            TC_LOG_INFO("bg.battleground",
                "BattlegroundFence: map {} bg {} has no enclosing geometry ({:.0f}% of {} rays found a wall within {:.1f} yd). No fence.",
                bg->GetMapId(), uint32(bg->GetTypeID()), enclosedFraction * 100.0f, BattlegroundFence::SectorCount, probeDistance);
            return fence;
        }

        float const reference = PercentileOf(hits, 0.75f);
        if (reference < 1.0f)
            return fence;

        // The rays can only report the first enclosure they meet, and on a map
        // that is not arena-shaped that is interior scenery, not the boundary -
        // a chapel in the middle of a field, the buildings around a flag room.
        // The tell is that the measured shape is far too small to hold the
        // battleground's own starting positions. An arena's starts sit at or
        // just inside its wall, so the ratio there is about one; anything that
        // needs the shape stretched by half again to fit the places players
        // spawn measured something else.
        float startReach = 0.0f;
        for (Position const* start : { allianceStart, hordeStart })
        {
            float const dx = start->GetPositionX() - fence.CentreX;
            float const dy = start->GetPositionY() - fence.CentreY;
            startReach = std::max(startReach, std::sqrt(dx * dx + dy * dy));
        }

        if (startReach > reference * MaxStartReachFactor)
        {
            TC_LOG_INFO("bg.battleground",
                "BattlegroundFence: map {} bg {} measured a {:.1f} yd enclosure but its starts reach {:.1f} yd - that is interior geometry, not the boundary. No fence.",
                bg->GetMapId(), uint32(bg->GetTypeID()), reference, startReach);
            return fence;
        }

        float const seal = reference * DoorwaySealFactor;
        float const measuredFloor = reference * MeasuredFloorFactor;
        float const measuredCeiling = reference * MeasuredCeilingFactor;

        for (uint32 sector = 0; sector < BattlegroundFence::SectorCount; ++sector)
        {
            // Trust the measurement wherever there is one. An arena is not a
            // circle - this one's wall is 55 yd out on one bearing and 69 on
            // another - and smoothing every sector toward the average is
            // exactly how a bot ends up standing outside a near wall while the
            // fence reports it comfortably inside an average one.
            fence.SectorRadius[sector] = raw[sector] > 0.0f
                ? std::clamp(raw[sector], measuredFloor, measuredCeiling)
                : seal;
        }

        // A fence that excludes a team's own start position would teleport that
        // team home the instant the gates opened. Widen rather than discard:
        // the shape is still useful, it was just measured from a centre that
        // sits off to one side of a long arena.
        for (Position const* start : { allianceStart, hordeStart })
        {
            float const dx = start->GetPositionX() - fence.CentreX;
            float const dy = start->GetPositionY() - fence.CentreY;
            float const distance = std::sqrt(dx * dx + dy * dy);
            if (distance < 0.01f)
                continue;

            // Just enough room to stand, and only where the start would
            // otherwise fall outside. Widening these sectors unconditionally to
            // 1.15x the start distance pushed them ~10 yd past the wall the
            // rays actually found, which opened a band where a bot is plainly
            // outside the arena and the recovery still calls it inside.
            float const needed = distance + 2.0f;
            float const angle = NormalizeAngle(std::atan2(dy, dx));
            int32 const centreSector = int32(angle / SectorArc);

            // Widen the neighbourhood, not the single sector, so the start is
            // inside with room to stand rather than balanced on the boundary.
            for (int32 offset = -4; offset <= 4; ++offset)
            {
                uint32 const sector = uint32((centreSector + offset + int32(BattlegroundFence::SectorCount)) % int32(BattlegroundFence::SectorCount));
                fence.SectorRadius[sector] = std::max(fence.SectorRadius[sector], needed);
            }
        }

        float const verticalBand = std::max(30.0f, startSpan * 0.5f);
        fence.FloorZ = std::min({ allianceStart->GetPositionZ(), hordeStart->GetPositionZ(), floorZ }) - verticalBand;
        fence.CeilZ = std::max({ allianceStart->GetPositionZ(), hordeStart->GetPositionZ(), floorZ }) + verticalBand;
        fence.ReferenceRadius = reference;
        fence.MapId = map->GetId();
        fence.Usable = true;

        // Which building the arena floor is in, sampled at a team start rather
        // than the geometric centre - the centre can sit over a pit or a gap,
        // while a start is by definition somewhere a player stands. Any hit
        // makes containment exact; none means the floor is terrain and the
        // radial shape above stays in charge.
        if (VMAP::VMapManager2* vmap = VMAP::VMapFactory::createOrGetVMapManager())
        {
            for (Position const* start : { allianceStart, hordeStart })
            {
                float sampleZ = start->GetPositionZ() + 2.0f;
                uint32 areaFlags = 0;
                int32 adtId = 0;
                int32 rootId = -1;
                int32 groupId = 0;
                if (vmap->getAreaInfo(map->GetId(), start->GetPositionX(), start->GetPositionY(),
                    sampleZ, areaFlags, adtId, rootId, groupId) && rootId >= 0)
                {
                    fence.WmoRootId = rootId;
                    break;
                }
            }
        }

        float const smallest = *std::min_element(fence.SectorRadius.begin(), fence.SectorRadius.end());
        float const largest = *std::max_element(fence.SectorRadius.begin(), fence.SectorRadius.end());
        TC_LOG_INFO("bg.battleground",
            "BattlegroundFence: map {} bg {} fenced at ({:.1f}, {:.1f}, {:.1f}) radius {:.1f}-{:.1f} yd (reference {:.1f}, {:.0f}% of rays walled, z {:.1f}..{:.1f}).",
            bg->GetMapId(), uint32(bg->GetTypeID()), fence.CentreX, fence.CentreY, fence.CentreZ,
            smallest, largest, reference, enclosedFraction * 100.0f, fence.FloorZ, fence.CeilZ);

        return fence;
    }
}

namespace BattlegroundFence
{

float Fence::RadiusAt(float x, float y) const
{
    float const dx = x - CentreX;
    float const dy = y - CentreY;
    if (dx * dx + dy * dy < 0.0001f)
        return SectorRadius[0];

    float const angle = NormalizeAngle(std::atan2(dy, dx));
    float const exact = angle / SectorArc;
    uint32 const lower = uint32(exact) % SectorCount;
    uint32 const upper = (lower + 1) % SectorCount;
    float const blend = exact - std::floor(exact);

    // Interpolating between neighbouring sectors keeps the boundary continuous.
    // A stepped boundary would let a bot walking along it cross in and out
    // repeatedly and retrigger the recovery every few yards.
    return SectorRadius[lower] * (1.0f - blend) + SectorRadius[upper] * blend;
}

bool Fence::Contains(float x, float y, float z, float margin) const
{
    if (!Usable)
        return true;

    if (z < FloorZ - margin || z > CeilZ + margin)
        return false;

    // Exact when it applies: the same building as the arena floor, or not in
    // the arena. No radius, no tolerance, and no argument about whether a bot
    // 1.7 yd past a 55 yd sector is really outside - the server either places
    // it in the arena's WMO or it does not.
    if (WmoRootId >= 0)
    {
        VMAP::VMapManager2* vmap = VMAP::VMapFactory::createOrGetVMapManager();
        if (vmap)
        {
            float sampleZ = z + 2.0f;
            uint32 flags = 0;
            int32 adtId = 0;
            int32 rootId = -1;
            int32 groupId = 0;
            if (!vmap->getAreaInfo(MapId, x, y, sampleZ, flags, adtId, rootId, groupId))
                return false;

            return rootId == WmoRootId;
        }
    }

    float const dx = x - CentreX;
    float const dy = y - CentreY;
    float const distance = std::sqrt(dx * dx + dy * dy);
    return distance <= RadiusAt(x, y) + margin;
}

void Fence::NearestInsidePoint(float x, float y, float& outX, float& outY) const
{
    outX = CentreX;
    outY = CentreY;

    if (!Usable)
        return;

    float const dx = x - CentreX;
    float const dy = y - CentreY;
    float const distance = std::sqrt(dx * dx + dy * dy);
    if (distance < 0.01f)
        return;

    // Four fifths of the way in rather than exactly on the boundary: a point
    // on the line is one rounding error from being outside again, and a bot
    // that is pulled back to the wall it just walked through has nowhere to
    // stand.
    float const target = RadiusAt(x, y) * 0.8f;
    if (distance <= target)
    {
        outX = x;
        outY = y;
        return;
    }

    float const scale = target / distance;
    outX = CentreX + dx * scale;
    outY = CentreY + dy * scale;
}

Fence const* GetForBattleground(Battleground const* bg)
{
    if (!bg)
        return nullptr;

    // Off unless a realm asks for it, and re-read on every call so
    // `.reload config` turns it on and off without a restart.
    //
    // It defaulted to on and was read once into a static, which is how a build
    // of an unrelated change carried it onto the live realm - both jobs build
    // the same branch - and pinned every bot inside its arena: they would not
    // follow anyone out through the doors, and only fought whoever came inside.
    // A containment rule that has been wrong twice does not get to be the
    // default, and a switch that needs a restart is not a switch you can use
    // while the thing it controls is misbehaving.
    //
    // A config lookup here is a string map hit, against a fence query that
    // already takes a mutex and can raycast; it does not register.
    if (!sConfigMgr->GetBoolDefault("Centurion.Playerbot.BattlegroundFence.Enable", false))
        return nullptr;

    // The probe rays are traced against the vmap model tree, which holds WMOs
    // and doodads but not the terrain heightmap. An arena's bounds are its WMO,
    // so they are found exactly; an open battleground's bounds are hills and
    // map edges, which are invisible to the probe. That asymmetry means a
    // battleground with enough buildings near its centre - Warsong Gulch's two
    // flag rooms, say - could clear the enclosure test and be handed a fence
    // that has nothing to do with where it actually ends.
    //
    // Arenas are where bots wander out of and where the probe is reliable, so
    // that is the default scope. An enclosed custom battleground (the Violet
    // Hold gauntlet, for one) benefits from the same treatment, but only on
    // purpose and after someone has checked the shape with `.debug bgfence`.
    if (!bg->isArena() && !sConfigMgr->GetBoolDefault("Centurion.Playerbot.BattlegroundFence.NonArenaBattlegrounds", false))
        return nullptr;

    uint64 const key = MakeFenceKey(bg);

    {
        std::lock_guard<std::mutex> guard(g_FenceCacheLock);
        auto const itr = g_FenceCache.find(key);
        if (itr != g_FenceCache.end())
            return itr->second.Usable ? &itr->second : nullptr;
    }

    // Build outside the lock. Two instances of the same arena starting at once
    // may both build; they produce identical fences from identical static
    // collision, so the duplicate is wasted work rather than a hazard.
    Fence built = BuildFence(bg);

    std::lock_guard<std::mutex> guard(g_FenceCacheLock);
    auto const result = g_FenceCache.emplace(key, built);
    return result.first->second.Usable ? &result.first->second : nullptr;
}

bool ClampToFence(Battleground const* bg, Position& destination, float margin)
{
    Fence const* fence = GetForBattleground(bg);
    if (!fence)
        return false;

    if (fence->Contains(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), margin))
        return false;

    float clampedX = destination.GetPositionX();
    float clampedY = destination.GetPositionY();
    fence->NearestInsidePoint(destination.GetPositionX(), destination.GetPositionY(), clampedX, clampedY);

    float clampedZ = std::clamp(destination.GetPositionZ(), fence->FloorZ, fence->CeilZ);
    destination.Relocate(clampedX, clampedY, clampedZ, destination.GetOrientation());
    return true;
}

void InvalidateCache()
{
    std::lock_guard<std::mutex> guard(g_FenceCacheLock);
    g_FenceCache.clear();
}

}
