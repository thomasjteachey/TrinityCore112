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
#include "GameTime.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "RBAC.h"
#include "ScriptMgr.h"

#include <sstream>

using namespace Trinity::ChatCommands;

namespace
{
char const* ToString(playerbot::BattlegroundState state)
{
    switch (state)
    {
        case playerbot::BattlegroundState::Queueing: return "queueing";
        case playerbot::BattlegroundState::WaitingToStart: return "waiting";
        case playerbot::BattlegroundState::Active: return "active";
        case playerbot::BattlegroundState::None:
        default: return "none";
    }
}

char const* ToString(playerbot::QueueOperationType op)
{
    switch (op)
    {
        case playerbot::QueueOperationType::Join: return "join";
        case playerbot::QueueOperationType::Leave: return "leave";
        case playerbot::QueueOperationType::None:
        default: return "none";
    }
}

char const* ToString(playerbot::InvitationResponseType response)
{
    switch (response)
    {
        case playerbot::InvitationResponseType::Accept: return "accept";
        case playerbot::InvitationResponseType::Decline: return "decline";
        case playerbot::InvitationResponseType::None:
        default: return "none";
    }
}

char const* ToString(playerbot::PvpClassSpellContext::MovementDirective directive)
{
    switch (directive)
    {
        case playerbot::PvpClassSpellContext::MovementDirective::ReachMeleeRange: return "reach_melee";
        case playerbot::PvpClassSpellContext::MovementDirective::ReachSpellRange: return "reach_spell";
        case playerbot::PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell: return "flee";
        case playerbot::PvpClassSpellContext::MovementDirective::FaceSpellTarget: return "face_target";
        case playerbot::PvpClassSpellContext::MovementDirective::DropInvalidTarget: return "drop_target";
        case playerbot::PvpClassSpellContext::MovementDirective::CheckMountState: return "check_mount";
        case playerbot::PvpClassSpellContext::MovementDirective::ResetCombatState: return "reset_combat";
        case playerbot::PvpClassSpellContext::MovementDirective::None:
        default: return "none";
    }
}

char const* ToString(playerbot::PvpClassSpellContext::TargetMode mode)
{
    switch (mode)
    {
        case playerbot::PvpClassSpellContext::TargetMode::Enemy: return "enemy";
        case playerbot::PvpClassSpellContext::TargetMode::Ally: return "ally";
        case playerbot::PvpClassSpellContext::TargetMode::Self: return "self";
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default: return "none";
    }
}

std::string BuildManagedBotStatusLine(Player* bot)
{
    if (!bot)
        return "Playerbot status unavailable.";

    playerbot::PvpValues const values = playerbot::PvpCore::CollectValues(bot);
    playerbot::PvpClassSpellContext const classContext = playerbot::PvpCore::BuildClassSpellContext(bot, values);
    playerbot::BattlegroundLifecycleContext const lifecycleContext = playerbot::PvpCore::BuildBattlegroundLifecycleContext(bot, values);
    playerbot::RandomBotParticipationHooks const hooks = playerbot::PvpCore::BuildRandomBotParticipationHooks(bot, values);

    std::ostringstream status;
    status << "PB status: "
           << "combat=" << (bot->IsInCombat() ? "yes" : "no")
           << " alive=" << (bot->IsAlive() ? "yes" : "no")
           << " moving=" << (bot->isMoving() ? "yes" : "no")
           << " casting=" << (bot->IsNonMeleeSpellCast(false, false, true) ? "yes" : "no")
           << " bg_state=" << ToString(values.battlegroundState)
           << " class_action=" << (classContext.actionName ? classContext.actionName : "none")
           << " spell=" << classContext.spellId
           << " reason=" << (classContext.reason ? classContext.reason : "none")
           << " target_mode=" << ToString(classContext.targetMode)
           << " target_guid=" << classContext.targetGuid.ToString()
           << " move_directive=" << ToString(classContext.movementDirective)
           << " move_target=" << classContext.movementTargetGuid.ToString()
           << " move_range=" << classContext.movementFollowRange
           << " lifecycle_q=" << ToString(lifecycleContext.queueOperation)
           << " lifecycle_invite=" << ToString(lifecycleContext.invitationResponse)
           << " hooks(bg=" << (hooks.battlegroundParticipationHook ? "on" : "off")
           << ",arena=" << (hooks.arenaParticipationHook ? "on" : "off") << ")"
           << " motion=" << uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType())
           << " pos=(" << bot->GetMapId() << ":" << bot->GetPositionX() << "," << bot->GetPositionY() << "," << bot->GetPositionZ() << ")"
           << " o=" << bot->GetOrientation();

    if (Unit* victim = bot->GetVictim())
        status << " victim=" << victim->GetName() << " victim_dist=" << bot->GetDistance(victim);
    else
        status << " victim=none";

    if (Unit* selected = ObjectAccessor::GetUnit(*bot, bot->GetSelection()))
    {
        constexpr float kHalfCircleArc = 3.14159265358979323846f;
        status << " selected=" << selected->GetName()
               << " selected_dist=" << bot->GetDistance(selected)
               << " in_front=" << (bot->HasInArc(kHalfCircleArc, selected) ? "yes" : "no");
    }
    else
    {
        status << " selected=none";
    }

    return status.str();
}

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

    void OnDuelRequest(Player* target, Player* challenger) override
    {
        if (!target || !challenger)
            return;

        if (!playerbot::IsManagedRandomBot(target))
            return;

        if (!target->duel || !challenger->duel || target->duel->State != DUEL_STATE_CHALLENGED)
            return;

        if (target->duel->Opponent != challenger || challenger->duel->Opponent != target)
            return;

        time_t const now = GameTime::GetGameTime();
        target->duel->StartTime = now + 3;
        challenger->duel->StartTime = now + 3;

        target->duel->State = DUEL_STATE_COUNTDOWN;
        challenger->duel->State = DUEL_STATE_COUNTDOWN;

        target->SendDuelCountdown(3000);
        challenger->SendDuelCountdown(3000);
    }

    void OnChat(Player* sender, uint32 type, uint32 lang, std::string& /*msg*/, Player* receiver) override
    {
        if (!sender || !receiver)
            return;

        if (type != CHAT_MSG_WHISPER)
            return;

        if (lang == LANG_ADDON)
            return;

        if (!sender->IsGameMaster())
            return;

        if (!playerbot::IsManagedRandomBot(receiver))
            return;

        receiver->Whisper(BuildManagedBotStatusLine(receiver), LANG_UNIVERSAL, sender);
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
