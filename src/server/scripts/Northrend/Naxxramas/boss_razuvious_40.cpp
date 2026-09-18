/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Instructor Razuvious. Ported from mod-individual-progression
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
#include "Containers.h"
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "VanillaRaids/VanillaRaids.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"

enum RazuviousSays40
{
    SAY_AGGRO                       = 0,
    SAY_SLAY                        = 1,
    SAY_TAUNTED                     = 2,
    SAY_DEATH                       = 3,
    SAY_PATHETIC                    = 4,
    SAY_TARGET_DUMMY                = 5,

    SAY_DEATH_KNIGHT_UNDERSTUDY     = 0
};

enum RazuviousSpells40
{
    SPELL_UNBALANCING_STRIKE        = 26613,
    SPELL_DISRUPTING_SHOUT          = 55543,
    SPELL_MANA_BURN                 = 26046, // the vanilla alternative to Disrupting Shout
    SPELL_HOPELESS                  = 29125,
    SPELL_TAUNT                     = 29060
};

enum RazuviousEvents40
{
    EVENT_UNBALANCING_STRIKE        = 1,
    EVENT_DISRUPTING_SHOUT          = 2
};

enum RazuviousNPCs40
{
    NPC_TARGET_DUMMY                = 16211
};

enum RazuviousActions40
{
    ACTION_FACE_ME                  = 0,
    ACTION_TALK                     = 1,
    ACTION_EMOTE                    = 2,
    ACTION_SALUTE                   = 3,
    ACTION_BACK_TO_TRAINING         = 4
};

enum RazuviousMisc40
{
    GROUP_OOC_RP                    = 0,
    POINT_DEATH_KNIGHT              = 0
};

struct boss_razuvious_40 : public VanillaRaidsBossAI
{
    boss_razuvious_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_RAZUVIOUS) { }

    void SpawnHelpers()
    {
        me->SummonCreature(NPC_DEATH_KNIGHT_UNDERSTUDY_40, 2762.23f, -3085.07f, 267.685f, 1.95f);
        me->SummonCreature(NPC_DEATH_KNIGHT_UNDERSTUDY_40, 2758.24f, -3110.97f, 267.685f, 3.94f);
        me->SummonCreature(NPC_DEATH_KNIGHT_UNDERSTUDY_40, 2782.45f, -3088.03f, 267.685f, 0.75f);
        me->SummonCreature(NPC_DEATH_KNIGHT_UNDERSTUDY_40, 2778.56f, -3113.74f, 267.685f, 5.28f);
    }

    void JustSummoned(Creature* summon) override
    {
        summons.Summon(summon);
    }

    void Reset() override
    {
        BossAI::Reset();
        summons.DespawnAll();
        events.Reset();
        SpawnHelpers();
        ScheduleRP();
    }

    Creature* RPBuddy()
    {
        if (!_rpBuddyGUID)
            return nullptr;

        return ObjectAccessor::GetCreature(*me, _rpBuddyGUID);
    }

    void ScheduleInteractWithDeathKnight()
    {
        if (Creature* understudy = RPBuddy())
            me->SetFacingToObject(understudy);

        scheduler.Schedule(2s, GROUP_OOC_RP, [this](TaskContext /*context*/)
        {
            if (roll_chance_i(75))
            {
                bool const longText = roll_chance_i(50);
                Talk(longText ? SAY_TARGET_DUMMY : SAY_PATHETIC);
                scheduler.Schedule(4s, GROUP_OOC_RP, [this](TaskContext /*context*/)
                {
                    if (Creature* understudy = RPBuddy())
                        understudy->AI()->DoAction(ACTION_TALK);
                });

                if (longText)
                    scheduler.DelayGroup(GROUP_OOC_RP, 5s);
            }
            else
            {
                me->HandleEmoteCommand(EMOTE_ONESHOT_EXCLAMATION);
                scheduler.Schedule(4s, GROUP_OOC_RP, [this](TaskContext /*context*/)
                {
                    if (Creature* understudy = RPBuddy())
                        understudy->AI()->DoAction(roll_chance_i(25) ? ACTION_EMOTE : ACTION_TALK);
                });
            }
        }).Schedule(4s, GROUP_OOC_RP, [this](TaskContext /*context*/)
        {
            if (Creature* understudy = RPBuddy())
                understudy->AI()->DoAction(ACTION_FACE_ME);
        }).Schedule(10s, GROUP_OOC_RP, [this](TaskContext /*context*/)
        {
            if (Creature* understudy = RPBuddy())
                understudy->AI()->DoAction(ACTION_SALUTE);
        }).Schedule(13s, GROUP_OOC_RP, [this](TaskContext /*context*/)
        {
            me->ResumeMovement();
        }).Schedule(16s, GROUP_OOC_RP, [this](TaskContext /*context*/)
        {
            if (Creature* understudy = RPBuddy())
                understudy->AI()->DoAction(ACTION_BACK_TO_TRAINING);

            ScheduleRP();
        });
    }

    void MovementInform(uint32 type, uint32 id) override
    {
        if (type == POINT_MOTION_TYPE && id == POINT_DEATH_KNIGHT)
            ScheduleInteractWithDeathKnight();
    }

    void ScheduleRP()
    {
        if (summons.empty())
            return;

        _rpBuddyGUID = Trinity::Containers::SelectRandomContainerElement(summons);

        scheduler.Schedule(60s, 80s, GROUP_OOC_RP, [this](TaskContext context)
        {
            if (Creature* understudy = RPBuddy())
            {
                if (me->GetDistance2d(understudy) <= 6.0f)
                {
                    me->PauseMovement();
                    scheduler.Schedule(500ms, GROUP_OOC_RP, [this](TaskContext /*context*/)
                    {
                        if (Creature* buddy = RPBuddy())
                            me->GetMotionMaster()->MovePoint(POINT_DEATH_KNIGHT,
                                buddy->GetNearPosition(3.2f, buddy->GetRelativeAngle(me)));
                    });
                    return;
                }
            }

            context.Repeat(2s);
        });
    }

    void KilledUnit(Unit* /*who*/) override
    {
        if (roll_chance_i(30))
            Talk(SAY_SLAY);
    }

    void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        // Damage dealt by a mind-controlled understudy still has to count toward
        // the raid's share, or the kill does not credit as player-dealt.
        if (attacker && attacker->IsCreature() && attacker->GetEntry() == NPC_DEATH_KNIGHT_UNDERSTUDY_40)
            me->LowerPlayerDamageReq(damage);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        Talk(SAY_DEATH);
        DoCastSelf(SPELL_HOPELESS, CastSpellExtraArgs(true));
    }

    void SpellHit(WorldObject* caster, SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id == SPELL_TAUNT)
            Talk(SAY_TAUNTED, caster);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        scheduler.CancelGroup(GROUP_OOC_RP);
        Talk(SAY_AGGRO);
        events.ScheduleEvent(EVENT_UNBALANCING_STRIKE, 30s);
        events.ScheduleEvent(EVENT_DISRUPTING_SHOUT, 15s);
        summons.DoZoneInCombat();
    }

    void UpdateAI(uint32 diff) override
    {
        // The idle drilling only runs while he is out of combat.
        if (!me->IsInCombat())
            scheduler.Update(diff);

        if (!UpdateVictim())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_UNBALANCING_STRIKE:
                DoCastVictim(SPELL_UNBALANCING_STRIKE);
                events.Repeat(30s);
                break;
            case EVENT_DISRUPTING_SHOUT:
                DoCastSelf(VanillaRaids::NerfRazuvious() ? SPELL_DISRUPTING_SHOUT : SPELL_MANA_BURN);
                events.Repeat(25s);
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    ObjectGuid _rpBuddyGUID;
};

