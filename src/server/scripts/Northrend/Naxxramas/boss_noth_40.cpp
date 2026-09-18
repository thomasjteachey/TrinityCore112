/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Noth the Plaguebringer. Ported from mod-individual-progression
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
#include "ScriptedCreature.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"

enum NothSays40
{
    SAY_AGGRO                               = 0,
    SAY_SUMMON                              = 1,
    SAY_SLAY                                = 2,
    SAY_DEATH                               = 3,
    EMOTE_SUMMON                            = 4,
    EMOTE_SUMMON_WAVE                       = 5,
    EMOTE_TELEPORT_BALCONY                  = 6,
    EMOTE_TELEPORT_BACK                     = 7,
    EMOTE_BLINK                             = 8
};

enum NothSpells40
{
    SPELL_CURSE_OF_THE_PLAGUEBRINGER        = 29213,
    SPELL_CRIPPLE                           = 29212,
    SPELL_SUMMON_PLAGUED_WARRIORS           = 29237,
    SPELL_TELEPORT                          = 29216,
    SPELL_TELEPORT_BACK                     = 29231,
    SPELL_BERSERK                           = 68378,
    SPELL_BLINK                             = 29208
};

enum NothEvents40
{
    EVENT_CURSE                             = 1,
    EVENT_CRIPPLE                           = 2,
    EVENT_SUMMON_PLAGUED_WARRIOR_ANNOUNCE   = 3,
    EVENT_MOVE_TO_BALCONY                   = 4,
    EVENT_BLINK                             = 5,
    EVENT_MOVE_TO_GROUND                    = 6,
    EVENT_SUMMON_PLAGUED_WARRIOR_REAL       = 7,
    EVENT_BALCONY_SUMMON_ANNOUNCE           = 8,
    EVENT_BALCONY_SUMMON_REAL               = 9
};

Position const summoningPosition40[5] =
{
    { 2728.06f, -3535.38f, 263.21f, 2.75f },
    { 2725.71f, -3514.80f, 263.23f, 2.86f },
    { 2728.24f, -3465.08f, 264.20f, 3.56f },
    { 2704.79f, -3459.17f, 263.74f, 4.25f },
    { 2652.02f, -3459.13f, 262.50f, 5.39f }
};

Position const nothPosition40 = { 2684.94f, -3502.53f, 261.31f, 4.7f };

