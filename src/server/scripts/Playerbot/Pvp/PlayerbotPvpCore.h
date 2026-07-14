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

#ifndef TRINITY_PLAYERBOT_PVP_CORE_H
#define TRINITY_PLAYERBOT_PVP_CORE_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

class Player;
class GameObject;

namespace playerbot
{
struct PvpCoreConfig
{
    bool moduleEnabled = false;
    bool pvpCoreEnabled = false;
    bool pvpTacticsEnabled = false;
    bool pvpLifecycleEnabled = false;
    bool pvpClassSpellsEnabled = false;
    float spellRange = 30.0f;
    float healRange = 40.0f;
    float meleeRange = 5.0f;
    float closeRange = 15.0f;
    float longRange = 35.0f;
};

enum class BattlegroundState : uint8
{
    None = 0,
    Queueing,
    WaitingToStart,
    Active
};

struct PvpValues
{
    BattlegroundState battlegroundState = BattlegroundState::None;
    BattlegroundTypeId battlegroundTypeId = BATTLEGROUND_TYPE_NONE;
    bool inBattleground = false;
    bool inBattlegroundQueue = false;
    bool hasBattlegroundQueue = false;
    bool hasArenaQueue = false;
    bool hasBattlegroundInvite = false;
    bool hasArenaInvite = false;
    bool hasArenaTeamInvite = false;
    bool playerHasFlag = false;
    bool flagPickupAvailable = false;
    bool nearbyEnemyActive = false;
    bool enemyFlagCarrierActive = false;
    bool enemyFlagCarrierNear = false;
    bool teamFlagCarrierNear = false;
    bool nodeAssaultAvailable = false;
    bool nodeDefenseAvailable = false;
    uint32 nodeObjectiveId = 0;
    uint32 battlegroundTeamHumanCount = 0;
    bool battlegroundTeamHasHumans = false;
};

enum class PvpTrigger : uint8
{
    InBattleground = 0,
    BgQueueing,
    BgWaiting,
    BgActive,
    BgInviteActive,
    InBattlegroundWithoutFlag,
    PlayerHasFlag,
    FlagPickupAvailable,
    EnemyFlagCarrierActive,
    EnemyFlagCarrierNear,
    TeamFlagCarrierNear,
    NodeAssaultAvailable,
    NodeDefenseAvailable
};

enum class BattlegroundObjectiveType : uint8
{
    None = 0,
    AssaultNode,
    DefendNode,
    CaptureFlag,
    AttackFlagCarrier,
    ProtectFlagCarrier
};

enum class BattlegroundMovementPrimitive : uint8
{
    None = 0,
    MoveToObjectivePosition,
    MoveToObjectiveUnit,
    FollowFlagCarrier
};

enum class FlagCarrierDirective : uint8
{
    None = 0,
    AttackEnemyCarrier,
    ProtectTeamCarrier
};

struct BattlegroundObjectiveSelection
{
    BattlegroundObjectiveType type = BattlegroundObjectiveType::None;
    uint32 objectiveId = 0;
};

struct BattlegroundTacticalContext
{
    bool tacticsEnabled = false;
    bool shouldEvaluate = false;
    char const* triggerName = nullptr;
    char const* actionName = nullptr;
    float actionPriority = 0.0f;
    BattlegroundObjectiveSelection objective;
    BattlegroundMovementPrimitive movement = BattlegroundMovementPrimitive::None;
    FlagCarrierDirective flagCarrierDirective = FlagCarrierDirective::None;
    bool nearbyEnemyActive = false;
};

enum class QueueOperationType : uint8
{
    None = 0,
    Join,
    Leave
};

enum class InvitationResponseType : uint8
{
    None = 0,
    Accept,
    Decline
};

enum class ArenaTeamInteractionType : uint8
{
    None = 0,
    AcceptInvite,
    DeclineInvite
};

struct PvpClassSpellContext
{
    enum class MovementDirective : uint8
    {
        None = 0,
        ReachMeleeRange,
        ReachSpellRange,
        FleeTooCloseForSpell,
        FaceSpellTarget,
        DropInvalidTarget,
        CheckMountState,
        ResetCombatState
    };

