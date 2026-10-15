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

#include "Arena.h"
#include "ArenaScore.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldStatePackets.h"

void ArenaScore::AppendToPacket(WorldPacket& data)
{
    data << uint64(PlayerGuid);

    data << uint32(KillingBlows);
    data << uint8(TeamId);
    data << uint32(DamageDone);
    data << uint32(HealingDone);

    BuildObjectivesBlock(data);
}

void ArenaScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // Objectives Count
}

void ArenaTeamScore::BuildRatingInfoBlock(WorldPacket& data)
{
    uint32 ratingLost = std::abs(std::min(RatingChange, 0));
    uint32 ratingWon = std::max(RatingChange, 0);

    // should be old rating, new rating, and client will calculate rating change itself
    data << uint32(ratingLost);
    data << uint32(ratingWon);
    data << uint32(MatchmakerRating);
}

void ArenaTeamScore::BuildTeamInfoBlock(WorldPacket& data)
{
    data << TeamName;
}

Arena::Arena()
{
    StartDelayTimes[BG_STARTING_EVENT_FIRST]  = BG_START_DELAY_30S;
    StartDelayTimes[BG_STARTING_EVENT_SECOND] = BG_START_DELAY_15S;
    StartDelayTimes[BG_STARTING_EVENT_THIRD]  = BG_START_DELAY_10S;
    StartDelayTimes[BG_STARTING_EVENT_FOURTH] = BG_START_DELAY_NONE;

    StartMessageIds[BG_STARTING_EVENT_FIRST]  = ARENA_TEXT_START_THIRTY_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_SECOND] = ARENA_TEXT_START_FIFTEEN_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_THIRD]  = ARENA_TEXT_START_TEN_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = ARENA_TEXT_START_BATTLE_HAS_BEGUN;
}

void Arena::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);
    if (IsCustomGame())
        _customGameEliminatedPlayers.erase(player->GetGUID());

    if (!isInBattleground)
        PlayerScores[player->GetGUID().GetCounter()] = new ArenaScore(player->GetGUID(), player->GetBGTeam());

    // A squad match: each squad carries its own flag (battleground_squad_start
    // FlagSpell) - two squads share each side, so the side's flag would make
    // two of them look alike.
    if (uint32 const squadFlag = GetSquadFlagSpell(GetPlayerSquad(player->GetGUID())))
        player->CastSpell(player, squadFlag, true);
    else if (player->GetBGTeam() == ALLIANCE)   // gold
    {
        if (player->GetTeam() == HORDE)
            player->CastSpell(player, SPELL_HORDE_GOLD_FLAG, true);
        else
            player->CastSpell(player, SPELL_ALLIANCE_GOLD_FLAG, true);
    }
    else                                        // green
    {
        if (player->GetTeam() == HORDE)
            player->CastSpell(player, SPELL_HORDE_GREEN_FLAG, true);
        else
            player->CastSpell(player, SPELL_ALLIANCE_GREEN_FLAG, true);
    }

    UpdateArenaWorldState();
}

void Arena::RemovePlayer(Player* player, ObjectGuid guid, uint32 /*team*/)
{
    if (IsCustomGame())
        _customGameEliminatedPlayers.erase(guid);

    // SpellInfo::CheckLocation would drop a squad flag once the player is
    // out of the arena; take it off on the way out so it never lingers.
    if (player)
        if (uint32 const squadFlag = GetSquadFlagSpell(GetPlayerSquad(guid)))
            player->RemoveAurasDueToSpell(squadFlag);

    // Their enemy frames go with them; the others see the roster change on
    // the feed's next look.
    if (player && IsSquadMatch())
        SendSquadFeedLine(player, "SQ:-");

    if (GetStatus() == STATUS_WAIT_LEAVE)
        return;

    UpdateArenaWorldState();
    CheckWinConditions();
}

void Arena::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    if (IsSquadMatch())
    {
        for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
        {
            packet.Worldstates.emplace_back(ARENA_WORLD_STATE_SQUAD_SHOW_FIRST + squad, IsSquadInMatch(squad) ? 1 : 0);
            packet.Worldstates.emplace_back(ARENA_WORLD_STATE_SQUAD_ALIVE_FIRST + squad, GetArenaAlivePlayersCountBySquad(squad));
        }
        return;
    }

    packet.Worldstates.emplace_back(ARENA_WORLD_STATE_ALIVE_PLAYERS_GREEN, GetArenaAlivePlayersCountByTeam(HORDE));
    packet.Worldstates.emplace_back(ARENA_WORLD_STATE_ALIVE_PLAYERS_GOLD, GetArenaAlivePlayersCountByTeam(ALLIANCE));
}

