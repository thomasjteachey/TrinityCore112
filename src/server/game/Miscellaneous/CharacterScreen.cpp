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

#include "Miscellaneous/CharacterScreen.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Util.h"
#include "WorldSession.h"
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CharacterScreen
{
namespace
{
struct ScreenConfig
{
    bool Reorder = false;
    uint32 TournamentListZone = 0;
    bool CreateChallenges = false;
};

struct PendingChallenges
{
    std::string Name;
    uint32 Mask = 0;
    time_t Expires = 0;
};

// Everything here is touched from the world thread only: glue requests are
// PROCESS_THREADUNSAFE, and the character list and creation callbacks run in
// WorldSession::Update.
ScreenConfig Config;
bool OrderTableExists = false;
std::unordered_map<ObjectGuid::LowType, uint8> Positions;
std::unordered_map<uint32, std::vector<ObjectGuid::LowType>> OrderByAccount;
std::unordered_map<uint32, PendingChallenges> PendingByAccount;

constexpr std::size_t MaxRequestLength = 1024;
// Well past CharactersPerRealm; a list the client could never show is ignored beyond this.
constexpr std::size_t MaxOrderedCharacters = 50;
constexpr uint32 UnpositionedCharacter = 0xFF;
// ChallengeModeSettings SETTING_HARDCORE..SETTING_IRON_MAN; HARDCORE_DEAD is never chosen.
constexpr uint32 ChallengeMaskBits = 0xFF;
// Long enough to finish customizing after typing the name, short enough that a
// forgotten choice does not land on a character made much later.
constexpr time_t PendingLifetime = 30 * MINUTE;

void HandleOrder(WorldSession* session, std::string_view list)
{
    if (!ReorderEnabled())
        return;

    uint32 const accountId = session->GetAccountId();
    std::vector<ObjectGuid::LowType> ordered;
    std::unordered_set<ObjectGuid::LowType> seen;
    for (std::string_view token : Trinity::Tokenize(list, ',', false))
    {
        if (ordered.size() == MaxOrderedCharacters)
            break;

        std::string name(token);
        if (!normalizePlayerName(name))
            continue;

        // Only this account's own characters: a deleted character's account is
        // cleared, and a name belonging to someone else is simply skipped.
        ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(name);
        if (guid.IsEmpty() || sCharacterCache->GetCharacterAccountIdByGuid(guid) != accountId)
            continue;

        if (seen.insert(guid.GetCounter()).second)
            ordered.push_back(guid.GetCounter());
    }

    if (ordered.empty())
        return;

    std::vector<ObjectGuid::LowType>& accountOrder = OrderByAccount[accountId];
    for (ObjectGuid::LowType guid : accountOrder)
        Positions.erase(guid);

    std::string values;
    for (std::size_t i = 0; i < ordered.size(); ++i)
    {
        Positions[ordered[i]] = uint8(i);
        values += Trinity::StringFormat("{}({}, {}, {})", i ? ", " : "", ordered[i], accountId, i);
    }
    accountOrder = ordered;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->PAppend("DELETE FROM `character_select_order` WHERE `account` = {}", accountId);
    trans->PAppend("INSERT INTO `character_select_order` (`guid`, `account`, `position`) VALUES {}", values);
    CharacterDatabase.CommitTransaction(trans);
}

void HandleCreate(WorldSession* session, std::string_view rawName, std::string_view rawMask)
{
    if (!Config.CreateChallenges)
        return;

    uint32 const accountId = session->GetAccountId();
    Optional<uint32> const mask = Trinity::StringTo<uint32>(rawMask);
    std::string name(rawName);
    if (!mask || !(*mask & ChallengeMaskBits) || !normalizePlayerName(name))
    {
        // Nothing (valid) chosen this time: forget an earlier attempt's choice.
        PendingByAccount.erase(accountId);
        return;
    }

    PendingChallenges& pending = PendingByAccount[accountId];
    pending.Name = std::move(name);
    pending.Mask = *mask & ChallengeMaskBits;
    pending.Expires = GameTime::GetGameTime() + PendingLifetime;
}
}

void LoadConfig()
{
    ScreenConfig loaded;
    loaded.Reorder = sConfigMgr->GetBoolDefault("Centurion.CharacterSelect.Reorder", false);
    loaded.TournamentListZone = uint32(sConfigMgr->GetIntDefault("Centurion.CharacterSelect.TournamentZoneId", 0));
    loaded.CreateChallenges = sConfigMgr->GetBoolDefault("Centurion.CharacterCreate.Challenges", false);
    Config = loaded;
}

void LoadOrder()
{
    uint32 const oldMSTime = getMSTime();
    Positions.clear();
    OrderByAccount.clear();
    OrderTableExists = false;

    // Probe first: a missing table aborts the server (ER_NO_SUCH_TABLE), and
    // only the Centurion realms have this one.
    if (!CharacterDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_select_order'"))
    {
        if (Config.Reorder)
            TC_LOG_ERROR("server.loading", "Centurion.CharacterSelect.Reorder is on, but the characters database has no `character_select_order` table: character lists keep creation order.");
        return;
    }

    OrderTableExists = true;
    if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `account`, `position` FROM `character_select_order` ORDER BY `account`, `position`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType const guid = fields[0].GetUInt32();
            Positions[guid] = fields[2].GetUInt8();
            OrderByAccount[fields[1].GetUInt32()].push_back(guid);
        }
        while (result->NextRow());
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} character list position(s) for {} account(s) in {} ms", Positions.size(), OrderByAccount.size(), GetMSTimeDiffToNow(oldMSTime));
}

bool ReorderEnabled()
{
    return Config.Reorder && OrderTableExists;
}

uint32 GetListPosition(ObjectGuid::LowType guid)
{
    auto itr = Positions.find(guid);
    return itr != Positions.end() ? itr->second : UnpositionedCharacter;
}

uint32 GetTournamentListZone()
{
    return Config.TournamentListZone;
}

void HandleGlueRequest(WorldSession* session, std::string const& text)
{
    if (!session || text.size() > MaxRequestLength)
        return;

    std::vector<std::string_view> const parts = Trinity::Tokenize(text, '\t', true);
    if (parts.size() == 2 && parts[0] == "ORDER")
        HandleOrder(session, parts[1]);
    else if (parts.size() == 3 && parts[0] == "CREATE")
        HandleCreate(session, parts[1], parts[2]);
    else
        TC_LOG_DEBUG("network", "CharacterScreen: account {} sent an unrecognised glue request.", session->GetAccountId());
}

uint32 TakeCreateChallenges(uint32 accountId, std::string const& name)
{
    auto itr = PendingByAccount.find(accountId);
    if (itr == PendingByAccount.end())
        return 0;

    PendingChallenges const pending = std::move(itr->second);
    PendingByAccount.erase(itr);

    if (!Config.CreateChallenges || pending.Expires < GameTime::GetGameTime() || !StringEqualI(pending.Name, name))
        return 0;

    return pending.Mask;
}
}
