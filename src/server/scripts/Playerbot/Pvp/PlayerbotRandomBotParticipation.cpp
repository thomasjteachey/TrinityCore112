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
#include "PlayerbotPvpClassActions.h"
#include "PlayerbotPvpLifecycleActions.h"

#include "AccountMgr.h"
#include "Battleground.h"
#include "Configuration/Config.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Opcodes.h"
#include "Player.h"
#include "Realm.h"
#include "StringConvert.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
bool IsCrowdControlledForLifecyclePause(Player const* player)
{
    if (!player)
        return false;

    constexpr uint32 ccMechanicMask =
        (1u << MECHANIC_CHARM) |
        (1u << MECHANIC_DISORIENTED) |
        (1u << MECHANIC_FEAR) |
        (1u << MECHANIC_SLEEP) |
        (1u << MECHANIC_STUN) |
        (1u << MECHANIC_FREEZE) |
        (1u << MECHANIC_POLYMORPH) |
        (1u << MECHANIC_BANISH) |
        (1u << MECHANIC_HORROR) |
        (1u << MECHANIC_SAPPED);

    return player->HasUnitState(UNIT_STATE_LOST_CONTROL) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->HasAuraWithMechanic(ccMechanicMask) ||
        player->IsPolymorphed();
}

void ClearActiveMovementForControlLoss(Player* player)
{
    if (!player)
        return;

    player->AttackStop();
    player->SetSelection(ObjectGuid::Empty);
    // Preserve server-owned confused movement (e.g. polymorph drift). Clearing
    // active movement while confused pins the unit in place.
    if (player->HasUnitState(UNIT_STATE_CONFUSED) || player->HasAuraType(SPELL_AURA_MOD_CONFUSE) || player->IsPolymorphed())
        return;

    if (MotionMaster* motionMaster = player->GetMotionMaster())
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
}

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
std::unordered_set<uint64> g_StartupRevivedManagedBotGuids;
std::mutex g_StartupReviveLock;
bool g_StartupRevivePending = false;

void EmitLifecycleGmDebug(Player const* player, std::string const& detail, uint32 throttleMs = 5000)
{
    (void)player;
    (void)detail;
    (void)throttleMs;
}

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

struct RandomBotPopulationConfig
{
    bool enabled = false;
    uint32 targetMin = 0;
    uint32 targetMax = 0;
    uint32 rebalanceIntervalMs = 15000;
    uint8 minLevel = 10;
    uint8 maxLevel = 80;
    uint8 allianceRatioPercent = 50;
    uint32 maxOnlineBotsPerAccount = 0;
    uint32 selectionHistorySize = 48;
    std::unordered_set<uint32> botAccountIds;
};

struct RandomBotPoolCandidate
{
    uint32 lowGuid = 0;
    uint32 account = 0;
    uint8 level = 1;
    uint8 race = 0;
};

struct RandomBotPopulationState
{
    RandomBotPopulationConfig config;
    bool runtimeEnabled = false;
    bool startupBootstrapDone = false;
    bool rebalanceRequested = false;
    uint32 rebalanceTimerMs = 0;
    uint64 rebalanceEpoch = 0;
    uint64 rebalanceTicks = 0;
    uint64 loginAttempts = 0;
    uint64 loginSuccess = 0;
    uint64 logoutAttempts = 0;
    uint64 logoutSuccess = 0;
    uint64 skippedSafetyRealPlayers = 0;
    uint64 skippedNoCandidatePool = 0;
    uint64 skippedIntegrationGap = 0;
    uint64 lastRebalanceUnixTime = 0;
    std::deque<uint32> recentSelectedLowGuids;
    std::unordered_set<uint32> recentSelectedLowGuidSet;
};

RandomBotPopulationState g_RandomPopulation;
std::mutex g_RandomPopulationLock;

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

    (void)reasonLabel;
    (void)guid;
    (void)total;
}