// ---------------------------------------------------------------- squad feed
//
// What the multi-team arena enemy frames (client CenturionSquadFrames.lua)
// draw, and what colours the scoreboard by squad - all CCGAME addon lines:
//
//   SQ:<mine>:<squads>        you are in squad <mine> (0-3, "-" for none) of <squads>
//   SQ:-                      not a squad match, or you left it: clear everything
//   SQR:<s>:<team>:<members>  squad s's roster in seat order, "name,class;...",
//                             <team> being its arena team's name (rated only)
//   SQH:<s>:<states>          the same members' states, comma-separated:
//                             <health%>.<power%>.<power type>, or d (dead),
//                             s (stealthed or invisible), o (not on the map)
//   SQW:<s> / SQW:-           the winning squad, or a draw, once it is over
//
// A line goes out when it changes, looked at every BATTLEGROUND_SQUAD_FEED_INTERVAL;
// a client that has just loaded asks for all of it (CCGAMEREQ SQUADS). A member
// in stealth is "s" to everyone, the way the stock frames lose an opponent who
// stealths: their health is not sent while they cannot be seen.

Arena::SquadFeedPlayers Arena::GetSquadFeedPlayers() const
{
    // From the map rather than ObjectAccessor: it holds the bot clones too, and
    // it asks nothing of anyone who has already gone.
    SquadFeedPlayers players;
    if (BattlegroundMap* map = FindBgMap())
        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
            if (Player* player = itr->GetSource())
                players[player->GetGUID()] = player;
    return players;
}

std::vector<ObjectGuid> Arena::GetSquadFeedMembers(uint8 squad) const
{
    std::vector<ObjectGuid> members;
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        if (GetPlayerSquad(guid) == squad)
            members.push_back(guid);
    }
    return members;
}

std::string Arena::BuildSquadFeedRoster(uint8 squad, std::vector<ObjectGuid> const& members, SquadFeedPlayers const& players) const
{
    std::string line = Trinity::StringFormat("SQR:{}:", squad);
    if (uint32 const arenaTeamId = GetSquadArenaTeamId(squad))
        if (ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(arenaTeamId))
            line += arenaTeam->GetName();
    line += ':';

    bool first = true;
    for (ObjectGuid guid : members)
    {
        // The name the client knows them by (a clone's own name is internal).
        std::string name;
        uint32 playerClass = 0;
        SquadFeedPlayers::const_iterator const itr = players.find(guid);
        if (itr != players.end())
        {
            name = GetPlayerDisplayName(itr->second);
            playerClass = itr->second->GetClass();
        }
        else if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid))
        {
            name = entry->Name;
            playerClass = entry->Class;
        }
        if (name.empty())
            name = "?";

        if (!first)
            line += ';';
        first = false;
        line += name;
        line += ',';
        line += std::to_string(playerClass);
    }
    return line;
}

std::string Arena::BuildSquadFeedStates(uint8 squad, std::vector<ObjectGuid> const& members, SquadFeedPlayers const& players) const
{
    std::string line = Trinity::StringFormat("SQH:{}:", squad);
    bool first = true;
    for (ObjectGuid guid : members)
    {
        if (!first)
            line += ',';
        first = false;

        SquadFeedPlayers::const_iterator const itr = players.find(guid);
        Player* player = itr != players.end() ? itr->second : nullptr;
        if (!player)
            line += 'o';
        else if (!IsArenaAlive(guid))
            line += 'd';
        else if (player->HasStealthAura() || player->HasInvisibilityAura())
            line += 's';
        else
        {
            // Rounded up, so nobody alive reads 0%.
            uint64 const maxHealth = std::max<uint32>(player->GetMaxHealth(), 1);
            uint32 const health = uint32(std::min<uint64>((uint64(player->GetHealth()) * 100 + maxHealth - 1) / maxHealth, 100));
            Powers const powerType = player->GetPowerType();
            uint64 const maxPower = player->GetMaxPower(powerType);
            uint32 const power = maxPower ? uint32(std::min<uint64>(uint64(player->GetPower(powerType)) * 100 / maxPower, 100)) : 0;
            line += Trinity::StringFormat("{}.{}.{}", std::max<uint32>(health, 1), power, uint32(powerType));
        }
    }
    return line;
}

