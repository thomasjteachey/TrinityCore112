#ifndef TRINITY_LOSBLOCKER_H
#define TRINITY_LOSBLOCKER_H

#include "Define.h"

class WorldObject;

/**
 * Players who are themselves line-of-sight blockers.
 *
 * Unlike the client-side player collision (which works because movement is
 * client-authoritative), spell line of sight is decided by the SERVER - so this
 * has to live here or it would be cosmetic and trivially bypassed by not running
 * the client tweak.
 *
 * A registered blocker stops HOSTILE casters from casting THROUGH them at
 * something else. Spells aimed at the blocker itself are unaffected, and so are
 * friendly casters.
 *
 * Deliberately scoped to spell casting (Spell::CheckCast / CheckTarget) rather
 * than WorldObject::IsWithinLOSInMap: hooking the general LoS routine would also
 * cut creature aggro, chase and AI sight through a player, which is a far larger
 * behavioural change, and those are hot paths.
 *
 * Cost when unused is a single relaxed atomic load - Active() is false whenever
 * nobody has the aura, which is the overwhelmingly common case.
 */
namespace LosBlocker
{
    // Registration, driven by the aura script.
    void Add(WorldObject const* who, uint32 spellId);
    void Remove(WorldObject const* who);

    // True when at least one blocker exists anywhere. Cheap early-out.
    bool Active();

    /**
     * Does a registered blocker stand between `caster` and `target`?
     *
     * Ignores the caster and the target themselves, blockers that are not
     * hostile to the caster, and blockers whose vertical extent does not
     * overlap the shot (so a player on the floor above does not block).
     */
    bool Blocks(WorldObject const* caster, WorldObject const* target);

    // Same, for a ground-targeted destination rather than a unit.
    bool BlocksPosition(WorldObject const* caster, float x, float y, float z);
}

#endif
