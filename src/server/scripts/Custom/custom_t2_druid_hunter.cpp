/*
 * Legionnaire Plus T2 set bonuses - the druid and hunter half of the scripted
 * ones (moonkitten 90327/90328/90329, monkey stalker 90332, penguin stalker
 * 90333/90334/90335).
 *
 * Same shape as custom_t1_set_bonuses.cpp: the plain bonuses are pure Spell.dbc
 * passives and need no code, and everything here is a bonus no aura type can
 * express. Each carrier is an inert dummy the ItemSet grants; the scripts
 * either sit on that dummy or on the ability it modifies and look for the
 * dummy on the caster.
 *
 * Revision 2026-08-19 (after the user's live test):
 *   - beef's regalia (90324-90326 / 90376) is GONE from here: the fork already
 *     has the Hurricane boons in core (89767 Beef's Focus, 89760 Beef's
 *     Mobility, 89766 Beef's Tenacity keyed in Spell.cpp).
 *   - Moonlit Wound / Moonlit Prey are now invisible, undispellable riders tied
 *     to the Mangle / Moonfire aura they belong to, built exactly like the T1
 *     Exposing Mark (90138) rider on Hunter's Mark.
 *   - Feline Grace is a scripted "hidden real buff" keyed on Moonkin Form.
 *   - Simian Frenzy gets its Aspect of the Monkey gate here (its proc row is in
 *     the world SQL).
 *   - Cinderbite uses the replacement-wrapper pattern (see
 *     spell_warr_disarm_wrapper): the player learns a DUMMY wrapper per Serpent
 *     Sting rank which casts either the stock rank or its Fire clone, and the
 *     trap melt spawns a real Frost Trap slick (13810) where the freeze broke.
 *
 * Wiring lives in ItemSet.dbc (SetSpellID/SetThreshold); spell_script_names
 * binds these classes to their spell ids - see
 * sql/custom/world/2026_08_19_05_world_t2_druid_hunter.sql, which also
 * carries the spell_proc / spell_ranks rows these scripts need.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "DynamicObject.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
    enum T2DruidHunterSpells
    {
        // moonkitten (druid)
        SPELL_T2_MOONLIT_WOUND          = 90327,   // 3pc carrier (inert dummy)
        SPELL_T2_MOONLIT_WOUND_DEBUFF   = 90377,   // +5% Balance damage taken, rides the Mangle debuff
        SPELL_T2_PROWLING_MOONFIRE      = 90328,   // 5pc carrier (inert dummy)
        SPELL_T2_MOONLIT_PREY           = 90378,   // +5% melee damage taken, rides the Moonfire DoT
        SPELL_T2_FELINE_GRACE           = 90329,   // 8pc carrier (inert dummy)
        SPELL_T2_FELINE_GRACE_HELPER    = 90484,   // hidden: Cat Form costs -100% (lives while in Moonkin Form)
        SPELL_DRUID_MOONFIRE            = 8921,    // rank 1, for the ranked lookup
        SPELL_DRUID_MOONKIN_FORM        = 24858,

        // monkey stalker (hunter)
        SPELL_T2_SIMIAN_FRENZY          = 90332,   // 8pc carrier (proc -> 90379)
        SPELL_HUNTER_ASPECT_OF_MONKEY   = 13163,

        // penguin stalker (hunter)
        SPELL_T2_COLD_EFFICIENCY        = 90333,   // 3pc carrier
        SPELL_T2_CINDERBITE             = 90334,   // 5pc carrier (also the melt proc aura)
        SPELL_T2_SLICK_GETAWAY          = 90335,   // 8pc carrier (periodic dummy)
        SPELL_T2_SLICK_GETAWAY_SPRINT   = 90382,   // the 3 s sprint

        SPELL_HUNTER_FROST_TRAP_AURA    = 13810,   // the ice slick's own spell
    };

    // Serpent Sting: one learnable DUMMY wrapper per rank, casting either the
    // stock rank or its Fire clone (same damage / duration / family mask,
    // SchoolMask fire, Explosive Shot's missile). Ids verified against
    // Spell.dbc; the wrapper/clone rows are in
    // sql/custom/dbc/2026_08_19_05_dbc_t2_druid_hunter.sql.
    struct SerpentStingRank
    {
        uint32 Wrapper;
        uint32 Original;
        uint32 FireClone;
    };

    constexpr SerpentStingRank SERPENT_STING_RANKS[] =
    {
        { 90460,  1978, 90472 },   // Rank 1
        { 90461, 13549, 90473 },   // Rank 2
        { 90462, 13550, 90474 },   // Rank 3
        { 90463, 13551, 90475 },   // Rank 4
        { 90464, 13552, 90476 },   // Rank 5
        { 90465, 13553, 90477 },   // Rank 6
        { 90466, 13554, 90478 },   // Rank 7
        { 90467, 13555, 90479 },   // Rank 8
        { 90468, 25295, 90480 },   // Rank 9
        { 90469, 27016, 90481 },   // Rank 10
        { 90470, 49000, 90482 },   // Rank 11
        { 90471, 49001, 90483 },   // Rank 12
    };

    SerpentStingRank const* FindSerpentStingRank(uint32 wrapperId)
    {
        for (SerpentStingRank const& rank : SERPENT_STING_RANKS)
            if (rank.Wrapper == wrapperId)
                return &rank;
        return nullptr;
    }

    // .gm diagnostics customauras - broadcast to every opted-in GM session
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // A rider is an invisible, undispellable debuff that belongs to a parent
    // aura (Exposing Mark 90138 on Hunter's Mark is the shipped model): it is
    // cast every time the parent lands (so a refresh of the parent refreshes
    // the rider), clamped to whatever the parent has left, and torn down from
    // the parent's remove hook. The rider rows are infinite-duration, so the
    // clamp is what gives them a lifetime at all.
    void ApplyRider(Unit* caster, Unit* target, Aura const* parent, uint32 riderId)
    {
        if (!caster || !target || !parent)
            return;

        caster->CastSpell(target, riderId, true);
        if (Aura* rider = target->GetAura(riderId, caster->GetGUID()))
        {
            int32 const remaining = parent->GetDuration();
            if (remaining > 0)
            {
                rider->SetMaxDuration(remaining);
                rider->SetDuration(remaining);
            }
        }
    }

    // Cinderbite's melt has an ordering problem that no single hook solves.
    // Freezing Trap carries AURA_INTERRUPT_FLAG_TAKE_DAMAGE, and Unit::DealDamage
    // runs that interrupt sweep BEFORE Unit::ProcSkillsAndAuras - so by the time
    // the fire proc fires, the trap the fire just shattered is already gone and
    // the proc has nothing left to test. The trap's own removal hook therefore
    // drops a breadcrumb that the proc immediately following it in the same call
    // stack picks up. (The remove hook itself cannot act: it has no idea what
    // school of damage broke the freeze - RemoveAurasWithInterruptFlags only
    // knows the flag - and the proc is the only place the school is known.)
    //
    // thread_local, not a plain global: maps update on parallel threads, but the
    // DealDamage that breaks the trap and the ProcSkillsAndAuras it feeds are
    // always the same statement sequence on one thread, never two.
    struct FrozenTrapBreak
    {
        ObjectGuid Hunter;
        ObjectGuid Victim;
        uint32 TimeMs = 0;
    };

    thread_local FrozenTrapBreak g_lastTrapBreak;

    // The proc that owns a break follows it within the same tick. The window is
    // only wide enough to survive that, so a later unrelated fire tick on the
    // same victim cannot claim a break it had nothing to do with.
    constexpr uint32 TRAP_BREAK_CLAIM_WINDOW_MS = 200;
}

// ===========================================================================
// moonkitten (druid)
// ===========================================================================

// 33876/33982/33983/48565/48566 Mangle (Cat) and 33878/33986/33987/48563/48564
// Mangle (Bear), carrying the moonkitten 3pc: the Mangle debuff also makes the
// target take 5% more damage from this druid's Balance spells. The rider
// (90377) lives exactly as long as the Mangle debuff it rides.
//
// Why the shipped version never worked: 90327 was a PROC_TRIGGER_SPELL carrier
// whose Mangle filter was meant to live in world.spell_proc, and that row was
// never written; with ProcTypeMask 0 in the DBC the core builds no proc entry
// at all, so Aura::GetProcEffectMask returned 0 forever. 90327 is now an inert
// dummy and the work is done here, on Mangle itself.
class spell_t2_moonkitten_mangle : public SpellScript
{
    PrepareSpellScript(spell_t2_moonkitten_mangle);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_T2_MOONLIT_WOUND))
            return;

        // GetHitAura() is null unless this cast created or refreshed the Mangle
        // debuff on this target - misses, dodges and parries fall out here.
        Aura* mangle = GetHitAura();
        if (!mangle)
            return;

        ApplyRider(caster, target, mangle, SPELL_T2_MOONLIT_WOUND_DEBUFF);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_t2_moonkitten_mangle::HandleAfterHit);
    }
};

// The aura half of the pair: when the Mangle debuff goes (expiry, a Mangle
// from another druid replacing it through the exclusive stack group, dispel,
// death), the rider goes with it. GetCasterGUID(), not GetCaster(): the druid
// may be offline or out of range by then and the rider still has to leave.
class spell_t2_moonkitten_mangle_aura : public AuraScript
{
    PrepareAuraScript(spell_t2_moonkitten_mangle_aura);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAura(SPELL_T2_MOONLIT_WOUND_DEBUFF, GetCasterGUID());
    }

    void Register() override
    {
        // EFFECT_1 is the APPLY_AURA (bleed-damage-taken) effect on every Mangle
        // rank; effects 0 and 2 are the weapon-damage effects and never become
        // aura effects.
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_moonkitten_mangle_aura::HandleRemove, EFFECT_1, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 8921 and ranks - Moonfire, carrying the moonkitten 5pc: a Moonfire landing on
// a target not already burning with this druid's Moonfire is worth a combo
// point, and the target takes 5% more damage from the druid's melee abilities
// for as long as the Moonfire burns.
class spell_t2_moonkitten_moonfire : public SpellScript
{
    PrepareSpellScript(spell_t2_moonkitten_moonfire);

    void HandleBeforeHit(SpellMissInfo /*missInfo*/)
    {
        _wasBurning = false;

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        // BeforeHit runs in Spell::TargetInfo::PreprocessTarget, ahead of
        // DoSpellEffectHit creating or refreshing the Moonfire aura, so it is
        // the last moment the PREVIOUS state is still readable. Ranked lookup
        // because the druid casts whatever rank they know, not rank 1.
        _wasBurning = target->GetAuraOfRankedSpell(SPELL_DRUID_MOONFIRE, caster->GetGUID()) != nullptr;
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_T2_PROWLING_MOONFIRE))
            return;

        // AfterHit fires for misses, resists and immunes too. GetHitAura() is
        // the exact "did the Moonfire actually stick" test - it is null unless
        // this cast created or refreshed the aura on this target.
        Aura* moonfire = GetHitAura();
        if (!moonfire)
            return;

        // The debuff is a property of being Moonfired, so every landed cast
        // refreshes it to the Moonfire's own remaining time; only the combo
        // point is rationed to a target that was not already burning.
        ApplyRider(caster, target, moonfire, SPELL_T2_MOONLIT_PREY);

        if (!_wasBurning)
            caster->AddComboPoints(target, 1);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_t2_moonkitten_moonfire::HandleBeforeHit);
        AfterHit += SpellHitFn(spell_t2_moonkitten_moonfire::HandleAfterHit);
    }

