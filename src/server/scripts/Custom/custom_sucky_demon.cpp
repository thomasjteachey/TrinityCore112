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
 *                                    The form also makes the wearer immune to
 *                                    interrupts and crowd control (below).
 *        90619                       -50% speed, +200 magic resistance, +200% Stamina.
 *        90620                       +30% drain effect and the allowed-ability mask:
 *                                    MOD_IGNORE_SHAPESHIFT for Drain Life, Drain
 *                                    Mana, Fear, Death Coil, Life Tap, the demon
 *                                    summons and Ritual of Doom. Soul Link, Ritual
 *                                    of Summoning (family 0, no mask to match),
 *                                    Inferno and the Health Funnel wrappers carry
 *                                    form 22 in their Stances instead.
 *
 * WHAT IS AND IS NOT DATA
 * Everything expressible as an aura lives in Spell.dbc, so it can be retuned
 * without a rebuild. What cannot be:
 *
 *   The splash. "Drains every enemy within range" is a targeting change, and
 *   no aura reshapes a single-target spell into an area one. Drain Life, Drain
 *   Mana and Death Coil keep their own range (Grim Reach included), and every
 *   other enemy inside that range of the warlock is hit as well.
 *
 *   The immunity. SPELL_AURA_MECHANIC_IMMUNITY_MASK's misc value is not a
 *   mechanic mask on 3.3.5 (see SpellInfo::_LoadImmunityInfo), and three
 *   effects cannot hold one MECHANIC_IMMUNITY per mechanic.
 *
 *   Health Funnel. It is a wrapper (WrappedSpells.h, 93000-93006, one per
 *   rank): outside the form it casts the stock rank at the pet, inside it
 *   casts the matching 93007-93013, a copy aimed at the friendly target that
 *   also hands over mana.
 */

#include "ScriptMgr.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

#include <algorithm>
#include <array>
#include <vector>

namespace
{
    enum SuckyDemonSpells
    {
        SPELL_SUCKY_FORM        = 90618,
        SPELL_SUCKY_FORM_STATS  = 90619,
        SPELL_SUCKY_FORM_HUNGER = 90620,
    };

    bool InSuckyDemonForm(Unit const* unit)
    {
        return unit && unit->HasAura(SPELL_SUCKY_FORM);
    }

    // What "immune to interrupt effects and all crowd control" means. Snares
    // and dazes are left out on purpose: they are slows, not loss of control,
    // and the form already sets the wearer's own speed.
    constexpr std::array<Mechanics, 18> SuckyImmuneMechanics =
    {
        MECHANIC_CHARM, MECHANIC_DISORIENTED, MECHANIC_DISARM, MECHANIC_DISTRACT,
        MECHANIC_FEAR, MECHANIC_GRIP, MECHANIC_ROOT, MECHANIC_SILENCE,
        MECHANIC_SLEEP, MECHANIC_STUN, MECHANIC_FREEZE, MECHANIC_KNOCKOUT,
        MECHANIC_POLYMORPH, MECHANIC_BANISH, MECHANIC_SHACKLE, MECHANIC_HORROR,
        MECHANIC_SAPPED, MECHANIC_INTERRUPT,
    };

    // Plenty of NPC stuns, fears and roots carry no mechanic at all, so the
    // aura types are blocked too. MOD_DECREASE_SPEED is deliberately absent:
    // 90619's own slow is one.
    constexpr std::array<AuraType, 8> SuckyImmuneAuraTypes =
    {
        SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_CONFUSE,
        SPELL_AURA_MOD_CHARM, SPELL_AURA_MOD_POSSESS, SPELL_AURA_MOD_ROOT,
        SPELL_AURA_MOD_SILENCE, SPELL_AURA_MOD_PACIFY_SILENCE,
    };

    void ApplySuckyImmunities(Unit* target, bool apply)
    {
        uint32 mechanicMask = 0;
        for (Mechanics mechanic : SuckyImmuneMechanics)
        {
            target->ApplySpellImmune(SPELL_SUCKY_FORM, IMMUNITY_MECHANIC, mechanic, apply);
            mechanicMask |= 1 << mechanic;
        }
        for (AuraType auraType : SuckyImmuneAuraTypes)
            target->ApplySpellImmune(SPELL_SUCKY_FORM, IMMUNITY_STATE, auraType, apply);
        // Kick, Pummel, Shield Bash and Spell Lock carry MECHANIC_INTERRUPT on
        // the effect; this also catches an interrupt that does not.
        target->ApplySpellImmune(SPELL_SUCKY_FORM, IMMUNITY_EFFECT, SPELL_EFFECT_INTERRUPT_CAST, apply);

        if (!apply)
            return;

        // Immunity only refuses what lands from now on; clear what is already up.
        target->RemoveAurasWithMechanic(mechanicMask, AURA_REMOVE_BY_DEFAULT, SPELL_SUCKY_FORM, true);
        for (AuraType auraType : SuckyImmuneAuraTypes)
            target->RemoveAurasByType(auraType, ObjectGuid::Empty, nullptr, true, false);
    }

