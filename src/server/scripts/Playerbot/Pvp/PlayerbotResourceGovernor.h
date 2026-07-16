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

#ifndef TRINITY_PLAYERBOT_RESOURCE_GOVERNOR_H
#define TRINITY_PLAYERBOT_RESOURCE_GOVERNOR_H

#include "Define.h"

namespace playerbot
{
enum class ResourcePressureLevel : uint8
{
    Normal = 0,
    Soft = 1,
    Hard = 2
};

struct ResourceGovernorSnapshot
{
    bool enabled = false;
    ResourcePressureLevel level = ResourcePressureLevel::Normal;
    uint32 averageWorldUpdateMs = 0;
    uint32 softUpdateTimeMs = 0;
    uint32 hardUpdateTimeMs = 0;
    uint32 softMatchBotCap = 0;
    uint32 maxTotalCustomMatchBots = 0;
};

/*
 * Watches the world update time and decides how much custom-match bot load
 * the machine can currently afford. Human-participant matches are never
 * throttled by this class; only bot fill for custom games and matches whose
 * humans are at most spectators are governed.
 */
class ResourceGovernor
{
public:
    static void LoadConfig();

    // World thread, once per World::Update with the world tick diff.
    static void NoteWorldUpdate(uint32 diffMs);

    static ResourcePressureLevel GetPressureLevel();

    // Gate for adding one more bot/clone to a custom-game roster.
    // currentMatchBots: bots already requested for this lobby's match.
    // totalActiveCustomMatchBots: bots across all currently running custom matches.
    static bool CanAddCustomMatchBot(uint32 currentMatchBots, uint32 totalActiveCustomMatchBots);

    // True when sustained hard pressure justifies ending one bot-only /
    // spectator-only custom match. Callers pick the victim, then report the
    // cull so the cooldown restarts.
    static bool ShouldCullNow();
    static void NoteCullExecuted();

    static ResourceGovernorSnapshot GetSnapshot();
};
}

#endif
