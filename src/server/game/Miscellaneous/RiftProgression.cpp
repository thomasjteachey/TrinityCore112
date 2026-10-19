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

#include "RiftProgression.h"

#include "Battleground.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // League's starting purse, and its gold to the character's money: one League
    // gold is one silver, so prices read the same in both games.
    uint32 constexpr START_GOLD = 475;
    uint32 constexpr COPPER_PER_LEAGUE_GOLD = 100;

    // League's experience curve (Season 3): from level n to n+1 takes 180 + 100n,
    // and 18 is the cap. The character's real level is reached at League 18.
    uint8 constexpr LEAGUE_MAX_LEVEL = 18;
    float XpToNext(uint8 leagueLevel) { return 180.0f + 100.0f * float(leagueLevel); }

    // The addon line holds at most this many offers; the rest wait their turn.
    size_t constexpr MAX_OFFERS_SENT = 30;

    struct Chain
    {
        std::vector<uint32> Ranks;   // the ranks the character knows, lowest first
    };

    struct Talent
    {
        uint32 Tab = 0;
        uint32 Tier = 0;
        uint32 Column = 0;
        uint32 Cost = 0;             // ranks learned = points spent
        uint32 Spell = 0;            // the learned rank's spell
        std::vector<uint32> Taught;  // spells that rank teaches (Mortal Strike and the like)
    };

    struct State
    {
        uint8 RealLevel = 1;
        uint32 RealXp = 0;
        uint32 RealMoney = 0;
        bool Persistent = true;      // false for a transient copy: nothing is written down

        float LeagueXp = 0.0f;
        uint8 LeagueLevel = 1;
        uint8 MatchLevel = 1;

        std::map<uint32 /*first rank*/, Chain> Chains;
        std::unordered_map<uint32 /*spell*/, uint32 /*first rank*/> SpellToChain;
        std::map<uint32 /*first rank*/, uint8 /*ranks unlocked*/> Unlocked;
        uint32 PendingPicks = 0;

        std::vector<Talent> Talents; // in the order they come back: tier, then tree, then column
        size_t TalentsRestored = 0;
        std::unordered_set<uint32> LockedTalentSpells;
    };

    // Recursive: GiveLevel, called with the lock held, comes straight back through
    // InitTalentForLevel into SuppressTalentReset.
    std::recursive_mutex StateMutex;
    std::unordered_map<ObjectGuid::LowType, State> States;
    // Characters with a row in custom_rift_stash. Read at startup and kept with the
    // rows, because a character that crashed out of a match loads at level 1 BEFORE
    // OnLogin runs, and its talents must already be protected while it loads.
    std::unordered_set<ObjectGuid::LowType> Stashed;
    bool StoragePresent = false;

    bool IsTransient(Player const* player)
    {
        WorldSession const* session = player->GetSession();
        return !session || session->IsTransientPlayerSession();
    }

    // A class ability the match governs: one of the character's own class skill
    // lines, not passive, and trained at a level. Talents (and what they teach)
    // are governed by the talent rules instead.
    bool IsGovernedClassSpell(Player const* player, SpellInfo const* spellInfo)
    {
        if (!spellInfo || spellInfo->IsPassive() || spellInfo->SpellLevel == 0 || GetTalentSpellPos(spellInfo->Id))
            return false;

        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            SkillLineEntry const* skill = sSkillLineStore.LookupEntry(ability->SkillLine);
            if (skill && skill->CategoryID == SKILL_CATEGORY_CLASS && (!ability->ClassMask || (ability->ClassMask & player->GetClassMask())))
                return true;
        }
        return false;
    }

    void BuildState(Player const* player, State& state)
    {
        uint8 const spec = player->GetActiveSpec();

        // ---- talents
        std::unordered_set<uint32> taughtByTalents;
        for (TalentEntry const* talent : sTalentStore)
        {
            TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TabID) : nullptr;
            if (!tab || !(tab->ClassMask & player->GetClassMask()))
                continue;

            int32 learned = -1;
            for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
                if (talent->SpellRank[rank] && player->HasTalent(talent->SpellRank[rank], spec))
                {
                    learned = rank;
                    break;
                }
            if (learned < 0)
                continue;

            Talent t;
            t.Tab = talent->TabID;
            t.Tier = talent->TierID;
            t.Column = talent->ColumnIndex;
            t.Cost = uint32(learned) + 1;
            t.Spell = talent->SpellRank[learned];
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(t.Spell))
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    {
                        t.Taught.push_back(effect.TriggerSpell);
                        taughtByTalents.insert(effect.TriggerSpell);
                    }
            state.Talents.push_back(t);
            state.LockedTalentSpells.insert(t.Spell);
            state.LockedTalentSpells.insert(t.Taught.begin(), t.Taught.end());
        }
        std::sort(state.Talents.begin(), state.Talents.end(), [](Talent const& a, Talent const& b)
        {
            return std::tie(a.Tier, a.Tab, a.Column) < std::tie(b.Tier, b.Tab, b.Column);
        });

        // ---- class spells, grouped by rank chain
        for (auto const& [spellId, playerSpell] : player->GetSpellMap())
        {
            if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled || taughtByTalents.count(spellId))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!IsGovernedClassSpell(player, info))
                continue;
            uint32 const first = sSpellMgr->GetFirstSpellInChain(spellId);
            state.Chains[first ? first : spellId].Ranks.push_back(spellId);
        }
        for (auto& [first, chain] : state.Chains)
        {
            std::sort(chain.Ranks.begin(), chain.Ranks.end(), [](uint32 a, uint32 b)
            {
                return sSpellMgr->GetSpellRank(a) < sSpellMgr->GetSpellRank(b);
            });
            for (uint32 spell : chain.Ranks)
                state.SpellToChain[spell] = first;
        }
    }

    uint32 TotalSteps(State const& state)
    {
        uint32 total = 0;
        for (auto const& [first, chain] : state.Chains)
            total += uint32(chain.Ranks.size());
        return total;
    }

    uint32 UnlockedSteps(State const& state)
    {
        uint32 total = 0;
        for (auto const& [first, ranks] : state.Unlocked)
            total += ranks;
        return total;
    }

    // Picks for reaching match level `level`, spread over the levels still to come
    // so the last one unlocks the last rank.
    uint32 PicksForLevel(State const& state, uint8 level)
    {
        uint32 const remaining = TotalSteps(state) - std::min(TotalSteps(state), UnlockedSteps(state) + state.PendingPicks);
        uint32 const levelsLeft = state.RealLevel >= level ? uint32(state.RealLevel - level + 1) : 1;
        return remaining ? (remaining + levelsLeft - 1) / levelsLeft : 0;
    }

    // What can be chosen now: each chain's next rank, once the match level has
    // reached the level it is trained at.
    std::vector<uint32> Offers(State const& state)
    {
        std::vector<uint32> offers;
        for (auto const& [first, chain] : state.Chains)
        {
            auto const itr = state.Unlocked.find(first);
            uint8 const have = itr != state.Unlocked.end() ? itr->second : 0;
            if (have >= chain.Ranks.size())
                continue;
            SpellInfo const* next = sSpellMgr->GetSpellInfo(chain.Ranks[have]);
            if (next && next->SpellLevel <= state.MatchLevel)
                offers.push_back(chain.Ranks[have]);
        }
        return offers;
    }

    void SetTalentAuras(Player* player, Talent const& talent, bool on)
    {
        auto apply = [&](uint32 spell)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            if (!info || !info->IsPassive())
                return;
            if (on)
            {
                if (!player->HasAura(spell))
                    player->CastSpell(player, spell, true);
            }
            else
                player->RemoveAurasDueToSpell(spell);
        };
        apply(talent.Spell);
        for (uint32 spell : talent.Taught)
            apply(spell);
    }

    // As many talents as the match level's points pay for come back, cheapest
    // tier first.
    void RestoreTalents(Player* player, State& state)
    {
        uint32 const budget = player->CalculateTalentsPointsForLevel(state.MatchLevel);
        uint32 spent = 0;
        for (size_t i = 0; i < state.TalentsRestored; ++i)
            spent += state.Talents[i].Cost;
        while (state.TalentsRestored < state.Talents.size() && spent + state.Talents[state.TalentsRestored].Cost <= budget)
        {
            Talent const& talent = state.Talents[state.TalentsRestored++];
            spent += talent.Cost;
            state.LockedTalentSpells.erase(talent.Spell);
            for (uint32 spell : talent.Taught)
                state.LockedTalentSpells.erase(spell);
            SetTalentAuras(player, talent, true);
        }
    }

    void SendLine(Player* player, std::string const& line)
    {
        if (!IsTransient(player))
            Battleground::SendSquadFeedLine(player, line);
    }

    void SendState(Player* player, State const& state)
    {
        SendLine(player, Trinity::StringFormat("RIFT:{}:{}:{}:{}", state.MatchLevel, state.RealLevel, state.LeagueLevel, state.PendingPicks));
        std::string offers;
        if (state.PendingPicks)
        {
            std::vector<uint32> const list = Offers(state);
            for (size_t i = 0; i < list.size() && i < MAX_OFFERS_SENT; ++i)
                offers += (i ? "," : "") + std::to_string(list[i]);
        }
        SendLine(player, "RIFTPICKS:" + offers);
    }

    // The XP bar shows the way to the next match level.
    void ShowProgress(Player* player, State const& state)
    {
        float const leagueProgress = float(state.LeagueLevel - 1)
            + (state.LeagueLevel < LEAGUE_MAX_LEVEL ? state.LeagueXp / XpToNext(state.LeagueLevel) : 0.0f);
        float const levels = leagueProgress / float(LEAGUE_MAX_LEVEL - 1) * float(std::max<uint8>(state.RealLevel, 2) - 1);
        float const frac = std::clamp(levels - float(state.MatchLevel - 1), 0.0f, 1.0f);
        player->SetUInt32Value(PLAYER_XP, uint32(frac * float(player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP))));
    }

    void SetMatchLevel(Player* player, State& state, uint8 level)
    {
        while (state.MatchLevel < level)
        {
            ++state.MatchLevel;
            state.PendingPicks += PicksForLevel(state, state.MatchLevel);
        }
        player->GiveLevel(state.MatchLevel, true);
        RestoreTalents(player, state);
        player->SetFullHealth();
        ShowProgress(player, state);
        SendState(player, state);
    }

    State* Find(Player const* player)
    {
        auto const itr = States.find(player->GetGUID().GetCounter());
        return itr != States.end() ? &itr->second : nullptr;
    }

    bool IsInRiftBattleground(Player const* player)
    {
        Battleground const* bg = player->GetBattleground();
        return bg && bg->GetTypeID(true) == BATTLEGROUND_SRF && bg->GetStatus() != STATUS_WAIT_LEAVE;
    }
}

