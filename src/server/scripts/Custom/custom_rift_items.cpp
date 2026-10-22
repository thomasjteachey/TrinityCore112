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

// Summoner's Rift's League items: their passives and actives. The spells are
// Centurion custom rows 94121-94141 (import/rift/scripts/rift_item_spells.py),
// worn as the shop items' equip spells (rift_shop.py); this is what they do.
//
//   94121 / 94122  Spellblade (Sheen / Trinity Force): an ability charges the next
//                  basic attack (94123) with 100% / 150% of League's base attack
//                  damage (RiftProgression::BaseAttackDamage); 2 sec cooldown
//   94124  Thornmail: basic attackers take 30% of what they dealt, as magic
//   (a basic attack is a melee swing or a ranged one: Auto Shot, Shoot, Throw)
//   94126  Immolate (Sunfire Cape): League's 40 magic a second, 10 yards round
//   94127  Warmog's Heart: 1% of maximum health every 5 sec
//   94128  Eternity (Rod of Ages): a stack of 94129 a minute, up to 10
//   94130  Guardian Angel: lethal damage is held off, 4 sec of stasis (94131),
//          back with 30% health and mana; 5 min cooldown
//   94134  Spell Shield (Banshee's Veil): keeps 94135 up, which eats the next
//          damaging enemy spell; 45 sec to come back
//   94136  Rage (Phage / Trinity Force): DBC proc, nothing here
//   94133  Stasis (Zhonya's Hourglass), 94138-94140: DBC auras, nothing here
//   94141  Valor's Reward: RiftProgression::OnLevelChanged

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RiftProgression.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <cmath>

namespace
{
    uint32 constexpr SPELL_SPELLBLADE_SHEEN   = 94121;
    uint32 constexpr SPELL_SPELLBLADE_TRINITY = 94122;
    uint32 constexpr SPELL_SPELLBLADE_CHARGED = 94123;
    uint32 constexpr SPELL_THORNMAIL          = 94124;
    uint32 constexpr SPELL_IMMOLATE           = 94126;
    uint32 constexpr SPELL_ROD_OF_AGES_STACK  = 94129;
    uint32 constexpr SPELL_GUARDIAN_ANGEL     = 94130;
    uint32 constexpr SPELL_GUARDIAN_STASIS    = 94131;
    uint32 constexpr SPELL_SPELL_SHIELD       = 94135;
    uint32 constexpr RIFT_ITEM_SPELL_FIRST    = 94120;
    uint32 constexpr RIFT_ITEM_SPELL_LAST     = 94141;

    float constexpr IMMOLATE_RANGE = 10.0f;
    Milliseconds constexpr SPELLBLADE_COOLDOWN = 2000ms;
    float constexpr IMMOLATE_DAMAGE = 40.0f;           // League's, per second
    Seconds constexpr SPELL_SHIELD_RECHARGE = 45s;
    Minutes constexpr GUARDIAN_ANGEL_COOLDOWN = 5min;

    // Damage from an item, named in the combat log by the item's spell.
    void DealItemDamage(Unit* source, Unit* victim, uint32 spellId, uint32 damage)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !damage || !victim->IsAlive() || !source->IsValidAttackTarget(victim))
            return;
        SpellNonMeleeDamage log(source, victim, spellId, spellInfo->GetSchoolMask());
        source->CalculateSpellDamageTaken(&log, int32(damage), spellInfo);
        Unit::DealDamageMods(log.target, log.damage, &log.absorb);
        source->SendSpellNonMeleeDamageLog(&log);
        source->DealSpellDamage(&log, false);
    }

    float WeaponAverage(Unit* unit, WeaponAttackType attackType)
    {
        return (unit->GetWeaponDamageRange(attackType, MINDAMAGE) + unit->GetWeaponDamageRange(attackType, MAXDAMAGE)) / 2.0f;
    }

    // A basic attack made at range: the repeating shot (Auto Shot, a wand's
    // Shoot), or a warrior's or rogue's Shoot and Throw - single shots since 3.0
    // (Throw is even filed as a rogue spell): free, through the ranged slot, the
    // weapon's damage and nothing else. No hunter shot is all three.
    bool IsRangedBasicAttack(SpellInfo const* info)
    {
        if (info->IsAutoRepeatRangedSpell())
            return true;
        if (!info->HasAttribute(SPELL_ATTR0_REQ_AMMO) || info->ManaCost || info->ManaCostPercentage)
            return false;
        bool weapon = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 const effect = info->GetEffect(SpellEffIndex(i)).Effect;
            if (effect == SPELL_EFFECT_WEAPON_DAMAGE || effect == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL)
                weapon = true;
            else if (effect)
                return false;
        }
        return weapon;
    }
}

