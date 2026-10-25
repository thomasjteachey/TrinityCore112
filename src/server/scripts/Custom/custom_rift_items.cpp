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
//   94136  Icy (Phage / Trinity Force, League S3): basic attacks roll the 94166
//          slow (custom_rift_items_unit::BasicAttack)
//   94134  Spell Shield (Banshee's Veil): keeps 94135 up, which eats the next
//          damaging enemy spell; 45 sec to come back
//   94136  Rage (Phage / Trinity Force): DBC proc, nothing here
//   94133  Stasis (Zhonya's Hourglass), 94138-94140: DBC auras, nothing here
//   94141  Valor's Reward: RiftProgression::OnLevelChanged
//   94170  the champion's cooldown reduction - the items', the Crest of the Ancient
//          Golem's, Mejai's at 20 stacks - one modifier RiftProgression::CooldownMod
//          builds from the carrier's amount
//   94153  Summoner's Focus (the match aura): its third effect is the match's
//          shorter ranges, one modifier RiftProgression::RangeMod builds
//
// League's UNIQUE item effects (world `rift_league_item_unique`, summed by
// RiftProgression, read here through ItemHooksOf) on basic attacks and damage:
//   Ninja Tabi        a share of every basic attack's damage blocked
//   Doran's Shield    League damage blocked off each champion's basic attack
//   Doran's Blade     League health back on each basic attack
//   Maim              League magic damage (94171) on each basic attack at a monster
//   Butcher           a share more damage to monsters, from anything the champion does
//   Cold Steel        a basic attacker slowed (94172): attack speed, Randuin's run speed too
//   Spectre's Cowl    League health regeneration (94173) for 10 sec after an enemy
//                     champion's damage (Spirit Visage's as well)

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
    uint32 constexpr SPELL_BANSHEE            = 94134;
    uint32 constexpr SPELL_BANSHEE_REGEN      = 94167;
    uint32 constexpr SPELL_ICY_SLOW           = 94166;
    uint32 constexpr ITEM_PHAGE               = 991021;
    uint32 constexpr ITEM_TRINITY_FORCE       = 991035;
    uint32 constexpr RIFT_ITEM_SPELL_FIRST    = 94120;
    uint32 constexpr RIFT_ITEM_SPELL_LAST     = 94141;
    uint32 constexpr SPELL_MAIM               = 94171;
    uint32 constexpr SPELL_COLD_STEEL         = 94172;
    uint32 constexpr SPELL_SPECTRE_REGEN      = 94173;
    // The jungle's monsters (BattlegroundSRF.h: Blue Sentinel 922130 .. Baron Nashor 922141).
    uint32 constexpr RIFT_MONSTER_FIRST       = 922130;
    uint32 constexpr RIFT_MONSTER_LAST        = 922141;
    // Spectre's Cowl is put on again only once its 10 sec have run down to this:
    // a champion's damage comes many times a second.
    int32 constexpr SPECTRE_REFRESH_BELOW     = 9000;

    float constexpr IMMOLATE_RANGE = 10.0f;
    Milliseconds constexpr SPELLBLADE_COOLDOWN = 2000ms;
    float constexpr IMMOLATE_DAMAGE = 40.0f;           // League's, per second
    // Banshee's Veil, League S3 (mid-2013 wiki): the shield returns after 25 sec
    // with no damage from an enemy champion, and that damage gives 45 health
    // regeneration (League's, per 5 sec) for 10 sec.
    Seconds constexpr SPELL_SHIELD_RECHARGE = 25s;
    float constexpr BANSHEE_REGEN_PER_5 = 45.0f;
    // Icy, League S3: a 25% chance a basic attack slows by 30% for 2 sec, 20% at
    // range. The slow does not stack; each Phage or Trinity Force held rolls its own
    // chance (two: 44%).
    float constexpr ICY_MISS_CHANCE = 0.75f;
    int32 constexpr ICY_SLOW = 30;
    int32 constexpr ICY_SLOW_RANGED = 20;
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

    // League's true damage: no armour, resistance or crit - shields still take it.
    void DealTrueDamage(Unit* source, Unit* victim, SpellInfo const* spellInfo, uint32 damage)
    {
        if (!damage || !victim->IsAlive() || !source->IsValidAttackTarget(victim))
            return;
        DamageInfo info(source, victim, damage, spellInfo, SPELL_SCHOOL_MASK_NORMAL, DOT, BASE_ATTACK);
        Unit::CalcAbsorbResist(info);
        SpellNonMeleeDamage log(source, victim, spellInfo->Id, SPELL_SCHOOL_MASK_NORMAL);
        log.damage = info.GetDamage();
        log.absorb = info.GetAbsorb();
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

    bool IsMonster(Unit const* unit)
    {
        return unit && unit->GetTypeId() == TYPEID_UNIT && unit->GetEntry() >= RIFT_MONSTER_FIRST && unit->GetEntry() <= RIFT_MONSTER_LAST;
    }

    // Butcher: the champion's damage to a monster, a share more.
    template <typename T>
    void Butcher(Unit* attacker, Unit* target, T& damage)
    {
        if (!(damage > 0) || !IsMonster(target))
            return;
        Player const* champion = attacker ? attacker->ToPlayer() : nullptr;
        if (!champion)
            return;
        RiftProgression::ItemHooks const hooks = RiftProgression::ItemHooksOf(champion);
        if (hooks.Butcher > 0.0f)
            damage = T(std::lround(float(damage) * (1.0f + hooks.Butcher / 100.0f)));
    }

    // A champion healing itself from an item, League's way: Spirit Visage adds its
    // share, healing cuts (Mortal Strike's, as Grievous Wounds) take theirs.
    void HealChampion(Player* player, float heal, float healAmp)
    {
        if (!player->IsAlive())
            return;
        heal *= 1.0f + healAmp / 100.0f;
        if (int32 const cut = player->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_HEALING_PCT))
            AddPct(heal, cut);
        if (heal < 1.0f)
            return;
        HealInfo info(player, player, uint32(heal), nullptr, SPELL_SCHOOL_MASK_NORMAL);
        Unit::DealHeal(info);
    }

    // Cold Steel on whoever struck the holder with a basic attack: one at a time on
    // an attacker, whoever's (a UNIQUE); the holder's own is put on afresh.
    void ColdSteel(Player* holder, Unit* attacker, RiftProgression::ItemHooks const& hooks)
    {
        if (Aura const* other = attacker->GetAura(SPELL_COLD_STEEL); other && other->GetCasterGUID() != holder->GetGUID())
            return;
        int32 const attackSpeed = int32(std::lround(hooks.ColdSteelAs));
        int32 const moveSpeed = int32(std::lround(hooks.ColdSteelMs));
        ObjectGuid const attackerGuid = attacker->GetGUID();
        holder->m_Events.AddEventAtOffset([holder, attackerGuid, attackSpeed, moveSpeed]()
        {
            Unit* back = ObjectAccessor::GetUnit(*holder, attackerGuid);
            if (!back || !back->IsAlive() || !holder->IsAlive())
                return;
            CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
            args.AddSpellMod(SPELLVALUE_BASE_POINT0, -attackSpeed - 1);   // the row's die adds 1
            args.AddSpellMod(SPELLVALUE_BASE_POINT1, -attackSpeed - 1);
            args.AddSpellMod(SPELLVALUE_BASE_POINT2, -moveSpeed - 1);
            holder->CastSpell(back, SPELL_COLD_STEEL, args);
        }, 1ms);
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

    // Damage from an enemy champion (or its pet): Banshee's Veil holds the spell
    // shield back for another 25 sec and starts its regeneration; Spectre's Cowl
    // (and Spirit Visage) start theirs.
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!damage || !attacker || !victim || attacker == victim || victim->GetTypeId() != TYPEID_PLAYER)
            return;
        Player* champion = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!champion || champion == victim || !champion->IsHostileTo(victim))
            return;
        SpectreRegen(victim->ToPlayer());
        if (!victim->HasAura(SPELL_BANSHEE))
            return;
        if (!victim->HasAura(SPELL_SPELL_SHIELD))
            victim->GetSpellHistory()->AddCooldown(SPELL_SPELL_SHIELD, 0, SPELL_SHIELD_RECHARGE);
        float const scale = RiftProgression::MatchScale(victim->ToPlayer());
        // League's health per 5 sec, as the champion carriers convert it: aura 161
        // adds its amount at every 2 sec tick (RiftProgression::SetChampionCarriers)
        int32 const regen = std::max(1, int32(std::lround(0.4f * (scale > 0.0f ? scale : 1.0f) * BANSHEE_REGEN_PER_5)));
        ObjectGuid const guid = victim->GetGUID();
        victim->m_Events.AddEventAtOffset([victim, guid, regen]()
        {
            if (!victim->IsAlive() || victim->GetGUID() != guid)
                return;
            CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
            args.AddSpellMod(SPELLVALUE_BASE_POINT0, regen - 1);   // the row's die adds 1
            victim->CastSpell(victim, SPELL_BANSHEE_REGEN, args);
        }, 1ms);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!spellInfo || damage <= 0)
            return;
        if (!IsRangedBasicAttack(spellInfo))
        {
            Butcher(attacker, target, damage);
            return;
        }
        uint32 dealt = uint32(damage);
        BasicAttack(target, attacker, dealt, RANGED_ATTACK);
        damage = int32(dealt);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        Butcher(attacker, target, damage);
    }