namespace RiftProgression
{
void ProbeStorage()
{
    StoragePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'custom_rift_stash'") != nullptr;
    if (!StoragePresent)
    {
        TC_LOG_INFO("server.loading", ">> Summoner's Rift progression off: `custom_rift_stash` is absent.");
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    Stashed.clear();
    if (QueryResult rows = CharacterDatabase.Query("SELECT guid FROM custom_rift_stash"))
        do
            Stashed.insert(rows->Fetch()[0].GetUInt32());
        while (rows->NextRow());
    TC_LOG_INFO("server.loading", ">> Summoner's Rift progression on; {} character(s) to restore.", Stashed.size());
}

bool IsInMatch(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    return player && Find(player);
}

void Enter(Player* player)
{
    if (!player || !StoragePresent)
        return;

    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    if (State* existing = Find(player))
    {
        // Back in the match it left: the engine put the talent auras back at login.
        for (size_t i = existing->TalentsRestored; i < existing->Talents.size(); ++i)
            SetTalentAuras(player, existing->Talents[i], false);
        SendState(player, *existing);
        return;
    }

    State& state = States[player->GetGUID().GetCounter()];
    state.RealLevel = player->GetLevel();
    state.RealXp = player->GetUInt32Value(PLAYER_XP);
    state.RealMoney = player->GetMoney();
    state.Persistent = !IsTransient(player);
    BuildState(player, state);

    // Written down before anything changes: from here on every way out restores.
    if (state.Persistent)
    {
        CharacterDatabase.DirectPExecute("REPLACE INTO custom_rift_stash (guid, real_level, real_xp, money) VALUES ({}, {}, {}, {})",
            player->GetGUID().GetCounter(), uint32(state.RealLevel), state.RealXp, state.RealMoney);
        Stashed.insert(player->GetGUID().GetCounter());
    }

    player->SetMoney(START_GOLD * COPPER_PER_LEAGUE_GOLD);
    for (Talent const& talent : state.Talents)
        SetTalentAuras(player, talent, false);

    state.MatchLevel = 1;
    state.PendingPicks = PicksForLevel(state, 1);
    player->GiveLevel(1, true);
    player->SetFullHealth();
    player->SetFullPower(player->GetPowerType());
    ShowProgress(player, state);
    SendState(player, state);

    TC_LOG_DEBUG("bg.battleground", "Rift: {} enters at level 1 (real {}), {} spell steps and {} talents to earn back.",
        player->GetName(), state.RealLevel, TotalSteps(state), state.Talents.size());
}

void Leave(Player* player)
{
    if (!player)
        return;

    State state;
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        auto const itr = States.find(player->GetGUID().GetCounter());
        if (itr == States.end())
            return;
        state = std::move(itr->second);
        States.erase(itr);
        Stashed.erase(player->GetGUID().GetCounter());
    }

    // With the state gone the talent guard stands down: at the real level every
    // talent is paid for again, so InitTalentForLevel has nothing to reset.
    player->GiveLevel(state.RealLevel, true);
    player->SetUInt32Value(PLAYER_XP, state.RealXp);
    for (Talent const& talent : state.Talents)
        SetTalentAuras(player, talent, true);
    player->SetMoney(state.RealMoney);

    if (state.Persistent)
        CharacterDatabase.PExecute("DELETE FROM custom_rift_stash WHERE guid = {}", player->GetGUID().GetCounter());
    SendLine(player, "RIFT:END");
}

void OnLogin(Player* player)
{
    if (!player || !StoragePresent)
        return;

    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        if (!Stashed.count(player->GetGUID().GetCounter()))
            return;
        if (Find(player) && IsInRiftBattleground(player))
            return;   // BattlegroundSRF::AddPlayer re-applies the rules on the way back in
    }

