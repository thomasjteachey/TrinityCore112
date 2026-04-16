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
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT = 29073;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK = 22734;
constexpr uint32 SPELL_WAITING_FOR_RESURRECT = 2584;
constexpr uint32 SPELL_DESERTER = 26013;
std::unordered_map<uint64, uint32> g_BattlegroundOverstackRequeueCooldownUntilMsByGuid;
std::unordered_map<uint64, uint32> g_BattlegroundOverstackInstanceNextDepartureMsByInstance;
uint32 g_LastHumanInterestPopulationRebalanceAttemptMs = 0;
uint64 BuildBattlegroundInstanceKey(Battleground const* battleground);

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
    if (!player || !battleground || !playerbot::IsManagedRandomBot(player))
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

    uint64 const instanceKey = BuildBattlegroundInstanceKey(battleground);
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

    Map const* map = player->GetMap();
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

Player* FindNearestEnemyBattlegroundPlayer(Player* player, float maxDistance, uint32* scannedPlayers = nullptr, uint32* attackableEnemies = nullptr);
bool MoveTowardUnit(Player* player, Unit* target, float desiredDistance);
bool EngageNearestEnemyPlayer(Player* player, float scanDistance);
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

bool TryPursueNearestEnemyInWarsong(Player* player)
{
    if (!player || !IsWarsongGulch(player))
        return false;

    static std::unordered_map<uint64, ObjectGuid> chaseTargetByBotGuid;
    static std::unordered_map<uint64, uint32> chaseTargetSetMsByBotGuid;

    auto const isValidChaseTarget = [player](Player* candidate) -> bool
    {
        if (!candidate || candidate == player)
            return false;
        if (!candidate->IsAlive() || !candidate->IsInWorld())
            return false;
        if (candidate->GetMapId() != player->GetMapId())
            return false;
        if (candidate->GetBattlegroundId() != player->GetBattlegroundId())
            return false;
        return player->IsValidAttackTarget(candidate);
    };

    uint32 scannedPlayers = 0;
    uint32 attackableEnemies = 0;
    Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), &scannedPlayers, &attackableEnemies);
    uint64 const botGuid = player->GetGUID().GetRawValue();
    uint32 const nowMs = GameTime::GetGameTimeMS();
    Player* selectedEnemy = nearestEnemy;
    bool stickyTargetHeld = false;

    std::unordered_map<uint64, ObjectGuid>::const_iterator stickyItr = chaseTargetByBotGuid.find(botGuid);
    if (stickyItr != chaseTargetByBotGuid.end() && !stickyItr->second.IsEmpty())
    {
        Player* stickyEnemy = ObjectAccessor::FindConnectedPlayer(stickyItr->second);
        if (isValidChaseTarget(stickyEnemy))
        {
            uint32 const setMs = chaseTargetSetMsByBotGuid[botGuid];
            uint32 const stickyAgeMs = nowMs - setMs;
            float const stickyDist = player->GetDistance(stickyEnemy);
            float const nearestDist = nearestEnemy ? player->GetDistance(nearestEnemy) : std::numeric_limits<float>::max();
            bool const withinStickyWindow = stickyAgeMs < 6000;
            bool const nearestNotMeaningfullyBetter = nearestDist + 12.0f >= stickyDist;

            if (withinStickyWindow && nearestNotMeaningfullyBetter)
            {
                selectedEnemy = stickyEnemy;
                stickyTargetHeld = true;
            }
        }
    }

    if (!selectedEnemy)
    {
        chaseTargetByBotGuid.erase(botGuid);
        chaseTargetSetMsByBotGuid.erase(botGuid);
        EmitBattlegroundGmDebug(player,
            "wsg-pursuit=no-enemy scanned=" + std::to_string(scannedPlayers) +
            " attackable=" + std::to_string(attackableEnemies), 1500);
        return false;
    }

    if (!stickyTargetHeld || chaseTargetByBotGuid[botGuid] != selectedEnemy->GetGUID())
    {
        chaseTargetByBotGuid[botGuid] = selectedEnemy->GetGUID();
        chaseTargetSetMsByBotGuid[botGuid] = nowMs;
    }

    float const distanceToEnemy = player->GetDistance(selectedEnemy);
    float const combatEngageDistance = std::clamp(GetAggressiveCombatScanDistance(player, 100.0f), 25.0f, 60.0f);
    if (distanceToEnemy <= combatEngageDistance)
    {
        EmitBattlegroundGmDebug(player,
            "wsg-pursuit=engage target=" + selectedEnemy->GetName() +
            " dist=" + std::to_string(int32(distanceToEnemy)) +
            " scan=" + std::to_string(scannedPlayers) +
            " attackable=" + std::to_string(attackableEnemies) +
            " sticky=" + std::to_string(stickyTargetHeld ? 1 : 0), 1200);
        return EngageNearestEnemyPlayer(player, combatEngageDistance);
    }

    bool chaseIssued = MoveTowardUnit(player, selectedEnemy, 20.0f);
    if (!chaseIssued && CanIssueBotMovement(player))
    {
        // Last-resort kick in case pursuit helper declines movement while the
        // bot is otherwise free to move.
        chaseIssued = IssueMovePointThrottled(player, selectedEnemy->GetPosition(), 30.0f, 2000) || player->isMoving();
    }

    EmitBattlegroundGmDebug(player,
        "wsg-pursuit=chase target=" + selectedEnemy->GetName() +
        " dist=" + std::to_string(int32(distanceToEnemy)) +
        " scan=" + std::to_string(scannedPlayers) +
        " attackable=" + std::to_string(attackableEnemies) +
        " sticky=" + std::to_string(stickyTargetHeld ? 1 : 0) +
        " issued=" + std::to_string(chaseIssued ? 1 : 0), 1200);
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

