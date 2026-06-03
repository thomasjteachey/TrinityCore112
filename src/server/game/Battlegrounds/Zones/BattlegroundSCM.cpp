#include "BattlegroundSCM.h"

#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include "WorldSession.h"

void BattlegroundSCMScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra custom scoreboard columns yet
}

BattlegroundSCM::BattlegroundSCM()
{
    BgObjects.resize(BG_SCM_OBJECT_MAX);
    BgCreatures.resize(BG_SCM_CREATURE_MAX);
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _usePrimaryGraveyard = true;
    _graveyardSwapTimer = 0;
    m_BuffChange = true;
}

void BattlegroundSCM::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    TrackHumanParticipantAdded(player, isInBattleground);

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundSCMScore(player->GetGUID(), scoreboardTeamMarker);
    }
}

void BattlegroundSCM::RemovePlayer(Player* player, ObjectGuid /*guid*/, uint32 team)
{
    AwardLeavePointIfNeeded(player, team);
    TrackHumanParticipantRemoved(player, team);
}

void BattlegroundSCM::Reset()
{
    Battleground::Reset();
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _usePrimaryGraveyard = true;
    _graveyardSwapTimer = 0;
}

void BattlegroundSCM::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    _graveyardSwapTimer += diff;
    if (_graveyardSwapTimer >= GetResurrectionInterval())
    {
        _graveyardSwapTimer = 0;
        _usePrimaryGraveyard = !_usePrimaryGraveyard;
    }
}

void BattlegroundSCM::TrackHumanParticipantAdded(Player const* player, bool isInBattleground)
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

void BattlegroundSCM::TrackHumanParticipantRemoved(Player const* player, uint32 team)
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

void BattlegroundSCM::UpdateHumanFaceoffState()
{
    if (_allianceHumanParticipants > 0 && _hordeHumanParticipants > 0)
        _humanFaceoffEverHappened = true;
}

uint32 BattlegroundSCM::GetHonorRewardForTeam() const
{
    uint32 reward = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) / 2;

    if (!_humanFaceoffEverHappened)
        reward /= 2;

    return reward;
}

void BattlegroundSCM::ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const
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

bool BattlegroundSCM::SetupBattleground()
{
    if (!AddObject(BG_SCM_OBJECT_STATUE_1, BG_SCM_OBJECT_STATUE_ENTRY,
            863.768f, 1384.96f, 19.5695f, 1.5568f,
            0.0f, 0.0f, -0.702142f, -0.712037f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_SCM_OBJECT_DOOR_ALLIANCE, BG_SCM_OBJECT_DOOR_ALLIANCE_ENTRY,
            880.31f, 1399.26f, 18.6765f, 6.24168f,
            0.0f, 0.0f, -0.020749f, 0.999785f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_SCM_OBJECT_DOOR_HORDE, BG_SCM_OBJECT_DOOR_HORDE_ENTRY,
            1069.95f, 1399.14f, 30.7956f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.000001f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_SCM_OBJECT_STATUE_2, BG_SCM_OBJECT_STATUE_ENTRY,
            1091.96f, 1398.61f, 30.3063f, 3.14726f,
            0.0f, 0.0f, -0.999996f, 0.00283472f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_SCM_OBJECT_STATUE_3, BG_SCM_OBJECT_STATUE_ENTRY,
            1095.32f, 1385.36f, 30.2991f, 1.59609f,
            0.0f, 0.0f, -0.715992f, -0.698108f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_SCM_OBJECT_STATUE_4, BG_SCM_OBJECT_STATUE_ENTRY,
            1095.3f, 1411.06f, 30.3003f, 4.69841f,
            0.0f, 0.0f, -0.712033f, 0.702146f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_SCM_OBJECT_BUFF1_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            939.852112f, 1400.462524f, 18.339478f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF1_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            939.852112f, 1400.462524f, 18.339478f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF1_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            939.852112f, 1400.462524f, 18.339478f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF2_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            997.415649f, 1399.062012f, 27.136366f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF2_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            997.415649f, 1399.062012f, 27.136366f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF2_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            997.415649f, 1399.062012f, 27.136366f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF3_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            963.067078f, 1360.810181f, 18.677767f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF3_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            963.067078f, 1360.810181f, 18.677767f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF3_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            963.067078f, 1360.810181f, 18.677767f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF4_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            986.732442f, 1454.590088f, 29.099308f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF4_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            986.732442f, 1454.590088f, 29.099308f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF4_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            986.732442f, 1454.590088f, 29.099308f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF5_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            963.067078f, 1437.870605f, 18.677767f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF5_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            963.067078f, 1437.870605f, 18.677767f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF5_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            963.067078f, 1437.870605f, 18.677767f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF6_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            987.244507f, 1343.777466f, 29.146633f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF6_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            987.244507f, 1343.777466f, 29.146633f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME)
        || !AddObject(BG_SCM_OBJECT_BUFF6_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            987.244507f, 1343.777466f, 29.146633f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_SCM_BUFF_RESPAWN_TIME))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundSCM::SetupBattleground: failed to spawn one or more Scarlet Chapel gameobjects.");
        return false;
    }

    WorldSafeLocsEntry const* allianceA = sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_ALLIANCE_A);
    WorldSafeLocsEntry const* allianceB = sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_ALLIANCE_B);
    WorldSafeLocsEntry const* hordeA = sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_HORDE_A);
    WorldSafeLocsEntry const* hordeB = sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_HORDE_B);

    if (!allianceA || !allianceB || !hordeA || !hordeB)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundSCM::SetupBattleground: one or more Scarlet Chapel graveyards are missing in WorldSafeLocs.");
        return false;
    }

    if (!AddSpiritGuide(BG_SCM_SPIRIT_ALLIANCE_A, allianceA->Loc.X, allianceA->Loc.Y, allianceA->Loc.Z, 0.0f, TEAM_ALLIANCE))
        return false;
    if (!AddSpiritGuide(BG_SCM_SPIRIT_ALLIANCE_B, allianceB->Loc.X, allianceB->Loc.Y, allianceB->Loc.Z, 0.0f, TEAM_ALLIANCE))
        return false;
    if (!AddSpiritGuide(BG_SCM_SPIRIT_HORDE_A, hordeA->Loc.X, hordeA->Loc.Y, hordeA->Loc.Z, 0.0f, TEAM_HORDE))
        return false;
    if (!AddSpiritGuide(BG_SCM_SPIRIT_HORDE_B, hordeB->Loc.X, hordeB->Loc.Y, hordeB->Loc.Z, 0.0f, TEAM_HORDE))
        return false;

    ApplyNonInteractableObjectFlags();
    return true;
}

