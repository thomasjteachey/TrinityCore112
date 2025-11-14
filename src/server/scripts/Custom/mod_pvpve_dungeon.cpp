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

#include "mod_pvpve_dungeon.h"

#include "Duration.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <ctime>
#include <string>

namespace
{
char const* const kTemplateQuery = "SELECT Id, MapId, Enabled, MinLevel, MaxLevel, MaxTeams, MinPlayersPerTeam, MaxPlayersPerTeam "
    "FROM pvpve_dungeon_template";

char const* const kSpawnQuery = "SELECT TemplateId, SpawnIndex, PositionX, PositionY, PositionZ, Orientation "
    "FROM pvpve_dungeon_spawn ORDER BY TemplateId, SpawnIndex";
}

DungeonTemplate const* PvpveDungeonMgr::GetDungeonTemplate(uint32 templateId) const
{
    auto itr = _templates.find(templateId);
    if (itr == _templates.end())
        return nullptr;

    return &itr->second;
}

std::vector<SpawnPoint> const* PvpveDungeonMgr::GetSpawnPoints(uint32 templateId) const
{
    auto itr = _spawns.find(templateId);
    if (itr == _spawns.end())
        return nullptr;

    return &itr->second;
}

bool PvpveDungeonMgr::IsPlayerInPvpveRun(ObjectGuid const& guid) const
{
    return _playerToRun.find(guid) != _playerToRun.end();
}

bool PvpveDungeonMgr::IsPlayerInPvpveRun(Player const* player) const
{
    return player && IsPlayerInPvpveRun(player->GetGUID());
}

PvpveDungeonMgr* PvpveDungeonMgr::Instance()
{
    static PvpveDungeonMgr instance;
    return &instance;
}

PvpveDungeonMgr::PvpveDungeonMgr()
{
    _nextRunId = 1;
    _nextTeamId = 1;
    _queue.clear();
    _runs.clear();
    _teams.clear();
    _playerToRun.clear();
    _playerToTeam.clear();
    _templates.clear();
    _spawns.clear();
}

void PvpveDungeonMgr::LoadConfigFromDB()
{
    _templates.clear();
    _spawns.clear();

    QueryResult templateResult = WorldDatabase.Query(kTemplateQuery);
    if (!templateResult)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: no dungeon templates found in pvpve_dungeon_template.");
    }
    else
    {
        do
        {
            Field* fields = templateResult->Fetch();

            DungeonTemplate entry;
            entry.Id = fields[0].GetUInt32();
            entry.MapId = fields[1].GetUInt16();
            entry.Enabled = fields[2].GetBool();
            entry.MinLevel = fields[3].GetUInt8();
            entry.MaxLevel = fields[4].GetUInt8();
            entry.MaxTeams = std::max<uint8>(1, fields[5].GetUInt8());
            entry.MinPlayers = fields[6].GetUInt8();
            entry.MaxPlayers = fields[7].GetUInt8();

            _templates[entry.Id] = entry;
        }
        while (templateResult->NextRow());

        TC_LOG_INFO("server.custom", "PvpveDungeonMgr: loaded {} dungeon templates.", _templates.size());
    }

    QueryResult spawnResult = WorldDatabase.Query(kSpawnQuery);
    if (!spawnResult)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: no spawn points found in pvpve_dungeon_spawn.");
        return;
    }

    do
    {
        Field* fields = spawnResult->Fetch();

        SpawnPoint spawn;
        spawn.TemplateId = fields[0].GetUInt32();
        spawn.Index = fields[1].GetUInt8();
        spawn.X = fields[2].GetFloat();
        spawn.Y = fields[3].GetFloat();
        spawn.Z = fields[4].GetFloat();
        spawn.O = fields[5].GetFloat();

        _spawns[spawn.TemplateId].push_back(spawn);
    }
    while (spawnResult->NextRow());

    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: loaded spawn point data for {} templates.", _spawns.size());
}

