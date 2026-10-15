#include "BattlegroundTOK.h"

#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"

#include <algorithm>

namespace
{
struct TokOrbSpawn
{
    char const* name;
    float x, y;
};

// The pedestals are terrain doodads (murderball_pedestal.m2, scale 1.78), so
// only the ball is spawned. x/y are the pedestals' own placements read out of
// the ADTs; the ball rests on the pedestal top, 10.643 + 1.63 * 1.78.
TokOrbSpawn constexpr TOK_ORBS[BG_TOK_ORB_COUNT] =
{
    { "Blue Orb",   1716.778f, 1416.739f },
    { "Purple Orb", 1850.251f, 1416.822f },
    { "Green Orb",  1850.252f, 1250.275f },
    { "Orange Orb", 1716.882f, 1249.900f }
};

float constexpr TOK_PEDESTAL_BASE_Z = 10.643f;
float constexpr TOK_ORB_Z           = 13.571f;

// Scoring rings, from the temple's WMO placements. The centre is the sunken
// pool (MG_MIDDLEPOOL's bounds); its floor sits near z 5.6 against 10.6 for the
// courtyard around it, so the height test keeps someone walking the rim out of
// it. The courtyard is the area enclosed by the four corner buildings.
float constexpr TOK_CENTER_MIN_X = 1744.26f, TOK_CENTER_MAX_X = 1822.79f;
float constexpr TOK_CENTER_MIN_Y = 1281.86f, TOK_CENTER_MAX_Y = 1384.94f;
float constexpr TOK_CENTER_MAX_Z = 9.5f;
float constexpr TOK_COURTYARD_MIN_X = 1685.74f, TOK_COURTYARD_MAX_X = 1881.11f;
float constexpr TOK_COURTYARD_MIN_Y = 1212.31f, TOK_COURTYARD_MAX_Y = 1454.23f;

// Where a bot carrier goes to hold its orb: the middle of the sunken pool,
// spread a little per orb so four carriers do not stand in one spot.
float constexpr TOK_HOLD_X = 1783.5f;
float constexpr TOK_HOLD_Y = 1333.4f;
float constexpr TOK_HOLD_Z = 5.62f;
float constexpr TOK_HOLD_OFFSET[BG_TOK_ORB_COUNT][2] =
{
    { -6.0f,  7.0f },
    {  6.0f,  7.0f },
    {  6.0f, -7.0f },
    { -6.0f, -7.0f }
};

bool InRect(float x, float y, float minX, float maxX, float minY, float maxY)
{
    return x >= minX && x <= maxX && y >= minY && y <= maxY;
}

uint32 OrbHolderSpell(uint8 orb) { return BG_TOK_SPELL_ORB_HOLDER_FIRST + orb; }
uint32 OrbStackSpell(uint8 orb)  { return BG_TOK_SPELL_ORB_STACK_FIRST + orb; }
}

void BattlegroundTOKScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra scoreboard columns
}

BattlegroundTOK::BattlegroundTOK()
{
    BgObjects.resize(BG_TOK_OBJECT_MAX);
    BgCreatures.resize(BG_TOK_CREATURE_MAX);
    _pointTickTimer = 0;
    _carrierCheckTimer = 0;
    _honorScoreTicks[TEAM_ALLIANCE] = 0;
    _honorScoreTicks[TEAM_HORDE] = 0;
    _honorTicks = BG_TOK_HONOR_TICKS;
}

void BattlegroundTOK::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundTOKScore(player->GetGUID(), scoreboardTeamMarker);
    }
}

void BattlegroundTOK::RemovePlayer(Player* player, ObjectGuid guid, uint32 /*team*/)
{
    int32 const orb = FindOrbHeldBy(guid);
    if (orb >= 0)
        ReturnOrb(uint8(orb), player, true);
}

void BattlegroundTOK::Reset()
{
    Battleground::Reset();

    for (OrbState& orb : _orbs)
        orb = OrbState();

    m_TeamScores[TEAM_ALLIANCE] = 0;
    m_TeamScores[TEAM_HORDE] = 0;
    _pointTickTimer = 0;
    _carrierCheckTimer = 0;
    _honorScoreTicks[TEAM_ALLIANCE] = 0;
    _honorScoreTicks[TEAM_HORDE] = 0;
    _honorTicks = sBattlegroundMgr->IsBGWeekend(GetTypeID()) ? BG_TOK_HONOR_TICKS_WEEKEND : BG_TOK_HONOR_TICKS;
}

