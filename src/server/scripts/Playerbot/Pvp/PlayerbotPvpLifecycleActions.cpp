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

#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "Time/GameTime.h"
#include "GameObject.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Opcodes.h"
#include "ObjectAccessor.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Util.h"
#include "Containers.h"

#include <cstring>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <array>
#include <algorithm>
#include <queue>
#include <vector>

namespace
{
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

bool TryGetWarsongEnemyBasePosition(Player* player, Position& destination)
{
    if (!player || !IsWarsongGulch(player))
        return false;

    TeamId const botTeam = ResolveBotTeamId(player);

    Position const allianceFlagStand(1540.423f, 1481.325f, 351.8284f, 3.089233f);
    Position const hordeFlagStand(916.0226f, 1434.405f, 345.413f, 0.01745329f);

    destination = (botTeam == TEAM_ALLIANCE) ? hordeFlagStand : allianceFlagStand;
    return true;
}

bool TryGetWarsongBaseExitWaypoint(Player* player, Position& waypointOut)
{
    if (!player || !IsWarsongGulch(player))
        return false;

    TeamId const botTeam = ResolveBotTeamId(player);
    Position const allianceFlagRoom(1519.53f, 1481.87f, 352.024f, 0.0f);
    Position const hordeFlagRoom(933.331f, 1433.72f, 345.536f, 0.0f);
    Position const allianceExit(1508.27f, 1493.17f, 352.005f, 0.0f);
    Position const hordeExit(944.859f, 1423.05f, 345.437f, 0.0f);

    Position const& baseAnchor = (botTeam == TEAM_ALLIANCE) ? allianceFlagRoom : hordeFlagRoom;
    if (player->GetDistance(baseAnchor) > 45.0f)
        return false;

    waypointOut = (botTeam == TEAM_ALLIANCE) ? allianceExit : hordeExit;
    return true;
}

struct WsgBattlePathNode
{
    Position point;
    std::vector<uint32> neighbors;
};

std::vector<WsgBattlePathNode> const& GetWarsongBattlePathGraph()
{
    // Reference-module parity: mirror mod-playerbots WSG waypoint lanes (vmangos
    // battle paths) and connect adjacent points as a routing graph.
    static std::vector<WsgBattlePathNode> const graph =
    {
        // Alliance graveyard(lower) <-> Horde flag room push lane
        { Position(1316.07f, 1533.53f, 315.700f, 0.0f), { 1 } },
        { Position(1276.17f, 1533.72f, 311.722f, 0.0f), { 0, 2 } },
        { Position(1246.25f, 1533.86f, 307.072f, 0.0f), { 1, 3 } },
        { Position(1206.84f, 1528.22f, 307.677f, 0.0f), { 2, 4 } },
        { Position(1172.28f, 1523.28f, 301.958f, 0.0f), { 3, 5 } },
        { Position(1135.93f, 1505.27f, 308.085f, 0.0f), { 4, 6 } },
        { Position(1103.54f, 1521.89f, 314.583f, 0.0f), { 5, 7 } },
        { Position(1073.49f, 1551.19f, 319.418f, 0.0f), { 6, 8 } },
        { Position(1042.92f, 1530.49f, 336.667f, 0.0f), { 7, 9 } },
        { Position(1052.11f, 1493.52f, 342.176f, 0.0f), { 8, 10 } },
        { Position(1057.42f, 1452.75f, 341.131f, 0.0f), { 9, 11, 26 } },
        { Position(1037.96f, 1422.27f, 339.919f, 0.0f), { 10, 12 } },
        { Position(966.01f, 1422.84f, 345.223f, 0.0f), { 11, 13, 36 } },
        { Position(942.74f, 1423.10f, 345.467f, 0.0f), { 12, 14 } },
        { Position(929.39f, 1434.75f, 345.535f, 0.0f), { 13, 15 } },
        { Position(933.331f, 1433.72f, 345.536f, 0.0f), { 14, 16, 40 } },

        // Horde graveyard -> Horde tunnel
        { Position(1029.14f, 1387.49f, 340.836f, 0.0f), { 17, 34 } },
        { Position(1034.95f, 1392.62f, 340.856f, 0.0f), { 16, 18 } },
        { Position(1038.21f, 1406.43f, 341.562f, 0.0f), { 17, 19 } },
        { Position(1043.87f, 1426.9f, 339.197f, 0.0f), { 18, 20 } },
        { Position(1054.53f, 1441.47f, 339.725f, 0.0f), { 19, 21 } },
        { Position(1056.33f, 1456.03f, 341.463f, 0.0f), { 20, 22 } },
        { Position(1057.39f, 1469.98f, 342.148f, 0.0f), { 21, 23 } },
        { Position(1057.67f, 1487.55f, 342.537f, 0.0f), { 22, 24 } },
        { Position(1048.7f, 1505.37f, 341.117f, 0.0f), { 23, 25 } },
        { Position(1042.19f, 1521.69f, 338.003f, 0.0f), { 24, 26 } },
        { Position(1050.01f, 1538.22f, 332.43f, 0.0f), { 25, 10, 27 } },
        { Position(1068.15f, 1548.1f, 321.446f, 0.0f), { 26, 28 } },
        { Position(1088.14f, 1538.45f, 316.398f, 0.0f), { 27, 29 } },
        { Position(1101.26f, 1522.79f, 314.918f, 0.0f), { 28, 30 } },
        { Position(1114.67f, 1503.18f, 312.947f, 0.0f), { 29, 31 } },
        { Position(1126.45f, 1487.4f, 314.136f, 0.0f), { 30, 32 } },
        { Position(1124.37f, 1462.28f, 315.853f, 0.0f), { 31, 33, 59 } },

        // Horde tunnel -> Horde flag room
        { Position(1106.87f, 1462.13f, 316.558f, 0.0f), { 32, 34 } },
        { Position(1089.44f, 1461.04f, 316.332f, 0.0f), { 33, 35 } },
        { Position(1072.07f, 1459.46f, 317.449f, 0.0f), { 34, 36 } },
        { Position(1051.09f, 1459.89f, 323.126f, 0.0f), { 35, 12, 37 } },
        { Position(1030.1f, 1459.58f, 330.204f, 0.0f), { 36, 38 } },
        { Position(1010.76f, 1457.49f, 334.896f, 0.0f), { 37, 39 } },
        { Position(1005.47f, 1448.19f, 335.864f, 0.0f), { 38, 40 } },
        { Position(999.974f, 1458.49f, 335.632f, 0.0f), { 39, 41 } },
        { Position(982.632f, 1459.18f, 336.127f, 0.0f), { 40, 42 } },
        { Position(965.049f, 1459.15f, 338.076f, 0.0f), { 41, 43 } },
        { Position(944.526f, 1459.0f, 344.207f, 0.0f), { 42, 44 } },
        { Position(937.479f, 1451.12f, 345.553f, 0.0f), { 43, 15 } },

        // Alliance tunnel -> Alliance flag room
        { Position(1348.02f, 1461.06f, 323.167f, 0.0f), { 45, 63 } },
        { Position(1359.8f, 1461.49f, 324.527f, 0.0f), { 44, 46 } },
        { Position(1372.47f, 1461.61f, 324.354f, 0.0f), { 45, 47 } },
        { Position(1389.08f, 1461.12f, 325.913f, 0.0f), { 46, 48 } },
        { Position(1406.57f, 1460.48f, 330.615f, 0.0f), { 47, 49 } },
        { Position(1424.04f, 1459.57f, 336.029f, 0.0f), { 48, 50 } },
        { Position(1442.5f, 1459.7f, 342.024f, 0.0f), { 49, 51 } },
        { Position(1449.59f, 1469.14f, 342.65f, 0.0f), { 50, 52 } },
        { Position(1458.03f, 1458.43f, 342.746f, 0.0f), { 51, 53 } },
        { Position(1469.4f, 1458.14f, 342.794f, 0.0f), { 52, 54 } },
        { Position(1489.06f, 1457.86f, 342.794f, 0.0f), { 53, 55 } },
        { Position(1502.27f, 1457.52f, 347.589f, 0.0f), { 54, 56 } },
        { Position(1512.87f, 1457.81f, 352.039f, 0.0f), { 55, 57 } },
        { Position(1517.53f, 1468.79f, 352.033f, 0.0f), { 56, 58 } },
        { Position(1519.53f, 1481.87f, 352.024f, 0.0f), { 57, 79 } },

        // Alliance graveyard -> Alliance tunnel
        { Position(1415.33f, 1554.79f, 343.156f, 0.0f), { 60 } },
        { Position(1428.29f, 1551.79f, 342.751f, 0.0f), { 59, 61 } },
        { Position(1441.51f, 1545.79f, 342.757f, 0.0f), { 60, 62 } },
        { Position(1441.15f, 1530.35f, 343.712f, 0.0f), { 61, 63 } },
        { Position(1435.53f, 1517.29f, 346.698f, 0.0f), { 62, 64 } },
        { Position(1424.81f, 1499.24f, 349.486f, 0.0f), { 63, 65 } },
        { Position(1416.31f, 1483.94f, 348.536f, 0.0f), { 64, 66 } },
        { Position(1408.83f, 1468.4f, 347.648f, 0.0f), { 65, 67 } },
        { Position(1404.64f, 1449.79f, 347.279f, 0.0f), { 66, 68 } },
        { Position(1405.34f, 1432.33f, 345.792f, 0.0f), { 67, 69 } },
        { Position(1406.38f, 1416.18f, 344.755f, 0.0f), { 68, 70 } },
        { Position(1400.22f, 1401.87f, 340.496f, 0.0f), { 69, 71 } },
        { Position(1385.96f, 1394.15f, 333.829f, 0.0f), { 70, 72 } },
        { Position(1372.38f, 1390.75f, 328.722f, 0.0f), { 71, 73 } },
        { Position(1362.93f, 1390.02f, 327.034f, 0.0f), { 72, 74 } },
        { Position(1357.91f, 1398.07f, 325.674f, 0.0f), { 73, 75 } },
        { Position(1354.17f, 1411.56f, 324.327f, 0.0f), { 74, 76 } },
        { Position(1351.44f, 1430.38f, 323.506f, 0.0f), { 75, 77 } },
        { Position(1350.36f, 1444.43f, 323.388f, 0.0f), { 76, 78 } },
        { Position(1348.02f, 1461.06f, 323.167f, 0.0f), { 77, 44 } },

        // Alliance flag room -> Alliance graveyard
        { Position(1508.27f, 1493.17f, 352.005f, 0.0f), { 58, 80 } },
        { Position(1490.78f, 1493.51f, 352.141f, 0.0f), { 79, 81 } },
        { Position(1469.79f, 1494.13f, 351.774f, 0.0f), { 80, 82 } },
        { Position(1453.65f, 1494.39f, 350.614f, 0.0f), { 81, 83 } },
        { Position(1443.51f, 1501.75f, 348.317f, 0.0f), { 82, 84 } },
        { Position(1443.33f, 1517.78f, 345.534f, 0.0f), { 83, 85 } },
        { Position(1443.55f, 1533.4f, 343.148f, 0.0f), { 84, 86 } },
        { Position(1441.47f, 1548.12f, 342.752f, 0.0f), { 85, 87 } },
        { Position(1433.79f, 1552.67f, 342.763f, 0.0f), { 86, 88 } },
        { Position(1422.88f, 1552.37f, 342.751f, 0.0f), { 87, 59 } }
    };

    return graph;
}

uint32 FindClosestGraphNode(std::vector<WsgBattlePathNode> const& graph, Position const& origin)
{
    uint32 closestIndex = 0;
    float closestDistance = std::numeric_limits<float>::max();

    for (uint32 i = 0; i < graph.size(); ++i)
    {
        float const dx = graph[i].point.GetPositionX() - origin.GetPositionX();
        float const dy = graph[i].point.GetPositionY() - origin.GetPositionY();
        float const dz = graph[i].point.GetPositionZ() - origin.GetPositionZ();
        float const distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < closestDistance)
        {
            closestDistance = distSq;
            closestIndex = i;
        }
    }