// Spellblade's charge, and its spending; Thornmail's reflection - on every basic
// attack, as League counts them: a melee swing, or a ranged one (Auto Shot, a
// wand's Shoot, Throw), which is a spell and comes the spell-damage way.
class custom_rift_items_unit : public UnitScript
{
public:
    custom_rift_items_unit() : UnitScript("custom_rift_items_unit") { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        BasicAttack(target, attacker, damage, BASE_ATTACK);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!spellInfo || damage <= 0 || !IsRangedBasicAttack(spellInfo))
            return;
        uint32 dealt = uint32(damage);
        BasicAttack(target, attacker, dealt, RANGED_ATTACK);
        damage = int32(dealt);
    }

private:
    static void BasicAttack(Unit* target, Unit* attacker, uint32& damage, WeaponAttackType attackType)
    {
        if (!attacker || !target || attacker == target)
            return;

        if (attacker->HasAura(SPELL_SPELLBLADE_CHARGED))
        {
            float const times = attacker->HasAura(SPELL_SPELLBLADE_TRINITY) ? 1.5f : 1.0f;
            Player const* champion = attacker->ToPlayer();
            damage += uint32(champion ? RiftProgression::BaseAttackDamage(champion) * times : WeaponAverage(attacker, attackType) * times);
            attacker->RemoveAurasDueToSpell(SPELL_SPELLBLADE_CHARGED);
            attacker->GetSpellHistory()->AddCooldown(SPELL_SPELLBLADE_CHARGED, 0, SPELLBLADE_COOLDOWN);
        }

        // Dealt a moment later: this runs inside the attacker's swing.
        if (damage && target->HasAura(SPELL_THORNMAIL))
        {
            uint32 const reflect = CalculatePct(damage, 30);
            ObjectGuid const attackerGuid = attacker->GetGUID();
            target->m_Events.AddEventAtOffset([target, attackerGuid, reflect]()
            {
                if (Unit* back = ObjectAccessor::GetUnit(*target, attackerGuid))
                    if (target->IsAlive())
                        DealItemDamage(target, back, SPELL_THORNMAIL, reflect);
            }, 1ms);
        }
    }
};

class custom_rift_items_player : public PlayerScript
{
public:
    custom_rift_items_player() : PlayerScript("custom_rift_items_player") { }

    // An ability - a class spell the player cast, not an item, a passive or a
    // ranged basic attack - charges Spellblade.
    void OnSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info || spell->IsTriggered() || info->IsPassive() || IsRangedBasicAttack(info)
            || info->SpellFamilyName == SPELLFAMILY_GENERIC
            || (info->Id >= RIFT_ITEM_SPELL_FIRST && info->Id <= RIFT_ITEM_SPELL_LAST))
            return;
        if (!player->HasAura(SPELL_SPELLBLADE_SHEEN) && !player->HasAura(SPELL_SPELLBLADE_TRINITY))
            return;
        if (player->GetSpellHistory()->HasCooldown(SPELL_SPELLBLADE_CHARGED))
            return;
        player->CastSpell(player, SPELL_SPELLBLADE_CHARGED, true);
    }
};

// 94126 Immolate
class spell_rift_item_immolate : public AuraScript
{
    PrepareAuraScript(spell_rift_item_immolate);

