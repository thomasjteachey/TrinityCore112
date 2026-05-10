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

#include "PlayerbotPvpLifecycleActions.h"
#include "PlayerbotPvpClassActions.h"
#include "PlayerbotRandomBotParticipation.h"
#include "SpellHistory.h"
#include "BattlegroundMgr.h"
#include "Battleground.h"
#include "BattlegroundQueue.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "Time/GameTime.h"
#include "GameObject.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "Chat.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "MovementTypedefs.h"
#include "Opcodes.h"
#include "ObjectAccessor.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Util.h"
#include "Containers.h"
#include "CommonHelpers.h"

#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <algorithm>
#include <queue>
#include <vector>
#include <list>

namespace playerbot
{
uint64 BuildBattlegroundInstanceKey(Battleground const* battleground);
Player* FindNearestEnemyBattlegroundPlayer(Player* player, float maxDistance, uint32* scannedPlayers = nullptr, uint32* attackableEnemies = nullptr);
bool EngageNearestEnemyPlayer(Player* player, float scanDistance);
}

namespace
{
std::unordered_map<uint64, uint32> g_HunterAutoShotPauseUntilMs;
std::unordered_map<uint64, uint32> g_BattlegroundNoHumanSinceMsByInstance;
constexpr uint32 PLAYERBOT_BG_NO_HUMAN_END_DELAY_MS = 45000;
constexpr uint32 PLAYERBOT_BG_WAIT_JOIN_NO_HUMAN_END_DELAY_MS = 15000;
constexpr uint32 PLAYERBOT_BG_OVERSTACK_MIN_DIFF = 2;
constexpr uint32 PLAYERBOT_BG_OVERSTACK_REQUEUE_COOLDOWN_MS = 30000;
constexpr uint32 PLAYERBOT_BG_OVERSTACK_INSTANCE_DEPARTURE_SPACING_MS = 8000;
constexpr uint32 PLAYERBOT_BG_OVERSTACK_INSTANCE_JITTER_WINDOW_MS = 14000;
constexpr uint32 PLAYERBOT_BG_HUMAN_INTEREST_REBALANCE_THROTTLE_MS = 5000;
constexpr uint32 PLAYERBOT_BG_SCM_REFILL_THROTTLE_MS = 3000;
constexpr uint32 PLAYERBOT_BG_QUEUE_REQUEUE_TIMEOUT_MS = 15000;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT = 29073;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK = 22734;
constexpr uint32 SPELL_WAITING_FOR_RESURRECT = 2584;
constexpr uint32 SPELL_DESERTER = 26013;
std::unordered_map<uint64, uint32> g_BattlegroundOverstackRequeueCooldownUntilMsByGuid;
std::unordered_map<uint64, uint32> g_BattlegroundOverstackInstanceNextDepartureMsByInstance;
std::unordered_map<uint64, uint32> g_BattlegroundQueuedNoInviteSinceMsByGuid;
uint32 g_LastHumanInterestPopulationRebalanceAttemptMs = 0;
uint32 g_LastScmSlotRefillAttemptMs = 0;
bool BattlegroundHasAnyRealHumanPlayers(Player const* player);
bool RemoveMatchingQueues(Player* player, bool arenaOnly, bool invitedOnly, bool scheduleNonArenaUpdate);

bool IsScmManagedBotCandidate(Player const* player)
{
    if (!player)
        return false;

    if (playerbot::IsManagedRandomBot(player))
        return true;

    WorldSession const* session = player->GetSession();
    return session && session->IsVirtualSession();
}

uint32 ComputeOverstackDepartureJitterMs(Player const* player, Battleground const* battleground)
{
    if (!player || !battleground)
        return 0;

    uint64 const mix = (player->GetGUID().GetRawValue() * 11400714819323198485ull) ^
        (static_cast<uint64>(battleground->GetInstanceID()) * 7046029254386353131ull);
    return static_cast<uint32>(mix % PLAYERBOT_BG_OVERSTACK_INSTANCE_JITTER_WINDOW_MS);
}

bool ShouldManagedBotLeaveForOverstack(Player* player, Battleground* battleground)
{
    if (!player || !battleground || !IsScmManagedBotCandidate(player))
        return false;

    uint32 const assignedTeam = battleground->GetPlayerTeam(player->GetGUID());
    if (assignedTeam != ALLIANCE && assignedTeam != HORDE)
        return false;

    uint32 const allianceCount = battleground->GetPlayersCountByTeam(ALLIANCE);
    uint32 const hordeCount = battleground->GetPlayersCountByTeam(HORDE);
    if (!allianceCount || !hordeCount)
        return false;

    // Never rebalance on a one-player gap. A departure on 9v8 would immediately
    // flip (or re-flip) stack pressure and cause oscillation between teams.
    uint32 const absoluteTeamDiff = (allianceCount > hordeCount) ? (allianceCount - hordeCount) : (hordeCount - allianceCount);
    if (absoluteTeamDiff <= 1)
        return false;

    uint32 const botTeamCount = assignedTeam == ALLIANCE ? allianceCount : hordeCount;
    uint32 const otherTeamCount = assignedTeam == ALLIANCE ? hordeCount : allianceCount;
    if (botTeamCount <= otherTeamCount)
        return false;

    uint32 const teamDiff = botTeamCount - otherTeamCount;
    if (teamDiff < PLAYERBOT_BG_OVERSTACK_MIN_DIFF)
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint64 const botGuidRaw = player->GetGUID().GetRawValue();
    uint32 const cooldownUntilMs = g_BattlegroundOverstackRequeueCooldownUntilMsByGuid[botGuidRaw];
    if (cooldownUntilMs && nowMs < cooldownUntilMs)
        return false;

    uint64 const instanceKey = playerbot::BuildBattlegroundInstanceKey(battleground);
    uint32 const nextDepartureEarliestMs = g_BattlegroundOverstackInstanceNextDepartureMsByInstance[instanceKey];
    if (nextDepartureEarliestMs && nowMs < nextDepartureEarliestMs)
        return false;

    uint32 const jitterMs = ComputeOverstackDepartureJitterMs(player, battleground);
    if (nowMs % PLAYERBOT_BG_OVERSTACK_INSTANCE_JITTER_WINDOW_MS < jitterMs)
        return false;

    g_BattlegroundOverstackRequeueCooldownUntilMsByGuid[botGuidRaw] = nowMs + PLAYERBOT_BG_OVERSTACK_REQUEUE_COOLDOWN_MS;
    g_BattlegroundOverstackInstanceNextDepartureMsByInstance[instanceKey] = nowMs + PLAYERBOT_BG_OVERSTACK_INSTANCE_DEPARTURE_SPACING_MS;

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP overstack rebalance trigger: guid={} bgTypeId={} instanceId={} assignedTeam={} teamCount={} otherTeamCount={} diff={}.",
        player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID(), assignedTeam, botTeamCount,
        otherTeamCount, teamDiff);
    return true;
}

bool HasQueuedRealHumanForBattleground(BattlegroundTypeId targetBgType)
{
    if (targetBgType == BATTLEGROUND_TYPE_NONE)
        return false;

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(targetBgType, 0);
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    for (auto const& [queuedGuid, queueInfo] : bgQueue.m_QueuedPlayers)
    {
        (void)queueInfo;
        Player* participant = ObjectAccessor::FindConnectedPlayer(queuedGuid);
        if (!participant)
            continue;

        WorldSession const* session = participant->GetSession();
        bool const isVirtualSession = session && session->IsVirtualSession();
        if (isVirtualSession || playerbot::IsManagedRandomBot(participant))
            continue;

        return true;
    }

    return false;
}

bool ShouldManagedBotLeaveForQueuedHuman(Player* player, Battleground* battleground)
{
    if (!player || !battleground || !IsScmManagedBotCandidate(player))
        return false;

    if (battleground->GetTypeID() != BATTLEGROUND_SCM)
        return false;

    uint32 const maxPlayers = battleground->GetMaxPlayers();
    if (!maxPlayers || battleground->GetPlayersSize() < maxPlayers)
        return false;

    return HasQueuedRealHumanForBattleground(battleground->GetTypeID());
}

bool TryRefillManagedScmSlots(Player* player, Battleground* battleground)
{
    if (!player || !battleground || battleground->GetTypeID() != BATTLEGROUND_SCM)
        return false;

    uint32 const maxPlayers = battleground->GetMaxPlayers();
    uint32 const playersInInstance = battleground->GetPlayersSize();
    if (!maxPlayers || playersInInstance >= maxPlayers)
        return false;

    // Only force-fill while real humans are participating in SCM.
    if (!BattlegroundHasAnyRealHumanPlayers(player))
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    if (nowMs < g_LastScmSlotRefillAttemptMs + PLAYERBOT_BG_SCM_REFILL_THROTTLE_MS)
        return false;

    g_LastScmSlotRefillAttemptMs = nowMs;

    bool const rebalanceTriggered = playerbot::RandomBotParticipationManager::TriggerImmediateRebalance();
    uint32 const queuedCount = playerbot::QueueEligibleManagedBotsForBattleground(BATTLEGROUND_SCM, 0);
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP SCM refill attempt: guid={} instanceId={} players={} maxPlayers={} rebalanceTriggered={} queuedCount={}.",
        player->GetGUID().ToString(), battleground->GetInstanceID(), playersInInstance, maxPlayers, rebalanceTriggered ? 1 : 0, queuedCount);

    return rebalanceTriggered || queuedCount > 0;
}

void ForcePlayerbotDismount(Player* player)
{
    if (!player)
        return;

    if (player->IsMounted())
        player->Dismount();

    player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    player->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
    player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS);
    player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK);
    player->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_FLIGHT_SPEED_ALWAYS);
    player->RemoveAurasByType(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK);

    player->UpdateSpeed(MOVE_RUN);
    player->UpdateSpeed(MOVE_SWIM);
    player->UpdateSpeed(MOVE_FLIGHT);
}

bool IsEffectivelyOutdoors(Player const* player)
{
    if (!player)
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return player->IsOutdoors();

    PositionFullTerrainStatus terrainStatus;
    map->GetFullTerrainStatusForPosition(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        terrainStatus, MAP_ALL_LIQUIDS, player->GetCollisionHeight());
    return player->IsOutdoors() || terrainStatus.outdoors;
}

bool ShouldForceIndoorDismount(Player const* player, bool outdoors, uint32 lingerMs = 1500)
{
    if (!player)
        return false;

    static std::unordered_map<uint64, uint32> indoorSinceMsByGuid;
    uint64 const guid = player->GetGUID().GetRawValue();

    if (outdoors)
    {
        indoorSinceMsByGuid.erase(guid);
        return false;
    }

    uint32 const nowMs = GameTime::GetGameTimeMS();
    auto itr = indoorSinceMsByGuid.find(guid);
    if (itr == indoorSinceMsByGuid.end())
    {
        indoorSinceMsByGuid.emplace(guid, nowMs);
        return false;
    }

    return nowMs >= itr->second + lingerMs;
}

bool IsRecoveringByEatingOrDrinking(Player const* player)
{
    if (!player || !player->IsAlive() || player->IsInCombat())
        return false;

    bool const needsFood = player->GetHealthPct() < 100.0f && player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
    bool const needsDrink = player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 100.0f &&
        player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
    return needsFood || needsDrink;
}

void ClearEatDrinkAurasForMovement(Player* player)
{
    if (!player)
        return;

    if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
        player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
    if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
        player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
}

void ClearStaleWaitingForResurrectAura(Player* player)
{
    if (!player || !player->IsAlive())
        return;

    if (player->HasAura(SPELL_WAITING_FOR_RESURRECT))
        player->RemoveAurasDueToSpell(SPELL_WAITING_FOR_RESURRECT);
}


bool IsWarsongGulch(Player const* player)
{
    if (!player)
        return false;

    Battleground const* battleground = player->GetBattleground();
    return battleground && battleground->GetMapId() == 489;
}

TeamId ResolveTeamId(uint32 teamValue)
{
    if (teamValue == TEAM_ALLIANCE || teamValue == ALLIANCE)
        return TEAM_ALLIANCE;
    if (teamValue == TEAM_HORDE || teamValue == HORDE)
        return TEAM_HORDE;
    return TEAM_NEUTRAL;
}

TeamId ResolveBotTeamId(Player const* player)
{
    if (!player)
        return TEAM_NEUTRAL;

    if (Battleground const* battleground = player->GetBattleground())
    {
        uint32 const assignedTeam = battleground->GetPlayerTeam(player->GetGUID());
        if (assignedTeam)
        {
            TeamId const resolved = ResolveTeamId(assignedTeam);
            if (resolved != TEAM_NEUTRAL)
                return resolved;
        }
    }

    if (uint32 const bgTeam = player->GetBGTeam())
    {
        TeamId const resolved = ResolveTeamId(bgTeam);
        if (resolved != TEAM_NEUTRAL)
            return resolved;
    }

    return ResolveTeamId(player->GetTeam());
}

