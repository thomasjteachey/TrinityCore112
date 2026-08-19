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

    // Helpers cast by these hooks (rows exist in dbc.spell_lplus).
    constexpr uint32 SPELL_FEINT_CADENCE_BUFF = 90393;
    constexpr uint32 SPELL_DEAD_AIR_BUFF      = 90394;
    constexpr uint32 SPELL_ICE_BLOCK          = 45438;

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
}

#endif // TRINITY_T2_SPELL_HOOKS_H
