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
#include "ScriptMgr.h"

namespace
{
class PlayerbotBootstrapWorldScript final : public WorldScript
{
public:
    PlayerbotBootstrapWorldScript() : WorldScript("PlayerbotBootstrapWorldScript") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        playerbot::PvpCore::LoadConfig();
        playerbot::RandomBotParticipationManager::ResetCadence();
    }

    void OnStartup() override
    {
        playerbot::PvpCore::LoadConfig();
        playerbot::RandomBotParticipationManager::ResetCadence();
        playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();

        TC_LOG_INFO("server.loading", "Playerbot bootstrap loaded (enabled: {}, pvp core: {}, pvp tactics: {}, pvp lifecycle: {}).",
            config.moduleEnabled ? "true" : "false", config.pvpCoreEnabled ? "true" : "false",
            config.pvpTacticsEnabled ? "true" : "false", config.pvpLifecycleEnabled ? "true" : "false");
    }

};

}

void AddPlayerbotScripts()
{
    new PlayerbotBootstrapWorldScript();
}
