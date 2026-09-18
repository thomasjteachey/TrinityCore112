/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Thaddius, with Stalagg and Feugen. Ported from
 * mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0);
 * Naxxramas 40 scripts by Sogladev.
 *
 * The polarity spells themselves stay on TrinityCore's own scripts - they are
 * the same spell ids in every version - with the 40-player damage and stack cap
 * branched inside boss_thaddius.cpp.
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

#include "ScriptMgr.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"

enum ThaddiusSays40
{
    // Stalagg
    SAY_STAL_AGGRO                      = 0,
    SAY_STAL_SLAY                       = 1,
    SAY_STAL_DEATH                      = 2,
    EMOTE_STAL_DEATH                    = 3,
    EMOTE_STAL_REVIVE                   = 4,

    // Feugen
    SAY_FEUG_AGGRO                      = 0,
    SAY_FEUG_SLAY                       = 1,
    SAY_FEUG_DEATH                      = 2,
    EMOTE_FEUG_DEATH                    = 3,
    EMOTE_FEUG_REVIVE                   = 4,

    // Thaddius
    SAY_AGGRO                           = 1,
    SAY_SLAY                            = 2,
    SAY_ELECT                           = 3,
    SAY_DEATH                           = 4,
    EMOTE_POLARITY_SHIFTED              = 6,

    // Tesla coil
    EMOTE_TESLA_LINK_BREAKS             = 0,
    EMOTE_TESLA_OVERLOAD                = 1
};

enum ThaddiusSpells40
{
    SPELL_MAGNETIC_PULL                 = 28337,
    SPELL_TESLA_SHOCK                   = 28099,
    SPELL_SHOCK_VISUAL                  = 28159,

    SPELL_POWER_SURGE                   = 28134,
    SPELL_STALAGG_CHAIN                 = 28096,

    SPELL_STATIC_FIELD                  = 28135,
    SPELL_FEUGEN_CHAIN                  = 28111,

    SPELL_POLARITY_SHIFT                = 28089,
    SPELL_BALL_LIGHTNING                = 28299,
    SPELL_CHAIN_LIGHTNING               = 28167,
    SPELL_BERSERK                       = 27680,
    SPELL_THADDIUS_VISUAL_LIGHTNING     = 28136,
    SPELL_THADDIUS_SPAWN_STUN           = 28160,

    SPELL_POSITIVE_CHARGE_STACK         = 29659,
    SPELL_NEGATIVE_CHARGE_STACK         = 29660,
    SPELL_POSITIVE_POLARITY             = 28059,
    SPELL_NEGATIVE_POLARITY             = 28084
};

enum ThaddiusEvents40
{
    EVENT_MINION_POWER_SURGE            = 1,
    EVENT_MINION_MAGNETIC_PULL          = 2,
    EVENT_MINION_CHECK_DISTANCE         = 3,
    EVENT_MINION_STATIC_FIELD           = 4,

    EVENT_THADDIUS_INIT                 = 5,
    EVENT_THADDIUS_ENTER_COMBAT         = 6,
    EVENT_THADDIUS_CHAIN_LIGHTNING      = 7,
    EVENT_THADDIUS_BERSERK              = 8,
    EVENT_THADDIUS_POLARITY_SHIFT       = 9,
    EVENT_ALLOW_BALL_LIGHTNING          = 10
};

enum ThaddiusMisc40
{
    ACTION_MAGNETIC_PULL                = 1,
    ACTION_SUMMON_DIED                  = 2,
    ACTION_RESTORE                      = 3,

    GO_TESLA_COIL_LEFT                  = 181478,
    GO_TESLA_COIL_RIGHT                 = 181477,
    NPC_TESLA_COIL                      = 16218
};