void BattlegroundSCM::ApplyNonInteractableObjectFlags()
{
    for (uint32 type = BG_SCM_OBJECT_STATUE_1; type <= BG_SCM_OBJECT_STATUE_4; ++type)
        if (GameObject* statue = GetBGObject(type))
            statue->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* allianceDoor = GetBGObject(BG_SCM_OBJECT_DOOR_ALLIANCE))
        allianceDoor->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* hordeDoor = GetBGObject(BG_SCM_OBJECT_DOOR_HORDE))
        hordeDoor->SetFlag(GO_FLAG_NOT_SELECTABLE);
}

void BattlegroundSCM::SpawnRandomBuffSet(uint32 speedTypeIndex)
{
    // Despawn all three buff variants at this node first.
    SpawnBGObject(speedTypeIndex + 0, RESPAWN_ONE_DAY);
    SpawnBGObject(speedTypeIndex + 1, RESPAWN_ONE_DAY);
    SpawnBGObject(speedTypeIndex + 2, RESPAWN_ONE_DAY);

    uint8 const buff = urand(0, 2);
    SpawnBGObject(speedTypeIndex + buff, RESPAWN_IMMEDIATELY);
}

void BattlegroundSCM::StartingEventCloseDoors()
{
    for (uint32 type = BG_SCM_OBJECT_STATUE_1; type <= BG_SCM_OBJECT_STATUE_4; ++type)
        SpawnBGObject(type, RESPAWN_IMMEDIATELY);

    DoorClose(BG_SCM_OBJECT_DOOR_ALLIANCE);
    DoorClose(BG_SCM_OBJECT_DOOR_HORDE);
    SpawnBGObject(BG_SCM_OBJECT_DOOR_ALLIANCE, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_SCM_OBJECT_DOOR_HORDE, RESPAWN_IMMEDIATELY);

    // Hide all buff variants until battle start.
    for (uint32 type = BG_SCM_OBJECT_BUFF1_SPEED; type <= BG_SCM_OBJECT_BUFF6_BERSERK; ++type)
        SpawnBGObject(type, RESPAWN_ONE_DAY);

    ApplyNonInteractableObjectFlags();
}

void BattlegroundSCM::StartingEventOpenDoors()
{
    DoorOpen(BG_SCM_OBJECT_DOOR_ALLIANCE);
    DoorOpen(BG_SCM_OBJECT_DOOR_HORDE);

    SpawnRandomBuffSet(BG_SCM_OBJECT_BUFF1_SPEED);
    SpawnRandomBuffSet(BG_SCM_OBJECT_BUFF2_SPEED);
    SpawnRandomBuffSet(BG_SCM_OBJECT_BUFF3_SPEED);
    SpawnRandomBuffSet(BG_SCM_OBJECT_BUFF4_SPEED);
    SpawnRandomBuffSet(BG_SCM_OBJECT_BUFF5_SPEED);
    SpawnRandomBuffSet(BG_SCM_OBJECT_BUFF6_SPEED);

    ApplyNonInteractableObjectFlags();
}