bool BattlegroundTOK::SetupBattleground()
{
    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
    {
        TokOrbSpawn const& spawn = TOK_ORBS[orb];
        if (!AddObject(BG_TOK_OBJECT_ORB_FIRST + orb, BG_TOK_ENTRY_ORB_FIRST + orb,
                spawn.x, spawn.y, TOK_ORB_Z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, RESPAWN_ONE_DAY)
            || !AddObject(BG_TOK_OBJECT_BEAM_FIRST + orb, BG_TOK_ENTRY_BEAM_FIRST + orb,
                spawn.x, spawn.y, TOK_PEDESTAL_BASE_Z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, RESPAWN_ONE_DAY))
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundTOK::SetupBattleground: failed to spawn the {}.", spawn.name);
            return false;
        }
    }

    struct SpiritSpawn
    {
        uint32 type;
        uint32 safeLoc;
        TeamId team;
    };

    SpiritSpawn constexpr spirits[] =
    {
        { BG_TOK_SPIRIT_ALLIANCE_1, BG_TOK_GY_ALLIANCE_1, TEAM_ALLIANCE },
        { BG_TOK_SPIRIT_ALLIANCE_2, BG_TOK_GY_ALLIANCE_2, TEAM_ALLIANCE },
        { BG_TOK_SPIRIT_HORDE_1,    BG_TOK_GY_HORDE_1,    TEAM_HORDE },
        { BG_TOK_SPIRIT_HORDE_2,    BG_TOK_GY_HORDE_2,    TEAM_HORDE }
    };

    for (SpiritSpawn const& spirit : spirits)
    {
        WorldSafeLocsEntry const* loc = sWorldSafeLocsStore.LookupEntry(spirit.safeLoc);
        if (!loc)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundTOK::SetupBattleground: WorldSafeLocs {} is missing.", spirit.safeLoc);
            return false;
        }

        if (!AddSpiritGuide(spirit.type, loc->Loc.X, loc->Loc.Y, loc->Loc.Z, 0.0f, spirit.team))
            return false;
    }

    return true;
}

void BattlegroundTOK::StartingEventCloseDoors()
{
    // No gates: battleground_template.StartMaxDist holds everyone in their start
    // area until the doors would open. The orbs appear with the start.
    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
        ShowOrbOnPedestal(orb, false);
}

void BattlegroundTOK::StartingEventOpenDoors()
{
    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
    {
        _orbs[orb] = OrbState();
        ShowOrbOnPedestal(orb, true);
    }

    _pointTickTimer = 0;
    _carrierCheckTimer = 0;
    UpdateScoreWorldStates();
}

void BattlegroundTOK::ShowOrbOnPedestal(uint8 orb, bool show)
{
    uint32 const respawn = show ? RESPAWN_IMMEDIATELY : RESPAWN_ONE_DAY;
    SpawnBGObject(BG_TOK_OBJECT_ORB_FIRST + orb, respawn);
    SpawnBGObject(BG_TOK_OBJECT_BEAM_FIRST + orb, respawn);

    // The beam is scenery; only the ball takes clicks.
    if (GameObject* beam = GetBGObject(BG_TOK_OBJECT_BEAM_FIRST + orb, false))
        beam->SetFlag(GO_FLAG_NOT_SELECTABLE);
}

void BattlegroundTOK::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    _carrierCheckTimer += diff;
    if (_carrierCheckTimer >= BG_TOK_CARRIER_CHECK_TIME)
    {
        _carrierCheckTimer = 0;
        ValidateCarriers();
    }

    _pointTickTimer += diff;
    if (_pointTickTimer >= BG_TOK_POINT_TICK_TIME)
    {
        _pointTickTimer -= BG_TOK_POINT_TICK_TIME;
        AwardCarrierPoints();
    }
}

int32 BattlegroundTOK::FindOrbHeldBy(ObjectGuid guid) const
{
    if (guid.IsEmpty())
        return -1;

    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
        if (_orbs[orb].Carrier == guid)
            return orb;

    return -1;
}

uint32 BattlegroundTOK::CountOrbsHeldBy(uint32 team) const
{
    return uint32(std::count_if(_orbs.begin(), _orbs.end(),
        [team](OrbState const& orb) { return !orb.Carrier.IsEmpty() && orb.CarrierTeam == team; }));
}

