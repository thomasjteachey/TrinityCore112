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

#include "ScriptMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "SpellMgr.h"
#include "SpellInfo.h"

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

void LoadHiddenItemsetBonuses();

namespace
{
struct HiddenItemsetBonus
{
    uint32 itemsetId;
    uint8 requiredCount;
    uint32 spellId;
    std::vector<uint32> learnedSpells;
    std::vector<uint32> summonedEntries;
};

// One row is "the N-piece bonus of set X", and a set has several of them
// (3pc / 5pc / 8pc). Keying on itemset_id alone meant the loader's
// `map[itemsetId] = b` kept only whichever row was read last, so two of every
// three tiers were discarded before any player was looked at. The key is the
// pair the table is really identified by, packed high-itemset/low-count so the
// map still iterates a set's tiers together and in ascending piece order.
using HiddenItemsetBonusKey = uint64;

constexpr HiddenItemsetBonusKey MakeHiddenBonusKey(uint32 itemsetId, uint8 requiredCount)
{
    return (uint64(itemsetId) << 8) | uint64(requiredCount);
}

using HiddenItemsetBonusMap = std::map<HiddenItemsetBonusKey, HiddenItemsetBonus>;

HiddenItemsetBonusMap s_HiddenItemsetBonuses;
std::set<uint32> s_AllHiddenItemsetBonusSpells;
std::unordered_map<uint32, std::vector<uint32>> s_KnownHiddenItemsetBonusLearns;
std::unordered_map<HiddenItemsetBonusKey, uint32> s_KnownHiddenItemsetSpellIds;
std::unordered_map<uint32, std::vector<uint32>> s_KnownHiddenItemsetBonusSummons;
using PlayerActiveItemsetMap = std::unordered_map<uint64, std::unordered_set<HiddenItemsetBonusKey>>;
PlayerActiveItemsetMap s_PlayerActiveHiddenItemsets;

struct LearnedSpellRefState
{
    uint32 refCount = 0;
    bool originallyKnown = false;
};

using PlayerLearnedSpellRefMap = std::unordered_map<uint32, LearnedSpellRefState>;
std::unordered_map<uint64, PlayerLearnedSpellRefMap> s_PlayerLearnedSpellRefs;

// Everything above is reachable from more than one thread. HiddenSets::
// OnEquipmentChanged runs from Player::Update (item duration expiry ->
// DestroyItem -> RemoveItem), and Player::Update runs under Map::Update, which
// the MapUpdater executes on parallel worker threads - so two players on two
// maps can insert into the same unordered_map in the same tick and rehash its
// bucket array concurrently. That is a heap corruption, not a benign race, and
// it is exactly the signature the open crash hunt is chasing. `.hiddenitemset
// reload` is worse still: it clears the bonus map while another thread may be
// holding a `HiddenItemsetBonus const&` into one of its nodes.
//
// RECURSIVE on purpose. The lock is held across LearnSpell / RemoveSpell /
// AddAura / CastSpell, any of which can re-enter through a hook, and a plain
// mutex would self-deadlock the moment one did. Nothing under this lock ever
// takes another lock (the helpers only touch the player's own controlled list),
// and the only caller that holds a different lock first - the reload command,
// which takes HashMapHolder<Player> - always takes them in that order, so there
// is no inversion. These paths run on equipment change and login, never per
// tick, so serialising them costs nothing measurable.
std::recursive_mutex s_stateMutex;

void CollectLearnedSpellsFromSpell(uint32 spellId, std::unordered_set<uint32>& learnedSpellSet, std::unordered_set<uint32>& visitedSpells)
{
    if (!spellId)
        return;

    if (!visitedSpells.insert(spellId).second)
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return;

    SpellLearnSpellMapBounds learnBounds = sSpellMgr->GetSpellLearnSpellMapBounds(spellId);
    for (auto itr = learnBounds.first; itr != learnBounds.second; ++itr)
        learnedSpellSet.insert(itr->second.spell);

    for (SpellEffectInfo const& effectInfo : spellInfo->GetEffects())
    {
        if ((effectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL) || effectInfo.IsEffect(SPELL_EFFECT_SCRIPT_EFFECT)) && effectInfo.TriggerSpell)
            learnedSpellSet.insert(effectInfo.TriggerSpell);

        if (effectInfo.TriggerSpell)
            CollectLearnedSpellsFromSpell(effectInfo.TriggerSpell, learnedSpellSet, visitedSpells);
    }
}

