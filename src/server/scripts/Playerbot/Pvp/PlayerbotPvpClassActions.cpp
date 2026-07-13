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
#include "ObjectMgr.h"
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
#include "SpellAuras.h"
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
#include <vector>

namespace
{
void SetLastMovementDebugStatus(Player const* player, std::string const& status);
void WhisperHunterCastDiagnostic(Player* player, Unit* target, char const* phase, uint32 spellId, char const* extra);
bool HasActiveMovementEffectSpline(Player const* player);
bool TryMoveOutOfHazardousLiquid(Player* player);

bool IsLifeTapSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && firstRank->Id == 1454; // Life Tap (rank 1)
}

bool IsPriestFlashHealSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && firstRank->Id == 2061; // Flash Heal (rank 1)
}

bool IsSpiritOfRedemptionFreeHeal(Player const* player, SpellInfo const* spellInfo)
{
    return player && player->GetClass() == CLASS_PRIEST && IsPriestFlashHealSpell(spellInfo) &&
        player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION);
}

bool SpellAppliesBreakableByDamageCrowdControl(SpellInfo const* spellInfo)
{
    if (!spellInfo || !(spellInfo->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE))
        return false;

    for (SpellEffectInfo const& effectInfo : spellInfo->GetEffects())
    {
        if (effectInfo.Effect != SPELL_EFFECT_APPLY_AURA)
            continue;

        switch (effectInfo.ApplyAuraName)
        {
            case SPELL_AURA_MOD_CONFUSE:
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_MOD_STUN:
            case SPELL_AURA_MOD_ROOT:
            case SPELL_AURA_TRANSFORM:
                return true;
            default:
                break;
        }
    }

    return false;
}

bool HasAuraInSpellChain(Unit const* unit, uint32 baseSpellId)
{
    if (!unit || !baseSpellId)
        return false;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
    if (!baseSpellInfo)
        return false;

    SpellInfo const* firstRank = baseSpellInfo->GetFirstRankSpell();
    if (!firstRank)
        return unit->HasAura(baseSpellId);

    for (uint32 chainSpellId = firstRank->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
        if (unit->HasAura(chainSpellId))
            return true;

    return false;
}

bool IsDruidFeralMeleePositioning(Player const* player)
{
    if (!player || player->GetClass() != CLASS_DRUID)
        return false;

    switch (player->GetShapeshiftForm())
    {
    case FORM_CAT:
    case FORM_BEAR:
    case FORM_DIREBEAR:
        return true;
    default:
        break;
    }

    return player->HasAura(768) ||   // Cat Form
           player->HasAura(5487) ||  // Bear Form
           player->HasAura(9634) ||  // Dire Bear Form
           player->HasAura(9913);    // Prowl
}

bool IsStealthedMeleeOpener(Player const* player)
{
    if (!player || !player->HasStealthAura())
        return false;

    return player->GetClass() == CLASS_ROGUE || IsDruidFeralMeleePositioning(player);
}

bool IsHunterAimedShotSpellId(uint32 spellId)
{
    switch (spellId)
    {
        case 19434: // Aimed Shot rank 1
        case 20900:
        case 20901:
        case 20902:
        case 20903:
        case 20904:
        case 27065:
        case 49049:
        case 49050:
            return true;
        default:
            return false;
    }
}

bool IsHunterAimedShotSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    if (IsHunterAimedShotSpellId(spellInfo->Id))
        return true;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && IsHunterAimedShotSpellId(firstRank->Id);
}

bool IsHunterMultiShotSpellId(uint32 spellId)
{
    switch (spellId)
    {
        case 2643:  // Multi-Shot rank 1
        case 14288:
        case 14289:
        case 14290:
        case 25294:
        case 27021:
        case 49047:
        case 49048:
            return true;
        default:
            return false;
    }
}

bool IsHunterMultiShotSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    if (IsHunterMultiShotSpellId(spellInfo->Id))
        return true;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && IsHunterMultiShotSpellId(firstRank->Id);
}

uint32 GetHunterStationaryCastTimeMs(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return 0;

    uint32 castTimeMs = uint32(std::max<int32>(0, spellInfo->CalcCastTime()));

    // Some 3.3.5 / custom DBC branches do not report the rank-chain or cast
    // time consistently for Aimed Shot. Treat every known Aimed Shot rank as
    // a true stationary hard-cast so the selector/lifecycle cannot re-cast it
    // or restart Auto Shot while it is still preparing.
    if (IsHunterAimedShotSpell(spellInfo))
        return std::max<uint32>(castTimeMs, 3000);

    // Revive Pet is a real hunter cast and must not be clipped by the stutter
    // movement loop either. Use the DBC cast time, but keep a non-zero fallback
    // for custom data where CalcCastTime() is missing.
    if (spellInfo->Id == 982)
        return std::max<uint32>(castTimeMs, 1000);

    // Multi-Shot is short, but it still has a stationary firing delay/cast
    // window in this branch. It must be protected from stutter movement too;
    // otherwise the hunter starts Multi-Shot, immediately resumes fleeing, and
    // clips the shot before it launches. Use a short explicit guard even when
    // custom DBC data reports zero cast time.
    if (IsHunterMultiShotSpell(spellInfo))
        return std::max<uint32>(castTimeMs, 500);

    return castTimeMs > 0 ? castTimeMs : 0;
}

bool IsHunterCastTimeShot(Player const* player, SpellInfo const* spellInfo)
{
    if (!player || player->GetClass() != CLASS_HUNTER || !spellInfo)
        return false;

    return GetHunterStationaryCastTimeMs(spellInfo) > 0;
}

bool HunterHasActiveAutoShot(Player const* player)
{
    if (!player || player->GetClass() != CLASS_HUNTER)
        return false;

    Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
    return autoRepeat && autoRepeat->GetSpellInfo() && autoRepeat->GetSpellInfo()->Id == 75;
}

void StopHunterAutoShotForStationaryCast(Player* player, char const* reason)
{
    if (!player || player->GetClass() != CLASS_HUNTER)
        return;

    if (HunterHasActiveAutoShot(player))
    {
        WhisperHunterCastDiagnostic(player, nullptr, "autoshot_interrupt_for_cast", 20904, reason);
        player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    }

    if (reason)
        SetLastMovementDebugStatus(player, reason);
}

constexpr uint32 kHunterFeignDeathSpellId = 5384;

bool IsHunterTrapSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    uint32 const firstRankSpellId = firstRank ? firstRank->Id : spellInfo->Id;

    switch (firstRankSpellId)
    {
        case 1499:  // Freezing Trap
        case 13795: // Immolation Trap
        case 13809: // Frost Trap
        case 13813: // Explosive Trap
            return true;
        default:
            return false;
    }
}

bool HasHunterFeignDeathAura(Player const* player)
{
    return player && player->GetClass() == CLASS_HUNTER && player->HasAura(kHunterFeignDeathSpellId);
}

void BreakHunterFeignDeath(Player* player)
{
    if (!HasHunterFeignDeathAura(player))
        return;

    player->RemoveAurasDueToSpell(kHunterFeignDeathSpellId);
    if (player->IsSitState())
        player->SetStandState(UNIT_STAND_STATE_STAND);
}

void ScheduleHunterFeignDeathStandup(Player* player)
{
    if (!player || player->GetClass() != CLASS_HUNTER)
        return;

    ObjectGuid const hunterGuid = player->GetGUID();
    player->m_Events.AddEventAtOffset([hunterGuid]()
    {
        Player* hunter = ObjectAccessor::FindConnectedPlayer(hunterGuid);
        if (!hunter || !hunter->IsInWorld() || !hunter->IsAlive())
            return;

        if (!HasHunterFeignDeathAura(hunter))
            return;

        hunter->RemoveAurasDueToSpell(kHunterFeignDeathSpellId);
        hunter->SetStandState(UNIT_STAND_STATE_STAND);

        if (MotionMaster* motionMaster = hunter->GetMotionMaster())
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
    }, std::chrono::milliseconds(1200));
}

constexpr uint32 kPlayerbotDispelCooldownToken = 900004;
constexpr uint32 kPlayerbotHandOfSacrificeCooldownToken = 900005;
constexpr uint32 kDruidCasterFaerieFireSpellId = 9907;
constexpr uint32 kHunterCallPetSpellId = 883;
constexpr uint32 kHunterRevivePetSpellId = 982;
constexpr uint32 kPlayerbotHunterStationaryCastLockToken = 900006;
constexpr uint32 kRacialNightElfShadowmeldSpellId = 20580;
constexpr uint32 kPlayerbotShadowmeldGraceToken = 900007;
// Environmental Magma periodically damages units standing on hazardous ground.
// Some custom terrain does not expose the matching liquid flags, so the aura is
// also a generic signal that the bot is currently standing in a hazard.
constexpr uint32 kEnvironmentalMagmaDamageAuraId = 57634;
constexpr std::chrono::seconds kPlayerbotDispelCooldown = std::chrono::seconds(5);
constexpr std::chrono::seconds kDruidCasterFaerieFireCooldown = std::chrono::seconds(10);
constexpr std::chrono::seconds kPlayerbotAutoRepeatRangedStartCooldown = std::chrono::seconds(2);
constexpr std::chrono::seconds kHunterPetFailureBackoff = std::chrono::seconds(12);

bool IsPlayerbotDispelSpell(uint32 spellId)
{
    if (!spellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;

    switch (firstRankSpellId)
    {
        case 370:  // Purge
        case 475:  // Remove Lesser Curse
        case 527:  // Dispel Magic
        case 2782: // Remove Curse
        case 2893: // Abolish Poison
        case 4987: // Cleanse
            return true;
        default:
            return false;
    }
}

SpellInfo const* GetFirstOnUseItemSpellInfo(Item const* item)
{
    if (!item)
        return nullptr;

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate)
        return nullptr;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& spellData = itemTemplate->Spells[i];
        if (spellData.SpellId <= 0 || spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellData.SpellId))
            return spellInfo;
    }

    return nullptr;
}

bool IsSurvivalHunter(Player const* player)
{
    return player && player->GetClass() == CLASS_HUNTER && player->HasTalent(19386, player->GetActiveSpec());
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
void MarkTargetRelativeMovementLaunch(Player const* player);
bool ShouldPreserveTargetRelativeMovement(Player const* player, Unit const* target, float desiredRange, uint32 minRunMs, char const* label, std::string* reasonOut);

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

    Map const* map = player->FindMap();
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

    Map const* map = player->FindMap();
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

    if (Map const* map = player->FindMap())
    {
        LiquidData liquidData{};
        if (map->GetLiquidStatus(player->GetPhaseMask(), adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(),
                adjustedZ + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight()))
        {
            bool const canWalkOnWater = player->HasAuraType(SPELL_AURA_WATER_WALK);
            if (!canWalkOnWater)
                adjustedZ = std::max(liquidData.depth_level + 0.05f, std::min(adjustedZ, liquidData.level - 0.25f));
        }
    }

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

// PathGenerator hands whatever position we give it straight to Detour's
// nearest-poly search with no signal back if that position was ambiguous
// (straddling a thin wall between two navmesh regions, or sampled against
// the wrong layer of a multi-level structure). A mid-spline reissue can
// catch the bot at exactly this kind of borderline spot - IssueStrictHumanMove
// replans from wherever the bot's live position happens to be at that exact
// instant, which is wherever the previous spline had carried it to, not
// necessarily anywhere previously validated. Re-ground the bot's own current
// position the same way BuildCollisionSafeDestination already re-grounds
// proposed destinations, and snap back to solid ground first if it has
// drifted meaningfully, before letting that position anchor a new path.
void ValidateAndCorrectCurrentPosition(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsInFlight())
        return;

    Position const current = player->GetPosition();
    Position const corrected = BuildCollisionSafeDestination(player, current);

    // GetMapHeight/UpdateAllowedPositionZ search around the input Z, so a
    // legitimately elevated bot (a bridge, a ramp) should resolve back to
    // very nearly its own height. Only correct on a drift large enough to
    // indicate the sampled position landed on the wrong layer entirely, not
    // ordinary stair-step/mesh sampling noise.
    float const verticalDrift = std::fabs(corrected.GetPositionZ() - current.GetPositionZ());
    if (verticalDrift < 3.0f)
        return;

    player->NearTeleportTo(corrected.GetPositionX(), corrected.GetPositionY(), corrected.GetPositionZ(), corrected.GetOrientation());
}

