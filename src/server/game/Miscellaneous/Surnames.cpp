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

#include "Miscellaneous/Surnames.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Timer.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <shared_mutex>
#include <unordered_map>

namespace Surnames
{
namespace
{
struct PendingSurname
{
    std::string Name;
    std::string Surname;
    time_t Expires = 0;
};

bool Configured = false;
// Set once at startup by the information_schema probe. A realm whose
// `characters` table was never given the column runs with plain names instead
// of failing every read of it.
bool ColumnExists = false;

// Read from the network threads (CMSG_NAME_QUERY is PROCESS_INPLACE) and
// written from the world thread, including a whole-table refill by
// `.reload character_surname`, so this one does need a lock. It is taken for
// reading a handful of times a second at most.
std::shared_mutex StoreLock;
std::unordered_map<ObjectGuid, std::string> Store;

// Create screen choices, world thread only (glue requests and the creation
// callback both run in WorldSession::Update).
std::unordered_map<uint32, PendingSurname> PendingByAccount;

// Long enough to finish customizing after typing the name, short enough that a
// forgotten choice does not land on a character made much later. Same window as
// the challenge modes next to it on the screen.
constexpr time_t PendingLifetime = 30 * MINUTE;

std::string const Empty;
}

void LoadConfig()
{
    Configured = sConfigMgr->GetBoolDefault("Centurion.Surnames.Enable", false);
}

void Load()
{
    uint32 const oldMSTime = getMSTime();

    // Probe first: selecting a column that is not there aborts the query, and
    // only the Centurion realms have been given this one.
    ColumnExists = bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'surname'"));
    if (!ColumnExists)
    {
        std::unique_lock lock(StoreLock);
        Store.clear();
        if (Configured)
            TC_LOG_ERROR("server.loading", "Centurion.Surnames.Enable is on, but the characters database has no `characters`.`surname` column: characters keep plain names.");
        return;
    }

    std::unordered_map<ObjectGuid, std::string> loaded;
    if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `surname` FROM `characters` WHERE `surname` <> ''"))
    {
        do
        {
            Field* fields = result->Fetch();
            loaded[ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt32())] = fields[1].GetString();
        }
        while (result->NextRow());
    }

    // The character cache indexes characters under their full name, so it has
    // to hear about every surname that appeared OR disappeared. This runs on
    // the world thread, which is the only thread that touches those indexes.
    for (auto const& [guid, surname] : Store)
        if (!loaded.count(guid))
            sCharacterCache->UpdateCharacterSurname(guid, "");
    for (auto const& [guid, surname] : loaded)
        sCharacterCache->UpdateCharacterSurname(guid, surname);

    {
        std::unique_lock lock(StoreLock);
        Store = std::move(loaded);
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} surname(s) in {} ms", Store.size(), GetMSTimeDiffToNow(oldMSTime));
}

bool Enabled()
{
    return Configured && ColumnExists;
}

ResponseCodes Check(std::string& surname, LocaleConstant locale)
{
    // The client's edit box cannot produce leading or trailing spaces, but a
    // hand-typed GM command can.
    std::size_t const first = surname.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        surname.clear();
        return CHAR_NAME_NO_NAME;
    }
    surname = surname.substr(first, surname.find_last_not_of(' ') - first + 1);

    // One word: everything past a space is dropped on the way back in, so a
    // two-part surname would answer to only half of itself.
    if (surname.find(' ') != std::string::npos)
        return CHAR_NAME_INVALID_SPACE;

    ResponseCodes const res = sObjectMgr->CheckPlayerName(surname, locale, true);
    if (res != CHAR_NAME_SUCCESS)
        return res;

    normalizePlayerName(surname);
    return CHAR_NAME_SUCCESS;
}

std::string const& Get(ObjectGuid guid)
{
    if (!Enabled())
        return Empty;

    std::shared_lock lock(StoreLock);
    auto itr = Store.find(guid);
    return itr != Store.end() ? itr->second : Empty;
}