struct boss_thaddius_40 : public VanillaRaidsBossAI
{
    boss_thaddius_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_THADDIUS) { }

    bool IsAnyPlayerInMeleeRange() const
    {
        for (ThreatReference const* ref : me->GetThreatManager().GetUnsortedThreatList())
            if (Unit* target = ref->GetVictim())
                if (target->IsPlayer() && me->IsWithinMeleeRange(target))
                    return true;

        return false;
    }

    void DoAction(int32 param) override
    {
        if (param != ACTION_SUMMON_DIED)
            return;

        // The first add down starts the revive countdown; the second one
        // starts the transition instead.
        if (_summonTimer)
        {
            _summonTimer = 0;
            _reviveTimer = 1;
            return;
        }

        _summonTimer = 1;
    }

    void ClearPolarityAuras()
    {
        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_POSITIVE_POLARITY);
        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_POSITIVE_CHARGE_STACK);
        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_NEGATIVE_POLARITY);
        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_NEGATIVE_CHARGE_STACK);
    }

    void SummonCoil(float x, float y, float z, uint32 chainSpell)
    {
        TempSummon* coil = me->SummonCreature(NPC_TESLA_COIL, x, y, z, 0.0f);
        if (!coil)
            return;

        coil->RemoveAllAuras();
        coil->InterruptNonMeleeSpells(true);
        coil->CastSpell(coil, chainSpell, CastSpellExtraArgs(TRIGGERED_NONE));
        coil->SetDisableGravity(true);
        coil->SetImmuneToPC(false);
        coil->SetControlled(true, UNIT_STATE_ROOT);
    }

    void Reset() override
    {
        BossAI::Reset();
        events.Reset();
        summons.DespawnAll();
        me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
        me->SetControlled(true, UNIT_STATE_ROOT);
        _summonTimer = 0;
        _reviveTimer = 0;
        _resetTimer = 1;
        me->NearTeleportTo(me->GetHomePosition());
        _ballLightningEnabled = false;

        me->SummonCreature(NPC_STALAGG_40, 3450.45f, -2931.42f, 312.091f, 5.49779f);
        me->SummonCreature(NPC_FEUGEN_40, 3508.14f, -2988.65f, 312.092f, 2.37365f);
        SummonCoil(3527.34f, -2951.56f, 318.75f, SPELL_FEUGEN_CHAIN);
        SummonCoil(3487.04f, -2911.68f, 318.75f, SPELL_STALAGG_CHAIN);

        if (GameObject* go = me->FindNearestGameObject(GO_TESLA_COIL_LEFT, 100.0f))
            go->SetGoState(GO_STATE_ACTIVE);
        if (GameObject* go = me->FindNearestGameObject(GO_TESLA_COIL_RIGHT, 100.0f))
            go->SetGoState(GO_STATE_ACTIVE);

        ClearPolarityAuras();
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        Talk(SAY_SLAY);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        Talk(SAY_DEATH);
        ClearPolarityAuras();
    }

    void JustSummoned(Creature* summon) override
    {
        summons.Summon(summon);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        DoZoneInCombat();
        summons.DoZoneInCombat(NPC_FEUGEN_40);
        summons.DoZoneInCombat(NPC_STALAGG_40);
    }

    // Applies an action to every tesla coil this encounter summoned.
    template <typename Action>
    void ForEachCoil(Action&& action)
    {
        for (ObjectGuid const& guid : summons)
            if (Creature* coil = ObjectAccessor::GetCreature(*me, guid))
                if (coil->GetEntry() == NPC_TESLA_COIL)
                    action(coil);
    }

    void UpdateAI(uint32 diff) override
    {
        if (_resetTimer)
        {
            _resetTimer += diff;
            if (_resetTimer > 1000)
            {
                _resetTimer = 0;
                DoCastSelf(SPELL_THADDIUS_SPAWN_STUN, CastSpellExtraArgs(true));
            }

            return;
        }

        if (_reviveTimer)
        {
            _reviveTimer += diff;
            if (_reviveTimer >= 12000)
            {
                ForEachCoil([this](Creature* coil)
                {
                    coil->AI()->Talk(EMOTE_TESLA_OVERLOAD);
                    coil->CastSpell(me, SPELL_SHOCK_VISUAL, CastSpellExtraArgs(true));
                });

                _reviveTimer = 0;
                events.ScheduleEvent(EVENT_THADDIUS_INIT, 750ms);
            }

            return;
        }

        if (!UpdateVictim())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        if (_summonTimer) // one add is down, put it back up
        {
            _summonTimer += diff;
            if (_summonTimer >= 5000)
            {
                for (ObjectGuid const& guid : summons)
                    if (Creature* summon = ObjectAccessor::GetCreature(*me, guid))
                        if (summon->GetEntry() == NPC_STALAGG_40 || summon->GetEntry() == NPC_FEUGEN_40)
                            summon->AI()->DoAction(ACTION_RESTORE);

                _summonTimer = 0;
            }
        }

        switch (events.ExecuteEvent())
        {
            case EVENT_THADDIUS_INIT:
                me->RemoveAllAuras();
                me->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                ForEachCoil([](Creature* coil) { Unit::Kill(coil, coil); });

                if (GameObject* go = me->FindNearestGameObject(GO_TESLA_COIL_LEFT, 100.0f))
                    go->SetGoState(GO_STATE_READY);
                if (GameObject* go = me->FindNearestGameObject(GO_TESLA_COIL_RIGHT, 100.0f))
                    go->SetGoState(GO_STATE_READY);

                DoCastSelf(SPELL_THADDIUS_VISUAL_LIGHTNING, CastSpellExtraArgs(true));
                events.ScheduleEvent(EVENT_THADDIUS_ENTER_COMBAT, 1s);
                break;
            case EVENT_THADDIUS_ENTER_COMBAT:
                Talk(SAY_AGGRO);
                me->SetReactState(REACT_AGGRESSIVE);
                me->SetControlled(false, UNIT_STATE_STUNNED);
                me->SetControlled(false, UNIT_STATE_ROOT);
                me->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                events.ScheduleEvent(EVENT_THADDIUS_CHAIN_LIGHTNING, 14s);
                events.ScheduleEvent(EVENT_THADDIUS_BERSERK, 6min);
                events.ScheduleEvent(EVENT_THADDIUS_POLARITY_SHIFT, 20s);
                events.ScheduleEvent(EVENT_ALLOW_BALL_LIGHTNING, 5s);
                return;
            case EVENT_THADDIUS_BERSERK:
                DoCastSelf(SPELL_BERSERK, CastSpellExtraArgs(true));
                break;
            case EVENT_THADDIUS_CHAIN_LIGHTNING:
                me->CastSpell(me->GetVictim(), SPELL_CHAIN_LIGHTNING, CastSpellExtraArgs(TRIGGERED_NONE)
                    .AddSpellBP0(1850) // 1850-2150, die 675
                    .AddSpellMod(SPELLVALUE_MAX_TARGETS, 15));
                events.Repeat(15s);
                break;
            case EVENT_THADDIUS_POLARITY_SHIFT:
                // TrinityCore's spell_thaddius_polarity_shift hands out the
                // charges; the shout belongs to the boss.
                DoCastSelf(SPELL_POLARITY_SHIFT);
                Talk(SAY_ELECT);
                Talk(EMOTE_POLARITY_SHIFTED);
                events.Repeat(30s);
                break;
            case EVENT_ALLOW_BALL_LIGHTNING:
                _ballLightningEnabled = true;
                break;
            default:
                break;
        }

        if (IsAnyPlayerInMeleeRange())
            DoMeleeAttackIfReady();
        else if (_ballLightningEnabled && !me->HasUnitState(UNIT_STATE_CASTING))
        {
            if (Unit* target = SelectTarget(SelectTargetMethod::MaxThreat))
                me->CastSpell(target, SPELL_BALL_LIGHTNING, CastSpellExtraArgs(TRIGGERED_NONE).AddSpellBP0(6000));
        }
    }

