/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITYCORE_PAIR_STANCE_H
#define TRINITYCORE_PAIR_STANCE_H

#include "Define.h"
#include "ObjectGuid.h"

class WorldObject;

// One bot and one person, set apart from the realm's ordinary rules.
//
// Whether a world bot and a person can fight is otherwise a property of the
// person alone: War Mode on, the whole fleet is red; off, all of it is green
// and neither side can land a blow. A bounty-board notice needs exceptions for
// exactly one pair:
//
//   TRUCE  A Mok'gora challenger is walking up to the person who took its
//          notice. It is not red to them, it cannot be hit and it hits nobody
//          on that walk - War Mode or not. A duel between the two outranks it,
//          so the Mok'gora itself is fought normally.
//
//   FEUD   The two are enemies whatever either one's War Mode says: a refused
//          Mok'gora, or somebody going after the bot that walked off with
//          their gear. Sanctuaries still hold.
//
// A stance can also be SHARED: it then holds between the bot and everybody in
// the person's party or raid as well, following the group as people join and
// leave. Friends can help with a feud, and a truce covers the whole party, so
// nobody in it lands the first blow on a challenger walking up. A pair's own
// stance outranks the one it gets through its group, and where two shared
// stances meet on one group a feud outranks a truce.
//
// The scripts library decides who gets a stance and when it ends; this library
// owns the rule because IsValidAttackTarget, GetReactionTo and the per-viewer
// faction render all live here and cannot see scripts/.
//
// Thread-safe. Read on every map thread (attack legality, reaction, the values
// update), written from wherever a notice changes state.
namespace PairStance
{
    enum class Stance : uint8
    {
        None  = 0,
        Truce = 1,
        Feud  = 2,
    };

    // Order of the two does not matter. Setting None clears the pair.
    TC_GAME_API void Set(ObjectGuid a, ObjectGuid b, Stance stance);
    // Set, shared with everybody in `person`'s party or raid (see above).
    // Setting a pair again with Set takes the sharing back off.
    TC_GAME_API void SetShared(ObjectGuid bot, ObjectGuid person, Stance stance);
    TC_GAME_API void Clear(ObjectGuid a, ObjectGuid b);

    // Every stance this character is part of.
    TC_GAME_API void ClearAllFor(ObjectGuid who);

    // The pair's own stance only - nothing it gets through a group.
    TC_GAME_API Stance Get(ObjectGuid a, ObjectGuid b);

    // Between the players behind two world objects - a pet, a totem or a trap
    // answers for its owner - their own stance first, then one shared with the
    // group either of them is in. Always None inside a battleground or an
    // arena, where the match decides who is on whose side.
    TC_GAME_API Stance Between(WorldObject const* a, WorldObject const* b);

    // One relaxed atomic load. Every caller on a hot path asks this first, so a
    // realm with no notice out pays nothing else. False while switched off.
    TC_GAME_API bool Any();

    // The kill switch (Centurion.BountyBoard.Notices.Stances.Enable, applied by
    // the notices on every config load). Off, every pair reads None everywhere -
    // the core behaves as if this file did not exist - but the pairs are kept,
    // so switching it back on restores them. Both ways re-colour every pair.
    TC_GAME_API void SetEnabled(bool enabled);

    // World thread, maps idle - call it from a world tick; any number of
    // callers is fine. Follows every shared stance onto its group as the group
    // stands now (re-read once a second, since nobody tells this file when a
    // party changes), then re-sends the faction view of everybody whose colour
    // changed since the last call - Unit::BuildValuesUpdate computes it per
    // viewer, and nothing in the core dirties that field on its own - and drops
    // any fight the change made illegal. Cheap when nothing changed.
    TC_GAME_API void Update();
}

#endif // TRINITYCORE_PAIR_STANCE_H
