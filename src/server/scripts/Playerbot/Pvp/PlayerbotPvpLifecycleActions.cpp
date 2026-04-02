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

#include "Log.h"
#include "Player.h"

namespace
{
bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
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
            didExecute = AcceptInvitePlaceholder(player) || didExecute;
            break;
        case InvitationResponseType::Decline:
            didExecute = DeclineInvitePlaceholder(player) || didExecute;
            break;
        case InvitationResponseType::None:
        default:
            break;
    }

    if (context.shouldHandleInProgressStatus)
        didExecute = HandleInProgressStatusPlaceholder(player) || didExecute;

    return didExecute;
}

bool BattlegroundLifecycleActions::JoinQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle battleground queue join primitive placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}

bool BattlegroundLifecycleActions::LeaveQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle battleground queue leave primitive placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}

bool BattlegroundLifecycleActions::AcceptInvitePlaceholder(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle battleground invite accept placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}

bool BattlegroundLifecycleActions::DeclineInvitePlaceholder(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle battleground invite decline placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}

bool BattlegroundLifecycleActions::HandleInProgressStatusPlaceholder(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle battleground in-progress status placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
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
            didExecute = AcceptTeamInvitePlaceholder(player) || didExecute;
            break;
        case ArenaTeamInteractionType::DeclineInvite:
            didExecute = DeclineTeamInvitePlaceholder(player) || didExecute;
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

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle arena queue join primitive placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}

bool ArenaLifecycleActions::LeaveQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle arena queue leave primitive placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}

bool ArenaLifecycleActions::AcceptTeamInvitePlaceholder(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle arena team accept placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}

bool ArenaLifecycleActions::DeclineTeamInvitePlaceholder(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    TC_LOG_DEBUG("playerbot", "Playerbot PvP lifecycle arena team decline placeholder for player {}.",
        player->GetGUID().ToString());
    return false;
}
}
