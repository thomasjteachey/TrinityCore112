#include "BattlegroundGS.h"

#include "BattlegroundMgr.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include "WorldSession.h"

namespace
{
// First-pass transport-local deck points. These are intentionally centralized so
// we can tune them quickly after the first live test on the moving ships.
BG_GS_TransportPoint const GS_SKYBREAKER_START =
{
    BG_GS_TRANSPORT_SKYBREAKER,
    Position(6.666975f, 0.013001f, 20.878880f, 0.0f)
};

BG_GS_TransportPoint const GS_SKYBREAKER_GY =
{
    BG_GS_TRANSPORT_SKYBREAKER,
    Position(-17.557380f, -0.090421f, 21.183660f, 0.0f)
};

BG_GS_TransportPoint const GS_ORGRIM_START =
{
    BG_GS_TRANSPORT_ORGRIM_HAMMER,
    Position(47.550990f, -0.101778f, 37.611110f, 3.14159265f)
};

BG_GS_TransportPoint const GS_ORGRIM_GY =
{
    BG_GS_TRANSPORT_ORGRIM_HAMMER,
    Position(7.461699f, 0.158853f, 35.729890f, 3.14159265f)
};
}

void BattlegroundGSScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra custom scoreboard columns yet
}

BattlegroundGS::BattlegroundGS()
{
    BgObjects.resize(0);
    BgCreatures.resize(BG_GS_CREATURE_MAX);
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _skybreaker = nullptr;
    _orgrimsHammer = nullptr;
}

void BattlegroundGS::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    TrackHumanParticipantAdded(player, isInBattleground);

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundGSScore(player->GetGUID(), scoreboardTeamMarker);
    }

    if (GetStatus() != STATUS_NONE)
        TeleportPlayerToTransportPoint(player, GetTeamStartPoint(player->GetBGTeam()));
}

void BattlegroundGS::RemovePlayer(Player* player, ObjectGuid /*guid*/, uint32 team)
{
    AwardLeavePointIfNeeded(player, team);
    TrackHumanParticipantRemoved(player, team);
}

void BattlegroundGS::Reset()
{
    Battleground::Reset();
    _allianceKills = 0;
    _hordeKills = 0;
    _allianceHumanParticipants = 0;
    _hordeHumanParticipants = 0;
    _humanFaceoffEverHappened = false;
    _skybreaker = nullptr;
    _orgrimsHammer = nullptr;
}

bool BattlegroundGS::SetupBattleground()
{
    if (!SpawnGunshipTransports())
        return false;

    if (!AddTransportSpiritGuide(BG_GS_SPIRIT_ALLIANCE, _skybreaker, GS_SKYBREAKER_GY.Offset, TEAM_ALLIANCE))
        return false;

    if (!AddTransportSpiritGuide(BG_GS_SPIRIT_HORDE, _orgrimsHammer, GS_ORGRIM_GY.Offset, TEAM_HORDE))
        return false;

    return true;
}

void BattlegroundGS::StartingEventCloseDoors()
{
}

void BattlegroundGS::StartingEventOpenDoors()
{
}

bool BattlegroundGS::SpawnGunshipTransports()
{
    Map* map = FindBgMap();
    if (!map)
        return false;

    _skybreaker = sTransportMgr->CreateTransport(BG_GS_TRANSPORT_SKYBREAKER, 0, map);
    _orgrimsHammer = sTransportMgr->CreateTransport(BG_GS_TRANSPORT_ORGRIM_HAMMER, 0, map);

    if (!_skybreaker || !_orgrimsHammer)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundGS::SetupBattleground: failed to create one or both gunship transports.");
        return false;
    }

    return true;
}

Transport* BattlegroundGS::GetTransportForEntry(uint32 entry) const
{
    switch (entry)
    {
        case BG_GS_TRANSPORT_SKYBREAKER:
            return _skybreaker;
        case BG_GS_TRANSPORT_ORGRIM_HAMMER:
            return _orgrimsHammer;
        default:
            return nullptr;
    }
}

BG_GS_TransportPoint const& BattlegroundGS::GetTeamStartPoint(uint32 team) const
{
    return team == HORDE ? GS_ORGRIM_START : GS_SKYBREAKER_START;
}

BG_GS_TransportPoint const& BattlegroundGS::GetTeamGraveyardPoint(uint32 team) const
{
    return team == HORDE ? GS_ORGRIM_GY : GS_SKYBREAKER_GY;
}

bool BattlegroundGS::TeleportPlayerToTransportPoint(Player* player, BG_GS_TransportPoint const& point, bool resurrectAtTeleport)
{
    if (!player)
        return false;

    Transport* transport = GetTransportForEntry(point.TransportEntry);
    if (!transport)
        return false;

    if (Transport* oldTransport = player->GetTransport())
        if (oldTransport != transport)
            oldTransport->RemovePassenger(player);

    player->m_movementInfo.transport.pos.Relocate(point.Offset);
    transport->AddPassenger(player);

    float x = point.Offset.GetPositionX();
    float y = point.Offset.GetPositionY();
    float z = point.Offset.GetPositionZ();
    float o = point.Offset.GetOrientation();
    transport->CalculatePassengerPosition(x, y, z, &o);

    uint32 options = TELE_TO_NOT_LEAVE_TRANSPORT | TELE_TO_TRANSPORT_TELEPORT;
    if (resurrectAtTeleport)
        options |= TELE_REVIVE_AT_TELEPORT;

    return player->TeleportTo(GetMapId(), x, y, z, o, options);
}