void LogLifecycleBranchSummary(ObjectGuid const& guid, char const* branchLabel)
{
    (void)guid;
    (void)branchLabel;
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
        EmitLifecycleGmDebug(player, "can-process=no gate-disabled");
        return false;
    }

    if (!playerbot::IsManagedRandomBot(player))
    {
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        EmitLifecycleGmDebug(player, "can-process=no unmanaged-bot");
        return false;
    }

    if (!player->IsInWorld())
    {
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, guid);
        EmitLifecycleGmDebug(player, "can-process=no not-in-world");
        return false;
    }

    if (player->IsBeingTeleported())
    {
        // Managed random bots can occasionally retain the generic teleport flag
        // after a battleground transition (especially when start countdowns are
        // skipped). If near/far teleport semaphores are clear and the bot is
        // already placed in a battleground map, continue lifecycle processing so
        // tactical movement does not deadlock at match start.
        bool const hasPendingTeleportAck = player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear();
        if (hasPendingTeleportAck || !player->InBattleground())
        {
            ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, guid);
            return false;
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot lifecycle pre-check tolerated stale teleport flag: guid={} battlegroundId={}.",
            guid.ToString(), player->GetBattlegroundId());
    }

    uint64 const playerGuid = guid.GetRawValue();
    LifecycleCadenceTimePoint const now = LifecycleCadenceClock::now();
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    LifecycleCadenceTimePoint& nextProcessTime = g_NextRandomBotLifecycleProcessTimeByGuid[playerGuid];

    bool const inActiveBattleground = player->InBattleground() &&
        player->GetBattleground() &&
        player->GetBattleground()->GetStatus() == STATUS_IN_PROGRESS;
    std::chrono::milliseconds const cadenceInterval = RandomBotLifecycleCadenceInterval;

    if (nextProcessTime > now)
    {
        ObserveLifecycleReason(LifecycleObservationReason::CadenceThrottled, guid);
        auto const waitRemainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextProcessTime - now).count();
        std::ostringstream cadenceDetail;
        cadenceDetail << "can-process=no cadence-throttled wait_ms=" << waitRemainingMs
                      << " cadence_ms=" << cadenceInterval.count()
                      << " bg_active=" << (inActiveBattleground ? 1 : 0)
                      << " model=bg-fasttick-v3";
        EmitLifecycleGmDebug(player, cadenceDetail.str());
        return false;
    }

    nextProcessTime = now + cadenceInterval;
    return true;
}

void ProcessActiveBattlegroundTacticalTick(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return;

    if (!playerbot::IsManagedRandomBot(player))
        return;

    if (!player->IsInWorld() || !player->InBattleground())
        return;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
        return;

    if (IsCrowdControlledForLifecyclePause(player))
    {
        ClearActiveMovementForControlLoss(player);
        EmitLifecycleGmDebug(player, "bg-fasttick paused crowd-controlled", 1000);
        return;
    }

    playerbot::PvpValues const values = playerbot::PvpCore::CollectValues(player);
    playerbot::BattlegroundTacticalContext const tacticalContext = playerbot::PvpCore::BuildBattlegroundTacticalContext(player, values);
    bool const didExecuteTactical = playerbot::BattlegroundTacticalActions::Execute(player, tacticalContext);

    playerbot::BattlegroundLifecycleContext inProgressContext;
    inProgressContext.lifecycleEnabled = true;
    inProgressContext.queueOperation = playerbot::QueueOperationType::None;
    inProgressContext.invitationResponse = playerbot::InvitationResponseType::None;
    inProgressContext.shouldHandleInProgressStatus = true;
    bool const didExecuteLifecycle = playerbot::BattlegroundLifecycleActions::Execute(player, inProgressContext);

    std::ostringstream tickDetail;
    uint32 const bgTeam = player->GetBGTeam();
    uint32 const assignedTeam = battleground->GetPlayerTeam(player->GetGUID());
    tickDetail << "bg-fasttick tactical=" << (didExecuteTactical ? 1 : 0)
               << " lifecycle=" << (didExecuteLifecycle ? 1 : 0)
               << " alive=" << (player->IsAlive() ? 1 : 0)
               << " rooted=" << (player->HasUnitState(UNIT_STATE_ROOT) ? 1 : 0)
               << " stunned=" << (player->HasUnitState(UNIT_STATE_STUNNED) ? 1 : 0)
               << " bgTeam=" << bgTeam
               << " assignedTeam=" << assignedTeam
               << " x=" << int32(player->GetPositionX())
               << " y=" << int32(player->GetPositionY());
    EmitLifecycleGmDebug(player, tickDetail.str(), 1500);
}

void TryFinalizePendingVirtualBotTeleport(Player* player)
{
    if (!player || !playerbot::IsManagedRandomBot(player))
        return;

    WorldSession* session = player->GetSession();
    if (!session || !session->IsVirtualSession())
        return;

    if (player->IsBeingTeleportedFar())
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot lifecycle pre-check teleport finalization: guid={} type=far.",
            player->GetGUID().ToString());
        session->HandleMoveWorldportAck();
    }

    if (player->IsBeingTeleportedNear())
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot lifecycle pre-check teleport finalization: guid={} type=near.",
            player->GetGUID().ToString());
        WorldPacket teleportAck(MSG_MOVE_TELEPORT_ACK, 20);
        teleportAck << player->GetPackGUID();
        teleportAck << uint32(0);
        teleportAck << uint32(0);
        session->HandleMoveTeleportAck(teleportAck);

        if (player->IsBeingTeleportedNear())
        {
            uint32 const oldZone = player->GetZoneId();
            WorldLocation const& dest = player->GetTeleportDest();
            player->SetSemaphoreTeleportNear(false);
            player->UpdatePosition(dest, true);
            player->SetFallInformation(0, player->GetPositionZ());

            uint32 newZone = 0;
            uint32 newArea = 0;
            player->GetZoneAndAreaId(newZone, newArea);
            player->UpdateZone(newZone, newArea);

            if (oldZone != newZone)
            {
                if (player->pvpInfo.IsHostile)
                    player->CastSpell(player, 2479, true);
                else if (player->IsPvP() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
                    player->UpdatePvP(false, false);
            }

            player->ResummonPetTemporaryUnSummonedIfAny();
            player->ProcessDelayedOperations();
        }
    }
}

