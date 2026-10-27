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

#include "PairStance.h"

#include "GameTime.h"
#include "Group.h"
#include "Map.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    using PairKey = std::pair<uint64, uint64>;

    PairKey KeyOf(ObjectGuid a, ObjectGuid b)
    {
        uint64 const x = a.GetRawValue();
        uint64 const y = b.GetRawValue();
        return x < y ? PairKey(x, y) : PairKey(y, x);
    }

    struct Entry
    {
        PairStance::Stance Stance = PairStance::Stance::None;
        uint64 SharedBy = 0;   // the person whose party or raid shares it, 0 for the pair alone
    };

    // (bot, group), in that order: unlike a pair, a lookup always knows which
    // half is the group.
    using GroupKey = std::pair<uint64, uint64>;

    // A handful of pairs at any one time - one per notice in play - so an
    // ordered map is plenty and keeps ClearAllFor a simple walk.
    std::shared_mutex g_lock;
    std::map<PairKey, Entry> g_stances;
    std::map<GroupKey, PairStance::Stance> g_groupStances;   // built by Update from the shared pairs
    uint32 g_sharedCount = 0;
    std::atomic<bool> g_any{ false };
    std::atomic<bool> g_anyShared{ false };
    std::atomic<bool> g_anyGroup{ false };
    std::atomic<bool> g_enabled{ true };
    std::atomic<bool> g_sharedDirty{ false };

    // Update's own, world thread only: who was in each group when its entry was
    // built, so a change of members re-sends the bot's colour.
    std::map<GroupKey, uint64> g_groupMembers;
    uint32 g_lastGroupBuildMs = 0;
    constexpr uint32 kGroupRereadMs = 1000;

    std::mutex g_staleLock;
    std::unordered_set<uint64> g_stale;
    std::atomic<bool> g_anyStale{ false };

    void MarkStale(uint64 a, uint64 b)
    {
        std::lock_guard<std::mutex> guard(g_staleLock);
        g_stale.insert(a);
        if (b)
            g_stale.insert(b);
        g_anyStale.store(true, std::memory_order_relaxed);
    }

    // With g_lock held exclusively.
    void ForgetLocked(std::map<PairKey, Entry>::iterator itr)
    {
        if (itr->second.SharedBy)
        {
            --g_sharedCount;
            g_sharedDirty.store(true, std::memory_order_relaxed);
        }
        g_stances.erase(itr);
        g_any.store(!g_stances.empty(), std::memory_order_relaxed);
        g_anyShared.store(g_sharedCount != 0, std::memory_order_relaxed);
    }

    void Put(ObjectGuid a, ObjectGuid b, PairStance::Stance stance, uint64 sharedBy)
    {
        if (a.IsEmpty() || b.IsEmpty() || a == b)
            return;

        if (stance == PairStance::Stance::None)
        {
            PairStance::Clear(a, b);
            return;
        }

        PairKey const key = KeyOf(a, b);
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            Entry& entry = g_stances[key];
            if (entry.Stance == stance && entry.SharedBy == sharedBy)
                return;

            if (entry.SharedBy || sharedBy)
                g_sharedDirty.store(true, std::memory_order_relaxed);
            if (entry.SharedBy && !sharedBy)
                --g_sharedCount;
            else if (!entry.SharedBy && sharedBy)
                ++g_sharedCount;

            entry.Stance = stance;
            entry.SharedBy = sharedBy;
            g_any.store(true, std::memory_order_relaxed);
            g_anyShared.store(g_sharedCount != 0, std::memory_order_relaxed);
        }

        MarkStale(key.first, key.second);
    }

    uint64 GroupOf(Player const* player)
    {
        // Only ever compared against keys, never followed.
        Group const* group = player->GetGroup();
        return group ? group->GetGUID().GetRawValue() : 0;
    }

    // Order-free, so the same members in any order read the same.
    uint64 MemberMix(uint64 raw)
    {
        raw ^= raw >> 33;
        raw *= 0xff51afd7ed558ccdull;
        raw ^= raw >> 33;
        return raw;
    }

    // World thread. Every shared pair, laid onto its person's group as it
    // stands now.
    void RebuildGroups()
    {
        bool const dirty = g_sharedDirty.exchange(false, std::memory_order_relaxed);
        bool const anyShared = g_anyShared.load(std::memory_order_relaxed);
        if (!dirty && !anyShared && g_groupMembers.empty())
            return;

        uint32 const now = GameTime::GetGameTimeMS();
        if (!dirty && now - g_lastGroupBuildMs < kGroupRereadMs)
            return;
        g_lastGroupBuildMs = now;

        std::vector<std::tuple<uint64, uint64, PairStance::Stance>> shared;   // bot, person, stance
        {
            std::shared_lock<std::shared_mutex> guard(g_lock);
            for (auto const& [key, entry] : g_stances)
                if (entry.SharedBy)
                    shared.emplace_back(key.first == entry.SharedBy ? key.second : key.first, entry.SharedBy, entry.Stance);
        }

        std::map<GroupKey, PairStance::Stance> fresh;
        std::map<GroupKey, uint64> members;
        for (auto const& [botRaw, personRaw, stance] : shared)
        {
            Player* person = ObjectAccessor::FindConnectedPlayer(ObjectGuid(personRaw));
            if (!person || !person->IsInWorld() || person->InBattleground())
                continue;

            Group* group = person->GetGroup();
            if (!group || group->IsMember(ObjectGuid(botRaw)))
                continue;   // no group, or the bot is one of them

            GroupKey const key(botRaw, group->GetGUID().GetRawValue());
            auto const [itr, inserted] = fresh.emplace(key, stance);
            if (!inserted)
            {
                if (stance == PairStance::Stance::Feud)
                    itr->second = PairStance::Stance::Feud;
                continue;
            }

            uint64 mix = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
                if (Player* member = ref->GetSource())
                    mix += MemberMix(member->GetGUID().GetRawValue());
            members[key] = mix;
        }

        // Every bot whose colour some viewer now has wrong. Re-sending the bot's
        // own faction field reaches every one of them, each with their own view.
        std::vector<uint64> stale;
        for (auto const& [key, stance] : fresh)
        {
            auto const old = g_groupStances.find(key);   // written only here, on this thread
            auto const oldMembers = g_groupMembers.find(key);
            if (old == g_groupStances.end() || old->second != stance ||
                oldMembers == g_groupMembers.end() || oldMembers->second != members[key])
                stale.push_back(key.first);
        }
        for (auto const& [key, stance] : g_groupStances)
            if (!fresh.count(key))
                stale.push_back(key.first);

        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            g_groupStances.swap(fresh);
            g_anyGroup.store(!g_groupStances.empty(), std::memory_order_relaxed);
        }
        g_groupMembers.swap(members);

        for (uint64 bot : stale)
            MarkStale(bot, 0);
    }

    // World thread. Re-send the faction view of everybody marked stale, and
    // drop the fights that just stopped being legal.
    void FlushStaleViews()
    {
        if (!g_anyStale.load(std::memory_order_relaxed))
            return;

        std::vector<uint64> stale;
        {
            std::lock_guard<std::mutex> guard(g_staleLock);
            stale.assign(g_stale.begin(), g_stale.end());
            g_stale.clear();
            g_anyStale.store(false, std::memory_order_relaxed);
        }

        for (uint64 raw : stale)
        {
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(raw));
            if (!player || !player->IsInWorld())
                continue;

            player->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
            player->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
            for (Unit* controlled : player->m_Controlled)
            {
                controlled->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
                controlled->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
            }

            // Nothing re-checks a fight already under way. Asked of a bot, this
            // also stops anybody hitting it who no longer may - the friends of
            // a person whose shared truce just began, say.
            player->ValidateAttackersAndOwnTarget();
        }
    }
}

