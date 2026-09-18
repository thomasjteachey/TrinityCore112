/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Gothik the Harvester. Ported from mod-individual-progression
 * (ZhengPeiRu21, AzerothCore, AGPL-3.0); Naxxramas 40 scripts by Sogladev.
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
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "ThreatManager.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <iterator>
#include <list>
#include <vector>

enum GothikYells40
{
    SAY_INTRO_1                     = 0,
    SAY_INTRO_2                     = 1,
    SAY_INTRO_3                     = 2,
    SAY_INTRO_4                     = 3,
    SAY_PHASE_TWO                   = 4,
    SAY_DEATH                       = 5,
    SAY_KILL                        = 6,

    EMOTE_PHASE_TWO                 = 7,
    EMOTE_GATE_OPENED               = 8
};

enum GothikSpells40
{
    // Gothik
    SPELL_HARVEST_SOUL              = 28679,
    SPELL_SHADOW_BOLT               = 29317,
    SPELL_TELEPORT_DEAD             = 28025,
    SPELL_TELEPORT_LIVE             = 28026,
    // The chain that turns a dead trainee/knight/rider into its undead twin
    SPELL_ANCHOR_1_TRAINEE          = 27892,
    SPELL_ANCHOR_1_DK               = 27928,
    SPELL_ANCHOR_1_RIDER            = 27935,
    SPELL_ANCHOR_2_TRAINEE          = 27893,
    SPELL_ANCHOR_2_DK               = 27929,
    SPELL_ANCHOR_2_RIDER            = 27936,
    SPELL_SKULLS_TRAINEE            = 27915,
    SPELL_SKULLS_DK                 = 27931,
    SPELL_SKULLS_RIDER              = 27937,
    // Adds
    SPELL_DEATH_PLAGUE              = 55604,
    SPELL_ARCANE_EXPLOSION          = 27989,
    SPELL_SHADOW_MARK               = 27825,
    SPELL_WHIRLWIND                 = 56408,
    SPELL_SHADOW_BOLT_VOLLEY        = 27831,
    SPELL_DRAIN_LIFE                = 27994,
    SPELL_UNHOLY_FRENZY             = 55648,
    SPELL_STOMP                     = 27993
};

enum GothikEvents40
{
    // Gothik
    EVENT_SUMMON_ADDS               = 1,
    EVENT_HARVEST_SOUL              = 2,
    EVENT_SHADOW_BOLT               = 3,
    EVENT_TELEPORT                  = 4,
    EVENT_CHECK_HEALTH              = 5,
    EVENT_CHECK_PLAYERS             = 6,
    // Adds
    EVENT_DEATH_PLAGUE              = 7,
    EVENT_ARCANE_EXPLOSION          = 8,
    EVENT_SHADOW_MARK               = 9,
    EVENT_WHIRLWIND                 = 10,
    EVENT_SHADOW_BOLT_VOLLEY        = 11,
    EVENT_DRAIN_LIFE                = 12,
    EVENT_UNHOLY_FRENZY             = 13,
    EVENT_STOMP                     = 14,
    // Intro
    EVENT_INTRO_2                   = 15,
    EVENT_INTRO_3                   = 16,
    EVENT_INTRO_4                   = 17
};

