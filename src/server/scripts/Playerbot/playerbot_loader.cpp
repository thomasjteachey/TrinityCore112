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
#include "Chat.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "RBAC.h"
#include "ScriptMgr.h"

using namespace Trinity::ChatCommands;

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
        playerbot::RandomBotParticipationManager::LoadPopulationConfig();
    }

    void OnStartup() override
    {
        playerbot::PvpCore::LoadConfig();
        playerbot::RandomBotParticipationManager::ResetCadence();
        playerbot::RandomBotParticipationManager::LoadPopulationConfig();
        playerbot::RandomBotParticipationManager::OnStartupBootstrap();
        playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
        playerbot::RandomBotPopulationSnapshot const population = playerbot::RandomBotParticipationManager::GetPopulationSnapshot();

        TC_LOG_INFO("server.loading", "Playerbot bootstrap loaded (enabled: {}, pvp core: {}, pvp tactics: {}, pvp lifecycle: {}, pvp class spells: {}).",
            config.moduleEnabled ? "true" : "false", config.pvpCoreEnabled ? "true" : "false",
            config.pvpTacticsEnabled ? "true" : "false", config.pvpLifecycleEnabled ? "true" : "false",
            config.pvpClassSpellsEnabled ? "true" : "false");

        TC_LOG_INFO("server.loading", "Playerbot random population bootstrap (configEnabled: {}, runtimeEnabled: {}, target=[{}, {}], onlineBots: {}, loginOrchestration: {}).",
            population.configEnabled ? "true" : "false", population.runtimeEnabled ? "true" : "false",
            population.targetMin, population.targetMax, population.onlineRandomBots,
            population.supportsLoginOrchestration ? "true" : "false");
    }

    void OnUpdate(uint32 diff) override
    {
        playerbot::RandomBotParticipationManager::OnWorldUpdate(diff);
    }
};

class PlayerbotLifecyclePlayerScript final : public PlayerScript
{
public:
    PlayerbotLifecyclePlayerScript() : PlayerScript("PlayerbotLifecyclePlayerScript") { }

    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        playerbot::RandomBotParticipationManager::ProcessPlayerLifecycle(player);
    }

    void OnLogout(Player* player) override
    {
        playerbot::RandomBotParticipationManager::OnPlayerLogout(player);
    }
};

