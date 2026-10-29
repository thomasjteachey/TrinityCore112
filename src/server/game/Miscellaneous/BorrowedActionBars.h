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

#ifndef TRINITYCORE_BORROWED_ACTION_BARS_H
#define TRINITYCORE_BORROWED_ACTION_BARS_H

// Characters whose client is shown someone else's action bars: a replay viewer
// spectating a replayed player (scripts/Custom/BGReplay.cpp) gets that player's
// buttons on their own default UI. The client answers a bars packet by clearing
// every button it holds no known spell for - and tells the server
// (CMSG_SET_ACTION_BUTTON, see reference_client_action_bar_packets) - so while
// the bars are borrowed, and for a few seconds after the real ones go back,
// WorldSession::HandleSetActionButtonOpcode must not touch the character's
// saved bars. Header only: the replay script and the handler share it.

#include "Define.h"
#include "GameTime.h"
#include "ObjectGuid.h"

#include <mutex>
#include <unordered_map>

namespace BorrowedActionBars
{
    uint32 constexpr KEEP_AFTER_MS = 3000;

    struct Registry
    {
        std::mutex Lock;
        // guid -> 0 while borrowed, else the game time (ms) to ignore until
        std::unordered_map<uint64, uint32> Until;
    };

    inline Registry& GetRegistry()
    {
        static Registry registry;
        return registry;
    }

    inline void SetBorrowed(ObjectGuid guid, bool borrowed)
    {
        Registry& registry = GetRegistry();
        std::lock_guard<std::mutex> lock(registry.Lock);
        if (borrowed)
            registry.Until[guid.GetRawValue()] = 0;
        else
            registry.Until[guid.GetRawValue()] = GameTime::GetGameTimeMS() + KEEP_AFTER_MS;
    }

    inline bool IgnoresClientButtons(ObjectGuid guid)
    {
        Registry& registry = GetRegistry();
        std::lock_guard<std::mutex> lock(registry.Lock);
        auto const itr = registry.Until.find(guid.GetRawValue());
        if (itr == registry.Until.end())
            return false;
        if (!itr->second || GameTime::GetGameTimeMS() < itr->second)
            return true;
        registry.Until.erase(itr);
        return false;
    }
}

#endif // TRINITYCORE_BORROWED_ACTION_BARS_H
