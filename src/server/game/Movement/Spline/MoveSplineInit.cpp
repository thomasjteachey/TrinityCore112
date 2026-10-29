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

#include "MoveSplineInit.h"
#include "Battleground.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MovementPacketBuilder.h"
#include "Unit.h"
#include "Player.h"
#include "PathGenerator.h"
#include "Transport.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Movement
{
    UnitMoveType SelectSpeedType(uint32 moveFlags)
    {
        if (moveFlags & MOVEMENTFLAG_FLYING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.flight >= speed_obj.flight_back*/)
                return MOVE_FLIGHT_BACK;
            else
                return MOVE_FLIGHT;
        }
        else if (moveFlags & MOVEMENTFLAG_SWIMMING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.swim >= speed_obj.swim_back*/)
                return MOVE_SWIM_BACK;
            else
                return MOVE_SWIM;
        }
        else if (moveFlags & MOVEMENTFLAG_WALKING)
        {
            //if (speed_obj.run > speed_obj.walk)
            return MOVE_WALK;
        }
        else if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.run >= speed_obj.run_back*/)
            return MOVE_RUN_BACK;

        // Flying creatures use MOVEMENTFLAG_CAN_FLY or MOVEMENTFLAG_DISABLE_GRAVITY
        // Run speed is their default flight speed.
        return MOVE_RUN;
    }

    // Last-line wall guard for socketless playerbots in battlegrounds and
    // arenas. A real player is held in by their own client's collision; a bot
    // goes wherever its spline says, so any route that crosses static geometry
    // walks it straight through the wall. Every server-driven move - chase,
    // follow, fear, retreat, knockback, jump - passes through Launch(), so
    // checking each leg here covers routes no generator-level guard sees.
    //
    // Only WMO collision is tested: arena bounds are WMOs, and M2 props are
    // what navmesh corners hug at agent radius, so including them would trip
    // on legal paths. Gameobjects are left out for the same reason, except
    // the gates a battleground reports still standing (IsBotSplineLegThroughGate).
    //
    // In an arena the chest-height ray alone decides, because arena walls are
    // what keeps bots in. Several arena perimeters are parapets only 1.5 to
    // 2.5 yards tall over a void or a drop (Obelisk of the Stars, Baradin
    // Hold, Thakraj by its east start), and the arenas built inside open maps
    // (Nefarian's, the Inventor's Library, the Amphitheater of Anguish) have
    // navmesh on both sides of them.
    //
    // A battleground leg the chest ray blocks still goes ahead when the space
    // above the head is open AND the navmesh joins its two ends. The navmesh
    // climbs up to 1.6 yards in a single step, so between two path points the
    // floor can rise past a ray drawn 1.5 yards over both ends: the Scarlet
    // Chapel dais puts 2.3 yards of floor under a 4-yard leg, and that one ray
    // read the dais lip and top as walls and froze bots on its steps for over
    // a minute at a time. A step, a ledge lip or a floor the leg climbs over
    // leaves the space above the head open, and the navmesh walks it. A point
    // past a low wall is off the mesh or on an island of its own.
    //
    // A leg still held, in an arena or a battleground, can be split over the
    // floor instead (SplitBotSplineLegOverNavMeshFloor): a point on the
    // navmesh where it rises over the line, each shorter leg through this
    // same test. That relaxes nothing above; it only lets a route follow a
    // lip the navmesh walks, where the straight leg ran into the top of it.

    // The gates a battleground still has standing, by position. Gates are
    // gameobjects and the navmesh holds none, so the pathfinder draws its
    // routes straight through a closed Strand of the Ancients or Isle of
    // Conquest gate, and without this the bot walks through it. Read once per
    // map update at most: a route is tested leg by leg.
    static std::vector<Vector3> const& GetStandingGatePositions(Unit const* unit)
    {
        struct GateCache
        {
            Map const* map = nullptr;
            uint32 readMs = 0;
            std::vector<Vector3> positions;
        };
        thread_local GateCache cache;

        Map* map = unit->FindMap();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (cache.map == map && nowMs - cache.readMs < 250)
            return cache.positions;

        cache.map = map;
        cache.readMs = nowMs;
        cache.positions.clear();

        BattlegroundMap* battlegroundMap = map ? map->ToBattlegroundMap() : nullptr;
        Battleground* battleground = battlegroundMap ? battlegroundMap->GetBG() : nullptr;
        if (!battleground)
            return cache.positions;

        GuidVector gates;
        battleground->GetStandingGates(gates);
        for (ObjectGuid const& gateGuid : gates)
            if (GameObject const* gate = map->GetGameObject(gateGuid))
                cache.positions.emplace_back(gate->GetPositionX(), gate->GetPositionY(), gate->GetPositionZ());

        return cache.positions;
    }

    // A leg is refused at a standing gate when gameobject collision blocks it
    // both at chest height and well above the head: a gate does, the crate or
    // banner that navmesh corners hug beside it does not. Only legs passing
    // near a gate are tested, since gameobject collision is otherwise left out
    // on purpose (see below).
    static bool IsBotSplineLegThroughGate(Unit const* unit, Map const* map, Vector3 const& from, Vector3 const& to)
    {
        std::vector<Vector3> const& gates = GetStandingGatePositions(unit);
        if (gates.empty())
            return false;

        // The widest gate model (Isle of Conquest's Alliance gate) spans 37
        // yards either side of its origin.
        constexpr float GateReach = 40.0f;
        Vector3 const leg(to.x - from.x, to.y - from.y, 0.0f);
        float const legLengthSq = leg.squaredLength();
        bool nearGate = false;
        for (Vector3 const& gate : gates)
        {
            Vector3 const toGate(gate.x - from.x, gate.y - from.y, 0.0f);
            float const t = legLengthSq > 0.0f ? std::clamp(toGate.dot(leg) / legLengthSq, 0.0f, 1.0f) : 0.0f;
            Vector3 const closest(from.x + leg.x * t, from.y + leg.y * t, 0.0f);
            Vector3 const offset(gate.x - closest.x, gate.y - closest.y, 0.0f);
            if (offset.squaredLength() <= GateReach * GateReach)
            {
                nearGate = true;
                break;
            }
        }

        if (!nearGate)
            return false;

        auto const blockedAt = [map, unit, &from, &to](float height)
        {
            return !map->isInLineOfSight(from.x, from.y, from.z + height, to.x, to.y, to.z + height,
                unit->GetPhaseMask(), LINEOFSIGHT_CHECK_GOBJECT, VMAP::ModelIgnoreFlags::Nothing);
        };

        return blockedAt(1.5f) && blockedAt(4.0f);
    }

    static constexpr float BotSplineChestHeight = 1.5f;
    static constexpr float BotSplineOverheadHeight = 2.5f;

    static bool IsBotSplineRayBlocked(Unit const* unit, Map const* map, Vector3 const& from, Vector3 const& to, float height)
    {
        return !map->isInLineOfSight(from.x, from.y, from.z + height, to.x, to.y, to.z + height,
            unit->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::M2);
    }

    static bool IsBotSplineLegBlocked(Unit const* unit, Map const* map, std::optional<PathGenerator>& navmesh,
        Vector3 const& from, Vector3 const& to)
    {
        if ((to - from).squaredLength() < 0.25f)
            return false;

        if (IsBotSplineLegThroughGate(unit, map, from, to))
            return true;

        if (!IsBotSplineRayBlocked(unit, map, from, to, BotSplineChestHeight))
            return false;

        if (!map->IsBattleArena() && !IsBotSplineRayBlocked(unit, map, from, to, BotSplineOverheadHeight))
        {
            if (!navmesh)
                navmesh.emplace(unit);
            if (navmesh->IsNavMeshConnected(from, to))
                return false;
        }

        return true;
    }

    // A held leg can be plain floor all the same: a raised lip or a step the
    // navmesh climbs (up to 1.6 yards at a time) standing above the straight
    // line between two route points, so the chest ray over both ends runs into
    // the top of it. Thakraj's centre ring is one, a rim 1.2 yards high and two
    // wide on the platform floor: routes across the ring were held at its
    // edge, and bots running to the middle stood still or slid back. Nagrand's
    // pillar bases, a Lordaeron step and the low wall inside Nefarian's Horde
    // gate (where Durgan froze) are others.
    //
    // So the leg gets points on the navmesh floor where it rises over the line,
    // as the terrain hug does on open ground, and every shorter leg goes
    // through the full leg test again. The split is only tried where the
    // navmesh itself walks the leg (GetNavMeshFloorAlongLine), on a short leg,
    // under open space 2.5 yards up: a lip leaves that open, a wall or a ramp
    // overhead does not. A leg out of an arena has no navmesh under its far
    // part, so it stays held. Offline, against every arena wall edge with legs
    // four and eight yards out, it released none the chest ray held.
    static constexpr float NavMeshFloorSampleStep = 0.5f;
    static constexpr float NavMeshFloorMinRise = 0.25f;     // floor this little over the line is left alone
    static constexpr float NavMeshFloorMaxLeg = 8.0f;       // two route steps
    static constexpr uint32 NavMeshFloorMaxDepth = 4;
    static constexpr uint32 NavMeshFloorMaxRoutePoints = 512;
    // The navmesh smooths its floor over a thin wall or a gap; a point the real
    // floor puts further than this off the navmesh is not on that floor.
    static constexpr float NavMeshFloorMaxSnap = 1.5f;

    struct NavMeshFloorSample
    {
        float t;            // along the held leg, 0 at its start and 1 at its end
        Vector3 point;
    };

    // False when a point would not sit on the floor the navmesh describes.
    static bool AppendNavMeshFloorLeg(Unit const* unit, Vector3 const& from, Vector3 const& to, float fromT, float toT,
        std::vector<NavMeshFloorSample> const& samples, uint32 depth, PointsArray& out)
    {
        if (depth < NavMeshFloorMaxDepth)
        {
            float bestRise = NavMeshFloorMinRise;
            NavMeshFloorSample const* best = nullptr;
            for (NavMeshFloorSample const& sample : samples)
            {
                if (sample.t <= fromT || sample.t >= toT)
                    continue;

                float const u = (sample.t - fromT) / (toT - fromT);
                float const rise = sample.point.z - (from.z + (to.z - from.z) * u);
                if (rise > bestRise)
                {
                    bestRise = rise;
                    best = &sample;
                }
            }

            if (best)
            {
                // Onto the real floor there, as the route's own points are.
                Vector3 step = best->point;
                unit->UpdateAllowedPositionZ(step.x, step.y, step.z);
                if (std::fabs(step.z - best->point.z) > NavMeshFloorMaxSnap)
                    return false;
                return AppendNavMeshFloorLeg(unit, from, step, fromT, best->t, samples, depth + 1, out) &&
                    AppendNavMeshFloorLeg(unit, step, to, best->t, toT, samples, depth + 1, out);
            }
        }

        out.push_back(to);
        return true;
    }

    // On success `floorPoints` holds the points to put between `from` and `to`.
    static bool SplitBotSplineLegOverNavMeshFloor(Unit const* unit, Map const* map, std::optional<PathGenerator>& navmesh,
        Vector3 const& from, Vector3 const& to, PointsArray& floorPoints)
    {
        floorPoints.clear();

        float const dx = to.x - from.x;
        float const dy = to.y - from.y;
        float const lengthSq = dx * dx + dy * dy;
        if (lengthSq < 0.25f || lengthSq > NavMeshFloorMaxLeg * NavMeshFloorMaxLeg)
            return false;

        if (IsBotSplineRayBlocked(unit, map, from, to, BotSplineOverheadHeight))
            return false;

        if (!navmesh)
            navmesh.emplace(unit);

        std::vector<Vector3> floor;
        if (!navmesh->GetNavMeshFloorAlongLine(from, to, NavMeshFloorSampleStep, floor) || floor.empty())
            return false;

        std::vector<NavMeshFloorSample> samples;
        samples.reserve(floor.size());
        for (Vector3 const& point : floor)
            samples.push_back({ ((point.x - from.x) * dx + (point.y - from.y) * dy) / lengthSq, point });

        PointsArray legEnds;
        if (!AppendNavMeshFloorLeg(unit, from, to, 0.0f, 1.0f, samples, 0, legEnds) || legEnds.size() < 2)
            return false;

        Vector3 legStart = from;
        for (Vector3 const& legEnd : legEnds)
        {
            if (IsBotSplineLegBlocked(unit, map, navmesh, legStart, legEnd))
                return false;
            legStart = legEnd;
        }

        // The last leg end is `to`, already on the route.
        floorPoints.assign(legEnds.begin(), legEnds.end() - 1);
        return true;
    }

    // `floorSplits` counts the held legs split over the floor; only a plain run
    // on the ground (`followFloor`) is split, as only it is terrain-hugged.
    static bool FindBotSplineWallCrossing(Unit const* unit, PointsArray& path, uint32& blockedLeg,
        std::optional<PathGenerator>& navmesh, bool followFloor, uint32& floorSplits)
    {
        Map const* map = unit->FindMap();
        if (!map || !map->IsBattlegroundOrArena())
            return false;

        PointsArray floorPoints;
        for (uint32 i = 1; i < path.size(); ++i)
        {
            if (!IsBotSplineLegBlocked(unit, map, navmesh, path[i - 1], path[i]))
                continue;

            if (followFloor && path.size() < NavMeshFloorMaxRoutePoints &&
                SplitBotSplineLegOverNavMeshFloor(unit, map, navmesh, path[i - 1], path[i], floorPoints))
            {
                path.insert(path.begin() + i, floorPoints.begin(), floorPoints.end());
                ++floorSplits;
                // Every leg up to the old leg end is tested clear.
                i += uint32(floorPoints.size());
                continue;
            }

            blockedLeg = i;
            return true;
        }

        return false;
    }

    // A route held on its FIRST leg has no clean prefix to walk, so the bot
    // stays where it stands, and a repath from there draws the same first leg.
    // That happens when a bot ends up hard against a wall corner: navmesh
    // corners can sit closer to a wall than the agent radius, and the straight
    // line on to the next corner then clips the wall. Durgan stopped 0.2 yards
    // off the end of a two-yard wall just inside the Horde gate of Nefarian's
    // Arena and was held there for 80 seconds while his team fought 2v3.
    //
    // So try a short step onto the navmesh beside the bot and route on from
    // there. The step and every leg after it go through the same leg test, so
    // this never lets a bot walk a leg the guard would refuse; it only changes
    // where the route starts. Steps that bend the route least are tried first.
    static bool TryBotSplineSidestep(Unit const* unit, PointsArray& path, uint32& blockedLeg,
        std::optional<PathGenerator>& navmesh, bool followFloor, uint32& floorSplits)
    {
        Map const* map = unit->FindMap();
        if (!map || path.size() < 2)
            return false;

        Vector3 const from = path[0];
        Vector3 const next = path[1];
        Vector3 heading(next.x - from.x, next.y - from.y, 0.0f);
        float const headingLength = heading.length();
        if (headingLength < 0.1f)
            return false;
        heading /= headingLength;

        if (!navmesh)
            navmesh.emplace(unit);

        constexpr float StepLengths[] = { 0.8f, 1.6f };
        constexpr float StepAngles[] = { float(M_PI) / 4, -float(M_PI) / 4, float(M_PI) / 2, -float(M_PI) / 2,
            3 * float(M_PI) / 4, -3 * float(M_PI) / 4, float(M_PI) };
        // Shorter than this is no step at all, and the leg test skips it.
        constexpr float MinStep = 0.5f;
        // Snapping to a floor this far above or below is a climb or a drop.
        constexpr float MaxStepRise = 1.0f;

        for (float stepLength : StepLengths)
        {
            for (float angle : StepAngles)
            {
                float const c = std::cos(angle);
                float const s = std::sin(angle);
                Vector3 const candidate(from.x + (heading.x * c - heading.y * s) * stepLength,
                    from.y + (heading.x * s + heading.y * c) * stepLength, from.z);

                Vector3 step;
                if (!navmesh->FindNearestNavMeshPoint(candidate, 0.5f, step))
                    continue;

                if (std::fabs(step.z - from.z) > MaxStepRise)
                    continue;

                float const stepX = step.x - from.x;
                float const stepY = step.y - from.y;
                if (stepX * stepX + stepY * stepY < MinStep * MinStep)
                    continue;

                if (IsBotSplineLegBlocked(unit, map, navmesh, from, step) ||
                    IsBotSplineLegBlocked(unit, map, navmesh, step, next))
                    continue;

                if (!navmesh->IsNavMeshConnected(step, next))
                    continue;

                path.insert(path.begin() + 1, step);
                blockedLeg = 0;
                FindBotSplineWallCrossing(unit, path, blockedLeg, navmesh, followFloor, floorSplits);
                return true;
            }
        }

        return false;
    }

    // A route is a point every four yards or so, each one on the ground, and a
    // spline runs - and every client draws it - in a straight line between
    // them. Over a bump in the terrain that line goes through the hill. A
    // player never shows it because each client holds its own character to
    // the ground every frame; a bot has no client, so it ran visibly waist-deep
    // over every rise sharp enough, on the server as much as on screen, and a
    // bot killed there left its cache in the ground. Measured around the live
    // fleet, one four-yard leg in thirty sank half a yard or more, and one in
    // seven in Searing Gorge.
    //
    // So where the terrain stands above a leg, the leg gets a point on the
    // ground at its highest spot, and each half is looked at again. Only legs
    // with both ends on open terrain: a bridge deck, a building floor or a cave
    // passes under or over the heightmap legitimately and is left exactly as
    // the navmesh drew it.
    static constexpr float TerrainHugLegEndSlack = 0.5f;   // a leg end this near the terrain is on it
    static constexpr float TerrainHugMinRise = 0.25f;      // terrain this little over the line is left alone
    static constexpr float TerrainHugSampleStep = 1.0f;
    static constexpr uint32 TerrainHugMaxDepth = 4;
    static constexpr uint32 TerrainHugMaxPoints = 512;

    static bool IsOnTerrain(Map const* map, Vector3 const& point)
    {
        float const ground = map->GetGridHeight(point.x, point.y);
        return ground > INVALID_HEIGHT && std::fabs(point.z - ground) <= TerrainHugLegEndSlack;
    }

    static void AppendTerrainHuggingLeg(Unit const* unit, Map const* map, Vector3 const& from, Vector3 const& to,
        uint32 depth, PointsArray& out)
    {
        float const dx = to.x - from.x;
        float const dy = to.y - from.y;
        uint32 const samples = uint32(std::sqrt(dx * dx + dy * dy) / TerrainHugSampleStep);
        if (samples >= 2 && depth < TerrainHugMaxDepth && out.size() + 2 < TerrainHugMaxPoints)
        {
            float bestRise = TerrainHugMinRise;
            Vector3 best;
            bool found = false;
            for (uint32 s = 1; s < samples; ++s)
            {
                float const t = float(s) / float(samples);
                float const x = from.x + dx * t;
                float const y = from.y + dy * t;
                // A hole in the heightmap is a cave mouth or a building's own
                // floor; the terrain says nothing about the line there.
                float const ground = map->GetGridHeight(x, y);
                if (ground <= INVALID_HEIGHT)
                    continue;

                float const rise = ground - (from.z + (to.z - from.z) * t);
                if (rise > bestRise)
                {
                    bestRise = rise;
                    best = Vector3(x, y, ground);
                    found = true;
                }
            }

            if (found)
            {
                // Onto whatever stands on the terrain there, as the route's own
                // points were.
                unit->UpdateAllowedPositionZ(best.x, best.y, best.z);
                AppendTerrainHuggingLeg(unit, map, from, best, depth + 1, out);
                AppendTerrainHuggingLeg(unit, map, best, to, depth + 1, out);
                return;
            }
        }

        out.push_back(to);
    }

    static void HugBotSplineToTerrain(Unit const* unit, PointsArray& path)
    {
        Map const* map = unit->FindMap();
        if (!map || path.size() < 2)
            return;

        PointsArray hugged;
        hugged.reserve(path.size() * 2);
        hugged.push_back(path[0]);
        for (uint32 i = 1; i < path.size(); ++i)
        {
            if (IsOnTerrain(map, path[i - 1]) && IsOnTerrain(map, path[i]))
                AppendTerrainHuggingLeg(unit, map, path[i - 1], path[i], 0, hugged);
            else
                hugged.push_back(path[i]);
        }

        if (hugged.size() != path.size())
            path.swap(hugged);
    }

    int32 MoveSplineInit::Launch()
    {
        MoveSpline& move_spline = *unit->movespline;

        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes (movementInfo.transport.guid is 0 in that case)
        bool transport = unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && unit->GetTransGUID();
        Location real_position;
        // there is a big chance that current position is unknown if current state is not finalized, need compute it
        // this also allows CalculatePath spline position and update map position in much greater intervals
        // Don't compute for transport movement if the unit is in a motion between two transports
        if (!move_spline.Finalized() && move_spline.onTransport == transport)
            real_position = move_spline.ComputePosition();
        else
        {
            Position const* pos;
            if (!transport)
                pos = unit;
            else
                pos = &unit->m_movementInfo.transport.pos;

            real_position.x = pos->GetPositionX();
            real_position.y = pos->GetPositionY();
            real_position.z = pos->GetPositionZ();
            real_position.orientation = unit->GetOrientation();
        }

        // should i do the things that user should do? - no.
        if (args.path.empty())
            return 0;

        // corrent first vertex
        args.path[0] = real_position;
        args.initialOrientation = real_position.orientation;
        args.flags.enter_cycle = args.flags.cyclic;
        move_spline.onTransport = transport;

        if (!transport)
        {
            // The bot itself, or a unit a bot charms or owns: the shadow
            // priest's Shadow Wraith (89784) is a possessed creature whose
            // flee order fell through to a raw spline when no path reached its
            // point, and Fade's expiry then teleported the priest to it - out
            // of Blackrock Throne. Pets come along for the same reason.
            Player const* driver = unit->ToPlayer();
            if (!driver)
                if (Unit const* controller = unit->GetCharmerOrOwner())
                    driver = controller->ToPlayer();
            WorldSession const* session = driver ? driver->GetSession() : nullptr;
            bool const serverDriven = session && (session->IsVirtualSession() || session->IsTransientPlayerSession());

            // A plain run on the ground only: an arc, a fall, a swim or a flight
            // is not meant to follow the terrain, and a waypoint route's point
            // indexes must stay the ones it was given.
            bool const plainGroundRun = serverDriven && args.path_Idx_offset == 0 && args.flags.isLinear() && !args.flags.parabolic &&
                !args.flags.falling && !args.flags.animation && !args.flags.cyclic && !args.flags.flying &&
                !unit->IsFlying() && !unit->IsHovering() && !unit->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
            if (plainGroundRun)
                HugBotSplineToTerrain(unit, args.path);

            // Map updates run one map per thread and a bot is on one map, so a
            // thread-local throttle needs no lock. One line per bot per five
            // seconds: a generator that keeps re-requesting the same blocked
            // route would otherwise log every tick.
            auto const logThrottled = [this]()
            {
                thread_local std::unordered_map<uint64, uint32> lastLogMsByGuid;
                uint32 const nowMs = GameTime::GetGameTimeMS();
                uint32& lastLogMs = lastLogMsByGuid[unit->GetGUID().GetRawValue()];
                if (lastLogMs && nowMs - lastLogMs < 5000)
                    return false;
                lastLogMs = nowMs;
                return true;
            };

            uint32 blockedLeg = 0;
            uint32 floorSplits = 0;
            std::optional<PathGenerator> navmesh;
            if (serverDriven && FindBotSplineWallCrossing(unit, args.path, blockedLeg, navmesh, plainGroundRun, floorSplits))
            {
                // Only a plain run is bent around a corner. A jump, knockback
                // or fall keeps its own arc, so it is held as before.
                Vector3 const heldTo = args.path[1];
                bool const sidestepped = blockedLeg == 1 && args.flags.isLinear() && !args.flags.parabolic &&
                    !args.flags.falling && !args.flags.animation && !args.flags.cyclic &&
                    TryBotSplineSidestep(unit, args.path, blockedLeg, navmesh, plainGroundRun, floorSplits);

                if (logThrottled())
                {
                    MotionMaster const* motionMaster = unit->GetMotionMaster();
                    uint32 const motionType = motionMaster ? uint32(motionMaster->GetCurrentMovementGeneratorType()) : 0u;
                    Vector3 const& end = args.path.back();
                    if (blockedLeg)
                    {
                        Vector3 const& from = args.path[blockedLeg - 1];
                        Vector3 const& to = args.path[blockedLeg];
                        TC_LOG_WARN("playerbots.movement.spline",
                            "PB spline: bot={} mover={} outcome=held-wall-crossing map={} motion_type={} points={} leg={} kept_legs={} sidestep={} parabolic={} "
                            "leg_from=({}, {}, {}) leg_to=({}, {}, {}) final=({}, {}, {}).",
                            driver->GetGUID().ToString(), unit->GetGUID().ToString(), unit->GetMapId(), motionType,
                            uint32(args.path.size()), blockedLeg, blockedLeg - 1, sidestepped ? 1 : 0, args.flags.parabolic ? 1 : 0,
                            from.x, from.y, from.z, to.x, to.y, to.z, end.x, end.y, end.z);
                    }
                    else
                    {
                        Vector3 const& from = args.path[0];
                        Vector3 const& step = args.path[1];
                        TC_LOG_WARN("playerbots.movement.spline",
                            "PB spline: bot={} mover={} outcome=sidestep-wall-crossing map={} motion_type={} points={} "
                            "leg_from=({}, {}, {}) step=({}, {}, {}) held_to=({}, {}, {}) final=({}, {}, {}).",
                            driver->GetGUID().ToString(), unit->GetGUID().ToString(), unit->GetMapId(), motionType,
                            uint32(args.path.size()),
                            from.x, from.y, from.z, step.x, step.y, step.z, heldTo.x, heldTo.y, heldTo.z, end.x, end.y, end.z);
                    }
                }

                // Walk the legs before the blocked one and stop where it
                // starts. Holding at the first point instead froze the bot for
                // as long as its generator kept asking: a repath from the same
                // spot draws the same route through the same wall, while a few
                // yards on it usually draws a different one. A first leg no
                // sidestep gets around still gets the two-point stay spline
                // MoveTo builds for a rejected route, minus any jump arc.
                if (blockedLeg)
                {
                    args.path_Idx_offset = 0;
                    if (blockedLeg > 1)
                        args.path.resize(blockedLeg);
                    else
                    {
                        args.path.resize(2);
                        args.path[1] = args.path[0];
                    }
                    args.flags.parabolic = false;
                    args.flags.animation = false;
                }
            }
            else if (floorSplits && logThrottled())
            {
                MotionMaster const* motionMaster = unit->GetMotionMaster();
                uint32 const motionType = motionMaster ? uint32(motionMaster->GetCurrentMovementGeneratorType()) : 0u;
                Vector3 const& start = args.path.front();
                Vector3 const& end = args.path.back();
                TC_LOG_INFO("playerbots.movement.spline",
                    "PB spline: bot={} mover={} outcome=floor-split map={} motion_type={} points={} split_legs={} "
                    "start=({}, {}, {}) final=({}, {}, {}).",
                    driver->GetGUID().ToString(), unit->GetGUID().ToString(), unit->GetMapId(), motionType,
                    uint32(args.path.size()), floorSplits, start.x, start.y, start.z, end.x, end.y, end.z);
            }
        }

        uint32 moveFlags = unit->m_movementInfo.GetMovementFlags();
        moveFlags |= MOVEMENTFLAG_SPLINE_ENABLED;

        if (!args.flags.backward)
            moveFlags = (moveFlags & ~(MOVEMENTFLAG_BACKWARD)) | MOVEMENTFLAG_FORWARD;
        else
            moveFlags = (moveFlags & ~(MOVEMENTFLAG_FORWARD)) | MOVEMENTFLAG_BACKWARD;

        if (moveFlags & MOVEMENTFLAG_ROOT)
            moveFlags &= ~MOVEMENTFLAG_MASK_MOVING;

        if (!args.HasVelocity)
        {
            // If spline is initialized with SetWalk method it only means we need to select
            // walk move speed for it but not add walk flag to unit
            uint32 moveFlagsForSpeed = moveFlags;
            if (args.walk)
                moveFlagsForSpeed |= MOVEMENTFLAG_WALKING;
            else
                moveFlagsForSpeed &= ~MOVEMENTFLAG_WALKING;

            args.velocity = unit->GetSpeed(SelectSpeedType(moveFlagsForSpeed));
            if (Creature* creature = unit->ToCreature())
                if (creature->HasSearchedAssistance())
                    args.velocity *= 0.66f;
        }

        // limit the speed in the same way the client does
        args.velocity = std::min(args.velocity, args.flags.catmullrom || args.flags.flying ? 50.0f : std::max(28.0f, unit->GetSpeed(MOVE_RUN) * 4.0f));

        if (!args.Validate(unit))
            return 0;

        unit->m_movementInfo.SetMovementFlags(moveFlags);
        move_spline.Initialize(args);

        // A bot standing on a boat or lift that only the clients animate. Sent
        // in world coordinates, the move would take it off the deck on every
        // client and put it where the server has it - up to a second's sailing
        // behind a Strand boat - until its next placement brought it back: bots
        // blinking in and out along the crossing. The clients keep it at its
        // place on the deck instead; whoever carries it takes the spline off
        // the server (BattlegroundSA::CarryBoatRider, GameObject::CarryLiftRiders).
        if (unit->IsServerPlacedGameObjectPassenger())
        {
            unit->BroadcastServerPlacedMovement();
            return move_spline.Duration();
        }

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        data << unit->GetPackGUID();
        if (transport)
        {
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << unit->GetTransGUID().WriteAsPacked();
            data << int8(unit->GetTransSeat());
        }

        PacketBuilder::WriteMonsterMove(move_spline, data);
        unit->SendMessageToSet(&data, true);

        return move_spline.Duration();
    }

    void MoveSplineInit::Stop()
    {
        MoveSpline& move_spline = *unit->movespline;

        // No need to stop if we are not moving
        if (move_spline.Finalized())
            return;

        bool transport = unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && unit->GetTransGUID();
        Location loc;
        if (move_spline.onTransport == transport)
            loc = move_spline.ComputePosition();
        else
        {
            Position const* pos;
            if (!transport)
                pos = unit;
            else
                pos = &unit->m_movementInfo.transport.pos;

            loc.x = pos->GetPositionX();
            loc.y = pos->GetPositionY();
            loc.z = pos->GetPositionZ();
            loc.orientation = unit->GetOrientation();
        }

        args.flags = MoveSplineFlag::Done;
        unit->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_SPLINE_ENABLED);
        move_spline.onTransport = transport;
        move_spline.Initialize(args);

        // See Launch: on a boat or lift the clients animate, a stop sent in
        // world coordinates takes the bot off the deck just the same.
        if (unit->IsServerPlacedGameObjectPassenger())
        {
            unit->BroadcastServerPlacedMovement();
            return;
        }

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        data << unit->GetPackGUID();
        if (transport)
        {
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << unit->GetTransGUID().WriteAsPacked();
            data << int8(unit->GetTransSeat());
        }

        PacketBuilder::WriteStopMovement(loc, args.splineId, data);
        unit->SendMessageToSet(&data, true);
    }

    MoveSplineInit::MoveSplineInit(Unit* m) : unit(m)
    {
        args.splineId = splineIdGen.NewId();
        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes
        args.TransformForTransport = unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && unit->GetTransGUID();
        // mix existing state into new
        args.flags.canswim = unit->CanSwim();
        args.walk = unit->HasUnitMovementFlag(MOVEMENTFLAG_WALKING);
        args.flags.flying = unit->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY);
    }

    MoveSplineInit::~MoveSplineInit() = default;

    void MoveSplineInit::SetFacing(Vector3 const& spot)
    {
        TransportPathTransform transform(unit, args.TransformForTransport);
        Vector3 finalSpot = transform(spot);
        args.facing.f.x = finalSpot.x;
        args.facing.f.y = finalSpot.y;
        args.facing.f.z = finalSpot.z;
        args.flags.EnableFacingPoint();
    }

    void MoveSplineInit::SetFacing(Unit const* target)
    {
        SetFacing(target->GetGUID());
    }

    void MoveSplineInit::SetFacing(ObjectGuid const& target)
    {
        args.flags.EnableFacingTarget();
        args.facing.target = target.GetRawValue();
    }

    void MoveSplineInit::SetFacing(float angle)
    {
        if (args.TransformForTransport)
        {
            if (Unit* vehicle = unit->GetVehicleBase())
                angle -= vehicle->GetOrientation();
            else if (Transport* transport = unit->GetTransport())
                angle -= transport->GetOrientation();
        }

        args.facing.angle = G3D::wrap(angle, 0.f, (float)G3D::twoPi());
        args.flags.EnableFacingAngle();
    }

    void MoveSplineInit::MovebyPath(PointsArray const& controls, int32 path_offset)
    {
        args.path_Idx_offset = path_offset;
        args.path.resize(controls.size());
        std::transform(controls.begin(), controls.end(), args.path.begin(), TransportPathTransform(unit, args.TransformForTransport));
    }

    void MoveSplineInit::MoveTo(float x, float y, float z, bool generatePath, bool forceDestination)
    {
        MoveTo(G3D::Vector3(x, y, z), generatePath, forceDestination);
    }

    void MoveSplineInit::MoveTo(Vector3 const& dest, bool generatePath, bool forceDestination)
    {
        if (generatePath)
        {
            // Only units that actually have something driving them belong in the strict
            // arm below: a real client, or a bot session. A player-owned CREATURE has
            // neither and must path exactly like stock 3.3.5.
            //
            // Unit::SetMinion stamps both m_ControlledByPlayer (Unit.cpp:6949) and the
            // owner GUID (Unit.cpp:6945) onto every minion, so testing those alone swept
            // in every pet, guardian, charmed creature and player temp summon. Once in
            // the strict arm a rejected route yields buildStayPath() - a two-point spline
            // to the unit's own current position. That passes MoveSplineInitArgs::Validate
            // (it only checks path.size() > 1), launches, finalizes immediately, and
            // PetAI::MovementInform then latches SetIsAtStay(true)/MoveIdle(). The pet is
            // recorded as having arrived without having moved, and never retries.
            //
            // Playerbots are Player objects, so they still take the strict arm and their
            // behaviour is unchanged.
            bool const playerControlled = unit->GetTypeId() == TYPEID_PLAYER
                && (unit->IsControlledByPlayer() || unit->GetOwnerGUID().IsPlayer());
            bool serverDrivenPlayer = false;
            if (Player const* moverPlayer = unit->ToPlayer())
                if (WorldSession const* session = moverPlayer->GetSession())
                    serverDrivenPlayer = session->IsVirtualSession() || session->IsTransientPlayerSession();

            auto const buildStayPath = [&]()
            {
                args.path_Idx_offset = 0;
                args.path.resize(2);
                TransportPathTransform transform(unit, args.TransformForTransport);
                Vector3 stay(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
                args.path[1] = transform(stay);
            };

            PathGenerator path(unit);
            bool result = path.CalculatePath(dest.x, dest.y, dest.z, forceDestination);

            // Every movement order a bot receives ends here, whichever generator
            // issued it. Chase and Follow each log their own resolved path type,
            // but MovePoint - and anything else that reaches a spline directly -
            // logged nothing, which was the one blind spot left when a bot was
            // seen walking through an arena wall and neither of those two
            // generators had produced a single non-navmesh path in ~72,000
            // samples. Name the outcome here so the next occurrence identifies
            // its own cause without another live logging session.
            //
            // Off unless playerbots.movement.spline is switched on, and only
            // ever evaluated for a session with no client of its own.
            auto const logServerDrivenSplineDecision = [&](char const* outcome)
            {
                if (!serverDrivenPlayer)
                    return;

                TC_LOG_DEBUG("playerbots.movement.spline",
                    "PB spline: bot={} outcome={} calc_ok={} path_type={} navmesh={} points={} force_dest={} "
                    "from=({}, {}, {}) requested=({}, {}, {}) actual_end=({}, {}, {}).",
                    unit->GetGUID().ToString(), outcome, result ? 1 : 0,
                    uint32(path.GetPathType()), path.HasNavigationData() ? 1 : 0,
                    uint32(path.GetPath().size()), forceDestination ? 1 : 0,
                    unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
                    dest.x, dest.y, dest.z,
                    path.GetActualEndPosition().x, path.GetActualEndPosition().y, path.GetActualEndPosition().z);
            };

            if (result)
            {
                PathType const pathType = path.GetPathType();
                bool const navmeshAvailable = path.HasNavigationData();
                bool const usesUnsafePathMode = navmeshAvailable && (pathType & (PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT));

                if (!(pathType & PATHFIND_NOPATH))
                {
                    bool const strictPlayerRejectPath = playerControlled && !serverDrivenPlayer &&
                        ((pathType & PATHFIND_INCOMPLETE) || usesUnsafePathMode);
                    bool const serverDrivenPlayerRejectPath = serverDrivenPlayer && usesUnsafePathMode;

                    if (!(strictPlayerRejectPath || serverDrivenPlayerRejectPath))
                    {
                        logServerDrivenSplineDecision("accepted-navmesh-path");
                        MovebyPath(path.GetPath());
                        return;
                    }
                }

                if (serverDrivenPlayer && ((pathType & PATHFIND_NOPATH) || usesUnsafePathMode))
                {
                    logServerDrivenSplineDecision("held-position-unsafe-route");
                    buildStayPath();
                    return;
                }

                if ((playerControlled && !serverDrivenPlayer) && ((pathType & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) ||
                    usesUnsafePathMode))
                {
                    buildStayPath();
                    return;
                }
            }
            else if (serverDrivenPlayer)
            {
                logServerDrivenSplineDecision("held-position-path-calc-failed");
                // A server-controlled Player has no client movement input to
                // repair a failed generated route. Falling through to the raw
                // two-point spline below lets it cut through walls or descend
                // between stacked floors. Hold position and let its movement
                // owner retry a fresh navmesh order instead.
                buildStayPath();
                return;
            }
        }

        // The raw two-point spline: start to destination, straight through
        // whatever lies between. Every guarded branch above returns before this
        // for a server-driven player, so a bot arriving here is the exact shape
        // of bug this diagnostic exists to catch - which is why it is a warning
        // rather than a debug line, and why it names how it got here.
        if (Player const* moverPlayer = unit->ToPlayer())
            if (WorldSession const* session = moverPlayer->GetSession())
                if (session->IsVirtualSession() || session->IsTransientPlayerSession())
                    TC_LOG_WARN("playerbots.movement.spline",
                        "PB spline: bot={} outcome=raw-direct-spline generate_path={} force_dest={} "
                        "from=({}, {}, {}) dest=({}, {}, {}).",
                        unit->GetGUID().ToString(), generatePath ? 1 : 0, forceDestination ? 1 : 0,
                        unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
                        dest.x, dest.y, dest.z);

        args.path_Idx_offset = 0;
        args.path.resize(2);
        TransportPathTransform transform(unit, args.TransformForTransport);
        args.path[1] = transform(dest);
    }

    Vector3 TransportPathTransform::operator()(Vector3 input)
    {
        if (_transformForTransport)
            if (TransportBase* transport = _owner->GetDirectTransport())
                transport->CalculatePassengerOffset(input.x, input.y, input.z);

        return input;
    }
}
