#include "BattlegroundBRT.h"

#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include "WorldSession.h"

void BattlegroundBRTScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra custom scoreboard columns yet
}

BattlegroundBRT::BattlegroundBRT()
{
    BgObjects.resize(BG_BRT_OBJECT_MAX);
    BgCreatures.resize(BG_BRT_CREATURE_MAX);
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _usePrimaryGraveyard = true;
    _graveyardSwapTimer = 0;
    m_BuffChange = true;
}

void BattlegroundBRT::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    TrackHumanParticipantAdded(player, isInBattleground);

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundBRTScore(player->GetGUID(), scoreboardTeamMarker);
    }
}

void BattlegroundBRT::RemovePlayer(Player* player, ObjectGuid /*guid*/, uint32 team)
{
    TrackHumanParticipantRemoved(player, team);
}

void BattlegroundBRT::Reset()
{
    Battleground::Reset();
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _usePrimaryGraveyard = true;
    _graveyardSwapTimer = 0;
    m_BuffChange = true;
}

void BattlegroundBRT::PostUpdateImpl(uint32 diff)
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

void BattlegroundBRT::TrackHumanParticipantAdded(Player const* player, bool isInBattleground)
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

void BattlegroundBRT::TrackHumanParticipantRemoved(Player const* player, uint32 team)
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

void BattlegroundBRT::UpdateHumanFaceoffState()
{
    if (_allianceHumanParticipants > 0 && _hordeHumanParticipants > 0)
        _humanFaceoffEverHappened = true;
}

uint32 BattlegroundBRT::GetHonorRewardForTeam() const
{
    uint32 reward = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) / 2;

    if (!_humanFaceoffEverHappened)
        reward /= 2;

    return reward;
}

void BattlegroundBRT::ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const
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

bool BattlegroundBRT::SetupBattleground()
{
    // Alliance gates: orientation/rotation were not provided with the placement logs, so these use identity rotation as a first pass.
    if (!AddObject(BG_BRT_OBJECT_ALLIANCE_GATE_LEFT, BG_BRT_OBJECT_ALLIANCE_GATE_ENTRY,
            1368.356323f, -826.367615f, -91.981422f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_BRT_OBJECT_ALLIANCE_GATE_RIGHT, BG_BRT_OBJECT_ALLIANCE_GATE_ENTRY,
            1389.290405f, -826.072998f, -92.415840f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_BRT_OBJECT_HORDE_GATE, BG_BRT_OBJECT_HORDE_GATE_ENTRY,
            1380.119995f, -710.481995f, -92.009300f, -1.5708f,
            0.0f, 0.0f, -0.707108f, 0.707106f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_BRT_OBJECT_GHOST_WALL_LEFT, BG_BRT_OBJECT_GHOST_WALL_ENTRY,
            1372.210693f, -687.344177f, -92.055161f, 4.71634f,
            0.0f, 0.0f, 0.7057085f, -0.7085023f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_BRT_OBJECT_GHOST_WALL_RIGHT, BG_BRT_OBJECT_GHOST_WALL_ENTRY,
            1390.874756f, -687.270325f, -92.055161f, 4.71634f,
            0.0f, 0.0f, 0.7057085f, -0.7085023f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_BRT_OBJECT_IMPERIAL_THRONE, BG_BRT_OBJECT_IMPERIAL_THRONE_ENTRY,
            1380.52f, -834.296f, -86.6783f, 1.5708f,
            0.0f, 0.0f, 0.707108f, 0.707106f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_BRT_OBJECT_BUFF1_VARIANT_A, BG_OBJECTID_REGENBUFF_ENTRY,
            1356.120850f, -726.623169f, -91.981697f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF1_VARIANT_B, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            1356.120850f, -726.623169f, -91.981697f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF1_VARIANT_C, BG_OBJECTID_REGENBUFF_ENTRY,
            1356.120850f, -726.623169f, -91.981697f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF2_VARIANT_A, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            1404.745361f, -726.598328f, -91.981483f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF2_VARIANT_B, BG_OBJECTID_REGENBUFF_ENTRY,
            1404.745361f, -726.598328f, -91.981483f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF2_VARIANT_C, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            1404.745361f, -726.598328f, -91.981483f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF3_VARIANT_A, BG_OBJECTID_REGENBUFF_ENTRY,
            1356.383423f, -818.263794f, -91.981094f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF3_VARIANT_B, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            1356.383423f, -818.263794f, -91.981094f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF3_VARIANT_C, BG_OBJECTID_REGENBUFF_ENTRY,
            1356.383423f, -818.263794f, -91.981094f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF4_VARIANT_A, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            1404.938232f, -818.510925f, -91.981621f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF4_VARIANT_B, BG_OBJECTID_REGENBUFF_ENTRY,
            1404.938232f, -818.510925f, -91.981621f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_BRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_BRT_OBJECT_BUFF4_VARIANT_C, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            1404.938232f, -818.510925f, -91.981621f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            BG_BRT_BUFF_RESPAWN_TIME))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundBRT::SetupBattleground: failed to spawn one or more Blackrock Throne battleground objects.");
        return false;
    }

    WorldSafeLocsEntry const* allianceA = sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_ALLIANCE_A);
    WorldSafeLocsEntry const* allianceB = sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_ALLIANCE_B);
    WorldSafeLocsEntry const* hordeA = sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_HORDE_A);
    WorldSafeLocsEntry const* hordeB = sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_HORDE_B);

    if (!allianceA || !allianceB || !hordeA || !hordeB)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundBRT::SetupBattleground: one or more Blackrock Throne graveyards are missing in WorldSafeLocs.");
        return false;
    }

    if (!AddSpiritGuide(BG_BRT_SPIRIT_ALLIANCE_A, allianceA->Loc.X, allianceA->Loc.Y, allianceA->Loc.Z, 0.0f, TEAM_ALLIANCE))
        return false;
    if (!AddSpiritGuide(BG_BRT_SPIRIT_ALLIANCE_B, allianceB->Loc.X, allianceB->Loc.Y, allianceB->Loc.Z, 0.0f, TEAM_ALLIANCE))
        return false;
    if (!AddSpiritGuide(BG_BRT_SPIRIT_HORDE_A, hordeA->Loc.X, hordeA->Loc.Y, hordeA->Loc.Z, 0.0f, TEAM_HORDE))
        return false;
    if (!AddSpiritGuide(BG_BRT_SPIRIT_HORDE_B, hordeB->Loc.X, hordeB->Loc.Y, hordeB->Loc.Z, 0.0f, TEAM_HORDE))
        return false;

    ApplyNonInteractableObjectFlags();
    return true;
}

