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
 *   - Prowling Moonfire books its combo point on the Spell (Spell::
 *     AddComboPointGain, the SPELL_EFFECT_ADD_COMBO_POINTS path) and rolls
 *     Primal Fury on a crit, mirroring the Surprise Bear Maul / Swipe script.
 *   - Feline Grace is a scripted "hidden real buff" keyed on Moonkin Form.
 *   - Simian Frenzy gets its Aspect of the Monkey gate here (its proc row is in
 *     the world SQL).
 *   - Cinderbite uses the replacement-wrapper pattern (see
 *     spell_warr_disarm_wrapper): the player learns a DUMMY wrapper per Serpent
 *     Sting rank which casts either the stock rank or its Fire clone, and the
 *     trap melt spawns a real Frost Trap slick (13810) where the freeze broke.
 *
 * Revision 2026-08-19b (second live test):
 *   - Cinderbite's Fire sting now also fires when the hunter casts the STOCK
 *     Serpent Sting rank. The wrapper is only reachable from a spellbook that
 *     holds it, which no already-existing character's does (and no client does
 *     until patch-enUS-8 ships the wrapper rows), so the bonus looked
 *     conditional in game; the swap is now on both ids and has no precondition.
 *   - Cold Efficiency is a real cost reduction carried by the 90333 row itself
 *     instead of an AfterCast refund, so the client's own cost check agrees and
 *     the traps are castable at low mana. Its script is gone - the reasoning is
 *     in the comment where the class used to be.
 *
 * Wiring lives in ItemSet.dbc (SetSpellID/SetThreshold); spell_script_names
 * binds these classes to their spell ids - see
 * sql/custom/world/2026_08_19_05_world_t2_druid_hunter.sql, which also
 * carries the spell_proc / spell_ranks rows these scripts need.
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "Chat.h"
#include "DynamicObject.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellDefines.h"
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
        SPELL_DRUID_PRIMAL_FRENZY_R1    = 16952,   // the fork's Primal Fury talent; rank 2 is 16954 (ranked lookup)

        // monkey stalker (hunter)
        SPELL_T2_SIMIAN_FRENZY          = 90332,   // 8pc carrier (proc -> 90379)
        SPELL_HUNTER_ASPECT_OF_MONKEY   = 13163,

        // penguin stalker (hunter)
        SPELL_T2_COLD_EFFICIENCY        = 90333,   // 3pc carrier (pure Spell.dbc, no script)
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

    SerpentStingRank const* FindSerpentStingRankByOriginal(uint32 originalId)
    {
        for (SerpentStingRank const& rank : SERPENT_STING_RANKS)
            if (rank.Original == originalId)
                return &rank;
        return nullptr;
    }

    // .gm diagnostics customauras - broadcast to every opted-in GM session
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

        // NO DURATION CLAMP. The parent's remove hook is the authoritative teardown,
        // and it fires for every real removal - expiry, dispel, death, an exclusive
        // overwrite by another druid - so the rider already lives exactly as long as
        // the parent and not one tick longer.
        //
        // Clamping on top of that is not merely redundant, it is actively wrong once
        // the parent can be REFRESHED. Unit::_TryStackingOrRefreshingExistingAura only
        // bumps the existing application's timers; it never re-applies, so an aura
        // hook does not run again on a refresh. A rider clamped on first application
        // therefore expires on the parent's ORIGINAL deadline while the parent itself
        // is renewed indefinitely - a feral druid clawing on cooldown got the bonus
        // for the first 12 seconds of a fight and never again, with the Mangle debuff
        // still plainly up. Leaving the rider at its own infinite duration and letting
        // the remove hook end it is correct on every route and refresh-proof.
        caster->CastSpell(target, riderId, true);
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

    // The rider is placed from the DEBUFF, not from the Mangle cast, because the
    // debuff does not only arrive by casting Mangle: the fork's Primal Precision
    // (48410) puts it on Claw through a bare Unit::AddAura (spell_dru_claw in
    // spell_druid.cpp), and no Mangle spell is cast at all on that path. A
    // SpellScript bound to the Mangle ability ids never runs for it, which is why
    // a feral druid saw this bonus do nothing. Keying on the aura covers every way
    // the debuff can arrive, including any added later.
    //
    // This also gets the duration clamp right. In the cast's AfterHit the parent's
    // duration is not necessarily initialised yet; by AURA_EFFECT_HANDLE_REAL on
    // apply it always is, so the rider actually inherits the Mangle's remaining time.
    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target)
            return;

        // Cheapest test last of the free ones: almost no druid carries the 3pc.
        if (!caster->HasAura(SPELL_T2_MOONLIT_WOUND))
            return;

        ApplyRider(caster, target, GetAura(), SPELL_T2_MOONLIT_WOUND_DEBUFF);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Moonlit Wound applied to {} (Mangle debuff {})",
            caster->GetName(), target->GetName(), GetId()));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAura(SPELL_T2_MOONLIT_WOUND_DEBUFF, GetCasterGUID());
    }

    void Register() override
    {
        // EFFECT_1 is the APPLY_AURA (bleed-damage-taken) effect on every Mangle
        // rank; effects 0 and 2 are the weapon-damage effects and never become
        // aura effects.
        AfterEffectApply += AuraEffectApplyFn(spell_t2_moonkitten_mangle_aura::HandleApply, EFFECT_1, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_moonkitten_mangle_aura::HandleRemove, EFFECT_1, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 8921 and ranks - Moonfire, carrying the moonkitten 5pc: a Moonfire landing on
// a target not already burning with this druid's Moonfire is worth a combo
// point (two if it crits and Primal Fury rolls, exactly as Maul / Swipe with
// Surprise Bear 89759 - see spell_dru_surprise_bear_combo), and the target
// takes 5% more damage from the druid's melee abilities for as long as the
// Moonfire burns.
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

        if (_wasBurning)
            return;

        // Through Spell::AddComboPointGain, not Unit::AddComboPoints here: the
        // gain is booked on the Spell and paid once in
        // Spell::_handle_finish_phase through Unit::AddComboPoints, which is
        // what sends SMSG_UPDATE_COMBO_POINTS - the one packet the client's
        // combo frame and floating combat text (UNIT_COMBO_POINTS) react to.
        // That is the path SPELL_EFFECT_ADD_COMBO_POINTS and the Surprise
        // Bear Maul / Swipe script take, so the Moonfire point shows up exactly
        // where a Mangle point does.
        GetSpell()->AddComboPointGain(target, 1);

        // Primal Fury: "critical strikes from abilities that add combo points
        // have a chance to add an additional combo point". Only a Moonfire
        // that earned the base point qualifies (the refresh case returned
        // above - a refresh adds nothing, so there is nothing to double). The
        // direct hit's crit is already folded into the spell's hit mask by the
        // time AfterHit runs (m_hitMask is updated before the AfterHit
        // handlers), the same read the Maul script does.
        bool const crit = (GetSpell()->GetHitMask() & PROC_HIT_CRITICAL) != 0;
        float const frenzyChance = PrimalFrenzyChance(caster);
        bool const bonus = crit && roll_chance_f(frenzyChance);
        if (bonus)
            GetSpell()->AddComboPointGain(target, 1);

        // Kept after the fix: this is what found the fix. Static reading had cleared
        // every condition - rank chain present, ProcChance 100, no competing spellmod,
        // hit mask written before AfterHit - and the point still did not appear. One
        // live line ("crit=YES frenzy=MISSING") isolated it to the aura lookup in a
        // way four rounds of reading the data had not.
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Moonfire combo - hitMask=0x{:X} crit={} frenzy={} chance={} -> {}",
            caster->GetName(), GetSpell()->GetHitMask(), crit ? "YES" : "no",
            FindPrimalFrenzy(caster) ? "known" : "NOT KNOWN", int32(frenzyChance),
            bonus ? "2 points" : "1 point"));
    }

    // Verbatim the roll spell_dru_surprise_bear_combo uses for Maul / Swipe,
    // so Moonfire and the bear abilities agree: the chance is the talent row's
    // own ProcChance (16952 rank 1 = 50, 16954 rank 2 = 100 in Spell.dbc) run
    // through SPELLMOD_CHANCE_OF_SUCCESS. Ranked lookup - the druid has one
    // rank or the other, never rank 1 by id.
    // Found by KNOWN SPELL, not by active aura, and that distinction is the whole
    // bug this set bonus had (diagnosed live 2026-08-21: "crit=YES frenzy=MISSING").
    //
    // Primal Frenzy is ShapeshiftMask 145 = Cat | Bear | Dire Bear, so
    // Player::HandleShapeshiftBoosts applies its passive on shifting INTO those
    // forms and strips it on the way out. Moonfire is cast in caster or Moonkin
    // form, where the aura is by definition absent - so the old
    // GetAuraEffectOfRankedSpell lookup could never succeed on the one spell this
    // script exists to handle, no matter how the talent was specced. The rank chain
    // and the 100% ProcChance were both fine; nothing was ever there to find.
    //
    // Knowing the talent is the right test: the set's whole premise is that Moonfire
    // generates combo points like a cat ability, so the talent that rewards
    // combo-generating crits should apply to it too.
    //
    // The chain is still walked (rather than testing 16954 directly) because a druid
    // holds whichever rank they specced, and this fork collapsed the talent to a
    // single rank that happens to be rank 2.
    static SpellInfo const* FindPrimalFrenzy(Unit* caster)
    {
        Player* player = caster->ToPlayer();
        if (!player)
            return nullptr;

        for (uint32 rank = sSpellMgr->GetFirstSpellInChain(SPELL_DRUID_PRIMAL_FRENZY_R1);
             rank; rank = sSpellMgr->GetNextSpellInChain(rank))
            if (player->HasSpell(rank))
                return sSpellMgr->GetSpellInfo(rank);

        return nullptr;
    }

    static float PrimalFrenzyChance(Unit* caster)
    {
        SpellInfo const* primalFrenzy = FindPrimalFrenzy(caster);
        if (!primalFrenzy)
            return 0.0f;

        float chance = float(primalFrenzy->ProcChance);
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(primalFrenzy->Id, SPELLMOD_CHANCE_OF_SUCCESS, chance);

        return chance;
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

// 90333 Cold Efficiency (penguin stalker 3pc) has NO script any more - the
// carrier row itself is now the discount. The shipped version refunded half the
// cost from an AfterCast hook on the trap, which is not what the bonus says: the
// mana still left the bar, so the trap was uncastable at low mana and the client
// greyed the button exactly as if the bonus were not worn.
//
// It is a SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT (72) effect on 90333, school mask
// 16 (frost), -50%. Two reasons that aura and not the house SPELLMOD_COST
// (108 / misc 14) pattern:
//   * A spellmod cannot name the two traps. Frost Trap 13809 and Freezing Trap
//     1499/14310/14311 carry family word 0 bit 0x80, and so do Immolation Trap,
//     Explosive Trap, Snake Trap, Trap Launcher, Freezing Arrow, Black Arrow
//     ranks 2-6 and Deterrence - nothing in the three mask words separates them
//     (verified over every SpellClassSet 9 row in Spell.dbc). Aura 72 keys on
//     the SCHOOL instead, and the only frost-school hunter spells that cost
//     mana are the two traps plus Freezing Arrow 60192/60202 - a Freezing Trap
//     fired from a bow, which is as close to "these two traps" as the data can
//     express.
//   * The client has to agree, or the button stays grey. It reads
//     UNIT_FIELD_POWER_COST_MULTIPLIER[school], which is exactly where aura 72
//     lands (AuraEffect::HandleModPowerCostPCT), and SpellInfo::CalcPowerCost
//     applies the same field server-side. Whether the client is told about a
//     spellmod carried by a PASSIVE aura - which every set carrier is - is an
//     open question in this project; the unit field is not in doubt.

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

// 1978 and ranks - the STOCK Serpent Sting, the other half of Cinderbite's
// swap and the reason the bonus looked conditional in the live test.
//
// The wrapper above can only fire for a hunter who is actually CASTING the
// wrapper, and nothing makes that true for a character that already exists:
// sql/custom/helper/2026_08_19_05 repoints SkillLineAbility, the trainer list
// and playercreateinfo, all of which decide what a character learns NEXT - the
// stock rank stays in character_spell, stays on the action bar, and is the only
// one of the two the client can cast until patch-enUS-8 ships the wrapper rows.
// Whichever of the two ids a hunter ends up pressing (and one hunter can hold
// both), the sting has to burn, so the swap lives on both spells. It also
// catches every OTHER way a stock rank is cast - a macro, a bot rotation, a
// Violet Hold echo - which is what "no precondition" means.
//
// Mechanically this is the wrapper's swap read backwards: instead of choosing
// the clone before casting, the rank's own payload is prevented and the clone
// is cast at the same target. Every effect on every rank is an APPLY_AURA (the
// DoT, plus the Chimera Shot rider on ranks 11-12), so preventing them all
// leaves the cast itself untouched - cost, GCD, range and the ranged hit roll
// all still happen, only the damage that lands is fire. The one cosmetic price
// on this path is a second missile (the rank's own arrow, then the clone's);
// the wrapper avoids it with Speed 0 / SpellVisualID 0, which is why the
// wrapper stays the primary path once the client has its rows.
class spell_t2_penguin_serpent_sting_direct : public SpellScript
{
    PrepareSpellScript(spell_t2_penguin_serpent_sting_direct);

    void HandleSwap(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_CINDERBITE))
            return;

        if (!FindSerpentStingRankByOriginal(GetSpellInfo()->Id))
            return;

        // Both are required, exactly as spell_t2_intercept_knockback documents:
        // removing the aura does not stop EffectApplyAura running against it.
        // Called for every effect the rank has - the DoT on all twelve, plus
        // the Chimera Shot rider on ranks 11 and 12 - so nothing of the Nature
        // sting survives.
        PreventHitAura();
        PreventHitDefaultEffect(effIndex);
        _swapped = true;
    }

    // Applied, not cast. A cast would roll the ranged hit check a SECOND time -
    // the stock rank already passed one to get here - so ~5% of stings would
    // suppress the nature DoT and then land nothing at all, and a refresh would
    // strip the fire DoT already ticking and fail to replace it. The wrapper
    // path does not have this problem because the wrapper itself carries
    // IGNORE_HIT_RESULT (ATTR3 0x40000) and the clone is its only roll; the
    // stock rank obviously cannot carry it, so the second roll is removed on
    // this side instead. Unit::AddAura still honours IsImmunedToSpell and the
    // per-effect immunity mask, so an immune target is unaffected, and dropping
    // the cast also drops the clone's own missile - the rank's missile has
    // already flown and impacted by the time this runs.
    //
    // Still done from AfterHit rather than the effect handler: that keeps the
    // aura application out of the hit phase that is still walking this target's
    // effects. Missed / resisted / immune casts never reach the effect handler
    // at all, so _swapped stays false and nothing is substituted.
    void HandleAfterHit()
    {
        bool const swapped = _swapped;
        _swapped = false;

        if (!swapped)
            return;

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        SerpentStingRank const* rank = FindSerpentStingRankByOriginal(GetSpellInfo()->Id);
        if (!rank)
            return;

        caster->AddAura(rank->FireClone, target);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_t2_penguin_serpent_sting_direct::HandleSwap, EFFECT_ALL, SPELL_EFFECT_APPLY_AURA);
        AfterHit += SpellHitFn(spell_t2_penguin_serpent_sting_direct::HandleAfterHit);
    }

private:
    bool _swapped = false;
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
        // 90334's effect 0 is a DUMMY with TriggerSpell 0. Without this the core
        // logs "has non-existent spell 0 in EffectTriggered[0]" as TC_LOG_ERROR
        // into the 'spells' channel every time the bonus SUCCEEDS - straight
        // into the log the heap-corruption hunt reads.
        PreventDefaultAction();

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
    // penguin stalker (Cold Efficiency 90333 is pure Spell.dbc - see above)
    RegisterSpellScript(spell_t2_penguin_serpent_sting);
    RegisterSpellScript(spell_t2_penguin_serpent_sting_direct);
    RegisterSpellScript(spell_t2_penguin_trap_break);
    RegisterSpellScript(spell_t2_penguin_melt);
    RegisterSpellScript(spell_t2_penguin_slick);
}
