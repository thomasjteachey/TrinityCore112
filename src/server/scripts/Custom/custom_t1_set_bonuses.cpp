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
        return player && IsDualWielding1H(player);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
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
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T1_HM_PASSIVE))
            return;
        caster->CastSpell(GetTarget(), SPELL_T1_HM_DEBUFF, true);
        if (Aura* rider = GetTarget()->GetAura(SPELL_T1_HM_DEBUFF, caster->GetGUID()))
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
        if (!caster || !target || !caster->HasAura(SPELL_T1_MINDFLAY_PASSIVE))
            return;

        // ranked lookup: the priest casts whatever SWP rank they know, not rank 1
        Aura* swp = target->GetAuraOfRankedSpell(SPELL_SWP, caster->GetGUID());
        if (!swp)
            return;

        int32 const base = swp->GetSpellInfo()->GetMaxDuration();
        int32 const cap = base + 6 * IN_MILLISECONDS;
        if (swp->GetMaxDuration() >= cap)
            return;
        int32 const extend = std::min(3 * IN_MILLISECONDS, cap - swp->GetMaxDuration());
        swp->SetMaxDuration(swp->GetMaxDuration() + extend);
        swp->SetDuration(swp->GetDuration() + extend);
    }

    void Register() override
    {
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

void AddSC_custom_t1_set_bonuses()
{
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
