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

#ifndef TRINITY_PLAYERBOT_RANDOM_BOT_PARTICIPATION_H
#define TRINITY_PLAYERBOT_RANDOM_BOT_PARTICIPATION_H

#include "Define.h"

class Player;

namespace playerbot
{
struct PvpValues;
struct RandomBotParticipationHooks;

struct LifecycleObservationSnapshot
{
    uint64 gateDisabled = 0;
    uint64 cadenceThrottled = 0;
    uint64 invalidPlayerState = 0;
    uint64 noLifecycleHooksActive = 0;
    uint64 battlegroundLifecycleExecuted = 0;
    uint64 arenaLifecycleExecuted = 0;
};

struct RandomBotPopulationSnapshot
{
    bool configEnabled = false;
    bool runtimeEnabled = false;
    bool supportsLoginOrchestration = false;
    uint32 targetMin = 0;
    uint32 targetMax = 0;
    uint32 onlineRandomBots = 0;
    uint32 onlineAllianceRandomBots = 0;
    uint32 onlineHordeRandomBots = 0;
    uint32 offlinePoolSize = 0;
    uint64 rebalanceTicks = 0;
    uint64 loginAttempts = 0;
    uint64 loginSuccess = 0;
    uint64 logoutAttempts = 0;
    uint64 logoutSuccess = 0;
    uint64 skippedSafetyRealPlayers = 0;
    uint64 skippedNoCandidatePool = 0;
    uint64 skippedIntegrationGap = 0;
    uint64 lastRebalanceUnixTime = 0;
};

class RandomBotParticipationManager
{
public:
    static void ResetCadence();
    static void LoadPopulationConfig();
    static void OnStartupBootstrap();
    static void OnWorldUpdate(uint32 diffMs);
    static void OnPlayerLogout(Player const* player);
    static void ProcessPlayerLifecycle(Player* player);

    static void SetPopulationRuntimeEnabled(bool enabled);
    static bool IsPopulationRuntimeEnabled();
    static bool TriggerImmediateRebalance();

    static LifecycleObservationSnapshot GetLifecycleObservationSnapshot();
    static RandomBotPopulationSnapshot GetPopulationSnapshot();
};

class RandomBotParticipationLifecycle
{
public:
    // Lifecycle seam: dispatcher only, executes decisions from context.
    static void ProcessLifecycleEntryPoint(Player* player);

    // Seam entry points for random-bot population manager wiring.
    static bool ProcessBattlegroundLifecycleEntryPoint(Player* player, PvpValues const& values, RandomBotParticipationHooks const& hooks);
    static bool ProcessArenaLifecycleEntryPoint(Player* player, PvpValues const& values, RandomBotParticipationHooks const& hooks);
};
}

#endif
