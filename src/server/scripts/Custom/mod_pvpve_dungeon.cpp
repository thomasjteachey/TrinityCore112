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

#include "DatabaseEnv.h"
#include "Log.h"

#include <algorithm>
#include <ctime>

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
    if (_queue.empty())
        return;

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

        selectedRun->Teams.push_back(teamId);
        AssignTeamToRun(*selectedRun, teamItr->second);
        _queue.erase(queueItr);
    }
}

void PvpveDungeonMgr::AssignTeamToRun(PvpveDungeonRun& run, PvpveTeam& team)
{
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: TODO assign team {} to run {}.", team.Id ? team.Id : run.Teams.back(), run.Id);
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

void PvpveDungeonMgr::OnPlayerEliminated(Player* /*player*/)
{
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: TODO handle player elimination.");
}

void PvpveDungeonMgr::OnPlayerLeftMap(Player* /*player*/)
{
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: TODO handle player leaving map.");
}
