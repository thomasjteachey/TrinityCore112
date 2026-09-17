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

#include "PlayerbotCtfCoordinator.h"
#include "PlayerbotObcClone.h"
#include "PlayerbotPvpCore.h"
#include "PlayerbotRandomBotParticipation.h"
#include "PlayerbotSharedStateGuard.h"
#include "Battleground.h"
#include "BattlegroundTP.h"
#include "BattlegroundWS.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using playerbot::CtfRole;

    // Both battlegrounds number their flag states the same way
    // (BG_WS_FLAG_STATE_* and BG_TP_FLAG_STATE_*).
    constexpr uint8 kFlagOnBase = 0;
    constexpr uint8 kFlagWaitRespawn = 1;
    constexpr uint8 kFlagOnPlayer = 2;
    constexpr uint8 kFlagOnGround = 3;

    constexpr uint32 kPlanRefreshMs = 400;
    constexpr uint32 kPlanPruneIntervalMs = 60 * IN_MILLISECONDS;
    constexpr uint32 kPlanStaleMs = 10 * MINUTE * IN_MILLISECONDS;
    // Class and talents do not change inside a match; the protection check
    // walks the talent store, so it is not repeated every plan rebuild.
    constexpr uint32 kRunnerPriorityCacheMs = 30 * IN_MILLISECONDS;

    constexpr float kClickNowRange = 10.0f;
    // "Near the flag" for a bot that is not the runner. A dropped flag goes
    // home on a timer, so the reach for one on the ground is a little longer.
    constexpr float kOpportunisticStandRange = 20.0f;
    constexpr float kOpportunisticGroundRange = 25.0f;
    // The runner is already on top of the flag: a closer escort leaves it.
    constexpr float kLeaveToRunnerRange = 15.0f;
    // Somebody at the keyboard standing on a flag gets to take it.
    constexpr float kHumanCourtesyRange = 6.0f;

    // Our dropped flag: anybody this close returns it, and the two closest
    // bots come for it from further out.
    constexpr float kReturnAnyoneRange = 30.0f;
    constexpr float kReturnClosestRange = 90.0f;
    constexpr std::size_t kReturnClosestCount = 2;
    // A runner on its way to the enemy flag only detours for a close return.
    constexpr float kRunnerDetourForReturnRange = 40.0f;

    constexpr float kCarrierHoldRange = 20.0f;

    constexpr float kHandoffStartRange = 30.0f;
    constexpr float kHandoffAbortRange = 45.0f;
    constexpr float kHandoffEnemyClearRange = 40.0f;
    constexpr float kHandoffDropRange = 5.0f;
    // A carrier this close to scoring just scores.
    constexpr float kHandoffNearCaptureRange = 35.0f;
    constexpr uint32 kHandoffConvergeTimeoutMs = 12 * IN_MILLISECONDS;
    constexpr uint32 kHandoffPickupWindowMs = 3500;
    constexpr uint32 kHandoffAbortCooldownMs = 15 * IN_MILLISECONDS;
    constexpr uint32 kHandoffDoneCooldownMs = 5 * IN_MILLISECONDS;

    constexpr uint32 kRecentlyDroppedFlagSpellId = 42792;

    // Where each flag stands, from BattlegroundWS/BattlegroundTP::SetupBattleground.
    // Used only when the stand object itself cannot be found.
    Position const kWsgAllianceFlagStand(1540.423f, 1481.325f, 351.8284f, 0.0f);
    Position const kWsgHordeFlagStand(916.0226f, 1434.405f, 345.413f, 0.0f);
    Position const kTpAllianceFlagStand(2118.210f, 191.621f, 44.052f, 0.0f);
    Position const kTpHordeFlagStand(1578.380f, 344.037f, 2.419f, 0.0f);

    enum class HandoffPhase : uint8
    {
        None = 0,
        Converging,
        Dropped
    };

    char const* GetHandoffPhaseName(HandoffPhase phase)
    {
        switch (phase)
        {
            case HandoffPhase::Converging: return "converging";
            case HandoffPhase::Dropped: return "dropped";
            case HandoffPhase::None:
            default: return "none";
        }
    }

    struct TeamPlan
    {
        uint32 lastRefreshMs = 0;
        uint32 lastSeenMs = 0;
        ObjectGuid runnerGuid;
        std::unordered_map<ObjectGuid, CtfRole> roles;
        std::vector<ObjectGuid> returnDuty;

        HandoffPhase handoffPhase = HandoffPhase::None;
        ObjectGuid handoffGiver;
        ObjectGuid handoffReceiver;
        ObjectGuid handoffFlagGuid;
        uint32 handoffPhaseStartMs = 0;
        uint32 handoffCooldownUntilMs = 0;
    };

    struct RunnerPriorityCacheEntry
    {
        uint8 tier = 0;
        uint32 computedMs = 0;
    };

    std::unordered_map<uint64, TeamPlan> g_CtfTeamPlans;
    std::unordered_map<uint64, RunnerPriorityCacheEntry> g_RunnerPriorityCache;
    uint32 g_LastPlanPruneMs = 0;

    struct FlagSnapshot
    {
        TeamId team = TEAM_NEUTRAL;
        TeamId enemyTeam = TEAM_NEUTRAL;
        uint8 ownFlagState = kFlagOnBase;
        uint8 enemyFlagState = kFlagOnBase;
        ObjectGuid ownDroppedFlagGuid;
        ObjectGuid enemyDroppedFlagGuid;
        ObjectGuid enemyStandFlagGuid;
        ObjectGuid teamCarrierGuid;   // holds the enemy flag
        ObjectGuid enemyCarrierGuid;  // holds our flag
        Position ownFlagStand;
        Position enemyFlagStand;
    };

    char const* GetFlagStateName(uint8 state)
    {
        switch (state)
        {
            case kFlagOnBase: return "base";
            case kFlagWaitRespawn: return "respawning";
            case kFlagOnPlayer: return "carried";
            case kFlagOnGround: return "ground";
            default: return "unknown";
        }
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

    // Only a FAR teleport takes a player out of the match. A near one - Blink,
    // the airborne-stranded recovery - is acknowledged on the bot's own next
    // update, and counting it as a loss would hand the runner's job to someone
    // else for that moment and reshuffle every role twice.
    bool IsAliveParticipant(Player const* player)
    {
        return player && player->IsInWorld() && player->IsAlive() &&
            !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) &&
            !player->IsBeingTeleportedFar() && !player->IsSpectator() && !player->IsGameMaster();
    }

    bool IsSupportClass(Player const* player)
    {
        switch (player->GetClass())
        {
            case CLASS_PRIEST:
            case CLASS_PALADIN:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
            default:
                return false;
        }
    }

    // Classes that hold a flag room well: they see stealth coming, lock a
    // doorway down, or sit in stealth themselves.
    bool IsRoomHoldingClass(Player const* player)
    {
        switch (player->GetClass())
        {
            case CLASS_HUNTER:
            case CLASS_WARLOCK:
            case CLASS_MAGE:
            case CLASS_ROGUE:
                return true;
            default:
                return false;
        }
    }

    uint64 BuildPlanKey(Battleground const* battleground, TeamId team)
    {
        return (uint64(battleground->GetTypeID()) << 40) |
            (uint64(battleground->GetInstanceID()) << 1) |
            uint64(team == TEAM_HORDE ? 1 : 0);
    }

    Position ResolveStandPosition(Battleground* battleground, uint32 objectType, Position const& fallback)
    {
        if (GameObject* stand = battleground->GetBGObject(objectType, false))
            return stand->GetPosition();
        return fallback;
    }

    bool BuildFlagSnapshot(Battleground* battleground, TeamId team, FlagSnapshot& snapshot)
    {
        snapshot = FlagSnapshot();
        if (!battleground || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
            return false;

        snapshot.team = team;
        snapshot.enemyTeam = team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;

        if (BattlegroundWS* warsong = dynamic_cast<BattlegroundWS*>(battleground))
        {
            // BattlegroundWS keys flag states and dropped flags by ALLIANCE /
            // HORDE, but its flag keepers by TeamId. A TeamId passed to the
            // first group silently reads the OTHER team's flag.
            uint32 const ownFaction = team == TEAM_ALLIANCE ? ALLIANCE : HORDE;
            uint32 const enemyFaction = team == TEAM_ALLIANCE ? HORDE : ALLIANCE;
            snapshot.ownFlagState = warsong->GetFlagState(ownFaction);
            snapshot.enemyFlagState = warsong->GetFlagState(enemyFaction);
            snapshot.ownDroppedFlagGuid = warsong->GetDroppedFlagGUID(ownFaction);
            snapshot.enemyDroppedFlagGuid = warsong->GetDroppedFlagGUID(enemyFaction);
            snapshot.teamCarrierGuid = warsong->GetFlagPickerGUID(snapshot.enemyTeam);
            snapshot.enemyCarrierGuid = warsong->GetFlagPickerGUID(team);

            uint32 const ownStandType = team == TEAM_ALLIANCE ? BG_WS_OBJECT_A_FLAG : BG_WS_OBJECT_H_FLAG;
            uint32 const enemyStandType = team == TEAM_ALLIANCE ? BG_WS_OBJECT_H_FLAG : BG_WS_OBJECT_A_FLAG;
            if (GameObject* enemyStand = warsong->GetBGObject(enemyStandType, false))
                snapshot.enemyStandFlagGuid = enemyStand->GetGUID();
            snapshot.ownFlagStand = ResolveStandPosition(warsong, ownStandType,
                team == TEAM_ALLIANCE ? kWsgAllianceFlagStand : kWsgHordeFlagStand);
            snapshot.enemyFlagStand = ResolveStandPosition(warsong, enemyStandType,
                team == TEAM_ALLIANCE ? kWsgHordeFlagStand : kWsgAllianceFlagStand);
            return true;
        }

        if (BattlegroundTP* twinPeaks = dynamic_cast<BattlegroundTP*>(battleground))
        {
            snapshot.ownFlagState = twinPeaks->GetFlagState(team);
            snapshot.enemyFlagState = twinPeaks->GetFlagState(snapshot.enemyTeam);
            snapshot.ownDroppedFlagGuid = twinPeaks->GetDroppedFlagGUID(team);
            snapshot.enemyDroppedFlagGuid = twinPeaks->GetDroppedFlagGUID(snapshot.enemyTeam);
            snapshot.teamCarrierGuid = twinPeaks->GetFlagPickerGUID(snapshot.enemyTeam);
            snapshot.enemyCarrierGuid = twinPeaks->GetFlagPickerGUID(team);

            uint32 const ownStandType = team == TEAM_ALLIANCE ? BG_TP_OBJECT_A_FLAG : BG_TP_OBJECT_H_FLAG;
            uint32 const enemyStandType = team == TEAM_ALLIANCE ? BG_TP_OBJECT_H_FLAG : BG_TP_OBJECT_A_FLAG;
            if (GameObject* enemyStand = twinPeaks->GetBGObject(enemyStandType, false))
                snapshot.enemyStandFlagGuid = enemyStand->GetGUID();
            snapshot.ownFlagStand = ResolveStandPosition(twinPeaks, ownStandType,
                team == TEAM_ALLIANCE ? kTpAllianceFlagStand : kTpHordeFlagStand);
            snapshot.enemyFlagStand = ResolveStandPosition(twinPeaks, enemyStandType,
                team == TEAM_ALLIANCE ? kTpHordeFlagStand : kTpAllianceFlagStand);
            return true;
        }

        return false;
    }

    // Values are copied in and out under the structure lock, never held by
    // reference: the plan sweep prunes this cache from whichever map thread
    // runs it (every clone arrives with a new guid, so it would only grow).
    uint8 GetCachedRunnerPriority(Player const* player, uint32 nowMs)
    {
        uint64 const key = player->GetGUID().GetRawValue();
        RunnerPriorityCacheEntry cached;
        if (playerbot::LockedGetCopy(g_RunnerPriorityCache, key, cached) &&
            cached.computedMs != 0 && nowMs - cached.computedMs < kRunnerPriorityCacheMs)
            return cached.tier;

        RunnerPriorityCacheEntry fresh;
        fresh.tier = playerbot::PvpCore::GetFlagRunnerPriority(player);
        fresh.computedMs = nowMs ? nowMs : 1;
        playerbot::LockedSet(g_RunnerPriorityCache, key, fresh);
        return fresh.tier;
    }

    // An enemy the viewer can actually see (stealth hides a rogue here exactly
    // as it would from a person) within range.
    bool HasVisibleEnemyNear(Battleground const* battleground, Map* map, Player const* viewer, TeamId team, float range)
    {
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player const* other = itr->GetSource();
            if (!other || other == viewer || !IsAliveParticipant(other))
                continue;

            TeamId const otherTeam = ResolveTeam(battleground, other);
            if (otherTeam == TEAM_NEUTRAL || otherTeam == team)
                continue;

            if (!viewer->IsWithinDistInMap(other, range) || !viewer->IsValidAttackTarget(other))
                continue;

            return true;
        }

        return false;
    }

    bool HumanTeammateClaimsFlag(Battleground const* battleground, Map* map, Player const* bot, TeamId team, GameObject const* flag)
    {
        float const botDistance = bot->GetDistance(flag);
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player const* other = itr->GetSource();
            if (!other || other == bot || !IsAliveParticipant(other))
                continue;

            // Cheap filters first; the bot test takes two global locks.
            float const distance = other->GetDistance(flag);
            if (distance > kHumanCourtesyRange || distance >= botDistance)
                continue;
            if (ResolveTeam(battleground, other) != team || IsBotControlled(other))
                continue;

            return true;
        }

        return false;
    }

    // Our flag is home and the carrier is nearly in its capture room: scoring
    // beats any handoff. The handoff walk never enters the capture trigger's
    // code path, so a carrier left on it could stand next to a free point.
    bool CanCarrierScoreSoon(FlagSnapshot const& flags, Player const* carrier)
    {
        return flags.ownFlagState == kFlagOnBase && carrier->GetExactDist(flags.ownFlagStand) <= kHandoffNearCaptureRange;
    }

    // Conditions that must keep holding for the whole handoff, not just at the
    // start: both alive, still together, and nobody hostile in sight of either.
    bool IsHandoffStillSafe(Battleground const* battleground, Map* map, FlagSnapshot const& flags,
        Player const* giver, Player const* receiver)
    {
        if (!IsAliveParticipant(giver) || !IsAliveParticipant(receiver))
            return false;
        if (!giver->IsWithinDistInMap(receiver, kHandoffAbortRange))
            return false;

        return !HasVisibleEnemyNear(battleground, map, giver, flags.team, kHandoffEnemyClearRange) &&
            !HasVisibleEnemyNear(battleground, map, receiver, flags.team, kHandoffEnemyClearRange);
    }

    bool CanStartHandoff(Battleground const* battleground, Map* map, FlagSnapshot const& flags,
        Player const* giver, Player const* receiver, uint32 nowMs)
    {
        // Only bots are asked to give up a flag, and only a bot is handed one.
        if (!IsBotControlled(giver) || !IsBotControlled(receiver))
            return false;
        if (flags.enemyFlagState != kFlagOnPlayer)
            return false;

        // Only to a better runner. An equal one never comes up: holding the flag
        // outranks holding the job, so a carrier of the runner's class already
        // IS the runner.
        if (GetCachedRunnerPriority(receiver, nowMs) <= GetCachedRunnerPriority(giver, nowMs))
            return false;

        if (!giver->IsWithinDistInMap(receiver, kHandoffStartRange) || !giver->IsWithinLOSInMap(receiver))
            return false;

        if (CanCarrierScoreSoon(flags, giver))
            return false;

        if (receiver->HasAura(kRecentlyDroppedFlagSpellId) || receiver->GetVehicle())
            return false;

        return IsHandoffStillSafe(battleground, map, flags, giver, receiver);
    }

    // Registers this plan as in use and, at most once a minute, drops plans of
    // matches nobody has evaluated for ten minutes. Both happen under the
    // structure lock, so a plan that another map is using this tick is always
    // fresh and never pruned out from under it.
    TeamPlan& TouchTeamPlan(uint64 key, uint32 nowMs)
    {
        std::lock_guard<std::mutex> guard(playerbot::SharedBotStateStructureLock());
        if (nowMs - g_LastPlanPruneMs >= kPlanPruneIntervalMs)
        {
            g_LastPlanPruneMs = nowMs;
            for (auto itr = g_CtfTeamPlans.begin(); itr != g_CtfTeamPlans.end();)
            {
                if (itr->first != key && nowMs - itr->second.lastSeenMs >= kPlanStaleMs)
                    itr = g_CtfTeamPlans.erase(itr);
                else
                    ++itr;
            }

            for (auto itr = g_RunnerPriorityCache.begin(); itr != g_RunnerPriorityCache.end();)
            {
                if (nowMs - itr->second.computedMs >= kPlanStaleMs)
                    itr = g_RunnerPriorityCache.erase(itr);
                else
                    ++itr;
            }
        }

        TeamPlan& plan = g_CtfTeamPlans[key];
        plan.lastSeenMs = nowMs;
        return plan;
    }

    void EndHandoff(TeamPlan& plan, uint32 nowMs, uint32 cooldownMs, char const* reason)
    {
        TC_LOG_INFO("playerbots.pvp.ctf", "CTF handoff ended: giver={} receiver={} phase={} reason={}.",
            plan.handoffGiver.ToString(), plan.handoffReceiver.ToString(), GetHandoffPhaseName(plan.handoffPhase), reason);

        plan.handoffPhase = HandoffPhase::None;
        plan.handoffGiver.Clear();
        plan.handoffReceiver.Clear();
        plan.handoffFlagGuid.Clear();
        plan.handoffPhaseStartMs = nowMs;
        plan.handoffCooldownUntilMs = nowMs + cooldownMs;
    }

    void RefreshTeamPlan(Battleground* battleground, Map* map, FlagSnapshot const& flags, TeamPlan& plan, uint32 nowMs)
    {
        plan.lastRefreshMs = nowMs;

        struct Member
        {
            Player* player;
            uint8 tier;
            bool alive;
        };

        std::vector<Member> bots;
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || member->IsSpectator() || member->IsGameMaster())
                continue;
            if (ResolveTeam(battleground, member) != flags.team || !IsBotControlled(member))
                continue;

            bots.push_back({ member, GetCachedRunnerPriority(member, nowMs), IsAliveParticipant(member) });
        }

        // The runner: best class first. Holding the flag outweighs sitting in
        // the job, and sitting in the job outweighs nothing, but neither
        // outweighs a whole class step - a druid always takes over.
        Player* runner = nullptr;
        int32 bestScore = std::numeric_limits<int32>::min();
        for (Member const& member : bots)
        {
            if (!member.alive)
                continue;

            int32 score = int32(member.tier) * 100;
            if (member.player->GetGUID() == flags.teamCarrierGuid)
                score += 60;
            if (member.player->GetGUID() == plan.runnerGuid)
                score += 50;

            if (!runner || score > bestScore || (score == bestScore && member.player->GetGUID() < runner->GetGUID()))
            {
                runner = member.player;
                bestScore = score;
            }
        }

        ObjectGuid const runnerGuid = runner ? runner->GetGUID() : ObjectGuid::Empty;
        if (runnerGuid != plan.runnerGuid)
        {
            TC_LOG_INFO("playerbots.pvp.ctf", "CTF flag runner: bg={} team={} runner={} ({}) tier={} previous={}.",
                battleground->GetInstanceID(), flags.team == TEAM_ALLIANCE ? "alliance" : "horde",
                runner ? runner->GetName() : std::string("none"), runnerGuid.ToString(),
                runner ? GetCachedRunnerPriority(runner, nowMs) : 0, plan.runnerGuid.ToString());
            plan.runnerGuid = runnerGuid;
        }

        // Everybody else, dead or alive, so a death does not reshuffle the
        // whole team. Support classes escort first; room holders defend.
        std::vector<Player*> others;
        others.reserve(bots.size());
        for (Member const& member : bots)
            if (member.player != runner && member.player->GetGUID() != flags.teamCarrierGuid)
                others.push_back(member.player);

        std::sort(others.begin(), others.end(), [](Player const* left, Player const* right)
        {
            bool const leftSupport = IsSupportClass(left);
            bool const rightSupport = IsSupportClass(right);
            if (leftSupport != rightSupport)
                return leftSupport;
            return left->GetGUID() < right->GetGUID();
        });

        std::size_t const count = others.size();
        std::size_t const escortCount = count <= 2 ? count : std::size_t(std::lround(float(count) * 0.4f));
        std::size_t const defenderCount = count >= 4 ? std::max<std::size_t>(1, count / 5) : 0;

        plan.roles.clear();
        for (std::size_t i = 0; i < escortCount && i < count; ++i)
            plan.roles[others[i]->GetGUID()] = CtfRole::Escort;

        std::vector<Player*> remaining(others.begin() + std::min(escortCount, count), others.end());
        std::sort(remaining.begin(), remaining.end(), [](Player const* left, Player const* right)
        {
            bool const leftHolder = IsRoomHoldingClass(left);
            bool const rightHolder = IsRoomHoldingClass(right);
            if (leftHolder != rightHolder)
                return leftHolder;
            return left->GetGUID() < right->GetGUID();
        });

        for (std::size_t i = 0; i < remaining.size(); ++i)
            plan.roles[remaining[i]->GetGUID()] = i < defenderCount ? CtfRole::Defender : CtfRole::Midfield;

        // The bots closest to our dropped flag go back for it.
        plan.returnDuty.clear();
        if (flags.ownFlagState == kFlagOnGround && !flags.ownDroppedFlagGuid.IsEmpty())
        {
            if (GameObject* dropped = map->GetGameObject(flags.ownDroppedFlagGuid))
            {
                std::vector<std::pair<float, ObjectGuid>> byDistance;
                for (Member const& member : bots)
                {
                    if (!member.alive || member.player->GetGUID() == flags.teamCarrierGuid)
                        continue;

                    float const distance = member.player->GetDistance(dropped);
                    if (distance <= kReturnClosestRange)
                        byDistance.emplace_back(distance, member.player->GetGUID());
                }

                std::sort(byDistance.begin(), byDistance.end(),
                    [](std::pair<float, ObjectGuid> const& left, std::pair<float, ObjectGuid> const& right)
                    {
                        return left.first < right.first;
                    });

                for (std::size_t i = 0; i < byDistance.size() && i < kReturnClosestCount; ++i)
                    plan.returnDuty.push_back(byDistance[i].second);
            }
        }

        // Handoff state machine.
        Player* giver = flags.teamCarrierGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(map, flags.teamCarrierGuid);
        switch (plan.handoffPhase)
        {
            case HandoffPhase::None:
                if (nowMs >= plan.handoffCooldownUntilMs && giver && runner && giver != runner &&
                    CanStartHandoff(battleground, map, flags, giver, runner, nowMs))
                {
                    plan.handoffPhase = HandoffPhase::Converging;
                    plan.handoffGiver = giver->GetGUID();
                    plan.handoffReceiver = runner->GetGUID();
                    plan.handoffFlagGuid.Clear();
                    plan.handoffPhaseStartMs = nowMs;
                    TC_LOG_INFO("playerbots.pvp.ctf", "CTF handoff started: bg={} giver={} ({}) receiver={} ({}) distance={:.1f}.",
                        battleground->GetInstanceID(), giver->GetName(), giver->GetGUID().ToString(),
                        runner->GetName(), runner->GetGUID().ToString(), giver->GetDistance(runner));
                }
                break;
            case HandoffPhase::Converging:
            {
                Player* receiver = ObjectAccessor::GetPlayer(map, plan.handoffReceiver);
                if (!giver || giver->GetGUID() != plan.handoffGiver)
                    EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "giver-no-longer-carrying");
                else if (!receiver || plan.handoffReceiver != runnerGuid)
                    EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "receiver-no-longer-runner");
                else if (nowMs - plan.handoffPhaseStartMs >= kHandoffConvergeTimeoutMs)
                    EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "timed-out");
                else if (CanCarrierScoreSoon(flags, giver))
                    EndHandoff(plan, nowMs, kHandoffDoneCooldownMs, "carrier-can-score");
                else if (!IsHandoffStillSafe(battleground, map, flags, giver, receiver))
                    EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "unsafe");
                break;
            }
            case HandoffPhase::Dropped:
            {
                Player* receiver = ObjectAccessor::GetPlayer(map, plan.handoffReceiver);
                if (!flags.teamCarrierGuid.IsEmpty() && flags.teamCarrierGuid == plan.handoffReceiver)
                    EndHandoff(plan, nowMs, kHandoffDoneCooldownMs, "completed");
                else if (!flags.teamCarrierGuid.IsEmpty())
                    EndHandoff(plan, nowMs, kHandoffDoneCooldownMs, "taken-by-someone-else");
                else if (flags.enemyFlagState != kFlagOnGround)
                    EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "flag-returned");
                else if (!receiver || !IsAliveParticipant(receiver))
                    EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "receiver-lost");
                else if (nowMs - plan.handoffPhaseStartMs >= kHandoffPickupWindowMs)
                    EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "pickup-window-expired");
                break;
            }
        }
    }

    void ResolvePickup(Battleground const* battleground, Map* map, FlagSnapshot const& flags, TeamPlan const& plan,
        Player const* bot, playerbot::CtfBotOrders& orders)
    {
        ObjectGuid const botGuid = bot->GetGUID();

        GameObject* ownDropped = nullptr;
        float ownDroppedDistance = std::numeric_limits<float>::max();
        if (flags.ownFlagState == kFlagOnGround && !flags.ownDroppedFlagGuid.IsEmpty())
        {
            ownDropped = map->GetGameObject(flags.ownDroppedFlagGuid);
            if (ownDropped && ownDropped->IsInWorld())
                ownDroppedDistance = bot->GetDistance(ownDropped);
            else
                ownDropped = nullptr;
        }

        bool const returnDuty = ownDropped && (ownDroppedDistance <= kReturnAnyoneRange ||
            std::find(plan.returnDuty.begin(), plan.returnDuty.end(), botGuid) != plan.returnDuty.end());

        GameObject* enemyFlag = nullptr;
        bool enemyFlagOnGround = false;
        if (flags.enemyFlagState == kFlagOnBase && !flags.enemyStandFlagGuid.IsEmpty())
            enemyFlag = map->GetGameObject(flags.enemyStandFlagGuid);
        else if (flags.enemyFlagState == kFlagOnGround && !flags.enemyDroppedFlagGuid.IsEmpty())
        {
            enemyFlag = map->GetGameObject(flags.enemyDroppedFlagGuid);
            enemyFlagOnGround = true;
        }

        if (enemyFlag && !enemyFlag->IsInWorld())
            enemyFlag = nullptr;

        bool enemyPickupAllowed = false;
        float enemyFlagDistance = std::numeric_limits<float>::max();
        if (enemyFlag)
        {
            enemyFlagDistance = bot->GetDistance(enemyFlag);
            if (orders.isDesignatedRunner)
                enemyPickupAllowed = true;
            else
            {
                enemyPickupAllowed = enemyFlagDistance <= (enemyFlagOnGround ? kOpportunisticGroundRange : kOpportunisticStandRange);

                if (enemyPickupAllowed && !plan.runnerGuid.IsEmpty())
                {
                    Player const* runner = ObjectAccessor::GetPlayer(map, plan.runnerGuid);
                    if (runner && IsAliveParticipant(runner))
                    {
                        float const runnerDistance = runner->GetDistance(enemyFlag);
                        if (runnerDistance <= kLeaveToRunnerRange && runnerDistance < enemyFlagDistance)
                            enemyPickupAllowed = false;
                    }
                }
            }

            // A flag just put down for the runner belongs to the runner.
            if (enemyPickupAllowed && enemyFlagOnGround && plan.handoffPhase == HandoffPhase::Dropped &&
                plan.handoffReceiver != botGuid)
                enemyPickupAllowed = false;

            if (enemyPickupAllowed && enemyFlagDistance <= kOpportunisticGroundRange &&
                HumanTeammateClaimsFlag(battleground, map, bot, flags.team, enemyFlag))
                enemyPickupAllowed = false;
        }

        // A runner bound for the enemy flag leaves a distant return to others.
        bool const takeReturn = returnDuty &&
            (!enemyPickupAllowed || !orders.isDesignatedRunner || ownDroppedDistance <= kRunnerDetourForReturnRange);
        if (takeReturn)
        {
            orders.pickupGuid = ownDropped->GetGUID();
            orders.pickupIsReturn = true;
            orders.pickupNearby = ownDroppedDistance <= kClickNowRange;
            return;
        }

        if (enemyPickupAllowed)
        {
            orders.pickupGuid = enemyFlag->GetGUID();
            orders.pickupIsOpportunistic = !orders.isDesignatedRunner;
            orders.pickupNearby = enemyFlagDistance <= kClickNowRange;
        }
    }

    // Plan lookup without creating or refreshing one, for the reservation
    // queries on the pickup path.
    TeamPlan const* FindTeamPlan(Player const* bot)
    {
        if (!bot || !bot->InBattleground())
            return nullptr;

        Battleground const* battleground = bot->GetBattleground();
        if (!playerbot::CtfCoordinator::IsTwoFlagCtf(battleground))
            return nullptr;

        TeamId const team = ResolveTeam(battleground, bot);
        if (team == TEAM_NEUTRAL)
            return nullptr;

        return playerbot::LockedFind(g_CtfTeamPlans, BuildPlanKey(battleground, team));
    }
}

