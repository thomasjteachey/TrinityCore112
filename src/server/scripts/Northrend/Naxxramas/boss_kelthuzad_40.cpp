/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Kel'Thuzad. Ported from mod-individual-progression
 * (ZhengPeiRu21, AzerothCore, AGPL-3.0); Naxxramas 40 scripts by Sogladev.
 *
 * Frost Blast and Detonate Mana stay on TrinityCore's own spell scripts - the
 * spell ids are the same in every version.
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
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <cmath>
#include <iterator>
#include <list>
#include <vector>

enum KelThuzadYells40
{
    SAY_ANSWER_REQUEST                      = 3,
    SAY_AGGRO                               = 7,
    SAY_SLAY                                = 8,
    SAY_DEATH                               = 9,
    SAY_FROST_BLAST                         = 11,
    SAY_REQUEST_AID                         = 12,
    EMOTE_PHASE_TWO                         = 13,
    SAY_SUMMON_MINIONS                      = 14,
    SAY_SPECIAL                             = 15,

    EMOTE_GUARDIAN_FLEE                     = 0,
    EMOTE_GUARDIAN_APPEAR                   = 1
};

enum KelThuzadSpells40
{
    SPELL_FROST_BOLT_SINGLE                 = 28478,
    SPELL_FROST_BOLT_MULTI                  = 28479,
    SPELL_SHADOW_FISSURE                    = 27810,
    SPELL_DETONATE_MANA                     = 27819,
    SPELL_FROST_BLAST                       = 27808,
    SPELL_BERSERK                           = 28498,
    SPELL_KELTHUZAD_CHANNEL                 = 29423,

    // Minions
    SPELL_FRENZY                            = 28468,
    SPELL_MORTAL_WOUND                      = 28467,
    SPELL_BLOOD_TAP                         = 28470
};

enum KelThuzadActions40
{
    ACTION_CALL_HELP_ON                     = 1,
    ACTION_CALL_HELP_OFF                    = 2,
    ACTION_SECOND_PHASE                     = 3,
    ACTION_GUARDIANS_OFF                    = 4
};

enum KelThuzadEvents40
{
    EVENT_SUMMON_SOLDIER                    = 1,
    EVENT_SUMMON_UNSTOPPABLE_ABOMINATION    = 2,
    EVENT_SUMMON_SOUL_WEAVER                = 3,
    EVENT_PHASE_2                           = 4,
    EVENT_FROST_BOLT_SINGLE                 = 5,
    EVENT_FROST_BOLT_MULTI                  = 6,
    EVENT_DETONATE_MANA                     = 7,
    EVENT_PHASE_3                           = 8,
    EVENT_P3_LICH_KING_SAY                  = 9,
    EVENT_SHADOW_FISSURE                    = 10,
    EVENT_FROST_BLAST                       = 11,
    EVENT_SUMMON_GUARDIAN_OF_ICECROWN       = 13,
    EVENT_FLOOR_CHANGE                      = 14,
    EVENT_ENRAGE                            = 15,
    EVENT_SPAWN_POOL                        = 16,

    // Minions
    EVENT_MINION_FRENZY                     = 17,
    EVENT_MINION_MORTAL_WOUND               = 18,
    EVENT_MINION_BLOOD_TAP                  = 19
};

Position const SummonGroups40[12] =
{
    // Portals
    { 3783.272705f, -5062.697266f, 143.711203f, 3.617599f }, // LEFT_FAR
    { 3730.291260f, -5027.239258f, 143.956909f, 4.461900f }, // LEFT_MIDDLE
    { 3683.868652f, -5057.281250f, 143.183884f, 5.237086f }, // LEFT_NEAR
    { 3759.355225f, -5174.128418f, 143.802383f, 2.170104f }, // RIGHT_FAR
    { 3700.724365f, -5185.123047f, 143.928024f, 1.309310f }, // RIGHT_MIDDLE
    { 3665.121094f, -5138.679199f, 143.183212f, 0.604023f }, // RIGHT_NEAR

    // Room centres
    { 3769.34f, -5071.80f, 143.2082f, 3.658f },
    { 3729.78f, -5043.56f, 143.3867f, 4.475f },
    { 3682.75f, -5055.26f, 143.1848f, 5.295f },
    { 3752.58f, -5161.82f, 143.2944f, 2.126f },
    { 3702.83f, -5171.70f, 143.4356f, 1.305f },
    { 3665.30f, -5141.55f, 143.1846f, 0.566f }
};

