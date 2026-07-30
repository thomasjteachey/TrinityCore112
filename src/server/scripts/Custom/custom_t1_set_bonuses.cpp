/*
 * Legionnaire Plus T1 set bonuses - the scripted half.
 *
 * The plain aura bonuses (cost/range/crit/energy modifiers) live entirely in
 * Spell.dbc as passives 90101-90135 and need no code. Everything here is a
 * bonus no aura type can express: off-hand-only behaviour, cross-spell
 * triggers, and conditional state. Each passive is a dummy the ItemSet grants;
 * the scripts below either sit on that dummy or on the ability it modifies,
 * checking for the dummy on the caster.
 *
 * Wiring lives in ItemSet.dbc (SetSpellID/SetThreshold), written by the forge
 * tooling; spell_script_names binds these classes to their spell ids.
 */

#include "ScriptMgr.h"
#include "CellImpl.h"
#include "Chat.h"
#include "SpellDefines.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
    enum T1SetSpells
    {
        // fury
        SPELL_T1_FURY_RAGE_PASSIVE      = 90128,
        SPELL_T1_FURY_RAGE_ENERGIZE     = 90129,
        SPELL_T1_FURY_SPEED_PASSIVE     = 90130,
        SPELL_T1_FURY_SPEED_BUFF        = 90131,
        // hunter
        SPELL_T1_SCORPID_PASSIVE        = 90106,
        SPELL_T1_SCORPID_DEBUFF         = 90137,
        SPELL_T1_HM_PASSIVE             = 90108,
        SPELL_T1_HM_DEBUFF              = 90138,
        SPELL_T1_FLARE_PASSIVE          = 90109,
        SPELL_T1_LESSER_MARK            = 90110,
        // priest
        SPELL_T1_WRAITH_SLOW_PASSIVE    = 90112,
        SPELL_T1_WRAITH_SLOW            = 90113,
        SPELL_T1_MINDFLAY_PASSIVE       = 90114,
        SPELL_T1_MC_SNARE_PASSIVE       = 90116,
        SPELL_T1_MC_SNARE               = 90117,
        // rogue
        SPELL_T1_POISON_CD_PASSIVE      = 90121,
        SPELL_T1_SHELLCRACKER_PASSIVE   = 90123,
        SPELL_SHELL_CRACKER_BUFF        = 83718,
        // shaman
        SPELL_T1_REHGAR_FLURRY_PASSIVE  = 90126,
        SPELL_BLURRY                    = 89745,
        SPELL_IMPROVED_FLURRY_TALENT    = 89746,
        // paladin
        SPELL_T1_JUDGEMENT_PASSIVE      = 90132,
        SPELL_T1_SEAL_PERSIST_PASSIVE   = 90133,
        SPELL_JUDGEMENT_OF_THE_CRUSADER = 21183,
        // druid
        SPELL_T1_THINNERVATE_PASSIVE    = 90135,
        SPELL_T1_FERAL_CHARGE_PASSIVE   = 90136,
        SPELL_CLEARCASTING              = 16870,

        SPELL_PRIEST_SHADOW_WRAITH      = 89784,
        NPC_PRIEST_SHADOW_WRAITH        = 89784,
        SPELL_SWP                       = 589,
        SPELL_HUNTERS_MARK              = 1130,

        // phase 3
        SPELL_T1_SOW_MANA_PASSIVE       = 90139,
        SPELL_T1_HOLY_SHIELD_PASSIVE    = 90140,
        SPELL_T1_HOLY_SHIELD_WARD       = 90141,
        SPELL_T1_RECKONING_PASSIVE      = 90142,
        SPELL_T1_SOLSTICE_PASSIVE       = 90143,
        SPELL_T1_SOLSTICE_DURATION      = 90144,
        SPELL_T1_LUNAR_GRACE_PASSIVE    = 90145,
        SPELL_T1_MOONKIN_SPEED_OFFENSE  = 90147,
        SPELL_T1_MOONKIN_SPEED_DEFENSE  = 90148,
        SPELL_T1_EVOCATION_PASSIVE      = 90149,
        SPELL_T1_EVOCATION_WARD         = 90150,
        SPELL_T1_SCHOOL_STACK           = 90152,
        SPELL_T1_SOULFIRE_DAZE_PASSIVE  = 90156,
        SPELL_T1_LB_RANGE_PASSIVE       = 90157,
        SPELL_T1_ES_REFUND_PASSIVE      = 90158,
        SPELL_DRUID_EMP_REGROWTH        = 81342,
        SPELL_DRUID_EMP_REJUV           = 81343,
        SPELL_WARLOCK_AFTERMATH_DAZE    = 18118,
    };

    bool IsDualWielding1H(Player const* player)
    {
        Item* main = player->GetWeaponForAttack(BASE_ATTACK);
        Item* off = player->GetWeaponForAttack(OFF_ATTACK);
        if (!main || !off)
            return false;
        // Titan's Grip puts a two-hander in each hand and the fury set is
        // explicitly one-handed-only, so subclass is checked, not just slot.
        return main->GetTemplate()->InventoryType != INVTYPE_2HWEAPON
            && off->GetTemplate()->InventoryType != INVTYPE_2HWEAPON;
    }

    // .gm diagnostics customauras - broadcast to every opted-in GM session
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // The Flurry buffs innately apply with their full 3 attack charges; the
    // set bonus grants exactly one swing's worth, stacking up to that cap.
    void GrantOneFlurryCharge(Player* player, uint32 buffId)
    {
        uint8 charges = 0;
        if (Aura* existing = player->GetAura(buffId))
            charges = existing->GetCharges();
        player->CastSpell(player, buffId, true);
        if (Aura* buff = player->GetAura(buffId))
            buff->SetCharges(std::min<uint8>(charges + 1, 3));
    }
}

