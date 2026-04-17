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
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Battleground.h"
#include "PathGenerator.h"
#include "Pet.h"
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
        return true;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return false;

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

void IssueRangedApproachMovement(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target)
        return;

    float const safeDistance = std::max(1.0f, desiredDistance);
    if (RequiresStrictHumanPathing(player))
    {
        IssueStrictHumanFollow(player, target, safeDistance);
        return;
    }

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    if (player->IsValidAttackTarget(target))
        motionMaster->MoveChase(target, safeDistance);
    else
        motionMaster->MoveFollow(target, safeDistance, player->GetFollowAngle());
}

void IssueMeleeApproachMovement(Player* player, Unit* target)
{
    if (!player || !target)
        return;

    if (RequiresStrictHumanPathing(player))
    {
        // Keep melee bots close to contact range instead of orbiting around a
        // larger follow radius (which can look like "running away" after
        // melee openers and while continuously reissuing approach directives).
        IssueStrictHumanFollow(player, target, 1.5f);
        return;
    }

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    if (player->HasStealthAura())
    {
        // MoveChase can stall for non-swinging stealth openers. Use follow so
        // rogues consistently close to opener distance while preserving stealth.
        motionMaster->MoveFollow(target, 1.5f, player->GetFollowAngle());
    }
    else if (player->IsValidAttackTarget(target))
        motionMaster->MoveChase(target);
    else
        motionMaster->MoveFollow(target, 1.5f, player->GetFollowAngle());
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

bool CastDirectSpell(Player* player, playerbot::PvpClassSpellContext const& context, std::string& failureReason)
{
    failureReason.clear();

    if (!player || !context.spellId)
    {
        failureReason = "missing_spell";
        return false;
    }

    uint32 const resolvedSpellId = ResolveKnownSpellInChain(player, context.spellId);
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

    Unit* target = ResolveTarget(player, context);

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
            // stealth early, but keep chase active so rogues continue
            // closing distance instead of idling in place during openers.
            //
            // AttackStop can clear chase intent in some movement states, so
            // only call it when we are actively swinging a victim and then
            // (re)issue chase immediately afterwards.
            if (player->GetVictim())
                player->AttackStop();

            if (CanIssueFollowCommands(player))
            {
                if (RequiresStrictHumanPathing(player))
                    IssueStrictHumanFollow(player, target, std::max(1.0f, playerbot::PvpCore::GetConfig().meleeRange - 1.0f));
                else
                    player->GetMotionMaster()->MoveFollow(target, std::max(1.0f, playerbot::PvpCore::GetConfig().meleeRange - 1.0f), player->GetFollowAngle());
            }
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
                float const desiredRange = maxRange > 0.0f ? std::max(1.0f, maxRange - 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().spellRange - 1.0f);
                IssueRangedApproachMovement(player, target, desiredRange);
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
            if (RequiresStrictHumanPathing(player))
                IssueStrictHumanFollow(player, target, desiredRange);
            else
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
            if (RequiresStrictHumanPathing(player))
                IssueStrictHumanFollow(player, target, desiredRange);
            else
                player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
        }
        else if (CanIssueFollowCommands(player) && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
        {
            float const desiredRange = std::max(1.0f, minRange + 1.0f);
            if (RequiresStrictHumanPathing(player))
                IssueStrictHumanFollow(player, target, desiredRange);
            else
                player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
        }

        failureReason = "too_close";
        return false;
    }

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
    }

    // Auto-repeat ranged attacks (e.g., wand Shoot) and Life Tap are validated
    // by the core cast pipeline and should not be blocked by this local
    // pre-check. For low-mana caster fallbacks we intentionally allow entering
    // the cast flow so the server can apply the spell-specific resource rules.
    bool const bypassPowerPrecheck = spellInfo->IsAutoRepeatRangedSpell() || IsLifeTapSpell(spellInfo);
    if (!bypassPowerPrecheck && spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
        if (player->GetPower(Powers(spellInfo->PowerType)) < int32(spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask())))
        {
            failureReason = "insufficient_power";
            return false;
        }

    // Cast-time spells like Frostbolt fail while moving. Since playerbots do
    // not have client-side stop-cast behavior, explicitly stop movement before
    // attempting non-instant casts.
    bool const isFoodOrDrinkSpell = resolvedSpellId == SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT || resolvedSpellId == SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK;
    if (spellInfo->CalcCastTime() > 0 || isFoodOrDrinkSpell)
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
                if (RequiresStrictHumanPathing(player))
                    IssueStrictHumanFollow(player, target, desiredRange);
                else
                    player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
            }
            else if (castResult == SPELL_FAILED_LINE_OF_SIGHT)
            {
                if (shouldUseMeleeApproachForEnemySpell)
                    IssueMeleeApproachMovement(player, target);
                else
                {
                    float const desiredRange = maxRange > 0.0f ? std::max(1.0f, maxRange - 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().spellRange - 1.0f);
                    IssueRangedApproachMovement(player, target, desiredRange);
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

bool PvpClassActions::Execute(Player* player, PvpClassSpellContext const& context)
{
    if (!player || !context.classSpellsEnabled || !context.shouldExecute)
        return false;

    if (context.movementDirective != PvpClassSpellContext::MovementDirective::None)
    {
        if (ShouldThrottleDirective(player, context))
            return true;

        Unit* movementTarget = context.movementTargetGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*player, context.movementTargetGuid);
        bool const directiveNeedsTarget =
            context.movementDirective == PvpClassSpellContext::MovementDirective::ReachMeleeRange ||
            context.movementDirective == PvpClassSpellContext::MovementDirective::ReachSpellRange ||
            context.movementDirective == PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell ||
            context.movementDirective == PvpClassSpellContext::MovementDirective::FaceSpellTarget;
        if (directiveNeedsTarget && (!movementTarget || !movementTarget->IsAlive()))
            return false;

        if (directiveNeedsTarget && !CanIssueFollowCommands(player))
        {
            if (IsCrowdControlledForAction(player))
                ClearActiveMovementForControlLoss(player);
            return false;
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
                float const desiredRange = std::max(1.0f,
                    context.movementFollowRange > 0.0f ? context.movementFollowRange : (PvpCore::GetConfig().spellRange - 1.0f));
                IssueRangedApproachMovement(player, movementTarget, desiredRange);
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
                    IssueStrictHumanMove(player, destination);
                else
                    player->GetMotionMaster()->MovePoint(0, destination, true);
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
        return true;
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
    return casted;
}
}
