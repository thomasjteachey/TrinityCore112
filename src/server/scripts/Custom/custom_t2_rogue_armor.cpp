/*
 * Legionnaire Plus T2 set bonuses - the rogue-armour group.
 *
 * Carriers 90348-90365 and helpers 90388-90394 are Spell.dbc rows from
 * sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql; the 2026-08-19 rework
 * adds 90510-90520 (sql/custom/dbc/2026_08_19_07_dbc_t2_rogue_armor.sql),
 * which is the contract this file implements.
 *
 * Sets covered: rogue-ice-fang (90348/90349/90350), rogue-deadly-poison
 * (90352/90353), mail-momentum (90359's half of the 90392 buff),
 * leather-mister-reset (90360/90362).
 *
 * ICE FANG uses the REPLACEMENT-WRAPPER pattern (spell_warr_disarm_wrapper in
 * Spells/spell_warrior.cpp): the rogue no longer learns Crippling Poison /
 * Sprint / Cold Blood directly. They learn a DUMMY wrapper with the same cost,
 * cast time, cooldown, family and tooltip, and the wrapper script casts either
 * the untouched original or the set-bonus alternate, TRIGGERED, based on the
 * carrier aura. The originals are never edited, so a rogue without the set
 * plays exactly as before.
 *
 * Momentum (90358) is NOT here any more: the straight-line tracker lives in
 * the movement handler (T2UnitHooks.cpp). Only the 8pc gate on the 90392 buff
 * remains in this file. Feint Cadence / Dead Air are core (T2SpellHooks).
 *
 * Structure, naming and idioms follow custom_t1_set_bonuses.cpp.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
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

        // 2026-08-19 rework: wrappers, alternates and helpers (90510-90520)
        SPELL_T2_CRIPPLING_WRAPPER_R1   = 90510,   // learned instead of 3408
        SPELL_T2_CRIPPLING_WRAPPER_R2   = 90511,   // learned instead of 11202
        SPELL_T2_CHILLING_POISON_COAT   = 90512,   // clone of 3408, enchant 3889
        SPELL_T2_CHILLING_POISON_PROC   = 90513,   // clone of 3409: -55% move, -15% attack speed
        SPELL_T2_SPRINT_WRAPPER_R1      = 90514,   // learned instead of 2983
        SPELL_T2_SPRINT_WRAPPER_R2      = 90515,   // learned instead of 8696
        SPELL_T2_SPRINT_WRAPPER_R3      = 90516,   // learned instead of 11305
        SPELL_T2_ICE_SKATE              = 90517,   // +45% for 18 s, effect 2 drives the trail
        SPELL_T2_ICE_TRAIL_PATCH        = 90518,   // persistent area aura, the visible slick
        SPELL_T2_COLD_BLOOD_WRAPPER     = 90519,   // learned instead of 14177
        SPELL_T2_VENOM_SUSTENANCE_HEAL  = 90520,   // SPELL_EFFECT_HEAL, bp at runtime

        // the stock abilities the wrappers hand off to
        SPELL_ROGUE_CRIPPLING_POISON_R1 = 3408,
        SPELL_ROGUE_CRIPPLING_POISON_R2 = 11202,
        SPELL_ROGUE_SPRINT_R1           = 2983,
        SPELL_ROGUE_SPRINT_R2           = 8696,
        SPELL_ROGUE_SPRINT_R3           = 11305,
        SPELL_ROGUE_COLD_BLOOD          = 14177,
    };

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
// ICE FANG - replacement wrappers
// ===========================================================================

// 90510 \ 90511 - Crippling Poison wrappers (ranks 1 and 2). The wrapper is
// an item-targeted 3 s cast exactly like 3408/11202; on completion it coats
// the chosen weapon with either the rank's own Crippling Poison or, for a
// rogue wearing the 3pc, Chilling Poison (90512 -> enchant 3889 -> proc 90513).
//
// OnEffectHit rather than OnEffectHitTarget: a DUMMY effect adds no targets of
// its own (EFFECT_IMPLICIT_TARGET_NONE in SpellInfo's effect table) and the
// wrapper keeps the original's ImplicitTargetA 0, so the per-target hook would
// never fire. The item comes from the explicit cast targets instead.
class spell_t2_icefang_crippling_wrapper : public SpellScript
{
    PrepareSpellScript(spell_t2_icefang_crippling_wrapper);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_CHILLING_POISON_COAT, SPELL_ROGUE_CRIPPLING_POISON_R1, SPELL_ROGUE_CRIPPLING_POISON_R2 });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Item* weapon = GetExplTargetItem();
        if (!caster || !weapon)
            return;

        uint32 coat;
        if (caster->HasAura(SPELL_T2_CHILLING_POISON))
            coat = SPELL_T2_CHILLING_POISON_COAT;
        else
            coat = GetSpellInfo()->Id == SPELL_T2_CRIPPLING_WRAPPER_R2 ? SPELL_ROGUE_CRIPPLING_POISON_R2 : SPELL_ROGUE_CRIPPLING_POISON_R1;

        // Triggered: no second cast time, cost or cooldown - the wrapper paid
        // all three. The coat spell's own CheckItems still runs against the
        // weapon (TRIGGERED_FULL_MASK does not skip item checks).
        caster->CastSpell(weapon, coat, true);
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_t2_icefang_crippling_wrapper::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
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

// 90519 - Cold Blood wrapper. Without the 8pc it casts the real Cold Blood
// 14177 (crit guarantee, consumed by the next finisher, 3 min cooldown that
// starts when the buff is used). With Colder Blood (90350) it instead roots
// the current selection with 90391 and the 3 min cooldown starts at once.
//
// Cooldown plumbing, because 14177 is SPELL_ATTR0_DISABLED_WHILE_ACTIVE:
// SpellHistory::HandleCooldowns skips on-event spells entirely, and the held /
// released cooldown is driven from Aura::_ApplyForTarget / _UnapplyForTarget
// of the AURA's spell - which is 14177, not the wrapper. So the wrapper row
// carries the same attribute (the client then waits for SMSG_COOLDOWN_EVENT
// instead of starting a local timer), and:
//   * spell_t2_icefang_cold_blood_cd on 14177 puts the WRAPPER on hold when
//     the buff is applied and releases it (event + live 3 min) when the buff
//     goes away - exactly what the core does for 14177 itself;
//   * the root branch has no buff, so the wrapper releases itself right away.
// Nothing here touches the caster's aura containers.
class spell_t2_icefang_cold_blood_wrapper : public SpellScript
{
    PrepareSpellScript(spell_t2_icefang_cold_blood_wrapper);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_ROGUE_COLD_BLOOD, SPELL_T2_COLDER_BLOOD_ROOT });
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
            rogue->CastSpell(rogue, SPELL_ROGUE_COLD_BLOOD, true);
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

// 14177 - Cold Blood, mirroring its on-event cooldown onto the wrapper the
// rogue actually has on the bar. Apply: hold. Remove (consumed, expired,
// dispelled, death, logout cleanup): cooldown event, which the client turns
// into the 3 min timer on the wrapper and the server into a live cooldown.
// This is the same pair of calls Aura::_ApplyForTarget/_UnapplyForTarget make
// for 14177 itself, so the two stay in lockstep (and Preparation, which resets
// every rogue-family cooldown, clears both).
class spell_t2_icefang_cold_blood_cd : public AuraScript
{
    PrepareAuraScript(spell_t2_icefang_cold_blood_cd);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_COLD_BLOOD_WRAPPER });
    }

    Player* WrapperOwner() const
    {
        Unit* caster = GetCaster();
        Player* rogue = caster ? caster->ToPlayer() : nullptr;
        if (!rogue || !rogue->HasSpell(SPELL_T2_COLD_BLOOD_WRAPPER))
            return nullptr;
        return rogue;
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* rogue = WrapperOwner())
            rogue->GetSpellHistory()->StartCooldown(sSpellMgr->AssertSpellInfo(SPELL_T2_COLD_BLOOD_WRAPPER), 0, nullptr, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* rogue = WrapperOwner())
            rogue->GetSpellHistory()->SendCooldownEvent(sSpellMgr->AssertSpellInfo(SPELL_T2_COLD_BLOOD_WRAPPER));
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
    RegisterSpellScript(spell_t2_icefang_crippling_wrapper);
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
