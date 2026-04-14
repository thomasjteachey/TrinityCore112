/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "BattlegroundSCM.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "Util.h"
#include "WorldStatePackets.h"

BattlegroundSCM::BattlegroundSCM()
{
    BgCreatures.resize(BG_SCM_CREATURE_MAX);
}

void BattlegroundSCM::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);

    if (!isInBattleground)
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundSCMScore(player->GetGUID());
}

void BattlegroundSCM::Reset()
{
    Battleground::Reset();

    m_TeamScores[TEAM_ALLIANCE] = 0;
    m_TeamScores[TEAM_HORDE] = 0;
}

bool BattlegroundSCM::SetupBattleground()
{
    struct SpiritSpawn
    {
        uint32 Type;
        uint32 SafeLocId;
        TeamId Team;
        float Orientation;
    };

    SpiritSpawn const spawns[] =
    {
        { BG_SCM_SPIRIT_ALLIANCE_A, BG_SCM_GY_ALLIANCE_A, TEAM_ALLIANCE, 0.0f },
        { BG_SCM_SPIRIT_ALLIANCE_B, BG_SCM_GY_ALLIANCE_B, TEAM_ALLIANCE, 0.0f },
        { BG_SCM_SPIRIT_HORDE_A,    BG_SCM_GY_HORDE_A,    TEAM_HORDE,    3.1415927f },
        { BG_SCM_SPIRIT_HORDE_B,    BG_SCM_GY_HORDE_B,    TEAM_HORDE,    3.1415927f }
    };

    for (SpiritSpawn const& spawn : spawns)
    {
        WorldSafeLocsEntry const* safeLoc = sWorldSafeLocsStore.LookupEntry(spawn.SafeLocId);
        if (!safeLoc)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundSCM: Missing WorldSafeLocs entry {}.", spawn.SafeLocId);
            return false;
        }

        if (!AddSpiritGuide(spawn.Type, safeLoc->Loc.X, safeLoc->Loc.Y, safeLoc->Loc.Z, spawn.Orientation, spawn.Team))
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundSCM: Failed to spawn spirit guide {} at WorldSafeLocs {}.", spawn.Type, spawn.SafeLocId);
            return false;
        }
    }

    return true;
}

void BattlegroundSCM::StartingEventCloseDoors()
{
}

void BattlegroundSCM::StartingEventOpenDoors()
{
}

void BattlegroundSCM::HandleKillPlayer(Player* victim, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    Battleground::HandleKillPlayer(victim, killer);

    if (!killer || killer == victim)
        return;

    if (killer->GetBGTeam() == victim->GetBGTeam())
        return;

    TeamId const killerTeamIndex = GetTeamIndexByTeamId(killer->GetBGTeam());
    ++m_TeamScores[killerTeamIndex];

    UpdateWorldState(BG_SCM_WORLDSTATE_ALLIANCE_SCORE, m_TeamScores[TEAM_ALLIANCE]);
    UpdateWorldState(BG_SCM_WORLDSTATE_HORDE_SCORE, m_TeamScores[TEAM_HORDE]);

    if (m_TeamScores[killerTeamIndex] >= KillLimit)
        EndBattleground(killer->GetBGTeam());
}

WorldSafeLocsEntry const* BattlegroundSCM::GetTeamStartLoc(TeamId teamId) const
{
    return sWorldSafeLocsStore.LookupEntry(teamId == TEAM_ALLIANCE ? BG_SCM_START_ALLIANCE : BG_SCM_START_HORDE);
}

WorldSafeLocsEntry const* BattlegroundSCM::GetRandomTeamGraveyard(TeamId teamId) const
{
    uint32 graveyardId = 0;

    if (teamId == TEAM_ALLIANCE)
        graveyardId = urand(0, 1) == 0 ? BG_SCM_GY_ALLIANCE_A : BG_SCM_GY_ALLIANCE_B;
    else
        graveyardId = urand(0, 1) == 0 ? BG_SCM_GY_HORDE_A : BG_SCM_GY_HORDE_B;

    if (WorldSafeLocsEntry const* graveyard = sWorldSafeLocsStore.LookupEntry(graveyardId))
        return graveyard;

    return GetTeamStartLoc(teamId);
}

WorldSafeLocsEntry const* BattlegroundSCM::GetClosestGraveyard(Player* player)
{
    TeamId const teamId = GetTeamIndexByTeamId(player->GetBGTeam());

    if (GetStatus() != STATUS_IN_PROGRESS)
        return GetTeamStartLoc(teamId);

    return GetRandomTeamGraveyard(teamId);
}

void BattlegroundSCM::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_ALLIANCE_SCORE, m_TeamScores[TEAM_ALLIANCE]);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_HORDE_SCORE, m_TeamScores[TEAM_HORDE]);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_MAX_SCORE, KillLimit);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_TIMER_ACTIVE, 0);
    packet.Worldstates.emplace_back(BG_SCM_WORLDSTATE_TIMER_VALUE, 0);
}

bool BattlegroundSCM::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    WorldSafeLocsEntry const* safeLoc = GetClosestGraveyard(player);
    if (!safeLoc)
        return false;

    player->TeleportTo(GetMapId(), safeLoc->Loc.X, safeLoc->Loc.Y, safeLoc->Loc.Z, player->GetOrientation());
    return true;
}