WorldSafeLocsEntry const* BattlegroundSCM::GetCurrentTeamGraveyard(TeamId teamId) const
{
    if (teamId == TEAM_ALLIANCE)
        return _usePrimaryGraveyard ? sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_ALLIANCE_A)
                                    : sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_ALLIANCE_B);

    return _usePrimaryGraveyard ? sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_HORDE_A)
                                : sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_HORDE_B);
}

WorldSafeLocsEntry const* BattlegroundSCM::GetClosestGraveyard(Player* player)
{
    if (!player)
        return nullptr;

    if (player->GetTeam() == ALLIANCE)
    {
        if (GetStatus() == STATUS_IN_PROGRESS)
            return GetCurrentTeamGraveyard(TEAM_ALLIANCE);

        return sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_ALLIANCE_START);
    }

    if (GetStatus() == STATUS_IN_PROGRESS)
        return GetCurrentTeamGraveyard(TEAM_HORDE);

    return sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_HORDE_START);
}

void BattlegroundSCM::UpdateTeamScoreWorldStates()
{
    UpdateWorldState(BG_SCM_WORLDSTATE_SHOW, 1);
    UpdateWorldState(BG_SCM_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    UpdateWorldState(BG_SCM_WORLDSTATE_HORDE_SCORE, _hordeKills);
    UpdateWorldState(BG_SCM_WORLDSTATE_MAX_SCORE, BG_SCM_KILL_LIMIT);
    UpdateWorldState(BG_SCM_WORLDSTATE_MAX_KILLS_UI, BG_SCM_KILL_LIMIT);
}

void BattlegroundSCM::AwardPointToTeam(uint32 team)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (team == ALLIANCE)
    {
        ++_allianceKills;
        m_TeamScores[TEAM_ALLIANCE] = _allianceKills;

        if ((_allianceKills % 10) == 0)
            RewardHonorToTeam(GetHonorRewardForTeam(), ALLIANCE);
    }
    else if (team == HORDE)
    {
        ++_hordeKills;
        m_TeamScores[TEAM_HORDE] = _hordeKills;

        if ((_hordeKills % 10) == 0)
            RewardHonorToTeam(GetHonorRewardForTeam(), HORDE);
    }
    else
        return;

    UpdateTeamScoreWorldStates();

    if (_allianceKills >= BG_SCM_KILL_LIMIT)
        EndBattleground(ALLIANCE);
    else if (_hordeKills >= BG_SCM_KILL_LIMIT)
        EndBattleground(HORDE);
}

void BattlegroundSCM::AwardLeavePointIfNeeded(Player const* player, uint32 team)
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

    AwardPointToTeam(team == ALLIANCE ? HORDE : ALLIANCE);
}

void BattlegroundSCM::HandleKillPlayer(Player* victim, Player* killer)
{
    Battleground::HandleKillPlayer(victim, killer);

    if (GetStatus() != STATUS_IN_PROGRESS || !victim || !killer || victim == killer)
        return;

    uint32 killerTeam = GetPlayerTeam(killer->GetGUID());
    uint32 victimTeam = GetPlayerTeam(victim->GetGUID());

    if (!killerTeam || !victimTeam || killerTeam == victimTeam)
        return;

    AwardPointToTeam(killerTeam);
}

void BattlegroundSCM::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_SHOW, 1);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_HORDE_SCORE, _hordeKills);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_MAX_SCORE, BG_SCM_KILL_LIMIT);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_MAX_KILLS_UI, BG_SCM_KILL_LIMIT);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_TIMER_ACTIVE, 0);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_TIMER, 0);
}

bool BattlegroundSCM::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    WorldSafeLocsEntry const* safeLoc = nullptr;
    if (player->GetTeam() == ALLIANCE)
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_ALLIANCE_START);
    else
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_SCM_GY_HORDE_START);

    if (!safeLoc)
        return false;

    player->TeleportTo(GetMapId(), safeLoc->Loc.X, safeLoc->Loc.Y, safeLoc->Loc.Z + 1.0f, player->GetOrientation());
    return true;
}

void BattlegroundSCM::EndBattleground(uint32 winner)
{
    UpdateTeamScoreWorldStates();
    Battleground::EndBattleground(winner);
}

uint32 BattlegroundSCM::GetBuffRespawnTime(uint32 /*type*/) const
{
    return BG_SCM_BUFF_RESPAWN_TIME;
}
