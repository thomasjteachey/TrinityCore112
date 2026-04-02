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
#include <unordered_map>

namespace
{
using CadenceClock = std::chrono::steady_clock;
using TimePoint = CadenceClock::time_point;

constexpr std::chrono::milliseconds LifecycleCadenceInterval(2000);

bool g_ManagerHooksRegistered = false;
std::unordered_map<uint64, TimePoint> g_NextLifecycleProcessTimeByGuid;

bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

bool IsEligibleForLifecycleProcessing(Player const* player)
{
    return player && player->IsInWorld() && !player->IsBeingTeleported();
}

bool IsCadenceReady(Player* player)
{
    if (!player)
        return false;

    uint64 const playerGuid = player->GetGUID().GetRawValue();
    TimePoint const now = CadenceClock::now();
    TimePoint& nextProcessTime = g_NextLifecycleProcessTimeByGuid[playerGuid];
    if (nextProcessTime > now)
        return false;

    nextProcessTime = now + LifecycleCadenceInterval;
    return true;
}
}

namespace playerbot
{
void RandomBotParticipationLifecycle::RegisterManagerHooks()
{
    if (!IsLifecycleGateEnabled())
        return;

    g_ManagerHooksRegistered = true;
    g_NextLifecycleProcessTimeByGuid.clear();
}

void RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(Player* player)
{
    if (!g_ManagerHooksRegistered || !IsLifecycleGateEnabled() || !IsEligibleForLifecycleProcessing(player) || !IsCadenceReady(player))
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
