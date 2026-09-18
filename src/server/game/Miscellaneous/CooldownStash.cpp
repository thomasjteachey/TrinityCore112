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

// The world's cooldowns, kept aside for the length of a battleground or an arena
// and given back on the way out. Miscellaneous/CooldownStash.h holds the rules;
// this is how they are kept.
//
// The order of events on the way in (Player::TeleportTo):
//
//   1. everything on cooldown is written to `character_battleground_cooldown`,
//   2. Player::RemoveArenaSpellCooldowns clears the lot,
//   3. the match is fought with a clean slate.
//
// And on the way out (Battleground::RemovePlayerAtLeave, or the next login when
// the character was not there to be handed anything):
//
//   4. each row still in the future is put back, unless the match itself left
//      that spell with longer to run,
//   5. the rows are dropped and the client is told what it is holding.
//
// The rows are the whole record. Nothing here depends on the character being
// online at the end, on the battleground ending tidily, or on the realm being
// shut down rather than killed: the way back is a `SELECT` at the next login.
//
// Every query is written out rather than prepared. A prepared statement for a
// table that does not exist is fatal at startup, and this branch is shared with
// realms that have never heard of this table - hence the presence probe and the
// dynamic SQL (the same reasoning as Miscellaneous/TournamentLoadout.cpp).

#include "Miscellaneous/CooldownStash.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Miscellaneous/TournamentMode.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CooldownStash
{
namespace
{
    bool Enabled = true;

    // Whether this realm has the table at all. Probed once at startup: a realm
    // without it never queries it, and every path here does nothing.
    bool StoragePresent = false;

    // Characters whose world cooldowns are being kept aside right now. The rows
    // are the record that survives a restart; this is what the hot paths ask, so
    // a battleground emptying out does not put a query behind every leaver.
    std::mutex StashMutex;
    std::unordered_set<ObjectGuid> Stashed;

    // Characters whose cooldowns were handed back while they were on their way
    // out of the battleground map, with the spells to say again once they have
    // arrived. The count is read first so the arrival path costs one atomic read
    // when nobody is waiting.
    std::mutex ResendMutex;
    std::unordered_map<ObjectGuid, std::vector<uint32>> PendingResend;
    std::atomic<uint32> PendingResendCount{ 0 };

    void MarkStashed(ObjectGuid guid, bool stashed)
    {
        std::lock_guard<std::mutex> lock(StashMutex);
        if (stashed)
            Stashed.insert(guid);
        else
            Stashed.erase(guid);
    }

    void MarkResendPending(ObjectGuid guid, std::vector<uint32> spells)
    {
        std::lock_guard<std::mutex> lock(ResendMutex);
        PendingResend[guid] = std::move(spells);
        PendingResendCount.store(uint32(PendingResend.size()), std::memory_order_relaxed);
    }

    void DropResendPending(ObjectGuid guid)
    {
        if (PendingResendCount.load(std::memory_order_relaxed) == 0)
            return;

        std::lock_guard<std::mutex> lock(ResendMutex);
        if (PendingResend.erase(guid))
            PendingResendCount.store(uint32(PendingResend.size()), std::memory_order_relaxed);
    }

    // Somebody whose cooldowns are worth writing down at all: this realm keeps
    // them, and the character is not a copy. A battleground fill clone and a
    // colosseum mirror own nothing that outlives the match they were minted for,
    // and they must not touch the table - they wear a real character's name but
    // not its guid.
    bool IsCandidate(Player const* player)
    {
        if (!Enabled || !StoragePresent || !player)
            return false;

        WorldSession const* session = player->GetSession();
        return session && !session->IsVirtualSession() && !session->IsTransientPlayerSession();
    }

    // ... and whose cooldowns this rule is about. A tournament character fights
    // with a clean slate and goes back to one; only world characters carry the
    // world's cooldowns in and out. (A Game Master never reaches this: the wipe
    // itself passes them by.)
    bool IsKept(Player const* player)
    {
        return IsCandidate(player) && !Tournament::IsTournamentCharacter(player);
    }

    // Puts back what the rows say, drops them, and answers how many cooldowns
    // the character got back. `sendNow` is false on the login path, where the
    // restored cooldowns ride out with the initial spells.
    uint32 ApplyStash(Player* player, bool sendNow)
    {
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

        QueryResult rows = CharacterDatabase.PQuery("SELECT spell, item, time, categoryId, categoryEnd FROM character_battleground_cooldown WHERE guid = {}", guid);

        MarkStashed(player->GetGUID(), false);

        if (!rows)
            return 0;

        CharacterDatabase.Execute("DELETE FROM character_battleground_cooldown WHERE guid = {}", guid);

        SpellHistory* history = player->GetSpellHistory();
        SpellHistory::Clock::time_point const now = GameTime::GetSystemTime();

        std::vector<uint32> restored;
        do
        {
            Field* fields = rows->Fetch();

            uint32 const spellId = fields[0].GetUInt32();
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            SpellHistory::Clock::time_point const cooldownEnd = SpellHistory::Clock::from_time_t(time_t(fields[2].GetUInt32()));
            if (cooldownEnd <= now)
                continue;                   // it ran out while the match was being fought: the clock kept running

            uint32 const remaining = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(cooldownEnd - now).count());
            if (history->GetRemainingCooldown(spellInfo) >= remaining)
                continue;                   // the match left this spell with longer to run; that is what it keeps

            // A spell that holds a category holds it for everything else in that
            // category. If the category half of this row has run out, the spell
            // comes back on its own: handing it the category anyway would put an
            // expired holder in front of one that is still running - the classic
            // way to get a free potion out of a shared cooldown.
            uint32 categoryId = fields[3].GetUInt32();
            SpellHistory::Clock::time_point categoryEnd = SpellHistory::Clock::from_time_t(time_t(fields[4].GetUInt32()));
            if (categoryEnd <= now)
            {
                categoryId = 0;
                categoryEnd = SpellHistory::Clock::time_point();
            }

            history->AddCooldown(spellId, fields[1].GetUInt32(), cooldownEnd, categoryId, categoryEnd);
            restored.push_back(spellId);
        }
        while (rows->NextRow());

        if (!restored.empty() && sendNow)
        {
            history->SendCooldowns(restored);

            // The character is usually on its way to its entry point as this
            // runs. Say it again once it has arrived, in case the client was too
            // busy porting to listen.
            MarkResendPending(player->GetGUID(), restored);
        }

        return uint32(restored.size());
    }
}

