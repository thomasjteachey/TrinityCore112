/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITY_PLAYERBOT_PVP_ROLE_SELECTOR_H
#define TRINITY_PLAYERBOT_PVP_ROLE_SELECTOR_H

#include <array>
#include <cstdint>

namespace playerbot
{
    enum class BattlegroundPvpRole : std::uint8_t
    {
        Frontline = 0,
        Defender = 1,
        ObjectiveRunner = 2,
        Support = 3
    };

    struct BattlegroundPvpStateSnapshot
    {
        std::uint8_t TeamSize = 0;
        std::uint8_t EnemyTeamSize = 0;
        bool TeamHasFlagCarrier = false;
        bool EnemyHasFlagCarrier = false;
        bool IsNearFriendlyObjective = false;
        bool IsNearEnemyObjective = false;
        bool IsHealthLow = false;
    };

    class PlayerbotPvpRoleSelector
    {
    public:
        BattlegroundPvpRole SelectRole(BattlegroundPvpStateSnapshot const& snapshot, std::uint32_t randomSeed) const;

    private:
        static constexpr std::array<BattlegroundPvpRole, 4> Roles = {
            BattlegroundPvpRole::Frontline,
            BattlegroundPvpRole::Defender,
            BattlegroundPvpRole::ObjectiveRunner,
            BattlegroundPvpRole::Support};
    };
}

#endif
