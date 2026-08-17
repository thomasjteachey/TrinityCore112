/*
 * Legionnaire Plus T2 set bonuses - priest and mage, the scripted half.
 *
 * The carriers themselves are Spell.dbc rows 90300-90394 (see
 * sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql, which is the contract
 * for every id in this file). Everything a plain aura type could express -
 * cost, range, global cooldown, the flat +300% physical on Wraithblade - is
 * already done in data; only the bonuses that need to look at state, at a
 * different spell, or at the player's hands live here.
 *
 * Same shape as custom_t1_set_bonuses.cpp: the passive is an inert dummy the
 * ItemSet grants, and each script either sits on that dummy or on the ability
 * it modifies and tests HasAura() on the caster.
 *
 * Wiring lives in ItemSet.dbc (SetSpellID/SetThreshold), written by the forge
 * tooling; sql/custom/world/2026_08_17_01_world_t2_priest-mage.sql binds these
 * classes and carries the spell_proc rows the proc carriers need.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
    enum T2PriestMageSpells
    {
        // priest - holy (90336 3pc, 90337 5pc, 90338 8pc)
        SPELL_T2_HOLY_CENSER_PASSIVE    = 90338,
        SPELL_T2_HOLY_CENSER_IMBUE      = 90384,
        // Holy Fire's chain tops out at rank 8, level 60. 15267 is rank 7 and
        // is the id that usually gets mistaken for the last one.
        SPELL_PRIEST_HOLY_FIRE          = 15261,
        // The BUFF, not the talent: 15270 is the passive that triggers it.
        SPELL_PRIEST_SPIRIT_TAP         = 15271,

        // priest - shadow (90339 3pc, 90340 5pc, 90341 8pc)
        SPELL_T2_UMBRAL_MERCY           = 90340,
        SPELL_T2_WRAITHBLADE            = 90341,

        // mage - dusty / Ice Block (90342 3pc, 90343 5pc, 90344 8pc)
        SPELL_T2_GLACIAL_REPRIEVE       = 90342,
        SPELL_T2_HOARFROST_BLOOM        = 90343,
        SPELL_T2_ENCASED_IN_ICE         = 90385,
        SPELL_MAGE_ICE_BLOCK            = 45438,

        // mage - fiery payback (90345 3pc, 90346 5pc, 90347 8pc)
        SPELL_T2_SCORCHING_MOMENTUM     = 90346,
        SPELL_T2_ASHEN_CONFISCATION     = 90347,
        SPELL_T2_CONFISCATED_ARMS       = 90387,
        // Fiery Payback's own disarm: mechanic 3, 6 sec, and it takes the main
        // hand (aura 67) AND the ranged slot (aura 278) - which is why the
        // seizure copies visible item slots 15 and 17 and not the off hand.
        SPELL_MAGE_FIERY_PAYBACK_DISARM = 64346,
    };

    // Fire Blast, family 3, word 0 bit 0x2 - identical on all eleven ranks.
    uint32 constexpr MAGE_FIRE_BLAST_FAMILY_MASK0 = 0x2;

    // .gm diagnostics customauras - broadcast to every opted-in GM session
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }
}

// 90384 - Censer of the Faithful (holy 8pc), the imbue itself. The 8pc carrier
// (90338) is a plain PROC_TRIGGER_SPELL that hangs this 30 sec buff on the
// priest whenever Holy Fire crits; the payload is here, on the buff, so that
// it spends on the swing rather than on the crit that granted it.
class spell_t2_holy_censer : public AuraScript
{
    PrepareAuraScript(spell_t2_holy_censer);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        // "Imbue your MAIN HAND weapon". The spell_proc row already narrows
        // this to melee, but a dual-wielding priest's off-hand swing would
        // otherwise spend the single charge the main hand is holding.
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        return damageInfo && damageInfo->GetAttackType() == BASE_ATTACK;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        Unit* priest = GetTarget();
        Unit* victim = eventInfo.GetProcTarget();
        if (!victim)
            return;

        // Max rank, cast triggered: the imbue is not the priest casting Holy
        // Fire, so it must not pay mana, respect range or eat the global.
        priest->CastSpell(victim, SPELL_PRIEST_HOLY_FIRE, aurEff);
        priest->CastSpell(priest, SPELL_PRIEST_SPIRIT_TAP, aurEff);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Censer of the Faithful discharged into {} - Holy Fire + Spirit Tap",
            priest->GetName(), victim->GetName()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_holy_censer::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_holy_censer::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 2054 \ 2055 \ 6063 \ 6064 - Heal, carrying the shadow 5pc: cast in
// Shadowform it costs the priest half of what it healed.
//
// This is only the price half. Being ALLOWED to cast Heal in Shadowform is a
// ShapeshiftExclude edit on those four rows, deliberately kept out of the T2
// dbc file because it changes shipped spells for every priest on the server -
// until that ships, this script simply never has anything to do.
class spell_t2_umbral_mercy : public SpellScript
{
    PrepareSpellScript(spell_t2_umbral_mercy);

    void CaptureHeal()
    {
        _selfDamage = 0;

        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_UMBRAL_MERCY))
            return;
        // "Doing so" is specifically casting it in Shadowform; a priest who
        // drops form to heal normally pays nothing.
        if (caster->GetShapeshiftForm() != FORM_SHADOW)
            return;

        // OnHit, NOT AfterHit: by AfterHit m_healing has been overwritten with
        // the EFFECTIVE heal, so healing a full-health ally would cost the
        // priest nothing at all. Here it is still the full computed heal -
        // healing-taken debuffs are already in it, overheal is not.
        _selfDamage = CalculatePct(GetHitHeal(), 50);
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster || _selfDamage <= 0)
            return;

        // Never lethal. Killing the caster from AfterHit re-enters setDeathState ->
        // RemoveAllAurasOnDeath + InterruptNonMeleeSpells underneath the Spell that
        // is still executing, which is precisely the re-entrancy shape not worth
        // introducing while a sporadic heap corruption is unexplained. It is also a
        // gameplay footgun: a priest should not one-shot himself with his own heal.
        if (uint32(_selfDamage) >= caster->GetHealth())
            _selfDamage = int32(caster->GetHealth()) - 1;
        if (_selfDamage <= 0)
            return;

        // Dealt raw and as shadow rather than through a spell: Shadowform's
        // -15% physical damage taken (effect 3 of 15473) would otherwise
        // refund a chunk of the priest's own price, and routing it through
        // DealDamage skips resistance and school absorbs the same way.
        Unit::DealDamage(caster, caster, uint32(_selfDamage), nullptr,
            SELF_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Umbral Mercy - Heal in Shadowform cost {} health",
            caster->GetName(), _selfDamage));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_t2_umbral_mercy::CaptureHeal);
        AfterHit += SpellHitFn(spell_t2_umbral_mercy::HandleAfterHit);
    }

private:
    int32 _selfDamage = 0;
};

// 90341 - Wraithblade (shadow 8pc): the +300% melee damage is effect 2 and
// needs no code. This is the mana half - 10% of what the swing actually dealt.
class spell_t2_wraithblade : public AuraScript
{
    PrepareAuraScript(spell_t2_wraithblade);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        return damageInfo && damageInfo->GetDamage() > 0;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo)
            return;

        Unit* priest = GetTarget();
        int32 const mana = CalculatePct(int32(damageInfo->GetDamage()), 10);
        if (mana <= 0)
            return;

        // Direct energize rather than a helper cast, matching the T1 fury rage
        // bonus: a passive-cast helper never paid out there either.
        priest->EnergizeBySpell(priest, GetSpellInfo(), mana, POWER_MANA);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_wraithblade::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_wraithblade::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 120 and up - Cone of Cold, carrying the dusty 3pc: 3 sec off Ice Block per
// enemy struck. OnHit runs once per target that was actually hit, so the
// per-enemy accumulation is free - misses and immunes never reach here.
class spell_t2_dusty_glacial_reprieve : public SpellScript
{
    PrepareSpellScript(spell_t2_dusty_glacial_reprieve);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        if (!caster || !GetHitUnit())
            return;
        Player* mage = caster->ToPlayer();
        if (!mage || !mage->HasAura(SPELL_T2_GLACIAL_REPRIEVE))
            return;

        // ModifyCooldown is a no-op when Ice Block is not on cooldown, and it
        // walks the category cooldown down with the spell cooldown, so there
        // is nothing to guard here.
        mage->GetSpellHistory()->ModifyCooldown(SPELL_MAGE_ICE_BLOCK, -3 * IN_MILLISECONDS);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_t2_dusty_glacial_reprieve::HandleHit);
    }
};

// 45438 - Ice Block, carrying the dusty 5pc: riding it out to the end freezes
// everything within 10 yards. Cancelling it early does not.
//
// This is a second script on 45438; the shipped spell_mage_ice_block (which
// triggers Hypothermia) is a SpellScript on the same id and must survive.
class spell_t2_dusty_hoarfrost : public AuraScript
{
    PrepareAuraScript(spell_t2_dusty_hoarfrost);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        AuraApplication const* application = GetTargetApplication();
        // EXPIRE only: BY_CANCEL is the player clicking it off, BY_DEATH is
        // the block failing to save them, and neither should pay out.
        if (!application || application->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        Unit* mage = GetTarget();
        if (!mage->HasAura(SPELL_T2_HOARFROST_BLOOM))
            return;

        // 90385 selects TARGET_UNIT_SRC_AREA_ENEMY, so this adds auras to
        // nearby enemies and never to the mage - nothing is inserted into the
        // container the aura machinery is currently walking.
        mage->CastSpell(mage, SPELL_T2_ENCASED_IN_ICE, true);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Hoarfrost Bloom - Ice Block ran out, freezing 10 yd",
            mage->GetName()));
    }

    void Register() override
    {
        // EFFECT_FIRST_FOUND: Ice Block's effect 0 is the self stun, effects 1
        // and 2 are the two halves of the immunity. Any of them going away
        // means the block is over.
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_dusty_hoarfrost::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// -----------------------------------------------------------------------
// 90344 Reflective Carapace (dusty 8pc) - NOT IMPLEMENTED.
//
// The decision taken was "reflect 50% of the damage Ice Block PREVENTED", and
// that number does not exist anywhere a script can reach:
//
//   * melee: Unit::CalculateMeleeDamage bails at the `immunedMask ==
//     ((1 << 0) | (1 << 1))` early return (Unit.cpp) BEFORE it computes any
//     damage at all - the weapon damage roll never happens, and the
//     UnitScript::ModifyMeleeDamage hook sits below that return.
//   * spells: Spell::DoAllEffectOnTarget stamps SPELL_MISS_IMMUNE and skips
//     effect handling entirely, so no SpellScript hook and no proc ever fires.
//
// TODO(core): Unit::CalculateMeleeDamage (the immunedMask early return) and
// Spell::DoAllEffectOnTarget (the SPELL_MISS_IMMUNE branch) have to compute the
// would-be damage and surface it - a UnitScript hook there, or a per-unit
// "prevented this hit" counter. Once that exists this is a short AuraScript on
// 45438. Left inert rather than half-built: the data-only substitutes (spell
// reflect, which is inert against melee, or a flat damage shield, which is not
// a percentage) would both silently ship the wrong bonus.
// -----------------------------------------------------------------------

// 31643 - Blazing Speed, carrying the fiery payback 5pc: the buff going up
// also resets Fire Blast. Bound to the triggered buff, not to the talent
// 31641/31642, because only the buff marks the moment it actually fired.
class spell_t2_fp_scorching_momentum : public AuraScript
{
    PrepareAuraScript(spell_t2_fp_scorching_momentum);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* mage = GetTarget()->ToPlayer();
        if (!mage || !mage->HasAura(SPELL_T2_SCORCHING_MOMENTUM))
            return;

        // Predicate rather than a rank list: Fire Blast is eleven ranks and
        // the mage only ever has the one they cast on cooldown. ResetCooldown
        // erases the category entry alongside the spell entry, so the shared
        // category 19 lockout goes with it.
        uint32 cleared = 0;
        mage->GetSpellHistory()->ResetCooldowns([&cleared](SpellHistory::CooldownStorageType::iterator itr)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(itr->first);
            if (!info || info->SpellFamilyName != SPELLFAMILY_MAGE)
                return false;
            if (!(info->SpellFamilyFlags[0] & MAGE_FIRE_BLAST_FAMILY_MASK0))
                return false;
            ++cleared;
            return true;
        }, true);

        if (cleared)
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Scorching Momentum - Blazing Speed reset Fire Blast",
                mage->GetName()));
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_fp_scorching_momentum::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90347 - Ashen Confiscation (fiery payback 8pc). Two jobs in one script,
// because no tier of this set grants a disarm: the proc IS the disarm (the
// spell_proc row narrows it to Fire Blast), and a disarm that actually lands
// is immediately followed by the seizure.
//
// Scripting 64346 itself was the alternative and was rejected: it would run
// inside the VICTIM's aura machinery to hang a buff on the mage, and this set
// is the only disarm a mage without the Fiery Payback talent has anyway.
class spell_t2_ashen_confiscation : public AuraScript
{
    PrepareAuraScript(spell_t2_ashen_confiscation);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        Unit* victim = eventInfo.GetProcTarget();
        Player* mage = GetTarget()->ToPlayer();
        if (!mage || !victim)
            return;

        mage->CastSpell(victim, SPELL_MAGE_FIERY_PAYBACK_DISARM, aurEff);

        // Mechanic immunity, a PvP trinket or a resist all eat the disarm, and
        // "when you disarm an enemy" means the disarm has to be on them.
        if (!victim->HasAura(SPELL_MAGE_FIERY_PAYBACK_DISARM, mage->GetGUID()))
            return;

        // The attack power and the disarm immunity are 90387's two effects;
        // the amount is computed in its own script, at apply.
        mage->CastSpell(mage, SPELL_T2_CONFISCATED_ARMS, true);

        // "Seize their weapons" is an appearance swap and nothing more - the
        // items are not moved and the mage's own weapons keep working. Only
        // players have PLAYER_VISIBLE_ITEM_*; those indices run off the end of
        // a creature's update-field block, so a creature victim gives the
        // stat half of the bonus and no visual.
        //
        // CAVEAT: a mage transmogging their own main hand or ranged slot keeps
        // showing the transmog. Player::GetVisibleItemEntryFor prefers
        // _transmogSlotCache over the real field, and that cache is private
        // with no public way to suppress one slot temporarily. The stat half
        // is unaffected.
        Player* robbed = victim->ToPlayer();
        if (!robbed)
            return;
        mage->SetUInt32Value(PLAYER_VISIBLE_ITEM_16_ENTRYID,
            robbed->GetUInt32Value(PLAYER_VISIBLE_ITEM_16_ENTRYID));
        mage->SetUInt32Value(PLAYER_VISIBLE_ITEM_18_ENTRYID,
            robbed->GetUInt32Value(PLAYER_VISIBLE_ITEM_18_ENTRYID));

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Ashen Confiscation - disarmed {} and seized their arms",
            mage->GetName(), robbed->GetName()));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_t2_ashen_confiscation::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 90387 - Confiscated Arms, the buff 90347 hangs on the mage. Ships with a
// zero attack power amount on purpose; the real number is the mage's own spell
// power, which only exists at runtime.
class spell_t2_confiscated_arms : public AuraScript
{
    PrepareAuraScript(spell_t2_confiscated_arms);

    void CalcAttackPower(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        // Resolved once, at apply, and pinned: SpellBaseDamageBonusDone folds
        // attack power back into spell power for anything carrying
        // MOD_SPELL_DAMAGE_OF_ATTACK_POWER, and a recalculating amount would
        // chase its own tail on such a target.
        canBeRecalculated = false;
        amount = 0;

        // Fire, not the whole magic mask: this is the fiery payback set, and
        // all-school spell power (misc 126) is counted by a fire query anyway,
        // while a magic-wide query would also sweep in +frost / +arcane gear.
        if (Unit* caster = GetCaster())
            amount = std::max(0, caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* mage = GetTarget()->ToPlayer();
        if (!mage)
            return;

        // SetVisibleItemSlot, not a bare field write: it restores the entry,
        // both enchantment halves and the transmog slot cache from whatever is
        // really equipped, which is exactly what undoing the seizure means.
        // Passing the item back in (or nullptr for an empty slot) is what the
        // core's own unequip path does.
        mage->SetVisibleItemSlot(EQUIPMENT_SLOT_MAINHAND,
            mage->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND));
        mage->SetVisibleItemSlot(EQUIPMENT_SLOT_RANGED,
            mage->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_t2_confiscated_arms::CalcAttackPower, EFFECT_0, SPELL_AURA_MOD_ATTACK_POWER);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_confiscated_arms::HandleRemove, EFFECT_0, SPELL_AURA_MOD_ATTACK_POWER, AURA_EFFECT_HANDLE_REAL);
    }
};

// NOTE 90383 Kindled Zeal (the one-charge Holy Nova buff the holy 5pc
// triggers) deliberately has NO script. Both of its effects are spellmods, and
// Player::ApplySpellMod / Spell::TakeMods drop the aura's charge when the
// modified cast completes - a proc entry or a script here would consume it a
// second time.

void AddSC_custom_t2_priest_mage()
{
    RegisterSpellScript(spell_t2_holy_censer);
    RegisterSpellScript(spell_t2_umbral_mercy);
    RegisterSpellScript(spell_t2_wraithblade);
    RegisterSpellScript(spell_t2_dusty_glacial_reprieve);
    RegisterSpellScript(spell_t2_dusty_hoarfrost);
    RegisterSpellScript(spell_t2_fp_scorching_momentum);
    RegisterSpellScript(spell_t2_ashen_confiscation);
    RegisterSpellScript(spell_t2_confiscated_arms);
}
