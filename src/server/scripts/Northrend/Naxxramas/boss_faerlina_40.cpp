/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Grand Widow Faerlina. Ported from mod-individual-progression
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
#include "SpellInfo.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"

enum FaerlinaYells40
{
    SAY_GREET                           = 0,
    SAY_AGGRO                           = 1,
    SAY_SLAY                            = 2,
    SAY_DEATH                           = 3,
    EMOTE_WIDOWS_EMBRACE                = 4,
    EMOTE_FRENZY                        = 5,
    SAY_FRENZY                          = 6
};

enum FaerlinaSpells40
{
    SPELL_POISON_BOLT_VOLLEY            = 28796,
    SPELL_RAIN_OF_FIRE                  = 28794,
    SPELL_FRENZY                        = 28798,
    SPELL_WIDOWS_EMBRACE                = 28732
};

enum FaerlinaSpellValues40 : int32
{
    POISON_BOLT_VOLLEY_BP0              = 1224,
    POISON_BOLT_VOLLEY_BP1              = 416,
    RAIN_OF_FIRE_BP0                    = 1849,
    FRENZY_BP0                          = 149,
    FRENZY_BP1                          = 74,
    FRENZY_BP2                          = 49
};

enum FaerlinaGroups40
{
    GROUP_FRENZY                        = 1
};

struct boss_faerlina_40 : public VanillaRaidsBossAI
{
    boss_faerlina_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_FAERLINA) { }

    void SummonHelpers()
    {
        me->SummonCreature(NPC_NAXXRAMAS_WORSHIPPER_40, 3362.66f, -3620.97f, 261.08f, 4.57276f);
        me->SummonCreature(NPC_NAXXRAMAS_WORSHIPPER_40, 3344.3f, -3618.31f, 261.08f, 4.69494f);
        me->SummonCreature(NPC_NAXXRAMAS_WORSHIPPER_40, 3356.71f, -3620.05f, 261.08f, 4.57276f);
        me->SummonCreature(NPC_NAXXRAMAS_WORSHIPPER_40, 3350.26f, -3619.11f, 261.08f, 4.67748f);
        me->SummonCreature(NPC_NAXXRAMAS_FOLLOWER_40, 3347.49f, -3617.59f, 261.0f, 4.49f);
        me->SummonCreature(NPC_NAXXRAMAS_FOLLOWER_40, 3359.64f, -3619.16f, 261.0f, 4.56f);
    }

    void Reset() override
    {
        BossAI::Reset();
        summons.DespawnAll();
        SummonHelpers();
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_AGGRO);

        scheduler.Schedule(1200ms, [this](TaskContext /*context*/)
        {
            summons.DoZoneInCombat();
        });

        ScheduleTimedEvent(7s, 15s, [this]
        {
            if (!me->HasAura(SPELL_WIDOWS_EMBRACE))
                me->CastSpell(me, SPELL_POISON_BOLT_VOLLEY, CastSpellExtraArgs(TRIGGERED_NONE)
                    .AddSpellMod(SPELLVALUE_MAX_TARGETS, 10)
                    .AddSpellBP0(POISON_BOLT_VOLLEY_BP0)
                    .AddSpellBP1(POISON_BOLT_VOLLEY_BP1));
        }, 7s, 15s);

        ScheduleTimedEvent(8s, 18s, [this]
        {
            if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0))
                me->CastSpell(target, SPELL_RAIN_OF_FIRE, CastSpellExtraArgs(TRIGGERED_NONE)
                    .AddSpellBP0(RAIN_OF_FIRE_BP0));
        }, 8s, 18s);

        scheduler.Schedule(60s, 80s, GROUP_FRENZY, [this](TaskContext context)
        {
            if (!me->HasAura(SPELL_WIDOWS_EMBRACE))
            {
                Talk(SAY_FRENZY);
                Talk(EMOTE_FRENZY);
                me->CastSpell(me, SPELL_FRENZY, CastSpellExtraArgs(true)
                    .AddSpellBP0(FRENZY_BP0)
                    .AddSpellBP1(FRENZY_BP1)
                    .AddSpellBP2(FRENZY_BP2));
                context.Repeat(1min);
            }
            else
                context.Repeat(30s);
        });
    }

    void MoveInLineOfSight(Unit* who) override
    {
        if (!_introDone && who->IsPlayer())
        {
            Talk(SAY_GREET);
            _introDone = true;
        }

        ScriptedAI::MoveInLineOfSight(who);
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        if (!urand(0, 3))
            Talk(SAY_SLAY);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        Talk(SAY_DEATH);
    }

    void SpellHit(WorldObject* caster, SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id != SPELL_WIDOWS_EMBRACE)
            return;

        Talk(EMOTE_WIDOWS_EMBRACE); // %s is affected by Widow's Embrace!

        if (me->HasAura(SPELL_FRENZY))
        {
            // Sacrificing the worshipper AFTER she enrages is what buys the
            // full 60 seconds.
            scheduler.RescheduleGroup(GROUP_FRENZY, 1min);
            me->RemoveAurasDueToSpell(SPELL_FRENZY);
        }
        else
        {
            // Before the enrage it merely delays it by 30 seconds.
            scheduler.RescheduleGroup(GROUP_FRENZY, 30s);
        }

        if (Unit* casterUnit = caster->ToUnit())
            casterUnit->KillSelf();
    }

private:
    bool _introDone = false;
};

void AddSC_boss_faerlina_40()
{
    RegisterNaxxramasCreatureAI(boss_faerlina_40);
}
