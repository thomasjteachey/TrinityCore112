/*
 * T2 set-bonus hooks that live in the SPELL pipeline - bodies for T2SpellHooks.h.
 *
 * Call sites:
 *   IsShockUnbound            SpellHistory (StartCooldown / HasCooldown /
 *                             GetRemainingCooldown / SyncShockCooldownsToClient)
 *   OnPlayerCancelCast        WorldSession::HandleCancelCastOpcode
 *   OnInterruptWhileNotCasting Spell::EffectInterruptCast
 *   OnImmuneSpellHit          Spell::TargetInfo::PreprocessTarget
 *   OnImmuneMeleeHit          Unit::CalculateMeleeDamage (physical-immune branch)
 *   OnCancelAuraRequest       WorldSession::HandleCancelAuraOpcode (first statement)
 *   OnCastSpellRequest        WorldSession::HandleCastSpellOpcode (before prepare)
 *   WaivesShapeshiftRestriction Spell::CheckCast (CheckShapeshift refusal)
 *
 * Every body is a cheap early-out for anyone who does not carry the set-bonus
 * aura it keys on.
 */

#include "T2SpellHooks.h"
#include "Chat.h"
#include "GameTime.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Unit.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"
#include <mutex>
#include <unordered_map>

namespace
{
    // .gm diagnostics customauras - same channel the T2 script files use, so a
    // live test can watch the core half of these bonuses too. Nothing here is
    // reached unless a GM has opted in AND the hook already decided it has work.
    void SendCustomAuraDiag(std::string const& msg)
    {
        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // -----------------------------------------------------------------------
    // UMBRAL MERCY
    // -----------------------------------------------------------------------

    // How long a swallowed CMSG_CANCEL_AURA(Shadowform) waits for the heal's
    // CMSG_CAST_SPELL before it is honoured after all. ZERO - one player tick,
    // not a wall-clock grace - and that is exact rather than optimistic:
    //
    //   * CMSG_CANCEL_AURA is PROCESS_INPLACE, so it is drained by
    //     World::UpdateSessions().
    //   * CMSG_CAST_SPELL is PROCESS_THREADSAFE, so it is drained by the
    //     per-map session pass at the TOP of Map::Update().
    //   * Player::Update() (and with it Unit::Update -> m_Events.Update) runs
    //     in the object-update pass AFTER that, in the same Map::Update.
    //   * WorldSession::Update pops from a single FIFO through
    //     LockedQueue::next(), which peeks the HEAD and stops when the filter
    //     rejects it. A packet can therefore never overtake one that was sent
    //     before it, whichever filter each of them belongs to.
    //
    // So a cancel and the cast the client sent behind it in the same frame are
    // always both handled before the event fires, even if they arrive in
    // different world ticks. An offset of zero is "after the rest of this
    // client frame", which is the shortest deadline that is still correct - and
    // it makes a deliberate toggle come off on the very same tick it was
    // pressed instead of a third of a second later.
    //
    // The old 300 ms wall-clock grace was ALSO starvable: every press replaced
    // the pending mark with a new stamp and pushed the deadline out, so a
    // player mashing the button faster than once per 300 ms kept Shadowform on
    // for ever. That is the reported "can't leave shadowform" bug. A zero
    // offset cannot be starved, and the second-press rule below is the belt to
    // that brace.
    constexpr uint32 UMBRAL_CANCEL_GRACE_MS = 0;

    // A cast request only matches a pending cancel inside this window. Wider
    // than the grace so a tick that runs late still pairs the two packets;
    // anything older is a leftover from a cancel whose event died with a map
    // change or logout and must not be acted on.
    constexpr uint32 UMBRAL_PENDING_MAX_AGE_MS = 1000;

    // The heals the 5pc lets through: a priest spell Shadowform explicitly
    // forbids (its StancesNot carries the form - exactly the set the client
    // auto-unshifts for) that heals directly (SPELL_EFFECT_HEAL) or over time
    // (SPELL_AURA_PERIODIC_HEAL). Against the binary Spell.dbc that is Lesser
    // Heal, Heal, Greater Heal, Flash Heal, Binding Heal, Prayer of Healing,
    // Circle of Healing, Desperate Prayer and Renew - the tooltip's list.
    // Prayer of Mending (a proc aura, effect 142) and Holy Nova (damage plus a
    // linked heal) do not match and keep the stock behaviour on purpose: their
    // healing cannot be priced by the script.
    bool IsUmbralHeal(SpellInfo const* spellInfo)
    {
        if (!spellInfo || spellInfo->SpellFamilyName != SPELLFAMILY_PRIEST)
            return false;

        if (spellInfo->CheckShapeshift(FORM_SHADOW) == SPELL_CAST_OK)
            return false;

        return spellInfo->HasEffect(SPELL_EFFECT_HEAL) || spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL);
    }