bool MoveTowardUnit(Player* player, Unit* target, float desiredDistance);
float GetAggressiveCombatScanDistance(Player const* player, float fallbackDistance);
bool CanIssueBotMovement(Player* player);
bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold = 6.0f, uint32 minReissueMs = 2000);
Position BuildFollowDestination(Player* player, Unit* target, float desiredDistance);
bool IssueHumanLikeFollow(Player* player, Unit* target, float desiredDistance, float destinationChangeThreshold = 6.0f, uint32 minReissueMs = 2000);
void EmitBattlegroundGmDebug(Player* bot, std::string const& detail, uint32 throttleMs);

bool IsCrowdControlledForAction(Player const* player)
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

    bool const hasLostControlState = player->HasUnitState(UNIT_STATE_LOST_CONTROL);
    bool const hasHardCcState = player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING);
    bool const hasCcAura = player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->HasAuraWithMechanic(ccMechanicMask) ||
        player->IsPolymorphed();

    return hasLostControlState || hasHardCcState || hasCcAura;
}

bool TryPursueNearestEnemyInBattleground(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    float const combatEngageDistance = std::clamp(GetAggressiveCombatScanDistance(player, 100.0f), 25.0f, 60.0f);
    if (playerbot::EngageNearestEnemyPlayer(player, combatEngageDistance))
        return true;

    Player* nearestEnemy = playerbot::FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), nullptr, nullptr);
    if (!nearestEnemy)
        return false;

    bool chaseIssued = MoveTowardUnit(player, nearestEnemy, combatEngageDistance);
    if (!chaseIssued && !player->isMoving())
    {
        // If direct pursuit did not issue movement, clear stale motion so a new
        // navmesh-segmented path request can be accepted. The MovePoint helper
        // still validates battleground navigation and refuses no-path segments.
        player->GetMotionMaster()->Clear();
        chaseIssued = IssueMovePointThrottled(player, nearestEnemy->GetPosition(), 30.0f, 700) || player->isMoving();
    }

    return chaseIssued;
}

bool RecoverStaleBattlegroundState(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    if (player->GetBattleground())
        return false;

    // Ignore in-flight teleports; let normal worldport handling finish first.
    if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
        return false;

    uint32 const staleBattlegroundId = player->GetBattlegroundId();
    BattlegroundTypeId const staleBattlegroundTypeId = player->GetBattlegroundTypeId();

    player->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE);
    player->SetBGTeam(0);

    bool const teleported = player->TeleportToBGEntryPoint();
    TC_LOG_INFO("playerbots.pvp.lifecycle",
        "Playerbot PvP stale battleground recovery: guid={} staleInstanceId={} staleTypeId={} teleported={}.",
        player->GetGUID().ToString(), staleBattlegroundId, uint32(staleBattlegroundTypeId), teleported ? 1 : 0);

    return true;
}

bool MoveToClosestBattlegroundGraveyard(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    Battleground* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    if (WorldSafeLocsEntry const* graveyard = battleground->GetClosestGraveyard(player))
    {
        Position destination(graveyard->Loc.X, graveyard->Loc.Y, graveyard->Loc.Z, player->GetOrientation());
        if (!player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
            IssueMovePointThrottled(player, destination, 6.0f, 2000);
        return true;
    }

    return false;
}

bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

std::array<BattlegroundTypeId, 6> BuildRandomBattlegroundOrder()
{
    std::array<BattlegroundTypeId, 6> battlegroundTypes =
    {
        BATTLEGROUND_AV,
        BATTLEGROUND_EY,
        BATTLEGROUND_AB,
        BATTLEGROUND_WS,
        BATTLEGROUND_SA,
        BATTLEGROUND_IC
    };

    Trinity::Containers::RandomShuffle(battlegroundTypes);
    return battlegroundTypes;
}


std::string BuildQueueDebugSummary(Player* player)
{
    if (!player)
        return "queue=none";

    std::ostringstream summary;
    summary << "queue_slots=[";
    bool first = true;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const queueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        if (!first)
            summary << ',';
        first = false;
        summary << uint32(queueTypeId) << ":inv=" << (player->IsInvitedForBattlegroundQueueType(queueTypeId) ? 1 : 0);
    }

    if (first)
        summary << "none";

    summary << "] invitedArenaTeamId=" << player->GetArenaTeamIdInvited();
    return summary.str();
}

void EmitLifecycleDiagnostic(Player* player, char const* phase, std::string const& detail)
{
    if (!player)
        return;

    TC_LOG_INFO("playerbots.pvp.lifecycle",
        "Playerbot lifecycle diagnostic: guid={} phase={} inBg={} bgId={} inQueue={} deserter={} {} detail={}",
        player->GetGUID().ToString(), phase ? phase : "none", player->InBattleground() ? 1 : 0, player->GetBattlegroundId(),
        player->InBattlegroundQueue() ? 1 : 0, player->HasAura(SPELL_DESERTER) ? 1 : 0, BuildQueueDebugSummary(player), detail);
}

void EmitBattlegroundGmDebug(Player* bot, std::string const& detail, uint32 throttleMs = 3000)
{
    (void)bot;
    (void)detail;
    (void)throttleMs;
}

void ClearMovementBeforeBattlegroundTeleport(Player* player)
{
    if (!player)
        return;

    player->AttackStop();
    player->SetSelection(ObjectGuid::Empty);

    if (player->isMoving())
        player->StopMoving();

    if (MotionMaster* motionMaster = player->GetMotionMaster())
        motionMaster->Clear();

    // The teleport/visibility path must not inherit stale client movement
    // flags from whatever the bot was doing before the queue invite. A stale
    // spline flag with an interrupted/finalized MoveSpline can make object
    // create packets assert while HandleMoveWorldportAck() rebuilds visibility.
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_SPLINE_ENABLED);
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION);
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING);
}


bool IsResolvingBattlegroundGravityFall(Player const* player)
{
    return player &&
        player->InBattleground() &&
        player->IsFalling() &&
        !player->IsFlying() &&
        !player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
}

bool FinishCompletedBattlegroundGravityFall(Player* player)
{
    if (!IsResolvingBattlegroundGravityFall(player))
        return false;

    if (player->movespline && !player->movespline->Finalized())
        return false;

    float groundZ = player->GetMapHeight(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), true, MAX_FALL_DISTANCE);
    if (groundZ <= INVALID_HEIGHT || std::fabs(player->GetPositionZ() - groundZ) > 1.0f)
        return false;

    player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING);
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_SPLINE_ENABLED);
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_FORWARD);
    player->SetFallInformation(0, player->GetPositionZ());
    return true;
}

float GetHumanLikeFallHorizontalSpeed(Player const* player)
{
    if (!player)
        return 0.0f;

    if (player->movespline && !player->movespline->Finalized() && player->movespline->Velocity() > 0.0f)
        return player->movespline->Velocity();

    if (player->HasUnitMovementFlag(MOVEMENTFLAG_WALKING))
        return player->GetSpeed(MOVE_WALK);

    if (player->HasUnitMovementFlag(MOVEMENTFLAG_BACKWARD))
        return player->GetSpeed(MOVE_RUN_BACK);

    return player->GetSpeed(MOVE_RUN);
}

bool CanIssueMovementCommand(Player const* player, uint32 cooldownMs = 500)
{
    if (!player)
        return false;

    static std::unordered_map<uint64, uint32> nextAllowedMoveCommandMsByGuid;
    uint64 const botGuid = player->GetGUID().GetRawValue();
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32& nextAllowedMs = nextAllowedMoveCommandMsByGuid[botGuid];
    if (nowMs < nextAllowedMs)
        return false;

    nextAllowedMs = nowMs + cooldownMs;
    return true;
}

Position BuildCollisionSafeDestination(Player const* player, Position const& destination)
{
    if (!player)
        return destination;

    Position adjustedDestination = destination;
    float adjustedZ = adjustedDestination.GetPositionZ();
    player->UpdateAllowedPositionZ(adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(), adjustedZ);
    adjustedDestination.Relocate(adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(), adjustedZ, adjustedDestination.GetOrientation());
    return adjustedDestination;
}

Position BuildFollowDestination(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target)
        return Position();

    float x = target->GetPositionX();
    float y = target->GetPositionY();
    float z = target->GetPositionZ();
    float const followDistance = std::max(0.5f, desiredDistance);

    target->GetNearPoint(player, x, y, z, followDistance, target->GetAbsoluteAngle(player));
    Position destination(x, y, z, player->GetOrientation());
    return BuildCollisionSafeDestination(player, destination);
}

bool IsForbiddenBattlegroundPathType(PathType pathType)
{
    uint32 const forbiddenPathFlags = PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH;
    return (pathType & forbiddenPathFlags) != 0;
}

constexpr float PLAYERBOT_BG_PATH_CALCULATION_LENGTH_LIMIT = 2400.0f;
constexpr float PLAYERBOT_BG_MOVEMENT_SEGMENT_DISTANCE = 80.0f;
constexpr float PLAYERBOT_BG_MIN_FALL_SHORTCUT_DROP = 6.0f;
constexpr float PLAYERBOT_BG_FALL_SHORTCUT_SPEED_TOLERANCE = 0.75f;
constexpr uint32 PLAYERBOT_BG_FALL_SHORTCUT_COOLDOWN_MS = 4000;

bool BuildNavPathSegmentDestination(Player const* player, Movement::PointsArray const& points, float orientation, Position& segmentDestination)
{
    if (!player || points.size() < 2)
        return false;

    G3D::Vector3 previous(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    float traversedDistance = 0.0f;

    for (std::size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const& point = points[i];
        G3D::Vector3 const delta = point - previous;
        float const segmentLength = delta.length();
        if (segmentLength <= 0.01f)
        {
            previous = point;
            continue;
        }

        if (traversedDistance + segmentLength >= PLAYERBOT_BG_MOVEMENT_SEGMENT_DISTANCE)
        {
            float const fraction = (PLAYERBOT_BG_MOVEMENT_SEGMENT_DISTANCE - traversedDistance) / segmentLength;
            G3D::Vector3 const selected = previous + delta * fraction;
            segmentDestination.Relocate(selected.x, selected.y, selected.z, orientation);
            return true;
        }

        traversedDistance += segmentLength;
        previous = point;
    }

    G3D::Vector3 const& finalPoint = points.back();
    segmentDestination.Relocate(finalPoint.x, finalPoint.y, finalPoint.z, orientation);
    return player->GetDistance(segmentDestination) > 0.5f;
}

bool TryBuildBattlegroundFallShortcutDestination(Player* player, Position const& destination, Position& fallDestination)
{
    if (!player || !player->InBattleground() || player->IsFlying() || player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING))
        return false;

    if (player->IsFalling())
        return false;

    static std::unordered_map<uint64, uint32> nextAllowedFallShortcutMsByGuid;
    uint64 const botGuid = player->GetGUID().GetRawValue();
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const nextAllowedMs = nextAllowedFallShortcutMsByGuid[botGuid];
    if (nowMs < nextAllowedMs)
        return false;

    float const dx = destination.GetPositionX() - player->GetPositionX();
    float const dy = destination.GetPositionY() - player->GetPositionY();
    float const planarDistance = std::sqrt(dx * dx + dy * dy);
    if (planarDistance < 2.0f)
        return false;

    float const horizontalSpeed = GetHumanLikeFallHorizontalSpeed(player);
    if (horizontalSpeed <= 0.0f)
        return false;

    std::array<float, 5> const edgeProbeDistances =
    {
        3.0f,
        5.0f,
        8.0f,
        12.0f,
        16.0f
    };

    for (float edgeProbeDistance : edgeProbeDistances)
    {
        float const edgeDistance = std::min(planarDistance, edgeProbeDistance);
        if (edgeDistance < 1.0f)
            continue;

        float const edgeFraction = edgeDistance / planarDistance;
        float const edgeX = player->GetPositionX() + dx * edgeFraction;
        float const edgeY = player->GetPositionY() + dy * edgeFraction;
        float edgeZ = player->GetPositionZ();
        player->UpdateAllowedPositionZ(edgeX, edgeY, edgeZ);

        float const edgeDrop = player->GetPositionZ() - edgeZ;
        if (edgeDrop < PLAYERBOT_BG_MIN_FALL_SHORTCUT_DROP)
            continue;

        // Step only far enough over the ledge to prove there is a drop, then
        // project the rest of the fall using the bot's current horizontal speed.
        // Do not keep probing toward the tactical destination: that turns a
        // graveyard cliff drop into a long, destination-directed glide.
        float const edgeFallTimeSeconds = Movement::computeFallTime(edgeDrop, false);
        float const naturalHorizontalDistance = edgeDistance + horizontalSpeed * edgeFallTimeSeconds;
        float const landingDistance = std::min(planarDistance, naturalHorizontalDistance);
        float const landingFraction = landingDistance / planarDistance;
        float const landingX = player->GetPositionX() + dx * landingFraction;
        float const landingY = player->GetPositionY() + dy * landingFraction;
        float landingZ = player->GetPositionZ();
        player->UpdateAllowedPositionZ(landingX, landingY, landingZ);

        float const drop = player->GetPositionZ() - landingZ;
        if (drop < PLAYERBOT_BG_MIN_FALL_SHORTCUT_DROP)
            continue;

        float const fallTimeSeconds = Movement::computeFallTime(drop, false);
        float const maxHumanLikeHorizontalDistance = horizontalSpeed * fallTimeSeconds + PLAYERBOT_BG_FALL_SHORTCUT_SPEED_TOLERANCE;
        if (landingDistance > maxHumanLikeHorizontalDistance)
        {
            float const cappedLandingDistance = std::min(planarDistance, maxHumanLikeHorizontalDistance);
            if (cappedLandingDistance <= edgeDistance)
                continue;

            float const cappedFraction = cappedLandingDistance / planarDistance;
            float cappedZ = player->GetPositionZ();
            float const cappedX = player->GetPositionX() + dx * cappedFraction;
            float const cappedY = player->GetPositionY() + dy * cappedFraction;
            player->UpdateAllowedPositionZ(cappedX, cappedY, cappedZ);
            if (player->GetPositionZ() - cappedZ < PLAYERBOT_BG_MIN_FALL_SHORTCUT_DROP)
                continue;

            if (!player->IsWithinLOS(cappedX, cappedY, cappedZ + 1.0f))
                continue;

            fallDestination.Relocate(cappedX, cappedY, cappedZ, destination.GetOrientation());
            nextAllowedFallShortcutMsByGuid[botGuid] = nowMs + PLAYERBOT_BG_FALL_SHORTCUT_COOLDOWN_MS;
            return true;
        }

        if (!player->IsWithinLOS(landingX, landingY, landingZ + 1.0f))
            continue;

        fallDestination.Relocate(landingX, landingY, landingZ, destination.GetOrientation());
        nextAllowedFallShortcutMsByGuid[botGuid] = nowMs + PLAYERBOT_BG_FALL_SHORTCUT_COOLDOWN_MS;
        return true;
    }

    return false;
}