std::string Arena::BuildSquadFeedWinner() const
{
    uint8 const winner = GetWinnerSquad();
    return winner < GetSquadCount() ? Trinity::StringFormat("SQW:{}", winner) : std::string("SQW:-");
}

void Arena::BroadcastSquadFeedLine(std::string const& line, SquadFeedPlayers const& players) const
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty, ObjectGuid::Empty, "CCGAME\t" + line, 0);
    for (auto const& [guid, player] : players)
    {
        (void)guid;
        if (player->GetSession() && !player->GetSession()->IsVirtualSession())
            player->SendDirectMessage(&data);
    }
}

void Arena::UpdateSquadFeed()
{
    if (!IsSquadMatch() || !FindBgMap())
        return;

    SquadFeedPlayers const players = GetSquadFeedPlayers();
    for (uint8 squad = 0; squad < GetSquadCount() && squad < MAX_BATTLEGROUND_SQUADS; ++squad)
    {
        std::vector<ObjectGuid> const members = GetSquadFeedMembers(squad);
        std::string roster = BuildSquadFeedRoster(squad, members, players);
        if (roster != _squadFeedRoster[squad])
        {
            _squadFeedRoster[squad] = std::move(roster);
            // The states are read in the roster's order, so they follow it out.
            _squadFeedStates[squad].clear();
            BroadcastSquadFeedLine(_squadFeedRoster[squad], players);
        }

        std::string states = BuildSquadFeedStates(squad, members, players);
        if (states != _squadFeedStates[squad])
        {
            _squadFeedStates[squad] = std::move(states);
            BroadcastSquadFeedLine(_squadFeedStates[squad], players);
        }
    }

    if (GetStatus() == STATUS_WAIT_LEAVE && !_squadFeedWinnerSent)
    {
        _squadFeedWinnerSent = true;
        BroadcastSquadFeedLine(BuildSquadFeedWinner(), players);
    }
}

void Arena::SendSquadFeedTo(Player* player)
{
    if (!IsSquadMatch())
    {
        Battleground::SendSquadFeedTo(player);
        return;
    }
    if (!player)
        return;

    uint8 const mine = GetPlayerSquad(player->GetGUID());
    SendSquadFeedLine(player, Trinity::StringFormat("SQ:{}:{}",
        mine < GetSquadCount() ? std::to_string(mine) : std::string("-"), uint32(GetSquadCount())));

    SquadFeedPlayers const players = GetSquadFeedPlayers();
    for (uint8 squad = 0; squad < GetSquadCount() && squad < MAX_BATTLEGROUND_SQUADS; ++squad)
    {
        std::vector<ObjectGuid> const members = GetSquadFeedMembers(squad);
        SendSquadFeedLine(player, BuildSquadFeedRoster(squad, members, players));
        SendSquadFeedLine(player, BuildSquadFeedStates(squad, members, players));
    }

    if (_squadFeedWinnerSent)
        SendSquadFeedLine(player, BuildSquadFeedWinner());
}

bool Arena::IsSquadInMatch(uint8 squad) const
{
    // Once the gates are open, the squads that were there; before, anyone
    // seated or on the way in. A corner nobody came for stays off the readout.
    if (_squadsAtStart)
        return (_squadsAtStart & (1u << squad)) != 0;

    return CountSquadSeats(squad) != 0;
}

void Arena::UpdateArenaWorldState()
{
    if (IsSquadMatch())
    {
        UpdateSquadWorldStates();
        return;
    }

    UpdateWorldState(ARENA_WORLD_STATE_ALIVE_PLAYERS_GREEN, GetArenaAlivePlayersCountByTeam(HORDE));
    UpdateWorldState(ARENA_WORLD_STATE_ALIVE_PLAYERS_GOLD, GetArenaAlivePlayersCountByTeam(ALLIANCE));
}

void Arena::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (IsCustomGame() && player)
        _customGameEliminatedPlayers.insert(player->GetGUID());

    Battleground::HandleKillPlayer(player, killer);

    UpdateArenaWorldState();
    CheckWinConditions();
}

