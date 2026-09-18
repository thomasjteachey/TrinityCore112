/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Damage and radius corrections for the vanilla (40-player) Naxxramas. Every
 * script here shares its spell with the Wrath versions, so each one returns
 * untouched unless the caster is standing in the 40-player wing - which also
 * means the whole file goes quiet when VanillaRaids.Enable is off.
 *
 * Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0);
 * Naxxramas 40 scripts by Sogladev.
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
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "VanillaRaids/VanillaRaids.h"

// 28785 - Locust Swarm: ~1500 damage down to ~1000, 25 yard radius up to 30.
enum LocustSwarm40
{
    SPELL_LOCUST_SWARM_TRIGGER      = 28786
};

class spell_anub_locust_swarm_aura_40 : public AuraScript
{
    PrepareAuraScript(spell_anub_locust_swarm_aura_40);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_LOCUST_SWARM_TRIGGER });
    }

    void HandleTriggerSpell(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        if (!VanillaRaids::IsNaxx40(caster))
            return;

        PreventDefaultAction();
        caster->CastSpell(caster, SPELL_LOCUST_SWARM_TRIGGER, CastSpellExtraArgs(TRIGGERED_FULL_MASK)
            .SetOriginalCaster(GetCasterGUID())
            .AddSpellBP0(812)
            .AddSpellMod(SPELLVALUE_RADIUS_MOD, 3000)); // 30 yards
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_anub_locust_swarm_aura_40::HandleTriggerSpell, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 28241 - Poison Cloud: ~3000 down to ~1200.
class spell_grobbulus_poison_cloud_poison_damage_40 : public SpellScript
{
    PrepareSpellScript(spell_grobbulus_poison_cloud_poison_damage_40);

    void HandleDamageCalc(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetEffectValue(int32(urand(1110, 1290)));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_grobbulus_poison_cloud_poison_damage_40::HandleDamageCalc, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 29350 - Plague Cloud
enum PlagueCloud40
{
    SPELL_PLAGUE_CLOUD_TRIGGER      = 30122
};

class spell_heigan_plague_cloud_aura_40 : public AuraScript
{
    PrepareAuraScript(spell_heigan_plague_cloud_aura_40);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PLAGUE_CLOUD_TRIGGER });
    }

    void HandleTriggerSpell(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        if (!VanillaRaids::IsNaxx40(caster))
            return;

        PreventDefaultAction();
        caster->CastSpell(caster, SPELL_PLAGUE_CLOUD_TRIGGER, CastSpellExtraArgs(true).AddSpellBP0(4000));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_heigan_plague_cloud_aura_40::HandleTriggerSpell, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 29371 - Eruption (the dance)
class spell_heigan_eruption_40 : public SpellScript
{
    PrepareSpellScript(spell_heigan_eruption_40);

    void HandleDamageCalc(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetEffectValue(int32(urand(3500, 4500)));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_heigan_eruption_40::HandleDamageCalc, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 28819 - Submerge Visual (Heigan's eye stalks)
class spell_naxx40_submerge_visual_aura : public AuraScript
{
    PrepareAuraScript(spell_naxx40_submerge_visual_aura);

    void OnApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (VanillaRaids::IsNaxx40(GetTarget()))
            GetTarget()->SetStandState(UNIT_STAND_STATE_SUBMERGED);
    }

    void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (VanillaRaids::IsNaxx40(GetTarget()))
            GetTarget()->SetStandState(UNIT_STAND_STATE_STAND);
    }

    void Register() override
    {
        OnEffectApply += AuraEffectApplyFn(spell_naxx40_submerge_visual_aura::OnApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectRemove += AuraEffectRemoveFn(spell_naxx40_submerge_visual_aura::OnRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 28457 - Dark Blast (Soul Weaver)
class spell_kelthuzad_dark_blast_40 : public SpellScript
{
    PrepareSpellScript(spell_kelthuzad_dark_blast_40);

    void CalculateDamage(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetEffectValue(int32(urand(1750, 2250)));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_kelthuzad_dark_blast_40::CalculateDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 28479 - Frostbolt Volley
class spell_kelthuzad_frostbolt_40 : public SpellScript
{
    PrepareSpellScript(spell_kelthuzad_frostbolt_40);

    void CalculateDamage(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetEffectValue(int32(urand(2550, 3450)));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_kelthuzad_frostbolt_40::CalculateDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 28547 - Chill (Sapphiron's blizzard)
class spell_sapphiron_chill_40 : public AuraScript
{
    PrepareAuraScript(spell_sapphiron_chill_40);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        amount = int32(urand(3063, 3937));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_sapphiron_chill_40::CalculateAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// 28522 - Icebolt
class spell_sapphiron_icebolt_40 : public SpellScript
{
    PrepareSpellScript(spell_sapphiron_icebolt_40);

    void CalculateDamage(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetEffectValue(int32(urand(2625, 3375)));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_sapphiron_icebolt_40::CalculateDamage, EFFECT_1, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 28531 - Frost Aura
class spell_sapphiron_frost_aura_40 : public AuraScript
{
    PrepareAuraScript(spell_sapphiron_frost_aura_40);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        if (urand(0, 99) == 0) // 1% chance of a full-strength tick
            return;

        amount /= 2; // 1200 base points down to 600
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_sapphiron_frost_aura_40::CalculateAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// 60960 - War Stomp (Patchwork Golem)
class spell_patchwork_golem_war_stomp_40 : public SpellScript
{
    PrepareSpellScript(spell_patchwork_golem_war_stomp_40);

    void CalculateDamage(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetHitDamage(int32(urand(936, 1064)));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_patchwork_golem_war_stomp_40::CalculateDamage, EFFECT_2, SPELL_EFFECT_WEAPON_DAMAGE);
    }
};

// 29213 - Curse of the Plaguebringer
enum CurseOfThePlaguebringer40
{
    SPELL_REVENGE_OF_THE_PLAGUEBRINGER = 29214
};

class spell_noth_curse_of_the_plaguebringer_aura_40 : public AuraScript
{
    PrepareAuraScript(spell_noth_curse_of_the_plaguebringer_aura_40);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_REVENGE_OF_THE_PLAGUEBRINGER });
    }

    void HandleTriggerSpell(AuraEffect const* /*aurEff*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        PreventDefaultAction();
        GetTarget()->CastSpell(GetTarget(), SPELL_REVENGE_OF_THE_PLAGUEBRINGER, CastSpellExtraArgs(TRIGGERED_NONE)
            .SetOriginalCaster(GetCasterGUID())
            .AddSpellBP0(1757)  // instant
            .AddSpellBP1(874)   // periodic
            .AddSpellMod(SPELLVALUE_RADIUS_MOD, 3500)); // 35 yards
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_noth_curse_of_the_plaguebringer_aura_40::HandleTriggerSpell, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 26046 - Mana Burn, Razuvious's vanilla stand-in for Disrupting Shout
class spell_razuvious_disrupting_shout_40 : public SpellScript
{
    PrepareSpellScript(spell_razuvious_disrupting_shout_40);

    void CalculateDamage(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetEffectValue(int32(urand(4050, 4950)));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_razuvious_disrupting_shout_40::CalculateDamage, EFFECT_0, SPELL_EFFECT_POWER_BURN);
    }
};

// 28450 - Unholy Staff (Gothik's dead-side riders)
class spell_unholy_staff_arcane_explosion_40 : public SpellScript
{
    PrepareSpellScript(spell_unholy_staff_arcane_explosion_40);

    void PreventLaunchHit(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        if (!VanillaRaids::IsNaxx40(caster))
            return;

        if (Unit* target = GetHitUnit())
        {
            if (target->IsWithinDist2d(caster, 20.0f))
                SetEffectValue(int32(urand(1838, 2361)));
            else
                PreventHitDefaultEffect(effIndex);
        }
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_unholy_staff_arcane_explosion_40::PreventLaunchHit, EFFECT_1, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 28153 - Disease Cloud (Sewage Slime)
class spell_disease_cloud_damage_40 : public SpellScript
{
    PrepareSpellScript(spell_disease_cloud_damage_40);

    void HandleDamageCalc(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        SetEffectValue(int32(urand(278, 322)));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_disease_cloud_damage_40::HandleDamageCalc, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 28135 - Static Field: burns a flat 500 mana and deals what it drained.
class spell_feugen_static_field_40 : public SpellScript
{
    PrepareSpellScript(spell_feugen_static_field_40);

    void HandleDamageCalc(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsNaxx40(GetCaster()))
            return;

        if (Unit* target = GetHitUnit())
            SetEffectValue(-target->ModifyPower(POWER_MANA, -500));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_feugen_static_field_40::HandleDamageCalc, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 29201 - Corrupted Mind: silences a healer's own school rather than the
// Wrath-era Necrotic Aura.
class spell_loatheb_corrupted_mind_40 : public SpellScript
{
    PrepareSpellScript(spell_loatheb_corrupted_mind_40);

    void HandleEffect(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!VanillaRaids::IsNaxx40(caster))
            return;

        Player* playerTarget = GetHitUnit() ? GetHitUnit()->ToPlayer() : nullptr;
        if (!playerTarget)
            return;

        uint32 spellId = 0;
        switch (playerTarget->GetClass())
        {
            case CLASS_PRIEST:
            case CLASS_DRUID:
                // Priests should get 29185, but it fires on damage effects too.
                spellId = 29194;
                break;
            case CLASS_PALADIN:
                spellId = 29196;
                break;
            case CLASS_SHAMAN:
                spellId = 29198;
                break;
            default:
                return; // nothing to silence on a non-healing class
        }

        caster->CastSpell(playerTarget, spellId, CastSpellExtraArgs(TRIGGERED_FULL_MASK));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_loatheb_corrupted_mind_40::HandleEffect, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Attached to the healing spells Corrupted Mind is meant to lock out: the
// override class script alone does not stop the cast, so it is refused here.
class spell_naxx40_corrupted_mind_check : public SpellScript
{
    PrepareSpellScript(spell_naxx40_corrupted_mind_check);

    SpellCastResult CheckCorruptedMind()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || !VanillaRaids::IsNaxx40(player))
            return SPELL_CAST_OK;

        Unit::AuraEffectList const& classScripts = player->GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
        for (AuraEffect const* auraEff : classScripts)
        {
            if (!auraEff)
                continue;

            SpellInfo const* auraInfo = auraEff->GetSpellInfo();
            if (auraInfo && auraInfo->GetEffect(EFFECT_0).MiscValue == 4327)
                return SPELL_FAILED_FIZZLE;
        }

        return SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_naxx40_corrupted_mind_check::CheckCorruptedMind);
    }
};

void AddSC_naxxramas40_spells()
{
    RegisterSpellScript(spell_anub_locust_swarm_aura_40);
    RegisterSpellScript(spell_grobbulus_poison_cloud_poison_damage_40);
    RegisterSpellScript(spell_heigan_plague_cloud_aura_40);
    RegisterSpellScript(spell_heigan_eruption_40);
    RegisterSpellScript(spell_naxx40_submerge_visual_aura);
    RegisterSpellScript(spell_kelthuzad_dark_blast_40);
    RegisterSpellScript(spell_kelthuzad_frostbolt_40);
    RegisterSpellScript(spell_sapphiron_icebolt_40);
    RegisterSpellScript(spell_sapphiron_frost_aura_40);
    RegisterSpellScript(spell_sapphiron_chill_40);
    RegisterSpellScript(spell_patchwork_golem_war_stomp_40);
    RegisterSpellScript(spell_noth_curse_of_the_plaguebringer_aura_40);
    RegisterSpellScript(spell_razuvious_disrupting_shout_40);
    RegisterSpellScript(spell_unholy_staff_arcane_explosion_40);
    RegisterSpellScript(spell_disease_cloud_damage_40);
    RegisterSpellScript(spell_feugen_static_field_40);
    RegisterSpellScript(spell_loatheb_corrupted_mind_40);
    RegisterSpellScript(spell_naxx40_corrupted_mind_check);
}
