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

#ifndef TradeData_h__
#define TradeData_h__

#include "ObjectGuid.h"

#include <ctime>

enum TradeSlots
{
    TRADE_SLOT_COUNT          = 7,
    TRADE_SLOT_TRADED_COUNT   = 6,
    TRADE_SLOT_NONTRADED      = 6,
    TRADE_SLOT_INVALID        = -1
};

class Item;
class Player;

class TC_GAME_API TradeData
{
public:
    TradeData(Player* player, Player* trader);

    Player* GetTrader() const { return _trader; }
    TradeData* GetTraderData() const;

    Item* GetItem(TradeSlots slot) const;
    bool HasItem(ObjectGuid itemGuid) const;
    TradeSlots GetTradeSlotForItem(ObjectGuid itemGuid) const;
    void SetItem(TradeSlots slot, Item* item, bool update = false);

    uint32 GetSpell() const { return _spell; }
    void SetSpell(uint32 spell_id, Item* castItem = nullptr);

    Item*  GetSpellCastItem() const;
    bool HasSpellCastItem() const { return !_spellCastItem.IsEmpty(); }

    uint32 GetMoney() const { return _money; }
    void SetMoney(uint32 money);

    bool IsAccepted() const { return _accepted; }
    void SetAccepted(bool state, bool forTrader = false);

    bool IsInAcceptProcess() const { return _acceptProccess; }
    void SetInAcceptProcess(bool state) { _acceptProccess = state; }

    // Whether the other end ever put the trade window on screen (CMSG_BEGIN_TRADE).
    //
    // A trade is created for BOTH players the moment one of them asks for it, but
    // the target's client answers only if it can actually open the window - with
    // the auction house, or another full-screen frame, already up it says nothing
    // at all. Neither a begin nor a cancel then arrives, and both players keep a
    // trade object for the rest of the session: one is told "you are already
    // trading", everybody else is told that character is busy. So a trade that was
    // never opened is allowed to go stale, and the next trade attempt clears it.
    bool IsOpened() const { return _opened; }
    void SetOpened() { _opened = true; }
    time_t GetStartedAt() const { return _startedAt; }

private:
    void Update(bool for_trader = true) const;

    Player*    _player;                                // Player who own of this TradeData
    Player*    _trader;                                // Player who trade with _player

    bool       _accepted;                              // _player press accept for trade list
    bool       _acceptProccess;                        // one from player/trader press accept and this processed
    bool       _opened;                                // the trade window was actually opened by the client
    time_t     _startedAt;                             // when the trade was created, for the stale sweep

    uint32     _money;                                 // _player place money to trade

    uint32     _spell;                                 // _player apply spell to non-traded slot item
    ObjectGuid _spellCastItem;                         // applied spell cast by item use

    ObjectGuid _items[TRADE_SLOT_COUNT];               // traded items from _player side including non-traded slot
};

#endif // TradeData_h__