uint32 const gothikWaves40[24][2] =
{
    { NPC_LIVING_TRAINEE_40,    20000 },
    { NPC_LIVING_TRAINEE_40,    20000 },
    { NPC_LIVING_TRAINEE_40,    10000 },
    { NPC_LIVING_KNIGHT_40,     10000 },
    { NPC_LIVING_TRAINEE_40,    15000 },
    { NPC_LIVING_KNIGHT_40,     10000 },
    { NPC_LIVING_TRAINEE_40,    15000 },
    { NPC_LIVING_TRAINEE_40,        0 },
    { NPC_LIVING_KNIGHT_40,     10000 },
    { NPC_LIVING_RIDER_40,      10000 },
    { NPC_LIVING_TRAINEE_40,     5000 },
    { NPC_LIVING_KNIGHT_40,     15000 },
    { NPC_LIVING_RIDER_40,          0 },
    { NPC_LIVING_TRAINEE_40,    10000 },
    { NPC_LIVING_KNIGHT_40,     10000 },
    { NPC_LIVING_TRAINEE_40,    10000 },
    { NPC_LIVING_RIDER_40,       5000 },
    { NPC_LIVING_KNIGHT_40,      5000 },
    { NPC_LIVING_TRAINEE_40,    20000 },
    { NPC_LIVING_RIDER_40,          0 },
    { NPC_LIVING_KNIGHT_40,         0 },
    { NPC_LIVING_TRAINEE_40,    15000 },
    { NPC_LIVING_TRAINEE_40,    29000 },
    { 0, 0 }
};

Position const PosSummonLiving40[6] =
{
    { 2669.7f, -3428.76f, 268.56f, 1.6f },
    { 2692.1f, -3428.76f, 268.56f, 1.6f },
    { 2714.4f, -3428.76f, 268.56f, 1.6f },
    { 2669.7f, -3431.67f, 268.56f, 1.6f },
    { 2692.1f, -3431.67f, 268.56f, 1.6f },
    { 2714.4f, -3431.67f, 268.56f, 1.6f }
};

static constexpr float POS_Y_GATE_40  = -3360.78f;
static constexpr float POS_Y_WEST_40  = -3285.0f;
static constexpr float POS_Y_EAST_40  = -3434.0f;
static constexpr float POS_X_NORTH_40 =  2750.49f;
static constexpr float POS_X_SOUTH_40 =  2633.84f;

// The live side of the room is everything south of the central gate.
static bool InLiveSide40(WorldObject const* who)
{
    return who->GetPositionY() < POS_Y_GATE_40;
}