class PlayerbotLifecycleCommandScript final : public CommandScript
{
public:
    PlayerbotLifecycleCommandScript() : CommandScript("PlayerbotLifecycleCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotPvpLifecycleTable =
        {
            { "snapshot", HandlePlayerbotPvpLifecycleSnapshotCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };

        static ChatCommandTable playerbotPvpTable =
        {
            { "lifecycle", playerbotPvpLifecycleTable },
        };

        static ChatCommandTable playerbotRandomPopulationTable =
        {
            { "status", HandlePlayerbotPopulationStatusCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "start", HandlePlayerbotPopulationStartCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "stop", HandlePlayerbotPopulationStopCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "rebalance now", HandlePlayerbotPopulationRebalanceCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "list", HandlePlayerbotPopulationPoolCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };

        static ChatCommandTable playerbotTable =
        {
            { "pvp", playerbotPvpTable },
            { "population", playerbotRandomPopulationTable },
        };

        static ChatCommandTable commandTable =
        {
            { "playerbot", playerbotTable },
        };

        return commandTable;
    }

    static bool HandlePlayerbotPvpLifecycleSnapshotCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::LifecycleObservationSnapshot const snapshot = playerbot::RandomBotParticipationManager::GetLifecycleObservationSnapshot();

        handler->PSendSysMessage("Playerbot PvP lifecycle observation snapshot:");
        handler->PSendSysMessage(" - gateDisabled: " UI64FMTD, snapshot.gateDisabled);
        handler->PSendSysMessage(" - cadenceThrottled: " UI64FMTD, snapshot.cadenceThrottled);
        handler->PSendSysMessage(" - invalidPlayerState: " UI64FMTD, snapshot.invalidPlayerState);
        handler->PSendSysMessage(" - noLifecycleHooksActive: " UI64FMTD, snapshot.noLifecycleHooksActive);
        handler->PSendSysMessage(" - battlegroundLifecycleExecuted: " UI64FMTD, snapshot.battlegroundLifecycleExecuted);
        handler->PSendSysMessage(" - arenaLifecycleExecuted: " UI64FMTD, snapshot.arenaLifecycleExecuted);
        return true;
    }

    static bool HandlePlayerbotPopulationStatusCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotPopulationSnapshot const snapshot = playerbot::RandomBotParticipationManager::GetPopulationSnapshot();
        handler->PSendSysMessage("Playerbot random population status:");
        handler->PSendSysMessage(" - configEnabled: %u", snapshot.configEnabled ? 1u : 0u);
        handler->PSendSysMessage(" - runtimeEnabled: %u", snapshot.runtimeEnabled ? 1u : 0u);
        handler->PSendSysMessage(" - loginOrchestrationSupported: %u", snapshot.supportsLoginOrchestration ? 1u : 0u);
        handler->PSendSysMessage(" - targetRange: %u-%u", snapshot.targetMin, snapshot.targetMax);
        handler->PSendSysMessage(" - maxOnlineBotsPerAccount: %u (0 means unlimited)", snapshot.maxOnlineBotsPerAccount);
        handler->PSendSysMessage(" - onlineRandomBots: %u (alliance=%u horde=%u)", snapshot.onlineRandomBots,
            snapshot.onlineAllianceRandomBots, snapshot.onlineHordeRandomBots);
        handler->PSendSysMessage(" - offlinePoolSize: %u", snapshot.offlinePoolSize);
        handler->PSendSysMessage(" - rebalanceTicks: " UI64FMTD, snapshot.rebalanceTicks);
        handler->PSendSysMessage(" - loginAttempts/success: " UI64FMTD "/" UI64FMTD, snapshot.loginAttempts, snapshot.loginSuccess);
        handler->PSendSysMessage(" - logoutAttempts/success: " UI64FMTD "/" UI64FMTD, snapshot.logoutAttempts, snapshot.logoutSuccess);
        handler->PSendSysMessage(" - skippedSafetyRealPlayers: " UI64FMTD, snapshot.skippedSafetyRealPlayers);
        handler->PSendSysMessage(" - skippedNoCandidatePool: " UI64FMTD, snapshot.skippedNoCandidatePool);
        handler->PSendSysMessage(" - skippedIntegrationGap: " UI64FMTD, snapshot.skippedIntegrationGap);
        handler->PSendSysMessage(" - lastRebalanceUnixTime: " UI64FMTD, snapshot.lastRebalanceUnixTime);
        return true;
    }

    static bool HandlePlayerbotPopulationStartCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotParticipationManager::SetPopulationRuntimeEnabled(true);
        handler->PSendSysMessage("Playerbot random population manager runtime state set to STARTED.");
        return true;
    }

    static bool HandlePlayerbotPopulationStopCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotParticipationManager::SetPopulationRuntimeEnabled(false);
        handler->PSendSysMessage("Playerbot random population manager runtime state set to STOPPED.");
        return true;
    }

    static bool HandlePlayerbotPopulationRebalanceCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        bool const executed = playerbot::RandomBotParticipationManager::TriggerImmediateRebalance();
        handler->PSendSysMessage("Playerbot random population rebalance executed: %u", executed ? 1u : 0u);
        return true;
    }

    static bool HandlePlayerbotPopulationPoolCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotPopulationSnapshot const snapshot = playerbot::RandomBotParticipationManager::GetPopulationSnapshot();
        handler->PSendSysMessage("Playerbot random population pool stats:");
        handler->PSendSysMessage(" - offlinePoolSize: %u", snapshot.offlinePoolSize);
        handler->PSendSysMessage(" - targetRange: %u-%u", snapshot.targetMin, snapshot.targetMax);
        handler->PSendSysMessage(" - onlineRandomBots: %u", snapshot.onlineRandomBots);
        return true;
    }
};

}

void AddPlayerbotScripts()
{
    new PlayerbotBootstrapWorldScript();
    new PlayerbotLifecyclePlayerScript();
    new PlayerbotLifecycleCommandScript();
}
