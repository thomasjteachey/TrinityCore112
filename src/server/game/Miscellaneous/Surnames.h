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

#ifndef TRINITYCORE_SURNAMES_H
#define TRINITYCORE_SURNAMES_H

#include "Common.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <string>
#include <string_view>

class WorldSession;

// Family names, chosen on the character create screen and shown after the
// character's own name everywhere the client asks the server for it.
//
// The 3.3.5a protocol has no field for one, and `characters`.`name` is what
// every other character is checked against for uniqueness, so the surname is a
// column of its own (`characters`.`surname`, added by hand on the Centurion
// realms) and is pasted onto the name on the way OUT:
//
//   SMSG_NAME_QUERY_RESPONSE  the name behind nameplates, chat, tooltips, mail
//   SMSG_CHAR_ENUM            the character select list
//   SMSG_GROUP_LIST           party and raid frames
//   SMSG_GUILD_ROSTER, SMSG_WHO, SMSG_ARENA_TEAM_ROSTER
//
// On the way IN a name may therefore arrive with the surname attached - an
// invite or a mail addressed to "Elgrom Doomhammer" - so normalizePlayerName()
// drops everything from the first space. First names stay unique, so that is
// still an exact lookup. The one place this cannot work is a whisper, where the
// client has already split the line on the same space and handed us "Elgrom"
// with the message "Doomhammer hello"; WorldSession::HandleMessagechatOpcode
// puts that back together.
//
// Everything here is inert unless Centurion.Surnames.Enable is on AND the
// column exists - a realm without the column keeps plain names rather than
// failing every query against `characters`.
namespace Surnames
{
    // World::LoadConfigSettings, so `.reload config` applies it.
    void LoadConfig();
    // Startup, after the character cache. Probes for the column before reading
    // it; also the body of `.reload character_surname`, which is how a bulk
    // UPDATE reaches a running realm.
    void Load();

    bool Enabled();

    // Trimmed and normalized in place, then held to the same rules as a first
    // name (length, alphabet, profanity, reserved words). CHAR_NAME_SUCCESS on
    // a surname that may be worn.
    ResponseCodes Check(std::string& surname, LocaleConstant locale);

    // The surname on file, or "".
    std::string const& Get(ObjectGuid guid);
    // "<name> <surname>", or name untouched. Called on every outgoing name.
    void Decorate(ObjectGuid guid, std::string& name);
    std::string Decorated(ObjectGuid guid, std::string_view name);

    // A whisper is the one line the client splits for us: "/w Elgrom Doomhammer
    // hi" arrives as the target "Elgrom" and the message "Doomhammer hi". Drops
    // that first word when it is exactly the target's family name and something
    // follows it.
    void StripLeadingSurname(std::string const& name, std::string& msg);

    // Writes `characters`.`surname`, updates the cache and tells every client
    // holding the old name to ask again. An empty surname clears it. False if
    // surnames are off or the character is unknown.
    bool Set(ObjectGuid guid, std::string surname);

    // "SURNAME\t<name>\t<surname>" from the create screen, kept until that
    // character is created (Miscellaneous/CharacterScreen.h).
    void HandleCreateRequest(WorldSession* session, std::string_view name, std::string_view surname);
    // Consumes the pending choice and writes it to the new character.
    void ApplyOnCreate(uint32 accountId, ObjectGuid guid, std::string const& name);
}

#endif // TRINITYCORE_SURNAMES_H
