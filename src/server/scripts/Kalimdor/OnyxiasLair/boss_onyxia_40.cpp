/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player, level 60) Onyxia. She lives on raid difficulty 2 of map
 * 249 as creature 301000, so the level 80 10- and 25-player lair on
 * difficulties 0 and 1 is untouched.
 *
 * Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0).
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
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "onyxias_lair.h"
#include <cmath>

enum Onyxia40Spells
{
    SPELL_WINGBUFFET                = 18500,
    SPELL_FLAMEBREATH               = 18435,
    SPELL_CLEAVE                    = 68868,
    SPELL_TAIL_SWEEP                = 15847,
    SPELL_FIREBALL                  = 18392,
    SPELL_BELLOWINGROAR             = 18431,

    // 92000, not the module's 91003: this realm's own Spell.dbc already defines
    // 91003, and TrinityCore aborts at startup when spell_dbc repeats an id the
    // binary DBC already has ("Index N already exists in dbc").
    SPELL_SUMMON_WHELP              = 92000,
    SPELL_SUMMON_LAIR_GUARD         = 68968,
    SPELL_ERUPTION                  = 17731,

    SPELL_BREATH_N_TO_S             = 17086,
    SPELL_BREATH_S_TO_N             = 18351,
    SPELL_BREATH_E_TO_W             = 18576,
    SPELL_BREATH_W_TO_E             = 18609,
    SPELL_BREATH_SE_TO_NW           = 18564,
    SPELL_BREATH_NW_TO_SE           = 18584,
    SPELL_BREATH_SW_TO_NE           = 18596,
    SPELL_BREATH_NE_TO_SW           = 18617
};

enum Onyxia40Events
{
    EVENT_SPELL_WINGBUFFET          = 1,
    EVENT_SPELL_FLAMEBREATH         = 2,
    EVENT_SPELL_TAILSWEEP           = 3,
    EVENT_SPELL_CLEAVE              = 4,
    EVENT_START_PHASE_2             = 5,
    EVENT_SPELL_FIREBALL_FIRST      = 6,
    EVENT_SPELL_FIREBALL_SECOND     = 7,
    EVENT_PHASE_2_STEP_CW           = 8,
    EVENT_PHASE_2_STEP_ACW          = 9,
    EVENT_PHASE_2_STEP_ACROSS       = 10,
    EVENT_SPELL_BREATH              = 11,
    EVENT_START_PHASE_3             = 12,
    EVENT_PHASE_3_ATTACK            = 13,
    EVENT_SPELL_BELLOWINGROAR       = 14,
    EVENT_WHELP_SPAM                = 15,
    EVENT_SUMMON_LAIR_GUARD         = 16,
    EVENT_SUMMON_WHELP              = 17,
    EVENT_ERUPTION                  = 20,

    EVENT_LIFTOFF                   = 31,
    EVENT_FLY_S_TO_N                = 32,
    EVENT_LAND                      = 33,
    EVENT_END_MANY_WHELPS_TIME      = 34
};

enum Onyxia40Phases
{
    PHASE_NONE_40                   = 0,
    PHASE_GROUNDED_40               = 1, // phase 1
    PHASE_AIRPHASE_40               = 2, // phase 2, from 65%
    PHASE_LANDED_40                 = 3  // phase 3, from 40%
};

enum Onyxia40Yells
{
    SAY_AGGRO                       = 0,
    SAY_KILL                        = 1,
    SAY_PHASE_2_TRANS               = 2,
    SAY_PHASE_3_TRANS               = 3,
    EMOTE_BREATH                    = 4,
    SAY_EVADE                       = 5
};

enum Onyxia40Misc
{
    NPC_ERUPTION_TRIGGER            = 12758
};

struct sOnyxMove40
{
    uint8 CurrId;
    uint8 DestId;
    uint32 spellId;
    float x, y, z, o;
};

