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

#ifndef TRINITY_PLAYERBOT_SHARED_STATE_GUARD_H
#define TRINITY_PLAYERBOT_SHARED_STATE_GUARD_H

#include <mutex>

namespace playerbot
{
/*
 * Concurrency model for the PvP decision modules.
 *
 * Decision ticks are serialized per map instance (see
 * AcquireDecisionTickLock), so two bots on different battleground instances
 * evaluate concurrently. A single bot only ever runs on its own map's update
 * thread, so per-bot VALUES in the shared by-GUID containers are never
 * touched from two threads at once. The remaining hazard is structural: an
 * insert/erase/rehash on a shared container racing a lookup from another
 * map's thread. std::unordered_map/set guarantee element references stay
 * valid across rehash, so it is sufficient to serialize only the structural
 * operations themselves. Every access to a cross-map shared container must
 * therefore go through the helpers below (or hold a module-specific mutex
 * for the full access, as the older guarded containers do).
 */
inline std::mutex& SharedBotStateStructureLock()
{
    static std::mutex lock;
    return lock;
}

// map[key] under the structure lock. The returned reference stays valid for
// the rest of the tick: unordered containers never move elements.
template<typename Map, typename Key>
inline typename Map::mapped_type& LockedGetOrCreate(Map& map, Key const& key)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    return map[key];
}

// find() under the structure lock; nullptr when absent. The element pointer
// stays valid because only this bot's own map thread can erase this key.
template<typename Map, typename Key>
inline typename Map::mapped_type* LockedFind(Map& map, Key const& key)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    auto itr = map.find(key);
    return itr == map.end() ? nullptr : &itr->second;
}

template<typename Map, typename Key>
inline typename Map::mapped_type const* LockedFind(Map const& map, Key const& key)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    auto itr = map.find(key);
    return itr == map.end() ? nullptr : &itr->second;
}

template<typename Container, typename Key>
inline void LockedErase(Container& container, Key const& key)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    container.erase(key);
}

// Copies the value out; for small trivially-copied state reads.
template<typename Map, typename Key>
inline bool LockedGetCopy(Map const& map, Key const& key, typename Map::mapped_type& out)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    auto itr = map.find(key);
    if (itr == map.end())
        return false;

    out = itr->second;
    return true;
}

template<typename Map, typename Key>
inline void LockedSet(Map& map, Key const& key, typename Map::mapped_type value)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    map[key] = std::move(value);
}

template<typename Set, typename Key>
inline bool LockedContains(Set const& set, Key const& key)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    return set.find(key) != set.end();
}

template<typename Set, typename Key>
inline void LockedInsert(Set& set, Key const& key)
{
    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    set.insert(key);
}
}

#endif
