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

#include "PlayerbotRandomBotParticipation.h"

#include "PlayerbotPvpCore.h"
#include "PlayerbotPvpLifecycleActions.h"

#include "Player.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace
{
bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

using LifecycleCadenceClock = std::chrono::steady_clock;
using LifecycleCadenceTimePoint = LifecycleCadenceClock::time_point;

constexpr std::chrono::milliseconds RandomBotLifecycleCadenceInterval(2000);

std::unordered_map<uint64, LifecycleCadenceTimePoint> g_NextRandomBotLifecycleProcessTimeByGuid;
std::mutex g_RandomBotLifecycleCadenceLock;

bool CanProcessPlayerLifecycle(Player const* player)
{
    if (!player)
        return false;

    if (!IsLifecycleGateEnabled())
        return false;

    if (!player->IsInWorld() || player->IsBeingTeleported())
        return false;

    uint64 const playerGuid = player->GetGUID().GetRawValue();
    LifecycleCadenceTimePoint const now = LifecycleCadenceClock::now();
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    LifecycleCadenceTimePoint& nextProcessTime = g_NextRandomBotLifecycleProcessTimeByGuid[playerGuid];
    if (nextProcessTime > now)
        return false;

    nextProcessTime = now + RandomBotLifecycleCadenceInterval;
    return true;
}
}

namespace playerbot
{
void RandomBotParticipationManager::ResetCadence()
{
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    g_NextRandomBotLifecycleProcessTimeByGuid.clear();
}

void RandomBotParticipationManager::OnPlayerLogout(Player const* player)
{
    if (!player)
        return;

    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    g_NextRandomBotLifecycleProcessTimeByGuid.erase(player->GetGUID().GetRawValue());
}

void RandomBotParticipationManager::ProcessPlayerLifecycle(Player* player)
{
    if (!CanProcessPlayerLifecycle(player))
        return;

    RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(player);
}

void RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return;

    PvpValues const values = PvpCore::CollectValues(player);
    RandomBotParticipationHooks const hooks = PvpCore::BuildRandomBotParticipationHooks(player, values);
    if (!hooks.lifecycleEnabled)
        return;

    ProcessBattlegroundLifecycleEntryPoint(player, values, hooks);
    ProcessArenaLifecycleEntryPoint(player, values, hooks);
}

void RandomBotParticipationLifecycle::ProcessBattlegroundLifecycleEntryPoint(Player* player, PvpValues const& values,
    RandomBotParticipationHooks const& hooks)
{
    if (!player || !IsLifecycleGateEnabled())
        return;

    if (!hooks.lifecycleEnabled || !hooks.battlegroundParticipationHook)
        return;

    BattlegroundLifecycleContext const battlegroundContext = PvpCore::BuildBattlegroundLifecycleContext(player, values);
    BattlegroundLifecycleActions::Execute(player, battlegroundContext);
}

void RandomBotParticipationLifecycle::ProcessArenaLifecycleEntryPoint(Player* player, PvpValues const& values,
    RandomBotParticipationHooks const& hooks)
{
    if (!player || !IsLifecycleGateEnabled())
        return;

    if (!hooks.lifecycleEnabled || !hooks.arenaParticipationHook)
        return;

    ArenaLifecycleContext const arenaContext = PvpCore::BuildArenaLifecycleContext(player, values);
    ArenaLifecycleActions::Execute(player, arenaContext);
}
}
