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

#include "PlayerbotPvpLifecycleActions.h"
#include "PlayerbotRandomBotParticipation.h"

#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "Time/GameTime.h"
#include "GameObject.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Opcodes.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Util.h"
#include "Containers.h"

#include <cstring>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <array>

namespace
{
bool IsWarsongGulch(Player const* player)
{
    if (!player)
        return false;

    Battleground const* battleground = player->GetBattleground();
    return battleground && battleground->GetMapId() == 489;
}

bool TryGetWarsongEnemyBasePosition(Player* player, Position& destination)
{
    if (!player || !IsWarsongGulch(player))
        return false;

    uint32 const bgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    TeamId const botTeam = (bgTeam == ALLIANCE) ? TEAM_ALLIANCE : TEAM_HORDE;

    Position const allianceFlagStand(1540.423f, 1481.325f, 351.8284f, 3.089233f);
    Position const hordeFlagStand(916.0226f, 1434.405f, 345.413f, 0.01745329f);

    destination = (botTeam == TEAM_ALLIANCE) ? hordeFlagStand : allianceFlagStand;
    return true;
}

bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold, uint32 minReissueMs);

bool MoveToClosestBattlegroundGraveyard(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    Battleground* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    if (WorldSafeLocsEntry const* graveyard = battleground->GetClosestGraveyard(player))
    {
        Position destination(graveyard->Loc.X, graveyard->Loc.Y, graveyard->Loc.Z, player->GetOrientation());
        if (!player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
            IssueMovePointThrottled(player, destination, 6.0f, 2000);
        return true;
    }

    return false;
}

bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

std::array<BattlegroundTypeId, 6> BuildRandomBattlegroundOrder()
{
    std::array<BattlegroundTypeId, 6> battlegroundTypes =
    {
        BATTLEGROUND_AV,
        BATTLEGROUND_EY,
        BATTLEGROUND_AB,
        BATTLEGROUND_WS,
        BATTLEGROUND_SA,
        BATTLEGROUND_IC
    };

    Trinity::Containers::RandomShuffle(battlegroundTypes);
    return battlegroundTypes;
}

void EmitLifecycleDiagnostic(Player* player, char const* phase, std::string const& detail)
{
    if (!player || !phase)
        return;

    std::ostringstream oss;
    oss << "[Playerbot PvP][" << phase << "] bot=" << player->GetName() << " guid=" << player->GetGUID().ToString()
        << " map=" << player->GetMapId() << " detail=" << detail;
    std::string const message = oss.str();

    TC_LOG_WARN("playerbots.pvp.lifecycle", "{}", message);

    if (WorldSession* session = player->GetSession())
        ChatHandler(session).SendGlobalGMSysMessage(message.c_str());
}

void EmitBattlegroundGmDebug(Player* bot, std::string const& detail, uint32 throttleMs = 3000)
{
    if (!bot || !bot->InBattleground())
        return;

    static std::unordered_map<uint64, uint32> nextEmitTimeByBotGuid;
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint64 const botGuid = bot->GetGUID().GetRawValue();
    uint32& nextEmitMs = nextEmitTimeByBotGuid[botGuid];
    if (nowMs < nextEmitMs)
        return;

    nextEmitMs = nowMs + throttleMs;

    Map* map = bot->GetMap();
    if (!map)
        return;

    std::ostringstream whisper;
    whisper << "[PBDBG] bot=" << bot->GetName() << " guid=" << bot->GetGUID().ToString()
            << " map=" << bot->GetMapId() << " detail=" << detail;
    std::string const message = whisper.str();

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* observer = itr->GetSource();
        if (!observer || !observer->IsGameMaster())
            continue;

        if (observer->GetBattlegroundId() != bot->GetBattlegroundId())
            continue;

        bot->Whisper(message, LANG_UNIVERSAL, observer);
    }
}

bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold = 6.0f, uint32 minReissueMs = 2000)
{
    if (!player)
        return false;

    struct MoveOrderState
    {
        Position lastDestination;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, MoveOrderState> stateByGuid;
    MoveOrderState& state = stateByGuid[player->GetGUID().GetRawValue()];
    uint32 const nowMs = GameTime::GetGameTimeMS();

    bool const destinationChanged = state.lastIssueMs == 0 ||
        state.lastDestination.GetExactDist(destination) >= destinationChangeThreshold;
    bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + minReissueMs;
    if (!destinationChanged && !canReissueByTime)
    {
        std::ostringstream throttledDetail;
        throttledDetail << "movepoint-skip reason=throttle"
                        << " curr=(" << int32(player->GetPositionX()) << "," << int32(player->GetPositionY()) << "," << int32(player->GetPositionZ()) << ")"
                        << " dest=(" << int32(destination.GetPositionX()) << "," << int32(destination.GetPositionY()) << "," << int32(destination.GetPositionZ()) << ")"
                        << " lastIssuedMsAgo=" << (nowMs - state.lastIssueMs);
        EmitBattlegroundGmDebug(player, throttledDetail.str(), 2000);
        return false;
    }

    MotionMaster* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const currentMovement = motionMaster->GetCurrentMovementGeneratorType();
    if (currentMovement == FOLLOW_MOTION_TYPE || currentMovement == DISTRACT_MOTION_TYPE)
    {
        std::ostringstream overrideDetail;
        overrideDetail << "movement generator override before MovePoint type=" << static_cast<uint32>(currentMovement);
        EmitBattlegroundGmDebug(player, overrideDetail.str(), 5000);
        motionMaster->Clear();
    }

    bool const generatePath = !player->IsFlying() && !player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);

    Position issuedDestination = destination;
    if (IsWarsongGulch(player))
    {
        float const directDistance = player->GetDistance(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
        constexpr float maxStepDistance = 45.0f;
        if (directDistance > maxStepDistance)
        {
            float const ratio = maxStepDistance / directDistance;
            issuedDestination.Relocate(
                player->GetPositionX() + (destination.GetPositionX() - player->GetPositionX()) * ratio,
                player->GetPositionY() + (destination.GetPositionY() - player->GetPositionY()) * ratio,
                player->GetPositionZ() + (destination.GetPositionZ() - player->GetPositionZ()) * ratio,
                player->GetOrientation());
        }

        // Keep WSG movement path-safe for managed virtual bots: if the local step
        // still resolves to unsafe navmesh modes, iteratively shorten the step.
        auto isUnsafePath = [player](Position const& candidate) -> bool
        {
            PathGenerator path(player);
            if (!path.CalculatePath(candidate.GetPositionX(), candidate.GetPositionY(), candidate.GetPositionZ(), false))
                return true;

            PathType const pathType = path.GetPathType();
            if (pathType & PATHFIND_NOPATH)
                return true;

            return pathType & (PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT);
        };

        if (isUnsafePath(issuedDestination))
        {
            constexpr float fallbackSteps[] = { 32.0f, 24.0f, 16.0f, 10.0f };
            for (float stepDistance : fallbackSteps)
            {
                if (directDistance <= stepDistance)
                    continue;

                float const ratio = stepDistance / directDistance;
                Position candidate(
                    player->GetPositionX() + (destination.GetPositionX() - player->GetPositionX()) * ratio,
                    player->GetPositionY() + (destination.GetPositionY() - player->GetPositionY()) * ratio,
                    player->GetPositionZ() + (destination.GetPositionZ() - player->GetPositionZ()) * ratio,
                    player->GetOrientation());

                if (!isUnsafePath(candidate))
                {
                    issuedDestination = candidate;
                    break;
                }
            }
        }
    }

    std::ostringstream moveDetail;
    moveDetail << "movepoint-issue"
               << " from=(" << int32(player->GetPositionX()) << "," << int32(player->GetPositionY()) << "," << int32(player->GetPositionZ()) << ")"
               << " to=(" << int32(issuedDestination.GetPositionX()) << "," << int32(issuedDestination.GetPositionY()) << "," << int32(issuedDestination.GetPositionZ()) << ")"
               << " movementType=" << static_cast<uint32>(currentMovement)
               << " generatePath=" << (generatePath ? 1 : 0);
    EmitBattlegroundGmDebug(player, moveDetail.str(), 2000);

    // Reference-module parity: issue MovePoint directly and let MotionMaster handle path generation.
    motionMaster->MovePoint(0, issuedDestination, generatePath);
    state.lastDestination = issuedDestination;
    state.lastIssueMs = nowMs;
    return true;
}

bool QueuePlayer(Player* player, BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    if (!player || player->InBattleground())
        return false;

    // Allow managed bots to keep participating in queue/invite lifecycle even if
    // they died in the open world. Battleground queue/port handlers can reject
    // dead actors, so recover to alive before queueing.
    if (!player->IsAlive())
        player->ResurrectPlayer(1.0f);

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    // Managed random bots can run on disconnected virtual sessions where RBAC
    // battleground permissions are not always populated like live client sessions.
    // Gate queue eligibility by battleground level + free queue slots instead.
    if (!player->GetBGAccessByLevel(bgTypeId) || !player->HasFreeBattlegroundQueueId())
        return false;

    BattlegroundQueueTypeId const bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType);
    if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    if (player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
        return false;

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
    GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketEntry, arenaType, false, false, 0, 0);
    if (!ginfo)
    {
        EmitLifecycleDiagnostic(player, "queue-add-failed", "BattlegroundQueue::AddGroup returned null.");
        return false;
    }

    player->AddBattlegroundQueueId(bgQueueTypeId);
    sBattlegroundMgr->ScheduleQueueUpdate(ginfo->ArenaMatchmakerRating, ginfo->ArenaType, bgQueueTypeId, bgTypeId,
        bracketEntry->GetBracketId());
    EmitLifecycleDiagnostic(player, "queue-add-success",
        "Queued for bgTypeId=" + std::to_string(uint32(bgTypeId)) + " queueTypeId=" + std::to_string(uint32(bgQueueTypeId)));
    return true;
}

