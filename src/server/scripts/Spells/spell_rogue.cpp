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
 * Scripts for spells with SPELLFAMILY_ROGUE and SPELLFAMILY_GENERIC spells used by rogue players.
 * Ordered alphabetically using scriptname.
 * Scriptnames of files in this file should be prefixed with "spell_rog_".
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Containers.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Spell.h"

enum RogueSpells
{
    SPELL_ROGUE_BLADE_FLURRY_EXTRA_ATTACK       = 22482,
    SPELL_ROGUE_CHEAT_DEATH_COOLDOWN            = 31231,
    SPELL_ROGUE_GLYPH_OF_PREPARATION            = 56819,
    SPELL_ROGUE_KILLING_SPREE                   = 51690,
    SPELL_ROGUE_KILLING_SPREE_TELEPORT          = 57840,
    SPELL_ROGUE_KILLING_SPREE_WEAPON_DMG        = 57841,
    SPELL_ROGUE_KILLING_SPREE_DMG_BUFF          = 61851,
    SPELL_ROGUE_PREY_ON_THE_WEAK                = 58670,
    SPELL_ROGUE_SHIV_TRIGGERED                  =  5940,
    SPELL_ROGUE_SLICE_AND_DICE_R1               =  5171,
    SPELL_ROGUE_TRICKS_OF_THE_TRADE             = 57934,
    SPELL_ROGUE_TRICKS_OF_THE_TRADE_DMG_BOOST   = 57933,
    SPELL_ROGUE_TRICKS_OF_THE_TRADE_PROC        = 59628,
    SPELL_ROGUE_HONOR_AMONG_THIEVES             = 51698,
    SPELL_ROGUE_HONOR_AMONG_THIEVES_PROC        = 52916,
    SPELL_ROGUE_HONOR_AMONG_THIEVES_2           = 51699,
    SPELL_ROGUE_T10_2P_BONUS                    = 70804,
    SPELL_ROGUE_GLYPH_OF_BACKSTAB_TRIGGER       = 63975,
    SPELL_ROGUE_QUICK_RECOVERY_ENERGY           = 31663,
    SPELL_ROGUE_CRIPPLING_POISON                =  3409,
    SPELL_ROGUE_MASTER_OF_SUBTLETY_BUFF         = 31665,
    SPELL_ROGUE_OVERKILL_BUFF                   = 58427,
    SPELL_ROGUE_STEALTH                         =  1784,
    SPELL_ROGUE_IMPROVED_SAP                    = 14095,
    SPELL_ROGUE_DEADLY_BREW                     = 81301,
    SPELL_ROGUE_GARROTE_POISON                  = 81302,
    SPELL_ROGUE_SEAL_FATE                       = 14186,
    SPELL_ROGUE_RUTHLESSNESS_R1                 = 14156,
    SPELL_ROGUE_RUTHLESSNESS_R2                 = 14160,
    SPELL_ROGUE_RUTHLESSNESS_R3                 = 14161,
    SPELL_ROGUE_RUTHLESSNESS_BONUS              = 81407,
    SPELL_ROGUE_IMPROVED_EVASION_TRIGGER        = 81403,
    SPELL_ROGUE_IMPROVED_EVASION_AURA           = 81404,
    SPELL_ROGUE_GOUGE_DOT_REMOVAL_AURA          = 81410,
    SPELL_ROGUE_STEALTH_AURA_STALKER            = 81439,
    SPELL_ROGUE_VANISH_AURA                     = 89783,
    SPELL_ROGUE_DEADLY_SHOT_INTERRUPT_TRIGGER   = 89159
};

// 13877, 33735, (check 51211, 65956) - Blade Flurry
class spell_rog_blade_flurry : public AuraScript
{
    PrepareAuraScript(spell_rog_blade_flurry);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_BLADE_FLURRY_EXTRA_ATTACK });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        _procTarget = eventInfo.GetActor()->SelectNearbyTarget(eventInfo.GetProcTarget());
        return _procTarget != nullptr;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        if (DamageInfo* damageInfo = eventInfo.GetDamageInfo())
        {
            CastSpellExtraArgs args(aurEff);
            args.AddSpellBP0(damageInfo->GetDamage());
            GetTarget()->CastSpell(_procTarget, SPELL_ROGUE_BLADE_FLURRY_EXTRA_ATTACK, args);
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_rog_blade_flurry::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_rog_blade_flurry::HandleProc, EFFECT_0, SPELL_AURA_MOD_MELEE_HASTE);
    }

    Unit* _procTarget = nullptr;
};

class spell_rog_evasion : public SpellScript
{
    PrepareSpellScript(spell_rog_evasion);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_IMPROVED_EVASION_TRIGGER });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        if (caster->HasAura(SPELL_ROGUE_IMPROVED_EVASION_AURA))
            caster->CastSpell(caster, SPELL_ROGUE_IMPROVED_EVASION_TRIGGER, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_rog_evasion::HandleAfterCast);
    }
};

// 703 - Garrote
class spell_rog_garrote : public SpellScript
{
    PrepareSpellScript(spell_rog_garrote);

    bool Load() override
    {
        return GetCaster()->GetTypeId() == TYPEID_PLAYER;
    }

    void HandleAfterHit()
    {
        Player* player = GetCaster()->ToPlayer();
        Unit* target = GetHitUnit();
        if (!player || !target)
            return;

        if (!player->HasAura(SPELL_ROGUE_GARROTE_POISON))
            return;

        auto applyWeaponPoisons = [player, target](Item* weapon) -> void
        {
            if (!weapon)
                return;

            for (uint8 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
            {
                SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(weapon->GetEnchantmentId(EnchantmentSlot(slot)));
                if (!enchant)
                    continue;

                for (uint8 s = 0; s < 3; ++s)
                {
                    if (enchant->Effect[s] != ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL)
                        continue;

                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(enchant->EffectArg[s]);
                    if (!spellInfo)
                    {
                        TC_LOG_ERROR("spells", "Player::CastItemCombatSpell Enchant {}, player (Name: {}, {}) cast unknown spell {}", enchant->ID, player->GetName(), player->GetGUID().ToString(), enchant->EffectArg[s]);
                        continue;
                    }

                    if (spellInfo->SpellFamilyName != SPELLFAMILY_ROGUE || spellInfo->Dispel != DISPEL_POISON)
                        continue;

                    player->CastSpell(target, enchant->EffectArg[s], weapon);
                }
            }
        };

        applyWeaponPoisons(player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND));
        applyWeaponPoisons(player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND));
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_rog_garrote::HandleAfterHit);
    }
};