struct boss_noth_40 : public VanillaRaidsBossAI
{
    boss_noth_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_NOTH) { }

    void StartGroundPhase()
    {
        me->SetReactState(REACT_AGGRESSIVE);
        me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
        me->SetControlled(false, UNIT_STATE_ROOT);
        events.Reset();
        events.ScheduleEvent(EVENT_MOVE_TO_BALCONY, 110s);
        events.ScheduleEvent(EVENT_CURSE, 15s);
        events.ScheduleEvent(EVENT_SUMMON_PLAGUED_WARRIOR_ANNOUNCE, 10s);
        events.ScheduleEvent(EVENT_BLINK, 35s);
    }

    void StartBalconyPhase()
    {
        me->SetReactState(REACT_PASSIVE);
        me->AttackStop();
        me->SetUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
        me->SetControlled(true, UNIT_STATE_ROOT);
        events.Reset();
        events.ScheduleEvent(EVENT_BALCONY_SUMMON_ANNOUNCE, 4s);
        events.ScheduleEvent(EVENT_MOVE_TO_GROUND, 70s);
    }

    void SummonHelper(uint32 entry, uint32 count)
    {
        for (uint32 i = 0; i < count; ++i)
            me->SummonCreature(entry, summoningPosition40[urand(0, 4)]);
    }

    bool IsInRoom()
    {
        if (me->GetExactDist(2684.8f, -3502.5f, 261.3f) > 80.0f)
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
        DoCastSelf(SPELL_TELEPORT_BACK, CastSpellExtraArgs(true));
        me->SetControlled(false, UNIT_STATE_ROOT);
        me->SetReactState(REACT_AGGRESSIVE);
        _timesInBalcony = 0;
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        me->SetControlled(false, UNIT_STATE_ROOT);
        ScriptedAI::EnterEvadeMode(why);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_AGGRO);
        StartGroundPhase();
    }

    void JustSummoned(Creature* summon) override
    {
        summons.Summon(summon);
        DoZoneInCombat(summon);
    }

    void JustDied(Unit* killer) override
    {
        if (me->GetPositionZ() > 270.27f)
        {
            me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
            me->NearTeleportTo(nothPosition40.GetPositionX(), nothPosition40.GetPositionY(),
                nothPosition40.GetPositionZ(), nothPosition40.GetOrientation(), true);
        }

        BossAI::JustDied(killer);
        Talk(SAY_DEATH);
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        Talk(SAY_SLAY);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!IsInRoom())
            return;

        if (!UpdateVictim())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        events.Update(diff);

        switch (events.ExecuteEvent())
        {
            // GROUND
            case EVENT_CURSE:
                if (events.GetPhaseMask() == 0)
                    me->CastSpell(me, SPELL_CURSE_OF_THE_PLAGUEBRINGER,
                        CastSpellExtraArgs(TRIGGERED_NONE).AddSpellMod(SPELLVALUE_MAX_TARGETS, 20));
                events.Repeat(50s, 60s);
                break;
            case EVENT_SUMMON_PLAGUED_WARRIOR_ANNOUNCE:
                Talk(SAY_SUMMON);
                Talk(EMOTE_SUMMON);
                events.Repeat(30s);
                events.ScheduleEvent(EVENT_SUMMON_PLAGUED_WARRIOR_REAL, 4s);
                break;
            case EVENT_SUMMON_PLAGUED_WARRIOR_REAL:
                DoCastSelf(SPELL_SUMMON_PLAGUED_WARRIORS, CastSpellExtraArgs(true));
                SummonHelper(NPC_PLAGUED_WARRIOR_40, 3);
                break;
            case EVENT_MOVE_TO_BALCONY:
                Talk(EMOTE_TELEPORT_BALCONY);
                DoCastSelf(SPELL_TELEPORT, CastSpellExtraArgs(true));
                StartBalconyPhase();
                break;
            case EVENT_BLINK:
                ResetThreatList();
                DoCastSelf(SPELL_CRIPPLE);
                DoCastSelf(SPELL_BLINK, CastSpellExtraArgs(true));
                Talk(EMOTE_BLINK);
                events.Repeat(30s);
                break;
            // BALCONY
            case EVENT_BALCONY_SUMMON_ANNOUNCE:
                Talk(EMOTE_SUMMON_WAVE);
                events.Repeat(30s);
                events.ScheduleEvent(EVENT_BALCONY_SUMMON_REAL, 4s);
                break;
            case EVENT_BALCONY_SUMMON_REAL:
                DoCastSelf(SPELL_SUMMON_PLAGUED_WARRIORS, CastSpellExtraArgs(true)); // visual
                switch (_timesInBalcony)
                {
                    case 0:
                        SummonHelper(NPC_PLAGUED_CHAMPION_40, 4);
                        break;
                    case 1:
                        SummonHelper(NPC_PLAGUED_CHAMPION_40, 2);
                        SummonHelper(NPC_PLAGUED_GUARDIAN_40, 2);
                        break;
                    default:
                        SummonHelper(NPC_PLAGUED_GUARDIAN_40, 4);
                        break;
                }
                break;
            case EVENT_MOVE_TO_GROUND:
                Talk(EMOTE_TELEPORT_BACK);
                DoCastSelf(SPELL_TELEPORT_BACK, CastSpellExtraArgs(true));
                ++_timesInBalcony;
                if (_timesInBalcony == 3)
                    DoCastSelf(SPELL_BERSERK);
                StartGroundPhase();
                break;
            default:
                break;
        }

        if (me->HasReactState(REACT_AGGRESSIVE))
            DoMeleeAttackIfReady();
    }

private:
    uint8 _timesInBalcony = 0;
};

void AddSC_boss_noth_40()
{
    RegisterNaxxramasCreatureAI(boss_noth_40);
}
