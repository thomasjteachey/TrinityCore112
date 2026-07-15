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

#ifndef TRINITY_PLAYERBOT_PVP_LIFECYCLE_ACTIONS_H
#define TRINITY_PLAYERBOT_PVP_LIFECYCLE_ACTIONS_H

#include "PlayerbotPvpCore.h"

class Player;
namespace playerbot
{
bool NormalizeLifecycleQueueState(Player* player);
uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType);
void NotifyHunterAutoShotFired(Player* player);

class BattlegroundLifecycleActions
{
public:
    static bool Execute(Player* player, BattlegroundLifecycleContext const& context);

    static bool JoinQueuePrimitive(Player* player);
    static bool LeaveQueuePrimitive(Player* player);
    static bool AcceptInvitePrimitive(Player* player);
    static bool DeclineInvitePrimitive(Player* player);
    static bool HandleInProgressStatusPrimitive(Player* player);
    static bool HandleDeathPrimitive(Player* player);
};

class BattlegroundTacticalActions
{
public:
    static bool Execute(Player* player, BattlegroundTacticalContext const& context);

    static bool MoveToStartPrimitive(Player* player);
    static bool MoveToObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context);
    static bool PursueEnemyPrimitive(Player* player);
    static bool ResetObjectiveForcePrimitive(Player* player);
    static bool UseBuffPrimitive(Player* player);
    static bool AttackEnemyFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context);
    static bool ProtectFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context);
    static void DelayFlagPickup(Player* player, uint32 delayMs);
};

class ArenaLifecycleActions
{
public:
    static bool Execute(Player* player, ArenaLifecycleContext const& context);

    static bool JoinQueuePrimitive(Player* player);
    static bool LeaveQueuePrimitive(Player* player);
    static bool AcceptTeamInvitePrimitive(Player* player);
    static bool DeclineTeamInvitePrimitive(Player* player);
};

class DuelTacticalActions
{
public:
    static bool Execute(Player* player);
};
}

#endif