static sOnyxMove40 const OnyxiaMoveData40[] =
{
    { 0, 0, 0,                     -64.496f, -214.906f, -84.4f, 0.0f },                       // south, on the ground
    { 1, 5, SPELL_BREATH_S_TO_N,   -64.496f, -214.906f, -60.0f, 0.0f },                       // south
    { 2, 6, SPELL_BREATH_SW_TO_NE, -59.809f, -190.758f, -60.0f, 7.0f * float(M_PI) / 4.0f },  // south-west
    { 3, 7, SPELL_BREATH_W_TO_E,   -29.450f, -180.600f, -60.0f, float(M_PI) + float(M_PI) / 2.0f }, // west
    { 4, 8, SPELL_BREATH_NW_TO_SE,   6.895f, -180.246f, -60.0f, float(M_PI) + float(M_PI) / 4.0f }, // north-west
    { 5, 1, SPELL_BREATH_N_TO_S,    22.876f, -217.152f, -60.0f, float(M_PI) },                // north
    { 6, 2, SPELL_BREATH_NE_TO_SW,  10.2191f, -247.912f, -60.0f, 3.0f * float(M_PI) / 4.0f }, // north-east
    { 7, 3, SPELL_BREATH_E_TO_W,   -31.496f, -250.123f, -60.0f, float(M_PI) / 2.0f },         // east
    { 8, 4, SPELL_BREATH_SE_TO_NW, -63.5156f, -240.096f, -60.0f, float(M_PI) / 4.0f }         // south-east
};

struct boss_onyxia_40 : public VanillaRaidsBossAI
{
    boss_onyxia_40(Creature* creature) : VanillaRaidsBossAI(creature, DATA_ONYXIA)
    {
        Initialize();
    }

