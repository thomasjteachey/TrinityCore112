#ifndef TRINITY_PLAYERCOLLISIONSERVER_H
#define TRINITY_PLAYERCOLLISIONSERVER_H

#include "Define.h"

class Player;
class WorldObject;
struct Position;

/**
 * Server-side half of buff-gated player collision.
 *
 * Human players are handled by the client tweak: movement is client-authoritative,
 * so a client clipping its own movement IS the mechanic. Playerbots have no client
 * - they are moved server-side by the motion master - so they walk through
 * blockers unless the server stops them, which is what this is for.
 *
 * The registry mirrors the client's rule matrix exactly, so a bot and a human see
 * the same walls. Two axes: who is affected, and one-way vs mutual.
 *
 *   BlockEnemies   on X : X's enemies cannot pass through X (X still passes them)
 *   BlockAll       on X : nobody can pass through X          (X still passes them)
 *   CollideEnemies on X : mutual collision between X and X's enemies
 *   CollideAll     on X : mutual collision between X and everyone
 *
 * Cost when unused is one relaxed atomic load.
 */
namespace PlayerCollisionServer
{
    enum Rule : uint8
    {
        RULE_BLOCK_ENEMIES = 0,     // one-way, hostile only
        RULE_BLOCK_ALL,             // one-way, everyone
        RULE_COLLIDE_ENEMIES,       // mutual, hostile only
        RULE_COLLIDE_ALL            // mutual, everyone
    };

    // Registration, driven by the aura script. `spellId` is re-verified on use so
    // a stale entry (a disconnect that skipped the remove handler) cannot make
    // somebody block without the buff.
    void Add(WorldObject const* who, uint32 spellId, Rule rule);
    void Remove(WorldObject const* who, uint32 spellId);

    // True when at least one blocker exists anywhere. Cheap early-out.
    bool Active();

    // Does `blocker` block `mover`, per the rule matrix above?
    bool Blocks(Player const* mover, Player const* blocker);

    /**
     * Shorten a movement segment so it stops at the first blocker it would enter.
     *
     * Returns true when `dest` was modified. Clipping the DESTINATION rather than
     * correcting the position afterwards is deliberate: a spline that keeps
     * advancing while the unit is held back accumulates, and discharges as a
     * teleport the moment the obstruction clears - exactly the artefact the
     * client-side work hit and had to solve the same way.
     */
    bool ClipSegment(Player const* mover, Position const& from, Position& dest);
}

#endif