void TryReviveManagedBotAfterStartup(Player* player)
{
    if (!player || !playerbot::IsManagedRandomBot(player))
        return;

    bool shouldAttemptRevive = false;
    {
        std::lock_guard<std::mutex> startupReviveLock(g_StartupReviveLock);
        if (!g_StartupRevivePending)
            return;

        auto const [_, inserted] = g_StartupRevivedManagedBotGuids.emplace(player->GetGUID().GetRawValue());
        shouldAttemptRevive = inserted;
    }

    if (!shouldAttemptRevive || player->IsAlive())
        return;

    player->ResurrectPlayer(1.0f);
    TC_LOG_INFO("playerbots.population", "Startup managed bot revive applied: guid={}", player->GetGUID().ToString());
}

uint32 ResolvePlayerAccountId(Player const* player)
{
    if (!player)
        return 0;

    if (WorldSession const* session = player->GetSession())
        return session->GetAccountId();

    return sCharacterCache->GetCharacterAccountIdByGuid(player->GetGUID());
}

bool IsManagedRandomBotImpl(Player const* player, std::unordered_set<uint32> const& botAccounts)
{
    if (!player || botAccounts.empty())
        return false;

    uint32 const accountId = ResolvePlayerAccountId(player);
    if (!accountId || botAccounts.find(accountId) == botAccounts.end())
        return false;

    if (WorldSession const* session = player->GetSession())
    {
        // BotAccountIds is an explicit allow-list; treat listed accounts as
        // managed bots even when their virtual session is marked connected.
        return true;
    }

    return true;
}

std::unordered_set<uint32> GetManagedBotAccountIdsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    return g_RandomPopulation.config.botAccountIds;
}

bool IsRandomBotCandidate(Player const* player, std::unordered_set<uint32> const& botAccounts)
{
    return IsManagedRandomBotImpl(player, botAccounts);
}

struct OnlineRandomBotMetrics
{
    uint32 total = 0;
    uint32 alliance = 0;
    uint32 horde = 0;
    std::vector<ObjectGuid> guids;
};

OnlineRandomBotMetrics CollectOnlineRandomBotMetrics(std::unordered_set<uint32> const& botAccounts)
{
    OnlineRandomBotMetrics metrics;

    std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!IsRandomBotCandidate(player, botAccounts))
            continue;

        ++metrics.total;
        if (player->GetTeamId() == TEAM_ALLIANCE)
            ++metrics.alliance;
        else if (player->GetTeamId() == TEAM_HORDE)
            ++metrics.horde;

        metrics.guids.push_back(guid);
    }

    return metrics;
}

std::vector<RandomBotPoolCandidate> QueryOfflinePool(RandomBotPopulationConfig const& config)
{
    std::vector<RandomBotPoolCandidate> candidates;

    if (config.botAccountIds.empty())
        return candidates;

    std::string accountList;
    accountList.reserve(config.botAccountIds.size() * 6);
    for (uint32 accountId : config.botAccountIds)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    QueryResult result = CharacterDatabase.PQuery(
        "SELECT guid, account, level, race FROM characters "
        "WHERE online = 0 AND account IN ({}) AND level >= {} AND level <= {}",
        accountList, config.minLevel, config.maxLevel);

    if (!result)
        return candidates;

    do
    {
        Field* fields = result->Fetch();
        RandomBotPoolCandidate candidate;
        candidate.lowGuid = fields[0].GetUInt32();
        candidate.account = fields[1].GetUInt32();
        candidate.level = fields[2].GetUInt8();
        candidate.race = fields[3].GetUInt8();
        candidates.push_back(candidate);
    }
    while (result->NextRow());

    return candidates;
}

std::unordered_map<uint32, uint32> QueryOnlineBotCountsByAccount(RandomBotPopulationConfig const& config)
{
    std::unordered_map<uint32, uint32> onlineByAccount;
    if (config.botAccountIds.empty())
        return onlineByAccount;

    std::string accountList;
    accountList.reserve(config.botAccountIds.size() * 6);
    for (uint32 accountId : config.botAccountIds)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    QueryResult result = CharacterDatabase.PQuery(
        "SELECT account, COUNT(*) FROM characters WHERE online = 1 AND account IN ({}) GROUP BY account",
        accountList);
    if (!result)
        return onlineByAccount;

    do
    {
        Field* fields = result->Fetch();
        onlineByAccount[fields[0].GetUInt32()] = fields[1].GetUInt32();
    } while (result->NextRow());

    return onlineByAccount;
}