    // Swallowed Shadowform cancels: player -> GameTime stamp of the cancel. The
    // removal event carries the same stamp, so a mark consumed by a heal (or
    // superseded by a later cancel) is recognised and left alone. Map updates
    // run on several threads; the map is shared, so it is locked. Nobody
    // without 90340 ever reaches the lock.
    std::mutex                               s_umbralMutex;
    std::unordered_map<ObjectGuid, uint32>   s_umbralPendingCancel;

    bool UmbralIsShadowPriest(Player const* player)
    {
        return player && player->HasAura(T2SpellHooks::SPELL_UMBRAL_MERCY)
            && player->GetShapeshiftForm() == FORM_SHADOW;
    }

    // Shaman family flags (Spell.dbc SpellClassMask_1, identical on every rank).
    constexpr uint32 SHOCK_FLAG_EARTH = 0x00100000;
    constexpr uint32 SHOCK_FLAG_FLAME = 0x10000000;
    constexpr uint32 SHOCK_FLAG_FROST = 0x80000000;
    constexpr uint32 SHOCK_CATEGORY   = 19;

    // The reflect helper - SPELL_EFFECT_SCHOOL_DAMAGE (frost), bp supplied at
    // cast time, NO_DONE_BONUS / FIXED_DAMAGE / CANT_CRIT / IGNORE_HIT_RESULT /
    // CANT_TRIGGER_PROC / USABLE_WHILE_STUNNED. Row in dbc.spell_lplus, see
    // sql/custom/dbc/2026_08_19_01_dbc_t2_core.sql.
    constexpr uint32 SPELL_REFLECTIVE_ICE_DAMAGE = 90400;
    constexpr uint32 REFLECT_PERCENT             = 50;

    // Feint Cadence threshold fallback when the carrier's effect amount is not
    // readable (it is bp 999 -> 1000 ms in the DBC; keep the two in sync).
    constexpr int32 FEINT_CADENCE_MIN_ELAPSED_MS = 1000;

    // Victim must be a living player in Ice Block carrying Reflective Carapace,
    // attacker must be a living hostile that is not the victim itself.
    bool ReflectApplies(Unit* attacker, Unit* victim)
    {
        if (!attacker || !victim || attacker == victim)
            return false;

        if (!victim->IsAlive() || !attacker->IsAlive())
            return false;

        Player* mage = victim->ToPlayer();
        if (!mage)
            return false;

        if (!mage->HasAura(T2SpellHooks::SPELL_REFLECTIVE_ICE))
            return false;

        if (!mage->HasAura(T2SpellHooks::SPELL_ICE_BLOCK))
            return false;

        if (!mage->IsHostileTo(attacker) && !attacker->IsHostileTo(mage))
            return false;

        return true;
    }

