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
 * sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql; spell_script_names
 * bindings for this file are in
 * sql/custom/world/2026_08_17_01_world_t2_warrior-paladin.sql.
 */

#include "ScriptMgr.h"
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
#include "World.h"
#include "WorldSession.h"

namespace
{
    enum T2WarriorPaladinSpells
    {
        // warrior - rend set
        SPELL_T2_GAPING_WOUND_PASSIVE   = 90301,
        SPELL_T2_BRUISING_CHARGE_PASSIVE = 90302,
        SPELL_T2_GAPING_WOUND_BLEED     = 90366,
        SPELL_T2_BRUISING_KNOCKBACK     = 90367,
        // warrior - bare knuckle set
        SPELL_T2_IRON_FISTS_PASSIVE     = 90303,
        SPELL_T2_FLURRY_BLOWS_PASSIVE   = 90304,
        SPELL_T2_IRON_FISTS_BUFF        = 90368,
        SPELL_T2_FLURRY_BLOWS_BUFF      = 90369,
        // paladin - consecration set
        SPELL_T2_SANCTIFIED_CORE_PASSIVE = 90307,
        SPELL_T2_SANCTIFIED_CORE_TICK   = 90371,
        // paladin - holy shock set
        SPELL_T2_SECOND_SHOCK_PASSIVE   = 90311,

        SPELL_WARRIOR_REND_R1           = 772,
        SPELL_PALADIN_CONSECRATION_R1   = 26573,
        SPELL_PALADIN_HOLY_SHOCK_R1     = 20473,
    };

    // Gaping Wound pays its whole pool out over this much movement. Both halves
    // of the bonus (the Rend replacement and the bleed's own tick) have to agree
    // on it, so it lives here rather than in either script.
    constexpr float GAPING_WOUND_YARDS = 21.0f;

    // A single 500 ms tick can only credit this much travel. Blink, Intercept,
    // knockbacks and teleports all move a player further than any run speed
    // could in half a second, and without the clamp one Blink dumps the entire
    // pool in a single tick.
    constexpr float GAPING_WOUND_MAX_STEP = 10.0f;

    // Radius of the "heart" of a Consecration for the 5pc.
    constexpr float SANCTIFIED_CORE_RADIUS = 3.0f;

    // .gm diagnostics customauras - broadcast to every opted-in GM session.
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // "Hands empty" is about the equipment slots, not about weapons:
    // GetWeaponForAttack returns nullptr for a shield as well, so a
    // sword-and-board warrior would have read as bare-fisted in the off hand.
    bool HasEmptyHands(Player const* player)
    {
        return !player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)
            && !player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
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
}

// -772 - Rend, carrying the Rend 5pc (90301): the bleed is replaced by Gaping
// Wound (90366), which holds twice the damage this rank would have dealt over
// its full duration and pays it out only as the victim moves.
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
        _pool = int32(std::max(bleed->GetAmount(), 0)) * int32(ticks) * 2;

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
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(_pool);
        caster->CastSpell(target, SPELL_T2_GAPING_WOUND_BLEED, args);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Gaping Wound replaces Rend on {} - pool {} over {:.0f} yd",
            caster->GetName(), target->GetName(), _pool, GAPING_WOUND_YARDS));
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

// 90366 - Gaping Wound: a 1 min bleed that only bleeds while its victim moves.
// Ticks twice a second, measures how far the target travelled since the last
// tick, and pays the pool out in proportion, finishing after 21 yards.
class spell_t2_gaping_wound_tick : public AuraScript
{
    PrepareAuraScript(spell_t2_gaping_wound_tick);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();

        // GetBaseAmount, NOT GetAmount: 90366 carries Rend's own SpellClassMask
        // so that Improved Rend keeps recognising it, which means CalcValue
        // would apply Improved Rend's percentage to the pool - and the pool was
        // built from a Rend amount that already had it. GetBaseAmount is the
        // raw BP0 the replacement passed in, and the core rewrites it on a
        // refresh too (Unit::_TryStackingOrRefreshingExistingAura).
        int32 const pool = aurEff->GetBaseAmount();
        if (pool <= 0)
        {
            SetDuration(0);
            return;
        }

        // A re-cast REFRESHES this aura instead of replacing it, and no apply
        // hook fires for that, so the paid-out counters would survive into the
        // new bleed. Duration only ever grows on a refresh, so that is the
        // signal to restock - and to re-anchor the position, since the gap
        // between the old and new cast is not movement this bleed paid for.
        int32 const duration = GetDuration();
        if (duration > _lastDuration)
        {
            _paid = 0;
            _yards = 0.0f;
            _lastPos = target->GetPosition();
        }
        _lastDuration = duration;

