/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Sapphiron. Ported from mod-individual-progression
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
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <algorithm>
#include <vector>

enum SapphironYells40
{
    EMOTE_AIR_PHASE         = 0,
    EMOTE_GROUND_PHASE      = 1,
    EMOTE_BREATH            = 2,
    EMOTE_ENRAGE            = 3
};

enum SapphironSpells40
{
    SPELL_FROST_AURA                = 28531,
    SPELL_CLEAVE                    = 19983,
    SPELL_TAIL_SWEEP                = 15847, // the vanilla tail sweep, not 55697
    SPELL_LIFE_DRAIN                = 28542,
    SPELL_BERSERK                   = 26662,

    SPELL_ICEBOLT_CAST              = 28526,
    SPELL_ICEBOLT_TRIGGER           = 28522,
    SPELL_FROST_MISSILE             = 30101,
    SPELL_FROST_EXPLOSION           = 28524,

    SPELL_SAPPHIRON_DIES            = 29357
};

enum SapphironMisc40
{
    NPC_BLIZZARD_40                 = 16474,
    POINT_CENTER                    = 1,

    // Must match ACTION_BIRTH in boss_sapphiron.cpp: go_sapphiron_birth sends it
    // to whichever Sapphiron the instance is holding, both versions included.
    ACTION_BIRTH                    = 1
};

enum SapphironEvents40
{
    EVENT_BERSERK                   = 1,
    EVENT_CLEAVE                    = 2,
    EVENT_TAIL_SWEEP                = 3,
    EVENT_LIFE_DRAIN                = 4,
    EVENT_BLIZZARD                  = 5,
    EVENT_FLIGHT_START              = 6,
    EVENT_FLIGHT_LIFTOFF            = 7,
    EVENT_FLIGHT_ICEBOLT            = 8,
    EVENT_FLIGHT_BREATH             = 9,
    EVENT_FLIGHT_SPELL_EXPLOSION    = 10,
    EVENT_FLIGHT_START_LAND         = 11,
    EVENT_LAND                      = 12,
    EVENT_GROUND                    = 13
};