    bool classSpellsEnabled = false;
    bool shouldExecute = false;
    char const* actionName = nullptr;
    char const* reason = nullptr;
    uint32 spellId = 0;
    bool selfCast = false;
    enum class TargetMode : uint8
    {
        None = 0,
        Enemy,
        Self,
        Ally
    };
    TargetMode targetMode = TargetMode::None;
    ObjectGuid targetGuid = ObjectGuid::Empty;
    ObjectGuid allyTargetGuid = ObjectGuid::Empty;
    MovementDirective movementDirective = MovementDirective::None;
    ObjectGuid movementTargetGuid = ObjectGuid::Empty;
    float movementFollowRange = 0.0f;
    float movementPriority = 0.0f;
    bool preserveFlagObjectiveMovement = false;
    bool preserveFlagCarrierMovement = false;
    uint32 itemEntry = 0;
};

struct BattlegroundLifecycleContext
{
    bool lifecycleEnabled = false;
    QueueOperationType queueOperation = QueueOperationType::None;
    InvitationResponseType invitationResponse = InvitationResponseType::None;
    bool shouldHandleInProgressStatus = false;
};

struct ArenaLifecycleContext
{
    bool lifecycleEnabled = false;
    QueueOperationType queueOperation = QueueOperationType::None;
    ArenaTeamInteractionType teamInteraction = ArenaTeamInteractionType::None;
};

struct RandomBotParticipationHooks
{
    bool lifecycleEnabled = false;
    bool battlegroundParticipationHook = false;
    bool arenaParticipationHook = false;
};

class PvpCore
{
public:
    static void LoadConfig();
    static PvpCoreConfig const& GetConfig();

    static PvpValues CollectValues(Player const* player);
    static bool CanMageBlinkOutOfControl(Player const* player);
    static bool IsTriggerActive(PvpTrigger trigger, PvpValues const& values);
    static BattlegroundTacticalContext BuildBattlegroundTacticalContext(Player const* player, PvpValues const& values);
    static BattlegroundLifecycleContext BuildBattlegroundLifecycleContext(Player const* player, PvpValues const& values);
    static ArenaLifecycleContext BuildArenaLifecycleContext(Player const* player, PvpValues const& values);
    static PvpClassSpellContext BuildClassSpellContext(Player const* player, PvpValues const& values);
    static RandomBotParticipationHooks BuildRandomBotParticipationHooks(Player const* player, PvpValues const& values);
    static uint32 CountHumanPlayersOnBattlegroundTeam(Player const* player);
    static GameObject* FindUsableLightwell(Player const* player, float maxDistance);
    static bool ShouldSeekLightwell(Player const* player);
    static GameObject* FindUsableSoulwell(Player const* player, float maxDistance);
    static bool HasHealthstone(Player const* player);
    static bool IsBattlegroundFlagCarrier(Player const* player);
    static bool SpellWouldBreakFlagCarry(uint32 spellId);
    static bool TeamHasHumanPlayers(Player const* player);

private:
    static bool IsLifecycleEnabled();
    static bool IsInBattlegroundQueue(Player const* player);
    static BattlegroundState DetectBattlegroundState(Player const* player, bool inQueue);
    static BattlegroundObjectiveSelection SelectObjectiveSkeleton(PvpValues const& values);
    static BattlegroundMovementPrimitive SelectMovementPrimitiveSkeleton(PvpValues const& values,
        BattlegroundObjectiveSelection const& objective);
    static FlagCarrierDirective SelectFlagCarrierDirectiveSkeleton(PvpValues const& values);
    static QueueOperationType SelectBattlegroundQueueOperationSkeleton(PvpValues const& values);
    static InvitationResponseType SelectBattlegroundInvitationResponseSkeleton(PvpValues const& values);
    static bool ShouldHandleBattlegroundInProgressStatusSkeleton(PvpValues const& values);
    static QueueOperationType SelectArenaQueueOperationSkeleton(PvpValues const& values);
    static ArenaTeamInteractionType SelectArenaTeamInteractionSkeleton(PvpValues const& values);
};
}

#endif
