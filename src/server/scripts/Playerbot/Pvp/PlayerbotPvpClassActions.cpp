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

#include "PlayerbotPvpClassActions.h"

#include "GameTime.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "Log.h"
#include "Map.h"
#include "MovementDefines.h"
#include "MoveSpline.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Battleground.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Position.h"
#include "Protocol/Opcodes.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Unit.h"
#include "WorldSession.h"

#include <array>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace
{
bool IsLifeTapSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && firstRank->Id == 1454; // Life Tap (rank 1)
}

char const* GetTargetModeLabel(playerbot::PvpClassSpellContext::TargetMode mode);
bool CanIssueFollowCommands(Player const* player);
bool IsEffectivelyOutdoors(Player const* player);
bool IsStrictlyOutdoorsForMount(Player const* player);
bool IsPrimaryMeleeClassForSpacing(uint8 classId);
bool IsFriendlySupportTarget(Player const* player, Unit const* target, SpellInfo const* spellInfo);
void SetLastExecutionStatus(Player const* player, std::string const& status);
void SetLastMovementDebugStatus(Player const* player, std::string const& status);
void RecordTargetRelativeMovementOrder(Player const* player, Unit const* target, float issuedRange, uint8 mode);
bool ShouldPreserveTargetRelativeMovement(Player const* player, Unit const* target, float desiredRange, uint32 minRunMs, char const* label, std::string* reasonOut);
void AppendMotionSnapshotDiag(std::ostringstream& diag, Player const* player, char const* prefix);

struct LastLosCastFailureState
{
    ObjectGuid targetGuid = ObjectGuid::Empty;
    uint32 spellId = 0;
    uint32 failureMs = 0;
    float botX = 0.0f;
    float botY = 0.0f;
    float botZ = 0.0f;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
    float edgeDistance = 0.0f;
    float exactDistance = 0.0f;
    float requestedRecoveryRange = 0.0f;
};

std::unordered_map<uint64, LastLosCastFailureState> g_LastLosCastFailureByGuid;

bool IsEffectivelyOutdoors(Player const* player)
{
    if (!player)
        return false;

    Map const* map = player->GetMap();
    if (!map)
        return player->IsOutdoors();

    PositionFullTerrainStatus terrainStatus;
    map->GetFullTerrainStatusForPosition(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        terrainStatus, MAP_ALL_LIQUIDS, player->GetCollisionHeight());
    return player->IsOutdoors() || terrainStatus.outdoors;
}

bool IsStrictlyOutdoorsForMount(Player const* player)
{
    if (!player)
        return false;

    Map const* map = player->GetMap();
    if (!map)
        return player->IsOutdoors();

    PositionFullTerrainStatus terrainStatus;
    map->GetFullTerrainStatusForPosition(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        terrainStatus, MAP_ALL_LIQUIDS, player->GetCollisionHeight());
    return player->IsOutdoors() && terrainStatus.outdoors;
}

bool IsPrimaryMeleeClassForSpacing(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_DEATH_KNIGHT:
            return true;
        default:
            return false;
    }
}

bool RequiresStrictHumanPathing(Player const* player)
{
    return player && player->InBattleground();
}

Position BuildCollisionSafeDestination(Player* player, Position const& destination)
{
    if (!player)
        return destination;

    Position adjustedDestination = destination;
    float adjustedZ = adjustedDestination.GetPositionZ();
    player->UpdateAllowedPositionZ(adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(), adjustedZ);
    adjustedDestination.Relocate(adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(), adjustedZ, adjustedDestination.GetOrientation());
    return adjustedDestination;
}

Position BuildFollowDestination(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target)
        return Position();

    float x = target->GetPositionX();
    float y = target->GetPositionY();
    float z = target->GetPositionZ();
    float const followDistance = std::max(0.5f, desiredDistance);

    target->GetNearPoint(player, x, y, z, followDistance, target->GetAbsoluteAngle(player));
    Position destination(x, y, z, player->GetOrientation());
    return BuildCollisionSafeDestination(player, destination);
}

bool TryBuildStrictHumanSegmentDestination(Player* player, Position const& desiredDestination, Position& segmentDestination)
{
    if (!player)
        return false;

    auto const tryResolveDestination = [&](Position const& requestedDestination, Position& resolvedDestination) -> bool
    {
        Position const safeDestination = BuildCollisionSafeDestination(player, requestedDestination);

        PathGenerator path(player);
        path.SetPathLengthLimit(90.0f);
        bool pathOk = path.CalculatePath(safeDestination.GetPositionX(), safeDestination.GetPositionY(), safeDestination.GetPositionZ(), true);
        PathType pathType = path.GetPathType();
        Movement::PointsArray points = path.GetPath();
        G3D::Vector3 actualEnd = path.GetActualEndPosition();

        if ((pathType & PATHFIND_SHORTCUT) != 0)
        {
            PathGenerator retryPath(player);
            retryPath.SetPathLengthLimit(90.0f);
            bool const retryOk = retryPath.CalculatePath(safeDestination.GetPositionX(), safeDestination.GetPositionY(), safeDestination.GetPositionZ(), false);
            PathType const retryType = retryPath.GetPathType();
            if (retryOk && (retryType & PATHFIND_SHORTCUT) == 0)
            {
                points = retryPath.GetPath();
                pathType = retryType;
                pathOk = true;
                actualEnd = retryPath.GetActualEndPosition();
            }
        }

        uint32 const forbiddenPathFlags = PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH;
        if (!pathOk || (pathType & forbiddenPathFlags) != 0)
            return false;

        bool haveResolvedDestination = false;
        if (points.size() > 1)
        {
            G3D::Vector3 const& lastPoint = points.back();
            resolvedDestination.Relocate(lastPoint.x, lastPoint.y, lastPoint.z, safeDestination.GetOrientation());
            haveResolvedDestination = true;
        }
        else
        {
            Position actualEndDestination(actualEnd.x, actualEnd.y, actualEnd.z, safeDestination.GetOrientation());
            float const destinationDistance = player->GetDistance(safeDestination);
            float const actualEndDistance = player->GetDistance(actualEndDestination);
            if (actualEndDistance > 1.5f && actualEndDistance + 1.0f < destinationDistance)
            {
                resolvedDestination = actualEndDestination;
                haveResolvedDestination = true;
            }
        }

        if (!haveResolvedDestination)
            return false;

        resolvedDestination = BuildCollisionSafeDestination(player, resolvedDestination);
        float const dx = resolvedDestination.GetPositionX() - player->GetPositionX();
        float const dy = resolvedDestination.GetPositionY() - player->GetPositionY();
        float const planarDelta = std::sqrt(dx * dx + dy * dy);
        float const verticalDelta = std::fabs(resolvedDestination.GetPositionZ() - player->GetPositionZ());
        if (planarDelta < 0.5f || verticalDelta > std::max(8.0f, planarDelta * 0.75f + 2.0f))
            return false;

        return true;
    };

    if (tryResolveDestination(desiredDestination, segmentDestination))
        return true;

    float const dx = desiredDestination.GetPositionX() - player->GetPositionX();
    float const dy = desiredDestination.GetPositionY() - player->GetPositionY();
    float const dz = desiredDestination.GetPositionZ() - player->GetPositionZ();
    float const planarDistance = std::sqrt(dx * dx + dy * dy);
    if (planarDistance < 1.0f)
        return false;

    std::array<float, 6> const probeDistances =
    {
        24.0f,
        18.0f,
        12.0f,
        8.0f,
        5.0f,
        3.0f
    };

    for (float probeDistance : probeDistances)
    {
        float const cappedDistance = std::min(planarDistance - 0.25f, probeDistance);
        if (cappedDistance <= 0.5f)
            continue;

        float const fraction = cappedDistance / planarDistance;
        Position probeDestination(
            player->GetPositionX() + dx * fraction,
            player->GetPositionY() + dy * fraction,
            player->GetPositionZ() + dz * fraction,
            desiredDestination.GetOrientation());

        if (tryResolveDestination(probeDestination, segmentDestination))
            return true;
    }

    return false;
}

bool IssueStrictHumanMove(Player* player, Position const& destination, float destinationChangeThreshold = 4.0f, uint32 minReissueMs = 350)
{
    if (!player || !player->IsAlive())
        return false;

    if (!CanIssueFollowCommands(player))
        return false;

    struct MoveOrderState
    {
        Position lastDestination;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, MoveOrderState> stateByGuid;
    uint64 const botGuid = player->GetGUID().GetRawValue();
    MoveOrderState& state = stateByGuid[botGuid];
    uint32 const nowMs = GameTime::GetGameTimeMS();

    bool const destinationChanged = state.lastIssueMs == 0 ||
        state.lastDestination.GetExactDist(destination) >= destinationChangeThreshold;
    bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + minReissueMs;

    if (!destinationChanged && !canReissueByTime)
    {
        // Treat strict move as unsuccessful when the throttled order is stale
        // and we are not currently moving; callers can then fall back to
        // alternate movement instead of assuming progress.
        return player->isMoving();
    }

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return false;

    if (!destinationChanged && !canReissueByTime)
    {
        // Do not suppress strict re-issue while stalled. Battleground pathing
        // can occasionally leave a stale/idle generator active, which causes
        // repeated "reach spell" directives to report success while the bot
        // remains stationary.
        MovementGeneratorType const movementType = motionMaster->GetCurrentMovementGeneratorType();
        bool const hasActivePointMove = player->isMoving() && movementType == POINT_MOTION_TYPE;
        if (hasActivePointMove)
            return true;
    }

    Position segmentDestination;
    if (!TryBuildStrictHumanSegmentDestination(player, destination, segmentDestination))
        return false;

    motionMaster->Clear(MOTION_SLOT_ACTIVE);
    motionMaster->MovePoint(0, segmentDestination, true);

    state.lastDestination = segmentDestination;
    state.lastIssueMs = nowMs;
    return true;
}

bool IssueStrictHumanFollow(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target)
        return false;

    return IssueStrictHumanMove(player, BuildFollowDestination(player, target, desiredDistance));
}

bool PrepareMotionMasterForExplicitBotMovement(Player* player)
{
    if (!player || !player->IsInWorld())
        return false;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return false;

    bool const lookedPending = motionMaster->Empty() ||
        (motionMaster->GetCurrentMovementGenerator() &&
            (motionMaster->GetCurrentMovementGenerator()->HasFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING) ||
             motionMaster->GetCurrentMovementGenerator()->HasFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED)));

    // Playerbots can occasionally enter battlegrounds while their MotionMaster
    // is still carrying MOTIONMASTER_FLAG_INITIALIZATION_PENDING. When that
    // happens, MotionMaster::Add(...) delays new Chase/Follow generators and
    // MotionMaster::Update(...) returns before initializing the queued top
    // generator. The visible symptom is: motion=chase/follow,
    // prime_top_init_before=yes, prime_update=yes, but moving_after=no and
    // CHASE_MOVE/FOLLOW_MOVE never becomes true.
    //
    // AddToWorld() safely no-ops unless the MotionMaster is actually pending.
    // It must be called BEFORE MoveChase/MoveFollow; calling it afterward would
    // reset motion and throw away the newly queued active generator.
    motionMaster->AddToWorld();
    return lookedPending;
}