bool TryBuildBattlegroundSegmentDestination(Player* player, Position const& safeDestination, Position& segmentDestination, PathType* resolvedPathType = nullptr)
{
    if (!player)
        return false;

    auto const tryResolveDestination = [&](Position const& requestedDestination, Position& resolvedDestination, PathType* outPathType) -> bool
    {
        Position const collisionSafeDestination = BuildCollisionSafeDestination(player, requestedDestination);

        PathGenerator path(player);
        // Allow longer battleground route segments so bots can commit to
        // meaningful navmesh progress toward distant enemies instead of
        // repeatedly selecting tiny local hops that catch on terrain.
        path.SetPathLengthLimit(PLAYERBOT_BG_PATH_CALCULATION_LENGTH_LIMIT);
        bool pathOk = path.CalculatePath(collisionSafeDestination.GetPositionX(), collisionSafeDestination.GetPositionY(), collisionSafeDestination.GetPositionZ(), true);
        PathType pathType = path.GetPathType();
        Movement::PointsArray points = path.GetPath();
        G3D::Vector3 actualEnd = path.GetActualEndPosition();

        if ((pathType & PATHFIND_SHORTCUT) != 0)
        {
            PathGenerator retryPath(player);
            retryPath.SetPathLengthLimit(PLAYERBOT_BG_PATH_CALCULATION_LENGTH_LIMIT);
            bool const retryOk = retryPath.CalculatePath(collisionSafeDestination.GetPositionX(), collisionSafeDestination.GetPositionY(), collisionSafeDestination.GetPositionZ(), false);
            PathType const retryType = retryPath.GetPathType();
            if (retryOk && (retryType & PATHFIND_SHORTCUT) == 0)
            {
                points = retryPath.GetPath();
                pathType = retryType;
                pathOk = true;
                actualEnd = retryPath.GetActualEndPosition();
            }
        }

        if (!pathOk || IsForbiddenBattlegroundPathType(pathType))
            return false;

        bool haveResolvedDestination = false;
        if (BuildNavPathSegmentDestination(player, points, collisionSafeDestination.GetOrientation(), resolvedDestination))
        {
            haveResolvedDestination = true;
        }
        else
        {
            Position actualEndDestination(actualEnd.x, actualEnd.y, actualEnd.z, collisionSafeDestination.GetOrientation());
            float const destinationDistance = player->GetDistance(collisionSafeDestination);
            float const actualEndDistance = player->GetDistance(actualEndDestination);
            if (actualEndDistance > 1.5f && actualEndDistance + 2.0f < destinationDistance)
            {
                resolvedDestination = actualEndDestination;
                haveResolvedDestination = true;
            }
        }

        if (!haveResolvedDestination)
            return false;

        resolvedDestination = BuildCollisionSafeDestination(player, resolvedDestination);
        float const dx = resolvedDestination.GetPositionX() - player->GetPositionX();
        float const dy = resolvedDestination.GetPositionY() - player->GetPositionY();
        float const planarDelta = std::sqrt(dx * dx + dy * dy);
        float const verticalDelta = std::fabs(resolvedDestination.GetPositionZ() - player->GetPositionZ());
        if (planarDelta < 0.5f || verticalDelta > std::max(8.0f, planarDelta * 0.75f + 2.0f))
            return false;

        if (outPathType)
            *outPathType = pathType;

        return true;
    };

    if (tryResolveDestination(safeDestination, segmentDestination, resolvedPathType))
        return true;

    float const dx = safeDestination.GetPositionX() - player->GetPositionX();
    float const dy = safeDestination.GetPositionY() - player->GetPositionY();
    float const dz = safeDestination.GetPositionZ() - player->GetPositionZ();
    float const planarDistance = std::sqrt(dx * dx + dy * dy);
    if (planarDistance < 1.0f)
        return false;

    std::array<float, 8> const probeDistances =
    {
        80.0f,
        60.0f,
        45.0f,
        30.0f,
        20.0f,
        12.0f,
        8.0f,
        5.0f
    };

    PathType probePathType = PathType(0);
    for (float probeDistance : probeDistances)
    {
        float const cappedDistance = std::min(planarDistance - 0.25f, probeDistance);
        if (cappedDistance <= 0.5f)
            continue;

        float const fraction = cappedDistance / planarDistance;
        Position probeDestination(
            player->GetPositionX() + dx * fraction,
            player->GetPositionY() + dy * fraction,
            player->GetPositionZ() + dz * fraction,
            safeDestination.GetOrientation());

        if (tryResolveDestination(probeDestination, segmentDestination, &probePathType))
        {
            if (resolvedPathType)
                *resolvedPathType = probePathType;
            return true;
        }
    }

    return false;
}

bool IssueHumanLikeFollow(Player* player, Unit* target, float desiredDistance, float destinationChangeThreshold, uint32 minReissueMs)
{
    if (!player || !target)
        return false;

    return IssueMovePointThrottled(player, BuildFollowDestination(player, target, desiredDistance), destinationChangeThreshold, minReissueMs);
}

bool TryIssueBattlegroundFallMovement(Player* player, Position const& destination, char const* reason = nullptr)
{
    if (!player || !player->IsAlive())
        return false;

    FinishCompletedBattlegroundGravityFall(player);
    if (IsResolvingBattlegroundGravityFall(player))
        return true;

    Position fallDestination;
    if (!TryBuildBattlegroundFallShortcutDestination(player, destination, fallDestination))
        return false;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return false;

    motionMaster->Clear(MOTION_SLOT_ACTIVE);

    // Keep the unit movement state in sync with the fall spline.  The spline
    // packet alone is not enough for player-controlled bots: without the falling
    // movement flag, combat movement can queue fresh chase/follow orders while
    // the visual fall is still in progress, which makes observers see the bot
    // snap back up and restart the fall repeatedly.
    player->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
    player->SetFallInformation(0, player->GetPositionZ());

    motionMaster->LaunchMoveSpline([fallDestination](Movement::MoveSplineInit& init)
    {
        init.MoveTo(fallDestination.GetPositionX(), fallDestination.GetPositionY(), fallDestination.GetPositionZ(), false);
        init.SetFall();
    }, 0, MOTION_PRIORITY_HIGHEST, EFFECT_MOTION_TYPE);

    std::ostringstream detail;
    detail << "movepoint=fall-shortcut"
           << " reason=" << (reason ? reason : "unspecified")
           << " drop=" << int32(player->GetPositionZ() - fallDestination.GetPositionZ())
           << " segDist=" << int32(player->GetDistance(fallDestination));
    EmitBattlegroundGmDebug(player, detail.str(), 1000);
    return true;
}

bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold, uint32 minReissueMs)
{
    if (!player)
        return false;

    FinishCompletedBattlegroundGravityFall(player);

    if (IsResolvingBattlegroundGravityFall(player))
        return true;

    if (!CanIssueMovementCommand(player, 500))
        return false;

    ClearEatDrinkAurasForMovement(player);

    minReissueMs = std::max<uint32>(minReissueMs, 500);

    if (IsWarsongGulch(player))
        minReissueMs = std::max<uint32>(minReissueMs, 500);

    struct MoveOrderState
    {
        Position lastDestination;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, MoveOrderState> stateByGuid;
    static std::unordered_map<uint64, uint8> stationaryReissueCountByGuid;
    uint64 const botGuid = player->GetGUID().GetRawValue();
    MoveOrderState& state = stateByGuid[player->GetGUID().GetRawValue()];
    uint8& stationaryReissueCount = stationaryReissueCountByGuid[botGuid];
    uint32 const nowMs = GameTime::GetGameTimeMS();

    bool const destinationChanged = state.lastIssueMs == 0 ||
        state.lastDestination.GetExactDist(destination) >= destinationChangeThreshold;
    bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + minReissueMs;
    bool const botCurrentlyMoving = player->isMoving();
    bool const hardThrottleActive = IsWarsongGulch(player) && !canReissueByTime;
    bool const forcedStationaryReissue = !destinationChanged && !canReissueByTime && !botCurrentlyMoving;
    if (forcedStationaryReissue)
        stationaryReissueCount = std::min<uint8>(uint8(stationaryReissueCount + 1), 20);
    else
        stationaryReissueCount = 0;

    if (hardThrottleActive && botCurrentlyMoving)
        return false;

    if (!destinationChanged && !canReissueByTime && botCurrentlyMoving)
        return false;

    uint32 bgStatus = 0;
    if (Battleground* bg = player->GetBattleground())
        bgStatus = uint32(bg->GetStatus());

    if (forcedStationaryReissue)
    {
        EmitBattlegroundGmDebug(player,
            "movepoint=forced-reissue reason=stationary-with-throttle lastIssueMs=" + std::to_string(state.lastIssueMs) +
            " nowMs=" + std::to_string(nowMs) +
            " motionType=" + std::to_string(uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType())) +
            " bgStatus=" + std::to_string(bgStatus) +
            " reissueCount=" + std::to_string(stationaryReissueCount), 1000);
    }

    MotionMaster* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const currentMovement = motionMaster->GetCurrentMovementGeneratorType();
    if (IsResolvingBattlegroundGravityFall(player))
        return true;

    if (player->InBattleground() && botCurrentlyMoving && currentMovement == EFFECT_MOTION_TYPE)
        return true;

    if (currentMovement == FOLLOW_MOTION_TYPE || currentMovement == DISTRACT_MOTION_TYPE)
    {
        motionMaster->Clear();
    }
    else if (!botCurrentlyMoving && player->InBattleground() &&
        currentMovement != IDLE_MOTION_TYPE &&
        currentMovement != CHASE_MOTION_TYPE &&
        currentMovement != POINT_MOTION_TYPE &&
        // Charge/Intercept movement is issued through effect generators.
        // Do not treat those as stale while they are resolving.
        currentMovement != EFFECT_MOTION_TYPE)
    {
        EmitBattlegroundGmDebug(player,
            "movepoint=clear-stale-generator motionType=" + std::to_string(uint32(currentMovement)), 1000);
        motionMaster->Clear();
    }

    bool const generatePath = !player->IsFlying() && !player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
    Position const safeDestination = generatePath ? BuildCollisionSafeDestination(player, destination) : destination;

    Position issuedDestination = safeDestination;
    if (generatePath && player->InBattleground())
    {
        if (TryIssueBattlegroundFallMovement(player, safeDestination, "move-point"))
        {
            issuedDestination = safeDestination;
        }
        else
        {
            Position segmentDestination;
            PathType pathType = PathType(0);
            if (!TryBuildBattlegroundSegmentDestination(player, safeDestination, segmentDestination, &pathType))
            {
                EmitBattlegroundGmDebug(player,
                    "movepoint=blocked-no-nav destDist=" + std::to_string(int32(player->GetDistance(safeDestination))), 1000);
                return false;
            }

            motionMaster->MovePoint(0, segmentDestination, true);
            issuedDestination = segmentDestination;
            EmitBattlegroundGmDebug(player,
                "movepoint=nav-segment pathType=" + std::to_string(uint32(pathType)) +
                " segDist=" + std::to_string(int32(player->GetDistance(segmentDestination))), 0);
        }
    }
    else
    {
        motionMaster->MovePoint(0, safeDestination, generatePath);
        issuedDestination = safeDestination;
    }

    state.lastDestination = issuedDestination;
    state.lastIssueMs = nowMs;
    return true;
}

