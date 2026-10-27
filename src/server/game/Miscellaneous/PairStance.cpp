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

#include "Map.h"
#include "Object.h"
#include "Player.h"

#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <utility>

namespace
{
    using PairKey = std::pair<uint64, uint64>;

    PairKey KeyOf(ObjectGuid a, ObjectGuid b)
    {
        uint64 const x = a.GetRawValue();
        uint64 const y = b.GetRawValue();
        return x < y ? PairKey(x, y) : PairKey(y, x);
    }

    // A handful of pairs at any one time - one per notice in play - so an
    // ordered map is plenty and keeps ClearAllFor a simple walk.
    std::shared_mutex g_lock;
    std::map<PairKey, PairStance::Stance> g_stances;
    std::atomic<bool> g_any{ false };

    std::mutex g_staleLock;
    std::unordered_set<uint64> g_stale;
    std::atomic<bool> g_anyStale{ false };

    void MarkStale(uint64 a, uint64 b)
    {
        std::lock_guard<std::mutex> guard(g_staleLock);
        g_stale.insert(a);
        g_stale.insert(b);
        g_anyStale.store(true, std::memory_order_relaxed);
    }
}

namespace PairStance
{
    void Set(ObjectGuid a, ObjectGuid b, Stance stance)
    {
        if (a.IsEmpty() || b.IsEmpty() || a == b)
            return;

        if (stance == Stance::None)
        {
            Clear(a, b);
            return;
        }

        PairKey const key = KeyOf(a, b);
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            auto itr = g_stances.find(key);
            if (itr != g_stances.end() && itr->second == stance)
                return;

            g_stances[key] = stance;
            g_any.store(true, std::memory_order_relaxed);
        }

        MarkStale(key.first, key.second);
    }

    void Clear(ObjectGuid a, ObjectGuid b)
    {
        if (a.IsEmpty() || b.IsEmpty())
            return;

        PairKey const key = KeyOf(a, b);
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            if (!g_stances.erase(key))
                return;

            g_any.store(!g_stances.empty(), std::memory_order_relaxed);
        }

        MarkStale(key.first, key.second);
    }

    void ClearAllFor(ObjectGuid who)
    {
        if (who.IsEmpty() || !Any())
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
                    itr = g_stances.erase(itr);
                }
                else
                    ++itr;
            }

            g_any.store(!g_stances.empty(), std::memory_order_relaxed);
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
        return itr != g_stances.end() ? itr->second : Stance::None;
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

        return Get(first->GetGUID(), second->GetGUID());
    }

    bool Any()
    {
        return g_any.load(std::memory_order_relaxed);
    }

    bool TakeStaleViews(std::vector<ObjectGuid>& out)
    {
        if (!g_anyStale.load(std::memory_order_relaxed))
            return false;

        std::lock_guard<std::mutex> guard(g_staleLock);
        for (uint64 raw : g_stale)
            out.push_back(ObjectGuid(raw));
        g_stale.clear();
        g_anyStale.store(false, std::memory_order_relaxed);
        return !out.empty();
    }
}