void LoadConfig()
{
    Enabled = sConfigMgr->GetBoolDefault("Centurion.Battleground.KeepWorldCooldowns", true);
}

void ProbeStorage()
{
    StoragePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_battleground_cooldown'") != nullptr;

    if (!StoragePresent)
        TC_LOG_INFO("server.loading", "Cooldown stash: `character_battleground_cooldown` is not on this realm; battleground entry clears cooldowns for good, as it did before.");
}

bool IsEnabled()
{
    return Enabled && StoragePresent;
}

bool HasStash(Player const* player)
{
    if (!player)
        return false;

    std::lock_guard<std::mutex> lock(StashMutex);
    return Stashed.count(player->GetGUID()) != 0;
}

void StashBeforeMatch(Player* player)
{
    if (!IsKept(player) || HasStash(player))
        return;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    SpellHistory::Clock::time_point const now = GameTime::GetSystemTime();

    std::string values;
    for (SpellHistory::CooldownEntry const& cooldown : player->GetSpellHistory()->GetCooldownSnapshot())
    {
        if (cooldown.CooldownEnd <= now)
            continue;                       // about to go anyway; it would come back as nothing

        values += Trinity::StringFormat("{}({},{},{},{},{},{})", values.empty() ? "" : ",",
            guid, cooldown.SpellId, cooldown.ItemId, uint32(SpellHistory::Clock::to_time_t(cooldown.CooldownEnd)),
            cooldown.CategoryId, uint32(std::max<time_t>(0, SpellHistory::Clock::to_time_t(cooldown.CategoryEnd))));
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append(Trinity::StringFormat("DELETE FROM character_battleground_cooldown WHERE guid = {}", guid).c_str());
    if (!values.empty())
        trans->Append(Trinity::StringFormat("INSERT INTO character_battleground_cooldown (guid, spell, item, time, categoryId, categoryEnd) VALUES {}", values).c_str());
    CharacterDatabase.CommitTransaction(trans);

    // Marked even when there was nothing to write: the mark is what stops a
    // second teleport inside the match from stashing the clean slate over the
    // real thing, and a character that walked in with everything ready has
    // nothing to be given back anyway.
    MarkStashed(player->GetGUID(), true);
}

void RestoreAfterMatch(Player* player)
{
    if (!StoragePresent || !player || !HasStash(player))
        return;

    // Not in the world yet: this is Player::LoadFromDB taking a character out of
    // a battleground that ended without it. Its spell history has not even been
    // read, so the rows are left exactly where they are - RestoreAtLogin, a few
    // lines further into the same load, is what gives them back.
    if (!player->IsInWorld())
        return;

    uint32 const restored = ApplyStash(player, true);

    TC_LOG_DEBUG("bg.battleground", "Cooldown stash: gave {} back {} cooldown(s) on the way out.", player->GetName(), restored);
}

void RestoreAtLogin(Player* player)
{
    if (!IsCandidate(player))
        return;

    DropResendPending(player->GetGUID());

    // Logged back into the match it left: it is meant to be fighting with a
    // clean slate, and leaving will still hand the world's cooldowns back.
    if (player->GetBattlegroundId())
    {
        if (CharacterDatabase.PQuery("SELECT 1 FROM character_battleground_cooldown WHERE guid = {} LIMIT 1", player->GetGUID().GetCounter()))
            MarkStashed(player->GetGUID(), true);
        return;
    }

    // The mode is deliberately not asked here. A character whose rows were
    // written as a world character and whose mode was changed while they sat
    // there is still owed them.
    if (uint32 const restored = ApplyStash(player, false))
        TC_LOG_DEBUG("bg.battleground", "Cooldown stash: gave {} back {} cooldown(s) left over from a match it did not walk out of.",
            player->GetName(), restored);
}

void ResendIfPending(Player* player)
{
    if (!player || PendingResendCount.load(std::memory_order_relaxed) == 0)
        return;

    std::vector<uint32> spells;
    {
        std::lock_guard<std::mutex> lock(ResendMutex);
        auto itr = PendingResend.find(player->GetGUID());
        if (itr == PendingResend.end())
            return;

        spells = std::move(itr->second);
        PendingResend.erase(itr);
        PendingResendCount.store(uint32(PendingResend.size()), std::memory_order_relaxed);
    }

    player->GetSpellHistory()->SendCooldowns(spells);
}
}