    return closestIndex;
}

bool TryBuildWarsongGraphPath(uint32 startNode, uint32 goalNode, std::vector<uint32>& pathOut)
{
    std::vector<WsgBattlePathNode> const& graph = GetWarsongBattlePathGraph();
    if (startNode >= graph.size() || goalNode >= graph.size())
        return false;

    std::vector<float> bestDistance(graph.size(), std::numeric_limits<float>::max());
    std::vector<int32> previous(graph.size(), -1);

    using QueueEntry = std::pair<float, uint32>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
    bestDistance[startNode] = 0.0f;
    queue.push({ 0.0f, startNode });

    while (!queue.empty())
    {
        QueueEntry const current = queue.top();
        queue.pop();

        float const distanceSoFar = current.first;
        uint32 const node = current.second;
        if (distanceSoFar > bestDistance[node])
            continue;

        if (node == goalNode)
            break;

        Position const& nodePos = graph[node].point;
        for (uint32 neighbor : graph[node].neighbors)
        {
            if (neighbor >= graph.size())
                continue;

            Position const& neighborPos = graph[neighbor].point;
            float const edgeWeight = nodePos.GetExactDist(neighborPos);
            float const candidateDistance = distanceSoFar + edgeWeight;
            if (candidateDistance < bestDistance[neighbor])
            {
                bestDistance[neighbor] = candidateDistance;
                previous[neighbor] = static_cast<int32>(node);
                queue.push({ candidateDistance, neighbor });
            }
        }
    }

    if (bestDistance[goalNode] == std::numeric_limits<float>::max())
        return false;

    pathOut.clear();
    for (int32 node = static_cast<int32>(goalNode); node >= 0; node = previous[node])
        pathOut.push_back(static_cast<uint32>(node));

    std::reverse(pathOut.begin(), pathOut.end());
    return !pathOut.empty();
}