void BattlegroundTOK::EventPlayerClickedOnFlag(Player* player, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !player || !target_obj || !player->IsAlive())
        return;

    // One orb per player.
    if (FindOrbHeldBy(player->GetGUID()) >= 0)
        return;

    int32 orb = -1;
    for (uint8 i = 0; i < BG_TOK_ORB_COUNT; ++i)
        if (BgObjects[BG_TOK_OBJECT_ORB_FIRST + i] == target_obj->GetGUID())
            orb = i;

    if (orb < 0 || !_orbs[orb].Carrier.IsEmpty() || !player->IsWithinDistInMap(target_obj, 10.0f))
        return;

    uint32 const team = GetPlayerTeam(player->GetGUID());
    if (team != ALLIANCE && team != HORDE)
        return;

    // The holder aura drops when its owner mounts, so a carrier must never be
    // left sitting on one.
    player->RemoveAurasByType(SPELL_AURA_MOUNTED);

    ShowOrbOnPedestal(uint8(orb), false);
    _orbs[orb].Carrier = player->GetGUID();
    _orbs[orb].CarrierTeam = team;

    player->CastSpell(player, OrbHolderSpell(uint8(orb)), true);
    player->CastSpell(player, OrbStackSpell(uint8(orb)), true);
    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);

    PSendMessageToAll(BG_TOK_STRING_ORB_TAKEN, team == ALLIANCE ? CHAT_MSG_BG_SYSTEM_ALLIANCE : CHAT_MSG_BG_SYSTEM_HORDE,
        player, player->GetName().c_str(), TOK_ORBS[orb].name);
    PlaySoundToAll(team == ALLIANCE ? BG_TOK_SOUND_ORB_PICKED_UP_ALLIANCE : BG_TOK_SOUND_ORB_PICKED_UP_HORDE);

    UpdateScoreWorldStates();
}

void BattlegroundTOK::EventPlayerDroppedFlag(Player* player)
{
    if (!player)
        return;

    int32 const orb = FindOrbHeldBy(player->GetGUID());
    if (orb >= 0)
        ReturnOrb(uint8(orb), player, GetStatus() == STATUS_IN_PROGRESS);
}

void BattlegroundTOK::ReturnOrb(uint8 orb, Player* carrier, bool announce)
{
    if (orb >= BG_TOK_ORB_COUNT || _orbs[orb].Carrier.IsEmpty())
        return;

    _orbs[orb] = OrbState();

    if (carrier)
    {
        carrier->RemoveAurasDueToSpell(OrbHolderSpell(orb));
        carrier->RemoveAurasDueToSpell(OrbStackSpell(orb));
    }

    // Retail sends an orb home the moment its carrier loses it; there is no
    // ground orb to fight over.
    bool const running = GetStatus() == STATUS_IN_PROGRESS;
    ShowOrbOnPedestal(orb, running);

    if (announce && running)
    {
        PSendMessageToAll(BG_TOK_STRING_ORB_RETURNED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TOK_ORBS[orb].name);
        PlaySoundToAll(BG_TOK_SOUND_ORB_RETURNED);
    }

    UpdateScoreWorldStates();
}

void BattlegroundTOK::ValidateCarriers()
{
    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
    {
        ObjectGuid const carrierGuid = _orbs[orb].Carrier;
        if (carrierGuid.IsEmpty())
            continue;

        // The holder aura carries every way of losing the orb that is not a
        // battleground event: it is interrupted by mounting, by turning immune
        // (Divine Shield, Ice Block) and by changing map, and death removes it.
        // Checking it here keeps all of those in one place.
        Player* carrier = ObjectAccessor::FindPlayer(carrierGuid);
        if (!carrier || carrier->GetMapId() != GetMapId() || !carrier->IsAlive() || !carrier->HasAura(OrbHolderSpell(orb)))
        {
            ReturnOrb(orb, carrier, true);
            continue;
        }

        // Retail does not let an orb carrier hide.
        carrier->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);
        carrier->RemoveAurasByType(SPELL_AURA_MOD_INVISIBILITY);
    }
}

uint32 BattlegroundTOK::GetPointsAtCarrierPosition(Player const* carrier) const
{
    float const x = carrier->GetPositionX();
    float const y = carrier->GetPositionY();

    if (InRect(x, y, TOK_CENTER_MIN_X, TOK_CENTER_MAX_X, TOK_CENTER_MIN_Y, TOK_CENTER_MAX_Y) &&
        carrier->GetPositionZ() <= TOK_CENTER_MAX_Z)
        return BG_TOK_POINTS_CENTER;

    if (InRect(x, y, TOK_COURTYARD_MIN_X, TOK_COURTYARD_MAX_X, TOK_COURTYARD_MIN_Y, TOK_COURTYARD_MAX_Y))
        return BG_TOK_POINTS_COURTYARD;

    return BG_TOK_POINTS_OUTSIDE;
}

