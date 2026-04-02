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
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

bool QueuePlayer(Player* player, BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    if (!player || player->InBattleground())
        return false;

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    if (!player->CanJoinToBattleground(bgTemplate) || !player->HasFreeBattlegroundQueueId())
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
        return false;

    uint32 const queueSlot = player->AddBattlegroundQueueId(bgQueueTypeId);
    uint32 const avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());

    WorldPacket statusPacket;
    sBattlegroundMgr->BuildBattlegroundStatusPacket(&statusPacket, bgTemplate, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, arenaType, 0);
    player->SendDirectMessage(&statusPacket);
    return true;
}

bool ProcessQueuePortAction(Player* player, bool accept, bool arenaOnly)
{
    if (!player || !player->GetSession() || !player->InBattlegroundQueue())
        return false;

    bool executed = false;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
        if (arenaOnly != isArenaQueue)
            continue;

        if (accept && !player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            continue;

        WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
        packet << uint8(BattlegroundMgr::BGArenaType(bgQueueTypeId)) << uint8(0)
               << uint32(BattlegroundMgr::BGTemplateId(bgQueueTypeId)) << uint16(0x1F90) << uint8(accept ? 1 : 0);
        player->GetSession()->HandleBattleFieldPortOpcode(packet);
        executed = true;
    }

    return executed;
}
}

namespace playerbot
{
bool BattlegroundLifecycleActions::Execute(Player* player, BattlegroundLifecycleContext const& context)
{
    if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
        return false;

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

    return QueuePlayer(player, BATTLEGROUND_RB, 0);
}

bool BattlegroundLifecycleActions::LeaveQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return ProcessQueuePortAction(player, false, false);
}

bool BattlegroundLifecycleActions::AcceptInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return ProcessQueuePortAction(player, true, false);
}

bool BattlegroundLifecycleActions::DeclineInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return ProcessQueuePortAction(player, false, false);
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

    player->LeaveBattleground();
    return true;
}

bool ArenaLifecycleActions::Execute(Player* player, ArenaLifecycleContext const& context)
{
    if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
        return false;

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

    return ProcessQueuePortAction(player, false, true);
}

bool ArenaLifecycleActions::AcceptTeamInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!player->GetSession())
        return false;

    if (!player->GetArenaTeamIdInvited())
        return false;

    WorldPacket packet(CMSG_ARENA_TEAM_ACCEPT);
    player->GetSession()->HandleArenaTeamAcceptOpcode(packet);
    return true;
}

bool ArenaLifecycleActions::DeclineTeamInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!player->GetSession())
        return false;

    if (!player->GetArenaTeamIdInvited())
        return false;

    WorldPacket packet(CMSG_ARENA_TEAM_DECLINE);
    player->GetSession()->HandleArenaTeamDeclineOpcode(packet);
    return true;
}
}