// -31228 - Cheat Death
class spell_rog_cheat_death : public AuraScript
{
    PrepareAuraScript(spell_rog_cheat_death);

    uint32 absorbChance = 0;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_CHEAT_DEATH_COOLDOWN });
    }

    bool Load() override
    {
        absorbChance = GetEffectInfo(EFFECT_0).CalcValue();
        return GetUnitOwner()->GetTypeId() == TYPEID_PLAYER;
    }

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32 & amount, bool & /*canBeRecalculated*/)
    {
        // Set absorbtion amount to unlimited
        amount = -1;
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo & dmgInfo, uint32 & absorbAmount)
    {
        Player* target = GetTarget()->ToPlayer();
        if (dmgInfo.GetDamage() < target->GetHealth() || target->GetSpellHistory()->HasCooldown(SPELL_ROGUE_CHEAT_DEATH_COOLDOWN) || !roll_chance_i(absorbChance))
            return;

        target->CastSpell(target, SPELL_ROGUE_CHEAT_DEATH_COOLDOWN, true);
        target->GetSpellHistory()->AddCooldown(SPELL_ROGUE_CHEAT_DEATH_COOLDOWN, 0, std::chrono::minutes(1));

        uint32 health10 = target->CountPctFromMaxHealth(10);

        // hp > 10% - absorb hp till 10%
        if (target->GetHealth() > health10)
            absorbAmount = dmgInfo.GetDamage() - target->GetHealth() + health10;
        // hp lower than 10% - absorb everything
        else
            absorbAmount = dmgInfo.GetDamage();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_rog_cheat_death::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_rog_cheat_death::Absorb, EFFECT_0);
    }
};

// -51664 - Cut to the Chase
class spell_rog_cut_to_the_chase : public AuraScript
{
    PrepareAuraScript(spell_rog_cut_to_the_chase);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        // "refresh your Slice and Dice duration to its 5 combo point maximum"
        Unit* caster = eventInfo.GetActor();
        // lookup Slice and Dice
        if (AuraEffect const* snd = caster->GetAuraEffect(SPELL_AURA_MOD_MELEE_HASTE, SPELLFAMILY_ROGUE, 0x00040000, 0x00000000, 0x00000000, caster->GetGUID()))
        {
            // Max 5 cp duration
            uint32 countMax = snd->GetSpellInfo()->GetMaxDuration();

            snd->GetBase()->SetDuration(countMax, true);
            snd->GetBase()->SetMaxDuration(snd->GetBase()->GetDuration());
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_rog_cut_to_the_chase::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Slice and Dice (5 combo points, no combo point cost)
class spell_rog_slice_and_dice_5cp : public SpellScript
{
    PrepareSpellScript(spell_rog_slice_and_dice_5cp);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_SLICE_AND_DICE_R1 });
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        uint32 sliceAndDiceId = 6774;
        caster->AddAura(6774, caster);

        if (AuraEffect const* snd = caster->GetAuraEffect(SPELL_AURA_MOD_MELEE_HASTE, SPELLFAMILY_ROGUE, 0x00040000, 0x00000000, 0x00000000, caster->GetGUID()))
        {
            // Max 5 cp duration
            uint32 countMax = snd->GetSpellInfo()->GetMaxDuration();

            snd->GetBase()->SetDuration(countMax, true);
            snd->GetBase()->SetMaxDuration(snd->GetBase()->GetDuration());
        }
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_rog_slice_and_dice_5cp::HandleAfterCast);
    }
};


// -51625 - Deadly Brew
class spell_rog_deadly_brew : public AuraScript
{
    PrepareAuraScript(spell_rog_deadly_brew);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return true;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        eventInfo.GetActor()->CastSpell(eventInfo.GetProcTarget(), 11201, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_rog_deadly_brew::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_rog_ruthlessness_bonus : public AuraScript
{
    PrepareAuraScript(spell_rog_ruthlessness_bonus);

    bool Validate(SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id != SPELL_ROGUE_RUTHLESSNESS_BONUS)
            return false;

        return ValidateSpellInfo({ SPELL_ROGUE_RUTHLESSNESS_R1, SPELL_ROGUE_RUTHLESSNESS_R2, SPELL_ROGUE_RUTHLESSNESS_R3 });
    }

    bool Load() override
    {
        _ruthlessnessMask = flag96();
        _hasRuthlessnessMask = false;

        uint32 const ruthlessnessRanks[] = { SPELL_ROGUE_RUTHLESSNESS_R1, SPELL_ROGUE_RUTHLESSNESS_R2, SPELL_ROGUE_RUTHLESSNESS_R3 };
        for (uint32 spellId : ruthlessnessRanks)
        {
            if (SpellInfo const* ruthlessness = sSpellMgr->GetSpellInfo(spellId))
            {
                if (!ruthlessness->SpellFamilyFlags.IsEqual())
                {
                    _ruthlessnessMask |= ruthlessness->SpellFamilyFlags;
                    _hasRuthlessnessMask = true;
                }
            }
        }

        return _hasRuthlessnessMask;
    }

    void HandleEffectCalcSpellMod(AuraEffect const* aurEff, SpellModifier*& spellMod)
    {
        if (!_hasRuthlessnessMask)
            return;

        if (!spellMod)
        {
            spellMod = new SpellModifier(aurEff->GetBase());
            spellMod->op = SPELLMOD_CHANCE_OF_SUCCESS;
            spellMod->type = SPELLMOD_FLAT;
            spellMod->spellId = SPELL_ROGUE_RUTHLESSNESS_R1;
            spellMod->mask = _ruthlessnessMask;
        }

        spellMod->value = 40;
    }

    void Register() override
    {
        DoEffectCalcSpellMod += AuraEffectCalcSpellModFn(spell_rog_ruthlessness_bonus::HandleEffectCalcSpellMod, EFFECT_1, SPELL_AURA_DUMMY);
    }

    flag96 _ruthlessnessMask;
    bool _hasRuthlessnessMask = false;
};

//2818 - deadly poison
//5760 - mind numbing poison
//8680 - instant poison
//13218 - wound poison
class spell_rog_poison : public SpellScript
{
    PrepareSpellScript(spell_rog_poison);

