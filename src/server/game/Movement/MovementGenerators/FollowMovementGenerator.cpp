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

#include "FollowMovementGenerator.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Log.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Optional.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Unit.h"
#include "Util.h"

static void DoMovementInform(Unit* owner, Unit* target)
{
    if (owner->GetTypeId() != TYPEID_UNIT)
        return;

    if (CreatureAI* AI = owner->ToCreature()->AI())
        AI->MovementInform(FOLLOW_MOTION_TYPE, target->GetGUID().GetCounter());
}

FollowMovementGenerator::FollowMovementGenerator(Unit* target, float range, ChaseAngle angle) : AbstractFollower(ASSERT_NOTNULL(target)), _range(range), _angle(angle), _checkTimer(CHECK_INTERVAL)
{
    Mode = MOTION_MODE_DEFAULT;
    Priority = MOTION_PRIORITY_NORMAL;
    Flags = MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING;
    BaseUnitState = UNIT_STATE_FOLLOW;
}
FollowMovementGenerator::~FollowMovementGenerator() = default;

static bool PositionOkay(Unit* owner, Unit* target, float range, Optional<ChaseAngle> angle = {})
{
    if (owner->GetExactDistSq(target) > square(owner->GetCombatReach() + target->GetCombatReach() + range))
        return false;

    return !angle || angle->IsAngleOkay(target->GetRelativeAngle(owner));
}

static bool ShouldLogPlayerbotFollow(Unit const* owner)
{
    return owner && owner->GetTypeId() == TYPEID_PLAYER;
}

static void LogPlayerbotFollowDiag(Unit* owner, Unit* target, char const* phase, float range, ChaseAngle const& angle)
{
    if (!ShouldLogPlayerbotFollow(owner) || !target)
        return;

    float const hitboxSum = owner->GetCombatReach() + target->GetCombatReach();
    bool const positionOkay = PositionOkay(owner, target, range, angle);

    TC_LOG_DEBUG("playerbots.pvp.classspell",
        "PB follow diag: phase={} owner={} target={} edge_dist={} exact_dist={} hitbox_sum={} range={} max_exact={} position_ok={} moving={} not_move={} casting_prevent={} follow_move={} spline_finalized={} angle_ok={}.",
        phase ? phase : "unknown",
        owner->GetGUID().ToString(), target->GetGUID().ToString(),
        owner->GetDistance(target), owner->GetExactDist(target), hitboxSum,
        range, range + hitboxSum, positionOkay, owner->isMoving(),
        owner->HasUnitState(UNIT_STATE_NOT_MOVE),
        owner->IsMovementPreventedByCasting(),
        owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE),
        owner->movespline->Finalized(),
        angle.IsAngleOkay(target->GetRelativeAngle(owner)));
}

void FollowMovementGenerator::Initialize(Unit* owner)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING | MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    AddFlag(MOVEMENTGENERATOR_FLAG_INITIALIZED | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

    owner->StopMoving();
    UpdatePetSpeed(owner);
    _path = nullptr;
    _lastTargetPosition.reset();
}

void FollowMovementGenerator::Reset(Unit* owner)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);

    Initialize(owner);
}