bool QueuePlayer(Player* player, BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    if (!player || player->InBattleground())
        return false;

    // Allow managed bots to keep participating in queue/invite lifecycle even if
    // they died in the open world. Battleground queue/port handlers can reject
    // dead actors, so recover to alive before queueing.
    if (!player->IsAlive())
        player->ResurrectPlayer(1.0f);

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    // Recover stale local queue slot state before evaluating queue eligibility.
    // Managed bots can occasionally keep orphaned queue ids after lifecycle
    // transitions, which blocks HasFreeBattlegroundQueueId() and prevents
    // requeueing after subsequent battlegrounds.
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const existingQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (existingQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        BattlegroundQueue& existingQueue = sBattlegroundMgr->GetBattlegroundQueue(existingQueueTypeId);
        GroupQueueInfo ginfo{};
        if (existingQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
            continue;

        player->RemoveBattlegroundQueueId(existingQueueTypeId);
        EmitLifecycleDiagnostic(player, "queue-prune-stale-slot",
            "Removed stale queueTypeId=" + std::to_string(uint32(existingQueueTypeId)));
    }

    // SCM is intentionally prioritized over other BG queues for managed bots:
    // free any existing queue slots first so SCM can always enqueue.
    if (bgTypeId == BATTLEGROUND_SCM)
    {
        RemoveMatchingQueues(player, false, false, true);
        RemoveMatchingQueues(player, true, false, false);
        player->SetArenaTeamIdInvited(0);
    }

    // Managed random bots can run on disconnected virtual sessions where RBAC
    // battleground permissions are not always populated like live client sessions.
    // Gate queue eligibility by battleground level + free queue slots instead.
    if (!player->GetBGAccessByLevel(bgTypeId) || !player->HasFreeBattlegroundQueueId())
        return false;

    BattlegroundQueueTypeId const bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    if (player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
    {
        if (bgTypeId == BATTLEGROUND_SCM)
        {
            if (PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel()))
            {
                sBattlegroundMgr->ScheduleQueueUpdate(0, arenaType, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
                EmitLifecycleDiagnostic(player, "queue-refresh-existing",
                    "Already queued for SCM; forced queue update refresh.");
                return true;
            }
        }

        return false;
    }

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
        return false;

    // Playerbots should never request a fixed Scarlet Chapel side. Only the custom queue NPC
    // should set ALLIANCE/HORDE explicitly. Clear any stale raw override before AddGroup so
    // BattlegroundQueue can assign a synthetic balanced side.
    if (bgTypeId == BATTLEGROUND_SCM)
        player->SetBGTeam(0);

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
    GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketEntry, arenaType, false, false, 0, 0);
    if (!ginfo)
    {
        EmitLifecycleDiagnostic(player, "queue-add-failed", "BattlegroundQueue::AddGroup returned null.");
        return false;
    }

    player->AddBattlegroundQueueId(bgQueueTypeId);
    sBattlegroundMgr->ScheduleQueueUpdate(ginfo->ArenaMatchmakerRating, ginfo->ArenaType, bgQueueTypeId, bgTypeId,
        bracketEntry->GetBracketId());
    EmitLifecycleDiagnostic(player, "queue-add-success",
        "Queued for bgTypeId=" + std::to_string(uint32(bgTypeId)) + " queueTypeId=" + std::to_string(uint32(bgQueueTypeId)));
    return true;
}

bool RemovePlayerFromQueue(Player* player, BattlegroundQueueTypeId bgQueueTypeId, bool scheduleNonArenaUpdate)
{
    if (!player || bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    if (player->GetBattlegroundQueueIndex(bgQueueTypeId) >= PLAYER_MAX_BATTLEGROUND_QUEUES)
        return false;

    BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

    GroupQueueInfo ginfo{};
    bool const haveGroupInfo = bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo);

    // Always clear the player's local queue slot if it is still present.
    player->RemoveBattlegroundQueueId(bgQueueTypeId);

    if (haveGroupInfo)
        bgQueue.RemovePlayer(player->GetGUID(), true);

    if (scheduleNonArenaUpdate && haveGroupInfo && !ginfo.ArenaType && bgTemplate)
    {
        if (PvPDifficultyEntry const* bracketEntry =
            GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel()))
        {
            sBattlegroundMgr->ScheduleQueueUpdate(
                ginfo.ArenaMatchmakerRating,
                ginfo.ArenaType,
                bgQueueTypeId,
                bgTypeId,
                bracketEntry->GetBracketId());
        }
    }

    return true;
}
bool RemoveMatchingQueues(Player* player, bool arenaOnly, bool invitedOnly, bool scheduleNonArenaUpdate)
{
    if (!player || !player->InBattlegroundQueue())
        return false;

    bool removed = false;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
        if (arenaOnly != isArenaQueue)
            continue;

        if (invitedOnly && !player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            continue;

        removed = RemovePlayerFromQueue(player, bgQueueTypeId, scheduleNonArenaUpdate) || removed;
    }

    return removed;
}

bool AcceptMatchingInvite(Player* player, bool arenaInvite)
{
    if (!player || player->InBattleground())
        return false;

    // Keep battleground/arena participation consistent for dead managed bots:
    // invites should still be accepted and transitioned immediately.
    if (!player->IsAlive())
        player->ResurrectPlayer(1.0f);

    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
        if (arenaInvite != isArenaQueue)
            continue;

        if (!player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            continue;

        BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
        uint8 const arenaType = BattlegroundMgr::BGArenaType(bgQueueTypeId);
        if ((arenaType != 0) != arenaInvite)
            continue;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
        {
            EmitLifecycleDiagnostic(player, "invite-missing-group-info",
                "No GroupQueueInfo for queueTypeId=" + std::to_string(uint32(bgQueueTypeId)));
            continue;
        }

        BattlegroundTypeId packetBgTypeId = bgTypeId;
        if (arenaType != 0)
        {
            // Arena invites can target dynamic map-specific arena templates rather
            // than BATTLEGROUND_AA; resolve the invited instance type first.
            Battleground* invited = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, BATTLEGROUND_TYPE_NONE);
            if (!invited)
            {
                EmitLifecycleDiagnostic(player, "invite-arena-instance-missing",
                    "No invited arena instance for guid=" + std::to_string(ginfo.IsInvitedToBGInstanceGUID));
                continue;
            }

            packetBgTypeId = invited->GetTypeID();
        }

        WorldSession* session = player->GetSession();
        if (!session)
        {
            EmitLifecycleDiagnostic(player, "invite-no-session", "WorldSession is null.");
            continue;
        }

        // Execute invite acceptance directly. Managed random bots can run on
        // disconnected virtual sessions where queued outbound packets are not
        // guaranteed to be pumped like real client traffic.
        ClearMovementBeforeBattlegroundTeleport(player);

        WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
        packet << arenaType << uint8(0) << uint32(packetBgTypeId) << uint16(0x1F90) << uint8(1);
        session->HandleBattleFieldPortOpcode(packet);

        if (player->IsBeingTeleportedFar())
        {
            EmitLifecycleDiagnostic(player, "invite-accept-far-teleport-pending",
                "Issuing server-side HandleMoveWorldportAck for bot teleport finalization.");
            ClearMovementBeforeBattlegroundTeleport(player);
            session->HandleMoveWorldportAck();
        }

        if (!player->InBattleground() && !player->IsBeingTeleported())
        {
            EmitLifecycleDiagnostic(player, "invite-accept-no-transition",
                "HandleBattleFieldPortOpcode did not transition to battleground/teleport.");
        }
        else
        {
            EmitLifecycleDiagnostic(player, "invite-accept-transition",
                "Accepted invite for bgTypeId=" + std::to_string(uint32(packetBgTypeId)));
        }
        return true;
    }

    return false;
}

bool HasConflictingBattlegroundLifecycleContext(playerbot::BattlegroundLifecycleContext const& context)
{
    return (context.queueOperation != playerbot::QueueOperationType::None) &&
        (context.invitationResponse != playerbot::InvitationResponseType::None);
}

bool HasPendingBattlegroundInvite(Player const* player)
{
    if (!player)
        return false;

    for (uint32 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        if (player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            return true;
    }

    return false;
}

bool ShouldRefreshLongQueuedBot(Player* player)
{
    if (!player)
        return false;

    uint64 const guidRaw = player->GetGUID().GetRawValue();
    if (player->InBattleground() || !player->InBattlegroundQueue() || HasPendingBattlegroundInvite(player))
    {
        g_BattlegroundQueuedNoInviteSinceMsByGuid.erase(guidRaw);
        return false;
    }

    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32& queuedSinceMs = g_BattlegroundQueuedNoInviteSinceMsByGuid[guidRaw];
    if (!queuedSinceMs)
    {
        queuedSinceMs = nowMs;
        return false;
    }

    if (nowMs < queuedSinceMs + PLAYERBOT_BG_QUEUE_REQUEUE_TIMEOUT_MS)
        return false;

    queuedSinceMs = nowMs;
    return true;
}

bool HandleBattlegroundDeathState(Player* player)
{
    static std::unordered_map<uint64, uint32> queuedSinceMsByGuid;
    auto resolveSpiritGuide = [](Player* candidate, uint32 preferredEntry) -> Creature*
    {
        if (!candidate)
            return nullptr;

        // Keep spirit-healer lookup local to the graveyard.
        // Some virtual-session bots can temporarily report a neutral BG team;
        // in that case we still search both faction spirit-guide entries.
        float constexpr spiritGuideSearchRadius = 30.0f;

        auto findGuideByEntry = [candidate, spiritGuideSearchRadius](uint32 entry) -> Creature*
        {
            if (!entry)
                return nullptr;

            std::list<Creature*> guides;
            candidate->GetCreatureListWithEntryInGrid(guides, entry, spiritGuideSearchRadius);

            Creature* nearestGuide = nullptr;
            float nearestDistanceSq = std::numeric_limits<float>::max();
            for (Creature* guide : guides)
            {
                if (!guide || !guide->IsSpiritService())
                    continue;

                float const distanceSq = candidate->GetExactDist2dSq(guide);
                if (distanceSq < nearestDistanceSq)
                {
                    nearestGuide = guide;
                    nearestDistanceSq = distanceSq;
                }
            }

            return nearestGuide;
        };

        Creature* spiritGuide = findGuideByEntry(preferredEntry);
        if (!spiritGuide)
        {
            spiritGuide = findGuideByEntry(BG_CREATURE_ENTRY_A_SPIRITGUIDE);
            if (!spiritGuide)
                spiritGuide = findGuideByEntry(BG_CREATURE_ENTRY_H_SPIRITGUIDE);
        }

        return spiritGuide;
    };

    if (!player || !player->InBattleground())
        return false;

    if (player->IsAlive())
    {
        queuedSinceMsByGuid.erase(player->GetGUID().GetRawValue());
        return false;
    }

    if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        player->BuildPlayerRepop();
        player->RepopAtGraveyard();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP death handling: guid={} action=release-spirit.",
            player->GetGUID().ToString());
        return true;
    }

    Battleground* battleground = player->GetBattleground();
    if (!battleground)
        return true;

    if (battleground->IsPlayerInResurrectQueue(player->GetGUID()))
    {
        uint64 const playerGuidRaw = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& queuedSinceMs = queuedSinceMsByGuid[playerGuidRaw];
        if (!queuedSinceMs)
            queuedSinceMs = nowMs;

        // Fallback recovery: if a bot remains ghosted in the resurrect queue for too long,
        // refresh its spirit-guide queue registration so it revives on normal BG wave timing.
        if (nowMs >= queuedSinceMs + 12000)
        {
            battleground->RemovePlayerFromResurrectQueue(player->GetGUID());

            uint32 const spiritEntry = ResolveBotTeamId(player) == TEAM_ALLIANCE ? BG_CREATURE_ENTRY_A_SPIRITGUIDE : BG_CREATURE_ENTRY_H_SPIRITGUIDE;
            Creature* spiritGuide = resolveSpiritGuide(player, spiritEntry);

            if (spiritGuide)
            {
                battleground->AddPlayerToResurrectQueue(spiritGuide->GetGUID(), player->GetGUID());
                sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(player, battleground, spiritGuide->GetGUID());
                queuedSinceMs = nowMs;
                TC_LOG_WARN("playerbots.pvp.lifecycle",
                    "Playerbot PvP death handling fallback spirit queue refresh: guid={} spiritGuide={}.",
                    player->GetGUID().ToString(), spiritGuide->GetGUID().ToString());
            }
            else
            {
                queuedSinceMs = nowMs;
            }
        }
        return true;
    }

    uint32 const spiritEntry = ResolveBotTeamId(player) == TEAM_ALLIANCE ? BG_CREATURE_ENTRY_A_SPIRITGUIDE : BG_CREATURE_ENTRY_H_SPIRITGUIDE;

    // Mirror player core BG death handling behavior: once ghosted at a battleground
    // graveyard, register at a nearby spirit guide and wait for the periodic wave rez.
    // Avoid script-driven ghost movement because missed/path-blocked moves can prevent
    // ever getting queued for resurrection.
    Creature* spiritGuide = resolveSpiritGuide(player, spiritEntry);
    if (!spiritGuide)
        return true;

    battleground->AddPlayerToResurrectQueue(spiritGuide->GetGUID(), player->GetGUID());
    queuedSinceMsByGuid[player->GetGUID().GetRawValue()] = GameTime::GetGameTimeMS();
    sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(player, battleground, spiritGuide->GetGUID());
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP death handling: guid={} action=queue-resurrect spiritGuide={}.",
        player->GetGUID().ToString(), spiritGuide->GetGUID().ToString());
    return true;
}

