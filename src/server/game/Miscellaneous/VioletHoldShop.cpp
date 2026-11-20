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

#include "VioletHoldShop.h"
#include "Config.h"
#include "Player.h"
#include "Random.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

using namespace VioletHoldBoons;

namespace
{
constexpr uint32 MAX_SLOTS = 8;
constexpr uint32 MAX_SHARD_AMOUNT = 100000;

// Common, uncommon, rare, class spell, legendary, artifact - GetRarityTier 1-6.
constexpr std::array<uint32, 6> DEFAULT_TIER_PRICES = { 3, 5, 8, 10, 20, 15 };

uint32 ReadInt(char const* name, int32 def, int32 lo, int32 hi)
{
    return uint32(std::clamp<int32>(sConfigMgr->GetIntDefault(name, def), lo, hi));
}

// Six whole numbers, separated by anything that is not a digit ("3 5 8 10 20
// 15" or "3,5,8,10,20,15"). A tier the string leaves out keeps its default.
std::array<uint32, 6> ReadTierPrices()
{
    std::array<uint32, 6> prices = DEFAULT_TIER_PRICES;
    std::string const text = sConfigMgr->GetStringDefault("Centurion.VioletHold.Shop.TierPrices", "3 5 8 10 20 15");

    size_t tier = 0;
    uint32 value = 0;
    bool inNumber = false;
    for (char c : text)
    {
        if (c >= '0' && c <= '9')
        {
            value = std::min<uint32>(value * 10 + uint32(c - '0'), MAX_SHARD_AMOUNT);
            inNumber = true;
            continue;
        }
        if (inNumber && tier < prices.size())
            prices[tier++] = value;
        value = 0;
        inNumber = false;
    }
    if (inNumber && tier < prices.size())
        prices[tier] = value;

    return prices;
}

uint32 GetTierPrice(Offer offer)
{
    uint8 const tier = GetRarityTier(offer);
    std::array<uint32, 6> const prices = ReadTierPrices();
    return tier >= 1 && tier <= prices.size() ? prices[tier - 1] : prices[0];
}
}

bool VioletHoldShop::IsEnabledInConfig()
{
    return sConfigMgr->GetBoolDefault("Centurion.VioletHold.Shop.Enable", false);
}

uint32 VioletHoldShop::GetShardsPerWave()
{
    return ReadInt("Centurion.VioletHold.Shop.ShardsPerWave", 4, 0, 1000);
}

uint32 VioletHoldShop::GetShardsPerKillingBlow()
{
    return ReadInt("Centurion.VioletHold.Shop.ShardsPerKillingBlow", 1, 0, 1000);
}

uint32 VioletHoldShop::GetRerollChancePercent()
{
    return ReadInt("Centurion.VioletHold.Shop.RerollChancePercent", 50, 0, 100);
}

uint32 VioletHoldShop::GetDiscountPercent()
{
    return ReadInt("Centurion.VioletHold.Shop.DiscountPercent", 25, 0, 100);
}

void VioletHoldShop::Reset()
{
    _slots.clear();
    _shards.clear();
    _killShardsThisWave.clear();
    _viewers.clear();
    _open = false;
}

void VioletHoldShop::Open(std::vector<Player const*> const& roster)
{
    Reset();
    _open = true;

    _slots.resize(ReadInt("Centurion.VioletHold.Shop.Slots", 5, 1, int32(MAX_SLOTS)));
    for (size_t i = 0; i < _slots.size(); ++i)
        Restock(roster, i);

    uint32 const starting = ReadInt("Centurion.VioletHold.Shop.StartingShards", 0, 0, 1000);
    if (starting)
        for (Player const* member : roster)
            if (member)
                AddShards(member->GetGUID(), starting);
}

uint32 VioletHoldShop::GetPrice(Slot const& slot)
{
    if (!slot.basePrice)
        return 0;

    uint32 const discountPercent = GetDiscountPercent();
    double const kept = std::pow(double(100 - discountPercent) / 100.0, double(slot.discounts));
    long const price = std::lround(double(slot.basePrice) * kept);
    return uint32(std::max<long>(1, price));
}