// 90128 - Frenzied Rhythm (fury 5pc): off-hand hits 50% chance for 1 rage.
// The 50% lives in world.spell_proc; this script only guards WHICH hits count.
class spell_t1_fury_rage : public AuraScript
{
    PrepareAuraScript(spell_t1_fury_rage);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || damageInfo->GetAttackType() != OFF_ATTACK)
            return false;
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return false;
        bool const pass = IsDualWielding1H(player);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Frenzied Rhythm off-hand hit {}",
            player->GetName(),
            pass ? "passed the gate - rolling 50%" : "gate FAILED (needs two one-handers)"));
        return pass;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Frenzied Rhythm proc SUCCESS - +5 rage",
            GetTarget()->GetName()));
        GetTarget()->CastSpell(GetTarget(), SPELL_T1_FURY_RAGE_ENERGIZE, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t1_fury_rage::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t1_fury_rage::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 90130 - Unburdened Fury (fury 8pc): +15% speed while dual-wielding 1H, not
// stacking with other movement speed effects. The passive ticks once a second
// (periodic dummy) and toggles the real speed aura, because "does not stack"
// cannot be expressed in the aura system - it has to look at what else is up.
class spell_t1_fury_speed : public AuraScript
{
    PrepareAuraScript(spell_t1_fury_speed);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        bool wanted = IsDualWielding1H(player);
        if (wanted)
        {
            // any OTHER positive speed effect suppresses ours
            for (AuraEffect const* eff : player->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_SPEED))
                if (eff->GetId() != SPELL_T1_FURY_SPEED_BUFF && eff->GetAmount() > 0)
                {
                    wanted = false;
                    break;
                }
        }

        bool has = player->HasAura(SPELL_T1_FURY_SPEED_BUFF);
        if (wanted && !has)
            player->CastSpell(player, SPELL_T1_FURY_SPEED_BUFF, true);
        else if (!wanted && has)
            player->RemoveAurasDueToSpell(SPELL_T1_FURY_SPEED_BUFF);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_T1_FURY_SPEED_BUFF);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t1_fury_speed::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t1_fury_speed::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 14277 - Scorpid Sting (Rank 4), carrying the Beekeeper 8pc rider: the sting
