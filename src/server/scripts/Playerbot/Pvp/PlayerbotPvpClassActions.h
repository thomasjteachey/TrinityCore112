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
    struct RangedApproachDiagnostic
    {
        bool valid = false;
        ObjectGuid targetGuid = ObjectGuid::Empty;
        float desiredRange = 0.0f;
        float currentDistance = 0.0f;
        uint8 stagnantSamples = 0;
        uint32 lastSampleMs = 0;
        uint32 lastFallbackMs = 0;
        uint8 motionType = 0;
        bool moving = false;
    };

    static bool Execute(Player* player, PvpClassSpellContext const& context);
    static bool IsWarlockCurseTargetCooldownActive(Player const* player, Unit const* target, uint32 spellId);
    static void RegisterWarlockCurseTargetCooldown(Player const* player, Unit const* target, uint32 spellId, std::chrono::seconds cooldown);
    static bool IsCasterSpellCooldownActive(Player const* player, uint32 spellId);
    static void RegisterCasterSpellCooldown(Player const* player, uint32 spellId, std::chrono::seconds cooldown);
    static std::string GetLastExecutionStatus(Player const* player);
    static RangedApproachDiagnostic GetRangedApproachDiagnostic(Player const* player);
};
}

#endif
