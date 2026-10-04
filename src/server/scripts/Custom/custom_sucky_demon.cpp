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

/*
 * "Sucky Demon" warlock set (ItemSet 1078, items 100990-100997).
 *
 *   3pc  90615 Unshakeable Thirst  - pure Spell.dbc, no code: a 100%
 *                                    SPELLMOD_NOT_LOSE_CASTING_TIME masked to
 *                                    Drain Life and Drain Mana.
 *   5pc  90616 Efficient Gluttony  - pure Spell.dbc: -30% SPELLMOD_COST, same mask.
 *   8pc  90617 All Fear the Sucky  - tooltip only. The form is FORCED on the
 *                                    wearer by custom_hidden_itemset_bonus
 *                                    (1078, 8 -> 90618), the same way Odr's
 *                                    Battlegear forces Hungry.
 *        90618 Sucky Demon Form    - shapeshift 22 (the Metamorphosis graphic)
 *                                    plus the two bundles below. CANT_CANCEL,
 *                                    death-persistent and kept in arenas, so
 *                                    taking a piece off is the only way out.
 *        90619                       -90% speed, +200 magic resistance, +200% Stamina.
 *        90620                       +30% drain effect and the allowed-ability mask.
 *
 * WHAT IS AND IS NOT DATA
 * Everything expressible as an aura lives in Spell.dbc, so it can be retuned
 * without a rebuild. Two things cannot be:
 *
 *   The 10 yard leash. Drain Life ranks 1-6 and Drain Mana use a 20 yard range
 *   index, ranks 7-9 use 30. SPELLMOD_RANGE is a PERCENTAGE, so no single value
 *   yields exactly 10 for both - it would be 10 on one rank and 15 on another.
 *   CheckCast enforces the real number instead.
 *
 *   The splash. "Drains every enemy within 10 yards" is a targeting change, and
 *   no aura reshapes a single-target channel into an area one.
 */

#include "ScriptMgr.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

#include <vector>

namespace
{
    enum SuckyDemonSpells
    {
        SPELL_SUCKY_FORM        = 90618,
        SPELL_SUCKY_FORM_STATS  = 90619,
        SPELL_SUCKY_FORM_HUNGER = 90620,
    };

    constexpr float SUCKY_DEMON_RANGE = 10.0f;

    // Drain Life and Drain Mana, by warlock family mask. Named here rather than
    // matched on spell id so every rank - and any rank added later - is covered.
    constexpr uint32 DRAIN_LIFE_MASK = 8;
    constexpr uint32 DRAIN_MANA_MASK = 16;

    bool IsDrainSpell(SpellInfo const* spellInfo)
    {
        return spellInfo && spellInfo->SpellFamilyName == SPELLFAMILY_WARLOCK &&
            (spellInfo->SpellFamilyFlags[0] & (DRAIN_LIFE_MASK | DRAIN_MANA_MASK)) != 0;
    }

    bool InSuckyDemonForm(Unit const* unit)
    {
        return unit && unit->HasAura(SPELL_SUCKY_FORM);
    }
}

// 90618 - the form. Carries the shapeshift itself; the two stat/behaviour
// bundles ride along so they can be retuned in Spell.dbc independently.
class spell_sucky_demon_form : public AuraScript
{
    PrepareAuraScript(spell_sucky_demon_form);

    void OnApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->CastSpell(target, SPELL_SUCKY_FORM_STATS, true);
        target->CastSpell(target, SPELL_SUCKY_FORM_HUNGER, true);
    }

    void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_SUCKY_FORM_STATS);
        target->RemoveAurasDueToSpell(SPELL_SUCKY_FORM_HUNGER);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_sucky_demon_form::OnApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_sucky_demon_form::OnRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The two things the form needs that no aura expresses: the hard 10 yard leash,
// and turning a single-target channel into an area drain.
//
// Registered against Drain Life and Drain Mana themselves and gated on the form,
// so a warlock not wearing the set is completely unaffected.
class spell_sucky_demon_drain : public SpellScript
{
    PrepareSpellScript(spell_sucky_demon_drain);

    SpellCastResult CheckRange()
    {
        Unit* caster = GetCaster();
        if (!InSuckyDemonForm(caster))
            return SPELL_CAST_OK;

        Unit* target = GetExplTargetUnit();
        if (target && !caster->IsWithinDistInMap(target, SUCKY_DEMON_RANGE))
            return SPELL_FAILED_OUT_OF_RANGE;

        return SPELL_CAST_OK;
    }

    // Every other hostile inside the leash joins the cast as an extra target of
    // the SAME channel. Each one gets its own drain aura ticking alongside the
    // primary's, the healing or mana from all of them returns to the caster, and
    // the whole thing ends when the one channel does.
    //
    // This used to re-cast the drain as a triggered copy on each extra victim.
    // Every copy was a channel of its own, so each one replaced the last as the
    // caster's current channel, and each copy's hit splashed again - a burst of
    // separate Drain Lifes instead of one drain spread across the cast time.
    void HandleSplash()
    {
        Unit* caster = GetCaster();
        if (!InSuckyDemonForm(caster))
            return;

        Unit* primary = GetExplTargetUnit();
        SpellInfo const* spellInfo = GetSpellInfo();
        if (!primary || !spellInfo)
            return;

        // The effects that land on the chosen enemy - the drain aura itself.
        uint32 effectMask = 0;
        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            if (spellEffectInfo.IsEffect() && spellEffectInfo.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY)
                effectMask |= 1 << spellEffectInfo.EffectIndex;
        if (!effectMask)
            return;

        std::vector<Unit*> nearby;
        Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(caster, caster, SUCKY_DEMON_RANGE);
        Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(caster, nearby, check);
        Cell::VisitAllObjects(caster, searcher, SUCKY_DEMON_RANGE);

        for (Unit* extra : nearby)
        {
            if (extra == primary || !extra->IsAlive())
                continue;
            if (!caster->IsValidAttackTarget(extra))
                continue;

            // AddUnitTarget runs the usual target, line-of-sight, immunity and
            // hit/resist checks, so an extra can still miss or be immune.
            AddUnitTarget(extra, effectMask);
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_sucky_demon_drain::CheckRange);
        // OnCast runs after target selection and before any target is hit.
        OnCast += SpellCastFn(spell_sucky_demon_drain::HandleSplash);
    }
};

void AddSC_custom_sucky_demon()
{
    RegisterSpellScript(spell_sucky_demon_form);
    // Bound to every Drain Life / Drain Mana rank through
    // spell_script_names, so it attaches to the core spells rather than
    // to one of ours.
    RegisterSpellScript(spell_sucky_demon_drain);
}