// also reduces all damage the target deals by 2% while it is up.
class spell_t1_scorpid_r4 : public AuraScript
{
    PrepareAuraScript(spell_t1_scorpid_r4);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T1_SCORPID_PASSIVE))
            return;
        caster->CastSpell(GetTarget(), SPELL_T1_SCORPID_DEBUFF, true);
        if (Aura* rider = GetTarget()->GetAura(SPELL_T1_SCORPID_DEBUFF, caster->GetGUID()))
        {
            rider->SetMaxDuration(GetAura()->GetDuration());
            rider->SetDuration(GetAura()->GetDuration());
        }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            GetTarget()->RemoveAura(SPELL_T1_SCORPID_DEBUFF, caster->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t1_scorpid_r4::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t1_scorpid_r4::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 1130 - Hunter's Mark, carrying the Tracker 5pc rider: +75 Fire and Arcane
// damage taken while marked.
class spell_t1_hunters_mark : public AuraScript
{
    PrepareAuraScript(spell_t1_hunters_mark);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();

        // Mark hierarchy: the proper Hunter's Mark outranks the lesser one.
        // A lesser mark landing on a properly-marked target backs off; a
        // proper mark landing evicts any lesser mark (before the rider is
        // cast, so the eviction cannot strip the fresh rider below).
        if (GetId() == SPELL_T1_LESSER_MARK)
        {
            if (target->GetAuraOfRankedSpell(SPELL_HUNTERS_MARK))
            {
                GetAura()->Remove();
                return;
            }
        }
        else
            target->RemoveAurasDueToSpell(SPELL_T1_LESSER_MARK);

        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T1_HM_PASSIVE))
            return;
        caster->CastSpell(target, SPELL_T1_HM_DEBUFF, true);
        if (Aura* rider = target->GetAura(SPELL_T1_HM_DEBUFF, caster->GetGUID()))
        {
            rider->SetMaxDuration(GetAura()->GetDuration());
            rider->SetDuration(GetAura()->GetDuration());
        }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            GetTarget()->RemoveAura(SPELL_T1_HM_DEBUFF, caster->GetGUID());
    }

    void Register() override
    {
        // EFFECT_FIRST_FOUND, not EFFECT_0: this script also rides Lesser
        // Hunter's Mark (90110), whose effect 0 (the stealth reveal) is empty.
        AfterEffectApply += AuraEffectApplyFn(spell_t1_hunters_mark::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t1_hunters_mark::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 1543 - Flare's area aura, carrying the Tracker 8pc rider: enemies inside the
// flare are given Lesser Hunter's Mark for 45 sec. The lesser mark (90110) has
// only the ranged-AP effect - deliberately no stealth reveal.
class spell_t1_flare : public AuraScript
{
    PrepareAuraScript(spell_t1_flare);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !caster->HasAura(SPELL_T1_FLARE_PASSIVE))
            return;
        if (!caster->IsValidAttackTarget(target))
            return;
        // the proper Hunter's Mark outranks the lesser one - don't even cast
        if (target->GetAuraOfRankedSpell(SPELL_HUNTERS_MARK))
            return;

        // Lesser Hunter's Mark IS Hunter's Mark rank 4 in the dbc - same
        // values, same family mask (so anything that modifies Hunter's Mark
        // modifies this too), minus the MOD_STALKED reveal. Nothing to
        // compute here; the spell carries its own numbers.
        caster->CastSpell(target, SPELL_T1_LESSER_MARK, true);
        if (Aura* mark = target->GetAura(SPELL_T1_LESSER_MARK, caster->GetGUID()))
        {
            mark->SetMaxDuration(45 * IN_MILLISECONDS);
            mark->SetDuration(45 * IN_MILLISECONDS);
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t1_flare::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90112 - Grasp of Gloom (shadow 5pc): while the Fade wraith is out, enemies
// within 5 yards of it are slowed 35%. Ticks once a second on the priest and
// pulses a short slow, so leaving the radius wears off within two seconds.
class spell_t1_gloom_wraith : public AuraScript
{
    PrepareAuraScript(spell_t1_gloom_wraith);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->HasAura(SPELL_PRIEST_SHADOW_WRAITH))
            return;

        Creature* wraith = nullptr;
        for (Unit* summon : player->m_Controlled)
            if (summon->GetEntry() == NPC_PRIEST_SHADOW_WRAITH)
            {
                wraith = summon->ToCreature();
                break;
            }
        if (!wraith)
            return;

        std::list<Unit*> targets;
        Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(wraith, player, 5.0f);
        Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(wraith, targets, check);
        Cell::VisitAllObjects(wraith, searcher, 5.0f);
        for (Unit* enemy : targets)
        {
            player->CastSpell(enemy, SPELL_T1_WRAITH_SLOW, true);
            if (Aura* slow = enemy->GetAura(SPELL_T1_WRAITH_SLOW, player->GetGUID()))
            {
                slow->SetMaxDuration(2 * IN_MILLISECONDS);
                slow->SetDuration(2 * IN_MILLISECONDS);
            }
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t1_gloom_wraith::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 15407 - Mind Flay, carrying the Gloom 8pc rider: each cast extends Shadow
// Word: Pain on the target by 3 sec, up to 6 sec over its base duration.
class spell_t1_mind_flay : public SpellScript
{
    PrepareSpellScript(spell_t1_mind_flay);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        if (!caster->HasAura(SPELL_T1_MINDFLAY_PASSIVE))
        {
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Gloom mind flay - no 8pc passive (90114), no extension",
                caster->GetName()));
            return;
        }

        // ranked lookup: the priest casts whatever SWP rank they know, not rank 1
        Aura* swp = target->GetAuraOfRankedSpell(SPELL_SWP, caster->GetGUID());
        if (!swp)
        {
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Gloom mind flay - no Shadow Word: Pain of yours on {}",
                caster->GetName(), target->GetName()));
            return;
        }

        // CalcMaxDuration, not the raw spell duration: Improved SWP already
        // stretches the applied aura (18s -> 24s), and diffing against the
        // untalented base made the cap sit exactly where the talent put it -
        // "+6 sec over base" must mean over what the priest actually applies.
        int32 const base = swp->CalcMaxDuration(caster);
        int32 const cap = base + 6 * IN_MILLISECONDS;
        if (swp->GetMaxDuration() >= cap)
        {
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Gloom mind flay - SWP already at cap ({:.1f}s max)",
                caster->GetName(), swp->GetMaxDuration() / 1000.0f));
            return;
        }
        int32 const extend = std::min(3 * IN_MILLISECONDS, cap - swp->GetMaxDuration());
        swp->SetMaxDuration(swp->GetMaxDuration() + extend);
        swp->SetDuration(swp->GetDuration() + extend);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Gloom mind flay - SWP extended +{:.1f}s, now {:.1f}s left of {:.1f}s max",
            caster->GetName(), extend / 1000.0f,
            swp->GetDuration() / 1000.0f, swp->GetMaxDuration() / 1000.0f));
    }

    // fires on channel start regardless of gates - proves the script is bound
    void HandleAfterCast()
    {
        if (Unit* caster = GetCaster())
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: mind flay cast (spell {}) - Gloom script is bound",
                caster->GetName(), GetSpellInfo()->Id));
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_t1_mind_flay::HandleAfterCast);
        AfterHit += SpellHitFn(spell_t1_mind_flay::HandleAfterHit);
    }
};

