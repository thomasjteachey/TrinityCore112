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
// And League's recall: on Summoner's Rift the Hearthstone (and a shaman's Astral
// Recall) takes its caster to its own fountain instead of home - the same cast,
// broken by damage the same way - and is ready again at once.

#include "AddonRequestRoutes.h"
#include "Battleground.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
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

struct npc_rift_minion : public CreatureAI
{
    explicit npc_rift_minion(Creature* creature) : CreatureAI(creature) { }

    void UpdateAI(uint32 /*diff*/) override
    {
        if (!UpdateVictim())
            return;
        DoMeleeAttackIfReady();
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        me->CombatStop(true);
        me->SetTarget(ObjectGuid::Empty);
        me->GetMotionMaster()->Clear();
        me->StopMoving();
    }
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