struct boss_gothik_40 : public VanillaRaidsBossAI
{
    boss_gothik_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_GOTHIK) { }

    bool IsInRoom()
    {
        if (me->GetPositionX() > 2767.0f || me->GetPositionX() < 2618.0f
            || me->GetPositionY() > -3285.0f || me->GetPositionY() < -3435.0f)
        {
            EnterEvadeMode(EVADE_REASON_BOUNDARY);
            return false;
        }

        return true;
    }

    void Reset() override
    {
        BossAI::Reset();
        events.Reset();
        summons.DespawnAll();
        me->RemoveUnitFlag(UNIT_FLAG_REMOVE_CLIENT_CONTROL);
        me->SetImmuneToPC(false);
        me->SetReactState(REACT_PASSIVE);
        _secondPhase = false;
        _gateOpened = false;
        _waveCount = 0;
        me->NearTeleportTo(2642.139f, -3386.959f, 285.492f, 6.265f);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        DoZoneInCombat();
        Talk(SAY_INTRO_1);
        events.ScheduleEvent(EVENT_INTRO_2, 4s);
        events.ScheduleEvent(EVENT_INTRO_3, 9s);
        events.ScheduleEvent(EVENT_INTRO_4, 14s);
        // Same bit AzerothCore calls UNIT_FLAG_DISABLE_MOVE - he stays on his platform.
        me->SetUnitFlag(UNIT_FLAG_REMOVE_CLIENT_CONTROL);
        events.ScheduleEvent(EVENT_SUMMON_ADDS, 30s);
        events.ScheduleEvent(EVENT_CHECK_PLAYERS, 2min);
    }

    void JustSummoned(Creature* summon) override
    {
        summons.Summon(summon);

        // With the gate open an add takes whoever is nearest; before that it may
        // only fight the half of the room it was raised on.
        if (_gateOpened)
        {
            if (Unit* target = SelectTarget(SelectTargetMethod::MinDistance, 0, 200.0f))
            {
                summon->AI()->AttackStart(target);
                summon->AI()->DoZoneInCombat();
                summon->SetReactState(REACT_AGGRESSIVE);
                summon->CallForHelp(150.0f);
            }

            return;
        }

        std::vector<Player*> candidates;
        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        for (auto const& itr : players)
        {
            Player* player = itr.GetSource();
            if (!player || !me->IsWithinDistInMap(player, 200.0f, true, false) || !player->IsAlive() || player->IsGameMaster())
                continue;

            if (InLiveSide40(player) != InLiveSide40(summon))
                continue;

            candidates.push_back(player);
        }

        if (!candidates.empty())
        {
            Player* target = candidates[urand(0, uint32(candidates.size()) - 1)];
            summon->AI()->AttackStart(target);
            summon->AI()->DoZoneInCombat();
            summon->SetReactState(REACT_AGGRESSIVE);
        }
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        summons.Despawn(summon);
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        Talk(SAY_KILL);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        Talk(SAY_DEATH);
        summons.DespawnAll();
    }

    void SummonWave(uint32 entry)
    {
        switch (entry)
        {
            case NPC_LIVING_TRAINEE_40:
                me->SummonCreature(NPC_LIVING_TRAINEE_40, PosSummonLiving40[0]);
                me->SummonCreature(NPC_LIVING_TRAINEE_40, PosSummonLiving40[1]);
                break;
            case NPC_LIVING_KNIGHT_40:
                me->SummonCreature(NPC_LIVING_KNIGHT_40, PosSummonLiving40[3]);
                break;
            case NPC_LIVING_RIDER_40:
                me->SummonCreature(NPC_LIVING_RIDER_40, PosSummonLiving40[4]);
                break;
            default:
                break;
        }
    }

    // True while there are living players on both sides of the gate.
    bool CheckGroupSplitted()
    {
        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        if (players.isEmpty())
            return false;

        bool liveSideManned = false;
        bool deadSideManned = false;

        for (auto const& itr : players)
        {
            Player* player = itr.GetSource();
            if (!player || !player->IsAlive())
                continue;

            if (player->GetPositionX() > POS_X_NORTH_40 || player->GetPositionX() < POS_X_SOUTH_40)
                continue;

            if (player->GetPositionY() <= POS_Y_GATE_40 && player->GetPositionY() >= POS_Y_EAST_40)
                liveSideManned = true;
            else if (player->GetPositionY() >= POS_Y_GATE_40 && player->GetPositionY() <= POS_Y_WEST_40)
                deadSideManned = true;

            if (liveSideManned && deadSideManned)
                return true;
        }

        return false;
    }

    void OpenInnerGate()
    {
        if (GameObject* gate = instance->GetGameObject(DATA_GOTHIK_GATE))
            gate->SetGoState(GO_STATE_ACTIVE);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        // He is untouchable until the waves are done.
        if (!_secondPhase)
            damage = 0;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!IsInRoom())
            return;

        if (!UpdateVictim())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_INTRO_2:
                Talk(SAY_INTRO_2);
                break;
            case EVENT_INTRO_3:
                Talk(SAY_INTRO_3);
                break;
            case EVENT_INTRO_4:
                Talk(SAY_INTRO_4);
                break;
            case EVENT_SHADOW_BOLT:
                DoCastVictim(SPELL_SHADOW_BOLT);
                events.Repeat(1s);
                break;
            case EVENT_HARVEST_SOUL:
                DoCastSelf(SPELL_HARVEST_SOUL);
                events.Repeat(15s);
                break;
            case EVENT_TELEPORT:
            {
                me->AttackStop();
                DoCastSelf(InLiveSide40(me) ? SPELL_TELEPORT_DEAD : SPELL_TELEPORT_LIVE);

                // Anyone left on the far side of the gate drops off his threat list.
                bool const nowLiveSide = InLiveSide40(me);
                std::vector<Unit*> wrongSide;
                for (ThreatReference const* ref : me->GetThreatManager().GetUnsortedThreatList())
                    if (Unit* victim = ref->GetVictim())
                        if (InLiveSide40(victim) != nowLiveSide)
                            wrongSide.push_back(victim);

                for (Unit* victim : wrongSide)
                    me->GetThreatManager().ClearThreat(victim);

                if (Unit* target = SelectTarget(SelectTargetMethod::MaxDistance, 0))
                {
                    me->GetThreatManager().AddThreat(target, 100.0f);
                    AttackStart(target);
                }

                events.Repeat(20s);
                break;
            }
            case EVENT_CHECK_HEALTH:
                if (me->HealthBelowPct(30))
                {
                    OpenInnerGate();
                    events.CancelEvent(EVENT_TELEPORT);
                    break;
                }
                events.Repeat(1s);
                break;
            case EVENT_SUMMON_ADDS:
                if (gothikWaves40[_waveCount][0])
                {
                    SummonWave(gothikWaves40[_waveCount][0]);
                    events.Repeat(Milliseconds(gothikWaves40[_waveCount][1]));
                }
                else
                {
                    _secondPhase = true;
                    Talk(SAY_PHASE_TWO);
                    Talk(EMOTE_PHASE_TWO);
                    DoCastSelf(SPELL_TELEPORT_LIVE);
                    me->SetReactState(REACT_AGGRESSIVE);
                    me->RemoveUnitFlag(UNIT_FLAG_REMOVE_CLIENT_CONTROL);
                    me->SetImmuneToPC(false);
                    me->RemoveAllAuras();
                    events.ScheduleEvent(EVENT_SHADOW_BOLT, 1s);
                    events.ScheduleEvent(EVENT_HARVEST_SOUL, 5s, 15s);
                    events.ScheduleEvent(EVENT_TELEPORT, 20s);
                    events.ScheduleEvent(EVENT_CHECK_HEALTH, 1s);
                }
                ++_waveCount;
                break;
            case EVENT_CHECK_PLAYERS:
                // Once the raid stops holding both sides the gate comes up and
                // everything still standing pours through.
                if (!CheckGroupSplitted())
                {
                    OpenInnerGate();
                    _gateOpened = true;

                    for (ObjectGuid const& guid : summons)
                    {
                        Creature* minion = ObjectAccessor::GetCreature(*me, guid);
                        if (!minion || !minion->IsAlive())
                            continue;

                        if (Unit* target = SelectTarget(SelectTargetMethod::MinDistance, 0, 200.0f))
                        {
                            minion->AI()->AttackStart(target);
                            minion->SetReactState(REACT_AGGRESSIVE);
                            minion->AI()->DoZoneInCombat();
                        }
                    }

                    Talk(EMOTE_GATE_OPENED);
                }
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    bool _secondPhase = false;
    bool _gateOpened = false;
    uint8 _waveCount = 0;
};