void Decorate(ObjectGuid guid, std::string& name)
{
    if (!Enabled() || name.empty())
        return;

    std::shared_lock lock(StoreLock);
    auto itr = Store.find(guid);
    if (itr == Store.end() || itr->second.empty())
        return;

    name.reserve(name.size() + itr->second.size() + 1);
    name += ' ';
    name += itr->second;
}

std::string Decorated(ObjectGuid guid, std::string_view name)
{
    std::string decorated(name);
    Decorate(guid, decorated);
    return decorated;
}

bool JoinWhisperTarget(std::string& to, std::string& msg)
{
    if (!Enabled() || to.empty())
        return false;

    // The client cut "/w Elgrom Fernbloom hi" at the first space, so the
    // surname is sitting at the front of the message. Take the word back only
    // if the two of them together name somebody.
    std::size_t const space = msg.find(' ');
    if (space == std::string::npos)
        return false;

    // Whatever follows has to still be a message.
    std::size_t const rest = msg.find_first_not_of(' ', space);
    if (rest == std::string::npos)
        return false;

    std::string candidate = to + ' ' + msg.substr(0, space);
    if (!normalizePlayerName(candidate) || !sCharacterCache->GetCharacterCacheByFullName(candidate))
        return false;

    to = std::move(candidate);
    msg.erase(0, rest);
    return true;
}

bool Set(ObjectGuid guid, std::string surname)
{
    if (!Enabled())
        return false;

    CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!cached)
        return false;

    CharacterDatabase.EscapeString(surname);
    CharacterDatabase.PExecute("UPDATE `characters` SET `surname` = '{}' WHERE `guid` = {}", surname, guid.GetCounter());

    // Re-index under the new full name first: that is the name everything
    // resolves by.
    sCharacterCache->UpdateCharacterSurname(guid, surname);

    {
        std::unique_lock lock(StoreLock);
        if (surname.empty())
            Store.erase(guid);
        else
            Store[guid] = surname;
    }

    // Every client that has already asked for this name is holding the old one.
    WorldPacket data(SMSG_INVALIDATE_PLAYER, 8);
    data << guid;
    sWorld->SendGlobalMessage(&data);
    return true;
}

void HandleCreateRequest(WorldSession* session, std::string_view name, std::string_view surname)
{
    if (!Enabled())
        return;

    uint32 const accountId = session->GetAccountId();
    std::string chosen(surname);
    std::string owner(name);
    if (chosen.empty() || !normalizePlayerName(owner) || Check(chosen, session->GetSessionDbcLocale()) != CHAR_NAME_SUCCESS)
    {
        // Nothing (valid) chosen this time: forget an earlier attempt's choice.
        PendingByAccount.erase(accountId);
        return;
    }

    PendingSurname& pending = PendingByAccount[accountId];
    pending.Name = std::move(owner);
    pending.Surname = std::move(chosen);
    pending.Expires = GameTime::GetGameTime() + PendingLifetime;
}

std::string PeekPending(uint32 accountId, std::string const& name)
{
    auto itr = PendingByAccount.find(accountId);
    if (itr == PendingByAccount.end() || !Enabled())
        return "";

    PendingSurname const& pending = itr->second;
    if (pending.Expires < GameTime::GetGameTime() || !StringEqualI(pending.Name, name))
        return "";

    return pending.Surname;
}

void ApplyOnCreate(uint32 accountId, ObjectGuid guid, std::string const& name)
{
    auto itr = PendingByAccount.find(accountId);
    if (itr == PendingByAccount.end())
        return;

    PendingSurname const pending = std::move(itr->second);
    PendingByAccount.erase(itr);

    if (!Enabled() || pending.Expires < GameTime::GetGameTime() || !StringEqualI(pending.Name, name))
        return;

    if (Set(guid, pending.Surname))
        TC_LOG_INFO("entities.player.character", "Account: {} Created character {} {} with the surname {}.", accountId, name, guid.ToString(), pending.Surname);
}
}