    bool Load() override
    {
        // at this point CastItem must already be initialized
        return GetCaster()->GetTypeId() == TYPEID_PLAYER && GetCastItem();
    }

    void HandleAfterHit()
    {
        AuraApplication* sealFate = GetCaster()->GetAuraApplicationOfRankedSpell(SPELL_ROGUE_SEAL_FATE);
        if (sealFate)
        {
            if (roll_chance_f(sealFate->GetBase()->GetSpellInfo()->ProcChance))
            {
                GetCaster()->AddAura(11201, GetHitUnit());
            }
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_rog_poison::HandleAfterHit);
    }

};

// -2818 - Deadly Poison
class spell_rog_deadly_poison : public SpellScript
{
    PrepareSpellScript(spell_rog_deadly_poison);

    bool Load() override
    {
        // at this point CastItem must already be initialized
        return GetCaster()->GetTypeId() == TYPEID_PLAYER && GetCastItem();
    }

    void HandleBeforeHit(SpellMissInfo missInfo)
    {
        if (missInfo != SPELL_MISS_NONE)
            return;

        if (Unit* target = GetHitUnit())
            // Deadly Poison
            if (AuraEffect const* aurEff = target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_ROGUE, 0x10000, 0x80000, 0, GetCaster()->GetGUID()))
                _stackAmount = aurEff->GetBase()->GetStackAmount();
    }

    void HandleAfterHit()
    {
        if (_stackAmount < 5)
            return;

        Player* player = GetCaster()->ToPlayer();

        if (Unit* target = GetHitUnit())
        {

            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);

            if (item == GetCastItem())
                item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

            if (!item)
                return;

            // item combat enchantments
            for (uint8 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
            {
                SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(EnchantmentSlot(slot)));
                if (!enchant)
                    continue;

                for (uint8 s = 0; s < 3; ++s)
                {
                    if (enchant->Effect[s] != ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL)
                        continue;

                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(enchant->EffectArg[s]);
                    if (!spellInfo)
                    {
                        TC_LOG_ERROR("spells", "Player::CastItemCombatSpell Enchant {}, player (Name: {}, {}) cast unknown spell {}", enchant->ID, player->GetName(), player->GetGUID().ToString(), enchant->EffectArg[s]);
                        continue;
                    }

                    // Proc only rogue poisons
                    if (spellInfo->SpellFamilyName != SPELLFAMILY_ROGUE || spellInfo->Dispel != DISPEL_POISON)
                        continue;

                    // Do not reproc deadly
                    if (spellInfo->SpellFamilyFlags.IsEqual(0x10000, 0x80000, 0))
                        continue;

                    if (spellInfo->IsPositive())
                        player->CastSpell(player, enchant->EffectArg[s], item);
                    else
                        player->CastSpell(target, enchant->EffectArg[s], item);
                }
            }
        }
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_rog_deadly_poison::HandleBeforeHit);
        AfterHit += SpellHitFn(spell_rog_deadly_poison::HandleAfterHit);
    }

    uint8 _stackAmount = 0;
};

// 51690 - Killing Spree
class spell_rog_killing_spree : public SpellScriptLoader
{
    public:
        static char constexpr const ScriptName[] = "spell_rog_killing_spree";

        spell_rog_killing_spree() : SpellScriptLoader(ScriptName) { }

        class spell_rog_killing_spree_SpellScript : public SpellScript
        {
            PrepareSpellScript(spell_rog_killing_spree_SpellScript);

            void FilterTargets(std::list<WorldObject*>& targets)
            {
                if (targets.empty() || GetCaster()->GetVehicleBase())
                    FinishCast(SPELL_FAILED_OUT_OF_RANGE);
            }

            void HandleDummy(SpellEffIndex /*effIndex*/)
            {
                if (Aura* aura = GetCaster()->GetAura(SPELL_ROGUE_KILLING_SPREE))
                    if (spell_rog_killing_spree_AuraScript* script = aura->GetScript<spell_rog_killing_spree_AuraScript>(ScriptName))
                        script->AddTarget(GetHitUnit());
            }