    void Tick(AuraEffect const* /*aurEff*/)
    {
        Unit* owner = GetTarget();
        if (!owner->IsAlive())
            return;
        std::list<Unit*> targets;
        Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(owner, owner, IMMOLATE_RANGE);
        Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(owner, targets, check);
        Cell::VisitAllObjects(owner, searcher, IMMOLATE_RANGE);
        Player const* champion = owner->ToPlayer();
        uint32 const damage = uint32(std::lround(IMMOLATE_DAMAGE * (champion ? RiftProgression::MatchScale(champion) : 1.0f)));
        for (Unit* unit : targets)
            DealItemDamage(owner, unit, SPELL_IMMOLATE, damage);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rift_item_immolate::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 94127 Warmog's Heart
class spell_rift_item_warmog : public AuraScript
{
    PrepareAuraScript(spell_rift_item_warmog);

    void Tick(AuraEffect const* /*aurEff*/)
    {
        Unit* owner = GetTarget();
        if (!owner->IsAlive() || owner->IsFullHealth())
            return;
        HealInfo heal(owner, owner, owner->CountPctFromMaxHealth(1), GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        Unit::DealHeal(heal);
        owner->SendHealSpellLog(heal);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rift_item_warmog::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 94128 Eternity
class spell_rift_item_rod_of_ages : public AuraScript
{
    PrepareAuraScript(spell_rift_item_rod_of_ages);

    void Tick(AuraEffect const* /*aurEff*/)
    {
        GetTarget()->CastSpell(GetTarget(), SPELL_ROD_OF_AGES_STACK, true);
    }

    void Remove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_ROD_OF_AGES_STACK);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rift_item_rod_of_ages::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_rift_item_rod_of_ages::Remove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 94130 Guardian Angel
class spell_rift_item_guardian_angel : public AuraScript
{
    PrepareAuraScript(spell_rift_item_guardian_angel);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1;
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* target = GetTarget();
        if (dmgInfo.GetDamage() < target->GetHealth() || target->GetSpellHistory()->HasCooldown(SPELL_GUARDIAN_ANGEL)
            || target->HasAura(SPELL_GUARDIAN_STASIS))
            return;
        absorbAmount = dmgInfo.GetDamage();
        target->GetSpellHistory()->AddCooldown(SPELL_GUARDIAN_ANGEL, 0, GUARDIAN_ANGEL_COOLDOWN);
        target->CastSpell(target, SPELL_GUARDIAN_STASIS, true);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_rift_item_guardian_angel::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_rift_item_guardian_angel::Absorb, EFFECT_0);
    }
};

// 94131 Guardian Angel's stasis: back with 30% of health and mana.
class spell_rift_item_guardian_stasis : public AuraScript
{
    PrepareAuraScript(spell_rift_item_guardian_stasis);

    void Remove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target->IsAlive())
            return;
        uint32 const health = target->CountPctFromMaxHealth(30);
        if (target->GetHealth() < health)
            target->SetHealth(health);
        if (target->GetMaxPower(POWER_MANA) > 0)
        {
            int32 const mana = int32(CalculatePct(target->GetMaxPower(POWER_MANA), 30));
            if (target->GetPower(POWER_MANA) < mana)
                target->SetPower(POWER_MANA, mana);
        }
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_rift_item_guardian_stasis::Remove, EFFECT_0, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL);
    }
};

// 94134 Spell Shield (the item): keeps the shield up while it is charged.
class spell_rift_item_banshee : public AuraScript
{
    PrepareAuraScript(spell_rift_item_banshee);

    void Tick(AuraEffect const* /*aurEff*/)
    {
        Unit* owner = GetTarget();
        if (owner->IsAlive() && !owner->HasAura(SPELL_SPELL_SHIELD) && !owner->GetSpellHistory()->HasCooldown(SPELL_SPELL_SHIELD))
            owner->AddAura(SPELL_SPELL_SHIELD, owner);
    }

    void Remove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_SPELL_SHIELD);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rift_item_banshee::Tick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_rift_item_banshee::Remove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 94135 Spell Shield (the buff): eats one damaging spell from an enemy player
// or their pet, then is gone for 45 sec.
class spell_rift_item_spell_shield : public AuraScript
{
    PrepareAuraScript(spell_rift_item_spell_shield);

    bool _spent = false;

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = -1;
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* target = GetTarget();
        Unit* attacker = dmgInfo.GetAttacker();
        if (_spent || !attacker || attacker == target || !dmgInfo.GetSpellInfo() || !attacker->GetCharmerOrOwnerPlayerOrPlayerItself())
            return;
        _spent = true;
        absorbAmount = dmgInfo.GetDamage();
        target->GetSpellHistory()->AddCooldown(SPELL_SPELL_SHIELD, 0, SPELL_SHIELD_RECHARGE);
        // Not from inside the absorb pass.
        target->m_Events.AddEventAtOffset([target]() { target->RemoveAurasDueToSpell(SPELL_SPELL_SHIELD); }, 1ms);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_rift_item_spell_shield::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_rift_item_spell_shield::Absorb, EFFECT_0);
    }
};

void AddSC_custom_rift_items()
{
    new custom_rift_items_unit();
    new custom_rift_items_player();
    RegisterSpellScript(spell_rift_item_immolate);
    RegisterSpellScript(spell_rift_item_warmog);
    RegisterSpellScript(spell_rift_item_rod_of_ages);
    RegisterSpellScript(spell_rift_item_guardian_angel);
    RegisterSpellScript(spell_rift_item_guardian_stasis);
    RegisterSpellScript(spell_rift_item_banshee);
    RegisterSpellScript(spell_rift_item_spell_shield);
}