bool PvpveDungeonMgr::QueueTeam(uint32 templateId, std::vector<ObjectGuid> const& memberGuids)
{
    if (memberGuids.empty())
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: cannot queue empty team for template {}.", templateId);
        return false;
    }

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(templateId);
    if (!dungeonTemplate || !dungeonTemplate->Enabled)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: template {} is not available for queueing.", templateId);
        return false;
    }

    if (dungeonTemplate->MinPlayers && memberGuids.size() < dungeonTemplate->MinPlayers)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: team has too few members ({}) for template {} (min {}).",
            memberGuids.size(), templateId, uint32(dungeonTemplate->MinPlayers));
        return false;
    }

    if (dungeonTemplate->MaxPlayers && memberGuids.size() > dungeonTemplate->MaxPlayers)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: team has too many members ({}) for template {} (max {}).",
            memberGuids.size(), templateId, uint32(dungeonTemplate->MaxPlayers));
        return false;
    }

    for (ObjectGuid const& guid : memberGuids)
    {
        if (!guid)
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: refusing to queue team for template {} with invalid member GUID.", templateId);
            return false;
        }

        if (_playerToTeam.find(guid) != _playerToTeam.end())
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: player {} is already assigned to a PvPvE team.", guid.ToString());
            return false;
        }
    }

    PvpveTeam team;
    team.Id = _nextTeamId++;
    team.TemplateId = templateId;
    team.Members = memberGuids;
    team.CreatedTime = std::time(nullptr);
    team.Ready = true;

    auto [teamItr, inserted] = _teams.emplace(team.Id, std::move(team));
    if (!inserted)
    {
        TC_LOG_ERROR("server.custom", "PvpveDungeonMgr: failed to store team {} for template {}.", team.Id, templateId);
        return false;
    }

    for (ObjectGuid const& guid : memberGuids)
        _playerToTeam[guid] = teamItr->first;

    QueueTeam(teamItr->first);
    return true;
}

void PvpveDungeonMgr::QueueTeam(uint64 teamId)
{
    auto teamItr = _teams.find(teamId);
    if (teamItr == _teams.end())
    {
        TC_LOG_ERROR("server.custom", "PvpveDungeonMgr: unable to queue unknown team {}.", teamId);
        return;
    }

    QueuedTeam queued;
    queued.TeamId = teamId;
    queued.TemplateId = teamItr->second.TemplateId;
    queued.QueueTime = std::time(nullptr);
    queued.Members = teamItr->second.Members;
    queued.Ready = teamItr->second.Ready;

    auto [itr, inserted] = _queue.insert_or_assign(teamId, queued);
    if (inserted)
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: queued team {} for template {}.", teamId, queued.TemplateId);
    else
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: updated queue entry for team {}.", teamId);
}

void PvpveDungeonMgr::CancelQueue(uint64 teamId)
{
    if (_queue.erase(teamId))
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: removed team {} from the queue.", teamId);
}

void PvpveDungeonMgr::Update(uint32 /*diff*/)
{
    if (!_queue.empty())
    {
        // TrinityCore executes world updates on a single thread; no explicit locking needed here.
        std::vector<uint64> queuedIds;
        queuedIds.reserve(_queue.size());
        for (auto const& entry : _queue)
            queuedIds.push_back(entry.first);

        for (uint64 teamId : queuedIds)
        {
            auto queueItr = _queue.find(teamId);
            if (queueItr == _queue.end())
                continue;

            auto teamItr = _teams.find(teamId);
            if (teamItr == _teams.end())
            {
                TC_LOG_WARN("server.custom", "PvpveDungeonMgr: dropping non-existent team {} from queue.", teamId);
                _queue.erase(queueItr);
                continue;
            }

            DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(teamItr->second.TemplateId);
            if (!dungeonTemplate || !dungeonTemplate->Enabled)
            {
                TC_LOG_WARN("server.custom", "PvpveDungeonMgr: template {} not available for team {}.", teamItr->second.TemplateId, teamId);
                _queue.erase(queueItr);
                continue;
            }

            PvpveDungeonRun* selectedRun = nullptr;
            for (auto& runPair : _runs)
            {
                PvpveDungeonRun& candidate = runPair.second;
                if (candidate.TemplateId != dungeonTemplate->Id)
                    continue;

                if (candidate.Completed)
                    continue;

                if (candidate.Teams.size() < dungeonTemplate->MaxTeams)
                {
                    selectedRun = &candidate;
                    break;
                }
            }

            if (!selectedRun)
            {
                PvpveDungeonRun newRun;
                newRun.Id = _nextRunId++;
                newRun.TemplateId = dungeonTemplate->Id;
                newRun.CreatedTime = std::time(nullptr);
                auto [runItr, inserted] = _runs.emplace(newRun.Id, std::move(newRun));
                selectedRun = &runItr->second;
                if (inserted)
                    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: created new run {} for template {}.", selectedRun->Id, dungeonTemplate->Id);
            }

            AssignTeamToRun(*selectedRun, queueItr->second);
            _queue.erase(queueItr);
        }
    }

    for (auto& runPair : _runs)
        EvaluateRunState(runPair.second);
}