void BattlegroundTOK::AwardCarrierPoints()
{
    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
    {
        OrbState const state = _orbs[orb];
        if (state.Carrier.IsEmpty())
            continue;

        Player* carrier = ObjectAccessor::FindPlayer(state.Carrier);
        if (!carrier || !carrier->IsAlive() || carrier->GetMapId() != GetMapId())
            continue;

        AddPoints(state.CarrierTeam, ScaleResourceGain(GetPointsAtCarrierPosition(carrier)));
        if (GetStatus() != STATUS_IN_PROGRESS)
            return;
    }
}

void BattlegroundTOK::AddPoints(uint32 team, uint32 points)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !points || (team != ALLIANCE && team != HORDE))
        return;

    TeamId const teamIndex = GetTeamIndexByTeamId(team);
    uint32 const limit = GetResourceLimit(BG_TOK_MAX_TEAM_SCORE);

    uint32 const score = std::min<uint32>(uint32(m_TeamScores[teamIndex]) + points, limit);
    m_TeamScores[teamIndex] = score;

    _honorScoreTicks[teamIndex] += points;
    if (_honorTicks && _honorScoreTicks[teamIndex] >= _honorTicks)
    {
        RewardHonorToTeam(sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) / 2, team);
        _honorScoreTicks[teamIndex] -= _honorTicks;
    }

    UpdateScoreWorldStates();

    if (score >= limit)
        EndBattleground(team);
}

void BattlegroundTOK::UpdateScoreWorldStates()
{
    UpdateWorldState(BG_TOK_WORLDSTATE_SHOW, 1);
    UpdateWorldState(BG_TOK_WORLDSTATE_ALLIANCE_SCORE, m_TeamScores[TEAM_ALLIANCE]);
    UpdateWorldState(BG_TOK_WORLDSTATE_HORDE_SCORE, m_TeamScores[TEAM_HORDE]);
    UpdateWorldState(BG_TOK_WORLDSTATE_MAX_SCORE, GetResourceLimit(BG_TOK_MAX_TEAM_SCORE));
    UpdateWorldState(BG_TOK_WORLDSTATE_ALLIANCE_ORBS, CountOrbsHeldBy(ALLIANCE));
    UpdateWorldState(BG_TOK_WORLDSTATE_HORDE_ORBS, CountOrbsHeldBy(HORDE));
}

void BattlegroundTOK::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_TOK_WORLDSTATE_SHOW, 1);
    packet.Worldstates.emplace_back(BG_TOK_WORLDSTATE_ALLIANCE_SCORE, m_TeamScores[TEAM_ALLIANCE]);
    packet.Worldstates.emplace_back(BG_TOK_WORLDSTATE_HORDE_SCORE, m_TeamScores[TEAM_HORDE]);
    packet.Worldstates.emplace_back(BG_TOK_WORLDSTATE_MAX_SCORE, GetResourceLimit(BG_TOK_MAX_TEAM_SCORE));
    packet.Worldstates.emplace_back(BG_TOK_WORLDSTATE_ALLIANCE_ORBS, CountOrbsHeldBy(ALLIANCE));
    packet.Worldstates.emplace_back(BG_TOK_WORLDSTATE_HORDE_ORBS, CountOrbsHeldBy(HORDE));
}

ObjectGuid BattlegroundTOK::GetFlagPickerGUID(int32 team) const
{
    for (OrbState const& orb : _orbs)
    {
        if (orb.Carrier.IsEmpty())
            continue;

        if (team == -1 ||
            (team == TEAM_ALLIANCE && orb.CarrierTeam == ALLIANCE) ||
            (team == TEAM_HORDE && orb.CarrierTeam == HORDE))
            return orb.Carrier;
    }

    return ObjectGuid::Empty;
}

void BattlegroundTOK::GetObjectiveCarriers(std::vector<ObjectGuid>& carriers) const
{
    for (OrbState const& orb : _orbs)
        if (!orb.Carrier.IsEmpty())
            carriers.push_back(orb.Carrier);
}