    // Health Funnel, one row per rank: the stock rank (cast at the pet outside
    // the form, never held - WrappedSpells.h), the wrapper the warlock holds,
    // and the in-form copy aimed at a friendly target. Ranks 8-9 (27259/47856)
    // have no wrapper: no trainer here sells them and nobody holds them.
    struct HealthFunnelRank
    {
        uint32 stock;
        uint32 wrapper;
        uint32 sucky;
    };

    constexpr std::array<HealthFunnelRank, 7> HealthFunnelRanks =
    {{
        {   755, 93000, 93007 },   // Rank 1
        {  3698, 93001, 93008 },   // Rank 2
        {  3699, 93002, 93009 },   // Rank 3
        {  3700, 93003, 93010 },   // Rank 4
        { 11693, 93004, 93011 },   // Rank 5
        { 11694, 93005, 93012 },   // Rank 6
        { 11695, 93006, 93013 },   // Rank 7
    }};

    HealthFunnelRank const* FindHealthFunnelRankByWrapper(uint32 wrapperId)
    {
        for (HealthFunnelRank const& rank : HealthFunnelRanks)
            if (rank.wrapper == wrapperId)
                return &rank;
        return nullptr;
    }

    // Every other hostile within the spell's own (modified) range of the
    // caster joins the cast as an extra target of the SAME spell, for the
    // effects that land on the chosen enemy. Called from OnCast: after target
    // selection, before anything is hit.
    void AddSuckySplashTargets(SpellScript* script, Spell* spell)
    {
        Unit* caster = script->GetCaster();
        Unit* primary = script->GetExplTargetUnit();
        SpellInfo const* spellInfo = script->GetSpellInfo();
        if (!caster || !primary || !spellInfo)
            return;

        uint32 effectMask = 0;
        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            if (spellEffectInfo.IsEffect() && spellEffectInfo.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY)
                effectMask |= 1 << spellEffectInfo.EffectIndex;
        if (!effectMask)
            return;

        float const range = spellInfo->GetMaxRange(false, caster, spell);
        if (range <= 0.0f)
            return;

        std::vector<Unit*> nearby;
        Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(caster, caster, range);
        Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(caster, nearby, check);
        Cell::VisitAllObjects(caster, searcher, range);

        for (Unit* extra : nearby)
        {
            if (extra == primary || !extra->IsAlive())
                continue;
            if (!caster->IsValidAttackTarget(extra, spellInfo))
                continue;

            // AddUnitTarget runs the usual target, line-of-sight, immunity and
            // hit/resist checks, so an extra can still miss or be immune.
            script->AddUnitTarget(extra, effectMask);
        }
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
        ApplySuckyImmunities(target, true);
    }

    void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_SUCKY_FORM_STATS);
        target->RemoveAurasDueToSpell(SPELL_SUCKY_FORM_HUNGER);
        ApplySuckyImmunities(target, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_sucky_demon_form::OnApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_sucky_demon_form::OnRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Drain Life and Drain Mana, every rank (spell_script_names), gated on the
// form so a warlock not wearing the set is completely unaffected. The extra
// victims are extra targets of the one channel: each gets its own drain aura,
// everything drained returns to the caster, and it all ends with the channel.
class spell_sucky_demon_drain : public SpellScript
{
    PrepareSpellScript(spell_sucky_demon_drain);

    void HandleSplash()
    {
        if (InSuckyDemonForm(GetCaster()))
            AddSuckySplashTargets(this, GetSpell());
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_sucky_demon_drain::HandleSplash);
    }
};

// Death Coil, every rank. Same splash: every enemy within Death Coil's range
// of the warlock takes the coil and its horror, and each hit heals the caster
// through the coil's own HEALTH_LEECH.
class spell_sucky_demon_death_coil : public SpellScript
{
    PrepareSpellScript(spell_sucky_demon_death_coil);

    void HandleSplash()
    {
        if (InSuckyDemonForm(GetCaster()))
            AddSuckySplashTargets(this, GetSpell());
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_sucky_demon_death_coil::HandleSplash);
    }
};