bool IssueThrottledFollowMovement(Player* player, Unit* target, float desiredDistance, uint32 minReissueMs = 250, float rangeChangeThreshold = 0.2f)
{
    if (!player || !target || !target->IsAlive())
        return false;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return false;

    struct FollowOrderState
    {
        ObjectGuid targetGuid = ObjectGuid::Empty;
        float range = 0.0f;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, FollowOrderState> stateByGuid;
    FollowOrderState& state = stateByGuid[player->GetGUID().GetRawValue()];
    // Follow distance should allow true melee contact for stealth openers.
    // Clamping to >= 1.0f can leave bots hovering outside melee reach
    // depending on hitbox combinations.
    float const safeDistance = std::max(0.1f, desiredDistance);
    uint32 const nowMs = GameTime::GetGameTimeMS();

    bool const targetChanged = state.targetGuid != target->GetGUID();
    bool const rangeChanged = std::fabs(state.range - safeDistance) >= rangeChangeThreshold;
    bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + minReissueMs;
    if (!targetChanged && !rangeChanged && !canReissueByTime &&
        motionMaster->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        return true;
    }

    std::string preserveDiag;
    if (ShouldPreserveTargetRelativeMovement(player, target, safeDistance, 1800, "follow_order_preserved", &preserveDiag))
    {
        SetLastMovementDebugStatus(player, preserveDiag);
        state.targetGuid = target->GetGUID();
        state.range = safeDistance;
        return true;
    }

    bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
    motionMaster->MoveFollow(target, safeDistance, player->GetFollowAngle());
    (void)preparedMotionMaster;
    RecordTargetRelativeMovementOrder(player, target, safeDistance, 2);
    state.targetGuid = target->GetGUID();
    state.range = safeDistance;
    state.lastIssueMs = nowMs;
    return true;
}

ChaseRange BuildEdgeDistanceChaseRange(float desiredEdgeDistance, float toleranceBackoff = 1.0f)
{
    // MotionMaster::MoveChase(float) wraps ChaseRange(float), which adds
    // CONTACT_DISTANCE to MaxRange. For spell range movement that makes
    // 27y behave like roughly 27.5y + hitboxes inside ChaseMovementGenerator.
    // Use the explicit constructor so the max range means "edge distance".
    float const maxEdge = std::max(0.5f, desiredEdgeDistance);
    float const maxToleranceEdge = std::max(0.5f, maxEdge - std::max(0.0f, toleranceBackoff));
    return ChaseRange(0.0f, 0.0f, maxToleranceEdge, maxEdge);
}

struct TargetRelativeMoveOrderState
{
    ObjectGuid targetGuid = ObjectGuid::Empty;
    float issuedRange = 0.0f;
    float lastDistance = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastZ = 0.0f;
    uint32 lastIssueMs = 0;
    uint32 lastProgressMs = 0;
    uint32 lastPositionProgressMs = 0;
    uint8 mode = 0; // 1=chase, 2=follow
};

std::unordered_map<uint64, TargetRelativeMoveOrderState> g_TargetRelativeMoveOrderByGuid;

void RecordTargetRelativeMovementOrder(Player const* player, Unit const* target, float issuedRange, uint8 mode)
{
    if (!player || !target)
        return;

    TargetRelativeMoveOrderState& state = g_TargetRelativeMoveOrderByGuid[player->GetGUID().GetRawValue()];
    state.targetGuid = target->GetGUID();
    state.issuedRange = issuedRange;
    state.lastDistance = player->GetDistance(target);
    state.lastX = player->GetPositionX();
    state.lastY = player->GetPositionY();
    state.lastZ = player->GetPositionZ();
    state.lastIssueMs = GameTime::GetGameTimeMS();
    // Do not count the act of issuing MoveChase/MoveFollow as movement progress.
    // Progress timestamps should only update after actual distance/position
    // improvement, otherwise the preserve logic keeps dead generators alive for
    // several seconds with moving=no.
    state.lastProgressMs = 0;
    state.lastPositionProgressMs = 0;
    state.mode = mode;
}

void AppendMotionSnapshotDiag(std::ostringstream& diag, Player const* player, char const* prefix)
{
    char const* pfx = prefix ? prefix : "motion";
    if (!player)
    {
        diag << ' ' << pfx << "_snapshot=player_null";
        return;
    }

    MotionMaster const* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
    {
        diag << ' ' << pfx << "_snapshot=mm_null";
        return;
    }

    MovementGeneratorType const motionType = motionMaster->GetCurrentMovementGeneratorType();
    MovementGenerator const* top = motionMaster->GetCurrentMovementGenerator();

    diag << ' ' << pfx << "_mm_size=" << motionMaster->Size()
         << ' ' << pfx << "_slot=" << uint32(motionMaster->GetCurrentSlot())
         << ' ' << pfx << "_type=" << uint32(motionType)
         << ' ' << pfx << "_top=" << (top ? "yes" : "no");

    if (top)
    {
        diag << ' ' << pfx << "_top_type=" << uint32(top->GetMovementGeneratorType())
             << ' ' << pfx << "_top_flags=" << uint32(top->Flags)
             << ' ' << pfx << "_top_init=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING) ? "yes" : "no")
             << ' ' << pfx << "_top_inited=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_INITIALIZED) ? "yes" : "no")
             << ' ' << pfx << "_top_speed=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_SPEED_UPDATE_PENDING) ? "yes" : "no")
             << ' ' << pfx << "_top_interrupt=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_INTERRUPTED) ? "yes" : "no")
             << ' ' << pfx << "_top_paused=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_PAUSED) ? "yes" : "no")
             << ' ' << pfx << "_top_timed_paused=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_TIMED_PAUSED) ? "yes" : "no")
             << ' ' << pfx << "_top_deact=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED) ? "yes" : "no")
             << ' ' << pfx << "_top_final=" << (top->HasFlag(MOVEMENTGENERATOR_FLAG_FINALIZED) ? "yes" : "no")
             << ' ' << pfx << "_top_base_state=" << top->BaseUnitState;
    }

    diag << ' ' << pfx << "_unit_moving=" << (player->isMoving() ? "yes" : "no")
         << ' ' << pfx << "_unit_chase=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
         << ' ' << pfx << "_unit_follow=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
         << ' ' << pfx << "_unit_notmove=" << (player->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
         << ' ' << pfx << "_unit_root=" << (player->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no")
         << ' ' << pfx << "_unit_stunned=" << (player->HasUnitState(UNIT_STATE_STUNNED) ? "yes" : "no")
         << ' ' << pfx << "_casting_prevent=" << (player->IsMovementPreventedByCasting() ? "yes" : "no");

    if (player->movespline)
    {
        diag << ' ' << pfx << "_spline_init=" << (player->movespline->Initialized() ? "yes" : "no")
             << ' ' << pfx << "_spline_done=" << (player->movespline->Finalized() ? "yes" : "no")
             << ' ' << pfx << "_spline_started=" << (player->movespline->HasStarted() ? "yes" : "no")
             << ' ' << pfx << "_spline_falling=" << (player->movespline->isFalling() ? "yes" : "no")
             << ' ' << pfx << "_spline_cyclic=" << (player->movespline->isCyclic() ? "yes" : "no")
             << ' ' << pfx << "_spline_idx=" << player->movespline->currentPathIdx()
             << ' ' << pfx << "_spline_duration=" << player->movespline->Duration()
             << ' ' << pfx << "_spline_time=" << player->movespline->timePassed()
             << ' ' << pfx << "_spline_velocity=" << player->movespline->Velocity();
    }
    else
        diag << ' ' << pfx << "_spline=null";
}

bool ShouldPreserveTargetRelativeMovement(Player const* player, Unit const* target, float desiredRange, uint32 minRunMs,
    char const* label = nullptr, std::string* reasonOut = nullptr)
{
    if (!player || !target)
        return false;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return false;

    MovementGeneratorType const motionType = motionMaster->GetCurrentMovementGeneratorType();
    bool const activeTargetRelativeMotion = motionType == CHASE_MOTION_TYPE || motionType == FOLLOW_MOTION_TYPE;
    if (!activeTargetRelativeMotion)
        return false;

    TargetRelativeMoveOrderState& state = g_TargetRelativeMoveOrderByGuid[player->GetGUID().GetRawValue()];
    bool const sameTarget = state.targetGuid == target->GetGUID();
    if (!sameTarget)
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const ageMs = state.lastIssueMs != 0 && nowMs >= state.lastIssueMs ? nowMs - state.lastIssueMs : 0;
    float const currentDistance = player->GetDistance(target);

    float const dx = player->GetPositionX() - state.lastX;
    float const dy = player->GetPositionY() - state.lastY;
    float const dz = player->GetPositionZ() - state.lastZ;
    float const positionDelta2D = std::sqrt(dx * dx + dy * dy);
    float const positionDelta3D = std::sqrt(dx * dx + dy * dy + dz * dz);

    bool const madeDistanceProgress = state.lastDistance > 0.0f && currentDistance + 0.20f < state.lastDistance;
    bool const madePositionProgress = positionDelta2D >= 0.35f || positionDelta3D >= 0.50f;

    if (madeDistanceProgress)
    {
        state.lastDistance = currentDistance;
        state.lastProgressMs = nowMs;
    }

    if (madePositionProgress)
    {
        state.lastX = player->GetPositionX();
        state.lastY = player->GetPositionY();
        state.lastZ = player->GetPositionZ();
        state.lastPositionProgressMs = nowMs;
    }

    uint32 const distanceProgressAgeMs = state.lastProgressMs != 0 && nowMs >= state.lastProgressMs ? nowMs - state.lastProgressMs : 0;
    uint32 const positionProgressAgeMs = state.lastPositionProgressMs != 0 && nowMs >= state.lastPositionProgressMs ? nowMs - state.lastPositionProgressMs : 0;

    MovementGenerator const* top = motionMaster->GetCurrentMovementGenerator();
    bool const topInitPending = top && top->HasFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING);
    bool const topDeactivated = top && top->HasFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    bool const generatorStarted = player->isMoving() ||
        player->HasUnitState(UNIT_STATE_CHASE_MOVE) ||
        player->HasUnitState(UNIT_STATE_FOLLOW_MOVE);
    bool const movementBlocked = player->HasUnitState(UNIT_STATE_NOT_MOVE) ||
        player->HasUnitState(UNIT_STATE_ROOT) ||
        player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->IsMovementPreventedByCasting();
    bool const unlaunchedTargetMotion = !generatorStarted && !topInitPending && !topDeactivated && !movementBlocked;

    // Do not preserve CHASE/FOLLOW for multiple seconds if the generator never
    // actually launches. That was the visible stuck state:
    // motion=chase/follow, moving=no, chase_move=no, follow_move=no,
    // reason=settle_window.
    uint32 constexpr UnlaunchedMotionGraceMs = 650;
    uint32 constexpr StartedButUnprovenMotionGraceMs = 1200;
    bool const inSettleWindow = ageMs < minRunMs;
    bool const inUnlaunchedGraceWindow = ageMs < UnlaunchedMotionGraceMs;
    bool const inStartedGraceWindow = ageMs < StartedButUnprovenMotionGraceMs;
    bool const preserveSettle = inSettleWindow && (
        (unlaunchedTargetMotion && inUnlaunchedGraceWindow) ||
        (generatorStarted && inStartedGraceWindow) ||
        topInitPending ||
        movementBlocked);
    bool const recentDistanceProgress = state.lastProgressMs != 0 && distanceProgressAgeMs < minRunMs;
    bool const recentPositionProgress = state.lastPositionProgressMs != 0 && positionProgressAgeMs < minRunMs;
    bool const preserve = preserveSettle || madeDistanceProgress || madePositionProgress || recentDistanceProgress || recentPositionProgress;

    if (!preserve)
    {
        if (reasonOut)
        {
            std::ostringstream diag;
            diag << (label ? label : "target_relative_motion_not_preserved")
                 << " motion=" << uint32(motionType)
                 << " mode=" << uint32(state.mode)
                 << " age_ms=" << ageMs
                 << " distance_progress_age_ms=" << distanceProgressAgeMs
                 << " position_progress_age_ms=" << positionProgressAgeMs
                 << " desired_range=" << desiredRange
                 << " issued_range=" << state.issuedRange
                 << " dist=" << currentDistance
                 << " last_dist=" << state.lastDistance
                 << " pos_delta_2d=" << positionDelta2D
                 << " pos_delta_3d=" << positionDelta3D
                 << " moving=" << (player->isMoving() ? "yes" : "no")
                 << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
                 << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
                 << " not_move=" << (player->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
                 << " root=" << (player->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no")
                 << " stunned=" << (player->HasUnitState(UNIT_STATE_STUNNED) ? "yes" : "no")
                 << " casting_prevent=" << (player->IsMovementPreventedByCasting() ? "yes" : "no")
                 << " top_init=" << (topInitPending ? "yes" : "no")
                 << " top_deact=" << (topDeactivated ? "yes" : "no")
                 << " generator_started=" << (generatorStarted ? "yes" : "no")
                 << " movement_blocked=" << (movementBlocked ? "yes" : "no")
                 << " unlaunched=" << (unlaunchedTargetMotion ? "yes" : "no")
                 << " unlaunched_grace=" << (inUnlaunchedGraceWindow ? "yes" : "no")
                 << " started_grace=" << (inStartedGraceWindow ? "yes" : "no")
                 << " reason=" << (unlaunchedTargetMotion ? "unlaunched_settle_expired" : "no_position_or_distance_progress");
            AppendMotionSnapshotDiag(diag, player, "preserve_fail");
            *reasonOut = diag.str();
        }
        return false;
    }

    if (reasonOut)
    {
        std::ostringstream diag;
        diag << (label ? label : "target_relative_motion_preserved")
             << " motion=" << uint32(motionType)
             << " mode=" << uint32(state.mode)
             << " age_ms=" << ageMs
             << " distance_progress_age_ms=" << distanceProgressAgeMs
             << " position_progress_age_ms=" << positionProgressAgeMs
             << " desired_range=" << desiredRange
             << " issued_range=" << state.issuedRange
             << " dist=" << currentDistance
             << " last_dist=" << state.lastDistance
             << " pos_delta_2d=" << positionDelta2D
             << " pos_delta_3d=" << positionDelta3D
             << " moving=" << (player->isMoving() ? "yes" : "no")
             << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
             << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
             << " not_move=" << (player->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
             << " root=" << (player->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no")
             << " stunned=" << (player->HasUnitState(UNIT_STATE_STUNNED) ? "yes" : "no")
             << " casting_prevent=" << (player->IsMovementPreventedByCasting() ? "yes" : "no")
             << " top_init=" << (topInitPending ? "yes" : "no")
             << " top_deact=" << (topDeactivated ? "yes" : "no")
             << " generator_started=" << (generatorStarted ? "yes" : "no")
             << " movement_blocked=" << (movementBlocked ? "yes" : "no")
             << " unlaunched=" << (unlaunchedTargetMotion ? "yes" : "no")
             << " unlaunched_grace=" << (inUnlaunchedGraceWindow ? "yes" : "no")
             << " started_grace=" << (inStartedGraceWindow ? "yes" : "no")
             << " reason=" << (preserveSettle ? "settle_window" : (madePositionProgress || recentPositionProgress ? "position_progress" : "distance_progress"));
        AppendMotionSnapshotDiag(diag, player, "preserve_ok");
        *reasonOut = diag.str();
    }

    return true;
}

enum class TargetRelativeRangedMoveResult : uint8
{
    None,
    ChaseIssued,
    FollowIssued
};

char const* GetTargetRelativeRangedMoveResultLabel(TargetRelativeRangedMoveResult result)
{
    switch (result)
    {
        case TargetRelativeRangedMoveResult::ChaseIssued:
            return "chase";
        case TargetRelativeRangedMoveResult::FollowIssued:
            return "follow";
        case TargetRelativeRangedMoveResult::None:
        default:
            return "none";
    }
}

struct MotionPrimeResult
{
    bool attempted = false;
    bool addToWorldCalled = false;
    bool updateCalled = false;
    bool skippedBecauseUpdating = false;
    bool skippedBecauseInitPending = false;
    bool mmInitPendingBefore = false;
    bool mmUpdatingBefore = false;
    bool topInitPendingBefore = false;
    bool topDeactivatedBefore = false;
    bool topInitPendingAfter = false;
    bool topDeactivatedAfter = false;
    uint32 mmSizeBefore = 0;
    uint32 mmSizeAfter = 0;
    MovementGeneratorType motionBefore = IDLE_MOTION_TYPE;
    MovementGeneratorType motionAfter = IDLE_MOTION_TYPE;
    bool movingBefore = false;
    bool movingAfter = false;
    bool chaseMoveBefore = false;
    bool chaseMoveAfter = false;
    bool followMoveBefore = false;
    bool followMoveAfter = false;
};

MotionPrimeResult PrimeTargetRelativeMotion(Player* player)
{
    MotionPrimeResult result;
    if (!player)
        return result;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return result;

    result.attempted = true;
    // MotionMaster::HasFlag(...) is private in this TrinityCore branch, so do not
    // introspect MOTIONMASTER_FLAG_* here. We still log the public current motion
    // and the top MovementGenerator flags, then prime one motion tick.
    result.mmSizeBefore = motionMaster->Size();
    result.motionBefore = motionMaster->GetCurrentMovementGeneratorType();
    result.movingBefore = player->isMoving();
    result.chaseMoveBefore = player->HasUnitState(UNIT_STATE_CHASE_MOVE);
    result.followMoveBefore = player->HasUnitState(UNIT_STATE_FOLLOW_MOVE);

    if (MovementGenerator* top = motionMaster->GetCurrentMovementGenerator())
    {
        result.topInitPendingBefore = top->HasFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING);
        result.topDeactivatedBefore = top->HasFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    }

    // Force Initialize()+first Update() for freshly queued Chase/Follow.
    // Without this, virtual-session playerbots can show motion=chase/follow
    // for >1s while CHASE_MOVE/FOLLOW_MOVE never gets set.
    motionMaster->Update(1);
    result.updateCalled = true;

    result.mmSizeAfter = motionMaster->Size();
    result.motionAfter = motionMaster->GetCurrentMovementGeneratorType();
    if (MovementGenerator* top = motionMaster->GetCurrentMovementGenerator())
    {
        result.topInitPendingAfter = top->HasFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING);
        result.topDeactivatedAfter = top->HasFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    }
    result.movingAfter = player->isMoving();
    result.chaseMoveAfter = player->HasUnitState(UNIT_STATE_CHASE_MOVE);
    result.followMoveAfter = player->HasUnitState(UNIT_STATE_FOLLOW_MOVE);
    return result;
}

void AppendMotionPrimeDiag(std::ostringstream& diag, MotionPrimeResult const& prime)
{
    diag << " prime_attempted=" << (prime.attempted ? "yes" : "no")
         << " prime_add_world=" << (prime.addToWorldCalled ? "yes" : "no")
         << " prime_update=" << (prime.updateCalled ? "yes" : "no")
         << " prime_skip_update=" << (prime.skippedBecauseUpdating ? "yes" : "no")
         << " prime_skip_init=" << (prime.skippedBecauseInitPending ? "yes" : "no")
         << " prime_mm_init_before=" << (prime.mmInitPendingBefore ? "yes" : "no")
         << " prime_mm_updating_before=" << (prime.mmUpdatingBefore ? "yes" : "no")
         << " prime_top_init_before=" << (prime.topInitPendingBefore ? "yes" : "no")
         << " prime_top_deact_before=" << (prime.topDeactivatedBefore ? "yes" : "no")
         << " prime_top_init_after=" << (prime.topInitPendingAfter ? "yes" : "no")
         << " prime_top_deact_after=" << (prime.topDeactivatedAfter ? "yes" : "no")
         << " prime_mm_size_before=" << prime.mmSizeBefore
         << " prime_mm_size_after=" << prime.mmSizeAfter
         << " prime_motion_before=" << uint32(prime.motionBefore)
         << " prime_motion_after=" << uint32(prime.motionAfter)
         << " prime_moving_before=" << (prime.movingBefore ? "yes" : "no")
         << " prime_moving_after=" << (prime.movingAfter ? "yes" : "no")
         << " prime_chase_before=" << (prime.chaseMoveBefore ? "yes" : "no")
         << " prime_chase_after=" << (prime.chaseMoveAfter ? "yes" : "no")
         << " prime_follow_before=" << (prime.followMoveBefore ? "yes" : "no")
         << " prime_follow_after=" << (prime.followMoveAfter ? "yes" : "no");
}


struct RangedPathProbeResult
{
    bool attempted = false;
    bool success = false;
    uint32 pathType = 0;
    uint32 pointCount = 0;
    float destX = 0.0f;
    float destY = 0.0f;
    float destZ = 0.0f;
    float range = 0.0f;
    float relativeAngle = 0.0f;
    char const* mode = "none";
};

bool IsUsableProbePath(RangedPathProbeResult const& probe)
{
    return probe.attempted && probe.success && !(probe.pathType & PATHFIND_NOPATH) && probe.pointCount > 1;
}

RangedPathProbeResult ProbeChasePath(Player* player, Unit* target)
{
    RangedPathProbeResult probe;
    probe.mode = "chase_center";
    if (!player || !target)
        return probe;

    probe.attempted = true;
    target->GetPosition(probe.destX, probe.destY, probe.destZ);
    if (player->IsHovering())
        player->UpdateAllowedPositionZ(probe.destX, probe.destY, probe.destZ);

    PathGenerator path(player);
    probe.success = path.CalculatePath(probe.destX, probe.destY, probe.destZ, player->CanFly());
    probe.pathType = uint32(path.GetPathType());
    probe.pointCount = uint32(path.GetPath().size());
    return probe;
}

RangedPathProbeResult ProbeFollowPath(Player* player, Unit* target, float edgeRange, float relativeAngle)
{
    RangedPathProbeResult probe;
    probe.mode = "follow_nearpoint";
    probe.range = std::max(0.5f, edgeRange);
    probe.relativeAngle = Position::NormalizeOrientation(relativeAngle);
    if (!player || !target)
        return probe;

    probe.attempted = true;
    target->GetNearPoint(player, probe.destX, probe.destY, probe.destZ, probe.range, target->ToAbsoluteAngle(probe.relativeAngle));
    if (player->IsHovering())
        player->UpdateAllowedPositionZ(probe.destX, probe.destY, probe.destZ);

    PathGenerator path(player);
    probe.success = path.CalculatePath(probe.destX, probe.destY, probe.destZ, false);
    probe.pathType = uint32(path.GetPathType());
    probe.pointCount = uint32(path.GetPath().size());
    return probe;
}

RangedPathProbeResult FindBestFollowProbe(Player* player, Unit* target, float edgeRange)
{
    RangedPathProbeResult best;
    if (!player || !target)
        return best;

    float const currentRelative = target->GetRelativeAngle(player);
    std::array<float, 7> const offsets = { 0.0f, float(M_PI_4), -float(M_PI_4), float(M_PI_2), -float(M_PI_2), float(M_PI), -float(M_PI) };

    for (float offset : offsets)
    {
        RangedPathProbeResult probe = ProbeFollowPath(player, target, edgeRange, currentRelative + offset);
        if (!best.attempted || (probe.pointCount > best.pointCount && !(probe.pathType & PATHFIND_NOPATH)))
            best = probe;
        if (IsUsableProbePath(probe))
            return probe;
    }

    return best;
}

void AppendProbeDiag(std::ostringstream& diag, char const* prefix, RangedPathProbeResult const& probe)
{
    diag << ' ' << prefix << "_mode=" << (probe.mode ? probe.mode : "none")
         << ' ' << prefix << "_attempted=" << (probe.attempted ? "yes" : "no")
         << ' ' << prefix << "_ok=" << (IsUsableProbePath(probe) ? "yes" : "no")
         << ' ' << prefix << "_success=" << (probe.success ? "yes" : "no")
         << ' ' << prefix << "_type=" << probe.pathType
         << ' ' << prefix << "_points=" << probe.pointCount
         << ' ' << prefix << "_range=" << probe.range
         << ' ' << prefix << "_angle=" << probe.relativeAngle
         << ' ' << prefix << "_dest=(" << probe.destX << ',' << probe.destY << ',' << probe.destZ << ')';
}

TargetRelativeRangedMoveResult IssuePathProbedFollow(Player* player, Unit* target, RangedPathProbeResult const& followProbe, float fallbackEdgeRange, MotionPrimeResult* primeOut = nullptr)
{
    if (!player || !target)
        return TargetRelativeRangedMoveResult::None;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return TargetRelativeRangedMoveResult::None;

    float const safeRange = std::max(0.5f, followProbe.range > 0.0f ? followProbe.range : fallbackEdgeRange);
    float const angle = followProbe.attempted ? followProbe.relativeAngle : target->GetRelativeAngle(player);

    std::string preserveDiag;
    if (ShouldPreserveTargetRelativeMovement(player, target, safeRange, 2500, "pathprobed_follow_preserved", &preserveDiag))
    {
        SetLastMovementDebugStatus(player, preserveDiag);
        if (primeOut)
            *primeOut = MotionPrimeResult();
        return TargetRelativeRangedMoveResult::FollowIssued;
    }

    bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
    motionMaster->MoveFollow(target, safeRange, ChaseAngle(angle, float(M_PI_4)));
    RecordTargetRelativeMovementOrder(player, target, safeRange, 2);
    MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
    prime.addToWorldCalled = preparedMotionMaster;
    if (primeOut)
        *primeOut = prime;
    return TargetRelativeRangedMoveResult::FollowIssued;
}

TargetRelativeRangedMoveResult IssueTargetRelativeRangedMovement(Player* player, Unit* target, float desiredEdgeDistance, bool targetAttackable, bool forceFollow = false, MotionPrimeResult* primeOut = nullptr)
{
    if (!player || !target)
        return TargetRelativeRangedMoveResult::None;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return TargetRelativeRangedMoveResult::None;

    float const safeDistance = std::max(0.5f, desiredEdgeDistance);

    // Important: do not inspect player->isMoving() immediately after MoveChase
    // or MoveFollow. Those generators normally set CHASE_MOVE/FOLLOW_MOVE from
    // their next Update() call, not synchronously from MoveChase()/MoveFollow().
    // Immediate "chase idle -> follow" fallbacks can therefore clear a valid
    // newly queued generator before it ever gets a tick to launch its spline.
    std::string preserveDiag;
    if (ShouldPreserveTargetRelativeMovement(player, target, safeDistance, 2500, "ranged_target_relative_preserved", &preserveDiag))
    {
        SetLastMovementDebugStatus(player, preserveDiag);
        if (primeOut)
            *primeOut = MotionPrimeResult();
        MovementGeneratorType const motionType = motionMaster->GetCurrentMovementGeneratorType();
        return motionType == FOLLOW_MOTION_TYPE ? TargetRelativeRangedMoveResult::FollowIssued : TargetRelativeRangedMoveResult::ChaseIssued;
    }

    bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
    if (targetAttackable && !forceFollow)
    {
        motionMaster->MoveChase(target, BuildEdgeDistanceChaseRange(safeDistance));
        RecordTargetRelativeMovementOrder(player, target, safeDistance, 1);
        MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
        prime.addToWorldCalled = preparedMotionMaster;
        if (primeOut)
            *primeOut = prime;
        return TargetRelativeRangedMoveResult::ChaseIssued;
    }

    motionMaster->MoveFollow(target, safeDistance, player->GetFollowAngle());
    RecordTargetRelativeMovementOrder(player, target, safeDistance, 2);
    MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
    prime.addToWorldCalled = preparedMotionMaster;
    if (primeOut)
        *primeOut = prime;
    return TargetRelativeRangedMoveResult::FollowIssued;
}

TargetRelativeRangedMoveResult IssueContactChaseRescue(Player* player, Unit* target)
{
    if (!player || !target)
        return TargetRelativeRangedMoveResult::None;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return TargetRelativeRangedMoveResult::None;

    bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);

    if (!player->IsValidAttackTarget(target))
    {
        motionMaster->MoveFollow(target, 1.0f, player->GetFollowAngle());
        RecordTargetRelativeMovementOrder(player, target, 1.0f, 2);
        MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
        prime.addToWorldCalled = preparedMotionMaster;
        return TargetRelativeRangedMoveResult::FollowIssued;
    }

    if (player->GetVictim() != target || !player->IsInCombat())
        player->Attack(target, false);

    // Still target-relative/path-generator movement; this is not a raw MovePoint.
    // Use this only after ranged edge Chase/Follow repeatedly installs a
    // generator but never enters CHASE_MOVE/FOLLOW_MOVE. Default MoveChase
    // avoids the ranged stop-band math and asks the core to path to contact.
    motionMaster->MoveChase(target);
    RecordTargetRelativeMovementOrder(player, target, 0.5f, 1);
    MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
    prime.addToWorldCalled = preparedMotionMaster;
    return TargetRelativeRangedMoveResult::ChaseIssued;
}

std::string BuildRangedMovementDiag(Player const* player, Unit const* target, char const* label, float desiredEdgeDistance, float issuedEdgeDistance,
    bool targetLos, bool targetAttackable, bool cleared, MovementGeneratorType motionBefore, char const* issuedMode = nullptr)
{
    std::ostringstream diag;
    if (!player || !target)
    {
        diag << (label ? label : "ranged_move") << " unavailable";
        return diag.str();
    }

    float const edgeDistance = player->GetDistance(target);
    float const exactDistance = player->GetExactDist(target);
    float const hitboxSum = player->GetCombatReach() + target->GetCombatReach();
    float const singleFloatStopEdge = issuedEdgeDistance + CONTACT_DISTANCE;

    diag << (label ? label : "ranged_move")
         << " edge_dist=" << edgeDistance
         << " exact_dist=" << exactDistance
         << " hitbox_sum=" << hitboxSum
         << " desired_edge=" << desiredEdgeDistance
         << " issued_edge=" << issuedEdgeDistance
         << " single_float_stop_edge=" << singleFloatStopEdge
         << " custom_chase_max_exact=" << (issuedEdgeDistance + hitboxSum)
         << " issued_mode=" << (issuedMode ? issuedMode : "unknown")
         << " los=" << (targetLos ? "yes" : "no")
         << " attackable=" << (targetAttackable ? "yes" : "no")
         << " cleared=" << (cleared ? "yes" : "no")
         << " motion_before=" << uint32(motionBefore);

    if (MotionMaster const* motionMaster = player->GetMotionMaster())
        diag << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType());

    diag << " moving_after=" << (player->isMoving() ? "yes" : "no")
         << " not_move=" << (player->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
         << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
         << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
         << " casting_prevent=" << (player->IsMovementPreventedByCasting() ? "yes" : "no");

    auto orderItr = g_TargetRelativeMoveOrderByGuid.find(player->GetGUID().GetRawValue());
    if (orderItr != g_TargetRelativeMoveOrderByGuid.end())
    {
        TargetRelativeMoveOrderState const& order = orderItr->second;
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32 const orderAgeMs = order.lastIssueMs != 0 && nowMs >= order.lastIssueMs ? nowMs - order.lastIssueMs : 0;
        uint32 const distanceProgressAgeMs = order.lastProgressMs != 0 && nowMs >= order.lastProgressMs ? nowMs - order.lastProgressMs : 0;
        uint32 const positionProgressAgeMs = order.lastPositionProgressMs != 0 && nowMs >= order.lastPositionProgressMs ? nowMs - order.lastPositionProgressMs : 0;
        float const dx = player->GetPositionX() - order.lastX;
        float const dy = player->GetPositionY() - order.lastY;
        float const dz = player->GetPositionZ() - order.lastZ;
        float const positionDelta2D = std::sqrt(dx * dx + dy * dy);
        float const positionDelta3D = std::sqrt(dx * dx + dy * dy + dz * dz);

        diag << " order_match=" << (order.targetGuid == target->GetGUID() ? "yes" : "no")
             << " order_mode=" << uint32(order.mode)
             << " order_age_ms=" << orderAgeMs
             << " order_issued_range=" << order.issuedRange
             << " order_last_dist=" << order.lastDistance
             << " order_dist_progress_age_ms=" << distanceProgressAgeMs
             << " order_pos_progress_age_ms=" << positionProgressAgeMs
             << " order_pos_delta_2d=" << positionDelta2D
             << " order_pos_delta_3d=" << positionDelta3D;
    }
    else
        diag << " order_match=none";

    AppendMotionSnapshotDiag(diag, player, "move");
    return diag.str();
}

bool IsStaleTargetRelativeMotion(Player const* player)
{
    if (!player)
        return false;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const motionType = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    return (motionType == CHASE_MOTION_TYPE || motionType == FOLLOW_MOTION_TYPE) &&
        !player->isMoving() &&
        !player->HasUnitState(UNIT_STATE_CHASE_MOVE) &&
        !player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) &&
        !player->HasUnitState(UNIT_STATE_NOT_MOVE) &&
        !player->IsMovementPreventedByCasting();
}

void ClearStaleTargetRelativeMotionForCast(Player* player, char const* reason)
{
    if (!player || !IsStaleTargetRelativeMotion(player))
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    MovementGeneratorType const motionBefore = motionMaster->GetCurrentMovementGeneratorType();
    motionMaster->Clear(MOTION_SLOT_ACTIVE);

    std::ostringstream diag;
    diag << (reason ? reason : "cleared_stale_target_relative")
         << " motion_before=" << uint32(motionBefore)
         << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
         << " moving_after=" << (player->isMoving() ? "yes" : "no")
         << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
         << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
         << " not_move=" << (player->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
         << " casting_prevent=" << (player->IsMovementPreventedByCasting() ? "yes" : "no");
    SetLastMovementDebugStatus(player, diag.str());
}

bool IsSpellReadyAtCurrentPosition(Player* player, Unit* target, SpellInfo const* spellInfo, playerbot::PvpClassSpellContext::TargetMode targetMode)
{
    if (!player || !target || !spellInfo || !target->IsAlive())
        return false;

    if (targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
    {
        if (!player->IsValidAttackTarget(target, spellInfo))
            return false;
    }
    else if (targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
    {
        if (!IsFriendlySupportTarget(player, target, spellInfo))
            return false;
    }
    else if (targetMode != playerbot::PvpClassSpellContext::TargetMode::Self)
        return false;

    if (!player->IsWithinLOSInMap(target))
        return false;

    float const maxRange = spellInfo->GetMaxRange(false);
    if (maxRange > 0.0f && !player->IsWithinDistInMap(target, maxRange))
        return false;

    float const minRange = spellInfo->GetMinRange(false);
    if (minRange > 0.0f && player->IsWithinDistInMap(target, minRange))
        return false;

    return true;
}

void IssueStealthOpenerMovement(Player* player, Unit* target)
{
    if (!player || !target || !CanIssueFollowCommands(player))
        return;

    // Do not use 0.1f here. On some BG/custom-map pathing, the behind/contact
    // follow point is inside the target collision band or an invalid local
    // triangle, so FollowMovementGenerator installs but never launches. A small
    // but real follow band keeps stealth and still lets the generator produce a path.
    float constexpr stealthFollowRange = 1.5f;
    bool const issued = IssueThrottledFollowMovement(player, target, stealthFollowRange, 750, 0.25f);

    std::ostringstream diag;
    diag << "stealth_opener_follow"
         << " issued=" << (issued ? "yes" : "no")
         << " follow_range=" << stealthFollowRange;
    if (MotionMaster const* motionMaster = player->GetMotionMaster())
        diag << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType());
    diag << " moving_after=" << (player->isMoving() ? "yes" : "no")
         << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
         << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no");
    SetLastMovementDebugStatus(player, diag.str());
}

void IssueRangedApproachMovement(Player* player, Unit* target, float desiredDistance, bool forceMovementWhenAlreadyInRange = false, char const* forcedReason = nullptr)
{
    if (!player || !target)
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    struct RangedApproachStallState
    {
        ObjectGuid targetGuid = ObjectGuid::Empty;
        float lastDistance = 0.0f;
        float lastIssuedRange = 0.0f;
        uint32 lastSampleMs = 0;
        uint32 lastFallbackMs = 0;
        uint32 lastIssueMs = 0;
        uint8 stagnantSamples = 0;
        uint8 lastIssuedMode = 0; // 1=chase, 2=follow
    };

    static std::unordered_map<uint64, RangedApproachStallState> stallStateByGuid;
    RangedApproachStallState& stallState = stallStateByGuid[player->GetGUID().GetRawValue()];

    float const requestedSafeDistance = std::max(1.0f, desiredDistance);
    float const currentDistance = player->GetDistance(target);
    bool const targetLos = player->IsWithinLOSInMap(target);
    bool const targetAttackable = player->IsValidAttackTarget(target);
    MovementGeneratorType const initialMotionType = motionMaster->GetCurrentMovementGeneratorType();
    bool const currentlyMoving = player->isMoving();
    bool const strictPathing = RequiresStrictHumanPathing(player);

    // Important: SPELL_FAILED_LINE_OF_SIGHT is more authoritative than the
    // generic IsWithinLOSInMap() diagnostic. On custom BG maps/vmaps the simple
    // LOS check can say yes while Spell::CheckCast still rejects the cast. In
    // that case do not treat "already in range" as cast-ready; force a small
    // target-relative reposition by asking Chase/Follow for a range inside our
    // current distance. This keeps the movement pathing-aware and avoids raw
    // MovePoint wall/barrier shoves.
    float safeDistance = requestedSafeDistance;
    bool const forcedInRangeLosRecovery = forceMovementWhenAlreadyInRange && currentDistance <= (requestedSafeDistance + 0.25f);
    if (forcedInRangeLosRecovery)
    {
        float const forcedCloserDistance = currentDistance > 4.0f ? (currentDistance - 3.0f) : (currentDistance * 0.5f);
        safeDistance = std::max(1.0f, std::min(requestedSafeDistance, forcedCloserDistance));
    }

    bool const nearRangeEdge = currentDistance > (safeDistance + 1.0f) && currentDistance <= (safeDistance + 8.0f);
    uint32 const nowMs = GameTime::GetGameTimeMS();
    bool const sameStallTarget = stallState.targetGuid == target->GetGUID();
    bool const activeTargetRelativeMotion = initialMotionType == CHASE_MOTION_TYPE || initialMotionType == FOLLOW_MOTION_TYPE;
    bool const movementGeneratorHasNotLaunched = !player->isMoving() && !player->HasUnitState(UNIT_STATE_CHASE_MOVE) && !player->HasUnitState(UNIT_STATE_FOLLOW_MOVE);
    uint32 const lastIssueAgeMs = sameStallTarget && stallState.lastIssueMs != 0 && nowMs >= stallState.lastIssueMs ? nowMs - stallState.lastIssueMs : 0;

    if (!forceMovementWhenAlreadyInRange && activeTargetRelativeMotion && currentDistance > (safeDistance + 0.75f))
    {
        std::string preserveDiag;
        if (ShouldPreserveTargetRelativeMovement(player, target, safeDistance, 3000, "ranged_existing_motion_preserved", &preserveDiag))
        {
            SetLastMovementDebugStatus(player, preserveDiag);
            stallState.targetGuid = target->GetGUID();
            stallState.lastDistance = currentDistance;
            stallState.lastSampleMs = nowMs;
            return;
        }
    }

    if (!forceMovementWhenAlreadyInRange && targetLos && currentDistance <= (safeDistance + 0.25f))
    {
        bool const clearedStale = IsStaleTargetRelativeMotion(player);
        if (clearedStale)
            motionMaster->Clear(MOTION_SLOT_ACTIVE);

        SetLastMovementDebugStatus(player, BuildRangedMovementDiag(player, target, "ranged_move_skipped_already_in_range",
            safeDistance, safeDistance, targetLos, targetAttackable, clearedStale, initialMotionType, "none"));
        return;
    }

    if (!currentlyMoving && initialMotionType == POINT_MOTION_TYPE && currentDistance > (safeDistance + 1.0f))
    {
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "Ranged approach cleared stalled point movement: guid={} target={} desiredRange={} currentDistance={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), safeDistance, currentDistance);
    }

    // For target-relative ranged approach, do NOT use MovePoint here.
    // MoveChase/MoveFollow keep the movement generator attached to the target
    // and route through the server movement/pathing stack instead of pushing a
    // raw destination that can ignore battleground walls/barriers.
    //
    // The near-edge bug is that MoveChase(target, 27) can decide that 29-30y is
    // "close enough" after hitbox/tolerance math. Solve that by issuing the
    // same target-relative generator with a deeper desired range for this tick.
    if (strictPathing && nearRangeEdge)
    {
        // If Chase/Follow is already installed but has not launched yet, do NOT
        // immediately clear/reissue it. First prime the existing generator in
        // place. The latest diagnostics showed the bad loop clearly:
        //   motion_before=14/follow, moving=no, then after reissue+prime
        //   moving_after=yes follow_move=yes.
        // Reissuing every stale window caused visible inch/stop movement.
        if (sameStallTarget && activeTargetRelativeMotion && movementGeneratorHasNotLaunched && stallState.lastIssueMs != 0)
        {
            MotionPrimeResult existingPrimeResult = PrimeTargetRelativeMotion(player);
            bool const existingMotionStarted = player->isMoving() ||
                player->HasUnitState(UNIT_STATE_CHASE_MOVE) || player->HasUnitState(UNIT_STATE_FOLLOW_MOVE);

            if (existingMotionStarted)
            {
                stallState.lastDistance = currentDistance;
                stallState.lastSampleMs = nowMs;
                std::ostringstream extra;
                extra << BuildRangedMovementDiag(player, target, "near_edge_existing_motion_primed",
                    safeDistance, stallState.lastIssuedRange > 0.0f ? stallState.lastIssuedRange : safeDistance,
                    targetLos, targetAttackable, false, initialMotionType,
                    stallState.lastIssuedMode == 2 ? "existing_follow_primed" : "existing_chase_primed")
                      << " issue_age_ms=" << lastIssueAgeMs
                      << " last_mode=" << uint32(stallState.lastIssuedMode)
                      << " last_range=" << stallState.lastIssuedRange;
                AppendMotionPrimeDiag(extra, existingPrimeResult);
                SetLastMovementDebugStatus(player, extra.str());
                return;
            }

            // Give the queued generator a settle window after a failed prime.
            // Only after that window do we clear/reissue.
            if (lastIssueAgeMs < 900)
            {
                std::ostringstream extra;
                extra << BuildRangedMovementDiag(player, target, "near_edge_waiting_for_motion_update",
                    safeDistance, stallState.lastIssuedRange > 0.0f ? stallState.lastIssuedRange : safeDistance,
                    targetLos, targetAttackable, false, initialMotionType,
                    stallState.lastIssuedMode == 2 ? "follow_wait" : "chase_wait")
                      << " issue_age_ms=" << lastIssueAgeMs
                      << " last_mode=" << uint32(stallState.lastIssuedMode)
                      << " last_range=" << stallState.lastIssuedRange;
                AppendMotionPrimeDiag(extra, existingPrimeResult);
                SetLastMovementDebugStatus(player, extra.str());
                return;
            }
        }

        bool const staleQueuedGenerator = sameStallTarget && activeTargetRelativeMotion && movementGeneratorHasNotLaunched && stallState.lastIssueMs != 0 && lastIssueAgeMs >= 2500;
        float const forcedRange = std::max(1.0f, safeDistance - 8.0f);

        if (targetAttackable && (player->GetVictim() != target || !player->IsInCombat()))
            player->Attack(target, false);

        RangedPathProbeResult const chaseProbe = ProbeChasePath(player, target);
        RangedPathProbeResult const followProbe = FindBestFollowProbe(player, target, forcedRange);

        if (!(activeTargetRelativeMotion && (player->isMoving() || player->HasUnitState(UNIT_STATE_CHASE_MOVE) || player->HasUnitState(UNIT_STATE_FOLLOW_MOVE))))
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
        MotionPrimeResult primeResult;
        TargetRelativeRangedMoveResult const moveResult = staleQueuedGenerator && IsUsableProbePath(followProbe)
            ? IssuePathProbedFollow(player, target, followProbe, forcedRange, &primeResult)
            : IssueTargetRelativeRangedMovement(player, target, forcedRange, targetAttackable, false, &primeResult);

        stallState.targetGuid = target->GetGUID();
        stallState.lastDistance = currentDistance;
        stallState.lastSampleMs = nowMs;
        stallState.lastFallbackMs = nowMs;
        stallState.lastIssueMs = nowMs;
        stallState.lastIssuedRange = forcedRange;
        stallState.lastIssuedMode = moveResult == TargetRelativeRangedMoveResult::FollowIssued ? 2 : 1;
        stallState.stagnantSamples = staleQueuedGenerator ? 1 : 0;

        char const* label = staleQueuedGenerator
            ? (IsUsableProbePath(followProbe) ? "near_edge_stale_pathprobed_follow" : "near_edge_stale_reissued_chase_no_follow_path")
            : "near_edge_chase_follow_nudge";

        std::string diag = BuildRangedMovementDiag(player, target, label,
            safeDistance, forcedRange, targetLos, targetAttackable, true, initialMotionType,
            GetTargetRelativeRangedMoveResultLabel(moveResult));
        std::ostringstream extra;
        extra << diag
              << " stale=" << (staleQueuedGenerator ? "yes" : "no")
              << " issue_age_ms=" << lastIssueAgeMs
              << " force_in_range=" << (forceMovementWhenAlreadyInRange ? "yes" : "no")
              << " forced_reason=" << (forcedReason ? forcedReason : "none")
              << " requested_edge=" << requestedSafeDistance;
        AppendMotionPrimeDiag(extra, primeResult);
        AppendProbeDiag(extra, "chase_probe", chaseProbe);
        AppendProbeDiag(extra, "follow_probe", followProbe);
        SetLastMovementDebugStatus(player, extra.str());

        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "Ranged near-edge queued target-relative movement: guid={} target={} currentDistance={} desiredRange={} forcedEdgeRange={} los={} attackable={} issuedMode={} stale={} issueAgeMs={} chaseProbeOk={} followProbeOk={} followProbeType={} followProbePoints={} movingBefore={} motionBefore={} motionAfter={} movingAfter={} chaseMove={} followMove={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), currentDistance, safeDistance, forcedRange, targetLos, targetAttackable,
            GetTargetRelativeRangedMoveResultLabel(moveResult), staleQueuedGenerator, lastIssueAgeMs, IsUsableProbePath(chaseProbe), IsUsableProbePath(followProbe), followProbe.pathType, followProbe.pointCount, currentlyMoving, static_cast<uint32>(initialMotionType),
            static_cast<uint32>(motionMaster->GetCurrentMovementGeneratorType()), player->isMoving(),
            player->HasUnitState(UNIT_STATE_CHASE_MOVE), player->HasUnitState(UNIT_STATE_FOLLOW_MOVE));
        return;
    }

    if (strictPathing)
    {
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "Strict ranged approach using target-relative chase/follow only: guid={} target={} desiredRange={} currentDistance={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), safeDistance, currentDistance);
    }

    bool hardStaleTargetRelative = activeTargetRelativeMotion && movementGeneratorHasNotLaunched &&
        sameStallTarget && stallState.lastIssueMs != 0 && lastIssueAgeMs >= 900;

    // Same protection for the generic ranged path: before clearing/reissuing a
    // stale Chase/Follow generator, prime the existing generator once in place.
    // If it starts moving, preserve it instead of resetting the spline.
    if (hardStaleTargetRelative)
    {
        MotionPrimeResult existingPrimeResult = PrimeTargetRelativeMotion(player);
        bool const existingMotionStarted = player->isMoving() ||
            player->HasUnitState(UNIT_STATE_CHASE_MOVE) || player->HasUnitState(UNIT_STATE_FOLLOW_MOVE);

        if (existingMotionStarted)
        {
            stallState.lastDistance = currentDistance;
            stallState.lastSampleMs = nowMs;
            std::ostringstream diag;
            diag << BuildRangedMovementDiag(player, target, "generic_existing_motion_primed",
                safeDistance, stallState.lastIssuedRange > 0.0f ? stallState.lastIssuedRange : safeDistance,
                targetLos, targetAttackable, false, initialMotionType,
                stallState.lastIssuedMode == 2 ? "existing_follow_primed" : "existing_chase_primed")
                 << " issue_age_ms=" << lastIssueAgeMs
                 << " last_mode=" << uint32(stallState.lastIssuedMode)
                 << " last_range=" << stallState.lastIssuedRange;
            AppendMotionPrimeDiag(diag, existingPrimeResult);
            SetLastMovementDebugStatus(player, diag.str());
            return;
        }

        hardStaleTargetRelative = true;
    }

    bool const shouldForceActiveRepath = !player->isMoving() && currentDistance > (safeDistance + 1.5f) &&
        (!activeTargetRelativeMotion || hardStaleTargetRelative);
    if (shouldForceActiveRepath)
    {
        // Recover from stale active movement generators only after a real settle
        // window. Clearing every lifecycle tick causes the visible inch/stop
        // behavior because MoveChase/MoveFollow replaces the active spline.
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
    }

    float const genericMoveRange = nearRangeEdge ? std::max(1.0f, safeDistance - 5.0f) : safeDistance;
    if (targetAttackable && (player->GetVictim() != target || !player->IsInCombat()))
    {
        // Ensure hostile ranged approach can engage chase generators even when
        // the bot is currently out of combat.
        player->Attack(target, false);
    }

    if (activeTargetRelativeMotion && !hardStaleTargetRelative)
    {
        std::string preserveDiag;
        if (ShouldPreserveTargetRelativeMovement(player, target, genericMoveRange, 3000, "generic_existing_motion_preserved", &preserveDiag))
        {
            SetLastMovementDebugStatus(player, preserveDiag);
            stallState.targetGuid = target->GetGUID();
            stallState.lastDistance = currentDistance;
            stallState.lastSampleMs = nowMs;
            return;
        }
    }

    MotionPrimeResult genericPrimeResult;
    TargetRelativeRangedMoveResult const genericMoveResult = IssueTargetRelativeRangedMovement(player, target, genericMoveRange, targetAttackable, false, &genericPrimeResult);

    {
        std::ostringstream diag;
        diag << BuildRangedMovementDiag(player, target, forceMovementWhenAlreadyInRange ? "generic_forced_los_recovery_move" : "generic_ranged_move",
            forceMovementWhenAlreadyInRange ? requestedSafeDistance : safeDistance, genericMoveRange, targetLos, targetAttackable, shouldForceActiveRepath, initialMotionType,
            GetTargetRelativeRangedMoveResultLabel(genericMoveResult))
             << " force_in_range=" << (forceMovementWhenAlreadyInRange ? "yes" : "no")
             << " forced_reason=" << (forcedReason ? forcedReason : "none")
             << " requested_edge=" << requestedSafeDistance;
        AppendMotionPrimeDiag(diag, genericPrimeResult);
        SetLastMovementDebugStatus(player, diag.str());
    }

    stallState.lastIssueMs = nowMs;
    stallState.lastIssuedRange = genericMoveRange;
    stallState.lastIssuedMode = genericMoveResult == TargetRelativeRangedMoveResult::FollowIssued ? 2 : 1;

    // Some battleground edge-cases keep a stale chase/follow generator active
    // without producing displacement while we are still out of range. Recover
    // by clearing and reissuing a target-relative chase/follow with a deeper
    // range, not by using MovePoint.
    float const postIssueDistance = player->GetDistance(target);
    bool const targetChanged = stallState.targetGuid != target->GetGUID();
    bool const recentlySampled = !targetChanged && stallState.lastSampleMs != 0 && nowMs <= (stallState.lastSampleMs + 1500);
    bool const distanceStagnant = recentlySampled && std::fabs(postIssueDistance - stallState.lastDistance) < 0.35f;

    if (targetChanged || !distanceStagnant)
        stallState.stagnantSamples = 0;

    if (!player->isMoving() && postIssueDistance > (safeDistance + 1.0f) && distanceStagnant)
        ++stallState.stagnantSamples;

    stallState.targetGuid = target->GetGUID();
    stallState.lastDistance = postIssueDistance;
    stallState.lastSampleMs = nowMs;

    // Avoid clearing active movement every tick; only recover when we have
    // repeated stagnant samples and throttle the fallback issue rate.
    if (!player->isMoving() &&
        postIssueDistance > (safeDistance + 1.0f) &&
        stallState.stagnantSamples >= 2 &&
        (stallState.lastFallbackMs == 0 || nowMs >= (stallState.lastFallbackMs + 2500)))
    {
        MovementGeneratorType const motionType = motionMaster->GetCurrentMovementGeneratorType();
        if (motionType == POINT_MOTION_TYPE)
        {
            // If we are already in a stalled point movement, prefer reissuing
            // chase/follow instead of chaining another point destination.
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
            MotionPrimeResult fallbackPrimeResult;
            TargetRelativeRangedMoveResult const fallbackMoveResult = IssueTargetRelativeRangedMovement(player, target,
                std::max(1.0f, safeDistance - 2.0f), player->IsValidAttackTarget(target), false, &fallbackPrimeResult);

            stallState.lastFallbackMs = nowMs;
            {
                std::ostringstream diag;
                diag << "stalled_point_reissued_target_relative"
                     << " dist=" << postIssueDistance
                     << " desired=" << safeDistance
                     << " issued_mode=" << GetTargetRelativeRangedMoveResultLabel(fallbackMoveResult)
                     << " motion_before=" << uint32(motionType)
                     << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
                     << " moving_after=" << (player->isMoving() ? "yes" : "no")
                     << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
                     << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no");
                AppendMotionPrimeDiag(diag, fallbackPrimeResult);
                SetLastMovementDebugStatus(player, diag.str());
            }
            TC_LOG_DEBUG("playerbots.pvp.classspell",
                "Ranged approach fallback reissued chase/follow from stalled point: guid={} target={} desiredRange={} currentDistance={} motionType={}.",
                player->GetGUID().ToString(), target->GetGUID().ToString(), safeDistance, postIssueDistance, static_cast<uint32>(motionType));
            return;
        }

        if (motionType == CHASE_MOTION_TYPE || motionType == FOLLOW_MOTION_TYPE || motionType == IDLE_MOTION_TYPE)
            motionMaster->Clear(MOTION_SLOT_ACTIVE);

        bool const hostileTarget = player->IsValidAttackTarget(target);
        float const fallbackRange = std::max(1.0f, safeDistance - 2.0f);
        RangedPathProbeResult const chaseProbe = ProbeChasePath(player, target);
        RangedPathProbeResult const followProbe = FindBestFollowProbe(player, target, fallbackRange);
        MotionPrimeResult fallbackPrimeResult;
        TargetRelativeRangedMoveResult const fallbackMoveResult = IsUsableProbePath(followProbe)
            ? IssuePathProbedFollow(player, target, followProbe, fallbackRange, &fallbackPrimeResult)
            : IssueTargetRelativeRangedMovement(player, target, fallbackRange, hostileTarget, !hostileTarget, &fallbackPrimeResult);
        stallState.lastFallbackMs = nowMs;
        {
            std::ostringstream diag;
            diag << (IsUsableProbePath(followProbe) ? "stagnant_pathprobed_follow" : "stagnant_reissued_target_relative_no_follow_path")
                 << " dist=" << postIssueDistance
                 << " desired=" << safeDistance
                 << " issued_range=" << fallbackRange
                 << " issued_mode=" << GetTargetRelativeRangedMoveResultLabel(fallbackMoveResult)
                 << " motion_before=" << uint32(motionType)
                 << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
                 << " moving_after=" << (player->isMoving() ? "yes" : "no")
                 << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
                 << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no");
            AppendMotionPrimeDiag(diag, fallbackPrimeResult);
            AppendProbeDiag(diag, "chase_probe", chaseProbe);
            AppendProbeDiag(diag, "follow_probe", followProbe);
            SetLastMovementDebugStatus(player, diag.str());
        }
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "Ranged approach forced target-relative fallback: guid={} target={} desiredRange={} currentDistance={} motionType={} issuedMode={} chaseProbeOk={} followProbeOk={} followProbeType={} followProbePoints={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), safeDistance, postIssueDistance, static_cast<uint32>(motionType), GetTargetRelativeRangedMoveResultLabel(fallbackMoveResult), IsUsableProbePath(chaseProbe), IsUsableProbePath(followProbe), followProbe.pathType, followProbe.pointCount);
    }
}

void IssueMeleeApproachMovement(Player* player, Unit* target)
{
    if (!player || !target)
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    if (player->HasStealthAura())
    {
        // For hostile stealth openers, use default MoveChase instead of
        // MoveFollow. FollowMovementGenerator can choose a behind/angle point
        // that never launches on some BG/mmaps, leaving rogues stealthed at
        // their base. Default MoveChase is still target-relative and pathing-
        // aware, but it does not depend on a fragile follow angle point.
        if (player->IsValidAttackTarget(target))
        {
            if (player->GetVictim() != target || !player->IsInCombat())
                player->Attack(target, false);

            std::string preserveDiag;
            if (ShouldPreserveTargetRelativeMovement(player, target, 1.5f, 2000, "stealth_melee_existing_motion_preserved", &preserveDiag))
            {
                SetLastMovementDebugStatus(player, preserveDiag);
                return;
            }

            RangedPathProbeResult const followProbe = FindBestFollowProbe(player, target, 1.5f);
            MotionPrimeResult stealthPrimeResult;
            if (IsUsableProbePath(followProbe))
                IssuePathProbedFollow(player, target, followProbe, 1.5f, &stealthPrimeResult);
            else
            {
                bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
                motionMaster->MoveChase(target);
                RecordTargetRelativeMovementOrder(player, target, 0.5f, 1);
                stealthPrimeResult = PrimeTargetRelativeMotion(player);
                stealthPrimeResult.addToWorldCalled = preparedMotionMaster;
            }

            std::ostringstream diag;
            diag << (IsUsableProbePath(followProbe) ? "stealth_melee_pathprobed_follow" : "stealth_melee_chase_no_follow_path")
                 << " issued_mode=" << (IsUsableProbePath(followProbe) ? "follow" : "chase")
                 << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
                 << " moving_after=" << (player->isMoving() ? "yes" : "no")
                 << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
                 << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no");
            AppendMotionPrimeDiag(diag, stealthPrimeResult);
            AppendProbeDiag(diag, "follow_probe", followProbe);
            SetLastMovementDebugStatus(player, diag.str());
            return;
        }

        IssueThrottledFollowMovement(player, target, 1.5f);
        return;
    }

    if (RequiresStrictHumanPathing(player) && IssueStrictHumanFollow(player, target, 1.5f))
        return;

    if (RequiresStrictHumanPathing(player))
    {
        // Keep melee bots close to contact range instead of orbiting around a
        // larger follow radius (which can look like "running away"). If strict
        // pathing fails, fall back to regular chase so bots keep pressure.
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "Strict melee follow fallback to generic chase: guid={} target={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString());
    }

    std::string preserveDiag;
    if (ShouldPreserveTargetRelativeMovement(player, target, 1.5f, 2000, "melee_existing_motion_preserved", &preserveDiag))
    {
        SetLastMovementDebugStatus(player, preserveDiag);
        return;
    }

    bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
    if (player->IsValidAttackTarget(target))
    {
        motionMaster->MoveChase(target);
        RecordTargetRelativeMovementOrder(player, target, 0.5f, 1);
    }
    else
    {
        motionMaster->MoveFollow(target, 1.5f, player->GetFollowAngle());
        RecordTargetRelativeMovementOrder(player, target, 1.5f, 2);
    }

    MotionPrimeResult meleePrimeResult = PrimeTargetRelativeMotion(player);
    meleePrimeResult.addToWorldCalled = preparedMotionMaster;
}

float ComputeLosRecoveryRange(Player const* player, Unit const* target, float maxRange)
{
    if (!player || !target)
        return std::max(1.0f, playerbot::PvpCore::GetConfig().closeRange);

    // LOS recovery should use a stable follow band. If desired range changes
    // every tick from "current distance - X", bots can bounce between movement
    // directives (approach/flee) and look like they are stutter-looping.
    float const closeFloor = std::max(3.0f, playerbot::PvpCore::GetConfig().closeRange);
    float const upperBound = maxRange > 0.0f
        ? std::max(1.0f, maxRange - 3.0f)
        : std::max(1.0f, playerbot::PvpCore::GetConfig().spellRange - 1.0f);

    // Keep recovery closer than max cast distance to re-acquire LOS, but avoid
    // forcing a near-melee collapse that causes immediate spacing corrections.
    float const stableRecoveryBand = upperBound * 0.65f;
    float desiredRange = std::max(closeFloor, std::clamp(stableRecoveryBand, 1.0f, upperBound));

    // If config floors exceed spell upper bounds, prefer the spell bound to
    // avoid selecting an impossible follow distance.
    if (closeFloor > upperBound)
        desiredRange = upperBound;

    return desiredRange;
}

bool IsCrowdControlledForAction(Player const* player)
{
    if (!player)
        return false;

    constexpr uint32 ccMechanicMask =
        (1u << MECHANIC_CHARM) |
        (1u << MECHANIC_DISORIENTED) |
        (1u << MECHANIC_FEAR) |
        (1u << MECHANIC_SLEEP) |
        (1u << MECHANIC_STUN) |
        (1u << MECHANIC_FREEZE) |
        (1u << MECHANIC_POLYMORPH) |
        (1u << MECHANIC_BANISH) |
        (1u << MECHANIC_HORROR) |
        (1u << MECHANIC_SAPPED);

    bool const hasLostControlState = player->HasUnitState(UNIT_STATE_LOST_CONTROL);
    bool const hasHardCcState = player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING);
    bool const hasCcAura = player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->HasAuraWithMechanic(ccMechanicMask) ||
        player->IsPolymorphed();

    return hasLostControlState || hasHardCcState || hasCcAura;
}

bool IsFriendlySupportTarget(Player const* player, Unit const* target, SpellInfo const* spellInfo)
{
    if (!player || !target || !target->IsAlive())
        return false;

    if (target == player)
        return true;

    if (player->IsValidAssistTarget(target, spellInfo))
        return true;

    Player const* targetPlayer = target->ToPlayer();
    if (!targetPlayer || !player->InBattleground() || !targetPlayer->InBattleground())
        return false;

    if (player->GetBattlegroundId() != targetPlayer->GetBattlegroundId())
        return false;

    uint32 const playerTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    uint32 const targetTeam = targetPlayer->GetBGTeam() ? targetPlayer->GetBGTeam() : targetPlayer->GetTeam();
    return playerTeam == targetTeam;
}

struct WarlockCurseCooldownKey
{
    ObjectGuid casterGuid;
    ObjectGuid targetGuid;
    uint32 spellId = 0;

    bool operator==(WarlockCurseCooldownKey const& other) const
    {
        return casterGuid == other.casterGuid && targetGuid == other.targetGuid && spellId == other.spellId;
    }
};

struct WarlockCurseCooldownKeyHash
{
    std::size_t operator()(WarlockCurseCooldownKey const& key) const
    {
        std::size_t const casterHash = std::hash<uint64>{}(key.casterGuid.GetRawValue());
        std::size_t const targetHash = std::hash<uint64>{}(key.targetGuid.GetRawValue());
        std::size_t const spellHash = std::hash<uint32>{}(key.spellId);
        return casterHash ^ (targetHash << 1) ^ (spellHash << 2);
    }
};

std::unordered_map<WarlockCurseCooldownKey, std::chrono::steady_clock::time_point, WarlockCurseCooldownKeyHash> g_WarlockCurseTargetCooldowns;

struct CasterSpellCooldownKey
{
    ObjectGuid casterGuid;
    uint32 spellId = 0;

    bool operator==(CasterSpellCooldownKey const& other) const
    {
        return casterGuid == other.casterGuid && spellId == other.spellId;
    }
};

struct CasterSpellCooldownKeyHash
{
    std::size_t operator()(CasterSpellCooldownKey const& key) const
    {
        std::size_t const casterHash = std::hash<uint64>{}(key.casterGuid.GetRawValue());
        std::size_t const spellHash = std::hash<uint32>{}(key.spellId);
        return casterHash ^ (spellHash << 1);
    }
};

std::unordered_map<CasterSpellCooldownKey, std::chrono::steady_clock::time_point, CasterSpellCooldownKeyHash> g_CasterSpellCooldowns;
std::unordered_map<uint64, std::string> g_LastClassExecutionStatusByGuid;
std::unordered_map<uint64, std::string> g_LastMovementDebugStatusByGuid;
struct LastDirectiveState
{
    playerbot::PvpClassSpellContext::MovementDirective directive = playerbot::PvpClassSpellContext::MovementDirective::None;
    ObjectGuid targetGuid = ObjectGuid::Empty;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();
};
std::unordered_map<ObjectGuid, LastDirectiveState> g_LastDirectiveByBot;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT = 29073;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK = 22734;

bool ShouldThrottleDirective(Player const* player, playerbot::PvpClassSpellContext const& context)
{
    if (!player || context.movementDirective == playerbot::PvpClassSpellContext::MovementDirective::None)
        return false;

    bool const isTravelDirective =
        context.movementDirective == playerbot::PvpClassSpellContext::MovementDirective::ReachMeleeRange ||
        context.movementDirective == playerbot::PvpClassSpellContext::MovementDirective::ReachSpellRange ||
        context.movementDirective == playerbot::PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell;

    // If we intended to travel but currently have no active travel generator,
    // do not suppress directive execution. However, if CHASE/FOLLOW/POINT is
    // already installed, allow the normal short throttle below to fire.
    // Constantly clearing/reissuing a movement generator every AI tick can keep
    // the bot in the exact visible state we are diagnosing: motion=chase/point
    // with moving=no and no displacement. Give newly issued movement a few
    // ticks to start before replacing it.
    if (isTravelDirective && !player->isMoving())
    {
        MotionMaster const* motionMaster = player->GetMotionMaster();
        MovementGeneratorType const movementType = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
        if (movementType == IDLE_MOTION_TYPE)
            return false;
    }

    auto& state = g_LastDirectiveByBot[player->GetGUID()];
    std::chrono::steady_clock::time_point const now = GameTime::Now();
    if (state.directive == context.movementDirective &&
        state.targetGuid == context.movementTargetGuid &&
        now - state.timestamp < std::chrono::milliseconds(500))
    {
        return true;
    }

    state.directive = context.movementDirective;
    state.targetGuid = context.movementTargetGuid;
    state.timestamp = now;
    return false;
}

void SetLastExecutionStatus(Player const* player, std::string const& status)
{
    if (!player)
        return;

    uint64 const rawGuid = player->GetGUID().GetRawValue();
    auto& previous = g_LastClassExecutionStatusByGuid[rawGuid];
    if (previous != status)
    {
        TC_LOG_DEBUG("playerbots.pvp.classspell", "PB exec diag: bot={} guid={} status={}",
            player->GetName(), player->GetGUID().ToString(), status);
        previous = status;
    }
}

void SetLastMovementDebugStatus(Player const* player, std::string const& status)
{
    if (!player)
        return;

    uint64 const rawGuid = player->GetGUID().GetRawValue();
    auto& previous = g_LastMovementDebugStatusByGuid[rawGuid];
    if (previous != status)
    {
        TC_LOG_DEBUG("playerbots.pvp.classspell", "PB move diag: bot={} guid={} status={}",
            player->GetName(), player->GetGUID().ToString(), status);
        previous = status;
    }
}

void ForcePlayerbotDismount(Player* player)
{
    if (!player)
        return;

    if (player->IsMounted())
        player->Dismount();

    // Some server-side mount effects can remain applied when virtual bot
    // sessions dismount mid-action. Explicitly strip mount-related aura types
    // and refresh movement rates to prevent residual mounted speed.
    player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    player->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
    player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS);
    player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK);
    player->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_FLIGHT_SPEED_ALWAYS);
    player->RemoveAurasByType(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK);

    player->UpdateSpeed(MOVE_RUN);
    player->UpdateSpeed(MOVE_SWIM);
    player->UpdateSpeed(MOVE_FLIGHT);
}

