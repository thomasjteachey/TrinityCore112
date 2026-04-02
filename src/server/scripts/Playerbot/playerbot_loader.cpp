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

#include "Log.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
class PlayerbotBootstrapWorldScript final : public WorldScript
{
public:
    PlayerbotBootstrapWorldScript() : WorldScript("PlayerbotBootstrapWorldScript") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        playerbot::PvpCore::LoadConfig();
    }

    void OnStartup() override
    {
        playerbot::PvpCore::LoadConfig();
        playerbot::RandomBotParticipationLifecycle::RegisterManagerHooks();
        playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();

        TC_LOG_INFO("server.loading", "Playerbot bootstrap loaded (enabled: {}, pvp core: {}, pvp tactics: {}, pvp lifecycle: {}).",
            config.moduleEnabled ? "true" : "false", config.pvpCoreEnabled ? "true" : "false",
            config.pvpTacticsEnabled ? "true" : "false", config.pvpLifecycleEnabled ? "true" : "false");
    }

    void OnUpdate(uint32 diff) override
    {
        updateAccumulatorMs += diff;
        if (updateAccumulatorMs < 1000)
            return;

        updateAccumulatorMs = 0;

        playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
        if (!config.moduleEnabled || !config.pvpCoreEnabled || !config.pvpLifecycleEnabled)
            return;

        SessionMap const& sessions = sWorld->GetAllSessions();
        for (SessionMap::const_iterator itr = sessions.begin(); itr != sessions.end(); ++itr)
        {
            Player* player = itr->second ? itr->second->GetPlayer() : nullptr;
            if (!player)
                continue;

            playerbot::PvpValues const values = playerbot::PvpCore::CollectValues(player);
            playerbot::RandomBotParticipationHooks const hooks = playerbot::PvpCore::BuildRandomBotParticipationHooks(player, values);
            if (!hooks.battlegroundParticipationHook && !hooks.arenaParticipationHook)
                continue;

            playerbot::RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(player);
        }
    }

private:
    uint32 updateAccumulatorMs = 0;
};
}

void AddPlayerbotScripts()
{
    new PlayerbotBootstrapWorldScript();
}