    void DealReflect(Unit* attacker, Unit* victim, uint32 preventedDamage, char const* source)
    {
        int32 reflected = preventedDamage ? int32(CalculatePct(preventedDamage, REFLECT_PERCENT)) : 0;
        if (reflected <= 0)
        {
            // A zero estimate is a silent no-op that looks exactly like "the
            // bonus does nothing", so say it out loud on the diag channel.
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Reflective Carapace - {} from {} prevented 0 estimated damage, nothing to reflect",
                victim->GetName(), source, attacker->GetName()));
            return;
        }

        if (!sSpellMgr->GetSpellInfo(SPELL_REFLECTIVE_ICE_DAMAGE))
        {
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Reflective Carapace - helper spell {} is not in the loaded Spell.dbc, no reflect",
                victim->GetName(), SPELL_REFLECTIVE_ICE_DAMAGE));
            return;
        }

        // Cast by the blocked mage at the attacker, triggered (the mage is stunned
        // by Ice Block - TRIGGERED_FULL_MASK ignores caster auras) so it shows up
        // in the combat log as the mage's Frost damage.
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(reflected);
        victim->CastSpell(attacker, SPELL_REFLECTIVE_ICE_DAMAGE, args);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Reflective Carapace - {} from {} prevented {}, reflected {} frost",
            victim->GetName(), source, attacker->GetName(), preventedDamage, reflected));
    }

    // Why a reflect that should have happened did not. Only ever called on the
    // (rare) immune-hit path, and only for a player who is actually in Ice
    // Block, so it costs nobody anything.
    void ReportReflectMiss(Unit* attacker, Unit* victim, char const* source)
    {
        Player* mage = victim ? victim->ToPlayer() : nullptr;
        if (!mage || !mage->HasAura(T2SpellHooks::SPELL_ICE_BLOCK))
            return;

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Reflective Carapace did NOT fire for {} - carrier 90344 {}, attacker {}, hostile {}",
            mage->GetName(), source,
            mage->HasAura(T2SpellHooks::SPELL_REFLECTIVE_ICE) ? "present" : "MISSING (set bonus was never granted)",
            attacker ? (attacker->IsAlive() ? "alive" : "dead") : "none",
            attacker && (mage->IsHostileTo(attacker) || attacker->IsHostileTo(mage)) ? "yes" : "no"));
    }

    // Rough "what would this spell have done" for a spell that was flatly immune.
    // Direct-damage effects only: SCHOOL_DAMAGE through the caster's spell-damage
    // bonus, weapon-damage effects through one weapon roll plus the melee bonus.
    // No crit, no target-side modifiers - approximate by design, the number only
    // feeds a 50 % reflect.
    uint32 EstimateImmuneSpellDamage(Unit* caster, Unit* victim, SpellInfo const* spellInfo)
    {
        uint32 total = 0;
        bool weaponRolled = false;
        uint32 weaponDamage = 0;

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if (!effect.IsEffect())
                continue;

            switch (effect.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                {
                    int32 base = effect.CalcValue(caster);
                    if (base <= 0)
                        continue;

                    total += caster->SpellDamageBonusDone(victim, spellInfo, uint32(base), SPELL_DIRECT_DAMAGE, effect, {});
                    break;
                }
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                {
                    WeaponAttackType attType = spellInfo->GetAttackType();
                    if (!weaponRolled)
                    {
                        weaponRolled = true;
                        weaponDamage = caster->CalculateDamage(attType, effect.Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG, true);
                    }

                    int32 value = effect.CalcValue(caster);
                    uint32 dmg = weaponDamage;
                    if (effect.Effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE)
                        dmg = value > 0 ? CalculatePct(weaponDamage, value) : 0;
                    else if (value > 0)
                        dmg += uint32(value);

                    if (!dmg)
                        continue;

                    total += caster->MeleeDamageBonusDone(victim, dmg, attType, spellInfo, spellInfo->GetSchoolMask());
                    // One weapon roll per spell: the percent effect already carries
                    // the whole swing, extra flat effects are additive bonuses.
                    weaponDamage = 0;
                    break;
                }
                default:
                    break;
            }
        }

        return total;
    }
}

bool T2SpellHooks::IsShockUnbound(Unit const* caster, SpellInfo const* spellInfo)
{
    if (!caster || !spellInfo)
        return false;

    if (spellInfo->SpellFamilyName != SPELLFAMILY_SHAMAN || spellInfo->GetCategory() != SHOCK_CATEGORY)
        return false;

    if (spellInfo->SpellFamilyFlags[0] & SHOCK_FLAG_FLAME)
        return caster->HasAura(SPELL_UNBOUND_EMBER);

    if (spellInfo->SpellFamilyFlags[0] & SHOCK_FLAG_FROST)
        return caster->HasAura(SPELL_UNBOUND_RIME);

    return false;
}