struct boss_sapphiron_40 : public VanillaRaidsBossAI
{
    boss_sapphiron_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_SAPPHIRON) { }

    void InitializeAI() override
    {
        if (instance->GetBossState(BOSS_SAPPHIRON) != DONE)
        {
            me->SummonGameObject(GO_BIRTH, me->GetPosition(), QuaternionData(), 0s);
            me->SetVisible(false);
            me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
            me->SetReactState(REACT_PASSIVE);
            ScriptedAI::InitializeAI();
        }
    }

    bool IsInRoom()
    {
        if (me->GetExactDist(3523.5f, -5235.3f, 137.6f) > 100.0f)
        {
            EnterEvadeMode(EVADE_REASON_BOUNDARY);
            return false;
        }

        return true;
    }

    void Reset() override
    {
        BossAI::Reset();
        if (me->IsVisible())
            me->SetReactState(REACT_AGGRESSIVE);

        events.Reset();
        _iceboltCount = 0;
        _spawnTimer = 0;
        _currentTarget.Clear();
        _blockList.clear();
    }

    // He pulls the whole room rather than waiting for threat to spread.
    void PullEveryoneInRange()
    {
        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        if (players.isEmpty())
            return;

        for (auto const& itr : players)
        {
            Player* player = itr.GetSource();
            if (!player || player->IsGameMaster())
                continue;

            if (player->IsAlive() && me->GetDistance(player) < 80.0f)
            {
                me->SetInCombatWith(player);
                player->SetInCombatWith(me);
                me->GetThreatManager().AddThreat(player, 0.0f);
            }
        }
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        PullEveryoneInRange();
        DoCastSelf(SPELL_FROST_AURA, CastSpellExtraArgs(true));
        events.ScheduleEvent(EVENT_BERSERK, 15min);
        events.ScheduleEvent(EVENT_CLEAVE, 5s);
        events.ScheduleEvent(EVENT_TAIL_SWEEP, 10s);
        events.ScheduleEvent(EVENT_LIFE_DRAIN, 17s);
        events.ScheduleEvent(EVENT_BLIZZARD, 17s);
        events.ScheduleEvent(EVENT_FLIGHT_START, 45s);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        DoCastSelf(SPELL_SAPPHIRON_DIES, CastSpellExtraArgs(true));
    }

    void DoAction(int32 param) override
    {
        if (param == ACTION_BIRTH)
            _spawnTimer = 1;
    }

    void MovementInform(uint32 type, uint32 id) override
    {
        if (type == POINT_MOTION_TYPE && id == POINT_CENTER)
            events.ScheduleEvent(EVENT_FLIGHT_LIFTOFF, 500ms);
    }

    void SpellHitTarget(WorldObject* target, SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id == SPELL_ICEBOLT_CAST)
            me->CastSpell(target, SPELL_ICEBOLT_TRIGGER, CastSpellExtraArgs(true));
    }

    void UpdateAI(uint32 diff) override
    {
        if (_spawnTimer)
        {
            _spawnTimer += diff;
            if (_spawnTimer >= 21500)
            {
                me->SetVisible(true);
                me->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                me->SetReactState(REACT_AGGRESSIVE);
                _spawnTimer = 0;
            }

            return;
        }

        if (!IsInRoom())
            return;

        if (!UpdateVictim())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_BERSERK:
                Talk(EMOTE_ENRAGE);
                DoCastSelf(SPELL_BERSERK, CastSpellExtraArgs(true));
                return;
            case EVENT_CLEAVE:
                DoCastVictim(SPELL_CLEAVE);
                events.Repeat(10s);
                return;
            case EVENT_TAIL_SWEEP:
                DoCastSelf(SPELL_TAIL_SWEEP);
                events.Repeat(10s);
                return;
            case EVENT_LIFE_DRAIN:
                me->CastSpell(me, SPELL_LIFE_DRAIN, CastSpellExtraArgs(TRIGGERED_NONE)
                    .AddSpellBP0(1700)
                    .AddSpellMod(SPELLVALUE_MAX_TARGETS, 5));
                events.Repeat(24s);
                return;
            case EVENT_BLIZZARD:
            {
                TempSummon* blizzard = nullptr;
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                    blizzard = me->SummonCreature(NPC_BLIZZARD_40, *target, TEMPSUMMON_TIMED_DESPAWN, 16s);
                else
                    blizzard = me->SummonCreature(NPC_BLIZZARD_40, *me, TEMPSUMMON_TIMED_DESPAWN, 16s);

                if (blizzard)
                    blizzard->GetMotionMaster()->MoveRandom(40.0f);

                events.Repeat(6500ms);
                return;
            }
            case EVENT_FLIGHT_START:
            {
                if (me->HealthBelowPct(11))
                    return;

                me->SetReactState(REACT_PASSIVE);
                me->AttackStop();
                float x, y, z, o;
                me->GetHomePosition(x, y, z, o);
                me->GetMotionMaster()->MovePoint(POINT_CENTER, x, y, z);
                events.Repeat(45s);
                events.DelayEvents(35s);
                return;
            }
            case EVENT_FLIGHT_LIFTOFF:
                Talk(EMOTE_AIR_PHASE);
                _currentTarget.Clear();
                me->GetMotionMaster()->MoveIdle();
                me->SendMeleeAttackStop(me->GetVictim());
                me->SetDisableGravity(true);
                me->HandleEmoteCommand(EMOTE_ONESHOT_LIFTOFF);
                events.ScheduleEvent(EVENT_FLIGHT_ICEBOLT, 3s);
                _iceboltCount = 3;
                return;
            case EVENT_FLIGHT_ICEBOLT:
            {
                // The ice blocks themselves are TrinityCore's: spell_sapphiron_icebolt
                // raises one under each frozen player and removes it with the aura,
                // so this only has to pick who gets hit and keep count.
                std::vector<Unit*> targets;
                for (ThreatReference const* ref : me->GetThreatManager().GetUnsortedThreatList())
                {
                    Unit* victim = ref->GetVictim();
                    if (!victim || !victim->IsPlayer())
                        continue;

                    if (std::find(_blockList.begin(), _blockList.end(), victim->GetGUID()) == _blockList.end())
                        targets.push_back(victim);
                }

                if (!targets.empty() && _iceboltCount)
                {
                    auto itr = targets.begin();
                    std::advance(itr, urand(0, uint32(targets.size()) - 1));

                    me->CastSpell(*itr, SPELL_ICEBOLT_CAST, CastSpellExtraArgs(TRIGGERED_NONE));
                    _blockList.push_back((*itr)->GetGUID());
                    _currentTarget = (*itr)->GetGUID();
                    --_iceboltCount;
                    events.ScheduleEvent(EVENT_FLIGHT_ICEBOLT, Seconds(uint32(me->GetExactDist(*itr) / 13.0f)));
                }
                else
                    events.ScheduleEvent(EVENT_FLIGHT_BREATH, 1s);

                return;
            }
            case EVENT_FLIGHT_BREATH:
                _currentTarget.Clear();
                Talk(EMOTE_BREATH);
                DoCastSelf(SPELL_FROST_MISSILE);
                events.ScheduleEvent(EVENT_FLIGHT_SPELL_EXPLOSION, 8500ms);
                return;
            case EVENT_FLIGHT_SPELL_EXPLOSION:
                DoCastSelf(SPELL_FROST_EXPLOSION, CastSpellExtraArgs(true));
                events.ScheduleEvent(EVENT_FLIGHT_START_LAND, 3s);
                return;
            case EVENT_FLIGHT_START_LAND:
                for (ObjectGuid const& guid : _blockList)
                    if (Unit* block = ObjectAccessor::GetUnit(*me, guid))
                        block->RemoveAurasDueToSpell(SPELL_ICEBOLT_TRIGGER);

                _blockList.clear();
                events.ScheduleEvent(EVENT_LAND, 1s);
                return;
            case EVENT_LAND:
                me->HandleEmoteCommand(EMOTE_ONESHOT_LAND);
                me->SetDisableGravity(false);
                events.ScheduleEvent(EVENT_GROUND, 1500ms);
                return;
            case EVENT_GROUND:
                Talk(EMOTE_GROUND_PHASE);
                me->SetReactState(REACT_AGGRESSIVE);
                DoZoneInCombat();
                return;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    uint8 _iceboltCount = 0;
    uint32 _spawnTimer = 0;
    GuidVector _blockList;
    ObjectGuid _currentTarget;
};

void AddSC_boss_sapphiron_40()
{
    RegisterNaxxramasCreatureAI(boss_sapphiron_40);
}