bool TryGetWarsongLaneWaypoint(Player* player, Position const& finalDestination, Position& waypointOut)
{
    if (!player || !IsWarsongGulch(player))
        return false;

    std::vector<WsgBattlePathNode> const& graph = GetWarsongBattlePathGraph();
    if (graph.empty())
        return false;

    Position origin(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation());
    uint32 const startNode = FindClosestGraphNode(graph, origin);
    uint32 const goalNode = FindClosestGraphNode(graph, finalDestination);

    std::vector<uint32> path;
    if (!TryBuildWarsongGraphPath(startNode, goalNode, path))
        return false;

    uint32 nextNode = path.front();
    if (path.size() > 1)
        nextNode = path[1];

    waypointOut = graph[nextNode].point;
    return true;
}

bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold, uint32 minReissueMs);

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

void EmitLifecycleDiagnostic(Player* player, char const* phase, std::string const& detail)
{
    if (!player || !phase)
        return;

    std::ostringstream oss;
    oss << "[Playerbot PvP][" << phase << "] bot=" << player->GetName() << " guid=" << player->GetGUID().ToString()
        << " map=" << player->GetMapId() << " detail=" << detail;
    std::string const message = oss.str();

    TC_LOG_WARN("playerbots.pvp.lifecycle", "{}", message);

    if (WorldSession* session = player->GetSession())
        ChatHandler(session).SendGlobalGMSysMessage(message.c_str());
}

