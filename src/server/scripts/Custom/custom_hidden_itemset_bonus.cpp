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

using HiddenItemsetBonusMap = std::map<uint32, HiddenItemsetBonus>;

HiddenItemsetBonusMap s_HiddenItemsetBonuses;
std::set<uint32> s_AllHiddenItemsetBonusSpells;
std::unordered_map<uint32, std::vector<uint32>> s_KnownHiddenItemsetBonusLearns;
std::unordered_map<uint32, uint32> s_KnownHiddenItemsetSpellIds;
std::unordered_map<uint32, std::vector<uint32>> s_KnownHiddenItemsetBonusSummons;
using PlayerActiveItemsetMap = std::unordered_map<uint64, std::unordered_set<uint32>>;
PlayerActiveItemsetMap s_PlayerActiveHiddenItemsets;

struct LearnedSpellRefState
{
    uint32 refCount = 0;
    bool originallyKnown = false;
};

using PlayerLearnedSpellRefMap = std::unordered_map<uint32, LearnedSpellRefState>;
std::unordered_map<uint64, PlayerLearnedSpellRefMap> s_PlayerLearnedSpellRefs;
bool IsHiddenBonusActive(Player* player, uint32 itemsetId)
{
    if (!player)
        return false;

    auto itr = s_PlayerActiveHiddenItemsets.find(player->GetGUID().GetRawValue());
    if (itr == s_PlayerActiveHiddenItemsets.end())
        return false;

    return itr->second.find(itemsetId) != itr->second.end();
}

void MarkHiddenBonusActive(Player* player, uint32 itemsetId)
{
    if (!player)
        return;

    s_PlayerActiveHiddenItemsets[player->GetGUID().GetRawValue()].insert(itemsetId);
}

void MarkHiddenBonusInactive(Player* player, uint32 itemsetId)
{
    if (!player)
        return;

    auto itr = s_PlayerActiveHiddenItemsets.find(player->GetGUID().GetRawValue());
    if (itr == s_PlayerActiveHiddenItemsets.end())
        return;

    itr->second.erase(itemsetId);
    if (itr->second.empty())
        s_PlayerActiveHiddenItemsets.erase(itr);
}
void AddLearnedSpellRefs(Player* player, HiddenItemsetBonus const& bonus)
{
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
    if (!player)
        return;

    uint64 guid = player->GetGUID().GetRawValue();
    auto itr = s_PlayerActiveHiddenItemsets.find(guid);
    if (itr == s_PlayerActiveHiddenItemsets.end())
        return;

    for (uint32 itemsetId : itr->second)
    {
        auto bonusItr = s_HiddenItemsetBonuses.find(itemsetId);
        if (bonusItr == s_HiddenItemsetBonuses.end())
            continue;

        EnsureHiddenBonusSummons(player, bonusItr->second);
    }
}

void RecalcHiddenItemsetBonuses(Player* player)
{
    if (!player)
        return;

    std::set<uint32> validSpells;

    for (auto const& kv : s_HiddenItemsetBonuses)
    {
        uint32 itemsetId = kv.first;
        HiddenItemsetBonus const& bonus = kv.second;

        validSpells.insert(bonus.spellId);

        uint8 have = player->GetEquippedItemSetCount(itemsetId);

        bool hasAura = player->HasAura(bonus.spellId);

        bool shouldBeActive = have >= bonus.requiredCount;
        bool isActive = IsHiddenBonusActive(player, itemsetId);

        if (shouldBeActive)
        {
            if (!isActive)
            {
                MarkHiddenBonusActive(player, itemsetId);
                AddLearnedSpellRefs(player, bonus);
            }

            EnsureHiddenBonusLearns(player, bonus);
            EnsureHiddenBonusSummons(player, bonus);

            if (!hasAura)
            {
                player->AddAura(bonus.spellId, player);
                TC_LOG_INFO("server.custom",
                    "hidden_itemset: player {} gained hidden bonus for itemset {} (spell {}).",
                    player->GetGUID().ToString(), itemsetId, bonus.spellId);
            }
        }
        else
        {
            if (hasAura)
            {
                player->RemoveAura(bonus.spellId);
                TC_LOG_INFO("server.custom",
                    "hidden_itemset: player {} lost hidden bonus for itemset {} (spell {}).",
                    player->GetGUID().ToString(), itemsetId, bonus.spellId);
            }

            UnsummonHiddenBonusCreatures(player, bonus);

            if (isActive)
            {
                RemoveLearnedSpellRefs(player, bonus);
                MarkHiddenBonusInactive(player, itemsetId);
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
            uint32 activeItemset = *it;
            if (s_HiddenItemsetBonuses.find(activeItemset) != s_HiddenItemsetBonuses.end())
            {
                ++it;
                continue;
            }

            auto spellItr = s_KnownHiddenItemsetSpellIds.find(activeItemset);
            if (spellItr != s_KnownHiddenItemsetSpellIds.end())
            {
                uint32 spellId = spellItr->second;
                if (player->HasAura(spellId))
                    player->RemoveAura(spellId);

                HiddenItemsetBonus cleanup;
                cleanup.spellId = spellId;
                auto learnsItr = s_KnownHiddenItemsetBonusLearns.find(spellId);
                if (learnsItr != s_KnownHiddenItemsetBonusLearns.end())
                    cleanup.learnedSpells = learnsItr->second;

                auto summonsItr = s_KnownHiddenItemsetBonusSummons.find(spellId);
                if (summonsItr != s_KnownHiddenItemsetBonusSummons.end())
                    cleanup.summonedEntries = summonsItr->second;

                UnsummonHiddenBonusCreatures(player, cleanup);
                RemoveLearnedSpellRefs(player, cleanup);
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

        SpellLearnSpellMapBounds learnBounds = sSpellMgr->GetSpellLearnSpellMapBounds(b.spellId);
        for (auto itr = learnBounds.first; itr != learnBounds.second; ++itr)
            learnedSpellSet.insert(itr->second.spell);

        for (SpellEffectInfo const& effectInfo : spellInfo->GetEffects())
        {
            if ((effectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL) || effectInfo.IsEffect(SPELL_EFFECT_SCRIPT_EFFECT)) && effectInfo.TriggerSpell)
                learnedSpellSet.insert(effectInfo.TriggerSpell);

            if ((effectInfo.IsEffect(SPELL_EFFECT_SUMMON) || effectInfo.IsEffect(SPELL_EFFECT_SUMMON_PET)) && effectInfo.MiscValue > 0)
                b.summonedEntries.push_back(effectInfo.MiscValue);
        }

        b.learnedSpells.assign(learnedSpellSet.begin(), learnedSpellSet.end());

        s_HiddenItemsetBonuses[b.itemsetId] = b;
        currentSpells.insert(b.spellId);
        s_KnownHiddenItemsetBonusLearns[b.spellId] = b.learnedSpells;
        s_KnownHiddenItemsetSpellIds[b.itemsetId] = b.spellId;
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
