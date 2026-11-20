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

#ifndef TRINITYCORE_VIOLET_HOLD_SHOP_H
#define TRINITYCORE_VIOLET_HOLD_SHOP_H

#include "Define.h"
#include "ObjectGuid.h"
#include "VioletHoldBoons.h"

#include <unordered_map>
#include <vector>

class Player;

// The Violet Hold shop (Centurion.VioletHold.Shop.Enable): one board of cards
// the whole party shares, bought with shards, in place of the boon brokers.
//
//  - Every party member has their own shards. A cleared wave pays each of them
//    Shop.ShardsPerWave, and a killing blow on a wave clone pays its killer
//    Shop.ShardsPerKillingBlow.
//  - The board has Shop.Slots cards, drawn from the same pool and weights as a
//    broker's (VioletHoldBoons::RollShopOffer), minus the broker-only boons.
//    A card costs its rarity tier's price (Shop.TierPrices).
//  - Buying a card takes it off the board for everyone, and its slot is drawn
//    again at once.
//  - Each cleared wave, every card either rerolls into a new one
//    (Shop.RerollChancePercent) or stays and gets Shop.DiscountPercent cheaper.
//  - Anyone may pay Shop.RerollCost to redraw every card nobody has locked.
//
// The shop boons change all of that for their holder: Gambler (free rerolls),
// Safekeeping (locks), Tithes and the Usurer (more per wave), Bounties (more
// per kill), Haggler (lower prices) and Bazaar (another slot, for everyone).
//
// The rules and the state live here; BattlegroundVHR owns one per run, and the
// broker NPC's script (violet_hold_boons.cpp) is the counter people buy at.
class TC_GAME_API VioletHoldShop
{
public:
    struct Slot
    {
        VioletHoldBoons::Offer offer;
        // The tier price when the card was drawn; discounts come off this.
        uint32 basePrice = 0;
        // Waves the card has stood through without rerolling.
        uint8 discounts = 0;
        bool filled = false;
        // Boon of Safekeeping: who keeps this card from rerolling, if anyone.
        ObjectGuid lockedBy;
    };

    enum class LockResult : uint8
    {
        Locked,
        Unlocked,
        NoCard,      // not on the board
        NoLocks,     // the player has no Boon of Safekeeping
        AllInUse,    // every one of their locks is on a card already
        NotYours     // somebody else's lock
    };

    // Whether a run starting now uses the shop. Read once per run, as its
    // gates first open (BattlegroundVHR::StartingEventOpenDoors).
    static bool IsEnabledInConfig();
    // The economy knobs, read at use so `.reload config` applies from the
    // next wave or purchase.
    static uint32 GetShardsPerWave();
    static uint32 GetShardsPerKillingBlow();
    static uint32 GetRerollChancePercent();
    static uint32 GetDiscountPercent();
    static uint32 GetRerollCost();

    void Reset();
    // Fills the board for the party as it stands and hands everyone their
    // Shop.StartingShards.
    void Open(std::vector<Player const*> const& roster);
    bool IsOpen() const { return _open; }

    std::vector<Slot> const& GetSlots() const { return _slots; }
    // What a card costs now: its base price less Shop.DiscountPercent for every
    // discount, compounding, never under 1 (a base price of 0 stays free).
    static uint32 GetPrice(Slot const& slot);
    // The same for one buyer: Boon of the Haggler takes a shard off per stack.
    static uint32 GetPriceFor(Slot const& slot, Player const* buyer);
    // The slot showing this offer, or -1.
    int32 FindSlot(VioletHoldBoons::Offer offer) const;

    // Draws a new card into the slot, never one the board already shows. The
    // slot is left empty when the pool has nothing else. Clears any lock.
    void Restock(std::vector<Player const*> const& roster, size_t index);
    // Any card nobody in `roster` can take any more (a unique boon somebody
    // claimed, a class spell its only possible taker already learned) is drawn
    // again, and an empty slot gets another try. True when the board changed.
    bool RestockDeadSlots(std::vector<Player const*> const& roster);
    // A wave was cleared: each unlocked card rerolls or gets cheaper (a locked
    // one only gets cheaper), and everybody's free rerolls come back.
    void AgeBoard(std::vector<Player const*> const& roster);

    // Boon of the Bazaar: one more slot, filled at once. False at the most
    // the board can hold.
    bool AddSlot(std::vector<Player const*> const& roster);
    bool HasRoomForSlot() const;
    uint32 GetExtraSlots() const { return _extraSlots; }

    // The reroll button: free while `who` has Boon of the Gambler rerolls left
    // this wave, else Shop.RerollCost shards. Redraws every unlocked card.
    // False, with nothing changed, when they cannot pay.
    uint32 GetRerollCostFor(Player const* who) const;
    uint32 GetFreeRerollsLeft(Player const* who) const;
    bool Reroll(Player const* who, std::vector<Player const*> const& roster);

    // Boon of Safekeeping: lock the card if it is free, unlock it if it is yours.
    uint32 GetLocksMax(Player const* who) const;
    uint32 GetLocksUsed(ObjectGuid guid) const;
    LockResult ToggleLock(Player const* who, VioletHoldBoons::Offer offer);

    uint32 GetShards(ObjectGuid guid) const;
    void AddShards(ObjectGuid guid, uint32 amount);
    // False, and nothing taken, when the player has fewer than `amount`.
    bool Spend(ObjectGuid guid, uint32 amount);
    // A cleared wave pays `member`: Shop.ShardsPerWave, plus Boon of Tithes,
    // plus Boon of the Usurer's interest on what they held (`interest`).
    // Returns the whole amount paid.
    uint32 PayWave(Player const* member, uint32& interest);
    // What a cleared wave would pay `member` now, interest included.
    uint32 GetWavePayFor(Player const* member) const;
    uint32 GetInterestFor(Player const* member) const;
    // A killing blow on a wave clone: Shop.ShardsPerKillingBlow plus Boon of
    // Bounties, times `multiplier` (the Gold Rush wave event).
    void OnKillingBlow(Player const* killer, uint32 multiplier = 1);
    uint32 GetKillPayFor(Player const* killer) const;
    // Shards a player earned from killing blows since the last call, for the
    // wave-clear line.
    uint32 TakeKillingBlowShards(ObjectGuid guid);

    // Who has the shop window open (the card addon), so a change reaches them.
    void AddViewer(ObjectGuid guid) { _viewers.insert(guid); }
    void RemoveViewer(ObjectGuid guid) { _viewers.erase(guid); }
    GuidSet const& GetViewers() const { return _viewers; }

private:
    bool TryReplace(std::vector<Player const*> const& roster, size_t index);
    std::vector<VioletHoldBoons::Offer> OffersOnBoard() const;

    std::vector<Slot> _slots;
    std::unordered_map<ObjectGuid, uint32> _shards;
    std::unordered_map<ObjectGuid, uint32> _killShardsThisWave;
    std::unordered_map<ObjectGuid, uint32> _freeRerollsUsed;
    GuidSet _viewers;
    uint32 _extraSlots = 0;
    bool _open = false;
};

#endif