void EmitBattlegroundGmDebug(Player* bot, std::string const& detail, uint32 throttleMs = 3000)
{
    if (!bot || !bot->InBattleground())
        return;

    static std::unordered_map<uint64, uint32> nextEmitTimeByBotGuid;
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint64 const botGuid = bot->GetGUID().GetRawValue();
    uint32& nextEmitMs = nextEmitTimeByBotGuid[botGuid];
    if (nowMs < nextEmitMs)
        return;

    nextEmitMs = nowMs + throttleMs;

    Map* map = bot->GetMap();
    if (!map)
        return;

    std::ostringstream os;
    os << "[PBDBG movepoint] bot=" << bot->GetName()
       << " guid=" << bot->GetGUID().ToString()
       << " bgId=" << bot->GetBattlegroundId()
       << " detail=" << detail;
    std::string const message = os.str();

    TC_LOG_DEBUG("playerbots.pvp.lifecycle", "{}", message);

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* observer = itr->GetSource();
        if (!observer || !observer->IsGameMaster())
            continue;

        if (observer->GetBattlegroundId() != bot->GetBattlegroundId())
            continue;

        bot->Whisper(message, LANG_UNIVERSAL, observer);
    }
}

bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold = 6.0f, uint32 minReissueMs = 2000)
{
    if (!player)
        return false;

    struct MoveOrderState
    {
        Position lastDestination;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, MoveOrderState> stateByGuid;
    MoveOrderState& state = stateByGuid[player->GetGUID().GetRawValue()];
    uint32 const nowMs = GameTime::GetGameTimeMS();

    bool const destinationChanged = state.lastIssueMs == 0 ||
        state.lastDestination.GetExactDist(destination) >= destinationChangeThreshold;
    bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + minReissueMs;
    if (!destinationChanged && !canReissueByTime)
    {
        return false;
    }

    MotionMaster* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const currentMovement = motionMaster->GetCurrentMovementGeneratorType();
    if (currentMovement == FOLLOW_MOTION_TYPE || currentMovement == DISTRACT_MOTION_TYPE)
    {
        motionMaster->Clear();
    }

    bool generatePath = !player->IsFlying() && !player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
    Position issuedDestination = destination;
    if (IsWarsongGulch(player))
    {
        // Reference module behavior is waypoint-driven in WSG; avoid navmesh
        // dependence and always step lane-to-lane using fixed points.
        generatePath = false;

        Position bootstrapWaypoint;
        if (TryGetWarsongBaseExitWaypoint(player, bootstrapWaypoint))
            issuedDestination = bootstrapWaypoint;
        else
        {
            Position laneWaypoint;
            if (TryGetWarsongLaneWaypoint(player, destination, laneWaypoint))
                issuedDestination = laneWaypoint;
        }
    }

    motionMaster->MovePoint(0, issuedDestination, generatePath);
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

    BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
    GroupQueueInfo ginfo;
    if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
        return false;

    player->RemoveBattlegroundQueueId(bgQueueTypeId);
    bgQueue.RemovePlayer(player->GetGUID(), true);

    if (scheduleNonArenaUpdate && !ginfo.ArenaType)
    {
        sBattlegroundMgr->ScheduleQueueUpdate(ginfo.ArenaMatchmakerRating, ginfo.ArenaType, bgQueueTypeId, bgTypeId,
            bracketEntry->GetBracketId());
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
    if (!player || !player->InBattleground())
        return false;

    if (player->IsAlive())
        return false;

    if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        if (player->getDeathState() == JUST_DIED)
            player->KillPlayer();

        player->BuildPlayerRepop();
        player->RepopAtGraveyard();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP death handling: guid={} action=release-spirit.",
            player->GetGUID().ToString());
        return true;
    }

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

bool CanIssueBotMovement(Player const* player)
{
    if (!player || !player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return false;

    if (player->HasUnitState(UNIT_STATE_ROOT) || player->HasUnitState(UNIT_STATE_STUNNED))
        return false;

    return true;
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

    float const angleAway = target->GetAbsoluteAngle(player);
    float const currentDistance = player->GetDistance(target);
    float const moveDistance = std::max(4.0f, desiredDistance - currentDistance + 2.0f);

    Position destination(player->GetPositionX() + std::cos(angleAway) * moveDistance,
        player->GetPositionY() + std::sin(angleAway) * moveDistance,
        player->GetPositionZ(), player->GetOrientation());
    player->GetMotionMaster()->MovePoint(0, destination);
    return true;
}

bool TryRecoverLineOfSight(Player* player, Unit* target, CombatPositioningProfile const& profile, char const* reason)
{
    if (!player || !target || !target->IsAlive() || !CanIssueBotMovement(player))
        return false;

    if (player->IsWithinLOSInMap(target))
        return false;

    float const orbitAngle = target->GetAbsoluteAngle(player) + frand(-0.85f, 0.85f);
    float const orbitRange = std::max(profile.preferredMinRange + 2.0f, profile.preferredIdealRange);
    Position reposition(target->GetPositionX() + std::cos(orbitAngle) * orbitRange,
        target->GetPositionY() + std::sin(orbitAngle) * orbitRange,
        target->GetPositionZ(), player->GetOrientation());
    player->GetMotionMaster()->MovePoint(0, reposition);

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
    if (!player || !player->IsAlive() || !target || !target->IsAlive() || player->GetMapId() != target->GetMapId() || !CanIssueBotMovement(player))
        return false;

    CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
    if (!player->IsWithinLOSInMap(target))
        return TryRecoverLineOfSight(player, target, profile, "move-toward-unit");

    if (!player->IsWithinDistInMap(target, desiredDistance))
        player->GetMotionMaster()->MoveFollow(target, desiredDistance, player->GetFollowAngle());

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
        player->GetMotionMaster()->MovePoint(0, droppedFlag->GetPosition());
        return true;
    }

    bgWs->EventPlayerClickedOnFlag(player, droppedFlag);
    attemptNotBeforeMs = nowMs + urand(1200, 2200);
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP WSG return attempted: guid={} flag_guid={} next_attempt_ms={}.",
        player->GetGUID().ToString(), droppedFlag->GetGUID().ToString(), attemptNotBeforeMs);
    return true;
}

