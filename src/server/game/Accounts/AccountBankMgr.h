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

#ifndef TRINITYCORE_ACCOUNT_BANK_MGR_H
#define TRINITYCORE_ACCOUNT_BANK_MGR_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "Optional.h"
#include "ObjectGuid.h"

class ChatHandler;
class Creature;
class Item;
class Player;

namespace AccountBank
{
    static constexpr uint16 MAX_SLOTS = 28;

    // Tournament characters (Miscellaneous/TournamentMode.h) never see the
    // account bank. The same banker opens their TOURNAMENT bank instead: one more
    // account-wide bank, shared only by that account's tournament characters.
    // Its rows live in account_bank_item under the account id with this bit set,
    // which keeps the two banks apart without a schema change - nothing keyed on
    // a real account id (this file's own clears included) can read or delete
    // them, on this realm or any other running an older build.
    static constexpr uint32 TOURNAMENT_BANK_ACCOUNT_FLAG = 0x80000000;

    // The account_bank_item key for this character's bank.
    uint32 GetStorageAccountId(Player const* player);
    // "account bank" or "tournament bank", for messages.
    char const* GetBankName(Player const* player);

    bool IsDepositable(Item const* item);

    bool List(ChatHandler* handler);
    bool Deposit(ChatHandler* handler, Player* player, Item* item);
    bool Withdraw(ChatHandler* handler, Player* player, uint16 slot);

    PreparedQueryResult QueryAccountBankItems(uint32 accountId);

    bool OpenAccountBank(Player* player, ObjectGuid bankerGuid);
    void CloseAccountBank(Player* player);
    void UpdateAccountBankSession(Player* player);
    void UpdateAccountBankSessions();
    void HandleLogin(Player* player);
    bool IsAccountBankOpen(Player const* player);
    bool IsAccountBankAccessible(Player const* player);
    bool IsAccountBankerCreature(Creature const* creature);
    bool IsAccountBanker(Player const* player, ObjectGuid bankerGuid);
}

#endif // TRINITYCORE_ACCOUNT_BANK_MGR_H