uint32 ResolveKnownSpellInChain(Player const* player, uint32 baseSpellId)
{
    if (!player || !baseSpellId)
        return 0;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
    if (!baseSpellInfo)
        return 0;

    uint32 resolvedSpellId = 0;
    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
        if (player->HasSpell(chainSpellId))
            resolvedSpellId = chainSpellId;

    return resolvedSpellId;
}

uint32 ResolveKnownPetSpellInChain(Player const* player, uint32 baseSpellId)
{
    if (!player || !baseSpellId)
        return 0;

    Pet const* pet = player->GetPet();
    if (!pet || !pet->IsAlive())
        return 0;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
    if (!baseSpellInfo)
        return 0;

    uint32 resolvedSpellId = 0;
    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
        if (pet->HasSpell(chainSpellId))
            resolvedSpellId = chainSpellId;

    return resolvedSpellId;
}

void CommandPetAttackTarget(Player* player, Unit* target)
{
    if (!player || !target || !target->IsAlive())
        return;

    Pet* pet = player->GetPet();
    if (!pet || !pet->IsAlive() || !pet->IsValidAttackTarget(target))
        return;

    if (pet->GetVictim() != target)
        pet->Attack(target, true);

    if (CharmInfo* charmInfo = pet->GetCharmInfo())
    {
        charmInfo->SetIsCommandAttack(true);
        charmInfo->SetIsAtStay(false);
        charmInfo->SetIsCommandFollow(false);
        charmInfo->SetCommandState(COMMAND_ATTACK);
    }
}

