#include "BattlegroundTRT.h"

#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include "WorldSession.h"

void BattlegroundTRTScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra custom scoreboard columns yet
}

BattlegroundTRT::BattlegroundTRT()
{
    BgObjects.resize(BG_TRT_OBJECT_MAX);
    BgCreatures.resize(BG_TRT_CREATURE_MAX);
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _boundsCheckTimer = 0;
    m_BuffChange = true;
}

void BattlegroundTRT::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    TrackHumanParticipantAdded(player, isInBattleground);

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundTRTScore(player->GetGUID(), scoreboardTeamMarker);
    }
}

void BattlegroundTRT::RemovePlayer(Player* player, ObjectGuid /*guid*/, uint32 team)
{
    AwardLeavePointIfNeeded(player, team);
    TrackHumanParticipantRemoved(player, team);
}

void BattlegroundTRT::Reset()
{
    Battleground::Reset();
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _boundsCheckTimer = 0;
}

void BattlegroundTRT::PostUpdateImpl(uint32 diff)
{
    // Deliberately outside any STATUS_IN_PROGRESS gate: the warmup is exactly
    // when players test the start line, and a player who has wandered off into
    // the desert during the countdown is just as stuck as one who does it
    // mid-match.
    _boundsCheckTimer += diff;
    if (_boundsCheckTimer >= BG_TRT_BOUNDS_CHECK_INTERVAL)
    {
        _boundsCheckTimer = 0;
        ConfinePlayers();
    }
}

void BattlegroundTRT::ConfinePlayers()
{
    for (auto const& itr : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(itr.first);
        // A teleport already in flight will fix the position by itself;
        // issuing another one on top of it just fights the first.
        if (!player || player->IsBeingTeleported())
            continue;

        if (player->GetPositionZ() < BG_TRT_MIN_SAFE_Z)
        {
            HandlePlayerUnderMap(player);
            continue;
        }

        ConfineToRegion(player);
    }
}

// Slide a player back inside the region they are allowed to be in, keeping
// them as close to where they were as possible. Returns true if it moved them.
//
// Held Z rather than re-deriving it: the arena floor is level to within a yard,
// so the player's own Z is already the right height, and it avoids a terrain
// query per player per check. A player who was airborne simply falls to the
// shelf, which is the same thing that would happen anyway.
bool BattlegroundTRT::ConfineToRegion(Player* player) const
{
    float minX = BG_TRT_ARENA_MIN_X;
    float maxX = BG_TRT_ARENA_MAX_X;

    // Before the gates open each team is additionally held behind its own
    // gate line. The gates are open terrain dressing and can simply be walked
    // around, so this is what actually keeps the two sides apart.
    if (GetStatus() != STATUS_IN_PROGRESS)
    {
        if (player->GetBGTeam() == ALLIANCE)
            maxX = BG_TRT_GATE_LINE_ALLIANCE;
        else if (player->GetBGTeam() == HORDE)
            minX = BG_TRT_GATE_LINE_HORDE;
    }

    float x = player->GetPositionX();
    float y = player->GetPositionY();
    bool outside = false;

    if (x < minX)
    {
        x = minX + BG_TRT_BOUNDS_INSET;
        outside = true;
    }
    else if (x > maxX)
    {
        x = maxX - BG_TRT_BOUNDS_INSET;
        outside = true;
    }

    if (y < BG_TRT_ARENA_MIN_Y)
    {
        y = BG_TRT_ARENA_MIN_Y + BG_TRT_BOUNDS_INSET;
        outside = true;
    }
    else if (y > BG_TRT_ARENA_MAX_Y)
    {
        y = BG_TRT_ARENA_MAX_Y - BG_TRT_BOUNDS_INSET;
        outside = true;
    }

    if (!outside)
        return false;

    player->TeleportTo(GetMapId(), x, y, player->GetPositionZ(), player->GetOrientation());
    return true;
}

void BattlegroundTRT::TrackHumanParticipantAdded(Player const* player, bool isInBattleground)
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

void BattlegroundTRT::TrackHumanParticipantRemoved(Player const* player, uint32 team)
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

void BattlegroundTRT::UpdateHumanFaceoffState()
{
    if (_allianceHumanParticipants > 0 && _hordeHumanParticipants > 0)
        _humanFaceoffEverHappened = true;
}

uint32 BattlegroundTRT::GetHonorRewardForTeam() const
{
    uint32 reward = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) / 2;

    if (!_humanFaceoffEverHappened)
        reward /= 2;

    return reward;
}

void BattlegroundTRT::ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const
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