bool RemovePlayerFromQueue(Player* player, BattlegroundQueueTypeId bgQueueTypeId, bool scheduleNonArenaUpdate)
{
    if (!player || bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
    GroupQueueInfo ginfo;
    if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
        return false;

    player->RemoveBattlegroundQueueId(bgQueueTypeId);
    bgQueue.RemovePlayer(player->GetGUID(), true);

    if (scheduleNonArenaUpdate && !ginfo.ArenaType)
    {
        sBattlegroundMgr->ScheduleQueueUpdate(ginfo.ArenaMatchmakerRating, ginfo.ArenaType, bgQueueTypeId, bgTypeId,
            bracketEntry->GetBracketId());
    }

    return true;
}

bool RemoveMatchingQueues(Player* player, bool arenaOnly, bool invitedOnly, bool scheduleNonArenaUpdate)
{
    if (!player || !player->InBattlegroundQueue())
        return false;

    bool removed = false;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
        if (arenaOnly != isArenaQueue)
            continue;

        if (invitedOnly && !player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            continue;

        removed = RemovePlayerFromQueue(player, bgQueueTypeId, scheduleNonArenaUpdate) || removed;
    }

    return removed;
}

bool AcceptMatchingInvite(Player* player, bool arenaInvite)
{
    if (!player || player->InBattleground())
        return false;

    // Keep battleground/arena participation consistent for dead managed bots:
    // invites should still be accepted and transitioned immediately.
    if (!player->IsAlive())
        player->ResurrectPlayer(1.0f);

    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
        if (arenaInvite != isArenaQueue)
            continue;

        if (!player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            continue;

        BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
        uint8 const arenaType = BattlegroundMgr::BGArenaType(bgQueueTypeId);
        if ((arenaType != 0) != arenaInvite)
            continue;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
        {
            EmitLifecycleDiagnostic(player, "invite-missing-group-info",
                "No GroupQueueInfo for queueTypeId=" + std::to_string(uint32(bgQueueTypeId)));
            continue;
        }

        BattlegroundTypeId packetBgTypeId = bgTypeId;
        if (arenaType != 0)
        {
            // Arena invites can target dynamic map-specific arena templates rather
            // than BATTLEGROUND_AA; resolve the invited instance type first.
            Battleground* invited = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, BATTLEGROUND_TYPE_NONE);
            if (!invited)
            {
                EmitLifecycleDiagnostic(player, "invite-arena-instance-missing",
                    "No invited arena instance for guid=" + std::to_string(ginfo.IsInvitedToBGInstanceGUID));
                continue;
            }

            packetBgTypeId = invited->GetTypeID();
        }

        WorldSession* session = player->GetSession();
        if (!session)
        {
            EmitLifecycleDiagnostic(player, "invite-no-session", "WorldSession is null.");
            continue;
        }

        // Execute invite acceptance directly. Managed random bots can run on
        // disconnected virtual sessions where queued outbound packets are not
        // guaranteed to be pumped like real client traffic.
        WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
        packet << arenaType << uint8(0) << uint32(packetBgTypeId) << uint16(0x1F90) << uint8(1);
        session->HandleBattleFieldPortOpcode(packet);

        if (player->IsBeingTeleportedFar())
        {
            EmitLifecycleDiagnostic(player, "invite-accept-far-teleport-pending",
                "Issuing server-side HandleMoveWorldportAck for bot teleport finalization.");
            session->HandleMoveWorldportAck();
        }

        if (!player->InBattleground() && !player->IsBeingTeleported())
        {
            EmitLifecycleDiagnostic(player, "invite-accept-no-transition",
                "HandleBattleFieldPortOpcode did not transition to battleground/teleport.");
        }
        else
        {
            EmitLifecycleDiagnostic(player, "invite-accept-transition",
                "Accepted invite for bgTypeId=" + std::to_string(uint32(packetBgTypeId)));
        }
        return true;
    }

    return false;
}

bool HasConflictingBattlegroundLifecycleContext(playerbot::BattlegroundLifecycleContext const& context)
{
    return (context.queueOperation != playerbot::QueueOperationType::None) &&
        (context.invitationResponse != playerbot::InvitationResponseType::None);
}

bool HandleBattlegroundDeathState(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    if (player->IsAlive())
        return false;

    if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        if (player->getDeathState() == JUST_DIED)
            player->KillPlayer();

        player->BuildPlayerRepop();
        player->RepopAtGraveyard();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP death handling: guid={} action=release-spirit.",
            player->GetGUID().ToString());
        return true;
    }

    return true;
}

bool HasConflictingArenaLifecycleContext(playerbot::ArenaLifecycleContext const& context)
{
    return (context.queueOperation != playerbot::QueueOperationType::None) &&
        (context.teamInteraction != playerbot::ArenaTeamInteractionType::None);
}

bool IsTacticalAction(char const* actionName, char const* expected)
{
    return actionName && expected && std::strcmp(actionName, expected) == 0;
}

bool CanIssueBotMovement(Player const* player)
{
    if (!player || !player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return false;

    if (player->HasUnitState(UNIT_STATE_ROOT) || player->HasUnitState(UNIT_STATE_STUNNED))
        return false;

    return true;
}

bool IsMeleePressureTarget(Unit const* unit)
{
    Player const* player = unit ? unit->ToPlayer() : nullptr;
    if (!player)
        return false;

    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
            return true;
        case CLASS_SHAMAN:
        case CLASS_PALADIN:
        {
            Item const* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
            ItemTemplate const* mainHandTemplate = mainHand ? mainHand->GetTemplate() : nullptr;
            return mainHandTemplate && mainHandTemplate->InventoryType == INVTYPE_2HWEAPON;
        }
        default:
            return false;
    }
}

struct CombatPositioningProfile
{
    float preferredMinRange = 0.0f;
    float preferredIdealRange = 0.0f;
    float preferredMaxPressureRange = 0.0f;
    bool primarilyRanged = false;
    bool createDistanceWhenCrowded = false;
    bool meleeFallbackAcceptable = true;
    char const* label = "default";
};