private:
    bool _wasBurning = false;
};

// The aura half: the rider leaves when the Moonfire does.
class spell_t2_moonkitten_moonfire_aura : public AuraScript
{
    PrepareAuraScript(spell_t2_moonkitten_moonfire_aura);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAura(SPELL_T2_MOONLIT_PREY, GetCasterGUID());
    }

    void Register() override
    {
        // EFFECT_0 is the periodic damage on every Moonfire rank.
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_moonkitten_moonfire_aura::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }
};

// 24858 - Moonkin Form, carrying the moonkitten 8pc: while the druid is in
// Moonkin Form and wears the set, a hidden helper (90484, SPELLMOD_COST -100%
// on Cat Form) sits on them, so the shift out of Moonkin straight into Cat
// Form costs nothing. The helper is a non-passive hidden aura on purpose: the
// client has to be told about the cost modifier, and passive carriers with
// ShapeshiftMask games are ignored on the AddAura path the set bonuses use.
//
// Why the shipped version never worked: 90329 was a passive spellmod carrier
// gated by ShapeshiftMask, which only Player::ApplyEquipSpell honours - the set
// delivery AddAuras the carrier, so the gate was never evaluated.
class spell_t2_feline_grace_moonkin : public AuraScript
{
    PrepareAuraScript(spell_t2_feline_grace_moonkin);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* druid = GetTarget();
        if (!druid->HasAura(SPELL_T2_FELINE_GRACE))
            return;