void Arena::RemovePlayerAtLeave(ObjectGuid guid, bool transport, bool sendPacket)
{
    if (isRated() && GetStatus() == STATUS_IN_PROGRESS)
    {
        BattlegroundPlayerMap::const_iterator itr = m_Players.find(guid);
        if (itr != m_Players.end() && IsSquadMatch())
        {
            // Left a rated squad match while it was being fought: lost, against
            // the other squads taken together.
            uint8 const squad = GetPlayerSquad(guid);
            if (ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad)))
            {
                uint32 const against = GetSquadOpponentsMatchmakerRating(squad);
                if (Player* player = _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, "Arena::RemovePlayerAtLeave"))
                    arenaTeam->MemberLost(player, against);
                else
                    arenaTeam->OfflineMemberLost(guid, against);
            }
        }
        else if (itr != m_Players.end()) // check if the player was a participant of the match, or only entered through gm command (appear)
        {
            // if the player was a match participant, calculate rating
            uint32 team = itr->second.Team;

            ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(GetOtherTeam(team)));
            ArenaTeam* loserArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(team));

            // left a rated match while the encounter was in progress, consider as loser
            if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
            {
                if (Player* player = _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, "Arena::RemovePlayerAtLeave"))
                    loserArenaTeam->MemberLost(player, GetArenaMatchmakerRating(GetOtherTeam(team)));
                else
                    loserArenaTeam->OfflineMemberLost(guid, GetArenaMatchmakerRating(GetOtherTeam(team)));
            }
        }
    }

    // remove player
    Battleground::RemovePlayerAtLeave(guid, transport, sendPacket);
}

void Arena::CheckWinConditions()
{
    if (IsReplay())
    {
        return;
    }
    if (IsSquadMatch())
    {
        CheckSquadWinConditions();
        return;
    }
    if (!GetArenaAlivePlayersCountByTeam(ALLIANCE) && GetPlayersCountByTeam(HORDE))
        EndBattleground(HORDE);
    else if (GetPlayersCountByTeam(ALLIANCE) && !GetArenaAlivePlayersCountByTeam(HORDE))
        EndBattleground(ALLIANCE);
}

namespace
{
    void AnnounceToArena(Battleground* arena, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_BG_SYSTEM_NEUTRAL, LANG_UNIVERSAL, ObjectGuid::Empty, ObjectGuid::Empty, text, 0);
        arena->SendPacketToAll(&data);
    }

    char const* CountWord(uint32 count)
    {
        switch (count)
        {
            case 1: return "One";
            case 2: return "Two";
            case 3: return "Three";
            case 4: return "Four";
            default: return "No";
        }
    }
}

bool Arena::IsArenaAlive(ObjectGuid guid) const
{
    BattlegroundPlayerMap::const_iterator const itr = m_Players.find(guid);
    if (itr == m_Players.end())
        return false;

    if (IsCustomGame())
        return !itr->second.OfflineRemoveTime &&
            _customGameEliminatedPlayers.find(guid) == _customGameEliminatedPlayers.end();

    Player* player = ObjectAccessor::FindPlayer(guid);
    return player && player->IsAlive() && player->GetShapeshiftForm() != FORM_SPIRITOFREDEMPTION;
}

uint32 Arena::GetArenaAlivePlayersCountBySquad(uint8 squad) const
{
    uint32 aliveCount = 0;
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        if (GetPlayerSquad(guid) == squad && IsArenaAlive(guid))
            ++aliveCount;
    }

    return aliveCount;
}

void Arena::UpdateSquadWorldStates()
{
    // One line per squad in the top frame ("Blue Team: 2 Players Remaining"):
    // the WorldStateUI rows for the map read these, in place of the gold and
    // green pair the side-based rows show.
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
    {
        UpdateWorldState(ARENA_WORLD_STATE_SQUAD_SHOW_FIRST + squad, IsSquadInMatch(squad) ? 1 : 0);
        UpdateWorldState(ARENA_WORLD_STATE_SQUAD_ALIVE_FIRST + squad, GetArenaAlivePlayersCountBySquad(squad));
    }
}