        // Horizontal distance only: falling and jumping in place are not
        // "moving" for a bonus whose whole point is punishing a kiting target.
        float const moved = target->GetExactDist2d(&_lastPos);
        _lastPos = target->GetPosition();
        if (moved > GAPING_WOUND_MAX_STEP)
            return;

        Unit* caster = GetCaster();
        if (!caster || !target->IsAlive())
            return;

        // Accumulate distance and bill against a running total rather than
        // paying per tick: rounding a fraction of the pool down every half
        // second would quietly lose most of a slow walker's bleed.
        _yards += moved;
        float const progress = std::min(_yards / GAPING_WOUND_YARDS, 1.0f);
        int32 const owed = int32(float(pool) * progress) - _paid;
        if (owed <= 0)
            return;

        _paid += owed;
        DealGapingWoundDamage(caster, target, GetSpellInfo(), uint32(owed));

        // SetDuration(0) rather than Remove(): we are inside the owner's aura
        // update loop here, and Unit::_UpdateSpells sweeps expired auras in a
        // second pass once that loop has finished.
        if (_paid >= pool)
            SetDuration(0);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_gaping_wound_tick::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    Position _lastPos;
    float _yards = 0.0f;
    int32 _paid = 0;
    int32 _lastDuration = 0;
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

// 90303 - Iron Fists (bare knuckle 3pc): polls the hands once a second and
// toggles the real buffs. "While your hands are empty" is a state no aura
// condition can express, so it has to be watched; the same tick also drives the
// 5pc (90304), which is an inert flag with no poll of its own.
class spell_t2_bareknuckle_state : public AuraScript
{
    PrepareAuraScript(spell_t2_bareknuckle_state);

    // Add/remove one buff to match a wanted state. Called from inside the
    // owner's own aura update: Unit::RemoveOwnedAura advances
    // m_auraUpdateIterator past an entry it erases and AuraEffect::Update ticks
    // a snapshot of the application list, which is what makes this the shipped
    // pattern for polled buffs (see spell_t1_fury_speed).
    static void Toggle(Unit* wearer, uint32 spellId, bool wanted)
    {
        bool const has = wearer->HasAura(spellId);
        if (wanted && !has)
            wearer->CastSpell(wearer, spellId, true);
        else if (!wanted && has)
            wearer->RemoveAurasDueToSpell(spellId);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        bool const empty = HasEmptyHands(player);
        Toggle(player, SPELL_T2_IRON_FISTS_BUFF, empty);
        Toggle(player, SPELL_T2_FLURRY_BLOWS_BUFF, empty && player->HasAura(SPELL_T2_FLURRY_BLOWS_PASSIVE));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* wearer = GetTarget();
        wearer->RemoveAurasDueToSpell(SPELL_T2_IRON_FISTS_BUFF);
        wearer->RemoveAurasDueToSpell(SPELL_T2_FLURRY_BLOWS_BUFF);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_bareknuckle_state::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_bareknuckle_state::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90305 - Rattling Blow (bare knuckle 8pc): crits stun for 1 sec. The crit
// filter and the trigger are both data (world.spell_proc HitMask 2 and the
// carrier's own PROC_TRIGGER_SPELL to 90370); this only guards the hands.
class spell_t2_bareknuckle_stun : public AuraScript
{
    PrepareAuraScript(spell_t2_bareknuckle_stun);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        Player* player = GetTarget()->ToPlayer();
        return player && HasEmptyHands(player);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_bareknuckle_stun::CheckProc);
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
// TODO(client): the ring does not follow on screen. A dynobject's position only
// reaches the client in its create block, and there is no relocation opcode for
// one in 3.3.5, so the visual half is client work and is not covered here.
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

        for (uint32 rank = SPELL_PALADIN_CONSECRATION_R1; rank; rank = sSpellMgr->GetNextSpellInChain(rank))
        {
            for (DynamicObject* dynObj : paladin->GetDynObjects(rank))
            {
                if (!dynObj->IsInWorld() || dynObj->GetMap() != map)
                    continue;

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
    RegisterSpellScript(spell_t2_bareknuckle_stun);
    RegisterSpellScript(spell_t2_consecration_core);
    RegisterSpellScript(spell_t2_walking_consecration);
    RegisterSpellScript(spell_t2_second_shock);
}
