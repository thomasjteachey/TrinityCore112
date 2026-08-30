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

#ifndef TRINITY_PLAYERBOT_PVP_CLASS_ACTIONS_H
#define TRINITY_PLAYERBOT_PVP_CLASS_ACTIONS_H

#include "PlayerbotPvpCore.h"

#include <chrono>
#include <string>

class Player;
class Unit;

namespace playerbot
{
class PvpClassActions
{
public:
    static bool AreRehgarMovementDiagnosticsEnabled();

    static bool Execute(Player* player, PvpClassSpellContext const& context);
    // Seams for the PvE manager: player MotionMasters can sit in a pending/
    // uninitialized state (see PrepareMotionMasterForExplicitBotMovement), so
    // external modules must issue follow/point movement through these instead
    // of calling MotionMaster directly.
    static bool PrepareForExplicitMovement(Player* player);
    static bool IssueFollowMovement(Player* player, Unit* target, float desiredDistance);
    static bool IsWarlockCurseTargetCooldownActive(Player const* player, Unit const* target, uint32 spellId);
    static void RegisterWarlockCurseTargetCooldown(Player const* player, Unit const* target, uint32 spellId, std::chrono::seconds cooldown);
    static bool IsCasterSpellCooldownActive(Player const* player, uint32 spellId);
    static void RegisterCasterSpellCooldown(Player const* player, uint32 spellId, std::chrono::seconds cooldown);
    static void RegisterCasterSpellCooldown(Player const* player, uint32 spellId, std::chrono::milliseconds cooldown);
    static std::string GetLastExecutionStatus(Player const* player);
    static std::string GetLastMovementDebugStatus(Player const* player);
    static bool HasRecentTargetRelativeMovementOrder(Player const* player, Unit const* target, uint32 maxAgeMs = 1500);
    static bool IsBattlegroundObjectInteractionInProgress(Player const* player);
    static bool IsPetSpellAction(Player const* player, PvpClassSpellContext const& context);
    static bool TryIssueShadowWraithFleeMovement(Player* player, Unit* threat);
    // Seam for the PvE manager: send the bot's pet at its victim. A pet that
    // is not attacking cannot growl, cannot hold threat and contributes
    // nothing, and the PvE tick had no way to command one at all.
    static void CommandPetAttack(Player* player, Unit* target);
};
}

#endif