Player* FindNearestEnemyBattlegroundPlayer(Player* player, float maxDistance)
{
    if (!player || !player->InBattleground() || !player->GetMap())
        return nullptr;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    TeamId const playerBgTeam = ResolveBotTeamId(player);

    float nearestDistance = std::numeric_limits<float>::max();
    Player* nearestEnemy = nullptr;

    Map::PlayerList const& players = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (candidate->GetBattlegroundId() != player->GetBattlegroundId())
            continue;
        TeamId const candidateBgTeam = ResolveBotTeamId(candidate);
        if (candidateBgTeam == playerBgTeam)
            continue;

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

    Unit* target = player->GetVictim();
    if (!target || !target->IsAlive())
        target = player->GetSelectedUnit();
    if ((!target || !target->IsAlive()) && player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS)
    {
        Unit* duelOpponent = player->duel->Opponent;
        if (duelOpponent && duelOpponent->IsAlive() && duelOpponent->GetMapId() == player->GetMapId())
            target = duelOpponent;
    }
    if ((!target || !target->IsAlive()) && player->InBattleground())
        target = FindNearestEnemyBattlegroundPlayer(player, scanDistance);
    if (!target || !target->IsAlive())
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
        if (profile.createDistanceWhenCrowded && IsMeleePressureTarget(target) && distance < profile.preferredIdealRange)
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
            player->GetMotionMaster()->MoveFollow(target, profile.preferredIdealRange, player->GetFollowAngle());
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
        return true;
    }

    if (distance > profile.preferredMaxPressureRange || !player->IsWithinMeleeRange(target))
    {
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

    Unit* target = AcquireCombatTarget(player, scanDistance);
    if (!target)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP movement skipped: bot={} reason=no-combat-target scanDistance={}.",
            player ? player->GetGUID().ToString() : ObjectGuid::Empty.ToString(), scanDistance);
        return false;
    }

    CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
    bool const useMeleeAttack = !profile.primarilyRanged || profile.meleeFallbackAcceptable;
    bool const isStealthedRogue = player->GetClass() == CLASS_ROGUE && player->HasStealthAura();
    if (isStealthedRogue)
        player->AttackStop();
    else
        player->Attack(target, useMeleeAttack);

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

    // WSG needs deterministic cross-map intent; base anchors can be missing or
    // locally-biased in sparse managed-bot matches, which leaves bots stalling.
    if (TryGetWarsongEnemyBasePosition(player, destination))
    {
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

    // Fallback for sparse/invalid start-anchor states:
    // WSG bots can otherwise idle in their own base graveyard when no objective
    // anchor is resolved from battleground starts.
    if (battleground->GetMapId() == 489) // Warsong Gulch
    {
        Position const allianceFlagStand(1540.423f, 1481.325f, 351.8284f, 3.089233f);
        Position const hordeFlagStand(916.0226f, 1434.405f, 345.413f, 0.01745329f);

        if (botTeam == TEAM_ALLIANCE)
            destination = hordeFlagStand;
        else if (botTeam == TEAM_HORDE)
            destination = allianceFlagStand;
        else
            destination = hordeFlagStand;

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

    for (BattlegroundTypeId bgTypeId : BuildRandomBattlegroundOrder())
    {
        if (QueuePlayer(player, bgTypeId, 0))
            return true;
    }

    return false;
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

    if (Battleground* battleground = player->GetBattleground())
    {
        if (battleground->GetStatus() != STATUS_IN_PROGRESS)
            return false;
    }

    if (HandleBattlegroundDeathState(player))
        return true;

    if (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION))
    {
        player->RemoveAurasDueToSpell(SPELL_PREPARATION);
        player->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
        player->RemoveUnitFlag(UNIT_FLAG_PREPARATION);
    }

    // Keep managed bots moving even when tactical decision hooks are disabled
    // or when another behavior tree branch (e.g. buffing) wins the current tick.
    // This prevents "stand still at gate-open" stalls in active battlegrounds.
    if (!CanIssueBotMovement(player))
    {
        std::ostringstream blockedReason;
        blockedReason << "movement-blocked alive=" << (player->IsAlive() ? 1 : 0)
                      << " ghost=" << (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? 1 : 0)
                      << " rooted=" << (player->HasUnitState(UNIT_STATE_ROOT) ? 1 : 0)
                      << " stunned=" << (player->HasUnitState(UNIT_STATE_STUNNED) ? 1 : 0);
        EmitBattlegroundGmDebug(player, blockedReason.str());
        return true;
    }

    float const engageDistance = IsWarsongGulch(player) ? 2000.0f : 65.0f;
    if (EngageNearestEnemyPlayer(player, engageDistance))
        return true;

    if (Battleground* battleground = player->GetBattleground())
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
    }

    if (IsWarsongGulch(player))
        return MoveToClosestBattlegroundGraveyard(player);

    EmitBattlegroundGmDebug(player, "no enemy target and no objective position resolved");
    return true;
}

