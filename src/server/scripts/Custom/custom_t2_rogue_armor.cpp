/*
 * Legionnaire Plus T2 set bonuses - the rogue-armour group.
 *
 * Carriers 90348-90365 and helpers 90388-90394 are Spell.dbc rows; see
 * sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql, which is the contract
 * this file implements. Everything the aura system can express already lives
 * there - what is left here is the behaviour no aura type has: a rider that
 * must match another aura's duration, a trail that has to exist in the world
 * rather than on a unit, a spellmod that has to be traded for a root, and a
 * "have you been running in a straight line" question that no packet answers.
 *
 * Sets covered: rogue-ice-fang (90348/90349/90350), rogue-deadly-poison
 * (90352/90353), mail-momentum (90358/90359), leather-mister-reset
 * (90360/90362). Four bonuses in the group are deliberately NOT implemented -
 * see the deferred block at the bottom of the file, which names the hook and
 * the obstacle for each.
 *
 * Structure, naming and idioms follow custom_t1_set_bonuses.cpp; where a bonus
 * has a T1 twin the T1 approach is copied rather than re-invented.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "Unit.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace
{
    enum T2RogueArmorSpells
    {
        // rogue-ice-fang
        SPELL_T2_CHILLING_POISON        = 90348,
        SPELL_T2_ICE_SKATE              = 90349,
        SPELL_T2_COLDER_BLOOD           = 90350,
        // rogue-deadly-poison
        SPELL_T2_VENOM_SUSTENANCE       = 90352,
        SPELL_T2_RUPTURING_VENOM        = 90353,
        // mail-momentum
        SPELL_T2_MOMENTUM               = 90358,
        SPELL_T2_INERTIAL_GUARD         = 90359,
        // leather-mister-reset
        SPELL_T2_HEARTY_APPETITE        = 90360,
        SPELL_T2_FIELD_DRESSING         = 90362,

        // helpers the carriers above trigger
        SPELL_T2_CHILLING_POISON_SLOW   = 90388,
        SPELL_T2_ICE_TRAIL_PULSE        = 90389,
        SPELL_T2_ICE_TRAIL_PATCH        = 90390,
        SPELL_T2_COLDER_BLOOD_ROOT      = 90391,
        SPELL_T2_MOMENTUM_BUFF          = 90392,

        // the abilities the bonuses ride
        SPELL_ROGUE_COLD_BLOOD          = 14177,
        SPELL_FIRST_AID_RECENTLY_BANDAGED = 11196,
    };

    // Deadly Poison's periodic damage effect, on every rank. Same family mask
    // the core's own spell_rog_deadly_poison matches on.
    constexpr uint32 DEADLY_POISON_FAMILY_MASK_0 = 0x10000;
    constexpr uint32 DEADLY_POISON_FAMILY_MASK_1 = 0x80000;
    constexpr uint8  DEADLY_POISON_RUPTURE_STACKS = 5;

    // .gm diagnostics customauras - broadcast to every opted-in GM session
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // -----------------------------------------------------------------------
    // Momentum (90358 five-piece, 90359 eight-piece)
    //
    // "Running in a straight line" cannot be answered from a single movement
    // packet. Keyboard turning shows up as MOVEMENTFLAG_LEFT/RIGHT, but mouse
    // turning arrives as a bare MSG_MOVE_SET_FACING carrying no flag at all,
    // so the heading has to be sampled and compared. It is compared against
    // the heading the current leg STARTED with, not against the previous
    // sample: a per-sample delta sits under the noise floor of ordinary aim
    // jitter and would clear the stacks about once a second.
    //
    // The state is polled from PlayerScript::OnUpdate rather than hooked into
    // WorldSession::HandleMovementOpcodes. Player::Update calls that hook
    // after Unit::Update has returned - the one point the core documents as
    // safe for adding and removing auras - and it keeps this bonus inside the
    // one file the group owns.
    // -----------------------------------------------------------------------
    constexpr uint32 MOMENTUM_POLL_MS  = 250;
    constexpr uint32 MOMENTUM_STACK_MS = 3000;
    constexpr float  MOMENTUM_PI = 3.14159265f;
    constexpr float  MOMENTUM_TWO_PI = 2.0f * MOMENTUM_PI;
    constexpr float  MOMENTUM_TURN_TOLERANCE = MOMENTUM_PI / 6.0f;   // 30 degrees
    // Fraction of run speed a poll must actually cover. A quarter sits under
    // walking pace and under any survivable snare, so only a genuinely stuck
    // player fails it.
    constexpr float  MOMENTUM_MIN_SPEED_FRACTION = 0.25f;

    struct MomentumRun
    {
        uint32 sincePollMs    = 0;
        uint32 sinceStackMs   = 0;
        float  refOrientation = 0.0f;
        float  lastX          = 0.0f;
        float  lastY          = 0.0f;
        bool   hasLastPos     = false;
    };

    // Map updates run on several threads, so the tracker is guarded like
    // LosBlocker's registry. Nothing touches it unless the wearer actually has
    // the five-piece, which is what keeps the lock off the hot path.
    std::mutex                                  s_momentumMutex;
    std::unordered_map<ObjectGuid, MomentumRun> s_momentum;

    enum class MomentumAction : uint8 { None, Break, Stack };

    void MomentumForget(ObjectGuid const& guid)
    {
        std::lock_guard<std::mutex> guard(s_momentumMutex);
        s_momentum.erase(guid);
    }

    MomentumAction MomentumAdvance(Player* player, uint32 diff)
    {
        std::lock_guard<std::mutex> guard(s_momentumMutex);
        MomentumRun& run = s_momentum[player->GetGUID()];

        run.sincePollMs += diff;
        if (run.sincePollMs < MOMENTUM_POLL_MS)
            return MomentumAction::None;

        uint32 const elapsed = run.sincePollMs;
        run.sincePollMs = 0;

        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const o = player->GetOrientation();
        float const moved = run.hasLastPos ? player->GetExactDist2d(run.lastX, run.lastY) : 0.0f;
        bool const hadFix = run.hasLastPos;
        run.lastX = x;
        run.lastY = y;
        run.hasLastPos = true;

        // The tooltip's "stopping, turning or strafing", in movement flags.
        // Mouse turning is deliberately absent here and caught below instead.
        uint32 const flags = player->GetUnitMovementFlags();
        bool broken = !player->IsAlive()
            || !(flags & MOVEMENTFLAG_FORWARD)
            || (flags & (MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT
                       | MOVEMENTFLAG_LEFT | MOVEMENTFLAG_RIGHT
                       | MOVEMENTFLAG_ROOT)) != 0;

        if (!broken)
        {
            float delta = std::fabs(o - run.refOrientation);
            if (delta > MOMENTUM_PI)                  // both are normalised to [0, 2pi)
                delta = MOMENTUM_TWO_PI - delta;
            broken = delta > MOMENTUM_TURN_TOLERANCE;
        }

        // Displacement, because running face-first into a wall keeps the
        // forward flag set forever and would otherwise bank free stacks.
        if (!broken && hadFix)
            broken = moved < player->GetSpeed(MOVE_RUN) * (elapsed / 1000.0f) * MOMENTUM_MIN_SPEED_FRACTION;

        if (broken)
        {
            run.sinceStackMs = 0;
            run.refOrientation = o;
            return MomentumAction::Break;
        }

        run.sinceStackMs += elapsed;
        if (run.sinceStackMs < MOMENTUM_STACK_MS)
            return MomentumAction::None;

        // Each stack opens a new leg from the heading we are on now, so a long
        // gentle curve costs progress rather than being flatly impossible.
        run.sinceStackMs = 0;
        run.refOrientation = o;
        return MomentumAction::Stack;
    }
}

// 3409 \ 11201 - Crippling Poison, carrying the Chilling Poison 3pc rider.
// The extra 5% snare is data-only on 90348; the attack-speed half needs its
// own aura, which rides for exactly as long as the poison the rogue applied
// (Improved Poisons and the like move that duration around).
class spell_t2_icefang_chilling_poison : public AuraScript
{
    PrepareAuraScript(spell_t2_icefang_chilling_poison);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_CHILLING_POISON))
            return;
        Unit* target = GetTarget();
        caster->CastSpell(target, SPELL_T2_CHILLING_POISON_SLOW, true);
        if (Aura* rider = target->GetAura(SPELL_T2_CHILLING_POISON_SLOW, caster->GetGUID()))
        {
            rider->SetMaxDuration(GetAura()->GetDuration());
            rider->SetDuration(GetAura()->GetDuration());
        }
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            GetTarget()->RemoveAura(SPELL_T2_CHILLING_POISON_SLOW, caster->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_icefang_chilling_poison::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_icefang_chilling_poison::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 2983 \ 8696 \ 11305 - Sprint, carrying the Ice Skate 5pc. The speed and
// duration changes are data-only on 90349; only the trail needs code, and it
// is driven by a pulse aura rather than from here so that it survives the
// rogue being interrupted, dazed or slowed mid-sprint.
class spell_t2_icefang_ice_trail : public SpellScript
{
    PrepareSpellScript(spell_t2_icefang_ice_trail);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_T2_ICE_SKATE))
            return;
        caster->CastSpell(caster, SPELL_T2_ICE_TRAIL_PULSE, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_t2_icefang_ice_trail::HandleAfterCast);
    }
};

// 90389 - Ice Trail, the 500 ms pulse that lays the patches down.
//
// A patch has to be a thing standing in the world, because 90390 is an
// APPLY_AREA_AURA_ENEMY and an area aura is centred on the unit that owns it.
// The world trigger is only a place to hang it: the ROGUE has to be the aura's
// caster, since enemy area-aura selection validates caster->IsValidAttackTarget
// and a summoned trigger fails that against everything - the documented
// spell_t1_gloom_wraith failure.
class spell_t2_icefang_ice_trail_pulse : public AuraScript
{
    PrepareAuraScript(spell_t2_icefang_ice_trail_pulse);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* rogue = GetTarget()->ToPlayer();
        if (!rogue || !rogue->IsAlive())
            return;

        // One patch per 2.5 yards rather than one per pulse. The patches are
        // 3 yards across, so this still leaves an unbroken trail, while a
        // sprinting rogue who stops drops nothing and stacks nothing.
        if (_hasLast && rogue->GetExactDist2d(_lastX, _lastY) < 2.5f)
            return;
        _lastX = rogue->GetPositionX();
        _lastY = rogue->GetPositionY();
        _hasLast = true;

        TempSummon* patch = rogue->SummonCreature(WORLD_TRIGGER,
            rogue->GetPositionX(), rogue->GetPositionY(), rogue->GetPositionZ(),
            0.0f, TEMPSUMMON_TIMED_DESPAWN, Milliseconds(5000));
        if (!patch)
            return;
        patch->SetReactState(REACT_PASSIVE);

        // The world trigger inherits its template's blanket mechanic and
        // school immunities, which would make it reject the very aura it was
        // summoned to hold. Creature::LoadTemplateImmunities keys those on
        // uint32 max, so the same key lifts exactly the two this patch needs -
        // and only on this one summon.
        patch->ApplySpellImmune(std::numeric_limits<uint32>::max(), IMMUNITY_MECHANIC, MECHANIC_SNARE, false);
        patch->ApplySpellImmune(std::numeric_limits<uint32>::max(), IMMUNITY_SCHOOL, SPELL_SCHOOL_MASK_FROST, false);

        if (!rogue->AddAura(SPELL_T2_ICE_TRAIL_PATCH, patch))
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ice Trail - patch carrier REFUSED the slow aura "
                "(world trigger {} immunities)", rogue->GetName(), uint32(WORLD_TRIGGER)));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_t2_icefang_ice_trail_pulse::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    float _lastX = 0.0f;
    float _lastY = 0.0f;
    bool  _hasLast = false;
};

// 14177 - Cold Blood, the Colder Blood 8pc half that removes the guarantee.
// The modifier is zeroed rather than the aura removed: the aura is what the
// client draws and what the charge consumption rides on, so it has to stay.
class spell_t2_icefang_colder_blood : public AuraScript
{
    PrepareAuraScript(spell_t2_icefang_colder_blood);

    void CalcCrit(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (GetUnitOwner()->HasAura(SPELL_T2_COLDER_BLOOD))
            amount = 0;
    }

    void Register() override
    {
        // SPELL_AURA_ANY, not the modifier type by name: naming the aura is
        // exactly what made the T1 mind-control hook silently never bind.
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_t2_icefang_colder_blood::CalcCrit, EFFECT_FIRST_FOUND, SPELL_AURA_ANY);
    }
};

// 14177 - Cold Blood, the other half of the 8pc: the root the crit is traded
// for. Split from the AuraScript above because one class cannot be both, and
// AfterCast keeps the root out of the aura-application frame entirely.
class spell_t2_icefang_colder_blood_root : public SpellScript
{
    PrepareSpellScript(spell_t2_icefang_colder_blood_root);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        Player* rogue = caster ? caster->ToPlayer() : nullptr;
        if (!rogue || !rogue->HasAura(SPELL_T2_COLDER_BLOOD))
            return;

        // Cold Blood is a self-buff, so there is no spell target to inherit;
        // the current selection is the only thing that says who to root.
        Unit* victim = rogue->GetSelectedUnit();
        if (!victim || !rogue->IsValidAttackTarget(victim))
            return;

        // The cast is triggered, so it skips every range and sight check the
        // player would normally be held to - they are applied by hand here
        // instead, from the root spell's own dbc range.
        SpellInfo const* root = sSpellMgr->GetSpellInfo(SPELL_T2_COLDER_BLOOD_ROOT);
        if (!root)
            return;
        if (!rogue->IsWithinDistInMap(victim, root->GetMaxRange(false)) || !rogue->IsWithinLOSInMap(victim))
            return;

        rogue->CastSpell(victim, SPELL_T2_COLDER_BLOOD_ROOT, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_t2_icefang_colder_blood_root::HandleAfterCast);
    }
};

// 90352 - Venom Sustenance (deadly-poison 5pc): poison damage is partly
// returned as health. Which hits reach the script at all is world.spell_proc's
// job; this only decides what counts as "your poisons" and how much comes back.
class spell_t2_deadlypoison_leech : public AuraScript
{
    PrepareAuraScript(spell_t2_deadlypoison_leech);

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
        if (!damageInfo)
            return;
        Unit* rogue = GetTarget();
        int32 const heal = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (heal <= 0)
            return;
        HealInfo healInfo(rogue, rogue, uint32(heal), GetSpellInfo(), GetSpellInfo()->GetSchoolMask());
        rogue->HealBySpell(healInfo);
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

// 90392 - Momentum, the stacking buff. Effect 1's speed is the 5pc and is
// always live; the damage reduction on effect 2 belongs to Inertial Guard, so
// it calculates to zero unless the 8pc is worn. Same shape as the T1
// prismatic buff, and it is why the 8pc needs no spell of its own.
class spell_t2_momentum_buff : public AuraScript
{
    PrepareAuraScript(spell_t2_momentum_buff);

    void CalcGuard(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (!GetUnitOwner()->HasAura(SPELL_T2_INERTIAL_GUARD))
            amount = 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_t2_momentum_buff::CalcGuard, EFFECT_1, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    }
};

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
        AuraEffect const* passive = target->GetAuraEffect(SPELL_T2_FIELD_DRESSING, EFFECT_0);
        if (!passive)
            return;
        Aura* aura = GetAura();
        if (aura->GetMaxDuration() <= 0)     // permanent; nothing sane to subtract from
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

// 90358 \ 90359 - Momentum and Inertial Guard, the polling half. See the
// MomentumRun block above for why this is a poll and not a movement hook.
class player_t2_momentum : public PlayerScript
{
public:
    player_t2_momentum() : PlayerScript("player_t2_momentum") { }

    void OnUpdate(Player* player, uint32 diff) override
    {
        if (!player)
            return;

        // Hot path: every player, every tick. For anyone not wearing the set
        // the whole cost is one aura lookup, and the tracker is never touched.
        if (!player->HasAura(SPELL_T2_MOMENTUM))
        {
            // The set came off (or the wearer zoned) mid-run. The buff is
            // dropped from here rather than from an aura hook on 90358
            // precisely because this point is after Unit::Update has finished
            // with the aura containers.
            if (player->HasAura(SPELL_T2_MOMENTUM_BUFF))
            {
                player->RemoveAurasDueToSpell(SPELL_T2_MOMENTUM_BUFF);
                MomentumForget(player->GetGUID());
            }
            return;
        }

        switch (MomentumAdvance(player, diff))
        {
            case MomentumAction::Break:
                if (player->HasAura(SPELL_T2_MOMENTUM_BUFF))
                {
                    player->RemoveAurasDueToSpell(SPELL_T2_MOMENTUM_BUFF);
                    SendCustomAuraDiag(Trinity::StringFormat(
                        "[CustomAuras] {}: Momentum broken - stacks cleared", player->GetName()));
                }
                break;
            case MomentumAction::Stack:
            {
                if (Aura* buff = player->GetAura(SPELL_T2_MOMENTUM_BUFF))
                    buff->ModStackAmount(1);          // clamps itself at the dbc stack cap
                else
                    player->CastSpell(player, SPELL_T2_MOMENTUM_BUFF, true);
                if (Aura* buff = player->GetAura(SPELL_T2_MOMENTUM_BUFF))
                    SendCustomAuraDiag(Trinity::StringFormat(
                        "[CustomAuras] {}: Momentum now at {} stack(s)",
                        player->GetName(), uint32(buff->GetStackAmount())));
                break;
            }
            case MomentumAction::None:
            default:
                break;
        }
    }

    void OnLogout(Player* player) override
    {
        if (player)
            MomentumForget(player->GetGUID());
    }
};

/* ===========================================================================
 * DEFERRED - not implemented, deliberately, rather than half-implemented.
 *
 * 90355 Frost Aura (plate-icebane 6pc), the Polymorph/Charm break.
 *   Effects 1 and 2 (the frost damage and the snare) are data-only and already
 *   work; effect 3 is the dummy anchor for a script that is not written here.
 *   The obstacle is the design, not the core: the shipped tooltip does not
 *   mention a Polymorph or Charm break at all, and the dbc file's own note asks
 *   whether it breaks those ON THE ENEMIES standing in the aura or ON THE
 *   WEARER. Those are opposite bonuses with opposite hooks -
 *     wearer:   AuraScript on 90355, OnEffectPeriodic, calling
 *               GetTarget()->RemoveAurasByType(SPELL_AURA_MOD_CONFUSE /
 *               SPELL_AURA_MOD_CHARM / SPELL_AURA_MOD_POSSESS) - effectively a
 *               permanent, uncounterable self-cleanse of sheep and charm;
 *     enemies:  nothing, since the 2-second frost tick already breaks
 *               Polymorph on everyone standing in it by dealing damage.
 *   Guessing gives a plate wearer permanent Polymorph immunity in PvP, which is
 *   not a thing to guess at. Needs a design answer, then roughly ten lines.
 *
 * 90356 Tomb of Ice (plate-icebane 8pc).
 *   The dbc plan is a creature_template tomb spawned on death, carrying the
 *   shipped 90214 Obstructing Presence. That cannot work as written:
 *   LosBlocker::SegmentEclipsedBy (src/server/game/Entities/Object/LosBlocker.cpp)
 *   collects candidate blockers with caster->GetPlayerListInGrid(), so it only
 *   ever considers PLAYERS. A summoned creature registers itself via
 *   LosBlocker::Add and is then never looked at, and the tomb blocks nothing.
 *   Two ways forward, both outside this file:
 *     (a) widen LosBlocker to walk units rather than players - it would need a
 *         hostility source other than the blocker's own faction, since a
 *         faction-35 trigger is hostile to nobody; or
 *     (b) drop the creature and put 90214 on the corpse-body of the dead player
 *         instead (90214 is already DEATH_PERSISTENT), removing it from
 *         PlayerScript::OnPlayerRepop so a released ghost does not carry an
 *         invisible wall to the graveyard. That needs no core change but gives
 *         no ice-block model, which the tooltip promises.
 *   Also unresolved either way: the "existing ice-block display id" the dbc
 *   comment relies on is not named anywhere and could not be verified here.
 *
 * 90364 Feint Cadence (cloth-jukebox 5pc).
 *   Needs a hook in WorldSession::HandleCancelCastOpcode plus an elapsed-cast
 *   accessor that does not exist. Both live in files this group does not own.
 *   Exact shape required:
 *     src/server/game/Spells/Spell.h, public:
 *         int32 GetCastTimeElapsed() const { return m_casttime - m_timer; }
 *     src/server/game/Handlers/SpellHandler.cpp, in HandleCancelCastOpcode,
 *     before the InterruptNonMeleeSpells call:
 *         if (Spell* spell = _player->GetCurrentSpell(CURRENT_GENERIC_SPELL))
 *             if (AuraEffect const* eff = _player->GetAuraEffect(90364, EFFECT_0))
 *                 if (spell->GetCastTimeElapsed() >= eff->GetAmount())
 *                     _player->CastSpell(_player, 90393, true);
 *   90393 already carries the whole payoff (aura 65, 4 s, cleared by
 *   AURA_INTERRUPT_FLAG_CAST), so nothing else is needed. The 1000 ms threshold
 *   is read out of 90364's own effect amount, not hardcoded.
 *
 * 90365 Dead Air (cloth-jukebox 8pc).
 *   Needs the generic hook the dbc file asks for, in Spell::EffectInterruptCast
 *   (src/server/game/Spells/SpellEffects.cpp). That function is the only place
 *   that knows whether there WAS a cast to interrupt - by the time any proc or
 *   script hook runs, the answer is "not casting" either way, so it cannot be
 *   reconstructed from here. Exact shape required, at the top of
 *   Spell::EffectInterruptCast, before the current-spell loop:
 *         if (unitTarget->HasAura(90365) && !unitTarget->IsNonMeleeSpellCast(false, false, true))
 *             unitTarget->CastSpell(unitTarget, 90394, true);
 *   90394 carries the payoff. Note 90393 and 90394 both use aura 65
 *   NOT_STACK, so 25% eclipses 10% rather than adding to it - intended.
 * ======================================================================== */

void AddSC_custom_t2_rogue_armor()
{
    RegisterSpellScript(spell_t2_icefang_chilling_poison);
    RegisterSpellScript(spell_t2_icefang_ice_trail);
    RegisterSpellScript(spell_t2_icefang_ice_trail_pulse);
    RegisterSpellScript(spell_t2_icefang_colder_blood);
    RegisterSpellScript(spell_t2_icefang_colder_blood_root);
    RegisterSpellScript(spell_t2_deadlypoison_leech);
    RegisterSpellScript(spell_t2_deadlypoison_burst);
    RegisterSpellScript(spell_t2_momentum_buff);
    RegisterSpellScript(spell_t2_reset_hearty_appetite);
    RegisterSpellScript(spell_t2_reset_field_dressing);

    new player_t2_momentum();
}
