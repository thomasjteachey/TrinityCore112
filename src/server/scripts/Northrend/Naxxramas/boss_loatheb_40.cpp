/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Loatheb. Ported from mod-individual-progression
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

enum LoathebSpells40
{
    // 180-220 AoE poison damage; recast every 6 seconds it stands in for the
    // vanilla poison aura.
    SPELL_POISON_SHOCK                          = 22595,
    // Triggers one of 29185, 29194, 29196, 29198 depending on the target's class.
    SPELL_CORRUPTED_MIND                        = 29201,
    SPELL_INEVITABLE_DOOM                       = 29204,
    SPELL_REMOVE_CURSE                          = 30281  // he periodically cleanses himself
    // No Berserk: he does not cast it in Naxxramas 40.
};

enum LoathebEvents40
{
    EVENT_NECROTIC_AURA                         = 1, // actually Corrupted Mind; the name is kept for strategy parity
    EVENT_DEATHBLOOM                            = 2, // actually Poison Shock
    EVENT_INEVITABLE_DOOM                       = 3,
    EVENT_REMOVE_CURSE                          = 4,
    EVENT_SUMMON_SPORE                          = 5
};

struct boss_loatheb_40 : public VanillaRaidsBossAI
{
    boss_loatheb_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_LOATHEB)
    {
        me->SetHomePosition(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation());
    }

    void Reset() override
    {
        BossAI::Reset();
        events.Reset();
        summons.DespawnAll();
        _doomCounter = 0;
    }

    void JustSummoned(Creature* summon) override
    {
        DoZoneInCombat(summon);
        summons.Summon(summon);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        DoZoneInCombat();
        events.ScheduleEvent(EVENT_NECROTIC_AURA, 5s);
        events.ScheduleEvent(EVENT_DEATHBLOOM, 5s);
        events.ScheduleEvent(EVENT_INEVITABLE_DOOM, 2min);
        events.ScheduleEvent(EVENT_SUMMON_SPORE, 15s);
        events.ScheduleEvent(EVENT_REMOVE_CURSE, 5s);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        summons.DespawnAll();
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim() || !IsInRoom())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_SUMMON_SPORE:
                DoCastSelf(SPELL_SUMMON_SPORE_40, CastSpellExtraArgs(true));
                events.Repeat(13s);
                break;
            case EVENT_NECROTIC_AURA: // Corrupted Mind
                if (me->CastSpell(me, SPELL_CORRUPTED_MIND, true) == SPELL_CAST_OK)
                    events.Repeat(10s);
                else
                    events.Repeat(100ms);
                break;
            case EVENT_DEATHBLOOM: // Poison Shock
                if (me->CastSpell(me, SPELL_POISON_SHOCK, true) == SPELL_CAST_OK)
                    events.Repeat(6s);
                else
                    events.Repeat(100ms);
                break;
            case EVENT_INEVITABLE_DOOM:
                if (me->CastSpell(me, SPELL_INEVITABLE_DOOM,
                        CastSpellExtraArgs(TRIGGERED_NONE).AddSpellBP0(2549)) == SPELL_CAST_OK)
                {
                    ++_doomCounter;
                    events.Repeat(_doomCounter < 6 ? 30s : 15s);
                }
                else
                    events.Repeat(100ms);
                break;
            case EVENT_REMOVE_CURSE:
                DoCastSelf(SPELL_REMOVE_CURSE, CastSpellExtraArgs(true));
                events.Repeat(30s);
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

    bool IsInRoom()
    {
        // Distance from his home position, i.e. has he been dragged out past the gate.
        if (me->GetExactDist(me->GetHomePosition().GetPositionX(),
                             me->GetHomePosition().GetPositionY(),
                             me->GetHomePosition().GetPositionZ()) > 50.0f)
        {
            EnterEvadeMode(EVADE_REASON_BOUNDARY);
            return false;
        }

        return true;
    }

private:
    uint8 _doomCounter = 0;
};

void AddSC_boss_loatheb_40()
{
    RegisterNaxxramasCreatureAI(boss_loatheb_40);
}
