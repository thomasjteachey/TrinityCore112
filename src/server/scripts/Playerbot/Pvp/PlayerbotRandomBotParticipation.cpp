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

#include "Log.h"
#include "Player.h"

#include <atomic>
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

enum class LifecycleObservationReason : uint8
{
    GateDisabled = 0,
    CadenceThrottled,
    InvalidPlayerState,
    NoLifecycleHooksActive,
    BattlegroundLifecycleExecuted,
    ArenaLifecycleExecuted
};

struct LifecycleObservationCounters
{
    std::atomic<uint64> gateDisabled{ 0 };
    std::atomic<uint64> cadenceThrottled{ 0 };
    std::atomic<uint64> invalidPlayerState{ 0 };
    std::atomic<uint64> noLifecycleHooksActive{ 0 };
    std::atomic<uint64> battlegroundLifecycleExecuted{ 0 };
    std::atomic<uint64> arenaLifecycleExecuted{ 0 };
};

LifecycleObservationCounters g_LifecycleObservationCounters;

bool IsNoOp(playerbot::BattlegroundLifecycleContext const& context)
{
    return context.queueOperation == playerbot::QueueOperationType::None &&
        context.invitationResponse == playerbot::InvitationResponseType::None &&
        !context.shouldHandleInProgressStatus;
}

bool IsNoOp(playerbot::ArenaLifecycleContext const& context)
{
    return context.queueOperation == playerbot::QueueOperationType::None &&
        context.teamInteraction == playerbot::ArenaTeamInteractionType::None;
}

void ObserveLifecycleReason(LifecycleObservationReason reason, ObjectGuid const& guid)
{
    char const* reasonLabel = "unknown";
    uint64 total = 0;

    switch (reason)
    {
        case LifecycleObservationReason::GateDisabled:
            total = ++g_LifecycleObservationCounters.gateDisabled;
            reasonLabel = "gate-disabled";
            break;
        case LifecycleObservationReason::CadenceThrottled:
            total = ++g_LifecycleObservationCounters.cadenceThrottled;
            reasonLabel = "cadence-throttled";
            break;
        case LifecycleObservationReason::InvalidPlayerState:
            total = ++g_LifecycleObservationCounters.invalidPlayerState;
            reasonLabel = "invalid-player-state";
            break;
        case LifecycleObservationReason::NoLifecycleHooksActive:
            total = ++g_LifecycleObservationCounters.noLifecycleHooksActive;
            reasonLabel = "no-lifecycle-hooks-active";
            break;
        case LifecycleObservationReason::BattlegroundLifecycleExecuted:
            total = ++g_LifecycleObservationCounters.battlegroundLifecycleExecuted;
            reasonLabel = "battleground-lifecycle-executed";
            break;
        case LifecycleObservationReason::ArenaLifecycleExecuted:
            total = ++g_LifecycleObservationCounters.arenaLifecycleExecuted;
            reasonLabel = "arena-lifecycle-executed";
            break;
        default:
            break;
    }

    TC_LOG_DEBUG("playerbots.pvp.lifecycle", "Playerbot PvP lifecycle observation: reason={} guid={} count={}.",
        reasonLabel, guid.ToString(), total);
}

bool CanProcessPlayerLifecycle(Player const* player)
{
    if (!player)
    {
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, ObjectGuid::Empty);
        return false;
    }

    ObjectGuid const guid = player->GetGUID();

    if (!IsLifecycleGateEnabled())
    {
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        return false;
    }

    if (!player->IsInWorld() || player->IsBeingTeleported())
    {
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, guid);
        return false;
    }

    uint64 const playerGuid = guid.GetRawValue();
    LifecycleCadenceTimePoint const now = LifecycleCadenceClock::now();
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    LifecycleCadenceTimePoint& nextProcessTime = g_NextRandomBotLifecycleProcessTimeByGuid[playerGuid];
    if (nextProcessTime > now)
    {
        ObserveLifecycleReason(LifecycleObservationReason::CadenceThrottled, guid);
        return false;
    }

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