bool HasConflictingArenaLifecycleContext(playerbot::ArenaLifecycleContext const& context)
{
    return (context.queueOperation != playerbot::QueueOperationType::None) &&
        (context.teamInteraction != playerbot::ArenaTeamInteractionType::None);
}

bool IsTacticalAction(char const* actionName, char const* expected)
{
    return actionName && expected && std::strcmp(actionName, expected) == 0;
}

void ClearActiveMovementForControlLoss(Player* player)
{
    if (!player)
        return;

    player->AttackStop();
    player->SetSelection(ObjectGuid::Empty);
    // Preserve server-side confused wander (e.g. polymorph/sheep). Clearing
    // the active movement slot repeatedly can freeze the expected drifting.
    if (player->HasUnitState(UNIT_STATE_CONFUSED) || player->HasAuraType(SPELL_AURA_MOD_CONFUSE) || player->IsPolymorphed())
        return;

    if (MotionMaster* motionMaster = player->GetMotionMaster())
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
}

bool CanIssueBotMovement(Player* player)
{
    if (!player || !player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return false;

    // Once a bot has stepped off battleground terrain (for example graveyard
    // cliffs), do not let combat/objective steering replace the fall with a
    // fresh ground-snapped MovePoint/Follow/Chase order. The core fall state
    // should resolve at normal gravity, after which pathing can resume.
    FinishCompletedBattlegroundGravityFall(player);

    if (IsResolvingBattlegroundGravityFall(player))
        return false;

    if (IsCrowdControlledForAction(player))
    {
        ClearActiveMovementForControlLoss(player);
        return false;
    }

    if (player->HasUnitState(UNIT_STATE_ROOT) ||
        player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING))
    {
        return false;
    }

    return true;
}

float GetAggressiveCombatScanDistance(Player const* player, float fallbackDistance)
{
    if (!player)
        return fallbackDistance;

    return std::max(fallbackDistance, player->GetVisibilityRange());
}

bool IsMeleePressureTarget(Unit const* unit)
{
    Player const* player = unit ? unit->ToPlayer() : nullptr;
    if (!player)
        return false;

    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
            return true;
        case CLASS_SHAMAN:
        case CLASS_PALADIN:
        {
            Item const* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
            ItemTemplate const* mainHandTemplate = mainHand ? mainHand->GetTemplate() : nullptr;
            return mainHandTemplate && mainHandTemplate->InventoryType == INVTYPE_2HWEAPON;
        }
        default:
            return false;
    }
}

bool IsActivelyPressuringInMelee(Unit const* attacker, Player const* bot)
{
    if (!attacker || !bot || !attacker->IsAlive() || !bot->IsAlive())
        return false;

    if (!IsMeleePressureTarget(attacker))
        return false;

    // Only kite if the melee-capable target is actually threatening this bot.
    // Otherwise, ranged casters (e.g. frost mages) should hold position and
    // continue turret casting from their current firing band.
    if (attacker->GetVictim() == bot)
        return true;

    return attacker->IsWithinMeleeRange(bot) || bot->IsWithinMeleeRange(attacker);
}

bool ShouldForceMeleeFallbackOnLowMana(Player const* player)
{
    if (!player || player->GetPowerType() != POWER_MANA)
        return false;

    if (player->GetClass() != CLASS_SHAMAN && player->GetClass() != CLASS_PALADIN)
        return false;

    return player->GetPowerPct(POWER_MANA) <= 10.0f || player->GetPower(POWER_MANA) < 250;
}

struct CombatPositioningProfile
{
    float preferredMinRange = 0.0f;
    float preferredIdealRange = 0.0f;
    float preferredMaxPressureRange = 0.0f;
    bool primarilyRanged = false;
    bool createDistanceWhenCrowded = false;
    bool meleeFallbackAcceptable = true;
    char const* label = "default";
};

CombatPositioningProfile GetCombatPositioningProfile(Player const* player)
{
    if (!player)
        return {};

    if (Trinity::Helpers::Entity::IsPlayerHealer(player))
    {
        switch (player->GetClass())
        {
            case CLASS_PRIEST: return { 0.0f, 25.0f, 34.0f, true, false, false, "priest-healer" };
            case CLASS_SHAMAN: return { 0.0f, 20.0f, 30.0f, true, false, true, "shaman-healer" };
            case CLASS_DRUID: return { 0.0f, 18.0f, 28.0f, true, false, true, "druid-healer" };
            case CLASS_PALADIN: return { 0.0f, 16.0f, 26.0f, true, false, true, "paladin-healer" };
            default: break;
        }
    }

    switch (player->GetClass())
    {
        case CLASS_HUNTER: return { 8.0f, 28.0f, 38.0f, true, true, false, "hunter-ranged" };
        case CLASS_MAGE: return { 12.0f, 27.0f, 36.0f, true, true, false, "mage-ranged" };
        case CLASS_PRIEST: return { 10.0f, 25.0f, 34.0f, true, true, false, "priest-ranged" };
        case CLASS_WARLOCK: return { 10.0f, 26.0f, 35.0f, true, true, false, "warlock-ranged" };
        case CLASS_WARRIOR: return { 0.0f, 1.5f, 5.0f, false, false, true, "warrior-melee" };
        case CLASS_ROGUE: return { 0.0f, 1.5f, 5.0f, false, false, true, "rogue-melee" };
        case CLASS_PALADIN: return { 0.0f, 3.0f, 8.0f, false, false, true, "paladin-hybrid" };
        case CLASS_SHAMAN: return { 5.0f, 20.0f, 30.0f, true, true, true, "shaman-hybrid" };
        case CLASS_DRUID: return { 4.0f, 18.0f, 28.0f, true, true, true, "druid-hybrid" };
        default: return { 0.0f, 3.0f, 8.0f, false, false, true, "default-melee" };
    }
}

bool MoveAwayFromUnit(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target || !CanIssueBotMovement(player))
        return false;
    if (!CanIssueMovementCommand(player, 500))
        return false;

    float const angleAway = target->GetAbsoluteAngle(player);
    float const currentDistance = player->GetDistance(target);
    float const moveDistance = std::max(4.0f, desiredDistance - currentDistance + 2.0f);

    Position destination(player->GetPositionX() + std::cos(angleAway) * moveDistance,
        player->GetPositionY() + std::sin(angleAway) * moveDistance,
        player->GetPositionZ(), player->GetOrientation());
    return IssueMovePointThrottled(player, destination, 4.0f, 500);
}

bool TryRecoverLineOfSight(Player* player, Unit* target, CombatPositioningProfile const& profile, char const* reason)
{
    if (!player || !target || !target->IsAlive() || !CanIssueBotMovement(player))
        return false;
    if (!CanIssueMovementCommand(player, 500))
        return false;

    if (player->IsWithinLOSInMap(target))
        return false;

    float const orbitAngle = target->GetAbsoluteAngle(player) + frand(-0.85f, 0.85f);
    float const orbitRange = std::max(profile.preferredMinRange + 2.0f, profile.preferredIdealRange);
    Position reposition(target->GetPositionX() + std::cos(orbitAngle) * orbitRange,
        target->GetPositionY() + std::sin(orbitAngle) * orbitRange,
        target->GetPositionZ(), player->GetOrientation());
    if (!IssueMovePointThrottled(player, reposition, 4.0f, 500))
        return false;

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP LOS recovery: bot={} target={} profile={} reason={} orbitRange={}.",
        player->GetGUID().ToString(), target->GetGUID().ToString(), profile.label, reason ? reason : "unknown", orbitRange);
    return true;
}

Player* FindFlagCarrierForDirective(Player* player, playerbot::FlagCarrierDirective directive)
{
    if (!player || directive == playerbot::FlagCarrierDirective::None || !player->InBattleground())
        return nullptr;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    TeamId const botTeam = player->GetTeamId();
    TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;

    if (BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(battleground))
    {
        ObjectGuid carrierGuid = ObjectGuid::Empty;
        if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier)
            carrierGuid = bgWs->GetFlagPickerGUID(botTeam);
        else if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier)
            carrierGuid = bgWs->GetFlagPickerGUID(enemyTeam);

        if (carrierGuid.IsEmpty())
            return nullptr;

        return ObjectAccessor::FindConnectedPlayer(carrierGuid);
    }

    if (BattlegroundEY* bgEy = dynamic_cast<BattlegroundEY*>(battleground))
    {
        ObjectGuid const carrierGuid = bgEy->GetFlagPickerGUID();
        if (carrierGuid.IsEmpty())
            return nullptr;

        Player* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
        if (!carrier)
            return nullptr;

        if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier && carrier->GetTeamId() != botTeam)
            return carrier;
        if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier && carrier->GetTeamId() == botTeam)
            return carrier;
    }

    return nullptr;
}

bool MoveTowardUnit(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target)
        return false;
    if (!player->IsAlive() || !target->IsAlive() || player->GetMapId() != target->GetMapId() || !CanIssueBotMovement(player))
    {
        EmitBattlegroundGmDebug(player,
            "move-toward-unit=blocked botAlive=" + std::to_string(player->IsAlive() ? 1 : 0) +
            " targetAlive=" + std::to_string(target->IsAlive() ? 1 : 0) +
            " sameMap=" + std::to_string(player->GetMapId() == target->GetMapId() ? 1 : 0) +
            " canMove=" + std::to_string(CanIssueBotMovement(player) ? 1 : 0) +
            " motionType=" + std::to_string(uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType())), 1200);
        return false;
    }

    float const distanceToTarget = player->GetDistance(target);
    if (player->InBattleground() && distanceToTarget > desiredDistance)
    {
        Position destination = target->GetPosition();
        bool const moved = IssueMovePointThrottled(player, destination, 30.0f, 2000);
        EmitBattlegroundGmDebug(player,
            "move-toward-unit mode=battleground-segmented target=" + target->GetName() +
            " dist=" + std::to_string(int32(distanceToTarget)) +
            " issued=" + std::to_string(moved ? 1 : 0), 1200);
        return moved || player->isMoving();
    }

    CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
    if (!player->IsWithinLOSInMap(target))
        return TryRecoverLineOfSight(player, target, profile, "move-toward-unit");

    if (!player->IsWithinDistInMap(target, desiredDistance))
    {
        if (!CanIssueMovementCommand(player, 500))
            return false;
        ClearEatDrinkAurasForMovement(player);
        return IssueHumanLikeFollow(player, target, desiredDistance, 6.0f, 500);
    }

    return true;
}

std::unordered_map<uint64, uint32> g_WsgReturnAttemptNotBeforeMsByGuid;

GameObject* GetFriendlyDroppedWsgFlag(Player* player, BattlegroundWS* bgWs)
{
    if (!player || !bgWs || !player->FindMap())
        return nullptr;

    TeamId const botBgTeam = ResolveBotTeamId(player);
    if (bgWs->GetFlagState(botBgTeam) != BG_WS_FLAG_STATE_ON_GROUND)
        return nullptr;

    ObjectGuid const droppedFlagGuid = bgWs->GetDroppedFlagGUID(botBgTeam);
    if (droppedFlagGuid.IsEmpty())
        return nullptr;

    return player->FindMap()->GetGameObject(droppedFlagGuid);
}

bool HumanTeammateNearDroppedFlag(Player* player, GameObject const* droppedFlag, float veryCloseDistance)
{
    if (!player || !droppedFlag || !player->FindMap())
        return false;

    TeamId const botBgTeam = ResolveBotTeamId(player);
    float const botDistance = player->GetDistance(droppedFlag);

    Map::PlayerList const& players = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* teammate = itr->GetSource();
        if (!teammate || teammate == player || !teammate->IsAlive())
            continue;
        if (teammate->GetBattlegroundId() != player->GetBattlegroundId())
            continue;

        TeamId const teammateBgTeam = ResolveBotTeamId(teammate);
        if (teammateBgTeam != botBgTeam || playerbot::IsManagedRandomBot(teammate))
            continue;

        float const teammateDistance = teammate->GetDistance(droppedFlag);
        if (teammateDistance <= veryCloseDistance && teammateDistance <= botDistance + 1.0f)
            return true;
    }

    return false;
}



bool BattlegroundHasAnyRealHumanPlayers(Player const* player)
{
    if (!player || !player->InBattleground() || !player->FindMap())
        return false;

    uint32 const battlegroundId = player->GetBattlegroundId();
    Map::PlayerList const& players = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player const* participant = itr->GetSource();
        if (!participant || participant->GetBattlegroundId() != battlegroundId)
            continue;

        WorldSession const* session = participant->GetSession();
        bool const isVirtualSession = session && session->IsVirtualSession();
        if (!isVirtualSession && !playerbot::IsManagedRandomBot(participant))
            return true;
    }

    return false;
}


bool HasAnyRealHumanInterestInBattleground(BattlegroundTypeId targetBgType)
{
    if (targetBgType == BATTLEGROUND_TYPE_NONE)
        return false;

    BattlegroundQueueTypeId const targetQueueType = BattlegroundMgr::BGQueueTypeId(targetBgType, 0);

    std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
    {
        if (!participant)
            continue;

        WorldSession const* session = participant->GetSession();
        bool const isVirtualSession = session && session->IsVirtualSession();
        if (isVirtualSession || playerbot::IsManagedRandomBot(participant))
            continue;

        if (participant->InBattleground() && participant->GetBattlegroundTypeId() == targetBgType)
            return true;

        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            if (participant->GetBattlegroundQueueTypeId(i) == targetQueueType)
                return true;
        }
    }

    return false;
}