    QueryResult row = CharacterDatabase.PQuery("SELECT real_level, real_xp, money FROM custom_rift_stash WHERE guid = {}", player->GetGUID().GetCounter());
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        States.erase(player->GetGUID().GetCounter());
        Stashed.erase(player->GetGUID().GetCounter());
    }
    if (!row)
        return;

    Field* fields = row->Fetch();
    uint8 const realLevel = fields[0].GetUInt8();
    player->GiveLevel(realLevel, true);
    player->SetUInt32Value(PLAYER_XP, fields[1].GetUInt32());
    player->SetMoney(fields[2].GetUInt32());
    CharacterDatabase.PExecute("DELETE FROM custom_rift_stash WHERE guid = {}", player->GetGUID().GetCounter());
    TC_LOG_INFO("bg.battleground", "Rift: {} logged in outside the match it left; level {}, experience and money restored.", player->GetName(), realLevel);
}

bool SuppressTalentReset(Player const* player)
{
    if (!player)
        return false;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    return States.count(guid) || Stashed.count(guid);
}

SpellInfo const* AdjustCast(Player const* player, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo)
        return spellInfo;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    if (!state)
        return spellInfo;
    if (state->LockedTalentSpells.count(spellInfo->Id))
        return nullptr;
    auto const chain = state->SpellToChain.find(spellInfo->Id);
    if (chain == state->SpellToChain.end())
        return spellInfo;
    auto const unlocked = state->Unlocked.find(chain->second);
    uint8 const have = unlocked != state->Unlocked.end() ? unlocked->second : 0;
    if (!have)
        return nullptr;
    std::vector<uint32> const& ranks = state->Chains.at(chain->second).Ranks;
    auto const pos = std::find(ranks.begin(), ranks.end(), spellInfo->Id);
    if (pos != ranks.end() && size_t(pos - ranks.begin()) < have)
        return spellInfo;
    // A higher rank than the one unlocked: the action bar keeps its button and
    // casts the best rank the match has given back.
    return sSpellMgr->GetSpellInfo(ranks[have - 1]);
}

