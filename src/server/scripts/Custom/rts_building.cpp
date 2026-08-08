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
 */

#include "Creature.h"
#include "CreatureAI.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ThreatManager.h"

namespace
{
    constexpr uint32 RTS_BUILDING_COMBAT_DROP_MS = 5 * IN_MILLISECONDS;
}

struct RtsBuildingAI : public CreatureAI
{
    explicit RtsBuildingAI(Creature* creature)
        : CreatureAI(creature), _sinceLastHit(0)
    {
        creature->SetReactState(REACT_PASSIVE);
    }

    // Structures never act on their own: no target selection, no chasing, no
    // retaliation. Combat exists only as a record of who is hitting them.
    void AttackStart(Unit* /*victim*/) override { }
    void JustEngagedWith(Unit* /*attacker*/) override { _sinceLastHit = 0; }

    // Garrison seats give the occupant the building's action bar via
    // CAN_CONTROL - but CAN_CONTROL also lets the rider's client steer, and
    // no seat flag suppresses that: a garrisoned player could spin the whole
    // building with their mouse. Revoke movement control while keeping the
    // possess bar, the same split scripted possessions use. Revoked on the
    // next update tick as well, because the charm's own control grant can
    // land after this hook fires.
    void PassengerBoarded(Unit* passenger, int8 /*seatId*/, bool apply) override
    {
        if (Player* player = passenger->ToPlayer())
        {
            if (apply)
            {
                player->SetClientControl(me, false);
                _controlRevokePending = passenger->GetGUID();
            }
            else
            {
                _controlRevokePending.Clear();
                // Replace the vehicle-exit spline with an instant relocation
                // to the gate. The stock exit walks a spline the whole way,
                // during which the server counts the player as moving and
                // refuses casts; over a building-sized exit distance that is
                // seconds of dead time. Deferred one tick so the exit
                // finishes tearing down before the teleport lands.
                _exitTeleportPending = passenger->GetGUID();
            }
        }
    }

    void DamageTaken(Unit* /*attacker*/, uint32& /*damage*/,
                     DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        _sinceLastHit = 0;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!_controlRevokePending.IsEmpty())
        {
            if (Player* rider = ObjectAccessor::GetPlayer(*me, _controlRevokePending))
                rider->SetClientControl(me, false);
            _controlRevokePending.Clear();
        }

        if (!_exitTeleportPending.IsEmpty())
        {
            if (Player* rider = ObjectAccessor::GetPlayer(*me, _exitTeleportPending))
            {
                // The door is baked onto model +X, so "in front of the gate"
                // is a straight offset along the building's facing.
                float const dist = me->GetCombatReach() + 5.0f;
                float const x = me->GetPositionX() + std::cos(me->GetOrientation()) * dist;
                float const y = me->GetPositionY() + std::sin(me->GetOrientation()) * dist;
                float z = me->GetPositionZ() + 1.0f;
                me->UpdateGroundPositionZ(x, y, z);
                rider->NearTeleportTo(x, y, z, me->GetOrientation());
            }
            _exitTeleportPending.Clear();
        }

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
    ObjectGuid _controlRevokePending;
    ObjectGuid _exitTeleportPending;
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