struct npc_boss_gothik_minion_40 : public ScriptedAI
{
    npc_boss_gothik_minion_40(Creature* creature) : ScriptedAI(creature)
    {
        _livingSide = InLiveSide40(me);
    }

    bool IsOnSameSide(Unit const* who) const { return _livingSide == InLiveSide40(who); }

    void Reset() override
    {
        me->SetReactState(REACT_AGGRESSIVE);
        me->SetNoCallAssistance(false);
        _events.Reset();
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        switch (me->GetEntry())
        {
            case NPC_LIVING_TRAINEE_40:
                _events.ScheduleEvent(EVENT_DEATH_PLAGUE, 3s);
                break;
            case NPC_DEAD_TRAINEE_40:
                _events.ScheduleEvent(EVENT_ARCANE_EXPLOSION, 2500ms);
                break;
            case NPC_LIVING_KNIGHT_40:
                _events.ScheduleEvent(EVENT_SHADOW_MARK, 3s);
                break;
            case NPC_DEAD_KNIGHT_40:
                _events.ScheduleEvent(EVENT_WHIRLWIND, 2s);
                break;
            case NPC_LIVING_RIDER_40:
                _events.ScheduleEvent(EVENT_SHADOW_BOLT_VOLLEY, 3s);
                break;
            case NPC_DEAD_RIDER_40:
                _events.ScheduleEvent(EVENT_DRAIN_LIFE, 2000ms, 3500ms);
                _events.ScheduleEvent(EVENT_UNHOLY_FRENZY, 5s, 9s);
                break;
            case NPC_DEAD_HORSE_40:
                _events.ScheduleEvent(EVENT_STOMP, 2s, 5s);
                break;
            default:
                break;
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        // A living add that dies anchors its undead twin on the far side.
        switch (me->GetEntry())
        {
            case NPC_LIVING_TRAINEE_40:
                DoCastAOE(SPELL_ANCHOR_1_TRAINEE, CastSpellExtraArgs(true));
                break;
            case NPC_LIVING_KNIGHT_40:
                DoCastAOE(SPELL_ANCHOR_1_DK, CastSpellExtraArgs(true));
                break;
            case NPC_LIVING_RIDER_40:
                DoCastAOE(SPELL_ANCHOR_1_RIDER, CastSpellExtraArgs(true));
                break;
            default:
                break;
        }
    }

    void UpdateAI(uint32 diff) override
    {
        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (_events.ExecuteEvent())
        {
            case EVENT_DEATH_PLAGUE:
                DoCastVictim(SPELL_DEATH_PLAGUE);
                _events.Repeat(4s, 7s);
                break;
            case EVENT_ARCANE_EXPLOSION:
                if (Unit* victim = me->GetVictim())
                    if (victim->IsWithinDist(me, 20.0f))
                        me->CastSpell(victim, SPELL_ARCANE_EXPLOSION, CastSpellExtraArgs(TRIGGERED_NONE));
                _events.Repeat(2500ms);
                break;
            case EVENT_SHADOW_MARK:
                if (Unit* victim = me->GetVictim())
                    if (victim->IsWithinDist(me, 10.0f))
                        me->CastSpell(victim, SPELL_SHADOW_MARK, CastSpellExtraArgs(TRIGGERED_NONE));
                _events.Repeat(5s, 7s);
                break;
            case EVENT_WHIRLWIND:
                if (Unit* victim = me->GetVictim())
                    if (victim->IsWithinDist(me, 10.0f))
                        me->CastSpell(victim, SPELL_WHIRLWIND, CastSpellExtraArgs(TRIGGERED_NONE));
                _events.Repeat(4s, 6s);
                break;
            case EVENT_SHADOW_BOLT_VOLLEY:
                DoCastVictim(SPELL_SHADOW_BOLT_VOLLEY);
                _events.Repeat(5s);
                break;
            case EVENT_DRAIN_LIFE:
                if (Unit* victim = me->GetVictim())
                    if (victim->IsWithinDist(me, 20.0f))
                        me->CastSpell(victim, SPELL_DRAIN_LIFE, CastSpellExtraArgs(TRIGGERED_NONE));
                _events.Repeat(8s, 12s);
                break;
            case EVENT_UNHOLY_FRENZY:
                me->AddAura(SPELL_UNHOLY_FRENZY, me);
                _events.Repeat(15s, 17s);
                break;
            case EVENT_STOMP:
                if (Unit* victim = me->GetVictim())
                    if (victim->IsWithinDist(me, 10.0f))
                        me->CastSpell(victim, SPELL_STOMP, CastSpellExtraArgs(TRIGGERED_NONE));
                _events.Repeat(4s, 9s);
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    EventMap _events;
    bool _livingSide = false;
};

// The skull piles on the dead side: they relay the anchor chain and raise the
// undead copy of whatever just died across the gate.
struct npc_gothik_trigger_40 : public ScriptedAI
{
    npc_gothik_trigger_40(Creature* creature) : ScriptedAI(creature)
    {
        creature->SetDisableGravity(true);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override { }
    void UpdateAI(uint32 /*diff*/) override { }
    void JustEngagedWith(Unit* /*who*/) override { }
    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override { damage = 0; }

    Creature* SelectRandomSkullPile()
    {
        std::list<Creature*> triggers;
        me->GetCreatureListWithEntryInGrid(triggers, NPC_TRIGGER_40, 150.0f);

        // Only the piles on the dead side, and not the soul triggers up on the platform.
        triggers.remove_if([](Creature* trigger)
        {
            return trigger->GetPositionY() < POS_Y_GATE_40 || trigger->GetPositionZ() > 280.0f;
        });

        if (triggers.empty())
            return nullptr;

        auto itr = triggers.begin();
        std::advance(itr, urand(0, uint32(triggers.size()) - 1));
        return *itr;
    }

    void SpellHit(WorldObject* /*caster*/, SpellInfo const* spellInfo) override
    {
        if (!spellInfo)
            return;

        switch (spellInfo->Id)
        {
            case SPELL_ANCHOR_1_TRAINEE:
                DoCastAOE(SPELL_ANCHOR_2_TRAINEE, CastSpellExtraArgs(true));
                break;
            case SPELL_ANCHOR_1_DK:
                DoCastAOE(SPELL_ANCHOR_2_DK, CastSpellExtraArgs(true));
                break;
            case SPELL_ANCHOR_1_RIDER:
                DoCastAOE(SPELL_ANCHOR_2_RIDER, CastSpellExtraArgs(true));
                break;
            case SPELL_ANCHOR_2_TRAINEE:
                if (Creature* target = SelectRandomSkullPile())
                    DoCast(target, SPELL_SKULLS_TRAINEE, CastSpellExtraArgs(true));
                break;
            case SPELL_ANCHOR_2_DK:
                if (Creature* target = SelectRandomSkullPile())
                    DoCast(target, SPELL_SKULLS_DK, CastSpellExtraArgs(true));
                break;
            case SPELL_ANCHOR_2_RIDER:
                if (Creature* target = SelectRandomSkullPile())
                    DoCast(target, SPELL_SKULLS_RIDER, CastSpellExtraArgs(true));
                break;
            case SPELL_SKULLS_TRAINEE:
                DoSummon(NPC_DEAD_TRAINEE_40, me, 0.0f, 15s, TEMPSUMMON_CORPSE_TIMED_DESPAWN);
                break;
            case SPELL_SKULLS_DK:
                DoSummon(NPC_DEAD_KNIGHT_40, me, 0.0f, 15s, TEMPSUMMON_CORPSE_TIMED_DESPAWN);
                break;
            case SPELL_SKULLS_RIDER:
                DoSummon(NPC_DEAD_RIDER_40, me, 0.0f, 15s, TEMPSUMMON_CORPSE_TIMED_DESPAWN);
                DoSummon(NPC_DEAD_HORSE_40, me, 0.0f, 15s, TEMPSUMMON_CORPSE_TIMED_DESPAWN);
                break;
            default:
                break;
        }
    }

    // The dead-side adds belong to Gothik, not to the pile that raised them.
    void JustSummoned(Creature* summon) override
    {
        if (InstanceScript* instance = me->GetInstanceScript())
            if (Creature* gothik = instance->GetCreature(DATA_GOTHIK))
                gothik->AI()->JustSummoned(summon);
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        if (InstanceScript* instance = me->GetInstanceScript())
            if (Creature* gothik = instance->GetCreature(DATA_GOTHIK))
                gothik->AI()->SummonedCreatureDespawn(summon);
    }
};

void AddSC_boss_gothik_40()
{
    RegisterNaxxramasCreatureAI(boss_gothik_40);
    RegisterNaxxramasCreatureAI(npc_boss_gothik_minion_40);
    RegisterCreatureAI(npc_gothik_trigger_40);
}