void PvpveDungeonMgr::AssignTeamToRun(PvpveDungeonRun& run, QueuedTeam const& queued)
{
    if (queued.Members.empty())
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: queued team {} has no members for run {}.", queued.TeamId, run.Id);
        return;
    }

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run.TemplateId);
    if (!dungeonTemplate)
    {
        TC_LOG_ERROR("server.custom", "PvpveDungeonMgr: unable to assign team {} to run {} because template {} is missing.", queued.TeamId, run.Id, run.TemplateId);
        return;
    }

    std::vector<SpawnPoint> const* spawnList = GetSpawnPoints(run.TemplateId);
    if (!spawnList || spawnList->empty())
    {
        TC_LOG_ERROR("server.custom", "PvpveDungeonMgr: run {} for template {} has no spawn points configured.", run.Id, run.TemplateId);
        return;
    }

    uint8 spawnIndex = PickSpawnIndex(run.TemplateId);
    auto spawnItr = std::find_if(spawnList->begin(), spawnList->end(), [spawnIndex](SpawnPoint const& spawn)
    {
        return spawn.Index == spawnIndex;
    });

    if (spawnItr == spawnList->end())
    {
        TC_LOG_ERROR("server.custom", "PvpveDungeonMgr: spawn index {} not found for template {} (run {}).", uint32(spawnIndex), run.TemplateId, run.Id);
        return;
    }

    PvpveTeam team;
    team.Id = queued.TeamId ? queued.TeamId : _nextTeamId++;
    team.TemplateId = run.TemplateId;
    team.Members = queued.Members;
    team.SpawnIndex = spawnIndex;
    team.CreatedTime = std::time(nullptr);
    team.Ready = queued.Ready;

    Group* group = nullptr;
    if (!run.GroupGuid.IsEmpty())
        group = sGroupMgr->GetGroupByGUID(run.GroupGuid.GetCounter());

    Player* leader = nullptr;
    for (ObjectGuid const& guid : team.Members)
    {
        leader = ObjectAccessor::FindPlayer(guid);
        if (leader)
            break;
    }

    if (!leader)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: unable to find an online leader for team {} in run {}.", team.Id, run.Id);
        return;
    }

    if (!group)
    {
        group = new Group();
        if (!group->Create(leader))
        {
            TC_LOG_ERROR("server.custom", "PvpveDungeonMgr: failed to create raid group for run {} (team {}).", run.Id, team.Id);
            delete group;
            return;
        }

        group->ConvertToRaid();
        sGroupMgr->AddGroup(group);
        run.GroupGuid = group->GetGUID();
    }
    else if (!group->isRaidGroup())
        group->ConvertToRaid();

    auto addToRaid = [group, runId = run.Id](Player* player) -> bool
    {
        if (group->IsMember(player->GetGUID()))
            return true;

        if (group->AddMember(player))
            return true;

        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: failed to add player {} to raid for run {}.", player->GetGUID().ToString(), runId);
        return false;
    };

    for (ObjectGuid const& memberGuid : team.Members)
    {
        Player* player = ObjectAccessor::FindPlayer(memberGuid);
        if (!player)
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: unable to find player {} for team {} in run {}.", memberGuid.ToString(), team.Id, run.Id);
            continue;
        }

        if (!addToRaid(player))
            continue;

        if (!player->TeleportTo(dungeonTemplate->MapId, spawnItr->X, spawnItr->Y, spawnItr->Z, spawnItr->O))
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: teleport failed for player {} joining run {}.", memberGuid.ToString(), run.Id);

        if (std::find(run.Players.begin(), run.Players.end(), memberGuid) == run.Players.end())
            run.Players.push_back(memberGuid);

        run.PlayerSpawns[memberGuid] = spawnIndex;
        _playerToRun[memberGuid] = run.Id;
        _playerToTeam[memberGuid] = team.Id;
    }

    run.Teams.push_back(team.Id);
    _teams[team.Id] = std::move(team);
}

PvpveDungeonRun* PvpveDungeonMgr::GetRun(uint64 runId)
{
    auto itr = _runs.find(runId);
    if (itr == _runs.end())
        return nullptr;

    return &itr->second;
}

PvpveTeam* PvpveDungeonMgr::GetTeam(uint64 teamId)
{
    auto itr = _teams.find(teamId);
    if (itr == _teams.end())
        return nullptr;

    return &itr->second;
}