bool BattlegroundGS::AddTransportSpiritGuide(uint32 type, Transport* transport, Position const& offset, TeamId teamId)
{
    if (!transport)
        return false;

    uint32 entry = (teamId == TEAM_ALLIANCE) ? BG_CREATURE_ENTRY_A_SPIRITGUIDE : BG_CREATURE_ENTRY_H_SPIRITGUIDE;

    Creature* creature = AddCreature(entry, type, offset.GetPositionX(), offset.GetPositionY(), offset.GetPositionZ(), offset.GetOrientation(), teamId, 0, transport);
    if (!creature)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundGS::AddTransportSpiritGuide: cannot create spirit guide on transport entry {}.", transport->GetEntry());
        return false;
    }

    creature->setDeathState(DEAD);
    creature->SetChannelObjectGuid(creature->GetGUID());
    creature->SetChannelSpellId(SPELL_SPIRIT_HEAL_CHANNEL);
    creature->SetModCastingSpeed(1.0f);
    return true;
}

WorldSafeLocsEntry const* BattlegroundGS::GetClosestGraveyard(Player* player)
{
    if (!player)
        return nullptr;

    return sWorldSafeLocsStore.LookupEntry(player->GetBGTeam() == HORDE ? BG_GS_GY_HORDE : BG_GS_GY_ALLIANCE);
}

bool BattlegroundGS::HandlePlayerRepopAtGraveyard(Player* player, bool /*shouldResurrect*/)
{
    if (!player)
        return false;

    // For this BG, do not auto-revive just because the player died on a transport.
    // Release them as a ghost onto their moving ship and let the normal BG
    // resurrection wave revive them at the shipboard spirit guide.
    if (!TeleportPlayerToTransportPoint(player, GetTeamGraveyardPoint(player->GetBGTeam()), false))
        return false;

    if (player->isDead())
    {
        uint32 const spiritType = player->GetBGTeam() == HORDE ? BG_GS_SPIRIT_HORDE : BG_GS_SPIRIT_ALLIANCE;
        if (Creature* spiritGuide = GetBGCreature(spiritType, false))
        {
            if (!IsPlayerInResurrectQueue(player->GetGUID()))
                AddPlayerToResurrectQueue(spiritGuide->GetGUID(), player->GetGUID());

            sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(player, this, spiritGuide->GetGUID());
        }
    }

    return true;
}

void BattlegroundGS::UpdateTeamScoreWorldStates()
{
    UpdateWorldState(BG_GS_WORLDSTATE_SHOW, 1);
    UpdateWorldState(BG_GS_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    UpdateWorldState(BG_GS_WORLDSTATE_HORDE_SCORE, _hordeKills);
    UpdateWorldState(BG_GS_WORLDSTATE_MAX_SCORE, BG_GS_KILL_LIMIT);
    UpdateWorldState(BG_GS_WORLDSTATE_MAX_KILLS_UI, BG_GS_KILL_LIMIT);
}

void BattlegroundGS::AwardPointToTeam(uint32 team)
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

    if (_allianceKills >= BG_GS_KILL_LIMIT)
        EndBattleground(ALLIANCE);
    else if (_hordeKills >= BG_GS_KILL_LIMIT)
        EndBattleground(HORDE);
}

void BattlegroundGS::AwardLeavePointIfNeeded(Player const* player, uint32 team)
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

void BattlegroundGS::HandleKillPlayer(Player* victim, Player* killer)
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

void BattlegroundGS::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_GS_WORLDSTATE_SHOW, 1);
    packet.Worldstates.emplace_back(BG_GS_WORLDSTATE_ALLIANCE_SCORE, _allianceKills);
    packet.Worldstates.emplace_back(BG_GS_WORLDSTATE_HORDE_SCORE, _hordeKills);
    packet.Worldstates.emplace_back(BG_GS_WORLDSTATE_MAX_SCORE, BG_GS_KILL_LIMIT);
    packet.Worldstates.emplace_back(BG_GS_WORLDSTATE_MAX_KILLS_UI, BG_GS_KILL_LIMIT);
    packet.Worldstates.emplace_back(BG_GS_WORLDSTATE_TIMER_ACTIVE, 0);
    packet.Worldstates.emplace_back(BG_GS_WORLDSTATE_TIMER, 0);
}

bool BattlegroundGS::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    return TeleportPlayerToTransportPoint(player, GetTeamStartPoint(player->GetBGTeam()), true);
}

void BattlegroundGS::EndBattleground(uint32 winner)
{
    UpdateTeamScoreWorldStates();
    Battleground::EndBattleground(winner);
}

void BattlegroundGS::TrackHumanParticipantAdded(Player const* player, bool isInBattleground)
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

void BattlegroundGS::TrackHumanParticipantRemoved(Player const* player, uint32 team)
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

void BattlegroundGS::UpdateHumanFaceoffState()
{
    if (_allianceHumanParticipants > 0 && _hordeHumanParticipants > 0)
        _humanFaceoffEverHappened = true;
}

uint32 BattlegroundGS::GetHonorRewardForTeam() const
{
    uint32 reward = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) / 2;

    if (!_humanFaceoffEverHappened)
        reward /= 2;

    return reward;
}

void BattlegroundGS::ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const
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
