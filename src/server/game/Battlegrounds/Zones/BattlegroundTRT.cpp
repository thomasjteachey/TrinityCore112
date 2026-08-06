#include "BattlegroundTRT.h"

#include "BattlegroundMgr.h"
#include "DBCStores.h"
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
    // No BgObjects: the zone carries no start gates and no buff nodes. The RTS
    // structures will be spawned and owned by their own system rather than
    // living in this fixed-size array.
    BgCreatures.resize(BG_TRT_CREATURE_MAX);
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _boundsCheckTimer = 0;
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
    // when players test the boundary, and someone who has wandered off during
    // the countdown is just as out of place as one who does it mid-match.
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

// Keep a player inside the region they are allowed to be in, moving them as
// little as possible. Returns true if it moved them.
//
// Z is kept rather than re-derived: the player is already standing on valid
// ground, and holding their height avoids a terrain query per player per check.
// Someone airborne simply falls, which is what would have happened anyway.
bool BattlegroundTRT::ConfineToRegion(Player* player) const
{
    // Before the gates would have opened, hold each team near its own spawn.
    // The zone is open ground with the two starts over two thousand yards
    // apart, so a radius around the spawn is the only thing separating the
    // sides during the warmup.
    if (GetStatus() != STATUS_IN_PROGRESS)
    {
        uint32 const startLoc = (player->GetBGTeam() == HORDE)
            ? BG_TRT_GY_HORDE_START : BG_TRT_GY_ALLIANCE_START;

        if (WorldSafeLocsEntry const* start = sWorldSafeLocsStore.LookupEntry(startLoc))
        {
            float const dx = player->GetPositionX() - start->Loc.X;
            float const dy = player->GetPositionY() - start->Loc.Y;
            if ((dx * dx + dy * dy) > (BG_TRT_WARMUP_RADIUS * BG_TRT_WARMUP_RADIUS))
            {
                player->TeleportTo(GetMapId(), start->Loc.X, start->Loc.Y, start->Loc.Z + 1.0f,
                    player->GetOrientation());
                return true;
            }
        }
        return false;
    }

    float x = player->GetPositionX();
    float y = player->GetPositionY();
    bool outside = false;

    if (x < BG_TRT_FIELD_MIN_X)
    {
        x = BG_TRT_FIELD_MIN_X + BG_TRT_BOUNDS_INSET;
        outside = true;
    }
    else if (x > BG_TRT_FIELD_MAX_X)
    {
        x = BG_TRT_FIELD_MAX_X - BG_TRT_BOUNDS_INSET;
        outside = true;
    }

    if (y < BG_TRT_FIELD_MIN_Y)
    {
        y = BG_TRT_FIELD_MIN_Y + BG_TRT_BOUNDS_INSET;
        outside = true;
    }
    else if (y > BG_TRT_FIELD_MAX_Y)
    {
        y = BG_TRT_FIELD_MAX_Y - BG_TRT_BOUNDS_INSET;
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
    WorldSafeLocsEntry const* alliance = sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_ALLIANCE);
    WorldSafeLocsEntry const* horde = sWorldSafeLocsStore.LookupEntry(BG_TRT_GY_HORDE);

    if (!alliance || !horde)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundTRT::SetupBattleground: one or more Tanaris graveyards are missing in WorldSafeLocs.");
        return false;
    }

    if (!AddSpiritGuide(BG_TRT_SPIRIT_ALLIANCE, alliance->Loc.X, alliance->Loc.Y, alliance->Loc.Z, 0.0f, TEAM_ALLIANCE))
        return false;
    if (!AddSpiritGuide(BG_TRT_SPIRIT_HORDE, horde->Loc.X, horde->Loc.Y, horde->Loc.Z, 0.0f, TEAM_HORDE))
        return false;

    return true;
}

// Both required by the base class. There is nothing to open or close: the zone
// has no start gates, and the warmup is enforced by the spawn radius in
// ConfineToRegion instead.
void BattlegroundTRT::StartingEventCloseDoors()
{
}

void BattlegroundTRT::StartingEventOpenDoors()
{
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