PvpveDungeonRun* PvpveDungeonMgr::GetRunForPlayer(ObjectGuid const& guid)
{
    auto itr = _playerToRun.find(guid);
    if (itr == _playerToRun.end())
        return nullptr;

    return GetRun(itr->second);
}

uint8 PvpveDungeonMgr::PickSpawnIndex(uint32 templateId)
{
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: TODO pick spawn index for template {}.", templateId);
    return 0;
}

void PvpveDungeonMgr::OnInstanceCreated(Map* /*map*/)
{
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: TODO handle instance creation.");
}

bool PvpveDungeonMgr::TeamHasActiveMembers(PvpveTeam const& team, DungeonTemplate const* dungeonTemplate) const
{
    if (!dungeonTemplate)
        return false;

    for (ObjectGuid const& memberGuid : team.Members)
    {
        Player* member = ObjectAccessor::FindPlayer(memberGuid);
        if (!member)
            continue;

        if (member->GetMapId() != dungeonTemplate->MapId)
            continue;

        if (!member->IsAlive())
            continue;

        return true;
    }

    return false;
}

void PvpveDungeonMgr::EvaluateRunState(PvpveDungeonRun& run)
{
    if (run.Completed || run.Finished)
        return;

    if (run.Teams.empty())
        return;

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run.TemplateId);
    if (!dungeonTemplate)
        return;

    uint32 activeTeams = 0;
    for (uint64 teamId : run.Teams)
    {
        PvpveTeam* team = GetTeam(teamId);
        if (!team)
            continue;

        if (!TeamHasActiveMembers(*team, dungeonTemplate))
        {
            if (!team->Eliminated)
            {
                team->Eliminated = true;
                TC_LOG_INFO("server.custom", "PvpveDungeonMgr: team {} eliminated in run {} (no active members).", team->Id, run.Id);
            }

            continue;
        }

        if (!team->Eliminated)
            ++activeTeams;
    }

    bool shouldFinish = false;
    if (run.Teams.size() > 1 && activeTeams <= 1)
        shouldFinish = true;
    else if (run.Teams.size() == 1 && activeTeams == 0)
        shouldFinish = true;

    if (shouldFinish)
        FinishRun(run);
}

void PvpveDungeonMgr::OnPlayerEliminated(Player* player)
{
    if (!player)
        return;

    ObjectGuid const guid = player->GetGUID();

    auto runItr = _playerToRun.find(guid);
    if (runItr == _playerToRun.end())
        return;

    PvpveDungeonRun* run = GetRun(runItr->second);
    if (!run)
        return;

    auto teamItr = _playerToTeam.find(guid);
    if (teamItr == _playerToTeam.end())
        return;

    PvpveTeam* team = GetTeam(teamItr->second);
    if (!team || team->Eliminated)
        return;

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run->TemplateId);
    if (!dungeonTemplate)
        return;

    if (TeamHasActiveMembers(*team, dungeonTemplate))
        return;

    team->Eliminated = true;
    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: team {} eliminated in run {} (triggered by player {}).", team->Id, run->Id, guid.ToString());

    EvaluateRunState(*run);
}

void PvpveDungeonMgr::OnPlayerLeftMap(Player* player)
{
    if (!player)
        return;

    if (!IsPlayerInPvpveRun(player))
        return;

    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: player {} left PvPvE map, checking elimination state.", player->GetGUID().ToString());
    OnPlayerEliminated(player);
}

void PvpveDungeonMgr::FinishRun(PvpveDungeonRun& run)
{
    if (run.Finished)
        return;

    run.Finished = true;
    run.Completed = true;

    std::vector<uint64> winningTeams;
    winningTeams.reserve(run.Teams.size());
    for (uint64 teamId : run.Teams)
    {
        PvpveTeam* team = GetTeam(teamId);
        if (team && !team->Eliminated)
            winningTeams.push_back(teamId);
    }

    if (winningTeams.empty())
    {
        TC_LOG_INFO("server.custom", "PvpveDungeonMgr: run {} ended with no surviving teams.", run.Id);
    }
    else
    {
        std::string winnersList;
        for (uint64 id : winningTeams)
        {
            if (!winnersList.empty())
                winnersList += ", ";
            winnersList += std::to_string(id);
        }

        TC_LOG_INFO("server.custom", "PvpveDungeonMgr: run {} finished. Winning teams: {}.", run.Id, winnersList);
    }

    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: TODO notify instance scripts and distribute rewards for run {}.", run.Id);
}

