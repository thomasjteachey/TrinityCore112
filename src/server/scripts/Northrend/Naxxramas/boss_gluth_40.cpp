/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Gluth. Ported from mod-individual-progression
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
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "VanillaRaids/VanillaRaids.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <list>

enum GluthSpells40
{
    SPELL_MORTAL_WOUND                  = 25646,
    SPELL_ENRAGE                        = 28371,
    SPELL_DECIMATE                      = 28374,
    SPELL_BERSERK                       = 26662,
    SPELL_INFECTED_WOUND                = 29306,
    SPELL_TERRIFYING_ROAR               = 29685
};

enum GluthEvents40
{
    EVENT_MORTAL_WOUND                  = 1,
    EVENT_ENRAGE                        = 2,
    EVENT_DECIMATE                      = 3,
    EVENT_BERSERK                       = 4,
    EVENT_SUMMON_ZOMBIE                 = 5,
    EVENT_CAN_EAT_ZOMBIE                = 6,
    EVENT_TERRIFYING_ROAR               = 7
};

enum GluthEmotes40
{
    EMOTE_SPOTS_ONE                     = 0,
    EMOTE_DECIMATE                      = 1,
    EMOTE_ENRAGE                        = 2,
    EMOTE_DEVOURS_ALL                   = 3,
    EMOTE_BERSERK                       = 4
};

Position const zombiePos40[3] =
{
    { 3267.9f, -3172.1f, 297.42f, 0.94f },
    { 3253.2f, -3132.3f, 297.42f, 0.0f },
    { 3308.3f, -3185.8f, 297.42f, 1.58f }
};

struct boss_gluth_40 : public VanillaRaidsBossAI
{
    boss_gluth_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_GLUTH) { }

    void Reset() override
    {
        BossAI::Reset();
        me->ApplySpellImmune(SPELL_INFECTED_WOUND, IMMUNITY_ID, SPELL_INFECTED_WOUND, true);
        events.Reset();
        summons.DespawnAll();
        me->SetReactState(REACT_AGGRESSIVE);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        DoZoneInCombat();
        events.ScheduleEvent(EVENT_MORTAL_WOUND, 10s);
        events.ScheduleEvent(EVENT_ENRAGE, 10s);
        events.ScheduleEvent(EVENT_DECIMATE, 110s);
        events.ScheduleEvent(EVENT_BERSERK, 6min);
        events.ScheduleEvent(EVENT_SUMMON_ZOMBIE, 6s);
        events.ScheduleEvent(EVENT_CAN_EAT_ZOMBIE, 3s);
        events.ScheduleEvent(EVENT_TERRIFYING_ROAR, 20s);
    }

    void JustSummoned(Creature* summon) override
    {
        if (summon->GetEntry() == NPC_ZOMBIE_CHOW_40)
            summon->AI()->AttackStart(me);

        summons.Summon(summon);
    }

    void SummonedCreatureDies(Creature* summon, Unit* /*killer*/) override
    {
        summons.Despawn(summon);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        summons.DespawnAll();
    }

    // He eats his way toward whoever is in the room even before anyone pulls.
    bool SelectPlayerInRoom()
    {
        if (me->IsInCombat())
            return false;

        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        for (auto const& itr : players)
        {
            Player* player = itr.GetSource();
            if (!player || !player->IsAlive())
                continue;

            if (player->GetPositionZ() > 300.0f || me->GetExactDist(player) > 50.0f)
                continue;

            AttackStart(player);
            return true;
        }

        return false;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim() && !SelectPlayerInRoom())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_BERSERK:
                Talk(EMOTE_BERSERK);
                DoCastSelf(SPELL_BERSERK, CastSpellExtraArgs(true));
                break;
            case EVENT_ENRAGE:
                Talk(EMOTE_ENRAGE);
                // bp1 melee haste, bp2 damage percent
                if (me->CastSpell(me, SPELL_ENRAGE, CastSpellExtraArgs(true)
                        .AddSpellBP0(99)
                        .AddSpellBP1(49)) == SPELL_CAST_OK)
                    events.Repeat(10s);
                else
                    events.Repeat(100ms);
                break;
            case EVENT_MORTAL_WOUND:
                if (Unit* victim = me->GetVictim())
                    me->CastSpell(victim, SPELL_MORTAL_WOUND, CastSpellExtraArgs(TRIGGERED_NONE));
                events.Repeat(10s);
                break;
            case EVENT_DECIMATE:
            {
                Talk(EMOTE_DECIMATE);
                // The damaging half is TrinityCore's own spell_gluth_decimate;
                // the chow herding below is what the vanilla script adds.
                DoCastSelf(SPELL_DECIMATE);

                std::list<Creature*> zombies;
                me->GetCreatureListWithEntryInGrid(zombies, NPC_ZOMBIE_CHOW_40, 150.0f);
                for (Creature* zombie : zombies)
                {
                    if (!zombie->IsAlive())
                        continue;

                    uint32 const reduceHp = uint32(zombie->GetMaxHealth() * 0.05f);
                    if (zombie->GetHealth() > reduceHp)
                        zombie->SetHealth(reduceHp);

                    zombie->SetWalk(true);
                    zombie->GetMotionMaster()->MoveFollow(me, 0.0f, ChaseAngle(0.0f));
                    zombie->SetReactState(REACT_PASSIVE);

                    Talk(EMOTE_DEVOURS_ALL);
                }

                events.Repeat(105s);
                break;
            }
            case EVENT_SUMMON_ZOMBIE:
            {
                int32 const count = RAID_MODE<int32>(1, 2, 2, 2);
                for (int32 i = 0; i < count; ++i)
                    me->SummonCreature(NPC_ZOMBIE_CHOW_40, zombiePos40[urand(0, 2)]);
                events.Repeat(6s);
                break;
            }
            case EVENT_CAN_EAT_ZOMBIE:
            {
                std::list<Creature*> zombies;
                me->GetCreatureListWithEntryInGrid(zombies, NPC_ZOMBIE_CHOW_40, 10.0f);

                for (Creature* zombie : zombies)
                {
                    if (!zombie->IsAlive())
                        continue;

                    if (me->GetDistance(zombie) < 10.0f) // the range spell 28404 uses
                    {
                        Talk(EMOTE_SPOTS_ONE);

                        zombie->SetWalk(true);
                        zombie->GetMotionMaster()->MoveFollow(me, 0.0f, ChaseAngle(0.0f));
                        zombie->SetReactState(REACT_PASSIVE);

                        if (uint32 damage = uint32(zombie->GetHealth()))
                            Unit::DealDamage(me, zombie, damage);

                        // heals for 5% of his maximum health per chow
                        me->SetHealth(me->GetHealth() + uint32(me->GetMaxHealth() * 0.05f));
                        break; // one chow per go
                    }
                }

                events.Repeat(VanillaRaids::NerfGluth() ? 12s : 3s);
                break;
            }
            case EVENT_TERRIFYING_ROAR:
                if (me->CastSpell(me, SPELL_TERRIFYING_ROAR, true) == SPELL_CAST_OK)
                    events.Repeat(20s);
                else
                    events.Repeat(100ms);
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }
};

void AddSC_boss_gluth_40()
{
    RegisterNaxxramasCreatureAI(boss_gluth_40);
}
