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
#include "Log.h"
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
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace
{
    // .gm diagnostics customauras - same channel the T2 script files use, so a
    // live test can watch the core half of these bonuses too. Nothing here is
    // reached unless a GM has opted in AND the hook already decided it has work.
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

    // -----------------------------------------------------------------------
    // UMBRAL MERCY
    // -----------------------------------------------------------------------

    // A swallowed CMSG_CANCEL_AURA(Shadowform) waits exactly ONE PLAYER TICK for
    // the heal's CMSG_CAST_SPELL before it is honoured after all. That is exact
    // rather than optimistic, and the whole ordering argument is:
    //
    //   * CMSG_CANCEL_AURA is PROCESS_INPLACE, so it is drained by
    //     World::UpdateSessions() - World.cpp line ~2561.
    //   * CMSG_CAST_SPELL is PROCESS_THREADSAFE, so it is drained by the
    //     per-map session pass at the TOP of Map::Update(), inside the
    //     sMapMgr->Update(diff) that follows at World.cpp line ~2607.
    //   * Player::Update() runs in the object-update pass after that same map's
    //     session pass, and t2_priest_mage_update::OnUpdate is called from the
    //     END of Player::Update - after Unit::Update has returned.
    //   * WorldSession::Update pops from a single FIFO through
    //     LockedQueue::next(), which peeks the HEAD and stops when the filter
    //     rejects it. A packet can therefore never overtake one that was sent
    //     before it, whichever filter each of them belongs to.
    //
    // So a cancel and the cast the client sent behind it in the same frame are
    // both handled before the resolve runs, even if they land in different world
    // ticks - and a deliberate toggle comes off on the very tick it was pressed.
    //
    // The old 300 ms wall-clock grace was starvable: every press replaced the
    // pending mark with a new stamp and pushed the deadline out, so a player
    // mashing the button kept Shadowform on for ever. One tick cannot be
    // starved, and the second-press rule below is the belt to that brace.
    //
    // The resolve used to be a zero-offset event on the player's own
    // EventProcessor. It is a PlayerScript tick now: an EventProcessor is
    // emptied by KillAllEvents on a far teleport, a map change and logout, and a
    // removal event that dies that way leaves Shadowform stuck up with nothing
    // left to retry it. See T2SpellHooks::ResolvePendingShadowformCancel.

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

    // When each 90340 holder's client last asked to leave Shadowform. Stamped by
    // OnCancelAuraRequest whether the cancel is swallowed or honoured, and read
    // by CountsAsHealingInShadowform.
    std::unordered_map<ObjectGuid, uint32>   s_umbralFormLeftAt;

    // Fast path for the per-player-tick resolve: nobody on the server has a
    // swallowed cancel outstanding, so ResolvePendingShadowformCancel costs one
    // relaxed load and returns. Kept in step with s_umbralPendingCancel.size()
    // under s_umbralMutex.
    std::atomic<uint32>                      s_umbralPendingCount{ 0 };

    // Must be called with s_umbralMutex held, after every insert/erase.
    void UmbralPublishPendingCount()
    {
        s_umbralPendingCount.store(uint32(s_umbralPendingCancel.size()), std::memory_order_release);
    }

    // A mark is meant to live exactly one player tick. If the resolve never runs
    // - the player left the map, or logged out before his next tick - the mark
    // outlives the session and comes back with the GUID, so anything older than
    // this is treated as debris rather than as a real pending cancel. Generous
    // by three orders of magnitude against the one tick it should take, so it
    // can never fire on a live mark.
    constexpr uint32 UMBRAL_CANCEL_STALE_MS = 2000;

    // How long after the client's Shadowform cancel a landing heal still counts
    // as "healing done in Shadowform". Wide enough to cover the longest heal the
    // 5pc unlocks (Greater Heal, 3 s, plus its hit frame) so a cast that was
    // authorised in the form is priced even if the form came off underneath it;
    // short enough that a priest who left the form on purpose and then healed a
    // while later pays nothing.
    constexpr uint32 UMBRAL_FORM_GRACE_MS = 6000;

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

        if (!mage->HasAura(T2SpellHooks::SPELL_ICE_BLOCK)
            && !mage->HasAura(T2SpellHooks::SPELL_ICE_BLOCK_WOTLK))
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
        if (!mage || (!mage->HasAura(T2SpellHooks::SPELL_ICE_BLOCK)
            && !mage->HasAura(T2SpellHooks::SPELL_ICE_BLOCK_WOTLK)))
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
                // HEALTH_LEECH is direct damage that also heals the caster; it
                // goes through the same spell-damage pipeline. Without it a
                // Drain Life / Death Coil stopped by the block estimated ZERO
                // and reflected nothing.
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_HEALTH_LEECH:
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

    // Holders only. BOTH halves of the form are required, not just the aura: if
    // the aura is up but the shapeshift byte is not FORM_SHADOW (or the other way
    // round) the state is already inconsistent, and swallowing the cancel would
    // turn that into a form the player can never take off. Handing those to the
    // stock path costs nothing - RemoveOwnedAura is what the client asked for.
    if (!player->HasAura(SPELL_UMBRAL_MERCY) || !player->HasAura(SPELL_SHADOWFORM)
        || player->GetShapeshiftForm() != FORM_SHADOW)
        return false;

    uint32 const stamp = GameTime::GetGameTimeMS();
    bool secondPress = false;
    {
        std::lock_guard<std::mutex> guard(s_umbralMutex);

        auto itr = s_umbralPendingCancel.find(player->GetGUID());
        if (itr != s_umbralPendingCancel.end() && getMSTimeDiff(itr->second, stamp) <= UMBRAL_CANCEL_STALE_MS)
        {
            // A cancel is already waiting on its resolve. The client
            // auto-unshifts exactly once per cast attempt and always follows it
            // with the cast in the same frame, so a SECOND cancel arriving
            // before the first was resolved cannot be an auto-unshift - it is
            // the player pressing the button. Honour it on the spot; the resolve
            // then finds no mark and does nothing.
            s_umbralPendingCancel.erase(itr);
            secondPress = true;

            // Unambiguously a deliberate un-shift, so it must NOT leave a
            // healing-in-Shadowform grace behind: a heal the priest presses two
            // seconds after choosing to leave the form is an ordinary heal and
            // is free.
            s_umbralFormLeftAt.erase(player->GetGUID());
        }
        else
        {
            // Covers both the ordinary first press and the debris case: an
            // orphaned mark is simply overwritten with a live stamp, so the
            // press behaves exactly like a first press instead of skipping the
            // auto-unshift grace.
            s_umbralPendingCancel[player->GetGUID()] = stamp;

            // This is the cancel that MAY be the client unshifting to cast a
            // heal - the ambiguity this whole hook exists for. Record when it
            // happened so a heal that lands with the form already gone is still
            // priced; CountsAsHealingInShadowform reads it.
            s_umbralFormLeftAt[player->GetGUID()] = stamp;
        }
        UmbralPublishPendingCount();
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
    // with this cancel. Nothing is scheduled here: the mark IS the schedule, and
    // ResolvePendingShadowformCancel drains it at the end of this player's very
    // next Player::Update - a tick that cannot be cancelled or dropped.
    return true;
}

void T2SpellHooks::ResolvePendingShadowformCancel(Player* player)
{
    // Nobody anywhere has a swallowed cancel outstanding: this is what the hook
    // costs the other 99.99 % of player ticks.
    if (!player || s_umbralPendingCount.load(std::memory_order_acquire) == 0)
        return;

    bool honour = false;
    {
        std::lock_guard<std::mutex> guard(s_umbralMutex);
        auto itr = s_umbralPendingCancel.find(player->GetGUID());
        if (itr == s_umbralPendingCancel.end())
            return;

        // A mark this old belongs to a cancel whose owner never got a tick -
        // a load screen, a map change. Dropping the form now, seconds later and
        // somewhere else, is not what the player asked for; forget it instead.
        honour = getMSTimeDiff(itr->second, GameTime::GetGameTimeMS()) <= UMBRAL_CANCEL_STALE_MS;
        s_umbralPendingCancel.erase(itr);
        UmbralPublishPendingCount();
    }

    if (!honour)
        return;

    // Nobody consumed it: it was a real cancel, not an auto-unshift. Same call
    // the stock handler would have made one tick ago. Safe here - this runs from
    // t2_priest_mage_update::OnUpdate, after Unit::Update has returned, so no
    // aura container is being walked.
    player->RemoveOwnedAura(SPELL_SHADOWFORM, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
    SendCustomAuraDiag(Trinity::StringFormat(
        "[CustomAuras] {}: Umbral Mercy - Shadowform cancel was not followed by a heal, form dropped",
        player->GetName()));
}

bool T2SpellHooks::CountsAsHealingInShadowform(Player const* player)
{
    if (!player)
        return false;

    // The ordinary case, and the only one the old code tested.
    if (player->GetShapeshiftForm() == FORM_SHADOW || player->HasAura(SPELL_SHADOWFORM))
        return true;

    // The form byte alone is NOT a safe test for this bonus, because the whole
    // feature runs while the client is actively trying to take the form off. The
    // client auto-unshifts from its OWN Spell.dbc, which knows nothing about
    // 90340's MOD_IGNORE_SHAPESHIFT, so every Holy heal pressed in Shadowform is
    // preceded by a CMSG_CANCEL_AURA(15473). If that cancel is honoured for any
    // reason - it arrived without a cast behind it, a second press raced it, the
    // form was dropped by something else in the same frame - the priest gets the
    // heal for free and the price silently does nothing, which is the reported
    // "shadowform heals are not dealing damage to me". Anchor on the cancel
    // instead: it is the same key press as the heal.
    std::lock_guard<std::mutex> guard(s_umbralMutex);
    auto itr = s_umbralFormLeftAt.find(player->GetGUID());
    if (itr == s_umbralFormLeftAt.end())
        return false;

    if (getMSTimeDiff(itr->second, GameTime::GetGameTimeMS()) > UMBRAL_FORM_GRACE_MS)
    {
        s_umbralFormLeftAt.erase(itr);
        return false;
    }

    return true;
}

void T2SpellHooks::ForgetPlayer(Player const* player)
{
    if (!player)
        return;

    std::lock_guard<std::mutex> guard(s_umbralMutex);
    if (s_umbralPendingCancel.erase(player->GetGUID()))
        UmbralPublishPendingCount();
    s_umbralFormLeftAt.erase(player->GetGUID());
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

        // A mark older than the window belongs to a cancel whose resolve never
        // ran (the player left the map before his next tick); it must neither
        // keep nor drop the form now.
        if (getMSTimeDiff(itr->second, GameTime::GetGameTimeMS()) > UMBRAL_PENDING_MAX_AGE_MS)
        {
            s_umbralPendingCancel.erase(itr);
            UmbralPublishPendingCount();
            return;
        }

        if (IsUmbralHeal(spellInfo))
        {
            // The whole point: consumed even if the cast goes on to fail
            // (range, mana, already casting) - the form stays either way.
            s_umbralPendingCancel.erase(itr);
            UmbralPublishPendingCount();
            keptForHeal = true;
        }

        // Not a heal, but a spell Shadowform forbids (Smite, Holy Fire, Holy
        // Nova, Prayer of Mending...): the client auto-unshifted for THIS
        // spell, so the cancel is honoured right now and the cast then sees
        // exactly what it sees without the set - no form, no refusal.
        // Anything else (Mind Blast after a manual cancel) leaves the mark to
        // the resolve.
        else if (player->GetShapeshiftForm() == FORM_SHADOW && spellInfo->CheckShapeshift(FORM_SHADOW) != SPELL_CAST_OK)
        {
            s_umbralPendingCancel.erase(itr);
            UmbralPublishPendingCount();
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

// ---------------------------------------------------------------------------
// PENGUINSTALKER - what school is breaking the aura being removed right now
// ---------------------------------------------------------------------------
namespace
{
    // thread_local, not a locked map: the Unit::DealDamage that records this and
    // the aura-removal hook that reads it are the same statement sequence on one
    // thread, always. Maps update on parallel threads, so a shared global would
    // be a race bought for nothing.
    struct PendingDamageSchool
    {
        ObjectGuid Victim;
        uint32     SchoolMask = 0;
    };

    thread_local PendingDamageSchool s_pendingDamageSchool;
}

void T2SpellHooks::NoteDamageSchool(Unit const* victim, uint32 schoolMask)
{
    s_pendingDamageSchool.Victim     = victim ? victim->GetGUID() : ObjectGuid::Empty;
    s_pendingDamageSchool.SchoolMask = victim ? schoolMask : 0u;
}

uint32 T2SpellHooks::DamageSchoolThatBroke(Unit const* victim)
{
    // Matching the victim as well as being inside the bracketed window: an aura
    // removed for some other reason on some other unit must never read a school
    // left behind by a different unit's hit.
    if (!victim || s_pendingDamageSchool.Victim != victim->GetGUID())
        return 0;

    return s_pendingDamageSchool.SchoolMask;
}

// ---------------------------------------------------------------------------
// MOONKITTY - Starfire spends combo points for cast speed
// ---------------------------------------------------------------------------
int32 T2SpellHooks::MoonkittyStarfireCastTimeCutMs(Unit const* caster, SpellInfo const* spellInfo)
{
    if (!caster || !spellInfo)
        return 0;

    // Starfire by family, not by id: SpellClassSet 7 (druid) with word 0 bit 2,
    // which every rank from 2912 to 48465 carries. Matching ids would silently
    // miss a rank, and the several other "Starfire" rows in Spell.dbc are NPC
    // copies with SpellClassSet 0 that must NOT be caught.
    if (spellInfo->SpellFamilyName != SPELLFAMILY_DRUID || !(spellInfo->SpellFamilyFlags[0] & 0x4))
        return 0;

    if (!caster->HasAura(T2SpellHooks::SPELL_MOONKITTY_LUNAR_MOMENTUM))
        return 0;

    return int32(caster->GetComboPoints()) * 250;
}