void T2SpellHooks::OnPlayerCancelCast(Player* player, Spell* cancelled)
{
    if (!player || !cancelled)
        return;

    // Only a real cast bar that is still running: generic spell, has a cast
    // time, not yet launched. Channels arrive as CMSG_CANCEL_CHANNELLING and
    // instants have nothing to cancel.
    if (cancelled->getState() != SPELL_STATE_PREPARING || cancelled->GetCastTime() <= 0)
        return;

    AuraEffect const* carrier = player->GetAuraEffect(SPELL_FEINT_CADENCE, EFFECT_0);
    if (!carrier)
        return;

    int32 minElapsed = carrier->GetAmount();
    if (minElapsed <= 0)
        minElapsed = FEINT_CADENCE_MIN_ELAPSED_MS;

    if (cancelled->GetCastTimeElapsed() < minElapsed)
        return;

    // 90393: aura 65 +10 % cast speed, 4 s, AuraInterruptFlags CAST so it is
    // consumed at the START of the next cast (after Spell::prepare has already
    // computed that cast's time with the buff in place).
    player->CastSpell(player, SPELL_FEINT_CADENCE_BUFF, true);
}

void T2SpellHooks::OnInterruptWhileNotCasting(Unit* interrupter, Unit* target)
{
    if (!interrupter || !target || interrupter == target)
        return;

    if (!target->IsAlive())
        return;

    // Set-bonus aura, so in practice only players - but keep it generic and cheap.
    if (!target->HasAura(SPELL_DEAD_AIR))
        return;

    // "When an enemy interrupts you" - friendly interrupts (and self) do not count.
    if (!interrupter->IsHostileTo(target) && !target->IsHostileTo(interrupter))
        return;

    // 90394: aura 65 +25 % cast speed, 5 s, consumed at the start of the next cast.
    target->CastSpell(target, SPELL_DEAD_AIR_BUFF, true);
}

void T2SpellHooks::OnImmuneSpellHit(Unit* caster, Unit* victim, SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return;

    // Never reflect the reflection: two blocked mages would otherwise bounce
    // 90400 back and forth for ever.
    if (spellInfo->Id == SPELL_REFLECTIVE_ICE_DAMAGE)
        return;

    // Only hostile, damaging casts count as "attacks against you".
    if (spellInfo->IsPositive())
        return;

    if (!ReflectApplies(caster, victim))
    {
        // The one place the whole feature can be observed from outside: this
        // hook is called for EVERY immune hostile spell hit, not just a
        // carrier's, so the diag can say whether the 8pc aura is even there.
        ReportReflectMiss(caster, victim, "an immune spell");
        return;
    }

    uint32 prevented = EstimateImmuneSpellDamage(caster, victim, spellInfo);
    char const* spellName = spellInfo->SpellName[sWorld->GetDefaultDbcLocale()];
    DealReflect(caster, victim, prevented, spellName ? spellName : "a spell");
}

void T2SpellHooks::OnImmuneMeleeHit(Unit* attacker, Unit* victim, uint32 wouldBeDamage)
{
    if (!ReflectApplies(attacker, victim))
    {
        // Unit::CalculateMeleeDamage already gated this call on the victim
        // carrying 90344, so getting here means Ice Block, life or hostility
        // failed - worth naming.
        ReportReflectMiss(attacker, victim, "an immune melee swing");
        return;
    }

    DealReflect(attacker, victim, wouldBeDamage, "a melee swing");
}