bool IsHiddenBonusActive(Player* player, HiddenItemsetBonusKey bonusKey)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    if (!player)
        return false;

    auto itr = s_PlayerActiveHiddenItemsets.find(player->GetGUID().GetRawValue());
    if (itr == s_PlayerActiveHiddenItemsets.end())
        return false;

    return itr->second.find(bonusKey) != itr->second.end();
}

void MarkHiddenBonusActive(Player* player, HiddenItemsetBonusKey bonusKey)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    if (!player)
        return;

    s_PlayerActiveHiddenItemsets[player->GetGUID().GetRawValue()].insert(bonusKey);
}

void MarkHiddenBonusInactive(Player* player, HiddenItemsetBonusKey bonusKey)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    if (!player)
        return;

    auto itr = s_PlayerActiveHiddenItemsets.find(player->GetGUID().GetRawValue());
    if (itr == s_PlayerActiveHiddenItemsets.end())
        return;

    itr->second.erase(bonusKey);
    if (itr->second.empty())
        s_PlayerActiveHiddenItemsets.erase(itr);
}
void AddLearnedSpellRefs(Player* player, HiddenItemsetBonus const& bonus)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    if (!player || bonus.learnedSpells.empty())
        return;

    uint64 guid = player->GetGUID().GetRawValue();
    PlayerLearnedSpellRefMap& playerRefs = s_PlayerLearnedSpellRefs[guid];

    for (uint32 learnedSpell : bonus.learnedSpells)
    {
        LearnedSpellRefState& state = playerRefs[learnedSpell];
        if (state.refCount == 0)
            state.originallyKnown = player->HasSpell(learnedSpell);

        state.refCount += 1;
    }
}

void EnsureOriginalSpellsRestored(Player* player, uint32 learnedSpell, LearnedSpellRefState const& state)
{
    if (!player)
        return;

    if (state.originallyKnown)
    {
        if (!player->HasSpell(learnedSpell))
            player->LearnSpell(learnedSpell, false);
    }
    else
    {
        if (player->HasSpell(learnedSpell))
            player->RemoveSpell(learnedSpell, false, false);
    }
}

void RemoveLearnedSpellRefs(Player* player, HiddenItemsetBonus const& bonus)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    if (!player || bonus.learnedSpells.empty())
        return;

    uint64 guid = player->GetGUID().GetRawValue();
    auto playerItr = s_PlayerLearnedSpellRefs.find(guid);
    if (playerItr == s_PlayerLearnedSpellRefs.end())
        return;

    PlayerLearnedSpellRefMap& playerRefs = playerItr->second;

    for (uint32 learnedSpell : bonus.learnedSpells)
    {
        auto stateItr = playerRefs.find(learnedSpell);
        if (stateItr == playerRefs.end())
            continue;

        LearnedSpellRefState& state = stateItr->second;
        if (state.refCount == 0)
        {
            playerRefs.erase(stateItr);
            continue;
        }

        state.refCount -= 1;
        if (state.refCount == 0)
        {
            EnsureOriginalSpellsRestored(player, learnedSpell, state);
            playerRefs.erase(stateItr);
        }
    }

    if (playerRefs.empty())
        s_PlayerLearnedSpellRefs.erase(playerItr);
}

