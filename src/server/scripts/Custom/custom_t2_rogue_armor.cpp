/*
 * Legionnaire Plus T2 set bonuses - the rogue-armour group.
 *
 * Carriers 90348-90365 and helpers 90391-90394 are Spell.dbc rows from
 * sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql; the 2026-08-19 rework
 * adds 90513-90518, 90520, 90521, enchant 3889 and rewrites 14177 in place
 * (sql/custom/dbc/2026_08_19_07_dbc_t2_rogue_armor.sql), which is the
 * contract this file implements.
 *
 * Sets covered: rogue-ice-fang (90348/90349/90350), rogue-deadly-poison
 * (90352/90353), mail-momentum (90359's half of the 90392 buff),
 * leather-mister-reset (90360/90362).
 *
 * ICE FANG: Sprint uses the REPLACEMENT-WRAPPER pattern
 * (spell_warr_disarm_wrapper in Spells/spell_warrior.cpp): the rogue no
 * longer learns Sprint directly. They learn a DUMMY wrapper with the same
 * cost, cast time, cooldown, family and tooltip, and the wrapper script casts
 * either the untouched original or the set-bonus alternate, TRIGGERED, based
 * on the carrier aura. The originals are never edited, so a rogue without the
 * set plays exactly as before.
 *
 * Crippling Poison is NOT wrapped. The coat 3408/11202 stays the trained
 * spell, untouched in the DBC, and carries a script instead: with the 3pc
 * the ENCHANT_ITEM_TEMPORARY effect is replaced by hand with the Chilling
 * Poison enchant 3889 (proc 90513), mirroring Spell::EffectEnchantItemTmp
 * line for line. Reason: an item-targeted spell whose only effect is DUMMY
 * exists nowhere else in Spell.dbc, and if the client's item-target
 * validation keys on an enchant-type effect, every rogue on the realm loses
 * poison application - not a risk worth taking blind for one set bonus.
 * (90510/90511/90512, a first draft's DUMMY coat wrappers + coat clone, were
 * deleted before they ever shipped.)
 *
 * Cold Blood is the other exception, because 14177 is a TALENT (Talent.dbc
 * 142, Assassination tier 4), not a trained spell: there is no skill line to
 * repoint, and a new-id wrapper would need Talent.dbc rewritten on server AND
 * client plus a character_talent migration on every realm before a single
 * rogue learned it. So the wrapper is 14177 ITSELF, rewritten in place to a
 * DUMMY (what the user literally asked for: "make the original spells dummy
 * spells that pick"), and the stock crit-guarantee buff lives on as an exact
 * clone, 90521. Talent, spellbook, action bars and client SLA are untouched.
 *
 * Momentum (90358) is NOT here any more: the straight-line tracker lives in
 * the movement handler (T2UnitHooks.cpp). Only the 8pc gate on the 90392 buff
 * remains in this file. Feint Cadence / Dead Air are core (T2SpellHooks).
 *
 * Structure, naming and idioms follow custom_t1_set_bonuses.cpp.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "T2UnitHooks.h"
#include "Unit.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>

namespace
{
    enum T2RogueArmorSpells
    {
        // rogue-ice-fang carriers (inert DUMMY passives, read by the wrappers)
        SPELL_T2_CHILLING_POISON        = 90348,
        SPELL_T2_ICE_SKATE_CARRIER      = 90349,
        SPELL_T2_COLDER_BLOOD           = 90350,
        // rogue-deadly-poison
        SPELL_T2_VENOM_SUSTENANCE       = 90352,
        SPELL_T2_RUPTURING_VENOM        = 90353,
        // leather-mister-reset
        SPELL_T2_HEARTY_APPETITE        = 90360,
        SPELL_T2_FIELD_DRESSING         = 90362,

        // helpers from the 2026-08-17 file still in use
        SPELL_T2_COLDER_BLOOD_ROOT      = 90391,

        // 2026-08-19 rework: wrappers, alternates and helpers (90513-90521;
        // 90510-90512 were a first draft's Crippling Poison wrappers + coat
        // clone, deleted - the coat keeps its stock id, see the header)
        SPELL_T2_CHILLING_POISON_PROC   = 90513,   // clone of 3409: -55% move, -15% attack speed
        SPELL_T2_SPRINT_WRAPPER_R1      = 90514,   // learned instead of 2983
        SPELL_T2_SPRINT_WRAPPER_R2      = 90515,   // learned instead of 8696
        SPELL_T2_SPRINT_WRAPPER_R3      = 90516,   // learned instead of 11305
        SPELL_T2_ICE_SKATE              = 90517,   // +45% for 18 s, effect 2 drives the trail
        SPELL_T2_ICE_TRAIL_PATCH        = 90518,   // persistent area aura, the visible slick
        // (90519 was briefly a new-id Cold Blood wrapper; deleted - see below)
        SPELL_T2_VENOM_SUSTENANCE_HEAL  = 90520,   // SPELL_EFFECT_HEAL, bp at runtime
        SPELL_T2_COLD_BLOOD_BUFF        = 90521,   // exact clone of the stock 14177 crit buff

        // the stock abilities the wrappers hand off to
        SPELL_ROGUE_CRIPPLING_POISON_R1 = 3408,    // NOT wrapped: scripted in place (spell_t2_icefang_chilling_coat)
        SPELL_ROGUE_CRIPPLING_POISON_R2 = 11202,
        SPELL_ROGUE_SPRINT_R1           = 2983,
        SPELL_ROGUE_SPRINT_R2           = 8696,
        SPELL_ROGUE_SPRINT_R3           = 11305,
        SPELL_ROGUE_COLD_BLOOD          = 14177,   // the talent spell, rewritten IN PLACE to the wrapper
    };

    // SpellItemEnchantment 3889 - the Chilling Poison weapon coat, a clone of
    // Crippling Poison's enchant 22 with EffectArg_1 = 90513 (dbc SQL, 8.).
    constexpr uint32 ENCHANT_T2_CHILLING_POISON = 3889;

    // Spell::EffectEnchantItemTmp's duration ladder, for the one rung the
    // coat script can reach: 3408/11202 are SPELLFAMILY_ROGUE and not 38615,
    // so "other rogue family enchantments always 1 hour". (The rows' "lasts
    // for 30 minutes" tooltip is stale 3.3.5 text the core never honoured;
    // stock Crippling already lasts an hour on this realm.) Must stay equal to
    // what the core hands stock Crippling or the two poisons would drift apart.
    constexpr uint32 ROGUE_POISON_COAT_SECONDS = 3600;

    // Deadly Poison's periodic damage effect, on every rank. Same family mask
    // the core's own spell_rog_deadly_poison matches on.
    constexpr uint32 DEADLY_POISON_FAMILY_MASK_0 = 0x10000;
    constexpr uint32 DEADLY_POISON_FAMILY_MASK_1 = 0x80000;
    constexpr uint8  DEADLY_POISON_RUPTURE_STACKS = 5;

    // Colder Blood: the root is cast on the current selection, which no range
    // check of the self-cast wrapper ever covered. 90391 itself has RangeIndex
    // 13 ("anywhere"), so the reach is pinned here.
    constexpr float COLDER_BLOOD_ROOT_RANGE = 30.0f;

    // Ice Skate trail: one patch per this many yards of movement. Patches are
    // 2 yd radius (SpellRadius 7), so 2 yd spacing keeps the slick unbroken.
    constexpr float ICE_TRAIL_STEP_YARDS = 2.0f;

    // .gm diagnostics customauras - broadcast to every opted-in GM session
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }
}

// ===========================================================================
// ICE FANG - Crippling Poison in place, Sprint / Cold Blood wrapped
// ===========================================================================

// -3408 - Crippling Poison (both ranks), the stock coat spell, untouched in
// the DBC. With the 3pc (90348) the ENCHANT_ITEM_TEMPORARY effect is taken
// over: the default is prevented and the weapon gets the Chilling Poison
// enchant 3889 (proc 90513) instead of the rank's own 22 / 603. Without the
// carrier the effect runs as stock and nothing here is observable.
//
// The body is Spell::EffectEnchantItemTmp (SpellEffects.cpp) with only the
// enchant id and the duration rung swapped for constants: same player/item
// gates, same "owner may differ from caster (trade window)" rule, same GM
// trade log, same ApplyEnchantment(false) -> SetEnchantment -> Apply
// Enchantment(true) sequence - the apply side's default apply_dur = true is
// what books the enchant duration (AddEnchantmentDuration), exactly as the
// core's call does. Everything that happens BEFORE the hit (CheckItems'
// soulbound/trade rules against enchant 22, weapon subclass fit, cast time,
// cost) is the stock spell's own and still runs, since the spell IS the stock
// spell. No aura is added or removed here; the enchant is an item field.
class spell_t2_icefang_chilling_coat : public SpellScript
{
    PrepareSpellScript(spell_t2_icefang_chilling_coat);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_CHILLING_POISON, SPELL_T2_CHILLING_POISON_PROC });
    }

    void HandleEnchant(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        Item* itemTarget = GetHitItem();
        if (!player || !itemTarget)
            return;
        if (!player->HasAura(SPELL_T2_CHILLING_POISON))
            return;                                         // stock Crippling, default effect

        // The enchant row ships with SpellItemEnchantment.dbc; if this server
        // has not been regenerated yet, fall back to stock Crippling rather
        // than leave the weapon bare after a 3 s cast.
        SpellItemEnchantmentEntry const* pEnchant = sSpellItemEnchantmentStore.LookupEntry(ENCHANT_T2_CHILLING_POISON);
        if (!pEnchant)
        {
            TC_LOG_ERROR("spells", "spell_t2_icefang_chilling_coat: enchant {} missing from SpellItemEnchantment.dbc, {} coats stock Crippling Poison",
                ENCHANT_T2_CHILLING_POISON, player->GetName());
            return;
        }

        // The duration rung below is the rogue-family one; a non-rogue
        // binding would need the core's full ladder, so refuse the takeover.
        if (GetSpellInfo()->SpellFamilyName != SPELLFAMILY_ROGUE)
            return;

        // item can be in trade slot and have owner diff. from caster
        Player* item_owner = itemTarget->GetOwner();
        if (!item_owner)
            return;

        // Past every gate: from here on the stock enchant must NOT also land.
        PreventHitDefaultEffect(effIndex);

        if (item_owner != player && player->GetSession()->HasPermission(rbac::RBAC_PERM_LOG_GM_TRADE))
        {
            sLog->OutCommand(player->GetSession()->GetAccountId(), "GM {} (Account: {}) enchanting(temp): {} (Entry: {}) for player: {} (Account: {})",
                player->GetName(), player->GetSession()->GetAccountId(),
                itemTarget->GetTemplate()->Name1, itemTarget->GetEntry(),
                item_owner->GetName(), item_owner->GetSession()->GetAccountId());
        }

        // remove old enchanting before applying new if equipped
        item_owner->ApplyEnchantment(itemTarget, TEMP_ENCHANTMENT_SLOT, false);

        itemTarget->SetEnchantment(TEMP_ENCHANTMENT_SLOT, ENCHANT_T2_CHILLING_POISON, ROGUE_POISON_COAT_SECONDS * 1000, 0, player->GetGUID());

        // add new enchanting if equipped
        item_owner->ApplyEnchantment(itemTarget, TEMP_ENCHANTMENT_SLOT, true);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Chilling Poison coated {} (enchant {} for {} s)",
            player->GetName(), itemTarget->GetTemplate()->Name1, ENCHANT_T2_CHILLING_POISON, ROGUE_POISON_COAT_SECONDS));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_t2_icefang_chilling_coat::HandleEnchant, EFFECT_0, SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY);
    }
};

// 90514 \ 90515 \ 90516 - Sprint wrappers. With the 5pc the rogue gets Ice
// Skate (90517) regardless of rank; without it, that rank's Sprint.
//
// The wrapper keeps Sprint's family mask 0x40 so Endurance's cooldown mod
// lands on it (the wrapper owns the cooldown). That same mask makes it a
// target of Improved Sprint's ADD_TARGET_TRIGGER, which fires per HIT unit -
// so the caster is dropped from the wrapper's target list and only the inner
// Sprint / Ice Skate hit rolls the snare removal, once, as before.
class spell_t2_icefang_sprint_wrapper : public SpellScript
{
    PrepareSpellScript(spell_t2_icefang_sprint_wrapper);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_ICE_SKATE, SPELL_ROGUE_SPRINT_R1, SPELL_ROGUE_SPRINT_R2, SPELL_ROGUE_SPRINT_R3 });
    }

    void DropCasterTarget(WorldObject*& target)
    {
        target = nullptr;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        uint32 spell = SPELL_ROGUE_SPRINT_R1;
        if (caster->HasAura(SPELL_T2_ICE_SKATE_CARRIER))
            spell = SPELL_T2_ICE_SKATE;
        else
        {
            switch (GetSpellInfo()->Id)
            {
                case SPELL_T2_SPRINT_WRAPPER_R2: spell = SPELL_ROGUE_SPRINT_R2; break;
                case SPELL_T2_SPRINT_WRAPPER_R3: spell = SPELL_ROGUE_SPRINT_R3; break;
                default: break;
            }
        }
        caster->CastSpell(caster, spell, true);
    }

    void Register() override
    {
        OnObjectTargetSelect += SpellObjectTargetSelectFn(spell_t2_icefang_sprint_wrapper::DropCasterTarget, EFFECT_0, TARGET_UNIT_CASTER);
        OnEffectHit += SpellEffectFn(spell_t2_icefang_sprint_wrapper::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// 90517 - Ice Skate, the 5pc Sprint. Effect 1 is the data-only +45% for 18 s;
// effect 2 is a 250 ms PERIODIC_DUMMY that lays the trail: every 2 yards of
// movement, one 90518 patch (a PERSISTENT_AREA_AURA dynobject, 2 yd radius,
// 5 s, -30% move) is dropped at the rogue's feet. The rogue is the caster of
// every patch, so the enemy-only target check of the area aura validates
// against a real hostile unit - never against a summoned immune helper.
class spell_t2_icefang_ice_skate : public AuraScript
{
    PrepareAuraScript(spell_t2_icefang_ice_skate);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_ICE_TRAIL_PATCH });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* rogue = GetTarget();
        if (!rogue || !rogue->IsAlive() || !rogue->IsInWorld())
            return;

        // Distance-gated, not tick-gated: a rogue who stops drops nothing and
        // stacks nothing, a rogue at full speed drops roughly one per tick.
        if (_hasLast && rogue->GetExactDist2d(_lastX, _lastY) < ICE_TRAIL_STEP_YARDS)
            return;
        _lastX = rogue->GetPositionX();
        _lastY = rogue->GetPositionY();
        _hasLast = true;

        // Explicit destination (90518 keeps Frost Trap Aura's TARGET_FLAG_DEST
        // _LOCATION, so the dest survives InitExplicitTargets). Triggered, so
        // no GCD, no stealth break, no cooldown.
        rogue->CastSpell(rogue->GetPosition(), SPELL_T2_ICE_TRAIL_PATCH, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_icefang_ice_skate::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    float _lastX = 0.0f;
    float _lastY = 0.0f;
    bool  _hasLast = false;
};

// 14177 - Cold Blood, now the wrapper (the talent still teaches 14177, the
// spellbook row and the client are untouched; only the Spell.dbc row changed
// from "apply the crit buff" to DUMMY). Without the 8pc it casts 90521, an
// exact clone of the stock buff (crit guarantee, consumed by the next
// finisher, 3 min cooldown that starts when the buff is used). With Colder
// Blood (90350) it instead roots the current selection with 90391 and the
// 3 min cooldown starts at once.
//
// Cooldown plumbing: 14177 keeps SPELL_ATTR0_DISABLED_WHILE_ACTIVE, so
// SpellHistory::HandleCooldowns never starts its cooldown from the cast and
// the client waits for SMSG_COOLDOWN_EVENT instead of a local timer. The buff
// clone 90521 has that attribute STRIPPED (it is never cast directly, so the
// core's own hold/release on the buff's id would only be noise); instead
//   * spell_t2_icefang_cold_blood_cd on 90521 puts 14177 on hold when the
//     buff is applied and releases it (event + live 3 min) when the buff goes
//     away - the same two SpellHistory calls Aura::_ApplyForTarget /
//     _UnapplyForTarget made for the stock 14177;
//   * the root branch has no buff, so the wrapper releases itself right away.
// Nothing here touches the caster's aura containers.
class spell_t2_icefang_cold_blood_wrapper : public SpellScript
{
    PrepareSpellScript(spell_t2_icefang_cold_blood_wrapper);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_COLD_BLOOD_BUFF, SPELL_T2_COLDER_BLOOD_ROOT });
    }

    // The root needs a target the self-cast wrapper never asked for; refuse
    // the cast up front (no cooldown, no GCD) rather than eat the press.
    SpellCastResult CheckRootTarget()
    {
        Unit* caster = GetCaster();
        Player* rogue = caster ? caster->ToPlayer() : nullptr;
        if (!rogue || !rogue->HasAura(SPELL_T2_COLDER_BLOOD))
            return SPELL_CAST_OK;

        Unit* victim = rogue->GetSelectedUnit();
        if (!victim || !rogue->IsValidAttackTarget(victim))
            return SPELL_FAILED_BAD_TARGETS;
        if (!rogue->IsWithinDistInMap(victim, COLDER_BLOOD_ROOT_RANGE))
            return SPELL_FAILED_OUT_OF_RANGE;
        if (!rogue->IsWithinLOSInMap(victim))
            return SPELL_FAILED_LINE_OF_SIGHT;
        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Player* rogue = caster ? caster->ToPlayer() : nullptr;
        if (!rogue)
            return;

        if (!rogue->HasAura(SPELL_T2_COLDER_BLOOD))
        {
            rogue->CastSpell(rogue, SPELL_T2_COLD_BLOOD_BUFF, true);
            return;
        }

        // Re-validated: CheckCast ran when the press arrived, this runs when
        // the (instant) cast executes - same frame, but cheap to be sure.
        Unit* victim = rogue->GetSelectedUnit();
        if (!victim || !rogue->IsValidAttackTarget(victim)
            || !rogue->IsWithinDistInMap(victim, COLDER_BLOOD_ROOT_RANGE) || !rogue->IsWithinLOSInMap(victim))
            return;

        rogue->CastSpell(victim, SPELL_T2_COLDER_BLOOD_ROOT, true);

        // No buff to wait for: release the wrapper's cooldown now. The wrapper
        // is IsCooldownStartedOnEvent, so its own Spell::SendSpellCooldown is
        // a no-op and cannot overwrite this with a held entry afterwards.
        rogue->GetSpellHistory()->SendCooldownEvent(GetSpellInfo());

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Colder Blood rooted {}", rogue->GetName(), victim->GetName()));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_t2_icefang_cold_blood_wrapper::CheckRootTarget);
        OnEffectHit += SpellEffectFn(spell_t2_icefang_cold_blood_wrapper::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// 90521 - the Cold Blood buff, driving the on-event cooldown of 14177, the
// wrapper the rogue actually has on the bar. Apply: hold. Remove (consumed,
// expired, dispelled, death, logout cleanup): cooldown event, which the client
// turns into the 3 min timer on 14177 and the server into a live cooldown.
// This is the same pair of calls Aura::_ApplyForTarget/_UnapplyForTarget made
// for the stock 14177 buff, so nothing changes for the rogue (and whatever
// resets 14177's cooldown - Preparation in rulesets where it does - still
// finds it under the same id).
class spell_t2_icefang_cold_blood_cd : public AuraScript
{
    PrepareAuraScript(spell_t2_icefang_cold_blood_cd);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_COLD_BLOOD });
    }

    Player* WrapperOwner() const
    {
        Unit* caster = GetCaster();
        Player* rogue = caster ? caster->ToPlayer() : nullptr;
        if (!rogue || !rogue->HasSpell(SPELL_ROGUE_COLD_BLOOD))
            return nullptr;
        return rogue;
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* rogue = WrapperOwner())
            rogue->GetSpellHistory()->StartCooldown(sSpellMgr->AssertSpellInfo(SPELL_ROGUE_COLD_BLOOD), 0, nullptr, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* rogue = WrapperOwner())
            rogue->GetSpellHistory()->SendCooldownEvent(sSpellMgr->AssertSpellInfo(SPELL_ROGUE_COLD_BLOOD));
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_icefang_cold_blood_cd::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_icefang_cold_blood_cd::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ===========================================================================
// DEADLY POISON
// ===========================================================================

// 90352 - Venom Sustenance (deadly-poison 5pc): poison damage is partly
// returned as health. Which hits reach the script at all is world.spell_proc's
// job; this only decides what counts as "your poisons" and how much comes
// back. The heal is a real spell (90520, SPELL_EFFECT_HEAL) so it shows in
// the combat log and as floating text, not a silent HealBySpell.
class spell_t2_deadlypoison_leech : public AuraScript
{
    PrepareAuraScript(spell_t2_deadlypoison_leech);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_VENOM_SUSTENANCE_HEAL });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!damageInfo || !damageInfo->GetDamage() || !info)
            return false;
        // The same test the core's own deadly-poison script uses, rather than
        // a hand-kept id list that would rot the moment a poison is retuned.
        return info->SpellFamilyName == SPELLFAMILY_ROGUE && info->Dispel == DISPEL_POISON;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* rogue = GetTarget();
        if (!damageInfo || !rogue)
            return;
        int32 const heal = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (heal <= 0)
            return;
        // 90520 has DieSides 0, so BP0 is used exactly; NO_DONE_BONUS and
        // CANT_CRIT on the row keep it at exactly this number.
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(heal);
        rogue->CastSpell(rogue, SPELL_T2_VENOM_SUSTENANCE_HEAL, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_deadlypoison_leech::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_deadlypoison_leech::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// -2818 - Deadly Poison, carrying the Rupturing Venom 8pc: the fifth stack
// ruptures for most of the poison's remaining damage and consumes the stacks.
//
// This rides alongside the core's own spell_rog_deadly_poison; a spell carries
// as many script names as the table gives it.
class spell_t2_deadlypoison_burst : public SpellScript
{
    PrepareSpellScript(spell_t2_deadlypoison_burst);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        Player* rogue = caster->ToPlayer();
        if (!rogue)
            return;
        AuraEffect const* passive = rogue->GetAuraEffect(SPELL_T2_RUPTURING_VENOM, EFFECT_0);
        if (!passive)
            return;

        AuraEffect const* dot = target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_ROGUE,
            DEADLY_POISON_FAMILY_MASK_0, DEADLY_POISON_FAMILY_MASK_1, 0, rogue->GetGUID());
        if (!dot || dot->GetBase()->GetStackAmount() < DEADLY_POISON_RUPTURE_STACKS)
            return;

        // GetAmount() is already multiplied by the stack count - the tail of
        // AuraEffect::CalculateAmount does it for every aura type - so this is
        // the whole remaining pool, not one stack's share of it.
        int32 const remaining = dot->GetAmount() * int32(dot->GetRemainingTicks());
        int32 const burst = CalculatePct(remaining, passive->GetAmount());
        if (burst <= 0)
            return;

        // Deferred by a tick on purpose. We are inside the spell hit frame and
        // the payout has to remove the aura it is reading; draining it from the
        // rogue's own event processor puts both the damage and the removal at
        // the top of Unit::Update, clear of every aura iteration.
        ObjectGuid const rogueGuid = rogue->GetGUID();
        ObjectGuid const victimGuid = target->GetGUID();
        uint32 const dotId = dot->GetId();
        uint32 const school = dot->GetSpellInfo()->GetSchoolMask();
        rogue->m_Events.AddEventAtOffset([rogueGuid, victimGuid, dotId, school, burst]()
        {
            Player* owner = ObjectAccessor::FindPlayer(rogueGuid);
            if (!owner)
                return;
            Unit* victim = ObjectAccessor::GetUnit(*owner, victimGuid);
            if (!victim || !victim->IsAlive())
                return;
            // Re-read rather than trust the captured numbers: a second poison
            // application in the same tick may have burst this already.
            Aura* poison = victim->GetAura(dotId, rogueGuid);
            if (!poison || poison->GetStackAmount() < DEADLY_POISON_RUPTURE_STACKS)
                return;

            victim->RemoveAurasDueToSpell(dotId, rogueGuid);

            SpellNonMeleeDamage log(owner, victim, dotId, school);
            log.damage = uint32(burst);
            Unit::DealDamageMods(log.target, log.damage, &log.absorb);
            owner->DealSpellDamage(&log, false);
            owner->SendSpellNonMeleeDamageLog(&log);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Rupturing Venom burst {} on {} for {}",
                owner->GetName(), dotId, victim->GetName(), log.damage));
        }, Milliseconds(1));
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_t2_deadlypoison_burst::HandleAfterHit);
    }
};

// ===========================================================================
// MOMENTUM (8pc half only - the tracker is T2UnitHooks::OnPlayerMovement)
// ===========================================================================

// 90392 - Momentum, the stacking buff. Effect 1's speed is the 5pc and is
// always live; the damage reduction on effect 2 belongs to Inertial Guard, so
// it calculates to zero unless the 8pc is worn. AuraEffect::CalculateAmount
// multiplies by the stack count AFTER this hook, so the -5 here is per stack.
class spell_t2_momentum_buff : public AuraScript
{
    PrepareAuraScript(spell_t2_momentum_buff);

    void CalcGuard(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        Unit* owner = GetUnitOwner();
        if (!owner || !owner->HasAura(T2UnitHooks::SPELL_INERTIAL_GUARD))
            amount = 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_t2_momentum_buff::CalcGuard, EFFECT_1, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    }
};

// ===========================================================================
// MISTER RESET
// ===========================================================================

// 29073 - Eat, carrying the Hearty Appetite 3pc. The percentage is read off
// the passive rather than written here, so retuning it is a dbc change.
class spell_t2_reset_hearty_appetite : public AuraScript
{
    PrepareAuraScript(spell_t2_reset_hearty_appetite);

    void CalcRegen(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        Unit* owner = GetUnitOwner();
        if (!owner)
            return;
        if (AuraEffect const* passive = owner->GetAuraEffect(SPELL_T2_HEARTY_APPETITE, EFFECT_0))
            AddPct(amount, passive->GetAmount());
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_t2_reset_hearty_appetite::CalcRegen, EFFECT_FIRST_FOUND, SPELL_AURA_MOD_REGEN);
    }
};

// 11196 - Recently Bandaged, carrying the Field Dressing 8pc: the lockout is
// shortened by the millisecond figure the passive carries. Verbatim from
// spell_t1_scorpid_r4's duration handling.
class spell_t2_reset_field_dressing : public AuraScript
{
    PrepareAuraScript(spell_t2_reset_field_dressing);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target)
            return;
        AuraEffect const* passive = target->GetAuraEffect(SPELL_T2_FIELD_DRESSING, EFFECT_0);
        if (!passive)
            return;
        Aura* aura = GetAura();
        if (!aura || aura->GetMaxDuration() <= 0)     // permanent; nothing sane to subtract from
            return;

        // Floored at a second. The subtrahend is dbc data somebody may retune,
        // and a zero-length lockout is an unlimited bandage channel.
        int32 const cut = std::max(1000, aura->GetMaxDuration() - passive->GetAmount());
        aura->SetMaxDuration(cut);
        aura->SetDuration(cut);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_reset_field_dressing::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddSC_custom_t2_rogue_armor()
{
    RegisterSpellScript(spell_t2_icefang_chilling_coat);
    RegisterSpellScript(spell_t2_icefang_sprint_wrapper);
    RegisterSpellScript(spell_t2_icefang_ice_skate);
    RegisterSpellScript(spell_t2_icefang_cold_blood_wrapper);
    RegisterSpellScript(spell_t2_icefang_cold_blood_cd);
    RegisterSpellScript(spell_t2_deadlypoison_leech);
    RegisterSpellScript(spell_t2_deadlypoison_burst);
    RegisterSpellScript(spell_t2_momentum_buff);
    RegisterSpellScript(spell_t2_reset_hearty_appetite);
    RegisterSpellScript(spell_t2_reset_field_dressing);
}