int32 VioletHoldShop::FindSlot(Offer offer) const
{
    for (size_t i = 0; i < _slots.size(); ++i)
        if (_slots[i].filled && _slots[i].offer == offer)
            return int32(i);
    return -1;
}

std::vector<Offer> VioletHoldShop::OffersOnBoard() const
{
    std::vector<Offer> out;
    for (Slot const& slot : _slots)
        if (slot.filled)
            out.push_back(slot.offer);
    return out;
}

bool VioletHoldShop::TryReplace(std::vector<Player const*> const& roster, size_t index)
{
    if (index >= _slots.size())
        return false;

    // Never a card already on the board - this slot's own included, so a
    // reroll or a restock always shows something new.
    Optional<Offer> const rolled = RollShopOffer(roster, OffersOnBoard());
    if (!rolled)
        return false;

    Slot& slot = _slots[index];
    slot.offer = *rolled;
    slot.basePrice = GetTierPrice(*rolled);
    slot.discounts = 0;
    slot.filled = true;
    return true;
}

void VioletHoldShop::Restock(std::vector<Player const*> const& roster, size_t index)
{
    if (index >= _slots.size())
        return;
    if (!TryReplace(roster, index))
        _slots[index] = Slot();
}

bool VioletHoldShop::RestockDeadSlots(std::vector<Player const*> const& roster)
{
    bool changed = false;
    for (size_t i = 0; i < _slots.size(); ++i)
    {
        Slot const& slot = _slots[i];
        if (slot.filled)
        {
            bool const anyoneCan = std::any_of(roster.begin(), roster.end(), [&slot](Player const* member)
            {
                return member && CanTake(member, slot.offer) == PickResult::Ok;
            });
            if (anyoneCan)
                continue;
            Restock(roster, i);
            changed = true;
        }
        else
            changed |= TryReplace(roster, i);
    }
    return changed;
}

void VioletHoldShop::AgeBoard(std::vector<Player const*> const& roster)
{
    int32 const rerollChance = int32(GetRerollChancePercent());
    for (size_t i = 0; i < _slots.size(); ++i)
    {
        if (!_slots[i].filled)
        {
            TryReplace(roster, i);
            continue;
        }

        // A card that rolls a reroll but has nothing new to become stays, and
        // takes the discount instead.
        if (roll_chance_i(rerollChance) && TryReplace(roster, i))
            continue;

        if (_slots[i].discounts < 255)
            ++_slots[i].discounts;
    }

    // Whatever nobody can take any more goes too.
    RestockDeadSlots(roster);
}

uint32 VioletHoldShop::GetShards(ObjectGuid guid) const
{
    auto const itr = _shards.find(guid);
    return itr != _shards.end() ? itr->second : 0;
}

void VioletHoldShop::AddShards(ObjectGuid guid, uint32 amount)
{
    if (!guid || !amount)
        return;
    uint32& shards = _shards[guid];
    shards = std::min<uint32>(shards + amount, MAX_SHARD_AMOUNT);
}

bool VioletHoldShop::Spend(ObjectGuid guid, uint32 amount)
{
    if (!amount)
        return true;
    auto const itr = _shards.find(guid);
    if (itr == _shards.end() || itr->second < amount)
        return false;
    itr->second -= amount;
    return true;
}

void VioletHoldShop::OnKillingBlow(ObjectGuid killer)
{
    uint32 const amount = GetShardsPerKillingBlow();
    if (!_open || !killer || !amount)
        return;
    AddShards(killer, amount);
    uint32& tally = _killShardsThisWave[killer];
    tally = std::min<uint32>(tally + amount, MAX_SHARD_AMOUNT);
}

uint32 VioletHoldShop::TakeKillingBlowShards(ObjectGuid guid)
{
    auto const itr = _killShardsThisWave.find(guid);
    if (itr == _killShardsThisWave.end())
        return 0;
    uint32 const amount = itr->second;
    _killShardsThisWave.erase(itr);
    return amount;
}