            void Register() override
            {
                OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_rog_killing_spree_SpellScript::FilterTargets, EFFECT_1, TARGET_UNIT_DEST_AREA_ENEMY);
                OnEffectHitTarget += SpellEffectFn(spell_rog_killing_spree_SpellScript::HandleDummy, EFFECT_1, SPELL_EFFECT_DUMMY);
            }
        };

        SpellScript* GetSpellScript() const override
        {
            return new spell_rog_killing_spree_SpellScript();
        }

        class spell_rog_killing_spree_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_rog_killing_spree_AuraScript);

            bool Validate(SpellInfo const* /*spellInfo*/) override
            {
                return ValidateSpellInfo(
                {
                    SPELL_ROGUE_KILLING_SPREE_TELEPORT,
                    SPELL_ROGUE_KILLING_SPREE_WEAPON_DMG,
                    SPELL_ROGUE_KILLING_SPREE_DMG_BUFF
                });
            }

            void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
            {
                GetTarget()->CastSpell(GetTarget(), SPELL_ROGUE_KILLING_SPREE_DMG_BUFF, true);
            }

            void HandleEffectPeriodic(AuraEffect const* /*aurEff*/)
            {
                while (!_targets.empty())
                {
                    ObjectGuid guid = Trinity::Containers::SelectRandomContainerElement(_targets);
                    if (Unit* target = ObjectAccessor::GetUnit(*GetTarget(), guid))
                    {
                        GetTarget()->CastSpell(target, SPELL_ROGUE_KILLING_SPREE_TELEPORT, true);
                        GetTarget()->CastSpell(target, SPELL_ROGUE_KILLING_SPREE_WEAPON_DMG, true);
                        break;
                    }
                    else
                        _targets.remove(guid);
                }
            }

            void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
            {
                GetTarget()->RemoveAurasDueToSpell(SPELL_ROGUE_KILLING_SPREE_DMG_BUFF);
            }

            void Register() override
            {
                AfterEffectApply += AuraEffectApplyFn(spell_rog_killing_spree_AuraScript::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
                OnEffectPeriodic += AuraEffectPeriodicFn(spell_rog_killing_spree_AuraScript::HandleEffectPeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
                AfterEffectRemove += AuraEffectRemoveFn(spell_rog_killing_spree_AuraScript::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
            }

        public:
            void AddTarget(Unit* target)
            {
                _targets.push_back(target->GetGUID());
            }

        private:
            GuidList _targets;
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_rog_killing_spree_AuraScript();
        }
};
char constexpr const spell_rog_killing_spree::ScriptName[];

// -31130 - Nerves of Steel
class spell_rog_nerves_of_steel : public AuraScript
{
    PrepareAuraScript(spell_rog_nerves_of_steel);

public:
    spell_rog_nerves_of_steel()
    {
        absorbPct = 0;
    }

private:
    uint32 absorbPct;

    bool Load() override
    {
        absorbPct = GetEffectInfo(EFFECT_0).CalcValue(GetCaster());
        return true;
    }

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32 & amount, bool & /*canBeRecalculated*/)
    {
        // Set absorbtion amount to unlimited
        amount = -1;
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo & dmgInfo, uint32 & absorbAmount)
    {
        // reduces all damage taken while stun or fear
        if (GetTarget()->HasUnitFlag(UNIT_FLAG_FLEEING) || (GetTarget()->HasUnitFlag(UNIT_FLAG_STUNNED) && GetTarget()->HasAuraWithMechanic(1<<MECHANIC_STUN)))
            absorbAmount = CalculatePct(dmgInfo.GetDamage(), absorbPct);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_rog_nerves_of_steel::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_rog_nerves_of_steel::Absorb, EFFECT_0);
    }
};

