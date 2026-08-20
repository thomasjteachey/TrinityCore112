/*
 * Legionnaire Plus T2 set bonuses - the warrior and paladin half of the
 * scripted ones.
 *
 * Same shape as custom_t1_set_bonuses.cpp: the plain aura bonuses live entirely
 * in Spell.dbc as passives 90300-90365 and need no code; everything here is a
 * bonus no aura type can express. The carriers are inert dummies the ItemSet
 * grants, and each script either sits on the carrier itself or on the ability
 * it rewrites, checking for the carrier on the caster.
 *
 * Carrier ids, tooltips and the intended hook per bonus are the contract in
 * sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql, amended for this file by
 * sql/custom/dbc/2026_08_19_03_dbc_t2_warrior_paladin.sql; spell_script_names
 * bindings for this file are in
 * sql/custom/world/2026_08_19_03_world_t2_warrior_paladin.sql.
 *
 * 2026-08-19 live-test fixes:
 *   - Gaping Wound is now a 21-stack debuff: every 3 yards the victim moves
 *     (or is moved) pops one stack and pays 1/21 of the pool. No periodic
 *     damage without movement, no per-tick travel clamp. One combat-log line
 *     per yard, and the pool is pre-rounded to a multiple of 21, so every
 *     line is the same number - a tick that covers two yards deals two hits,
 *     it does not deal one double hit.
 *   - Iron Fists / Flurry of Blows are event-driven (T2Unarmed::OnEquipmentChanged
 *     from every Player equipment-change site + the carrier's apply/remove +
 *     login / map change), not a 1 s poll.
 *   - Rattling Blow casts its stun from the script with full diagnostics.
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "Chat.h"
#include "DynamicObject.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellDefines.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "T2UnitHooks.h"
#include "World.h"
#include "WorldSession.h"

#include <cmath>

namespace
{
    enum T2WarriorPaladinSpells
    {
        // warrior - rend set
        SPELL_T2_WALKING_SANCTUARY_RING = 90523,   // unit-attached ring; follows the paladin
        SPELL_T2_GAPING_WOUND_PASSIVE   = 90301,
        SPELL_T2_BRUISING_CHARGE_PASSIVE = 90302,
        SPELL_T2_GAPING_WOUND_BLEED     = 90366,
        SPELL_T2_BRUISING_KNOCKBACK     = 90367,
        // warrior - bare knuckle set
        SPELL_T2_IRON_FISTS_PASSIVE     = 90303,
        SPELL_T2_FLURRY_BLOWS_PASSIVE   = 90304,
        SPELL_T2_RATTLING_BLOW_PASSIVE  = 90305,
        SPELL_T2_IRON_FISTS_BUFF        = 90368,
        SPELL_T2_FLURRY_BLOWS_BUFF      = 90369,
        SPELL_T2_RATTLING_BLOW_STUN     = 90370,
        // paladin - consecration set
        SPELL_T2_SANCTIFIED_CORE_PASSIVE = 90307,
        SPELL_T2_SANCTIFIED_CORE_TICK   = 90371,
        // paladin - holy shock set
        SPELL_T2_SECOND_SHOCK_PASSIVE   = 90311,

        SPELL_WARRIOR_REND_R1           = 772,
        SPELL_PALADIN_CONSECRATION_R1   = 26573,
        SPELL_PALADIN_HOLY_SHOCK_R1     = 20473,
    };

    // Gaping Wound is applied at this many stacks (90366 CumulativeAura must
    // agree - it is the client-visible counter) and pays pool/21 per stack,
    // one stack per whole yard of movement. Both halves of the bonus (the Rend
    // replacement and the bleed's own tick) read it, so it lives here.
    constexpr uint8 GAPING_WOUND_STACKS = 21;

    // Yards of travel that buy ONE stack. 21 stacks x 3 yd = the wound bleeds
    // out over 63 yards of running rather than 21 (user, 2026-08-20). The pool
    // is unchanged, so the same total damage is now spread over three times the
    // distance - each instalment is identical, they just arrive a third as
    // often. 21 yards was roughly one Charge plus a stumble; 63 is a real kite.
    constexpr float GAPING_WOUND_YARDS_PER_STACK = 3.0f;

    // Radius of the "heart" of a Consecration for the 5pc.
    constexpr float SANCTIFIED_CORE_RADIUS = 3.0f;

    // .gm diagnostics customauras - broadcast to every opted-in GM session.
    void SendCustomAuraDiag(std::string const& msg)
    {
        // Also to the log, not just to whoever happens to be watching. A
        // chat-only diagnostic cannot distinguish "the chain never ran" from
        // "no GM had the category enabled", which is precisely how three set
        // bonuses got mis-diagnosed twice. Logger `custom.auras`; free when off.
        TC_LOG_INFO("custom.auras", "{}", msg);

        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // One hand slot is empty when neither the item array nor the visible
    // PLAYER_FIELD_INV_SLOT guid holds anything. Two reads on purpose: the
    // evaluator below is called from inside Player::DestroyItem at a point
    // where (following the HiddenSets/Polearm convention it copies) the slot
    // guid has already been cleared but m_items[slot] has not - reading only
    // GetItemByPos there would report a destroyed weapon as still held until
    // the next equipment change. Every other site (EquipItem/VisualizeItem,
    // RemoveItem) sets or clears both before the hook, so the pair agrees.
    bool IsHandSlotEmpty(Player const* player, uint8 slot)
    {
        return !player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)
            || player->GetGuidValue(PLAYER_FIELD_INV_SLOT_HEAD + (slot * 2)).IsEmpty();
    }

    // "Hands empty" is about the equipment slots, not about weapons:
    // GetWeaponForAttack returns nullptr for a shield as well, so a
    // sword-and-board warrior would have read as bare-fisted in the off hand.
    // A disarmed or broken weapon is still IN the slot and still counts as
    // equipped - the set is about fighting with nothing, not about being
    // unable to use what you hold.
    bool HasEmptyHands(Player const* player)
    {
        return IsHandSlotEmpty(player, EQUIPMENT_SLOT_MAINHAND)
            && IsHandSlotEmpty(player, EQUIPMENT_SLOT_OFFHAND);
    }

    // Pay one instalment of a Gaping Wound. Deliberately NOT the spell bonus
    // pipeline: the pool was fixed when Rend was replaced and re-running
    // SpellDamageBonusDone/Taken over it would scale the whole 15 seconds of
    // Rend by attack power a second time. CalculateSpellDamageTaken only ever
    // takes damage away - armour (waived here, the bleed mechanic gives 90366
    // SPELL_ATTR0_CU_IGNORE_ARMOR), resilience, absorbs - which is what a
    // bleed tick should still respect.
    void DealGapingWoundDamage(Unit* caster, Unit* target, SpellInfo const* spellInfo, uint32 damage)
    {
        SpellNonMeleeDamage log(caster, target, spellInfo->Id, spellInfo->GetSchoolMask());
        log.periodicLog = true;   // also suppresses the "sitting target is always crit" rule
        caster->CalculateSpellDamageTaken(&log, int32(damage), spellInfo, BASE_ATTACK);
        Unit::DealDamageMods(log.target, log.damage, &log.absorb);
        caster->SendSpellNonMeleeDamageLog(&log);

        // DOT, not SPELL_DIRECT_DAMAGE: this is a bleed, and the direct types
        // force the victim to stand and wake their pet on every instalment.
        CleanDamage clean(log.cleanDamage, log.absorb, BASE_ATTACK, MELEE_HIT_NORMAL);
        Unit::DealDamage(caster, target, log.damage, &clean, DOT,
            SpellSchoolMask(log.schoolMask), spellInfo, false);
    }

    // Add/remove one buff to match a wanted state. Idempotent, so it is safe to
    // call from every site that might change the answer. Shipped pattern for
    // state-mirroring buffs (see spell_t1_fury_speed).
    void ToggleBuff(Unit* wearer, uint32 spellId, bool wanted)
    {
        bool const has = wearer->HasAura(spellId);
        if (wanted && !has)
            wearer->CastSpell(wearer, spellId, true);
        else if (!wanted && has)
            wearer->RemoveAurasDueToSpell(spellId);
    }
}

// ===========================================================================
// T2Unarmed - the bare knuckle set's equipment-change fan-out.
//
// Declared in T2UnitHooks.h and called by the core from every Player
// equipment-change site (EquipItem / QuickEquipItem / RemoveItem / DestroyItem
// / the swap paths), following the HiddenSets / PolearmStaffInnerAuras
// convention. Also called from the carriers' own apply/remove hooks below,
// on login and on every map change, so there is no polling anywhere: the
// buffs flip exactly when the answer to "are the hands empty" can change.
//
// State table (evaluated in full every call, so every site is idempotent):
//   90303 on wearer && both hand slots empty          -> 90368 Iron Fists
//   ... && 90304 on wearer                            -> 90369 Flurry of Blows
//   anything else                                     -> neither
// ===========================================================================
namespace T2Unarmed
{
    void OnEquipmentChanged(Player* player)
    {
        if (!player)
            return;

        // Player::_LoadInventory runs before the player is added to its map
        // and the set passives are (re)cast from there. Casting the real
        // buffs on a unit that is not in world yet is not something
        // Spell::prepare is written for (precedent: PolearmStaffInnerAuras
        // skips too). 90368/90369 are saved auras, so a logged-out state is
        // restored by _LoadAuras, and the OnLogin hook below re-evaluates
        // once the player IS in world.
        if (!player->IsInWorld())
            return;

        bool const hasIronFists = player->HasAura(SPELL_T2_IRON_FISTS_PASSIVE);

        // Cheap early-out for the 99% of equipment changes that belong to
        // players without the set and without the buffs: three map lookups.
        if (!hasIronFists
            && !player->HasAura(SPELL_T2_IRON_FISTS_BUFF)
            && !player->HasAura(SPELL_T2_FLURRY_BLOWS_BUFF))
            return;

        bool const empty = hasIronFists && HasEmptyHands(player);
        ToggleBuff(player, SPELL_T2_IRON_FISTS_BUFF, empty);
        ToggleBuff(player, SPELL_T2_FLURRY_BLOWS_BUFF, empty && player->HasAura(SPELL_T2_FLURRY_BLOWS_PASSIVE));
    }
}

// -772 - Rend, carrying the Rend 5pc (90301): the bleed is replaced by Gaping
// Wound (90366), a 21-stack debuff holding twice the damage this rank would
// have dealt over its full duration, paid out one stack per 3 yards the victim
// moves.
class spell_t2_rend_gaping_wound : public SpellScript
{
    PrepareSpellScript(spell_t2_rend_gaping_wound);

    void HandleAura(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_T2_GAPING_WOUND_PASSIVE))
            return;

        // The Rend aura object exists but has not been applied to the target
        // yet (Spell::DoSpellEffectHit sets _spellAura immediately before this
        // hook, Spell::TargetInfo::DoDamageAndTriggers applies it after), so
        // its EFFECT_0 amount is final - attack power, Improved Rend and the
        // above-75%-health bonus are all already folded in by spell_warr_rend -
        // and PreventHitAura below unwinds a bleed nobody ever saw.
        Aura* rend = GetHitAura();
        if (!rend)
            return;
        AuraEffect const* bleed = rend->GetEffect(EFFECT_0);
        if (!bleed)
            return;

        // GetTotalTicks reads the FINAL max duration, which is what we want:
        // Glyph of Rend stretches the aura to 21 sec after the per-tick amount
        // was computed, and those two extra ticks are damage the untouched Rend
        // really would have dealt.
        uint32 const ticks = std::max<uint32>(bleed->GetTotalTicks(), 1);
        int32 const raw = int32(std::max(bleed->GetAmount(), 0)) * int32(ticks) * 2;

        // Round the pool UP to a whole number of stacks. Every yard is billed
        // pool/21, and unless 21 divides the pool exactly that quotient is not
        // an integer: paying it out of a running total - the only scheme in
        // which the 21 instalments still add up to exactly the pool - then
        // makes adjacent yards cost floor() and ceil() of it in an irregular
        // pattern, so the yard price wobbles by 1. Snapping the pool up here
        // costs at most 20 damage over the whole minute and makes every
        // instalment byte-identical, which is what a wearer can actually read
        // off their combat log.
        _pool = int32((int64(raw) + int64(GAPING_WOUND_STACKS) - 1) / int64(GAPING_WOUND_STACKS)) * int32(GAPING_WOUND_STACKS);

        // Both are required. PreventHitAura only calls aura->Remove(); it does NOT
        // set the prevent-default mask, so Spell::HandleEffects would still run
        // EffectApplyAura against the aura we just destroyed. Not a crash here -
        // this fork hardened the upstream ASSERT into a log+return - but it writes
        // a TC_LOG_ERROR with two full GetDebugInfo() dumps into the `spells`
        // channel on every cast, which is the channel the open heap-corruption
        // hunt reads. Shipped precedent: the Blink return script in spell_mage.cpp.
        PreventHitAura();
        PreventHitDefaultEffect(effIndex);
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || _pool <= 0)
            return;

        // Cast from AfterHit rather than out of the effect handler so the whole
        // aura-application pass for Rend has finished first. Rend is
        // single-target, so one _pool per script instance is enough.
        //
        // SPELLVALUE_AURA_STACK: Spell::DoSpellEffectHit applies it as
        // SetStackAmount(21) on a fresh aura and ModStackAmount(+21) on a
        // refresh, which Aura::ModStackAmount clamps to the row's
        // CumulativeAura - so a re-cast always restocks to exactly 21 while
        // Unit::_TryStackingOrRefreshingExistingAura rewrites the base amount
        // to the new pool.
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(_pool);
        args.AddSpellMod(SPELLVALUE_AURA_STACK, GAPING_WOUND_STACKS);
        caster->CastSpell(target, SPELL_T2_GAPING_WOUND_BLEED, args);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Gaping Wound replaces Rend on {} - pool {} over {} stacks ({} per stack, one per 3 yd)",
            caster->GetName(), target->GetName(), _pool, uint32(GAPING_WOUND_STACKS), _pool / GAPING_WOUND_STACKS));
        _pool = 0;
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_t2_rend_gaping_wound::HandleAura, EFFECT_0, SPELL_EFFECT_APPLY_AURA);
        AfterHit += SpellHitFn(spell_t2_rend_gaping_wound::HandleAfterHit);
    }

private:
    int32 _pool = 0;
};

// 90366 - Gaping Wound: a 1 min, 21-stack bleed that only bleeds while its
// victim moves. Ticks four times a second, measures how far the target
// travelled since the last tick (displacement included - knockbacks, Blink,
// Intercept and teleports all count, there is deliberately no per-tick clamp),
// and for every whole 3-yard stride pops one stack and pays 1/21 of the pool. Ends at
// 0 stacks or when the minute runs out.
//
// The tick is a sampler, not the unit of damage: a tick that finds two yards
// of travel deals TWO separate hits of the one yard price rather than one
// double-sized one, so the combat log reads as the same number repeated at a
// rate that tracks the victim's speed. The pool is captured at application
// (aurEff->GetBaseAmount(), never GetAmount() - this fork multiplies effect
// amounts by the stack count, so GetAmount() would shrink as the stacks are
// spent) and pre-rounded to a multiple of 21 by the Rend replacement, so that
// one yard price is the same integer for the whole life of the debuff.
class spell_t2_gaping_wound_tick : public AuraScript
{
    PrepareAuraScript(spell_t2_gaping_wound_tick);

    // Anchor the odometer to wherever the victim is RIGHT NOW. Runs on the
    // initial application and again on every restock (re-cast), so the gap
    // between two casts is never billed as movement. `pool` is the current
    // base amount; `stacks` is whatever the aura holds at that instant.
    void Restock(Unit* target, int32 pool, uint8 stacks)
    {
        _lastPos = target->GetPosition();
        _yards = 0.0f;
        _lastStacks = stacks;
        // What the consumed stacks (if any) are already worth, so the running
        // total stays exact across a restock that did not land on 21.
        _paid = OwedFor(pool, stacks);
    }

    // Total damage owed once the aura is down to `stacks` stacks.
    static int32 OwedFor(int32 pool, uint8 stacks)
    {
        int32 const consumed = int32(GAPING_WOUND_STACKS) - int32(std::min<uint8>(stacks, GAPING_WOUND_STACKS));
        return int32(int64(pool) * consumed / int64(GAPING_WOUND_STACKS));
    }

    void HandleApply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        // Anchor at the moment the debuff lands, not lazily on the first tick
        // (the old build relied on a duration comparison inside the tick to
        // do this, which left the reference point implicit).
        Restock(GetTarget(), aurEff->GetBaseAmount(), GetStackAmount());
        _lastDuration = GetDuration();
    }

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();

        // GetBaseAmount, NOT GetAmount: 90366 carries Rend's own SpellClassMask
        // so that Improved Rend keeps recognising it, which means CalcValue
        // would apply Improved Rend's percentage to the pool - and the pool was
        // built from a Rend amount that already had it. On top of that
        // AuraEffect::CalculateAmount multiplies by the stack count. The base
        // amount is the raw BP0 the replacement passed in, and the core
        // rewrites it on a refresh too (Unit::_TryStackingOrRefreshingExistingAura).
        int32 const pool = aurEff->GetBaseAmount();
        if (pool <= 0)
        {
            SetDuration(0);
            return;
        }

        uint8 const stacks = GetStackAmount();
        int32 const duration = GetDuration();

        // A re-cast REFRESHES this aura instead of replacing it, and no apply
        // hook fires for that. Two independent tells, either is enough:
        // duration only ever grows on a refresh, and stacks only ever grow on
        // a refresh (this script is the only thing that lowers them).
        if (duration > _lastDuration || stacks > _lastStacks)
            Restock(target, pool, stacks);
        _lastDuration = duration;
        _lastStacks = stacks;

        // Horizontal distance only: falling and jumping in place are not
        // "moving" for a bonus whose whole point is punishing a kiting target.
        // Everything else - running, Blink, Intercept, knockbacks, teleports -
        // is movement or displacement and counts in full.
        float const moved = target->GetExactDist2d(&_lastPos);
        _lastPos = target->GetPosition();

        // Accumulate fractional yards so a slow walker is billed exactly as
        // much as a sprinter over the same ground; only WHOLE STRIDES of
        // GAPING_WOUND_YARDS_PER_STACK pop a stack. The remainder is carried,
        // so distance is never lost to rounding across ticks.
        _yards += moved;
        float const strides = std::floor(_yards / GAPING_WOUND_YARDS_PER_STACK);
        if (strides < 1.0f)
            return;
        _yards -= strides * GAPING_WOUND_YARDS_PER_STACK;

        uint8 const pop = uint8(std::min<float>(strides, float(stacks)));
        if (!pop)
            return;

        uint8 const newStacks = stacks - pop;
        _lastStacks = newStacks;

        // The caster may have logged off; the movement still costs stacks, it
        // just cannot be billed without an attacker to book it against.
        Unit* caster = GetCaster();

        // ONE damage event per STRIDE, not one per tick. The instalment is the
        // same number every time; what varies with speed is how many yards a
        // single 250 ms sample covers - a sprinter crosses ~1.75 - and paying
        // the whole sample as one lump is what made the log alternate between
        // one and two (mounted: three and four) times the yard price and read
        // as "variable damage per tick". The event count is bounded by the
        // stack count, so a whole Gaping Wound is at most 21 lines however
        // fast the victim runs - fewer than Rend plus its Deep Wounds - and a
        // teleport that pops every remaining stack at once is one burst and
        // then the debuff is gone.
        for (uint8 i = 0; i < pop; ++i)
        {
            // Running total rather than a flat pool/21, so a restock that did
            // not land on 21 stacks - and any pool that never went through the
            // rounding above, e.g. a hand-cast 90366 - still adds up to
            // exactly the pool.
            uint8 const remaining = uint8(stacks - (i + 1));
            int32 const owed = OwedFor(pool, remaining) - _paid;
            if (owed <= 0)
                continue;

            _paid += owed;

            // Re-checked every yard: the victim can die part-way through a
            // burst, and DealDamage below is what would have killed them.
            if (caster && target->IsAlive())
                DealGapingWoundDamage(caster, target, GetSpellInfo(), uint32(owed));
        }

        if (newStacks > 0)
        {
            // SetStackAmount rather than ModStackAmount: the latter removes
            // the aura outright at 0, and we are inside the owner's aura
            // update loop here. It re-runs CalculateAmount (harmless for a
            // PERIODIC_DUMMY) and flags the application for a client update,
            // which is what makes the counter on the debuff drop.
            SetStackAmount(newStacks);
        }
        else
        {
            // Fires once per spent debuff, only for GMs who opted into the
            // category. `paid` must equal `pool` exactly for a wound that ran
            // from a clean 21 stacks - that is the whole invariant.
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] Gaping Wound on {} spent - paid {} of pool {} over {} yards ({} each)",
                target->GetName(), _paid, pool, uint32(GAPING_WOUND_STACKS), pool / int32(GAPING_WOUND_STACKS)));

            // SetDuration(0) rather than Remove(): Unit::_UpdateSpells sweeps
            // expired auras in a second pass once this loop has finished.
            SetDuration(0);
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_gaping_wound_tick::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_gaping_wound_tick::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    Position _lastPos;
    float _yards = 0.0f;
    int32 _paid = 0;
    int32 _lastDuration = 0;
    uint8 _lastStacks = GAPING_WOUND_STACKS;
};

// 20253 \ 20614 \ 20615 \ 25273 \ 25274 \ 47995 \ 58747 \ 61491 - the Intercept
// stuns, carrying the Rend 8pc (90302): the stun is replaced by a knockback.
//
// TODO(core): the replaced stun still costs the target a stun diminishing-
// returns step. Spell::PreprocessSpellHit calls Unit::IncrDiminishing before
// any script hook can see the cast, so a SpellScript cannot take it back; only
// a core gate on the DR group could. It makes the warrior's own follow-up
// stuns land shorter than they should.
class spell_t2_intercept_knockback : public SpellScript
{
    PrepareSpellScript(spell_t2_intercept_knockback);

    void HandleBeforeHit(SpellMissInfo missInfo)
    {
        // SPELL_MISS_IMMUNE counts as landed on purpose. Spell::PreprocessSpellHit
        // reports immunity when the stun has been diminished to a zero duration,
        // and the replacement knockback is explicitly not diminished - a target
        // who has already eaten two stuns still gets knocked back.
        _landed = missInfo == SPELL_MISS_NONE || missInfo == SPELL_MISS_IMMUNE;
    }

    void HandleStun(SpellEffIndex effIndex)
    {
        // SpellEffectFn can only match the EFFECT name, so the aura name is
        // checked here: the stun does not sit on the same effect index across
        // all eight ids, and prevention must not fire for some other aura the
        // spell happens to carry.
        if (GetEffectInfo().ApplyAuraName != SPELL_AURA_MOD_STUN)
            return;

        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_BRUISING_CHARGE_PASSIVE))
            return;

        // Both are required - see the note in spell_t2_rend_gaping_wound. Removing
        // the aura does not stop EffectApplyAura running against it.
        PreventHitAura();
        PreventHitDefaultEffect(effIndex);
    }

    void HandleAfterHit()
    {
        bool const landed = _landed;
        _landed = false;

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!landed || !caster || !target || !caster->HasAura(SPELL_T2_BRUISING_CHARGE_PASSIVE))
            return;

        caster->CastSpell(target, SPELL_T2_BRUISING_KNOCKBACK, true);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_t2_intercept_knockback::HandleBeforeHit);
        // EFFECT_ALL, not EFFECT_FIRST_FOUND: the stun is not always the first
        // aura effect on these eight rows, and FIRST_FOUND would bind to the
        // wrong one and then silently do nothing.
        OnEffectHitTarget += SpellEffectFn(spell_t2_intercept_knockback::HandleStun, EFFECT_ALL, SPELL_EFFECT_APPLY_AURA);
        AfterHit += SpellHitFn(spell_t2_intercept_knockback::HandleAfterHit);
    }

private:
    bool _landed = false;
};

// 90303 - Iron Fists (bare knuckle 3pc). The carrier is a plain DUMMY now; all
// the state logic is T2Unarmed::OnEquipmentChanged above. This script only
// makes sure that evaluator also runs when the CARRIER comes and goes - the
// set being completed/broken is an equipment change the core sites do not
// phrase as one - and that losing the carrier strips both buffs.
//
// Login is covered: the carrier is a passive, so it is never saved and is
// re-cast by the set machinery on every login, which lands here. If that
// happens before the player is in world the evaluator defers to the OnLogin
// hook below (and the buffs themselves ARE saved auras in the meantime).
class spell_t2_bareknuckle_state : public AuraScript
{
    PrepareAuraScript(spell_t2_bareknuckle_state);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            T2Unarmed::OnEquipmentChanged(player);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        // The aura is already out of the applied list at this point (Unit::
        // _UnapplyAura erases first, then runs effect handlers), so the
        // evaluator sees "no Iron Fists" and strips both buffs. Done by hand
        // anyway so the contract does not depend on that ordering.
        Unit* wearer = GetTarget();
        wearer->RemoveAurasDueToSpell(SPELL_T2_IRON_FISTS_BUFF);
        wearer->RemoveAurasDueToSpell(SPELL_T2_FLURRY_BLOWS_BUFF);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_bareknuckle_state::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_bareknuckle_state::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90304 - Flurry of Blows (bare knuckle 5pc). Inert flag; gaining or losing it
// is the only thing besides the hands that changes the 90369 answer, so it
// re-runs the same evaluator. Removal is handled by the evaluator too (no
// 90304 -> no 90369), no hand-stripping needed.
class spell_t2_bareknuckle_flurry_flag : public AuraScript
{
    PrepareAuraScript(spell_t2_bareknuckle_flurry_flag);

    void HandleChange(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            T2Unarmed::OnEquipmentChanged(player);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_bareknuckle_flurry_flag::HandleChange, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_bareknuckle_flurry_flag::HandleChange, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Login re-evaluation for the bare knuckle set. Player::_LoadInventory re-casts
// the set passives before the player is in world, where the evaluator
// (correctly) refuses to cast; this runs once the player is on its map.
class t2_unarmed_login : public PlayerScript
{
public:
    t2_unarmed_login() : PlayerScript("t2_unarmed_login") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        T2Unarmed::OnEquipmentChanged(player);
    }

    // Map changes are the other place the buffs can vanish without an
    // equipment change: Player::TeleportTo runs Unit::RemoveArenaAuras() on
    // entry to any battleground-or-arena map (GMs exempt). 90368/90369 now
    // carry SPELL_ATTR4_DONT_REMOVE_IN_ARENA so that particular sweep spares
    // them, but the evaluator is idempotent and cheap for everyone without
    // the set, so re-running it here makes any future bulk strip self-heal
    // instead of waiting for the next equipment change.
    //
    // ScriptMgr::OnPlayerEnterMap fires this from Map::AddPlayerToMap after
    // player->AddToWorld(), outside any aura iteration, so the evaluator's
    // IsInWorld gate passes and casting here is safe.
    void OnMapChanged(Player* player) override
    {
        T2Unarmed::OnEquipmentChanged(player);
    }
};

// 90305 - Rattling Blow (bare knuckle 8pc): melee crits stun for 1 sec while
// the hands are empty. The proc FILTER is data (world.spell_proc: ProcFlags
// 0x14 melee auto-attack + melee-class spells, SpellPhaseMask HIT, HitMask
// CRITICAL) and the carrier is a PROC_TRIGGER_SPELL -> 90370; this script
// guards the hands and, since 2026-08-19, performs the stun cast itself
// (PreventDefaultAction + explicit cast on the proc target) so that every
// rejection and every cast result is visible under .gm diagnostics
// customauras. Functionally identical to the core's
// HandleProcTriggerSpellAuraProc, but observable.
//
// Known non-bugs that look like "not working":
//   - 90370 is Mechanic 12 (stun) and is cast as triggered, so it sits in the
//     triggered-stun diminishing group (DRTYPE_ALL - creatures too): 1.0 s,
//     0.5 s, 0.25 s, then IMMUNE until 15 s pass with no stun on the target.
//     In continuous melee the third crit of a fight is the last one that
//     stuns. The diag line below prints the resulting duration so this is
//     not mistaken for a dead proc. If a raw repeatable 1 s stun is wanted,
//     set 90370 Mechanic to 0 in the DBC (one-line change, design call).
//   - Target dummies and other stun-immune creatures take no stun at all
//     (SPELL_MISS_IMMUNE). Test on a player or a regular mob.
class spell_t2_bareknuckle_stun : public AuraScript
{
    PrepareAuraScript(spell_t2_bareknuckle_stun);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return false;

        if (!HasEmptyHands(player))
        {
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Rattling Blow - crit ignored, hands not empty", player->GetName()));
            return false;
        }

        Unit* victim = eventInfo.GetProcTarget();
        if (!victim || victim == player || !victim->IsAlive())
            return false;

        return true;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        // Cast by hand so the result is observable; the core would otherwise
        // do exactly this in AuraEffect::HandleProcTriggerSpellAuraProc.
        PreventDefaultAction();

        Unit* warrior = GetTarget();
        Unit* victim = eventInfo.GetProcTarget();
        if (!warrior || !victim)
            return;

        SpellCastResult const result = warrior->CastSpell(victim, SPELL_T2_RATTLING_BLOW_STUN, aurEff);

        int32 landedMs = 0;
        if (Aura const* stun = victim->GetAura(SPELL_T2_RATTLING_BLOW_STUN, warrior->GetGUID()))
            landedMs = stun->GetDuration();

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Rattling Blow crit on {} - cast result {}, stun on target {} ms{}",
            warrior->GetName(), victim->GetName(), uint32(result), landedMs,
            (result == SPELL_CAST_OK && landedMs <= 0) ? " (immune or diminished to 0 - DR/immunity, not a dead proc)" : ""));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_bareknuckle_stun::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_bareknuckle_stun::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// -26573 - Consecration, carrying the Consecration 5pc (90307): enemies inside
// the middle 3 yards take the tick twice.
class spell_t2_consecration_core : public AuraScript
{
    PrepareAuraScript(spell_t2_consecration_core);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target || !caster->HasAura(SPELL_T2_SANCTIFIED_CORE_PASSIVE))
            return;

        // Consecration's aura is owned by the ground dynobject, not by the
        // paladin, so the ring's centre is the dynobject's position - the
        // paladin may have walked off it long ago.
        DynamicObject* dynObj = GetDynobjOwner();
        if (!dynObj)
            return;

        // GetExactDist2d, not GetDistance2d: the latter subtracts both objects'
        // combat reach, which on a 3 yard test would let a tauren stand a
        // metre outside the core and still count.
        if (dynObj->GetExactDist2d(target) > SANCTIFIED_CORE_RADIUS)
            return;

        // A second explicit hit rather than doubling the tick amount: the tick
        // amount is shared by every target of the dynobject aura, and this way
        // the combat log shows what the player is actually taking.
        int32 const extra = std::max(aurEff->GetAmount(), 0);
        if (extra <= 0)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(extra);   // 90371 has DieSides 0, so this is used exactly
        caster->CastSpell(target, SPELL_T2_SANCTIFIED_CORE_TICK, args);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_consecration_core::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// 90308 - Walking Sanctuary (Consecration 8pc): drags the paladin's
// Consecration dynobject along with them twice a second. DynObjAura::FillTargetMap
// picks its victims from the dynobject's position, so moving the object is the
// whole server-side bonus.
//
// The ring DOES follow on screen since 2026-08-20. A dynobject's position only
// reaches the client in its create block and 3.3.5 has no relocation opcode for
// one, so the drawn ring can never move - but it never had to. Consecration's
// art (SpellVisualKit 9366) hangs off field 5, baseEffect, which is a UNIT
// ATTACHMENT slot; it is only parented to a dynobject by accident of being a
// PersistentAreaKit. 90523 "Walking Sanctuary" is an aura on the PALADIN whose
// SpellVisual 21101 puts that same kit in the StateKit slot, so the client
// parents the ring to the paladin's model and it follows smoothly, for free,
// with no per-tick server work. This periodic still moves the dynobject because
// that is what actually picks the victims.
class spell_t2_walking_consecration : public AuraScript
{
    PrepareAuraScript(spell_t2_walking_consecration);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* paladin = GetTarget()->ToPlayer();
        if (!paladin || !paladin->IsInWorld())
            return;

        Map* map = paladin->GetMap();
        if (!map)
            return;

        // Keep the ring aura up for exactly as long as a Consecration is out.
        // Refreshed from the poll rather than applied once at cast, so it can
        // never outlive the dynobject (a dispel, a death, a zone change) and
        // leave a paladin wearing consecrated ground that does nothing.
        bool anyConsecration = false;

        for (uint32 rank = SPELL_PALADIN_CONSECRATION_R1; rank; rank = sSpellMgr->GetNextSpellInChain(rank))
        {
            for (DynamicObject* dynObj : paladin->GetDynObjects(rank))
            {
                if (!dynObj->IsInWorld() || dynObj->GetMap() != map)
                    continue;

                anyConsecration = true;

                // Map::DynamicObjectRelocation asserts the object's cell matches
                // its position, and a no-op relocation is not free, so skip the
                // call while the paladin has not actually moved.
                if (dynObj->GetExactDist2d(paladin) < 0.1f)
                    continue;

                // Safe from here: cross-cell moves are queued onto the map's
                // move list and applied after the object update pass.
                map->DynamicObjectRelocation(dynObj, paladin->GetPositionX(),
                    paladin->GetPositionY(), paladin->GetPositionZ(), dynObj->GetOrientation());
            }
        }

        if (anyConsecration)
        {
            if (!paladin->HasAura(SPELL_T2_WALKING_SANCTUARY_RING))
                paladin->CastSpell(paladin, SPELL_T2_WALKING_SANCTUARY_RING, true);
        }
        else if (paladin->HasAura(SPELL_T2_WALKING_SANCTUARY_RING))
            paladin->RemoveAurasDueToSpell(SPELL_T2_WALKING_SANCTUARY_RING);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_walking_consecration::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 90311 - Second Shock (Holy Shock 8pc): a critical Holy Shock resets its own
// cooldown. The once-per-10-sec limit is world.spell_proc Cooldown, and the
// crit requirement is its HitMask - re-checked here so that a missing or wrong
// proc row cannot turn this into a free-cast button.
class spell_t2_second_shock : public AuraScript
{
    PrepareAuraScript(spell_t2_second_shock);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!info)
            return false;

        // Holy Shock's damage (25912) and heal (25914) spells both carry the
        // 0x200000 family flag, which is how the shipped Infusion of Light
        // script recognises them - the parent 20473 does no damage of its own.
        if (info->SpellFamilyName != SPELLFAMILY_PALADIN || !(info->SpellFamilyFlags[0] & 0x200000))
            return false;

        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;

        return GetTarget()->GetTypeId() == TYPEID_PLAYER;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        // DUMMY carrier with TriggerSpell 0 - see the note on Cinderbite's melt
        // in custom_t2_druid_hunter.cpp. Omitting this logs a TC_LOG_ERROR on
        // every successful proc.
        PreventDefaultAction();

        Player* paladin = GetTarget()->ToPlayer();
        if (!paladin)
            return;

        SpellHistory* history = paladin->GetSpellHistory();
        if (!history)
            return;

        // Spell::SendSpellCooldown runs before the effects, so by the time the
        // triggered damage/heal procs this, the cooldown of the rank that was
        // cast is already on the books. Which rank that is is unknown here -
        // the proc carries the triggered spell, not its parent - so walk the
        // chain. ResetCooldown is a no-op for a rank with no entry, and the one
        // that hits also drops the shared category (SpellHistory::EraseCooldown).
        for (uint32 rank = SPELL_PALADIN_HOLY_SHOCK_R1; rank; rank = sSpellMgr->GetNextSpellInChain(rank))
            history->ResetCooldown(rank, true);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Second Shock - Holy Shock crit, cooldown reset",
            paladin->GetName()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_second_shock::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_second_shock::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

void AddSC_custom_t2_warrior_paladin()
{
    RegisterSpellScript(spell_t2_rend_gaping_wound);
    RegisterSpellScript(spell_t2_gaping_wound_tick);
    RegisterSpellScript(spell_t2_intercept_knockback);
    RegisterSpellScript(spell_t2_bareknuckle_state);
    RegisterSpellScript(spell_t2_bareknuckle_flurry_flag);
    RegisterSpellScript(spell_t2_bareknuckle_stun);
    RegisterSpellScript(spell_t2_consecration_core);
    RegisterSpellScript(spell_t2_walking_consecration);
    RegisterSpellScript(spell_t2_second_shock);
    new t2_unarmed_login();
}