bool T2SpellHooks::OnCancelAuraRequest(Player* player, uint32 spellId)
{
    if (!player || spellId != SPELL_SHADOWFORM)
        return false;

    // Holders only; and without Shadowform up the stock path is a no-op anyway.
    if (!player->HasAura(SPELL_UMBRAL_MERCY) || !player->HasAura(SPELL_SHADOWFORM))
        return false;

    uint32 const stamp = GameTime::GetGameTimeMS();
    bool secondPress = false;
    {
        std::lock_guard<std::mutex> guard(s_umbralMutex);
        auto itr = s_umbralPendingCancel.find(player->GetGUID());
        if (itr != s_umbralPendingCancel.end())
        {
            // A cancel is already waiting on its resolve event. The client
            // auto-unshifts exactly once per cast attempt and always follows it
            // with the cast in the same frame, so a SECOND cancel arriving
            // before the first was resolved cannot be an auto-unshift - it is
            // the player pressing the button. Honour it on the spot; the
            // pending event then finds no mark and does nothing.
            s_umbralPendingCancel.erase(itr);
            secondPress = true;
        }
        else
            s_umbralPendingCancel[player->GetGUID()] = stamp;
    }

    if (secondPress)
    {
        player->RemoveOwnedAura(SPELL_SHADOWFORM, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Umbral Mercy - second Shadowform cancel before the first resolved, form dropped now",
            player->GetName()));
        return true;
    }

    // The removal is deferred past the frame the heal's CMSG_CAST_SPELL shares
    // with this cancel. The event lives on the player's own queue, so it dies
    // with him (logout, map change) and runs from the top of Unit::Update -
    // never from inside aura iteration.
    player->m_Events.AddEventAtOffset([player, stamp]()
    {
        {
            std::lock_guard<std::mutex> guard(s_umbralMutex);
            auto itr = s_umbralPendingCancel.find(player->GetGUID());
            // Consumed by a heal, or superseded by a later cancel that has
            // its own event: not this event's job.
            if (itr == s_umbralPendingCancel.end() || itr->second != stamp)
                return;
            s_umbralPendingCancel.erase(itr);
        }

        // Nobody consumed it: it was a real cancel. Same call the stock
        // handler would have made one tick ago.
        player->RemoveOwnedAura(SPELL_SHADOWFORM, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Umbral Mercy - Shadowform cancel was not followed by a heal, form dropped",
            player->GetName()));
    }, Milliseconds(UMBRAL_CANCEL_GRACE_MS));

    return true;
}

void T2SpellHooks::OnCastSpellRequest(Player* player, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo)
        return;

    // Cheapest test first: almost nobody carries the aura, and only a holder
    // can have a pending mark (OnCancelAuraRequest gates on it too).
    if (!player->HasAura(SPELL_UMBRAL_MERCY))
        return;

    bool honourNow = false;
    bool keptForHeal = false;
    {
        std::lock_guard<std::mutex> guard(s_umbralMutex);
        auto itr = s_umbralPendingCancel.find(player->GetGUID());
        if (itr == s_umbralPendingCancel.end())
            return;

        // A mark older than the window belongs to a cancel whose removal event
        // was dropped (RemoveFromWorld kills the queue); it must neither keep
        // nor drop the form now.
        if (getMSTimeDiff(itr->second, GameTime::GetGameTimeMS()) > UMBRAL_PENDING_MAX_AGE_MS)
        {
            s_umbralPendingCancel.erase(itr);
            return;
        }

        if (IsUmbralHeal(spellInfo))
        {
            // The whole point: consumed even if the cast goes on to fail
            // (range, mana, already casting) - the form stays either way.
            s_umbralPendingCancel.erase(itr);
            keptForHeal = true;
        }

        // Not a heal, but a spell Shadowform forbids (Smite, Holy Fire, Holy
        // Nova, Prayer of Mending...): the client auto-unshifted for THIS
        // spell, so the cancel is honoured right now and the cast then sees
        // exactly what it sees without the set - no form, no refusal.
        // Anything else (Mind Blast after a manual cancel) leaves the mark to
        // its event.
        else if (player->GetShapeshiftForm() == FORM_SHADOW && spellInfo->CheckShapeshift(FORM_SHADOW) != SPELL_CAST_OK)
        {
            s_umbralPendingCancel.erase(itr);
            honourNow = true;
        }
    }

    if (honourNow)
        player->RemoveOwnedAura(SPELL_SHADOWFORM, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);

    if (keptForHeal || honourNow)
    {
        char const* spellName = spellInfo->SpellName[sWorld->GetDefaultDbcLocale()];
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Umbral Mercy - autoUnshift cancel {} by {} (id {})",
            player->GetName(), keptForHeal ? "consumed, Shadowform kept" : "honoured, Shadowform dropped",
            spellName ? spellName : "?", spellInfo->Id));
    }
}

bool T2SpellHooks::WaivesShapeshiftRestriction(Unit const* caster, SpellInfo const* spellInfo)
{
    if (!caster || !spellInfo)
        return false;

    Player const* priest = caster->ToPlayer();
    if (!UmbralIsShadowPriest(priest))
        return false;

    return IsUmbralHeal(spellInfo);
}
