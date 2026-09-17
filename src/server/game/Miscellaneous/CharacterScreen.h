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

#ifndef TRINITYCORE_CHARACTER_SCREEN_H
#define TRINITYCORE_CHARACTER_SCREEN_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>

class WorldSession;

// The character select and create screens on the Centurion realms, past what
// the 3.3.5a glue protocol carries:
//  - the order a player arranges their characters in
//    (Centurion.CharacterSelect.Reorder, table character_select_order),
//  - a World/Tournament badge on every character, carried in the zone field of
//    the character list (Centurion.CharacterSelect.TournamentZoneId): the list
//    has no other field the glue screens can read,
//  - challenge modes picked on the create screen
//    (Centurion.CharacterCreate.Challenges).
//
// The glue screens cannot send anything of their own, so the client-tweaks DLL
// registers CenturionGlueRequest(text), which sends the text on opcode 0x002
// (CMSG_DBLOOKUP, which the stock client never sends):
//   "ORDER\t<name>,<name>,..."  every character on the account, top to bottom
//   "CREATE\t<name>\t<mask>"    challenge modes for the character about to be
//                               created, bit n = ChallengeModeSettings n
// Everything is inert with the keys at their defaults.
namespace CharacterScreen
{
    // World::LoadConfigSettings, so `.reload config` applies it.
    void LoadConfig();
    // Startup. Probes for character_select_order before reading it.
    void LoadOrder();

    bool ReorderEnabled();
    // Sort key within the account's list: the saved position, or past every
    // positioned character (the caller breaks ties by guid, i.e. creation order).
    uint32 GetListPosition(ObjectGuid::LowType guid);

    // The zone id the character list sends for a tournament character; 0 = off.
    uint32 GetTournamentListZone();

    void HandleGlueRequest(WorldSession* session, std::string const& text);

    // The challenge modes chosen on the create screen for this character, or 0.
    // Consumed by the call.
    uint32 TakeCreateChallenges(uint32 accountId, std::string const& name);
}

#endif // TRINITYCORE_CHARACTER_SCREEN_H
