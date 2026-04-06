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

#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Opcodes.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cstring>
#include <sstream>

namespace
{
bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
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

bool QueuePlayer(Player* player, BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    if (!player || player->InBattleground())
        return false;

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

bool HasConflictingArenaLifecycleContext(playerbot::ArenaLifecycleContext const& context)
{
    return (context.queueOperation != playerbot::QueueOperationType::None) &&
        (context.teamInteraction != playerbot::ArenaTeamInteractionType::None);
}

bool IsTacticalAction(char const* actionName, char const* expected)
{
    return actionName && expected && std::strcmp(actionName, expected) == 0;
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
    if (!player || !target || !target->IsAlive() || player->GetMapId() != target->GetMapId())
        return false;

    if (!player->IsWithinLOSInMap(target))
        return false;

    if (!player->IsWithinDistInMap(target, desiredDistance))
        player->GetMotionMaster()->MoveFollow(target, desiredDistance, player->GetFollowAngle());

    return true;
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

    return QueuePlayer(player, BattlegroundLifecycleActions::ManagedRandomBotQueueTarget(), 0);
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

    return true;
}

bool BattlegroundTacticalActions::Execute(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !context.tacticsEnabled || !context.shouldEvaluate || !context.actionName)
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

    if (context.objective.type == BattlegroundObjectiveType::None &&
        context.movement == BattlegroundMovementPrimitive::None &&
        context.flagCarrierDirective == FlagCarrierDirective::None)
    {
        return false;
    }

    if (context.movement == BattlegroundMovementPrimitive::MoveToObjectiveUnit ||
        context.movement == BattlegroundMovementPrimitive::FollowFlagCarrier ||
        context.flagCarrierDirective != FlagCarrierDirective::None)
    {
        if (Player* carrier = FindFlagCarrierForDirective(player, context.flagCarrierDirective))
            return MoveTowardUnit(player, carrier, 20.0f);
    }

    if (context.movement == BattlegroundMovementPrimitive::MoveToObjectivePosition)
    {
        if (Battleground* battleground = player->GetBattleground())
        {
            if (WorldSafeLocsEntry const* graveyard = battleground->GetClosestGraveyard(player))
            {
                Position destination(graveyard->Loc.X, graveyard->Loc.Y, graveyard->Loc.Z, player->GetOrientation());
                if (!player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
                    player->GetMotionMaster()->MovePoint(0, destination);
                return true;
            }
        }
    }

    return true;
}

bool BattlegroundTacticalActions::CheckObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

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
    return MoveTowardUnit(player, enemyCarrier, 15.0f);
}

bool BattlegroundTacticalActions::ProtectFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    if (context.flagCarrierDirective != FlagCarrierDirective::ProtectTeamCarrier)
        return false;

    Player* teamCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::ProtectTeamCarrier);
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

    return QueuePlayer(player, BATTLEGROUND_AA, ARENA_TYPE_2v2);
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
}
