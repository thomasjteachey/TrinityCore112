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

    // A whisper is the one line the client splits for us: "/w Elgrom Fernbloom
    // hi" arrives as the target "Elgrom" and the message "Fernbloom hi". Moves
    // that first word back onto the target when the two together name a real
    // character, and says whether it did.
    bool JoinWhisperTarget(std::string& to, std::string& msg);

    // The reverse. The Centurion whisper box keeps TWO words together as the
    // target, since a name is first and last now, so "/w Bob hello there" to
    // a character without a family name arrives as "Bob Hello" and "there".
    // Hands the second word back to the message when the two words name
    // nobody and the first alone does.
    bool SplitWhisperTarget(std::string& to, std::string& msg);

    // The surname waiting for the character about to be created on this
    // account, without consuming it: the name it will be created under has to
    // be checked as a pair.
    std::string PeekPending(uint32 accountId, std::string const& name);

    // Writes `characters`.`surname`, updates the cache and tells every client
    // holding the old name to ask again. An empty surname clears it. False if
    // surnames are off or the character is unknown.
    bool Set(ObjectGuid guid, std::string surname);

    // "SURNAME\t<name>\t<surname>" from the create screen or the rename prompt,
    // kept until that character is created or renamed
    // (Miscellaneous/CharacterScreen.h).
    void HandleCreateRequest(WorldSession* session, std::string_view name, std::string_view surname);
    // Consumes the pending choice and writes it to the character that has just
    // been created or renamed to `name`. The log line says which, from `what`.
    void ApplyPending(uint32 accountId, ObjectGuid guid, std::string const& name, char const* what);
}

#endif // TRINITYCORE_SURNAMES_H