void TrackRecentSelection(RandomBotPopulationState& state, uint32 lowGuid)
{
    if (!lowGuid)
        return;

    uint32 const historyLimit = std::max<uint32>(state.config.selectionHistorySize, 1);

    if (state.recentSelectedLowGuidSet.find(lowGuid) == state.recentSelectedLowGuidSet.end())
    {
        state.recentSelectedLowGuids.push_back(lowGuid);
        state.recentSelectedLowGuidSet.insert(lowGuid);
    }

    while (state.recentSelectedLowGuids.size() > historyLimit)
    {
        uint32 const dropped = state.recentSelectedLowGuids.front();
        state.recentSelectedLowGuids.pop_front();
        state.recentSelectedLowGuidSet.erase(dropped);
    }
}

uint64 ComputeDeterministicCandidateScore(RandomBotPoolCandidate const& candidate, uint64 epoch, bool wasRecent)
{
    uint64 score = (static_cast<uint64>(candidate.lowGuid) * 2654435761ULL) ^ (epoch * 11400714819323198485ULL);
    score ^= static_cast<uint64>(candidate.account) << 17;
    score ^= static_cast<uint64>(candidate.level) << 9;
    if (wasRecent)
        score = std::numeric_limits<uint64>::max() - score;

    return score;
}

std::vector<RandomBotPoolCandidate> PickLoginCandidates(RandomBotPopulationState const& state,
    std::vector<RandomBotPoolCandidate> pool, uint32 requiredCount, uint32 currentAllianceOnline, uint32 currentHordeOnline,
    std::unordered_map<uint32, uint32> onlineByAccount)
{
    if (!requiredCount || pool.empty())
        return {};

    std::stable_sort(pool.begin(), pool.end(), [&](RandomBotPoolCandidate const& left, RandomBotPoolCandidate const& right)
    {
        bool const leftRecent = state.recentSelectedLowGuidSet.find(left.lowGuid) != state.recentSelectedLowGuidSet.end();
        bool const rightRecent = state.recentSelectedLowGuidSet.find(right.lowGuid) != state.recentSelectedLowGuidSet.end();
        uint64 const leftScore = ComputeDeterministicCandidateScore(left, state.rebalanceEpoch + 1, leftRecent);
        uint64 const rightScore = ComputeDeterministicCandidateScore(right, state.rebalanceEpoch + 1, rightRecent);
        return leftScore < rightScore;
    });

    std::vector<RandomBotPoolCandidate> selected;
    selected.reserve(std::min<uint32>(requiredCount, static_cast<uint32>(pool.size())));

    uint32 projectedAlliance = currentAllianceOnline;
    uint32 projectedHorde = currentHordeOnline;

    for (RandomBotPoolCandidate const& candidate : pool)
    {
        if (selected.size() >= requiredCount)
            break;

        TeamId const candidateTeam = Player::TeamForRace(candidate.race) == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE;
        uint32 const totalProjected = projectedAlliance + projectedHorde;
        uint32 const alliancePercent = totalProjected ? static_cast<uint32>((projectedAlliance * 100) / totalProjected) : 50;

        bool shouldTake = true;
        if (candidateTeam == TEAM_ALLIANCE && alliancePercent > state.config.allianceRatioPercent + 10)
            shouldTake = false;
        if (candidateTeam == TEAM_HORDE && alliancePercent + 10 < state.config.allianceRatioPercent)
            shouldTake = false;

        if (!shouldTake)
            continue;

        if (state.config.maxOnlineBotsPerAccount > 0)
        {
            uint32 const onlineOnAccount = onlineByAccount[candidate.account];
            if (onlineOnAccount >= state.config.maxOnlineBotsPerAccount)
                continue;
            onlineByAccount[candidate.account] = onlineOnAccount + 1;
        }

        selected.push_back(candidate);
        if (candidateTeam == TEAM_ALLIANCE)
            ++projectedAlliance;
        else
            ++projectedHorde;
    }

    for (RandomBotPoolCandidate const& candidate : pool)
    {
        if (selected.size() >= requiredCount)
            break;

        auto const exists = std::find_if(selected.begin(), selected.end(), [&](RandomBotPoolCandidate const& value)
        {
            return value.lowGuid == candidate.lowGuid;
        });

        if (exists == selected.end())
            selected.push_back(candidate);
    }

    return selected;
}

bool SupportsLoginOrchestration()
{
    return true;
}