struct boss_razuvious_minion_40 : public ScriptedAI
{
    boss_razuvious_minion_40(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override
    {
        _scheduler.CancelAll();
        ScheduleAttackDummy();
    }

    void ScheduleAttackDummy()
    {
        me->SetEmoteState(EMOTE_STATE_READY1H);
        if (Creature* targetDummy = me->FindNearestCreature(NPC_TARGET_DUMMY, 10.0f))
            me->SetFacingToObject(targetDummy);

        _scheduler.Schedule(6s, 9s, GROUP_OOC_RP, [this](TaskContext context)
        {
            me->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK1H);
            context.Repeat(6s, 9s);
        });
    }

    void DoAction(int32 action) override
    {
        switch (action)
        {
            case ACTION_FACE_ME:
                _scheduler.CancelGroup(GROUP_OOC_RP);
                me->SetSheath(SHEATH_STATE_UNARMED);
                me->SetEmoteState(EMOTE_ONESHOT_NONE);
                if (InstanceScript* instance = me->GetInstanceScript())
                    if (Creature* razuvious = instance->GetCreature(DATA_RAZUVIOUS))
                        me->SetFacingToObject(razuvious);
                break;
            case ACTION_TALK:
                Talk(SAY_DEATH_KNIGHT_UNDERSTUDY);
                break;
            case ACTION_EMOTE:
                me->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                break;
            case ACTION_SALUTE:
                me->HandleEmoteCommand(EMOTE_ONESHOT_SALUTE);
                break;
            case ACTION_BACK_TO_TRAINING:
                me->SetSheath(SHEATH_STATE_MELEE);
                ScheduleAttackDummy();
                break;
            default:
                break;
        }
    }

    void JustEngagedWith(Unit* who) override
    {
        _scheduler.CancelGroup(GROUP_OOC_RP);

        if (InstanceScript* instance = me->GetInstanceScript())
        {
            if (Creature* razuvious = instance->GetCreature(DATA_RAZUVIOUS))
            {
                razuvious->AI()->DoZoneInCombat();
                razuvious->AI()->AttackStart(who);
            }
        }
    }

    void UpdateAI(uint32 diff) override
    {
        _scheduler.Update(diff);

        if (UpdateVictim())
            if (!me->HasUnitState(UNIT_STATE_CASTING) || !me->IsCharmed())
                DoMeleeAttackIfReady();
    }

private:
    TaskScheduler _scheduler;
};

void AddSC_boss_razuvious_40()
{
    RegisterNaxxramasCreatureAI(boss_razuvious_40);
    RegisterNaxxramasCreatureAI(boss_razuvious_minion_40);
}