    void Initialize()
    {
        _currentWP = 0;
        _whelpSpam = false;
        _whelpCount = 0;
        _whelpSpamTimer = 0;
        _manyWhelpsAvailable = false;

        // Immune to taunt, as in vanilla.
        me->ApplySpellImmune(0, IMMUNITY_STATE, SPELL_AURA_MOD_TAUNT, true);
        me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_ATTACK_ME, true);
    }

    void SetPhase(uint8 phase)
    {
        events.Reset();
        _phase = phase;

        switch (phase)
        {
            case PHASE_GROUNDED_40:
                events.ScheduleEvent(EVENT_SPELL_WINGBUFFET, 10s, 20s);
                events.ScheduleEvent(EVENT_SPELL_FLAMEBREATH, 10s, 20s);
                events.ScheduleEvent(EVENT_SPELL_TAILSWEEP, 15s, 20s);
                events.ScheduleEvent(EVENT_SPELL_CLEAVE, 2s, 5s);
                break;
            case PHASE_AIRPHASE_40:
                events.ScheduleEvent(EVENT_START_PHASE_2, 0ms);
                break;
            case PHASE_LANDED_40:
                events.ScheduleEvent(EVENT_START_PHASE_3, 5s);
                break;
            default:
                break;
        }
    }

    void Reset() override
    {
        Initialize();
        SetPhase(PHASE_NONE_40);
        me->SetReactState(REACT_AGGRESSIVE);
        me->SetCanFly(false);
        me->SetDisableGravity(false);
        me->SetSpeedRate(MOVE_RUN, me->GetCreatureTemplate()->speed_run);
        BossAI::Reset();
    }

    void KilledUnit(Unit* who) override
    {
        if (who->IsPlayer())
            Talk(SAY_KILL);
    }

    void JustEngagedWith(Unit* who) override
    {
        Talk(SAY_AGGRO);
        SetPhase(PHASE_GROUNDED_40);
        BossAI::JustEngagedWith(who);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        if (me->HealthBelowPctDamaged(65, damage) && _phase == PHASE_GROUNDED_40)
            SetPhase(PHASE_AIRPHASE_40);
        else if (me->HealthBelowPctDamaged(40, damage) && _phase == PHASE_AIRPHASE_40)
            SetPhase(PHASE_LANDED_40);
    }

    void JustSummoned(Creature* summon) override
    {
        if (summon->GetEntry() != NPC_ONYXIAN_WHELP_40 && summon->GetEntry() != NPC_ONYXIAN_LAIR_GUARD_40)
            return;

        if (Unit* target = summon->SelectNearestTarget(300.0f))
        {
            summon->AI()->AttackStart(target);
            DoZoneInCombat(summon);
        }

        summons.Summon(summon);
    }

    void MovementInform(uint32 type, uint32 id) override
    {
        if (type != POINT_MOTION_TYPE && type != EFFECT_MOTION_TYPE)
            return;

        if (id < 9)
        {
            if (id > 0 && _phase == PHASE_AIRPHASE_40)
            {
                me->SetFacingTo(OnyxiaMoveData40[id].o);
                me->SetSpeedRate(MOVE_RUN, 1.6f);
                _currentWP = int8(id);
                events.ScheduleEvent(EVENT_SPELL_FIREBALL_FIRST, 1s);
            }

            return;
        }

        switch (id)
        {
            case 10:
                me->SetFacingTo(OnyxiaMoveData40[0].o);
                events.ScheduleEvent(EVENT_LIFTOFF, 0ms);
                break;
            case 11:
                me->SetFacingTo(OnyxiaMoveData40[1].o);
                events.ScheduleEvent(EVENT_FLY_S_TO_N, 0ms);
                break;
            case 12:
                me->SetFacingTo(OnyxiaMoveData40[1].o);
                events.ScheduleEvent(EVENT_LAND, 0ms);
                break;
            case 13:
                me->SetCanFly(false);
                me->SetDisableGravity(false);
                me->SetSpeedRate(MOVE_RUN, me->GetCreatureTemplate()->speed_run);
                events.ScheduleEvent(EVENT_PHASE_3_ATTACK, 0ms);
                break;
            default:
                break;
        }
    }

    // Forty whelps out of the two cave mouths, six hundred milliseconds apart.
    void HandleWhelpSpam(uint32 diff)
    {
        if (!_whelpSpam)
            return;

        if (_whelpCount >= 40)
        {
            _whelpSpam = false;
            _whelpCount = 0;
            _whelpSpamTimer = 0;
            return;
        }

        _whelpSpamTimer -= int32(diff);
        if (_whelpSpamTimer > 0)
            return;

        static float const caveMouths[2][3] =
        {
            { -31.710f, -170.55f, -89.72f },
            { -32.086f, -258.55f, -89.72f }
        };

        uint8 const selectedCave = urand(0, 1);
        if (TempSummon* whelp = me->SummonCreature(NPC_ONYXIAN_WHELP_40,
                caveMouths[selectedCave][0], caveMouths[selectedCave][1], caveMouths[selectedCave][2], 0.0f))
        {
            if (Unit* target = whelp->SelectNearestTarget(300.0f))
                whelp->AI()->AttackStart(target);
        }

        ++_whelpCount;
        _whelpSpamTimer += 600;
    }

    bool CheckInRoom() override
    {
        if (me->GetDistance2d(me->GetHomePosition().GetPositionX(), me->GetHomePosition().GetPositionY()) > 95.0f)
        {
            Talk(SAY_EVADE);
            EnterEvadeMode(EVADE_REASON_BOUNDARY);
            return false;
        }

        return true;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim() || !CheckInRoom())
            return;

        events.Update(diff);
        HandleWhelpSpam(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        DoMeleeAttackIfReady();

        switch (events.ExecuteEvent())
        {
            case EVENT_SPELL_WINGBUFFET:
                DoCastSelf(SPELL_WINGBUFFET);
                events.Repeat(15s, 30s);
                break;
            case EVENT_SPELL_FLAMEBREATH:
                DoCastSelf(SPELL_FLAMEBREATH);
                events.Repeat(10s, 20s);
                break;
            case EVENT_SPELL_TAILSWEEP:
                DoCastSelf(SPELL_TAIL_SWEEP);
                events.Repeat(15s, 20s);
                break;
            case EVENT_SPELL_CLEAVE:
                DoCastVictim(SPELL_CLEAVE);
                events.Repeat(2s, 5s);
                break;
            case EVENT_START_PHASE_2:
                me->AttackStop();
                me->SetReactState(REACT_PASSIVE);
                me->StopMoving();
                ResetThreatList();
                me->GetMotionMaster()->MovePoint(10, OnyxiaMoveData40[0].x, OnyxiaMoveData40[0].y, OnyxiaMoveData40[0].z);
                break;
            case EVENT_LIFTOFF:
                Talk(SAY_PHASE_2_TRANS);
                me->SendMeleeAttackStop(me->GetVictim());
                me->GetMotionMaster()->MoveIdle();
                me->StopMoving(); // TrinityCore keeps DisableSpline protected; this clears the spline the same way
                me->SetCanFly(true);
                me->SetDisableGravity(true);
                me->SetOrientation(OnyxiaMoveData40[0].o);
                me->SendMovementFlagUpdate();
                me->GetMotionMaster()->MoveTakeoff(11,
                    Position(OnyxiaMoveData40[1].x + 1.0f, OnyxiaMoveData40[1].y, OnyxiaMoveData40[1].z), 12.0f);
                _manyWhelpsAvailable = true;
                events.RescheduleEvent(EVENT_END_MANY_WHELPS_TIME, 10s);
                break;
            case EVENT_END_MANY_WHELPS_TIME:
                _manyWhelpsAvailable = false;
                break;
            case EVENT_FLY_S_TO_N:
                me->SetSpeedRate(MOVE_RUN, 2.95f);
                me->GetMotionMaster()->MovePoint(5, OnyxiaMoveData40[5].x, OnyxiaMoveData40[5].y, OnyxiaMoveData40[5].z);
                _whelpSpam = true;
                events.ScheduleEvent(EVENT_WHELP_SPAM, 90s);
                break;
            case EVENT_SUMMON_LAIR_GUARD:
                me->CastSpell(Position(-101.654f, -214.491f, -80.70f), SPELL_SUMMON_LAIR_GUARD, CastSpellExtraArgs(true));
                events.Repeat(30s);
                break;
            case EVENT_WHELP_SPAM:
                _whelpSpam = true;
                events.Repeat(90s);
                break;
            case EVENT_LAND:
                Talk(SAY_PHASE_3_TRANS);
                me->SendMeleeAttackStop(me->GetVictim());
                me->GetMotionMaster()->MoveLand(13,
                    Position(OnyxiaMoveData40[0].x + 1.0f, OnyxiaMoveData40[0].y, OnyxiaMoveData40[0].z), 12.0f);
                ResetThreatList();
                break;
            case EVENT_SPELL_FIREBALL_FIRST:
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 200.0f, true))
                {
                    me->SetFacingToObject(target);
                    DoCast(target, SPELL_FIREBALL);
                }
                events.ScheduleEvent(EVENT_SPELL_FIREBALL_SECOND, 4s);
                break;
            case EVENT_SPELL_FIREBALL_SECOND:
            {
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 200.0f, true))
                {
                    me->SetFacingToObject(target);
                    DoCast(target, SPELL_FIREBALL);
                }

                uint8 const roll = urand(0, 99);
                if (roll < 33)
                    events.ScheduleEvent(EVENT_PHASE_2_STEP_CW, 4s);
                else if (roll < 66)
                    events.ScheduleEvent(EVENT_PHASE_2_STEP_ACW, 4s);
                else
                    events.ScheduleEvent(EVENT_PHASE_2_STEP_ACROSS, 4s);
                break;
            }
            case EVENT_PHASE_2_STEP_CW:
            {
                uint8 newWP = uint8(_currentWP + 1);
                if (newWP > 8)
                    newWP = 1;
                me->GetMotionMaster()->MovePoint(newWP, OnyxiaMoveData40[newWP].x, OnyxiaMoveData40[newWP].y, OnyxiaMoveData40[newWP].z);
                break;
            }
            case EVENT_PHASE_2_STEP_ACW:
            {
                int8 newWP = int8(_currentWP - 1);
                if (newWP < 1)
                    newWP = 8;
                me->GetMotionMaster()->MovePoint(uint32(newWP), OnyxiaMoveData40[newWP].x, OnyxiaMoveData40[newWP].y, OnyxiaMoveData40[newWP].z);
                break;
            }
            case EVENT_PHASE_2_STEP_ACROSS:
                Talk(EMOTE_BREATH);
                me->SetFacingTo(OnyxiaMoveData40[_currentWP].o);
                DoCastAOE(OnyxiaMoveData40[_currentWP].spellId);
                events.ScheduleEvent(EVENT_SPELL_BREATH, 8250ms);
                break;
            case EVENT_SPELL_BREATH:
            {
                uint8 const newWP = OnyxiaMoveData40[_currentWP].DestId;
                me->SetSpeedRate(MOVE_RUN, 2.95f);
                me->GetMotionMaster()->MovePoint(newWP, OnyxiaMoveData40[newWP].x, OnyxiaMoveData40[newWP].y, OnyxiaMoveData40[newWP].z);
                break;
            }
            case EVENT_START_PHASE_3:
                me->SetSpeedRate(MOVE_RUN, 2.95f);
                me->GetMotionMaster()->MovePoint(12, OnyxiaMoveData40[1].x, OnyxiaMoveData40[1].y, OnyxiaMoveData40[1].z);
                break;
            case EVENT_PHASE_3_ATTACK:
                me->SetReactState(REACT_AGGRESSIVE);

                if (Unit* target = SelectTarget(SelectTargetMethod::MaxThreat, 0, 0.0f, false))
                    AttackStart(target);

                DoCastAOE(SPELL_BELLOWINGROAR);
                events.ScheduleEvent(EVENT_ERUPTION, 0ms);
                events.ScheduleEvent(EVENT_SPELL_WINGBUFFET, 10s, 20s);
                events.ScheduleEvent(EVENT_SPELL_FLAMEBREATH, 10s, 20s);
                events.ScheduleEvent(EVENT_SPELL_TAILSWEEP, 15s, 20s);
                events.ScheduleEvent(EVENT_SPELL_CLEAVE, 2s, 5s);
                events.ScheduleEvent(EVENT_SPELL_BELLOWINGROAR, 15s);
                events.ScheduleEvent(EVENT_SUMMON_WHELP, 10s);
                break;
            case EVENT_SPELL_BELLOWINGROAR:
                DoCastAOE(SPELL_BELLOWINGROAR);
                events.Repeat(22s);
                events.ScheduleEvent(EVENT_ERUPTION, 0ms);
                break;
            case EVENT_ERUPTION:
                if (TempSummon* trigger = me->SummonCreature(NPC_ERUPTION_TRIGGER, *me, TEMPSUMMON_TIMED_DESPAWN, 1s))
                    trigger->CastSpell(trigger, SPELL_ERUPTION, CastSpellExtraArgs(TRIGGERED_NONE));
                break;
            case EVENT_SUMMON_WHELP:
            {
                float const angle = float(rand_norm()) * 2.0f * float(M_PI);
                float const dist = float(rand_norm()) * 4.0f;
                me->CastSpell(Position(-33.18f + std::cos(angle) * dist, -258.80f + std::sin(angle) * dist, -89.0f),
                    SPELL_SUMMON_WHELP, CastSpellExtraArgs(true));
                me->CastSpell(Position(-32.535f + std::cos(angle) * dist, -170.190f + std::sin(angle) * dist, -89.0f),
                    SPELL_SUMMON_WHELP, CastSpellExtraArgs(true));
                events.Repeat(30s);
                break;
            }
            default:
                break;
        }
    }

private:
    uint8 _phase = PHASE_NONE_40;
    int8 _currentWP = 0;

    bool _whelpSpam = false;
    uint8 _whelpCount = 0;
    int32 _whelpSpamTimer = 0;
    bool _manyWhelpsAvailable = false;
};

void AddSC_boss_onyxia_40()
{
    RegisterOnyxiasLairCreatureAI(boss_onyxia_40);
}