bool TryLoginBotCharacter(RandomBotPoolCandidate const& candidate)
{
    if (!candidate.lowGuid || !candidate.account)
    {
        TC_LOG_ERROR("playerbots.population", "Random bot login skipped due to invalid candidate payload (guidLow={}, account={}).",
            candidate.lowGuid, candidate.account);
        return false;
    }

    uint32 const virtualSessionKey = 0xF0000000u | (candidate.lowGuid & 0x0FFFFFFFu);
    if (sWorld->FindSession(virtualSessionKey))
    {
        TC_LOG_WARN("playerbots.population", "Random bot login skipped: virtual session key {} already active (candidate guidLow={} account={}).",
            virtualSessionKey, candidate.lowGuid, candidate.account);
        return false;
    }

    std::string accountName;
    if (!sAccountMgr->GetName(candidate.account, accountName))
    {
        TC_LOG_ERROR("playerbots.population", "Random bot login failed: unable to resolve account name for account {} (candidate guidLow={}).",
            candidate.account, candidate.lowGuid);
        return false;
    }

    ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(candidate.lowGuid);
    int32 const realmId = static_cast<int32>(realm.Id.Realm);
    AccountTypes const security = static_cast<AccountTypes>(sAccountMgr->GetSecurity(candidate.account, realmId));
    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    WorldSession* session = new WorldSession(candidate.account, std::move(accountName), nullptr, security, expansion, 0, Minutes(0),
        LOCALE_enUS, 0, false);
    session->SetSessionMapKey(virtualSessionKey);
    sWorld->AddSession(session);
    session->AllowCharacterLogin(playerGuid);

    WorldPacket loginPacket(CMSG_PLAYER_LOGIN, 8);
    loginPacket << playerGuid;
    session->HandlePlayerLoginOpcode(loginPacket);

    Player* loggedInPlayer = session->GetPlayer();
    if (loggedInPlayer && loggedInPlayer->GetGUID() != playerGuid)
    {
        TC_LOG_WARN("playerbots.population",
            "Random bot login failed post-dispatch verification: guid={} guidLow={} account={} hasPlayer={} guidMismatch=1.",
            playerGuid.ToString(), candidate.lowGuid, candidate.account, loggedInPlayer ? 1 : 0,
            1);

        session->KickPlayer("Random bot login verification failed");
        return false;
    }

    if (loggedInPlayer)
    {
        TC_LOG_INFO("playerbots.population", "Random bot login successful: guid={} guidLow={} account={} level={}.",
            playerGuid.ToString(), candidate.lowGuid, candidate.account, candidate.level);
    }
    else
    {
        TC_LOG_INFO("playerbots.population",
            "Random bot login dispatched: guid={} guidLow={} account={} level={} (player materialization pending).",
            playerGuid.ToString(), candidate.lowGuid, candidate.account, candidate.level);
    }

    return true;
}

bool TryLogoutRandomBot(ObjectGuid const& guid)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player)
    {
        TC_LOG_WARN("playerbots.population", "Random bot logout skipped: guid {} is no longer online.", guid.ToString());
        return false;
    }

    if (!playerbot::IsManagedRandomBot(player))
    {
        TC_LOG_WARN("playerbots.population", "Random bot logout safety skip: guid {} account {} is not a managed random bot.",
            guid.ToString(), ResolvePlayerAccountId(player));
        return false;
    }

    WorldSession* session = player->GetSession();
    if (!session)
    {
        TC_LOG_WARN("playerbots.population", "Random bot logout skipped: managed bot guid {} account {} has no world session. TODO: add sessionless bot eviction adapter.",
            guid.ToString(), ResolvePlayerAccountId(player));
        return false;
    }

    TC_LOG_INFO("playerbots.population", "Random bot logout dispatched: guid={} account={}.", guid.ToString(), session->GetAccountId());
    session->LogoutPlayer(true);
    session->KickPlayer("Random bot population manager logout");
    return true;
}

