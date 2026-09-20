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

#include "PlayerbotNodeCoordinator.h"
#include "PlayerbotObcClone.h"
#include "PlayerbotRandomBotParticipation.h"
#include "PlayerbotSharedStateGuard.h"
#include "Battleground.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    using playerbot::NodeRole;

    constexpr uint32 kNoNode = std::numeric_limits<uint32>::max();

    constexpr uint32 kPlanRefreshMs = 500;
    constexpr uint32 kPlanPruneIntervalMs = 60 * IN_MILLISECONDS;
    constexpr uint32 kPlanStaleMs = 10 * MINUTE * IN_MILLISECONDS;

    // An enemy inside this ring of a banner is pressure on that base. It is
    // wider than the ten yards the banner can be clicked from: the bots have
    // to be moving before the click starts, not after.
    constexpr float kNodeThreatRange = 45.0f;
    // A body counts as garrisoning a base from here.
    constexpr float kNodeHoldRange = 30.0f;
    // How far a fight may drag a bot off the base it is holding.
    constexpr float kDefendLeashRange = 45.0f;
    constexpr float kRecaptureLeashRange = 60.0f;
    // What the base a bot already has is worth when bodies are handed out, in
    // yards of head start. Without it every rebuild re-sorts the whole team.
    constexpr float kStickinessYards = 60.0f;

    // Base scores. A base of ours in the enemy's hands outranks the whole map;
    // a free base outranks one that has to be fought for.
    constexpr int32 kScoreUnderAttack = 200;
    constexpr int32 kScoreFriendlyContested = 120;
    constexpr int32 kScoreFriendlyControlled = 60;
    constexpr int32 kScoreNeutral = 150;
    constexpr int32 kScoreEnemyContested = 130;
    constexpr int32 kScoreEnemyControlled = 70;

    // Per enemy standing on a base of ours. Sixty is enough that one attacker
    // lifts a quiet base above an enemy base worth taking, which is the whole
    // point: somebody comes back. A base still capping for us counts the same:
    // one click from him and the minute we have spent on it is gone.
    constexpr int32 kScorePerEnemyOnOurs = 60;
    // Per enemy on a base we want. They are in the way, not an invitation.
    constexpr int32 kScorePerEnemyOnTheirs = 10;

    // Bases, not kills, win these matches: behind on bases, everything we do
    // not hold is worth more; ahead, the lead is worth protecting.
    constexpr int32 kScoreNeedBases = 45;
    constexpr int32 kScoreProtectLead = 30;

    // Each body already sent costs a base this much of its score. Defense
    // spreads - a pair on a base, then look elsewhere - while an assault
    // stacks, so the squad arrives together instead of feeding in one by one.
    constexpr int32 kCrowdPenaltyDefense = 22;
    constexpr int32 kCrowdPenaltyAssault = 8;
    // What each body past a base's quota costs. It is charged per body over,
    // not once, so a near base cannot quietly swallow every spare just because
    // it is the cheapest place to walk to.
    constexpr int32 kOverflowPenalty = 45;
    // Ties between equal bases go to the one our nearest free bot can reach.
    constexpr float kDistanceScoreWeight = 0.15f;

    // A free base belongs to whichever side starts nearer it. Two thirds of
    // the map is genuinely contested ground under this test: in Arathi Basin
    // only the Stables and the Farm come out as home ground (190 y against
    // 674, and 196 against 671), while the Blacksmith is 430 against 433.
    constexpr float kHomeGroundRatio = 0.75f;

    // Which side reaches a base first from the starting gates. It decides
    // nothing once a base has been claimed - only how the opening is spread.
    enum class NodeGround : uint8
    {
        Contested,  // both gates are about as far away: this is the match
        Ours,       // ours to walk into, so one bot is sent to claim it
        Theirs      // theirs to walk into: not worth a body while it is free
    };

    bool IsFriendlySideStatus(BattlegroundNodeStatus status)
    {
        return status == BattlegroundNodeStatus::FriendlyControlled ||
            status == BattlegroundNodeStatus::FriendlyContested ||
            status == BattlegroundNodeStatus::FriendlyUnderAttack;
    }

    char const* GetNodeStatusName(BattlegroundNodeStatus status)
    {
        switch (status)
        {
            case BattlegroundNodeStatus::FriendlyControlled:  return "ours";
            case BattlegroundNodeStatus::FriendlyContested:   return "ours-capping";
            case BattlegroundNodeStatus::FriendlyUnderAttack: return "ours-under-attack";
            case BattlegroundNodeStatus::EnemyControlled:     return "theirs";
            case BattlegroundNodeStatus::EnemyContested:      return "theirs-capping";
            case BattlegroundNodeStatus::Neutral:
            default:                                          return "free";
        }
    }

    char const* GetNodeGroundName(NodeGround ground)
    {
        switch (ground)
        {
            case NodeGround::Ours:   return "our side";
            case NodeGround::Theirs: return "their side";
            case NodeGround::Contested:
            default:                 return "midfield";
        }
    }

    struct NodeState
    {
        uint32 nodeId = 0;
        BattlegroundNodeStatus status = BattlegroundNodeStatus::Neutral;
        Position location;
        ObjectGuid bannerGuid;
        uint32 enemiesNear = 0;
        uint32 alliesNear = 0;   // living friendly humans, who fill a quota too
        NodeGround ground = NodeGround::Contested;
        uint32 quota = 0;
        int32 score = 0;
        uint32 assigned = 0;
    };

    struct NodeAssignment
    {
        uint32 nodeId = kNoNode;
        NodeRole role = NodeRole::None;
    };

    struct TeamPlan
    {
        uint32 lastRefreshMs = 0;
        uint32 lastSeenMs = 0;
        std::unordered_map<ObjectGuid, NodeAssignment> assignments;
        std::vector<NodeState> nodes;
        uint32 ownedCount = 0;
        uint32 enemyOwnedCount = 0;
        uint32 livingBots = 0;
    };

    std::unordered_map<uint64, TeamPlan> g_NodeTeamPlans;
    uint32 g_LastPlanPruneMs = 0;

    uint64 BuildPlanKey(Battleground const* battleground, TeamId team)
    {
        return (uint64(battleground->GetTypeID()) << 40) |
            (uint64(battleground->GetInstanceID()) << 1) |
            uint64(team == TEAM_HORDE ? 1 : 0);
    }

    TeamId ResolveTeam(Battleground const* battleground, Player const* player)
    {
        if (!battleground || !player)
            return TEAM_NEUTRAL;

        uint32 team = battleground->GetPlayerTeam(player->GetGUID());
        if (!team && player->GetBattlegroundId() == battleground->GetInstanceID())
            team = player->GetBGTeam();

        switch (team)
        {
            case ALLIANCE: return TEAM_ALLIANCE;
            case HORDE: return TEAM_HORDE;
            default: return TEAM_NEUTRAL;
        }
    }

    bool IsBotControlled(Player const* player)
    {
        return player && (playerbot::IsManagedRandomBot(player) ||
            playerbot::PlayerbotObcCloneManager::IsActiveClone(player));
    }

    // Only a FAR teleport takes a player out of the match; a near one is
    // acknowledged on that player's own next update.
    bool IsAliveParticipant(Player const* player)
    {
        return player && player->IsInWorld() && player->IsAlive() &&
            !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) &&
            !player->IsBeingTeleportedFar() && !player->IsSpectator() && !player->IsGameMaster();
    }

    NodeRole RoleForStatus(BattlegroundNodeStatus status)
    {
        switch (status)
        {
            case BattlegroundNodeStatus::FriendlyUnderAttack: return NodeRole::Recapture;
            case BattlegroundNodeStatus::FriendlyControlled:
            case BattlegroundNodeStatus::FriendlyContested:   return NodeRole::Defend;
            case BattlegroundNodeStatus::Neutral:             return NodeRole::Claim;
            default:                                          return NodeRole::Assault;
        }
    }

    // Registers this plan as in use and, at most once a minute, drops the plans
    // of matches nobody has evaluated for ten minutes. Both happen under the
    // structure lock, so a plan another map is using this tick is never pruned
    // out from under it.
    TeamPlan& TouchTeamPlan(uint64 key, uint32 nowMs)
    {
        std::lock_guard<std::mutex> guard(playerbot::SharedBotStateStructureLock());
        if (nowMs - g_LastPlanPruneMs >= kPlanPruneIntervalMs)
        {
            g_LastPlanPruneMs = nowMs;
            for (auto itr = g_NodeTeamPlans.begin(); itr != g_NodeTeamPlans.end();)
            {
                if (itr->first != key && nowMs - itr->second.lastSeenMs >= kPlanStaleMs)
                    itr = g_NodeTeamPlans.erase(itr);
                else
                    ++itr;
            }
        }

        TeamPlan& plan = g_NodeTeamPlans[key];
        plan.lastSeenMs = nowMs;
        return plan;
    }

    // Every base as this team sees it, with the bodies standing on each one
    // counted in a single pass over the map.
    void BuildNodeStates(Battleground* battleground, Map* map, TeamId team, ObjectGuid referenceGuid,
        std::vector<NodeState>& nodes, std::vector<Player*>& bots)
    {
        uint32 const nodeCount = battleground->GetDynamicNodeCount();
        nodes.reserve(nodeCount);
        for (uint32 nodeId = 0; nodeId < nodeCount; ++nodeId)
        {
            BattlegroundNodeObjective objective;
            if (!battleground->GetDynamicNodeInfo(referenceGuid, nodeId, objective))
                continue;

            NodeState state;
            state.nodeId = objective.NodeId;
            state.status = objective.Status;
            state.location = objective.Location;
            state.bannerGuid = objective.BannerGuid;
            nodes.push_back(state);
        }

        // Home ground, from the gates the two sides actually start at rather
        // than from a table of base names. A battleground that does not
        // declare starting positions simply has no home ground and opens the
        // way it always did.
        if (Position const* ourStart = battleground->GetTeamStartPosition(team))
        {
            if (Position const* theirStart = battleground->GetTeamStartPosition(
                team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE))
            {
                for (NodeState& state : nodes)
                {
                    float const fromUs = state.location.GetExactDist(ourStart);
                    float const fromThem = state.location.GetExactDist(theirStart);
                    if (fromUs < fromThem * kHomeGroundRatio)
                        state.ground = NodeGround::Ours;
                    else if (fromThem < fromUs * kHomeGroundRatio)
                        state.ground = NodeGround::Theirs;
                }
            }
        }

        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || member->IsSpectator() || member->IsGameMaster())
                continue;
            if (member->GetBattlegroundId() != battleground->GetInstanceID())
                continue;

            TeamId const memberTeam = ResolveTeam(battleground, member);
            if (memberTeam == TEAM_NEUTRAL)
                continue;

            bool const friendly = memberTeam == team;
            if (friendly && IsBotControlled(member))
            {
                // A bot holds a base because the plan sent it there, so it is
                // counted through its assignment and not a second time here.
                bots.push_back(member);
                continue;
            }

            if (!IsAliveParticipant(member))
                continue;

            // A stealthed enemy is not pressure the bots are allowed to feel:
            // nothing on the team can see him, and reacting anyway would be the
            // bots reading the server rather than the base. He announces
            // himself by clicking, and a base of ours being taken already
            // outranks the whole map.
            if (!friendly && (member->HasStealthAura() || member->HasInvisibilityAura()))
                continue;

            float const range = friendly ? kNodeHoldRange : kNodeThreatRange;
            for (NodeState& state : nodes)
            {
                if (!member->IsWithinDist3d(state.location.GetPositionX(), state.location.GetPositionY(),
                    state.location.GetPositionZ(), range))
                    continue;

                if (friendly)
                    ++state.alliesNear;
                else
                    ++state.enemiesNear;
            }
        }
    }

    void ScoreNodes(std::vector<NodeState>& nodes, bool needBases, bool aheadOnBases, uint32 baseGarrison,
        uint32 livingCount)
    {
        // The opening. One bot is enough to claim a free base nobody else can
        // reach first, so the rest of the team is shared over the bases the
        // match is actually decided on, and none is walked across the map to
        // a base the enemy will have clicked before it arrives.
        uint32 homeBases = 0;
        uint32 contestedBases = 0;
        for (NodeState const& state : nodes)
        {
            if (state.status != BattlegroundNodeStatus::Neutral)
                continue;
            if (state.ground == NodeGround::Ours)
                ++homeBases;
            else if (state.ground == NodeGround::Contested)
                ++contestedBases;
        }

        uint32 const spare = livingCount > homeBases ? livingCount - homeBases : 0u;
        uint32 const contestedQuota = contestedBases ?
            std::max<uint32>(2, (spare + contestedBases - 1) / contestedBases) : 0u;

        for (NodeState& state : nodes)
        {
            switch (state.status)
            {
                case BattlegroundNodeStatus::FriendlyUnderAttack:
                    state.score = kScoreUnderAttack + int32(state.enemiesNear) * kScorePerEnemyOnOurs;
                    state.quota = std::max<uint32>(2, state.enemiesNear + 1);
                    break;
                // A base capping for us is held exactly like one we own, and
                // it is the more fragile of the two: the minute it has been
                // standing there dies the moment an enemy clicks the banner.
                case BattlegroundNodeStatus::FriendlyContested:
                    state.score = kScoreFriendlyContested + int32(state.enemiesNear) * kScorePerEnemyOnOurs;
                    state.quota = std::max<uint32>(2, state.enemiesNear + 1);
                    break;
                case BattlegroundNodeStatus::FriendlyControlled:
                    state.score = kScoreFriendlyControlled + int32(state.enemiesNear) * kScorePerEnemyOnOurs;
                    state.quota = state.enemiesNear ? state.enemiesNear + 1 : baseGarrison;
                    break;
                case BattlegroundNodeStatus::Neutral:
                    state.score = kScoreNeutral - int32(state.enemiesNear) * kScorePerEnemyOnTheirs;
                    switch (state.ground)
                    {
                        case NodeGround::Ours:
                            state.quota = std::max<uint32>(1, state.enemiesNear + 1);
                            break;
                        case NodeGround::Theirs:
                            state.quota = 0;
                            break;
                        default:
                            state.quota = std::max<uint32>(contestedQuota, state.enemiesNear + 1);
                            break;
                    }
                    break;
                case BattlegroundNodeStatus::EnemyContested:
                    state.score = kScoreEnemyContested - int32(state.enemiesNear) * kScorePerEnemyOnTheirs;
                    state.quota = std::max<uint32>(2, state.enemiesNear + 1);
                    break;
                case BattlegroundNodeStatus::EnemyControlled:
                default:
                    state.score = kScoreEnemyControlled - int32(state.enemiesNear) * kScorePerEnemyOnTheirs;
                    state.quota = std::max<uint32>(3, state.enemiesNear + 2);
                    break;
            }

            if (IsFriendlySideStatus(state.status))
            {
                if (aheadOnBases)
                    state.score += kScoreProtectLead;
            }
            else if (needBases)
                state.score += kScoreNeedBases;

            // A teammate at the keyboard already standing on the base counts
            // against what it asks for.
            state.assigned = std::min(state.alliesNear, state.quota);
        }
    }

    // Distance from a bot to a base, discounted by what it would cost to move
    // that bot off the base it already has.
    float AssignmentCost(Player const* bot, NodeState const& state, NodeAssignment const* previous)
    {
        float cost = bot->GetExactDist(state.location.GetPositionX(), state.location.GetPositionY(),
            state.location.GetPositionZ());
        if (previous && previous->nodeId == state.nodeId)
            cost -= kStickinessYards;
        return cost;
    }

    void RefreshTeamPlan(Battleground* battleground, Map* map, TeamId team, ObjectGuid referenceGuid,
        TeamPlan& plan, uint32 nowMs)
    {
        plan.lastRefreshMs = nowMs;

        std::vector<NodeState> nodes;
        std::vector<Player*> bots;
        BuildNodeStates(battleground, map, team, referenceGuid, nodes, bots);
        if (nodes.empty())
        {
            plan.nodes.clear();
            plan.assignments.clear();
            return;
        }

        uint32 owned = 0;
        uint32 enemyOwned = 0;
        for (NodeState const& state : nodes)
        {
            if (state.status == BattlegroundNodeStatus::FriendlyControlled)
                ++owned;
            else if (state.status == BattlegroundNodeStatus::EnemyControlled)
                ++enemyOwned;
        }

        std::vector<Player*> living;
        living.reserve(bots.size());
        for (Player* bot : bots)
            if (IsAliveParticipant(bot))
                living.push_back(bot);

        bool const needBases = owned <= enemyOwned;
        uint32 const baseGarrison = owned > enemyOwned ? 2u : 1u;
        ScoreNodes(nodes, needBases, owned > enemyOwned, baseGarrison, uint32(living.size()));

        // Bases win these matches, so a side that is not ahead may never talk
        // itself into sitting on everything it has. Recapturing a base of ours
        // is exempt: it is the cheapest base on the map and counts as offense.
        // The cap only binds while there is somewhere else to send the bots -
        // a team holding the whole map has nothing to be held back for.
        uint32 const maxDefenders = living.empty() ? 0u :
            std::max<uint32>(1, uint32(living.size()) * (needBases ? 50u : 75u) / 100u);
        bool const capDefenders = std::any_of(nodes.begin(), nodes.end(),
            [](NodeState const& state) { return !IsFriendlySideStatus(state.status); });

        std::unordered_map<ObjectGuid, NodeAssignment> previous;
        previous.swap(plan.assignments);

        std::vector<bool> taken(living.size(), false);
        uint32 assignedCount = 0;
        uint32 defenders = 0;

        auto const findNearestFreeBot = [&](NodeState const& state, float& outCost) -> std::size_t
        {
            std::size_t best = living.size();
            float bestCost = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < living.size(); ++i)
            {
                if (taken[i])
                    continue;

                auto const previousItr = previous.find(living[i]->GetGUID());
                float const cost = AssignmentCost(living[i], state,
                    previousItr == previous.end() ? nullptr : &previousItr->second);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    best = i;
                }
            }

            outCost = bestCost;
            return best;
        };

        while (assignedCount < living.size())
        {
            NodeState* chosen = nullptr;
            std::size_t chosenBot = living.size();
            float chosenScore = 0.0f;

            // Quotas are filled before anybody is stacked past one. Without
            // the first pass the nearest base wins every spare body on
            // distance alone, and the opening is a ball on the home base
            // instead of a team spread over the bases that decide the match.
            for (int pass = 0; pass < 2 && !chosen; ++pass)
            {
                for (NodeState& state : nodes)
                {
                    if (pass == 0 && state.assigned >= state.quota)
                        continue;

                    bool const defensive = IsFriendlySideStatus(state.status) &&
                        state.status != BattlegroundNodeStatus::FriendlyUnderAttack;
                    if (defensive && capDefenders && defenders >= maxDefenders)
                        continue;

                    float cost = 0.0f;
                    std::size_t const candidate = findNearestFreeBot(state, cost);
                    if (candidate == living.size())
                        continue;

                    float score = float(state.score);
                    score -= float(state.assigned) * float(defensive ? kCrowdPenaltyDefense : kCrowdPenaltyAssault);
                    if (state.assigned >= state.quota)
                        score -= float(kOverflowPenalty) * float(state.assigned - state.quota + 1);
                    score -= std::max(0.0f, cost) * kDistanceScoreWeight;

                    if (!chosen || score > chosenScore)
                    {
                        chosen = &state;
                        chosenBot = candidate;
                        chosenScore = score;
                    }
                }
            }

            // Unreachable while there is a base left: the cap never closes the
            // last one. Guards the loop against an empty map all the same.
            if (!chosen)
                break;

            taken[chosenBot] = true;
            ++assignedCount;
            ++chosen->assigned;
            if (IsFriendlySideStatus(chosen->status) && chosen->status != BattlegroundNodeStatus::FriendlyUnderAttack)
                ++defenders;

            plan.assignments[living[chosenBot]->GetGUID()] = { chosen->nodeId, RoleForStatus(chosen->status) };
        }

        // A dead bot keeps the base it had so it walks back to the same place
        // it was fighting for; one that never had a base takes the nearest.
        for (Player* bot : bots)
        {
            ObjectGuid const guid = bot->GetGUID();
            if (plan.assignments.count(guid))
                continue;

            auto const previousItr = previous.find(guid);
            if (previousItr != previous.end())
            {
                auto const keptItr = std::find_if(nodes.begin(), nodes.end(), [&](NodeState const& candidate)
                    { return candidate.nodeId == previousItr->second.nodeId; });
                if (keptItr != nodes.end())
                {
                    plan.assignments[guid] = { keptItr->nodeId, RoleForStatus(keptItr->status) };
                    continue;
                }
            }

            NodeState const* nearest = nullptr;
            float nearestDistance = std::numeric_limits<float>::max();
            for (NodeState const& state : nodes)
            {
                float const distance = bot->GetExactDist(state.location.GetPositionX(),
                    state.location.GetPositionY(), state.location.GetPositionZ());
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearest = &state;
                }
            }

            if (nearest)
                plan.assignments[guid] = { nearest->nodeId, RoleForStatus(nearest->status) };
        }

        if (owned != plan.ownedCount || enemyOwned != plan.enemyOwnedCount)
            TC_LOG_DEBUG("playerbots.pvp.nodes",
                "Playerbot base plan: bg={} team={} held={} theirs={} bots={} defenders={} cap={}.",
                battleground->GetInstanceID(), team == TEAM_ALLIANCE ? "alliance" : "horde",
                owned, enemyOwned, living.size(), defenders, maxDefenders);

        plan.nodes = std::move(nodes);
        plan.ownedCount = owned;
        plan.enemyOwnedCount = enemyOwned;
        plan.livingBots = uint32(living.size());
    }
}

