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

#include "ScriptMgr.h"
#include "GameObject.h"
#include "Map.h"
#include "nexus.h"
#include "ScriptedCreature.h"
#include "SpellScript.h"

enum Spells
{
    SPELL_SPELL_REFLECTION                      = 47981,
    SPELL_TRAMPLE                               = 48016,
    SPELL_FRENZY                                = 48017,
    SPELL_SUMMON_CRYSTALLINE_TANGLER            = 61564,
    SPELL_CRYSTAL_SPIKES                        = 47958,

    SPELL_SUMMON_CRYSTAL_SPIKE_1                = 47954,
    SPELL_SUMMON_CRYSTAL_SPIKE_2                = 47955,
    SPELL_SUMMON_CRYSTAL_SPIKE_3                = 47956,
    SPELL_SUMMON_CRYSTAL_SPIKE_4                = 47957,

    SPELL_SUMMON_CRYSTAL_SPIKE_1_H              = 57077,
    SPELL_SUMMON_CRYSTAL_SPIKE_2_H              = 57078,
    SPELL_SUMMON_CRYSTAL_SPIKE_3_H              = 57080,
    SPELL_SUMMON_CRYSTAL_SPIKE_4_H              = 57081
};

enum Yells
{
    SAY_AGGRO                                   = 1,
    SAY_DEATH                                   = 2,
    SAY_REFLECT                                 = 3,
    SAY_CRYSTAL_SPIKES                          = 4,
    SAY_KILL                                    = 5,
    SAY_FRENZY                                  = 6
};

enum Events
{
    EVENT_CRYSTAL_SPIKES                        = 1,
    EVENT_TRAMPLE                               = 2,
    EVENT_SPELL_REFLECTION                      = 3,
    EVENT_CRYSTALLINE_TANGLER                   = 4,
};

class OrmorokTanglerPredicate
{
   public:
      OrmorokTanglerPredicate(Unit* unit) : me(unit) { }

    bool operator() (WorldObject* object) const
    {
        return object->GetDistance2d(me) >= 5.0f;
    }

    private:
        Unit* me;
};

struct boss_ormorok : public BossAI
{
    boss_ormorok(Creature* creature) : BossAI(creature, DATA_ORMOROK)
    {
        Initialize();
    }

    void Initialize()
    {
        frenzy = false;
    }

    void Reset() override
    {
        BossAI::Reset();
        Initialize();
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);

        events.ScheduleEvent(EVENT_CRYSTAL_SPIKES, 12s);
        events.ScheduleEvent(EVENT_TRAMPLE, 10s);
        events.ScheduleEvent(EVENT_SPELL_REFLECTION, 30s);
        if (IsHeroic())
            events.ScheduleEvent(EVENT_CRYSTALLINE_TANGLER, 15s);

        Talk(SAY_AGGRO);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& /*damage*/, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo = nullptr*/) override
    {
        if (!frenzy && HealthBelowPct(25))
        {
            Talk(SAY_FRENZY);
            DoCast(me, SPELL_FRENZY);
            frenzy = true;
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        _JustDied();
        Talk(SAY_DEATH);
    }

    void KilledUnit(Unit* who) override
    {
        if (who->GetTypeId() == TYPEID_PLAYER)
            Talk(SAY_KILL);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_TRAMPLE:
                    DoCast(me, SPELL_TRAMPLE);
                    events.ScheduleEvent(EVENT_TRAMPLE, 10s);
                    break;
                case EVENT_SPELL_REFLECTION:
                    Talk(SAY_REFLECT);
                    DoCast(me, SPELL_SPELL_REFLECTION);
                    events.ScheduleEvent(EVENT_SPELL_REFLECTION, 30s);
                    break;
                case EVENT_CRYSTAL_SPIKES:
                    Talk(SAY_CRYSTAL_SPIKES);
                    DoCast(SPELL_CRYSTAL_SPIKES);
                    events.ScheduleEvent(EVENT_CRYSTAL_SPIKES, 12s);
                    break;
                case EVENT_CRYSTALLINE_TANGLER:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, OrmorokTanglerPredicate(me)))
                        DoCast(target, SPELL_SUMMON_CRYSTALLINE_TANGLER);
                    events.ScheduleEvent(EVENT_CRYSTALLINE_TANGLER, 15s);
                    break;
                default:
                    break;
            }

            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        DoMeleeAttackIfReady();
    }

private:
    bool frenzy;

};

enum CrystalSpikes
{
    NPC_CRYSTAL_SPIKE_INITIAL        = 27101,
    NPC_CRYSTAL_SPIKE_TRIGGER        = 27079,