// 31666 - Master of Subtlety
// 58428 - Overkill - aura remove spell (SERVERSIDE)
template <uint32 RemoveSpellId>
class spell_rog_overkill_mos : public AuraScript
{
    PrepareAuraScript(spell_rog_overkill_mos);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ RemoveSpellId });
    }

    void PeriodicTick(AuraEffect const* /*aurEff*/)
    {
        GetTarget()->RemoveAurasDueToSpell(RemoveSpellId);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rog_overkill_mos::PeriodicTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 14185 - Preparation
class spell_rog_preparation : public SpellScript
{
    PrepareSpellScript(spell_rog_preparation);

    bool Load() override
    {
        return GetCaster()->GetTypeId() == TYPEID_PLAYER;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return true;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        caster->GetSpellHistory()->ResetCooldowns([caster](SpellHistory::CooldownStorageType::iterator itr) -> bool
        {
            SpellInfo const* spellInfo = sSpellMgr->AssertSpellInfo(itr->first);
            if (spellInfo->SpellFamilyName != SPELLFAMILY_ROGUE || spellInfo->Id == 14185)
                return false;

            return true;
            /*
            return (spellInfo->SpellFamilyFlags[1] & SPELLFAMILYFLAG1_ROGUE_COLDB_SHADOWSTEP ||  // Cold Blood, Shadowstep
                spellInfo->SpellFamilyFlags[0] & SPELLFAMILYFLAG_ROGUE_VAN_EVAS_SPRINT) ||       // Vanish, Evasion, Sprint
                (caster->HasAura(SPELL_ROGUE_GLYPH_OF_PREPARATION) &&
                (spellInfo->SpellFamilyFlags[1] & SPELLFAMILYFLAG1_ROGUE_DISMANTLE ||            // Dismantle
                spellInfo->SpellFamilyFlags[0] & SPELLFAMILYFLAG_ROGUE_KICK ||                   // Kick
                (spellInfo->SpellFamilyFlags[0] & SPELLFAMILYFLAG_ROGUE_BLADE_FLURRY &&          // Blade Flurry
                spellInfo->SpellFamilyFlags[1] & SPELLFAMILYFLAG1_ROGUE_BLADE_FLURRY)));
            */
        }, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_rog_preparation::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// -51685 - Prey on the Weak
class spell_rog_prey_on_the_weak : public AuraScript
{
    PrepareAuraScript(spell_rog_prey_on_the_weak);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_PREY_ON_THE_WEAK });
    }

    void HandleEffectPeriodic(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();
        Unit* victim = target->GetVictim();
        if (victim && (target->GetHealthPct() > victim->GetHealthPct()))
        {
            if (!target->HasAura(SPELL_ROGUE_PREY_ON_THE_WEAK))
            {
                CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
                args.AddSpellBP0(aurEff->GetSpellEffectInfo().CalcValue());
                target->CastSpell(target, SPELL_ROGUE_PREY_ON_THE_WEAK, args);
            }
        }
        else
            target->RemoveAurasDueToSpell(SPELL_ROGUE_PREY_ON_THE_WEAK);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_rog_prey_on_the_weak::HandleEffectPeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// -31244 - Quick Recovery
class spell_rog_quick_recovery : public AuraScript
{
    PrepareAuraScript(spell_rog_quick_recovery);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_QUICK_RECOVERY_ENERGY });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        if (!spellInfo)
            return;

        Unit* caster = eventInfo.GetActor();
        int32 amount = CalculatePct(spellInfo->CalcPowerCost(caster, spellInfo->GetSchoolMask()), aurEff->GetAmount());
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(amount);
        caster->CastSpell(nullptr, SPELL_ROGUE_QUICK_RECOVERY_ENERGY, args);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_rog_quick_recovery::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// -1943 - Rupture
class spell_rog_rupture : public SpellScriptLoader
{
    public:
        static char constexpr const ScriptName[] = "spell_rog_rupture";

        spell_rog_rupture() : SpellScriptLoader(ScriptName) { }

        class spell_rog_rupture_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_rog_rupture_AuraScript);

            bool Load() override
            {
                Unit* caster = GetCaster();
                BonusDuration = 0;
                return caster && caster->GetTypeId() == TYPEID_PLAYER;
            }

            void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
            {
                if (Unit* caster = GetCaster())
                {
                    canBeRecalculated = false;

                    float const attackpowerPerCombo[6] =
                    {
                        0.0f,
                        0.015f,         // 1 point:  ${($m1 + $b1*1 + 0.015 * $AP) * 4} damage over 8 secs
                        0.024f,         // 2 points: ${($m1 + $b1*2 + 0.024 * $AP) * 5} damage over 10 secs
                        0.03f,          // 3 points: ${($m1 + $b1*3 + 0.03 * $AP) * 6} damage over 12 secs
                        0.03428571f,    // 4 points: ${($m1 + $b1*4 + 0.03428571 * $AP) * 7} damage over 14 secs
                        0.0375f         // 5 points: ${($m1 + $b1*5 + 0.0375 * $AP) * 8} damage over 16 secs
                    };

                    uint8 cp = caster->ToPlayer()->GetComboPoints();
                    if (cp > 5)
                        cp = 5;

                    amount += int32(caster->GetTotalAttackPowerValue(BASE_ATTACK) * attackpowerPerCombo[cp]);
                }
            }

            void ResetDuration(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
            {
                BonusDuration = 0;
            }

            void Register() override
            {
                DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_rog_rupture_AuraScript::CalculateAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
                AfterEffectApply += AuraEffectApplyFn(spell_rog_rupture_AuraScript::ResetDuration, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAPPLY);
            }

        public:
            // For Glyph of Backstab use
            uint32 BonusDuration;
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_rog_rupture_AuraScript();
        }
};
char constexpr const spell_rog_rupture::ScriptName[];

// 56800 - Glyph of Backstab (dummy)
class spell_rog_glyph_of_backstab : public AuraScript
{
    PrepareAuraScript(spell_rog_glyph_of_backstab);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_GLYPH_OF_BACKSTAB_TRIGGER });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        eventInfo.GetActor()->CastSpell(eventInfo.GetProcTarget(), SPELL_ROGUE_GLYPH_OF_BACKSTAB_TRIGGER, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_rog_glyph_of_backstab::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 63975 - Glyph of Backstab (triggered - serverside)
class spell_rog_glyph_of_backstab_triggered : public SpellScriptLoader
{
    public:
        spell_rog_glyph_of_backstab_triggered() : SpellScriptLoader("spell_rog_glyph_of_backstab_triggered") { }

        class spell_rog_glyph_of_backstab_triggered_SpellScript : public SpellScript
        {
            PrepareSpellScript(spell_rog_glyph_of_backstab_triggered_SpellScript);

            typedef spell_rog_rupture::spell_rog_rupture_AuraScript RuptureAuraScript;

            void HandleScript(SpellEffIndex effIndex)
            {
                PreventHitDefaultEffect(effIndex);

                Unit* caster = GetCaster();
                // search our Rupture aura on target
                if (AuraEffect* aurEff = GetHitUnit()->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_ROGUE, 0x00100000, 0, 0, caster->GetGUID()))
                {
                    RuptureAuraScript* ruptureAuraScript = aurEff->GetBase()->GetScript<RuptureAuraScript>(spell_rog_rupture::ScriptName);
                    if (!ruptureAuraScript)
                        return;

                    uint32& bonusDuration = ruptureAuraScript->BonusDuration;

                    // already includes duration mod from Glyph of Rupture
                    uint32 countMin = aurEff->GetBase()->GetMaxDuration();
                    uint32 countMax = countMin - bonusDuration;

                    // this glyph
                    countMax += 6000;

                    if (countMin < countMax)
                    {
                        bonusDuration += 2000;

                        aurEff->GetBase()->SetDuration(aurEff->GetBase()->GetDuration() + 2000);
                        aurEff->GetBase()->SetMaxDuration(countMin + 2000);
                    }

                }
            }

            void Register() override
            {
                OnEffectHitTarget += SpellEffectFn(spell_rog_glyph_of_backstab_triggered_SpellScript::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
            }
        };

        SpellScript* GetSpellScript() const override
        {
            return new spell_rog_glyph_of_backstab_triggered_SpellScript();
        }
};

// -13983 - Setup
class spell_rog_setup : public AuraScript
{
    PrepareAuraScript(spell_rog_setup);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (Player* target = GetTarget()->ToPlayer())
            if (eventInfo.GetActor() == target->GetSelectedUnit())
                return true;

        return false;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_rog_setup::CheckProc);
    }
};


// 1776 et al - Gouge
class spell_rog_gouge : public SpellScript
{
    PrepareSpellScript(spell_rog_gouge);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        if (!caster->HasAura(SPELL_ROGUE_GOUGE_DOT_REMOVAL_AURA))
            return;

        target->RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE, ObjectGuid::Empty, nullptr, true, false);
        target->RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT, ObjectGuid::Empty, nullptr, true, false);
        target->RemoveAurasByType(SPELL_AURA_PERIODIC_LEECH, ObjectGuid::Empty, nullptr, true, false);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_rog_gouge::HandleAfterHit);
    }
};

