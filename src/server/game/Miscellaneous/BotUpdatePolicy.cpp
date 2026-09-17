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

#include "BotUpdatePolicy.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>

namespace
{
    // Written on the world thread when the config loads, read from every map
    // update thread.
    std::atomic<bool> s_skipClientPackets{ false };
    std::atomic<float> s_gridActivationRange{ 0.0f };

    // Virtual = constructed without a socket: managed bots, transient clones and
    // their offline sources. A person whose client dropped keeps a session that
    // once had a socket, so they are never counted here.
    bool HasNoClient(Player const* player)
    {
        WorldSession const* session = player ? player->GetSession() : nullptr;
        return session && session->IsVirtualSession();
    }
}

namespace BotUpdatePolicy
{
    void LoadConfig()
    {
        bool const skipPackets = sConfigMgr->GetBoolDefault("Centurion.Bots.SkipClientPackets", false);
        float const activationRange = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Bots.GridActivationRange", 0.0f));

        s_skipClientPackets.store(skipPackets, std::memory_order_relaxed);
        s_gridActivationRange.store(activationRange, std::memory_order_relaxed);

        TC_LOG_INFO("server.loading", "Bot update policy: skip client packets {}, continent grid activation range {}.",
            skipPackets ? "on" : "off", activationRange > 0.0f ? std::to_string(int32(activationRange)) + " yards" : "off");
    }

    bool SkipsClientPackets(Player const* receiver)
    {
        return s_skipClientPackets.load(std::memory_order_relaxed) && HasNoClient(receiver);
    }

    float GetReducedGridActivationRange(Player const* player)
    {
        float const range = s_gridActivationRange.load(std::memory_order_relaxed);
        if (range <= 0.0f || !HasNoClient(player) || !player->IsInWorld())
            return 0.0f;

        // Continents only. Instances, battlegrounds and arenas are small, run on
        // their own update and hold content that fights bots (Violet Hold waves);
        // lobby sub-maps host custom games. All of those keep the full range.
        Map const* map = player->GetMap();
        if (map->Instanceable() || map->IsServerOnlyWorldSubMap())
            return 0.0f;

        return range < map->GetVisibilityRange() ? range : 0.0f;
    }
}
