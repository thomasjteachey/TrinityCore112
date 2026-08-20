/*
 * T2 set-bonus hooks that live in the SPELL pipeline. Declared here once so the
 * core call sites (SpellHistory, Spell, SpellEffects, SpellHandler, Unit) and
 * the script files agree on one interface. Bodies are in T2SpellHooks.cpp.
 *
 * Everything here is gated on a player carrying a specific set-bonus aura and is
 * a cheap early-out for everyone else: no aura, no work.
 */
#ifndef TRINITY_T2_SPELL_HOOKS_H
#define TRINITY_T2_SPELL_HOOKS_H

#include "Define.h"

class Player;
class Spell;
class SpellInfo;
class Unit;

namespace T2SpellHooks
{
    // Set-bonus auras this module keys on.
    constexpr uint32 SPELL_UNBOUND_RIME   = 90313;   // Frost Shock off the shared shock category
    constexpr uint32 SPELL_UNBOUND_EMBER  = 90316;   // Flame Shock off the shared shock category
    constexpr uint32 SPELL_FEINT_CADENCE  = 90364;   // cancel a >= 1s cast -> next spell faster
    constexpr uint32 SPELL_DEAD_AIR       = 90365;   // interrupted while not casting -> next spell faster
    constexpr uint32 SPELL_REFLECTIVE_ICE = 90344;   // Ice Block reflects 50% of prevented damage
    constexpr uint32 SPELL_UMBRAL_MERCY   = 90340;   // priest heals keep Shadowform up

    // Helpers cast by these hooks (rows exist in dbc.spell_lplus).
    constexpr uint32 SPELL_FEINT_CADENCE_BUFF = 90393;
    constexpr uint32 SPELL_DEAD_AIR_BUFF      = 90394;
    // THIS SERVER'S Ice Block is 11958, the classic Frost talent (Talent 31,
    // tab 61) - 52 dev / 51 prod characters hold it. The WotLK id 45438 is held
    // by NOBODY, which is why the whole dusty-mage set watched a spell that
    // could never be cast. Both are accepted: the rows are structurally
    // identical and a future id switch must not silently break this again.
    constexpr uint32 SPELL_ICE_BLOCK          = 11958;
    constexpr uint32 SPELL_ICE_BLOCK_WOTLK    = 45438;
    constexpr uint32 SPELL_SHADOWFORM         = 15473;

    // SHOCKS. True when `caster` holds the unbound aura matching this shock, in
    // which case the spell keeps its OWN 6s cooldown but must neither receive the
    // shared category cooldown from the other shocks nor hand it to them. The
    // client computes the category from its own DBC, so the server also has to
    // send explicit SMSG_CLEAR_COOLDOWN for whichever side was wrongly started.
    bool IsShockUnbound(Unit const* caster, SpellInfo const* spellInfo);

    // Called from WorldSession::HandleCancelCastOpcode BEFORE the cast is
    // interrupted, while the Spell object still knows how long it has been casting.
    void OnPlayerCancelCast(Player* player, Spell* cancelled);

    // Called from Spell::EffectInterruptCast when the interrupt landed on a unit
    // that was NOT casting anything interruptible (the "whiffed kick").
    void OnInterruptWhileNotCasting(Unit* interrupter, Unit* target);

    // Ice Block reflect. Spells: called when a hostile spell hit returns
    // SPELL_MISS_IMMUNE against a victim in Ice Block; the body estimates the
    // damage that would have landed. Melee: called from the physical-immune
    // branch of Unit::CalculateMeleeDamage, which still has the would-be damage.
    void OnImmuneSpellHit(Unit* caster, Unit* victim, SpellInfo const* spellInfo);
    void OnImmuneMeleeHit(Unit* attacker, Unit* victim, uint32 wouldBeDamage);

    // UMBRAL MERCY (90340). The 3.3.5 client auto-unshifts: a Holy heal pressed
    // in Shadowform makes it send CMSG_CANCEL_AURA(15473) and then
    // CMSG_CAST_SPELL(heal) in the same frame, and there is nothing a
    // SpellScript can see before the form is already gone. So the cancel itself
    // is intercepted:
    //
    //  OnCancelAuraRequest  - FIRST thing in WorldSession::HandleCancelAuraOpcode.
    //      True = handled: the holder's Shadowform is NOT removed now; the cancel
    //      is remembered and ResolvePendingShadowformCancel removes the form on
    //      this player's very next tick unless a heal request consumed the mark
    //      first. One tick is exact, not optimistic - CMSG_CANCEL_AURA is
    //      PROCESS_INPLACE (World::UpdateSessions), CMSG_CAST_SPELL is
    //      PROCESS_THREADSAFE (the session pass at the top of Map::Update) and
    //      Player::Update runs after both, while LockedQueue::next peeks the
    //      queue HEAD so no packet can overtake one sent before it. A deliberate
    //      press therefore takes the form off on the very tick it was pressed. A
    //      second cancel arriving before the first resolved is honoured on the
    //      spot (the client auto-unshifts once per cast, so two in a row is a
    //      human pressing the button), which is also what stops a mashed button
    //      from starving the removal for ever - the bug the old 300 ms
    //      wall-clock grace had. Only swallowed when the aura AND the shapeshift
    //      byte both say Shadowform, so an inconsistent state can never become a
    //      form the player is unable to take off. False = not ours, stock path.
    //  OnCastSpellRequest   - WorldSession::HandleCastSpellOpcode, before
    //      Spell::prepare. A pending mark plus a priest heal (see the body for
    //      the predicate) consumes the mark - whether or not the cast goes on to
    //      succeed, because the form must survive a heal that fails on range or
    //      mana too. A pending mark plus a non-heal the form forbids (Smite...)
    //      honours the cancel on the spot, so those cast exactly as stock.
    //  WaivesShapeshiftRestriction - Spell::CheckCast, wrapped around the
    //      CheckShapeshift refusal: true when the caster is a player in
    //      Shadowform holding 90340 and the spell is one of the heals.
    //
    // Nothing here runs for a player without 90340.
    bool OnCancelAuraRequest(Player* player, uint32 spellId);
    void OnCastSpellRequest(Player* player, SpellInfo const* spellInfo);
    bool WaivesShapeshiftRestriction(Unit const* caster, SpellInfo const* spellInfo);

    // Drains a swallowed Shadowform cancel that no heal claimed. Called once per
    // player tick from t2_priest_mage_update::OnUpdate (custom_t2_priest_mage.cpp),
    // which runs in Player::Update AFTER Unit::Update has returned - the one place
    // this codebase documents as safe to add or remove auras from.
    //
    // This replaced a zero-offset event on the player's own EventProcessor. Same
    // tick, but the queue is emptied by EventProcessor::KillAllEvents on a far
    // teleport, a map change and logout, and an aborted removal leaves the form
    // stuck up for good with nothing left to retry it - which is exactly the
    // reported "I cannot leave Shadowform". A PlayerScript tick cannot be
    // cancelled. Free for everyone: one relaxed atomic load when nothing is
    // pending anywhere on the server.
    void ResolvePendingShadowformCancel(Player* player);

    // True when a heal landing NOW on this priest is healing that Umbral Mercy
    // made possible: he is in Shadowform, still carries the aura, or his client
    // cancelled the form so recently that the cancel and this heal are the same
    // key press. See the body for why the raw form byte is not a safe test.
    bool CountsAsHealingInShadowform(Player const* player);

    // Drops every per-player mark this module holds. Called from the T2
    // PlayerScript's OnLogout so nothing outlives the session.
    void ForgetPlayer(Player const* player);
}

#endif // TRINITY_T2_SPELL_HOOKS_H
