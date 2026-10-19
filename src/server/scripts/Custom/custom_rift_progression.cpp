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

// The CENTURION_Rift addon's requests, answered by RiftProgression (game/):
//   CCGAMEREQ\tRIFTPICK:<spellId>   learn or rank up that spell with a skill pick
//   CCGAMEREQ\tRIFTSTATE:           send the match state again (after a /reload)
//
// League's lane minions (npc_rift_minion, creature_template 922110-922113) never
// evade. The stock AI evades the moment its target dies: it turns immune, walks
// back to where the fight started and heals to full there - so waves ran back
// up their lane after every kill, never died, and shrugged off turret shots.
// A minion that runs out of enemies stops where it stands instead, and
// BattlegroundSRF picks its next target or walks it on down its lane.
//
// And no threat, League's way: a minion fights the target the battleground
// names (SetGUID) - the closest enemy - and nothing else moves it. Being hit,
// seeing someone, or whoever did the most damage play no part.
//
// And League's recall: on Summoner's Rift the Hearthstone (and a shaman's Astral
// Recall) takes its caster to its own fountain instead of home - the same cast,
// broken by damage the same way - and is ready again at once.

#include "AddonRequestRoutes.h"
#include "Battleground.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "DBCStores.h"
#include "Player.h"
#include "RiftProgression.h"
#include "ScriptMgr.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellScript.h"

namespace
{
    Battleground* RiftOf(Unit* caster)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        Battleground* bg = player ? player->GetBattleground() : nullptr;
        return bg && bg->GetTypeID(true) == BATTLEGROUND_SRF && player->GetMapId() == bg->GetMapId() ? bg : nullptr;
    }
}

// BattlegroundSRF's entries: turrets, inhibitors and nexuses; the lane minions.
uint32 constexpr RIFT_STRUCTURE_FIRST = 922100;
uint32 constexpr RIFT_STRUCTURE_LAST = 922105;
uint32 constexpr RIFT_MINION_FIRST = 922110;
uint32 constexpr RIFT_MINION_LAST = 922113;

// SetGUID id: a caster keeps its own distance (BattlegroundSRF moves it).
int32 constexpr RIFT_MINION_TARGET_MELEE = 0;
int32 constexpr RIFT_MINION_TARGET_RANGED = 1;

struct npc_rift_minion : public CreatureAI
{
    explicit npc_rift_minion(Creature* creature) : CreatureAI(creature) { }

    // Only the battleground picks a target.
    void AttackStart(Unit* /*who*/) override { }
    void MoveInLineOfSight(Unit* /*who*/) override { }

    void SetGUID(ObjectGuid const& guid, int32 id) override
    {
        _target = guid;
        Unit* victim = guid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*me, guid);
        if (!victim)
        {
            me->AttackStop();
            return;
        }
        if (me->GetVictim() == victim)
            return;
        me->Attack(victim, true);
        if (id == RIFT_MINION_TARGET_MELEE)
            me->GetMotionMaster()->MoveChase(victim);
    }

    void UpdateAI(uint32 /*diff*/) override
    {
        if (_target.IsEmpty())
            return;
        Unit* victim = ObjectAccessor::GetUnit(*me, _target);
        if (!victim || !victim->IsAlive() || !me->IsValidAttackTarget(victim))
        {
            _target.Clear();
            me->AttackStop();
            return;
        }
        if (me->GetVictim() != victim)
            me->Attack(victim, true);
        DoMeleeAttackIfReady();
    }

    // League's damage: 60% against champions and structures, and 2% of a lane
    // minion's current health on top against other minions.
    void DamageDealt(Unit* victim, uint32& damage, DamageEffectType /*damageType*/) override
    {
        uint32 const entry = victim->GetTypeId() == TYPEID_UNIT ? victim->GetEntry() : 0;
        if (victim->GetTypeId() == TYPEID_PLAYER || (entry >= RIFT_STRUCTURE_FIRST && entry <= RIFT_STRUCTURE_LAST))
            damage = damage * 60 / 100;
        else if (entry >= RIFT_MINION_FIRST && entry <= RIFT_MINION_LAST)
            damage += victim->GetHealth() * 2 / 100;
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        _target.Clear();
        me->CombatStop(true);
        me->SetTarget(ObjectGuid::Empty);
        me->GetMotionMaster()->Clear();
        me->StopMoving();
    }

private:
    ObjectGuid _target;
};

// 8690 Hearthstone, 556 Astral Recall
class spell_rift_recall : public SpellScript
{
    PrepareSpellScript(spell_rift_recall);

    void HandleTeleport(SpellEffIndex effIndex)
    {
        Battleground* bg = RiftOf(GetCaster());
        if (!bg)
            return;
        PreventHitDefaultEffect(effIndex);
        Player* player = GetCaster()->ToPlayer();
        if (WorldSafeLocsEntry const* fountain = bg->GetClosestGraveyard(player))
            player->NearTeleportTo(fountain->Loc.X, fountain->Loc.Y, fountain->Loc.Z, player->GetOrientation());
    }

    void HandleAfterCast()
    {
        if (RiftOf(GetCaster()))
            GetCaster()->GetSpellHistory()->ResetCooldown(GetSpellInfo()->Id, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_rift_recall::HandleTeleport, EFFECT_0, SPELL_EFFECT_TELEPORT_UNITS);
        AfterCast += SpellCastFn(spell_rift_recall::HandleAfterCast);
    }
};

void AddSC_custom_rift_progression()
{
    AddonRequestRoutes::Register("RIFTPICK", &RiftProgression::HandlePickRequest);
    AddonRequestRoutes::Register("RIFTSTATE", &RiftProgression::HandleStateRequest);
    RegisterSpellScript(spell_rift_recall);
    RegisterCreatureAI(npc_rift_minion);
}
