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

#ifndef TRINITYCORE_COOLDOWN_STASH_H
#define TRINITYCORE_COOLDOWN_STASH_H

#include "Define.h"

class Player;

// Stepping into a battleground or an arena clears every cooldown a character is
// carrying (Player::RemoveArenaSpellCooldowns): a match is fought with
// everything ready, whatever its owner was doing out in the world a minute
// earlier.
//
// What it was doing out in the world is still its own business afterwards,
// though. The cooldowns a WORLD-MODE character walks in with are kept aside for
// the length of the match and given back on the way out, so the door into a
// battleground is not also a free reset of the hearthstone, the trinket and the
// twenty-minute bubble.
//
// The clock keeps running while the match does: what comes back is what was
// left of each cooldown at the door, minus the time the match took. One that ran
// out meanwhile does not come back at all, and a cooldown the character picked
// up INSIDE the match is never shortened - whichever of the two has longer to
// run is what it keeps.
//
// Leaving by any route ends the same way. The rows in
// `character_battleground_cooldown` are written before the wipe and read back on
// the way out, so a logout, a disconnect, a crash or a restart mid-match is just
// a slower way out: the next login hands the cooldowns back instead. A realm
// without that table never queries it and behaves exactly as it did before.
//
// Not kept: tournament characters (a clean slate is the point of the tournament
// side), Game Masters (whose cooldowns are never wiped in the first place) and
// the transient copies that fill a battleground - a clone owns nothing that
// outlives its match.
namespace CooldownStash
{
    // Centurion.Battleground.KeepWorldCooldowns, read in World::LoadConfigSettings
    // so `.reload config` applies it.
    void LoadConfig();
    // Does this realm have the table? Probed once at startup, because a missing
    // table is fatal to the connection that asks for it and this branch is shared.
    void ProbeStorage();
    bool IsEnabled();

    // Player::TeleportTo, immediately before the wipe: everything on cooldown is
    // written down first.
    void StashBeforeMatch(Player* player);
    // Battleground::RemovePlayerAtLeave: the way out that the character chose,
    // and most of the ones it did not.
    void RestoreAfterMatch(Player* player);
    // Player::LoadFromDB, after the spell history has been read: the crash,
    // logout and shutdown route. What it gives back rides out with the initial
    // spells, so the client needs nothing further.
    void RestoreAtLogin(Player* player);
    // Player::SendInitialPacketsAfterAddToMap: the cooldowns were handed back
    // while the client was still porting out of the battleground map. Say it
    // again now that it has arrived. One relaxed atomic read when nobody is
    // waiting.
    void ResendIfPending(Player* player);

    // True while this character's world cooldowns are being kept aside.
    bool HasStash(Player const* player);
}

#endif // TRINITYCORE_COOLDOWN_STASH_H
