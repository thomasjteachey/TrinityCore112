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
 * the camera to the vehicle. Garrisoning is handled entirely by data instead:
 * the vehicle kit's facing limits freeze rotation, a vehicle_seat_addon
 * offset places the exit, and a permanent root pins the building - the aura
 * 42716 in creature_template_addon, re-asserted below in case anything ever
 * strips it. A ROOT, not a stun: the client refuses the Leave Vehicle action
 * while its mover is stunned, and a stunned vehicle cannot cast its bar.
 */

#include "Creature.h"
#include "CreatureAI.h"
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