LifecycleObservationSnapshot RandomBotParticipationManager::GetLifecycleObservationSnapshot()
{
    LifecycleObservationSnapshot snapshot;
    snapshot.gateDisabled = g_LifecycleObservationCounters.gateDisabled.load(std::memory_order_relaxed);
    snapshot.cadenceThrottled = g_LifecycleObservationCounters.cadenceThrottled.load(std::memory_order_relaxed);
    snapshot.invalidPlayerState = g_LifecycleObservationCounters.invalidPlayerState.load(std::memory_order_relaxed);
    snapshot.noLifecycleHooksActive = g_LifecycleObservationCounters.noLifecycleHooksActive.load(std::memory_order_relaxed);
    snapshot.battlegroundLifecycleExecuted = g_LifecycleObservationCounters.battlegroundLifecycleExecuted.load(std::memory_order_relaxed);
    snapshot.arenaLifecycleExecuted = g_LifecycleObservationCounters.arenaLifecycleExecuted.load(std::memory_order_relaxed);
    return snapshot;
}

void RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(Player* player)
{
    if (!player)
    {
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, ObjectGuid::Empty);
        return;
    }

    ObjectGuid const guid = player->GetGUID();

    if (!IsLifecycleGateEnabled())
    {
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        return;
    }

    PvpValues const values = PvpCore::CollectValues(player);
    RandomBotParticipationHooks const hooks = PvpCore::BuildRandomBotParticipationHooks(player, values);
    if (!hooks.lifecycleEnabled)
    {
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        return;
    }

    if (!hooks.battlegroundParticipationHook && !hooks.arenaParticipationHook)
    {
        ObserveLifecycleReason(LifecycleObservationReason::NoLifecycleHooksActive, guid);
        return;
    }

    BattlegroundLifecycleContext const battlegroundContext = PvpCore::BuildBattlegroundLifecycleContext(player, values);
    ArenaLifecycleContext const arenaContext = PvpCore::BuildArenaLifecycleContext(player, values);
    if (IsNoOp(battlegroundContext) && IsNoOp(arenaContext))
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle dispatcher no-op with active hooks: guid={}, bgHook={}, arenaHook={}.",
            guid.ToString(), hooks.battlegroundParticipationHook ? 1 : 0, hooks.arenaParticipationHook ? 1 : 0);
        return;
    }

    bool const didExecuteBattleground = hooks.battlegroundParticipationHook &&
        BattlegroundLifecycleActions::Execute(player, battlegroundContext);
    bool const didExecuteArena = hooks.arenaParticipationHook &&
        ArenaLifecycleActions::Execute(player, arenaContext);

    if (didExecuteBattleground)
        ObserveLifecycleReason(LifecycleObservationReason::BattlegroundLifecycleExecuted, guid);

    if (didExecuteArena)
        ObserveLifecycleReason(LifecycleObservationReason::ArenaLifecycleExecuted, guid);

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP lifecycle dispatcher complete: guid={}, didExecuteBattleground={}, didExecuteArena={}.",
        guid.ToString(), didExecuteBattleground ? 1 : 0, didExecuteArena ? 1 : 0);
}

bool RandomBotParticipationLifecycle::ProcessBattlegroundLifecycleEntryPoint(Player* player, PvpValues const& values,
    RandomBotParticipationHooks const& hooks)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!hooks.lifecycleEnabled || !hooks.battlegroundParticipationHook)
        return false;

    BattlegroundLifecycleContext const battlegroundContext = PvpCore::BuildBattlegroundLifecycleContext(player, values);
    return BattlegroundLifecycleActions::Execute(player, battlegroundContext);
}

bool RandomBotParticipationLifecycle::ProcessArenaLifecycleEntryPoint(Player* player, PvpValues const& values,
    RandomBotParticipationHooks const& hooks)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!hooks.lifecycleEnabled || !hooks.arenaParticipationHook)
        return false;

    ArenaLifecycleContext const arenaContext = PvpCore::BuildArenaLifecycleContext(player, values);
    return ArenaLifecycleActions::Execute(player, arenaContext);
}
}
