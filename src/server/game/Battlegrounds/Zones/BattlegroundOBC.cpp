#include "BattlegroundOBC.h"

#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include "WorldSession.h"

#include <cmath>

namespace
{
// Flag stand at the arena center
float constexpr OBC_FLAG_X = 3247.900391f;
float constexpr OBC_FLAG_Y = 536.422791f;
float constexpr OBC_FLAG_Z = 58.947762f;
float constexpr OBC_FLAG_O = 1.672456f;

// Objective light beam positions. The ALLIANCE base light is the capture
// point for a HORDE carrier and vice versa.
float constexpr OBC_LIGHT_ALLIANCE_X = 3157.859619f;
float constexpr OBC_LIGHT_ALLIANCE_Y = 515.362976f;
float constexpr OBC_LIGHT_ALLIANCE_Z = 91.482079f;

float constexpr OBC_LIGHT_HORDE_X = 3335.992676f;
float constexpr OBC_LIGHT_HORDE_Y = 529.767822f;
float constexpr OBC_LIGHT_HORDE_Z = 95.974312f;

struct ObcGateSpawn
{
    uint32 type;
    float x, y, z, o, rot2, rot3;
};

// Positions/rotations recorded from the hand-placed staging spawns on map
// 1615 (originally placed with .gobject add while designing the arena).
ObcGateSpawn constexpr OBC_GATES[] =
{
    { BG_OBC_OBJECT_GATE_H_1, 3381.17f, 527.089f, 96.0f, 1.79817f,   -0.782759f,  -0.622325f },
    { BG_OBC_OBJECT_GATE_H_2, 3394.19f, 514.102f, 96.0f, 6.10608f,   -0.088438f,   0.996082f },
    { BG_OBC_OBJECT_GATE_H_3, 3389.63f, 541.029f, 96.0f, 3.38076f,   -0.992858f,   0.1193f   },
    { BG_OBC_OBJECT_GATE_H_4, 3412.19f, 545.16f,  96.0f, 3.23546f,   -0.998899f,   0.0469143f },
    { BG_OBC_OBJECT_GATE_H_5, 3417.82f, 510.548f, 96.0f, 3.04698f,   -0.998881f,  -0.0472881f },
    { BG_OBC_OBJECT_GATE_A_1, 3113.13f, 517.42f,  86.0f, 3.29443f,   -0.997082f,   0.0763455f },
    { BG_OBC_OBJECT_GATE_A_2, 3125.66f, 504.82f,  86.0f, 1.62546f,   -0.726167f,  -0.687518f },
    { BG_OBC_OBJECT_GATE_A_3, 3114.91f, 492.19f,  86.0f, 0.0428774f, -0.0214372f, -0.99977f  },
    { BG_OBC_OBJECT_GATE_A_4, 3092.81f, 491.322f, 86.0f, 0.133222f,  -0.0665615f, -0.997782f }
};
}

void BattlegroundOBCScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra custom scoreboard columns yet
}

BattlegroundOBC::BattlegroundOBC()
{
    BgObjects.resize(BG_OBC_OBJECT_MAX);
    BgCreatures.resize(BG_OBC_CREATURE_MAX);
    _allianceScore = 0;
    _hordeScore = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _flagState = BG_OBC_FLAG_STATE_ON_BASE;
    _flagResetTimer = 0;
    _flagCarrierGuid.Clear();
    _droppedFlagGuid.Clear();
}

void BattlegroundOBC::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    TrackHumanParticipantAdded(player, isInBattleground);

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundOBCScore(player->GetGUID(), scoreboardTeamMarker);
    }
}

void BattlegroundOBC::RemovePlayer(Player* player, ObjectGuid guid, uint32 team)
{
    if (IsFlagPickedup() && _flagCarrierGuid == guid)
    {
        if (player)
            EventPlayerDroppedFlag(player);
        else
        {
            // Carrier is already gone (logout/disconnect): there is nobody to
            // drop a ground flag at, so put the flag straight back on base.
            SetFlagPicker(ObjectGuid::Empty);
            _flagState = BG_OBC_FLAG_STATE_ON_BASE;
            RespawnFlag();
            UpdateObjectiveLights();
        }
    }

    AwardLeavePointIfNeeded(player, team);
    TrackHumanParticipantRemoved(player, team);
}

