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

#include "PlayerbotPvpContext.h"

#include "Battleground.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cstdint>

namespace
{
    constexpr float LowHealthThresholdPct = 35.0f;
    constexpr float ObjectiveProximityRadius = 60.0f;

    uint32 GetEnemyTeamId(uint32 teamId)
    {
        return teamId == ALLIANCE ? HORDE : ALLIANCE;
    }

    TeamId GetEnemyTeamIndex(TeamId teamId)
    {
        return teamId == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    }
}

namespace playerbot
{
    BattlegroundPvpStateSnapshot PlayerbotPvpContext::BuildSnapshot(Player const* bot) const
    {
        BattlegroundPvpStateSnapshot snapshot;

        if (!bot)
            return snapshot;

        Battleground* battleground = bot->GetBattleground();
        if (!battleground)
            return snapshot;

        uint32 const teamId = bot->GetBGTeam();
        if (teamId != ALLIANCE && teamId != HORDE)
            return snapshot;

        TeamId const teamIndex = teamId == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE;
        TeamId const enemyTeamIndex = GetEnemyTeamIndex(teamIndex);

        snapshot.TeamSize = static_cast<std::uint8_t>(std::min<uint32>(battleground->GetPlayersCountByTeam(teamId), 255));
        snapshot.EnemyTeamSize = static_cast<std::uint8_t>(std::min<uint32>(battleground->GetPlayersCountByTeam(GetEnemyTeamId(teamId)), 255));

        snapshot.TeamHasFlagCarrier = !battleground->GetFlagPickerGUID(teamIndex).IsEmpty();
        snapshot.EnemyHasFlagCarrier = !battleground->GetFlagPickerGUID(enemyTeamIndex).IsEmpty();

        if (Position const* friendlyStart = battleground->GetTeamStartPosition(teamIndex))
            snapshot.IsNearFriendlyObjective = bot->GetDistance(*friendlyStart) <= ObjectiveProximityRadius;

        if (Position const* enemyStart = battleground->GetTeamStartPosition(enemyTeamIndex))
            snapshot.IsNearEnemyObjective = bot->GetDistance(*enemyStart) <= ObjectiveProximityRadius;

        snapshot.IsHealthLow = bot->GetHealthPct() <= LowHealthThresholdPct;

        return snapshot;
    }
}