bool RebalanceRandomPopulation(RandomBotPopulationState& state)
{
    state.rebalanceEpoch++;
    state.rebalanceTicks++;
    state.lastRebalanceUnixTime = static_cast<uint64>(GameTime::GetGameTime());

    OnlineRandomBotMetrics const online = CollectOnlineRandomBotMetrics(state.config.botAccountIds);

    uint32 target = state.config.targetMin;
    if (state.config.targetMax > state.config.targetMin)
    {
        uint64 const span = static_cast<uint64>(state.config.targetMax - state.config.targetMin + 1);
        target = state.config.targetMin + static_cast<uint32>(state.rebalanceEpoch % span);
    }

    TC_LOG_INFO("playerbots.population", "Random bot population rebalance tick={} online={} target={} range=[{}, {}] enabled={} runtime={}",
        state.rebalanceTicks, online.total, target, state.config.targetMin, state.config.targetMax,
        state.config.enabled ? 1 : 0, state.runtimeEnabled ? 1 : 0);

    if (online.total < target)
    {
        uint32 const needed = target - online.total;
        if (!SupportsLoginOrchestration())
        {
            state.skippedIntegrationGap += needed;
            TC_LOG_WARN("playerbots.population",
                "Random bot login orchestration is not supported in this build/runtime path; skipping {} planned login attempts.",
                needed);
            return false;
        }

        std::vector<RandomBotPoolCandidate> const pool = QueryOfflinePool(state.config);
        std::unordered_map<uint32, uint32> const onlineByAccount = QueryOnlineBotCountsByAccount(state.config);
        if (pool.empty())
        {
            state.skippedNoCandidatePool++;
            return false;
        }

        std::vector<RandomBotPoolCandidate> const selection = PickLoginCandidates(state, pool, needed, online.alliance, online.horde, onlineByAccount);
        for (RandomBotPoolCandidate const& candidate : selection)
        {
            state.loginAttempts++;
            TrackRecentSelection(state, candidate.lowGuid);
            if (TryLoginBotCharacter(candidate))
                state.loginSuccess++;
            else
                state.skippedIntegrationGap++;
        }

        return !selection.empty();
    }

    if (online.total > target)
    {
        uint32 const excess = online.total - target;
        uint32 processed = 0;
        for (ObjectGuid const& guid : online.guids)
        {
            if (processed >= excess)
                break;

            if (Player const* player = ObjectAccessor::FindConnectedPlayer(guid))
            {
                if (!IsManagedRandomBotImpl(player, state.config.botAccountIds))
                {
                    state.skippedSafetyRealPlayers++;
                    TC_LOG_WARN("playerbots.population", "Random bot logout safety skip: guid={} account={} is not a managed random bot.",
                        guid.ToString(), ResolvePlayerAccountId(player));
                    continue;
                }
            }

            state.logoutAttempts++;
            if (TryLogoutRandomBot(guid))
                state.logoutSuccess++;
            else
                state.skippedIntegrationGap++;

            ++processed;
        }

        return processed > 0;
    }

    return false;
}