CombatPositioningProfile GetCombatPositioningProfile(Player const* player)
{
    if (!player)
        return {};

    switch (player->GetClass())
    {
        case CLASS_HUNTER: return { 8.0f, 28.0f, 38.0f, true, true, false, "hunter-ranged" };
        case CLASS_MAGE: return { 12.0f, 27.0f, 36.0f, true, true, false, "mage-ranged" };
        case CLASS_PRIEST: return { 10.0f, 25.0f, 34.0f, true, true, false, "priest-ranged" };
        case CLASS_WARLOCK: return { 10.0f, 26.0f, 35.0f, true, true, false, "warlock-ranged" };
        case CLASS_WARRIOR: return { 0.0f, 1.5f, 5.0f, false, false, true, "warrior-melee" };
        case CLASS_ROGUE: return { 0.0f, 1.5f, 5.0f, false, false, true, "rogue-melee" };
        case CLASS_PALADIN: return { 0.0f, 3.0f, 8.0f, false, false, true, "paladin-hybrid" };
        case CLASS_SHAMAN: return { 5.0f, 20.0f, 30.0f, true, true, true, "shaman-hybrid" };
        case CLASS_DRUID: return { 4.0f, 18.0f, 28.0f, true, true, true, "druid-hybrid" };
        default: return { 0.0f, 3.0f, 8.0f, false, false, true, "default-melee" };
    }
}

bool MoveAwayFromUnit(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target || !CanIssueBotMovement(player))
        return false;

    float const angleAway = target->GetAbsoluteAngle(player);
    float const currentDistance = player->GetDistance(target);
    float const moveDistance = std::max(4.0f, desiredDistance - currentDistance + 2.0f);

    Position destination(player->GetPositionX() + std::cos(angleAway) * moveDistance,
        player->GetPositionY() + std::sin(angleAway) * moveDistance,
        player->GetPositionZ(), player->GetOrientation());
    player->GetMotionMaster()->MovePoint(0, destination);
    return true;
}

bool TryRecoverLineOfSight(Player* player, Unit* target, CombatPositioningProfile const& profile, char const* reason)
{
    if (!player || !target || !target->IsAlive() || !CanIssueBotMovement(player))
        return false;

    if (player->IsWithinLOSInMap(target))
        return false;

    float const orbitAngle = target->GetAbsoluteAngle(player) + frand(-0.85f, 0.85f);
    float const orbitRange = std::max(profile.preferredMinRange + 2.0f, profile.preferredIdealRange);
    Position reposition(target->GetPositionX() + std::cos(orbitAngle) * orbitRange,
        target->GetPositionY() + std::sin(orbitAngle) * orbitRange,
        target->GetPositionZ(), player->GetOrientation());
    player->GetMotionMaster()->MovePoint(0, reposition);

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP LOS recovery: bot={} target={} profile={} reason={} orbitRange={}.",
        player->GetGUID().ToString(), target->GetGUID().ToString(), profile.label, reason ? reason : "unknown", orbitRange);
    return true;
}

Player* FindFlagCarrierForDirective(Player* player, playerbot::FlagCarrierDirective directive)
{
    if (!player || directive == playerbot::FlagCarrierDirective::None || !player->InBattleground())
        return nullptr;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    TeamId const botTeam = player->GetTeamId();
    TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;

    if (BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(battleground))
    {
        ObjectGuid carrierGuid = ObjectGuid::Empty;
        if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier)
            carrierGuid = bgWs->GetFlagPickerGUID(botTeam);
        else if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier)
            carrierGuid = bgWs->GetFlagPickerGUID(enemyTeam);

        if (carrierGuid.IsEmpty())
            return nullptr;

        return ObjectAccessor::FindConnectedPlayer(carrierGuid);
    }

    if (BattlegroundEY* bgEy = dynamic_cast<BattlegroundEY*>(battleground))
    {
        ObjectGuid const carrierGuid = bgEy->GetFlagPickerGUID();
        if (carrierGuid.IsEmpty())
            return nullptr;

        Player* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
        if (!carrier)
            return nullptr;

        if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier && carrier->GetTeamId() != botTeam)
            return carrier;
        if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier && carrier->GetTeamId() == botTeam)
            return carrier;
    }

    return nullptr;
}

bool MoveTowardUnit(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !player->IsAlive() || !target || !target->IsAlive() || player->GetMapId() != target->GetMapId() || !CanIssueBotMovement(player))
        return false;

    CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
    if (!player->IsWithinLOSInMap(target))
        return TryRecoverLineOfSight(player, target, profile, "move-toward-unit");

    if (!player->IsWithinDistInMap(target, desiredDistance))
        player->GetMotionMaster()->MoveFollow(target, desiredDistance, player->GetFollowAngle());

    return true;
}

std::unordered_map<uint64, uint32> g_WsgReturnAttemptNotBeforeMsByGuid;

GameObject* GetFriendlyDroppedWsgFlag(Player* player, BattlegroundWS* bgWs)
{
    if (!player || !bgWs || !player->GetMap())
        return nullptr;

    uint32 const botBgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    if (bgWs->GetFlagState(botBgTeam) != BG_WS_FLAG_STATE_ON_GROUND)
        return nullptr;

    ObjectGuid const droppedFlagGuid = bgWs->GetDroppedFlagGUID(botBgTeam);
    if (droppedFlagGuid.IsEmpty())
        return nullptr;

    return player->GetMap()->GetGameObject(droppedFlagGuid);
}

bool HumanTeammateNearDroppedFlag(Player* player, GameObject const* droppedFlag, float veryCloseDistance)
{
    if (!player || !droppedFlag || !player->GetMap())
        return false;

    uint32 const botBgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    float const botDistance = player->GetDistance(droppedFlag);

    Map::PlayerList const& players = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* teammate = itr->GetSource();
        if (!teammate || teammate == player || !teammate->IsAlive())
            continue;
        if (teammate->GetBattlegroundId() != player->GetBattlegroundId())
            continue;

        uint32 const teammateBgTeam = teammate->GetBGTeam() ? teammate->GetBGTeam() : teammate->GetTeam();
        if (teammateBgTeam != botBgTeam || playerbot::IsManagedRandomBot(teammate))
            continue;

        float const teammateDistance = teammate->GetDistance(droppedFlag);
        if (teammateDistance <= veryCloseDistance && teammateDistance <= botDistance + 1.0f)
            return true;
    }

    return false;
}