bool FollowMovementGenerator::Update(Unit* owner, uint32 diff)
{
    // owner might be dead or gone
    if (!owner || !owner->IsAlive())
        return false;

    // our target might have gone away
    Unit* const target = GetTarget();
    if (!target || !target->IsInWorld())
        return false;

    if (owner->HasUnitState(UNIT_STATE_NOT_MOVE) || owner->IsMovementPreventedByCasting())
    {
        LogPlayerbotFollowDiag(owner, target, "stop_not_move_or_casting", _range, _angle);
        _path = nullptr;
        owner->StopMoving();
        _lastTargetPosition.reset();
        return true;
    }

    _checkTimer.Update(diff);
    if (_checkTimer.Passed())
    {
        _checkTimer.Reset(CHECK_INTERVAL);
        if (HasFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED) && PositionOkay(owner, target, _range, _angle))
        {
            LogPlayerbotFollowDiag(owner, target, "range_timer_position_ok_stop", _range, _angle);
            RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
            _path = nullptr;
            owner->StopMoving();
            _lastTargetPosition.reset();
            DoMovementInform(owner, target);
            return true;
        }
    }

    if (owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) && owner->movespline->Finalized())
    {
        LogPlayerbotFollowDiag(owner, target, "spline_finalized", _range, _angle);
        RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
        _path = nullptr;
        owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
        DoMovementInform(owner, target);
    }

    if (!_lastTargetPosition || _lastTargetPosition->GetExactDistSq(target->GetPosition()) > 0.0f)
    {
        _lastTargetPosition = target->GetPosition();
        bool const needsRepath = owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) || !PositionOkay(owner, target, _range + FOLLOW_RANGE_TOLERANCE);
        if (!needsRepath)
            LogPlayerbotFollowDiag(owner, target, "target_changed_position_ok_no_repath", _range + FOLLOW_RANGE_TOLERANCE, _angle);

        if (needsRepath)
        {
            LogPlayerbotFollowDiag(owner, target, "target_changed_repath_needed", _range + FOLLOW_RANGE_TOLERANCE, _angle);
            if (!_path)
                _path = std::make_unique<PathGenerator>(owner);

            float x, y, z;

            // select angle
            float tAngle;
            float const curAngle = target->GetRelativeAngle(owner);
            if (_angle.IsAngleOkay(curAngle))
                tAngle = curAngle;
            else
            {
                float const diffUpper = Position::NormalizeOrientation(curAngle - _angle.UpperBound());
                float const diffLower = Position::NormalizeOrientation(_angle.LowerBound() - curAngle);
                if (diffUpper < diffLower)
                    tAngle = _angle.UpperBound();
                else
                    tAngle = _angle.LowerBound();
            }

            target->GetNearPoint(owner, x, y, z, _range, target->ToAbsoluteAngle(tAngle));

            if (owner->IsHovering())
                owner->UpdateAllowedPositionZ(x, y, z);

            if (owner->GetTransport() && owner->GetTransport() == target->GetTransport())
            {
                LogPlayerbotFollowDiag(owner, target, "same_transport_direct_move", _range, _angle);
                owner->AddUnitState(UNIT_STATE_FOLLOW_MOVE);
                AddFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

                Movement::MoveSplineInit init(owner);
                init.MoveTo(x, y, z, false);
                init.SetWalk(target->IsWalking());
                init.SetFacing(target->GetOrientation());
                init.Launch();
                return true;
            }

            // pets are allowed to "cheat" on pathfinding when following their master
            bool allowShortcut = false;
            if (Pet* oPet = owner->ToPet())
            {
                if (target->GetGUID() == oPet->GetOwnerGUID())
                    allowShortcut = true;
            }

            bool success = _path->CalculatePath(x, y, z, allowShortcut);
            TC_LOG_DEBUG("playerbots.pvp.classspell",
                "PB follow path: owner={} target={} path_ok={} path_type={} points={} dest=({}, {}, {}) edge_dist={} exact_dist={} range={}.",
                owner->GetGUID().ToString(), target->GetGUID().ToString(), success,
                uint32(_path->GetPathType()), uint32(_path->GetPath().size()), x, y, z,
                owner->GetDistance(target), owner->GetExactDist(target), _range);

            if (!success || (_path->GetPathType() & PATHFIND_NOPATH))
            {
                LogPlayerbotFollowDiag(owner, target, "path_failed_stop", _range, _angle);
                owner->StopMoving();
                return true;
            }

            LogPlayerbotFollowDiag(owner, target, "launching_spline", _range, _angle);
            owner->AddUnitState(UNIT_STATE_FOLLOW_MOVE);
            AddFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

            Movement::MoveSplineInit init(owner);
            init.MovebyPath(_path->GetPath());
            init.SetWalk(target->IsWalking());
            init.SetFacing(target->GetOrientation());
            init.Launch();
        }
    }
    return true;
}

void FollowMovementGenerator::Deactivate(Unit* owner)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    RemoveFlag(MOVEMENTGENERATOR_FLAG_TRANSITORY | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
    owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
}

void FollowMovementGenerator::Finalize(Unit* owner, bool active, bool/* movementInform*/)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_FINALIZED);
    if (active)
    {
        owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
        UpdatePetSpeed(owner);
    }
}

void FollowMovementGenerator::UpdatePetSpeed(Unit* owner)
{
    if (Pet* oPet = owner->ToPet())
    {
        if (!GetTarget() || GetTarget()->GetGUID() == owner->GetOwnerGUID())
        {
            oPet->UpdateSpeed(MOVE_RUN);
            oPet->UpdateSpeed(MOVE_WALK);
            oPet->UpdateSpeed(MOVE_SWIM);
        }
    }
}