void LoadPopulationConfigLocked(RandomBotPopulationState& state)
{
    RandomBotPopulationConfig config;
    config.enabled = sConfigMgr->GetBoolDefault("Playerbot.RandomPopulation.Enable", false);
    config.targetMin = std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.TargetMin", 0));
    config.targetMax = std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.TargetMax", 0));
    config.rebalanceIntervalMs = std::max<int32>(1000, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.RebalanceIntervalMs", 15000));
    config.minLevel = static_cast<uint8>(std::clamp<int32>(sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.MinLevel", 10), 1, 80));
    config.maxLevel = static_cast<uint8>(std::clamp<int32>(sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.MaxLevel", 80), 1, 80));
    config.allianceRatioPercent = static_cast<uint8>(std::clamp<int32>(sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.AllianceRatioPercent", 50), 0, 100));
    config.maxOnlineBotsPerAccount = std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.MaxOnlineBotsPerAccount", 0));
    config.selectionHistorySize = std::max<int32>(1, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.SelectionHistorySize", 48));

    if (config.targetMax < config.targetMin)
        std::swap(config.targetMin, config.targetMax);
    if (config.maxLevel < config.minLevel)
        std::swap(config.maxLevel, config.minLevel);

    std::string const rawBotAccounts = sConfigMgr->GetStringDefault("Playerbot.RandomPopulation.BotAccountIds", "");
    for (std::string_view token : Trinity::Tokenize(rawBotAccounts, ',', false))
        if (Optional<uint32> accountId = Trinity::StringTo<uint32>(token))
            if (*accountId > 0)
                config.botAccountIds.insert(*accountId);

    state.config = std::move(config);
    state.runtimeEnabled = state.config.enabled;
    state.rebalanceTimerMs = 0;
    state.rebalanceRequested = false;

    while (state.recentSelectedLowGuids.size() > state.config.selectionHistorySize)
    {
        uint32 const guid = state.recentSelectedLowGuids.front();
        state.recentSelectedLowGuids.pop_front();
        state.recentSelectedLowGuidSet.erase(guid);
    }

    TC_LOG_INFO("playerbots.population", "Random bot population config loaded: enabled={}, targetMin={}, targetMax={}, intervalMs={}, levelRange=[{}, {}], allianceRatio={}, maxPerAccount={}, accountPoolSize={}.",
        state.config.enabled ? 1 : 0, state.config.targetMin, state.config.targetMax, state.config.rebalanceIntervalMs,
        state.config.minLevel, state.config.maxLevel, state.config.allianceRatioPercent, state.config.maxOnlineBotsPerAccount, state.config.botAccountIds.size());
}
}

namespace playerbot
{
bool IsManagedRandomBot(Player const* player)
{
    return IsManagedRandomBotImpl(player, GetManagedBotAccountIdsSnapshot());
}

void RandomBotParticipationManager::ResetCadence()
{
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    g_NextRandomBotLifecycleProcessTimeByGuid.clear();

    std::lock_guard<std::mutex> startupReviveLock(g_StartupReviveLock);
    g_StartupRevivePending = false;
    g_StartupRevivedManagedBotGuids.clear();
}

void RandomBotParticipationManager::LoadPopulationConfig()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    LoadPopulationConfigLocked(g_RandomPopulation);
}

void RandomBotParticipationManager::OnStartupBootstrap()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    if (!g_RandomPopulation.startupBootstrapDone)
    {
        g_RandomPopulation.startupBootstrapDone = true;
        g_RandomPopulation.rebalanceRequested = true;

        std::lock_guard<std::mutex> startupReviveLock(g_StartupReviveLock);
        g_StartupRevivePending = true;
        g_StartupRevivedManagedBotGuids.clear();
    }
}

void RandomBotParticipationManager::OnWorldUpdate(uint32 diffMs)
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);

    if (!g_RandomPopulation.config.enabled || !g_RandomPopulation.runtimeEnabled)
        return;

    if (g_RandomPopulation.config.botAccountIds.empty())
    {
        g_RandomPopulation.skippedNoCandidatePool++;
        return;
    }

    if (g_RandomPopulation.rebalanceTimerMs < g_RandomPopulation.config.rebalanceIntervalMs)
        g_RandomPopulation.rebalanceTimerMs += diffMs;

    if (g_RandomPopulation.rebalanceTimerMs < g_RandomPopulation.config.rebalanceIntervalMs && !g_RandomPopulation.rebalanceRequested)
        return;

    g_RandomPopulation.rebalanceTimerMs = 0;
    g_RandomPopulation.rebalanceRequested = false;
    RebalanceRandomPopulation(g_RandomPopulation);
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
    TryReviveManagedBotAfterStartup(player);
    TryFinalizePendingVirtualBotTeleport(player);
    ProcessActiveBattlegroundTacticalTick(player);

    if (!CanProcessPlayerLifecycle(player))
        return;

    RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(player);
}

void RandomBotParticipationManager::SetPopulationRuntimeEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    g_RandomPopulation.runtimeEnabled = enabled;
    if (enabled)
        g_RandomPopulation.rebalanceRequested = true;
}

bool RandomBotParticipationManager::IsPopulationRuntimeEnabled()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    return g_RandomPopulation.runtimeEnabled;
}

bool RandomBotParticipationManager::TriggerImmediateRebalance()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    g_RandomPopulation.rebalanceRequested = true;
    g_RandomPopulation.rebalanceTimerMs = g_RandomPopulation.config.rebalanceIntervalMs;
    return RebalanceRandomPopulation(g_RandomPopulation);
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

RandomBotPopulationSnapshot RandomBotParticipationManager::GetPopulationSnapshot()
{
    RandomBotPopulationSnapshot snapshot;

    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    OnlineRandomBotMetrics const online = CollectOnlineRandomBotMetrics(g_RandomPopulation.config.botAccountIds);

    snapshot.configEnabled = g_RandomPopulation.config.enabled;
    snapshot.runtimeEnabled = g_RandomPopulation.runtimeEnabled;
    snapshot.supportsLoginOrchestration = SupportsLoginOrchestration();
    snapshot.targetMin = g_RandomPopulation.config.targetMin;
    snapshot.targetMax = g_RandomPopulation.config.targetMax;
    snapshot.onlineRandomBots = online.total;
    snapshot.onlineAllianceRandomBots = online.alliance;
    snapshot.onlineHordeRandomBots = online.horde;
    snapshot.offlinePoolSize = static_cast<uint32>(QueryOfflinePool(g_RandomPopulation.config).size());
    snapshot.maxOnlineBotsPerAccount = g_RandomPopulation.config.maxOnlineBotsPerAccount;
    snapshot.rebalanceTicks = g_RandomPopulation.rebalanceTicks;
    snapshot.loginAttempts = g_RandomPopulation.loginAttempts;
    snapshot.loginSuccess = g_RandomPopulation.loginSuccess;
    snapshot.logoutAttempts = g_RandomPopulation.logoutAttempts;
    snapshot.logoutSuccess = g_RandomPopulation.logoutSuccess;
    snapshot.skippedSafetyRealPlayers = g_RandomPopulation.skippedSafetyRealPlayers;
    snapshot.skippedNoCandidatePool = g_RandomPopulation.skippedNoCandidatePool;
    snapshot.skippedIntegrationGap = g_RandomPopulation.skippedIntegrationGap;
    snapshot.lastRebalanceUnixTime = g_RandomPopulation.lastRebalanceUnixTime;

    return snapshot;
}

void RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(Player* player)
{
    if (!player)
    {
        LogLifecycleBranchSummary(ObjectGuid::Empty, "invalid-player-state");
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, ObjectGuid::Empty);
        return;
    }

    ObjectGuid const guid = player->GetGUID();

    if (!IsLifecycleGateEnabled())
    {
        LogLifecycleBranchSummary(guid, "gate-disabled");
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        return;
    }

    if (IsCrowdControlledForLifecyclePause(player))
    {
        ClearActiveMovementForControlLoss(player);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle paused due to crowd control: guid={}.",
            guid.ToString());
        return;
    }

    PvpValues const values = PvpCore::CollectValues(player);
    BattlegroundTacticalContext const tacticalContext = PvpCore::BuildBattlegroundTacticalContext(player, values);
    bool const didExecuteTactical = BattlegroundTacticalActions::Execute(player, tacticalContext);
    bool const didExecuteDuelTactical = DuelTacticalActions::Execute(player);
    PvpClassSpellContext const classSpellContext = PvpCore::BuildClassSpellContext(player, values);
    bool const didExecuteClassSpell = PvpClassActions::Execute(player, classSpellContext);
    if (didExecuteClassSpell &&
        (classSpellContext.spellId == 16166 || // Elemental Mastery (off-GCD)
         classSpellContext.spellId == 17116)) // Nature's Swiftness (off-GCD)
    {
        std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
        g_NextRandomBotLifecycleProcessTimeByGuid[guid.GetRawValue()] = LifecycleCadenceClock::now();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP cadence bypass applied: guid={} spell={} reason=off-gcd-burst-window.",
            guid.ToString(), classSpellContext.spellId);
    }

    RandomBotParticipationHooks const hooks = PvpCore::BuildRandomBotParticipationHooks(player, values);
    if (!hooks.lifecycleEnabled)
    {
        LogLifecycleBranchSummary(guid, "hooks-lifecycle-disabled");
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle dispatcher complete: guid={}, didExecuteBattleground=0, didExecuteArena=0, didExecuteTactical={}, didExecuteClassSpell={}.",
            guid.ToString(), (didExecuteTactical || didExecuteDuelTactical) ? 1 : 0, didExecuteClassSpell ? 1 : 0);
        return;
    }

    if (!hooks.battlegroundParticipationHook && !hooks.arenaParticipationHook)
    {
        LogLifecycleBranchSummary(guid, "no-lifecycle-hooks-active");
        ObserveLifecycleReason(LifecycleObservationReason::NoLifecycleHooksActive, guid);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle dispatcher complete: guid={}, didExecuteBattleground=0, didExecuteArena=0, didExecuteTactical={}, didExecuteClassSpell={}.",
            guid.ToString(), (didExecuteTactical || didExecuteDuelTactical) ? 1 : 0, didExecuteClassSpell ? 1 : 0);
        return;
    }

    bool didExecuteBattleground = false;
    bool didExecuteArena = false;

    if (hooks.battlegroundParticipationHook)
    {
        BattlegroundLifecycleContext const battlegroundContext = PvpCore::BuildBattlegroundLifecycleContext(player, values);
        if (!IsNoOp(battlegroundContext))
            didExecuteBattleground = BattlegroundLifecycleActions::Execute(player, battlegroundContext);
    }

    // Re-sample lifecycle values after battleground execution so arena decisions
    // are based on current queue/invite state and do not operate on stale data.
    PvpValues const postBattlegroundValues = PvpCore::CollectValues(player);
    RandomBotParticipationHooks const postBattlegroundHooks = PvpCore::BuildRandomBotParticipationHooks(player, postBattlegroundValues);
    if (postBattlegroundHooks.arenaParticipationHook)
    {
        ArenaLifecycleContext const arenaContext = PvpCore::BuildArenaLifecycleContext(player, postBattlegroundValues);
        if (!IsNoOp(arenaContext))
            didExecuteArena = ArenaLifecycleActions::Execute(player, arenaContext);
    }

    if (!didExecuteBattleground && !didExecuteArena)
    {
        LogLifecycleBranchSummary(guid, "active-hooks-no-op-context");
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle dispatcher no-op with active hooks: guid={}, bgHook={}, arenaHook={}.",
            guid.ToString(), hooks.battlegroundParticipationHook ? 1 : 0, hooks.arenaParticipationHook ? 1 : 0);
    }

    if (didExecuteBattleground)
    {
        LogLifecycleBranchSummary(guid, "battleground-lifecycle-executed");
        ObserveLifecycleReason(LifecycleObservationReason::BattlegroundLifecycleExecuted, guid);
    }

    if (didExecuteArena)
    {
        LogLifecycleBranchSummary(guid, "arena-lifecycle-executed");
        ObserveLifecycleReason(LifecycleObservationReason::ArenaLifecycleExecuted, guid);
    }

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP lifecycle dispatcher complete: guid={}, didExecuteBattleground={}, didExecuteArena={}, didExecuteTactical={}, didExecuteClassSpell={}.",
        guid.ToString(), didExecuteBattleground ? 1 : 0, didExecuteArena ? 1 : 0,
        (didExecuteTactical || didExecuteDuelTactical) ? 1 : 0, didExecuteClassSpell ? 1 : 0);
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