// 5938 - Shiv
class spell_rog_shiv : public SpellScript
{
    PrepareSpellScript(spell_rog_shiv);

    bool Load() override
    {
        return GetCaster()->GetTypeId() == TYPEID_PLAYER;
    }

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_SHIV_TRIGGERED });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (Unit* unitTarget = GetHitUnit())
            caster->CastSpell(unitTarget, SPELL_ROGUE_SHIV_TRIGGERED, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_rog_shiv::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// 57934 - Tricks of the Trade
class spell_rog_tricks_of_the_trade_aura : public AuraScript
{
    PrepareAuraScript(spell_rog_tricks_of_the_trade_aura);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo(
        {
            SPELL_ROGUE_TRICKS_OF_THE_TRADE_DMG_BOOST,
            SPELL_ROGUE_TRICKS_OF_THE_TRADE_PROC
        });
    }

    void OnRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT || !GetTarget()->HasAura(SPELL_ROGUE_TRICKS_OF_THE_TRADE_PROC))
            GetTarget()->GetThreatManager().UnregisterRedirectThreat(SPELL_ROGUE_TRICKS_OF_THE_TRADE);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Unit* rogue = GetTarget();
        Unit* target = ObjectAccessor::GetUnit(*rogue, _redirectTarget);
        if (target)
        {
            rogue->CastSpell(target, SPELL_ROGUE_TRICKS_OF_THE_TRADE_DMG_BOOST, aurEff);
            rogue->CastSpell(rogue, SPELL_ROGUE_TRICKS_OF_THE_TRADE_PROC, aurEff);
        }
        Remove(AURA_REMOVE_BY_DEFAULT);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_rog_tricks_of_the_trade_aura::OnRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectProc += AuraEffectProcFn(spell_rog_tricks_of_the_trade_aura::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }

    ObjectGuid _redirectTarget;
public:
    void SetRedirectTarget(ObjectGuid const& guid) { _redirectTarget = guid; }
};

// 57934 - Tricks of the Trade
class spell_rog_tricks_of_the_trade : public SpellScript
{
    PrepareSpellScript(spell_rog_tricks_of_the_trade);

    void DoAfterHit()
    {
        if (Aura* aura = GetHitAura())
            if (auto* script = aura->GetScript<spell_rog_tricks_of_the_trade_aura>("spell_rog_tricks_of_the_trade"))
            {
                if (Unit* explTarget = GetExplTargetUnit())
                    script->SetRedirectTarget(explTarget->GetGUID());
                else
                    script->SetRedirectTarget(ObjectGuid::Empty);
            }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_rog_tricks_of_the_trade::DoAfterHit);
    }
};

// 59628 - Tricks of the Trade (Proc)
class spell_rog_tricks_of_the_trade_proc : public AuraScript
{
    PrepareAuraScript(spell_rog_tricks_of_the_trade_proc);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->GetThreatManager().UnregisterRedirectThreat(SPELL_ROGUE_TRICKS_OF_THE_TRADE);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_rog_tricks_of_the_trade_proc::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// -51698, 51700, 51701 - Honor Among Thieves
class spell_rog_honor_among_thieves : public AuraScript
{
    PrepareAuraScript(spell_rog_honor_among_thieves);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo(
        {
            SPELL_ROGUE_HONOR_AMONG_THIEVES_2,
            spellInfo->GetEffect(EFFECT_0).TriggerSpell
        });
    }

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        Unit* caster = GetCaster();
        if (!caster || caster->HasAura(SPELL_ROGUE_HONOR_AMONG_THIEVES_2))
            return false;

        return true;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Unit* caster = GetCaster();
        if (!caster)
            return;

        Unit* target = GetTarget();
        target->CastSpell(target, aurEff->GetSpellEffectInfo().TriggerSpell, { aurEff, caster->GetGUID() });
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_rog_honor_among_thieves::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_rog_honor_among_thieves::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 52916 - Honor Among Thieves (Proc)
class spell_rog_honor_among_thieves_proc : public SpellScript
{
    PrepareSpellScript(spell_rog_honor_among_thieves_proc);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_HONOR_AMONG_THIEVES_PROC });
    }

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        targets.clear();

        Unit* target = GetOriginalCaster();
        if (!target)
            return;

        targets.push_back(target);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_rog_honor_among_thieves_proc::FilterTargets, EFFECT_0, TARGET_UNIT_CASTER_AREA_PARTY);
    }
};

class spell_rog_honor_among_thieves_proc_aura : public AuraScript
{
    PrepareAuraScript(spell_rog_honor_among_thieves_proc_aura);

    void HandleEffectApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        if (Player* player = caster->ToPlayer())
            player->CastSpell(nullptr, SPELL_ROGUE_HONOR_AMONG_THIEVES_2, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_rog_honor_among_thieves_proc_aura::HandleEffectApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }
};

// -51627 - Turn the Tables
class spell_rog_turn_the_tables : public AuraScript
{
    PrepareAuraScript(spell_rog_turn_the_tables);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->GetEffect(EFFECT_0).TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Unit* caster = GetCaster();
        if (!caster)
            return;

        caster->CastSpell(nullptr, aurEff->GetSpellEffectInfo().TriggerSpell, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_rog_turn_the_tables::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// -11327 - Vanish
class spell_rog_vanish : public AuraScript
{
    PrepareAuraScript(spell_rog_vanish);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_STEALTH, SPELL_ROGUE_VANISH_AURA, SPELL_ROGUE_STEALTH_AURA_STALKER });
    }

    void ApplyStealth(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* unitTarget = GetTarget();
        unitTarget->RemoveAurasByType(SPELL_AURA_MOD_STALKED);

        unitTarget->CombatStop(true, false);

        // See if we already are stealthed. If so, we're done.
        if (unitTarget->HasAura(SPELL_ROGUE_STEALTH))
            return;

        // Reset cooldown on stealth if needed
        if (unitTarget->GetSpellHistory()->HasCooldown(SPELL_ROGUE_STEALTH))
            unitTarget->GetSpellHistory()->ResetCooldown(SPELL_ROGUE_STEALTH);

        unitTarget->CastSpell(nullptr, SPELL_ROGUE_STEALTH, true);

        unitTarget->AddAura(SPELL_ROGUE_VANISH_AURA, unitTarget);

        if (unitTarget->HasAura(81412))
            unitTarget->AddAura(SPELL_ROGUE_STEALTH_AURA_STALKER, unitTarget);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_rog_vanish::ApplyStealth, EFFECT_1, SPELL_AURA_MOD_STEALTH, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }
};