// 605 - Mind Control, carrying the Hypnotism 5pc rider: the victim is snared
// 40% for 4 sec when control ends.
class spell_t1_mind_control : public AuraScript
{
    PrepareAuraScript(spell_t1_mind_control);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T1_MC_SNARE_PASSIVE))
            return;
        caster->CastSpell(GetTarget(), SPELL_T1_MC_SNARE, true);
        if (Aura* snare = GetTarget()->GetAura(SPELL_T1_MC_SNARE, caster->GetGUID()))
        {
            snare->SetMaxDuration(4 * IN_MILLISECONDS);
            snare->SetDuration(4 * IN_MILLISECONDS);
        }
    }

    void Register() override
    {
        // Mind Control's EFFECT_0 is MOD_POSSESS on this core, not MOD_CHARM -
        // matching a specific aura name made the hook silently never bind.
        AfterEffectRemove += AuraEffectRemoveFn(spell_t1_mind_control::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 81308 - Deadly Shot, carrying the Drifter 8pc rider: one Shell Cracker
// stack per combo point spent. Combo points are read BEFORE the cast consumes
// them and applied after the shot lands.
class spell_t1_deadly_shot : public SpellScript
{
    PrepareSpellScript(spell_t1_deadly_shot);

    void CaptureCombo()
    {
        if (Player* player = GetCaster()->ToPlayer())
            _combo = player->GetComboPoints();
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster->HasAura(SPELL_T1_SHELLCRACKER_PASSIVE) || !_combo)
            return;
        caster->CastSpell(caster, SPELL_SHELL_CRACKER_BUFF, true);
        if (Aura* buff = caster->GetAura(SPELL_SHELL_CRACKER_BUFF))
            buff->ModStackAmount(_combo - 1);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_t1_deadly_shot::CaptureCombo);
        AfterCast += SpellCastFn(spell_t1_deadly_shot::HandleAfterCast);
    }

    uint8 _combo = 0;
};