void RemoveLearnedSpellRefs(Player* player, uint32 spellId)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    auto learnsItr = s_KnownHiddenItemsetBonusLearns.find(spellId);
    if (learnsItr == s_KnownHiddenItemsetBonusLearns.end() || learnsItr->second.empty())
        return;

    HiddenItemsetBonus dummy;
    dummy.spellId = spellId;
    dummy.learnedSpells = learnsItr->second;
    RemoveLearnedSpellRefs(player, dummy);
}

void UnsummonHiddenBonusCreatures(Player* player, HiddenItemsetBonus const& bonus)
{
    if (!player || bonus.summonedEntries.empty())
        return;

    for (uint32 entry : bonus.summonedEntries)
        player->RemoveAllMinionsByEntry(entry);
}

void UnsummonHiddenBonusCreatures(Player* player, uint32 spellId)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    auto summonsItr = s_KnownHiddenItemsetBonusSummons.find(spellId);
    if (summonsItr == s_KnownHiddenItemsetBonusSummons.end() || summonsItr->second.empty())
        return;

    HiddenItemsetBonus dummy;
    dummy.spellId = spellId;
    dummy.summonedEntries = summonsItr->second;
    UnsummonHiddenBonusCreatures(player, dummy);
}

void EnsureHiddenBonusLearns(Player* player, HiddenItemsetBonus const& bonus)
{
    if (!player || bonus.learnedSpells.empty())
        return;

    for (uint32 learnedSpell : bonus.learnedSpells)
    {
        if (!player->HasSpell(learnedSpell))
            player->LearnSpell(learnedSpell, false);
    }
}

void EnsureHiddenBonusSummons(Player* player, HiddenItemsetBonus const& bonus)
{
    if (!player || bonus.summonedEntries.empty())
        return;

    bool needsSummon = false;

    for (uint32 entry : bonus.summonedEntries)
    {
        std::list<Creature*> minions;
        player->GetAllMinionsByEntry(minions, entry);
        if (minions.empty())
        {
            needsSummon = true;
            break;
        }
    }

    if (!needsSummon)
        return;

    UnsummonHiddenBonusCreatures(player, bonus);
    player->CastSpell(player, bonus.spellId, true);
}

void ResummonHiddenBonusCreatures(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    if (!player)
        return;

    uint64 guid = player->GetGUID().GetRawValue();
    auto itr = s_PlayerActiveHiddenItemsets.find(guid);
    if (itr == s_PlayerActiveHiddenItemsets.end())
        return;

    for (HiddenItemsetBonusKey bonusKey : itr->second)
    {
        auto bonusItr = s_HiddenItemsetBonuses.find(bonusKey);
        if (bonusItr == s_HiddenItemsetBonuses.end())
            continue;

        EnsureHiddenBonusSummons(player, bonusItr->second);
    }
}