ObjectGuid BattlegroundTOK::GetFlagPickupGUID(ObjectGuid playerGuid) const
{
    if (GetStatus() != STATUS_IN_PROGRESS || FindOrbHeldBy(playerGuid) >= 0)
        return ObjectGuid::Empty;

    Player const* player = ObjectAccessor::FindPlayer(playerGuid);
    if (!player || !FindBgMap())
        return ObjectGuid::Empty;

    // An orb nobody carries is on its pedestal: a lost orb goes straight home,
    // so the carrier slot is the whole state.
    ObjectGuid nearest;
    float nearestDist = 0.0f;
    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
    {
        if (!_orbs[orb].Carrier.IsEmpty())
            continue;

        GameObject const* go = GetBgMap()->GetGameObject(BgObjects[BG_TOK_OBJECT_ORB_FIRST + orb]);
        if (!go)
            continue;

        float const dist = player->GetExactDist2dSq(go);
        if (nearest.IsEmpty() || dist < nearestDist)
        {
            nearest = go->GetGUID();
            nearestDist = dist;
        }
    }

    return nearest;
}

bool BattlegroundTOK::GetFlagCapturePosition(ObjectGuid carrierGuid, Position& position) const
{
    int32 const orb = FindOrbHeldBy(carrierGuid);
    if (orb < 0)
        return false;

    position.Relocate(TOK_HOLD_X + TOK_HOLD_OFFSET[orb][0], TOK_HOLD_Y + TOK_HOLD_OFFSET[orb][1], TOK_HOLD_Z, 0.0f);
    return true;
}

WorldSafeLocsEntry const* BattlegroundTOK::GetClosestGraveyard(Player* player)
{
    if (!player)
        return nullptr;

    uint32 const team = GetPlayerTeam(player->GetGUID()) ? GetPlayerTeam(player->GetGUID()) : player->GetTeam();

    if (GetStatus() != STATUS_IN_PROGRESS)
        return sWorldSafeLocsStore.LookupEntry(team == HORDE ? BG_TOK_GY_HORDE_START : BG_TOK_GY_ALLIANCE_START);

    WorldSafeLocsEntry const* first = sWorldSafeLocsStore.LookupEntry(team == HORDE ? BG_TOK_GY_HORDE_1 : BG_TOK_GY_ALLIANCE_1);
    WorldSafeLocsEntry const* second = sWorldSafeLocsStore.LookupEntry(team == HORDE ? BG_TOK_GY_HORDE_2 : BG_TOK_GY_ALLIANCE_2);
    if (!first || !second)
        return first ? first : second;

    float const firstDist = player->GetExactDist2dSq(first->Loc.X, first->Loc.Y);
    float const secondDist = player->GetExactDist2dSq(second->Loc.X, second->Loc.Y);
    return firstDist <= secondDist ? first : second;
}

void BattlegroundTOK::HandleKillPlayer(Player* victim, Player* killer)
{
    Battleground::HandleKillPlayer(victim, killer);

    if (GetStatus() != STATUS_IN_PROGRESS || !victim)
        return;

    // A dead carrier's orb goes home first, so a killing blow that also ends
    // the match does not leave an orb on a corpse.
    int32 const orb = FindOrbHeldBy(victim->GetGUID());
    if (orb >= 0)
        ReturnOrb(uint8(orb), victim, true);

    if (!killer || killer == victim)
        return;

    uint32 const killerTeam = GetPlayerTeam(killer->GetGUID());
    uint32 const victimTeam = GetPlayerTeam(victim->GetGUID());
    if (killerTeam && victimTeam && killerTeam != victimTeam)
        AddPoints(killerTeam, ScaleResourceGain(BG_TOK_POINTS_PER_KILL));
}

bool BattlegroundTOK::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    WorldSafeLocsEntry const* safeLoc = sWorldSafeLocsStore.LookupEntry(
        player->GetTeam() == HORDE ? BG_TOK_GY_HORDE_START : BG_TOK_GY_ALLIANCE_START);
    if (!safeLoc)
        return false;

    player->TeleportTo(GetMapId(), safeLoc->Loc.X, safeLoc->Loc.Y, safeLoc->Loc.Z + 1.0f, player->GetOrientation());
    return true;
}

void BattlegroundTOK::EndBattleground(uint32 winner)
{
    // Carriers keep nothing past the end of the match.
    for (uint8 orb = 0; orb < BG_TOK_ORB_COUNT; ++orb)
    {
        ObjectGuid const carrierGuid = _orbs[orb].Carrier;
        if (carrierGuid.IsEmpty())
            continue;

        if (Player* carrier = ObjectAccessor::FindPlayer(carrierGuid))
        {
            carrier->RemoveAurasDueToSpell(OrbHolderSpell(orb));
            carrier->RemoveAurasDueToSpell(OrbStackSpell(orb));
        }

        _orbs[orb] = OrbState();
    }

    UpdateScoreWorldStates();
    Battleground::EndBattleground(winner);
}
