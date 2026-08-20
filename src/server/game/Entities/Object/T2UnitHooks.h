/*
 * T2 set-bonus hooks that live in the UNIT / PLAYER / MOVEMENT pipeline.
 * Declared here once so the core call sites (Unit, Player, MovementHandler) and
 * the script files agree on one interface. Bodies are in T2UnitHooks.cpp.
 *
 * Everything here is gated on a player carrying a specific set-bonus aura and is
 * a cheap early-out for everyone else: no aura, no work.
 */
#ifndef TRINITY_T2_UNIT_HOOKS_H
#define TRINITY_T2_UNIT_HOOKS_H

#include "Define.h"
#include "ObjectGuid.h"

class Item;
class Player;
class SpellInfo;
class Unit;
struct MovementInfo;

namespace T2UnitHooks
{
    // Set-bonus auras this module keys on.
    constexpr uint32 SPELL_BLOOD_FOR_POWER = 90322;   // no mana gain except Life Tap
    constexpr uint32 SPELL_MOMENTUM        = 90358;   // straight-line running stacks speed
    constexpr uint32 SPELL_INERTIAL_GUARD  = 90359;   // damage reduction per Momentum stack
    constexpr uint32 SPELL_MOMENTUM_BUFF   = 90392;   // the stacking buff itself (CumulativeAura 4)

    // Life Tap's energize, the one mana source Blood for Power allows.
    constexpr uint32 SPELL_LIFE_TAP_ENERGIZE = 31818;

    // BLOOD FOR POWER. True when `target` must not gain MANA from `source`
    // (nullptr source = passive regeneration). Consulted by Unit::EnergizeBySpell
    // and Player::Regenerate.
    bool BlocksManaGain(Unit const* target, SpellInfo const* source);

    // MOMENTUM. Called from WorldSession::HandleMovementOpcodes for the player's
    // own mover, after the packet has been validated, with the NEW movement info
    // (its `time` already converted to the server clock by the handler - the
    // body times everything off it, not off when the packet happened to be
    // processed). Keyboard turning sets MOVEMENTFLAG_LEFT/RIGHT; mouse turning
    // arrives as MSG_MOVE_SET_FACING with a new orientation and no turn flag,
    // so the body tracks orientation drift from the moment momentum started.
    void OnPlayerMovement(Player* player, uint16 opcode, MovementInfo const& newInfo);
    // Housekeeping for the momentum tracker: called from Player::RemoveFromWorld
    // (logout and far teleport both pass through it). Touches no auras.
    void OnPlayerRemovedFromWorld(Player* player);
    // Loss of control breaks a run: called from Unit::SetControlled when a
    // stun / root / confuse / fear is APPLIED to a player (a controlled player
    // sends no movement packets, so the tracker would otherwise never notice).
    // The run is forgotten at once; the buff itself is dropped from the
    // player's event queue on the next Unit::Update, because SetControlled runs
    // from inside the CC aura's apply handler.
    void OnPlayerControlLost(Player* player);

    // LEGION OF ONE (warlock imp 8pc). Called from Unit::DealDamage at the
    // instant damage is found to be lethal, BEFORE Unit::Kill. If the victim is
    // a warlock wearing 90320 with a living imp out, the imp is spent instead:
    // the warlock survives on the imp's remaining health and is moved to where
    // it stood. Returns true when it saved him, in which case the caller must
    // not kill him.
    //
    // Nothing is torn down synchronously - the pet unsummon, the teleport and
    // the visual are all queued onto the warlock's own event processor, because
    // this runs deep inside the attacker's damage frame.
    bool OnWouldBeLethalDamage(Unit* victim);

    // ASHEN CONFISCATION. Temporary weapons a mage "confiscates" from a disarmed
    // victim are registered here so Player::CanUseItem / weapon-skill lookups can
    // waive requirements for exactly these items and nothing else.
    void RegisterTemporaryWeapon(ObjectGuid itemGuid);
    void UnregisterTemporaryWeapon(ObjectGuid itemGuid);
    bool IsTemporaryWeapon(Item const* item);
    // Skill value to report for a temporary weapon ("act as if 300 skill").
    constexpr uint16 TEMPORARY_WEAPON_SKILL = 300;
}

// Equipment-change fan-out, following the fork's existing HiddenSets /
// PolearmStaffInnerAuras convention: a namespace function declared in Player.cpp
// and called at every equipment-change site. The body lives in the warrior T2
// script file (custom_t2_warrior_paladin.cpp).
namespace T2Unarmed
{
    void OnEquipmentChanged(Player* player);
}

#endif // TRINITY_T2_UNIT_HOOKS_H