void BattlegroundOBC::Reset()
{
    Battleground::Reset();
    _allianceScore = 0;
    _hordeScore = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _flagState = BG_OBC_FLAG_STATE_ON_BASE;
    _flagResetTimer = 0;
    _flagCarrierGuid.Clear();
    _droppedFlagGuid.Clear();
}

void BattlegroundOBC::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (_flagState == BG_OBC_FLAG_STATE_WAIT_RESPAWN || _flagState == BG_OBC_FLAG_STATE_ON_GROUND)
    {
        _flagResetTimer -= int32(diff);
        if (_flagResetTimer <= 0)
        {
            _flagResetTimer = 0;
            if (_flagState == BG_OBC_FLAG_STATE_WAIT_RESPAWN)
                RespawnFlag();
            else
                RespawnFlagAfterDrop();
        }
    }

    // Capture check: the carrier caps by physically reaching the lit enemy
    // base. No area trigger exists for these spots, so use a proximity check
    // against the objective light position.
    if (_flagState == BG_OBC_FLAG_STATE_ON_PLAYER)
    {
        Player* carrier = ObjectAccessor::FindPlayer(_flagCarrierGuid);
        if (carrier && carrier->IsAlive())
        {
            uint32 const carrierTeam = GetPlayerTeam(carrier->GetGUID());
            if (carrierTeam == HORDE &&
                carrier->IsWithinDist3d(OBC_LIGHT_ALLIANCE_X, OBC_LIGHT_ALLIANCE_Y, OBC_LIGHT_ALLIANCE_Z, BG_OBC_CAPTURE_RADIUS))
                EventPlayerCapturedFlag(carrier);
            else if (carrierTeam == ALLIANCE &&
                carrier->IsWithinDist3d(OBC_LIGHT_HORDE_X, OBC_LIGHT_HORDE_Y, OBC_LIGHT_HORDE_Z, BG_OBC_CAPTURE_RADIUS))
                EventPlayerCapturedFlag(carrier);
        }
    }
}

void BattlegroundOBC::TrackHumanParticipantAdded(Player const* player, bool isInBattleground)
{
    if (!player || isInBattleground)
        return;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsVirtualSession())
        return;

    if (player->GetBGTeam() == ALLIANCE)
        ++_allianceHumanParticipants;
    else if (player->GetBGTeam() == HORDE)
        ++_hordeHumanParticipants;

    UpdateHumanFaceoffState();
}

void BattlegroundOBC::TrackHumanParticipantRemoved(Player const* player, uint32 team)
{
    if (!player)
        return;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsVirtualSession())
        return;

    if (team == ALLIANCE && _allianceHumanParticipants > 0)
        --_allianceHumanParticipants;
    else if (team == HORDE && _hordeHumanParticipants > 0)
        --_hordeHumanParticipants;
}

void BattlegroundOBC::UpdateHumanFaceoffState()
{
    if (_allianceHumanParticipants > 0 && _hordeHumanParticipants > 0)
        _humanFaceoffEverHappened = true;
}

uint32 BattlegroundOBC::GetHonorRewardForTeam() const
{
    uint32 reward = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) / 2;

    if (!_humanFaceoffEverHappened)
        reward /= 2;

    return reward;
}

void BattlegroundOBC::ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const
{
    if (winner != ALLIANCE && winner != HORDE)
        return;

    if (!_humanFaceoffEverHappened)
    {
        winnerHonor /= 2;
        loserHonor /= 2;
    }

    winnerHonor = (winnerHonor * 3.5) / 5;
    loserHonor = (loserHonor * 3.5) / 5;
}