namespace PairStance
{
    void Set(ObjectGuid a, ObjectGuid b, Stance stance)
    {
        Put(a, b, stance, 0);
    }

    void SetShared(ObjectGuid bot, ObjectGuid person, Stance stance)
    {
        Put(bot, person, stance, person.GetRawValue());
    }

    void Clear(ObjectGuid a, ObjectGuid b)
    {
        if (a.IsEmpty() || b.IsEmpty())
            return;

        PairKey const key = KeyOf(a, b);
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            auto const itr = g_stances.find(key);
            if (itr == g_stances.end())
                return;
            ForgetLocked(itr);
        }

        MarkStale(key.first, key.second);
    }

    void ClearAllFor(ObjectGuid who)
    {
        // The registry, not Any(): a pair must still be clearable while the
        // switch is off, or switching it back on would resurrect it.
        if (who.IsEmpty() || !g_any.load(std::memory_order_relaxed))
            return;

        uint64 const raw = who.GetRawValue();
        std::vector<PairKey> gone;
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            for (auto itr = g_stances.begin(); itr != g_stances.end(); )
            {
                if (itr->first.first == raw || itr->first.second == raw)
                {
                    gone.push_back(itr->first);
                    auto const next = std::next(itr);
                    ForgetLocked(itr);
                    itr = next;
                }
                else
                    ++itr;
            }
        }

        for (PairKey const& key : gone)
            MarkStale(key.first, key.second);
    }

    Stance Get(ObjectGuid a, ObjectGuid b)
    {
        if (!Any() || a.IsEmpty() || b.IsEmpty() || a == b)
            return Stance::None;

        std::shared_lock<std::shared_mutex> guard(g_lock);
        auto itr = g_stances.find(KeyOf(a, b));
        return itr != g_stances.end() ? itr->second.Stance : Stance::None;
    }

    Stance Between(WorldObject const* a, WorldObject const* b)
    {
        if (!Any() || !a || !b)
            return Stance::None;

        Player const* first = a->GetAffectingPlayer();
        Player const* second = b->GetAffectingPlayer();
        if (!first || !second || first == second)
            return Stance::None;

        // The match owns who fights whom in there. Asked of both sides because
        // a stance is only ever set out in the world, and a mid-transfer object
        // can briefly sit on a different map from the one it is headed to.
        if (Map const* map = first->FindMap(); map && map->IsBattlegroundOrArena())
            return Stance::None;
        if (Map const* map = second->FindMap(); map && map->IsBattlegroundOrArena())
            return Stance::None;

        uint64 const x = first->GetGUID().GetRawValue();
        uint64 const y = second->GetGUID().GetRawValue();
        bool const groups = g_anyGroup.load(std::memory_order_relaxed);
        uint64 const firstGroup = groups ? GroupOf(first) : 0;
        uint64 const secondGroup = groups ? GroupOf(second) : 0;

        std::shared_lock<std::shared_mutex> guard(g_lock);
        if (auto const itr = g_stances.find(x < y ? PairKey(x, y) : PairKey(y, x)); itr != g_stances.end())
            return itr->second.Stance;
        if (secondGroup)
            if (auto const itr = g_groupStances.find(GroupKey(x, secondGroup)); itr != g_groupStances.end())
                return itr->second;
        if (firstGroup)
            if (auto const itr = g_groupStances.find(GroupKey(y, firstGroup)); itr != g_groupStances.end())
                return itr->second;
        return Stance::None;
    }

    bool Any()
    {
        return g_enabled.load(std::memory_order_relaxed) && g_any.load(std::memory_order_relaxed);
    }

    void SetEnabled(bool enabled)
    {
        if (g_enabled.exchange(enabled) == enabled)
            return;

        // Every pair just changed colour, one way or the other, and every bot
        // a group sees through one.
        std::shared_lock<std::shared_mutex> guard(g_lock);
        for (auto const& [key, entry] : g_stances)
            MarkStale(key.first, key.second);
        for (auto const& [key, stance] : g_groupStances)
            MarkStale(key.first, 0);
    }

    void Update()
    {
        RebuildGroups();
        FlushStaleViews();
    }
}