bool HasAnyRealHumanPlayerInBattleground(BattlegroundTypeId targetBgType)
{
    if (targetBgType == BATTLEGROUND_TYPE_NONE)
        return false;

    std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
    {
        if (!participant || !participant->InBattleground() || participant->GetBattlegroundTypeId() != targetBgType)
            continue;

        WorldSession const* session = participant->GetSession();
        bool const isVirtualSession = session && session->IsVirtualSession();
        if (!isVirtualSession && !playerbot::IsManagedRandomBot(participant))
            return true;
    }

    return false;
}

uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    std::vector<ObjectGuid> managedBotGuids;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
        {
            if (!participant || !participant->IsInWorld())
                continue;

            if (!IsScmManagedBotCandidate(participant))
                continue;

            managedBotGuids.push_back(guid);
        }
    }

    uint32 queuedCount = 0;
    for (ObjectGuid const& guid : managedBotGuids)
    {
        Player* managedBot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!managedBot)
            continue;

        // Mass-queue paths should also normalize stale battleground state for
        // each candidate; otherwise a subset of bots can remain perpetually
        // ineligible until their own lifecycle tick reaches recovery.
        RecoverStaleBattlegroundState(managedBot);

        if (QueuePlayer(managedBot, bgTypeId, arenaType))
            ++queuedCount;
    }

    return queuedCount;
}

}

namespace playerbot
{
uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    return ::QueueEligibleManagedBotsForBattleground(bgTypeId, arenaType);
}

void FinalizeManagedBotTeleportIfPending(Player* player)
{
    if (!player)
        return;

    if (!playerbot::IsManagedRandomBot(player))
        return;

    WorldSession* session = player->GetSession();
    if (!session || !session->IsVirtualSession())
        return;

    if (player->IsBeingTeleportedFar())
        session->HandleMoveWorldportAck();

    if (!player->IsBeingTeleportedNear())
        return;

    WorldPacket teleportAck(MSG_MOVE_TELEPORT_ACK, 20);
    teleportAck << player->GetPackGUID();
    teleportAck << uint32(0);
    teleportAck << uint32(0);
    session->HandleMoveTeleportAck(teleportAck);

    if (!player->IsBeingTeleportedNear())
        return;

    uint32 const oldZone = player->GetZoneId();
    WorldLocation destination = player->GetTeleportDest();
    float safeDestinationZ = destination.GetPositionZ();
    player->UpdateAllowedPositionZ(destination.GetPositionX(), destination.GetPositionY(), safeDestinationZ);
    destination.Relocate(destination.GetPositionX(), destination.GetPositionY(), safeDestinationZ, destination.GetOrientation());
    player->SetSemaphoreTeleportNear(false);
    player->UpdatePosition(destination, true);
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

uint64 BuildBattlegroundInstanceKey(Battleground const* battleground)
{
    if (!battleground)
        return 0;

    return (uint64(battleground->GetMapId()) << 32) | uint64(battleground->GetInstanceID());
}

bool ShouldDeferBattlegroundLeaveForTeleportAck(Player const* player)
{
    if (!player)
        return false;

    if (!player->IsBeingTeleportedFar() && !player->IsBeingTeleportedNear())
        return false;

    // Managed bots can retain stale near/far teleport semaphores inside
    // battleground instances; do not deadlock leave/end cleanup on those flags.
    if (player->InBattleground() && playerbot::IsManagedRandomBot(player))
        return false;

    WorldSession const* session = player->GetSession();
    if (session && session->IsVirtualSession() && player->InBattleground())
        return false;

    return true;
}

std::unordered_set<uint64> g_WaitJoinLockedBots;

void SetWaitJoinMovementLock(Player* player, bool locked)
{
    if (!player)
        return;

    uint64 const botGuid = player->GetGUID().GetRawValue();
    bool const alreadyLocked = g_WaitJoinLockedBots.find(botGuid) != g_WaitJoinLockedBots.end();

    if (!locked)
    {
        if (alreadyLocked)
        {
            player->SetControlled(false, UNIT_STATE_ROOT);
            g_WaitJoinLockedBots.erase(botGuid);
        }
        return;
    }

    player->AttackStop();
    player->SetSelection(ObjectGuid::Empty);

    if (player->isMoving())
        player->StopMoving();

    if (MotionMaster* motionMaster = player->GetMotionMaster())
        motionMaster->Clear();

    if (!alreadyLocked)
    {
        player->SetControlled(true, UNIT_STATE_ROOT);
        g_WaitJoinLockedBots.insert(botGuid);
    }
}

bool ForceHoldPlayerAtStartDuringWaitJoin(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    Battleground* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    if (battleground->GetStatus() != STATUS_WAIT_JOIN)
    {
        SetWaitJoinMovementLock(player, false);
        return false;
    }

    uint32 const assignedTeam = battleground->GetPlayerTeam(player->GetGUID());
    TeamId const teamId = ResolveTeamId(assignedTeam ? assignedTeam : player->GetBGTeam());
    TeamId const startTeam = (teamId == TEAM_NEUTRAL) ? player->GetTeamId() : teamId;
    Position const* start = battleground->GetTeamStartPosition(startTeam);
    if (!start)
        return false;

    SetWaitJoinMovementLock(player, true);

    float const dist = player->GetDistance(
        start->GetPositionX(),
        start->GetPositionY(),
        start->GetPositionZ());

    // Hard correction: they should not be moving at all before the battleground starts.
    if (dist > 1.0f)
    {
        player->NearTeleportTo(
            start->GetPositionX(),
            start->GetPositionY(),
            start->GetPositionZ(),
            start->GetOrientation());
    }

    if (player->isMoving())
        player->StopMoving();

    if (MotionMaster* motionMaster = player->GetMotionMaster())
        motionMaster->Clear();

    return true;
}

bool TryReturnDroppedFriendlyFlagWithHumanPriority(Player* player)
{
    if (!player || !player->InBattleground())
        return false;
    if (!CanIssueBotMovement(player))
        return false;

    BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(player->GetBattleground());
    if (!bgWs || bgWs->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    GameObject* droppedFlag = GetFriendlyDroppedWsgFlag(player, bgWs);
    if (!droppedFlag)
        return false;

    uint64 const botRawGuid = player->GetGUID().GetRawValue();
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32& attemptNotBeforeMs = g_WsgReturnAttemptNotBeforeMsByGuid[botRawGuid];

    if (HumanTeammateNearDroppedFlag(player, droppedFlag, 7.0f))
    {
        attemptNotBeforeMs = nowMs + urand(700, 1300);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP WSG return yielded: guid={} reason=nearby-human-priority wait_until_ms={}.",
            player->GetGUID().ToString(), attemptNotBeforeMs);
        return false;
    }

    if (attemptNotBeforeMs == 0)
    {
        attemptNotBeforeMs = nowMs + urand(350, 900);
        return false;
    }

    if (nowMs < attemptNotBeforeMs)
        return false;

    if (!player->IsWithinDistInMap(droppedFlag, 10.0f))
    {
        return IssueMovePointThrottled(player, droppedFlag->GetPosition(), 8.0f, 500);
    }

    bgWs->EventPlayerClickedOnFlag(player, droppedFlag);
    attemptNotBeforeMs = nowMs + urand(1200, 2200);
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP WSG return attempted: guid={} flag_guid={} next_attempt_ms={}.",
        player->GetGUID().ToString(), droppedFlag->GetGUID().ToString(), attemptNotBeforeMs);
    return true;
}

Player* FindNearestEnemyBattlegroundPlayer(Player* player, float maxDistance, uint32* scannedPlayers, uint32* attackableEnemies)
{
    if (!player || !player->InBattleground() || !player->FindMap())
        return nullptr;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    TeamId const playerBgTeam = ResolveBotTeamId(player);
    if (scannedPlayers)
        *scannedPlayers = 0;
    if (attackableEnemies)
        *attackableEnemies = 0;

    float nearestDistance = std::numeric_limits<float>::max();
    Player* nearestEnemy = nullptr;

    Map::PlayerList const& players = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (scannedPlayers)
            ++(*scannedPlayers);
        if (candidate->GetBattlegroundId() != player->GetBattlegroundId())
            continue;
        TeamId const candidateBgTeam = ResolveBotTeamId(candidate);
        if (candidateBgTeam == playerBgTeam)
            continue;
        if (!player->IsValidAttackTarget(candidate))
            continue;
        if (attackableEnemies)
            ++(*attackableEnemies);

        float const distance = player->GetDistance(candidate);
        if (distance > maxDistance || distance >= nearestDistance)
            continue;

        nearestDistance = distance;
        nearestEnemy = candidate;
    }

    return nearestEnemy;
}

Unit* AcquireCombatTarget(Player* player, float scanDistance)
{
    if (!player)
        return nullptr;

    auto isAttackableTarget = [player](Unit* candidate) -> bool
    {
        return candidate && candidate->IsAlive() && player->IsValidAttackTarget(candidate);
    };

    Unit* target = player->GetVictim();
    if (!isAttackableTarget(target))
        target = player->GetSelectedUnit();
    if ((!target || !target->IsAlive()) && player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS)
    {
        Unit* duelOpponent = player->duel->Opponent;
        if (isAttackableTarget(duelOpponent) && duelOpponent->GetMapId() == player->GetMapId())
            target = duelOpponent;
    }
    if (!isAttackableTarget(target) && player->InBattleground())
        target = FindNearestEnemyBattlegroundPlayer(player, scanDistance, nullptr, nullptr);
    if (!isAttackableTarget(target))
        return nullptr;

    player->SetSelection(target->GetGUID());
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP chosen combat target: bot={} target={} class={} distance={}.",
        player->GetGUID().ToString(), target->GetGUID().ToString(), uint32(player->GetClass()), player->GetDistance(target));
    return target;
}

bool DriveCombatPositioning(Player* player, Unit* target, CombatPositioningProfile const& profile)
{
    if (!player || !target || !target->IsAlive() || !CanIssueBotMovement(player))
        return false;

    // Class spell actions can issue target-relative chase/follow in the same
    // scheduler frame. Do not immediately override those orders from lifecycle
    // distance-band helpers (follow/stop), or bots can visibly inch/stop.
    if (playerbot::PvpClassActions::HasRecentTargetRelativeMovementOrder(player, nullptr, 1500))
        return true;

    float const distance = player->GetDistance(target);
    bool const hasLos = player->IsWithinLOSInMap(target);
    if (!hasLos)
        return TryRecoverLineOfSight(player, target, profile, "drive-combat-positioning");

    if (profile.primarilyRanged)
    {
        if (ShouldForceMeleeFallbackOnLowMana(player))
        {
            if (!CanIssueMovementCommand(player, 500))
                return true;

            return MoveTowardUnit(player, target, std::max(1.0f, playerbot::PvpCore::GetConfig().meleeRange - 1.0f));
        }

        if (player->GetClass() == CLASS_HUNTER)
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint64 const hunterGuidRaw = player->GetGUID().GetRawValue();
            uint32& pauseUntilMs = g_HunterAutoShotPauseUntilMs[hunterGuidRaw];
            if (pauseUntilMs > nowMs)
            {
                player->StopMoving();
                return true;
            }

            SpellInfo const* autoShotInfo = sSpellMgr->GetSpellInfo(75);
            if (autoShotInfo)
            {
                float const minAutoShotRange = autoShotInfo->GetMinRange(false);
                float const maxAutoShotRange = autoShotInfo->GetMaxRange(false);
                bool const canAutoShotWithoutDeadzone = distance > minAutoShotRange && distance <= maxAutoShotRange;
                uint32 const autoShotTimerMs = player->getAttackTimer(RANGED_ATTACK);
                if (canAutoShotWithoutDeadzone && autoShotTimerMs <= 600)
                {
                    uint32 const pauseDurationMs = std::max<uint32>(autoShotTimerMs, 150);
                    pauseUntilMs = nowMs + pauseDurationMs;

                    ObjectGuid const hunterGuid = player->GetGUID();
                    ObjectGuid const targetGuid = target->GetGUID();
                    float const followDistance = profile.preferredIdealRange;
                    float const followAngle = player->GetFollowAngle();

                    player->StopMoving();
                    if (WorldSession* session = player->GetSession(); session && session->IsVirtualSession())
                    {
                        player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
                        player->SendMovementFlagUpdate();
                    }

                    player->m_Events.AddEventAtOffset([hunterGuid, targetGuid, followDistance, followAngle]()
                    {
                        Player* hunter = ObjectAccessor::FindConnectedPlayer(hunterGuid);
                        if (!hunter || !hunter->IsInWorld() || !hunter->IsAlive())
                            return;

                        if (Unit* resumedTarget = ObjectAccessor::GetUnit(*hunter, targetGuid))
                        {
                            if (resumedTarget->IsAlive())
                                IssueHumanLikeFollow(hunter, resumedTarget, followDistance, 6.0f, 500);
                        }
                    }, std::chrono::milliseconds(pauseDurationMs));

                    return true;
                }
            }
        }

        if (profile.createDistanceWhenCrowded && IsActivelyPressuringInMelee(target, player) && distance < profile.preferredIdealRange)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=create-distance-vs-melee-pressure distance={} min={} ideal={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                profile.preferredMaxPressureRange);
            return MoveAwayFromUnit(player, target, profile.preferredIdealRange);
        }

        if (distance < profile.preferredMinRange && profile.createDistanceWhenCrowded)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=create-distance distance={} min={} ideal={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                profile.preferredMaxPressureRange);
            return MoveAwayFromUnit(player, target, profile.preferredIdealRange);
        }

        if (distance > profile.preferredMaxPressureRange)
        {
            if (!CanIssueMovementCommand(player, 500))
                return true;
            if (!IssueHumanLikeFollow(player, target, profile.preferredIdealRange, 6.0f, 500))
                return true;
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=close-distance distance={} min={} ideal={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                profile.preferredMaxPressureRange);
            return true;
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP distance band: bot={} profile={} decision=hold-band distance={} min={} ideal={} max={}.",
            player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
            profile.preferredMaxPressureRange);

        // Ranged bots should fully settle once they are inside their preferred
        // firing band. Continuously following in-band keeps movement active and
        // can suppress Auto Shot firing windows for hunters.
        player->StopMoving();
        if (WorldSession* session = player->GetSession(); session && session->IsVirtualSession())
        {
            player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            player->SendMovementFlagUpdate();
        }
        return true;
    }

    if (distance > profile.preferredMaxPressureRange || !player->IsWithinMeleeRange(target))
    {
        bool const isStealthedRogue = player->GetClass() == CLASS_ROGUE && player->HasStealthAura();
        if (!isStealthedRogue && !CanIssueMovementCommand(player, 500))
            return true;

        ClearEatDrinkAurasForMovement(player);

        if (isStealthedRogue)
        {
            // Stealth openers intentionally run without a committed victim for
            // part of the engage. MoveChase can pause when victim linkage is
            // absent, so use follow semantics to keep continuous closing.
            player->GetMotionMaster()->MoveFollow(target, 0.1f, player->GetFollowAngle());
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=stealth-melee-close-follow distance={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange);
            return true;
        }

        // Use core chase movement for non-stealth melee stickiness instead of
        // repeatedly recomputing follow points around the target.
        player->GetMotionMaster()->MoveChase(target);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP distance band: bot={} profile={} decision=melee-close distance={} max={}.",
            player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange);
        return true;
    }

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP distance band: bot={} profile={} decision=melee-stick distance={} max={}.",
        player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange);
    return true;
}

