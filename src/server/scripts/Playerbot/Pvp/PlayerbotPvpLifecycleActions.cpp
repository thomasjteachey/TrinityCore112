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
#include "BattlegroundTP.h"
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
#include "Configuration/Config.h"
#include "StringConvert.h"

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
    std::unordered_map<uint64, uint32> g_HunterAutoShotPlantStartedMs;
    std::unordered_map<uint64, uint32> g_HunterAutoShotPlantLastTimerMs;
    std::unordered_map<uint64, uint32> g_HunterForceFleeUntilMs;
    std::unordered_map<uint64, uint32> g_HunterLastFleeIssueMs;
    std::unordered_map<uint64, uint32> g_HunterKiteHoldUntilMs;
    std::unordered_map<uint64, int8> g_HunterKiteSideByGuid;

    struct HunterFleeState
    {
        Position destination;
        ObjectGuid targetGuid = ObjectGuid::Empty;
        uint32 untilMs = 0;
    };

    std::unordered_map<uint64, HunterFleeState> g_HunterFleeStateByGuid;
    constexpr uint32 PLAYERBOT_HUNTER_KITE_HOLD_MS = 3500;
    constexpr uint32 PLAYERBOT_HUNTER_FLEE_STICK_MS = 4200;
    constexpr uint32 PLAYERBOT_HUNTER_STUTTER_MIN_PLANT_MS = 434;
    constexpr uint32 PLAYERBOT_HUNTER_STUTTER_MAX_PLANT_MS = 1500;
    constexpr uint32 PLAYERBOT_HUNTER_POST_PLANT_FORCE_FLEE_MS = 1150;
    constexpr uint32 PLAYERBOT_HUNTER_STUTTER_PLANT_LEAD_MS = 550;
    constexpr uint32 PLAYERBOT_HUNTER_STUTTER_FIRED_TIMER_MS = 900;
    constexpr uint32 PLAYERBOT_HUNTER_FLEE_REISSUE_MS = 350;

    bool IsHunterKiteHoldActive(Player const* player, uint32 nowMs = GameTime::GetGameTimeMS())
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return false;

        auto itr = g_HunterKiteHoldUntilMs.find(player->GetGUID().GetRawValue());
        if (itr == g_HunterKiteHoldUntilMs.end())
            return false;

        if (itr->second <= nowMs)
        {
            g_HunterKiteHoldUntilMs.erase(itr);
            return false;
        }

        return true;
    }

    void MarkHunterKiteHold(Player const* player, uint32 holdMs = PLAYERBOT_HUNTER_KITE_HOLD_MS)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint64 const guid = player->GetGUID().GetRawValue();
        uint32 const untilMs = nowMs + holdMs;
        uint32& existingUntilMs = g_HunterKiteHoldUntilMs[guid];
        if (existingUntilMs < untilMs)
            existingUntilMs = untilMs;
    }

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
    uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType, bool respectQueueOnlySegments);

    bool IsScmManagedBotCandidate(Player const* player)
    {
        if (!player)
            return false;

        if (playerbot::IsManagedRandomBot(player))
            return true;

        WorldSession const* session = player->GetSession();
        return session && session->IsVirtualSession();
    }

    std::unordered_set<uint32> ParseAccountIdSetFromConfig(char const* key)
    {
        std::unordered_set<uint32> accountIds;
        std::string const raw = sConfigMgr->GetStringDefault(key, "");
        for (std::string_view token : Trinity::Tokenize(raw, ',', false))
        {
            if (Optional<uint32> accountId = Trinity::StringTo<uint32>(token))
                if (*accountId > 0)
                    accountIds.insert(*accountId);
        }

        return accountIds;
    }

    BattlegroundTypeId ResolveManagedBotQueueTargetForAccount(Player const* player)
    {
        if (!player)
            return BATTLEGROUND_SCM;

        uint32 const accountId = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
        if (!accountId)
            return BATTLEGROUND_SCM;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.ScarletChapelAccounts").count(accountId))
            return BATTLEGROUND_SCM;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.BlackrockThroneAccounts").count(accountId))
            return BATTLEGROUND_BRT;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.BattleForGilneasAccounts").count(accountId))
            return BATTLEGROUND_BFG;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.TwinPeaksAccounts").count(accountId))
            return BATTLEGROUND_TP;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.ArathiBasinAccounts").count(accountId))
            return BATTLEGROUND_AB;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.WarsongGulchAccounts").count(accountId))
            return BATTLEGROUND_WS;

        return BATTLEGROUND_SCM;
    }

    bool IsArenaOnlyManagedBotAccount(Player const* player)
    {
        if (!player || !player->GetSession())
            return false;

        uint32 const accountId = player->GetSession()->GetAccountId();
        if (!accountId)
            return false;

        return ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.ArenaAccounts").count(accountId) != 0;
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
        uint32 const queuedCount = ::QueueEligibleManagedBotsForBattleground(BATTLEGROUND_SCM, 0, true);
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

    bool IsScarletChapel(Player const* player)
    {
        Battleground const* battleground = player ? player->GetBattleground() : nullptr;
        return battleground && battleground->GetTypeID(true) == BATTLEGROUND_SCM;
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
    bool TryGetObjectivePosition(Battleground* battleground, Player* player, Position& destination);
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

    bool MoveToBattlegroundObjectivePosition(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        if (!battleground)
            return false;

        Position destination;
        if (!TryGetObjectivePosition(battleground, player, destination))
            return false;

        if (player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
        {
            EmitBattlegroundGmDebug(player, "objective-skip reason=already-near-objective range=12", 1000);
            return true;
        }

        return IssueMovePointThrottled(player, destination);
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

        // Prefer a physically grounded/swimming destination over liquid surface
        // hovering when the location is in water and the bot is not water-walking.
        if (Map const* map = player->FindMap())
        {
            LiquidData liquidData{};
            if (map->GetLiquidStatus(player->GetPhaseMask(), adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(),
                adjustedZ + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight()))
            {
                bool const canWalkOnWater = player->HasAuraType(SPELL_AURA_WATER_WALK);
                if (!canWalkOnWater)
                    adjustedZ = std::max(liquidData.depth_level + 0.05f, std::min(adjustedZ, liquidData.level - 0.25f));
            }
        }

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

    Position BuildDownhillEscapeDestination(Player* player, Position const& destination)
    {
        if (!player)
            return destination;

        float const dx = destination.GetPositionX() - player->GetPositionX();
        float const dy = destination.GetPositionY() - player->GetPositionY();
        float const planarDistance = std::sqrt(dx * dx + dy * dy);
        if (planarDistance < 0.5f)
            return BuildCollisionSafeDestination(player, destination);

        float const stepDistance = std::min(12.0f, std::max(4.0f, planarDistance * 0.35f));
        float const nx = dx / planarDistance;
        float const ny = dy / planarDistance;

        Position probe(
            player->GetPositionX() + nx * stepDistance,
            player->GetPositionY() + ny * stepDistance,
            player->GetPositionZ() - 6.0f,
            destination.GetOrientation());

        return BuildCollisionSafeDestination(player, probe);
    }

    bool ShouldPreferDirectDropShortcut(Player* player, Position const& destination)
    {
        if (!player)
            return false;

        float const destinationDistance = player->GetDistance(destination);
        if (destinationDistance < 15.0f || destinationDistance > 120.0f)
            return false;

        float const verticalDrop = player->GetPositionZ() - destination.GetPositionZ();
        if (verticalDrop < 8.0f)
            return false;

        PathGenerator path(player);
        path.SetPathLengthLimit(PLAYERBOT_BG_PATH_CALCULATION_LENGTH_LIMIT);
        if (!path.CalculatePath(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), true))
            return false;

        if (IsForbiddenBattlegroundPathType(path.GetPathType()))
            return false;

        Movement::PointsArray const& points = path.GetPath();
        if (points.size() < 2)
            return false;

        float pathLength = 0.0f;
        for (std::size_t i = 1; i < points.size(); ++i)
            pathLength += (points[i] - points[i - 1]).length();

        return pathLength > destinationDistance * 1.35f;
    }

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

    bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold, uint32 minReissueMs)
    {
        if (!player)
            return false;

        if (!CanIssueMovementCommand(player, 500))
            return false;

        ClearEatDrinkAurasForMovement(player);

        minReissueMs = std::max<uint32>(minReissueMs, 500);

        struct MoveOrderState
        {
            Position lastDestination;
            uint32 lastIssueMs = 0;
        };
        struct DirectDropState
        {
            Position startPosition;
            uint32 issueMs = 0;
            uint32 suppressUntilMs = 0;
            bool pending = false;
        };

        static std::unordered_map<uint64, MoveOrderState> stateByGuid;
        static std::unordered_map<uint64, uint8> stationaryReissueCountByGuid;
        static std::unordered_map<uint64, DirectDropState> directDropStateByGuid;
        uint64 const botGuid = player->GetGUID().GetRawValue();
        MoveOrderState& state = stateByGuid[player->GetGUID().GetRawValue()];
        uint8& stationaryReissueCount = stationaryReissueCountByGuid[botGuid];
        DirectDropState& directDropState = directDropStateByGuid[botGuid];
        uint32 const nowMs = GameTime::GetGameTimeMS();

        bool const destinationChanged = state.lastIssueMs == 0 ||
            state.lastDestination.GetExactDist(destination) >= destinationChangeThreshold;
        bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + minReissueMs;
        bool const botCurrentlyMoving = player->isMoving();
        bool const forcedStationaryReissue = !destinationChanged && !canReissueByTime && !botCurrentlyMoving;
        if (forcedStationaryReissue)
            stationaryReissueCount = std::min<uint8>(uint8(stationaryReissueCount + 1), 20);
        else
            stationaryReissueCount = 0;

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
            // Clear stale effect movement only once charge state has ended.
            (currentMovement != EFFECT_MOTION_TYPE || !player->HasUnitState(UNIT_STATE_CHARGING)))
        {
            EmitBattlegroundGmDebug(player,
                "movepoint=clear-stale-generator motionType=" + std::to_string(uint32(currentMovement)), 1000);
            motionMaster->Clear();
        }

        bool const generatePath = !player->IsFlying() && !player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        Position const safeDestination = generatePath ? BuildCollisionSafeDestination(player, destination) : destination;

        if (generatePath && player->InBattleground())
        {
            bool const allowDirectDrop = !IsScarletChapel(player);
            if (!allowDirectDrop)
                directDropState.pending = false;

            if (allowDirectDrop && directDropState.pending &&
                nowMs > directDropState.issueMs + 1200 &&
                !player->isMoving())
            {
                // Two failure shapes:
                // 1) Never launched from the start point.
                // 2) Launched, then settled/stuck on terrain partway down.
                float const fromStart = player->GetDistance(directDropState.startPosition);
                float const toDestination = player->GetDistance(safeDestination);
                if (fromStart < 3.0f || toDestination > 8.0f)
                {
                    directDropState.pending = false;
                    directDropState.suppressUntilMs = nowMs + 8000;
                    Position const escapeDestination = BuildDownhillEscapeDestination(player, safeDestination);
                    motionMaster->MovePoint(0, escapeDestination, false);
                    EmitBattlegroundGmDebug(player,
                        "movepoint=direct-drop-stalled fallback=nav-segment fromStart=" + std::to_string(int32(fromStart)) +
                        " toDest=" + std::to_string(int32(toDestination)) +
                        " escapeDist=" + std::to_string(int32(player->GetDistance(escapeDestination))), 0);

                    state.lastDestination = destination;
                    state.lastIssueMs = nowMs;
                    return true;
                }
            }

            if (allowDirectDrop && nowMs >= directDropState.suppressUntilMs && ShouldPreferDirectDropShortcut(player, safeDestination))
            {
                Position const shortcutDestination = BuildDownhillEscapeDestination(player, safeDestination);
                motionMaster->MovePoint(0, shortcutDestination, false);
                EmitBattlegroundGmDebug(player,
                    "movepoint=direct-drop-shortcut destDist=" + std::to_string(int32(player->GetDistance(safeDestination))) +
                    " stepDist=" + std::to_string(int32(player->GetDistance(shortcutDestination))), 0);
                directDropState.startPosition = player->GetPosition();
                directDropState.issueMs = nowMs;
                directDropState.pending = true;

                state.lastDestination = destination;
                state.lastIssueMs = nowMs;
                return true;
            }
            else
            {
                directDropState.pending = false;
            }

            Position segmentDestination;
            PathType pathType = PathType(0);
            if (!TryBuildBattlegroundSegmentDestination(player, safeDestination, segmentDestination, &pathType))
            {
                if (!allowDirectDrop)
                {
                    motionMaster->MovePoint(0, safeDestination, true);
                    EmitBattlegroundGmDebug(player,
                        "movepoint=blocked-no-nav fallback=full-path directDrop=disabled-scarlet-chapel destDist=" +
                        std::to_string(int32(player->GetDistance(safeDestination))), 0);

                    state.lastDestination = destination;
                    state.lastIssueMs = nowMs;
                    return true;
                }

                // Recovery path for segmented-nav failures (for example, after a
                // partial drop where local nav probing can't find a legal segment):
                // issue a direct movement order so the bot keeps progressing
                // instead of stalling in place waiting on nav segment recovery.
                Position const fallbackDestination = BuildDownhillEscapeDestination(player, safeDestination);
                motionMaster->MovePoint(0, fallbackDestination, false);
                EmitBattlegroundGmDebug(player,
                    "movepoint=blocked-no-nav fallback=direct destDist=" + std::to_string(int32(player->GetDistance(safeDestination))) +
                    " stepDist=" + std::to_string(int32(player->GetDistance(fallbackDestination))), 0);

                directDropState.startPosition = player->GetPosition();
                directDropState.issueMs = nowMs;
                directDropState.pending = true;
                directDropState.suppressUntilMs = std::max(directDropState.suppressUntilMs, nowMs + 2500);

                state.lastDestination = destination;
                state.lastIssueMs = nowMs;
                return true;
            }

            motionMaster->MovePoint(0, segmentDestination, true);
            EmitBattlegroundGmDebug(player,
                "movepoint=nav-segment pathType=" + std::to_string(uint32(pathType)) +
                " segDist=" + std::to_string(int32(player->GetDistance(segmentDestination))), 0);
        }
        else
        {
            motionMaster->MovePoint(0, safeDestination, generatePath);
        }

        // Throttle against the caller's tactical destination, not the intermediate
        // navmesh segment we just issued. Battleground segment walking intentionally
        // advances in chunks; storing the chunk endpoint here made the next tick see
        // the real objective as a different destination, clear the active spline,
        // and reinstall a fresh segment before bots could make visible progress.
        state.lastDestination = destination;
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

    bool HasActiveStationaryChannel(Player const* player)
    {
        if (!player)
            return false;

        Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!channel || channel->getState() == SPELL_STATE_FINISHED)
            return false;

        SpellInfo const* spellInfo = channel->GetSpellInfo();
        return spellInfo && spellInfo->IsChanneled() && !spellInfo->IsMoveAllowedChannel();
    }

    bool CanIssueBotMovement(Player* player)
    {
        if (!player || !player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            return false;

        if (HasActiveStationaryChannel(player))
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

    bool HasHunterKiteControl(Unit const* target)
    {
        if (!target)
            return false;

        return target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
            target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN));
    }

    void StopVirtualPlayerbotMovement(Player* player)
    {
        if (!player)
            return;

        player->StopMoving();
        if (WorldSession* session = player->GetSession(); session && session->IsVirtualSession())
        {
            player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            player->SendMovementFlagUpdate();
        }
    }

    bool GetHunterAutoShotRange(Player const* player, Unit const* target, float& exactDistance, float& minAutoShotRange, float& maxAutoShotRange)
    {
        exactDistance = 0.0f;
        minAutoShotRange = 0.0f;
        maxAutoShotRange = 0.0f;

        if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive() || !player->HasSpell(75))
            return false;

        SpellInfo const* autoShotInfo = sSpellMgr->GetSpellInfo(75);
        if (!autoShotInfo)
            return false;

        exactDistance = player->GetExactDist(target);
        minAutoShotRange = autoShotInfo->GetMinRange(false);
        maxAutoShotRange = autoShotInfo->GetMaxRange(false);
        return maxAutoShotRange > 0.0f;
    }

    bool IsHunterAutoShotBand(Player const* player, Unit const* target)
    {
        float exactDistance = 0.0f;
        float minAutoShotRange = 0.0f;
        float maxAutoShotRange = 0.0f;
        if (!GetHunterAutoShotRange(player, target, exactDistance, minAutoShotRange, maxAutoShotRange))
            return false;

        return player->IsWithinLOSInMap(target) &&
            exactDistance > std::max(minAutoShotRange + 0.75f, 8.75f) &&
            exactDistance <= maxAutoShotRange;
    }

    bool StopHunterAndStartAutoShot(Player* player, Unit* target, char const* logReason)
    {
        if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive() || !IsHunterAutoShotBand(player, target))
            return false;

        StopVirtualPlayerbotMovement(player);
        if (MotionMaster* motionMaster = player->GetMotionMaster())
            if (motionMaster->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE ||
                motionMaster->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                motionMaster->Clear(MOTION_SLOT_ACTIVE);

        player->SetFacingToObject(target);
        player->SetInFront(target);
        StopVirtualPlayerbotMovement(player);

        Spell const* autoRepeatSpell = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        bool const autoShotActive = autoRepeatSpell && autoRepeatSpell->GetSpellInfo() && autoRepeatSpell->GetSpellInfo()->Id == 75;
        if (!autoShotActive)
            player->CastSpell(target, 75, false);

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP hunter movement: bot={} target={} decision={} distance={} exact={} autoActive={}",
            player->GetGUID().ToString(), target->GetGUID().ToString(), logReason ? logReason : "stop-and-autoshot",
            player->GetDistance(target), player->GetExactDist(target), autoShotActive ? 1 : 0);
        return true;
    }

    constexpr uint32 kPlayerbotWandShootSpellId = 5019;
    std::unordered_map<uint64, uint32> g_WandLifecycleDiagNextMs;

    bool HasActiveWandAutoRepeat(Player const* player)
    {
        if (!player)
            return false;

        Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        if (!autoRepeat || !autoRepeat->GetSpellInfo())
            return false;

        SpellInfo const* firstRank = autoRepeat->GetSpellInfo()->GetFirstRankSpell();
        uint32 const firstRankSpellId = firstRank ? firstRank->Id : autoRepeat->GetSpellInfo()->Id;
        return firstRankSpellId == kPlayerbotWandShootSpellId;
    }

    bool IsInWandShootRange(Player const* player, Unit const* target)
    {
        if (!player || !target)
            return false;

        SpellInfo const* wandInfo = sSpellMgr->GetSpellInfo(kPlayerbotWandShootSpellId);
        if (!wandInfo)
            return false;

        float const distance = player->GetDistance(target);
        float const minRange = wandInfo->GetMinRange(false);
        float const maxRange = wandInfo->GetMaxRange(false);
        return distance >= minRange && (maxRange <= 0.0f || distance <= maxRange) && player->IsWithinLOSInMap(target);
    }

    void NotifyWandLifecycleDiagnostic(Player*, Unit*, char const*, bool, bool)
    {
        // Intentionally silent. This hook was used for temporary wand movement
        // troubleshooting whispers; keep HoldPositionForWand behavior without
        // player-visible diagnostics.
    }

    bool HoldPositionForWand(Player* player, Unit* target, char const* reason)
    {
        if (!player || !target || !target->IsAlive())
            return false;

        bool const activeWand = HasActiveWandAutoRepeat(player);
        bool const recentWandStart = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotWandShootSpellId);
        if (!activeWand && !recentWandStart)
            return false;

        if (!IsInWandShootRange(player, target))
            return false;

        bool const wasMoving = player->isMoving() || player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE) || (player->GetUnitMovementFlags() & MOVEMENTFLAG_MASK_MOVING);
        player->StopMoving();
        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear(MOTION_SLOT_ACTIVE);

        if (WorldSession* session = player->GetSession(); session && session->IsVirtualSession())
        {
            player->ClearUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE);
            player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            player->SendMovementFlagUpdate();
        }

        if (wasMoving || activeWand || recentWandStart)
            NotifyWandLifecycleDiagnostic(player, target, reason, activeWand, recentWandStart);

        return true;
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


    bool HunterHasActiveAutoShot(Player const* player)
    {
        if (!player)
            return false;

        Spell const* autoRepeatSpell = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        return autoRepeatSpell && autoRepeatSpell->GetSpellInfo() && autoRepeatSpell->GetSpellInfo()->Id == 75;
    }


    void StopHunterAutoShotForBreakableCrowdControl(Player* player, Unit* target, char const* reason)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        if (HunterHasActiveAutoShot(player))
            player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

        if (target && player->GetVictim() && player->GetVictim()->GetGUID() == target->GetGUID())
            player->AttackStop();

        if (Pet* pet = player->GetPet())
            if (target && pet->GetVictim() && pet->GetVictim()->GetGUID() == target->GetGUID())
                pet->AttackStop();

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP hunter suppress damage on breakable CC: bot={} target={} reason={}",
            player->GetGUID().ToString(), target ? target->GetGUID().ToString() : ObjectGuid::Empty.ToString(), reason ? reason : "breakable-cc");
    }

    bool HunterIsHardCastingStationaryShot(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return false;

        Spell const* current = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!current || current->getState() == SPELL_STATE_FINISHED)
            return false;

        SpellInfo const* spellInfo = current->GetSpellInfo();
        return spellInfo && spellInfo->CalcCastTime() > 0;
    }

    bool IssueHunterStutterFlee(Player* player, Unit* target, float desiredExactDistance, char const* reason)
    {
        if (!player || player->GetClass() != CLASS_HUNTER || !target || !CanIssueBotMovement(player))
            return false;

        uint64 const guid = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();

        // Do not let the generic movement throttle turn a flee phase into a
        // stationary turret phase. If the hunter is already moving, preserving
        // that movement is success; if he is stopped, allow a forced flee issue
        // even if a recent stop/face/autoshot command just ran.
        bool const alreadyMoving = player->isMoving() || player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE);
        uint32& lastFleeIssueMs = g_HunterLastFleeIssueMs[guid];
        if (alreadyMoving && nowMs < lastFleeIssueMs + PLAYERBOT_HUNTER_FLEE_REISSUE_MS)
            return true;

        int8& side = g_HunterKiteSideByGuid[guid];
        if (side == 0)
            side = (guid & 1) ? int8(1) : int8(-1);

        float const currentDistance = player->GetExactDist(target);
        float const angleAway = target->GetAbsoluteAngle(player);
        float const neededDistance = desiredExactDistance > currentDistance ? desiredExactDistance - currentDistance : 0.0f;
        float const moveDistance = std::clamp(neededDistance + 4.0f, 8.0f, 16.0f);

        // Bias only slightly, and keep the side stable, so the hunter travels
        // generally away from the attacker instead of re-picking triangle legs.
        float const edgeBias = 0.18f * float(side);
        float const fleeAngle = angleAway + edgeBias;

        Position destination(player->GetPositionX() + std::cos(fleeAngle) * moveDistance,
            player->GetPositionY() + std::sin(fleeAngle) * moveDistance,
            player->GetPositionZ(), player->GetOrientation());

        // Hunter stutter movement needs to be able to resume immediately after
        // StopMoving()/face/autoshot. The generic movement throttle is shared
        // with stop/follow helpers and can otherwise leave the hunter standing
        // still for the whole post-shot window. Issue this flee segment directly
        // and keep our own lightweight reissue throttle above.
        ClearEatDrinkAurasForMovement(player);
        Position const safeDestination = BuildCollisionSafeDestination(player, destination);
        bool issued = false;
        char const* movementMode = "direct";
        PathType pathType = PathType(0);
        if (MotionMaster* motionMaster = player->GetMotionMaster())
        {
            MovementGeneratorType const currentMovement = motionMaster->GetCurrentMovementGeneratorType();
            if (currentMovement == FOLLOW_MOTION_TYPE || currentMovement == DISTRACT_MOTION_TYPE)
                motionMaster->Clear();

            bool const shouldUsePath = !player->IsFlying() && !player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
            Position pathDestination;
            if (shouldUsePath && player->InBattleground() && TryBuildBattlegroundSegmentDestination(player, safeDestination, pathDestination, &pathType))
            {
                // Hunters were visually clipping through Blackrock Throne WMO
                // walls because the stutter-flee loop used a raw direct spline.
                // Always prefer a real mmap segment in battlegrounds; if mmaps
                // cannot build a non-shortcut path, do not fall back to direct
                // wall-crossing movement.
                motionMaster->MovePoint(0, pathDestination, true);
                movementMode = "mmap-segment";
                issued = true;
            }
            else if (!player->InBattleground())
            {
                // Outside battlegrounds keep the old lightweight behavior, but
                // still request path generation when possible so we do not draw
                // straight splines through nearby terrain.
                motionMaster->MovePoint(0, safeDestination, shouldUsePath);
                movementMode = shouldUsePath ? "path" : "direct";
                issued = true;
            }
            else if (alreadyMoving)
            {
                // Existing movement is safer than issuing a direct no-path flee
                // through WMO geometry. Let the current path continue and retry
                // on the next flee tick.
                movementMode = "preserve-existing";
                issued = true;
            }
            else
            {
                movementMode = "blocked-no-mmap";
            }

            if (issued)
                lastFleeIssueMs = nowMs;
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP hunter stutter loop: bot={} target={} decision=flee reason={} current={} desired={} move={} timer={} issued={} moving={} mode={} pathType={}",
            player->GetGUID().ToString(), target->GetGUID().ToString(), reason ? reason : "kite",
            currentDistance, desiredExactDistance, moveDistance, player->getAttackTimer(RANGED_ATTACK), issued ? 1 : 0, alreadyMoving ? 1 : 0,
            movementMode, uint32(pathType));
        return issued || alreadyMoving || player->isMoving();
    }

    bool DriveHunterKiteLoop(Player* player, Unit* target, CombatPositioningProfile const& /*profile*/)
    {
        if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive())
            return false;

        float exactDistance = 0.0f;
        float minAutoShotRange = 0.0f;
        float maxAutoShotRange = 0.0f;
        if (!GetHunterAutoShotRange(player, target, exactDistance, minAutoShotRange, maxAutoShotRange))
            return false;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint64 const hunterGuidRaw = player->GetGUID().GetRawValue();
        uint32& plantUntilMs = g_HunterAutoShotPauseUntilMs[hunterGuidRaw];
        uint32& forceFleeUntilMs = g_HunterForceFleeUntilMs[hunterGuidRaw];

        auto clearPlantState = [&]()
        {
            plantUntilMs = 0;
            g_HunterAutoShotPlantStartedMs[hunterGuidRaw] = 0;
            g_HunterAutoShotPlantLastTimerMs[hunterGuidRaw] = 0;
        };
        float const safeShootMin = std::max(minAutoShotRange + 0.75f, 8.75f);
        bool const hasLos = player->IsWithinLOSInMap(target);
        bool const inAutoShotBand = hasLos && exactDistance > safeShootMin && exactDistance <= maxAutoShotRange;
        bool const tooClose = exactDistance <= safeShootMin;
        bool const tooFar = exactDistance > maxAutoShotRange + 1.0f;

        if (target->HasBreakableByDamageCrowdControlAura())
        {
            clearPlantState();
            forceFleeUntilMs = 0;
            StopHunterAutoShotForBreakableCrowdControl(player, target, "kite-loop-breakable-cc");
            return false;
        }

        auto stopFaceAndKeepAutoShot = [&]()
        {
            if (target->HasBreakableByDamageCrowdControlAura())
            {
                StopHunterAutoShotForBreakableCrowdControl(player, target, "plant-suppressed-breakable-cc");
                return;
            }

            StopVirtualPlayerbotMovement(player);
            player->SetFacingToObject(target);
            player->SetInFront(target);
            StopVirtualPlayerbotMovement(player);

            if (!HunterHasActiveAutoShot(player) && player->HasSpell(75))
                player->CastSpell(target, 75, false);
        };

        if (HunterIsHardCastingStationaryShot(player))
        {
            // Aimed Shot and any other cast-time hunter shot must not be
            // interrupted by the kite loop. Stop/facing is enough here; spell
            // execution owns the cast.
            StopVirtualPlayerbotMovement(player);
            player->SetFacingToObject(target);
            player->SetInFront(target);
            return true;
        }

        if (!hasLos)
        {
            clearPlantState();
            return TryRecoverLineOfSight(player, target, GetCombatPositioningProfile(player), "hunter-stutter-los");
        }

        if (tooClose)
        {
            clearPlantState();
            forceFleeUntilMs = std::max(forceFleeUntilMs, nowMs + PLAYERBOT_HUNTER_POST_PLANT_FORCE_FLEE_MS);
            MarkHunterKiteHold(player);
            return IssueHunterStutterFlee(player, target, std::max(safeShootMin + 6.0f, 15.0f), "too-close-or-deadzone");
        }

        if (plantUntilMs > nowMs)
        {
            stopFaceAndKeepAutoShot();

            uint32 const currentAutoShotTimerMs = player->getAttackTimer(RANGED_ATTACK);
            uint32& plantStartedMs = g_HunterAutoShotPlantStartedMs[hunterGuidRaw];
            uint32& lastPlantTimerMs = g_HunterAutoShotPlantLastTimerMs[hunterGuidRaw];
            uint32 const elapsedPlantMs = plantStartedMs ? nowMs - plantStartedMs : 0;

            // Do not resume movement merely because a fixed 500ms window elapsed.
            // In this core, Auto Shot actually fires inside Unit::_UpdateAutoRepeatSpell()
            // when RANGED_ATTACK becomes ready, then resetAttackTimer(RANGED_ATTACK)
            // bumps the timer back up to bow/gun speed.  Hold still until we observe
            // that reset/increase, unless deadzone/melee already forced us out above.
            bool const observedTimerReset = elapsedPlantMs >= PLAYERBOT_HUNTER_STUTTER_MIN_PLANT_MS &&
                ((currentAutoShotTimerMs >= PLAYERBOT_HUNTER_STUTTER_FIRED_TIMER_MS) ||
                    (lastPlantTimerMs != 0 && currentAutoShotTimerMs > lastPlantTimerMs + 250));

            if (observedTimerReset)
            {
                plantUntilMs = 0;
                plantStartedMs = 0;
                lastPlantTimerMs = 0;
                forceFleeUntilMs = std::max(forceFleeUntilMs, nowMs + PLAYERBOT_HUNTER_POST_PLANT_FORCE_FLEE_MS);
                float const desiredFleeDistance = std::max(safeShootMin + 7.0f, maxAutoShotRange - 2.0f);
                return IssueHunterStutterFlee(player, target, desiredFleeDistance, "autoshot-fired-resume-flee");
            }

            lastPlantTimerMs = currentAutoShotTimerMs;
            return true;
        }

        // If we somehow reached the max plant deadline without seeing the ranged
        // timer reset, resume fleeing anyway so the hunter never turns into a
        // permanent turret because of a stale timer read.
        if (plantUntilMs != 0 && plantUntilMs <= nowMs)
        {
            plantUntilMs = 0;
            g_HunterAutoShotPlantStartedMs[hunterGuidRaw] = 0;
            g_HunterAutoShotPlantLastTimerMs[hunterGuidRaw] = 0;
            forceFleeUntilMs = std::max(forceFleeUntilMs, nowMs + PLAYERBOT_HUNTER_POST_PLANT_FORCE_FLEE_MS);
        }

        // After every completed plant window, force a flee window before another
        // plant is allowed. Some core states report the ranged timer as 0/ready
        // for more than one lifecycle tick, which previously made the hunter
        // chain-plant forever and appear completely stuck.
        if (forceFleeUntilMs > nowMs && inAutoShotBand)
        {
            float const desiredFleeDistance = std::max(safeShootMin + 7.0f, maxAutoShotRange - 2.0f);
            return IssueHunterStutterFlee(player, target, desiredFleeDistance, "post-shot-force-flee");
        }

        if (forceFleeUntilMs <= nowMs)
            forceFleeUntilMs = 0;

        uint32 const autoShotTimerMs = player->getAttackTimer(RANGED_ATTACK);
        bool const autoShotActive = HunterHasActiveAutoShot(player);
        bool const shouldPlantForAutoShot = inAutoShotBand && (autoShotTimerMs == 0 || autoShotTimerMs <= PLAYERBOT_HUNTER_STUTTER_PLANT_LEAD_MS);

        if (inAutoShotBand && !autoShotActive && !shouldPlantForAutoShot && player->HasSpell(75))
        {
            // Auto Shot can stay queued as an auto-repeat spell while the hunter
            // is moving. Do not park early just because Auto Shot is inactive;
            // start the auto-repeat and keep fleeing until the real shot window.
            if (target->HasBreakableByDamageCrowdControlAura())
            {
                StopHunterAutoShotForBreakableCrowdControl(player, target, "activate-autoshot-suppressed-breakable-cc");
                return false;
            }
            player->SetFacingToObject(target);
            player->SetInFront(target);
            player->CastSpell(target, 75, false);
            float const desiredFleeDistance = std::max(safeShootMin + 7.0f, maxAutoShotRange - 2.0f);
            return IssueHunterStutterFlee(player, target, desiredFleeDistance, "activate-autoshot-while-fleeing");
        }

        if (shouldPlantForAutoShot)
        {
            plantUntilMs = nowMs + PLAYERBOT_HUNTER_STUTTER_MAX_PLANT_MS;
            g_HunterAutoShotPlantStartedMs[hunterGuidRaw] = nowMs;
            g_HunterAutoShotPlantLastTimerMs[hunterGuidRaw] = autoShotTimerMs;
            g_HunterFleeStateByGuid.erase(hunterGuidRaw);
            stopFaceAndKeepAutoShot();

            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP hunter stutter loop: bot={} target={} decision=plant-autoshot distance={} timer={} active={} maxPlantMs={}",
                player->GetGUID().ToString(), target->GetGUID().ToString(), exactDistance, autoShotTimerMs,
                autoShotActive ? 1 : 0, PLAYERBOT_HUNTER_STUTTER_MAX_PLANT_MS);
            return true;
        }

        // Default state while Auto Shot is charging: keep moving away. Do not
        // turret at the minimum ranged band, and do not run generic preferred
        // range/chase logic.
        if (inAutoShotBand)
        {
            float const desiredFleeDistance = std::max(safeShootMin + 7.0f, maxAutoShotRange - 2.0f);
            return IssueHunterStutterFlee(player, target, desiredFleeDistance, "timer-filling");
        }

        if (tooFar)
        {
            // Only close if the hunter has genuinely drifted outside Auto Shot
            // range. Close to the outer edge, never to preferred/ideal range.
            if (!CanIssueMovementCommand(player, 500))
                return true;

            float const closeToRange = std::max(1.0f, maxAutoShotRange - 1.0f);
            bool const issued = IssueHumanLikeFollow(player, target, closeToRange, 6.0f, 700);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP hunter stutter loop: bot={} target={} decision=close-to-autoshot-edge distance={} followRange={} issued={}",
                player->GetGUID().ToString(), target->GetGUID().ToString(), exactDistance, closeToRange, issued ? 1 : 0);
            return issued || player->isMoving();
        }

        // Safety fallback for odd range gaps: flee rather than park.
        return IssueHunterStutterFlee(player, target, std::max(safeShootMin + 6.0f, 15.0f), "range-gap");
    }

    Player* FindFlagCarrierForDirective(Player* player, playerbot::FlagCarrierDirective directive)
    {
        if (!player || directive == playerbot::FlagCarrierDirective::None || !player->InBattleground())
            return nullptr;

        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
            return nullptr;

        TeamId const botTeam = ResolveBotTeamId(player);
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

        if (BattlegroundTP* bgTp = dynamic_cast<BattlegroundTP*>(battleground))
        {
            ObjectGuid carrierGuid = ObjectGuid::Empty;
            if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier)
                carrierGuid = bgTp->GetFlagPickerGUID(botTeam);
            else if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier)
                carrierGuid = bgTp->GetFlagPickerGUID(enemyTeam);

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

    uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType, bool respectQueueOnlySegments)
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

            // Normal lifecycle/refill sweeps must honor account queue segmentation.
            // GM forcequeue intentionally passes respectQueueOnlySegments=false so
            // the command still does exactly what it says: force all eligible
            // managed bots into the caller's current/queued battleground.
            if (respectQueueOnlySegments)
            {
                if (IsArenaOnlyManagedBotAccount(managedBot))
                {
                    EmitLifecycleDiagnostic(managedBot, "queue-skip-arena-only-account",
                        "Skipped battleground mass queue because account is configured arena-only.");
                    continue;
                }

                BattlegroundTypeId const configuredTarget = ResolveManagedBotQueueTargetForAccount(managedBot);
                if (configuredTarget != bgTypeId)
                {
                    EmitLifecycleDiagnostic(managedBot, "queue-skip-segment-mismatch",
                        "Requested bgTypeId=" + std::to_string(uint32(bgTypeId)) +
                        " but account target bgTypeId=" + std::to_string(uint32(configuredTarget)));
                    continue;
                }
            }

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
        return ::QueueEligibleManagedBotsForBattleground(bgTypeId, arenaType, false);
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

        // Hunter movement is handled by DriveHunterKiteLoop below. Do not issue
        // an early generic flee here; re-issuing ad-hoc destinations every tick
        // was one cause of visible triangle/orbit movement.
        if (player->GetClass() == CLASS_HUNTER)
        {
            float const exactDistance = player->GetExactDist(target);
            bool const targetPressuringHunter = target->GetVictim() == player || player->IsWithinMeleeRange(target);
            if ((targetPressuringHunter && exactDistance < 12.0f) || (exactDistance > 5.0f && exactDistance < 8.0f))
                MarkHunterKiteHold(player);
        }

        // Class spell actions can issue target-relative chase/follow in the same
        // scheduler frame. Do not immediately override those orders from lifecycle
        // distance-band helpers (follow/stop), or bots can visibly inch/stop.
        // Hunter exception: stale ranged approach orders must not preserve an
        // inward chase after the hunter has escaped into Auto Shot range or is
        // still inside a kite/dead-zone escape window.
        if (playerbot::PvpClassActions::HasRecentTargetRelativeMovementOrder(player, nullptr, 1500))
        {
            if (player->GetClass() != CLASS_HUNTER)
                return true;

            float exactDistance = 0.0f;
            float minAutoShotRange = 0.0f;
            float maxAutoShotRange = 0.0f;
            bool const hasHunterRange = GetHunterAutoShotRange(player, target, exactDistance, minAutoShotRange, maxAutoShotRange);
            bool const safeAutoShotBand = hasHunterRange && player->IsWithinLOSInMap(target) &&
                exactDistance > std::max(minAutoShotRange + 0.75f, 8.75f) && exactDistance <= maxAutoShotRange;
            bool const shouldBreakRecentOrder = safeAutoShotBand || IsHunterKiteHoldActive(player) || exactDistance < 12.0f;
            if (!shouldBreakRecentOrder)
                return true;
        }

        float const distance = player->GetDistance(target);
        bool const hasLos = player->IsWithinLOSInMap(target);
        if (!hasLos)
            return TryRecoverLineOfSight(player, target, profile, "drive-combat-positioning");

        if (HoldPositionForWand(player, target, "combat_positioning_hold_for_wand"))
            return true;

        if (profile.primarilyRanged)
        {
            if (ShouldForceMeleeFallbackOnLowMana(player) && player->GetClass() != CLASS_HUNTER)
            {
                if (!CanIssueMovementCommand(player, 500))
                    return true;

                return MoveTowardUnit(player, target, std::max(1.0f, playerbot::PvpCore::GetConfig().meleeRange - 1.0f));
            }

            if (player->GetClass() == CLASS_HUNTER && DriveHunterKiteLoop(player, target, profile))
                return true;

            if (profile.createDistanceWhenCrowded && IsActivelyPressuringInMelee(target, player) && distance < profile.preferredIdealRange)
            {
                if (player->GetClass() == CLASS_HUNTER)
                    MarkHunterKiteHold(player);

                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP distance band: bot={} profile={} decision=create-distance-vs-melee-pressure distance={} min={} ideal={} max={}.",
                    player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                    profile.preferredMaxPressureRange);
                return MoveAwayFromUnit(player, target, player->GetClass() == CLASS_HUNTER ? std::min(profile.preferredIdealRange, 16.0f) : profile.preferredIdealRange);
            }

            if (distance < profile.preferredMinRange && profile.createDistanceWhenCrowded)
            {
                if (player->GetClass() == CLASS_HUNTER)
                    MarkHunterKiteHold(player);

                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP distance band: bot={} profile={} decision=create-distance distance={} min={} ideal={} max={}.",
                    player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                    profile.preferredMaxPressureRange);
                return MoveAwayFromUnit(player, target, player->GetClass() == CLASS_HUNTER ? std::min(profile.preferredIdealRange, 16.0f) : profile.preferredIdealRange);
            }

            if (distance > profile.preferredMaxPressureRange)
            {
                if (player->GetClass() == CLASS_HUNTER)
                {
                    float exactDistance = 0.0f;
                    float minAutoShotRange = 0.0f;
                    float maxAutoShotRange = 0.0f;
                    bool const hasHunterRange = GetHunterAutoShotRange(player, target, exactDistance, minAutoShotRange, maxAutoShotRange);

                    if (IsHunterAutoShotBand(player, target))
                        return StopHunterAndStartAutoShot(player, target, "hold-autoshot-over-preferred-max");

                    if (hasHunterRange && exactDistance <= maxAutoShotRange + 1.0f)
                    {
                        StopVirtualPlayerbotMovement(player);
                        if (hasLos)
                        {
                            player->SetFacingToObject(target);
                            player->SetInFront(target);
                        }
                        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                            "Playerbot PvP hunter range band: bot={} target={} decision=no-close-near-autoshot-max distance={} exact={} maxAuto={}.",
                            player->GetGUID().ToString(), target->GetGUID().ToString(), distance, exactDistance, maxAutoShotRange);
                        return true;
                    }

                    if (!CanIssueMovementCommand(player, 500))
                        return true;

                    float const closeToRange = hasHunterRange ? std::max(1.0f, maxAutoShotRange - 1.0f) : profile.preferredMaxPressureRange;
                    if (!IssueHumanLikeFollow(player, target, closeToRange, 6.0f, 500))
                        return true;
                    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                        "Playerbot PvP hunter range band: bot={} target={} decision=close-only-to-autoshot-max distance={} exact={} followRange={}.",
                        player->GetGUID().ToString(), target->GetGUID().ToString(), distance, exactDistance, closeToRange);
                    return true;
                }

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
            if (player->GetClass() == CLASS_HUNTER && IsHunterAutoShotBand(player, target))
                return StopHunterAndStartAutoShot(player, target, "hold-band-autoshot");

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

    bool EngageSelectedEnemyPlayer(Player* player, Unit* target, char const* reason)
    {
        if (!player || !player->IsAlive() || !target || !target->IsAlive() || !player->IsValidAttackTarget(target))
            return false;

        if (IsCrowdControlledForAction(player))
        {
            player->AttackStop();
            return false;
        }

        player->SetSelection(target->GetGUID());

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

            if (targetInBreakableCrowdControl)
            {
                StopHunterAutoShotForBreakableCrowdControl(player, target, "engage-breakable-cc");
                return DriveCombatPositioning(player, target, profile);
            }

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
            "Playerbot PvP positioning profile: bot={} target={} reason={} profile={} ranged={} createDistance={} meleeFallback={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), reason ? reason : "combat", profile.label,
            profile.primarilyRanged, profile.createDistanceWhenCrowded, profile.meleeFallbackAcceptable);

        return DriveCombatPositioning(player, target, profile);
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

        return EngageSelectedEnemyPlayer(player, target, "nearest-enemy");
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

        if (IsArenaOnlyManagedBotAccount(player))
            return false;

        BattlegroundTypeId const kManagedBattleground = ResolveManagedBotQueueTargetForAccount(player);
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

        uint32 const massQueued = ::QueueEligibleManagedBotsForBattleground(kManagedBattleground, 0, true);
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
            QueuePlayer(player, ResolveManagedBotQueueTargetForAccount(player), 0);
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
        // tactical actions (enemy pursuit / movement), not lifecycle.
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
        if (IsTacticalAction(context.actionName, "bg pursue enemy"))
            return PursueEnemyPrimitive(player);
        if (IsTacticalAction(context.actionName, "bg move to objective"))
            return MoveToObjectivePrimitive(player, context);
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
        {
            if (context.flagCarrierDirective == FlagCarrierDirective::AttackEnemyCarrier)
                if (Player* enemyCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::AttackEnemyCarrier))
                    if (EngageSelectedEnemyPlayer(player, enemyCarrier, "enemy-flag-carrier"))
                        return true;

            return EngageNearestEnemyPlayer(player, GetAggressiveCombatScanDistance(player, 100.0f));
        }

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
                        EmitBattlegroundGmDebug(player, "objective-skip reason=already-near-objective range=12", 1000);
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

    bool BattlegroundTacticalActions::PursueEnemyPrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        return TryPursueNearestEnemyInBattleground(player);
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

        return EngageSelectedEnemyPlayer(player, enemyCarrier, "enemy-flag-carrier");
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
