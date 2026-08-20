/*
 * Legionnaire Plus T2 set bonuses - shaman and warlock.
 *
 * Companion to custom_t1_set_bonuses.cpp, same shape: the carriers are the
 * inert passives 90300-90365 that the ItemSet grants, the helper spells are
 * 90366-90394, and everything here is a bonus no aura type can express on its
 * own. Scripts either sit on the carrier (when the carrier is a periodic dummy
 * that has to poll something) or on the ability being modified, checking for
 * the carrier on the caster.
 *
 * Contract for the ids and the intended hooks:
 *   sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql      (carriers/helpers)
 *   sql/custom/dbc/2026_08_19_04_dbc_t2_shaman_warlock.sql   (Shadow Bolt wrappers
 *                                                             and clones 90420-90458,
 *                                                             Legion of One scale)
 * Bindings:
 *   sql/custom/world/2026_08_17_01_world_t2_shaman-warlock.sql (superseded by)
 *   sql/custom/world/2026_08_19_04_world_t2_shaman_warlock.sql
 * Skill-line / create-spell repoints for the wrappers:
 *   sql/custom/helper/2026_08_19_04_helper_t2_shaman_warlock.sql
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "Chat.h"
#include "Pet.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "Totem.h"
#include "Unit.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace
{
    enum T2ShamanWarlockSpells
    {
        // shaman - frost shock set
        SPELL_T2_RIMEWARD_OFFERING      = 90314,   // 8pc carrier
        SPELL_T2_RIMEWARD               = 90373,   // the 500 absorb on the totem
        // shaman - flame shock set
        SPELL_T2_PYRE_OFFERING          = 90317,   // 8pc carrier
        // warlock - imp set
        SPELL_T2_FELFLAME_BOLT          = 90318,   // 3pc carrier - Shadow Bolt becomes fire + Firebolt art
        SPELL_T2_LEGION_OF_ONE          = 90320,   // 8pc carrier
        SPELL_T2_LESSER_IMP             = 90375,   // -50% damage done
        // warlock - life tap set
        SPELL_T2_MANA_EQUILIBRIUM       = 90321,   // 3pc carrier
        SPELL_T2_BLOOD_FOR_POWER        = 90322,   // 5pc carrier

        // Purge in this fork is the two wrappers, NOT 370/8012 - the wrappers
        // are what the shaman actually casts and what carries spell_sha_purge.
        SPELL_SHAMAN_PURGE_R1           = 370,
        SPELL_SHAMAN_PURGE_R2           = 8012,
        SPELL_SHAMAN_PURGE_WRAPPER_R1   = 81324,
        SPELL_SHAMAN_PURGE_WRAPPER_R2   = 81325,

        SPELL_WARLOCK_SUMMON_IMP        = 688,
        NPC_WARLOCK_IMP                 = 416,

        // Improved Flurry is this fork's own talent: a PASSIVE (Attributes
        // 0x40) whose single effect is APPLY_AURA / PROC_TRIGGER_SPELL (42)
        // firing Blurry on melee autoattacks - verified in the binary
        // Spell.dbc, and it is NOT a rank chain (the only other "Improved
        // Flurry" row is the unrelated stock 37241). "Learned" therefore
        // means HasSpell; HasAura is checked too because a learned passive is
        // applied on login and the pair is what spell_t1_rehgar_flurry
        // (custom_t1_set_bonuses.cpp) already uses.
        SPELL_SHAMAN_IMPROVED_FLURRY    = 89746,
        SPELL_SHAMAN_BLURRY             = 89745,
    };

    // Rimeward puts Purge on 10 s, Pyre on 15 s.
    constexpr uint32 RIMEWARD_PURGE_COOLDOWN_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 PYRE_PURGE_COOLDOWN_MS     = 15 * IN_MILLISECONDS;

    // Lesser Imp is re-stamped by the 1 s poll on 90320, so a short lease is
    // enough - see StampLesserImp for why the lease exists at all.
    constexpr int32 LESSER_IMP_LEASE_MS = 10 * IN_MILLISECONDS;

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

    // True when `target` is a totem owned by `caster` and `caster` wears one of
    // the two totem-offering 8pc carriers. This is the SAME predicate that
    // spell_sha_purge::IsOwnTotemOfferingTarget (spell_shaman.cpp) uses to let
    // the Purge wrapper's CheckCast accept the friendly totem - the gate lives
    // there because Spell::CallScriptCheckCastHandlers keeps the first non-OK
    // result, so a second script cannot override spell_sha_purge's rejection.
    // Keep the two in sync.
    bool IsOwnTotemOfferingTarget(Unit const* caster, Unit const* target)
    {
        if (!caster || !target || caster->GetTypeId() != TYPEID_PLAYER)
            return false;

        if (!target->IsTotem() || target->GetOwnerGUID() != caster->GetGUID())
            return false;

        return caster->HasAura(SPELL_T2_RIMEWARD_OFFERING) || caster->HasAura(SPELL_T2_PYRE_OFFERING);
    }

    // Lifted from spell_sha_purge::AddVisiblePurgeCooldown (spell_shaman.cpp),
    // which is a private static of a class in another TU and so cannot be
    // called from here. All four ids get the cooldown because the shaman casts
    // the wrapper but the client shows the cooldown on whichever id sits on the
    // action bar; the packet is what makes the button actually grey out.
    void AddVisiblePurgeCooldown(Unit* caster, uint32 cooldownMs)
    {
        if (!caster)
            return;

        SpellHistory* spellHistory = caster->GetSpellHistory();
        if (!spellHistory)
            return;

        static constexpr std::array<uint32, 4> PurgeSpellIds =
        {
            SPELL_SHAMAN_PURGE_R1,
            SPELL_SHAMAN_PURGE_R2,
            SPELL_SHAMAN_PURGE_WRAPPER_R1,
            SPELL_SHAMAN_PURGE_WRAPPER_R2
        };

        Player* player = caster->ToPlayer();

        for (uint32 spellId : PurgeSpellIds)
        {
            if (!sSpellMgr->GetSpellInfo(spellId))
                continue;

            spellHistory->AddCooldown(spellId, 0, std::chrono::milliseconds(cooldownMs));

            if (player)
            {
                WorldPacket data;
                spellHistory->BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, spellId, cooldownMs);
                player->SendDirectMessage(&data);
            }
        }
    }

    // Pyre Offering's payout.
    //
    // A shaman who has learned Improved Flurry (89746) gets BLURRY (89745)
    // instead of Flurry - that talent's whole purpose is to convert Flurry
    // procs into Blurry, so handing its owner raw Flurry charges would be
    // handing him the thing he already traded away. Exactly ONE Blurry, not
    // "two charges' worth", for three reasons: 89745 ships with ProcCharges 1
    // and StackAmount 0 (verified in the binary Spell.dbc), so a second charge
    // could only be forced on; one charge is already worth far more than the
    // two Flurry swing charges below (-20% cast time, -0.5 s GCD and -100%
    // mana cost on the next LB / CL / HW / LHW / Chain Heal, i.e. one free
    // cast); and it matches every other grant of Blurry in the fork - the
    // Improved Flurry proc itself and spell_t1_rehgar_flurry both give one.
    // A recast while Blurry is already up refreshes it to a full 15 s / 1
    // charge rather than stacking, which is the same behaviour as the talent
    // proccing again.
    //
    // Flurry is charge-based, not stacking: the buff always lands with its full
    // 3 swing charges, so "grants N charges" means capping it back down. A
    // shaman with neither Improved Flurry nor a Flurry rank gets nothing -
    // deliberate, and noted on the 90317 row in the dbc file.
    void GrantFlurryReward(Player* player, uint8 grant)
    {
        if (player->HasSpell(SPELL_SHAMAN_IMPROVED_FLURRY) || player->HasAura(SPELL_SHAMAN_IMPROVED_FLURRY))
        {
            // Safe here: the only caller is the Purge wrapper's OnCast hook,
            // i.e. the spell-cast path, not a walk over this unit's aura
            // containers.
            player->CastSpell(player, SPELL_SHAMAN_BLURRY, true);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Pyre Offering - Improved Flurry learned, granted Blurry",
                player->GetName()));
            return;
        }

        // highest rank first; talents[i] pairs with buffs[i]
        static constexpr std::array<uint32, 5> talents = { 16284, 16283, 16282, 16281, 16256 };
        static constexpr std::array<uint32, 5> buffs   = { 16280, 16279, 16278, 16277, 16257 };

        for (uint8 i = 0; i < talents.size(); ++i)
        {
            if (!player->HasSpell(talents[i]) && !player->HasAura(talents[i]))
                continue;

            uint8 charges = 0;
            if (Aura* existing = player->GetAura(buffs[i]))
                charges = existing->GetCharges();
            player->CastSpell(player, buffs[i], true);
            if (Aura* buff = player->GetAura(buffs[i]))
                buff->SetCharges(std::min<uint8>(charges + grant, 3));
            return;
        }

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Pyre Offering - no Improved Flurry and no Flurry rank trained, nothing granted",
            player->GetName()));
    }

    // 90375 ships with an infinite duration, and pet auras are written to
    // `pet_aura` on logout: a wearer who logged out with the imp up and came
    // back without the set would own a permanently halved imp with nothing left
    // running to clear it. Leasing the applied aura and letting the one-second
    // poll on 90320 renew it makes the halving self-expiring, so it can never
    // outlive the bonus by more than the lease.
    void StampLesserImp(Pet* pet)
    {
        if (!pet || pet->GetEntry() != NPC_WARLOCK_IMP || !pet->IsAlive())
            return;

        Aura* lesser = pet->GetAura(SPELL_T2_LESSER_IMP);
        if (!lesser)
        {
            pet->CastSpell(pet, SPELL_T2_LESSER_IMP, true);
            lesser = pet->GetAura(SPELL_T2_LESSER_IMP);
            if (!lesser)
                return;
        }

        lesser->SetMaxDuration(LESSER_IMP_LEASE_MS);
        lesser->SetDuration(LESSER_IMP_LEASE_MS);
    }

    // ---------------------------------------------------------------------
    // Shadow Bolt replacement wrappers (Felflame Bolt 90318).
    //
    // One row triple per rank, ids from the shaman-warlock range 90420-90459:
    //   wrapper      90420 + (rank-1)   what the warlock LEARNS (skill line and
    //                                   playercreateinfo_spell_custom are
    //                                   repointed from the original rank to it)
    //   shadow clone 90433 + (rank-1)   the original rank as an instant, free,
    //                                   GCD-less spell - shadow school, visual 64
    //   fire clone   90446 + (rank-1)   same, fire school, Firebolt visual 67
    // The original 686..47809 rows are untouched (creatures cast them).
    // ---------------------------------------------------------------------
    struct ShadowBoltRank
    {
        uint32 original;
        uint32 wrapper;
        uint32 shadowClone;
        uint32 fireClone;
    };

    constexpr std::array<ShadowBoltRank, 13> ShadowBoltRanks =
    {{
        {   686, 90420, 90433, 90446 },   // Rank 1
        {   695, 90421, 90434, 90447 },   // Rank 2
        {   705, 90422, 90435, 90448 },   // Rank 3
        {  1088, 90423, 90436, 90449 },   // Rank 4
        {  1106, 90424, 90437, 90450 },   // Rank 5
        {  7641, 90425, 90438, 90451 },   // Rank 6
        { 11659, 90426, 90439, 90452 },   // Rank 7
        { 11660, 90427, 90440, 90453 },   // Rank 8
        { 11661, 90428, 90441, 90454 },   // Rank 9
        { 25307, 90429, 90442, 90455 },   // Rank 10
        { 27209, 90430, 90443, 90456 },   // Rank 11
        { 47808, 90431, 90444, 90457 },   // Rank 12
        { 47809, 90432, 90445, 90458 },   // Rank 13
    }};

    ShadowBoltRank const* FindShadowBoltRankByWrapper(uint32 wrapperId)
    {
        for (ShadowBoltRank const& rank : ShadowBoltRanks)
            if (rank.wrapper == wrapperId)
                return &rank;
        return nullptr;
    }
}

// 81324 \ 81325 - the Purge wrappers, carrying both totem offerings: purging
// your OWN totem stops being a wasted global and becomes the payoff.
//   90314 Rimeward Offering (frost-shock 8pc): the totem gets a 500 absorb and
//         Purge goes on 10 sec.
//   90317 Pyre Offering (flame-shock 8pc): the totem is destroyed, the shaman
//         gets 2 Flurry charges - or one Blurry (89745) if he has learned
//         Improved Flurry (89746), see GrantFlurryReward - and Purge goes on
//         15 sec.
//
// This rides the shipped spell_sha_purge rather than reimplementing Purge:
// spell_script_names is a multimap, so both scripts run on the same cast and
// spell_sha_purge keeps owning the enemy dispel and the Rehgar's Mercy path.
//
// The live-test failure ("nothing to dispel" on your own totem) was the gate,
// not this payload: spell_sha_purge::CheckMagicDispel rejected every friendly
// target that was not a Rehgar's Mercy crowd-control cleanse, and a second
// script cannot override that (Spell::CallScriptCheckCastHandlers keeps the
// FIRST non-OK result). The gate now has an own-totem exception
// (spell_sha_purge::IsOwnTotemOfferingTarget) mirroring the predicate above.
//
// Two hooks, on purpose:
//   OnCast               - the payload. It keys off the explicit target, so it
//                          runs even on the ~3% "resist" roll the wrapper makes
//                          against your own totem (MagicSpellHitResult has no
//                          friendly exemption for a negative spell).
//   OnEffectLaunchTarget - Effect 0 of the wrapper is SPELL_EFFECT_TRIGGER_SPELL
//                          (370/8012), and Spell::EffectTriggerSpell fires in the
//                          LAUNCH_TARGET phase, not at hit. Preventing it here is
//                          what actually stops the real Purge being thrown at the
//                          totem (it would fail BAD_TARGETS anyway - 370 is
//                          enemy-only - but there is no point launching it).
class spell_t2_purge_offering : public SpellScript
{
    PrepareSpellScript(spell_t2_purge_offering);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_T2_RIMEWARD_OFFERING, SPELL_T2_PYRE_OFFERING, SPELL_T2_RIMEWARD });
    }

    void HandleOfferingOnCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!IsOwnTotemOfferingTarget(caster, target))
            return;

        Player* shaman = caster->ToPlayer();
        Totem* totem = target->ToTotem();
        if (!shaman || !totem)
            return;

        // The two are different 8pc sets, so nobody can wear both; Rimeward
        // wins if the impossible happens, because Pyre would destroy the totem
        // the shield was just put on.
        bool const rimeward = shaman->HasAura(SPELL_T2_RIMEWARD_OFFERING);

        if (rimeward)
        {
            // Cast BY the totem, not by the shaman: totems are immune to
            // positive spells whose TargetA is not TARGET_UNIT_CASTER, and
            // 90373 is built as a self-cast for exactly that reason.
            totem->CastSpell(totem, SPELL_T2_RIMEWARD, true);
            AddVisiblePurgeCooldown(shaman, RIMEWARD_PURGE_COOLDOWN_MS);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Rimeward Offering - {} shielded for 500, Purge on 10s",
                shaman->GetName(), totem->GetName()));
            return;
        }

        GrantFlurryReward(shaman, 2);
        AddVisiblePurgeCooldown(shaman, PYRE_PURGE_COOLDOWN_MS);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Pyre Offering - {} consumed, Purge on 15s",
            shaman->GetName(), totem->GetName()));

        // Delayed by a millisecond on purpose. Totem::UnSummon strips auras
        // from the shaman and clears his totem slot, and we are standing
        // inside the wrapper's cast that still holds this totem as its
        // explicit target; the msTime arm queues a ForcedUnsummonDelayEvent
        // instead, so the teardown happens on the creature's own update.
        totem->UnSummon(1);
    }

    void PreventPurgeLaunch(SpellEffIndex effIndex)
    {
        if (IsOwnTotemOfferingTarget(GetCaster(), GetHitUnit()))
            PreventHitDefaultEffect(effIndex);
    }

    void Register() override
    {
        OnCast += SpellCastFn(spell_t2_purge_offering::HandleOfferingOnCast);
        OnEffectLaunchTarget += SpellEffectFn(spell_t2_purge_offering::PreventPurgeLaunch, EFFECT_0, SPELL_EFFECT_TRIGGER_SPELL);
    }
};

// 90420-90432 - the Shadow Bolt wrappers (one per rank), carrying the imp 3pc
// Felflame Bolt (90318): "Your Shadow Bolt is now fire damage and uses the
// Firebolt graphic."
//
// Same shape as spell_warr_disarm_wrapper (81492): the wrapper is a DUMMY
// with the rank's cast time / cost / range / family mask, so every cast-side
// rule (Bane, Backlash, Shadow Trance, Glyph of Shadow Bolt, haste, pushback,
// interrupts, school lockout) applies to it exactly as to the original, and
// the script hands the hit to either clone.
//
// The inner cast is deliberately NOT triggered. Two gates stop a triggered
// Shadow Bolt from proccing anything: Aura::GetProcEffectMask requires the
// PROC AURA (Improved Shadow Bolt, Shadow Embrace, Soul Leech, Eradication,
// Everlasting Affliction, every trinket...) to carry
// SPELL_ATTR3_CAN_PROC_WITH_TRIGGERED, and SpellMgr::CanSpellTriggerProcOnEvent
// requires the triggering SPELL to carry SPELL_ATTR2/3_TRIGGERED_CAN_TRIGGER_PROC.
// Putting the attribute on the inner spell satisfies neither, so instead the
// clones are built instant (CastingTimeIndex 0), free (cost 0) and GCD-less
// (StartRecovery 0) and are cast with TRIGGERED_NONE: a normal cast whose
// procs, crits, spell power and charged mods (Empowered Imp) all behave like
// the original. The wrapper itself carries SPELL_ATTR3_CANT_TRIGGER_PROC (no
// hit procs from the dummy), SPELL_ATTR3_IGNORE_HIT_RESULT and
// SPELL_ATTR1_CANT_BE_REFLECTED (one hit roll and one reflect roll per bolt,
// both on the clone) and SPELL_ATTR0_NEGATIVE_1 (the heuristic would read a
// dummy as positive). The wrapper's own cast-phase proc is what drops the
// charge of cast-time mods it consumed (Shadow Trance, Backlash, Backdraft -
// spell_proc PROC_ATTR_REQ_SPELLMOD); the clones have CastingTimeIndex 0 so
// CalcCastTime never registers those mods a second time.
//
// Casting a non-triggered spell from inside another spell's hit is an
// established pattern here (spell_gen_cannibalize, spell_gen_mounted_charge):
// Unit::SetCurrentCastSpell cannot cancel the wrapper because it is
// m_executedCurrently, and the clone simply takes over the GENERIC slot in
// DELAYED state, exactly where the original's missile would have been.
class spell_t2_shadow_bolt_wrapper : public SpellScript
{
    PrepareSpellScript(spell_t2_shadow_bolt_wrapper);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        std::vector<uint32> ids = { SPELL_T2_FELFLAME_BOLT };
        for (ShadowBoltRank const& rank : ShadowBoltRanks)
        {
            ids.push_back(rank.wrapper);
            ids.push_back(rank.shadowClone);
            ids.push_back(rank.fireClone);
        }
        return ValidateSpellInfo(ids);
    }

    bool Load() override
    {
        _rank = FindShadowBoltRankByWrapper(GetSpellInfo()->Id);
        return _rank != nullptr;
    }

    // The wrapper is shadow (it inherits the rank's SchoolMask, so a shadow
    // lockout still blocks it, and school-based cost mods still see shadow).
    // A Felflame wearer's bolt lands as fire, though, and SpellHistory::IsReady
    // would reject the fire clone after the cast time had already been spent.
    // Fail up front instead, the same way the original fails on its own school.
    SpellCastResult CheckFireLockout()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_FELFLAME_BOLT))
            return SPELL_CAST_OK;

        if (SpellHistory const* history = caster->GetSpellHistory())
            if (history->IsSchoolLocked(SPELL_SCHOOL_MASK_FIRE))
                return SPELL_FAILED_NOT_READY;

        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !_rank)
            return;

        // The wrapper can only be reflected if its dbc row lost
        // SPELL_ATTR1_CANT_BE_REFLECTED; never bolt yourself regardless.
        if (target == caster)
            return;

        bool const felflame = caster->HasAura(SPELL_T2_FELFLAME_BOLT);
        uint32 const innerId = felflame ? _rank->fireClone : _rank->shadowClone;

        // TRIGGERED_NONE on purpose - see the class comment. The clone is
        // instant, free and GCD-less by data, so the only things a normal cast
        // re-checks here (range, LoS, facing, lockout) were just verified for
        // the wrapper with identical fields.
        caster->CastSpell(target, innerId, CastSpellExtraArgs(TRIGGERED_NONE));

        if (felflame)
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Felflame Bolt - Shadow Bolt rank {} -> fire clone {} at {}",
                caster->GetName(), uint32(_rank - ShadowBoltRanks.data()) + 1, innerId, target->GetName()));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_t2_shadow_bolt_wrapper::CheckFireLockout);
        OnEffectHitTarget += SpellEffectFn(spell_t2_shadow_bolt_wrapper::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }

    ShadowBoltRank const* _rank = nullptr;
};

// 90320 - Legion of One (imp 8pc). The imp form is pure data (effect 1 is a
// TRANSFORM to creature 416, effect 2 (added 2026-08-19) is MOD_SCALE +100%
// so the form is twice the imp's size, and the carrier gained
// CASTABLE_WHILE_MOUNTED so the form can still mount); effect 0 is the
// one-second poll the dbc file reserved for the shared-death check.
//
// What the poll does today is keep Lesser Imp (90375) leased onto the imp, so
// the halving tracks the set: equipping the 8pc with the imp already out halves
// it within a second, and unequipping - or logging in without the set - lets
// the lease run out.
//
// TODO(deferred - the four-imp half is NOT implemented): three extra imps and
// the shared death are deliberately left out rather than half-built.
//   * TrinityCore allows exactly one permanent Pet, so the extras must be
//     Guardians, and 90300-90394 allocated no summon spell for them (90375 is
//     the whole warlock-imp helper budget). Building a Guardian by hand means
//     Map::SummonCreature with a borrowed SummonProperties row plus CharmInfo,
//     PetAI and faction wiring, and a teardown on every dismiss/zone/logout
//     path - none of which has a precedent in this fork's Custom scripts.
//   * The shared death would have to Kill() or unsummon three guardians from
//     inside this periodic tick, i.e. from inside the aura machinery. That is
//     precisely the re-entrancy the open heap-corruption investigation is
//     chasing; it needs a flag drained from a Player/World update tick instead.
class spell_t2_imp_legion : public AuraScript
{
    PrepareAuraScript(spell_t2_imp_legion);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        if (Player* warlock = GetTarget()->ToPlayer())
            StampLesserImp(warlock->GetPet());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        // The lease would expire on its own, but the wearer should not keep a
        // halved imp for ten seconds after taking the set off.
        if (Player* warlock = GetTarget()->ToPlayer())
            if (Pet* pet = warlock->GetPet())
                pet->RemoveAurasDueToSpell(SPELL_T2_LESSER_IMP);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_imp_legion::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_imp_legion::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 688 - Summon Imp, carrying the Legion of One rider. The poll above would pick
// a fresh imp up anyway; this only closes the one-second window in which a
// newly summoned imp would still hit for full.
class spell_t2_imp_legion_summon : public SpellScript
{
    PrepareSpellScript(spell_t2_imp_legion_summon);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        Player* warlock = caster->ToPlayer();
        if (!warlock || !warlock->HasAura(SPELL_T2_LEGION_OF_ONE))
            return;
        StampLesserImp(warlock->GetPet());
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_t2_imp_legion_summon::HandleAfterCast);
    }
};

// 90321 - Equilibrium of Blood (life-tap 3pc): damage taken drops by up to 5%,
// peaking at exactly half mana and falling to nothing at either extreme. No
// aura type can key an amount off a resource, so effect 0 polls once a second
// and rewrites effect 1's amount in place.
//
// Rewriting the amount is enough on its own: SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN
// is a HandleNoImmediateEffect aura, read live out of GetTotalAuraMultiplierBy-
// MiscMask by SpellDamageBonusTaken and MeleeDamageBonusTaken. Nothing is
// re-applied, so the aura container is never touched from inside the tick.
class spell_t2_mana_equilibrium : public AuraScript
{
    PrepareAuraScript(spell_t2_mana_equilibrium);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        AuraEffect* reduction = GetEffect(EFFECT_1);
        if (!reduction)
            return;

        Unit* target = GetTarget();
        int32 amount = 0;
        if (uint32 const maxMana = target->GetMaxPower(POWER_MANA))
        {
            // Linear tent, as specified on the 90321 row: 0% and 100% mana give
            // nothing, 50% gives the full 5%. Negative, because
            // MOD_DAMAGE_PERCENT_TAKEN is applied as a straight AddPct.
            float const manaPct = float(target->GetPower(POWER_MANA)) * 100.0f / float(maxMana);
            float const reduce = 5.0f * (1.0f - std::fabs(manaPct - 50.0f) / 50.0f);
            amount = -int32(std::lround(reduce));
        }

        if (reduction->GetAmount() == amount)
            return;

        reduction->SetAmount(amount);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Equilibrium of Blood - mana {}/{}, damage taken {}%",
            target->GetName(), target->GetPower(POWER_MANA), target->GetMaxPower(POWER_MANA), amount));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_mana_equilibrium::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 31818 - the Life Tap energize, carrying the life-tap 5pc (90322): the mana
// handed back is doubled.
//
// Bound to 31818 and not to Life Tap itself because spell_warl_life_tap does
// all the arithmetic - base value, half of shadow spell power with the level
// penalty, Improved Life Tap - and then delivers the result by casting 31818
// with it as base point 0. 31818 is the only place the final number exists.
//
// Mana Feed (32553) is deliberately left alone: it is a percentage of the tap
// taken before this doubling, and it is the pet's cut, not the warlock's.
//
// The other half of 90322 - "no mana regeneration by any means but Life Tap" -
// is core: effect 1 of the carrier is SPELL_AURA_PREVENT_REGENERATE_POWER for
// POWER_MANA (Player::Regenerate), and T2UnitHooks::BlocksManaGain zeroes every
// other POWER_MANA gain in Unit::EnergizeBySpell / Player::Regenerate while
// letting 31818 (and Life Tap itself) through. The doubled value below still
// arrives via Spell::EffectEnergize -> EnergizeBySpell(31818), so it passes.
class spell_t2_blood_for_power : public SpellScript
{
    PrepareSpellScript(spell_t2_blood_for_power);

    void HandleEnergize(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_BLOOD_FOR_POWER))
            return;

        int32 const doubled = GetEffectValue() * 2;
        SetEffectValue(doubled);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Blood for Power - Life Tap doubled to {} mana",
            caster->GetName(), doubled));
    }

    void Register() override
    {
        // EFFECT_FIRST_FOUND: 31818 is a single-effect energize, but matching
        // the effect type rather than a fixed index survives a dbc edit.
        OnEffectHitTarget += SpellEffectFn(spell_t2_blood_for_power::HandleEnergize, EFFECT_FIRST_FOUND, SPELL_EFFECT_ENERGIZE);
    }
};

void AddSC_custom_t2_shaman_warlock()
{
    RegisterSpellScript(spell_t2_purge_offering);
    RegisterSpellScript(spell_t2_shadow_bolt_wrapper);
    RegisterSpellScript(spell_t2_imp_legion);
    RegisterSpellScript(spell_t2_imp_legion_summon);
    RegisterSpellScript(spell_t2_mana_equilibrium);
    RegisterSpellScript(spell_t2_blood_for_power);
}