void Arena::CheckSquadWinConditions()
{
    // Before the gates open nobody can be beaten. Whoever is missing then is
    // settled when they open (SkipStartDelay runs this again), the way a
    // two-team arena hands the match to the side that turned up.
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint32 present[MAX_BATTLEGROUND_SQUADS] = { };
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        uint8 const squad = GetPlayerSquad(guid);
        if (squad < GetSquadCount())
            ++present[squad];
    }

    // The squads in this fight are the ones standing in their corner when the
    // gates opened. A squad nobody came for never took part, so it is never
    // announced as beaten.
    if (!_squadsAtStart)
    {
        for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
            if (present[squad])
                _squadsAtStart |= 1u << squad;

        uint32 squadCount = 0;
        for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
            if (_squadsAtStart & (1u << squad))
                ++squadCount;

        if (squadCount > 1)
            AnnounceToArena(this, Trinity::StringFormat("{} teams, one to a corner. The last team standing wins.", CountWord(squadCount)));
    }

    uint8 lastStanding = BATTLEGROUND_NO_SQUAD;
    uint32 standing = 0;
    std::vector<uint8> fallen;
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
    {
        if (!(_squadsAtStart & (1u << squad)))
            continue;

        if (GetArenaAlivePlayersCountBySquad(squad))
        {
            lastStanding = squad;
            ++standing;
            continue;
        }

        if (std::find(_eliminatedSquads.begin(), _eliminatedSquads.end(), squad) != _eliminatedSquads.end())
            continue;

        _eliminatedSquads.push_back(squad);
        fallen.push_back(squad);
        TC_LOG_DEBUG("bg.arena", "Arena::CheckSquadWinConditions: squad {} ({}) eliminated in instance {}.",
            uint32(squad), GetSquadName(squad), GetInstanceID());
    }

    // Only while the fight goes on: a squad wiped out by the blow that ends the
    // match hears who won instead.
    if (standing > 1)
        for (uint8 squad : fallen)
            AnnounceToArena(this, Trinity::StringFormat("The {} team has been eliminated. {} teams remain.",
                GetSquadName(squad), CountWord(standing)));

    if (standing == 1)
    {
        EndSquadMatch(lastStanding, Trinity::StringFormat("The {} team is the last one standing and wins!", GetSquadName(lastStanding)));
        return;
    }

    if (!standing)
    {
        // The last two fell to the same blow, or nobody turned up at all.
        SetWinnerSquad(BATTLEGROUND_NO_SQUAD);
        AnnounceToArena(this, "Nobody is left standing. The match is a draw.");
        EndBattleground(0);
    }
}

void Arena::EndSquadMatch(uint8 winnerSquad, std::string const& announcement)
{
    SetWinnerSquad(winnerSquad);
    AnnounceToArena(this, announcement);
    // The side the winning squad sits on. Rewards and the scoreboard ask the
    // squad (Battleground::IsWinningParticipant), so its side-mate squad, which
    // lost, is not paid as a winner.
    EndBattleground(GetSquadSide(winnerSquad));
}

void Arena::EndAtTimeLimit()
{
    if (!IsSquadMatch())
    {
        EndBattleground(0);
        return;
    }

    // Time up in a squad match: the squad with the most still standing takes
    // it, and between equal numbers the one with the most health left between
    // them. Still level is a draw, as a two-team arena's time limit always is.
    uint8 best = BATTLEGROUND_NO_SQUAD;
    uint32 bestAlive = 0;
    uint64 bestHealth = 0;
    bool level = false;
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
    {
        uint32 alive = 0;
        uint64 health = 0;
        for (auto const& [guid, participant] : m_Players)
        {
            (void)participant;
            if (GetPlayerSquad(guid) != squad || !IsArenaAlive(guid))
                continue;

            ++alive;
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                health += player->GetHealth();
        }

        if (!alive)
            continue;

        if (best == BATTLEGROUND_NO_SQUAD || alive > bestAlive || (alive == bestAlive && health > bestHealth))
        {
            best = squad;
            bestAlive = alive;
            bestHealth = health;
            level = false;
        }
        else if (alive == bestAlive && health == bestHealth)
            level = true;
    }

    if (best == BATTLEGROUND_NO_SQUAD || level)
    {
        SetWinnerSquad(BATTLEGROUND_NO_SQUAD);
        AnnounceToArena(this, "Time is up and no team is ahead. The match is a draw.");
        EndBattleground(0);
        return;
    }

    EndSquadMatch(best, Trinity::StringFormat("Time is up. The {} team has the most left standing and wins!", GetSquadName(best)));
}

