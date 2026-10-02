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

#include "Miscellaneous/DepletedMarks.h"
#include "Config.h"
#include "DBCStores.h"
#include "Entities/Player/Player.h"
#include "Log.h"
#include "Miscellaneous/TournamentMode.h"
#include "ObjectMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Util.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace Trinity::Custom
{
namespace
{
// A fixed array rather than a vector: map threads walk the list while a
// `.reload config` may be rewriting it, and an array never reallocates.
struct MarkConfig
{
    uint32 RestoredEntry = 20558;
    std::array<uint32, MAX_DEPLETED_MARK_ENTRIES> DepletedEntries = { 20559, 20560, 20561, 20562, 20563, 20564, 20565, 20566, 20567,
                                                                      20568, 20569, 20570, 20571, 20572, 20573, 20574, 20575 };
    std::size_t DepletedCount = 17;
    // Vendor price of the restored mark as a multiple of what it pays; 0 = the
    // npc_vendor row's own price stands.
    float HonorCostMultiplier = 0.0f;
    // Rate.Honor as of this load. The payout the price is a multiple of.
    float HonorRate = 1.0f;
    uint32 PriceRevision = 0;
};

MarkConfig Marks;

// The restored mark's price, resolved against the DBCs and item templates on
// first use after a config change - at startup the config is read before
// either is loaded. World thread only (ObjectMgr::LoadVendors).
uint32 ResolvedMarkCost = 0;   // 0 = leave the npc_vendor price alone
uint32 ResolvedMarkPriceRevision = uint32(-1);

void ResolveMarkPrice()
{
    ResolvedMarkCost = 0;
    ResolvedMarkPriceRevision = Marks.PriceRevision;

    if (Marks.HonorCostMultiplier <= 0.0f)
        return;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(Marks.RestoredEntry);
    if (!proto)
    {
        TC_LOG_ERROR("server.loading", "Centurion.Marks.HonorCostMultiplier: mark item {} does not exist; vendors keep their npc_vendor price.", Marks.RestoredEntry);
        return;
    }

    // What the mark's on-use honor spell pays before any scaling - the
    // `damage` Spell::EffectAddHonor hands to RewardHonor. A die roll is taken
    // at its top, so the price never undercuts a lucky mark.
    int32 basePayout = 0;
    for (_Spell const& itemSpell : proto->Spells)
    {
        if (itemSpell.SpellId <= 0)
            continue;

        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(uint32(itemSpell.SpellId)))
            for (SpellEffectInfo const& effect : spellInfo->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_ADD_HONOR))
                    basePayout += effect.BasePoints + std::max(effect.DieSides, 0);
    }

    if (basePayout <= 0)
    {
        TC_LOG_ERROR("server.loading", "Centurion.Marks.HonorCostMultiplier: mark item {} casts no honor spell; vendors keep their npc_vendor price.", Marks.RestoredEntry);
        return;
    }

    // RewardHonor's own arithmetic: float, times Rate.Honor, truncated. The
    // personal honor-gain auras are left out - a vendor price is one number
    // for everyone.
    int32 const payout = int32(float(basePayout) * Marks.HonorRate);
    if (payout <= 0)
    {
        TC_LOG_ERROR("server.loading", "Centurion.Marks.HonorCostMultiplier: at Rate.Honor {} the mark pays no honor; vendors keep their npc_vendor price.", Marks.HonorRate);
        return;
    }

    // The honor number lives in ItemExtendedCost.dbc, which the client reads
    // for itself, so the price is the cheapest honor-only row at or above the
    // wanted amount - rounded up, never down, so a mark can never be bought
    // back for less than the multiple. Centurion's ladder runs every 1 to 50,
    // every 5 to 200, every 10 to 1000.
    uint32 const wanted = uint32(std::ceil(float(payout) * Marks.HonorCostMultiplier));
    uint32 charged = 0;
    uint32 const costId = ::Tournament::FindHonorOnlyCost(wanted, &charged);
    if (!costId)
    {
        TC_LOG_ERROR("server.loading", "Centurion.Marks.HonorCostMultiplier: no ItemExtendedCost row asks for {} honor or more and nothing else; "
            "vendors keep their npc_vendor price.", wanted);
        return;
    }

    ResolvedMarkCost = costId;
    TC_LOG_INFO("server.loading", "Centurion Mark of Honor {}: pays {} honor at Rate.Honor {}, so vendors charge {} honor (ItemExtendedCost {}, {}x = {}).",
        Marks.RestoredEntry, payout, Marks.HonorRate, charged, costId, Marks.HonorCostMultiplier, wanted);
}