namespace playerbot
{
char const* GetNodeRoleName(NodeRole role)
{
    switch (role)
    {
        case NodeRole::Recapture: return "recapture";
        case NodeRole::Defend:    return "defend";
        case NodeRole::Claim:     return "claim";
        case NodeRole::Assault:   return "assault";
        case NodeRole::Roam:      return "roam";
        case NodeRole::None:
        default:                  return "none";
    }
}

bool NodeCoordinator::IsNodeBattleground(Battleground const* battleground)
{
    return battleground && !battleground->isArena() && battleground->GetDynamicNodeCount() > 0;
}

bool NodeCoordinator::GetOrders(Player const* bot, NodeBotOrders& orders)
{
    orders = NodeBotOrders();
    if (!bot || !bot->IsInWorld() || !bot->InBattleground())
        return false;

    Battleground* battleground = bot->GetBattleground();
    if (!IsNodeBattleground(battleground) || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    Map* map = bot->FindMap();
    if (!map || map->GetInstanceId() != battleground->GetInstanceID())
        return false;

    TeamId const team = ResolveTeam(battleground, bot);
    if (team == TEAM_NEUTRAL)
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    TeamPlan& plan = TouchTeamPlan(BuildPlanKey(battleground, team), nowMs);

    if (plan.lastRefreshMs == 0 || nowMs - plan.lastRefreshMs >= kPlanRefreshMs)
        RefreshTeamPlan(battleground, map, team, bot->GetGUID(), plan, nowMs);

    auto const assignmentItr = plan.assignments.find(bot->GetGUID());
    if (assignmentItr == plan.assignments.end())
    {
        orders.role = NodeRole::Roam;
        return true;
    }

    // The plan's snapshot is up to a rebuild old, and a base can flip inside
    // that window. The banner and the state are re-read now so a bot never
    // clicks at a base that already changed hands under it.
    BattlegroundNodeObjective objective;
    if (!battleground->GetDynamicNodeInfo(bot->GetGUID(), assignmentItr->second.nodeId, objective))
    {
        orders.role = NodeRole::Roam;
        return true;
    }

    orders.hasNode = true;
    orders.nodeId = objective.NodeId;
    orders.status = objective.Status;
    orders.location = objective.Location;
    orders.bannerGuid = objective.BannerGuid;
    orders.role = RoleForStatus(objective.Status);
    orders.interact = orders.role != NodeRole::Defend;

    for (NodeState const& state : plan.nodes)
    {
        if (state.nodeId != objective.NodeId)
            continue;

        orders.enemiesAtNode = state.enemiesNear;
        break;
    }

    switch (orders.role)
    {
        case NodeRole::Recapture:
            orders.leash = true;
            orders.leashRange = kRecaptureLeashRange;
            break;
        case NodeRole::Defend:
            orders.leash = true;
            orders.leashRange = kDefendLeashRange;
            break;
        default:
            break;
    }

    return true;
}

std::vector<std::string> NodeCoordinator::DescribeTeams(Player const* observer)
{
    std::vector<std::string> lines;
    if (!observer || !observer->InBattleground())
        return lines;

    Battleground* battleground = observer->GetBattleground();
    if (!IsNodeBattleground(battleground))
    {
        lines.emplace_back("Not a base-capture battleground.");
        return lines;
    }

    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        TeamPlan const* plan = LockedFind(g_NodeTeamPlans, BuildPlanKey(battleground, team));
        if (!plan || plan->nodes.empty())
            continue;

        std::ostringstream header;
        header << (team == TEAM_ALLIANCE ? "Alliance" : "Horde") << ": " << plan->ownedCount << " bases held, "
            << plan->enemyOwnedCount << " to them, " << plan->livingBots << " bots up.";
        lines.push_back(header.str());

        std::unordered_map<uint32, uint32> assignedByNode;
        for (auto const& assignment : plan->assignments)
            ++assignedByNode[assignment.second.nodeId];

        for (NodeState const& state : plan->nodes)
        {
            std::ostringstream line;
            line << "  base " << state.nodeId << " " << GetNodeStatusName(state.status)
                << " (" << GetNodeGroundName(state.ground) << ")"
                << ": score " << state.score << ", wants " << state.quota
                << ", sent " << assignedByNode[state.nodeId]
                << ", enemies " << state.enemiesNear << ", allies " << state.alliesNear << ".";
            lines.push_back(line.str());
        }
    }

    if (lines.empty())
        lines.emplace_back("No base plan yet: no bots have evaluated this match.");

    return lines;
}
}