namespace playerbot
{
char const* GetCtfRoleName(CtfRole role)
{
    switch (role)
    {
        case CtfRole::Carrier: return "carrier";
        case CtfRole::Runner: return "runner";
        case CtfRole::Escort: return "escort";
        case CtfRole::Defender: return "defender";
        case CtfRole::Midfield: return "midfield";
        case CtfRole::None:
        default: return "none";
    }
}

bool CtfCoordinator::IsTwoFlagCtf(Battleground const* battleground)
{
    return battleground &&
        (dynamic_cast<BattlegroundWS const*>(battleground) || dynamic_cast<BattlegroundTP const*>(battleground));
}

bool CtfCoordinator::GetOrders(Player const* bot, CtfBotOrders& orders)
{
    orders = CtfBotOrders();
    if (!bot || !bot->IsInWorld() || !bot->InBattleground())
        return false;

    Battleground* battleground = bot->GetBattleground();
    if (!IsTwoFlagCtf(battleground) || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    Map* map = bot->FindMap();
    if (!map || map->GetInstanceId() != battleground->GetInstanceID())
        return false;

    TeamId const team = ResolveTeam(battleground, bot);
    FlagSnapshot flags;
    if (team == TEAM_NEUTRAL || !BuildFlagSnapshot(battleground, team, flags))
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    TeamPlan& plan = TouchTeamPlan(BuildPlanKey(battleground, team), nowMs);

    // A dead runner is replaced on the very next look, not at the next rebuild.
    bool runnerLost = false;
    if (!plan.runnerGuid.IsEmpty())
    {
        Player const* runner = ObjectAccessor::GetPlayer(map, plan.runnerGuid);
        runnerLost = !runner || !IsAliveParticipant(runner);
    }

    if (plan.lastRefreshMs == 0 || nowMs - plan.lastRefreshMs >= kPlanRefreshMs || runnerLost)
        RefreshTeamPlan(battleground, map, flags, plan, nowMs);

    ObjectGuid const botGuid = bot->GetGUID();
    orders.runnerGuid = plan.runnerGuid;
    orders.teamCarrierGuid = flags.teamCarrierGuid;
    orders.enemyCarrierGuid = flags.enemyCarrierGuid;
    orders.enemyFlagPickable = flags.enemyFlagState == kFlagOnBase || flags.enemyFlagState == kFlagOnGround;
    orders.ownFlagAtBase = flags.ownFlagState == kFlagOnBase;
    orders.ownFlagStand = flags.ownFlagStand;
    orders.enemyFlagStand = flags.enemyFlagStand;
    orders.isDesignatedRunner = !plan.runnerGuid.IsEmpty() && plan.runnerGuid == botGuid;

    if (!flags.teamCarrierGuid.IsEmpty() && flags.teamCarrierGuid == botGuid)
    {
        orders.role = CtfRole::Carrier;
        orders.carrierHolding = !orders.ownFlagAtBase && bot->GetExactDist(flags.ownFlagStand) <= kCarrierHoldRange;
        if (plan.handoffPhase == HandoffPhase::Converging && plan.handoffGiver == botGuid)
        {
            orders.handoffGive = true;
            orders.handoffPartnerGuid = plan.handoffReceiver;
        }
        return true;
    }

    if (orders.isDesignatedRunner)
    {
        orders.role = CtfRole::Runner;
        if (plan.handoffPhase != HandoffPhase::None && plan.handoffReceiver == botGuid)
        {
            orders.handoffReceive = true;
            orders.handoffPartnerGuid = plan.handoffGiver;
        }
    }
    else
    {
        auto const roleItr = plan.roles.find(botGuid);
        orders.role = roleItr != plan.roles.end() ? roleItr->second : CtfRole::Midfield;
    }

    ResolvePickup(battleground, map, flags, plan, bot, orders);
    return true;
}

bool CtfCoordinator::IsHandoffReceiverFor(Player const* bot, ObjectGuid const& flagGuid)
{
    TeamPlan const* plan = FindTeamPlan(bot);
    return plan && plan->handoffPhase == HandoffPhase::Dropped && plan->handoffReceiver == bot->GetGUID() &&
        !flagGuid.IsEmpty() && flagGuid == plan->handoffFlagGuid;
}

bool CtfCoordinator::DropFlagForHandoff(Player* giver)
{
    if (!giver || !giver->IsInWorld() || !giver->InBattleground())
        return false;

    Battleground* battleground = giver->GetBattleground();
    if (!IsTwoFlagCtf(battleground) || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    Map* map = giver->FindMap();
    TeamId const team = ResolveTeam(battleground, giver);
    FlagSnapshot flags;
    if (!map || team == TEAM_NEUTRAL || !BuildFlagSnapshot(battleground, team, flags))
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    TeamPlan& plan = TouchTeamPlan(BuildPlanKey(battleground, team), nowMs);
    if (plan.handoffPhase != HandoffPhase::Converging || plan.handoffGiver != giver->GetGUID() ||
        flags.teamCarrierGuid != giver->GetGUID())
        return false;

    // Everything is checked again at the moment of the drop: the plan was
    // last validated up to a rebuild ago.
    if (CanCarrierScoreSoon(flags, giver))
    {
        EndHandoff(plan, nowMs, kHandoffDoneCooldownMs, "carrier-can-score");
        return false;
    }

    Player* receiver = ObjectAccessor::GetPlayer(map, plan.handoffReceiver);
    if (!receiver || plan.handoffReceiver != plan.runnerGuid ||
        !giver->IsWithinDistInMap(receiver, kHandoffDropRange) ||
        !IsHandoffStillSafe(battleground, map, flags, giver, receiver))
        return false;

    battleground->EventPlayerDroppedFlag(giver);

    FlagSnapshot after;
    BuildFlagSnapshot(battleground, team, after);
    if (after.teamCarrierGuid == giver->GetGUID())
    {
        EndHandoff(plan, nowMs, kHandoffAbortCooldownMs, "drop-refused");
        return false;
    }

    plan.handoffPhase = HandoffPhase::Dropped;
    plan.handoffFlagGuid = after.enemyFlagState == kFlagOnGround ? after.enemyDroppedFlagGuid : ObjectGuid::Empty;
    plan.handoffPhaseStartMs = nowMs;

    TC_LOG_INFO("playerbots.pvp.ctf", "CTF handoff drop: bg={} giver={} ({}) receiver={} ({}) flag={}.",
        battleground->GetInstanceID(), giver->GetName(), giver->GetGUID().ToString(),
        receiver->GetName(), receiver->GetGUID().ToString(), plan.handoffFlagGuid.ToString());
    return true;
}

std::vector<std::string> CtfCoordinator::DescribeTeams(Player const* observer)
{
    std::vector<std::string> lines;
    Battleground* battleground = observer ? observer->GetBattleground() : nullptr;
    if (!IsTwoFlagCtf(battleground))
    {
        lines.emplace_back("Not in Warsong Gulch or Twin Peaks.");
        return lines;
    }

    if (battleground->GetStatus() != STATUS_IN_PROGRESS)
    {
        lines.emplace_back("The match is not in progress.");
        return lines;
    }

    Map* map = observer->FindMap();
    if (!map)
        return lines;

    auto describePlayer = [map](ObjectGuid const& guid) -> std::string
    {
        if (guid.IsEmpty())
            return "-";

        Player const* player = ObjectAccessor::GetPlayer(map, guid);
        if (!player)
            return guid.ToString();

        std::string name = Battleground::GetPlayerDisplayName(player);
        if (!player->IsAlive())
            name += "(dead)";
        return name;
    };

    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        FlagSnapshot flags;
        if (!BuildFlagSnapshot(battleground, team, flags))
            continue;

        std::ostringstream line;
        line << (team == TEAM_ALLIANCE ? "Alliance" : "Horde")
             << ": our flag " << GetFlagStateName(flags.ownFlagState)
             << ", their flag " << GetFlagStateName(flags.enemyFlagState)
             << ", our carrier " << describePlayer(flags.teamCarrierGuid)
             << ", their carrier " << describePlayer(flags.enemyCarrierGuid);

        TeamPlan const* plan = LockedFind(g_CtfTeamPlans, BuildPlanKey(battleground, team));
        if (!plan)
        {
            line << ", no plan yet (no bot on this side has decided anything)";
            lines.push_back(line.str());
            continue;
        }

        Player const* runner = plan->runnerGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(map, plan->runnerGuid);
        line << " | runner " << describePlayer(plan->runnerGuid);
        if (runner)
            line << " (tier " << uint32(PvpCore::GetFlagRunnerPriority(runner)) << ")";

        line << " | handoff " << GetHandoffPhaseName(plan->handoffPhase);
        if (plan->handoffPhase != HandoffPhase::None)
            line << " " << describePlayer(plan->handoffGiver) << " -> " << describePlayer(plan->handoffReceiver);

        std::ostringstream escorts;
        std::ostringstream defenders;
        uint32 midfield = 0;
        for (auto const& roleEntry : plan->roles)
        {
            if (roleEntry.second == CtfRole::Escort)
                escorts << (escorts.tellp() > 0 ? ", " : "") << describePlayer(roleEntry.first);
            else if (roleEntry.second == CtfRole::Defender)
                defenders << (defenders.tellp() > 0 ? ", " : "") << describePlayer(roleEntry.first);
            else if (roleEntry.second == CtfRole::Midfield)
                ++midfield;
        }

        line << " | escorts [" << escorts.str() << "] defenders [" << defenders.str() << "] midfield " << midfield;
        if (!plan->returnDuty.empty())
        {
            line << " | returning our flag:";
            for (ObjectGuid const& guid : plan->returnDuty)
                line << " " << describePlayer(guid);
        }

        lines.push_back(line.str());
    }

    return lines;
}
}