        druid->CastSpell(druid, SPELL_T2_FELINE_GRACE_HELPER, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        // Runs inside the Moonkin -> Cat shift, AFTER Cat Form's cost was taken
        // (Spell::TakePower happens at cast, the form swap happens at hit), so
        // pulling the helper here does not un-discount the shift in progress.
        GetTarget()->RemoveAurasDueToSpell(SPELL_T2_FELINE_GRACE_HELPER);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_feline_grace_moonkin::HandleApply, EFFECT_0, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_feline_grace_moonkin::HandleRemove, EFFECT_0, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90329 - Feline Grace carrier: the other direction of the same sync. Gaining
// the set while already in Moonkin Form arms the helper; losing the set (or
// logging in, where the carrier is re-granted after the form was restored)
// resyncs it to the current form.
class spell_t2_feline_grace : public AuraScript
{
    PrepareAuraScript(spell_t2_feline_grace);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* druid = GetTarget();
        if (druid->GetShapeshiftForm() == FORM_MOONKIN)
            druid->CastSpell(druid, SPELL_T2_FELINE_GRACE_HELPER, true);
        else
            druid->RemoveAurasDueToSpell(SPELL_T2_FELINE_GRACE_HELPER);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_T2_FELINE_GRACE_HELPER);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_feline_grace::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_feline_grace::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ===========================================================================
// monkey stalker (hunter)
// ===========================================================================

// 90332 - Simian Frenzy (8pc): "While Aspect of the Monkey is active, your
// melee attacks have a 5% chance of increasing melee attack speed by 30% for
// 12 sec." The carrier is a PROC_TRIGGER_SPELL -> 90379; the 5% roll and the
// melee filter are the world.spell_proc row (which the first build never
// shipped - that is why it was inert); the Aspect gate is this check.
class spell_t2_monkey_frenzy : public AuraScript
{
    PrepareAuraScript(spell_t2_monkey_frenzy);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return GetTarget()->HasAura(SPELL_HUNTER_ASPECT_OF_MONKEY);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_monkey_frenzy::CheckProc);
    }
};