bool TryReturnDroppedFriendlyFlagWithHumanPriority(Player* player)
{
    if (!player || !player->InBattleground())
        return false;
    if (!CanIssueBotMovement(player))
        return false;

    BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(player->GetBattleground());
    if (!bgWs || bgWs->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    GameObject* droppedFlag = GetFriendlyDroppedWsgFlag(player, bgWs);
    if (!droppedFlag)
        return false;

    uint64 const botRawGuid = player->GetGUID().GetRawValue();
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32& attemptNotBeforeMs = g_WsgReturnAttemptNotBeforeMsByGuid[botRawGuid];

    if (HumanTeammateNearDroppedFlag(player, droppedFlag, 7.0f))
    {
        attemptNotBeforeMs = nowMs + urand(700, 1300);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP WSG return yielded: guid={} reason=nearby-human-priority wait_until_ms={}.",
            player->GetGUID().ToString(), attemptNotBeforeMs);
        return false;
    }

    if (attemptNotBeforeMs == 0)
    {
        attemptNotBeforeMs = nowMs + urand(350, 900);
        return false;
    }

    if (nowMs < attemptNotBeforeMs)
        return false;

    if (!player->IsWithinDistInMap(droppedFlag, 10.0f))
    {
        player->GetMotionMaster()->MovePoint(0, droppedFlag->GetPosition());
        return true;
    }

    bgWs->EventPlayerClickedOnFlag(player, droppedFlag);
    attemptNotBeforeMs = nowMs + urand(1200, 2200);
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP WSG return attempted: guid={} flag_guid={} next_attempt_ms={}.",
        player->GetGUID().ToString(), droppedFlag->GetGUID().ToString(), attemptNotBeforeMs);
    return true;
}

Player* FindNearestEnemyBattlegroundPlayer(Player* player, float maxDistance)
{
    if (!player || !player->InBattleground() || !player->GetMap())
        return nullptr;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    uint32 const playerBgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();

    float nearestDistance = std::numeric_limits<float>::max();
    Player* nearestEnemy = nullptr;

    Map::PlayerList const& players = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (candidate->GetBattlegroundId() != player->GetBattlegroundId())
            continue;
        uint32 const candidateBgTeam = candidate->GetBGTeam() ? candidate->GetBGTeam() : candidate->GetTeam();
        if (candidateBgTeam == playerBgTeam)
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance > maxDistance || distance >= nearestDistance)
            continue;

        nearestDistance = distance;
        nearestEnemy = candidate;
    }

    return nearestEnemy;
}

Unit* AcquireCombatTarget(Player* player, float scanDistance)
{
    if (!player)
        return nullptr;

    Unit* target = player->GetVictim();
    if (!target || !target->IsAlive())
        target = player->GetSelectedUnit();
    if ((!target || !target->IsAlive()) && player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS)
    {
        Unit* duelOpponent = player->duel->Opponent;
        if (duelOpponent && duelOpponent->IsAlive() && duelOpponent->GetMapId() == player->GetMapId())
            target = duelOpponent;
    }
    if ((!target || !target->IsAlive()) && player->InBattleground())
        target = FindNearestEnemyBattlegroundPlayer(player, scanDistance);
    if (!target || !target->IsAlive())
        return nullptr;

    player->SetSelection(target->GetGUID());
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP chosen combat target: bot={} target={} class={} distance={}.",
        player->GetGUID().ToString(), target->GetGUID().ToString(), uint32(player->GetClass()), player->GetDistance(target));
    return target;
}

bool DriveCombatPositioning(Player* player, Unit* target, CombatPositioningProfile const& profile)
{
    if (!player || !target || !target->IsAlive() || !CanIssueBotMovement(player))
        return false;

    float const distance = player->GetDistance(target);
    bool const hasLos = player->IsWithinLOSInMap(target);
    if (!hasLos)
        return TryRecoverLineOfSight(player, target, profile, "drive-combat-positioning");

    if (profile.primarilyRanged)
    {
        if (profile.createDistanceWhenCrowded && IsMeleePressureTarget(target) && distance < profile.preferredIdealRange)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=create-distance-vs-melee-pressure distance={} min={} ideal={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                profile.preferredMaxPressureRange);
            return MoveAwayFromUnit(player, target, profile.preferredIdealRange);
        }

        if (distance < profile.preferredMinRange && profile.createDistanceWhenCrowded)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=create-distance distance={} min={} ideal={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                profile.preferredMaxPressureRange);
            return MoveAwayFromUnit(player, target, profile.preferredIdealRange);
        }

        if (distance > profile.preferredMaxPressureRange)
        {
            player->GetMotionMaster()->MoveFollow(target, profile.preferredIdealRange, player->GetFollowAngle());
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=close-distance distance={} min={} ideal={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                profile.preferredMaxPressureRange);
            return true;
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP distance band: bot={} profile={} decision=hold-band distance={} min={} ideal={} max={}.",
            player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
            profile.preferredMaxPressureRange);
        return true;
    }

    if (distance > profile.preferredMaxPressureRange || !player->IsWithinMeleeRange(target))
    {
        player->GetMotionMaster()->MoveChase(target);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP distance band: bot={} profile={} decision=melee-close distance={} max={}.",
            player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange);
        return true;
    }

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP distance band: bot={} profile={} decision=melee-stick distance={} max={}.",
        player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange);
    return true;
}