bool TryBuildStrictHumanSegmentDestination(Player* player, Position const& desiredDestination, Position& segmentDestination)
{
    if (!player)
        return false;

    ValidateAndCorrectCurrentPosition(player);

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
        float const downwardDelta = player->GetPositionZ() - resolvedDestination.GetPositionZ();
        if (planarDelta < 0.5f || verticalDelta > std::max(8.0f, planarDelta * 0.75f + 2.0f))
            return false;

        // Never accept a strict movement segment that snaps sharply below the
        // bot. Battleground floors, bridges, and ramps can have map-height
        // samples below the walkable surface; issuing a MovePoint to that lower
        // sample makes bots dive through the floor instead of pathing like a
        // player. Real descents still work by chaining short, path-generated
        // segments whose Z changes are proportional to their horizontal travel.
        if (!player->IsInWater() && downwardDelta > std::max(4.0f, planarDelta * 0.35f + 1.0f))
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
        Position lastRequestedDestination;
        Position lastDestination;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, MoveOrderState> stateByGuid;
    uint64 const botGuid = player->GetGUID().GetRawValue();
    MoveOrderState& state = stateByGuid[botGuid];
    uint32 const nowMs = GameTime::GetGameTimeMS();

    // Compare against the last *requested* destination (e.g. a follow point
    // trailing a moving target), not the last *resolved* segment endpoint.
    // The resolved segment is frequently shortened by the strict-pathing
    // fallback below, so comparing against it made destinationChanged true on
    // almost every call -- defeating the time throttle and causing the
    // active spline to be cleared and reissued as a short MovePoint nearly
    // every tick (visible as the bot inching instead of running smoothly).
    bool const destinationChanged = state.lastIssueMs == 0 ||
        state.lastRequestedDestination.GetExactDist(destination) >= destinationChangeThreshold;
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

    if (destinationChanged && !canReissueByTime)
    {
        // The requested destination drifted (e.g. a chased target kept
        // moving) but we are still inside the reissue throttle window. Do
        // not cancel an in-flight segment for that drift -- clearing and
        // reissuing a fresh short MovePoint every tick is what produces the
        // visible inch/stop stutter. Let the active point move continue; a
        // fresh segment will be issued once the throttle window elapses.
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

    state.lastRequestedDestination = destination;
    state.lastDestination = segmentDestination;
    state.lastIssueMs = nowMs;
    return true;
}

bool IssueStrictHumanFollow(Player* player, Unit* target, float desiredDistance)
{
    if (!player || !target)
        return false;

    // IssueStrictHumanMove's defaults (4y drift / 350ms) were tuned for a
    // short reactive retreat, not for continuously chasing a live target.
    // A moving PvP target routinely drifts more than 4y in under 350ms, so
    // every call here was clearing the active spline and reissuing a fresh
    // short segment before the previous one had time to actually execute -
    // visible as a bot taking one short step, stopping, taking another step,
    // stopping, etc. Give each segment room to run before reconsidering.
    return IssueStrictHumanMove(player, BuildFollowDestination(player, target, desiredDistance), 8.0f, 900);
}

bool IsSafeDownhillTerrainDestination(Player* player, Position const& destination)
{
    if (!player)
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return false;

    PositionFullTerrainStatus terrainStatus;
    map->GetFullTerrainStatusForPosition(player->GetPhaseMask(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ() + player->GetCollisionHeight(), terrainStatus, MAP_ALL_LIQUIDS, player->GetCollisionHeight());

    if (terrainStatus.floorZ <= INVALID_HEIGHT)
        return false;

    float const floorDelta = std::fabs(destination.GetPositionZ() - (terrainStatus.floorZ + player->GetHoverOffset()));
    if (floorDelta > 1.5f)
        return false;

    LiquidData liquidData{};
    if (map->GetLiquidStatus(player->GetPhaseMask(), destination.GetPositionX(), destination.GetPositionY(),
            destination.GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight()) &&
        !player->CanSwim() && !player->HasAuraType(SPELL_AURA_WATER_WALK))
        return false;

    return true;
}

bool TryBuildSafeDownhillTeleportDestination(Player* player, Unit* target, Position& teleportDestination, float& teleportStep, char const*& teleportMode)
{
    teleportMode = "none";
    if (!player || !target || !player->InBattleground())
        return false;

    float const verticalDeltaToTarget = player->GetPositionZ() - target->GetPositionZ();
    float const dxToTarget = target->GetPositionX() - player->GetPositionX();
    float const dyToTarget = target->GetPositionY() - player->GetPositionY();
    float const planarDistanceToTarget = std::sqrt(dxToTarget * dxToTarget + dyToTarget * dyToTarget);
    if (verticalDeltaToTarget <= 8.0f || planarDistanceToTarget <= 30.0f)
        return false;

    teleportStep = std::clamp(planarDistanceToTarget * 0.20f, 12.0f, 35.0f);
    float const teleportFraction = std::min(teleportStep / std::max(0.01f, planarDistanceToTarget), 1.0f);
    Position requestedDestination(
        player->GetPositionX() + dxToTarget * teleportFraction,
        player->GetPositionY() + dyToTarget * teleportFraction,
        player->GetPositionZ() - std::min(10.0f, verticalDeltaToTarget * teleportFraction),
        player->GetOrientation());

    PathGenerator path(player);
    path.SetPathLengthLimit(45.0f);
    bool const pathOk = path.CalculatePath(requestedDestination.GetPositionX(), requestedDestination.GetPositionY(), requestedDestination.GetPositionZ(), true);
    PathType const pathType = path.GetPathType();
    uint32 const forbiddenPathFlags = PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH;
    if (pathOk && (pathType & forbiddenPathFlags) == 0)
    {
        Movement::PointsArray const& points = path.GetPath();
        G3D::Vector3 const* bestPoint = nullptr;
        float bestPlanarDistance = 0.0f;
        for (G3D::Vector3 const& point : points)
        {
            float const dx = point.x - player->GetPositionX();
            float const dy = point.y - player->GetPositionY();
            float const planarDistance = std::sqrt(dx * dx + dy * dy);
            if (planarDistance > teleportStep + 1.0f)
                break;

            if (planarDistance >= 3.0f && planarDistance > bestPlanarDistance)
            {
                bestPoint = &point;
                bestPlanarDistance = planarDistance;
            }
        }

        if (bestPoint)
        {
            // bestPoint->z is Detour's raw navmesh sample for this corner.
            // The (x,y) and path connectivity are trustworthy - the path was
            // rejected above if it wasn't a real route - but the navmesh
            // surface itself is a simplified approximation of the actual
            // walkable geometry and can sit a little off true floor height,
            // especially over reused dungeon geometry with multiple room
            // volumes stacked in the same X/Y footprint. Re-ground it and
            // sanity-check the floor the same way the terrain-snap fallback
            // below already does, instead of teleporting to the raw sample.
            Position candidateDestination(bestPoint->x, bestPoint->y, bestPoint->z, player->GetOrientation());
            Position const safeCandidateDestination = BuildCollisionSafeDestination(player, candidateDestination);
            Map* pathSafetyMap = player->FindMap();
            bool const candidateHasLos = pathSafetyMap && pathSafetyMap->isInLineOfSight(
                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + player->GetCollisionHeight(),
                safeCandidateDestination.GetPositionX(), safeCandidateDestination.GetPositionY(), safeCandidateDestination.GetPositionZ() + player->GetCollisionHeight(),
                player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
            if (candidateHasLos && IsSafeDownhillTerrainDestination(player, safeCandidateDestination))
            {
                teleportDestination = safeCandidateDestination;
                teleportStep = bestPlanarDistance;
                teleportMode = "path_safe_teleport";
                return true;
            }
        }
    }

    // Some battleground graveyards/cliffs have no navmesh route down even
    // though the lower terrain is walkable. Fall back to a capped terrain snap
    // in the same generic downhill direction, but only accept points that snap
    // to the destination floor and are not unsafe liquid for this bot.
    Position terrainDestination = requestedDestination;
    float terrainZ = terrainDestination.GetPositionZ();
    player->UpdateAllowedPositionZ(terrainDestination.GetPositionX(), terrainDestination.GetPositionY(), terrainZ);
    terrainDestination.Relocate(terrainDestination.GetPositionX(), terrainDestination.GetPositionY(), terrainZ, player->GetOrientation());

    float const terrainDx = terrainDestination.GetPositionX() - player->GetPositionX();
    float const terrainDy = terrainDestination.GetPositionY() - player->GetPositionY();
    float const terrainPlanarDistance = std::sqrt(terrainDx * terrainDx + terrainDy * terrainDy);
    float const downwardDelta = player->GetPositionZ() - terrainDestination.GetPositionZ();
    if (terrainPlanarDistance >= 3.0f && terrainPlanarDistance <= teleportStep + 1.0f && downwardDelta > 1.0f &&
        IsSafeDownhillTerrainDestination(player, terrainDestination))
    {
        // The terrain-height query above finds *a* floor near that X/Y - not
        // necessarily one actually connected to where the bot is standing.
        // Battlegrounds built on top of a larger dungeon map (multiple rooms/
        // levels stacked in the same X/Y footprint) can have an unrelated
        // lower chamber's floor picked up here. NearTeleportTo has no path
        // validation at all, so require an unobstructed line of sight to the
        // candidate point first; if there is a wall/floor between here and
        // there, the "floor" that was found is on the other side of it, not
        // reachable from here, and teleporting to it drops the bot through
        // the world instead of down a legitimate slope/cliff.
        Map* map = player->FindMap();
        bool const hasLos = map && map->isInLineOfSight(
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + player->GetCollisionHeight(),
            terrainDestination.GetPositionX(), terrainDestination.GetPositionY(), terrainDestination.GetPositionZ() + player->GetCollisionHeight(),
            player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
        if (hasLos)
        {
            teleportDestination = terrainDestination;
            teleportStep = terrainPlanarDistance;
            teleportMode = "terrain_safe_teleport";
            return true;
        }
    }

    return false;
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
    uint32 lastLaunchMs = 0;
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
    state.lastLaunchMs = 0;
    // Do not treat order issuance itself as progress. These are updated only
    // after observed distance/position gains in ShouldPreserve... .
    state.lastProgressMs = 0;
    state.lastPositionProgressMs = 0;
    state.mode = mode;
}

void MarkTargetRelativeMovementLaunch(Player const* player)
{
    if (!player)
        return;

    auto itr = g_TargetRelativeMoveOrderByGuid.find(player->GetGUID().GetRawValue());
    if (itr == g_TargetRelativeMoveOrderByGuid.end())
        return;

    bool const hasActiveSpline = player->movespline && player->movespline->Initialized() && !player->movespline->Finalized();
    bool const launched = player->isMoving() ||
        player->HasUnitState(UNIT_STATE_CHASE_MOVE) ||
        player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ||
        hasActiveSpline;
    if (!launched)
        return;

    itr->second.lastLaunchMs = GameTime::GetGameTimeMS();
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

    bool const madePositionProgress = positionDelta2D >= 0.35f || positionDelta3D >= 0.50f;
    bool const splineInitialized = player->movespline && player->movespline->Initialized() && !player->movespline->Finalized();
    bool const splineStarted = splineInitialized && player->movespline->HasStarted();
    bool const hasMovementSignal = player->isMoving() ||
        player->HasUnitState(UNIT_STATE_CHASE_MOVE) ||
        player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ||
        splineInitialized;
    bool const rawDistanceProgress = state.lastDistance > 0.0f && currentDistance + 0.20f < state.lastDistance;
    bool const madeDistanceProgress = rawDistanceProgress && (hasMovementSignal || madePositionProgress);

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
    uint32 const launchAgeMs = state.lastLaunchMs != 0 && nowMs >= state.lastLaunchMs ? nowMs - state.lastLaunchMs : 0;

    // Do not keep a dead CHASE/FOLLOW generator alive for the whole normal
    // settle window. The bad screenshots show motion=CHASE/FOLLOW with
    // moving=no, chase_move=no, follow_move=no, and spline_started=no. In that
    // state the order has not actually launched; preserving it for 1.5-3s
    // pins rogues/casters at spawn or makes ranged bots appear stuck.
    uint32 const effectiveSettleMs = hasMovementSignal ? minRunMs : std::min<uint32>(minRunMs, 350);
    bool inSettleWindow = ageMs < effectiveSettleMs;
    bool const recentLaunch = state.lastLaunchMs != 0 && launchAgeMs < 1200;
    bool const credibleRecentLaunch = recentLaunch && (hasMovementSignal || splineStarted);
    bool const recentDistanceProgress = state.lastProgressMs != 0 && distanceProgressAgeMs < minRunMs;
    bool const recentPositionProgress = state.lastPositionProgressMs != 0 && positionProgressAgeMs < minRunMs;
    bool const credibleRecentDistanceProgress = recentDistanceProgress && (hasMovementSignal || recentPositionProgress || madePositionProgress);
    bool const farFromDesiredRange = desiredRange > 0.0f && currentDistance > (desiredRange + 4.0f);
    bool const aggressiveUnlaunchedBattleground =
        player->InBattleground() &&
        farFromDesiredRange &&
        !hasMovementSignal &&
        !player->isMoving() &&
        !splineStarted &&
        !splineInitialized;
    bool const pathologicalUnlaunchedHold =
        player->InBattleground() &&
        farFromDesiredRange &&
        !hasMovementSignal &&
        !player->isMoving() &&
        !splineStarted &&
        !splineInitialized &&
        !madeDistanceProgress &&
        !madePositionProgress &&
        distanceProgressAgeMs > 1500 &&
        positionProgressAgeMs > 1500;

    // Elegant fail-safe for the "stuck but preserved" state:
    // if we are still significantly outside desired range and have neither
    // recent launch nor distance progress, stop preserving and reissue a fresh
    // target-relative order immediately.
    if (inSettleWindow && farFromDesiredRange && ageMs > 900 && !credibleRecentLaunch && !madeDistanceProgress && !credibleRecentDistanceProgress)
        inSettleWindow = false;

    // Battleground cliff/ledge stalls regularly present as repeated CHASE/FOLLOW
    // orders that never launch (no spline, no movement signal, moving=no) while
    // still very far from desired range. These can be reissued every tick, which
    // keeps age_ms near zero and would bypass any age-based gate. Break preserve
    // immediately so callers can clear/reissue and recovery paths can engage.
    if (aggressiveUnlaunchedBattleground)
        inSettleWindow = false;

    if (pathologicalUnlaunchedHold)
        inSettleWindow = false;

    bool const preserve = inSettleWindow || credibleRecentLaunch || madeDistanceProgress || madePositionProgress || credibleRecentDistanceProgress || recentPositionProgress;

    if (!preserve)
    {
        if (reasonOut)
        {
            std::ostringstream diag;
            diag << (label ? label : "target_relative_motion_not_preserved")
                 << " motion=" << uint32(motionType)
                 << " mode=" << uint32(state.mode)
                 << " age_ms=" << ageMs
                 << " launch_age_ms=" << launchAgeMs
                 << " launch_credible=" << (credibleRecentLaunch ? "yes" : "no")
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
                 << " spline_init=" << (splineInitialized ? "yes" : "no")
                 << " spline_started=" << (splineStarted ? "yes" : "no")
                 << " movement_signal=" << (hasMovementSignal ? "yes" : "no")
                 << " raw_dist_progress=" << (rawDistanceProgress ? "yes" : "no")
                 << " effective_settle_ms=" << effectiveSettleMs
                 << " far_desired=" << (farFromDesiredRange ? "yes" : "no")
                 << " not_move=" << (player->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
                 << " casting_prevent=" << (player->IsMovementPreventedByCasting() ? "yes" : "no")
                 << " aggressive_unlaunched_bg=" << (aggressiveUnlaunchedBattleground ? "yes" : "no")
                 << " pathological_unlaunched_hold=" << (pathologicalUnlaunchedHold ? "yes" : "no")
                 << " reason=" << (aggressiveUnlaunchedBattleground
                    ? "aggressive_unlaunched_bg"
                    : (pathologicalUnlaunchedHold
                    ? "pathological_unlaunched_hold"
                    : (!hasMovementSignal && ageMs >= effectiveSettleMs ? "unlaunched_settle_expired" : "no_position_or_distance_progress")));
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
             << " launch_age_ms=" << launchAgeMs
             << " launch_credible=" << (credibleRecentLaunch ? "yes" : "no")
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
             << " spline_init=" << (splineInitialized ? "yes" : "no")
             << " spline_started=" << (splineStarted ? "yes" : "no")
             << " movement_signal=" << (hasMovementSignal ? "yes" : "no")
             << " raw_dist_progress=" << (rawDistanceProgress ? "yes" : "no")
             << " effective_settle_ms=" << effectiveSettleMs
             << " far_desired=" << (farFromDesiredRange ? "yes" : "no")
             << " not_move=" << (player->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
             << " casting_prevent=" << (player->IsMovementPreventedByCasting() ? "yes" : "no")
             << " aggressive_unlaunched_bg=" << (aggressiveUnlaunchedBattleground ? "yes" : "no")
             << " pathological_unlaunched_hold=" << (pathologicalUnlaunchedHold ? "yes" : "no")
             << " reason=" << (inSettleWindow
                    ? (hasMovementSignal ? "settle_window" : "unlaunched_short_settle")
                    : (credibleRecentLaunch ? "recent_launch" : (madePositionProgress || recentPositionProgress ? "position_progress" : "distance_progress")));
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
    MarkTargetRelativeMovementLaunch(player);
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
    bool canUseHostileChase = targetAttackable && !forceFollow;
    if (canUseHostileChase && player->GetVictim() != target)
    {
        player->Attack(target, false);
        canUseHostileChase = player->GetVictim() == target;
    }

    if (canUseHostileChase)
    {
        motionMaster->MoveChase(target, BuildEdgeDistanceChaseRange(safeDistance));
        RecordTargetRelativeMovementOrder(player, target, safeDistance, 1);
        MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
        MarkTargetRelativeMovementLaunch(player);
        prime.addToWorldCalled = preparedMotionMaster;
        if (primeOut)
            *primeOut = prime;
        return TargetRelativeRangedMoveResult::ChaseIssued;
    }

    motionMaster->MoveFollow(target, safeDistance, player->GetFollowAngle());
    RecordTargetRelativeMovementOrder(player, target, safeDistance, 2);
    MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
    MarkTargetRelativeMovementLaunch(player);
    prime.addToWorldCalled = preparedMotionMaster;
    if (primeOut)
        *primeOut = prime;
    return TargetRelativeRangedMoveResult::FollowIssued;
}

TargetRelativeRangedMoveResult IssueContactChaseRescue(Player* player, Unit* target, MotionPrimeResult* primeOut = nullptr)
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
        MarkTargetRelativeMovementLaunch(player);
        prime.addToWorldCalled = preparedMotionMaster;
        if (primeOut)
            *primeOut = prime;
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
    MarkTargetRelativeMovementLaunch(player);
    prime.addToWorldCalled = preparedMotionMaster;
    if (primeOut)
        *primeOut = prime;
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

    return diag.str();
}

bool IsHunterExactDeadZone(Player const* player, Unit const* target)
{
    if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive())
        return false;

    float const distance = player->GetExactDist(target);
    return distance > 5.0f && distance < 8.0f;
}

bool IsHunterAutoShotBand(Player const* player, Unit const* target)
{
    if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive() || !player->HasSpell(75))
        return false;

    SpellInfo const* autoShotInfo = sSpellMgr->GetSpellInfo(75);
    if (!autoShotInfo)
        return false;

    if (!player->IsWithinLOSInMap(target))
        return false;

    float const exactDistance = player->GetExactDist(target);
    float const minAutoShotRange = autoShotInfo->GetMinRange(false);
    float const maxAutoShotRange = autoShotInfo->GetMaxRange(false);
    return exactDistance > std::max(minAutoShotRange + 0.75f, 8.75f) && exactDistance <= maxAutoShotRange;
}

void StopHunterDamageOnBreakableCrowdControl(Player* player, Unit* target, char const* reason);

void StopHunterAndStartAutoShot(Player* player, Unit* target, char const* reason)
{
    if (!player || !target || !target->IsAlive())
        return;

    if (TryMoveOutOfHazardousLiquid(player))
        return;

    if (target->HasBreakableByDamageCrowdControlAura())
    {
        StopHunterDamageOnBreakableCrowdControl(player, target, "hunter_hold_autoshot_suppressed_breakable_cc");
        return;
    }

    MotionMaster* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const motionBefore = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    if (motionMaster)
        motionMaster->Clear(MOTION_SLOT_ACTIVE);

    player->StopMoving();
    if (WorldSession* session = player->GetSession(); session && session->IsVirtualSession())
    {
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
        player->SendMovementFlagUpdate();
    }

    player->SetFacingToObject(target);
    player->SetInFront(target);

    Spell const* autoRepeatSpell = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
    bool const autoShotActive = autoRepeatSpell && autoRepeatSpell->GetSpellInfo() && autoRepeatSpell->GetSpellInfo()->Id == 75;
    if (!autoShotActive && player->HasSpell(75))
        player->CastSpell(target, 75, false);

    std::ostringstream diag;
    diag << (reason ? reason : "hunter_auto_shot_hold")
         << " dist=" << player->GetDistance(target)
         << " exact=" << player->GetExactDist(target)
         << " motion_before=" << uint32(motionBefore)
         << " motion_after=" << (motionMaster ? uint32(motionMaster->GetCurrentMovementGeneratorType()) : 0)
         << " auto_active=" << (autoShotActive ? "yes" : "no");
    SetLastMovementDebugStatus(player, diag.str());
}

float ComputeHunterDeadZoneRetreatStep(Player const* player, Unit const* target)
{
    if (!player || !target)
        return 6.0f;

    float const currentDistance = player->GetExactDist(target);
    return std::clamp(12.0f - currentDistance, 4.0f, 10.0f);
}

void IssueHunterDeadZoneRetreatMovement(Player* player, Unit* target, char const* reason)
{
    if (!player || !target || !target->IsAlive())
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    float const currentDistance = player->GetExactDist(target);
    float const retreatStep = ComputeHunterDeadZoneRetreatStep(player, target);
    float const angleToTarget = player->GetAbsoluteAngle(target->GetPosition());

    Position destination = player->GetPosition();
    destination.RelocateOffset({ std::cos(angleToTarget + static_cast<float>(M_PI)) * retreatStep,
        std::sin(angleToTarget + static_cast<float>(M_PI)) * retreatStep, 0.0f, 0.0f });

    MovementGeneratorType const motionBefore = motionMaster->GetCurrentMovementGeneratorType();
    bool issuedStrict = false;
    bool const strictPathingRequired = RequiresStrictHumanPathing(player);
    if (strictPathingRequired)
    {
        // These were 2.0f/150ms - tight enough that a hunter kiting a live
        // target reissued a fresh short segment several times a second,
        // clearing the previous one before it had finished executing. Beyond
        // the visible stutter, every reissue re-runs the segment's floor/
        // height computation from scratch, and re-running that far more
        // often than necessary meant far more chances to land on an
        // ambiguous height sample on this map's stacked geometry. Loosened
        // to still react quickly to being pushed into/out of the dead zone,
        // but without re-triggering on every few hundred milliseconds of
        // ordinary target movement.
        issuedStrict = IssueStrictHumanMove(player, destination, 4.0f, 500);
    }

    // If strict pathing was attempted and explicitly failed, PathGenerator
    // has already determined this retreat destination is not safely
    // reachable (wall, unnavigable geometry, etc). Falling back to a raw
    // MovePoint here is exactly what was walking kiting hunters straight
    // through walls and eventually below the map - do nothing instead and
    // let the next tick re-evaluate. Outside a battleground (no strict
    // pathing attempted at all) keep the simpler point move.
    if (!issuedStrict && !strictPathingRequired)
    {
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
        motionMaster->MovePoint(0, BuildCollisionSafeDestination(player, destination), true);
    }

    std::ostringstream diag;
    diag << BuildRangedMovementDiag(player, target,
        reason ? reason : "hunter_deadzone_retreat", 8.0f, 12.0f,
        player->IsWithinLOSInMap(target), player->IsValidAttackTarget(target), true, motionBefore,
        issuedStrict ? "strict_retreat" : "point_retreat")
         << " current_exact=" << currentDistance
         << " retreat_step=" << retreatStep;
    SetLastMovementDebugStatus(player, diag.str());
}

bool IsStaleTargetRelativeMotion(Player const* player)
{
    if (!player)
        return false;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const motionType = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    bool const hasActiveSpline = player->movespline && player->movespline->Initialized() && !player->movespline->Finalized();
    return (motionType == CHASE_MOTION_TYPE || motionType == FOLLOW_MOTION_TYPE) &&
        !player->isMoving() &&
        !player->HasUnitState(UNIT_STATE_CHASE_MOVE) &&
        !player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) &&
        !hasActiveSpline &&
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

bool IsGapCloserSpell(uint32 spellId)
{
    switch (spellId)
    {
        case 11578: // Charge
        case 20617: // Intercept
        case 81271: // Heroic Leap
        case 82419: // Rehgar's Fury
        case 49376: // Feral Charge - Cat
        case 16979: // Feral Charge - Bear
            return true;
        default:
            return false;
    }
}

void IssueRangedApproachMovement(Player* player, Unit* target, float desiredDistance, bool forceMovementWhenAlreadyInRange = false, char const* forcedReason = nullptr)
{
    if (!player || !target)
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    // See the matching guard in IssueMeleeApproachMovement: a charge/leap
    // effect (Rehgar's Fury included) drives its own EFFECT_MOTION_TYPE
    // motion once cast, and the very next decision tick landing here would
    // otherwise clear that in-flight spline and replace it with an ordinary
    // chase/follow order before the charge has actually landed.
    if (HasActiveMovementEffectSpline(player))
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
        uint8 lastIssuedMode = 0; // 1=chase, 2=follow, 3=contact_rescue
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

    // Hunters should not chase inward just because an instant shot is out of its
    // shorter spell range. If Auto Shot is valid, hold ground and shoot; only
    // close later when the target is actually beyond ranged-weapon range.
    if (!forceMovementWhenAlreadyInRange && targetAttackable && IsHunterAutoShotBand(player, target))
    {
        StopHunterAndStartAutoShot(player, target, "hunter_ranged_approach_suppressed_autoshot_band");
        stallState.targetGuid = target->GetGUID();
        stallState.lastDistance = currentDistance;
        stallState.lastSampleMs = GameTime::GetGameTimeMS();
        stallState.lastIssuedRange = 0.0f;
        stallState.lastIssuedMode = 0;
        return;
    }
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
    bool const hasActiveSpline = player->movespline && player->movespline->Initialized() && !player->movespline->Finalized();
    bool const hasFinalizedSpline = player->movespline && player->movespline->Initialized() && player->movespline->Finalized();
    bool const movementGeneratorHasNotLaunched = !player->isMoving() &&
        !player->HasUnitState(UNIT_STATE_CHASE_MOVE) &&
        !player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) &&
        !hasActiveSpline;
    uint32 const lastIssueAgeMs = sameStallTarget && stallState.lastIssueMs != 0 && nowMs >= stallState.lastIssueMs ? nowMs - stallState.lastIssueMs : 0;
    bool const staleUnlaunchedTargetRelative =
        sameStallTarget &&
        activeTargetRelativeMotion &&
        movementGeneratorHasNotLaunched &&
        stallState.lastIssueMs != 0 &&
        lastIssueAgeMs >= 700;

    if (!forceMovementWhenAlreadyInRange && activeTargetRelativeMotion && currentDistance > (safeDistance + 0.75f))
    {
        bool const staleFinalizedSplineHold =
            sameStallTarget &&
            activeTargetRelativeMotion &&
            hasFinalizedSpline &&
            !player->isMoving() &&
            !player->HasUnitState(UNIT_STATE_CHASE_MOVE) &&
            !player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) &&
            stallState.lastIssueMs != 0 &&
            lastIssueAgeMs >= 500;

        if (staleFinalizedSplineHold)
        {
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
            std::ostringstream resetDiag;
            resetDiag << BuildRangedMovementDiag(player, target, "ranged_stale_finalized_spline_cleared",
                safeDistance, safeDistance, targetLos, targetAttackable, true, initialMotionType, "none")
                     << " issue_age_ms=" << lastIssueAgeMs;
            SetLastMovementDebugStatus(player, resetDiag.str());
        }

        if (staleUnlaunchedTargetRelative)
        {
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
            std::ostringstream resetDiag;
            resetDiag << BuildRangedMovementDiag(player, target, "ranged_stale_unlaunched_cleared",
                safeDistance, safeDistance, targetLos, targetAttackable, true, initialMotionType, "none")
                     << " issue_age_ms=" << lastIssueAgeMs;
            SetLastMovementDebugStatus(player, resetDiag.str());
        }

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

    // Disabled entirely (2026-07-12): this was built to un-stick bots that
    // spawned at Twin Peaks (map 726) graveyards where the release point has
    // no VMAP area data (VMAP AreaInfo ok=0) and PathGenerator can't route
    // them down to the walkable ground below at all. Applied broadly to any
    // 8y+ vertical / 30y+ horizontal gap, it became a general-purpose
    // "can't find a path, so blind-teleport toward the target" hammer that
    // was dropping bots through the floor on other maps (confirmed on
    // Blackrock Throne) even after hardening both destination-selection
    // branches with LOS/floor checks - the underlying problem is that a
    // terrain-height or navmesh-corner sample near reused/complex geometry
    // can land inside a different, disconnected room below the intended
    // floor, and no single-point sanity check reliably catches every case of
    // that. If the graveyard-stuck problem needs solving again, it should be
    // scoped narrowly to graveyard release positions specifically rather
    // than firing for any bot with a large vertical/horizontal gap to target.
    bool const shouldForceDownhillCommit = false;
    if (shouldForceDownhillCommit)
    {
        Position teleportDestination;
        float teleportStep = 0.0f;
        char const* teleportMode = "none";
        if (TryBuildSafeDownhillTeleportDestination(player, target, teleportDestination, teleportStep, teleportMode))
        {
            float const preTeleportDistance = player->GetDistance(teleportDestination);
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
            player->NearTeleportTo(teleportDestination, false);

            stallState.targetGuid = target->GetGUID();
            stallState.lastDistance = currentDistance;
            stallState.lastSampleMs = nowMs;
            stallState.lastIssueMs = nowMs;
            stallState.lastIssuedRange = safeDistance;
            stallState.lastIssuedMode = 1;

            std::ostringstream diag;
            diag << BuildRangedMovementDiag(player, target, "bg_downhill_commit_safe_teleport",
                safeDistance, safeDistance, targetLos, targetAttackable, true, initialMotionType, teleportMode)
                 << " teleport_dist=" << preTeleportDistance
                 << " teleport_step=" << teleportStep
                 << " cooldown_ms=3000";
            SetLastMovementDebugStatus(player, diag.str());
            return;
        }
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

            // Give the queued generator a short settle window after a failed
            // prime, then force a reissue if still unlaunched. Waiting ~2.5s
            // here left bots visibly stuck with motion=follow/chase but
            // moving=no, no spline, and no CHASE_MOVE/FOLLOW_MOVE state.
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

        bool const staleQueuedGenerator = sameStallTarget && activeTargetRelativeMotion && movementGeneratorHasNotLaunched && stallState.lastIssueMs != 0 && lastIssueAgeMs >= 900;
        float const forcedRange = std::max(1.0f, safeDistance - 8.0f);
        bool const prefersContactRescue = requestedSafeDistance <= 8.0f;
        bool const shouldEscalateContactRescue = targetAttackable && staleQueuedGenerator && lastIssueAgeMs >= 1400 && prefersContactRescue;
        uint8 const prevIssuedMode = stallState.lastIssuedMode;
        uint32 const mmSizeBeforeIssue = motionMaster->Size();
        MovementGeneratorType const mmMotionBeforeIssue = motionMaster->GetCurrentMovementGeneratorType();

        if (targetAttackable && (player->GetVictim() != target || !player->IsInCombat()))
            player->Attack(target, false);

        RangedPathProbeResult const chaseProbe = ProbeChasePath(player, target);
        RangedPathProbeResult const followProbe = targetAttackable ? RangedPathProbeResult() : FindBestFollowProbe(player, target, forcedRange);

        bool const shouldClearBeforeIssue = initialMotionType == POINT_MOTION_TYPE;
        if (shouldClearBeforeIssue)
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
        MotionPrimeResult primeResult;
        TargetRelativeRangedMoveResult const moveResult = shouldEscalateContactRescue
            ? IssueContactChaseRescue(player, target, &primeResult)
            : ((!targetAttackable && staleQueuedGenerator && IsUsableProbePath(followProbe))
            ? IssuePathProbedFollow(player, target, followProbe, forcedRange, &primeResult)
            : IssueTargetRelativeRangedMovement(player, target, forcedRange, targetAttackable, false, &primeResult));
        uint32 const mmSizeAfterIssue = motionMaster->Size();
        MovementGeneratorType const mmMotionAfterIssue = motionMaster->GetCurrentMovementGeneratorType();

        stallState.targetGuid = target->GetGUID();
        stallState.lastDistance = currentDistance;
        stallState.lastSampleMs = nowMs;
        stallState.lastFallbackMs = nowMs;
        stallState.lastIssueMs = nowMs;
        stallState.lastIssuedRange = forcedRange;
        stallState.lastIssuedMode = shouldEscalateContactRescue ? 3 : (moveResult == TargetRelativeRangedMoveResult::FollowIssued ? 2 : 1);
        stallState.stagnantSamples = staleQueuedGenerator ? 1 : 0;

        char const* label = "near_edge_chase_nudge";
        if (staleQueuedGenerator)
        {
            if (shouldEscalateContactRescue)
                label = "near_edge_stale_contact_rescue";
            else if (!targetAttackable && IsUsableProbePath(followProbe))
                label = "near_edge_stale_pathprobed_follow";
            else
                label = "near_edge_stale_reissued_chase";
        }

        std::string diag = BuildRangedMovementDiag(player, target, label,
            safeDistance, forcedRange, targetLos, targetAttackable, true, initialMotionType,
            GetTargetRelativeRangedMoveResultLabel(moveResult));
        std::ostringstream extra;
        extra << diag
              << " stale=" << (staleQueuedGenerator ? "yes" : "no")
              << " issue_age_ms=" << lastIssueAgeMs
              << " cleared_before_issue=" << (shouldClearBeforeIssue ? "yes" : "no")
              << " contact_rescue_allowed=" << (prefersContactRescue ? "yes" : "no")
              << " rescue_prev_mode=" << uint32(prevIssuedMode)
              << " mm_size_before_issue=" << mmSizeBeforeIssue
              << " mm_size_after_issue=" << mmSizeAfterIssue
              << " mm_motion_before_issue=" << uint32(mmMotionBeforeIssue)
              << " mm_motion_after_issue=" << uint32(mmMotionAfterIssue)
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
        sameStallTarget && stallState.lastIssueMs != 0 && lastIssueAgeMs >= 3000;

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

    // Very long battleground approaches can exceed the practical launch range
    // of target-relative Chase/Follow. The diagnostic for this failure is a
    // CHASE/FOLLOW generator at hundreds of yards with no moving flag, no
    // CHASE_MOVE/FOLLOW_MOVE state, and no spline. In that case use the same
    // navmesh-validated strict segment walker used by other battleground
    // movement instead of repeatedly installing an unlaunched target-relative
    // generator. Near spell range still stays target-relative so range-edge
    // spacing remains attached to the target.
    bool const farStrictSegmentApproach = strictPathing && currentDistance > std::max(90.0f, safeDistance + 60.0f);
    if (farStrictSegmentApproach)
    {
        bool const strictIssued = IssueStrictHumanFollow(player, target, safeDistance);
        if (strictIssued)
        {
            g_TargetRelativeMoveOrderByGuid.erase(player->GetGUID().GetRawValue());
            stallState.targetGuid = target->GetGUID();
            stallState.lastDistance = currentDistance;
            stallState.lastSampleMs = nowMs;
            stallState.lastFallbackMs = nowMs;
            stallState.lastIssueMs = nowMs;
            stallState.lastIssuedRange = safeDistance;
            stallState.lastIssuedMode = 0;
            stallState.stagnantSamples = 0;

            std::ostringstream diag;
            diag << BuildRangedMovementDiag(player, target, "strict_far_segment_move",
                safeDistance, safeDistance, targetLos, targetAttackable, activeTargetRelativeMotion, initialMotionType, "strict_point")
                 << " far_segment=yes"
                 << " force_in_range=" << (forceMovementWhenAlreadyInRange ? "yes" : "no")
                 << " forced_reason=" << (forcedReason ? forcedReason : "none")
                 << " requested_edge=" << requestedSafeDistance
                 << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
                 << " moving_after=" << (player->isMoving() ? "yes" : "no");
            SetLastMovementDebugStatus(player, diag.str());
            return;
        }
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
        RangedPathProbeResult const followProbe = hostileTarget ? RangedPathProbeResult() : FindBestFollowProbe(player, target, fallbackRange);
        MotionPrimeResult fallbackPrimeResult;
        TargetRelativeRangedMoveResult const fallbackMoveResult = (!hostileTarget && IsUsableProbePath(followProbe))
            ? IssuePathProbedFollow(player, target, followProbe, fallbackRange, &fallbackPrimeResult)
            : IssueTargetRelativeRangedMovement(player, target, fallbackRange, hostileTarget, false, &fallbackPrimeResult);
        stallState.lastFallbackMs = nowMs;
        {
            std::ostringstream diag;
            diag << (!hostileTarget && IsUsableProbePath(followProbe) ? "stagnant_pathprobed_follow" : "stagnant_reissued_target_relative_chase")
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

void IssueBehindTargetMeleeMovement(Player* player, Unit* target)
{
    if (!player || !target || !target->IsAlive())
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    if (player->IsValidAttackTarget(target) && (player->GetVictim() != target || !player->IsInCombat()))
        player->Attack(target, false);

    float constexpr behindFollowRange = 1.5f;
    float constexpr behindAngle = static_cast<float>(M_PI);
    float constexpr behindAngleTolerance = static_cast<float>(M_PI_4);

    struct BehindFollowOrderState
    {
        ObjectGuid targetGuid = ObjectGuid::Empty;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, BehindFollowOrderState> stateByGuid;
    BehindFollowOrderState& state = stateByGuid[player->GetGUID().GetRawValue()];
    uint32 const nowMs = GameTime::GetGameTimeMS();
    bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + 750;
    bool const targetChanged = state.targetGuid != target->GetGUID();
    if (!targetChanged && !canReissueByTime && motionMaster->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        SetLastMovementDebugStatus(player, "behind_target_follow_preserved");
        return;
    }

    RangedPathProbeResult const behindProbe = ProbeFollowPath(player, target, behindFollowRange, behindAngle);
    bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
    motionMaster->MoveFollow(target, behindFollowRange, ChaseAngle(behindAngle, behindAngleTolerance));
    RecordTargetRelativeMovementOrder(player, target, behindFollowRange, 2);
    MotionPrimeResult prime = PrimeTargetRelativeMotion(player);
    MarkTargetRelativeMovementLaunch(player);
    prime.addToWorldCalled = preparedMotionMaster;

    state.targetGuid = target->GetGUID();
    state.lastIssueMs = nowMs;

    std::ostringstream diag;
    diag << "behind_target_follow"
         << " target_has_caster_in_front=" << (target->HasInArc(static_cast<float>(M_PI), player) ? "yes" : "no")
         << " follow_range=" << behindFollowRange
         << " follow_angle=" << behindAngle
         << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
         << " moving_after=" << (player->isMoving() ? "yes" : "no")
         << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
         << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no");
    AppendMotionPrimeDiag(diag, prime);
    AppendProbeDiag(diag, "behind_probe", behindProbe);
    SetLastMovementDebugStatus(player, diag.str());
}

bool IsBehindTargetRequiredAndMissing(Player const* player, Unit const* target, SpellInfo const* spellInfo)
{
    return player && target && spellInfo &&
        spellInfo->HasAttribute(SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET) &&
        target->HasInArc(static_cast<float>(M_PI), player);
}

void IssueMeleeApproachMovement(Player* player, Unit* target)
{
    if (!player || !target)
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    // A charge/leap-type spell (Charge, Intercept, Heroic Leap, Feral Charge,
    // Rehgar's Fury, ...) drives its own EFFECT_MOTION_TYPE movement once
    // cast. The spell itself goes on cooldown immediately, so the very next
    // decision tick - often before the charge has actually landed - picks a
    // different action and lands here wanting to close the same gap, which
    // clears the in-flight charge spline and replaces it with an ordinary
    // MoveChase/MoveFollow. That reads as the charge "getting hijacked"
    // mid-flight. Let the native charge motion finish on its own.
    if (HasActiveMovementEffectSpline(player))
        return;

    if (player->HasStealthAura())
    {
        // For hostile stealth openers, always use default MoveChase.
        // This keeps the behavior deterministic and avoids fragile
        // angle/near-point follow probes that can stall in BGs.
        if (player->IsValidAttackTarget(target))
        {
            if (player->GetVictim() != target || !player->IsInCombat())
                player->Attack(target, false);

            float const stealthTravelDistance = player->GetDistance(target);

            // In battlegrounds, very distant rogue stealth openers can install
            // FOLLOW_MOTION_TYPE without ever launching a spline. The visible
            // diagnostic is motion=14/follow, moving=no, follow_move=no,
            // finalized/zero-duration spline, and no position progress. Use
            // the strict navmesh segment walker until the rogue is close enough
            // for target-relative chase/follow to be reliable.
            if (RequiresStrictHumanPathing(player) && stealthTravelDistance > 90.0f)
            {
                if (IssueStrictHumanFollow(player, target, 3.0f))
                {
                    g_TargetRelativeMoveOrderByGuid.erase(player->GetGUID().GetRawValue());

                    std::ostringstream diag;
                    diag << "stealth_far_strict_segment_move"
                         << " travel_dist=" << stealthTravelDistance
                         << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
                         << " moving_after=" << (player->isMoving() ? "yes" : "no");
                    SetLastMovementDebugStatus(player, diag.str());
                    return;
                }
            }

            std::string preserveDiag;
            if (ShouldPreserveTargetRelativeMovement(player, target, 1.5f, 2000, "stealth_melee_existing_motion_preserved", &preserveDiag))
            {
                SetLastMovementDebugStatus(player, preserveDiag);
                return;
            }

            bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
            bool const hasVictimLink = player->GetVictim() == target;
            bool const useStealthTravelFollow = stealthTravelDistance > 8.0f;
            if (hasVictimLink && !useStealthTravelFollow)
            {
                motionMaster->MoveChase(target);
                RecordTargetRelativeMovementOrder(player, target, 0.5f, 1);
            }
            else
            {
                float const stealthFollowRange = useStealthTravelFollow ? 3.0f : 1.5f;
                motionMaster->MoveFollow(target, stealthFollowRange, player->GetFollowAngle());
                RecordTargetRelativeMovementOrder(player, target, stealthFollowRange, 2);
            }
            MotionPrimeResult stealthPrimeResult = PrimeTargetRelativeMotion(player);
            MarkTargetRelativeMovementLaunch(player);
            stealthPrimeResult.addToWorldCalled = preparedMotionMaster;

            std::ostringstream diag;
            diag << "stealth_melee_chase"
                 << " issued_mode=" << (hasVictimLink ? (useStealthTravelFollow ? "follow_stealth_travel" : "chase") : "follow_no_victim")
                 << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
                 << " moving_after=" << (player->isMoving() ? "yes" : "no")
                 << " target_is_victim=" << (hasVictimLink ? "yes" : "no")
                 << " stealth_travel_follow=" << (useStealthTravelFollow ? "yes" : "no")
                 << " travel_dist=" << stealthTravelDistance
                 << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
                 << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no");
            AppendMotionPrimeDiag(diag, stealthPrimeResult);
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


void IssueGapCloserRangeApproachMovement(Player* player, Unit* target, float maxRange)
{
    if (!player || !target || maxRange <= 0.0f)
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    if (HasActiveMovementEffectSpline(player))
        return;

    struct GapCloserApproachOrderState
    {
        ObjectGuid targetGuid = ObjectGuid::Empty;
        Position destination;
        uint32 lastIssueMs = 0;
    };

    static std::unordered_map<uint64, GapCloserApproachOrderState> stateByGuid;
    GapCloserApproachOrderState& state = stateByGuid[player->GetGUID().GetRawValue()];

    // Move to a stable point safely inside the gap-closer's real max range
    // instead of chasing all the way to melee. Reusing the point for a short
    // window prevents the cast retry loop from clearing and rebuilding the
    // spline every tick, which is the visible inch/stop stutter reported for
    // Charge at 25y while the warrior was still ~36y away.
    float const desiredRange = std::max(1.0f, maxRange - 2.0f);
    Position const destination = BuildFollowDestination(player, target, desiredRange);
    uint32 const nowMs = GameTime::GetGameTimeMS();
    bool const sameTarget = state.targetGuid == target->GetGUID();
    bool const destinationStable = sameTarget && state.lastIssueMs != 0 && state.destination.GetExactDist(destination) < 6.0f;
    bool const activePointMove = motionMaster->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
        (player->isMoving() || (player->movespline && player->movespline->Initialized() && !player->movespline->Finalized()));

    if (destinationStable && activePointMove && nowMs < state.lastIssueMs + 1000)
        return;

    bool issued = false;
    if (RequiresStrictHumanPathing(player))
        issued = IssueStrictHumanMove(player, destination, 8.0f, 1000);

    if (!issued)
    {
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
        motionMaster->MovePoint(0, BuildCollisionSafeDestination(player, destination), true);
        issued = true;
    }

    if (issued)
    {
        g_TargetRelativeMoveOrderByGuid.erase(player->GetGUID().GetRawValue());
        state.targetGuid = target->GetGUID();
        state.destination = destination;
        state.lastIssueMs = nowMs;

        std::ostringstream diag;
        diag << "gapcloser_range_point_move"
             << " desired_range=" << desiredRange
             << " max_range=" << maxRange
             << " dist=" << player->GetDistance(target)
             << " strict=" << (RequiresStrictHumanPathing(player) ? "yes" : "no")
             << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
             << " moving_after=" << (player->isMoving() ? "yes" : "no");
        SetLastMovementDebugStatus(player, diag.str());
    }
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


bool IsMageBlinkEscapeCast(Player const* player, playerbot::PvpClassSpellContext const& context, uint32 resolvedSpellId)
{
    if (!player || player->GetClass() != CLASS_MAGE)
        return false;

    if (context.targetMode != playerbot::PvpClassSpellContext::TargetMode::Self)
        return false;

    if (context.spellId != 1953 && resolvedSpellId != 1953)
        return false;

    constexpr uint32 blinkableMechanicMask =
        (1u << MECHANIC_STUN) |
        (1u << MECHANIC_ROOT);

    constexpr uint32 nonBlinkableControlMask =
        (1u << MECHANIC_CHARM) |
        (1u << MECHANIC_DISORIENTED) |
        (1u << MECHANIC_FEAR) |
        (1u << MECHANIC_SLEEP) |
        (1u << MECHANIC_FREEZE) |
        (1u << MECHANIC_POLYMORPH) |
        (1u << MECHANIC_BANISH) |
        (1u << MECHANIC_HORROR) |
        (1u << MECHANIC_SAPPED);

    bool const blinkableControl =
        player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_ROOT) ||
        player->HasAuraType(SPELL_AURA_MOD_ROOT) ||
        player->HasAuraWithMechanic(blinkableMechanicMask);

    if (!blinkableControl)
        return false;

    bool const hardNonBlinkableControl =
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->HasAuraWithMechanic(nonBlinkableControlMask) ||
        player->IsPolymorphed();

    return !hardNonBlinkableControl;
}


bool PlayerHasPoisonForStoneform(Player const* player)
{
    if (!player)
        return false;

    for (Unit::AuraApplicationMap::value_type const& appliedAura : player->GetAppliedAuras())
    {
        AuraApplication const* aurApp = appliedAura.second;
        SpellInfo const* spellInfo = aurApp ? aurApp->GetBase()->GetSpellInfo() : nullptr;
        if (spellInfo && spellInfo->Dispel == DISPEL_POISON)
            return true;
    }

    return false;
}

bool IsControlBreakingRacialCast(Player const* player, playerbot::PvpClassSpellContext const& context, uint32 resolvedSpellId)
{
    if (!player || context.targetMode != playerbot::PvpClassSpellContext::TargetMode::Self)
        return false;

    switch (resolvedSpellId)
    {
        case 7744: // Will of the Forsaken
        {
            constexpr uint32 wotfMechanicMask =
                (1u << MECHANIC_FEAR) |
                (1u << MECHANIC_CHARM) |
                (1u << MECHANIC_SLEEP);
            return player->HasUnitState(UNIT_STATE_FLEEING) || player->HasAuraWithMechanic(wotfMechanicMask);
        }
        case 20594: // Stoneform
            return PlayerHasPoisonForStoneform(player);
        default:
            return false;
    }
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

    g_LastClassExecutionStatusByGuid[player->GetGUID().GetRawValue()] = status;
}

void SetLastMovementDebugStatus(Player const* player, std::string const& status)
{
    if (!player)
        return;

    g_LastMovementDebugStatusByGuid[player->GetGUID().GetRawValue()] = status;
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


bool IsSpellInFirstRankChain(SpellInfo const* spellInfo, uint32 firstRankSpellId)
{
    if (!spellInfo || !firstRankSpellId)
        return false;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && firstRank->Id == firstRankSpellId;
}

bool IsHunterBreakableCrowdControlSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    if (IsSpellInFirstRankChain(spellInfo, 19386)) // Wyvern Sting
        return true;

    if (IsSpellInFirstRankChain(spellInfo, 19503)) // Scatter Shot
        return true;

    // Freezing Trap effect/ranks. The trap itself is a self-cast, but stopping
    // ranged auto-repeat after the cast is still safe and prevents a queued shot
    // from instantly breaking the trap target when the trap arms/triggers.
    if (IsSpellInFirstRankChain(spellInfo, 1499) || IsSpellInFirstRankChain(spellInfo, 3355))
        return true;

    return false;
}

void StopHunterDamageOnBreakableCrowdControl(Player* player, Unit* target, char const* reason)
{
    if (!player || player->GetClass() != CLASS_HUNTER)
        return;

    if (Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        if (SpellInfo const* autoRepeatInfo = autoRepeat->GetSpellInfo())
            if (autoRepeatInfo->Id == 75)
                player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

    if (target && player->GetVictim() && player->GetVictim()->GetGUID() == target->GetGUID())
        player->AttackStop();

    if (Pet* pet = player->GetPet())
    {
        if (target && pet->GetVictim() && pet->GetVictim()->GetGUID() == target->GetGUID())
            pet->AttackStop();
    }

    SetLastMovementDebugStatus(player, reason ? reason : "hunter_stop_damage_for_breakable_cc");
}

void CommandPetAttackTarget(Player* player, Unit* target)
{
    if (!player || !target || !target->IsAlive())
        return;

    if (target->HasBreakableByDamageCrowdControlAura())
    {
        StopHunterDamageOnBreakableCrowdControl(player, target, "hunter_pet_attack_suppressed_breakable_cc");
        return;
    }

    Pet* pet = player->GetPet();
    if (!pet || !pet->IsAlive() || !pet->IsValidAttackTarget(target))
        return;

    if (CharmInfo* charmInfo = pet->GetCharmInfo())
    {
        // Mirror an owner pet-attack command before issuing combat movement so
        // passive/stay pets are allowed to leave their current command state and
        // pursue the selected target.
        charmInfo->SetIsCommandAttack(true);
        charmInfo->SetIsAtStay(false);
        charmInfo->SetIsCommandFollow(false);
        charmInfo->SetCommandState(COMMAND_ATTACK);
    }

    if (pet->GetVictim() != target)
    {
        pet->AttackStop();
        pet->Attack(target, true);
    }

    if (!pet->IsWithinMeleeRange(target) ||
        !pet->HasUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE))
    {
        if (MotionMaster* motionMaster = pet->GetMotionMaster())
            motionMaster->MoveChase(target, ChaseRange(0.0f, pet->GetPetChaseDistance()));
    }

    pet->SetUnitFlag(UNIT_FLAG_PET_IN_COMBAT);
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
        if (context.itemEntry)
            message += " | item=" + std::to_string(context.itemEntry);
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

    Map* map = bot->FindMap();
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

bool IsWandShootSpell(uint32 spellId)
{
    if (!spellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;
    return firstRankSpellId == 5019;
}

char const* GetSpellStateLabel(Spell const* spell)
{
    if (!spell)
        return "none";

    switch (spell->getState())
    {
        case SPELL_STATE_NULL: return "null";
        case SPELL_STATE_PREPARING: return "preparing";
        case SPELL_STATE_CASTING: return "casting";
        case SPELL_STATE_FINISHED: return "finished";
        case SPELL_STATE_DELAYED: return "delayed";
        default: return "unknown";
    }
}

uint32 GetCurrentSpellId(Player const* player, CurrentSpellTypes spellType)
{
    if (!player)
        return 0;

    Spell const* spell = player->GetCurrentSpell(spellType);
    return spell && spell->GetSpellInfo() ? spell->GetSpellInfo()->Id : 0;
}

void WhisperPlayerbotDiagnostic(Player* bot, std::string const& message)
{
    if (!bot || message.empty())
        return;

    if (bot->duel && bot->duel->Opponent)
        bot->Whisper(message, LANG_UNIVERSAL, bot->duel->Opponent);

    Map* map = bot->FindMap();
    if (!map)
        return;

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* observer = itr->GetSource();
        if (!observer || !observer->IsGameMaster())
            continue;

        if (bot->duel && bot->duel->Opponent && observer->GetGUID() == bot->duel->Opponent->GetGUID())
            continue;

        bot->Whisper(message, LANG_UNIVERSAL, observer);
    }
}

std::string BuildRehgarsFuryMovementDiagnostic(Player const* player, Unit const* target, char const* phase, char const* extra = nullptr)
{
    std::ostringstream os;
    os << "REHGAR DIAG: phase=" << (phase ? phase : "unknown");
    if (extra && *extra)
        os << " extra=" << extra;

    if (!player)
        return os.str();

    MotionMaster const* motionMaster = player->GetMotionMaster();
    bool const hasSpline = player->movespline != nullptr;
    os << " motion=" << uint32(motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE)
       << " spline=" << (hasSpline ? "yes" : "no")
       << " initialized=" << (hasSpline && player->movespline->Initialized() ? "yes" : "no")
       << " finalized=" << (hasSpline && player->movespline->Finalized() ? "yes" : "no")
       << " moving=" << (player->isMoving() ? "yes" : "no")
       << " charging=" << (player->HasUnitState(UNIT_STATE_CHARGING) ? "yes" : "no")
       << " jumping=" << (player->HasUnitState(UNIT_STATE_JUMPING) ? "yes" : "no")
       << " form=" << uint32(player->GetShapeshiftForm())
       << " fury_cd=" << (player->GetSpellHistory()->HasCooldown(82419) ? "yes" : "no")
       << " ghost_wolf_cd=" << (player->GetSpellHistory()->HasCooldown(2645) ? "yes" : "no");

    if (target)
        os << " target=" << target->GetGUID().ToString() << " dist=" << player->GetDistance(target);

    return os.str();
}

void WhisperRehgarsFuryMovementDiagnostic(Player* player, Unit* target, char const* phase, char const* extra = nullptr)
{
    if (!player || player->GetClass() != CLASS_SHAMAN)
        return;

    WhisperPlayerbotDiagnostic(player, BuildRehgarsFuryMovementDiagnostic(player, target, phase, extra));
}

void ScheduleRehgarsFuryMovementDiagnostics(Player* player, Unit* target)
{
    if (!player)
        return;

    ObjectGuid const playerGuid = player->GetGUID();
    ObjectGuid const targetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
    for (uint32 delayMs : { 1u, 50u, 250u, 750u, 1500u })
    {
        player->m_Events.AddEventAtOffset([playerGuid, targetGuid, delayMs]()
        {
            Player* shaman = ObjectAccessor::FindConnectedPlayer(playerGuid);
            if (!shaman || !shaman->IsInWorld())
                return;

            Unit* chargeTarget = targetGuid ? ObjectAccessor::GetUnit(*shaman, targetGuid) : nullptr;
            std::string const extra = "delay_ms=" + std::to_string(delayMs);
            WhisperRehgarsFuryMovementDiagnostic(shaman, chargeTarget, "post_cast_snapshot", extra.c_str());
        }, std::chrono::milliseconds(delayMs));
    }
}

void NotifyWandDiagnostic(Player*, Unit*, std::string const&, uint32, char const* = nullptr)
{
    // Intentionally silent. This hook was used for temporary wand troubleshooting
    // whispers and is left as a no-op so the wand stabilization code can keep
    // its call sites without producing player-visible diagnostics.
}

#if 0 // Temporary Aimed Shot whisper diagnostics disabled; retain for future troubleshooting.
std::string BuildHunterCastDiagnostic(Player* player, Unit* target, char const* phase, uint32 spellId, char const* extra = nullptr)
{
    std::ostringstream os;
    os << "AIMED DIAG: phase=" << (phase ? phase : "unknown")
       << " spell=" << spellId;

    if (extra && *extra)
        os << " extra=" << extra;

    if (!player)
        return os.str();

    Spell const* generic = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);

    SpellInfo const* genericInfo = generic ? generic->GetSpellInfo() : nullptr;
    SpellInfo const* channelInfo = channel ? channel->GetSpellInfo() : nullptr;
    SpellInfo const* autoInfo = autoRepeat ? autoRepeat->GetSpellInfo() : nullptr;

    MotionMaster* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const motionType = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;

    if (SpellInfo const* diagSpellInfo = sSpellMgr->GetSpellInfo(spellId))
    {
        os << " cast_ms=" << std::max<int32>(0, diagSpellInfo->CalcCastTime())
           << " stationary_ms=" << GetHunterStationaryCastTimeMs(diagSpellInfo)
           << " is_aimed=" << (IsHunterAimedShotSpell(diagSpellInfo) ? "yes" : "no");
    }

    os << " gen=" << (genericInfo ? genericInfo->Id : 0) << ':' << GetSpellStateLabel(generic)
       << " chan=" << (channelInfo ? channelInfo->Id : 0) << ':' << GetSpellStateLabel(channel)
       << " auto=" << (autoInfo ? autoInfo->Id : 0) << ':' << GetSpellStateLabel(autoRepeat)
       << " moving=" << (player->isMoving() ? "yes" : "no")
       << " move_state=" << (player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE) ? "yes" : "no")
       << " move_flags=" << uint32(player->GetUnitMovementFlags())
       << " motion=" << uint32(motionType)
       << " nonmelee=" << (player->IsNonMeleeSpellCast(false, false, true) ? "yes" : "no")
       << " ranged_timer=" << player->getAttackTimer(RANGED_ATTACK)
       << " lock=" << (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHunterStationaryCastLockToken) ? "yes" : "no")
       << " combat=" << (player->IsInCombat() ? "yes" : "no");

    if (target)
    {
        os << " target=" << target->GetGUID().ToString()
           << " dist=" << player->GetDistance(target)
           << " exact=" << player->GetExactDist(target)
           << " los=" << (player->IsWithinLOSInMap(target) ? "yes" : "no")
           << " front=" << (player->isInFront(target) ? "yes" : "no")
           << " target_victim_me=" << (target->GetVictim() == player ? "yes" : "no");
    }

    Unit* victim = player->GetVictim();
    if (victim)
        os << " victim=" << victim->GetGUID().ToString();

    return os.str();
}
#endif

void WhisperHunterCastDiagnostic(Player* player, Unit* target, char const* phase, uint32 spellId, char const* extra = nullptr)
{
    (void)player;
    (void)target;
    (void)phase;
    (void)spellId;
    (void)extra;
    // WhisperPlayerbotDiagnostic(player, BuildHunterCastDiagnostic(player, target, phase, spellId, extra));
}


std::string BuildHunterPetDiagnostic(Player* player, char const* phase, SpellCastResult castResult)
{
    std::ostringstream os;
    EnumText const resultText = EnumUtils::ToString(castResult);
    os << "HUNTER PET DIAG: phase=" << (phase ? phase : "unknown")
       << " result=" << resultText.Title;

    if (!player)
        return os.str();

    Pet const* activePet = player->GetPet();
    os << " active_pet=" << (activePet ? "yes" : "no");
    if (activePet)
    {
        os << " active_guid=" << activePet->GetGUID().ToString()
           << " active_entry=" << activePet->GetEntry()
           << " active_alive=" << (activePet->IsAlive() ? "yes" : "no")
           << " active_dead=" << (activePet->isDead() ? "yes" : "no")
           << " active_health=" << activePet->GetHealth()
           << "/" << activePet->GetMaxHealth()
           << " active_type=" << uint32(activePet->getPetType());
    }

    PetStable const* stable = player->GetPetStable();
    os << " stable=" << (stable ? "yes" : "no");
    if (!stable)
        return os.str();

    std::size_t stabledCount = 0;
    for (Optional<PetStable::PetInfo> const& stableSlot : stable->StabledPets)
        if (stableSlot)
            ++stabledCount;

    os << " current=" << (stable->CurrentPet ? "yes" : "no")
       << " unslotted=" << stable->UnslottedPets.size()
       << " stabled=" << stabledCount;

    if (stable->CurrentPet)
    {
        PetStable::PetInfo const& current = *stable->CurrentPet;
        os << " current_num=" << current.PetNumber
           << " current_entry=" << current.CreatureId
           << " current_type=" << uint32(current.Type)
           << " current_health=" << current.Health;
    }

    std::pair<PetStable::PetInfo const*, PetSaveMode> const loadInfo = Pet::GetLoadPetInfo(*stable, 0, 0, false);
    PetStable::PetInfo const* loadable = loadInfo.first;
    os << " loadable=" << (loadable ? "yes" : "no");
    if (loadable)
    {
        CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(loadable->CreatureId);
        bool const tameable = creatureInfo && creatureInfo->IsTameable(player->CanTameExoticPets());
        os << " load_slot=" << uint32(loadInfo.second)
           << " load_num=" << loadable->PetNumber
           << " load_entry=" << loadable->CreatureId
           << " load_type=" << uint32(loadable->Type)
           << " load_health=" << loadable->Health
           << " load_template=" << (creatureInfo ? "yes" : "no")
           << " load_tameable=" << (tameable ? "yes" : "no")
           << " exotic_ok=" << (player->CanTameExoticPets() ? "yes" : "no");
    }

    return os.str();
}

void ScheduleWandDiagnostics(Player*, Unit*, uint32)
{
    // Intentionally silent; delayed wand diagnostics were temporary.
}

bool IsMindFlaySpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && firstRank->Id == 15407;
}

bool IsPlayerbotStationaryChannel(SpellInfo const* spellInfo)
{
    if (!spellInfo || !spellInfo->IsChanneled())
        return false;

    // Mind Flay must be treated like Drain Life for playerbot movement even on
    // DBC/custom data where the generic channel flags look movable.
    return !spellInfo->IsMoveAllowedChannel() || IsMindFlaySpell(spellInfo);
}

bool IsPlayerbotMovableCastTimeSpell(Player const* player, SpellInfo const* spellInfo)
{
    return player && spellInfo && spellInfo->IsStarfire() && player->GetStarfireSnareSpeedRate() > 0.0f;
}

bool HasActiveStationaryChannel(Player const* player)
{
    if (!player)
        return false;

    Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (!channel || channel->getState() == SPELL_STATE_FINISHED)
        return false;

    return IsPlayerbotStationaryChannel(channel->GetSpellInfo());
}

bool HasActiveMovementEffectSpline(Player const* player)
{
    if (!player)
        return false;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    if (!motionMaster || motionMaster->GetCurrentMovementGeneratorType() != EFFECT_MOTION_TYPE)
        return false;

    // The current generator is already known to be EFFECT_MOTION_TYPE (a charge
    // / Heroic Leap / Rehgar's Fury jump). Treat it as active for its whole
    // lifetime, including the brief window after MotionMaster::Add() queues the
    // jump but before its spline is launched (movespline not yet Initialized).
    // The old check only accepted an already-launched spline / charging / moving
    // state, so during that pre-launch gap it returned false and the very next
    // bot movement tick could Clear() the highest-priority jump before it ever
    // moved the bot. For an enhancement shaman that stranded it in Ghost Wolf,
    // re-casting Rehgar's Fury forever because the jump's arrival MovementInform
    // (which drops Ghost Wolf and puts it on cooldown) never fired.
    bool const splineLaunched = player->movespline && player->movespline->Initialized();
    bool const splineActive = splineLaunched && !player->movespline->Finalized();
    bool const splinePendingLaunch = !splineLaunched;
    return splineActive || splinePendingLaunch ||
        player->HasUnitState(UNIT_STATE_CHARGING | UNIT_STATE_JUMPING) || player->isMoving();
}

bool IsInHazardousLiquid(Player const* player)
{
    if (!player)
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return false;

    if (player->HasAura(kEnvironmentalMagmaDamageAuraId))
        return true;

    LiquidData liquidData{};
    ZLiquidStatus const status = map->GetLiquidStatus(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight());
    if ((status & MAP_LIQUID_STATUS_IN_CONTACT) == 0)
        return false;

    return (liquidData.type_flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
}

bool IsHazardousLiquidDestination(Player const* player, Position const& destination)
{
    if (!player)
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return false;

    LiquidData liquidData{};
    ZLiquidStatus const status = map->GetLiquidStatus(player->GetPhaseMask(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight());
    return (status & MAP_LIQUID_STATUS_IN_CONTACT) != 0 &&
        (liquidData.type_flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
}

bool TryMoveOutOfHazardousLiquid(Player* player)
{
    if (!IsInHazardousLiquid(player))
        return false;

    // Preserve an already-launched escape point. Recomputing from the bot's
    // new position and clearing/reissuing every AI tick repeatedly resets the
    // spline at its origin, which looks exactly like standing still in magma.
    if (MotionMaster const* motionMaster = player->GetMotionMaster())
        if (motionMaster->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            bool const splineLaunched = player->movespline && player->movespline->Initialized();
            if (!splineLaunched || !player->movespline->Finalized())
                return true;
        }

    float const baseAngle = player->GetOrientation();
    std::array<float, 12> const probeAngles =
    {
        0.0f, float(M_PI), float(M_PI_2), -float(M_PI_2),
        float(M_PI_4), -float(M_PI_4), float(3.0f * M_PI_4), -float(3.0f * M_PI_4),
        float(M_PI / 6.0f), -float(M_PI / 6.0f), float(5.0f * M_PI / 6.0f), -float(5.0f * M_PI / 6.0f)
    };
    std::array<float, 4> const probeDistances = { 8.0f, 12.0f, 16.0f, 24.0f };

    for (float distance : probeDistances)
    {
        for (float offset : probeAngles)
        {
            Position destination = player->GetPosition();
            float const angle = baseAngle + offset;
            destination.RelocateOffset({ std::cos(angle) * distance, std::sin(angle) * distance, 0.0f, 0.0f });
            destination = BuildCollisionSafeDestination(player, destination);
            if (IsHazardousLiquidDestination(player, destination))
                continue;

            if (IssueStrictHumanMove(player, destination, 6.0f, 1500))
            {
                SetLastMovementDebugStatus(player, "hazardous_liquid_stop_prevented_move_out");
                return true;
            }
        }
    }

    return false;
}

void StopPlayerbotForStationaryCast(Player* player)
{
    if (!player)
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();

    // Unit::StopMoving() halts the movespline unconditionally, and Clear()
    // below bypasses MotionMaster priority protection entirely -- neither
    // checks what generator is actually driving movement. A charge/intercept
    // spline runs through the effect motion slot at MOTION_PRIORITY_HIGHEST
    // for ~1-1.5s; if the bot's next decision tick wants a stationary cast
    // (target is already in range right after the charge lands) this used to
    // rip the charge spline out mid-flight. Let the charge resolve first --
    // the caller's SPELL_FAILED_MOVING retry path already handles waiting a
    // tick for a stationary cast, so this is a short defer, not a stall.
    if (HasActiveMovementEffectSpline(player))
        return;

    // Magma/slime is acceptable as a transit route when a path requires
    // swimming through it, but it must never become a place where a bot plants
    // for a stationary cast or ranged hold. If a stop request arrives while in
    // hazardous liquid, convert that stop into a generic "get out" move.
    if (TryMoveOutOfHazardousLiquid(player))
        return;

    player->StopMoving();
    if (motionMaster)
        motionMaster->Clear(MOTION_SLOT_ACTIVE);

    // Unit::StopMoving() only strips the spline forward flag when an active
    // server spline exists. Playerbots can also carry stale client-style
    // movement flags (strafe/back/fall/spline elevation) after chase/follow
    // movement was cleared, and Spell::CheckCast rejects stationary channeled
    // spells such as Drain Life while any MOVEMENTFLAG_MASK_MOVING bit remains.
    // Clear the full moving mask for bot-controlled stationary casts so the
    // immediate cast attempt observes the same stopped state that a real client
    // would send before casting.
    player->ClearUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE);
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
    player->SendMovementFlagUpdate();
}


void DelayHunterRangedTimerForStationaryShot(Player* player, SpellInfo const* spellInfo, char const* reason)
{
    if (!player || player->GetClass() != CLASS_HUNTER || !spellInfo)
        return;

    if (GetHunterStationaryCastTimeMs(spellInfo) == 0)
        return;

    uint32 const timerMs = player->getAttackTimer(RANGED_ATTACK);
    if (timerMs >= 500)
        return;

    // Mirror the core Auto Shot protection in Unit::_UpdateAutoRepeatSpell():
    // when a hunter has a generic/channel spell preparing and the ranged timer
    // gets inside the Auto Shot launch window, keep the ranged timer slightly
    // delayed.  The playerbot cast guard intentionally suppresses CURRENT_AUTOREPEAT_SPELL
    // during Aimed Shot/Multi-Shot, so the core's auto-repeat update does not
    // get a chance to perform this delay itself.  Without this, RANGED_ATTACK
    // can hit 0 in the middle of Aimed Shot and the shot can be clipped/aborted
    // even though the bot did not move.
    player->setAttackTimer(RANGED_ATTACK, 434);

    if (reason && IsHunterAimedShotSpell(spellInfo))
        WhisperHunterCastDiagnostic(player, nullptr, "ranged_timer_delayed", spellInfo->Id, reason);
}

void ScheduleHunterStationaryCastGuard(Player* player, Unit* target, uint32 spellId, uint32 castTimeMs)
{
    if (!player || player->GetClass() != CLASS_HUNTER || !spellId)
        return;

    ObjectGuid const hunterGuid = player->GetGUID();
    ObjectGuid const targetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
    uint32 const guardMs = std::clamp<uint32>(castTimeMs + 300, 700, 5000);

    // Use early, dense snapshots first. The previous diagnostics started too
    // late for the failure we are chasing: CastSpell(20904) returns OK, but
    // CURRENT_GENERIC_SPELL is already gone by the time the normal 100ms guard
    // notices. These probes tell us whether the spell ever entered the generic
    // slot, or whether the core accepts then immediately drops/finishes it.
    std::vector<uint32> probeDelaysMs = { 1, 25, 50, 100, 150, 250, 500 };
    for (uint32 delayMs = 750; delayMs <= guardMs; delayMs += 250)
        probeDelaysMs.push_back(delayMs);

    for (uint32 delayMs : probeDelaysMs)
    {
        player->m_Events.AddEventAtOffset([hunterGuid, targetGuid, spellId, delayMs, castTimeMs]()
        {
            Player* hunter = ObjectAccessor::FindConnectedPlayer(hunterGuid);
            if (!hunter || !hunter->IsInWorld() || !hunter->IsAlive() || hunter->GetClass() != CLASS_HUNTER)
                return;

            Unit* castTarget = !targetGuid.IsEmpty() ? ObjectAccessor::GetUnit(*hunter, targetGuid) : nullptr;
            std::string delayExtra = "delay=" + std::to_string(delayMs);

            Spell const* current = hunter->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!current)
            {
                // If the generic spell disappeared before the stationary shot
                // could plausibly finish, the core dropped/canceled it and the
                // fake lock should be released.  But if it disappears at or
                // after the expected cast time, that is the normal completion /
                // delayed-impact transition.  Keep the original post-cast lock
                // alive so racials/utility (notably Shadowmeld) cannot fire on
                // the same tick and break the shot/combat state.
                if (delayMs + 50 >= castTimeMs)
                {
                    WhisperHunterCastDiagnostic(hunter, castTarget, "guard_missing_generic_after_casttime", spellId, delayExtra.c_str());
                    return;
                }

                WhisperHunterCastDiagnostic(hunter, castTarget, delayMs <= 250 ? "guard_missing_generic_early" : "guard_missing_generic_before_casttime", spellId, delayExtra.c_str());

                if (delayMs >= 250)
                    playerbot::PvpClassActions::RegisterCasterSpellCooldown(hunter, kPlayerbotHunterStationaryCastLockToken, std::chrono::milliseconds(1));
                return;
            }

            SpellInfo const* currentInfo = current->GetSpellInfo();
            if (current->getState() == SPELL_STATE_FINISHED)
            {
                std::string finishedExtra = delayExtra + " current=" + std::to_string(currentInfo ? currentInfo->Id : 0);
                if (delayMs + 50 >= castTimeMs)
                {
                    WhisperHunterCastDiagnostic(hunter, castTarget, "guard_generic_finished_after_casttime", spellId, finishedExtra.c_str());
                    return;
                }

                WhisperHunterCastDiagnostic(hunter, castTarget, "guard_generic_finished_before_casttime", spellId, finishedExtra.c_str());
                if (delayMs >= 250)
                    playerbot::PvpClassActions::RegisterCasterSpellCooldown(hunter, kPlayerbotHunterStationaryCastLockToken, std::chrono::milliseconds(1));
                return;
            }

            if (!currentInfo || currentInfo->Id != spellId)
            {
                std::string mismatchExtra = delayExtra + " current=" + std::to_string(currentInfo ? currentInfo->Id : 0);
                if (delayMs + 50 >= castTimeMs)
                {
                    WhisperHunterCastDiagnostic(hunter, castTarget, "guard_generic_mismatch_after_casttime", spellId, mismatchExtra.c_str());
                    return;
                }

                WhisperHunterCastDiagnostic(hunter, castTarget, "guard_generic_mismatch_before_casttime", spellId, mismatchExtra.c_str());
                if (delayMs >= 250)
                    playerbot::PvpClassActions::RegisterCasterSpellCooldown(hunter, kPlayerbotHunterStationaryCastLockToken, std::chrono::milliseconds(1));
                return;
            }

            DelayHunterRangedTimerForStationaryShot(hunter, currentInfo, delayExtra.c_str());
            WhisperHunterCastDiagnostic(hunter, castTarget, "guard_active", spellId, delayExtra.c_str());

            // Once the stationary shot is actively preparing, do not keep
            // re-issuing StopMoving(), clearing MotionMaster, or turning the
            // hunter every guard tick. Those operations are safe before the
            // cast starts, but on virtual playerbots they can generate server
            // movement/facing updates during the cast bar and clip Aimed Shot.
            // The guard should only HOLD other systems out and delay the ranged
            // timer. If something actually made the hunter move, log it and
            // apply one emergency stop.
            StopHunterAutoShotForStationaryCast(hunter, "hunter_stationary_cast_guard_stop_autoshot");

            bool const movedDuringCast = hunter->isMoving() ||
                hunter->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE) ||
                (hunter->GetUnitMovementFlags() & MOVEMENTFLAG_MASK_MOVING);
            if (movedDuringCast)
            {
                WhisperHunterCastDiagnostic(hunter, castTarget, "guard_detected_movement_during_cast", spellId, delayExtra.c_str());
                StopPlayerbotForStationaryCast(hunter);
                DelayHunterRangedTimerForStationaryShot(hunter, currentInfo, "post_emergency_stop");
            }
        }, std::chrono::milliseconds(delayMs));
    }
}

bool CanIssueFollowCommands(Player const* player)
{
    if (!player || !player->IsAlive())
        return false;

    if (HasActiveStationaryChannel(player))
        return false;

    if (IsCrowdControlledForAction(player) ||
        player->HasUnitState(UNIT_STATE_ROOT) ||
        player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING))
    {
        return false;
    }

    // Charge/leap/jump movement is issued through the effect motion slot at
    // MOTION_PRIORITY_HIGHEST, but that engine-side priority protection only
    // stops other MotionMaster::Add() calls -- it does nothing against a bare
    // Clear(), which follow/strict-move callers issue unconditionally. Without
    // this guard the very next movement tick can wipe the in-flight effect
    // spline, so leap-style abilities visibly apply spell effects without
    // moving the bot.
    if (HasActiveMovementEffectSpline(player))
        return false;

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

    // Do not re-issue a facing packet for auto-repeat ranged spells when the
    // target is already in front. Diagnostics showed SetFacingToObject can
    // briefly restore stale forward/spline movement flags on virtual sessions
    // after StopPlayerbotForStationaryCast cleared them, and that 100ms
    // movement blip cancels wand Shoot before the first repeat tick lands.
    if (spellInfo->IsAutoRepeatRangedSpell() && player->isInFront(target))
        return;

    player->SetFacingToObject(target);
    player->SetInFront(target);
}

void RepositionDruidAfterTravelFormRecovery(Player* player)
{
    if (!player || !player->FindMap() || !CanIssueFollowCommands(player))
        return;

    Unit* nearestEnemy = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
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


struct DruidShapeshiftMovementResumeState
{
    ObjectGuid targetGuid = ObjectGuid::Empty;
    float desiredRange = 1.5f;
    uint8 mode = 1; // 1=chase, 2=follow
    MovementGeneratorType motionType = IDLE_MOTION_TYPE;
    bool shouldResume = false;
};

DruidShapeshiftMovementResumeState CaptureDruidShapeshiftMovementResume(Player* player,
    playerbot::PvpClassSpellContext const& context, SpellInfo const* spellInfo)
{
    DruidShapeshiftMovementResumeState state;
    if (!player || !spellInfo || player->GetClass() != CLASS_DRUID ||
        context.targetMode != playerbot::PvpClassSpellContext::TargetMode::Self ||
        !spellInfo->HasAura(SPELL_AURA_MOD_SHAPESHIFT))
        return state;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    state.motionType = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    bool const targetRelativeMotion = state.motionType == CHASE_MOTION_TYPE || state.motionType == FOLLOW_MOTION_TYPE;
    bool const hadMovementSignal = player->isMoving() ||
        player->HasUnitState(UNIT_STATE_CHASE_MOVE) ||
        player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ||
        (player->movespline && player->movespline->Initialized() && !player->movespline->Finalized());

    uint32 const nowMs = GameTime::GetGameTimeMS();
    auto orderItr = g_TargetRelativeMoveOrderByGuid.find(player->GetGUID().GetRawValue());
    if (orderItr != g_TargetRelativeMoveOrderByGuid.end())
    {
        TargetRelativeMoveOrderState const& order = orderItr->second;
        uint32 const orderAgeMs = order.lastIssueMs != 0 && nowMs >= order.lastIssueMs ? nowMs - order.lastIssueMs : 0;
        if (!order.targetGuid.IsEmpty() && orderAgeMs <= 6000)
        {
            state.targetGuid = order.targetGuid;
            state.desiredRange = order.issuedRange > 0.0f ? order.issuedRange : 1.5f;
            state.mode = order.mode ? order.mode : 1;
            state.shouldResume = targetRelativeMotion || hadMovementSignal || orderAgeMs <= 2500;
            return state;
        }
    }

    Unit* target = !context.targetGuid.IsEmpty() ? ObjectAccessor::GetUnit(*player, context.targetGuid) : nullptr;
    if (!target || !target->IsAlive())
        target = player->GetVictim();
    if (!target || !target->IsAlive())
        target = player->GetSelectedUnit();

    if (!target || !target->IsAlive() || target == player)
        return state;

    state.targetGuid = target->GetGUID();
    state.desiredRange = player->IsValidAttackTarget(target) ? 0.5f : 1.5f;
    state.mode = player->IsValidAttackTarget(target) ? 1 : 2;
    state.shouldResume = targetRelativeMotion;
    return state;
}

void ResumeDruidShapeshiftMovement(Player* player, DruidShapeshiftMovementResumeState const& state, uint32 spellId)
{
    if (!player || !state.shouldResume || state.targetGuid.IsEmpty() || !CanIssueFollowCommands(player))
        return;

    Unit* target = ObjectAccessor::GetUnit(*player, state.targetGuid);
    if (!target || !target->IsAlive() || target == player || player->GetMapId() != target->GetMapId())
        return;

    MotionMaster* motionMaster = player->GetMotionMaster();
    if (!motionMaster)
        return;

    bool const hostileTarget = player->IsValidAttackTarget(target);
    if (hostileTarget)
    {
        player->SetSelection(target->GetGUID());
        if (player->GetVictim() != target || !player->IsInCombat())
            player->Attack(target, false);
    }

    motionMaster->Clear(MOTION_SLOT_ACTIVE);
    bool const preparedMotionMaster = PrepareMotionMasterForExplicitBotMovement(player);
    if (hostileTarget && state.mode != 2)
    {
        motionMaster->MoveChase(target);
        RecordTargetRelativeMovementOrder(player, target, 0.5f, 1);
    }
    else
    {
        float const followRange = std::max(0.5f, state.desiredRange);
        motionMaster->MoveFollow(target, followRange, player->GetFollowAngle());
        RecordTargetRelativeMovementOrder(player, target, followRange, 2);
    }

    MotionPrimeResult primeResult = PrimeTargetRelativeMotion(player);
    MarkTargetRelativeMovementLaunch(player);
    primeResult.addToWorldCalled = preparedMotionMaster;

    std::ostringstream diag;
    diag << "druid_shapeshift_movement_resumed"
         << " spell=" << spellId
         << " target=" << target->GetGUID().ToString()
         << " hostile=" << (hostileTarget ? "yes" : "no")
         << " previous_motion=" << uint32(state.motionType)
         << " mode=" << uint32(state.mode)
         << " desired_range=" << state.desiredRange
         << " motion_after=" << uint32(motionMaster->GetCurrentMovementGeneratorType())
         << " moving_after=" << (player->isMoving() ? "yes" : "no")
         << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
         << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no");
    AppendMotionPrimeDiag(diag, primeResult);
    SetLastMovementDebugStatus(player, diag.str());
}


bool ShouldDeferStationaryCastForActiveMovement(Player* player, Unit* castTarget, SpellInfo const* spellInfo,
    playerbot::PvpClassSpellContext const& context, bool isFoodOrDrinkSpell, std::string* diagOut)
{
    if (!player || !castTarget || !spellInfo || isFoodOrDrinkSpell)
        return false;

    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
        return false;

    // If this spell is genuinely ready from the current position, stopping is
    // correct. The bug we are chasing is a cast-time/autorepeat spell stopping
    // an active range/LOS movement order before the bot can actually reach the
    // target it is trying to move toward.
    bool const genericLos = player->IsWithinLOSInMap(castTarget);
    float const maxRange = spellInfo->GetMaxRange(false);
    float const minRange = spellInfo->GetMinRange(false);
    bool const maxOk = maxRange <= 0.0f || player->IsWithinDistInMap(castTarget, maxRange);
    bool const minOk = minRange <= 0.0f || !player->IsWithinDistInMap(castTarget, minRange);
    bool const genericReady = genericLos && maxOk && minOk;
    if (genericReady)
        return false;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    MovementGeneratorType const motionType = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    bool const activeTargetRelativeMotion = motionType == CHASE_MOTION_TYPE || motionType == FOLLOW_MOTION_TYPE;

    auto orderItr = g_TargetRelativeMoveOrderByGuid.find(player->GetGUID().GetRawValue());
    TargetRelativeMoveOrderState const* order = orderItr != g_TargetRelativeMoveOrderByGuid.end() ? &orderItr->second : nullptr;

    Unit* moveTarget = nullptr;
    if (order && !order->targetGuid.IsEmpty())
        moveTarget = ObjectAccessor::GetUnit(*player, order->targetGuid);

    float moveTargetDistance = moveTarget ? player->GetDistance(moveTarget) : 0.0f;
    bool const activeMoveOrder = order && !order->targetGuid.IsEmpty();
    bool const moveOrderStillOutside = activeMoveOrder && moveTarget && order->issuedRange > 0.0f && moveTargetDistance > order->issuedRange + 0.75f;
    bool const hasMoveSignal = player->isMoving() ||
        player->HasUnitState(UNIT_STATE_CHASE_MOVE) ||
        player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ||
        (player->movespline && player->movespline->Initialized() && !player->movespline->Finalized());

    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const orderAgeMs = order && order->lastIssueMs != 0 && nowMs >= order->lastIssueMs ? nowMs - order->lastIssueMs : 0;
    bool const moveOrderMatchesCastTarget = moveTarget && moveTarget == castTarget;

    std::string preserveDiag;
    bool const preservingTargetRelativeMotion = moveOrderMatchesCastTarget &&
        ShouldPreserveTargetRelativeMovement(player, castTarget, order ? order->issuedRange : 0.0f, 1800,
            "stationary_cast_defer_motion_preserved", &preserveDiag);

    // Defer only while the active CHASE/FOLLOW order is still making progress
    // (or inside a short launch window). This avoids repeated defer loops when
    // a movement generator exists but never launched (moving=no/spline=no).
    bool const shouldDefer = preservingTargetRelativeMotion || (moveOrderStillOutside && orderAgeMs < 6000);
    if (!shouldDefer)
        return false;

    if (CanIssueFollowCommands(player))
    {
        if (!genericLos)
        {
            float const desiredRange = context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally
                ? std::max(1.5f, std::min(8.0f, maxRange > 0.0f ? (maxRange - 1.0f) : 8.0f))
                : ComputeLosRecoveryRange(player, castTarget, maxRange);
            IssueRangedApproachMovement(player, castTarget, desiredRange, true, "stationary_cast_deferred_no_los");
        }
        else if (!maxOk)
        {
            float const desiredRange = maxRange > 0.0f ? std::max(1.0f, maxRange - 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().spellRange - 1.0f);
            IssueRangedApproachMovement(player, castTarget, desiredRange, false, "stationary_cast_deferred_out_of_range");
        }
        else if (!minOk)
        {
            float const desiredRange = minRange > 0.0f ? std::max(1.0f, minRange + 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().closeRange);
            player->GetMotionMaster()->MoveFollow(castTarget, desiredRange, player->GetFollowAngle());
        }
    }

    if (diagOut)
    {
        std::ostringstream diag;
        diag << "stationary_cast_stop_suppressed"
             << " spell=" << spellInfo->Id
             << " action=" << (context.actionName ? context.actionName : "none")
             << " reason=" << (context.reason ? context.reason : "none")
             << " cast_target=" << castTarget->GetGUID().ToString()
             << " cast_dist=" << player->GetDistance(castTarget)
             << " cast_los=" << (genericLos ? "yes" : "no")
             << " cast_max=" << maxRange
             << " cast_max_ok=" << (maxOk ? "yes" : "no")
             << " cast_min=" << minRange
             << " cast_min_ok=" << (minOk ? "yes" : "no")
             << " motion=" << uint32(motionType)
             << " moving=" << (player->isMoving() ? "yes" : "no")
             << " chase_move=" << (player->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
             << " follow_move=" << (player->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
             << " move_signal=" << (hasMoveSignal ? "yes" : "no")
             << " order_present=" << (activeMoveOrder ? "yes" : "no")
             << " order_matches_cast_target=" << (moveOrderMatchesCastTarget ? "yes" : "no")
             << " order_age_ms=" << orderAgeMs
             << " order_range=" << (order ? order->issuedRange : 0.0f)
             << " order_target=" << (moveTarget ? moveTarget->GetGUID().ToString() : "none")
             << " order_target_dist=" << moveTargetDistance
             << " order_outside=" << (moveOrderStillOutside ? "yes" : "no")
             << " motion_preserve=" << (preservingTargetRelativeMotion ? "yes" : "no")
             << " preserve_diag=" << (preserveDiag.empty() ? "none" : preserveDiag);
        *diagOut = diag.str();
    }

    return true;
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

    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
        target->HasBreakableByDamageCrowdControlAura() &&
        SpellAppliesBreakableByDamageCrowdControl(spellInfo))
    {
        failureReason = "target_already_breakable_crowd_controlled";
        return false;
    }

    if (castFromPet)
    {
        if (!petCaster)
        {
            failureReason = "pet_missing";
            return false;
        }

        if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
            target = petCaster;

        if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
        {
            if (!petCaster->IsValidAttackTarget(target, spellInfo))
            {
                failureReason = "invalid_enemy_target";
                return false;
            }

            // Pet utility spells are off the owner's global cooldown, but the
            // pet still needs an explicit attack/chase command when the target
            // is not already in its face. Issue that command before range/LOS
            // validation so an out-of-range pet begins closing immediately
            // instead of spending repeated decision ticks on failed casts.
            if (target && target->HasBreakableByDamageCrowdControlAura())
            {
                StopHunterDamageOnBreakableCrowdControl(player, target, "hunter_pet_spell_suppressed_breakable_cc");
                failureReason = "target_breakable_crowd_control";
                return false;
            }
            CommandPetAttackTarget(player, target);

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

    if (IsCrowdControlledForAction(player) &&
        !IsMageBlinkEscapeCast(player, context, resolvedSpellId) &&
        !IsControlBreakingRacialCast(player, context, resolvedSpellId))
    {
        // Hard crowd-control gate: polymorphed/confused actors must not start
        // attacks or cast attempts until control is restored. Mage Blink and
        // specific control-breaking racials are explicit exceptions because
        // they are intentionally usable while the corresponding control is active.
        failureReason = "crowd_controlled_polymorph";
        return false;
    }

    // Rehgar's Fury is the only playerbot PvP action that should be attempted
    // while a shaman is in Ghost Wolf. Other casts fail shapeshift validation
    // (for example Lightning Shield reports SPELL_FAILED_NOT_SHAPESHIFT), so
    // cancel the form immediately and suppress this cast attempt instead of
    // spamming spell-fail logs until the next decision tick.
    if (player->GetClass() == CLASS_SHAMAN && HasAuraInSpellChain(player, 2645) && resolvedSpellId != 82419)
    {
        player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        failureReason = "shaman_ghost_wolf_cancelled_for_non_rehgar_action";
        return false;
    }

    // Druids can intentionally swap into Bear Form under melee pressure, but
    // many follow-up heals/utility spells are not castable in Bear/Cat forms.
    // The random bot cadence evaluates roughly every 2 seconds, so waiting for
    // the next tick to leave form makes bots appear locked out. If the selected
    // spell is blocked only by current shapeshift state, immediately cancel the
    // form and continue with the same cast attempt in this tick. This is also
    // the mechanism that drops a shaman out of Ghost Wolf the moment nothing
    // castable requires it (Ghost Wolf's only real use is charging with
    // Rehgar's Fury). 82419 itself is exempted for the same reason as the
    // matching check in IsDecisionImmediatelyCastable - it is specifically
    // meant to be cast while shapeshifted into Ghost Wolf.
    if (resolvedSpellId != 82419 && (player->GetClass() == CLASS_DRUID || player->GetClass() == CLASS_SHAMAN) && player->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
        if (spellInfo->CheckShapeshift(player->GetShapeshiftForm()) == SPELL_FAILED_NOT_SHAPESHIFT)
            player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

    if (player->GetSpellHistory()->HasCooldown(resolvedSpellId) ||
        player->GetSpellHistory()->HasGlobalCooldown(spellInfo) ||
        player->IsNonMeleeSpellCast(false, false, true))
    {
        failureReason = "cooldown_or_casting";
        return false;
    }

    DruidShapeshiftMovementResumeState const shapeshiftMovementResume = CaptureDruidShapeshiftMovementResume(player, context, spellInfo);

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
            // AttackStop can clear victim linkage that MoveChase relies on.
            // Only stop attacks when already in melee contact where an actual
            // swing could break stealth; keep victim linkage while closing.
            if (player->GetVictim() && target && player->IsWithinMeleeRange(target))
                player->AttackStop();

            if (CanIssueFollowCommands(player))
                IssueStealthOpenerMovement(player, target);
        }
        else if (target && target->HasBreakableByDamageCrowdControlAura())
            StopHunterDamageOnBreakableCrowdControl(player, target, "hunter_owner_attack_suppressed_breakable_cc");
        else if (player->GetVictim() != target)
            player->Attack(target, false);

        if (!target || !target->HasBreakableByDamageCrowdControlAura())
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
    bool const shouldMoveBehindForEnemySpell =
        shouldUseMeleeApproachForEnemySpell &&
        spellInfo->HasAttribute(SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET);
    // TEMPORARY diagnostic: the warrior/shaman gap closers (Charge, Intercept,
    // Heroic Leap, Rehgar's Fury) were reported as producing zero whisper output
    // during live kiting tests. NotifyDuelDecision only fires after an actual
    // CastSpell attempt below, but these no_los/out_of_range branches return
    // early and substitute pure movement without ever reaching that point -
    // so a genuinely out-of-range gap closer is indistinguishable from a
    // misselected one from the whisper log alone. This makes that branch
    // visible so the next test can tell them apart. Remove once confirmed.
    bool const isGapCloserDiagnosticSpell = IsGapCloserSpell(resolvedSpellId);

    if (!itemTarget && !player->IsWithinLOSInMap(target))
    {
        if (isGapCloserDiagnosticSpell)
        {
            std::ostringstream diag;
            diag << "GAPCLOSE DIAG: spell=" << resolvedSpellId << " phase=no_los dist=" << player->GetDistance(target);
            WhisperPlayerbotDiagnostic(player, diag.str());
        }

        if (CanIssueFollowCommands(player))
        {
            if (shouldMoveBehindForEnemySpell)
                IssueBehindTargetMeleeMovement(player, target);
            else if (shouldUseMeleeApproachForEnemySpell)
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
        if (isGapCloserDiagnosticSpell)
        {
            std::ostringstream diag;
            diag << "GAPCLOSE DIAG: spell=" << resolvedSpellId << " phase=out_of_range dist=" << player->GetDistance(target) << " maxRange=" << maxRange;
            WhisperPlayerbotDiagnostic(player, diag.str());
        }

        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

        // When we are trying to cast but are still out of range, proactively
        // close the gap instead of idling and repeating failed cast attempts.
        // Hunter exception: if ranged weapon Auto Shot is already valid, do not
        // chase inward just to make Arcane/Concussive/Serpent range metadata happy.
        if (player->GetClass() == CLASS_HUNTER && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
            IsHunterAutoShotBand(player, target))
        {
            StopHunterAndStartAutoShot(player, target, "hunter_cast_out_of_spell_range_hold_autoshot");
        }
        else if (CanIssueFollowCommands(player) && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
        {
            if (isGapCloserDiagnosticSpell)
                IssueGapCloserRangeApproachMovement(player, target, maxRange);
            else if (shouldMoveBehindForEnemySpell)
                IssueBehindTargetMeleeMovement(player, target);
            else if (shouldUseMeleeApproachForEnemySpell)
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
        if (isGapCloserDiagnosticSpell)
        {
            std::ostringstream diag;
            diag << "GAPCLOSE DIAG: spell=" << resolvedSpellId << " phase=too_close dist=" << player->GetDistance(target) << " minRange=" << minRange;
            WhisperPlayerbotDiagnostic(player, diag.str());
        }

        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

        // Mirror reference-style spacing control for ranged casts: when too close,
        // immediately re-establish spell distance instead of repeatedly failing.
        if (player->GetClass() == CLASS_HUNTER && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
        {
            IssueHunterDeadZoneRetreatMovement(player, target, "hunter_cast_too_close_retreat_no_follow");
        }
        else if (CanIssueFollowCommands(player) && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
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

    if (!itemTarget && shouldMoveBehindForEnemySpell && IsBehindTargetRequiredAndMissing(player, target, spellInfo))
    {
        if (CanIssueFollowCommands(player))
            IssueBehindTargetMeleeMovement(player, target);

        failureReason = "not_behind";
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
    if (isMountSpell && spellInfo->HasAttribute(SPELL_ATTR0_OUTDOORS_ONLY) && !player->IsOutdoors())
    {
        // Match Spell::CheckCast's outdoors-only rule without adding extra
        // terrain-status restrictions that can falsely block custom/playerbot
        // mounts in battleground prep rooms where mounting is otherwise legal.
        failureReason = "indoors_cannot_mount";
        return false;
    }

    // Shadowmeld should behave like a tiny tactical drop-combat window: after
    // it lands, do not immediately move or cast and break it. The successful
    // Shadowmeld cast registers a 500ms grace token; every later action waits
    // until that token expires.
    if (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotShadowmeldGraceToken) &&
        context.spellId != kRacialNightElfShadowmeldSpellId)
    {
        failureReason = "shadowmeld_grace";
        return false;
    }

    // Feign Death is a short tactical drop-combat attempt for hunter bots. Once
    // the next real action is selected, stand up first; otherwise a clipped FD
    // can leave the virtual client lying down forever and unable to resume the
    // normal flee/stutter/instant-shot loop.
    if (player->GetClass() == CLASS_HUNTER && context.spellId != kHunterFeignDeathSpellId)
        BreakHunterFeignDeath(player);

    // Classic hunter traps are out-of-combat only. Feign Death may make the
    // hunter eligible on a later tick, but never send an in-combat trap cast
    // into the core where it becomes SPELL_FAILED_AFFECTING_COMBAT spam.
    if (player->GetClass() == CLASS_HUNTER && IsHunterTrapSpell(spellInfo) && player->IsInCombat())
    {
        failureReason = "hunter_trap_requires_out_of_combat";
        return false;
    }

    // Only force dismount when the bot is actually transitioning into combat
    // pressure. Allow benign out-of-combat utility/self-maintenance actions to
    // execute without unnecessarily dropping travel speed in battlegrounds.
    bool const shouldForceCombatDismount = context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy ||
        player->IsInCombat();
    if (player->IsMounted() && !isMountSpell && shouldForceCombatDismount)
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
    bool const bypassPowerPrecheck = spellInfo->IsAutoRepeatRangedSpell() || IsLifeTapSpell(spellInfo) ||
        IsSpiritOfRedemptionFreeHeal(player, spellInfo);
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

    // Auto-repeat spells (wand Shoot / Auto Shot) should not be re-cast every
    // AI tick once the server has an active auto-repeat spell. Reissuing Shoot
    // repeatedly can restart the opener before the wand swing timer ever fires.
    if (spellInfo->IsAutoRepeatRangedSpell())
    {
        if (Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        {
            if (SpellInfo const* activeAutoRepeatInfo = autoRepeat->GetSpellInfo())
                if (activeAutoRepeatInfo->Id == resolvedSpellId)
                {
                    NotifyWandDiagnostic(player, target, "already_active_skip_recast", resolvedSpellId);
                    return true;
                }
        }
    }

    // Cast-time spells like Frostbolt fail while moving. Since playerbots do
    // not have client-side stop-cast behavior, explicitly stop movement before
    // attempting non-instant casts.
    //
    // Important: do not stop an active range/LOS movement order for a stationary
    // spell that is not ready yet. The stopper diagnostics showed exactly this
    // failure mode: PB move diag had a live CHASE/FOLLOW order, then a
    // stop_moving_request reason=cast_time_or_autorepeat killed the spline and
    // left the bot inching or stuck.
    // spellInfo->CalcCastTime() with no Spell* skips Unit::ModSpellCastTime
    // entirely and only ever returns the raw DBC base cast time. Casters with
    // a talent/aura that reduces a spell's effective cast time (including a
    // flat -100% "instant cast" aura, which resolves through
    // Unit::CanInstantCast() rather than changing the spell's own listed cast
    // time) still got treated as needing a stationary cast here, forcing an
    // unnecessary stop-to-cast for something that would have completed
    // instantly while moving. Fold the caster's actual cast-speed mods in
    // before deciding - ModSpellCastTime accepts a null Spell* for this kind
    // of preview/dry-run calculation.
    int32 effectiveCastTimeMs = static_cast<int32>(spellInfo->CalcCastTime());
    player->ModSpellCastTime(spellInfo, effectiveCastTimeMs);

    bool const isFoodOrDrinkSpell = resolvedSpellId == SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT || resolvedSpellId == SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK;
    bool const isHunterStationaryCastTimeAction = player->GetClass() == CLASS_HUNTER && IsHunterCastTimeShot(player, spellInfo);
    bool const movableCastTimeSpell = IsPlayerbotMovableCastTimeSpell(player, spellInfo);
    bool const requiresStationaryCast = (effectiveCastTimeMs > 0 && !movableCastTimeSpell) || spellInfo->IsAutoRepeatRangedSpell() || isFoodOrDrinkSpell ||
        isHunterStationaryCastTimeAction || IsPlayerbotStationaryChannel(spellInfo);

    if (isHunterStationaryCastTimeAction)
    {
        WhisperHunterCastDiagnostic(player, target, "stationary_candidate", resolvedSpellId);

        if (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHunterStationaryCastLockToken))
        {
            WhisperHunterCastDiagnostic(player, target, "stationary_blocked_lock_active", resolvedSpellId);
            failureReason = "hunter_stationary_cast_already_in_progress";
            return false;
        }

        if (Spell const* currentGeneric = player->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (currentGeneric->getState() != SPELL_STATE_FINISHED)
                if (SpellInfo const* currentInfo = currentGeneric->GetSpellInfo())
                    if (IsHunterCastTimeShot(player, currentInfo))
                    {
                        WhisperHunterCastDiagnostic(player, target, "stationary_blocked_casttime_active", resolvedSpellId);
                        failureReason = "hunter_stationary_cast_already_in_progress";
                        return false;
                    }

        // Aimed Shot/Revive Pet must not coexist with a queued Auto Shot.
        // Movement/decision locks are not enough: CURRENT_AUTOREPEAT_SPELL can
        // still update independently and clip the generic cast on this branch.
        StopHunterAutoShotForStationaryCast(player, "hunter_pre_cast_stop_autoshot_for_stationary_cast");
        WhisperHunterCastDiagnostic(player, target, "pre_cast_autoshot_suppressed", resolvedSpellId);
    }

    if (requiresStationaryCast)
    {
        std::string stationaryDeferDiag;
        if (!itemTarget && target && ShouldDeferStationaryCastForActiveMovement(player, target, spellInfo, context, isFoodOrDrinkSpell, &stationaryDeferDiag))
        {
            SetLastMovementDebugStatus(player, stationaryDeferDiag);
            TC_LOG_DEBUG("playerbots.pvp.classspell", "PB stationary cast stop suppressed: {}", stationaryDeferDiag);
            failureReason = "stationary_cast_deferred_for_active_movement";
            return false;
        }

        // Force the bot into a fully stopped server-side state and attempt the
        // stationary spell in the same decision tick. Playerbots do not have a
        // client that can send a separate stop packet between AI decisions, so
        // waiting for the next decision tick after stopping leaves them idle for
        // seconds before casting. If a movement update still races the first
        // cast attempt, the SPELL_FAILED_MOVING retry below immediately stops
        // again and retries once without yielding the decision tick.
        NotifyWandDiagnostic(player, target, "pre_stationary_stop", resolvedSpellId);
        StopPlayerbotForStationaryCast(player);
        if (isHunterStationaryCastTimeAction)
            DelayHunterRangedTimerForStationaryShot(player, spellInfo, "pre_cast_stationary_stop");
        NotifyWandDiagnostic(player, target, "post_stationary_stop", resolvedSpellId);
    }

    // Blink (1953) is a leap-forward spell with a destination target
    // (TARGET_DEST_CASTER_FRONT_LEAP). For virtual bot sessions, casting only
    // on a unit target can leave relocation unresolved; provide an explicit
    // front destination to mirror client cast payload semantics.
    bool const isInstantCast = spellInfo->CalcCastTime() == 0 && !isHunterStationaryCastTimeAction;
    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && isInstantCast)
    {
        FaceTargetForInstantCast(player, target, spellInfo);

        if (spellInfo->IsAutoRepeatRangedSpell())
        {
            NotifyWandDiagnostic(player, target, "post_face", resolvedSpellId);

            bool const faceRestoredMovement = player->isMoving() ||
                player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE) ||
                (player->GetUnitMovementFlags() & MOVEMENTFLAG_MASK_MOVING);
            if (faceRestoredMovement)
            {
                StopPlayerbotForStationaryCast(player);
                NotifyWandDiagnostic(player, target, "post_face_stop", resolvedSpellId);
            }
        }
    }

    // Starfire (and anything else IsPlayerbotMovableCastTimeSpell allows) can
    // be cast while moving via the caster-side snare mechanic in Spell.cpp,
    // so unlike a normal cast-time spell the bot is not necessarily already
    // stationary and facing the target when the cast starts - it could still
    // be turned toward whatever direction it was last kiting. Force facing
    // immediately before the cast attempt instead of relying on whatever
    // orientation movement last left the bot in.
    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && IsPlayerbotMovableCastTimeSpell(player, spellInfo))
    {
        player->SetFacingToObject(target);
        player->SetInFront(target);
    }

    NotifyWandDiagnostic(player, target, "pre_cast", resolvedSpellId);
    if (isHunterStationaryCastTimeAction)
        WhisperHunterCastDiagnostic(player, target, "pre_cast", resolvedSpellId);

    SpellCastResult castResult = SPELL_FAILED_ERROR;
    if (resolvedSpellId == 1953 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
    {
        // GetFirstCollisionPosition only raycasts for navmesh/VMap collision;
        // it does not re-ground the resulting Z against actual terrain height.
        // On multi-layer geometry (bridges, cliffs, caves) that can leave the
        // destination floating or clipped into the ground, which reads as the
        // bot falling through the map after the leap lands. Re-ground it the
        // same way regular movement destinations already do.
        Position const dest = BuildCollisionSafeDestination(player, player->GetFirstCollisionPosition(20.0f, player->GetOrientation()));
        castResult = player->CastSpell(CastSpellTargetArg(dest), resolvedSpellId);
    }
    else if (resolvedSpellId == 89160 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && target)
    {
        Position dest = target->GetPosition();
        castResult = player->CastSpell(CastSpellTargetArg(dest), resolvedSpellId);
    }
    // 81271 - Heroic Leap is a ground-targeted gap closer/escape (ranged like
    // the gnome racial). Enemy mode leaps at the gap-close target; Self mode
    // leaps away from the current melee threat to disengage.
    else if (resolvedSpellId == 81271 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && target)
    {
        Position dest = target->GetPosition();
        castResult = player->CastSpell(CastSpellTargetArg(dest), resolvedSpellId);
    }
    else if (resolvedSpellId == 81271 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
    {
        Unit* threat = player->GetVictim();
        float const awayAngle = threat ? player->GetAbsoluteAngle(threat->GetPosition()) + static_cast<float>(M_PI) : player->GetOrientation();
        // See the Blink comment above: re-ground the raycast destination so the
        // leap cannot land the warrior clipped into or floating above terrain.
        Position dest = BuildCollisionSafeDestination(player, player->GetFirstCollisionPosition(20.0f, awayAngle));
        castResult = player->CastSpell(CastSpellTargetArg(dest), resolvedSpellId);
    }
    // 82419 - Rehgar's Fury uses the same SPELL_EFFECT_JUMP_DEST mechanic as
    // Heroic Leap (Spell::EffectJumpDest requires m_targets.HasDst() or it
    // silently no-ops). A plain unit-target cast never populates a
    // destination, so without this the spell "casts" successfully but the
    // charge never actually happens - it needs the same explicit dest cast.
    else if (resolvedSpellId == 82419 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && target)
    {
        Position dest = target->GetPosition();
        WhisperRehgarsFuryMovementDiagnostic(player, target, "pre_cast");
        castResult = player->CastSpell(CastSpellTargetArg(dest), resolvedSpellId);
    }
    else if (itemTarget)
        castResult = player->CastSpell(CastSpellTargetArg(itemTarget), resolvedSpellId);
    else
        castResult = player->CastSpell(target, resolvedSpellId, false);

    if (castResult == SPELL_CAST_OK)
    {
        NotifyWandDiagnostic(player, target, "cast_ok", resolvedSpellId);
        if (resolvedSpellId == 82419)
        {
            WhisperRehgarsFuryMovementDiagnostic(player, target, "cast_ok");
            ScheduleRehgarsFuryMovementDiagnostics(player, target);
        }
        if (isHunterStationaryCastTimeAction)
        {
            WhisperHunterCastDiagnostic(player, target, "cast_ok", resolvedSpellId);
            Spell const* immediateGeneric = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            SpellInfo const* immediateGenericInfo = immediateGeneric ? immediateGeneric->GetSpellInfo() : nullptr;
            std::string postExtra = std::string("current=") + std::to_string(immediateGenericInfo ? immediateGenericInfo->Id : 0) +
                " state=" + GetSpellStateLabel(immediateGeneric);
            WhisperHunterCastDiagnostic(player, target, "post_cast_immediate_slot", resolvedSpellId, postExtra.c_str());
        }
    }
    else
    {
        EnumText const wandCastResultText = EnumUtils::ToString(castResult);
        NotifyWandDiagnostic(player, target, "cast_failed", resolvedSpellId, wandCastResultText.Title);
        if (resolvedSpellId == 82419)
            WhisperRehgarsFuryMovementDiagnostic(player, target, "cast_failed", wandCastResultText.Title);
        if (isHunterStationaryCastTimeAction)
            WhisperHunterCastDiagnostic(player, target, "cast_failed", resolvedSpellId, wandCastResultText.Title);
    }

    if (castResult != SPELL_CAST_OK)
    {
        if (castResult == SPELL_FAILED_MOVING && requiresStationaryCast)
        {
            StopPlayerbotForStationaryCast(player);

            // If an immediately preceding movement update raced this cast, the
            // failed moving check did not consume the spell. Retry once after
            // force-clearing the stopped state so channels do not keep reporting
            // SPELL_FAILED_MOVING even though the bot has now stopped.
            if (!player->isMoving())
            {
                if (itemTarget)
                    castResult = player->CastSpell(CastSpellTargetArg(itemTarget), resolvedSpellId);
                else
                    castResult = player->CastSpell(target, resolvedSpellId, false);

                if (castResult == SPELL_CAST_OK)
                    NotifyWandDiagnostic(player, target, "retry_after_moving_ok", resolvedSpellId);
                else
                {
                    EnumText const retryResultText = EnumUtils::ToString(castResult);
                    NotifyWandDiagnostic(player, target, "retry_after_moving_failed", resolvedSpellId, retryResultText.Title);
                }
            }
        }

        if (castResult != SPELL_CAST_OK && !itemTarget && target && CanIssueFollowCommands(player))
        {
            if (castResult == SPELL_FAILED_OUT_OF_RANGE)
            {
                if (IsHunterExactDeadZone(player, target))
                    IssueHunterDeadZoneRetreatMovement(player, target, "hunter_deadzone_retreat_from_out_of_range_cast");
                else if (shouldMoveBehindForEnemySpell)
                    IssueBehindTargetMeleeMovement(player, target);
                else if (shouldUseMeleeApproachForEnemySpell)
                    IssueMeleeApproachMovement(player, target);
                else
                {
                    float const desiredRange = maxRange > 0.0f ? std::max(1.0f, maxRange - 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().spellRange - 1.0f);
                    IssueRangedApproachMovement(player, target, desiredRange);
                }
            }
            else if (castResult == SPELL_FAILED_TOO_CLOSE)
            {
                if (IsHunterExactDeadZone(player, target))
                    IssueHunterDeadZoneRetreatMovement(player, target, "hunter_deadzone_retreat_from_too_close_cast");
                else
                {
                    float const desiredRange = minRange > 0.0f ? std::max(1.0f, minRange + 1.0f) : std::max(1.0f, playerbot::PvpCore::GetConfig().closeRange);
                    player->GetMotionMaster()->MoveFollow(target, desiredRange, player->GetFollowAngle());
                }
            }
            else if (castResult == SPELL_FAILED_LINE_OF_SIGHT)
            {
                if (shouldMoveBehindForEnemySpell)
                    IssueBehindTargetMeleeMovement(player, target);
                else if (shouldUseMeleeApproachForEnemySpell)
                    IssueMeleeApproachMovement(player, target);
                else
                {
                    float const desiredRange = (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
                        ? std::max(1.5f, std::min(8.0f, maxRange > 0.0f ? (maxRange - 1.0f) : 8.0f))
                        : ComputeLosRecoveryRange(player, target, maxRange);
                    IssueRangedApproachMovement(player, target, desiredRange, true, "cast_failed_spell_los");
                }
            }
            else if (castResult == SPELL_FAILED_NOT_BEHIND && shouldMoveBehindForEnemySpell)
                IssueBehindTargetMeleeMovement(player, target);
        }

        if (castResult != SPELL_CAST_OK)
        {
            if (context.spellId == kHunterCallPetSpellId || context.spellId == kHunterRevivePetSpellId)
            {
                playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, context.spellId, kHunterPetFailureBackoff);
                WhisperPlayerbotDiagnostic(player, BuildHunterPetDiagnostic(player,
                    context.spellId == kHunterCallPetSpellId ? "call_pet_failed" : "revive_pet_failed", castResult));
            }

            NotifySpellCastFailureToGameMasters(player, context, castResult);
            EnumText const reasonText = EnumUtils::ToString(castResult);
            failureReason = reasonText.Title;
            return false;
        }
    }

    if (player->GetClass() == CLASS_HUNTER && IsHunterCastTimeShot(player, spellInfo))
    {
        StopHunterAutoShotForStationaryCast(player, "hunter_cast_accepted_stop_autoshot_for_stationary_cast");
        DelayHunterRangedTimerForStationaryShot(player, spellInfo, "cast_accepted");
        uint32 const castTimeMs = GetHunterStationaryCastTimeMs(spellInfo);
        uint32 const lockMs = std::clamp<uint32>(castTimeMs + 750, 750, 12000);

        // Hunter shots with a stationary launch window can be clipped by the
        // lifecycle stutter loop before CURRENT_GENERIC_SPELL is visible to the
        // next AI tick. Publish an explicit movement/decision lock after the
        // cast is accepted, so every movement path yields until the shot/cast
        // has actually had time to finish. Aimed Shot gets a long guard from
        // its real cast time; Multi-Shot gets a short guard instead of the old
        // accidental 3-second minimum.
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, kPlayerbotHunterStationaryCastLockToken, std::chrono::milliseconds(lockMs));
        {
            std::string acceptExtra = "castTimeMs=" + std::to_string(castTimeMs) + " lockMs=" + std::to_string(lockMs);
            WhisperHunterCastDiagnostic(player, target, "accepted_lock_registered", resolvedSpellId, acceptExtra.c_str());
        }

        Unit* castGuardTarget = context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy ? target : nullptr;
        ScheduleHunterStationaryCastGuard(player, castGuardTarget, resolvedSpellId, castTimeMs);
    }

    if (player->GetClass() == CLASS_HUNTER && IsHunterBreakableCrowdControlSpell(spellInfo))
        StopHunterDamageOnBreakableCrowdControl(player, target, "hunter_breakable_cc_cast_stop_autoshot");

    if (IsPlayerbotStationaryChannel(spellInfo))
        StopPlayerbotForStationaryCast(player);

    ResumeDruidShapeshiftMovement(player, shapeshiftMovementResume, resolvedSpellId);

    if (resolvedSpellId == kRacialNightElfShadowmeldSpellId || context.spellId == kRacialNightElfShadowmeldSpellId)
    {
        StopPlayerbotForStationaryCast(player);
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, kPlayerbotShadowmeldGraceToken, std::chrono::milliseconds(500));
    }

    // Feign Death is only an instant defensive/trap-setup attempt. Do not queue a
    // delayed trap or pause movement waiting for combat to drop; if combat drops,
    // the next AI tick can select the out-of-combat trap normally. If combat is
    // clipped by damage, automatically stand back up shortly after and continue
    // the flee/stutter-shot and melee escape decisions instead of lying there.
    if (context.spellId == kHunterFeignDeathSpellId)
    {
        player->SetSelection(ObjectGuid::Empty);
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, kHunterFeignDeathSpellId, std::chrono::seconds(2));
        ScheduleHunterFeignDeathStandup(player);
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

    // Charge/Intercept target switching: preserve the intended enemy target in
    // selection/combat context, but do not immediately override an active
    // charge movement generator. For playerbot sessions an eager Attack() call
    // can replace the charge spline with chase in the same tick, which looks
    // like "charge debuff landed but the warrior never moved".
    if (hasChargeEffect &&
        context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
        target && target->IsAlive())
    {
        player->SetSelection(target->GetGUID());
        if (player->GetVictim() != target || !player->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            if (HasActiveMovementEffectSpline(player))
            {
                ObjectGuid const playerGuid = player->GetGUID();
                ObjectGuid const targetGuid = target->GetGUID();
                player->m_Events.AddEventAtOffset([playerGuid, targetGuid]()
                {
                    Player* delayedAttacker = ObjectAccessor::FindConnectedPlayer(playerGuid);
                    if (!delayedAttacker || !delayedAttacker->IsInWorld() || !delayedAttacker->IsAlive())
                        return;

                    Unit* delayedTarget = ObjectAccessor::GetUnit(*delayedAttacker, targetGuid);
                    if (!delayedTarget || !delayedTarget->IsAlive())
                        return;

                    if (delayedAttacker->GetVictim() != delayedTarget || !delayedAttacker->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                        delayedAttacker->Attack(delayedTarget, true);
                }, std::chrono::milliseconds(250));
            }
            else
                player->Attack(target, true);
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
    if (resolvedSpellId && sSpellMgr->GetFirstSpellInChain(resolvedSpellId) == sSpellMgr->GetFirstSpellInChain(9898))
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, 9898, std::chrono::seconds(10));

    if ((context.spellId == 11719 || context.spellId == 11713) && target)
        playerbot::PvpClassActions::RegisterWarlockCurseTargetCooldown(player, target, context.spellId, std::chrono::seconds(12));
    if (resolvedSpellId && sSpellMgr->GetFirstSpellInChain(resolvedSpellId) == sSpellMgr->GetFirstSpellInChain(6940))
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, kPlayerbotHandOfSacrificeCooldownToken, std::chrono::seconds(10));
    if (resolvedSpellId && sSpellMgr->GetFirstSpellInChain(resolvedSpellId) == sSpellMgr->GetFirstSpellInChain(32593))
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, 32593, std::chrono::seconds(12));
    if (resolvedSpellId && sSpellMgr->GetFirstSpellInChain(resolvedSpellId) == sSpellMgr->GetFirstSpellInChain(kDruidCasterFaerieFireSpellId))
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, kDruidCasterFaerieFireSpellId, kDruidCasterFaerieFireCooldown);
    if (spellInfo->IsAutoRepeatRangedSpell())
    {
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, resolvedSpellId, kPlayerbotAutoRepeatRangedStartCooldown);
        ScheduleWandDiagnostics(player, target, resolvedSpellId);
    }

    // Shared tactical cooldown for dispel/decurse effects. This keeps
    // playerbots from spam-casting into protected or undispellable auras while
    // allowing the next decision tick to fall through to another action.
    if (IsPlayerbotDispelSpell(resolvedSpellId))
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, kPlayerbotDispelCooldownToken, kPlayerbotDispelCooldown);

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

    SpellInfo const* itemSpellInfo = GetFirstOnUseItemSpellInfo(item);
    if (!itemSpellInfo)
    {
        failureReason = "item_spell_missing";
        return false;
    }

    if (player->GetSpellHistory()->HasCooldown(itemSpellInfo->Id) ||
        player->GetSpellHistory()->HasCooldown(itemSpellInfo, item->GetEntry()) ||
        player->GetSpellHistory()->HasGlobalCooldown(itemSpellInfo))
    {
        failureReason = "item_spell_not_ready";
        return false;
    }

    SpellCastTargets targets;
    targets.SetUnitTarget(player);
    player->CastItemUseSpell(item, targets, 1, 0);

    // CastItemUseSpell queues the Spell and does not return the prepare result.
    // Give the PvP decision layer a tiny local throttle so this item action cannot
    // monopolize several AI ticks if the item spell was rejected internally.
    playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, context.spellId ? context.spellId : itemSpellInfo->Id, std::chrono::seconds(2));
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

void PvpClassActions::RegisterCasterSpellCooldown(Player const* player, uint32 spellId, std::chrono::milliseconds cooldown)
{
    if (!player || !spellId || cooldown <= std::chrono::milliseconds::zero())
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


bool PvpClassActions::HasRecentTargetRelativeMovementOrder(Player const* player, Unit const* target, uint32 maxAgeMs)
{
    if (!player)
        return false;

    auto orderItr = g_TargetRelativeMoveOrderByGuid.find(player->GetGUID().GetRawValue());
    if (orderItr == g_TargetRelativeMoveOrderByGuid.end())
        return false;

    TargetRelativeMoveOrderState const& order = orderItr->second;
    if (target && order.targetGuid != target->GetGUID())
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const ageMs = order.lastIssueMs != 0 && nowMs >= order.lastIssueMs ? nowMs - order.lastIssueMs : std::numeric_limits<uint32>::max();
    return ageMs <= maxAgeMs;
}

bool PvpClassActions::IsPetSpellAction(Player const* player, PvpClassSpellContext const& context)
{
    return player && context.spellId != 0 && ResolveKnownPetSpellInChain(player, context.spellId) != 0;
}

// 89784 - Shadow Wraith ("Fade"). Casting it roots the priest's own body
// (see spell_pri_shadow_wraith_aura::OnApply in spell_priest.cpp) and hands
// control to a summoned wraith creature via possession instead. A real
// client automatically starts steering whatever it is possessing the moment
// control transfers, but playerbot movement code only ever issues motion
// orders to the Player* - the wraith itself never gets one, so it just sits
// still, defeating the entire point of using Fade to escape melee pressure.
// Move the possessed wraith away from the threat directly.
bool PvpClassActions::TryIssueShadowWraithFleeMovement(Player* player, Unit* threat)
{
    if (!player || !player->HasAura(89784) || !threat)
        return false;

    Unit* wraith = player->GetCharmed();
    if (!wraith || !wraith->IsAlive())
        return false;

    // Re-steering every tick would fight the wraith's own in-flight spline
    // for no reason once it has already put real distance between itself
    // and the threat. Only reissue while still uncomfortably close.
    constexpr float kWraithFleeDistance = 15.0f;
    if (wraith->GetDistance(threat) > kWraithFleeDistance)
        return false;

    float const awayAngle = wraith->GetAbsoluteAngle(threat->GetPosition()) + static_cast<float>(M_PI);
    Position destination = wraith->GetPosition();
    destination.RelocateOffset({ std::cos(awayAngle) * kWraithFleeDistance, std::sin(awayAngle) * kWraithFleeDistance, 0.0f, 0.0f });

    float adjustedZ = destination.GetPositionZ();
    wraith->UpdateAllowedPositionZ(destination.GetPositionX(), destination.GetPositionY(), adjustedZ);
    destination.Relocate(destination.GetPositionX(), destination.GetPositionY(), adjustedZ, destination.GetOrientation());

    MotionMaster* wraithMotionMaster = wraith->GetMotionMaster();
    if (!wraithMotionMaster)
        return false;

    wraithMotionMaster->MovePoint(0, destination, true);
    return true;
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
                // Root-cause fix for rogue opener stalls:
                // LOS recovery can emit ReachSpellRange with a tiny range (e.g. 4y)
                // while stealthed, but stealth opener movement should use the
                // deterministic melee chase path. Routing this through ranged
                // approach caused repeated slow/stale reissues in BG starts.
                if (IsStealthedMeleeOpener(player) && movementTarget && player->IsValidAttackTarget(movementTarget))
                {
                    IssueMeleeApproachMovement(player, movementTarget);
                    break;
                }

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
                    // If strict segment pathing fails to resolve, PathGenerator
                    // has already determined this flee destination is not
                    // safely reachable. Do NOT fall back to a raw point move
                    // here - that fallback was what let fleeing bots (hunters
                    // especially, since they flee constantly) walk straight
                    // through walls and eventually fall below the map.
                    // Standing still for a tick is far cheaper than clipping
                    // through geometry. Loosened reissue cadence for the same
                    // reason as the hunter dead-zone retreat: the default
                    // 4y/350ms throttle reissues a fresh segment (and re-runs
                    // its floor computation) far more often than a fleeing
                    // bot actually needs, which was contributing to both the
                    // visible stutter and the floor-clip risk.
                    IssueStrictHumanMove(player, destination, 5.0f, 500);
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
