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

#ifndef TRINITY_BATTLEGROUND_FENCE_H
#define TRINITY_BATTLEGROUND_FENCE_H

#include "Define.h"
#include "Position.h"

#include <array>

class Battleground;

// Why this exists
// ---------------
// A real player is kept inside an arena by their own client's collision. A
// playerbot moves by server-side splines, and a spline collides with nothing:
// the only thing between a bot and the outside world is PathGenerator handing
// back a navmesh route. It does not always do that. Every BuildShortcut() path
// in PathGenerator produces a straight line through solid geometry, and the
// first one - a start or end position whose mmap tile is not loaded - reports
// itself as PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH, which
// ChaseMovementGenerator accepts as a perfectly good path because it only
// rejects PATHFIND_NOPATH.
//
// Even a flawless navmesh does not confine these maps. The ported arenas are
// whole ADT worlds with an arena WMO dropped into them, and mmaps_generator
// meshed all of it - on map 982 that is seventeen tiles of walkable ground,
// nearly all of it outside the arena. Gates are gameobjects and no gameobject
// is ever baked into a navmesh, so each gate doorway is an open corridor in the
// mesh leading straight out. A bot that walks out through it is following a
// legal route and behaving correctly; there was simply nothing that knew where
// the arena ends.
//
// So this does not try to fix pathfinding. It answers the question nothing in
// the core could answer before - "is this position still inside the arena?" -
// and answers it from the same collision data the client uses, so it stays
// correct for maps nobody has surveyed.
//
// How the shape is found
// ----------------------
// From the midpoint of the two team start positions, cast rays outward every
// five degrees at three heights and keep the furthest wall each direction
// finds. That yields a star-shaped polygon that hugs the arena's real outer
// wall. Two corrections make it usable:
//
//   * Doorways and gate tunnels have no wall, so those rays run to the probe
//     limit. Capping every sector at a little over the 75th percentile seals
//     them without needing to know where they are.
//   * Pillars and pylons shorten the rays behind them. Flooring every sector
//     at a little under the 75th percentile ignores them, which is right for
//     an outer fence - the question is never "is this bot behind a pillar".
//
// An open battleground has no outer wall to find, so most of its rays reach the
// limit and the fence marks itself unusable rather than inventing a circle
// around Alterac Valley. Nothing enforces an unusable fence. That test is what
// makes this safe to leave switched on everywhere.

namespace BattlegroundFence
{
    // Five degrees. Fine enough to follow a doorway's edge, coarse enough that
    // one build is a few hundred raycasts rather than a few thousand.
    constexpr uint32 SectorCount = 72;

    struct Fence
    {
        // True only when the probe actually found an enclosing shape. A false
        // fence is never enforced and never clamps a destination.
        bool Usable = false;

        float CentreX = 0.0f;
        float CentreY = 0.0f;
        float CentreZ = 0.0f;

        // Wall distance from the centre, one entry per SectorCount sector,
        // sector 0 centred on angle 0 and increasing counter-clockwise.
        std::array<float, SectorCount> SectorRadius = {};

        // Generous vertical band. Bots that fall out of the world are already
        // handled by the under-map recovery; this only catches a bot that has
        // climbed far above the arena or sunk well below its floor.
        float FloorZ = 0.0f;
        float CeilZ = 0.0f;

        // Reported in the build log so a bad fence is visible without having
        // to instrument anything.
        float ReferenceRadius = 0.0f;

        // The WMO the arena's own floor sits in, or -1 when its floor is
        // terrain. When it is set, containment is this and nothing else: a
        // position in a different WMO, or in none, is outside, exactly and
        // without a radius or a tolerance. The radial shape below is then only
        // used to pull a destination back to somewhere sensible.
        //
        // This is strictly better than measuring a radius wherever it applies.
        // An arena is not a circle - the Imperial Arena's wall is 55 yd out in
        // one bearing and 69 in another - so a radial fence has to choose
        // between missing a bot a yard past a near wall and dragging back one
        // standing legitimately inside a far one. WMO identity has no such
        // trade: the server already knows which building you are in.
        int32 WmoRootId = -1;
        uint32 MapId = 0;

        float RadiusAt(float x, float y) const;
        bool Contains(float x, float y, float z, float margin) const;

        // The closest point that is comfortably inside, on the line from the
        // position toward the centre. Returns the centre itself if the fence
        // is degenerate.
        void NearestInsidePoint(float x, float y, float& outX, float& outY) const;
    };

    // Built once per battleground map on first use and cached for the process.
    // Returns nullptr when the battleground has no usable fence, which is the
    // answer for every open battleground.
    //
    // Must be called with the battleground's map loaded - the raycasts read the
    // static vmap tree, which is populated per grid. Calling it from a live
    // match satisfies that on its own.
    Fence const* GetForBattleground(Battleground const* bg);

    // Pull a requested destination back inside the fence before it is handed to
    // a movement generator. Leaves the position untouched when there is no
    // usable fence or the destination is already inside.
    bool ClampToFence(Battleground const* bg, Position& destination, float margin);

    // Drop every cached fence. Intended for a map-data reload; nothing calls it
    // on a normal server cycle.
    void InvalidateCache();
}

#endif // TRINITY_BATTLEGROUND_FENCE_H
