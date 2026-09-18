/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Anub'Rekhan. Ported from mod-individual-progression
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

enum AnubSays40
{
    SAY_AGGRO                       = 0,
    SAY_GREET                       = 1,
    SAY_SLAY                        = 2,
    EMOTE_LOCUST                    = 3
};

enum GuardSays40
{
    EMOTE_SPAWN                     = 1,
    EMOTE_SCARAB                    = 2
};

enum AnubSpells40
{
    SPELL_IMPALE                    = 28783,
    SPELL_LOCUST_SWARM              = 28785,
    SPELL_BERSERK                   = 26662
};

Position const cryptguardPositions40[] =
{
    { 3299.732f, -3502.489f, 287.077f, 2.378f },
    { 3299.086f, -3450.929f, 287.077f, 3.999f },
    { 3331.217f, -3476.607f, 287.074f, 3.269f }
};

struct boss_anubrekhan_40 : public VanillaRaidsBossAI
{
    boss_anubrekhan_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_ANUBREKHAN) { }

    void SummonCryptGuards()
    {
        me->SummonCreature(NPC_CRYPT_GUARD_40, cryptguardPositions40[0], TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60s);
        me->SummonCreature(NPC_CRYPT_GUARD_40, cryptguardPositions40[1], TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60s);
    }

    void Reset() override
    {
        BossAI::Reset();
        SummonCryptGuards();
    }

    void JustSummoned(Creature* summon) override
    {
        if (me->IsInCombat())
        {
            DoZoneInCombat(summon);
            if (summon->GetEntry() == NPC_CRYPT_GUARD_40)
                summon->AI()->Talk(EMOTE_SPAWN, me);
        }

        summons.Summon(summon);
    }

    void SummonedCreatureDies(Creature* summon, Unit* /*killer*/) override
    {
        if (summon->GetEntry() == NPC_CRYPT_GUARD_40)
        {
            summon->CastSpell(summon, SPELL_SUMMON_CORPSE_SCARABS_10_40,
                CastSpellExtraArgs(TRIGGERED_FULL_MASK).SetOriginalCaster(me->GetGUID()));
            summon->AI()->Talk(EMOTE_SCARAB);
        }
    }

    void KilledUnit(Unit* victim) override
    {
        if (!victim->IsPlayer())
            return;

        Talk(SAY_SLAY);
        victim->CastSpell(victim, SPELL_SUMMON_CORPSE_SCARABS_5_40,
            CastSpellExtraArgs(TRIGGERED_FULL_MASK).SetOriginalCaster(me->GetGUID()));
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        me->CallForHelp(30.0f);
        Talk(SAY_AGGRO);

        if (!summons.HasEntry(NPC_CRYPT_GUARD_40))
            SummonCryptGuards();

        me->m_Events.AddEventAtOffset([this]
        {
            me->SummonCreature(NPC_CRYPT_GUARD_40, cryptguardPositions40[2], TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60s);
        }, Milliseconds(urand(15000, 20000)));

        ScheduleTimedEvent(15s, [this]
        {
            if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 0.0f, true, true))
                me->CastSpell(target, SPELL_IMPALE, CastSpellExtraArgs(TRIGGERED_NONE)
                    .AddSpellBP1(3937)
                    .AddSpellBP2(299));
        }, 20s);

        ScheduleTimedEvent(70s, 2min, [this]
        {
            Talk(EMOTE_LOCUST);
            DoCastSelf(SPELL_LOCUST_SWARM);

            scheduler.Schedule(3s, [this](TaskContext /*context*/)
            {
                me->SummonCreature(NPC_CRYPT_GUARD_40, cryptguardPositions40[2], TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60s);
            });
        }, 90s);

        ScheduleEnrageTimer(SPELL_BERSERK, 10min);
    }

    void MoveInLineOfSight(Unit* who) override
    {
        if (!_sayGreet && who->IsPlayer())
        {
            Talk(SAY_GREET);
            _sayGreet = true;
        }

        ScriptedAI::MoveInLineOfSight(who);
    }

private:
    bool _sayGreet = false;
};

void AddSC_boss_anubrekhan_40()
{
    RegisterNaxxramasCreatureAI(boss_anubrekhan_40);
}
