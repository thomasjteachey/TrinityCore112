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

#include "MoveSplineInit.h"
#include "Creature.h"
#include "Log.h"
#include "MoveSpline.h"
#include "MovementPacketBuilder.h"
#include "Unit.h"
#include "Player.h"
#include "PathGenerator.h"
#include "Transport.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace Movement
{
    UnitMoveType SelectSpeedType(uint32 moveFlags)
    {
        if (moveFlags & MOVEMENTFLAG_FLYING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.flight >= speed_obj.flight_back*/)
                return MOVE_FLIGHT_BACK;
            else
                return MOVE_FLIGHT;
        }
        else if (moveFlags & MOVEMENTFLAG_SWIMMING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.swim >= speed_obj.swim_back*/)
                return MOVE_SWIM_BACK;
            else
                return MOVE_SWIM;
        }
        else if (moveFlags & MOVEMENTFLAG_WALKING)
        {
            //if (speed_obj.run > speed_obj.walk)
            return MOVE_WALK;
        }
        else if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.run >= speed_obj.run_back*/)
            return MOVE_RUN_BACK;

        // Flying creatures use MOVEMENTFLAG_CAN_FLY or MOVEMENTFLAG_DISABLE_GRAVITY
        // Run speed is their default flight speed.
        return MOVE_RUN;
    }

    int32 MoveSplineInit::Launch()
    {
        MoveSpline& move_spline = *unit->movespline;

        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes (movementInfo.transport.guid is 0 in that case)
        bool transport = unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && unit->GetTransGUID();
        Location real_position;
        // there is a big chance that current position is unknown if current state is not finalized, need compute it
        // this also allows CalculatePath spline position and update map position in much greater intervals
        // Don't compute for transport movement if the unit is in a motion between two transports
        if (!move_spline.Finalized() && move_spline.onTransport == transport)
            real_position = move_spline.ComputePosition();
        else
        {
            Position const* pos;
            if (!transport)
                pos = unit;
            else
                pos = &unit->m_movementInfo.transport.pos;

            real_position.x = pos->GetPositionX();
            real_position.y = pos->GetPositionY();
            real_position.z = pos->GetPositionZ();
            real_position.orientation = unit->GetOrientation();
        }

        // should i do the things that user should do? - no.
        if (args.path.empty())
            return 0;

        // corrent first vertex
        args.path[0] = real_position;
        args.initialOrientation = real_position.orientation;
        args.flags.enter_cycle = args.flags.cyclic;
        move_spline.onTransport = transport;

        uint32 moveFlags = unit->m_movementInfo.GetMovementFlags();
        moveFlags |= MOVEMENTFLAG_SPLINE_ENABLED;

        if (!args.flags.backward)
            moveFlags = (moveFlags & ~(MOVEMENTFLAG_BACKWARD)) | MOVEMENTFLAG_FORWARD;
        else
            moveFlags = (moveFlags & ~(MOVEMENTFLAG_FORWARD)) | MOVEMENTFLAG_BACKWARD;

        if (moveFlags & MOVEMENTFLAG_ROOT)
            moveFlags &= ~MOVEMENTFLAG_MASK_MOVING;

        if (!args.HasVelocity)
        {
            // If spline is initialized with SetWalk method it only means we need to select
            // walk move speed for it but not add walk flag to unit
            uint32 moveFlagsForSpeed = moveFlags;
            if (args.walk)
                moveFlagsForSpeed |= MOVEMENTFLAG_WALKING;
            else
                moveFlagsForSpeed &= ~MOVEMENTFLAG_WALKING;

            args.velocity = unit->GetSpeed(SelectSpeedType(moveFlagsForSpeed));
            if (Creature* creature = unit->ToCreature())
                if (creature->HasSearchedAssistance())
                    args.velocity *= 0.66f;
        }

        // limit the speed in the same way the client does
        args.velocity = std::min(args.velocity, args.flags.catmullrom || args.flags.flying ? 50.0f : std::max(28.0f, unit->GetSpeed(MOVE_RUN) * 4.0f));

        if (!args.Validate(unit))
            return 0;

        unit->m_movementInfo.SetMovementFlags(moveFlags);
        move_spline.Initialize(args);

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        data << unit->GetPackGUID();
        if (transport)
        {
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << unit->GetTransGUID().WriteAsPacked();
            data << int8(unit->GetTransSeat());
        }

        PacketBuilder::WriteMonsterMove(move_spline, data);
        unit->SendMessageToSet(&data, true);

        return move_spline.Duration();
    }

    void MoveSplineInit::Stop()
    {
        MoveSpline& move_spline = *unit->movespline;

        // No need to stop if we are not moving
        if (move_spline.Finalized())
            return;

        bool transport = unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && unit->GetTransGUID();
        Location loc;
        if (move_spline.onTransport == transport)
            loc = move_spline.ComputePosition();
        else
        {
            Position const* pos;
            if (!transport)
                pos = unit;
            else
                pos = &unit->m_movementInfo.transport.pos;

            loc.x = pos->GetPositionX();
            loc.y = pos->GetPositionY();
            loc.z = pos->GetPositionZ();
            loc.orientation = unit->GetOrientation();
        }

        args.flags = MoveSplineFlag::Done;
        unit->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_SPLINE_ENABLED);
        move_spline.onTransport = transport;
        move_spline.Initialize(args);

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        data << unit->GetPackGUID();
        if (transport)
        {
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << unit->GetTransGUID().WriteAsPacked();
            data << int8(unit->GetTransSeat());
        }

        PacketBuilder::WriteStopMovement(loc, args.splineId, data);
        unit->SendMessageToSet(&data, true);
    }

    MoveSplineInit::MoveSplineInit(Unit* m) : unit(m)
    {
        args.splineId = splineIdGen.NewId();
        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes
        args.TransformForTransport = unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && unit->GetTransGUID();
        // mix existing state into new
        args.flags.canswim = unit->CanSwim();
        args.walk = unit->HasUnitMovementFlag(MOVEMENTFLAG_WALKING);
        args.flags.flying = unit->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY);
    }

    MoveSplineInit::~MoveSplineInit() = default;

    void MoveSplineInit::SetFacing(Vector3 const& spot)
    {
        TransportPathTransform transform(unit, args.TransformForTransport);
        Vector3 finalSpot = transform(spot);
        args.facing.f.x = finalSpot.x;
        args.facing.f.y = finalSpot.y;
        args.facing.f.z = finalSpot.z;
        args.flags.EnableFacingPoint();
    }

    void MoveSplineInit::SetFacing(Unit const* target)
    {
        SetFacing(target->GetGUID());
    }

    void MoveSplineInit::SetFacing(ObjectGuid const& target)
    {
        args.flags.EnableFacingTarget();
        args.facing.target = target.GetRawValue();
    }

    void MoveSplineInit::SetFacing(float angle)
    {
        if (args.TransformForTransport)
        {
            if (Unit* vehicle = unit->GetVehicleBase())
                angle -= vehicle->GetOrientation();
            else if (Transport* transport = unit->GetTransport())
                angle -= transport->GetOrientation();
        }

        args.facing.angle = G3D::wrap(angle, 0.f, (float)G3D::twoPi());
        args.flags.EnableFacingAngle();
    }

    void MoveSplineInit::MovebyPath(PointsArray const& controls, int32 path_offset)
    {
        args.path_Idx_offset = path_offset;
        args.path.resize(controls.size());
        std::transform(controls.begin(), controls.end(), args.path.begin(), TransportPathTransform(unit, args.TransformForTransport));
    }

    void MoveSplineInit::MoveTo(float x, float y, float z, bool generatePath, bool forceDestination)
    {
        MoveTo(G3D::Vector3(x, y, z), generatePath, forceDestination);
    }

    void MoveSplineInit::MoveTo(Vector3 const& dest, bool generatePath, bool forceDestination)
    {
        if (generatePath)
        {
            // Only units that actually have something driving them belong in the strict
            // arm below: a real client, or a bot session. A player-owned CREATURE has
            // neither and must path exactly like stock 3.3.5.
            //
            // Unit::SetMinion stamps both m_ControlledByPlayer (Unit.cpp:6949) and the
            // owner GUID (Unit.cpp:6945) onto every minion, so testing those alone swept
            // in every pet, guardian, charmed creature and player temp summon. Once in
            // the strict arm a rejected route yields buildStayPath() - a two-point spline
            // to the unit's own current position. That passes MoveSplineInitArgs::Validate
            // (it only checks path.size() > 1), launches, finalizes immediately, and
            // PetAI::MovementInform then latches SetIsAtStay(true)/MoveIdle(). The pet is
            // recorded as having arrived without having moved, and never retries.
            //
            // Playerbots are Player objects, so they still take the strict arm and their
            // behaviour is unchanged.
            bool const playerControlled = unit->GetTypeId() == TYPEID_PLAYER
                && (unit->IsControlledByPlayer() || unit->GetOwnerGUID().IsPlayer());
            bool serverDrivenPlayer = false;
            if (Player const* moverPlayer = unit->ToPlayer())
                if (WorldSession const* session = moverPlayer->GetSession())
                    serverDrivenPlayer = session->IsVirtualSession() || session->IsTransientPlayerSession();

            auto const buildStayPath = [&]()
            {
                args.path_Idx_offset = 0;
                args.path.resize(2);
                TransportPathTransform transform(unit, args.TransformForTransport);
                Vector3 stay(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
                args.path[1] = transform(stay);
            };

            PathGenerator path(unit);
            bool result = path.CalculatePath(dest.x, dest.y, dest.z, forceDestination);

            // Every movement order a bot receives ends here, whichever generator
            // issued it. Chase and Follow each log their own resolved path type,
            // but MovePoint - and anything else that reaches a spline directly -
            // logged nothing, which was the one blind spot left when a bot was
            // seen walking through an arena wall and neither of those two
            // generators had produced a single non-navmesh path in ~72,000
            // samples. Name the outcome here so the next occurrence identifies
            // its own cause without another live logging session.
            //
            // Off unless playerbots.movement.spline is switched on, and only
            // ever evaluated for a session with no client of its own.
            auto const logServerDrivenSplineDecision = [&](char const* outcome)
            {
                if (!serverDrivenPlayer)
                    return;

                TC_LOG_DEBUG("playerbots.movement.spline",
                    "PB spline: bot={} outcome={} calc_ok={} path_type={} navmesh={} points={} force_dest={} "
                    "from=({}, {}, {}) requested=({}, {}, {}) actual_end=({}, {}, {}).",
                    unit->GetGUID().ToString(), outcome, result ? 1 : 0,
                    uint32(path.GetPathType()), path.HasNavigationData() ? 1 : 0,
                    uint32(path.GetPath().size()), forceDestination ? 1 : 0,
                    unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
                    dest.x, dest.y, dest.z,
                    path.GetActualEndPosition().x, path.GetActualEndPosition().y, path.GetActualEndPosition().z);
            };

            if (result)
            {
                PathType const pathType = path.GetPathType();
                bool const navmeshAvailable = path.HasNavigationData();
                bool const usesUnsafePathMode = navmeshAvailable && (pathType & (PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT));

                if (!(pathType & PATHFIND_NOPATH))
                {
                    bool const strictPlayerRejectPath = playerControlled && !serverDrivenPlayer &&
                        ((pathType & PATHFIND_INCOMPLETE) || usesUnsafePathMode);
                    bool const serverDrivenPlayerRejectPath = serverDrivenPlayer && usesUnsafePathMode;

                    if (!(strictPlayerRejectPath || serverDrivenPlayerRejectPath))
                    {
                        logServerDrivenSplineDecision("accepted-navmesh-path");
                        MovebyPath(path.GetPath());
                        return;
                    }
                }

                if (serverDrivenPlayer && ((pathType & PATHFIND_NOPATH) || usesUnsafePathMode))
                {
                    logServerDrivenSplineDecision("held-position-unsafe-route");
                    buildStayPath();
                    return;
                }

                if ((playerControlled && !serverDrivenPlayer) && ((pathType & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) ||
                    usesUnsafePathMode))
                {
                    buildStayPath();
                    return;
                }
            }
            else if (serverDrivenPlayer)
            {
                logServerDrivenSplineDecision("held-position-path-calc-failed");
                // A server-controlled Player has no client movement input to
                // repair a failed generated route. Falling through to the raw
                // two-point spline below lets it cut through walls or descend
                // between stacked floors. Hold position and let its movement
                // owner retry a fresh navmesh order instead.
                buildStayPath();
                return;
            }
        }

        // The raw two-point spline: start to destination, straight through
        // whatever lies between. Every guarded branch above returns before this
        // for a server-driven player, so a bot arriving here is the exact shape
        // of bug this diagnostic exists to catch - which is why it is a warning
        // rather than a debug line, and why it names how it got here.
        if (Player const* moverPlayer = unit->ToPlayer())
            if (WorldSession const* session = moverPlayer->GetSession())
                if (session->IsVirtualSession() || session->IsTransientPlayerSession())
                    TC_LOG_WARN("playerbots.movement.spline",
                        "PB spline: bot={} outcome=raw-direct-spline generate_path={} force_dest={} "
                        "from=({}, {}, {}) dest=({}, {}, {}).",
                        unit->GetGUID().ToString(), generatePath ? 1 : 0, forceDestination ? 1 : 0,
                        unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
                        dest.x, dest.y, dest.z);

        args.path_Idx_offset = 0;
        args.path.resize(2);
        TransportPathTransform transform(unit, args.TransformForTransport);
        args.path[1] = transform(dest);
    }

    Vector3 TransportPathTransform::operator()(Vector3 input)
    {
        if (_transformForTransport)
            if (TransportBase* transport = _owner->GetDirectTransport())
                transport->CalculatePassengerOffset(input.x, input.y, input.z);

        return input;
    }
}