// 82419 - Rehgar's Fury, carrying the Superwolf 8pc rider: the leap grants a
// stack of the shaman's Flurry, and Blurry too with Improved Flurry.
class spell_t1_rehgar_flurry : public SpellScript
{
    PrepareSpellScript(spell_t1_rehgar_flurry);

    void HandleAfterCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player || !player->HasAura(SPELL_T1_REHGAR_FLURRY_PASSIVE))
            return;

        // shaman Flurry: talents 16256/16281-16284 trigger buffs by rank
        static uint32 const talents[5] = { 16284, 16283, 16282, 16281, 16256 };
        static uint32 const buffs[5]   = { 16280, 16279, 16278, 16277, 16257 };
        for (uint8 i = 0; i < 5; ++i)
            if (player->HasSpell(talents[i]) || player->HasAura(talents[i]))
            {
                GrantOneFlurryCharge(player, buffs[i]);
                break;
            }

        if (player->HasSpell(SPELL_IMPROVED_FLURRY_TALENT) || player->HasAura(SPELL_IMPROVED_FLURRY_TALENT))
            player->CastSpell(player, SPELL_BLURRY, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_t1_rehgar_flurry::HandleAfterCast);
    }
};

// 89758 - Thinnervate, carrying the feral 5pc rider: each tick also restores
// 2 energy in Cat Form or 1 rage in Bear Form.
class spell_t1_thinnervate : public AuraScript
{
    PrepareAuraScript(spell_t1_thinnervate);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();
        if (!target->HasAura(SPELL_T1_THINNERVATE_PASSIVE))
            return;
        if (target->GetShapeshiftForm() == FORM_CAT)
            target->EnergizeBySpell(target, GetSpellInfo(), 2, POWER_ENERGY);
        else if (target->GetShapeshiftForm() == FORM_BEAR || target->GetShapeshiftForm() == FORM_DIREBEAR)
            target->EnergizeBySpell(target, GetSpellInfo(), 10, POWER_RAGE); // rage is stored x10
        (void)aurEff;
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t1_thinnervate::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_ENERGIZE);
    }
};

// 16979 / 49377 - Feral Charge (bear and cat), carrying the feral 8pc rider:
// arriving on a target that is mid-cast grants Clearcasting. The charge's own
// interrupt lands with the hit, so "was casting when we arrived" is the same
// event as "we interrupted it".
class spell_t1_feral_charge : public SpellScript
{
    PrepareSpellScript(spell_t1_feral_charge);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_T1_FERAL_CHARGE_PASSIVE))
            return;
        if (target->IsNonMeleeSpellCast(false, false, true))
            caster->CastSpell(caster, SPELL_CLEARCASTING, true);
    }

    void Register() override
    {
        // AfterHit rather than an effect hook: bear charge is EFFECT_0
        // SPELL_EFFECT_CHARGE but cat charge is a jump+triggers layout, and
        // one effect-matched handler cannot fit both.
        AfterHit += SpellHitFn(spell_t1_feral_charge::HandleAfterHit);
    }
};

// 20185 / 20186 - Judgement of Light / Wisdom debuffs, carrying the ret 3pc
// rider: judging also applies Judgement of the Crusader.
class spell_t1_judgement_crusader : public AuraScript
{
    PrepareAuraScript(spell_t1_judgement_crusader);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (caster && caster->HasAura(SPELL_T1_JUDGEMENT_PASSIVE))
            caster->CastSpell(GetTarget(), SPELL_JUDGEMENT_OF_THE_CRUSADER, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t1_judgement_crusader::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 20424 / 25742 - Seal of Command / Righteousness proc damage: the ret 8pc
// trades 35% of seal damage for the 6-second twist window (see Unit.cpp).
class spell_t1_seal_damage : public SpellScript
{
    PrepareSpellScript(spell_t1_seal_damage);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        if (caster && caster->HasAura(SPELL_T1_SEAL_PERSIST_PASSIVE))
            SetHitDamage(CalculatePct(GetHitDamage(), 65));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_t1_seal_damage::HandleHit);
    }
};