void NotifyDuelDecision(Player* player, playerbot::PvpClassSpellContext const& context, bool casted, std::string const& failureReason)
{
    if (!player || !player->duel)
        return;

    Player* opponent = player->duel->Opponent;
    if (opponent)
    {
        std::string message = "Decision: ";
        message += context.actionName ? context.actionName : "none";
        message += " | spell=" + std::to_string(context.spellId);
        message += " | target=";
        message += GetTargetModeLabel(context.targetMode);
        message += " | success=";
        message += casted ? "yes" : "no";
        message += " | reason=";
        message += context.reason ? context.reason : "none";
        message += " | fail_reason=";
        message += failureReason.empty() ? "none" : failureReason;

        player->Whisper(message, LANG_UNIVERSAL, opponent);
    }

    TC_LOG_DEBUG("playerbots.pvp.class",
        "[PvP duel] {} decision={} spell={} target={} success={} reason={} fail_reason={}",
        player->GetName(), context.actionName ? context.actionName : "none", context.spellId,
        GetTargetModeLabel(context.targetMode), casted ? "yes" : "no", context.reason ? context.reason : "none",
        failureReason.empty() ? "none" : failureReason);
}

void NotifySpellCastFailureToGameMasters(Player* bot, playerbot::PvpClassSpellContext const& context, SpellCastResult castResult)
{
    if (!bot || castResult == SPELL_CAST_OK || castResult == SPELL_FAILED_SPELL_IN_PROGRESS)
        return;

    Map* map = bot->GetMap();
    if (!map)
        return;

    EnumText const resultText = EnumUtils::ToString(castResult);
    std::ostringstream os;
    os << "[Playerbot spell-fail] bot=" << bot->GetName()
       << " guid=" << bot->GetGUID().ToString()
       << " map=" << bot->GetMapId()
       << " spell=" << context.spellId
       << " action=" << (context.actionName ? context.actionName : "none")
       << " target=" << GetTargetModeLabel(context.targetMode)
       << " result=" << resultText.Title;
    std::string const message = os.str();

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* observer = itr->GetSource();
        if (!observer || !observer->IsGameMaster())
            continue;

        bot->Whisper(message, LANG_UNIVERSAL, observer);
    }
}