//14076, 14094, 14095 Improved Sap
class spell_rog_imp_sap : public AuraScript
{
    PrepareAuraScript(spell_rog_imp_sap);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_IMPROVED_SAP });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        Unit* unitTarget = GetTarget();
        unitTarget->RemoveAurasByType(SPELL_AURA_MOD_STALKED);

        // See if we already are stealthed. If so, we're done.
        if (unitTarget->HasAura(SPELL_ROGUE_STEALTH))
            return;

        // Reset cooldown on stealth if needed
        if (unitTarget->GetSpellHistory()->HasCooldown(SPELL_ROGUE_STEALTH))
            unitTarget->GetSpellHistory()->ResetCooldown(SPELL_ROGUE_STEALTH);

        unitTarget->CastSpell(nullptr, SPELL_ROGUE_STEALTH, true);
    }


    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_rog_imp_sap::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 6770, 2070, 11297 - Sap
class spell_rog_sap_diagnostic : public SpellScript
{
    PrepareSpellScript(spell_rog_sap_diagnostic);

    static Creature* GetCreatureTarget(Unit* unitTarget)
    {
        return unitTarget ? unitTarget->ToCreature() : nullptr;
    }

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        Creature* target = GetCreatureTarget(GetExplTargetUnit());
        if (!player || !target)
            return SPELL_CAST_OK;

        SpellInfo const* spellInfo = GetSpellInfo();
        SpellCastResult targetCheck = spellInfo->CheckTarget(player, target, false);
        bool fullImmune = target->IsImmunedToSpell(spellInfo, player);

        uint8 immuneEffectMask = 0;
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (effect.IsEffect() && target->IsImmunedToSpellEffect(spellInfo, effect, player))
                immuneEffectMask |= 1 << effect.EffectIndex;

        ChatHandler handler(player->GetSession());
        handler.PSendSysMessage("[SapDiag] spell=%u mechanic=%u target=%s entry=%u type=%u typeMask=0x%08X requiredTypeMask=0x%08X",
            spellInfo->Id, spellInfo->Mechanic, target->GetName().c_str(), target->GetEntry(), target->GetCreatureType(),
            target->GetCreatureTypeMask(), spellInfo->TargetCreatureType);
        handler.PSendSysMessage("[SapDiag] targetCheck=%u alive=%u inCombat=%u petInCombatFlag=%u targetFlags=0x%08X templateMechanicImmuneMask=0x%08X",
            uint32(targetCheck), target->IsAlive(), target->IsInCombat(), target->HasUnitFlag(UNIT_FLAG_PET_IN_COMBAT),
            uint32(target->GetUnitFlags()), target->GetCreatureTemplate()->MechanicImmuneMask);
        handler.PSendSysMessage("[SapDiag] fullImmune=%u immuneEffectMask=0x%02X aura0=%u effect0Mechanic=%u casterInCombat=%u casterStealthed=%u",
            fullImmune, immuneEffectMask, spellInfo->GetEffect(EFFECT_0).ApplyAuraName, spellInfo->GetEffect(EFFECT_0).Mechanic,
            player->IsInCombat(), player->HasAura(SPELL_ROGUE_STEALTH));
        handler.PSendSysMessage("[SapDiag] selectionPrecheck validAttack=%u effect0TargetOk=%u positiveEffect0=%u los=%u distance=%.2f targetMap=%u casterMap=%u",
            player->IsValidAttackTarget(target, spellInfo), GetSpell()->CheckEffectTarget(target, spellInfo->GetEffect(EFFECT_0), nullptr),
            spellInfo->IsPositiveEffect(EFFECT_0), target->IsWithinLOSInMap(player), player->GetExactDist(target), target->GetMapId(), player->GetMapId());