bool EngageNearestEnemyPlayer(Player* player, float scanDistance)
{
    if (!player || !player->IsAlive())
        return false;

    Unit* target = AcquireCombatTarget(player, scanDistance);
    if (!target)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP movement skipped: bot={} reason=no-combat-target scanDistance={}.",
            player ? player->GetGUID().ToString() : ObjectGuid::Empty.ToString(), scanDistance);
        return false;
    }

    CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
    bool const useMeleeAttack = !profile.primarilyRanged || profile.meleeFallbackAcceptable;
    bool const isStealthedRogue = player->GetClass() == CLASS_ROGUE && player->HasStealthAura();
    if (isStealthedRogue)
        player->AttackStop();
    else
        player->Attack(target, useMeleeAttack);

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP positioning profile: bot={} profile={} ranged={} createDistance={} meleeFallback={}.",
        player->GetGUID().ToString(), profile.label, profile.primarilyRanged, profile.createDistanceWhenCrowded,
        profile.meleeFallbackAcceptable);

    return DriveCombatPositioning(player, target, profile);
}

void ApplyDeterministicObjectiveOffset(Battleground const* battleground, Player const* player, Position& destination)
{
    if (!battleground || !player)
        return;

    // Keep each bot slightly spread out, but stable across updates, to avoid
    // objective destination churn that causes oscillating movement.
    uint64 const seed = player->GetGUID().GetRawValue() ^ (uint64(battleground->GetMapId()) << 32) ^ battleground->GetInstanceID();
    float const angle = float(seed % 6283) / 1000.0f;
    float const radius = 2.0f + float((seed / 6283) % 600) / 100.0f; // [2.0, 8.0)
    destination.RelocateOffset(Position(std::cos(angle) * radius, std::sin(angle) * radius, 0.0f, 0.0f));
}

bool TryGetObjectivePosition(Battleground* battleground, Player* player, Position& destination)
{
    if (!battleground || !player)
        return false;

    // WSG needs deterministic cross-map intent; base anchors can be missing or
    // locally-biased in sparse managed-bot matches, which leaves bots stalling.
    if (TryGetWarsongEnemyBasePosition(player, destination))
    {
        ApplyDeterministicObjectiveOffset(battleground, player, destination);
        return true;
    }

    Position const* allianceStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(TEAM_ALLIANCE));
    Position const* hordeStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(TEAM_HORDE));

    if (allianceStart && hordeStart)
    {
        float const distanceToAllianceStart = player->GetDistance(allianceStart->GetPositionX(), allianceStart->GetPositionY(), allianceStart->GetPositionZ());
        float const distanceToHordeStart = player->GetDistance(hordeStart->GetPositionX(), hordeStart->GetPositionY(), hordeStart->GetPositionZ());
        destination = (distanceToAllianceStart > distanceToHordeStart) ? Position(*allianceStart) : Position(*hordeStart);
        ApplyDeterministicObjectiveOffset(battleground, player, destination);
        return true;
    }

    uint32 const bgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    TeamId const botTeam = (bgTeam == ALLIANCE) ? TEAM_ALLIANCE : TEAM_HORDE;
    TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
    if (Position const* enemyStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(enemyTeam)))
    {
        destination = Position(*enemyStart);
        ApplyDeterministicObjectiveOffset(battleground, player, destination);
        return true;
    }

    // Fallback for sparse/invalid start-anchor states:
    // WSG bots can otherwise idle in their own base graveyard when no objective
    // anchor is resolved from battleground starts.
    if (battleground->GetMapId() == 489) // Warsong Gulch
    {
        Position const allianceFlagStand(1540.423f, 1481.325f, 351.8284f, 3.089233f);
        Position const hordeFlagStand(916.0226f, 1434.405f, 345.413f, 0.01745329f);

        if (botTeam == TEAM_ALLIANCE)
            destination = hordeFlagStand;
        else if (botTeam == TEAM_HORDE)
            destination = allianceFlagStand;
        else
            destination = hordeFlagStand;

        ApplyDeterministicObjectiveOffset(battleground, player, destination);
        return true;
    }

    return false;
}
}

namespace playerbot
{
bool BattlegroundLifecycleActions::Execute(Player* player, BattlegroundLifecycleContext const& context)
{
    if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
        return false;

    if (HasConflictingBattlegroundLifecycleContext(context))
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP battleground lifecycle no-op due to conflicting context: guid={}, queueOperation={}, invitationResponse={}, handleInProgress={}.",
            player->GetGUID().ToString(), static_cast<uint8>(context.queueOperation), static_cast<uint8>(context.invitationResponse),
            context.shouldHandleInProgressStatus ? 1 : 0);
        return false;
    }

    bool didExecute = false;

    switch (context.queueOperation)
    {
        case QueueOperationType::Join:
            didExecute = JoinQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::Leave:
            didExecute = LeaveQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::None:
        default:
            break;
    }

    switch (context.invitationResponse)
    {
        case InvitationResponseType::Accept:
            didExecute = AcceptInvitePrimitive(player) || didExecute;
            break;
        case InvitationResponseType::Decline:
            didExecute = DeclineInvitePrimitive(player) || didExecute;
            break;
        case InvitationResponseType::None:
        default:
            break;
    }

    if (context.shouldHandleInProgressStatus)
        didExecute = HandleInProgressStatusPrimitive(player) || didExecute;

    return didExecute;
}

bool BattlegroundLifecycleActions::JoinQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    for (BattlegroundTypeId bgTypeId : BuildRandomBattlegroundOrder())
    {
        if (QueuePlayer(player, bgTypeId, 0))
            return true;
    }

    return false;
}