Position const SpawnPool40[7] =
{
    { 3783.272705f, -5062.697266f, 143.711203f, 3.617599f }, // LEFT_FAR
    { 3730.291260f, -5027.239258f, 143.956909f, 4.461900f }, // LEFT_MIDDLE
    { 3683.868652f, -5057.281250f, 143.183884f, 5.237086f }, // LEFT_NEAR
    { 3759.355225f, -5174.128418f, 143.802383f, 2.170104f }, // RIGHT_FAR
    { 3700.724365f, -5185.123047f, 143.928024f, 1.309310f }, // RIGHT_MIDDLE
    { 3665.121094f, -5138.679199f, 143.183212f, 0.604023f }, // RIGHT_NEAR
    { 3651.729980f, -5092.620117f, 143.380005f, 6.050000f }  // GATE
};

struct boss_kelthuzad_40 : public VanillaRaidsBossAI
{
    boss_kelthuzad_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_KELTHUZAD) { }

    // The waves pick their targets from everyone in the room, not from a threat
    // list Kel'Thuzad does not have while he is channelling.
    Unit* SelectPlayerInRoom(float range) const
    {
        std::vector<Player*> candidates;
        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        for (auto const& itr : players)
        {
            Player* player = itr.GetSource();
            if (player && player->IsAlive() && !player->IsGameMaster() && me->IsWithinDistInMap(player, range))
                candidates.push_back(player);
        }

        if (candidates.empty())
            return nullptr;

        return candidates[urand(0, uint32(candidates.size()) - 1)];
    }

    // The standing army in the six side rooms, laid out the way it is in 1.12.
    void SpawnStandingArmy()
    {
        // At the gate
        me->SummonCreature(NPC_UNSTOPPABLE_ABOMINATION_40, 3656.19f, -5093.78f, 143.33f, 6.08f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_UNSTOPPABLE_ABOMINATION_40, 3657.94f, -5087.68f, 143.60f, 6.08f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_UNSTOPPABLE_ABOMINATION_40, 3655.48f, -5100.05f, 143.53f, 6.08f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_SOUL_WEAVER_40, 3651.73f, -5092.62f, 143.38f, 6.05f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40, 3660.17f, -5092.45f, 143.37f, 6.07f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40, 3659.39f, -5096.21f, 143.29f, 6.07f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40, 3659.29f, -5090.19f, 143.48f, 6.07f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40, 3657.43f, -5098.03f, 143.41f, 6.07f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40, 3654.36f, -5090.51f, 143.48f, 6.09f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);
        me->SummonCreature(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40, 3653.35f, -5095.91f, 143.41f, 6.09f, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 2s);

        // Six rooms, each with eight soldiers, three abominations and a weaver.
        for (uint8 i = 6; i < 12; ++i)
        {
            for (uint8 j = 0; j < 8; ++j)
            {
                float const angle = float(M_PI) * 2.0f / 8.0f * j;
                me->SummonCreature(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40,
                    SummonGroups40[i].GetPositionX() + 6.0f * std::cos(angle),
                    SummonGroups40[i].GetPositionY() + 6.0f * std::sin(angle),
                    SummonGroups40[i].GetPositionZ(),
                    SummonGroups40[i].GetOrientation(), TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20s);
            }
        }

        for (uint8 i = 6; i < 12; ++i)
        {
            for (uint8 j = 1; j < 4; ++j)
            {
                float const dist = j == 2 ? 0.0f : 8.0f; // the second one stands in the middle
                float const angle = SummonGroups40[i].GetOrientation() + float(M_PI) * 2.0f / 4.0f * j;
                me->SummonCreature(NPC_UNSTOPPABLE_ABOMINATION_40,
                    SummonGroups40[i].GetPositionX() + dist * std::cos(angle),
                    SummonGroups40[i].GetPositionY() + dist * std::sin(angle),
                    SummonGroups40[i].GetPositionZ(),
                    SummonGroups40[i].GetOrientation(), TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20s);
            }
        }

        for (uint8 i = 6; i < 12; ++i)
        {
            float const angle = SummonGroups40[i].GetOrientation() + float(M_PI);
            me->SummonCreature(NPC_SOUL_WEAVER_40,
                SummonGroups40[i].GetPositionX() + 6.0f * std::cos(angle),
                SummonGroups40[i].GetPositionY() + 6.0f * std::sin(angle),
                SummonGroups40[i].GetPositionZ() + 0.5f,
                SummonGroups40[i].GetOrientation(), TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20s);
        }
    }

    void SummonHelper(uint32 entry, uint32 count)
    {
        for (uint32 i = 0; i < count; ++i)
        {
            if (TempSummon* summon = me->SummonCreature(entry, SpawnPool40[urand(0, 6)], TEMPSUMMON_CORPSE_TIMED_DESPAWN, 20s))
            {
                if (Unit* target = SelectPlayerInRoom(100.0f))
                {
                    summon->AI()->DoAction(ACTION_CALL_HELP_OFF);
                    summon->AI()->AttackStart(target);
                }
            }
        }
    }

    void SetPortalsState(GOState state)
    {
        for (uint32 portal : { DATA_KELTHUZAD_PORTAL01, DATA_KELTHUZAD_PORTAL02, DATA_KELTHUZAD_PORTAL03, DATA_KELTHUZAD_PORTAL04 })
            if (GameObject* go = instance->GetGameObject(portal))
                go->SetGoState(state);
    }

    // Sends an action to every minion this encounter is holding.
    void MinionAction(int32 action)
    {
        for (ObjectGuid const& guid : summons)
            if (Creature* minion = ObjectAccessor::GetCreature(*me, guid))
                minion->AI()->DoAction(action);
    }

    void Reset() override
    {
        BossAI::Reset();
        events.Reset();
        summons.DespawnAll();
        me->RemoveUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_REMOVE_CLIENT_CONTROL));
        me->SetReactState(REACT_AGGRESSIVE);

        if (GameObject* go = instance->GetGameObject(DATA_KELTHUZAD_THRONE))
        {
            go->SetPhaseMask(1, true);
            go->SetGoState(GO_STATE_READY);
        }

        SetPortalsState(GO_STATE_READY);
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        me->RemoveUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_REMOVE_CLIENT_CONTROL));
        ScriptedAI::EnterEvadeMode(why);
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
        MinionAction(ACTION_GUARDIANS_OFF);

        for (ObjectGuid const& guid : summons)
        {
            if (Creature* guardian = ObjectAccessor::GetCreature(*me, guid))
            {
                if (guardian->GetEntry() == NPC_GUARDIAN_OF_ICECROWN_40)
                {
                    guardian->AI()->Talk(EMOTE_GUARDIAN_FLEE);
                    break;
                }
            }
        }

        Talk(SAY_DEATH);
    }

    void MoveInLineOfSight(Unit* who) override
    {
        if (!me->IsInCombat() && who->IsPlayer() && who->IsAlive() && me->GetDistance(who) <= 50.0f)
            AttackStart(who);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_SUMMON_MINIONS);
        me->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_REMOVE_CLIENT_CONTROL));
        me->RemoveAllAttackers();
        me->SetTarget(ObjectGuid::Empty);
        me->SetReactState(REACT_PASSIVE);
        DoCastSelf(SPELL_KELTHUZAD_CHANNEL);
        events.ScheduleEvent(EVENT_SPAWN_POOL, 5s);
        events.ScheduleEvent(EVENT_SUMMON_SOLDIER, 6400ms);
        events.ScheduleEvent(EVENT_SUMMON_UNSTOPPABLE_ABOMINATION, 10s);
        events.ScheduleEvent(EVENT_SUMMON_SOUL_WEAVER, 12s);
        events.ScheduleEvent(EVENT_PHASE_2, 228s);
        events.ScheduleEvent(EVENT_ENRAGE, 15min);

        if (GameObject* go = instance->GetGameObject(DATA_KELTHUZAD_THRONE))
        {
            events.ScheduleEvent(EVENT_FLOOR_CHANGE, 15s);
            go->SetGoState(GO_STATE_ACTIVE);
        }
    }

    void JustSummoned(Creature* summon) override
    {
        summons.Summon(summon);

        if (!summon->IsInCombat())
            summon->GetMotionMaster()->MoveRandom(5.0f);

        if (summon->GetEntry() == NPC_GUARDIAN_OF_ICECROWN_40)
            summon->SetHomePosition(summon->GetPositionX(), summon->GetPositionY(), summon->GetPositionZ(), summon->GetOrientation());
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        // The phase 1 channel does not count as an interrupting cast.
        if (!me->HasAura(SPELL_KELTHUZAD_CHANNEL) && me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_FLOOR_CHANGE:
                if (GameObject* go = instance->GetGameObject(DATA_KELTHUZAD_THRONE))
                {
                    events.ScheduleEvent(EVENT_FLOOR_CHANGE, 15s);
                    go->SetGoState(GO_STATE_READY);
                    go->SetPhaseMask(2, true);
                }
                break;
            case EVENT_SPAWN_POOL:
                SpawnStandingArmy();
                break;
            case EVENT_SUMMON_SOLDIER:
                SummonHelper(NPC_SOLDIER_OF_THE_FROZEN_WASTES_40, 1);
                events.Repeat(3100ms);
                break;
            case EVENT_SUMMON_UNSTOPPABLE_ABOMINATION:
                SummonHelper(NPC_UNSTOPPABLE_ABOMINATION_40, 1);
                events.Repeat(18s + 500ms);
                break;
            case EVENT_SUMMON_SOUL_WEAVER:
                SummonHelper(NPC_SOUL_WEAVER_40, 1);
                events.Repeat(30s);
                break;
            case EVENT_PHASE_2:
                Talk(EMOTE_PHASE_TWO);
                Talk(SAY_AGGRO);
                events.Reset();
                MinionAction(ACTION_SECOND_PHASE);
                me->RemoveUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_REMOVE_CLIENT_CONTROL));
                me->GetMotionMaster()->MoveChase(me->GetVictim());
                me->RemoveAura(SPELL_KELTHUZAD_CHANNEL);
                me->SetReactState(REACT_AGGRESSIVE);
                events.ScheduleEvent(EVENT_FROST_BOLT_SINGLE, 2s, 10s);
                events.ScheduleEvent(EVENT_FROST_BOLT_MULTI, 15s, 30s);
                events.ScheduleEvent(EVENT_DETONATE_MANA, 30s);
                events.ScheduleEvent(EVENT_PHASE_3, 1s);
                events.ScheduleEvent(EVENT_SHADOW_FISSURE, 25s);
                events.ScheduleEvent(EVENT_FROST_BLAST, 45s);
                break;
            case EVENT_ENRAGE:
                DoCastSelf(SPELL_BERSERK, CastSpellExtraArgs(true));
                break;
            case EVENT_FROST_BOLT_SINGLE:
                DoCastVictim(SPELL_FROST_BOLT_SINGLE);
                events.Repeat(2s, 10s);
                break;
            case EVENT_FROST_BOLT_MULTI:
                DoCastSelf(SPELL_FROST_BOLT_MULTI);
                events.Repeat(15s, 30s);
                break;
            case EVENT_SHADOW_FISSURE:
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 100.0f, true))
                    me->CastSpell(target, SPELL_SHADOW_FISSURE, CastSpellExtraArgs(TRIGGERED_NONE));
                events.Repeat(25s);
                break;
            case EVENT_FROST_BLAST:
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, RAID_MODE<uint32>(1, 0, 0, 0), 0.0f, true))
                {
                    me->CastSpell(target, SPELL_FROST_BLAST, CastSpellExtraArgs(TRIGGERED_NONE));
                    Talk(SAY_FROST_BLAST);
                }
                events.Repeat(45s);
                break;
            case EVENT_DETONATE_MANA:
            {
                std::vector<Unit*> manaUsers;
                for (ThreatReference const* ref : me->GetThreatManager().GetUnsortedThreatList())
                {
                    Unit* target = ref->GetVictim();
                    if (target && target->IsPlayer() && target->GetPowerType() == POWER_MANA && target->GetPower(POWER_MANA))
                        manaUsers.push_back(target);
                }

                if (!manaUsers.empty())
                {
                    auto itr = manaUsers.begin();
                    std::advance(itr, urand(0, uint32(manaUsers.size()) - 1));
                    me->CastSpell(*itr, SPELL_DETONATE_MANA, CastSpellExtraArgs(TRIGGERED_NONE));
                    Talk(SAY_SPECIAL);
                }

                events.Repeat(30s);
                break;
            }
            case EVENT_PHASE_3:
                if (me->HealthBelowPct(45))
                {
                    Talk(SAY_REQUEST_AID);
                    events.DelayEvents(5500ms);
                    events.ScheduleEvent(EVENT_P3_LICH_KING_SAY, 5s);
                    SetPortalsState(GO_STATE_ACTIVE);
                    break;
                }
                events.Repeat(1s);
                break;
            case EVENT_P3_LICH_KING_SAY:
            {
                if (Creature* lichKing = instance->GetCreature(DATA_LICH_KING))
                    lichKing->AI()->Talk(SAY_ANSWER_REQUEST);

                uint32 const guardians = RAID_MODE<uint32>(2, 4, 4, 4);
                for (uint32 i = 0; i < guardians; ++i)
                    events.ScheduleEvent(EVENT_SUMMON_GUARDIAN_OF_ICECROWN, Milliseconds(10000 + (i * 5000)));

                break;
            }
            case EVENT_SUMMON_GUARDIAN_OF_ICECROWN:
                if (TempSummon* guardian = me->SummonCreature(NPC_GUARDIAN_OF_ICECROWN_40, SpawnPool40[RAND(0, 1, 3, 4)]))
                {
                    guardian->AI()->Talk(EMOTE_GUARDIAN_APPEAR);
                    guardian->AI()->AttackStart(me->GetVictim());
                }
                break;
            default:
                break;
        }

        if (!me->HasUnitFlag(UNIT_FLAG_REMOVE_CLIENT_CONTROL))
            DoMeleeAttackIfReady();
    }
};