void RecalcHiddenItemsetBonuses(Player* player)
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    if (!player)
        return;

    std::set<uint32> validSpells;

    // Decide every tier before touching anything. Tiers of one set are separate
    // rows judged separately - that is the point of the composite key - and two
    // rows are allowed to grant the same spell, so the strip below has to know
    // whether some other tier still wants it. Without that, a set whose 3pc and
    // 5pc share a spell would have the 5pc branch tear down the 3pc's aura.
    std::map<HiddenItemsetBonusKey, bool> wanted;
    std::set<uint32> wantedSpells;

    for (auto const& kv : s_HiddenItemsetBonuses)
    {
        HiddenItemsetBonus const& bonus = kv.second;

        validSpells.insert(bonus.spellId);

        bool const shouldBeActive =
            player->GetEquippedItemSetCount(bonus.itemsetId) >= bonus.requiredCount;

        wanted[kv.first] = shouldBeActive;
        if (shouldBeActive)
            wantedSpells.insert(bonus.spellId);
    }

    for (auto const& kv : s_HiddenItemsetBonuses)
    {
        HiddenItemsetBonusKey bonusKey = kv.first;
        HiddenItemsetBonus const& bonus = kv.second;

        bool hasAura = player->HasAura(bonus.spellId);

        bool shouldBeActive = wanted[bonusKey];
        bool isActive = IsHiddenBonusActive(player, bonusKey);

        if (shouldBeActive)
        {
            if (!isActive)
            {
                MarkHiddenBonusActive(player, bonusKey);
                AddLearnedSpellRefs(player, bonus);
            }

            EnsureHiddenBonusLearns(player, bonus);
            EnsureHiddenBonusSummons(player, bonus);

            if (!hasAura)
            {
                player->AddAura(bonus.spellId, player);
                TC_LOG_INFO("server.custom",
                    "hidden_itemset: player {} gained hidden bonus for itemset {} ({}pc, spell {}).",
                    player->GetGUID().ToString(), bonus.itemsetId,
                    uint32(bonus.requiredCount), bonus.spellId);
            }
        }
        else
        {
            // Only tear the spell down if no other tier of any set still wants
            // it; the learned-spell refs below are counted, so they are always
            // released per row.
            if (wantedSpells.find(bonus.spellId) == wantedSpells.end())
            {
                if (hasAura)
                {
                    player->RemoveAura(bonus.spellId);
                    TC_LOG_INFO("server.custom",
                        "hidden_itemset: player {} lost hidden bonus for itemset {} ({}pc, spell {}).",
                        player->GetGUID().ToString(), bonus.itemsetId,
                        uint32(bonus.requiredCount), bonus.spellId);
                }

                UnsummonHiddenBonusCreatures(player, bonus);
            }

            if (isActive)
            {
                RemoveLearnedSpellRefs(player, bonus);
                MarkHiddenBonusInactive(player, bonusKey);
            }
        }
    }

    for (uint32 spellId : s_AllHiddenItemsetBonusSpells)
    {
        if (validSpells.find(spellId) != validSpells.end())
            continue;

        if (player->HasAura(spellId))
            player->RemoveAura(spellId);

        RemoveLearnedSpellRefs(player, spellId);
        UnsummonHiddenBonusCreatures(player, spellId);
    }

    auto activeItr = s_PlayerActiveHiddenItemsets.find(player->GetGUID().GetRawValue());
    if (activeItr != s_PlayerActiveHiddenItemsets.end())
    {
        for (auto it = activeItr->second.begin(); it != activeItr->second.end();)
        {
            HiddenItemsetBonusKey activeKey = *it;
            if (s_HiddenItemsetBonuses.find(activeKey) != s_HiddenItemsetBonuses.end())
            {
                ++it;
                continue;
            }

            auto spellItr = s_KnownHiddenItemsetSpellIds.find(activeKey);
            if (spellItr != s_KnownHiddenItemsetSpellIds.end())
            {
                uint32 spellId = spellItr->second;

                HiddenItemsetBonus cleanup;
                cleanup.spellId = spellId;
                auto learnsItr = s_KnownHiddenItemsetBonusLearns.find(spellId);
                if (learnsItr != s_KnownHiddenItemsetBonusLearns.end())
                    cleanup.learnedSpells = learnsItr->second;

                auto summonsItr = s_KnownHiddenItemsetBonusSummons.find(spellId);
                if (summonsItr != s_KnownHiddenItemsetBonusSummons.end())
                    cleanup.summonedEntries = summonsItr->second;

                // The refcount is always released: it was taken when this key
                // went active and nothing else will ever hand it back.
                RemoveLearnedSpellRefs(player, cleanup);

                // The aura and the minions are NOT, unless the spell is really
                // gone. A retired KEY is not a retired SPELL - editing one row's
                // required_count retires (set, oldCount) while the identical
                // spell_to_apply stays live under (set, newCount), and this loop
                // runs AFTER the main one, so an unguarded strip here silently
                // undoes what the main loop just did (the "lost hidden bonus"
                // log line lives in the main loop, not here).
                if (validSpells.find(spellId) == validSpells.end())
                {
                    if (player->HasAura(spellId))
                        player->RemoveAura(spellId);

                    UnsummonHiddenBonusCreatures(player, cleanup);
                }
            }

            it = activeItr->second.erase(it);
        }

        if (activeItr->second.empty())
            s_PlayerActiveHiddenItemsets.erase(activeItr);
    }
}