bool BattlegroundOBC::SetupBattleground()
{
    for (ObcGateSpawn const& gate : OBC_GATES)
    {
        if (!AddObject(gate.type, BG_OBC_GATE_ENTRY,
                gate.x, gate.y, gate.z, gate.o,
                0.0f, 0.0f, gate.rot2, gate.rot3,
                RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundOBC::SetupBattleground: failed to spawn start gate (type {}).", gate.type);
            return false;
        }
    }

    if (!AddObject(BG_OBC_OBJECT_FLAG, BG_OBC_FLAG_STAND_ENTRY,
            OBC_FLAG_X, OBC_FLAG_Y, OBC_FLAG_Z, OBC_FLAG_O,
            0.0f, 0.0f, std::sin(OBC_FLAG_O / 2.0f), std::cos(OBC_FLAG_O / 2.0f),
            RESPAWN_ONE_DAY)
        || !AddObject(BG_OBC_OBJECT_LIGHT_ALLIANCE_BASE, BG_OBC_LIGHT_BEAM_ENTRY,
            OBC_LIGHT_ALLIANCE_X, OBC_LIGHT_ALLIANCE_Y, OBC_LIGHT_ALLIANCE_Z, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            RESPAWN_ONE_DAY)
        || !AddObject(BG_OBC_OBJECT_LIGHT_HORDE_BASE, BG_OBC_LIGHT_BEAM_ENTRY,
            OBC_LIGHT_HORDE_X, OBC_LIGHT_HORDE_Y, OBC_LIGHT_HORDE_Z, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            RESPAWN_ONE_DAY))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundOBC::SetupBattleground: failed to spawn flag/light objects.");
        return false;
    }

    WorldSafeLocsEntry const* allianceGy = sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_ALLIANCE);
    WorldSafeLocsEntry const* hordeGy = sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_HORDE);

    if (!allianceGy || !hordeGy)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundOBC::SetupBattleground: one or more Obsidian Colosseum graveyards are missing in WorldSafeLocs.");
        return false;
    }

    if (!AddSpiritGuide(BG_OBC_SPIRIT_ALLIANCE, allianceGy->Loc.X, allianceGy->Loc.Y, allianceGy->Loc.Z, 0.0f, TEAM_ALLIANCE))
        return false;
    if (!AddSpiritGuide(BG_OBC_SPIRIT_HORDE, hordeGy->Loc.X, hordeGy->Loc.Y, hordeGy->Loc.Z, 0.0f, TEAM_HORDE))
        return false;

    ApplyNonInteractableObjectFlags();
    return true;
}

void BattlegroundOBC::ApplyNonInteractableObjectFlags()
{
    // The flag stand must stay clickable; everything else is scenery.
    for (uint32 type = BG_OBC_OBJECT_GATE_H_1; type <= BG_OBC_OBJECT_GATE_A_4; ++type)
        if (GameObject* gate = GetBGObject(type))
            gate->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* allianceLight = GetBGObject(BG_OBC_OBJECT_LIGHT_ALLIANCE_BASE))
        allianceLight->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* hordeLight = GetBGObject(BG_OBC_OBJECT_LIGHT_HORDE_BASE))
        hordeLight->SetFlag(GO_FLAG_NOT_SELECTABLE);
}

void BattlegroundOBC::StartingEventCloseDoors()
{
    for (uint32 type = BG_OBC_OBJECT_GATE_H_1; type <= BG_OBC_OBJECT_GATE_A_4; ++type)
    {
        DoorClose(type);
        SpawnBGObject(type, RESPAWN_IMMEDIATELY);
    }

    // Flag and objective lights stay hidden until the match starts.
    SpawnBGObject(BG_OBC_OBJECT_FLAG, RESPAWN_ONE_DAY);
    SpawnBGObject(BG_OBC_OBJECT_LIGHT_ALLIANCE_BASE, RESPAWN_ONE_DAY);
    SpawnBGObject(BG_OBC_OBJECT_LIGHT_HORDE_BASE, RESPAWN_ONE_DAY);

    ApplyNonInteractableObjectFlags();
}

void BattlegroundOBC::StartingEventOpenDoors()
{
    for (uint32 type = BG_OBC_OBJECT_GATE_H_1; type <= BG_OBC_OBJECT_GATE_A_4; ++type)
        DoorOpen(type);

    _flagState = BG_OBC_FLAG_STATE_ON_BASE;
    SpawnBGObject(BG_OBC_OBJECT_FLAG, RESPAWN_IMMEDIATELY);

    ApplyNonInteractableObjectFlags();
}