bool BattlegroundTRT::SetupBattleground()
{
    // Every Z below is the terrain height read out of the server's own
    // 16204737.map tile at that exact point. The shelf is level but not
    // perfectly flat, so these are measured rather than shared - re-measure if
    // anything moves, or objects end up buried or floating.
    //
    // Gates sit on each team's start line, between the spawn and the middle,
    // facing each other down the x axis.
    if (!AddObject(BG_TRT_OBJECT_GATE_ALLIANCE, BG_TRT_OBJECT_GATE_ENTRY,
            BG_TRT_GATE_LINE_ALLIANCE, -3010.0f, 9.57f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            RESPAWN_IMMEDIATELY)
        || !AddObject(BG_TRT_OBJECT_GATE_HORDE, BG_TRT_OBJECT_GATE_ENTRY,
            BG_TRT_GATE_LINE_HORDE, -3010.0f, 8.73f, 3.14159f,
            0.0f, 0.0f, 1.0f, 0.0f,
            RESPAWN_IMMEDIATELY)
        // Four buff nodes, north/south/east/west of the arena centre and
        // equidistant from it, so neither side is closer to any of them.
        || !AddObject(BG_TRT_OBJECT_BUFF1_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            -8365.0f, -2955.0f, 8.63f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF1_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            -8365.0f, -2955.0f, 8.63f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF1_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            -8365.0f, -2955.0f, 8.63f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF2_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            -8365.0f, -3065.0f, 8.09f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF2_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            -8365.0f, -3065.0f, 8.09f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF2_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            -8365.0f, -3065.0f, 8.09f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF3_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            -8415.0f, -3010.0f, 8.65f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF3_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            -8415.0f, -3010.0f, 8.65f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF3_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            -8415.0f, -3010.0f, 8.65f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF4_SPEED, BG_OBJECTID_SPEEDBUFF_ENTRY,
            -8315.0f, -3010.0f, 8.63f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF4_REGEN, BG_OBJECTID_REGENBUFF_ENTRY,
            -8315.0f, -3010.0f, 8.63f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME)
        || !AddObject(BG_TRT_OBJECT_BUFF4_BERSERK, BG_OBJECTID_BERSERKERBUFF_ENTRY,
            -8315.0f, -3010.0f, 8.63f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, BG_TRT_BUFF_RESPAWN_TIME))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundTRT::SetupBattleground: failed to spawn one or more Tanaris gameobjects.");
        return false;
    }

    WorldSafeLocsEntry const* alliance = sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_ALLIANCE);
    WorldSafeLocsEntry const* horde = sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_HORDE);

    if (!alliance || !horde)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundTRT::SetupBattleground: one or more Tanaris graveyards are missing in WorldSafeLocs.");
        return false;
    }

    if (!AddSpiritGuide(BG_TRT_SPIRIT_ALLIANCE, alliance->Loc.X, alliance->Loc.Y, alliance->Loc.Z, 0.0f, TEAM_ALLIANCE))
        return false;
    if (!AddSpiritGuide(BG_TRT_SPIRIT_HORDE, horde->Loc.X, horde->Loc.Y, horde->Loc.Z, 3.14159f, TEAM_HORDE))
        return false;

    ApplyNonInteractableObjectFlags();
    return true;
}

void BattlegroundTRT::ApplyNonInteractableObjectFlags()
{
    if (GameObject* allianceGate = GetBGObject(BG_TRT_OBJECT_GATE_ALLIANCE))
        allianceGate->SetFlag(GO_FLAG_NOT_SELECTABLE);

    if (GameObject* hordeGate = GetBGObject(BG_TRT_OBJECT_GATE_HORDE))
        hordeGate->SetFlag(GO_FLAG_NOT_SELECTABLE);
}

void BattlegroundTRT::SpawnRandomBuffSet(uint32 speedTypeIndex)
{
    // Despawn all three buff variants at this node first.
    SpawnBGObject(speedTypeIndex + 0, RESPAWN_ONE_DAY);
    SpawnBGObject(speedTypeIndex + 1, RESPAWN_ONE_DAY);
    SpawnBGObject(speedTypeIndex + 2, RESPAWN_ONE_DAY);

    uint8 const buff = urand(0, 2);
    SpawnBGObject(speedTypeIndex + buff, RESPAWN_IMMEDIATELY);
}

