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
 *   sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql
 * Bindings:
 *   sql/custom/world/2026_08_17_01_world_t2_shaman-warlock.sql
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Pet.h"
#include "Player.h"
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
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
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

    // Flurry is charge-based, not stacking: the buff always lands with its full
    // 3 swing charges, so "grants N charges" means capping it back down. An
    // untalented shaman has no Flurry rank and gets nothing - deliberate, and
    // noted on the 90317 row in the dbc file.
    void GrantFlurryCharges(Player* player, uint8 grant)
    {
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
            "[CustomAuras] {}: Pyre Offering - no Flurry rank trained, no charges granted",
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
}

// 81324 \ 81325 - the Purge wrappers, carrying both totem offerings: purging
// your OWN totem stops being a wasted global and becomes the payoff.
//   90314 Rimeward Offering (frost-shock 8pc): the totem gets a 500 absorb and
//         Purge goes on 10 sec.
//   90317 Pyre Offering (flame-shock 8pc): the totem is destroyed, the shaman
//         gets 2 Flurry charges and Purge goes on 15 sec.
//
// This rides the shipped spell_sha_purge rather than reimplementing Purge:
// spell_script_names is a multimap, so both scripts run on the same cast and
// spell_sha_purge keeps owning the enemy dispel and the Rehgar's Mercy path.
//
// TODO(blocked - one line in spell_shaman.cpp): the payload below is complete
// but unreachable, because spell_sha_purge::CheckMagicDispel rejects every
// friendly target that is not a Rehgar's Mercy crowd-control cleanse:
//     if (caster->IsFriendlyTo(target))
//     {
//         if (!caster->HasAura(SPELL_SHAMAN_REHGARS_MERCY))
//             return SPELL_FAILED_BAD_TARGETS;
//         return HasRehgarsMercyCrowdControl(target) ? SPELL_CAST_OK
//                                                   : SPELL_FAILED_NOTHING_TO_DISPEL;
//     }
// Your own totem is friendly and carries no crowd control, so both arms reject
// it. A second script CANNOT override that: Spell::CallScriptCheckCastHandlers
// keeps the FIRST non-OK result (`if (retVal == SPELL_CAST_OK) retVal = ...`),
// so returning SPELL_CAST_OK from here is ignored regardless of script order.
// The gate has to open inside CheckMagicDispel itself, before the friendly
// rejection - an own-totem exception for a wearer of 90314/90317.
class spell_t2_purge_offering : public SpellScript
{
    PrepareSpellScript(spell_t2_purge_offering);

    void HandleOwnTotem(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        Player* shaman = caster->ToPlayer();
        if (!shaman)
            return;

        Totem* totem = target->ToTotem();
        if (!totem || !totem->GetOwner() || totem->GetOwner()->GetGUID() != shaman->GetGUID())
            return;

        // The two are different 8pc sets, so nobody can wear both; Rimeward
        // wins if the impossible happens, because Pyre would destroy the totem
        // the shield was just put on.
        bool const rimeward = shaman->HasAura(SPELL_T2_RIMEWARD_OFFERING);
        bool const pyre = !rimeward && shaman->HasAura(SPELL_T2_PYRE_OFFERING);
        if (!rimeward && !pyre)
            return;

        // Effect 0 of the wrapper is the TRIGGER_SPELL that fires the real
        // Purge (370/8012) - a dispel attempt on our own totem is meaningless
        // and would eat the totem's own aura, so it never runs here.
        PreventHitDefaultEffect(effIndex);

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

        GrantFlurryCharges(shaman, 2);
        AddVisiblePurgeCooldown(shaman, PYRE_PURGE_COOLDOWN_MS);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Pyre Offering - {} consumed, Purge on 15s",
            shaman->GetName(), totem->GetName()));

        // Delayed by a millisecond on purpose. Totem::UnSummon strips auras
        // from the shaman and clears his totem slot, and we are standing
        // inside a spell effect handler that is still holding this totem as
        // its current target; the msTime arm queues a ForcedUnsummonDelayEvent
        // instead, so the teardown happens on the creature's own update.
        totem->UnSummon(1);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_t2_purge_offering::HandleOwnTotem, EFFECT_0, SPELL_EFFECT_TRIGGER_SPELL);
    }
};

// 90320 - Legion of One (imp 8pc). The imp form is pure data (effect 1 is a
// TRANSFORM to creature 416 and the carrier gained CASTABLE_WHILE_MOUNTED so
// the form can still mount); effect 0 is the one-second poll the dbc file
// reserved for the shared-death check.
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
// TODO(core): the other half of 90322 - "no mana regeneration by any means but
// Life Tap" - is only half data. Effect 1 is SPELL_AURA_PREVENT_REGENERATE_POWER
// for POWER_MANA, which Player::Regenerate honours, so passive and Spirit regen
// are already dead. Every other source (potions, runes, Innervate, Mana Spring,
// Replenishment, Shadowfiend) arrives through Unit::EnergizeBySpell, which has
// no script seam - it needs a guard there that lets 31818/32553 through and
// zeroes POWER_MANA gains for anyone carrying 90322.
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
    RegisterSpellScript(spell_t2_imp_legion);
    RegisterSpellScript(spell_t2_imp_legion_summon);
    RegisterSpellScript(spell_t2_mana_equilibrium);
    RegisterSpellScript(spell_t2_blood_for_power);
}
