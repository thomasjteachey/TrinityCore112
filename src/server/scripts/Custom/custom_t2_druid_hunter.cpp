/*
 * Legionnaire Plus T2 set bonuses - the druid and hunter half of the scripted
 * ones (carriers 90326, 90328, 90333, 90334, 90335).
 *
 * Same shape as custom_t1_set_bonuses.cpp: the plain bonuses are pure Spell.dbc
 * passives and need no code, and everything here is a bonus no aura type can
 * express - a channel that has to be watched, a pre-hit state test, a cast-cost
 * refund that no single spellmod can target, a school swap, and a geometry
 * poll. Each carrier is an inert dummy the ItemSet grants; the scripts either
 * sit on that dummy or on the ability it modifies and look for the dummy on the
 * caster.
 *
 * Wiring lives in ItemSet.dbc (SetSpellID/SetThreshold); spell_script_names
 * binds these classes to their spell ids - see
 * sql/custom/world/2026_08_17_01_world_t2_druid-hunter.sql, which also carries
 * the one spell_proc row Cinderbite needs.
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
        // beef's regalia (druid, Hurricane)
        SPELL_T2_UNBROKEN_GALE          = 90326,   // 8pc carrier
        SPELL_T2_UNBROKEN_GALE_WARD     = 90376,   // pushback + silence immunity
        SPELL_DRUID_HURRICANE_R3        = 17402,

        // moonkitten (druid)
        SPELL_T2_PROWLING_MOONFIRE      = 90328,   // 5pc carrier
        SPELL_T2_MOONLIT_PREY           = 90378,   // +5% melee damage taken
        SPELL_DRUID_MOONFIRE            = 8921,    // rank 1, for the ranked lookup

        // penguin stalker (hunter)
        SPELL_T2_COLD_EFFICIENCY        = 90333,   // 3pc carrier
        SPELL_T2_CINDERBITE             = 90334,   // 5pc carrier
        SPELL_T2_SLICK_GETAWAY          = 90335,   // 8pc carrier (periodic dummy)
        SPELL_T2_MELTED_ICE             = 90380,   // the melted trap's snare
        SPELL_T2_CINDER_STING           = 90381,   // the fire Serpent Sting
        SPELL_T2_SLICK_GETAWAY_SPRINT   = 90382,   // the 3 s sprint

        SPELL_HUNTER_FROST_TRAP_AURA    = 13810,   // the ice slick's own spell
    };

    // .gm diagnostics customauras - broadcast to every opted-in GM session
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // Cinderbite's melt has an ordering problem that no single hook solves.
    // Freezing Trap carries AURA_INTERRUPT_FLAG_TAKE_DAMAGE, and Unit::DealDamage
    // runs that interrupt sweep BEFORE Unit::ProcSkillsAndAuras - so by the time
    // the fire proc fires, the trap the fire just shattered is already gone and
    // the proc has nothing left to test. The trap's own removal hook therefore
    // drops a breadcrumb that the proc immediately following it in the same call
    // stack picks up.
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

// 17402 - Hurricane (Rank 3), carrying beef's regalia 8pc: the channel cannot
// be interrupted, silenced or pushed back. The core already does exactly this
// for 89766 Beef's Tenacity in Spell.cpp; the set bonus reaches the same place
// from script land instead.
class spell_t2_beef_unstoppable : public AuraScript
{
    PrepareAuraScript(spell_t2_beef_unstoppable);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* druid = GetTarget();
        if (!druid->HasAura(SPELL_T2_UNBROKEN_GALE))
            return;

        druid->CastSpell(druid, SPELL_T2_UNBROKEN_GALE_WARD, true);

        // 90376 is an infinite-duration aura so that it can be torn down with
        // the channel rather than outliving it. Clamping it to whatever is left
        // of the channel is belt and braces: a ward that somehow misses its
        // remove hook still expires on its own instead of following the druid
        // into character_aura.
        if (Aura* ward = druid->GetAura(SPELL_T2_UNBROKEN_GALE_WARD, druid->GetGUID()))
        {
            int32 const remaining = GetAura()->GetDuration();
            if (remaining > 0)
            {
                ward->SetMaxDuration(remaining);
                ward->SetDuration(remaining);
            }
        }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_T2_UNBROKEN_GALE_WARD);
    }

    void Register() override
    {
        // EFFECT_2, not EFFECT_FIRST_FOUND. Hurricane's effect 0 is a dead
        // effect (Effect = 0) that still carries an ApplyAuraName, so
        // FIRST_FOUND resolves to an index the aura has no effect at and the
        // hook silently never binds. Effects 0 and 1 are the enemy-side
        // persistent area auras anyway; effect 2 is the periodic trigger that
        // targets the CASTER, which makes it the channel's own aura on the
        // druid - applied at channel start, removed the moment the channel
        // ends however it ends.
        AfterEffectApply += AuraEffectApplyFn(spell_t2_beef_unstoppable::HandleApply, EFFECT_2, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_beef_unstoppable::HandleRemove, EFFECT_2, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 8921 and ranks - Moonfire, carrying the moonkitten 5pc: a Moonfire landing on
// a target not already burning with this druid's Moonfire is worth a combo
// point, and the target takes 5% more damage from the druid's melee abilities.
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
        if (!GetHitAura())
            return;

        // The debuff is a property of being Moonfired, so every landed cast
        // refreshes it (12 s, matching Moonfire); only the combo point is
        // rationed to a target that was not already burning.
        caster->CastSpell(target, SPELL_T2_MOONLIT_PREY, true);

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
// hunter's fire melts their own Freezing Trap into a Frost Trap slow. The 5pc
// carrier itself is the proc aura; the fire filter is the SchoolMask on its
// spell_proc row, re-checked here so the script still behaves if that row is
// ever edited.
class spell_t2_penguin_melt : public AuraScript
{
    PrepareAuraScript(spell_t2_penguin_melt);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !(damageInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE))
            return false;

        // Self-interference guard, and it is not optional: with this same 5pc
        // worn the hunter's Serpent Sting IS fire, so its first tick on a
        // trapped target would melt the trap the hunter had just set.
        if (SpellInfo const* procSpell = eventInfo.GetSpellInfo())
            if (procSpell->Id == SPELL_T2_CINDER_STING)
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

        // The trap is already gone - the damage that got us here broke it. All
        // that is left to do is leave the consolation snare behind.
        hunter->CastSpell(victim, SPELL_T2_MELTED_ICE, true);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Cinderbite melted a Freezing Trap on {} - Melted Ice applied",
            hunter->GetName(), victim->GetName()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_penguin_melt::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_penguin_melt::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 1978 and ranks - Serpent Sting, carrying the penguin stalker 5pc: the sting
// burns instead of poisoning. 90381 is a byte clone of Serpent Sting rank 12
// with the school and art changed, so it keeps the ranged-weapon requirement,
// the AP scaling and the family mask - anything that modified Serpent Sting
// still modifies it.
class spell_t2_penguin_cinder_sting : public SpellScript
{
    PrepareSpellScript(spell_t2_penguin_cinder_sting);

    void HandleAura(SpellEffIndex effIndex)
    {
        _swapped = false;

        Unit* caster = GetCaster();
        if (!caster || !GetHitUnit() || !caster->HasAura(SPELL_T2_CINDERBITE))
            return;

        // PreventHitAura tears down the sting this cast built while it is still
        // only owned, before Spell::TargetInfo::DoDamageAndTriggers applies it
        // to the target. The replacement is cast in AfterHit rather than here,
        // to keep a second full cast out of the effect-handling loop.
        //
        // 90381 is the rank 12 clone whatever rank was cast, so downranking
        // Serpent Sting under this set is an upgrade, not a saving. Harmless at
        // 80 where rank 12 is what hunters cast anyway; if lower ranks ever
        // matter the fix is rank clones in the DBC, not a fudge here.
        // PreventHitAura alone is not enough: it removes the aura but does not set
        // the prevent-default mask, so EffectApplyAura still runs against it and
        // logs an error with two GetDebugInfo() dumps on every Serpent Sting - into
        // the same `spells` channel the heap-corruption hunt reads.
        PreventHitAura();
        PreventHitDefaultEffect(effIndex);
        _swapped = true;
    }

    void HandleAfterHit()
    {
        if (!_swapped)
            return;
        _swapped = false;

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        caster->CastSpell(target, SPELL_T2_CINDER_STING, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_t2_penguin_cinder_sting::HandleAura, EFFECT_0, SPELL_EFFECT_APPLY_AURA);
        AfterHit += SpellHitFn(spell_t2_penguin_cinder_sting::HandleAfterHit);
    }

private:
    bool _swapped = false;
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
    RegisterSpellScript(spell_t2_beef_unstoppable);
    RegisterSpellScript(spell_t2_moonkitten_moonfire);
    RegisterSpellScript(spell_t2_penguin_cold_efficiency);
    RegisterSpellScript(spell_t2_penguin_trap_break);
    RegisterSpellScript(spell_t2_penguin_melt);
    RegisterSpellScript(spell_t2_penguin_cinder_sting);
    RegisterSpellScript(spell_t2_penguin_slick);
}