bool IsHonorOnlyCost(uint32 extendedCost)
{
    ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(extendedCost);
    if (!cost || !cost->HonorPoints || cost->ArenaPoints || cost->ArenaBracket || cost->RequiredArenaRating)
        return false;

    for (uint8 slot = 0; slot < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++slot)
        if (cost->ItemID[slot])
            return false;

    return true;
}

std::string_view TrimToken(std::string_view token)
{
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
        token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
        token.remove_suffix(1);
    return token;
}

bool MaskMatches(uint32 allowedMask, uint32 playerMask)
{
    return allowedMask == 0 || allowedMask == uint32(-1) || (allowedMask & playerMask);
}

uint8 GetSpecificityScore(uint32 allowedMask, uint32 playerMask)
{
    if (allowedMask == 0 || allowedMask == uint32(-1))
        return 0;

    if (allowedMask == playerMask)
        return 2;

    if (allowedMask & playerMask)
        return 1;

    return 0;
}

bool IsTemplateEligibleForPlayer(ItemTemplate const* itemTemplate, uint32 classMask, uint32 raceMask)
{
    return MaskMatches(itemTemplate->AllowableClass, classMask) && MaskMatches(itemTemplate->AllowableRace, raceMask);
}
}

void LoadMarkConfig()
{
    MarkConfig loaded;
    loaded.RestoredEntry = uint32(sConfigMgr->GetIntDefault("Centurion.Marks.RestoredEntry", 20558));

    // "20559-20575" or "203608, 203609, ..."
    std::string const raw = sConfigMgr->GetStringDefault("Centurion.Marks.DepletedEntries", "20559-20575");
    std::size_t count = 0;
    for (std::string_view token : Trinity::Tokenize(raw, ',', false))
    {
        token = TrimToken(token);
        if (token.empty())
            continue;

        std::size_t const dash = token.find('-');
        Optional<uint32> const first = Trinity::StringTo<uint32>(TrimToken(token.substr(0, dash)));
        Optional<uint32> const last = dash == std::string_view::npos ? first : Trinity::StringTo<uint32>(TrimToken(token.substr(dash + 1)));
        if (!first || !last || *first > *last)
        {
            TC_LOG_ERROR("server.loading", "Centurion.Marks.DepletedEntries: ignoring '{}', expected an item id or a range first-last.", token);
            continue;
        }

        for (uint32 entry = *first; ; ++entry)
        {
            if (count == loaded.DepletedEntries.size())
            {
                TC_LOG_ERROR("server.loading", "Centurion.Marks.DepletedEntries: more than {} entries, the rest are ignored.", loaded.DepletedEntries.size());
                break;
            }
            loaded.DepletedEntries[count++] = entry;
            if (entry == *last)
                break;
        }
    }
    loaded.DepletedCount = count;

    if (!loaded.DepletedCount)
        TC_LOG_ERROR("server.loading", "Centurion.Marks.DepletedEntries is empty: battleground wins restore no marks and the Mark Transmuter converts nothing.");

    // Called from World::LoadConfigSettings after the rates, so this is the
    // Rate.Honor the reload just read.
    loaded.HonorCostMultiplier = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Marks.HonorCostMultiplier", 0.0f));
    loaded.HonorRate = sWorld->getRate(RATE_HONOR);
    loaded.PriceRevision = Marks.PriceRevision;
    if (loaded.RestoredEntry != Marks.RestoredEntry || loaded.HonorCostMultiplier != Marks.HonorCostMultiplier || loaded.HonorRate != Marks.HonorRate)
        ++loaded.PriceRevision;

    Marks = loaded;
}

uint32 GetRestoredMarkEntry()
{
    return Marks.RestoredEntry;
}

uint32 GetMarkVendorCost(uint32 itemId, uint32 extendedCost)
{
    // Only an honor price is re-priced in honor: a row selling the mark for
    // items, arena points or gold is served as the database wrote it.
    if (!extendedCost || itemId != Marks.RestoredEntry || !IsHonorOnlyCost(extendedCost))
        return extendedCost;

    if (ResolvedMarkPriceRevision != Marks.PriceRevision)
        ResolveMarkPrice();

    return ResolvedMarkCost ? ResolvedMarkCost : extendedCost;
}

uint32 GetMarkPriceRevision()
{
    return Marks.PriceRevision;
}

std::span<uint32 const> GetDepletedMarkEntries()
{
    return std::span<uint32 const>(Marks.DepletedEntries.data(), Marks.DepletedCount);
}

