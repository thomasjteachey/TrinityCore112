/*
 * RTS building AI: stationary attackable structures (building "cores" and
 * same-model creature twins of building gameobjects).
 *
 * A building never fights back, so nothing ever ends the fight: once attacked,
 * the creature holds every attacker in its combat set indefinitely, and a
 * player who poked it once stays flagged in combat until they physically leave
 * the area. That is wrong for an RTS, where you disengage from a structure the
 * moment you stop hitting it.
 *
 * This AI makes structure combat behave like player combat: five seconds after
 * the last damage taken, the building clears its own threat and combat state,
 * which releases everyone it was holding. Damage-over-time effects keep the
 * timer fresh, so a dotted building correctly keeps its attacker in combat
 * until the dots fall off.
 *
 * Deliberately NOT an evade (EnterEvadeMode): evading is a full creature reset
 * and restores health, and a building must keep its damage when the fight
 * pauses. Health only comes back if someone repairs (heals) it.
 *
 * Deliberately NO client-control tricks for the garrison (vehicle) layer.
 * Earlier revisions revoked SetClientControl on boarding and near-teleported
 * the rider on exit; both fought the client's own control handback and locked
 * the camera to the vehicle. The final architecture makes the rider a
 * PASSENGER, never the mover: vehicle kit 1000's only seat (90000) has
 * CAN_CAST without CAN_CONTROL, so the client physically cannot steer, turn
 * or jump the building - exact zero, by construction - while Vehicle.cpp
 * still hands the rider the building's action bar (the stock gunner
 * arrangement). Every possess-seat immobilizer failed before this: stun
 * dead-buttons Leave Vehicle, facing limits accumulate per input, TurnSpeed
 * is ignored for non-turret kits, and FIXED_POSITION breaks exits. The
 * permanent root (aura 42716, re-asserted below) and the zero turn rate are
 * kept as server-side belts.
 */

#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "ScriptMgr.h"
#include "ThreatManager.h"

namespace
{
    constexpr uint32 RTS_BUILDING_COMBAT_DROP_MS = 5 * IN_MILLISECONDS;

    // Building creature -> its invisible collision-shell gameobject (the
    // --collision-only conversion of the same model). The AI summons the
    // shell itself, co-located and co-oriented, so placing a building is ONE
    // spawn: the creature. No separate .gobject add, no DB gameobject row.
    struct RtsBuildingShell
    {
        uint32 creatureEntry;
        uint32 shellEntry;
    };
    constexpr RtsBuildingShell RTS_BUILDING_SHELLS[] =
    {
        { 900116, 900001 }, // Goblin Workshop
    };

    uint32 ShellEntryFor(uint32 creatureEntry)
    {
        for (RtsBuildingShell const& pair : RTS_BUILDING_SHELLS)
            if (pair.creatureEntry == creatureEntry)
                return pair.shellEntry;
        return 0;
    }
}

struct RtsBuildingAI : public CreatureAI
{
    explicit RtsBuildingAI(Creature* creature)
        : CreatureAI(creature), _sinceLastHit(0)
    {
        creature->SetReactState(REACT_PASSIVE);
        // Belt: the non-control seat already makes rider-driven movement
        // impossible, but the rule is EXACT zero with no code path exempt,
        // so the unit's own turn rate is pinned too (SetSpeedRate has no
        // floor clamp - a true 0 survives).
        creature->SetSpeed(MOVE_TURN_RATE, 0.0f);
    }

    // Structures never act on their own: no target selection, no chasing, no
    // retaliation. Combat exists only as a record of who is hitting them.
    void AttackStart(Unit* /*victim*/) override { }
    void JustEngagedWith(Unit* /*attacker*/) override { _sinceLastHit = 0; }

    void JustAppeared() override
    {
        uint32 const shellEntry = ShellEntryFor(me->GetEntry());
        if (!shellEntry)
            return;

        // Adopt a shell that already exists at this spot (a DB-spawned one,
        // or one left standing from this creature's previous life) rather
        // than stacking a second collision mesh inside it.
        if (GameObject* existing = me->FindNearestGameObject(shellEntry, 1.0f))
        {
            _shellGuid = existing->GetGUID();
            return;
        }

        if (GameObject* shell = me->SummonGameObject(shellEntry, me->GetPosition(),
                QuaternionData::fromEulerAnglesZYX(me->GetOrientation(), 0.0f, 0.0f), 0s))
            _shellGuid = shell->GetGUID();
    }

    // A dead building is rubble: the kill target is gone, so the collision
    // shell goes with it and the footprint becomes walkable.
    void JustDied(Unit* /*killer*/) override
    {
        if (GameObject* shell = ObjectAccessor::GetGameObject(*me, _shellGuid))
            shell->Delete();
        _shellGuid.Clear();
    }

    void DamageTaken(Unit* /*attacker*/, uint32& /*damage*/,
                     DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        _sinceLastHit = 0;
    }

    void UpdateAI(uint32 diff) override
    {
        // A building is architecture: keep it pinned even if the addon aura
        // is ever dispelled or a future building template forgets the aura.
        if (!me->HasUnitState(UNIT_STATE_ROOT))
            me->SetControlled(true, UNIT_STATE_ROOT);

        if (!me->IsInCombat())
            return;

        _sinceLastHit += diff;
        if (_sinceLastHit < RTS_BUILDING_COMBAT_DROP_MS)
            return;

        // Dropping the threat list and combat state releases every player the
        // building was holding in combat; their own client-side combat state
        // clears moments later, exactly like disengaging from a player.
        me->GetThreatManager().ClearAllThreat();
        me->CombatStop(true);
        _sinceLastHit = 0;
    }

private:
    uint32 _sinceLastHit;
    ObjectGuid _shellGuid;
};

class npc_rts_building : public CreatureScript
{
public:
    npc_rts_building() : CreatureScript("npc_rts_building") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new RtsBuildingAI(creature);
    }
};

void AddSC_rts_building()
{
    new npc_rts_building();
}