void BattlegroundBRT::ApplyNonInteractableObjectFlags()
{
    if (GameObject* allianceGateLeft = GetBGObject(BG_BRT_OBJECT_ALLIANCE_GATE_LEFT))
        allianceGateLeft->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* allianceGateRight = GetBGObject(BG_BRT_OBJECT_ALLIANCE_GATE_RIGHT))
        allianceGateRight->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* hordeGate = GetBGObject(BG_BRT_OBJECT_HORDE_GATE))
        hordeGate->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* ghostWallLeft = GetBGObject(BG_BRT_OBJECT_GHOST_WALL_LEFT))
        ghostWallLeft->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* ghostWallRight = GetBGObject(BG_BRT_OBJECT_GHOST_WALL_RIGHT))
        ghostWallRight->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* imperialThrone = GetBGObject(BG_BRT_OBJECT_IMPERIAL_THRONE))
        imperialThrone->SetFlag(GO_FLAG_NOT_SELECTABLE);
}

void BattlegroundBRT::SpawnRandomBuffSet(uint32 variantAIndex)
{
    SpawnBGObject(variantAIndex + 0, RESPAWN_ONE_DAY);
    SpawnBGObject(variantAIndex + 1, RESPAWN_ONE_DAY);
    SpawnBGObject(variantAIndex + 2, RESPAWN_ONE_DAY);

    uint8 const buff = urand(0, 2);
    SpawnBGObject(variantAIndex + buff, RESPAWN_IMMEDIATELY);
}

void BattlegroundBRT::StartingEventCloseDoors()
{
    DoorClose(BG_BRT_OBJECT_ALLIANCE_GATE_LEFT);
    DoorClose(BG_BRT_OBJECT_ALLIANCE_GATE_RIGHT);
    DoorClose(BG_BRT_OBJECT_HORDE_GATE);

    SpawnBGObject(BG_BRT_OBJECT_ALLIANCE_GATE_LEFT, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_BRT_OBJECT_ALLIANCE_GATE_RIGHT, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_BRT_OBJECT_HORDE_GATE, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_BRT_OBJECT_GHOST_WALL_LEFT, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_BRT_OBJECT_GHOST_WALL_RIGHT, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_BRT_OBJECT_IMPERIAL_THRONE, RESPAWN_IMMEDIATELY);

    for (uint32 type = BG_BRT_OBJECT_BUFF1_VARIANT_A; type <= BG_BRT_OBJECT_BUFF4_VARIANT_C; ++type)
        SpawnBGObject(type, RESPAWN_ONE_DAY);

    ApplyNonInteractableObjectFlags();
}

void BattlegroundBRT::StartingEventOpenDoors()
{
    DoorOpen(BG_BRT_OBJECT_ALLIANCE_GATE_LEFT);
    DoorOpen(BG_BRT_OBJECT_ALLIANCE_GATE_RIGHT);
    DoorOpen(BG_BRT_OBJECT_HORDE_GATE);

    SpawnRandomBuffSet(BG_BRT_OBJECT_BUFF1_VARIANT_A);
    SpawnRandomBuffSet(BG_BRT_OBJECT_BUFF2_VARIANT_A);
    SpawnRandomBuffSet(BG_BRT_OBJECT_BUFF3_VARIANT_A);
    SpawnRandomBuffSet(BG_BRT_OBJECT_BUFF4_VARIANT_A);

    ApplyNonInteractableObjectFlags();
}