class hidden_itemset_bonus_player : public PlayerScript
{
public:
    hidden_itemset_bonus_player() : PlayerScript("hidden_itemset_bonus_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        RecalcHiddenItemsetBonuses(player);
    }

    void OnPlayerResurrect(Player* player) override
    {
        ResummonHiddenBonusCreatures(player);
    }
};

using namespace Trinity::ChatCommands;

class hidden_itemset_bonus_command : public CommandScript
{
public:
    hidden_itemset_bonus_command() : CommandScript("hidden_itemset_bonus_command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable reloadCommandTable =
        {
            { "reload", HandleReload, rbac::RBAC_PERM_COMMAND_GM, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "hiddenitemset", reloadCommandTable }
        };

        return commandTable;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadHiddenItemsetBonuses();

        {
            std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            for (auto const& playerEntry : players)
                if (Player* player = playerEntry.second)
                    RecalcHiddenItemsetBonuses(player);
        }

        handler->SendSysMessage("Hidden itemset bonuses reloaded.");
        return true;
    }
};
} // namespace

void LoadHiddenItemsetBonuses()
{
    std::lock_guard<std::recursive_mutex> guard(s_stateMutex);

    s_HiddenItemsetBonuses.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT itemset_id, required_count, spell_to_apply FROM custom_hidden_itemset_bonus"
    );
    if (!result)
    {
        TC_LOG_INFO("server.custom", "Loaded 0 hidden itemset bonuses.");
        return;
    }

    std::set<uint32> currentSpells;

    do
    {
        Field* f = result->Fetch();

        HiddenItemsetBonus b;
        b.itemsetId     = f[0].GetUInt32();
        b.requiredCount = f[1].GetUInt8();
        b.spellId       = f[2].GetUInt32();

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(b.spellId);
        if (!spellInfo)
        {
            TC_LOG_ERROR("server.custom", "hidden_itemset: unknown spell {} for itemset {}.", b.spellId, b.itemsetId);
            continue;
        }

        std::unordered_set<uint32> learnedSpellSet;
        std::unordered_set<uint32> visitedSpells;
        CollectLearnedSpellsFromSpell(b.spellId, learnedSpellSet, visitedSpells);

        for (SpellEffectInfo const& effectInfo : spellInfo->GetEffects())
        {
            if ((effectInfo.IsEffect(SPELL_EFFECT_SUMMON) || effectInfo.IsEffect(SPELL_EFFECT_SUMMON_PET)) && effectInfo.MiscValue > 0)
                b.summonedEntries.push_back(effectInfo.MiscValue);
        }

        b.learnedSpells.assign(learnedSpellSet.begin(), learnedSpellSet.end());

        HiddenItemsetBonusKey const key = MakeHiddenBonusKey(b.itemsetId, b.requiredCount);

        s_HiddenItemsetBonuses[key] = b;
        currentSpells.insert(b.spellId);
        s_KnownHiddenItemsetBonusLearns[b.spellId] = b.learnedSpells;
        s_KnownHiddenItemsetSpellIds[key] = b.spellId;
        s_KnownHiddenItemsetBonusSummons[b.spellId] = b.summonedEntries;
    }
    while (result->NextRow());

    s_AllHiddenItemsetBonusSpells.insert(currentSpells.begin(), currentSpells.end());

    TC_LOG_INFO("server.custom", "Loaded {} hidden itemset bonuses.", s_HiddenItemsetBonuses.size());
}

namespace HiddenSets
{
void OnEquipmentChanged(Player* player)
{
    RecalcHiddenItemsetBonuses(player);
}
}

void AddSC_custom_hidden_itemset_bonus()
{
    new hidden_itemset_bonus_player();
    new hidden_itemset_bonus_command();
}