bool BattlegroundLifecycleActions::LeaveQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return RemoveMatchingQueues(player, false, false, true);
}

bool BattlegroundLifecycleActions::AcceptInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return AcceptMatchingInvite(player, false);
}

bool BattlegroundLifecycleActions::DeclineInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return RemoveMatchingQueues(player, false, true, true);
}

bool BattlegroundLifecycleActions::HandleInProgressStatusPrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!player->InBattleground())
        return false;

    if (Battleground* battleground = player->GetBattleground())
    {
        if (battleground->GetStatus() != STATUS_IN_PROGRESS)
            return false;
    }

    if (HandleBattlegroundDeathState(player))
        return true;

    if (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION))
    {
        player->RemoveAurasDueToSpell(SPELL_PREPARATION);
        player->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
        player->RemoveUnitFlag(UNIT_FLAG_PREPARATION);
    }

    // Keep managed bots moving even when tactical decision hooks are disabled
    // or when another behavior tree branch (e.g. buffing) wins the current tick.
    // This prevents "stand still at gate-open" stalls in active battlegrounds.
    if (!CanIssueBotMovement(player))
    {
        std::ostringstream blockedReason;
        blockedReason << "movement-blocked alive=" << (player->IsAlive() ? 1 : 0)
                      << " ghost=" << (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? 1 : 0)
                      << " rooted=" << (player->HasUnitState(UNIT_STATE_ROOT) ? 1 : 0)
                      << " stunned=" << (player->HasUnitState(UNIT_STATE_STUNNED) ? 1 : 0);
        EmitBattlegroundGmDebug(player, blockedReason.str());
        return true;
    }

    float const engageDistance = IsWarsongGulch(player) ? 2000.0f : 65.0f;
    if (EngageNearestEnemyPlayer(player, engageDistance))
        return true;

    if (Battleground* battleground = player->GetBattleground())
    {
        Position destination;
        if (TryGetObjectivePosition(battleground, player, destination))
        {
            bool const withinObjectiveRange = player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f);
            if (!withinObjectiveRange)
                IssueMovePointThrottled(player, destination);
            else
                EmitBattlegroundGmDebug(player, "objective-skip reason=already-near-objective range=12");
            return true;
        }
    }

    if (IsWarsongGulch(player))
        return MoveToClosestBattlegroundGraveyard(player);

    EmitBattlegroundGmDebug(player, "no enemy target and no objective position resolved");
    return true;
}

bool BattlegroundTacticalActions::Execute(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !context.tacticsEnabled || !context.shouldEvaluate || !context.actionName)
        return false;

    if (!player->IsAlive())
        return false;

    if (IsTacticalAction(context.actionName, "bg move to start"))
        return MoveToStartPrimitive(player);
    if (IsTacticalAction(context.actionName, "bg move to objective"))
        return MoveToObjectivePrimitive(player, context);
    if (IsTacticalAction(context.actionName, "bg check objective"))
        return CheckObjectivePrimitive(player, context);
    if (IsTacticalAction(context.actionName, "bg reset objective force"))
        return ResetObjectiveForcePrimitive(player);
    if (IsTacticalAction(context.actionName, "bg use buff"))
        return UseBuffPrimitive(player);
    if (IsTacticalAction(context.actionName, "attack enemy flag carrier"))
        return AttackEnemyFlagCarrierPrimitive(player, context);
    if (IsTacticalAction(context.actionName, "bg protect fc"))
        return ProtectFlagCarrierPrimitive(player, context);

    return false;
}

bool BattlegroundTacticalActions::MoveToStartPrimitive(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    if (Battleground* battleground = player->GetBattleground())
        return battleground->GetStatus() == STATUS_WAIT_JOIN;

    return false;
}

bool BattlegroundTacticalActions::MoveToObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;
    if (!CanIssueBotMovement(player))
        return false;

    bool const teamHasHumans = PvpCore::TeamHasHumanPlayers(player);
    if (teamHasHumans && TryReturnDroppedFriendlyFlagWithHumanPriority(player))
        return true;

    float const engageDistance = IsWarsongGulch(player) ? 2000.0f : 80.0f;
    if (EngageNearestEnemyPlayer(player, engageDistance))
        return true;

    if (context.objective.type == BattlegroundObjectiveType::None &&
        context.movement == BattlegroundMovementPrimitive::None &&
        context.flagCarrierDirective == FlagCarrierDirective::None)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP movement skipped: bot={} reason=no-objective-and-no-directive.",
            player->GetGUID().ToString());
        return false;
    }

    if (context.movement == BattlegroundMovementPrimitive::MoveToObjectiveUnit ||
        context.movement == BattlegroundMovementPrimitive::FollowFlagCarrier ||
        context.flagCarrierDirective != FlagCarrierDirective::None)
    {
        if (Player* carrier = FindFlagCarrierForDirective(player, context.flagCarrierDirective))
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP objective support selected: guid={} directive={} target={}.",
                player->GetGUID().ToString(), static_cast<uint8>(context.flagCarrierDirective), carrier->GetGUID().ToString());
            return MoveTowardUnit(player, carrier, 20.0f);
        }
    }

    if (context.movement == BattlegroundMovementPrimitive::MoveToObjectivePosition)
    {
        if (Battleground* battleground = player->GetBattleground())
        {
            Position destination;
            if (TryGetObjectivePosition(battleground, player, destination))
            {
                bool const withinObjectiveRange = player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f);
                if (!withinObjectiveRange)
                    IssueMovePointThrottled(player, destination);
                else
                    EmitBattlegroundGmDebug(player, "objective-skip reason=already-near-objective range=12");
                return true;
            }

            // WSG can enter sparse states where objective anchors are unavailable.
            // In those windows, force a large-radius enemy scan before falling
            // back to local graveyard movement so bots keep crossing the map.
            if (EngageNearestEnemyPlayer(player, 2000.0f))
                return true;

            if (WorldSafeLocsEntry const* graveyard = battleground->GetClosestGraveyard(player))
            {
                Position destination(graveyard->Loc.X, graveyard->Loc.Y, graveyard->Loc.Z, player->GetOrientation());
                if (!player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
                    IssueMovePointThrottled(player, destination);
                return true;
            }
        }
    }

    bool const fallbackEngage = EngageNearestEnemyPlayer(player, 55.0f);
    if (!fallbackEngage)
    {
        if (IsWarsongGulch(player))
            return MoveToClosestBattlegroundGraveyard(player);

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP movement skipped: bot={} reason=no-objective-movement-and-no-fallback-target.",
            player->GetGUID().ToString());
    }

    return fallbackEngage;
}