bool IsSpellLocked(Player const* player, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo)
        return false;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    if (!state)
        return false;
    if (state->LockedTalentSpells.count(spellInfo->Id))
        return true;
    auto const chain = state->SpellToChain.find(spellInfo->Id);
    if (chain == state->SpellToChain.end())
        return false;
    auto const unlocked = state->Unlocked.find(chain->second);
    uint8 const have = unlocked != state->Unlocked.end() ? unlocked->second : 0;
    std::vector<uint32> const& ranks = state->Chains.at(chain->second).Ranks;
    auto const pos = std::find(ranks.begin(), ranks.end(), spellInfo->Id);
    return pos == ranks.end() || size_t(pos - ranks.begin()) >= have;
}

void AddExperience(Player* player, float leagueXp)
{
    if (!player || leagueXp <= 0.0f)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;

    state->LeagueXp += leagueXp;
    while (state->LeagueLevel < LEAGUE_MAX_LEVEL && state->LeagueXp >= XpToNext(state->LeagueLevel))
    {
        state->LeagueXp -= XpToNext(state->LeagueLevel);
        ++state->LeagueLevel;
    }
    if (state->LeagueLevel >= LEAGUE_MAX_LEVEL)
        state->LeagueXp = 0.0f;

    float const leagueProgress = float(state->LeagueLevel - 1)
        + (state->LeagueLevel < LEAGUE_MAX_LEVEL ? state->LeagueXp / XpToNext(state->LeagueLevel) : 0.0f);
    uint8 const target = std::min<uint8>(state->RealLevel,
        uint8(1 + std::floor(leagueProgress / float(LEAGUE_MAX_LEVEL - 1) * float(std::max<uint8>(state->RealLevel, 1) - 1) + 1e-4f)));
    if (target > state->MatchLevel)
        SetMatchLevel(player, *state, target);
    else
        ShowProgress(player, *state);
}

void AddGold(Player* player, uint32 leagueGold)
{
    if (!player || !leagueGold || !IsInMatch(player))
        return;
    player->ModifyMoney(int32(leagueGold * COPPER_PER_LEAGUE_GOLD), false);
}

uint8 GetLeagueLevel(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? state->LeagueLevel : 1;
}

void Maintain(Player* player)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    if (!state)
        return;
    for (size_t i = state->TalentsRestored; i < state->Talents.size(); ++i)
        SetTalentAuras(player, state->Talents[i], false);
}

void HandlePickRequest(Player* player, std::string_view argument)
{
    if (!player)
        return;
    Optional<uint32> const spellId = Trinity::StringTo<uint32>(argument);
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state || !spellId)
        return;
    if (state->PendingPicks)
    {
        std::vector<uint32> const offers = Offers(*state);
        if (std::find(offers.begin(), offers.end(), *spellId) != offers.end())
        {
            ++state->Unlocked[state->SpellToChain.at(*spellId)];
            --state->PendingPicks;
        }
    }
    SendState(player, *state);
}

void HandleStateRequest(Player* player, std::string_view /*argument*/)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    if (State const* state = Find(player))
        SendState(player, *state);
}
}