uint32 Arena::GetArenaAlivePlayersCountByTeam(uint32 team) const
{
    if (!IsCustomGame())
        return GetAlivePlayersCountByTeam(team);

    uint32 aliveCount = 0;
    for (auto const& [guid, participant] : m_Players)
        if (participant.Team == team && !participant.OfflineRemoveTime &&
            _customGameEliminatedPlayers.find(guid) == _customGameEliminatedPlayers.end())
            ++aliveCount;

    return aliveCount;
}

void Arena::EndRatedSquadMatch()
{
    std::vector<uint8> squads;
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
        if (sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad)))
            squads.push_back(squad);

    if (squads.size() < 2)
        return;

    uint8 const winnerSquad = GetWinnerSquad();

    // Nobody won - time ran out level, or the last two fell together. A
    // two-team arena takes ARENA_TIMELIMIT_POINTS_LOSS off both teams then, and
    // so does this, off every team, and counts it a loss for everyone.
    if (winnerSquad == BATTLEGROUND_NO_SQUAD)
    {
        for (uint8 squad : squads)
        {
            ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad));
            uint32 const against = GetSquadOpponentsMatchmakerRating(squad);
            arenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
            _squadArenaScores[squad].Assign(ARENA_TIMELIMIT_POINTS_LOSS, GetSquadMatchmakerRating(squad), arenaTeam->GetName());
            for (auto const& [guid, participant] : m_Players)
            {
                if (GetPlayerSquad(guid) != squad)
                    continue;

                if (Player* player = _GetPlayer(guid, participant.OfflineRemoveTime != 0, "Arena::EndRatedSquadMatch"))
                {
                    arenaTeam->MemberLost(player, against, 0);
                    player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);
                }
                else
                    arenaTeam->OfflineMemberLost(guid, against, 0);
            }

            m_SquadScoreboardRival[squad] = squads.front() != squad ? squads.front() : squads.back();
            arenaTeam->SaveToDB();
            arenaTeam->NotifyStatsChanged();
        }
        return;
    }

    // Finishing places, lower is better: the winner; then any squad still on
    // its feet when time ran out, level with each other; then the eliminated,
    // the last to fall first; then, level and last, any team that never came
    // in (its members already lost when they let the invite go).
    uint32 place[MAX_BATTLEGROUND_SQUADS];
    for (uint8 squad = 0; squad < MAX_BATTLEGROUND_SQUADS; ++squad)
        place[squad] = MAX_BATTLEGROUND_SQUADS;

    auto const tookPart = [this](uint8 squad) { return (_squadsAtStart & (1u << squad)) != 0; };
    auto const eliminated = [this](uint8 squad)
    {
        return std::find(_eliminatedSquads.begin(), _eliminatedSquads.end(), squad) != _eliminatedSquads.end();
    };

    place[winnerSquad] = 0;
    uint32 next = 1;
    bool survivors = false;
    for (uint8 squad : squads)
    {
        if (squad != winnerSquad && tookPart(squad) && !eliminated(squad))
        {
            place[squad] = next;
            survivors = true;
        }
    }
    if (survivors)
        ++next;
    for (auto itr = _eliminatedSquads.rbegin(); itr != _eliminatedSquads.rend(); ++itr)
        if (*itr != winnerSquad)
            place[*itr] = next++;
    for (uint8 squad : squads)
        if (squad != winnerSquad && !tookPart(squad))
            place[squad] = next;

    // Everyone's opponents are read before any rating moves.
    std::vector<ArenaTeam::PlacedOpponent> opponents[MAX_BATTLEGROUND_SQUADS];
    for (uint8 squad : squads)
    {
        for (uint8 other : squads)
        {
            if (other == squad)
                continue;

            float const score = place[squad] < place[other] ? 1.0f : (place[squad] > place[other] ? 0.0f : 0.5f);
            opponents[squad].push_back({ GetSquadMatchmakerRating(other), score });
        }

        // The scoreboard sets each squad against the winner, and the winner
        // against whoever finished closest to it.
        uint8 rival = BATTLEGROUND_NO_SQUAD;
        for (uint8 other : squads)
            if (other != squad && (rival == BATTLEGROUND_NO_SQUAD || place[other] < place[rival]))
                rival = other;
        m_SquadScoreboardRival[squad] = rival;
    }

    for (uint8 squad : squads)
    {
        ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad));
        bool const won = squad == winnerSquad;
        int32 ratingChange = 0;
        int32 const matchmakerChange = arenaTeam->PlacedAgainst(GetSquadMatchmakerRating(squad), opponents[squad], won, ratingChange);
        _squadArenaScores[squad].Assign(ratingChange, GetSquadMatchmakerRating(squad), arenaTeam->GetName());

        TC_LOG_DEBUG("bg.arena", "Four Corners rated: squad {} ({}) team {} placed {} of {}, rating {:+}, MMR {:+}.",
            uint32(squad), GetSquadName(squad), arenaTeam->GetId(), place[squad] + 1, uint32(squads.size()), ratingChange, matchmakerChange);

        for (auto const& [guid, participant] : m_Players)
        {
            if (GetPlayerSquad(guid) != squad)
                continue;

            Player* player = _GetPlayer(guid, participant.OfflineRemoveTime != 0, "Arena::EndRatedSquadMatch");
            if (player && won)
            {
                // achievement BEFORE the personal rating moves, as for a two-team win
                uint32 const rating = player->GetArenaPersonalRating(arenaTeam->GetSlot());
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_RATED_ARENA, rating ? rating : 1);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_ARENA, GetMapId());
            }
            else if (player)
                player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);

            arenaTeam->MemberPlaced(player, guid, opponents[squad], matchmakerChange, won);
        }

        arenaTeam->SaveToDB();
        arenaTeam->NotifyStatsChanged();
    }
}