bool EngageNearestEnemyPlayer(Player* player, float scanDistance)
{
    if (!player || !player->IsAlive())
        return false;

    if (IsCrowdControlledForAction(player))
    {
        player->AttackStop();
        return false;
    }

    Unit* target = AcquireCombatTarget(player, scanDistance);
    if (!target)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP movement skipped: bot={} reason=no-combat-target scanDistance={}.",
            player ? player->GetGUID().ToString() : ObjectGuid::Empty.ToString(), scanDistance);
        return false;
    }

    // Ensure mounted bots immediately transition into combat posture once an
    // enemy target is acquired. Without this, bots that don't cast right away
    // (or rely on melee/auto attacks) can stay mounted and fail to engage.
    if (player->IsMounted())
        ForcePlayerbotDismount(player);

    CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
    bool const useMeleeAttack = !profile.primarilyRanged || profile.meleeFallbackAcceptable;
    bool const isStealthedRogue = player->GetClass() == CLASS_ROGUE && player->HasStealthAura();
    bool const targetInBreakableCrowdControl = target->HasBreakableByDamageCrowdControlAura();
    bool const alreadyAttackingTarget = player->GetVictim() && player->GetVictim()->GetGUID() == target->GetGUID();
    bool const meleeAutoAttackActive = player->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
    if (isStealthedRogue)
        player->AttackStop();
    else if (targetInBreakableCrowdControl)
    {
        if (alreadyAttackingTarget)
            player->AttackStop();
    }
    else if (!alreadyAttackingTarget || !meleeAutoAttackActive)
        player->Attack(target, useMeleeAttack);

    if (player->GetClass() == CLASS_HUNTER && profile.primarilyRanged && player->HasSpell(75))
    {
        Spell const* autoRepeatSpell = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        bool const autoShotActive = autoRepeatSpell && autoRepeatSpell->GetSpellInfo()->Id == 75;

        bool inAutoShotRange = false;
        if (SpellInfo const* autoShotInfo = sSpellMgr->GetSpellInfo(75))
        {
            float const minAutoShotRange = autoShotInfo->GetMinRange(false);
            float const maxAutoShotRange = autoShotInfo->GetMaxRange(false);
            float const distance = player->GetDistance(target);
            inAutoShotRange = player->IsWithinLOSInMap(target) && distance > minAutoShotRange && distance <= maxAutoShotRange;
        }

        if (!inAutoShotRange)
        {
            if (autoShotActive)
                player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        }
        else if (!autoShotActive)
            player->CastSpell(target, 75, false);
    }

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP positioning profile: bot={} profile={} ranged={} createDistance={} meleeFallback={}.",
        player->GetGUID().ToString(), profile.label, profile.primarilyRanged, profile.createDistanceWhenCrowded,
        profile.meleeFallbackAcceptable);

    return DriveCombatPositioning(player, target, profile);
}

void ApplyDeterministicObjectiveOffset(Battleground const* battleground, Player const* player, Position& destination)
{
    if (!battleground || !player)
        return;

    // Keep each bot slightly spread out, but stable across updates, to avoid
    // objective destination churn that causes oscillating movement.
    uint64 const seed = player->GetGUID().GetRawValue() ^ (uint64(battleground->GetMapId()) << 32) ^ battleground->GetInstanceID();
    float const angle = float(seed % 6283) / 1000.0f;
    float const radius = 2.0f + float((seed / 6283) % 600) / 100.0f; // [2.0, 8.0)
    destination.RelocateOffset(Position(std::cos(angle) * radius, std::sin(angle) * radius, 0.0f, 0.0f));
}

bool TryGetObjectivePosition(Battleground* battleground, Player* player, Position& destination)
{
    if (!battleground || !player)
        return false;

    if (IsWarsongGulch(player))
    {
        // Midfield brawl behavior for WSG: collapse both teams toward center map.
        Position const midfieldAnchor(1239.40f, 1543.60f, 306.00f, 0.0f);
        destination = midfieldAnchor;
        ApplyDeterministicObjectiveOffset(battleground, player, destination);
        return true;
    }

    Position const* allianceStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(TEAM_ALLIANCE));
    Position const* hordeStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(TEAM_HORDE));

    if (allianceStart && hordeStart)
    {
        float const distanceToAllianceStart = player->GetDistance(allianceStart->GetPositionX(), allianceStart->GetPositionY(), allianceStart->GetPositionZ());
        float const distanceToHordeStart = player->GetDistance(hordeStart->GetPositionX(), hordeStart->GetPositionY(), hordeStart->GetPositionZ());
        destination = (distanceToAllianceStart > distanceToHordeStart) ? Position(*allianceStart) : Position(*hordeStart);
        ApplyDeterministicObjectiveOffset(battleground, player, destination);
        return true;
    }

    TeamId const botTeam = ResolveBotTeamId(player);
    TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
    if (Position const* enemyStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(enemyTeam)))
    {
        destination = Position(*enemyStart);
        ApplyDeterministicObjectiveOffset(battleground, player, destination);
        return true;
    }

    return false;
}
}

namespace playerbot
{
bool BattlegroundLifecycleActions::Execute(Player* player, BattlegroundLifecycleContext const& context)
{
    if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
        return false;

    if (RecoverStaleBattlegroundState(player))
        return true;

    if (HasConflictingBattlegroundLifecycleContext(context))
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP battleground lifecycle no-op due to conflicting context: guid={}, queueOperation={}, invitationResponse={}, handleInProgress={}.",
            player->GetGUID().ToString(), static_cast<uint8>(context.queueOperation), static_cast<uint8>(context.invitationResponse),
            context.shouldHandleInProgressStatus ? 1 : 0);
        return false;
    }

    if (ShouldRefreshLongQueuedBot(player))
    {
        EmitLifecycleDiagnostic(player, "queue-timeout-requeue",
            "Queue wait exceeded 15s without invite; leaving queue and requeueing.");
        bool const leftQueue = LeaveQueuePrimitive(player);
        bool const requeued = JoinQueuePrimitive(player);
        return leftQueue || requeued;
    }

    bool didExecute = false;

    switch (context.queueOperation)
    {
        case QueueOperationType::Join:
            didExecute = JoinQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::Leave:
            didExecute = LeaveQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::None:
        default:
            break;
    }

    switch (context.invitationResponse)
    {
        case InvitationResponseType::Accept:
            didExecute = AcceptInvitePrimitive(player) || didExecute;
            break;
        case InvitationResponseType::Decline:
            didExecute = DeclineInvitePrimitive(player) || didExecute;
            break;
        case InvitationResponseType::None:
        default:
            break;
    }

    if (context.shouldHandleInProgressStatus)
        didExecute = HandleInProgressStatusPrimitive(player) || didExecute;

    return didExecute;
}

bool BattlegroundLifecycleActions::JoinQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    constexpr BattlegroundTypeId kManagedBattleground = BATTLEGROUND_SCM;
    uint32 const nowMs = GameTime::GetGameTimeMS();
    bool const hasHumanInterest = HasAnyRealHumanInterestInBattleground(kManagedBattleground);

    if (!hasHumanInterest)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle queue join blocked: no real human interest detected: guid={} bgTypeId={}.",
            player->GetGUID().ToString(), uint32(kManagedBattleground));
        return false;
    }

    // When real human interest exists (especially right after startup), force
    // an immediate population rebalance so additional managed bots can log in.
    if (hasHumanInterest &&
        nowMs >= g_LastHumanInterestPopulationRebalanceAttemptMs + PLAYERBOT_BG_HUMAN_INTEREST_REBALANCE_THROTTLE_MS)
    {
        g_LastHumanInterestPopulationRebalanceAttemptMs = nowMs;
        bool const rebalanceTriggered = RandomBotParticipationManager::TriggerImmediateRebalance();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle human-interest rebalance: guid={} bgTypeId={} triggered={}.",
            player->GetGUID().ToString(), uint32(kManagedBattleground), rebalanceTriggered ? 1 : 0);
    }

    uint32 const massQueued = QueueEligibleManagedBotsForBattleground(kManagedBattleground, 0);
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP lifecycle mass queue attempt: guid={} bgTypeId={} queuedCount={}.",
        player->GetGUID().ToString(), uint32(kManagedBattleground), massQueued);
    if (massQueued > 0)
        return true;

    return QueuePlayer(player, kManagedBattleground, 0);
}

bool BattlegroundLifecycleActions::LeaveQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return RemoveMatchingQueues(player, false, false, true);
}

bool BattlegroundLifecycleActions::AcceptInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return AcceptMatchingInvite(player, false);
}

bool BattlegroundLifecycleActions::DeclineInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return RemoveMatchingQueues(player, false, true, true);
}

