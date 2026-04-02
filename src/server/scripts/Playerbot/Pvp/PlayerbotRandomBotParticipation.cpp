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

#include "PlayerbotRandomBotParticipation.h"

#include "PlayerbotPvpCore.h"
#include "PlayerbotPvpLifecycleActions.h"

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
void RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return;

    // Hook seam for manager integration: caller determines whether this player is a random bot.
    PvpValues const values = PvpCore::CollectValues(player);
    RandomBotParticipationHooks const hooks = PvpCore::BuildRandomBotParticipationHooks(player, values);

    if (!hooks.lifecycleEnabled)
        return;

    if (hooks.battlegroundParticipationHook)
    {
        BattlegroundLifecycleContext const battlegroundContext = PvpCore::BuildBattlegroundLifecycleContext(player, values);
        BattlegroundLifecycleActions::Execute(player, battlegroundContext);
    }

    if (hooks.arenaParticipationHook)
    {
        ArenaLifecycleContext const arenaContext = PvpCore::BuildArenaLifecycleContext(player, values);
        ArenaLifecycleActions::Execute(player, arenaContext);
    }
}
}