void Arena::EndBattleground(uint32 winner)
{
    // A rated squad match is settled by finishing place, not by two sides.
    if (isRated() && IsSquadMatch())
    {
        EndRatedSquadMatch();
        Battleground::EndBattleground(winner);
        return;
    }

    // arena rating calculation
    if (isRated())
    {
        uint32 loserTeamRating        = 0;
        uint32 loserMatchmakerRating  = 0;
        int32  loserChange            = 0;
        int32  loserMatchmakerChange  = 0;
        uint32 winnerTeamRating       = 0;
        uint32 winnerMatchmakerRating = 0;
        int32  winnerChange           = 0;
        int32  winnerMatchmakerChange = 0;

        // In case of arena draw, follow this logic:
        // winnerArenaTeam => ALLIANCE, loserArenaTeam => HORDE
        ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winner == 0 ? uint32(ALLIANCE) : winner));
        ArenaTeam* loserArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winner == 0 ? uint32(HORDE) : GetOtherTeam(winner)));

        if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
        {
            // In case of arena draw, follow this logic:
            // winnerMatchmakerRating => ALLIANCE, loserMatchmakerRating => HORDE
            loserTeamRating = loserArenaTeam->GetRating();
            loserMatchmakerRating = GetArenaMatchmakerRating(winner == 0 ? uint32(HORDE) : GetOtherTeam(winner));
            winnerTeamRating = winnerArenaTeam->GetRating();
            winnerMatchmakerRating = GetArenaMatchmakerRating(winner == 0 ? uint32(ALLIANCE) : winner);

            if (winner != 0)
            {
                winnerMatchmakerChange = winnerArenaTeam->WonAgainst(winnerMatchmakerRating, loserMatchmakerRating, winnerChange);
                loserMatchmakerChange = loserArenaTeam->LostAgainst(loserMatchmakerRating, winnerMatchmakerRating, loserChange);

                TC_LOG_DEBUG("bg.arena", "match Type: {} --- Winner: old rating: {}, rating gain: {}, old MMR: {}, MMR gain: {} --- Loser: old rating: {}, rating loss: {}, old MMR: {}, MMR loss: {} ---",
                    GetArenaType(), winnerTeamRating, winnerChange, winnerMatchmakerRating, winnerMatchmakerChange,
                    loserTeamRating, loserChange, loserMatchmakerRating, loserMatchmakerChange);

                SetArenaMatchmakerRating(winner, winnerMatchmakerRating + winnerMatchmakerChange);
                SetArenaMatchmakerRating(GetOtherTeam(winner), loserMatchmakerRating + loserMatchmakerChange);

                // bg team that the client expects is different to TeamId
                // alliance 1, horde 0
                uint8 winnerTeam = winner == ALLIANCE ? PVP_TEAM_ALLIANCE : PVP_TEAM_HORDE;
                uint8 loserTeam = winner == ALLIANCE ? PVP_TEAM_HORDE : PVP_TEAM_ALLIANCE;

                _arenaTeamScores[winnerTeam].Assign(winnerChange, winnerMatchmakerRating, winnerArenaTeam->GetName());
                _arenaTeamScores[loserTeam].Assign(loserChange, loserMatchmakerRating, loserArenaTeam->GetName());

                TC_LOG_DEBUG("bg.arena", "Arena match Type: {} for Team1Id: {} - Team2Id: {} ended. WinnerTeamId: {}. Winner rating: +{}, Loser rating: {}",
                    GetArenaType(), GetArenaTeamIdByIndex(TEAM_ALLIANCE), GetArenaTeamIdByIndex(TEAM_HORDE), winnerArenaTeam->GetId(), winnerChange, loserChange);

                if (sWorld->getBoolConfig(CONFIG_ARENA_LOG_EXTENDED_INFO))
                    for (auto const& score : PlayerScores)
                        if (Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, score.first)))
                        {
                            TC_LOG_DEBUG("bg.arena", "Statistics match Type: {} for {} (GUID: {}, Team: {}, IP: {}): {}",
                                GetArenaType(), player->GetName(), score.first, player->GetArenaTeamId(GetArenaType() == 5 ? 2 : GetArenaType() == 3),
                                player->GetSession()->GetRemoteAddress(), score.second->ToString());
                        }
            }
            // Deduct 16 points from each teams arena-rating if there are no winners after 45+2 minutes
            else
            {
                _arenaTeamScores[PVP_TEAM_ALLIANCE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, winnerMatchmakerRating, winnerArenaTeam->GetName());
                _arenaTeamScores[PVP_TEAM_HORDE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, loserMatchmakerRating, loserArenaTeam->GetName());

                winnerArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
                loserArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
            }

            uint8 aliveWinners = GetArenaAlivePlayersCountByTeam(winner);

            for (auto const& i : GetPlayers())
            {
                uint32 team = i.second.Team;

                if (i.second.OfflineRemoveTime)
                {
                    // if rated arena match - make member lost!
                    if (team == winner)
                        winnerArenaTeam->OfflineMemberLost(i.first, loserMatchmakerRating, winnerMatchmakerChange);
                    else
                    {
                        if (winner == 0)
                            winnerArenaTeam->OfflineMemberLost(i.first, loserMatchmakerRating, winnerMatchmakerChange);

                        loserArenaTeam->OfflineMemberLost(i.first, winnerMatchmakerRating, loserMatchmakerChange);
                    }
                    continue;
                }

                Player* player = _GetPlayer(i.first, i.second.OfflineRemoveTime != 0, "Arena::EndBattleground");
                if (!player)
                    continue;

                // per player calculation
                if (team == winner)
                {
                    // update achievement BEFORE personal rating update
                    uint32 rating = player->GetArenaPersonalRating(winnerArenaTeam->GetSlot());
                    player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_RATED_ARENA, rating ? rating : 1);
                    player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_ARENA, GetMapId());

                    // Last standing - Rated 5v5 arena & be solely alive player
                    if (GetArenaType() == ARENA_TYPE_5v5 && aliveWinners == 1 && player->IsAlive())
                        player->CastSpell(player, SPELL_LAST_MAN_STANDING, true);

                    winnerArenaTeam->MemberWon(player, loserMatchmakerRating, winnerMatchmakerChange);
                }
                else
                {
                    if (winner == 0)
                        winnerArenaTeam->MemberLost(player, loserMatchmakerRating, winnerMatchmakerChange);

                    loserArenaTeam->MemberLost(player, winnerMatchmakerRating, loserMatchmakerChange);

                    // Arena lost => reset the win_rated_arena having the "no_lose" condition
                    player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);
                }
            }

            // update previous opponents for arena queue
            winnerArenaTeam->SetPreviousOpponents(loserArenaTeam->GetId());
            loserArenaTeam->SetPreviousOpponents(winnerArenaTeam->GetId());

            // save the stat changes
            winnerArenaTeam->SaveToDB();
            loserArenaTeam->SaveToDB();
            // send updated arena team stats to players
            // this way all arena team members will get notified, not only the ones who participated in this match
            winnerArenaTeam->NotifyStatsChanged();
            loserArenaTeam->NotifyStatsChanged();
        }
    }

    // end battleground
    Battleground::EndBattleground(winner);
}