WorldSafeLocsEntry const* BattlegroundBRT::GetCurrentTeamGraveyard(TeamId teamId) const
{
    if (teamId == TEAM_ALLIANCE)
        return _usePrimaryGraveyard ? sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_ALLIANCE_A)
                                    : sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_ALLIANCE_B);

    return _usePrimaryGraveyard ? sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_HORDE_A)
                                : sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_HORDE_B);
}

WorldSafeLocsEntry const* BattlegroundBRT::GetClosestGraveyard(Player* player)
{
    if (!player)
        return nullptr;

    if (player->GetTeam() == ALLIANCE)
    {
        if (GetStatus() == STATUS_IN_PROGRESS)
            return GetCurrentTeamGraveyard(TEAM_ALLIANCE);

        return sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_ALLIANCE_START);
    }

    if (GetStatus() == STATUS_IN_PROGRESS)
        return GetCurrentTeamGraveyard(TEAM_HORDE);

    return sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_HORDE_START);
}

void BattlegroundBRT::UpdateTeamScoreWorldStates()
{
    UpdateWorldState(BG_BRT_WORLDSTATE_SHOW, 1);
    UpdateWorldState(BG_BRT_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    UpdateWorldState(BG_BRT_WORLDSTATE_HORDE_SCORE, _hordeKills);
    UpdateWorldState(BG_BRT_WORLDSTATE_MAX_SCORE, BG_BRT_KILL_LIMIT);
    UpdateWorldState(BG_BRT_WORLDSTATE_MAX_KILLS_UI, BG_BRT_KILL_LIMIT);
}

void BattlegroundBRT::HandleKillPlayer(Player* victim, Player* killer)
{
    Battleground::HandleKillPlayer(victim, killer);

    if (GetStatus() != STATUS_IN_PROGRESS || !victim || !killer || victim == killer)
        return;

    uint32 killerTeam = GetPlayerTeam(killer->GetGUID());
    uint32 victimTeam = GetPlayerTeam(victim->GetGUID());

    if (!killerTeam || !victimTeam || killerTeam == victimTeam)
        return;

    if (killerTeam == ALLIANCE)
    {
        ++_allianceKills;
        m_TeamScores[TEAM_ALLIANCE] = _allianceKills;

        if ((_allianceKills % 10) == 0)
            RewardHonorToTeam(GetHonorRewardForTeam(), ALLIANCE);
    }
    else if (killerTeam == HORDE)
    {
        ++_hordeKills;
        m_TeamScores[TEAM_HORDE] = _hordeKills;

        if ((_hordeKills % 10) == 0)
            RewardHonorToTeam(GetHonorRewardForTeam(), HORDE);
    }

    UpdateTeamScoreWorldStates();

    if (_allianceKills >= BG_BRT_KILL_LIMIT)
        EndBattleground(ALLIANCE);
    else if (_hordeKills >= BG_BRT_KILL_LIMIT)
        EndBattleground(HORDE);
}

void BattlegroundBRT::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_BRT_WORLDSTATE_SHOW, 1);
    packet.Worldstates.emplace_back(BG_BRT_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    packet.Worldstates.emplace_back(BG_BRT_WORLDSTATE_HORDE_SCORE, _hordeKills);
    packet.Worldstates.emplace_back(BG_BRT_WORLDSTATE_MAX_SCORE, BG_BRT_KILL_LIMIT);
    packet.Worldstates.emplace_back(BG_BRT_WORLDSTATE_MAX_KILLS_UI, BG_BRT_KILL_LIMIT);
    packet.Worldstates.emplace_back(BG_BRT_WORLDSTATE_TIMER_ACTIVE, 0);
    packet.Worldstates.emplace_back(BG_BRT_WORLDSTATE_TIMER, 0);
}


uint32 BattlegroundBRT::GetBuffRespawnTime(uint32 /*type*/) const
{
    return BG_BRT_BUFF_RESPAWN_TIME;
}

bool BattlegroundBRT::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    WorldSafeLocsEntry const* safeLoc = nullptr;
    if (player->GetTeam() == ALLIANCE)
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_ALLIANCE_START);
    else
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_BRT_GY_HORDE_START);

    if (!safeLoc)
        return false;

    player->TeleportTo(GetMapId(), safeLoc->Loc.X, safeLoc->Loc.Y, safeLoc->Loc.Z + 1.0f, player->GetOrientation());
    return true;
}

void BattlegroundBRT::EndBattleground(uint32 winner)
{
    UpdateTeamScoreWorldStates();
    Battleground::EndBattleground(winner);
}