void FinalizeVirtualNearTeleport(Player* player)
{
    if (!player || !player->IsBeingTeleportedNear())
        return;

    uint32 const oldZone = player->GetZoneId();
    WorldLocation dest = player->GetTeleportDest();
    float safeDestZ = dest.GetPositionZ();
    player->UpdateAllowedPositionZ(dest.GetPositionX(), dest.GetPositionY(), safeDestZ);
    dest.Relocate(dest.GetPositionX(), dest.GetPositionY(), safeDestZ, dest.GetOrientation());

    player->SetSemaphoreTeleportNear(false);
    player->UpdatePosition(dest, true);
    player->SetFallInformation(0, player->GetPositionZ());

    uint32 newZone = 0;
    uint32 newArea = 0;
    player->GetZoneAndAreaId(newZone, newArea);
    player->UpdateZone(newZone, newArea);

    if (oldZone != newZone)
    {
        if (player->pvpInfo.IsHostile)
            player->CastSpell(player, 2479, true);
        else if (player->IsPvP() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
            player->UpdatePvP(false, false);
    }

    player->ResummonPetTemporaryUnSummonedIfAny();
    player->ProcessDelayedOperations();
}

char const* GetTargetModeLabel(playerbot::PvpClassSpellContext::TargetMode mode)
{
    switch (mode)
    {
        case playerbot::PvpClassSpellContext::TargetMode::Enemy: return "enemy";
        case playerbot::PvpClassSpellContext::TargetMode::Self: return "self";
        case playerbot::PvpClassSpellContext::TargetMode::Ally: return "ally";
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default: return "none";
    }
}

bool CanIssueFollowCommands(Player const* player)
{
    if (!player || !player->IsAlive())
        return false;

    if (IsCrowdControlledForAction(player) ||
        player->HasUnitState(UNIT_STATE_ROOT) ||
        player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING))
    {
        return false;
    }

    return true;
}