void BattlegroundOBC::UpdateObjectiveLights()
{
    uint32 carrierTeam = 0;
    if (_flagState == BG_OBC_FLAG_STATE_ON_PLAYER && !_flagCarrierGuid.IsEmpty())
        carrierTeam = GetPlayerTeam(_flagCarrierGuid);

    // The light marks the base the carrier has to reach: horde carrier
    // lights the alliance base, alliance carrier lights the horde base.
    // Exactly one light (or none) is ever visible.
    if (carrierTeam == HORDE)
    {
        SpawnBGObject(BG_OBC_OBJECT_LIGHT_ALLIANCE_BASE, RESPAWN_IMMEDIATELY);
        SpawnBGObject(BG_OBC_OBJECT_LIGHT_HORDE_BASE, RESPAWN_ONE_DAY);
    }
    else if (carrierTeam == ALLIANCE)
    {
        SpawnBGObject(BG_OBC_OBJECT_LIGHT_HORDE_BASE, RESPAWN_IMMEDIATELY);
        SpawnBGObject(BG_OBC_OBJECT_LIGHT_ALLIANCE_BASE, RESPAWN_ONE_DAY);
    }
    else
    {
        SpawnBGObject(BG_OBC_OBJECT_LIGHT_ALLIANCE_BASE, RESPAWN_ONE_DAY);
        SpawnBGObject(BG_OBC_OBJECT_LIGHT_HORDE_BASE, RESPAWN_ONE_DAY);
    }

    ApplyNonInteractableObjectFlags();
}

void BattlegroundOBC::RespawnFlag()
{
    _flagState = BG_OBC_FLAG_STATE_ON_BASE;
    SpawnBGObject(BG_OBC_OBJECT_FLAG, RESPAWN_IMMEDIATELY);
    PlaySoundToAll(BG_OBC_SOUND_FLAG_RESET);
    SendBroadcastText(BG_OBC_TEXT_FLAG_RESET, CHAT_MSG_BG_SYSTEM_NEUTRAL);
}

void BattlegroundOBC::RespawnFlagAfterDrop()
{
    RespawnFlag();

    if (!_droppedFlagGuid.IsEmpty())
    {
        if (GameObject* obj = GetBgMap()->GetGameObject(_droppedFlagGuid))
            obj->Delete();

        _droppedFlagGuid.Clear();
    }
}

void BattlegroundOBC::EventPlayerClickedOnFlag(Player* player, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !player || !target_obj)
        return;

    if (IsFlagPickedup() || !player->IsWithinDistInMap(target_obj, 10))
        return;

    bool const isBaseFlag = target_obj->GetGUID() == BgObjects[BG_OBC_OBJECT_FLAG];
    bool const isDroppedFlag = target_obj->GetEntry() == BG_OBC_FLAG_DROP_ENTRY;
    if (!isBaseFlag && !isDroppedFlag)
        return;

    if (isBaseFlag && _flagState != BG_OBC_FLAG_STATE_ON_BASE)
        return;
    if (isDroppedFlag && _flagState != BG_OBC_FLAG_STATE_ON_GROUND)
        return;

    if (isBaseFlag)
        SpawnBGObject(BG_OBC_OBJECT_FLAG, RESPAWN_ONE_DAY);
    else
    {
        // The FLAGDROP click handler in GameObject::Use deletes the ground
        // object after this callback returns; just forget its guid.
        _droppedFlagGuid.Clear();
        _flagResetTimer = 0;
    }

    _flagState = BG_OBC_FLAG_STATE_ON_PLAYER;
    SetFlagPicker(player->GetGUID());
    player->CastSpell(player, BG_OBC_NETHERSTORM_FLAG_SPELL, true);
    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);

    UpdateObjectiveLights();

    if (player->GetTeam() == ALLIANCE)
    {
        PlaySoundToAll(BG_OBC_SOUND_FLAG_PICKED_UP_ALLIANCE);
        SendBroadcastText(BG_OBC_TEXT_TAKEN_FLAG, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
    }
    else
    {
        PlaySoundToAll(BG_OBC_SOUND_FLAG_PICKED_UP_HORDE);
        SendBroadcastText(BG_OBC_TEXT_TAKEN_FLAG, CHAT_MSG_BG_SYSTEM_HORDE, player);
    }
}

