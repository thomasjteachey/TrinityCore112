/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Patchwerk. Ported from mod-individual-progression
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
#include "ThreatManager.h"
#include "VanillaRaids/VanillaRaids.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <vector>

enum PatchwerkYells40
{
    SAY_AGGRO                       = 0,
    SAY_SLAY                        = 1,
    SAY_DEATH                       = 2,
    EMOTE_BERSERK                   = 3,
    EMOTE_ENRAGE                    = 4
};

enum PatchwerkSpells40
{
    SPELL_HATEFUL_STRIKE            = 41926,
    SPELL_FRENZY                    = 28131,
    SPELL_BERSERK                   = 26662,
    SPELL_SLIME_BOLT                = 32309
};

enum PatchwerkEvents40
{
    EVENT_HEALTH_CHECK              = 1,
    EVENT_HATEFUL_STRIKE            = 2,
    EVENT_SLIME_BOLT                = 3,
    EVENT_BERSERK                   = 4
};

struct boss_patchwerk_40 : public VanillaRaidsBossAI
{
    boss_patchwerk_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_PATCHWERK) { }

    void Reset() override
    {
        BossAI::Reset();
        events.Reset();
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

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_AGGRO);
        DoZoneInCombat();
        events.ScheduleEvent(EVENT_HATEFUL_STRIKE, 1200ms);
        events.ScheduleEvent(EVENT_BERSERK, 7min);
        events.ScheduleEvent(EVENT_HEALTH_CHECK, 1s);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_HATEFUL_STRIKE:
            {
                // Hateful Strike lands on the highest-health player in melee
                // range, skipping whoever is first on threat, and the top three
                // on the list are topped up with threat so the tank keeps aggro.
                std::vector<Unit*> meleeRangeTargets;
                std::vector<Unit*> threatTopUp;
                uint8 counter = 0;

                for (ThreatReference const* ref : me->GetThreatManager().GetSortedThreatList())
                {
                    Unit* target = ref->GetVictim();
                    if (!target)
                    {
                        ++counter;
                        continue;
                    }

                    if (me->IsWithinMeleeRange(target))
                        meleeRangeTargets.push_back(target);

                    if (counter < 3)
                        threatTopUp.push_back(target);

                    ++counter;
                }

                // Applied after the walk so that re-sorting cannot disturb the
                // iteration above.
                for (Unit* target : threatTopUp)
                    me->GetThreatManager().AddThreat(target, 500.0f);

                Unit* finalTarget = nullptr;
                counter = 0;
                for (Unit* target : meleeRangeTargets)
                {
                    // if there is only one target available
                    if (meleeRangeTargets.size() == 1)
                    {
                        finalTarget = target;
                    }
                    else if (counter > 0) // skip first target
                    {
                        if (!finalTarget || target->GetHealth() > finalTarget->GetHealth())
                            finalTarget = target;

                        // third loop
                        if (counter >= 2)
                            break;
                    }

                    ++counter;
                }

                if (finalTarget)
                {
                    int32 dmg = int32(urand(22100, 22850));

                    if (VanillaRaids::NerfPatchwerk())
                        dmg = int32(urand(17680, 18280)); // 80% of 22100-22850

                    me->CastSpell(finalTarget, SPELL_HATEFUL_STRIKE,
                        CastSpellExtraArgs(TRIGGERED_NONE).AddSpellBP0(dmg));
                }

                events.Repeat(1200ms);
                break;
            }
            case EVENT_BERSERK:
                Talk(EMOTE_BERSERK);
                DoCastSelf(SPELL_BERSERK, CastSpellExtraArgs(true));
                events.ScheduleEvent(EVENT_SLIME_BOLT, 3s);
                break;
            case EVENT_SLIME_BOLT:
                DoCastSelf(SPELL_SLIME_BOLT);
                events.Repeat(3s);
                break;
            case EVENT_HEALTH_CHECK:
                if (me->GetHealthPct() <= 5)
                {
                    Talk(EMOTE_ENRAGE);
                    DoCastSelf(SPELL_FRENZY, CastSpellExtraArgs(true));
                    break;
                }
                events.Repeat(1s);
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }
};

void AddSC_boss_patchwerk_40()
{
    RegisterNaxxramasCreatureAI(boss_patchwerk_40);
}