void BattlegroundTRT::StartingEventCloseDoors()
{
    DoorClose(BG_TRT_OBJECT_GATE_ALLIANCE);
    DoorClose(BG_TRT_OBJECT_GATE_HORDE);
    SpawnBGObject(BG_TRT_OBJECT_GATE_ALLIANCE, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_TRT_OBJECT_GATE_HORDE, RESPAWN_IMMEDIATELY);

    // Hide all buff variants until battle start.
    for (uint32 type = BG_TRT_OBJECT_BUFF1_SPEED; type <= BG_TRT_OBJECT_BUFF4_BERSERK; ++type)
        SpawnBGObject(type, RESPAWN_ONE_DAY);

    ApplyNonInteractableObjectFlags();
}

void BattlegroundTRT::StartingEventOpenDoors()
{
    DoorOpen(BG_TRT_OBJECT_GATE_ALLIANCE);
    DoorOpen(BG_TRT_OBJECT_GATE_HORDE);

    SpawnRandomBuffSet(BG_TRT_OBJECT_BUFF1_SPEED);
    SpawnRandomBuffSet(BG_TRT_OBJECT_BUFF2_SPEED);
    SpawnRandomBuffSet(BG_TRT_OBJECT_BUFF3_SPEED);
    SpawnRandomBuffSet(BG_TRT_OBJECT_BUFF4_SPEED);

    ApplyNonInteractableObjectFlags();
}

WorldSafeLocsEntry const* BattlegroundTRT::GetClosestGraveyard(Player* player)
{
    if (!player)
        return nullptr;

    if (player->GetTeam() == ALLIANCE)
    {
        if (GetStatus() == STATUS_IN_PROGRESS)
            return sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_ALLIANCE);

        return sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_ALLIANCE_START);
    }

    if (GetStatus() == STATUS_IN_PROGRESS)
        return sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_HORDE);

    return sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_HORDE_START);
}

void BattlegroundTRT::UpdateTeamScoreWorldStates()
{
    UpdateWorldState(BG_TRT_WORLDSTATE_SHOW, 1);
    UpdateWorldState(BG_TRT_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    UpdateWorldState(BG_TRT_WORLDSTATE_HORDE_SCORE, _hordeKills);
    UpdateWorldState(BG_TRT_WORLDSTATE_MAX_SCORE, GetDeathmatchKillLimit(BG_TRT_KILL_LIMIT));
    UpdateWorldState(BG_TRT_WORLDSTATE_MAX_KILLS_UI, GetDeathmatchKillLimit(BG_TRT_KILL_LIMIT));
}

void BattlegroundTRT::AwardPointToTeam(uint32 team)
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

    if (_allianceKills >= GetDeathmatchKillLimit(BG_TRT_KILL_LIMIT))
        EndBattleground(ALLIANCE);
    else if (_hordeKills >= GetDeathmatchKillLimit(BG_TRT_KILL_LIMIT))
        EndBattleground(HORDE);
}

void BattlegroundTRT::AwardLeavePointIfNeeded(Player const* player, uint32 team)
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

void BattlegroundTRT::HandleKillPlayer(Player* victim, Player* killer)
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

void BattlegroundTRT::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_TRT_WORLDSTATE_SHOW, 1);
    packet.Worldstates.emplace_back(BG_TRT_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    packet.Worldstates.emplace_back(BG_TRT_WORLDSTATE_HORDE_SCORE, _hordeKills);
    packet.Worldstates.emplace_back(BG_TRT_WORLDSTATE_MAX_SCORE, GetDeathmatchKillLimit(BG_TRT_KILL_LIMIT));
    packet.Worldstates.emplace_back(BG_TRT_WORLDSTATE_MAX_KILLS_UI, GetDeathmatchKillLimit(BG_TRT_KILL_LIMIT));
    packet.Worldstates.emplace_back(BG_TRT_WORLDSTATE_TIMER_ACTIVE, 0);
    packet.Worldstates.emplace_back(BG_TRT_WORLDSTATE_TIMER, 0);
}

bool BattlegroundTRT::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    WorldSafeLocsEntry const* safeLoc = nullptr;
    if (player->GetTeam() == ALLIANCE)
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_ALLIANCE_START);
    else
        safeLoc = sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_HORDE_START);

    if (!safeLoc)
        return false;

    player->TeleportTo(GetMapId(), safeLoc->Loc.X, safeLoc->Loc.Y, safeLoc->Loc.Z + 1.0f, player->GetOrientation());
    return true;
}

void BattlegroundTRT::EndBattleground(uint32 winner)
{
    UpdateTeamScoreWorldStates();
    Battleground::EndBattleground(winner);
}

uint32 BattlegroundTRT::GetBuffRespawnTime(uint32 /*type*/) const
{
    return BG_TRT_BUFF_RESPAWN_TIME;
}