bool BattlegroundTacticalActions::CheckObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    float const engageDistance = IsWarsongGulch(player) ? 2000.0f : 60.0f;
    if (EngageNearestEnemyPlayer(player, engageDistance))
        return true;

    if (IsWarsongGulch(player))
        return MoveToClosestBattlegroundGraveyard(player);

    return context.movement != BattlegroundMovementPrimitive::None || context.objective.type != BattlegroundObjectiveType::None;
}

bool BattlegroundTacticalActions::ResetObjectiveForcePrimitive(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    return true;
}

bool BattlegroundTacticalActions::UseBuffPrimitive(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    return true;
}

bool BattlegroundTacticalActions::AttackEnemyFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    if (context.flagCarrierDirective != FlagCarrierDirective::AttackEnemyCarrier)
        return false;

    Player* enemyCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::AttackEnemyCarrier);
    if (enemyCarrier)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP enemy-FC support selected: guid={} target={}.",
            player->GetGUID().ToString(), enemyCarrier->GetGUID().ToString());
    }
    return MoveTowardUnit(player, enemyCarrier, 15.0f);
}

bool BattlegroundTacticalActions::ProtectFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context)
{
    if (!player || !player->InBattleground())
        return false;

    if (context.flagCarrierDirective != FlagCarrierDirective::ProtectTeamCarrier)
        return false;

    Player* teamCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::ProtectTeamCarrier);
    if (teamCarrier)
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP protect-FC support selected: guid={} target={}.",
            player->GetGUID().ToString(), teamCarrier->GetGUID().ToString());
    }
    return MoveTowardUnit(player, teamCarrier, 18.0f);
}

bool ArenaLifecycleActions::Execute(Player* player, ArenaLifecycleContext const& context)
{
    if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
        return false;

    if (HasConflictingArenaLifecycleContext(context))
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP arena lifecycle no-op due to conflicting context: guid={}, queueOperation={}, teamInteraction={}.",
            player->GetGUID().ToString(), static_cast<uint8>(context.queueOperation), static_cast<uint8>(context.teamInteraction));
        return false;
    }

    bool didExecute = false;

    switch (context.queueOperation)
    {
        case QueueOperationType::Join:
            didExecute = JoinQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::Leave:
            didExecute = LeaveQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::None:
        default:
            break;
    }

    switch (context.teamInteraction)
    {
        case ArenaTeamInteractionType::AcceptInvite:
            didExecute = AcceptTeamInvitePrimitive(player) || didExecute;
            break;
        case ArenaTeamInteractionType::DeclineInvite:
            didExecute = DeclineTeamInvitePrimitive(player) || didExecute;
            break;
        case ArenaTeamInteractionType::None:
        default:
            break;
    }

    didExecute = AcceptMatchingInvite(player, true) || didExecute;

    return didExecute;
}

bool ArenaLifecycleActions::JoinQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    std::array<uint8, 3> arenaTypes = { ARENA_TYPE_2v2, ARENA_TYPE_3v3, ARENA_TYPE_5v5 };
    Trinity::Containers::RandomShuffle(arenaTypes);
    for (uint8 arenaType : arenaTypes)
    {
        if (QueuePlayer(player, BATTLEGROUND_AA, arenaType))
            return true;
    }

    return false;
}

bool ArenaLifecycleActions::LeaveQueuePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    return RemoveMatchingQueues(player, true, false, false);
}

bool ArenaLifecycleActions::AcceptTeamInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    uint32 const invitedArenaTeamId = player->GetArenaTeamIdInvited();
    if (!invitedArenaTeamId)
        return false;

    ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(invitedArenaTeamId);
    if (!arenaTeam)
        return false;

    if (arenaTeam->GetMember(player->GetGUID()))
        return false;

    if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD) &&
        player->GetTeam() != sCharacterCache->GetCharacterTeamByGuid(arenaTeam->GetCaptain()))
        return false;

    return arenaTeam->AddMember(player->GetGUID());
}

bool ArenaLifecycleActions::DeclineTeamInvitePrimitive(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!player->GetArenaTeamIdInvited())
        return false;

    player->SetArenaTeamIdInvited(0);
    return true;
}

bool DuelTacticalActions::Execute(Player* player)
{
    if (!player || !player->IsAlive() || !CanIssueBotMovement(player))
        return false;

    if (!player->duel || player->duel->State != DUEL_STATE_IN_PROGRESS)
        return false;

    return EngageNearestEnemyPlayer(player, 65.0f);
}
}
