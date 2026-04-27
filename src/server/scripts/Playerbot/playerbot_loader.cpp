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
#include "Globals/ObjectAccessor.h"
#include "Item.h"
#include "MotionMaster.h"
#include "Movement/AbstractFollower.h"
#include "Player.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Playerbot/Pvp/PlayerbotPvpClassActions.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotPvpLifecycleActions.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellHistory.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <algorithm>
#include <cctype>

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

char const* ToString(MovementGeneratorType motionType)
{
    switch (motionType)
    {
        case IDLE_MOTION_TYPE: return "idle";
        case RANDOM_MOTION_TYPE: return "random";
        case WAYPOINT_MOTION_TYPE: return "waypoint";
        case CONFUSED_MOTION_TYPE: return "confused";
        case CHASE_MOTION_TYPE: return "chase";
        case HOME_MOTION_TYPE: return "home";
        case FLIGHT_MOTION_TYPE: return "flight";
        case POINT_MOTION_TYPE: return "point";
        case FLEEING_MOTION_TYPE: return "fleeing";
        case DISTRACT_MOTION_TYPE: return "distract";
        case ASSISTANCE_MOTION_TYPE: return "assist";
        case ASSISTANCE_DISTRACT_MOTION_TYPE: return "assist_distract";
        case TIMED_FLEEING_MOTION_TYPE: return "timed_flee";
        case FOLLOW_MOTION_TYPE: return "follow";
        case ROTATE_MOTION_TYPE: return "rotate";
        case EFFECT_MOTION_TYPE: return "effect";
        case SPLINE_CHAIN_MOTION_TYPE: return "spline_chain";
        case FORMATION_MOTION_TYPE: return "formation";
        case MAX_MOTION_TYPE:
        default:
            return "unknown";
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
    Unit* directiveTarget = classContext.movementTargetGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, classContext.movementTargetGuid);
    std::string const lastClassExecutionStatus = playerbot::PvpClassActions::GetLastExecutionStatus(bot);
    bool const lifecycleEnabled = lifecycleContext.lifecycleEnabled;
    bool const managedRandomBot = playerbot::IsManagedRandomBot(bot);
    bool const canFollowCommands = bot->IsAlive() &&
        !bot->HasUnitState(UNIT_STATE_ROOT) &&
        !bot->HasUnitState(UNIT_STATE_STUNNED) &&
        !bot->HasUnitState(UNIT_STATE_CONFUSED) &&
        !bot->HasUnitState(UNIT_STATE_FLEEING) &&
        !bot->HasUnitState(UNIT_STATE_LOST_CONTROL);

    std::ostringstream status;
    MovementGeneratorType const motionType = bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    uint32 const movementFlags = bot->GetUnitMovementFlags();
    status << "PB status: "
           << "lifecycle=" << (lifecycleEnabled ? "on" : "off")
           << " managed=" << (managedRandomBot ? "yes" : "no")
           << "combat=" << (bot->IsInCombat() ? "yes" : "no")
           << " alive=" << (bot->IsAlive() ? "yes" : "no")
           << " moving=" << (bot->isMoving() ? "yes" : "no")
           << " can_follow=" << (canFollowCommands ? "yes" : "no")
           << " rooted=" << (bot->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no")
           << " stunned=" << (bot->HasUnitState(UNIT_STATE_STUNNED) ? "yes" : "no")
           << " confused=" << (bot->HasUnitState(UNIT_STATE_CONFUSED) ? "yes" : "no")
           << " fleeing=" << (bot->HasUnitState(UNIT_STATE_FLEEING) ? "yes" : "no")
           << " lost_control=" << (bot->HasUnitState(UNIT_STATE_LOST_CONTROL) ? "yes" : "no")
           << " stealth=" << (bot->HasStealthAura() ? "yes" : "no")
           << " casting=" << (bot->IsNonMeleeSpellCast(false, false, true) ? "yes" : "no")
           << " bg_state=" << ToString(values.battlegroundState)
           << " class_gate=" << (classContext.classSpellsEnabled ? "on" : "off")
           << " class_exec=" << (classContext.shouldExecute ? "yes" : "no")
           << " class_action=" << (classContext.actionName ? classContext.actionName : "none")
           << " spell=" << classContext.spellId
           << " reason=" << (classContext.reason ? classContext.reason : "none")
           << " target_mode=" << ToString(classContext.targetMode)
           << " target_guid=" << classContext.targetGuid.ToString()
           << " move_directive=" << ToString(classContext.movementDirective)
           << " move_target=" << classContext.movementTargetGuid.ToString()
           << " move_target_resolved=" << (directiveTarget ? "yes" : "no")
           << " move_range=" << classContext.movementFollowRange
           << " lifecycle_q=" << ToString(lifecycleContext.queueOperation)
           << " lifecycle_invite=" << ToString(lifecycleContext.invitationResponse)
           << " hooks(bg=" << (hooks.battlegroundParticipationHook ? "on" : "off")
           << ",arena=" << (hooks.arenaParticipationHook ? "on" : "off") << ")"
           << " last_exec=" << lastClassExecutionStatus
           << " strict_pathing=" << (bot->InBattleground() ? "on" : "off")
           << " motion=" << uint32(motionType)
           << "/" << ToString(motionType)
           << " move_flags=0x" << std::hex << movementFlags << std::dec
           << " pos=(" << bot->GetMapId() << ":" << bot->GetPositionX() << "," << bot->GetPositionY() << "," << bot->GetPositionZ() << ")"
           << " o=" << bot->GetOrientation();

    if (Unit* victim = bot->GetVictim())
        status << " victim=" << victim->GetName() << " victim_dist=" << bot->GetDistance(victim);
    else
        status << " victim=none";

    Unit* motionTarget = nullptr;
    if (MotionMaster* motionMaster = bot->GetMotionMaster())
        if (MovementGenerator* movement = motionMaster->GetCurrentMovementGenerator())
            if (AbstractFollower* follower = dynamic_cast<AbstractFollower*>(movement))
                motionTarget = follower->GetTarget();

    if (motionTarget)
    {
        bool const motionTargetMatchesVictim = bot->GetVictim() && bot->GetVictim()->GetGUID() == motionTarget->GetGUID();
        bool const chaseVictimMismatch = bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE && !motionTargetMatchesVictim;
        status << " motion_target=" << motionTarget->GetName()
               << " motion_target_dist=" << bot->GetDistance(motionTarget)
               << " motion_target_is_victim=" << (motionTargetMatchesVictim ? "yes" : "no")
               << " chase_victim_mismatch=" << (chaseVictimMismatch ? "yes" : "no");
    }
    else
    {
        status << " motion_target=none";
    }

    if (directiveTarget)
    {
        status << " directive_target=" << directiveTarget->GetName()
               << " directive_target_dist=" << bot->GetDistance(directiveTarget)
               << " directive_target_los=" << (bot->IsWithinLOSInMap(directiveTarget) ? "yes" : "no")
               << " directive_target_attackable=" << (bot->IsValidAttackTarget(directiveTarget) ? "yes" : "no");
    }
    else
    {
        status << " directive_target=none";
    }

    if (Unit* selected = bot->GetSelectedUnit())
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


std::string BuildManagedBotMovementDiagnosticLine(Player* bot)
{
    if (!bot)
        return "PB move diag: unavailable";

    return "PB move diag: " + playerbot::PvpClassActions::GetLastMovementDiagnostic(bot);
}

std::string BuildManagedBotScmQueueDiagnosticLine(Player* bot)
{
    if (!bot)
        return "PB SCM diag unavailable.";

    constexpr BattlegroundTypeId kScmTypeId = BATTLEGROUND_SCM;
    constexpr uint32 kDeserterSpellId = 26013;
    BattlegroundQueueTypeId const scmQueueTypeId = BattlegroundMgr::BGQueueTypeId(kScmTypeId, 0);
    bool const queuedForScm = scmQueueTypeId != BATTLEGROUND_QUEUE_NONE &&
        bot->GetBattlegroundQueueIndex(scmQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES;
    bool const invitedForScm = scmQueueTypeId != BATTLEGROUND_QUEUE_NONE && bot->IsInvitedForBattlegroundQueueType(scmQueueTypeId);
    bool const hasScmAccessByLevel = bot->GetBGAccessByLevel(kScmTypeId);
    bool const hasAnyFreeQueueSlot = bot->HasFreeBattlegroundQueueId();

    uint8 usedQueueSlots = 0;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (bot->GetBattlegroundQueueTypeId(i) != BATTLEGROUND_QUEUE_NONE)
            ++usedQueueSlots;

    bool hasQueueRecord = false;
    GroupQueueInfo ginfo{};
    if (queuedForScm)
    {
        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(scmQueueTypeId);
        hasQueueRecord = queue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo);
    }

    std::string reason = "unknown";
    if (bot->InBattleground())
        reason = "already-in-battleground";
    else if (!bot->IsAlive())
        reason = "dead-outside-bg";
    else if (!hasScmAccessByLevel)
        reason = "no-scm-level-access";
    else if (bot->HasAura(kDeserterSpellId))
        reason = "deserter-aura";
    else if (queuedForScm && invitedForScm)
        reason = "invite-pending-accept";
    else if (queuedForScm && !hasQueueRecord)
        reason = "stale-local-scm-queue-slot";
    else if (queuedForScm)
        reason = "queued-waiting";
    else if (!hasAnyFreeQueueSlot)
        reason = "no-free-queue-slot";
    else if (!bot->IsInWorld())
        reason = "not-in-world";
    else if (bot->IsBeingTeleported())
        reason = "teleporting";
    else
        reason = "eligible-not-queued";

    std::ostringstream diag;
    diag << "PB SCM diag: "
         << "bot=" << bot->GetName()
         << " in_world=" << (bot->IsInWorld() ? "yes" : "no")
         << " in_bg=" << (bot->InBattleground() ? "yes" : "no")
         << " bg_type=" << uint32(bot->GetBattlegroundTypeId())
         << " bg_status=" << (bot->GetBattleground() ? uint32(bot->GetBattleground()->GetStatus()) : 255)
         << " scm_access=" << (hasScmAccessByLevel ? "yes" : "no")
         << " alive=" << (bot->IsAlive() ? "yes" : "no")
         << " deserter=" << (bot->HasAura(kDeserterSpellId) ? "yes" : "no")
         << " queued_scm=" << (queuedForScm ? "yes" : "no")
         << " invited_scm=" << (invitedForScm ? "yes" : "no")
         << " queue_record=" << (hasQueueRecord ? "yes" : "no")
         << " queue_slots_used=" << uint32(usedQueueSlots) << "/" << uint32(PLAYER_MAX_BATTLEGROUND_QUEUES)
         << " free_queue_slot=" << (hasAnyFreeQueueSlot ? "yes" : "no")
         << " teleporting=" << (bot->IsBeingTeleported() ? "yes" : "no")
         << " reason=" << reason;

    return diag.str();
}

BattlegroundTypeId ResolveCurrentBgTypeFromPlayerContext(Player const* player)
{
    if (!player)
        return BATTLEGROUND_TYPE_NONE;

    if (player->InBattleground())
        return player->GetBattlegroundTypeId();

    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const queueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        if (BattlegroundMgr::BGArenaType(queueTypeId) != 0)
            continue;

        BattlegroundTypeId const queueBgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        if (queueBgTypeId != BATTLEGROUND_TYPE_NONE)
            return queueBgTypeId;
    }

    return BATTLEGROUND_TYPE_NONE;
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

    void OnChat(Player* sender, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (!sender || !receiver)
            return;

        if (type != CHAT_MSG_WHISPER)
            return;

        if (lang == LANG_ADDON)
            return;

        if (!playerbot::IsManagedRandomBot(receiver))
            return;

        receiver->Whisper(BuildManagedBotStatusLine(receiver), LANG_UNIVERSAL, sender);

        receiver->Whisper(BuildManagedBotMovementDiagnosticLine(receiver), LANG_UNIVERSAL, sender);

        receiver->Whisper(BuildManagedBotScmQueueDiagnosticLine(receiver), LANG_UNIVERSAL, sender);
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
            { "forcequeue", HandlePlayerbotPvpForceQueueCurrentCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
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

    static bool HandlePlayerbotPvpForceQueueCurrentCommand(ChatHandler* handler)
    {
        if (!handler || !handler->GetSession())
            return false;

        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        BattlegroundTypeId const bgTypeId = ResolveCurrentBgTypeFromPlayerContext(player);
        if (bgTypeId == BATTLEGROUND_TYPE_NONE)
        {
            handler->PSendSysMessage("You must be inside a battleground or queued for one before using this command.");
            return false;
        }

        uint32 const queuedCount = playerbot::QueueEligibleManagedBotsForBattleground(bgTypeId, 0);
        handler->PSendSysMessage("Forced managed playerbots to queue for battleground type %u. Queued bots: %u", uint32(bgTypeId), queuedCount);
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