void BattlegroundOBC::EventPlayerDroppedFlag(Player* player)
{
    if (!player)
        return;

    if (GetStatus() != STATUS_IN_PROGRESS)
    {
        // Not running: just strip carrier state without messages or ground flags.
        if (IsFlagPickedup() && GetFlagPickerGUID() == player->GetGUID())
        {
            SetFlagPicker(ObjectGuid::Empty);
            player->RemoveAurasDueToSpell(BG_OBC_NETHERSTORM_FLAG_SPELL);
        }
        return;
    }

    if (!IsFlagPickedup() || GetFlagPickerGUID() != player->GetGUID())
        return;

    SetFlagPicker(ObjectGuid::Empty);
    player->RemoveAurasDueToSpell(BG_OBC_NETHERSTORM_FLAG_SPELL);
    _flagState = BG_OBC_FLAG_STATE_ON_GROUND;
    _flagResetTimer = BG_OBC_FLAG_RESPAWN_TIME;

    // Deliberately NO SPELL_RECENTLY_DROPPED_FLAG and no pickup-lockout
    // debuff: the dropper is allowed to grab the flag right back up. 34991
    // only summons the clickable ground flag (184142) at the drop spot.
    player->CastSpell(player, BG_OBC_PLAYER_DROPPED_FLAG_SPELL, true);

    // Both objective lights turn off while nobody is carrying.
    UpdateObjectiveLights();

    if (player->GetTeam() == ALLIANCE)
        SendBroadcastText(BG_OBC_TEXT_FLAG_DROPPED, CHAT_MSG_BG_SYSTEM_ALLIANCE);
    else
        SendBroadcastText(BG_OBC_TEXT_FLAG_DROPPED, CHAT_MSG_BG_SYSTEM_HORDE);
}

void BattlegroundOBC::EventPlayerCapturedFlag(Player* player)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !player || GetFlagPickerGUID() != player->GetGUID())
        return;

    SetFlagPicker(ObjectGuid::Empty);
    player->RemoveAurasDueToSpell(BG_OBC_NETHERSTORM_FLAG_SPELL);
    _flagState = BG_OBC_FLAG_STATE_WAIT_RESPAWN;
    _flagResetTimer = BG_OBC_FLAG_RESPAWN_TIME;

    uint32 const team = GetPlayerTeam(player->GetGUID());
    if (team == ALLIANCE)
    {
        PlaySoundToAll(BG_OBC_SOUND_FLAG_CAPTURED_ALLIANCE);
        SendBroadcastText(BG_OBC_TEXT_ALLIANCE_CAPTURED_FLAG, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
    }
    else
    {
        PlaySoundToAll(BG_OBC_SOUND_FLAG_CAPTURED_HORDE);
        SendBroadcastText(BG_OBC_TEXT_HORDE_CAPTURED_FLAG, CHAT_MSG_BG_SYSTEM_HORDE, player);
    }

    // Both lights despawn on a capture.
    UpdateObjectiveLights();

    AwardPointsToTeam(team, BG_OBC_POINTS_PER_CAPTURE);
}

WorldSafeLocsEntry const* BattlegroundOBC::GetClosestGraveyard(Player* player)
{
    if (!player)
        return nullptr;

    if (player->GetTeam() == ALLIANCE)
    {
        if (GetStatus() == STATUS_IN_PROGRESS)
            return sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_ALLIANCE);

        return sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_ALLIANCE_START);
    }

    if (GetStatus() == STATUS_IN_PROGRESS)
        return sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_HORDE);

    return sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_HORDE_START);
}

void BattlegroundOBC::UpdateTeamScoreWorldStates()
{
    UpdateWorldState(BG_OBC_WORLDSTATE_SHOW, 1);
    UpdateWorldState(BG_OBC_WORLDSTATE_ALLIANCE_SCORE, _allianceScore);
    UpdateWorldState(BG_OBC_WORLDSTATE_HORDE_SCORE, _hordeScore);
    UpdateWorldState(BG_OBC_WORLDSTATE_MAX_SCORE, BG_OBC_SCORE_LIMIT);
    UpdateWorldState(BG_OBC_WORLDSTATE_MAX_SCORE_UI, BG_OBC_SCORE_LIMIT);
}