bool TryJumpOffWarsongGraveyard(Player* player)
{
    if (!player || !IsWarsongGulch(player))
        return false;

    struct PostResurrectRouteState
    {
        uint32 battlegroundInstanceId = 0;
        bool wasAlive = true;
        bool active = false;
        uint8 phase = 0; // 0 = move to tip, 1 = run forward burst, 2 = move to mid
        uint32 forwardBurstEndMs = 0;
    };

    static std::unordered_map<uint64, PostResurrectRouteState> stateByGuid;
    PostResurrectRouteState& state = stateByGuid[player->GetGUID().GetRawValue()];

    Battleground* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    if (state.battlegroundInstanceId != battleground->GetInstanceID())
    {
        state = {};
        state.battlegroundInstanceId = battleground->GetInstanceID();
        state.wasAlive = player->IsAlive();
    }

    if (!player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        state.wasAlive = false;
        state.active = false;
        return false;
    }

    bool const justResurrected = !state.wasAlive;
    state.wasAlive = true;
    if (justResurrected)
    {
        state.active = true;
        state.phase = 0;
        state.forwardBurstEndMs = 0;
    }

    if (!state.active)
        return false;

    static Position const hordeGraveyardTip(1066.0946404f, 1380.843994f, 340.612305f, 0.0f);
    static Position const allianceGraveyardTip(1406.597412f, 1553.099121f, 343.533295f, 0.0f);
    static Position const midPoint(1258.810181f, 1463.801758f, 312.229401f, 0.0f);
    // Per-side anchors that point off the graveyard ledge into the field.
    static Position const hordeForwardAnchor(978.20f, 1427.10f, 335.20f, 0.0f);
    static Position const allianceForwardAnchor(1498.60f, 1484.30f, 340.20f, 0.0f);

    TeamId const teamId = ResolveBotTeamId(player);
    Position const& graveyardTip = (teamId == TEAM_HORDE) ? hordeGraveyardTip : allianceGraveyardTip;
    Position const& forwardAnchor = (teamId == TEAM_HORDE) ? hordeForwardAnchor : allianceForwardAnchor;

    if (state.phase == 0)
    {
        if (!player->IsWithinDist3d(graveyardTip.GetPositionX(), graveyardTip.GetPositionY(), graveyardTip.GetPositionZ(), 2.5f))
        {
            IssueMovePointThrottled(player, graveyardTip, 1.0f, 300);
            return true;
        }

        state.phase = 1;
    }

    if (state.phase == 1)
    {
        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (!state.forwardBurstEndMs)
            state.forwardBurstEndMs = nowMs + 1000;

        // Push straight off the graveyard ledge first, then route to mid.
        float const dx = forwardAnchor.GetPositionX() - graveyardTip.GetPositionX();
        float const dy = forwardAnchor.GetPositionY() - graveyardTip.GetPositionY();
        float const len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.001f)
        {
            state.phase = 2;
            return true;
        }

        float const forwardDistance = player->GetSpeed(MOVE_RUN) * 1.0f;
        Position forwardPoint(
            graveyardTip.GetPositionX() + (dx / len) * forwardDistance,
            graveyardTip.GetPositionY() + (dy / len) * forwardDistance,
            graveyardTip.GetPositionZ(),
            player->GetAbsoluteAngle(forwardAnchor.GetPositionX(), forwardAnchor.GetPositionY()));

        IssueMovePointThrottled(player, forwardPoint, 0.5f, 100);

        if (nowMs >= state.forwardBurstEndMs)
            state.phase = 2;
        return true;
    }

    if (state.phase == 2)
    {
        IssueMovePointThrottled(player, midPoint, 1.0f, 300);

        if (player->IsWithinDist3d(midPoint.GetPositionX(), midPoint.GetPositionY(), midPoint.GetPositionZ(), 6.0f))
            state.active = false;
        return true;
    }

    return true;
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