struct boss_kelthuzad_minion_40 : public ScriptedAI
{
    boss_kelthuzad_minion_40(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override
    {
        me->SetNoCallAssistance(true);
        _callHelp = true;
        _events.Reset();
    }

    void DoAction(int32 param) override
    {
        switch (param)
        {
            case ACTION_CALL_HELP_ON:
                _callHelp = true;
                break;
            case ACTION_CALL_HELP_OFF:
                _callHelp = false;
                break;
            case ACTION_SECOND_PHASE:
                // Anything still idle in the side rooms goes away when he comes down.
                if (!me->IsInCombat())
                    me->DespawnOrUnsummon(500ms);
                break;
            case ACTION_GUARDIANS_OFF:
                me->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE));
                me->RemoveAllAuras();
                EnterEvadeMode(EVADE_REASON_OTHER);
                me->NearTeleportTo(me->GetHomePosition());
                break;
            default:
                break;
        }
    }

    void MoveInLineOfSight(Unit* who) override
    {
        if (!who->IsPlayer() && !who->IsPet())
            return;

        ScriptedAI::MoveInLineOfSight(who);
    }

    void AttackStart(Unit* who) override
    {
        ScriptedAI::AttackStart(who);

        // The first one to notice drags its neighbours in with it.
        if (_callHelp)
        {
            std::list<Creature*> neighbours;
            me->GetCreatureListWithEntryInGrid(neighbours, me->GetEntry(), 15.0f);
            for (Creature* neighbour : neighbours)
            {
                if (neighbour->GetGUID() == me->GetGUID())
                    continue;

                neighbour->AI()->DoAction(ACTION_CALL_HELP_OFF);
                neighbour->AI()->AttackStart(who);
            }
        }

        if (me->GetEntry() != NPC_UNSTOPPABLE_ABOMINATION_40 && me->GetEntry() != NPC_GUARDIAN_OF_ICECROWN_40)
            me->GetThreatManager().AddThreat(who, 1000000.0f);
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        DoZoneInCombat();

        if (me->GetEntry() == NPC_UNSTOPPABLE_ABOMINATION_40)
        {
            _events.ScheduleEvent(EVENT_MINION_FRENZY, 1s);
            _events.ScheduleEvent(EVENT_MINION_MORTAL_WOUND, 5s);
        }
        else if (me->GetEntry() == NPC_GUARDIAN_OF_ICECROWN_40)
            _events.ScheduleEvent(EVENT_MINION_BLOOD_TAP, 15s);
    }

    void JustReachedHome() override
    {
        if (me->GetEntry() == NPC_GUARDIAN_OF_ICECROWN_40)
            me->DespawnOrUnsummon();
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (_events.ExecuteEvent())
        {
            case EVENT_MINION_MORTAL_WOUND:
                DoCastVictim(SPELL_MORTAL_WOUND);
                _events.Repeat(15s);
                break;
            case EVENT_MINION_FRENZY:
                if (me->HealthBelowPct(35))
                {
                    DoCastSelf(SPELL_FRENZY, CastSpellExtraArgs(true));
                    break;
                }
                _events.Repeat(1s);
                break;
            case EVENT_MINION_BLOOD_TAP:
                DoCastVictim(SPELL_BLOOD_TAP);
                _events.Repeat(15s);
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    EventMap _events;
    bool _callHelp = true;
};

void AddSC_boss_kelthuzad_40()
{
    RegisterNaxxramasCreatureAI(boss_kelthuzad_40);
    RegisterNaxxramasCreatureAI(boss_kelthuzad_minion_40);
}