uint32 GetTotalDepletedMarkCount(Player const* player, bool includeBank)
{
    if (!player)
        return 0;

    uint32 total = 0;
    for (uint32 entry : GetDepletedMarkEntries())
        total += player->GetItemCount(entry, includeBank);

    return total;
}

bool HasEnoughDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank)
{
    return GetTotalDepletedMarkCount(player, includeBank) >= requiredCount;
}

bool ConsumeDepletedMarks(Player* player, uint32 amount)
{
    if (!player || amount == 0)
        return false;

    if (!HasEnoughDepletedMarks(player, amount, false))
        return false;

    for (uint32 entry : GetDepletedMarkEntries())
    {
        if (amount == 0)
            break;

        uint32 available = player->GetItemCount(entry);
        if (!available)
            continue;

        uint32 toRemove = std::min(amount, available);
        player->DestroyItemCount(entry, toRemove, true);
        amount -= toRemove;
    }

    return amount == 0;
}

uint32 GetTotalIneligibleDepletedMarkCount(Player const* player, bool includeBank)
{
    if (!player)
        return 0;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    uint32 total = 0;
    for (uint32 entry : GetDepletedMarkEntries())
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        total += player->GetItemCount(entry, includeBank);
    }

    return total;
}

bool HasEnoughIneligibleDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank)
{
    return GetTotalIneligibleDepletedMarkCount(player, includeBank) >= requiredCount;
}

bool ConsumeIneligibleDepletedMarks(Player* player, uint32 amount)
{
    if (!player || amount == 0)
        return false;

    if (!HasEnoughIneligibleDepletedMarks(player, amount, false))
        return false;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    for (uint32 entry : GetDepletedMarkEntries())
    {
        if (amount == 0)
            break;

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        uint32 available = player->GetItemCount(entry);
        if (!available)
            continue;

        uint32 toRemove = std::min(amount, available);
        player->DestroyItemCount(entry, toRemove, true);
        amount -= toRemove;
    }

    return amount == 0;
}

uint32 GetTotalEligibleDepletedMarkCount(Player const* player, bool includeBank)
{
    if (!player)
        return 0;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    uint32 total = 0;
    for (uint32 entry : GetDepletedMarkEntries())
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (!IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        total += player->GetItemCount(entry, includeBank);
    }

    return total;
}

bool HasEnoughEligibleDepletedMarks(Player const* player, uint32 requiredCount, bool includeBank)
{
    return GetTotalEligibleDepletedMarkCount(player, includeBank) >= requiredCount;
}

bool ConsumeEligibleDepletedMarks(Player* player, uint32 amount)
{
    if (!player || amount == 0)
        return false;

    if (!HasEnoughEligibleDepletedMarks(player, amount, false))
        return false;

    while (amount > 0)
    {
        uint32 const entry = GetOwnedEligibleDepletedMarkEntry(player);
        if (!entry)
            break;

        player->DestroyItemCount(entry, 1, true);
        --amount;
    }

    return amount == 0;
}

uint32 GetOwnedEligibleDepletedMarkEntry(Player const* player)
{
    if (!player)
        return 0;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    uint32 bestEntry = 0;
    uint8 bestScore = 0;

    for (uint32 entry : GetDepletedMarkEntries())
    {
        if (!player->GetItemCount(entry))
            continue;

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (!IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        uint8 const classScore = GetSpecificityScore(itemTemplate->AllowableClass, classMask);
        uint8 const raceScore = GetSpecificityScore(itemTemplate->AllowableRace, raceMask);
        uint8 const score = classScore * 3 + raceScore;

        if (score > bestScore)
        {
            bestScore = score;
            bestEntry = entry;
        }
    }

    return bestEntry;
}

uint32 GetDepletedMarkEntryForPlayer(Player const* player)
{
    if (!player)
        return 0;

    uint32 const classMask = player->GetClassMask();
    uint32 const raceMask = player->GetRaceMask();

    uint32 bestEntry = 0;
    uint8 bestScore = 0;

    for (uint32 entry : GetDepletedMarkEntries())
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
        if (!itemTemplate)
            continue;

        if (!IsTemplateEligibleForPlayer(itemTemplate, classMask, raceMask))
            continue;

        uint8 const classScore = GetSpecificityScore(itemTemplate->AllowableClass, classMask);
        uint8 const raceScore = GetSpecificityScore(itemTemplate->AllowableRace, raceMask);
        uint8 const score = classScore * 3 + raceScore;

        if (score > bestScore)
        {
            bestScore = score;
            bestEntry = entry;
        }
    }

    return bestEntry;
}
}