// 20351 \ 20350 \ 20168 - Seal of Wisdom mana procs, carrying the prot 3pc:
// the restored mana is increased by 10%.
class spell_t1_sow_mana : public SpellScript
{
    PrepareSpellScript(spell_t1_sow_mana);

    void HandleEnergize(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (caster && caster->HasAura(SPELL_T1_SOW_MANA_PASSIVE))
            SetEffectValue(CalculatePct(GetEffectValue(), 110));
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_t1_sow_mana::HandleEnergize, EFFECT_FIRST_FOUND, SPELL_EFFECT_ENERGIZE);
    }
};

// 20925 - Holy Shield, carrying the prot 5pc: -3% spell damage taken while
// the shield is up, via a hidden ward toggled with the shield itself.
class spell_t1_holy_shield : public AuraScript
{
    PrepareAuraScript(spell_t1_holy_shield);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (target->HasAura(SPELL_T1_HOLY_SHIELD_PASSIVE))
            target->CastSpell(target, SPELL_T1_HOLY_SHIELD_WARD, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_T1_HOLY_SHIELD_WARD);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t1_holy_shield::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t1_holy_shield::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 19750 \ 635 - Flash of Light and Holy Light, carrying the prot 8pc: any
// Reckoning charges are consumed and the heal grows by 40%.
class spell_t1_reckoning_heal : public SpellScript
{
    PrepareSpellScript(spell_t1_reckoning_heal);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T1_RECKONING_PASSIVE))
            return;
        // whichever Reckoning aura actually carries the charges on this core
        static uint32 const reckoning[] = { 20178, 20177, 20179, 20180, 20181, 20182, 32746 };
        for (uint32 id : reckoning)
            if (Aura* charges = caster->GetAura(id))
            {
                charges->Remove();
                SetHitHeal(AddPct(GetHitHeal(), 40));
                SendCustomAuraDiag(Trinity::StringFormat(
                    "[CustomAuras] {}: Righteous Reckoning consumed {} - heal +40%",
                    caster->GetName(), id));
                return;
            }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_t1_reckoning_heal::HandleHit);
    }
};