// ===========================================================================
// penguin stalker (hunter)
// ===========================================================================

// 13809 - Frost Trap, and 1499 and ranks - Freezing Trap, carrying the penguin
// stalker 3pc: half the mana back.
class spell_t2_penguin_cold_efficiency : public SpellScript
{
    PrepareSpellScript(spell_t2_penguin_cold_efficiency);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_COLD_EFFICIENCY))
            return;

        if (GetSpellInfo()->PowerType != POWER_MANA)
            return;

        // A refund, not a cost modifier: all five traps share family word 0 bit
        // 0x80, so a SPELLMOD_COST carrier would read "all traps -50%" (see the
        // DBC file). AfterCast is safe for this - Spell::TakePower runs well
        // before CallScriptAfterCastHandlers, so the full cost has already left
        // the bar by the time we hand half of it back.
        //
        // No Spell* passed to CalcPowerCost on purpose: the cast has already
        // consumed its cost spellmods, and handing it the Spell again would
        // register them a second time.
        int32 const cost = GetSpellInfo()->CalcPowerCost(caster, GetSpellInfo()->GetSchoolMask());
        int32 const refund = CalculatePct(cost, 50);
        if (refund > 0)
            caster->EnergizeBySpell(caster, GetSpellInfo(), refund, POWER_MANA);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_t2_penguin_cold_efficiency::HandleAfterCast);
    }
};