void ClearActiveMovementForControlLoss(Player* player)
{
    if (!player)
        return;

    player->AttackStop();
    player->SetSelection(ObjectGuid::Empty);
    // Confused/polymorphed units need the server-driven wander movement to
    // remain intact. Clearing active movement each tick pins them in place.
    if (player->HasUnitState(UNIT_STATE_CONFUSED) || player->HasAuraType(SPELL_AURA_MOD_CONFUSE) || player->IsPolymorphed())
        return;

    if (MotionMaster* motionMaster = player->GetMotionMaster())
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
}

Unit* ResolveTarget(Player* player, playerbot::PvpClassSpellContext const& context)
{
    if (!player)
        return nullptr;

    switch (context.targetMode)
    {
        case playerbot::PvpClassSpellContext::TargetMode::Self:
            return player;
        case playerbot::PvpClassSpellContext::TargetMode::Ally:
        case playerbot::PvpClassSpellContext::TargetMode::Enemy:
            if (!context.targetGuid.IsEmpty())
                return ObjectAccessor::GetUnit(*player, context.targetGuid);
            return (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy) ? player->GetVictim() : nullptr;
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default:
            return nullptr;
    }
}

void FaceTargetForInstantCast(Player* player, Unit* target, SpellInfo const* spellInfo)
{
    if (!player || !target || !spellInfo)
        return;

    if (spellInfo->CalcCastTime() > 0)
        return;

    player->SetFacingToObject(target);
    player->SetInFront(target);
}

void RepositionDruidAfterTravelFormRecovery(Player* player)
{
    if (!player || !player->GetMap() || !CanIssueFollowCommands(player))
        return;

    Unit* nearestEnemy = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (!player->IsValidAttackTarget(candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, 20.0f))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearestEnemy = candidate;
        }
    }

    if (!nearestEnemy)
        return;

    Position destination = player->GetPosition();
    float const retreatDistance = std::max(8.0f, std::min(16.0f, nearestDistance + 6.0f));
    float const angleToEnemy = player->GetAbsoluteAngle(nearestEnemy->GetPosition());
    destination.RelocateOffset({ std::cos(angleToEnemy + static_cast<float>(M_PI)) * retreatDistance,
        std::sin(angleToEnemy + static_cast<float>(M_PI)) * retreatDistance, 0.0f, 0.0f });

    if (RequiresStrictHumanPathing(player))
        IssueStrictHumanMove(player, destination);
    else
        player->GetMotionMaster()->MovePoint(0, BuildCollisionSafeDestination(player, destination), true);
}

