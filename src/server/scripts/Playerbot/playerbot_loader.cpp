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
#include "CharacterCache.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "Item.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Optional.h"
#include "MoveSpline.h"
#include "Movement/AbstractFollower.h"
#include "Player.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "Playerbot/Pvp/PlayerbotObcClone.h"
#include "Playerbot/Pvp/PlayerbotVhrWaveDriver.h"
#include "Playerbot/Pvp/PlayerbotPvpClassActions.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotPvpLifecycleActions.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "Playerbot/Pvp/PlayerbotResourceGovernor.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellHistory.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cctype>

using namespace Trinity::ChatCommands;

namespace
{
constexpr uint32 PLAYERBOT_REDUCED_MAGMA_DAMAGE_SPELL_ID = 57634;

bool IsChromieWhisperFacade(Player const* player)
{
    if (!player)
        return false;

    std::string const& name = player->GetName();
    return name == "Chromie" || name == "Chromi";
}

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
        case playerbot::PvpClassSpellContext::TargetMode::Pet: return "pet";
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

struct ManagedBotUpdatePulseState
{
    uint64 tickCount = 0;
    uint32 lastUpdateMs = 0;
    uint32 lastDiff = 0;
    uint32 lastProgressMs = 0;
    uint32 lastLogMs = 0;
    ObjectGuid motionTargetGuid = ObjectGuid::Empty;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastZ = 0.0f;
    float lastDistance = 0.0f;
    float lastProgressX = 0.0f;
    float lastProgressY = 0.0f;
    float lastProgressZ = 0.0f;
    MovementGeneratorType lastMotionType = IDLE_MOTION_TYPE;
    bool lastMoving = false;
    bool lastChaseMove = false;
    bool lastFollowMove = false;
    bool lastSplineInitialized = false;
    bool lastSplineFinalized = true;
    bool lastSplineStarted = false;
    int32 lastSplineIndex = 0;
    int32 lastSplineDuration = 0;
    float lastSplineVelocity = 0.0f;
};

std::unordered_map<uint64, ManagedBotUpdatePulseState> g_ManagedBotUpdatePulseByGuid;
std::mutex g_ManagedBotUpdatePulseLock;

Unit* GetCurrentMotionTarget(Player* bot)
{
    if (!bot)
        return nullptr;

    if (MotionMaster* motionMaster = bot->GetMotionMaster())
        if (MovementGenerator* movement = motionMaster->GetCurrentMovementGenerator())
            if (AbstractFollower* follower = dynamic_cast<AbstractFollower*>(movement))
                return follower->GetTarget();

    return nullptr;
}

void AppendSplineSnapshot(std::ostringstream& os, Player const* bot, char const* prefix)
{
    char const* pfx = prefix ? prefix : "spline";
    if (!bot || !bot->movespline)
    {
        os << ' ' << pfx << "_spline=null";
        return;
    }

    os << ' ' << pfx << "_spline_init=" << (bot->movespline->Initialized() ? "yes" : "no")
       << ' ' << pfx << "_spline_done=" << (bot->movespline->Finalized() ? "yes" : "no")
       << ' ' << pfx << "_spline_started=" << (bot->movespline->HasStarted() ? "yes" : "no")
       << ' ' << pfx << "_spline_idx=" << bot->movespline->currentPathIdx()
       << ' ' << pfx << "_spline_duration=" << bot->movespline->Duration()
       << ' ' << pfx << "_spline_velocity=" << bot->movespline->Velocity();
}

void RecordManagedBotUpdatePulse(Player* bot, uint32 diff)
{
    // Pure diagnostics feeding the stall log and the whisper diag command:
    // per-tick mutex + hash-map + spline bookkeeping for every player. Skip
    // it entirely unless someone is actually watching the motion debug log.
    if (!sLog->ShouldLog("playerbots.pvp.motion", LOG_LEVEL_DEBUG))
        return;

    if (!bot || !playerbot::IsManagedRandomBot(bot))
        return;

    std::lock_guard<std::mutex> pulseLock(g_ManagedBotUpdatePulseLock);

    uint32 const nowMs = GameTime::GetGameTimeMS();
    ManagedBotUpdatePulseState& state = g_ManagedBotUpdatePulseByGuid[bot->GetGUID().GetRawValue()];
    Unit* motionTarget = GetCurrentMotionTarget(bot);
    ObjectGuid const motionTargetGuid = motionTarget ? motionTarget->GetGUID() : ObjectGuid::Empty;
    float const currentDistance = motionTarget ? bot->GetDistance(motionTarget) : 0.0f;
    MovementGeneratorType const motionType = bot->GetMotionMaster() ? bot->GetMotionMaster()->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;

    bool const firstSample = state.tickCount == 0;
    bool const targetChanged = state.motionTargetGuid != motionTargetGuid;
    float const dx = bot->GetPositionX() - state.lastX;
    float const dy = bot->GetPositionY() - state.lastY;
    float const dz = bot->GetPositionZ() - state.lastZ;
    float const posDelta2D = std::sqrt(dx * dx + dy * dy);
    float const posDelta3D = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool const distanceImproved = motionTarget && state.lastDistance > 0.0f && currentDistance + 0.10f < state.lastDistance;
    bool const positionMoved = !firstSample && (posDelta2D >= 0.05f || posDelta3D >= 0.10f);

    if (firstSample || targetChanged || distanceImproved || positionMoved)
    {
        state.lastProgressMs = nowMs;
        state.lastProgressX = bot->GetPositionX();
        state.lastProgressY = bot->GetPositionY();
        state.lastProgressZ = bot->GetPositionZ();
    }

    state.tickCount++;
    state.lastUpdateMs = nowMs;
    state.lastDiff = diff;
    state.motionTargetGuid = motionTargetGuid;
    state.lastX = bot->GetPositionX();
    state.lastY = bot->GetPositionY();
    state.lastZ = bot->GetPositionZ();
    state.lastDistance = currentDistance;
    state.lastMotionType = motionType;
    state.lastMoving = bot->isMoving();
    state.lastChaseMove = bot->HasUnitState(UNIT_STATE_CHASE_MOVE);
    state.lastFollowMove = bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE);
    state.lastSplineInitialized = bot->movespline && bot->movespline->Initialized();
    state.lastSplineFinalized = !bot->movespline || bot->movespline->Finalized();
    state.lastSplineStarted = bot->movespline && bot->movespline->HasStarted();
    state.lastSplineIndex = bot->movespline ? bot->movespline->currentPathIdx() : -1;
    state.lastSplineDuration = bot->movespline ? bot->movespline->Duration() : 0;
    state.lastSplineVelocity = bot->movespline ? bot->movespline->Velocity() : 0.0f;

