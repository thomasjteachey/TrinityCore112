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

#ifndef TRINITY_PLAYERBOT_PVP_CORE_H
#define TRINITY_PLAYERBOT_PVP_CORE_H

#include "Common.h"
#include "SharedDefines.h"

class Player;

namespace playerbot
{
struct PvpCoreConfig
{
    bool moduleEnabled = false;
    bool pvpCoreEnabled = false;
};

enum class BattlegroundState : uint8
{
    None = 0,
    Queueing,
    WaitingToStart,
    Active
};

struct PvpValues
{
    BattlegroundState battlegroundState = BattlegroundState::None;
    BattlegroundTypeId battlegroundTypeId = BATTLEGROUND_TYPE_NONE;
    bool inBattleground = false;
    bool inBattlegroundQueue = false;
    bool hasInvite = false;
};

enum class PvpTrigger : uint8
{
    InBattleground = 0,
    BgQueueing,
    BgWaiting,
    BgActive,
    BgInviteActive,
    InBattlegroundWithoutFlag,
    PlayerHasFlag,
    EnemyFlagCarrierNear,
    TeamFlagCarrierNear
};

class PvpCore
{
public:
    static void LoadConfig();
    static PvpCoreConfig const& GetConfig();

    static PvpValues CollectValues(Player const* player);
    static bool IsTriggerActive(PvpTrigger trigger, PvpValues const& values);

private:
    static bool IsInBattlegroundQueue(Player const* player);
    static BattlegroundState DetectBattlegroundState(Player const* player, bool inQueue);
};
}

#endif