// 81342 \ 81343 - Empowered Regrowth \ Rejuvenation, carrying the balance
// 3pc: while either is up, Regrowth and Rejuvenation last 3 sec longer
// (hidden duration spellmod, removed when the last empowered effect fades).
class spell_t1_empowered_duration : public AuraScript
{
    PrepareAuraScript(spell_t1_empowered_duration);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (target->HasAura(SPELL_T1_SOLSTICE_PASSIVE))
            target->CastSpell(target, SPELL_T1_SOLSTICE_DURATION, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        uint32 const other = GetId() == SPELL_DRUID_EMP_REGROWTH
            ? SPELL_DRUID_EMP_REJUV : SPELL_DRUID_EMP_REGROWTH;
        if (!target->HasAura(other))
            target->RemoveAurasDueToSpell(SPELL_T1_SOLSTICE_DURATION);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t1_empowered_duration::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t1_empowered_duration::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 8921 - Moonfire, carrying the balance 5pc: each tick has a 15% chance to
// grant Empowered Rejuvenation or Empowered Regrowth.
class spell_t1_moonfire_empower : public AuraScript
{
    PrepareAuraScript(spell_t1_moonfire_empower);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T1_LUNAR_GRACE_PASSIVE))
            return;
        if (!roll_chance_i(15))
            return;
        caster->CastSpell(caster, urand(0, 1) ? SPELL_DRUID_EMP_REJUV
                                              : SPELL_DRUID_EMP_REGROWTH, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t1_moonfire_empower::HandlePeriodic, EFFECT_FIRST_FOUND, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// 90146 - Celestial Momentum (balance 8pc): once a second, count this
// druid's Moonfire\Insect Swarm on enemies and Regrowth\Rejuvenation on
// allies within 100 yd, and mirror the counts into two stacking 4% speed
// auras. Moonkin form only.
class spell_t1_moonkin_speed : public AuraScript
{
    PrepareAuraScript(spell_t1_moonkin_speed);

    static void SetStacks(Unit* druid, uint32 spellId, uint32 count)
    {
        if (!count)
        {
            druid->RemoveAurasDueToSpell(spellId);
            return;
        }
        if (!druid->HasAura(spellId))
            druid->CastSpell(druid, spellId, true);
        if (Aura* aura = druid->GetAura(spellId))
        {
            aura->SetStackAmount(uint8(std::min<uint32>(count, 99)));
            aura->RefreshDuration();
        }
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* druid = GetTarget();
        if (druid->GetShapeshiftForm() != FORM_MOONKIN)
        {
            druid->RemoveAurasDueToSpell(SPELL_T1_MOONKIN_SPEED_OFFENSE);
            druid->RemoveAurasDueToSpell(SPELL_T1_MOONKIN_SPEED_DEFENSE);
            return;
        }
        std::list<Unit*> units;
        Trinity::AnyUnitInObjectRangeCheck check(druid, 100.0f);
        Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(druid, units, check);
        Cell::VisitAllObjects(druid, searcher, 100.0f);
        uint32 offense = 0, defense = 0;
        for (Unit* unit : units)
            for (auto const& pair : unit->GetAppliedAuras())
            {
                Aura const* aura = pair.second->GetBase();
                if (aura->GetCasterGUID() != druid->GetGUID())
                    continue;
                SpellInfo const* info = aura->GetSpellInfo();
                if (info->SpellFamilyName != SPELLFAMILY_DRUID)
                    continue;
                // moonfire 0x2, insect swarm 0x200000 - hostile side
                if ((info->SpellFamilyFlags[0] & 0x200002) && druid->IsValidAttackTarget(unit))
                    ++offense;
                // regrowth 0x40, rejuvenation 0x10 - friendly side
                else if ((info->SpellFamilyFlags[0] & 0x50) && !druid->IsValidAttackTarget(unit))
                    ++defense;
            }
        SetStacks(druid, SPELL_T1_MOONKIN_SPEED_OFFENSE, offense);
        SetStacks(druid, SPELL_T1_MOONKIN_SPEED_DEFENSE, defense);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t1_moonkin_speed::HandlePeriodic, EFFECT_FIRST_FOUND, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// 12051 - Evocation, carrying the mage 3pc: full pushback immunity for
// exactly as long as the channel aura is on the mage.
class spell_t1_evocation_pushback : public AuraScript
{
    PrepareAuraScript(spell_t1_evocation_pushback);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (target->HasAura(SPELL_T1_EVOCATION_PASSIVE))
            target->CastSpell(target, SPELL_T1_EVOCATION_WARD, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_T1_EVOCATION_WARD);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t1_evocation_pushback::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t1_evocation_pushback::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90151 - Prismatic Insight (mage 5pc): damaging spell hits from a school
// different to the previous CAST add a stack; repeating a school clears the
// stacks. Triggered spells and extra targets of the same cast are ignored -
// only deliberate casts move the tracker.
class spell_t1_school_stacks : public AuraScript
{
    PrepareAuraScript(spell_t1_school_stacks);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!info || !damageInfo || !damageInfo->GetDamage())
            return false;
        if (Spell const* spell = eventInfo.GetProcSpell())
            if (spell->IsTriggered())
                return false;

        uint32 const now = getMSTime();
        // several targets hit by one cast arrive as separate procs - same
        // spell inside half a second is the same button press
        if (info->Id == _lastSpellId && now - _lastTimeMs < 500)
            return false;
        _lastSpellId = info->Id;
        _lastTimeMs = now;

        uint32 const school = info->GetSchoolMask();
        bool const repeat = _lastSchool && school == _lastSchool;
        _lastSchool = school;
        if (repeat)
        {
            GetTarget()->RemoveAurasDueToSpell(SPELL_T1_SCHOOL_STACK);
            return false;
        }
        return true;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        Unit* mage = GetTarget();
        if (Aura* stack = mage->GetAura(SPELL_T1_SCHOOL_STACK))
            stack->ModStackAmount(1);   // refreshes duration, capped at 15
        else
            mage->CastSpell(mage, SPELL_T1_SCHOOL_STACK, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t1_school_stacks::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t1_school_stacks::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }

private:
    uint32 _lastSchool = 0;
    uint32 _lastSpellId = 0;
    uint32 _lastTimeMs = 0;
};

// 6353 - Soul Fire, carrying the destro 8pc: always dazes for 5 sec.
class spell_t1_soul_fire : public SpellScript
{
    PrepareSpellScript(spell_t1_soul_fire);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_T1_SOULFIRE_DAZE_PASSIVE))
            return;
        caster->CastSpell(target, SPELL_WARLOCK_AFTERMATH_DAZE, true);
        if (Aura* daze = target->GetAura(SPELL_WARLOCK_AFTERMATH_DAZE, caster->GetGUID()))
        {
            daze->SetMaxDuration(5 * IN_MILLISECONDS);
            daze->SetDuration(5 * IN_MILLISECONDS);
        }
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_t1_soul_fire::HandleAfterHit);
    }
};

