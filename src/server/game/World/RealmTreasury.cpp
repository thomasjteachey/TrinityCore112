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

#include "RealmTreasury.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <sstream>

namespace RealmTreasury
{
namespace
{
    constexpr size_t kInflows = size_t(Inflow::Max);
    constexpr size_t kOutflows = size_t(Outflow::Max);

    std::atomic<bool> s_enabled{ false };
    std::atomic<uint32> s_refillPercent{ 50 };
    bool s_loaded = false;

    std::atomic<uint64> s_balance{ 0 };
    std::atomic<bool> s_dirty{ false };

    // Interval counters for the ledger line, reset each time it is written.
    std::array<std::atomic<uint64>, kInflows> s_paidIn{};
    std::array<std::atomic<uint64>, kOutflows> s_paidOut{};
    std::atomic<uint64> s_destroyed{ 0 };
    // What bots asked for and did not get because the pool was dry - the
    // number that says whether the fleet is starving.
    std::atomic<uint64> s_unfunded{ 0 };

    std::string FormatGold(uint64 copper)
    {
        std::ostringstream out;
        out << (copper / 10000) << "g" << ((copper / 100) % 100) << "s" << (copper % 100) << "c";
        return out.str();
    }
}

char const* InflowName(Inflow source)
{
    switch (source)
    {
        case Inflow::Repair:      return "repairs";
        case Inflow::Training:    return "training";
        case Inflow::Vendor:      return "merchants";
        case Inflow::Taxi:        return "flights";
        case Inflow::TalentReset: return "talent resets";
        case Inflow::AuctionCut:  return "auction cut";
        case Inflow::DeathTax:    return "death tax";
        case Inflow::DeathDebt:   return "death debt";
        default:                  return "?";
    }
}

char const* OutflowName(Outflow destination)
{
    switch (destination)
    {
        case Outflow::BotVendorSale: return "bot vendor sales";
        case Outflow::BotLoot:       return "bot loot";
        case Outflow::BotStipend:    return "drifter stipends";
        default:                     return "?";
    }
}

bool IsEnabled()
{
    return s_enabled.load(std::memory_order_relaxed);
}

void Deposit(uint64 copper, Inflow source)
{
    if (!copper || !IsEnabled() || source >= Inflow::Max)
        return;

    uint64 const kept = copper * s_refillPercent.load(std::memory_order_relaxed) / 100;
    s_balance.fetch_add(kept, std::memory_order_relaxed);
    s_paidIn[size_t(source)].fetch_add(kept, std::memory_order_relaxed);
    s_destroyed.fetch_add(copper - kept, std::memory_order_relaxed);
    if (kept)
        s_dirty.store(true, std::memory_order_relaxed);
}

uint64 Withdraw(uint64 copper, Outflow destination)
{
    if (!copper)
        return 0;

    if (!IsEnabled())
        return copper;

    uint64 current = s_balance.load(std::memory_order_relaxed);
    uint64 granted = 0;
    do
    {
        granted = std::min(current, copper);
        if (!granted)
            break;
    } while (!s_balance.compare_exchange_weak(current, current - granted, std::memory_order_relaxed));

    if (destination < Outflow::Max)
        s_paidOut[size_t(destination)].fetch_add(granted, std::memory_order_relaxed);
    s_unfunded.fetch_add(copper - granted, std::memory_order_relaxed);
    if (granted)
        s_dirty.store(true, std::memory_order_relaxed);
    return granted;
}

uint64 GetBalance()
{
    return s_balance.load(std::memory_order_relaxed);
}

void LoadConfig()
{
    s_enabled.store(sConfigMgr->GetBoolDefault("Centurion.Treasury.Enable", false), std::memory_order_relaxed);
    s_refillPercent.store(uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Treasury.RefillPercent", 50), 0, 100)),
        std::memory_order_relaxed);

    // Loaded the first time the treasury is switched on, whether that is at
    // startup or by a later .reload config. Never re-read after that: the
    // in-memory balance is the live one and the row only trails it.
    if (IsEnabled() && !s_loaded)
        LoadFromDB();
}

void LoadFromDB()
{
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS realm_treasury ("
        "id TINYINT UNSIGNED NOT NULL PRIMARY KEY,"
        "copper BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "updatedAt INT UNSIGNED NOT NULL DEFAULT 0"
        ") ENGINE=InnoDB");

    uint64 copper = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT copper FROM realm_treasury WHERE id = 1"))
        copper = result->Fetch()[0].GetUInt64();

    s_balance.store(copper, std::memory_order_relaxed);
    s_dirty.store(false, std::memory_order_relaxed);
    s_loaded = true;

    TC_LOG_INFO("server.loading", "Realm treasury: {} in the pool, {}% of every payment to the world refills it.",
        FormatGold(copper), s_refillPercent.load(std::memory_order_relaxed));
}

bool IsDirty()
{
    return s_dirty.load(std::memory_order_relaxed);
}

void SaveToDB(bool direct)
{
    if (!s_loaded)
        return;

    s_dirty.store(false, std::memory_order_relaxed);
    uint64 const copper = GetBalance();
    uint32 const now = uint32(GameTime::GetGameTime());
    if (direct)
        CharacterDatabase.DirectPExecute(
            "REPLACE INTO realm_treasury (id, copper, updatedAt) VALUES (1, {}, {})", copper, now);
    else
        CharacterDatabase.PExecute(
            "REPLACE INTO realm_treasury (id, copper, updatedAt) VALUES (1, {}, {})", copper, now);
}

void LogLedger()
{
    if (!IsEnabled())
        return;

    uint64 totalIn = 0;
    std::ostringstream in;
    for (size_t i = 0; i < kInflows; ++i)
        if (uint64 const copper = s_paidIn[i].exchange(0, std::memory_order_relaxed))
        {
            totalIn += copper;
            in << (in.tellp() > 0 ? ", " : "") << InflowName(Inflow(i)) << " " << FormatGold(copper);
        }

    uint64 totalOut = 0;
    std::ostringstream out;
    for (size_t i = 0; i < kOutflows; ++i)
        if (uint64 const copper = s_paidOut[i].exchange(0, std::memory_order_relaxed))
        {
            totalOut += copper;
            out << (out.tellp() > 0 ? ", " : "") << OutflowName(Outflow(i)) << " " << FormatGold(copper);
        }

    uint64 const destroyed = s_destroyed.exchange(0, std::memory_order_relaxed);
    uint64 const unfunded = s_unfunded.exchange(0, std::memory_order_relaxed);

    TC_LOG_INFO("server.worldserver",
        "Realm treasury: {} in the pool. In {} ({}); out {} ({}); destroyed {}; bots went unpaid {}.",
        FormatGold(GetBalance()),
        FormatGold(totalIn), totalIn ? in.str() : "nothing",
        FormatGold(totalOut), totalOut ? out.str() : "nothing",
        FormatGold(destroyed), FormatGold(unfunded));
}
}