private:
    uint32 _summonTimer = 0;
    uint32 _reviveTimer = 0;
    uint32 _resetTimer = 0;
    bool _ballLightningEnabled = false;
};

// Stalagg and Feugen: one script, branching on entry, as upstream has it.
struct boss_thaddius_summon_40 : public ScriptedAI
{
    boss_thaddius_summon_40(Creature* creature) : ScriptedAI(creature) { }

    bool IsStalagg() const { return me->GetEntry() == NPC_STALAGG_40; }
    uint32 ChainSpell() const { return IsStalagg() ? SPELL_STALAGG_CHAIN : SPELL_FEUGEN_CHAIN; }

    void Reset() override
    {
        _pullTimer = 0;
        _visualTimer = 1;
        _overload = false;
        _events.Reset();
        me->SetControlled(false, UNIT_STATE_STUNNED);

        if (Creature* coil = me->FindNearestCreature(NPC_TESLA_COIL, 150.0f))
        {
            coil->CastSpell(coil, ChainSpell(), CastSpellExtraArgs(TRIGGERED_NONE));
            coil->SetImmuneToPC(false);
            _myCoil = coil->GetGUID();
        }
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        me->SetControlled(false, UNIT_STATE_STUNNED);
        ScriptedAI::EnterEvadeMode(why);
    }

