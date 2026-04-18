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
class Item;
class Player;

namespace AccountBank
{
    static constexpr uint16 MAX_SLOTS = 28;

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
    bool IsAccountBanker(Player const* player, ObjectGuid bankerGuid);
}

#endif // TRINITYCORE_ACCOUNT_BANK_MGR_H