namespace
{
struct TeleportDestination
{
    uint32 MapId;
    float X;
    float Y;
    float Z;
    float O;
};

TeleportDestination const kAllianceTeleportDestination{ 0, -8833.38f, 628.62f, 94.0066f, 1.0646f };
TeleportDestination const kHordeTeleportDestination{ 1, 1633.33f, -4439.09f, 15.999f, 5.3178f };

class PvpveDungeonPlayerScript : public PlayerScript
{
public:
    PvpveDungeonPlayerScript() : PlayerScript("pvpve_dungeon_player") { }

    void OnPVPKill(Player* /*killer*/, Player* killed) override
    {
        HandlePlayerDeath(killed);
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* killed) override
    {
        HandlePlayerDeath(killed);
    }

    void OnPlayerRepop(Player* player) override
    {
        if (!player)
            return;

        if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
        {
            ClearPendingState(player->GetGUID());
            return;
        }

        if (MarkPlayerEliminated(player))
            sPvpveDungeonMgr->OnPlayerEliminated(player);

        ScheduleTeleportOut(player);
    }

    void OnMapChanged(Player* player) override
    {
        if (!player)
            return;

        ObjectGuid const guid = player->GetGUID();
        PvpveDungeonRun* run = sPvpveDungeonMgr->GetRunForPlayer(guid);
        if (!run)
        {
            ClearPendingState(guid);
            return;
        }

        DungeonTemplate const* dungeonTemplate = sPvpveDungeonMgr->GetDungeonTemplate(run->TemplateId);
        if (!dungeonTemplate)
        {
            ClearPendingState(guid);
            return;
        }

        if (player->GetMapId() != dungeonTemplate->MapId)
        {
            sPvpveDungeonMgr->OnPlayerLeftMap(player);
            ClearPendingState(guid);
        }
    }

private:
    void HandlePlayerDeath(Player* player)
    {
        if (!player)
            return;

        if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
        {
            ClearPendingState(player->GetGUID());
            return;
        }

        bool const firstElimination = MarkPlayerEliminated(player);
        if (firstElimination)
            sPvpveDungeonMgr->OnPlayerEliminated(player);

        ForceRelease(player);
        ScheduleTeleportOut(player);
    }

    bool MarkPlayerEliminated(Player* player)
    {
        if (!player)
            return false;

        return _eliminatedPlayers.insert(player->GetGUID()).second;
    }

    void ForceRelease(Player* player)
    {
        if (!player)
            return;

        if (!player->IsAlive() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            player->RepopAtGraveyard();
    }

    TeleportDestination GetTeleportLocation(Player const* player) const
    {
        if (!player)
            return kAllianceTeleportDestination;

        return player->GetTeamId() == TEAM_ALLIANCE ? kAllianceTeleportDestination : kHordeTeleportDestination;
    }

    void ScheduleTeleportOut(Player* player)
    {
        if (!player)
            return;

        ObjectGuid const guid = player->GetGUID();
        if (!_pendingTeleport.insert(guid).second)
            return;

        player->m_Events.AddEventAtOffset([this, guid]()
        {
            _pendingTeleport.erase(guid);

            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player)
                return;

            if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
            {
                ClearPendingState(guid);
                return;
            }

            if (!player->IsAlive() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                player->RepopAtGraveyard();

            TeleportDestination const destination = GetTeleportLocation(player);
            player->TeleportTo(destination.MapId, destination.X, destination.Y, destination.Z, destination.O);
        }, 1s);
    }

    void ClearPendingState(ObjectGuid const& guid)
    {
        _eliminatedPlayers.erase(guid);
        _pendingTeleport.erase(guid);
    }

    GuidSet _eliminatedPlayers;
    GuidSet _pendingTeleport;
};

struct PvpveDungeonWorldScript : WorldScript
{
    PvpveDungeonWorldScript() : WorldScript("pvpve_dungeon_world") { }

    void OnStartup() override
    {
        PvpveDungeonMgr::instance()->LoadConfigFromDB();
    }

    void OnUpdate(uint32 diff) override
    {
        PvpveDungeonMgr::instance()->Update(diff);
    }
};
}

void AddSC_npc_pvpve_dungeon_queue();

void AddSC_custom_pvpve_dungeon()
{
    new PvpveDungeonPlayerScript();
    new PvpveDungeonWorldScript();
    AddSC_npc_pvpve_dungeon_queue();
}
