/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
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

#ifndef VANILLA_RAIDS_AI_H
#define VANILLA_RAIDS_AI_H

#include "ScriptedCreature.h"
#include "SpellDefines.h"
#include <functional>

// The vanilla Naxxramas and Onyxia encounters were ported from AzerothCore,
// whose BossAI carries two timer helpers TrinityCore does not have, and whose
// BossAI::UpdateAI ticks the TaskScheduler where TrinityCore's does not.
// Reproducing both here - rather than rewriting every timer by hand across
// fifteen boss scripts - keeps a ported encounter readable against the original
// and re-syncable later.
//
// AzerothCore reference: ScriptedAI::ScheduleTimedEvent, BossAI::ScheduleEnrageTimer
// and BossAI::UpdateAI in src/server/game/AI/ScriptedAI/ScriptedCreature.cpp.
//
// Only bosses need this: the trash and summon AIs in these raids drive their
// timers off EventMap, which TrinityCore ticks the same way AzerothCore does.
struct VanillaRaidsBossAI : public BossAI
{
    using BossAI::BossAI;

    // Runs exec once in [timerMin, timerMax] - or exactly at timerMax when
    // timerMin is zero - then repeats it every repeatMin, or at a random point
    // in [repeatMin, repeatMax] when repeatMax is given. A zero repeatMin means
    // "do not repeat", which is how upstream expresses a one-shot too.
    void ScheduleTimedEvent(Milliseconds timerMin, Milliseconds timerMax, std::function<void()> exec,
        Milliseconds repeatMin, Milliseconds repeatMax = 0ms)
    {
        scheduler.Schedule(timerMin == 0ms ? timerMax : timerMin, timerMax,
            [exec = std::move(exec), repeatMin, repeatMax](TaskContext context)
        {
            exec();

            if (repeatMin > 0ms)
                repeatMax > 0ms ? context.Repeat(repeatMin, repeatMax) : context.Repeat(repeatMin);
        });
    }

    void ScheduleTimedEvent(Milliseconds timerMax, std::function<void()> exec,
        Milliseconds repeatMin, Milliseconds repeatMax = 0ms)
    {
        ScheduleTimedEvent(0ms, timerMax, std::move(exec), repeatMin, repeatMax);
    }

    // A hard enrage: fires once and never repeats. Runs off the creature's own
    // event processor rather than the scheduler, so it survives the scheduler
    // resets an encounter does on a phase change - as upstream.
    void ScheduleEnrageTimer(uint32 spellId, Milliseconds timer, uint8 textId = 0)
    {
        me->m_Events.AddEventAtOffset([this, spellId, textId]
        {
            if (!me->IsAlive())
                return;

            if (textId)
                Talk(textId);

            DoCastSelf(spellId, CastSpellExtraArgs(true));
        }, timer);
    }

    // Mirrors AzerothCore's BossAI::UpdateAI, which ticks the scheduler as well
    // as the EventMap. An encounter that overrides UpdateAI and uses the
    // scheduler has to call UpdateSchedulers itself.
    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        UpdateSchedulers(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = events.ExecuteEvent())
        {
            ExecuteEvent(eventId);
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        DoMeleeAttackIfReady();
    }

protected:
    void UpdateSchedulers(uint32 diff)
    {
        events.Update(diff);
        scheduler.Update(diff);
    }
};

#endif // VANILLA_RAIDS_AI_H