bool BattlegroundLifecycleActions::HandleInProgressStatusPrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!player->InBattleground())
        return false;

    Battleground* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    if (battleground->GetStatus() != STATUS_WAIT_JOIN)
        SetWaitJoinMovementLock(player, false);

    if (battleground->GetStatus() == STATUS_WAIT_JOIN)
    {
        ForceHoldPlayerAtStartDuringWaitJoin(player);

        if (!HasAnyRealHumanInterestInBattleground(battleground->GetTypeID()))
        {
            battleground->EndBattleground(PVP_TEAM_NEUTRAL);
            g_BattlegroundNoHumanSinceMsByInstance.erase(BuildBattlegroundInstanceKey(battleground));
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP lifecycle wait-join end due to no real human battleground interest: guid={} bgTypeId={} instanceId={}.",
                player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
            return true;
        }

        uint64 const battlegroundInstanceKey = BuildBattlegroundInstanceKey(battleground);
        if (!BattlegroundHasAnyRealHumanPlayers(player))
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint32& noHumanSinceMs = g_BattlegroundNoHumanSinceMsByInstance[battlegroundInstanceKey];
            if (!noHumanSinceMs)
                noHumanSinceMs = nowMs;

            if (nowMs >= noHumanSinceMs + PLAYERBOT_BG_WAIT_JOIN_NO_HUMAN_END_DELAY_MS && !ShouldDeferBattlegroundLeaveForTeleportAck(player))
            {
                battleground->EndBattleground(PVP_TEAM_NEUTRAL);
                g_BattlegroundNoHumanSinceMsByInstance.erase(battlegroundInstanceKey);
                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP lifecycle wait-join end due to no real humans: guid={} bgTypeId={} instanceId={}.",
                    player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
                return true;
            }
        }
        else
            g_BattlegroundNoHumanSinceMsByInstance.erase(battlegroundInstanceKey);

        TryRefillManagedScmSlots(player, battleground);
        return true;
    }

    if (battleground->GetStatus() == STATUS_WAIT_LEAVE)
    {
        SetWaitJoinMovementLock(player, false);
        g_BattlegroundNoHumanSinceMsByInstance.erase(BuildBattlegroundInstanceKey(battleground));

        if (ShouldDeferBattlegroundLeaveForTeleportAck(player))
            return false;

        player->LeaveBattleground();
        FinalizeManagedBotTeleportIfPending(player);
        if (playerbot::IsManagedRandomBot(player))
            player->RemoveAurasDueToSpell(SPELL_DESERTER);
        RemoveMatchingQueues(player, false, false, true);
        RemoveMatchingQueues(player, true, false, false);
        player->SetArenaTeamIdInvited(0);
        EmitLifecycleDiagnostic(player, "wait-leave-cleanup", "Post-leave cleanup complete before returning to scheduler flow.");

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle leave after battleground end: guid={} bgTypeId={} instanceId={}",
            player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
        return true;
    }

    if (battleground->GetStatus() != STATUS_IN_PROGRESS)
    {
        g_BattlegroundNoHumanSinceMsByInstance.erase(BuildBattlegroundInstanceKey(battleground));
        return false;
    }

    // Secondary guard for non-virtual managed bot accounts: core battleground
    // shutdown treats any non-virtual session as human, so these matches can
    // persist indefinitely after real humans leave.
    uint64 const battlegroundInstanceKey = BuildBattlegroundInstanceKey(battleground);
    if (!HasAnyRealHumanInterestInBattleground(battleground->GetTypeID()))
    {
        battleground->EndBattleground(PVP_TEAM_NEUTRAL);
        g_BattlegroundNoHumanSinceMsByInstance.erase(battlegroundInstanceKey);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle end due to no real human battleground interest: guid={} bgTypeId={} instanceId={}.",
            player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
        return true;
    }

    if (!BattlegroundHasAnyRealHumanPlayers(player))
    {
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& noHumanSinceMs = g_BattlegroundNoHumanSinceMsByInstance[battlegroundInstanceKey];
        if (!noHumanSinceMs)
            noHumanSinceMs = nowMs;

        if (nowMs >= noHumanSinceMs + PLAYERBOT_BG_NO_HUMAN_END_DELAY_MS && !ShouldDeferBattlegroundLeaveForTeleportAck(player))
        {
            battleground->EndBattleground(PVP_TEAM_NEUTRAL);
            g_BattlegroundNoHumanSinceMsByInstance.erase(battlegroundInstanceKey);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP lifecycle end due to no real human participants: guid={} bgTypeId={} instanceId={}.",
                player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
            return true;
        }
    }
    else
        g_BattlegroundNoHumanSinceMsByInstance.erase(battlegroundInstanceKey);

    TryRefillManagedScmSlots(player, battleground);

    if (ShouldManagedBotLeaveForQueuedHuman(player, battleground))
    {
        player->LeaveBattleground();
        FinalizeManagedBotTeleportIfPending(player);
        player->RemoveAurasDueToSpell(SPELL_DESERTER);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP human-priority departure trigger: guid={} bgTypeId={} instanceId={} players={} maxPlayers={}.",
            player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID(),
            battleground->GetPlayersSize(), battleground->GetMaxPlayers());
        return true;
    }

    if (ShouldManagedBotLeaveForOverstack(player, battleground))
    {
        player->LeaveBattleground();
        FinalizeManagedBotTeleportIfPending(player);
        player->RemoveAurasDueToSpell(SPELL_DESERTER);
        QueuePlayer(player, BATTLEGROUND_SCM, 0);
        return true;
    }

    if (HandleBattlegroundDeathState(player))
        return true;

    ClearStaleWaitingForResurrectAura(player);

    if (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION))
    {
        player->RemoveAurasDueToSpell(SPELL_PREPARATION);
        player->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
        player->RemoveUnitFlag(UNIT_FLAG_PREPARATION);
    }

    // Reference module parity: in-progress movement/combat are handled by
    // tactical actions (move to objective / check objective), not lifecycle.
    return false;
}

bool BattlegroundTacticalActions::Execute(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !context.tacticsEnabled || !context.shouldEvaluate || !context.actionName)
        return false;

    if (!player->IsAlive())
        return false;

    ClearStaleWaitingForResurrectAura(player);

    if (IsRecoveringByEatingOrDrinking(player))
    {
        if (player->isMoving())
            player->StopMoving();
        return true;
    }

    if (Battleground* battleground = player->GetBattleground())
    {
        // During prep phase only process the explicit start-position action.
        // This prevents competing tactical actions from pulling bots back/forth.
        if (battleground->GetStatus() == STATUS_WAIT_JOIN)
        {
            if (IsTacticalAction(context.actionName, "bg move to start"))
                return MoveToStartPrimitive(player);
            return false;
        }
    }

    if (IsTacticalAction(context.actionName, "bg move to start"))
        return MoveToStartPrimitive(player);
    if (IsTacticalAction(context.actionName, "bg move to objective"))
        return MoveToObjectivePrimitive(player, context);
    if (IsTacticalAction(context.actionName, "bg check objective"))
        return CheckObjectivePrimitive(player, context);
    if (IsTacticalAction(context.actionName, "bg reset objective force"))
        return ResetObjectiveForcePrimitive(player);
    if (IsTacticalAction(context.actionName, "bg use buff"))
        return UseBuffPrimitive(player);
    if (IsTacticalAction(context.actionName, "attack enemy flag carrier"))
        return AttackEnemyFlagCarrierPrimitive(player, context);
    if (IsTacticalAction(context.actionName, "bg protect fc"))
        return ProtectFlagCarrierPrimitive(player, context);

    return false;
}

bool BattlegroundTacticalActions::MoveToStartPrimitive(Player* player)
{
    return ForceHoldPlayerAtStartDuringWaitJoin(player);
}

bool BattlegroundTacticalActions::MoveToObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() == STATUS_WAIT_JOIN)
        return false;

    if (!player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return false;

    bool const outdoors = IsEffectivelyOutdoors(player);
    if (player->IsMounted() && ShouldForceIndoorDismount(player, outdoors))
        ForcePlayerbotDismount(player);

    switch (player->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        case IDLE_MOTION_TYPE:
        case CHASE_MOTION_TYPE:
        case POINT_MOTION_TYPE:
        case FOLLOW_MOTION_TYPE:
        // Charge/Intercept traverse through EFFECT_MOTION_TYPE.
        case EFFECT_MOTION_TYPE:
            break;
        default:
        {
            // If the bot is effectively stationary while stuck in a non-tactical
            // movement generator, clear it so objective/pursuit movement can
            // issue a fresh MovePoint.
            if (!player->isMoving())
            {
                EmitBattlegroundGmDebug(player,
                    "move-to-objective=clear-stale-motion motionType=" +
                    std::to_string(uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType())), 1200);
                player->GetMotionMaster()->Clear();
                break;
            }

            return true;
        }
    }

    if (player->IsInCombat())
        return EngageNearestEnemyPlayer(player, GetAggressiveCombatScanDistance(player, 100.0f));

    if (TryPursueNearestEnemyInBattleground(player))
        return true;

    // Evaluate target pursuit before this guard so stale movement flags do not
    // suppress nearest-enemy pathing after battleground resurrection.
    if (player->isMoving())
        return false;

    if (context.objective.type == BattlegroundObjectiveType::None &&
        context.movement == BattlegroundMovementPrimitive::None &&
        context.flagCarrierDirective == FlagCarrierDirective::None)
    {
        if (battleground && battleground->GetTypeID() == BATTLEGROUND_SCM)
        {
            float const engageDistance = GetAggressiveCombatScanDistance(player, 100.0f);
            if (EngageNearestEnemyPlayer(player, engageDistance))
                return true;

            if (Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), nullptr, nullptr))
                return MoveTowardUnit(player, nearestEnemy, 20.0f);
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP movement skipped: bot={} reason=no-objective-and-no-directive.",
            player->GetGUID().ToString());
        return false;
    }

    if (context.movement == BattlegroundMovementPrimitive::MoveToObjectiveUnit ||
        context.movement == BattlegroundMovementPrimitive::FollowFlagCarrier ||
        context.flagCarrierDirective != FlagCarrierDirective::None)
    {
        if (Player* carrier = FindFlagCarrierForDirective(player, context.flagCarrierDirective))
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP objective support selected: guid={} directive={} target={}.",
                player->GetGUID().ToString(), static_cast<uint8>(context.flagCarrierDirective), carrier->GetGUID().ToString());
            return MoveTowardUnit(player, carrier, 20.0f);
        }
    }

    if (context.movement == BattlegroundMovementPrimitive::MoveToObjectivePosition)
    {
        if (battleground)
        {
            Position destination;
            if (TryGetObjectivePosition(battleground, player, destination))
            {
                bool const withinObjectiveRange = player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f);
                if (!withinObjectiveRange)
                    IssueMovePointThrottled(player, destination);
                else
                    EmitBattlegroundGmDebug(player, "objective-skip reason=already-near-objective range=12");
                return true;
            }

            if (battleground->GetTypeID() == BATTLEGROUND_SCM)
            {
                float const engageDistance = GetAggressiveCombatScanDistance(player, 100.0f);
                if (EngageNearestEnemyPlayer(player, engageDistance))
                    return true;

                if (Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), nullptr, nullptr))
                    return MoveTowardUnit(player, nearestEnemy, 20.0f);
            }

            return false;
        }
    }

    return false;
}

bool BattlegroundTacticalActions::CheckObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    (void)context;

    if (!player || !player->InBattleground())
        return false;

    if (TryPursueNearestEnemyInBattleground(player))
        return true;

    return MoveToClosestBattlegroundGraveyard(player);
}

bool BattlegroundTacticalActions::ResetObjectiveForcePrimitive(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    return true;
}

bool BattlegroundTacticalActions::UseBuffPrimitive(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    return false;
}

bool BattlegroundTacticalActions::AttackEnemyFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    if (context.flagCarrierDirective != FlagCarrierDirective::AttackEnemyCarrier)
        return false;

    Player* enemyCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::AttackEnemyCarrier);
    if (enemyCarrier)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP enemy-FC support selected: guid={} target={}.",
            player->GetGUID().ToString(), enemyCarrier->GetGUID().ToString());
    }
    return MoveTowardUnit(player, enemyCarrier, 15.0f);
}

bool BattlegroundTacticalActions::ProtectFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    if (context.flagCarrierDirective != FlagCarrierDirective::ProtectTeamCarrier)
        return false;

    Player* teamCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::ProtectTeamCarrier);
    if (teamCarrier)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP protect-FC support selected: guid={} target={}.",
            player->GetGUID().ToString(), teamCarrier->GetGUID().ToString());
    }
    return MoveTowardUnit(player, teamCarrier, 18.0f);
}

bool ArenaLifecycleActions::Execute(Player* player, ArenaLifecycleContext const& context)
{
    if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
        return false;

    if (RecoverStaleBattlegroundState(player))
        return true;

    if (HasConflictingArenaLifecycleContext(context))
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP arena lifecycle no-op due to conflicting context: guid={}, queueOperation={}, teamInteraction={}.",
            player->GetGUID().ToString(), static_cast<uint8>(context.queueOperation), static_cast<uint8>(context.teamInteraction));
        return false;
    }

    bool didExecute = false;

    switch (context.queueOperation)
    {
        case QueueOperationType::Join:
            didExecute = JoinQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::Leave:
            didExecute = LeaveQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::None:
        default:
            break;
    }

    switch (context.teamInteraction)
    {
        case ArenaTeamInteractionType::AcceptInvite:
            didExecute = AcceptTeamInvitePrimitive(player) || didExecute;
            break;
        case ArenaTeamInteractionType::DeclineInvite:
            didExecute = DeclineTeamInvitePrimitive(player) || didExecute;
            break;
        case ArenaTeamInteractionType::None:
        default:
            break;
    }

    // Warsong-only managed bot policy: never auto-accept arena invitations.
    return didExecute;
}

bool ArenaLifecycleActions::JoinQueuePrimitive(Player* player)
{
    (void)player;
    return false;
}

bool ArenaLifecycleActions::LeaveQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return RemoveMatchingQueues(player, true, false, false);
}

bool ArenaLifecycleActions::AcceptTeamInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    uint32 const invitedArenaTeamId = player->GetArenaTeamIdInvited();
    if (!invitedArenaTeamId)
        return false;

    ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(invitedArenaTeamId);
    if (!arenaTeam)
        return false;

    if (arenaTeam->GetMember(player->GetGUID()))
        return false;

    if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD) &&
        player->GetTeam() != sCharacterCache->GetCharacterTeamByGuid(arenaTeam->GetCaptain()))
        return false;

    return arenaTeam->AddMember(player->GetGUID());
}

bool ArenaLifecycleActions::DeclineTeamInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!player->GetArenaTeamIdInvited())
        return false;

    player->SetArenaTeamIdInvited(0);
    return true;
}

bool DuelTacticalActions::Execute(Player* player)
{
    if (!player || !player->IsAlive() || !CanIssueBotMovement(player))
        return false;

    if (!player->duel || player->duel->State != DUEL_STATE_IN_PROGRESS)
        return false;

    return EngageNearestEnemyPlayer(player, GetAggressiveCombatScanDistance(player, 100.0f));
}
}