        return SPELL_CAST_OK;
    }

    void HandleObjectTargetSelect(WorldObject*& target)
    {
        Player* player = GetCaster()->ToPlayer();
        Creature* creatureTarget = target ? target->ToCreature() : nullptr;
        if (!player)
            return;

        ChatHandler(player->GetSession()).PSendSysMessage("[SapDiag] objectTargetSelect target=%s entry=%u targetIsCreature=%u",
            creatureTarget ? creatureTarget->GetName().c_str() : (target ? target->GetName().c_str() : "<none>"),
            creatureTarget ? creatureTarget->GetEntry() : 0, creatureTarget != nullptr);

        if (creatureTarget)
        {
            GetSpell()->AddUnitTargetForScript(creatureTarget, 1 << EFFECT_0, true, false);
            ChatHandler(player->GetSession()).PSendSysMessage("[SapDiag] objectTargetSelect forcedAddUnitTarget=1 uniqueTargets=%u",
                GetSpell()->GetUniqueTargetInfoSize());
        }
    }

    void HandleOnCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

        Spell* spell = GetSpell();
        ChatHandler handler(player->GetSession());
        Unit* target = spell->m_targets.GetUnitTarget();
        handler.PSendSysMessage("[SapDiag] onCast explicitTarget=%s entry=%u targetIsCreature=%u uniqueTargets=%u auraScaleMask=0x%02X",
            target ? target->GetName().c_str() : "<none>", target ? target->GetEntry() : 0,
            target && target->ToCreature() != nullptr, spell->GetUniqueTargetInfoSize(), spell->GetAuraScaleMask());
    }

    void HandleBeforeHit(SpellMissInfo missInfo)
    {
        Player* player = GetCaster()->ToPlayer();
        Creature* target = GetCreatureTarget(GetHitUnit());
        if (!player || !target)
            return;

        ChatHandler(player->GetSession()).PSendSysMessage("[SapDiag] hitPhase target=%s entry=%u missInfo=%u",
            target->GetName().c_str(), target->GetEntry(), uint32(missInfo));
    }

    void HandleAfterHit()
    {
        Player* player = GetCaster()->ToPlayer();
        Creature* target = GetCreatureTarget(GetHitUnit());
        if (!player || !target)
            return;

        Aura* hitAura = GetHitAura();
        Aura* sapAura = target->GetAura(GetSpellInfo()->Id, player->GetGUID());
        ChatHandler(player->GetSession()).PSendSysMessage("[SapDiag] afterHit target=%s entry=%u hitAura=%u targetHasSapAura=%u sapAuraDuration=%d stunned=%u confused=%u rooted=%u unitFlags=0x%08X",
            target->GetName().c_str(), target->GetEntry(), hitAura ? hitAura->GetId() : 0, sapAura != nullptr,
            sapAura ? sapAura->GetDuration() : 0, target->HasUnitState(UNIT_STATE_STUNNED),
            target->HasUnitState(UNIT_STATE_CONFUSED), target->HasUnitState(UNIT_STATE_ROOT), uint32(target->GetUnitFlags()));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_rog_sap_diagnostic::CheckCast);
        OnObjectTargetSelect += SpellObjectTargetSelectFn(spell_rog_sap_diagnostic::HandleObjectTargetSelect, EFFECT_0, TARGET_UNIT_TARGET_ENEMY);
        OnCast += SpellCastFn(spell_rog_sap_diagnostic::HandleOnCast);
        BeforeHit += BeforeSpellHitFn(spell_rog_sap_diagnostic::HandleBeforeHit);
        AfterHit += SpellHitFn(spell_rog_sap_diagnostic::HandleAfterHit);
    }
};

class spell_rog_deadly_shot : public SpellScript
{
    PrepareSpellScript(spell_rog_deadly_shot);

    uint8 _comboPoints = 0;

    bool Load() override
    {
        if (Unit* caster = GetCaster())
            if (Player* player = caster->ToPlayer())
                _comboPoints = player->GetComboPoints();   // cache before finisher clears them

        return true;
    }

    void HandleOnHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!target || target == caster)
            target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;

        if (!_comboPoints)
            return;

        // Figure out what the target is casting *right now*
        SpellSchoolMask schoolMask = SPELL_SCHOOL_MASK_NONE;
        if (Spell* cur = target->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            schoolMask = cur->GetSpellInfo()->GetSchoolMask();
        }
        else if (Spell* cur = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            schoolMask = cur->GetSpellInfo()->GetSchoolMask();
        }

        // Not casting anything we care about => no lockout
        if (!schoolMask)
            return;

        // Stop the cast
        target->InterruptNonMeleeSpells(false);

        // 1 second per combo point, cap at 5s if you want
        uint32 lockMs = std::min<uint32>(_comboPoints, 5u) * IN_MILLISECONDS;

        // Lock out the school of the *interrupted* spell, not Deadly Shot's school
        target->GetSpellHistory()->LockSpellSchool(schoolMask, lockMs);

        if (caster->HasAura(SPELL_ROGUE_IMPROVED_EVASION_AURA))
            caster->CastSpell(caster, SPELL_ROGUE_DEADLY_SHOT_INTERRUPT_TRIGGER, true);
    }


    void Register() override
    {
        OnHit += SpellHitFn(spell_rog_deadly_shot::HandleOnHit);
    }
};

void AddSC_rogue_spell_scripts()
{
    RegisterSpellScript(spell_rog_blade_flurry);
    RegisterSpellScript(spell_rog_cheat_death);
    RegisterSpellScript(spell_rog_cut_to_the_chase);
    RegisterSpellScript(spell_rog_deadly_poison);
    RegisterSpellScript(spell_rog_slice_and_dice_5cp);
    RegisterSpellScript(spell_rog_garrote);
    RegisterSpellScript(spell_rog_ruthlessness_bonus);
    new spell_rog_killing_spree();
    RegisterSpellScript(spell_rog_nerves_of_steel);
    RegisterSpellScriptWithArgs(spell_rog_overkill_mos<SPELL_ROGUE_OVERKILL_BUFF>, "spell_rog_overkill");
    RegisterSpellScriptWithArgs(spell_rog_overkill_mos<SPELL_ROGUE_MASTER_OF_SUBTLETY_BUFF>, "spell_rog_master_of_subtlety");
    RegisterSpellScript(spell_rog_preparation);
    RegisterSpellScript(spell_rog_prey_on_the_weak);
    RegisterSpellScript(spell_rog_quick_recovery);
    new spell_rog_rupture();
    RegisterSpellScript(spell_rog_glyph_of_backstab);
    new spell_rog_glyph_of_backstab_triggered();
    RegisterSpellScript(spell_rog_setup);
    RegisterSpellScript(spell_rog_gouge);
    RegisterSpellScript(spell_rog_shiv);
    RegisterSpellAndAuraScriptPair(spell_rog_tricks_of_the_trade, spell_rog_tricks_of_the_trade_aura);
    RegisterSpellScript(spell_rog_tricks_of_the_trade_proc);
    RegisterSpellScript(spell_rog_honor_among_thieves);
    RegisterSpellAndAuraScriptPair(spell_rog_honor_among_thieves_proc, spell_rog_honor_among_thieves_proc_aura);
    RegisterSpellScript(spell_rog_turn_the_tables);
    RegisterSpellScript(spell_rog_vanish);
    RegisterSpellScript(spell_rog_imp_sap);
    RegisterSpellScript(spell_rog_sap_diagnostic);
    RegisterSpellScript(spell_rog_poison);
    RegisterSpellScript(spell_rog_evasion);
    RegisterSpellScript(spell_rog_deadly_shot);
}