// 403 - Lightning Bolt, carrying the ele 5pc: up to +8% damage with
// distance, maxing at 36 yards.
class spell_t1_lightning_range : public SpellScript
{
    PrepareSpellScript(spell_t1_lightning_range);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_T1_LB_RANGE_PASSIVE))
            return;
        float const dist = std::min(caster->GetDistance(target), 36.0f);
        int32 const pct = int32(dist / 36.0f * 8.0f);
        if (pct > 0)
            SetHitDamage(AddPct(GetHitDamage(), pct));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_t1_lightning_range::HandleHit);
    }
};

// 8042 - Earth Shock, carrying the ele 8pc: a successful interrupt refunds
// the shock's mana cost. "Was casting before the hit, is not after" is what
// a successful interrupt looks like from here.
class spell_t1_earthshock_refund : public SpellScript
{
    PrepareSpellScript(spell_t1_earthshock_refund);

    void HandleBeforeHit(SpellMissInfo /*missInfo*/)
    {
        if (Unit* target = GetHitUnit())
            _wasCasting = target->IsNonMeleeSpellCast(false, false, true);
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !_wasCasting)
            return;
        if (!caster->HasAura(SPELL_T1_ES_REFUND_PASSIVE))
            return;
        if (target->IsNonMeleeSpellCast(false, false, true))
            return;   // still casting - nothing was interrupted
        int32 const cost = GetSpellInfo()->CalcPowerCost(caster, GetSpellInfo()->GetSchoolMask());
        if (cost > 0)
        {
            caster->EnergizeBySpell(caster, GetSpellInfo(), cost, POWER_MANA);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Grounded Retort - interrupt refunded {} mana",
                caster->GetName(), cost));
        }
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_t1_earthshock_refund::HandleBeforeHit);
        AfterHit += SpellHitFn(spell_t1_earthshock_refund::HandleAfterHit);
    }

private:
    bool _wasCasting = false;
};

void AddSC_custom_t1_set_bonuses()
{
    RegisterSpellScript(spell_t1_sow_mana);
    RegisterSpellScript(spell_t1_holy_shield);
    RegisterSpellScript(spell_t1_reckoning_heal);
    RegisterSpellScript(spell_t1_empowered_duration);
    RegisterSpellScript(spell_t1_moonfire_empower);
    RegisterSpellScript(spell_t1_moonkin_speed);
    RegisterSpellScript(spell_t1_evocation_pushback);
    RegisterSpellScript(spell_t1_school_stacks);
    RegisterSpellScript(spell_t1_soul_fire);
    RegisterSpellScript(spell_t1_lightning_range);
    RegisterSpellScript(spell_t1_earthshock_refund);
    RegisterSpellScript(spell_t1_fury_rage);
    RegisterSpellScript(spell_t1_fury_speed);
    RegisterSpellScript(spell_t1_scorpid_r4);
    RegisterSpellScript(spell_t1_hunters_mark);
    RegisterSpellScript(spell_t1_flare);
    RegisterSpellScript(spell_t1_gloom_wraith);
    RegisterSpellScript(spell_t1_mind_flay);
    RegisterSpellScript(spell_t1_mind_control);
    RegisterSpellScript(spell_t1_deadly_shot);
    RegisterSpellScript(spell_t1_rehgar_flurry);
    RegisterSpellScript(spell_t1_thinnervate);
    RegisterSpellScript(spell_t1_feral_charge);
    RegisterSpellScript(spell_t1_judgement_crusader);
    RegisterSpellScript(spell_t1_seal_damage);
}
