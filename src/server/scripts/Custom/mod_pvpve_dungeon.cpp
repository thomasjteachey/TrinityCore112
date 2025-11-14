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
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"

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

        AssignTeamToRun(*selectedRun, queueItr->second);
        _queue.erase(queueItr);
    }
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

namespace
{
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
    new PvpveDungeonWorldScript();
    AddSC_npc_pvpve_dungeon_queue();
}