void BattlegroundOBC::AwardPointsToTeam(uint32 team, uint32 points)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !points)
        return;

    if (team == ALLIANCE)
    {
        uint32 const previousScore = _allianceScore;
        _allianceScore += points;
        m_TeamScores[TEAM_ALLIANCE] = _allianceScore;

        if ((previousScore / 10) != (_allianceScore / 10))
            RewardHonorToTeam(GetHonorRewardForTeam(), ALLIANCE);
    }
    else if (team == HORDE)
    {
        uint32 const previousScore = _hordeScore;
        _hordeScore += points;
        m_TeamScores[TEAM_HORDE] = _hordeScore;

        if ((previousScore / 10) != (_hordeScore / 10))
            RewardHonorToTeam(GetHonorRewardForTeam(), HORDE);
    }
    else
        return;

    UpdateTeamScoreWorldStates();

    if (_allianceScore >= BG_OBC_SCORE_LIMIT)
        EndBattleground(ALLIANCE);
    else if (_hordeScore >= BG_OBC_SCORE_LIMIT)
        EndBattleground(HORDE);
}

void BattlegroundOBC::AwardLeavePointIfNeeded(Player const* player, uint32 team)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !player || (team != ALLIANCE && team != HORDE))
        return;

    WorldSession const* session = player->GetSession();
    if (!session)
        return;

    bool const isHumanLeave = !session->IsVirtualSession();
    bool const isReportedBotLeave = session->IsVirtualSession() && (player->HasAura(43680) || player->HasAura(SPELL_AURA_PLAYER_INACTIVE));
    if (!isHumanLeave && !isReportedBotLeave)
        return;

    AwardPointsToTeam(team == ALLIANCE ? HORDE : ALLIANCE, BG_OBC_POINTS_PER_KILL);
}

void BattlegroundOBC::HandleKillPlayer(Player* victim, Player* killer)
{
    Battleground::HandleKillPlayer(victim, killer);

    if (GetStatus() != STATUS_IN_PROGRESS || !victim)
        return;

    // A dying carrier drops the flag regardless of who killed them (falling
    // damage, same-team weirdness, etc.).
    EventPlayerDroppedFlag(victim);

    if (!killer || victim == killer)
        return;

    uint32 const killerTeam = GetPlayerTeam(killer->GetGUID());
    uint32 const victimTeam = GetPlayerTeam(victim->GetGUID());

    if (!killerTeam || !victimTeam || killerTeam == victimTeam)
        return;

    AwardPointsToTeam(killerTeam, BG_OBC_POINTS_PER_KILL);
}

void BattlegroundOBC::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_OBC_WORLDSTATE_SHOW, 1);
    packet.Worldstates.emplace_back(BG_OBC_WORLDSTATE_ALLIANCE_SCORE, _allianceScore);
    packet.Worldstates.emplace_back(BG_OBC_WORLDSTATE_HORDE_SCORE, _hordeScore);
    packet.Worldstates.emplace_back(BG_OBC_WORLDSTATE_MAX_SCORE, BG_OBC_SCORE_LIMIT);
    packet.Worldstates.emplace_back(BG_OBC_WORLDSTATE_MAX_SCORE_UI, BG_OBC_SCORE_LIMIT);
    packet.Worldstates.emplace_back(BG_OBC_WORLDSTATE_TIMER_ACTIVE, 0);
    packet.Worldstates.emplace_back(BG_OBC_WORLDSTATE_TIMER, 0);
}

bool BattlegroundOBC::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    WorldSafeLocsEntry const* safeLoc = nullptr;
    if (player->GetTeam() == ALLIANCE)
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_ALLIANCE_START);
    else
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_OBC_GY_HORDE_START);

    if (!safeLoc)
        return false;

    player->TeleportTo(GetMapId(), safeLoc->Loc.X, safeLoc->Loc.Y, safeLoc->Loc.Z + 1.0f, player->GetOrientation());
    return true;
}

void BattlegroundOBC::EndBattleground(uint32 winner)
{
    UpdateTeamScoreWorldStates();
    Battleground::EndBattleground(winner);
}
