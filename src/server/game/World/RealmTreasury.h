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

#ifndef TRINITY_REALM_TREASURY_H
#define TRINITY_REALM_TREASURY_H

#include "Define.h"

// The realm's purse: the one place a playerbot's gold may come from.
//
// Bots used to mint copper - merchants paid them for junk, corpses paid them
// coin, a drifter was handed a stipend on landing - and every one of those
// coins could end up in a person's pocket through the auction house. So the
// economy inflated at whatever rate the fleet could farm.
//
// Now every copper a bot receives from the world is WITHDRAWN from this pool,
// and the pool is filled only by gold paid to the world: repairs, trainers,
// merchants, flight masters, talent resets, the auction house's cut and the
// death tax. When the pool is empty a bot is simply not paid. The fleet can
// therefore hand people back at most what people (and bots) spent - and only
// RefillPercent of that, the rest being destroyed as before.
//
// Thread-safe: deposits come from map threads and session handlers, withdrawals
// from the bot managers. The balance is one atomic; persistence and the ledger
// line belong to the world thread (custom_realm_treasury.cpp).
namespace RealmTreasury
{
    // Where a deposit came from. Kept per source so the ledger line can say
    // which sinks actually carry the economy.
    enum class Inflow : uint8
    {
        Repair = 0,
        Training,
        Vendor,
        Taxi,
        TalentReset,
        AuctionCut,
        DeathTax,
        DeathDebt,
        Max
    };

    // Where a withdrawal went.
    enum class Outflow : uint8
    {
        BotVendorSale = 0,
        BotLoot,
        BotStipend,
        Max
    };

    TC_GAME_API bool IsEnabled();

    // Gold paid to the world. RefillPercent of it lands in the pool, the rest
    // is destroyed. A no-op while the treasury is disabled.
    TC_GAME_API void Deposit(uint64 copper, Inflow source);

    // Gold the world pays a bot. Returns how much the pool could fund - all of
    // it while the treasury is disabled, which is the old minting behaviour.
    TC_GAME_API uint64 Withdraw(uint64 copper, Outflow destination);

    TC_GAME_API uint64 GetBalance();

    // Lifecycle, world thread only.
    TC_GAME_API void LoadConfig();
    TC_GAME_API void LoadFromDB();
    TC_GAME_API void SaveToDB(bool direct);
    TC_GAME_API bool IsDirty();

    // One line: balance plus what each source put in and each destination took
    // out since the previous line. Resets the interval counters.
    TC_GAME_API void LogLedger();

    TC_GAME_API char const* InflowName(Inflow source);
    TC_GAME_API char const* OutflowName(Outflow destination);
}

#endif
