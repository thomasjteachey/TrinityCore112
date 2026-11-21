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

#include "VioletHoldTuning.h"
#include "Config.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "VioletHoldBoons.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
enum class Kind : uint8 { Int, TierPrice, WorldInt, WorldFloat };

struct Knob
{
    char const* name;
    Kind kind;
    int32 def;
    int32 lo;
    int32 hi;
    uint32 index;        // tier (TierPrice) or World config index
    char const* help;
};

Knob const kKnobs[] =
{
    // --- the shop -------------------------------------------------------------
    { "Shop.Enable",                 Kind::Int, 0, 0, 1, 0, "1 = runs that start from now have the shop instead of brokers" },
    { "Shop.Slots",                  Kind::Int, 5, 1, 8, 0, "cards on the board (read as a run starts)" },
    { "Shop.StartingShards",         Kind::Int, 0, 0, 1000, 0, "shards everyone starts a run with" },
    { "Shop.ShardsPerWave",          Kind::Int, 4, 0, 1000, 0, "shards a cleared wave pays everyone" },
    { "Shop.ShardsPerKillingBlow",   Kind::Int, 1, 0, 1000, 0, "shards a killing blow pays its killer" },
    { "Shop.RerollChancePercent",    Kind::Int, 50, 0, 100, 0, "chance a card rerolls as a wave is cleared" },
    { "Shop.DiscountPercent",        Kind::Int, 25, 0, 100, 0, "how much cheaper a card that stays gets, per wave" },
    { "Shop.RerollCost",             Kind::Int, 2, 0, 1000, 0, "first paid reroll of a wave" },
    { "Shop.RerollCostStep",         Kind::Int, 1, 0, 100, 0, "each paid reroll after that, this much more" },
    { "Shop.SafekeepingLocks",       Kind::Int, 3, 0, 8, 0, "cards a Safekeeping holder may lock" },
    { "Shop.UsurerShardsPer",        Kind::Int, 10, 1, 1000, 0, "Usurer: 1 interest shard per this many held" },
    { "Shop.UsurerMax",              Kind::Int, 5, 0, 1000, 0, "Usurer: most interest a wave" },
    { "Shop.FortunePercentPerStack", Kind::Int, 50, 0, 1000, 0, "Fortune: rare+ card weight bonus per stack" },
    { "Shop.Price.Common",           Kind::TierPrice, 3, 0, 1000, 0, "price of a common card" },
    { "Shop.Price.Uncommon",         Kind::TierPrice, 5, 0, 1000, 1, "price of an uncommon card" },
    { "Shop.Price.Rare",             Kind::TierPrice, 8, 0, 1000, 2, "price of a rare card" },
    { "Shop.Price.ClassSpell",       Kind::TierPrice, 10, 0, 1000, 3, "price of a class spell" },
    { "Shop.Price.Legendary",        Kind::TierPrice, 20, 0, 1000, 4, "price of a legendary (and very-rare boons)" },
    { "Shop.Price.Artifact",         Kind::TierPrice, 15, 0, 1000, 5, "price of an artifact" },

    // --- rarity ---------------------------------------------------------------
    { "TopTierRollPercent",          Kind::Int, 50, 0, 1000, 0, "odds of class spells, legendaries, very-rare boons and artifacts, % of their table weight" },

    // --- wave events ----------------------------------------------------------
    { "EventChancePercent",          Kind::Int, 25, 0, 100, 0, "chance a wave carries an event" },
    { "Event.FirstWave",             Kind::Int, 3, 1, 25, 0, "first wave that can carry one" },
    { "Event.Weight.BloodMoon",      Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.Weight.Quicksilver",    Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.Weight.Giants",         Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.Weight.Swarm",          Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.Weight.GoldRush",       Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.Weight.Bounty",         Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.Weight.MirrorMatch",    Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.Weight.Doppelganger",   Kind::Int, 1, 0, 100, 0, "relative odds (0 = off)" },
    { "Event.BloodMoonDamagePercent", Kind::Int, 50, 0, 1000, 0, "Blood Moon: more damage done, %" },
    { "Event.QuicksilverSpeedPercent", Kind::Int, 50, 0, 500, 0, "Quicksilver: run speed, %" },
    { "Event.QuicksilverHastePercent", Kind::Int, 20, 0, 500, 0, "Quicksilver: attack and cast speed, %" },
    { "Event.GiantsCountPercent",    Kind::Int, 67, 1, 100, 0, "Giants: wave size, % of normal" },
    { "Event.GiantScalePercent",     Kind::Int, 100, 0, 400, 0, "Giants: bigger by, %" },
    { "Event.GiantHealthPercent",    Kind::Int, 50, 0, 1000, 0, "Giants: more Stamina, %" },
    { "Event.SwarmCountPercent",     Kind::Int, 200, 100, 400, 0, "Swarm: wave size, % of normal (max 40 clones)" },
    { "Event.SwarmStrengthPercent",  Kind::Int, 50, 1, 100, 0, "Swarm: each clone's strength, %" },
    { "Event.GoldRushMultiplier",    Kind::Int, 3, 1, 20, 0, "Gold Rush: killing-blow shards times this" },
    { "Event.BountyShards",          Kind::Int, 10, 0, 1000, 0, "Bounty: shards each when it dies in time" },
    { "Event.BountySeconds",         Kind::Int, 25, 5, 300, 0, "Bounty: seconds before it escapes" },

    // --- between waves (shop runs) -------------------------------------------
    { "Wave.ReadyToStart",           Kind::Int, 1, 0, 1, 0, "1 = in a shop run the next wave waits until any one player tells the broker to send it; 0 = PrepSeconds timer" },
    { "Wave.ReadyCountdownSeconds",  Kind::Int, 3, 1, 60, 0, "countdown once somebody sends the waiting wave in" },
    { "Wave.ReadyTimeoutSeconds",    Kind::Int, 0, 0, 3600, 0, "start anyway after this long (0 = wait forever)" },
    { "Wave.CallEarly",              Kind::Int, 1, 0, 1, 0, "1 = mid-fight, one player may call the next wave in on top of the live one (never the last)" },
    { "Wave.CallEarlyMaxEnemies",    Kind::Int, 60, 0, 200, 0, "refuse an early call that would put more enemies than this in the Hold (0 = no cap)" },
    { "Wave.CallEarlyShards",        Kind::Int, 2, 0, 1000, 0, "shards for everyone each time a wave is called early" },

    // --- the wave's release ---------------------------------------------------
    { "Release.TimedDelaySeconds",   Kind::Int, 30, 0, 300, 0, "the timed group comes out this long after the gates" },
    { "Release.BurstPercent",        Kind::Int, 50, 0, 100, 0, "% of the timed group that comes out at once" },
    { "Release.StaggerSeconds",      Kind::Int, 3, 1, 60, 0, "then one every this many seconds" },
    { "Release.HideHeld",            Kind::Int, 1, 0, 1, 0, "1 = clones waiting behind open gates are invisible" },

    // --- end of run -----------------------------------------------------------
    { "Reward.Divisor",              Kind::Int, 5, 1, 100, 0, "honor and gold for a run are divided by this" },
    { "Reward.HonorCompoundPercent", Kind::Int, 1, 0, 50, 0, "honor grows this % per wave reached" },

    // --- existing World settings, live until `.reload config` ------------------
    { "PrepSeconds",                 Kind::WorldInt, 0, 1, 300, CONFIG_CENTURION_VHR_PREP_SECONDS, "seconds between waves" },
    { "CountdownFromSeconds",        Kind::WorldInt, 0, 0, 300, CONFIG_CENTURION_VHR_COUNTDOWN_FROM_SECONDS, "countdown starts at" },
    { "FullPowerClonesPerPlayer",    Kind::WorldInt, 0, 0, 40, CONFIG_CENTURION_VHR_FULL_POWER_CLONES_PER_PLAYER, "full-power clones per player before the horde" },
    { "HordeDiminishedStacks",       Kind::WorldInt, 0, 1, 99, CONFIG_CENTURION_VHR_HORDE_DIMINISHED_STACKS, "% the horde clones are weakened" },
    { "PersonalSecondBrokerWave",    Kind::WorldInt, 0, 1, 25, CONFIG_CENTURION_VHR_PERSONAL_SECOND_BROKER_WAVE, "broker mode: wave the 2nd private broker starts" },
    { "PersonalDifficulty.ChampionStaminaPercentPerPlayer", Kind::WorldInt, 0, 0, 125, CONFIG_CENTURION_VHR_PERSONAL_CHAMPION_STAMINA_PER_PLAYER, "wave-15 champion" },
    { "PersonalDifficulty.ChampionDamageBasePercent", Kind::WorldInt, 0, 0, 255, CONFIG_CENTURION_VHR_PERSONAL_CHAMPION_DAMAGE_BASE, "wave-15 champion" },
    { "PersonalDifficulty.ChampionDamagePercentPerPlayer", Kind::WorldInt, 0, 0, 25, CONFIG_CENTURION_VHR_PERSONAL_CHAMPION_DAMAGE_PER_PLAYER, "wave-15 champion" },
    { "PersonalDifficulty.ChampionResolvePercent", Kind::WorldInt, 0, 0, 100, CONFIG_CENTURION_VHR_PERSONAL_CHAMPION_RESOLVE, "wave-15 champion" },
    { "ExperienceMultiplier",        Kind::WorldFloat, 0, 0, 100, CONFIG_CENTURION_VHR_EXPERIENCE_MULTIPLIER, "XP below 60" },
    { "BrokersPerExtraPlayer",       Kind::WorldFloat, 0, 0, 1, CONFIG_CENTURION_VHR_BROKERS_PER_EXTRA_PLAYER, "broker mode" },
    { "StackingBoonGrantMultiplier", Kind::WorldFloat, 0, 0, 5, CONFIG_CENTURION_VHR_STACKING_BOON_GRANT_MULTIPLIER, "stacks a stacking boon pick gives, times this" },
    { "PersonalDifficulty.HealthPercentPerExtraPlayer", Kind::WorldFloat, 0, 0, 25, CONFIG_CENTURION_VHR_PERSONAL_HEALTH_PER_EXTRA_PLAYER, "party scaling" },
    { "PersonalDifficulty.DamagePercentPerExtraPlayer", Kind::WorldFloat, 0, 0, 25, CONFIG_CENTURION_VHR_PERSONAL_DAMAGE_PER_EXTRA_PLAYER, "party scaling" },
};

std::mutex Lock;
std::unordered_map<std::string, int32> Overrides;            // knob -> value
std::unordered_map<uint8, uint32> BoonQuarterOverrides;       // boon -> quarters
std::unordered_map<uint8, uint32> BoonPriceOverrides;         // boon -> shards

// Config lists, parsed once per distinct text.
struct ParsedList
{
    std::string text;
    std::map<uint8, float> values;
};
ParsedList WeightList, PriceList;

std::string Lower(std::string const& text)
{
    std::string out;
    for (char c : text)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

Knob const* FindKnob(std::string const& name)
{
    // The knob names lowered once (thread-safe static init), so a lookup -
    // made for every card of every draw - lowers only the name asked for.
    static std::vector<std::string> const lowered = []
    {
        std::vector<std::string> out;
        for (Knob const& knob : kKnobs)
            out.push_back(Lower(knob.name));
        return out;
    }();
    std::string const key = Lower(name);
    for (size_t i = 0; i < lowered.size(); ++i)
        if (lowered[i] == key)
            return &kKnobs[i];
    return nullptr;
}

std::string ConfigKey(Knob const& knob)
{
    return std::string("Centurion.VioletHold.") + knob.name;
}

// The six tier prices from Shop.TierPrices ("3 5 8 10 20 15").
int32 TierPriceFromString(uint32 tier, int32 def)
{
    std::string const text = sConfigMgr->GetStringDefault("Centurion.VioletHold.Shop.TierPrices", "3 5 8 10 20 15", true);
    uint32 index = 0;
    int32 value = 0;
    bool inNumber = false;
    for (char c : text + " ")
    {
        if (c >= '0' && c <= '9')
        {
            value = std::min(value * 10 + (c - '0'), 100000);
            inNumber = true;
            continue;
        }
        if (inNumber)
        {
            if (index == tier)
                return value;
            ++index;
        }
        value = 0;
        inNumber = false;
    }
    return def;
}

// "phoenix", "Boon of the Phoenix" and "90292" all find Boon of the Phoenix.
Optional<uint8> FindBoon(std::string const& name)
{
    std::string const key = Lower(name);
    if (key.empty())
        return {};
    Optional<uint32> const spellId = Trinity::StringTo<uint32>(key);
    for (uint8 i = 0; i < uint8(VioletHoldBoons::Boon::Max); ++i)
    {
        VioletHoldBoons::BoonInfo const& info = VioletHoldBoons::GetBoon(VioletHoldBoons::Boon(i));
        if (spellId && info.spellId == *spellId)
            return i;
        std::string full = Lower(info.name);
        for (char const* prefix : { "boonofthe", "boonof" })
            if (full.rfind(prefix, 0) == 0)
            {
                full = full.substr(std::string(prefix).size());
                break;
            }
        if (full == key || Lower(info.name) == key)
            return i;
    }
    return {};
}

std::map<uint8, float> const& ParseList(ParsedList& cache, char const* configKey)
{
    std::string const text = sConfigMgr->GetStringDefault(configKey, "", true);
    if (text == cache.text)
        return cache.values;
    cache.text = text;
    cache.values.clear();
    // "name:value name:value", separators: spaces, commas, semicolons.
    std::string token;
    for (char c : text + " ")
    {
        if (c == ' ' || c == ',' || c == ';' || c == '\t')
        {
            size_t const colon = token.find(':');
            if (colon != std::string::npos)
                if (Optional<uint8> boon = FindBoon(token.substr(0, colon)))
                    if (Optional<float> v = Trinity::StringTo<float>(token.substr(colon + 1)))
                        cache.values[*boon] = std::max(0.0f, *v);
            token.clear();
            continue;
        }
        token += c;
    }
    return cache.values;
}

std::string CurrentText(Knob const& knob)
{
    switch (knob.kind)
    {
        case Kind::WorldInt:
            return std::to_string(sWorld->getIntConfig(WorldIntConfigs(knob.index)));
        case Kind::WorldFloat:
            return Trinity::StringFormat("{:.2f}", sWorld->getFloatConfig(WorldFloatConfigs(knob.index)));
        default:
            return std::to_string(VioletHoldTuning::Int(knob.name));
    }
}
}

namespace VioletHoldTuning
{
int32 Int(char const* name)
{
    Knob const* knob = FindKnob(name);
    if (!knob)
        return 0;
    if (knob->kind == Kind::WorldInt)
        return int32(sWorld->getIntConfig(WorldIntConfigs(knob->index)));
    if (knob->kind == Kind::WorldFloat)
        return int32(std::lround(sWorld->getFloatConfig(WorldFloatConfigs(knob->index)) * 100.0f));

    {
        std::lock_guard<std::mutex> guard(Lock);
        auto const itr = Overrides.find(knob->name);
        if (itr != Overrides.end())
            return itr->second;
    }

    int32 def = knob->def;
    if (knob->kind == Kind::TierPrice)
        def = TierPriceFromString(knob->index, knob->def);
    return std::clamp<int32>(sConfigMgr->GetIntDefault(ConfigKey(*knob), def, true), knob->lo, knob->hi);
}

std::string Set(std::string const& name, std::string const& value)
{
    Knob const* knob = FindKnob(name);
    if (!knob)
        return "No knob called '" + name + "'. .vhr tune lists them all.";

    if (Lower(value) == "reset")
    {
        if (knob->kind == Kind::WorldInt || knob->kind == Kind::WorldFloat)
            return std::string(knob->name) + " is a World setting: .reload config puts it back to the file's value.";
        std::lock_guard<std::mutex> guard(Lock);
        Overrides.erase(knob->name);
        return std::string(knob->name) + " is back to its config/default value.";
    }

    if (knob->kind == Kind::WorldFloat)
    {
        Optional<float> v = Trinity::StringTo<float>(value);
        if (!v)
            return "'" + value + "' is not a number.";
        float const clamped = std::clamp(*v, float(knob->lo), float(knob->hi));
        sWorld->setFloatConfig(WorldFloatConfigs(knob->index), clamped);
        return Trinity::StringFormat("{} = {:.2f} (until .reload config).", knob->name, clamped);
    }

    Optional<int32> v = Trinity::StringTo<int32>(value);
    if (!v)
        return "'" + value + "' is not a whole number.";
    int32 const clamped = std::clamp<int32>(*v, knob->lo, knob->hi);
    if (knob->kind == Kind::WorldInt)
    {
        sWorld->setIntConfig(WorldIntConfigs(knob->index), uint32(clamped));
        return Trinity::StringFormat("{} = {} (until .reload config).", knob->name, clamped);
    }

    {
        std::lock_guard<std::mutex> guard(Lock);
        Overrides[knob->name] = clamped;
    }
    return Trinity::StringFormat("{} = {} (live until restart; put Centurion.VioletHold.{} = {} in worldserver.conf to keep it).",
        knob->name, clamped, knob->name, clamped);
}

void List(std::string const& filter, std::vector<std::string>& lines)
{
    std::string const key = Lower(filter);
    for (Knob const& knob : kKnobs)
    {
        if (!key.empty() && Lower(knob.name).find(key) == std::string::npos)
            continue;
        bool overridden = false;
        {
            std::lock_guard<std::mutex> guard(Lock);
            overridden = Overrides.count(knob.name) != 0;
        }
        lines.push_back(Trinity::StringFormat("{} = {}{}  - {}", knob.name, CurrentText(knob), overridden ? " (live)" : "", knob.help));
    }
}

void ResetAll()
{
    std::lock_guard<std::mutex> guard(Lock);
    Overrides.clear();
    BoonQuarterOverrides.clear();
    BoonPriceOverrides.clear();
}

Optional<uint32> GetBoonQuarters(uint8 boonIndex)
{
    std::lock_guard<std::mutex> guard(Lock);
    auto const itr = BoonQuarterOverrides.find(boonIndex);
    if (itr != BoonQuarterOverrides.end())
        return itr->second;
    std::map<uint8, float> const& list = ParseList(WeightList, "Centurion.VioletHold.BoonWeights");
    auto const fromConfig = list.find(boonIndex);
    if (fromConfig != list.end())
        return uint32(std::lround(fromConfig->second * float(VioletHoldBoons::ROLL_QUARTERS_PER_WEIGHT)));
    return {};
}

Optional<uint32> GetBoonPrice(uint8 boonIndex)
{
    std::lock_guard<std::mutex> guard(Lock);
    auto const itr = BoonPriceOverrides.find(boonIndex);
    if (itr != BoonPriceOverrides.end())
        return itr->second;
    std::map<uint8, float> const& list = ParseList(PriceList, "Centurion.VioletHold.Shop.BoonPrices");
    auto const fromConfig = list.find(boonIndex);
    if (fromConfig != list.end())
        return uint32(std::lround(fromConfig->second));
    return {};
}

std::string BoonCommand(std::string const& name, std::string const& what, std::string const& value)
{
    Optional<uint8> const boon = FindBoon(name);
    if (!boon)
        return "No boon called '" + name + "' (try its short name, e.g. phoenix, or its spell id).";
    VioletHoldBoons::BoonInfo const& info = VioletHoldBoons::GetBoon(VioletHoldBoons::Boon(*boon));

    std::string const verb = Lower(what);
    if (verb == "weight" || verb == "price")
    {
        Optional<float> v = Trinity::StringTo<float>(value);
        if (!v || *v < 0.0f)
            return "'" + value + "' is not a number of 0 or more.";
        std::lock_guard<std::mutex> guard(Lock);
        if (verb == "weight")
            BoonQuarterOverrides[*boon] = uint32(std::lround(*v * float(VioletHoldBoons::ROLL_QUARTERS_PER_WEIGHT)));
        else
            BoonPriceOverrides[*boon] = uint32(std::lround(*v));
    }
    else if (verb == "reset")
    {
        std::lock_guard<std::mutex> guard(Lock);
        BoonQuarterOverrides.erase(*boon);
        BoonPriceOverrides.erase(*boon);
    }
    else if (!verb.empty())
        return "Use: .vhr boon <name> [weight <w> | price <shards> | reset]";

    Optional<uint32> const quarters = GetBoonQuarters(*boon);
    Optional<uint32> const price = GetBoonPrice(*boon);
    std::string const weightText = quarters
        ? (*quarters ? Trinity::StringFormat("{:.2f} (override)", float(*quarters) / VioletHoldBoons::ROLL_QUARTERS_PER_WEIGHT) : std::string("0 - never offered"))
        : (VioletHoldBoons::IsArtifact(info.boon) ? std::string("artifact rate") : Trinity::StringFormat("{} (table{})", uint32(info.weight), info.weight ? "" : ", very rare"));
    std::string const priceText = price ? Trinity::StringFormat("{} shards (override)", *price) : std::string("its tier's price");
    return Trinity::StringFormat("{} (spell {}): weight {}, shop price {}.", info.name, info.spellId, weightText, priceText);
}
}
