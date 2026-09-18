/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Grobbulus. Ported from mod-individual-progression
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
#include "PassiveAI.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <list>

enum GrobbulusSpells40
{
    SPELL_POISON_CLOUD                      = 28240,
    SPELL_MUTATING_INJECTION                = 28169,
    // 28157 cannot be reused: it triggers the Wrath spell script.
    SPELL_SLIME_SPRAY_40                    = 90008,
    SPELL_POISON_CLOUD_DAMAGE_AURA          = 28158,
    SPELL_BERSERK                           = 26662
};

enum GrobbulusEmotes40
{
    EMOTE_SLIME                             = 0
};

enum GrobbulusEvents40
{
    EVENT_BERSERK                           = 1,
    EVENT_POISON_CLOUD                      = 2,
    EVENT_SLIME_SPRAY                       = 3,
    EVENT_MUTATING_INJECTION                = 4
};

// FactionTemplate 21, Booty Bay: hostile to nothing, so the cloud cannot be
// attacked or pulled. Matches what the creature template already carries; set
// again here because the cloud is summoned by a spell that could carry over the
// summoner's faction.
static constexpr uint32 FACTION_TEMPLATE_BOOTY_BAY = 21;

struct boss_grobbulus_40 : public VanillaRaidsBossAI
{
    boss_grobbulus_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_GROBBULUS) { }

    void Reset() override
    {
        BossAI::Reset();
        events.Reset();
        summons.DespawnAll();
        _dropSludgeTimer = 0;
    }

    void PullChamberAdds()
    {
        std::list<Creature*> stichedGiants;
        me->GetCreatureListWithEntryInGrid(stichedGiants, NPC_STICHED_GIANT_40, 300.0f);
        for (Creature* giant : stichedGiants)
            giant->AI()->AttackStart(me->GetVictim());
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        PullChamberAdds();
        DoZoneInCombat();
        events.ScheduleEvent(EVENT_POISON_CLOUD, 15s);
        events.ScheduleEvent(EVENT_MUTATING_INJECTION, 20s);
        events.ScheduleEvent(EVENT_SLIME_SPRAY, 10s);
        events.ScheduleEvent(EVENT_BERSERK, 540s);
    }

    void SpellHitTarget(WorldObject* target, SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id == SPELL_SLIME_SPRAY_40 && target->IsPlayer())
            me->SummonCreature(NPC_FALLOUT_SLIME_40, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    }

    void JustSummoned(Creature* summon) override
    {
        if (summon->GetEntry() == NPC_FALLOUT_SLIME_40)
            DoZoneInCombat(summon);

        summons.Summon(summon);
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        summons.Despawn(summon);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        summons.DespawnAll();
    }

    void UpdateAI(uint32 diff) override
    {
        // Out of combat he keeps bombarding the sewer below with slime.
        _dropSludgeTimer += diff;
        if (!me->IsInCombat() && _dropSludgeTimer >= 5000)
        {
            if (me->IsWithinDist3d(3178.0f, -3305.0f, 319.0f, 5.0f) && !summons.HasEntry(NPC_SEWAGE_SLIME_40))
                me->CastSpell(Position(3128.96f + irand(-20, 20), -3312.96f + irand(-20, 20), 293.25f),
                    SPELL_BOMBARD_SLIME_40, CastSpellExtraArgs(TRIGGERED_NONE));

            _dropSludgeTimer = 0;
        }

        if (!UpdateVictim())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_POISON_CLOUD:
                DoCastSelf(SPELL_POISON_CLOUD, CastSpellExtraArgs(true));
                events.Repeat(15s);
                break;
            case EVENT_BERSERK:
                DoCastSelf(SPELL_BERSERK, CastSpellExtraArgs(true));
                break;
            case EVENT_SLIME_SPRAY:
                Talk(EMOTE_SLIME);
                if (Unit* target = me->GetVictim())
                    me->CastSpell(target, SPELL_SLIME_SPRAY_40,
                        CastSpellExtraArgs(TRIGGERED_NONE).AddSpellBP0(int32(urand(3200, 4800))));
                events.Repeat(20s);
                break;
            case EVENT_MUTATING_INJECTION:
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 1, 100.0f, true, true, -int32(SPELL_MUTATING_INJECTION)))
                    me->CastSpell(target, SPELL_MUTATING_INJECTION, CastSpellExtraArgs(TRIGGERED_NONE));
                events.Repeat(Milliseconds(6000 + uint32(120 * me->GetHealthPct())));
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    uint32 _dropSludgeTimer = 0;
};

struct boss_grobbulus_poison_cloud_40 : public NullCreatureAI
{
    boss_grobbulus_poison_cloud_40(Creature* creature) : NullCreatureAI(creature) { }

    void Reset() override
    {
        _sizeTimer = 0;
        _auraVisualTimer = 1;
        me->SetCombatReach(2.0f);
        me->SetFaction(FACTION_TEMPLATE_BOOTY_BAY);
    }

    void UpdateAI(uint32 diff) override
    {
        if (_auraVisualTimer) // delayed, or the client never shows it
        {
            _auraVisualTimer += diff;
            if (_auraVisualTimer >= 1000)
            {
                DoCastSelf(SPELL_POISON_CLOUD_DAMAGE_AURA, CastSpellExtraArgs(true));
                _auraVisualTimer = 0;
            }
        }

        // Grows to 15yd over 60 seconds - 0.00025 yards of reach per millisecond.
        _sizeTimer += diff;
        me->SetCombatReach(2.0f + (0.00025f * _sizeTimer));
    }

private:
    uint32 _sizeTimer = 0;
    uint32 _auraVisualTimer = 0;
};

void AddSC_boss_grobbulus_40()
{
    RegisterNaxxramasCreatureAI(boss_grobbulus_40);
    RegisterNaxxramasCreatureAI(boss_grobbulus_poison_cloud_40);
}