bool BattlegroundTacticalActions::Execute(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !context.tacticsEnabled || !context.shouldEvaluate || !context.actionName)
        return false;

    if (!player->IsAlive())
        return false;

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
    if (!player || !player->InBattleground())
        return false;

    if (Battleground* battleground = player->GetBattleground())
        return battleground->GetStatus() == STATUS_WAIT_JOIN;

    return false;
}

bool BattlegroundTacticalActions::MoveToObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;
    if (!CanIssueBotMovement(player))
        return false;

    bool const teamHasHumans = PvpCore::TeamHasHumanPlayers(player);
    if (teamHasHumans && TryReturnDroppedFriendlyFlagWithHumanPriority(player))
        return true;

    float const engageDistance = IsWarsongGulch(player) ? 2000.0f : 80.0f;
    if (EngageNearestEnemyPlayer(player, engageDistance))
        return true;

    if (context.objective.type == BattlegroundObjectiveType::None &&
        context.movement == BattlegroundMovementPrimitive::None &&
        context.flagCarrierDirective == FlagCarrierDirective::None)
    {
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
        if (Battleground* battleground = player->GetBattleground())
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

            // WSG can enter sparse states where objective anchors are unavailable.
            // In those windows, force a large-radius enemy scan before falling
            // back to local graveyard movement so bots keep crossing the map.
            if (EngageNearestEnemyPlayer(player, 2000.0f))
                return true;

            if (WorldSafeLocsEntry const* graveyard = battleground->GetClosestGraveyard(player))
            {
                Position destination(graveyard->Loc.X, graveyard->Loc.Y, graveyard->Loc.Z, player->GetOrientation());
                if (!player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
                    IssueMovePointThrottled(player, destination);
                return true;
            }
        }
    }

    bool const fallbackEngage = EngageNearestEnemyPlayer(player, 55.0f);
    if (!fallbackEngage)
    {
        if (IsWarsongGulch(player))
            return MoveToClosestBattlegroundGraveyard(player);

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP movement skipped: bot={} reason=no-objective-movement-and-no-fallback-target.",
            player->GetGUID().ToString());
    }

    return fallbackEngage;
}

bool BattlegroundTacticalActions::CheckObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    float const engageDistance = IsWarsongGulch(player) ? 2000.0f : 60.0f;
    if (EngageNearestEnemyPlayer(player, engageDistance))
        return true;

    if (IsWarsongGulch(player))
        return MoveToClosestBattlegroundGraveyard(player);

    return context.movement != BattlegroundMovementPrimitive::None || context.objective.type != BattlegroundObjectiveType::None;
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

    return true;
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

    didExecute = AcceptMatchingInvite(player, true) || didExecute;

    return didExecute;
}

bool ArenaLifecycleActions::JoinQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    std::array<uint8, 3> arenaTypes = { ARENA_TYPE_2v2, ARENA_TYPE_3v3, ARENA_TYPE_5v5 };
    Trinity::Containers::RandomShuffle(arenaTypes);
    for (uint8 arenaType : arenaTypes)
    {
        if (QueuePlayer(player, BATTLEGROUND_AA, arenaType))
            return true;
    }

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

    return EngageNearestEnemyPlayer(player, 65.0f);
}
}