// 90460-90471 - Serpent Sting wrappers (the spell the hunter actually learns
// and presses): a DUMMY that casts the stock rank, or - with Cinderbite worn -
// the rank's Fire clone. Both inner casts are triggered, so the wrapper pays
// the cost / GCD / cooldown once and the inner spell brings its own missile;
// the wrapper row has SpellVisualID 0 and Speed 0 so there is exactly one
// missile and no double travel time.
class spell_t2_penguin_serpent_sting : public SpellScript
{
    PrepareSpellScript(spell_t2_penguin_serpent_sting);

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        SerpentStingRank const* rank = FindSerpentStingRank(GetSpellInfo()->Id);
        if (!rank)
            return;

        uint32 const stingId = caster->HasAura(SPELL_T2_CINDERBITE) ? rank->FireClone : rank->Original;
        caster->CastSpell(target, stingId, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_t2_penguin_serpent_sting::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// 3355 \ 14308 \ 14309 - Freezing Trap Effect: the write half of Cinderbite's
// break handshake, see FrozenTrapBreak. Deliberately bound for every hunter and
// not gated on the 5pc - resolving the trap's caster here costs an
// ObjectAccessor lookup on a path that runs for every broken trap on the
// server, and the proc that reads the breadcrumb is the one that already knows
// whether the set is worn.
class spell_t2_penguin_trap_break : public AuraScript
{
    PrepareAuraScript(spell_t2_penguin_trap_break);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        // AURA_REMOVE_BY_DEFAULT is what Unit::RemoveAurasWithInterruptFlags
        // leaves behind; expiry, dispel and death all carry their own modes and
        // are not a trap somebody shattered.
        AuraApplication const* app = GetTargetApplication();
        if (!app || app->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT)
            return;

        g_lastTrapBreak.Hunter = GetCasterGUID();
        g_lastTrapBreak.Victim = GetTarget()->GetGUID();
        g_lastTrapBreak.TimeMs = getMSTime();
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_penguin_trap_break::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90334 - Cinderbite (penguin stalker 5pc), read half of the handshake: the
// hunter's fire melts their own Freezing Trap into a Frost Trap. The 5pc
// carrier itself is the proc aura; the fire filter is the SchoolMask on its
// spell_proc row, re-checked here so the script still behaves if that row is
// ever edited. Every fire source counts - Explosive Shot, the fire traps and
// the Fire Serpent Sting the same set grants: a frozen target stung with fire
// melts its trap on the first tick, which is the set's whole premise.
class spell_t2_penguin_melt : public AuraScript
{
    PrepareAuraScript(spell_t2_penguin_melt);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !(damageInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE))
            return false;

        Unit* victim = eventInfo.GetActionTarget();
        if (!victim)
            return false;

        // Peek only. The proc chance roll happens after this hook returns, so
        // consuming the breadcrumb here could throw away a break the proc then
        // declines to act on; HandleProc clears it instead.
        return g_lastTrapBreak.TimeMs != 0
            && g_lastTrapBreak.Hunter == GetTarget()->GetGUID()
            && g_lastTrapBreak.Victim == victim->GetGUID()
            && getMSTimeDiff(g_lastTrapBreak.TimeMs, getMSTime()) <= TRAP_BREAK_CLAIM_WINDOW_MS;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        g_lastTrapBreak = FrozenTrapBreak();

        Unit* hunter = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim)
            return;

        // The freeze is already gone - the damage that got us here broke it.
        // Leave a real Frost Trap slick (13810, the trap's own persistent area
        // aura: 30 s, 10 yd, -60%) where the victim stood. A dest cast from the
        // hunter hangs the dynamic object off the hunter exactly as a sprung
        // Frost Trap does, so Slick Getaway (90335) sees it too.
        hunter->CastSpell(victim->GetPosition(), SPELL_HUNTER_FROST_TRAP_AURA, true);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Cinderbite melted a Freezing Trap on {} - Frost Trap slick spawned at the target",
            hunter->GetName(), victim->GetName()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_penguin_melt::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_penguin_melt::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 90335 - Slick Getaway (penguin stalker 8pc): the first time the hunter steps
// into one of their own Frost Trap ice slicks, they sprint for 3 seconds. The
// carrier ticks once a second; there is no aura to hang this off, because Frost
// Trap Aura only ever applies to enemies and the hunter standing in their own
// slick is not a target of anything.
class spell_t2_penguin_slick : public AuraScript
{
    PrepareAuraScript(spell_t2_penguin_slick);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* hunter = GetTarget()->ToPlayer();
        if (!hunter)
            return;

        // The slick's dynamic object hangs off the HUNTER, not the trap
        // gameobject: GameObject::Update fires a hunter's traps through the
        // owner, so Spell::EffectPersistentAA's unitCaster is the player. That
        // also makes "your own slick" free - somebody else's is never in here.
        std::vector<DynamicObject*> const slicks = hunter->GetDynObjects(SPELL_HUNTER_FROST_TRAP_AURA);

        auto isLive = [&slicks](ObjectGuid const& guid)
        {
            for (DynamicObject* slick : slicks)
                if (slick->GetGUID() == guid)
                    return true;
            return false;
        };

        // Forget slicks that have melted, so a hunter who lays a fresh trap
        // gets paid again and the list stays the size of the traps on the
        // ground rather than the length of the session.
        for (std::size_t i = 0; i < _paid.size();)
        {
            if (isLive(_paid[i]))
                ++i;
            else
                _paid.erase(_paid.begin() + i);
        }

        for (DynamicObject* slick : slicks)
        {
            // 2D: a slick is a disc on the ground, and the hunter standing in
            // it is at its z whether the dynamic object spawned above or below.
            if (hunter->GetExactDist2d(slick) > slick->GetRadius())
                continue;

            bool alreadyPaid = false;
            for (ObjectGuid const& guid : _paid)
                if (guid == slick->GetGUID())
                {
                    alreadyPaid = true;
                    break;
                }
            if (alreadyPaid)
                continue;

            _paid.push_back(slick->GetGUID());
            hunter->CastSpell(hunter, SPELL_T2_SLICK_GETAWAY_SPRINT, true);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Slick Getaway - first entry into own Frost Trap slick, +30% for 3s",
                hunter->GetName()));
            break;
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_penguin_slick::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    std::vector<ObjectGuid> _paid;
};

void AddSC_custom_t2_druid_hunter()
{
    // moonkitten
    RegisterSpellAndAuraScriptPair(spell_t2_moonkitten_mangle, spell_t2_moonkitten_mangle_aura);
    RegisterSpellAndAuraScriptPair(spell_t2_moonkitten_moonfire, spell_t2_moonkitten_moonfire_aura);
    RegisterSpellScript(spell_t2_feline_grace_moonkin);
    RegisterSpellScript(spell_t2_feline_grace);
    // monkey stalker
    RegisterSpellScript(spell_t2_monkey_frenzy);
    // penguin stalker
    RegisterSpellScript(spell_t2_penguin_cold_efficiency);
    RegisterSpellScript(spell_t2_penguin_serpent_sting);
    RegisterSpellScript(spell_t2_penguin_trap_break);
    RegisterSpellScript(spell_t2_penguin_melt);
    RegisterSpellScript(spell_t2_penguin_slick);
}