    DATA_COUNT                       = 1,
    MAX_COUNT                        = 5,

    SPELL_CRYSTAL_SPIKE_DAMAGE       = 47944,
    SPELL_CRYSTAL_SPIKE_AURA         = 47941,

    GO_CRYSTAL_SPIKE_TRAP            = 188537,
};

uint32 const crystalSpikeSummon[3] =
{
        47936,
        47942,
        47943
};

struct npc_crystal_spike_trigger : public ScriptedAI
{
    npc_crystal_spike_trigger(Creature* creature) : ScriptedAI(creature)
    {
        _count = 0;
        _despawntimer = 0;
    }

    void IsSummonedBy(WorldObject* owner) override
    {
        switch (me->GetEntry())
        {
            case NPC_CRYSTAL_SPIKE_INITIAL:
                _count = 0;
                me->SetFacingToObject(owner);
                me->CastSpell(me, SPELL_CRYSTAL_SPIKE_AURA, true);
                break;
            case NPC_CRYSTAL_SPIKE_TRIGGER:
                me->CastSpell(me, SPELL_CRYSTAL_SPIKE_AURA, true);
                if (Creature* trigger = owner->ToCreature())
                    _count = trigger->AI()->GetData(DATA_COUNT) + 1;
                break;
            default:
                _count = MAX_COUNT;
                break;
        }

        if (me->GetEntry() == NPC_CRYSTAL_SPIKE_TRIGGER)
            if (GameObject* trap = me->FindNearestGameObject(GO_CRYSTAL_SPIKE_TRAP, 1.0f))
                trap->Use(me);

        _despawntimer = 2000;
    }

    uint32 GetData(uint32 type) const override
    {
        return type == DATA_COUNT ? _count : 0;
    }

    void UpdateAI(uint32 diff) override
    {
        if (_despawntimer <= diff)
        {
            if (me->GetEntry() == NPC_CRYSTAL_SPIKE_TRIGGER)
                if (GameObject* trap = me->FindNearestGameObject(GO_CRYSTAL_SPIKE_TRAP, 1.0f))
                    trap->Delete();

            me->DespawnOrUnsummon();
        }
        else
            _despawntimer -= diff;
    }

private:
    uint32 _count;
    uint32 _despawntimer;

};

std::array<uint32, 4> const SummonSpells =
{
    SPELL_SUMMON_CRYSTAL_SPIKE_1, SPELL_SUMMON_CRYSTAL_SPIKE_2, SPELL_SUMMON_CRYSTAL_SPIKE_3, SPELL_SUMMON_CRYSTAL_SPIKE_4
};

std::array<uint32, 4> const SummonSpellsHeroic =
{
    SPELL_SUMMON_CRYSTAL_SPIKE_1_H, SPELL_SUMMON_CRYSTAL_SPIKE_2_H, SPELL_SUMMON_CRYSTAL_SPIKE_3_H, SPELL_SUMMON_CRYSTAL_SPIKE_4_H
};

// 47958, 57082 - Crystal Spikes
class spell_ormorok_summon_crystal_spikes : public SpellScript
{
    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo(SummonSpells) && ValidateSpellInfo(SummonSpellsHeroic);
    }

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        for (uint32 spells : (caster->GetMap()->IsHeroic() ? SummonSpellsHeroic : SummonSpells))
            caster->CastSpell(caster, spells);
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_ormorok_summon_crystal_spikes::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// 47941 - Crystal Spike
class spell_ormorok_crystal_spike : public AuraScript
{
    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        if (target->GetEntry() == NPC_CRYSTAL_SPIKE_INITIAL || target->GetEntry() == NPC_CRYSTAL_SPIKE_TRIGGER)
            if (Creature* trigger = target->ToCreature())
            {
                uint32 spell = target->GetEntry() == NPC_CRYSTAL_SPIKE_INITIAL ? crystalSpikeSummon[0] : crystalSpikeSummon[urand(0, 2)];
                if (trigger->AI()->GetData(DATA_COUNT) < MAX_COUNT)
                    trigger->CastSpell(trigger, spell, true);
            }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ormorok_crystal_spike::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

void AddSC_boss_ormorok()
{
    RegisterNexusCreatureAI(boss_ormorok);
    RegisterNexusCreatureAI(npc_crystal_spike_trigger);
    RegisterSpellScript(spell_ormorok_summon_crystal_spikes);
    RegisterSpellScript(spell_ormorok_crystal_spike);
}