    void JustEngagedWith(Unit* who) override
    {
        DoZoneInCombat();

        if (Creature* coil = me->FindNearestCreature(NPC_TESLA_COIL, 150.0f, true))
            _myCoil = coil->GetGUID();

        if (IsStalagg())
        {
            _events.ScheduleEvent(EVENT_MINION_POWER_SURGE, 10s);
            Talk(SAY_STAL_AGGRO);
            // Only Stalagg drives the swap, so the two sides stay in step.
            _events.ScheduleEvent(EVENT_MINION_MAGNETIC_PULL, 20s);
        }
        else
        {
            _events.ScheduleEvent(EVENT_MINION_STATIC_FIELD, 5s);
            Talk(SAY_FEUG_AGGRO);
        }

        _events.ScheduleEvent(EVENT_MINION_CHECK_DISTANCE, 5s);

        if (InstanceScript* instance = me->GetInstanceScript())
        {
            if (Creature* thaddius = instance->GetCreature(DATA_THADDIUS))
            {
                thaddius->AI()->AttackStart(who);
                thaddius->GetThreatManager().AddThreat(who, 10.0f);
            }
        }
    }

    void DoAction(int32 param) override
    {
        if (param == ACTION_MAGNETIC_PULL)
        {
            _pullTimer = 1;
            me->SetControlled(true, UNIT_STATE_STUNNED);
        }
        else if (param == ACTION_RESTORE)
        {
            if (!me->IsAlive())
            {
                me->Respawn();
                DoZoneInCombat();
                Talk(IsStalagg() ? EMOTE_STAL_REVIVE : EMOTE_FEUG_REVIVE);
            }
            else
                me->SetHealth(me->GetMaxHealth());
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        Talk(IsStalagg() ? SAY_STAL_DEATH : SAY_FEUG_DEATH);
        Talk(IsStalagg() ? EMOTE_STAL_DEATH : EMOTE_FEUG_DEATH);

        if (InstanceScript* instance = me->GetInstanceScript())
            if (Creature* thaddius = instance->GetCreature(DATA_THADDIUS))
                thaddius->AI()->DoAction(ACTION_SUMMON_DIED);
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        if (!urand(0, 2))
            Talk(IsStalagg() ? SAY_STAL_SLAY : SAY_FEUG_SLAY);
    }

    void DoMagneticPull()
    {
        InstanceScript* instance = me->GetInstanceScript();
        if (!instance)
            return;

        Creature* feugen = instance->GetCreature(DATA_FEUGEN);
        if (!feugen || !feugen->IsAlive() || !feugen->GetVictim() || !me->GetVictim())
            return;

        Unit* tankFeugen = feugen->GetVictim();
        Unit* tankStalagg = me->GetVictim();
        float const threatFeugen = feugen->GetThreatManager().GetThreat(tankFeugen);
        float const threatStalagg = me->GetThreatManager().GetThreat(tankStalagg);

        feugen->GetThreatManager().ModifyThreatByPercent(tankFeugen, -100);
        feugen->GetThreatManager().AddThreat(tankStalagg, threatFeugen);
        feugen->CastSpell(tankStalagg, SPELL_MAGNETIC_PULL, CastSpellExtraArgs(true));
        feugen->AI()->DoAction(ACTION_MAGNETIC_PULL);

        me->GetThreatManager().ModifyThreatByPercent(tankStalagg, -100);
        me->GetThreatManager().AddThreat(tankFeugen, threatStalagg);
        me->CastSpell(tankFeugen, SPELL_MAGNETIC_PULL, CastSpellExtraArgs(true));
        DoAction(ACTION_MAGNETIC_PULL);
    }

    void UpdateAI(uint32 diff) override
    {
        if (_visualTimer)
        {
            _visualTimer += diff;
            if (_visualTimer >= 3000)
            {
                _visualTimer = 0;
                if (Creature* coil = me->FindNearestCreature(NPC_TESLA_COIL, 150.0f))
                    coil->CastSpell(coil, ChainSpell(), CastSpellExtraArgs(TRIGGERED_NONE));
            }
        }

        if (!UpdateVictim())
            return;

        if (_pullTimer) // AI is off while the swap plays out
        {
            _pullTimer += diff;
            if (_pullTimer >= 3000)
            {
                me->SetControlled(false, UNIT_STATE_STUNNED);
                _pullTimer = 0;
            }

            return;
        }

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (_events.ExecuteEvent())
        {
            case EVENT_MINION_POWER_SURGE:
                DoCastSelf(SPELL_POWER_SURGE);
                _events.Repeat(19s);
                break;
            case EVENT_MINION_STATIC_FIELD:
                DoCastSelf(SPELL_STATIC_FIELD);
                _events.Repeat(3s);
                break;
            case EVENT_MINION_MAGNETIC_PULL:
                _events.Repeat(20s);
                DoMagneticPull();
                break;
            case EVENT_MINION_CHECK_DISTANCE:
                // Dragged too far from his coil, the link snaps and the coil
                // starts shocking the raid instead.
                if (Creature* coil = ObjectAccessor::GetCreature(*me, _myCoil))
                {
                    if (!me->GetHomePosition().IsInDist(me, 28.0f) && me->IsInCombat())
                    {
                        if (!_overload)
                        {
                            _overload = true;
                            coil->AI()->Talk(EMOTE_TESLA_LINK_BREAKS);
                            me->RemoveAurasDueToSpell(ChainSpell());
                            coil->InterruptNonMeleeSpells(true);
                        }

                        if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 1000.0f, true))
                        {
                            coil->CastStop(SPELL_TESLA_SHOCK);
                            coil->CastSpell(target, SPELL_TESLA_SHOCK,
                                CastSpellExtraArgs(true).AddSpellBP0(4374));
                        }

                        _events.Repeat(1500ms);
                        break;
                    }

                    _overload = false;
                    coil->CastSpell(coil, ChainSpell(), CastSpellExtraArgs(TRIGGERED_NONE));
                }

                _events.Repeat(5s);
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    EventMap _events;
    uint32 _pullTimer = 0;
    uint32 _visualTimer = 0;
    bool _overload = false;
    ObjectGuid _myCoil;
};

void AddSC_boss_thaddius_40()
{
    RegisterNaxxramasCreatureAI(boss_thaddius_40);
    RegisterNaxxramasCreatureAI(boss_thaddius_summon_40);
}