// 93000-93006 - the Health Funnel wrappers. An instant self-targeted DUMMY
// with the rank's level and GCD, no cost, no range, and form 22 in its
// Stances. It picks the target and the inner spell, then casts it:
//   outside the form - the stock rank at the pet, as before;
//   in the form      - the 93007+ copy at the selected friendly unit, or at
//                      the pet when nothing friendly is selected.
// The wrapper is self-targeted so the client never range-checks a
// selection the stock spell would have ignored; the range and line of sight
// are checked here against the unit the funnel will actually reach.
//
// The inner spell takes the CHANNELED slot from inside the wrapper's own
// cast. Unit::SetCurrentCastSpell cannot cancel the wrapper there (it is
// m_executedCurrently). It is cast with TRIGGERED_IGNORE_GCD because the
// wrapper already started the GCD, and it pays its own health cost.
class spell_sucky_demon_health_funnel : public SpellScript
{
    PrepareSpellScript(spell_sucky_demon_health_funnel);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        std::vector<uint32> ids;
        for (HealthFunnelRank const& rank : HealthFunnelRanks)
        {
            ids.push_back(rank.stock);
            ids.push_back(rank.wrapper);
            ids.push_back(rank.sucky);
        }
        return ValidateSpellInfo(ids);
    }

    bool Load() override
    {
        _rank = FindHealthFunnelRankByWrapper(GetSpellInfo()->Id);
        return _rank != nullptr;
    }

    SpellCastResult CheckTarget()
    {
        Unit* caster = GetCaster();
        bool const sucky = InSuckyDemonForm(caster);
        _innerId = sucky ? _rank->sucky : _rank->stock;
        _target.Clear();

        Unit* target = nullptr;
        if (sucky)
            if (Player* player = caster->ToPlayer())
                if (Unit* selected = player->GetSelectedUnit())
                    if (selected != caster && selected->IsAlive() && caster->IsValidAssistTarget(selected))
                        target = selected;

        if (!target)
        {
            Guardian* pet = caster->GetGuardianPet();
            if (!pet)
                return SPELL_FAILED_NO_PET;
            if (!pet->IsAlive())
                return SPELL_FAILED_TARGETS_DEAD;
            target = pet;
        }

        SpellInfo const* inner = sSpellMgr->AssertSpellInfo(_innerId);
        if (!caster->IsWithinCombatRange(target, inner->GetMaxRange(true, caster)))
            return SPELL_FAILED_OUT_OF_RANGE;
        if (!caster->IsWithinLOSInMap(target))
            return SPELL_FAILED_LINE_OF_SIGHT;

        _target = target->GetGUID();
        return SPELL_CAST_OK;
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Unit* target = ObjectAccessor::GetUnit(*caster, _target);
        if (!target)
            return;

        CastSpellExtraArgs args(TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_SHAPESHIFT));
        SpellCastResult const result = caster->CastSpell(target, _innerId, args);
        // A triggered cast reports nothing itself; say why (low health, etc).
        if (result != SPELL_CAST_OK)
            if (Player* player = caster->ToPlayer())
                Spell::SendCastResult(player, GetSpellInfo(), 0, result);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_sucky_demon_health_funnel::CheckTarget);
        AfterCast += SpellCastFn(spell_sucky_demon_health_funnel::HandleAfterCast);
    }

    HealthFunnelRank const* _rank = nullptr;
    uint32 _innerId = 0;
    ObjectGuid _target;
};

// 93007-93013 - the in-form funnel. Effect 0 is the stock rank's periodic
// heal, paid for in health by the engine (SPELL_ATTR2_HEALTH_FUNNEL). Effect
// 2 is a PERIODIC_ENERGIZE of mana that no engine path charges for, so each
// tick moves the mana out of the caster here: never more than the caster has
// or the target can take.
class spell_sucky_demon_funnel_mana : public AuraScript
{
    PrepareAuraScript(spell_sucky_demon_funnel_mana);

    void HandleManaTick(AuraEffect const* aurEff)
    {
        PreventDefaultAction();

        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !caster->IsAlive() || caster == target)
            return;
        if (target->GetPowerType() != POWER_MANA || caster->GetPowerType() != POWER_MANA)
            return;

        int32 amount = std::max(aurEff->GetAmount(), 0);
        amount = std::min<int32>(amount, caster->GetPower(POWER_MANA));
        amount = std::min<int32>(amount, target->GetMaxPower(POWER_MANA) - target->GetPower(POWER_MANA));
        if (amount <= 0)
            return;

        caster->ModifyPower(POWER_MANA, -amount);
        caster->EnergizeBySpell(target, GetSpellInfo(), amount, POWER_MANA);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_sucky_demon_funnel_mana::HandleManaTick, EFFECT_2, SPELL_AURA_PERIODIC_ENERGIZE);
    }
};

void AddSC_custom_sucky_demon()
{
    RegisterSpellScript(spell_sucky_demon_form);
    // The next two are bound to every rank of the core spells through
    // spell_script_names, so they attach to Drain Life / Drain Mana / Death
    // Coil themselves rather than to one of ours.
    RegisterSpellScript(spell_sucky_demon_drain);
    RegisterSpellScript(spell_sucky_demon_death_coil);
    RegisterSpellScript(spell_sucky_demon_health_funnel);
    RegisterSpellScript(spell_sucky_demon_funnel_mana);
}