bool TryBuildBattlegroundSegmentDestination(Player* player, Position const& safeDestination, Position& segmentDestination, PathType* resolvedPathType = nullptr)
{
    if (!player)
        return false;

    auto const tryResolveDestination = [&](Position const& requestedDestination, Position& resolvedDestination, PathType* outPathType) -> bool
    {
        Position const collisionSafeDestination = BuildCollisionSafeDestination(player, requestedDestination);

        PathGenerator path(player);
        path.SetPathLengthLimit(90.0f);
        bool pathOk = path.CalculatePath(collisionSafeDestination.GetPositionX(), collisionSafeDestination.GetPositionY(), collisionSafeDestination.GetPositionZ(), true);
        PathType pathType = path.GetPathType();
        Movement::PointsArray points = path.GetPath();
        G3D::Vector3 actualEnd = path.GetActualEndPosition();

        if ((pathType & PATHFIND_SHORTCUT) != 0)
        {
            PathGenerator retryPath(player);
            retryPath.SetPathLengthLimit(90.0f);
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
        if (points.size() > 1)
        {
            G3D::Vector3 const& lastPoint = points.back();
            resolvedDestination.Relocate(lastPoint.x, lastPoint.y, lastPoint.z, collisionSafeDestination.GetOrientation());
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

    std::array<float, 6> const probeDistances =
    {
        24.0f,
        18.0f,
        12.0f,
        8.0f,
        5.0f,
        3.0f
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

bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold, uint32 minReissueMs)
{
    if (!player)
        return false;
    if (!CanIssueMovementCommand(player, 500))
        return false;

    ClearEatDrinkAurasForMovement(player);

    minReissueMs = std::max<uint32>(minReissueMs, 2000);

    if (IsWarsongGulch(player))
        minReissueMs = std::max<uint32>(minReissueMs, 2000);

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

    if (IsWarsongGulch(player) && botCurrentlyMoving && state.lastIssueMs != 0 &&
        nowMs < state.lastIssueMs + 8000 && player->GetDistance(state.lastDestination) > 10.0f)
    {
        return false;
    }

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
    if (currentMovement == FOLLOW_MOTION_TYPE || currentMovement == DISTRACT_MOTION_TYPE)
    {
        motionMaster->Clear();
    }
    else if (!botCurrentlyMoving && player->InBattleground() &&
        currentMovement != IDLE_MOTION_TYPE &&
        currentMovement != CHASE_MOTION_TYPE &&
        currentMovement != POINT_MOTION_TYPE)
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

    // Managed random bots can run on disconnected virtual sessions where RBAC
    // battleground permissions are not always populated like live client sessions.
    // Gate queue eligibility by battleground level + free queue slots instead.
    if (!player->GetBGAccessByLevel(bgTypeId) || !player->HasFreeBattlegroundQueueId())
        return false;

    BattlegroundQueueTypeId const bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    if (player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
        return false;

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
        WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
        packet << arenaType << uint8(0) << uint32(packetBgTypeId) << uint16(0x1F90) << uint8(1);
        session->HandleBattleFieldPortOpcode(packet);

        if (player->IsBeingTeleportedFar())
        {
            EmitLifecycleDiagnostic(player, "invite-accept-far-teleport-pending",
                "Issuing server-side HandleMoveWorldportAck for bot teleport finalization.");
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
    if (IsWarsongGulch(player) && distanceToTarget > desiredDistance)
    {
        Position destination = target->GetPosition();
        bool const moved = IssueMovePointThrottled(player, destination, 30.0f, 2000);
        EmitBattlegroundGmDebug(player,
            "move-toward-unit mode=segmented target=" + target->GetName() +
            " dist=" + std::to_string(int32(distanceToTarget)) +
            " issued=" + std::to_string(moved ? 1 : 0), 1200);
        return moved || player->isMoving();
    }

    CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
    if (!player->IsWithinLOSInMap(target))
        return TryRecoverLineOfSight(player, target, profile, "move-toward-unit");

    // WSG should not avoid fall damage while pursuing enemies.
    if (IsWarsongGulch(player) &&
        target->GetPositionZ() + 6.0f < player->GetPositionZ() &&
        player->IsWithinLOS(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()))
    {
        if (!CanIssueMovementCommand(player, 500))
            return false;
        ClearEatDrinkAurasForMovement(player);
        return IssueMovePointThrottled(player, target->GetPosition(), 30.0f, 500);
    }

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
    if (!player || !bgWs || !player->GetMap())
        return nullptr;

    TeamId const botBgTeam = ResolveBotTeamId(player);
    if (bgWs->GetFlagState(botBgTeam) != BG_WS_FLAG_STATE_ON_GROUND)
        return nullptr;

    ObjectGuid const droppedFlagGuid = bgWs->GetDroppedFlagGUID(botBgTeam);
    if (droppedFlagGuid.IsEmpty())
        return nullptr;

    return player->GetMap()->GetGameObject(droppedFlagGuid);
}

bool HumanTeammateNearDroppedFlag(Player* player, GameObject const* droppedFlag, float veryCloseDistance)
{
    if (!player || !droppedFlag || !player->GetMap())
        return false;

    TeamId const botBgTeam = ResolveBotTeamId(player);
    float const botDistance = player->GetDistance(droppedFlag);

    Map::PlayerList const& players = player->GetMap()->GetPlayers();
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
    if (!player || !player->InBattleground() || !player->GetMap())
        return false;

    uint32 const battlegroundId = player->GetBattlegroundId();
    Map::PlayerList const& players = player->GetMap()->GetPlayers();
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

uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    std::vector<ObjectGuid> managedBotGuids;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
        {
            if (!participant || !participant->IsInWorld())
                continue;

            if (!playerbot::IsManagedRandomBot(participant))
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

        if (QueuePlayer(managedBot, bgTypeId, arenaType))
            ++queuedCount;
    }

    return queuedCount;
}

void FinalizeVirtualBotTeleportIfPending(Player* player)
{
    if (!player)
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
    if (!player || !player->InBattleground() || !player->GetMap())
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

    Map::PlayerList const& players = player->GetMap()->GetPlayers();
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
        target = FindNearestEnemyBattlegroundPlayer(player, scanDistance);
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

    float const distance = player->GetDistance(target);
    bool const hasLos = player->IsWithinLOSInMap(target);
    if (!hasLos)
        return TryRecoverLineOfSight(player, target, profile, "drive-combat-positioning");

    if (profile.primarilyRanged)
    {
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
        bool const forceStealthRogueChase = player->GetClass() == CLASS_ROGUE && player->HasStealthAura();
        if (!forceStealthRogueChase && !CanIssueMovementCommand(player, 500))
            return true;
        // Use core chase movement for melee stickiness instead of repeatedly
        // recomputing follow points around the target. This avoids oscillation
        // where rogues can appear to peel away before re-engaging.
        ClearEatDrinkAurasForMovement(player);
        player->GetMotionMaster()->MoveChase(target);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP distance band: bot={} profile={} decision=melee-close distance={} max={} forceStealthRogueChase={}.",
            player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange, forceStealthRogueChase ? 1 : 0);
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

    if (!HasAnyRealHumanInterestInBattleground(kManagedBattleground))
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle queue join suppressed due to no real human interest: guid={} bgTypeId={}.",
            player->GetGUID().ToString(), uint32(kManagedBattleground));
        return false;
    }

    // When a real human queues (especially right after startup), force an
    // immediate population rebalance so additional managed bots can log in and
    // participate in SCM fill without waiting for the periodic rebalance tick.
    if (nowMs >= g_LastHumanInterestPopulationRebalanceAttemptMs + PLAYERBOT_BG_HUMAN_INTEREST_REBALANCE_THROTTLE_MS)
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

        return true;
    }

    if (battleground->GetStatus() == STATUS_WAIT_LEAVE)
    {
        SetWaitJoinMovementLock(player, false);
        g_BattlegroundNoHumanSinceMsByInstance.erase(BuildBattlegroundInstanceKey(battleground));

        if (ShouldDeferBattlegroundLeaveForTeleportAck(player))
            return false;

        player->LeaveBattleground();
        FinalizeVirtualBotTeleportIfPending(player);
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

    if (ShouldManagedBotLeaveForOverstack(player, battleground))
    {
        player->LeaveBattleground();
        FinalizeVirtualBotTeleportIfPending(player);
        player->RemoveAurasDueToSpell(SPELL_DESERTER);
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
    if (TryJumpOffWarsongGraveyard(player))
        return true;

    if (TryPursueNearestEnemyInWarsong(player))
        return true;

    // Evaluate combat/WSG post-res logic before this guard so stale movement
    // flags do not suppress target pursuit immediately after graveyard rez.
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

            if (Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max()))
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

                if (Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max()))
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

    if (IsWarsongGulch(player))
    {
        if (EngageNearestEnemyPlayer(player, 60.0f))
            return true;

        if (TryPursueNearestEnemyInWarsong(player) || MoveToClosestBattlegroundGraveyard(player))
            return true;

        return false;
    }

    float const engageDistance = GetAggressiveCombatScanDistance(player, 100.0f);
    if (EngageNearestEnemyPlayer(player, engageDistance))
        return true;

    if (Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max()))
        return MoveTowardUnit(player, nearestEnemy, 20.0f);

    return false;
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