private:
    // Spectre's Cowl, Spirit Visage: League health regeneration for 10 sec after an
    // enemy champion's damage, as Banshee's Veil's is put on.
    static void SpectreRegen(Player* victim)
    {
        RiftProgression::ItemHooks const hooks = RiftProgression::ItemHooksOf(victim);
        if (hooks.ChampionHitRegen <= 0.0f)
            return;
        if (Aura const* regen = victim->GetAura(SPELL_SPECTRE_REGEN); regen && regen->GetDuration() > SPECTRE_REFRESH_BELOW)
            return;
        int32 const amount = std::max(1, int32(std::lround(0.4f * hooks.Scale * hooks.ChampionHitRegen * (1.0f + hooks.HealAmp / 100.0f))));
        ObjectGuid const guid = victim->GetGUID();
        victim->m_Events.AddEventAtOffset([victim, guid, amount]()
        {
            if (!victim->IsAlive() || victim->GetGUID() != guid)
                return;
            CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
            args.AddSpellMod(SPELLVALUE_BASE_POINT0, amount - 1);   // the row's die adds 1
            victim->CastSpell(victim, SPELL_SPECTRE_REGEN, args);
        }, 1ms);
    }

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

        // League's UNIQUE item effects on a basic attack: the attacker's, then the struck champion's.
        if (Player* champion = attacker->ToPlayer(); champion && damage)
        {
            RiftProgression::ItemHooks const hooks = RiftProgression::ItemHooksOf(champion);
            if (hooks.InMatch)
            {
                Butcher(champion, target, damage);
                if (hooks.OnHitHeal > 0.0f)
                    HealChampion(champion, hooks.OnHitHeal * hooks.Scale, hooks.HealAmp);
                if (hooks.Maim > 0.0f && IsMonster(target))
                {
                    uint32 const maim = uint32(std::lround(hooks.Maim * hooks.Scale));
                    ObjectGuid const targetGuid = target->GetGUID();
                    champion->m_Events.AddEventAtOffset([champion, targetGuid, maim]()
                    {
                        if (Unit* struck = ObjectAccessor::GetUnit(*champion, targetGuid))
                            if (champion->IsAlive())
                                DealItemDamage(champion, struck, SPELL_MAIM, maim);
                    }, 1ms);
                }
            }
        }
        if (Player* struck = target->ToPlayer(); struck && damage)
        {
            RiftProgression::ItemHooks const hooks = RiftProgression::ItemHooksOf(struck);
            if (hooks.InMatch)
            {
                if (hooks.BasicBlockPct > 0.0f)
                    damage = uint32(std::lround(float(damage) * std::max(0.0f, 1.0f - hooks.BasicBlockPct / 100.0f)));
                if (hooks.ChampionBasicBlock > 0.0f && attacker->GetTypeId() == TYPEID_PLAYER)
                {
                    uint32 const block = uint32(std::lround(hooks.ChampionBasicBlock * hooks.Scale));
                    damage = damage > block ? damage - block : 0;
                }
                if ((hooks.ColdSteelAs > 0.0f || hooks.ColdSteelMs > 0.0f) && attacker->IsAlive())
                    ColdSteel(struck, attacker, hooks);
            }
        }

        // Icy: put on a moment later too.
        if (Player* champion = attacker->ToPlayer(); champion && damage && target->IsAlive())
            if (uint32 const icy = champion->GetItemCount(ITEM_PHAGE, false) + champion->GetItemCount(ITEM_TRINITY_FORCE, false))
                if (roll_chance_f(100.0f * (1.0f - std::pow(ICY_MISS_CHANCE, float(icy)))))
                {
                    int32 const slow = attackType == RANGED_ATTACK ? ICY_SLOW_RANGED : ICY_SLOW;
                    ObjectGuid const targetGuid = target->GetGUID();
                    champion->m_Events.AddEventAtOffset([champion, targetGuid, slow]()
                    {
                        Unit* struck = ObjectAccessor::GetUnit(*champion, targetGuid);
                        if (!struck || !struck->IsAlive() || !champion->IsValidAttackTarget(struck))
                            return;
                        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
                        args.AddSpellMod(SPELLVALUE_BASE_POINT0, -slow - 1);   // the row's die adds 1
                        champion->CastSpell(struck, SPELL_ICY_SLOW, args);
                    }, 1ms);
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

// 94168 the Season 3 Dragon's burn: its third effect's amount in true damage a
// second, 4 sec (BattlegroundSRF::DragonAttack puts it on and keeps it going).
class spell_rift_dragon_burn : public AuraScript
{
    PrepareAuraScript(spell_rift_dragon_burn);

    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        if (!caster || aurEff->GetAmount() <= 0)
            return;
        DealTrueDamage(caster, GetTarget(), GetSpellInfo(), uint32(aurEff->GetAmount()));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rift_dragon_burn::Tick, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 94163 Blessing of the Lizard Elder, on the one struck: its second effect's
// amount in true damage each 1.5 sec (RiftProgression::LizardBurn keeps it going).
class spell_rift_lizard_burn : public AuraScript
{
    PrepareAuraScript(spell_rift_lizard_burn);

    void Tick(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        if (!caster || aurEff->GetAmount() <= 0)
            return;
        DealTrueDamage(caster, GetTarget(), GetSpellInfo(), uint32(aurEff->GetAmount()));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rift_lizard_burn::Tick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 94170 the champion's cooldown reduction (its first effect's amount, which
// RiftProgression::SetChampionCarriers keeps): a SPELLMOD_COOLDOWN pct modifier
// on the holder's class spells, so the client shows and times the shorter
// cooldowns itself.
class spell_rift_cdr : public AuraScript
{
    PrepareAuraScript(spell_rift_cdr);

    void CalcSpellMod(AuraEffect const* aurEff, SpellModifier*& spellMod)
    {
        RiftProgression::CooldownMod(aurEff, spellMod);
    }

    void Register() override
    {
        DoEffectCalcSpellMod += AuraEffectCalcSpellModFn(spell_rift_cdr::CalcSpellMod, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 94153 Summoner's Focus, every champion's match aura (BattlegroundSRF): its
// third effect's amount is a SPELLMOD_RANGE pct modifier on the holder's class
// spells cast at a range (RiftProgression::RangeMod); melee keeps its reach.
class spell_rift_focus : public AuraScript
{
    PrepareAuraScript(spell_rift_focus);

    void CalcSpellMod(AuraEffect const* aurEff, SpellModifier*& spellMod)
    {
        RiftProgression::RangeMod(aurEff, spellMod);
    }

    void Register() override
    {
        DoEffectCalcSpellMod += AuraEffectCalcSpellModFn(spell_rift_focus::CalcSpellMod, EFFECT_2, SPELL_AURA_DUMMY);
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
// or their pet, then is gone until 25 sec pass with no champion damage (OnDamage
// above restarts the wait).
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
    RegisterSpellScript(spell_rift_lizard_burn);
    RegisterSpellScript(spell_rift_dragon_burn);
    RegisterSpellScript(spell_rift_cdr);
    RegisterSpellScript(spell_rift_focus);
    RegisterSpellScript(spell_rift_item_warmog);
    RegisterSpellScript(spell_rift_item_rod_of_ages);
    RegisterSpellScript(spell_rift_item_guardian_angel);
    RegisterSpellScript(spell_rift_item_guardian_stasis);
    RegisterSpellScript(spell_rift_item_banshee);
    RegisterSpellScript(spell_rift_item_spell_shield);
}