bool CastDirectSpell(Player* player, playerbot::PvpClassSpellContext const& context, std::string& failureReason)
{
    failureReason.clear();

    if (!player || !context.spellId)
    {
        failureReason = "missing_spell";
        return false;
    }

    uint32 resolvedSpellId = ResolveKnownSpellInChain(player, context.spellId);
    bool castFromPet = false;
    Pet* petCaster = nullptr;
    if (!resolvedSpellId)
    {
        resolvedSpellId = ResolveKnownPetSpellInChain(player, context.spellId);
        if (resolvedSpellId)
        {
            petCaster = player->GetPet();
            castFromPet = petCaster && petCaster->IsAlive();
        }
    }

    if (!resolvedSpellId)
    {
        failureReason = "missing_spell";
        return false;
    }

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(resolvedSpellId);
    if (!spellInfo)
    {
        failureReason = "spell_info_missing";
        return false;
    }

    Unit* target = ResolveTarget(player, context);
    if ((!target || !target->IsAlive()))
    {
        failureReason = "target_invalid_or_dead";
        return false;
    }

    if (castFromPet)
    {
        if (!petCaster)
        {
            failureReason = "pet_missing";
            return false;
        }

        if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
        {
            if (!petCaster->IsValidAttackTarget(target, spellInfo))
            {
                failureReason = "invalid_enemy_target";
                return false;
            }

            if (!petCaster->IsWithinLOSInMap(target))
            {
                failureReason = "no_los";
                return false;
            }

            float const maxRange = spellInfo->GetMaxRange(false);
            if (maxRange > 0.0f && !petCaster->IsWithinDistInMap(target, maxRange))
            {
                failureReason = "out_of_range";
                return false;
            }
        }

        SpellCastResult const petCastResult = petCaster->CastSpell(target, resolvedSpellId, false);
        if (petCastResult != SPELL_CAST_OK)
        {
            failureReason = "pet_cast_failed";
            return false;
        }

        return true;
    }

    if (IsCrowdControlledForAction(player))
    {
        // Hard crowd-control gate: polymorphed/confused actors must not start
        // attacks or cast attempts until control is restored.
        failureReason = "crowd_controlled_polymorph";
        return false;
    }

    // Druids can intentionally swap into Bear Form under melee pressure, but
    // many follow-up heals/utility spells are not castable in Bear/Cat forms.
    // The random bot cadence evaluates roughly every 2 seconds, so waiting for
    // the next tick to leave form makes bots appear locked out. If the selected
    // spell is blocked only by current shapeshift state, immediately cancel the
    // form and continue with the same cast attempt in this tick.
    if (player->GetClass() == CLASS_DRUID && player->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
        if (spellInfo->CheckShapeshift(player->GetShapeshiftForm()) == SPELL_FAILED_NOT_SHAPESHIFT)
            player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

    if (player->GetSpellHistory()->HasCooldown(resolvedSpellId) ||
        player->GetSpellHistory()->HasGlobalCooldown(spellInfo) ||
        player->IsNonMeleeSpellCast(false, false, true))
    {
        failureReason = "cooldown_or_casting";
        return false;
    }

    bool const isTemporaryWeaponImbue = [&spellInfo]()
    {
        for (SpellEffectInfo const& effectInfo : spellInfo->GetEffects())
            if (effectInfo.Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                return true;

        return false;
    }();

    Item* itemTarget = nullptr;
    if (resolvedSpellId == 11202 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
    {
        Item* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
        if (mainHand && !mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            itemTarget = mainHand;
        else
        {
            Item* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
            if (offHand && !offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                itemTarget = offHand;
        }

        if (!itemTarget)
        {
            failureReason = "weapon_already_poisoned";
            return false;
        }
    }
    else if (isTemporaryWeaponImbue && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
    {
        Item* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
        if (mainHand && !mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            itemTarget = mainHand;
        else
        {
            Item* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
            if (offHand && !offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                itemTarget = offHand;
        }

        if (!itemTarget)
        {
            failureReason = "weapon_already_imbued";
            return false;
        }
    }

    if ((!target || !target->IsAlive()) && !itemTarget)
    {
        failureReason = "target_invalid_or_dead";
        return false;
    }
    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self && target != player)
    {
        failureReason = "self_target_mismatch";
        return false;
    }

    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
        target &&
        player->IsValidAttackTarget(target) &&
        (player->GetVictim() != target || !player->IsInCombat()))
    {
        // Establish combat relationship before hostile casts so bots do not
        // repeatedly select enemy spells while staying idle out of combat.
        player->Attack(target, false);
    }

    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
    {
        if (!player->IsValidAttackTarget(target, spellInfo))
        {
            failureReason = "invalid_enemy_target";
            return false;
        }

        // Keep explicit enemy selection/victim linkage for virtual sessions so
        // cast checks and AI follow-up consistently reference the same hostile.
        player->SetSelection(target->GetGUID());
        bool const preserveStealthForOpener = player->HasStealthAura();
        if (preserveStealthForOpener)
        {
            // While stealthed, keep auto-attack disabled so we do not break
            // stealth early, but continue issuing movement so rogues still
            // close to opener distance instead of idling in place.
            //
            // AttackStop can clear active movement intent in some states, so
            // only call it when we are actively swinging a victim.
            if (player->GetVictim())
                player->AttackStop();

            if (CanIssueFollowCommands(player))
                IssueStealthOpenerMovement(player, target);
        }
        else if (player->GetVictim() != target)
            player->Attack(target, false);
        CommandPetAttackTarget(player, target);

        // Virtual sessions can visually "turn" while server-side facing checks
        // still fail for the immediate cast tick. SetInFront updates orientation
        // instantly, so facing-sensitive spells pass UNIT_NOT_INFRONT checks.
        player->SetFacingToObject(target);
        player->SetInFront(target);
    }
    else if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
    {
        if (!IsFriendlySupportTarget(player, target, spellInfo))
        {
            failureReason = "invalid_ally_target";
            return false;
        }
    }
    else if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::None)
    {
        failureReason = "target_mode_none";
        return false;
    }

    float const maxRange = spellInfo->GetMaxRange(false);
    bool const shouldUseMeleeApproachForEnemySpell =
        context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
        IsPrimaryMeleeClassForSpacing(player->GetClass()) &&
        maxRange > 0.0f && maxRange <= 5.5f;
    if (!itemTarget && !player->IsWithinLOSInMap(target))
    {
        if (CanIssueFollowCommands(player))
        {
            if (shouldUseMeleeApproachForEnemySpell)
                IssueMeleeApproachMovement(player, target);
            else
            {
                // Healing/support LOS recovery should collapse closer than DPS
                // spacing so bots can actually peek around pillars instead of
                // trying to hold long cast distance on allies.
                float const desiredRange = (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
                    ? std::max(1.5f, std::min(8.0f, maxRange > 0.0f ? (maxRange - 1.0f) : 8.0f))
                    : ComputeLosRecoveryRange(player, target, maxRange);
                IssueRangedApproachMovement(player, target, desiredRange, true, "precast_generic_no_los");
            }
        }

        failureReason = "no_los";
        return false;
    }

    if (!itemTarget && maxRange > 0.0f && !player->IsWithinDistInMap(target, maxRange))
    {
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

        // When we are trying to cast but are still out of range, proactively
        // close the gap instead of idling and repeating failed cast attempts.
        if (CanIssueFollowCommands(player) && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
        {
            if (shouldUseMeleeApproachForEnemySpell)
                IssueMeleeApproachMovement(player, target);
            else
            {
                float const desiredRange = std::max(1.0f, maxRange - 1.0f);
                IssueRangedApproachMovement(player, target, desiredRange);
            }
        }
        else if (CanIssueFollowCommands(player) && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
        {
            float const desiredRange = std::max(1.0f, maxRange - 1.0f);
            player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
        }

        failureReason = "out_of_range";
        return false;
    }

    float const minRange = spellInfo->GetMinRange(false);
    if (!itemTarget && minRange > 0.0f && player->IsWithinDistInMap(target, minRange))
    {
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

        // Mirror reference-style spacing control for ranged casts: when too close,
        // immediately re-establish spell distance instead of repeatedly failing.
        if (CanIssueFollowCommands(player) && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
        {
            float const desiredRange = std::max(1.0f, minRange + 1.0f);
            player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
        }
        else if (CanIssueFollowCommands(player) && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
        {
            float const desiredRange = std::max(1.0f, minRange + 1.0f);
            player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
        }

        failureReason = "too_close";
        return false;
    }

    // If a previous movement recovery left an idle chase/follow generator installed
    // but the spell is now valid from the current position, clear that stale
    // generator before attempting the cast. This prevents bots from visibly
    // holding a CHASE/FOLLOW motion while the class action is already in range.
    if (!itemTarget && IsSpellReadyAtCurrentPosition(player, target, spellInfo, context.targetMode))
        ClearStaleTargetRelativeMotionForCast(player, "cleared_stale_target_relative_before_cast");

    bool const isMountSpell = spellInfo->HasAura(SPELL_AURA_MOUNTED) || spellInfo->Mechanic == MECHANIC_MOUNT;

    if (isMountSpell &&
        (player->HasUnitState(UNIT_STATE_STUNNED) ||
         player->HasUnitState(UNIT_STATE_CONFUSED) ||
         player->HasUnitState(UNIT_STATE_FLEEING) ||
         player->HasUnitState(UNIT_STATE_ROOT)))
    {
        failureReason = "controlled_cannot_mount";
        return false;
    }
    if (isMountSpell && !IsStrictlyOutdoorsForMount(player))
    {
        // Enforce indoor mount denial server-side for virtual bot casters.
        // Mount selection already prefers outdoors, but execution must also
        // gate this so stale context cannot cast mounts while indoors.
        failureReason = "indoors_cannot_mount";
        return false;
    }


    // Most combat/utility spells require an unmounted caster. Dismount before
    // non-mount spell execution so bots do not keep kiting while mounted.
    if (player->IsMounted() && !isMountSpell)
        ForcePlayerbotDismount(player);

    // Food/drink should immediately break when the bot transitions into active
    // spellcasting (combat or utility), mirroring movement opcode behavior.
    if (context.spellId != SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT && context.spellId != SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK)
    {
        player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

        // Virtual sessions do not emit client stand-state opcodes before
        // casting. Explicitly stand when transitioning out of drink/eat casts
        // so bots do not remain visually seated while spellcasting.
        if (player->IsSitState())
            player->SetStandState(UNIT_STAND_STATE_STAND);
    }

    // Auto-repeat ranged attacks (e.g., wand Shoot) and Life Tap are validated
    // by the core cast pipeline and should not be blocked by this local
    // pre-check. For low-mana caster fallbacks we intentionally allow entering
    // the cast flow so the server can apply the spell-specific resource rules.
    bool const bypassPowerPrecheck = spellInfo->IsAutoRepeatRangedSpell() || IsLifeTapSpell(spellInfo);
    if (!bypassPowerPrecheck && spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
        if (player->GetPower(Powers(spellInfo->PowerType)) < int32(spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask())))
        {
            // If we cannot pay for the selected enemy spell, immediately
            // transition to melee pressure so bots do not idle while OOM.
            if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && target && CanIssueFollowCommands(player))
            {
                IssueMeleeApproachMovement(player, target);
                if (player->GetVictim() != target || !player->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    player->Attack(target, true);
            }

            failureReason = "insufficient_power";
            return false;
        }

    // Cast-time spells like Frostbolt fail while moving. Since playerbots do
    // not have client-side stop-cast behavior, explicitly stop movement before
    // attempting non-instant casts.
    bool const isFoodOrDrinkSpell = resolvedSpellId == SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT || resolvedSpellId == SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK;
    if (spellInfo->CalcCastTime() > 0 || spellInfo->IsAutoRepeatRangedSpell() || isFoodOrDrinkSpell)
    {
        player->StopMoving();
        if (WorldSession* session = player->GetSession(); session && session->IsVirtualSession())
        {
            player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            player->SendMovementFlagUpdate();
        }
    }

    // Blink (1953) is a leap-forward spell with a destination target
    // (TARGET_DEST_CASTER_FRONT_LEAP). For virtual bot sessions, casting only
    // on a unit target can leave relocation unresolved; provide an explicit
    // front destination to mirror client cast payload semantics.
    bool const isInstantCast = spellInfo->CalcCastTime() == 0;
    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && isInstantCast)
        FaceTargetForInstantCast(player, target, spellInfo);

    SpellCastResult castResult = SPELL_FAILED_ERROR;
    if (resolvedSpellId == 1953 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
    {
        Position const dest = player->GetFirstCollisionPosition(20.0f, player->GetOrientation());
        castResult = player->CastSpell(CastSpellTargetArg(dest), resolvedSpellId);
    }
    else if (itemTarget)
        castResult = player->CastSpell(CastSpellTargetArg(itemTarget), resolvedSpellId);
    else
        castResult = player->CastSpell(target, resolvedSpellId, false);

    if (castResult != SPELL_CAST_OK)
    {
        if (!itemTarget && target && CanIssueFollowCommands(player))
        {
            if (castResult == SPELL_FAILED_OUT_OF_RANGE)
            {
                if (shouldUseMeleeApproachForEnemySpell)
                    IssueMeleeApproachMovement(player, target);
                else
                {
                    float const desiredRange = maxRange > 0.0f ? std::max(1.0f, maxRange - 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().spellRange - 1.0f);
                    IssueRangedApproachMovement(player, target, desiredRange);
                }
            }
            else if (castResult == SPELL_FAILED_TOO_CLOSE)
            {
                float const desiredRange = minRange > 0.0f ? std::max(1.0f, minRange + 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().closeRange);
                player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
            }
            else if (castResult == SPELL_FAILED_LINE_OF_SIGHT)
            {
                if (shouldUseMeleeApproachForEnemySpell)
                    IssueMeleeApproachMovement(player, target);
                else
                {
                    float const desiredRange = (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
                        ? std::max(1.5f, std::min(8.0f, maxRange > 0.0f ? (maxRange - 1.0f) : 8.0f))
                        : ComputeLosRecoveryRange(player, target, maxRange);
                    IssueRangedApproachMovement(player, target, desiredRange, true, "cast_failed_spell_los");
                }
            }
        }

        NotifySpellCastFailureToGameMasters(player, context, castResult);
        EnumText const reasonText = EnumUtils::ToString(castResult);
        failureReason = reasonText.Title;
        return false;
    }

    // Hunter PvP trap setup: when Feign Death succeeds against a nearby melee
    // threat, pause movement, clear explicit target selection for visual parity,
    // then cast Freezing Trap exactly 500ms later before resuming chase.
    if (context.spellId == 5384)
    {
        Unit* pressureTarget = nullptr;
        if (!context.targetGuid.IsEmpty())
            pressureTarget = ObjectAccessor::GetUnit(*player, context.targetGuid);
        if (!pressureTarget)
            pressureTarget = player->GetVictim();

        bool const closeMeleePressure = pressureTarget && pressureTarget->IsAlive() && player->IsWithinDistInMap(pressureTarget, 5.0f);
        if (closeMeleePressure && player->HasSpell(1499))
        {
            ObjectGuid const hunterGuid = player->GetGUID();
            ObjectGuid const pressureTargetGuid = pressureTarget->GetGUID();

            player->StopMoving();
            player->SetSelection(ObjectGuid::Empty);

            player->m_Events.AddEventAtOffset([hunterGuid, pressureTargetGuid]()
            {
                Player* hunter = ObjectAccessor::FindConnectedPlayer(hunterGuid);
                if (!hunter || !hunter->IsInWorld() || !hunter->IsAlive())
                    return;

                SpellInfo const* freezingTrapInfo = sSpellMgr->GetSpellInfo(1499);
                if (freezingTrapInfo &&
                    !hunter->GetSpellHistory()->HasCooldown(1499) &&
                    !hunter->GetSpellHistory()->HasGlobalCooldown(freezingTrapInfo) &&
                    !hunter->IsNonMeleeSpellCast(false, false, true))
                {
                    SpellCastResult const trapCastResult = hunter->CastSpell(hunter, 1499, false);
                    if (trapCastResult != SPELL_CAST_OK)
                        TC_LOG_DEBUG("playerbots.pvp.class", "Hunter trap follow-up failed after feign death delay: guid={}, result={}.", hunter->GetGUID().ToString(), uint32(trapCastResult));
                }

                if (Unit* resumedTarget = ObjectAccessor::GetUnit(*hunter, pressureTargetGuid))
                    if (resumedTarget->IsAlive())
                        hunter->Attack(resumedTarget, false);
            }, std::chrono::milliseconds(500));
        }
    }

    bool hasTeleportEffect = false;
    bool hasChargeEffect = false;
    for (uint8 effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
    {
        switch (spellInfo->GetEffect(SpellEffIndex(effectIndex)).Effect)
        {
            case SPELL_EFFECT_TELEPORT_UNITS:
            case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
            case SPELL_EFFECT_LEAP:
            case SPELL_EFFECT_JUMP:
            case SPELL_EFFECT_JUMP_DEST:
            case SPELL_EFFECT_LEAP_BACK:
            case SPELL_EFFECT_CHARGE:
            case SPELL_EFFECT_CHARGE_DEST:
                hasTeleportEffect = true;
                hasChargeEffect = true;
                break;
            default:
                break;
        }

        if (hasTeleportEffect)
            break;
    }

    // Bot players do not own a real game client to naturally ACK near teleports
    // (for example Blink). If a teleport is still pending after cast, synthesize
    // the teleport ACK immediately so other combat actions (like Charge) resolve
    // against the post-Blink location.
    if (hasTeleportEffect && player->IsBeingTeleportedNear())
    {
        WorldSession* session = player->GetSession();
        if (session && session->IsVirtualSession())
        {
            TC_LOG_DEBUG("playerbots.pvp.class",
                "Playerbot PvP teleport ACK synthesized: guid={} spell={} map={} x={} y={} z={}.",
                player->GetGUID().ToString(), resolvedSpellId, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            WorldPacket teleportAck(MSG_MOVE_TELEPORT_ACK, 20);
            teleportAck << player->GetPackGUID();
            teleportAck << uint32(0);
            teleportAck << uint32(0);
            session->HandleMoveTeleportAck(teleportAck);

            if (player->IsBeingTeleportedNear())
                FinalizeVirtualNearTeleport(player);
        }
    }

    // Charge/Intercept target switching: when bots are already attacking one
    // unit and gap-close a different unit, preserve the charge destination by
    // immediately promoting the spell target to combat/selection context.
    // Otherwise downstream pursuit logic can snap movement back to the old
    // victim in the same tick, which looks like "stun/sound with no charge".
    if (hasChargeEffect &&
        context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
        target && target->IsAlive())
    {
        player->SetSelection(target->GetGUID());
        if (player->GetVictim() != target || !player->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            player->Attack(target, true);
    }

    // Avoid immediate reapplication loops after quick dispels by imposing
    // short tactical cooldowns on selected PvP debuffs.
    if (context.spellId == 112826)
        player->GetSpellHistory()->AddCooldown(context.spellId, 0, std::chrono::seconds(15));
    if (context.spellId == 3034 || context.spellId == 11719 || context.spellId == 11713)
    {
        if (uint32 const resolvedSpellId = ResolveKnownSpellInChain(player, context.spellId))
            player->GetSpellHistory()->AddCooldown(resolvedSpellId, 0, std::chrono::seconds(12));
    }
    if (context.spellId == 12323)
    {
        if (uint32 const resolvedSpellId = ResolveKnownSpellInChain(player, context.spellId))
            player->GetSpellHistory()->AddCooldown(resolvedSpellId, 0, std::chrono::seconds(8));
    }

    if ((context.spellId == 11719 || context.spellId == 11713) && target)
        playerbot::PvpClassActions::RegisterWarlockCurseTargetCooldown(player, target, context.spellId, std::chrono::seconds(12));
    if (context.spellId == 6940)
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, context.spellId, std::chrono::seconds(10));

    // Warlock curse openers are instant and can leave the bot with an idle
    // motion generator while still in combat against a moving target. Re-issue
    // ranged approach pressure so follow-up casts do not stall.
    if ((context.spellId == 11719 || context.spellId == 11713) &&
        context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
        target && target->IsAlive() && CanIssueFollowCommands(player))
    {
        float const desiredRange = maxRange > 0.0f ? std::max(1.0f, maxRange - 3.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().spellRange - 1.0f);
        IssueRangedApproachMovement(player, target, desiredRange);
    }

    return true;
}

bool UseDirectItem(Player* player, playerbot::PvpClassSpellContext const& context, std::string& failureReason)
{
    failureReason.clear();
    if (!player || !context.itemEntry)
    {
        failureReason = "missing_item_entry";
        return false;
    }

    Item* item = player->GetItemByEntry(context.itemEntry);
    if (!item)
    {
        failureReason = "item_missing";
        return false;
    }

    if (player->CanUseItem(item) != EQUIP_ERR_OK)
    {
        failureReason = "item_unusable";
        return false;
    }

    SpellCastTargets targets;
    targets.SetUnitTarget(player);
    player->CastItemUseSpell(item, targets, 1, 0);
    return true;
}
}

namespace playerbot
{
bool PvpClassActions::IsWarlockCurseTargetCooldownActive(Player const* player, Unit const* target, uint32 spellId)
{
    if (!player || !target || !spellId)
        return false;

    WarlockCurseCooldownKey const key{ player->GetGUID(), target->GetGUID(), spellId };
    auto const itr = g_WarlockCurseTargetCooldowns.find(key);
    if (itr == g_WarlockCurseTargetCooldowns.end())
        return false;

    if (GameTime::Now() >= itr->second)
    {
        g_WarlockCurseTargetCooldowns.erase(itr);
        return false;
    }

    return true;
}

void PvpClassActions::RegisterWarlockCurseTargetCooldown(Player const* player, Unit const* target, uint32 spellId, std::chrono::seconds cooldown)
{
    if (!player || !target || !spellId || cooldown <= std::chrono::seconds::zero())
        return;

    g_WarlockCurseTargetCooldowns[{ player->GetGUID(), target->GetGUID(), spellId }] = GameTime::Now() + cooldown;
}

bool PvpClassActions::IsCasterSpellCooldownActive(Player const* player, uint32 spellId)
{
    if (!player || !spellId)
        return false;

    CasterSpellCooldownKey const key{ player->GetGUID(), spellId };
    auto const itr = g_CasterSpellCooldowns.find(key);
    if (itr == g_CasterSpellCooldowns.end())
        return false;

    if (GameTime::Now() >= itr->second)
    {
        g_CasterSpellCooldowns.erase(itr);
        return false;
    }

    return true;
}

void PvpClassActions::RegisterCasterSpellCooldown(Player const* player, uint32 spellId, std::chrono::seconds cooldown)
{
    if (!player || !spellId || cooldown <= std::chrono::seconds::zero())
        return;

    g_CasterSpellCooldowns[{ player->GetGUID(), spellId }] = GameTime::Now() + cooldown;
}

std::string PvpClassActions::GetLastExecutionStatus(Player const* player)
{
    if (!player)
        return "none";

    auto const itr = g_LastClassExecutionStatusByGuid.find(player->GetGUID().GetRawValue());
    if (itr == g_LastClassExecutionStatusByGuid.end())
        return "none";

    return itr->second;
}

std::string PvpClassActions::GetLastMovementDebugStatus(Player const* player)
{
    if (!player)
        return "none";

    auto const itr = g_LastMovementDebugStatusByGuid.find(player->GetGUID().GetRawValue());
    if (itr == g_LastMovementDebugStatusByGuid.end())
        return "none";

    return itr->second;
}

bool PvpClassActions::Execute(Player* player, PvpClassSpellContext const& context)
{
    if (!player || !context.classSpellsEnabled || !context.shouldExecute)
        return false;

    bool const hasCastIntent = context.spellId != 0 || context.itemEntry != 0;
    bool const shouldExecuteMovementBeforeCast =
        !hasCastIntent || (
            context.movementDirective != PvpClassSpellContext::MovementDirective::ReachMeleeRange &&
            context.movementDirective != PvpClassSpellContext::MovementDirective::ReachSpellRange &&
            context.movementDirective != PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell &&
            context.movementDirective != PvpClassSpellContext::MovementDirective::FaceSpellTarget);

    if (context.movementDirective != PvpClassSpellContext::MovementDirective::None && shouldExecuteMovementBeforeCast)
    {
        if (ShouldThrottleDirective(player, context))
        {
            MovementGeneratorType const movementType = player->GetMotionMaster()
                ? player->GetMotionMaster()->GetCurrentMovementGeneratorType()
                : IDLE_MOTION_TYPE;
            std::ostringstream diag;
            diag << "directive_throttled"
                 << " directive=" << uint32(context.movementDirective)
                 << " moving=" << (player->isMoving() ? "yes" : "no")
                 << " motion=" << uint32(movementType);
            SetLastMovementDebugStatus(player, diag.str());
            SetLastExecutionStatus(player, "move_throttled");
            return true;
        }

        Unit* movementTarget = context.movementTargetGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*player, context.movementTargetGuid);
        bool const directiveNeedsTarget =
            context.movementDirective == PvpClassSpellContext::MovementDirective::ReachMeleeRange ||
            context.movementDirective == PvpClassSpellContext::MovementDirective::ReachSpellRange ||
            context.movementDirective == PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell ||
            context.movementDirective == PvpClassSpellContext::MovementDirective::FaceSpellTarget;
        if (directiveNeedsTarget && (!movementTarget || !movementTarget->IsAlive()))
        {
            // Defensive fallback: if GUID resolution fails for this tick, use
            // currently selected/victim targets so movement directives do not
            // silently drop to idle.
            if (context.movementDirective != PvpClassSpellContext::MovementDirective::FaceSpellTarget)
            {
                if (Unit* selectedTarget = player->GetSelectedUnit(); selectedTarget && selectedTarget->IsAlive())
                    movementTarget = selectedTarget;
                else if (Unit* victimTarget = player->GetVictim(); victimTarget && victimTarget->IsAlive())
                    movementTarget = victimTarget;
            }
        }
        if (directiveNeedsTarget && (!movementTarget || !movementTarget->IsAlive()))
        {
            SetLastExecutionStatus(player, "move_skipped_target_invalid");
            return false;
        }

        if (directiveNeedsTarget && !CanIssueFollowCommands(player))
        {
            if (IsCrowdControlledForAction(player))
                ClearActiveMovementForControlLoss(player);
            SetLastExecutionStatus(player, "move_skipped_cannot_follow");
            return false;
        }

        // Re-facing while a non-melee spellcast is in progress can interrupt
        // channels/cast bars and manifests as abrupt facing flips. Defer this
        // directive until cast completion.
        if (context.movementDirective == PvpClassSpellContext::MovementDirective::FaceSpellTarget &&
            player->IsNonMeleeSpellCast(false, false, true))
        {
            SetLastExecutionStatus(player, "move_skipped_face_while_casting");
            return true;
        }

        switch (context.movementDirective)
        {
            case PvpClassSpellContext::MovementDirective::ReachMeleeRange:
            {
                IssueMeleeApproachMovement(player, movementTarget);
            }
                break;
            case PvpClassSpellContext::MovementDirective::ReachSpellRange:
            {
                float desiredRange = std::max(1.0f,
                    context.movementFollowRange > 0.0f ? context.movementFollowRange : (PvpCore::GetConfig().spellRange - 1.0f));
                Unit* approachTarget = movementTarget;
                if (approachTarget && !player->IsValidAttackTarget(approachTarget))
                {
                    // Defensive fallback: spacing directives are intended to
                    // close on hostile casts. If the preserved movement target
                    // is no longer attackable for this tick, fall back to
                    // current hostile context so ranged casters do not idle.
                    if (Unit* selectedTarget = player->GetSelectedUnit(); selectedTarget && selectedTarget->IsAlive() && player->IsValidAttackTarget(selectedTarget))
                        approachTarget = selectedTarget;
                    else if (Unit* victimTarget = player->GetVictim(); victimTarget && victimTarget->IsAlive() && player->IsValidAttackTarget(victimTarget))
                        approachTarget = victimTarget;
                }
                if (approachTarget)
                {
                    float const currentDistance = player->GetDistance(approachTarget);
                    // ReachSpellRange must always request an actual "close the
                    // gap" distance. If desiredRange is >= current distance,
                    // chase movement can idle and repeatedly reissue the same
                    // directive (visible as bow-raise stutter loops).
                    if (desiredRange >= currentDistance)
                    {
                        float const closingRange = std::max(1.0f, currentDistance - 2.0f);
                        desiredRange = std::min(desiredRange, closingRange);
                    }
                }
                IssueRangedApproachMovement(player, approachTarget, desiredRange);
            }
                break;
            case PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell:
            {
                Position destination = player->GetPosition();
                float const fleeDistance = std::max(1.0f,
                    context.movementFollowRange > 0.0f ? context.movementFollowRange : PvpCore::GetConfig().closeRange);
                float const angleToTarget = player->GetAbsoluteAngle(movementTarget->GetPosition());
                destination.RelocateOffset({ std::cos(angleToTarget + static_cast<float>(M_PI)) * fleeDistance,
                    std::sin(angleToTarget + static_cast<float>(M_PI)) * fleeDistance, 0.0f, 0.0f });
                if (RequiresStrictHumanPathing(player))
                {
                    if (!IssueStrictHumanMove(player, destination))
                    {
                        // Strict segment pathing can fail to resolve around
                        // battleground geometry. Fall back to a direct point
                        // move so flee directives never devolve into idle.
                        MotionMaster* fallbackMotionMaster = player->GetMotionMaster();
                        if (fallbackMotionMaster)
                            fallbackMotionMaster->MovePoint(0, BuildCollisionSafeDestination(player, destination), true);
                    }
                }
                else
                {
                    MotionMaster* fallbackMotionMaster = player->GetMotionMaster();
                    if (fallbackMotionMaster)
                        fallbackMotionMaster->MovePoint(0, BuildCollisionSafeDestination(player, destination), true);
                }
                break;
            }
            case PvpClassSpellContext::MovementDirective::FaceSpellTarget:
                player->SetFacingToObject(movementTarget);
                player->SetInFront(movementTarget);
                break;
            case PvpClassSpellContext::MovementDirective::DropInvalidTarget:
                player->SetSelection(ObjectGuid::Empty);
                player->AttackStop();
                break;
            case PvpClassSpellContext::MovementDirective::CheckMountState:
                if (player->IsMounted())
                    ForcePlayerbotDismount(player);
                break;
            case PvpClassSpellContext::MovementDirective::ResetCombatState:
                player->SetSelection(ObjectGuid::Empty);
                player->AttackStop();
                player->CombatStop(true);
                break;
            case PvpClassSpellContext::MovementDirective::None:
            default:
                break;
        }

        TC_LOG_DEBUG("playerbots.pvp.class",
            "Playerbot PvP movement directive executed: action={} target_guid={} directive={}.",
            context.actionName ? context.actionName : "none",
            movementTarget ? movementTarget->GetGUID().ToString() : ObjectGuid::Empty.ToString(),
            static_cast<uint8>(context.movementDirective));
        SetLastExecutionStatus(player, "move_executed");
        return true;
    }

    // Recovery guard: after LOS-related cast failures, reserve a tick for
    // explicit re-positioning before retrying the same cast. Without this,
    // caster bots can repeatedly fail with LOS while remaining idle when
    // class-selection keeps returning a spell action without a move directive.
    if (hasCastIntent &&
        context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
        CanIssueFollowCommands(player))
    {
        std::string const lastStatus = GetLastExecutionStatus(player);
        bool const previousLosFailure =
            lastStatus == "cast_failed_no_los" ||
            lastStatus == "cast_failed_SPELL_FAILED_LINE_OF_SIGHT";

        if (previousLosFailure && !context.targetGuid.IsEmpty())
        {
            if (Unit* recoveryTarget = ObjectAccessor::GetUnit(*player, context.targetGuid); recoveryTarget && recoveryTarget->IsAlive())
            {
                uint32 resolvedSpellId = context.spellId;
                if (context.spellId)
                {
                    if (uint32 knownSpell = ResolveKnownSpellInChain(player, context.spellId))
                        resolvedSpellId = knownSpell;
                }

                SpellInfo const* spellInfo = resolvedSpellId ? sSpellMgr->GetSpellInfo(resolvedSpellId) : nullptr;
                float const maxRange = spellInfo ? spellInfo->GetMaxRange(false) : 0.0f;
                bool const genericReady = spellInfo && IsSpellReadyAtCurrentPosition(player, recoveryTarget, spellInfo, context.targetMode);
                bool const enemyMeleeSpacing =
                    context.targetMode == PvpClassSpellContext::TargetMode::Enemy &&
                    IsPrimaryMeleeClassForSpacing(player->GetClass()) &&
                    maxRange > 0.0f && maxRange <= 5.5f;

                // Do not skip LOS recovery just because IsWithinLOSInMap() says
                // the target is visible. The previous server cast result was
                // SPELL_FAILED_LINE_OF_SIGHT, and Spell::CheckCast can disagree
                // with the generic debug LOS check on custom map/vmap geometry.
                if (enemyMeleeSpacing)
                    IssueMeleeApproachMovement(player, recoveryTarget);
                else
                {
                    float const desiredRange = (context.targetMode == PvpClassSpellContext::TargetMode::Ally)
                        ? std::max(1.5f, std::min(8.0f, maxRange > 0.0f ? (maxRange - 1.0f) : 8.0f))
                        : ComputeLosRecoveryRange(player, recoveryTarget, maxRange);
                    IssueRangedApproachMovement(player, recoveryTarget, desiredRange, true,
                        genericReady ? "previous_spell_los_generic_ready" : "previous_spell_los_generic_blocked");
                }

                SetLastExecutionStatus(player, "move_recover_los");
                return true;
            }
        }
    }

    std::string failureReason;
    bool casted = false;
    if (context.itemEntry)
        casted = UseDirectItem(player, context, failureReason);
    else
        casted = CastDirectSpell(player, context, failureReason);
    NotifyDuelDecision(player, context, casted, failureReason);
    TC_LOG_DEBUG("playerbots.pvp.class",
        "Playerbot PvP class execution: action={} spell={} target_mode={} target_guid={} success={} reason={}.",
        context.actionName ? context.actionName : "none",
        context.spellId,
        GetTargetModeLabel(context.targetMode),
        context.targetGuid.ToString(),
        casted,
        context.reason ? context.reason : "none");
    if (casted)
    {
        if (context.spellId == 783 && context.reason && std::string_view(context.reason) == "recovering from polymorph by travel-form reposition")
            RepositionDruidAfterTravelFormRecovery(player);
        SetLastExecutionStatus(player, "cast_executed");
    }
    else
        SetLastExecutionStatus(player, "cast_failed_" + failureReason);
    return casted;
}
}