    bool const targetRelativeMotion = motionType == CHASE_MOTION_TYPE || motionType == FOLLOW_MOTION_TYPE;
    bool const claimsMovement = bot->isMoving() || bot->HasUnitState(UNIT_STATE_CHASE_MOVE) || bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE) || (bot->movespline && !bot->movespline->Finalized());
    uint32 const noProgressMs = state.lastProgressMs != 0 && nowMs >= state.lastProgressMs ? nowMs - state.lastProgressMs : 0;
    if (targetRelativeMotion && claimsMovement && noProgressMs >= 1000 && (state.lastLogMs == 0 || nowMs >= state.lastLogMs + 1000))
    {
        state.lastLogMs = nowMs;
        std::ostringstream diag;
        diag << "PB update pulse stalled: bot=" << bot->GetName()
             << " guid=" << bot->GetGUID().ToString()
             << " diff=" << diff
             << " ticks=" << state.tickCount
             << " no_progress_ms=" << noProgressMs
             << " motion=" << uint32(motionType)
             << " moving=" << (bot->isMoving() ? "yes" : "no")
             << " chase_move=" << (bot->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
             << " follow_move=" << (bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
             << " not_move=" << (bot->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
             << " root=" << (bot->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no")
             << " stunned=" << (bot->HasUnitState(UNIT_STATE_STUNNED) ? "yes" : "no")
             << " casting_prevent=" << (bot->IsMovementPreventedByCasting() ? "yes" : "no")
             << " pos=(" << bot->GetMapId() << ':' << bot->GetPositionX() << ',' << bot->GetPositionY() << ',' << bot->GetPositionZ() << ')';
        if (motionTarget)
            diag << " motion_target=" << motionTarget->GetName() << " motion_target_dist=" << currentDistance;
        else
            diag << " motion_target=none";
        AppendSplineSnapshot(diag, bot, "pulse");
        TC_LOG_DEBUG("playerbots.pvp.motion", "{}", diag.str());
    }
}

std::string BuildManagedBotUpdateDiagnosticLine(Player* bot)
{
    if (!bot)
        return "PB update diag unavailable.";

    uint32 const nowMs = GameTime::GetGameTimeMS();
    ManagedBotUpdatePulseState state;
    {
        std::lock_guard<std::mutex> pulseLock(g_ManagedBotUpdatePulseLock);
        auto itr = g_ManagedBotUpdatePulseByGuid.find(bot->GetGUID().GetRawValue());
        if (itr == g_ManagedBotUpdatePulseByGuid.end())
            return "PB update diag: no-update-pulse-recorded";

        state = itr->second;
    }
    uint32 const updateAgeMs = state.lastUpdateMs != 0 && nowMs >= state.lastUpdateMs ? nowMs - state.lastUpdateMs : 0;
    uint32 const progressAgeMs = state.lastProgressMs != 0 && nowMs >= state.lastProgressMs ? nowMs - state.lastProgressMs : 0;
    float const progressDx = bot->GetPositionX() - state.lastProgressX;
    float const progressDy = bot->GetPositionY() - state.lastProgressY;
    float const progressDz = bot->GetPositionZ() - state.lastProgressZ;
    float const progressDelta2D = std::sqrt(progressDx * progressDx + progressDy * progressDy);
    float const progressDelta3D = std::sqrt(progressDx * progressDx + progressDy * progressDy + progressDz * progressDz);

    std::ostringstream diag;
    diag << "PB update diag:"
         << " ticks=" << state.tickCount
         << " last_update_age_ms=" << updateAgeMs
         << " last_diff=" << state.lastDiff
         << " progress_age_ms=" << progressAgeMs
         << " progress_delta_2d=" << progressDelta2D
         << " progress_delta_3d=" << progressDelta3D
         << " last_motion=" << uint32(state.lastMotionType)
         << " last_moving=" << (state.lastMoving ? "yes" : "no")
         << " last_chase=" << (state.lastChaseMove ? "yes" : "no")
         << " last_follow=" << (state.lastFollowMove ? "yes" : "no")
         << " last_spline_init=" << (state.lastSplineInitialized ? "yes" : "no")
         << " last_spline_done=" << (state.lastSplineFinalized ? "yes" : "no")
         << " last_spline_started=" << (state.lastSplineStarted ? "yes" : "no")
         << " last_spline_idx=" << state.lastSplineIndex
         << " last_spline_duration=" << state.lastSplineDuration
         << " last_spline_velocity=" << state.lastSplineVelocity;

    Unit* motionTarget = GetCurrentMotionTarget(bot);
    if (motionTarget)
        diag << " motion_target=" << motionTarget->GetName() << " motion_target_dist=" << bot->GetDistance(motionTarget);
    else
        diag << " motion_target=none";

    AppendSplineSnapshot(diag, bot, "now");
    return diag.str();
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
           << " not_move=" << (bot->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
           << " chase_state=" << (bot->HasUnitState(UNIT_STATE_CHASE) ? "yes" : "no")
           << " chase_move=" << (bot->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
           << " follow_state=" << (bot->HasUnitState(UNIT_STATE_FOLLOW) ? "yes" : "no")
           << " follow_move=" << (bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
           << " casting_prevent=" << (bot->IsMovementPreventedByCasting() ? "yes" : "no")
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
        float const motionTargetExactDist = bot->GetExactDist(motionTarget);
        float const motionTargetHitboxSum = bot->GetCombatReach() + motionTarget->GetCombatReach();
        status << " motion_target=" << motionTarget->GetName()
               << " motion_target_dist=" << bot->GetDistance(motionTarget)
               << " motion_target_exact=" << motionTargetExactDist
               << " motion_target_hitbox_sum=" << motionTargetHitboxSum
               << " motion_target_edge_over_range=" << (bot->GetDistance(motionTarget) - classContext.movementFollowRange)
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
               << " directive_target_exact=" << bot->GetExactDist(directiveTarget)
               << " directive_target_hitbox_sum=" << (bot->GetCombatReach() + directiveTarget->GetCombatReach())
               << " directive_target_edge_over_range=" << (bot->GetDistance(directiveTarget) - classContext.movementFollowRange)
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
        playerbot::PlayerbotObcCloneManager::LoadConfig();
        playerbot::ResourceGovernor::LoadConfig();
        playerbot::PveManager::LoadConfig();
    }

    void OnStartup() override
    {
        playerbot::PvpCore::LoadConfig();
        playerbot::RandomBotParticipationManager::ResetCadence();
        playerbot::RandomBotParticipationManager::LoadPopulationConfig();
        playerbot::RandomBotParticipationManager::OnStartupBootstrap();
        playerbot::PlayerbotObcCloneManager::LoadConfig();
        playerbot::PlayerbotObcCloneManager::OnStartupSweep();
        playerbot::ResourceGovernor::LoadConfig();
        playerbot::PveManager::LoadConfig();
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
        playerbot::ResourceGovernor::NoteWorldUpdate(diff);
        playerbot::RandomBotParticipationManager::OnWorldUpdate(diff);
        playerbot::PlayerbotObcCloneManager::OnWorldUpdate(diff);
        playerbot::PlayerbotVhrWaveDriver::OnWorldUpdate(diff);
        playerbot::PveManager::OnWorldUpdate(diff);
    }

    void OnShutdown() override
    {
        playerbot::PlayerbotObcCloneManager::OnShutdown();
    }
};

class PlayerbotDamageUnitScript final : public UnitScript
{
public:
    PlayerbotDamageUnitScript() : UnitScript("PlayerbotDamageUnitScript") { }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* /*attacker*/, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!spellInfo || spellInfo->Id != PLAYERBOT_REDUCED_MAGMA_DAMAGE_SPELL_ID)
            return;

        Player* player = target ? target->ToPlayer() : nullptr;
        if (!playerbot::IsManagedRandomBot(player) && !playerbot::PlayerbotObcCloneManager::IsActiveClone(player))
            return;

        damage = 0;
    }
};

class PlayerbotLifecyclePlayerScript final : public PlayerScript
{
public:
    PlayerbotLifecyclePlayerScript() : PlayerScript("PlayerbotLifecyclePlayerScript") { }

    void OnUpdate(Player* player, uint32 diff) override
    {
        RecordManagedBotUpdatePulse(player, diff);
        playerbot::RandomBotParticipationManager::ProcessPlayerLifecycle(player);
    }

    void OnSpellCast(Player* player, Spell* spell, bool) override
    {
        if (!player || !spell || !spell->IsTriggered())
            return;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo || spellInfo->Id != 75)
            return;

        if (!playerbot::IsManagedRandomBot(player) && !playerbot::PlayerbotObcCloneManager::IsActiveClone(player))
            return;

        playerbot::NotifyHunterAutoShotFired(player);
    }

    void OnLogout(Player* player) override
    {
        playerbot::PveManager::OnBotLogout(player);
        playerbot::RandomBotParticipationManager::OnPlayerLogout(player);
        playerbot::PlayerbotObcCloneManager::OnPlayerLogout(player);
    }

    void OnLevelChanged(Player* player, uint8 oldLevel) override
    {
        playerbot::PveManager::OnManagedBotLevelChanged(player, oldLevel);
    }

    void OnPVPKill(Player* killer, Player* killed) override
    {
        playerbot::PlayerbotObcCloneManager::OnPvpKill(killer, killed);
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

        // The hard-coded Chromie whisper responder uses a virtual player so whispers
        // look like real player whispers. Do not treat her as a playerbot or reply
        // with diagnostics, otherwise bot-to-bot whisper hooks can recurse indefinitely.
        if (IsChromieWhisperFacade(sender) || IsChromieWhisperFacade(receiver))
            return;

        bool const senderIsPlayerbot = playerbot::IsManagedRandomBot(sender) ||
            playerbot::PlayerbotObcCloneManager::IsActiveClone(sender);
        bool const receiverIsPlayerbot = playerbot::IsManagedRandomBot(receiver) ||
            playerbot::PlayerbotObcCloneManager::IsActiveClone(receiver);

        // Diagnostic commands are human/GM -> bot only. A reply emitted by one
        // bot must never be interpreted as a fresh command by another bot, or
        // Whisper -> OnPlayerChat -> Whisper recursively re-enters forever.
        if (!receiverIsPlayerbot || senderIsPlayerbot || sender == receiver)
            return;

        std::string command = msg;
        command.erase(command.begin(), std::find_if(command.begin(), command.end(), [](unsigned char character)
        {
            return !std::isspace(character);
        }));
        command.erase(std::find_if(command.rbegin(), command.rend(), [](unsigned char character)
        {
            return !std::isspace(character);
        }).base(), command.end());
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

        // PvE companion orders ("follow", "stay", "attack", "passive", "come",
        // "dismiss") take precedence over the diagnostic dump below.
        if (playerbot::PveManager::HandleWhisperCommand(sender, receiver, command))
            return;

        if (command == "drop")
        {
            bool const wasFlagCarrier = playerbot::PvpCore::IsBattlegroundFlagCarrier(receiver);
            if (Battleground* battleground = receiver->GetBattleground())
                battleground->EventPlayerDroppedFlag(receiver);

            if (wasFlagCarrier)
                playerbot::BattlegroundTacticalActions::DelayFlagPickup(receiver, 5 * IN_MILLISECONDS);

            receiver->Whisper("Flag drop requested.", LANG_UNIVERSAL, sender);
            return;
        }

        receiver->Whisper(BuildManagedBotStatusLine(receiver), LANG_UNIVERSAL, sender);
        receiver->Whisper(std::string("PB move diag: ") + playerbot::PvpClassActions::GetLastMovementDebugStatus(receiver), LANG_UNIVERSAL, sender);
        receiver->Whisper(BuildManagedBotUpdateDiagnosticLine(receiver), LANG_UNIVERSAL, sender);

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
            { "movediag", HandlePlayerbotPvpMoveDiagCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
        };

        static ChatCommandTable playerbotRandomPopulationTable =
        {
            { "status", HandlePlayerbotPopulationStatusCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "start", HandlePlayerbotPopulationStartCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "stop", HandlePlayerbotPopulationStopCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "rebalance now", HandlePlayerbotPopulationRebalanceCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "list", HandlePlayerbotPopulationPoolCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };

        static ChatCommandTable playerbotPveTable =
        {
            { "summon", HandlePlayerbotPveSummonCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "dismiss", HandlePlayerbotPveDismissCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "status", HandlePlayerbotPveStatusCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "reset", HandlePlayerbotPveResetCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };

        static ChatCommandTable playerbotTable =
        {
            { "pvp", playerbotPvpTable },
            { "pve", playerbotPveTable },
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

    // Reports the last movement directive each nearby bot issued, plus the state
    // that drives it. Exists because the whisper diagnostic is unusable for
    // clones: they carry generated internal names and their display name lives
    // only in the character cache, so there is nothing to type. Reports on the
    // current selection when something is selected, otherwise sweeps the radius.
    static bool HandlePlayerbotPvpMoveDiagCommand(ChatHandler* handler, Optional<float> radiusArg)
    {
        if (!handler)
            return false;

        Player* player = handler->GetPlayer();
        if (!player || !player->FindMap())
            return false;

        float const radius = std::max(1.0f, radiusArg.value_or(80.0f));

        std::vector<Player*> bots;
        if (Player* selected = handler->getSelectedPlayer())
        {
            if (selected != player)
                bots.push_back(selected);
        }

        if (bots.empty())
        {
            Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
            for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            {
                Player* candidate = itr->GetSource();
                if (!candidate || candidate == player || !candidate->IsInWorld())
                    continue;
                if (!candidate->IsWithinDistInMap(player, radius))
                    continue;
                bots.push_back(candidate);
            }

            std::sort(bots.begin(), bots.end(), [player](Player const* left, Player const* right)
            {
                return player->GetDistance(left) < player->GetDistance(right);
            });
        }

        if (bots.empty())
        {
            handler->PSendSysMessage("No players found (nothing selected, none within %.0f yd).", radius);
            return true;
        }

        // Two lines each - cap the sweep so a full battleground does not flood
        // the chat frame and push the interesting rows off screen.
        constexpr size_t maxReported = 8;
        size_t const reported = std::min(bots.size(), maxReported);
        handler->PSendSysMessage("Playerbot move diagnostics (%u of %u within %.0f yd):",
            uint32(reported), uint32(bots.size()), radius);

        for (size_t index = 0; index < reported; ++index)
        {
            Player* bot = bots[index];

            std::string displayName;
            if (!sCharacterCache->GetCharacterNameByGuid(bot->GetGUID(), displayName) || displayName.empty())
                displayName = bot->GetName();

            MotionMaster const* motionMaster = bot->GetMotionMaster();
            Unit const* victim = bot->GetVictim();

            handler->PSendSysMessage("%s (%s) cls=%u dist=%.1f motion=%u moving=%s combat=%s rage=%u victim=%s@%.1f",
                displayName.c_str(), bot->GetName().c_str(), uint32(bot->GetClass()),
                player->GetDistance(bot),
                uint32(motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE),
                bot->isMoving() ? "yes" : "no",
                bot->IsInCombat() ? "yes" : "no",
                uint32(bot->GetPower(POWER_RAGE) / 10),
                victim ? victim->GetName().c_str() : "none",
                victim ? bot->GetDistance(victim) : 0.0f);

            std::string const moveDiag = playerbot::PvpClassActions::GetLastMovementDebugStatus(bot);
            handler->PSendSysMessage("   move: %s", moveDiag.empty() ? "(none recorded)" : moveDiag.c_str());

            // What the bot last actually tried to cast, and why it failed. The
            // movement line only shows the consequence; this shows the cause.
            std::string const execDiag = playerbot::PvpClassActions::GetLastExecutionStatus(bot);
            handler->PSendSysMessage("   exec: %s", execDiag.empty() ? "(none recorded)" : execDiag.c_str());

            // Sticky: only written when Every Man for Himself is actually
            // chosen, so it can be read long after the moment has passed.
            if (std::string const emfhDiag = playerbot::PvpCore::GetLastEveryManForHimselfDiagnostic(bot); !emfhDiag.empty())
                handler->PSendSysMessage("   emfh: %s", emfhDiag.c_str());

            // Warrior gap closers have a lot of independent gates (known rank,
            // cooldown, stance, rage, min/max range, combat) and a failure in
            // any one of them looks identical from outside: the bot just runs.
            // Clones are memory-only, so this is the only way to see which gate
            // is the one saying no.
            if (bot->GetClass() == CLASS_WARRIOR)
            {
                auto knownRank = [bot](std::initializer_list<uint32> ranks) -> uint32
                {
                    uint32 best = 0;
                    for (uint32 rank : ranks)
                        if (bot->HasSpell(rank))
                            best = rank;
                    return best;
                };
                auto readyText = [bot](uint32 spellId) -> char const*
                {
                    if (!spellId)
                        return "unknown";
                    return bot->GetSpellHistory()->HasCooldown(spellId) ? "cooldown" : "ready";
                };

                uint32 const chargeId = knownRank({ 100, 6178, 11578 });
                uint32 const interceptId = knownRank({ 20252, 20616, 20617 });
                uint32 const leapId = knownRank({ 81271 });
                uint32 const bloodrageId = knownRank({ 2687 });

                char const* stance = bot->HasAura(2457) ? "battle" :
                    (bot->HasAura(71) ? "defensive" : (bot->HasAura(2458) ? "berserker" : "none"));

                float const victimDist = victim ? bot->GetDistance(victim) : -1.0f;
                handler->PSendSysMessage("   warrior: charge=%u/%s intercept=%u/%s leap=%u/%s bloodrage=%u/%s stance=%s rage=%u combat=%s victim_dist=%.1f band8-25=%s",
                    chargeId, readyText(chargeId),
                    interceptId, readyText(interceptId),
                    leapId, readyText(leapId),
                    bloodrageId, readyText(bloodrageId),
                    stance,
                    uint32(bot->GetPower(POWER_RAGE) / 10),
                    bot->IsInCombat() ? "yes" : "no",
                    victimDist,
                    (victimDist >= 8.0f && victimDist <= 25.0f) ? "yes" : "no");

                std::string const gapDiag = playerbot::PvpCore::GetLastWarriorGapCloserDiagnostic(bot);
                handler->PSendSysMessage("   gapclose: %s", gapDiag.empty() ? "(warrior selector has not run)" : gapDiag.c_str());
            }
        }

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

    // .playerbot pve reset [percent] - rebirth that share of the online
    // managed bots as fresh level-1 characters at their racial start.
    static bool HandlePlayerbotPveResetCommand(ChatHandler* handler, Optional<uint8> percent)
    {
        if (!handler)
            return false;

        uint32 const resetCount = playerbot::PveManager::ResetBotsToLevelOne(percent.value_or(100));
        handler->PSendSysMessage("Reset %u managed playerbots to level 1 and sent them home.", resetCount);
        return true;
    }

    static bool HandlePlayerbotPveSummonCommand(ChatHandler* handler, std::string characterName)
    {
        if (!handler || !handler->GetSession())
            return false;

        Player* summoner = handler->GetPlayer();
        if (!summoner)
            return false;

        std::string statusMessage;
        bool const accepted = playerbot::PveManager::RequestCompanionSummon(summoner, characterName, statusMessage);
        handler->PSendSysMessage("%s", statusMessage.c_str());
        return accepted;
    }

    static bool HandlePlayerbotPveDismissCommand(ChatHandler* handler, Optional<std::string> characterName)
    {
        if (!handler || !handler->GetSession())
            return false;

        Player* bot = nullptr;
        if (characterName)
            bot = ObjectAccessor::FindPlayerByName(*characterName);
        else
            bot = handler->getSelectedPlayer();

        if (!bot)
        {
            handler->PSendSysMessage("Select an online playerbot or provide its name.");
            return false;
        }

        std::string statusMessage;
        bool const dismissed = playerbot::PveManager::RequestCompanionDismiss(handler->GetPlayer(), bot, statusMessage);
        handler->PSendSysMessage("%s", statusMessage.c_str());
        return dismissed;
    }

    static bool HandlePlayerbotPveStatusCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        Player* bot = handler->getSelectedPlayer();
        handler->PSendSysMessage("%s", playerbot::PveManager::BuildStatusLine(bot).c_str());
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
    new PlayerbotDamageUnitScript();
    new PlayerbotLifecyclePlayerScript();
    new PlayerbotLifecycleCommandScript();
}
