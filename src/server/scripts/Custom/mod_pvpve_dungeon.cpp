/*
 * This file is part of the the TrinityCore Project. See AUTHORS file for Copyright information
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

#include "Chat.h"
#include "Duration.h"
#include "DatabaseEnv.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MapInstanced.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <ctime>
#include <set>
#include <string>

namespace
{
    char const* const kTemplateQuery = "SELECT Id, MapId, Enabled, MinLevel, MaxLevel, MaxTeams, MinPlayersPerTeam, MaxPlayersPerTeam, MaxRuntimeSecs "
        "FROM pvpve_dungeon_template";

    char const* const kSpawnQuery = "SELECT TemplateId, SpawnIndex, PositionX, PositionY, PositionZ, Orientation "
        "FROM pvpve_dungeon_spawn ORDER BY TemplateId, SpawnIndex";

    constexpr uint32 kPvpveFfaAuraSpellId = 0;
    constexpr UnitPVPStateFlags kPvpveFfaPvpFlag = UNIT_BYTE2_FLAG_FFA_PVP;

    void SnapPetToLocation(Player* player, uint32 mapId, float x, float y, float z, float orientation)
    {
        if (!player)
            return;

        Pet* pet = player->GetPet();
        if (!pet)
            return;

        ObjectGuid const ownerGuid = player->GetGUID();
        ObjectGuid const petGuid = pet->GetGUID();

        player->m_Events.AddEventAtOffset([ownerGuid, petGuid, mapId, x, y, z, orientation]()
        {
            Player* owner = ObjectAccessor::FindPlayer(ownerGuid);
            if (!owner)
                return;

            Pet* ownedPet = owner->GetPet();
            if (!ownedPet || ownedPet->GetGUID() != petGuid)
                return;

            if (ownedPet->GetMapId() != mapId)
                return;

            ownedPet->NearTeleportTo(x, y, z, orientation);
        }, 250ms);
    }
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

bool PvpveDungeonMgr::IsPvpveDungeonMap(uint32 mapId) const
{
    if (!mapId)
        return false;

    for (auto const& templatePair : _templates)
    {
        DungeonTemplate const& dungeonTemplate = templatePair.second;
        if (!dungeonTemplate.Enabled)
            continue;

        if (dungeonTemplate.MapId == mapId)
            return true;
    }

    return false;
}

void PvpveDungeonMgr::OnPlayerEnteredInstance(Player* player, PvpveDungeonInstance* instanceScript)
{
    if (!player || !instanceScript)
        return;

    auto runItr = _playerToRun.find(player->GetGUID());
    if (runItr == _playerToRun.end())
        return;

    PvpveDungeonRun* run = GetRun(runItr->second);
    if (!run)
        return;

    if (!run->InstanceMap)
        run->InstanceMap = player->GetMap();

    run->InstanceScript = instanceScript;

    // Allow PvPvE participants to remain inside the dungeon without being
    // ejected for not being in the instance owner?s party.
    player->m_InstanceValid = true;
    player->SetInstanceValidityOverride(true);

    if (WorldSession* session = player->GetSession())
    {
        uint32 instanceId = player->GetInstanceId();
        if (!instanceId)
            if (Map* map = player->GetMap())
                instanceId = map->GetInstanceId();

        if (instanceId)
            ChatHandler(session).PSendSysMessage("PvPvE Stockades instance ID: %u", instanceId);
    }
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
    _playerRunLockouts.clear();
    _templates.clear();
    _spawns.clear();
    _queuedPlayers.clear();
    _playerReturnLocations.clear();
    _lastStatsLog = 0;
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
            entry.MaxTeams = fields[5].GetUInt8();
            entry.MinPlayers = fields[6].GetUInt8();
            entry.MaxPlayers = fields[7].GetUInt8();
            entry.MaxRuntimeSecs = fields[8].GetUInt32();

            _templates[entry.Id] = entry;
        } while (templateResult->NextRow());

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
    } while (spawnResult->NextRow());

    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: loaded spawn point data for {} templates.", _spawns.size());
}

bool PvpveDungeonMgr::QueueTeam(uint32 templateId, std::vector<ObjectGuid> const& memberGuids, uint64 preferredRunId)
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

        if (_playerToRun.find(guid) != _playerToRun.end())
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: player {} is already participating in a PvPvE run.", guid.ToString());
            return false;
        }

        if (_queuedPlayers.find(guid) != _queuedPlayers.end())
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: player {} is already queued for a PvPvE run.", guid.ToString());
            return false;
        }

        if (_playerToTeam.find(guid) != _playerToTeam.end())
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: player {} is already assigned to a PvPvE team.", guid.ToString());
            return false;
        }

        if (preferredRunId)
        {
            auto lockoutItr = _playerRunLockouts.find(guid);
            if (lockoutItr != _playerRunLockouts.end() && lockoutItr->second == preferredRunId)
            {
                TC_LOG_WARN("server.custom", "PvpveDungeonMgr: player {} was eliminated from run {} and cannot rejoin it.", guid.ToString(), preferredRunId);
                return false;
            }
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

    uint64 const teamId = teamItr->first;
    for (ObjectGuid const& guid : memberGuids)
        _playerToTeam[guid] = teamId;

    if (!QueueTeam(teamId, preferredRunId))
    {
        for (ObjectGuid const& guid : memberGuids)
            _playerToTeam.erase(guid);

        _teams.erase(teamItr);
        return false;
    }

    return true;
}

bool PvpveDungeonMgr::QueueTeam(uint64 teamId, uint64 preferredRunId)
{
    auto teamItr = _teams.find(teamId);
    if (teamItr == _teams.end())
    {
        TC_LOG_ERROR("server.custom", "PvpveDungeonMgr: unable to queue unknown team {}.", teamId);
        return false;
    }

    if (preferredRunId)
    {
        for (ObjectGuid const& guid : teamItr->second.Members)
        {
            if (!guid)
                continue;

            auto lockoutItr = _playerRunLockouts.find(guid);
            if (lockoutItr != _playerRunLockouts.end() && lockoutItr->second == preferredRunId)
            {
                TC_LOG_WARN("server.custom", "PvpveDungeonMgr: cannot queue team {} for run {}; player {} has already been eliminated from it.",
                    teamId, preferredRunId, guid.ToString());
                return false;
            }
        }
    }

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(teamItr->second.TemplateId);
    if (!dungeonTemplate || !dungeonTemplate->Enabled)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: template {} not available for team {}.", teamItr->second.TemplateId, teamId);
        return false;
    }

    for (ObjectGuid const& guid : teamItr->second.Members)
    {
        if (!guid)
            continue;

        if (_playerToRun.find(guid) != _playerToRun.end())
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: cannot queue team {}; player {} already in an active run.", teamId, guid.ToString());
            return false;
        }

        if (_queuedPlayers.find(guid) != _queuedPlayers.end())
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: cannot queue team {}; player {} already queued.", teamId, guid.ToString());
            return false;
        }
    }

    QueuedTeam queued;
    queued.TeamId = teamId;
    queued.TemplateId = teamItr->second.TemplateId;
    queued.QueueTime = std::time(nullptr);
    queued.Members = teamItr->second.Members;
    queued.Ready = teamItr->second.Ready;
    queued.PreferredRunId = preferredRunId;

    auto queueItr = _queue.find(teamId);
    if (queueItr != _queue.end())
    {
        UntrackQueuedMembers(queueItr->second.Members);
        queueItr->second = queued;
    }
    else
    {
        queueItr = _queue.emplace(teamId, queued).first;
    }

    TrackQueuedMembers(queueItr->second.Members);

    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: queued team {} for template {} (queue size: {}).", teamId, queued.TemplateId, _queue.size());
    return true;
}

void PvpveDungeonMgr::CancelQueue(uint64 teamId)
{
    auto queueItr = _queue.find(teamId);
    if (queueItr == _queue.end())
        return;

    UntrackQueuedMembers(queueItr->second.Members);
    _queue.erase(queueItr);
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: removed team {} from the queue (queue size: {}).", teamId, _queue.size());
}

void PvpveDungeonMgr::Update(uint32 /*diff*/)
{
    time_t const now = std::time(nullptr);

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
                UntrackQueuedMembers(queueItr->second.Members);
                _queue.erase(queueItr);
                continue;
            }

            DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(teamItr->second.TemplateId);
            if (!dungeonTemplate || !dungeonTemplate->Enabled)
            {
                TC_LOG_WARN("server.custom", "PvpveDungeonMgr: template {} not available for team {}.", teamItr->second.TemplateId, teamId);
                UntrackQueuedMembers(queueItr->second.Members);
                _queue.erase(queueItr);
                continue;
            }

            auto const runEligible = [&](PvpveDungeonRun& candidate)
            {
                if (candidate.TemplateId != dungeonTemplate->Id)
                    return false;

                // IMPORTANT: a run is only considered "ended" for queue/invasion
                // once it is marked Finished. Boss kills and team eliminations
                // no longer automatically finish the run.
                if (candidate.Finished)
                    return false;

                if (dungeonTemplate->MaxTeams && candidate.Teams.size() >= dungeonTemplate->MaxTeams)
                    return false;

                std::vector<SpawnPoint> const* candidateSpawns = GetSpawnPoints(candidate.TemplateId);
                if (!candidateSpawns || candidateSpawns->empty())
                    return false;

                std::set<uint8> usedIndices = candidate.UsedSpawnIndices;
                if (usedIndices.empty())
                {
                    for (uint64 teamId : candidate.Teams)
                    {
                        if (PvpveTeam* team = GetTeam(teamId))
                            usedIndices.insert(team->SpawnIndex);
                    }
                }

                if (usedIndices.size() >= candidateSpawns->size())
                    return false;

                bool const memberHasLockout = std::any_of(queueItr->second.Members.begin(), queueItr->second.Members.end(),
                    [this, &candidate](ObjectGuid const& guid)
                {
                    auto lockoutItr = _playerRunLockouts.find(guid);
                    return lockoutItr != _playerRunLockouts.end() && lockoutItr->second == candidate.Id;
                });

                return !memberHasLockout;
            };

            PvpveDungeonRun* selectedRun = nullptr;
            if (queueItr->second.PreferredRunId)
            {
                auto runItr = _runs.find(queueItr->second.PreferredRunId);
                if (runItr != _runs.end() && runEligible(runItr->second))
                    selectedRun = &runItr->second;
            }

            if (!selectedRun)
            {
                for (auto& runPair : _runs)
                {
                    PvpveDungeonRun& candidate = runPair.second;
                    if (!runEligible(candidate))
                        continue;

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
                newRun.InstanceId = 0;
                newRun.InstanceMap = nullptr;
                newRun.InstanceScript = nullptr;
                newRun.StartTime = 0;
                newRun.Active = false;
                newRun.Completed = false;
                newRun.Finished = false;
                newRun.BossDefeated = false;
                newRun.TimeoutWarningSent = false;
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
    {
        EvaluateRunState(runPair.second);
        CheckRunRuntime(runPair.second, now);
    }

    if (now != _lastStatsLog)
    {
        _lastStatsLog = now;
        LogQueueStats(now);
    }

    ProcessTeamEliminationTimers(now);
    MaintainActivePlayerPvpState();
}

void PvpveDungeonMgr::AssignTeamToRun(PvpveDungeonRun& run, QueuedTeam const& queued)
{
    if (queued.Members.empty())
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: queued team {} has no members for run {}.", queued.TeamId, run.Id);
        return;
    }

    UntrackQueuedMembers(queued.Members);

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

    uint32 const runInstanceId = run.InstanceId ? run.InstanceId : (run.InstanceMap ? run.InstanceMap->GetInstanceId() : 0u);
    InstanceSave* instanceSave = nullptr;
    if (runInstanceId)
    {
        instanceSave = sInstanceSaveMgr->GetInstanceSave(runInstanceId);
        if (!instanceSave)
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: run {} is tracking instance {} but no InstanceSave exists yet.", run.Id, runInstanceId);
    }

    uint8 spawnIndex = 0;
    if (!PickSpawnIndex(run, spawnIndex))
    {
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: run {} has exhausted its spawn points; queueing team {} elsewhere.", run.Id, queued.TeamId);
        return;
    }

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

    run.UsedSpawnIndices.insert(spawnIndex);

    for (ObjectGuid const& memberGuid : team.Members)
    {
        Player* player = ObjectAccessor::FindPlayer(memberGuid);
        if (!player)
        {
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: unable to find player {} for team {} in run {}.", memberGuid.ToString(), team.Id, run.Id);
            continue;
        }

        player->SetInstanceValidityOverride(true);
        if (instanceSave)
            player->BindToInstance(instanceSave, false);

        StoreReturnLocation(player);

        if (!player->TeleportTo(dungeonTemplate->MapId, spawnItr->X, spawnItr->Y, spawnItr->Z, spawnItr->O))
            TC_LOG_WARN("server.custom", "PvpveDungeonMgr: teleport failed for player {} joining run {}.", memberGuid.ToString(), run.Id);
        else
            SnapPetToLocation(player, dungeonTemplate->MapId, spawnItr->X, spawnItr->Y, spawnItr->Z, spawnItr->O);

        if (std::find(run.Players.begin(), run.Players.end(), memberGuid) == run.Players.end())
            run.Players.push_back(memberGuid);

        run.PlayerSpawns[memberGuid] = spawnIndex;
        _playerToRun[memberGuid] = run.Id;
        _playerToTeam[memberGuid] = team.Id;
    }

    run.Teams.push_back(team.Id);
    _teams[team.Id] = std::move(team);

    if (!run.StartTime)
    {
        run.StartTime = std::time(nullptr);
        run.Active = true;
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: run {} started at {} (template {}).", run.Id, uint64(run.StartTime), run.TemplateId);
    }
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

PvpveDungeonRun* PvpveDungeonMgr::GetRunForTeam(uint64 teamId)
{
    for (auto& runPair : _runs)
    {
        auto teamItr = std::find(runPair.second.Teams.begin(), runPair.second.Teams.end(), teamId);
        if (teamItr != runPair.second.Teams.end())
            return &runPair.second;
    }

    return nullptr;
}

bool PvpveDungeonMgr::PickSpawnIndex(PvpveDungeonRun const& run, uint8& outIndex)
{
    auto spawnList = GetSpawnPoints(run.TemplateId);
    if (!spawnList || spawnList->empty())
        return false;

    std::set<uint8> usedIndices = run.UsedSpawnIndices;
    if (usedIndices.empty())
    {
        for (uint64 teamId : run.Teams)
        {
            if (PvpveTeam* team = const_cast<PvpveDungeonMgr*>(this)->GetTeam(teamId))
                usedIndices.insert(team->SpawnIndex);
        }
    }

    std::vector<uint8> available;
    available.reserve(spawnList->size());
    for (SpawnPoint const& spawn : *spawnList)
    {
        if (!usedIndices.count(spawn.Index))
            available.push_back(spawn.Index);
    }

    if (available.empty())
        return false;

    outIndex = available[urand(0, available.size() - 1)];
    return true;
}

void PvpveDungeonMgr::HandleServerShutdown()
{
    if (_playerToRun.empty())
        return;

    std::vector<ObjectGuid> participants;
    participants.reserve(_playerToRun.size());
    for (auto const& entry : _playerToRun)
        participants.push_back(entry.first);

    for (ObjectGuid const& guid : participants)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player)
            continue;

        OnPlayerLeftMap(player);
    }
}

void PvpveDungeonMgr::OnInstanceCreated(uint32 templateId, uint64 runId, uint32 instanceId)
{
    if (!instanceId)
        return;

    PvpveDungeonRun* run = GetRun(runId);
    if (!run || run->TemplateId != templateId)
        return;

    if (run->InstanceId == instanceId)
        return;

    if (run->InstanceId && run->InstanceId != instanceId)
    {
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: run {} already tracked with instance {} (new {}).", run->Id, run->InstanceId, instanceId);
        return;
    }

    run->InstanceId = instanceId;
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: tracking instance {} for run {}.", run->InstanceId, run->Id);
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
    // IMPORTANT:
    //
    // We NO LONGER finish the run here when all teams are eliminated.
    // A PvPvE run is considered "ended" (Finished=true) only when the last
    // PvPvE participant actually leaves the instance (see OnPlayerLeftMap).
    //
    // This makes it possible to queue and invade a run as long as at least
    // one player is still in the dungeon, regardless of boss state.
    if (run.Finished)
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
        if (!team || team->Eliminated)
            continue;

        if (!TeamHasActiveMembers(*team, dungeonTemplate))
            continue;

        ++activeTeams;
    }

    if (!activeTeams)
    {
        TC_LOG_INFO("server.custom",
            "PvpveDungeonMgr: run {} currently has no active (alive) teams; waiting for players to leave the instance before finishing the run.",
            run.Id);
    }
}

void PvpveDungeonMgr::OnPlayerDeath(Player* player)
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
    {
        _teamEliminationDeadlines.erase(team->Id);
        return;
    }

    if (_teamEliminationDeadlines.find(team->Id) != _teamEliminationDeadlines.end())
        return;

    time_t const deadline = std::time(nullptr) + 10;
    _teamEliminationDeadlines[team->Id] = deadline;
    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: team {} has no living members in run {}; elimination in 10 seconds unless they recover.", team->Id, run->Id);
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
    _teamEliminationDeadlines.erase(team->Id);
    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: team {} eliminated in run {} (triggered by player {}).", team->Id, run->Id, guid.ToString());

    EvaluateRunState(*run);
}

void PvpveDungeonMgr::OnPlayerLeftMap(Player* player)
{
    if (!player)
        return;

    if (!IsPlayerInPvpveRun(player))
        return;

    WorldLocation savedLocation;
    bool hasSavedLocation = false;
    if (WorldLocation const* storedLocation = GetReturnLocation(player->GetGUID()))
    {
        savedLocation = *storedLocation;
        hasSavedLocation = true;
        ClearReturnLocation(player->GetGUID());
    }

    ClearPvpveFfaState(player);
    player->SetInstanceValidityOverride(false);

    ObjectGuid const guid = player->GetGUID();
    auto runItr = _playerToRun.find(guid);
    if (runItr == _playerToRun.end())
        return;

    PvpveDungeonRun* run = GetRun(runItr->second);
    if (!run)
    {
        _playerToRun.erase(runItr);
        return;
    }

    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: player {} left PvPvE map, treating as elimination in run {}.", guid.ToString(), run->Id);

    // Mark elimination for this player / team
    OnPlayerEliminated(player);

    // Remove from run-level tracking
    auto playerListItr = std::find(run->Players.begin(), run->Players.end(), guid);
    if (playerListItr != run->Players.end())
        run->Players.erase(playerListItr);

    run->PlayerSpawns.erase(guid);
    _playerToRun.erase(runItr);
    _playerToTeam.erase(guid);
    ClearReturnLocation(guid);

    bool const lastPlayer = run->Players.empty();

    // Re-evaluate team states (but do NOT end the run here)
    EvaluateRunState(*run);

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run->TemplateId);

    if (lastPlayer && !run->Finished)
    {
        // This is the moment the run is considered truly "ended" for gameplay.
        // Decide a preferred winner if the boss was defeated and a non-eliminated
        // team still exists.
        uint64 preferredWinner = 0;
        if (run->BossDefeated)
        {
            for (uint64 teamId : run->Teams)
            {
                PvpveTeam* team = GetTeam(teamId);
                if (team && !team->Eliminated)
                {
                    preferredWinner = teamId;
                    break;
                }
            }
        }

        TC_LOG_INFO("server.custom",
            "PvpveDungeonMgr: player {} was the last participant in run {}; marking run finished (preferred winner: {}).",
            guid.ToString(), run->Id, preferredWinner);

        FinishRun(*run, preferredWinner);

        // For the last player leaving a finished run, clear their lockout
        // and unbind the instance.
        if (dungeonTemplate)
        {
            player->UnbindInstance(dungeonTemplate->MapId, player->GetDifficulty(false));
            _playerRunLockouts.erase(guid);
        }
        else
            _playerRunLockouts.erase(guid);
    }
    else
    {
        // Run is still in progress; this player may not rejoin the same run.
        if (dungeonTemplate)
            _playerRunLockouts[guid] = run->Id;
        else
            _playerRunLockouts[guid] = run->Id;
    }

    if (hasSavedLocation && player->IsInWorld())
    {
        player->TeleportTo(savedLocation.GetMapId(),
            savedLocation.GetPositionX(), savedLocation.GetPositionY(), savedLocation.GetPositionZ(), savedLocation.GetOrientation());
    }
}

WorldLocation const* PvpveDungeonMgr::GetReturnLocation(ObjectGuid const& guid) const
{
    auto itr = _playerReturnLocations.find(guid);
    if (itr == _playerReturnLocations.end())
        return nullptr;

    return &itr->second;
}

void PvpveDungeonMgr::StoreReturnLocation(Player* player)
{
    if (!player)
        return;

    ObjectGuid const guid = player->GetGUID();
    if (!guid)
        return;

    if (_playerReturnLocations.find(guid) != _playerReturnLocations.end())
        return;

    _playerReturnLocations.emplace(guid, player->GetWorldLocation());
}

void PvpveDungeonMgr::ClearReturnLocation(ObjectGuid const& guid)
{
    if (!guid)
        return;

    _playerReturnLocations.erase(guid);
}

void PvpveDungeonMgr::ProcessTeamEliminationTimers(time_t now)
{
    if (_teamEliminationDeadlines.empty())
        return;

    for (auto itr = _teamEliminationDeadlines.begin(); itr != _teamEliminationDeadlines.end();)
    {
        uint64 const teamId = itr->first;
        PvpveTeam* team = GetTeam(teamId);
        if (!team || team->Eliminated)
        {
            itr = _teamEliminationDeadlines.erase(itr);
            continue;
        }

        PvpveDungeonRun* run = GetRunForTeam(teamId);
        if (!run || run->Finished)
        {
            itr = _teamEliminationDeadlines.erase(itr);
            continue;
        }

        DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run->TemplateId);
        if (!dungeonTemplate)
        {
            itr = _teamEliminationDeadlines.erase(itr);
            continue;
        }

        if (TeamHasActiveMembers(*team, dungeonTemplate))
        {
            TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: team {} regained a living member before the elimination timer expired.", teamId);
            itr = _teamEliminationDeadlines.erase(itr);
            continue;
        }

        if (now < itr->second)
        {
            ++itr;
            continue;
        }

        TC_LOG_INFO("server.custom", "PvpveDungeonMgr: team {} eliminated from run {} after remaining dead for 10 seconds.", teamId, run->Id);
        ForceEliminateTeam(*team, *run);
        itr = _teamEliminationDeadlines.erase(itr);
    }
}

void PvpveDungeonMgr::MaintainActivePlayerPvpState()
{
    if (_playerToRun.empty())
        return;

    for (auto const& entry : _playerToRun)
    {
        Player* player = ObjectAccessor::FindPlayer(entry.first);
        if (!player)
            continue;

        if (player->IsGameMaster())
            continue;

        bool hasFfaState = false;
        if (kPvpveFfaAuraSpellId)
            hasFfaState = player->HasAura(kPvpveFfaAuraSpellId);
        else
            hasFfaState = player->HasPvpFlag(kPvpveFfaPvpFlag);

        if (!player->pvpInfo.IsInFFAPvPArea || !hasFfaState)
            ApplyPvpveFfaState(player);
    }
}

void PvpveDungeonMgr::ForceEliminateTeam(PvpveTeam& team, PvpveDungeonRun& /*run*/)
{
    for (ObjectGuid const& memberGuid : team.Members)
    {
        Player* member = ObjectAccessor::FindPlayer(memberGuid);
        if (!member)
            continue;

        if (!IsPlayerInPvpveRun(member))
            continue;

        if (member->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        {
            member->ResurrectPlayer(1.0f);
            member->SpawnCorpseBones();
        }
        else if (!member->IsAlive())
        {
            member->ResurrectPlayer(1.0f);
        }

        OnPlayerLeftMap(member);
    }
}

void PvpveDungeonMgr::OnBossDefeated(uint64 runId, ObjectGuid const& creditGuid)
{
    PvpveDungeonRun* run = GetRun(runId);
    if (!run)
    {
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: ignoring boss defeat for unknown run {}.", runId);
        return;
    }

    if (run->Finished)
    {
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: run {} already finished; boss defeat notification ignored.", runId);
        return;
    }

    run->BossDefeated = true;
    run->Completed = true; // objective complete, but run may remain open while players are inside

    uint64 preferredWinner = 0;
    if (!creditGuid.IsEmpty())
    {
        auto teamItr = _playerToTeam.find(creditGuid);
        if (teamItr != _playerToTeam.end())
        {
            if (PvpveTeam* team = GetTeam(teamItr->second))
            {
                if (!team->Eliminated)
                    preferredWinner = team->Id;
            }
        }
    }

    TC_LOG_INFO("server.custom",
        "PvpveDungeonMgr: boss defeated for run {} (credit team: {}). Run will be marked finished once the last player leaves the instance.",
        runId, preferredWinner);

    // NOTE:
    // We no longer call FinishRun() here. The run is only considered
    // "finished" for queue/invasion purposes once the last PvPvE
    // participant leaves the dungeon (see OnPlayerLeftMap).
}

void PvpveDungeonMgr::FinishRun(PvpveDungeonRun& run, uint64 preferredWinner)
{
    if (run.Finished)
        return;

    run.Finished = true;
    // 'Completed' is already set when the boss is defeated. For non-boss
    // endings (e.g. everyone leaves early), it may remain false.
    if (!run.Completed)
        run.Completed = run.BossDefeated;

    std::vector<uint64> winningTeams;
    winningTeams.reserve(run.Teams.size());
    if (preferredWinner)
    {
        if (PvpveTeam* team = GetTeam(preferredWinner))
        {
            if (!team->Eliminated)
                winningTeams.push_back(preferredWinner);
        }
        else
            preferredWinner = 0;
    }

    if (winningTeams.empty())
    {
        for (uint64 teamId : run.Teams)
        {
            PvpveTeam* team = GetTeam(teamId);
            if (team && !team->Eliminated)
                winningTeams.push_back(teamId);
        }
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

    PvpveDungeonInstance* instanceScript = run.InstanceScript;
    if (!instanceScript)
    {
        DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run.TemplateId);
        if (dungeonTemplate && run.InstanceId)
        {
            if (Map* map = sMapMgr->FindMap(dungeonTemplate->MapId, run.InstanceId))
            {
                if (InstanceMap* instanceMap = map->ToInstanceMap())
                    instanceScript = dynamic_cast<PvpveDungeonInstance*>(instanceMap->GetInstanceScript());
            }
        }
    }

    if (instanceScript)
    {
        for (uint64 teamId : winningTeams)
        {
            if (PvpveTeam* team = GetTeam(teamId))
                instanceScript->OnPvpveRunFinished(run.Id, *team);
        }
    }

    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: TODO distribute rewards for run {}.", run.Id);

    ClearRunLockouts(run.Id);
}

void PvpveDungeonMgr::ClearRunLockouts(uint64 runId)
{
    for (auto itr = _playerRunLockouts.begin(); itr != _playerRunLockouts.end(); )
    {
        if (itr->second == runId)
            itr = _playerRunLockouts.erase(itr);
        else
            ++itr;
    }
}

void PvpveDungeonMgr::TrackQueuedMembers(std::vector<ObjectGuid> const& members)
{
    for (ObjectGuid const& guid : members)
    {
        if (!guid)
            continue;

        _queuedPlayers.insert(guid);
    }
}

void PvpveDungeonMgr::UntrackQueuedMembers(std::vector<ObjectGuid> const& members)
{
    for (ObjectGuid const& guid : members)
    {
        if (!guid)
            continue;

        _queuedPlayers.erase(guid);
    }
}

void PvpveDungeonMgr::CheckRunRuntime(PvpveDungeonRun& run, time_t now)
{
    if (!run.StartTime || run.Finished)
        return;

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run.TemplateId);
    if (!dungeonTemplate || !dungeonTemplate->MaxRuntimeSecs)
        return;

    uint32 const elapsed = uint32(now - run.StartTime);
    if (elapsed < dungeonTemplate->MaxRuntimeSecs)
    {
        run.TimeoutWarningSent = false;
        return;
    }

    if (!run.TimeoutWarningSent)
    {
        run.TimeoutWarningSent = true;
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: run {} exceeded max runtime ({}s / {}s) but will remain active until all players leave.", run.Id, elapsed, dungeonTemplate->MaxRuntimeSecs);
    }
}

uint32 PvpveDungeonMgr::CountActiveRuns() const
{
    uint32 count = 0;
    for (auto const& runPair : _runs)
    {
        if (!runPair.second.Finished)
            ++count;
    }

    return count;
}

void PvpveDungeonMgr::LogQueueStats(time_t now) const
{
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: status @{} queueTeams={} queuedPlayers={} runs={} activeRuns={}", now, _queue.size(), _queuedPlayers.size(), _runs.size(), CountActiveRuns());

    for (auto const& runPair : _runs)
    {
        PvpveDungeonRun const& run = runPair.second;
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: run {} template {} teams={} players={} completed={} finished={} startTime={}", run.Id, run.TemplateId, run.Teams.size(), run.Players.size(), run.Completed, run.Finished, uint64(run.StartTime));
    }
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
}

class PvpveDungeonPlayerScript : public PlayerScript
{
public:
    PvpveDungeonPlayerScript() : PlayerScript("pvpve_dungeon_player") {}

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

    void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        if (!player)
            return;

        if (sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
            ApplyPvpveFfaState(player);
    }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (!player)
            return;

        ObjectGuid const guid = player->GetGUID();
        if (!guid)
            return;

        if (sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
        {
            ApplyPvpveFfaState(player);
            return;
        }

        if (!sPvpveDungeonMgr->IsPvpveDungeonMap(player->GetMapId()))
            return;

        player->m_Events.AddEventAtOffset([this, guid]()
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player)
                return;

            if (sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
                return;

            if (!sPvpveDungeonMgr->IsPvpveDungeonMap(player->GetMapId()))
                return;

            ClearPvpveFfaState(player);

            if (WorldSession* session = player->GetSession())
                session->SendNotification("The PvPvE Stockades run ended while you were offline. You have been eliminated.");

            TeleportOutImmediately(player);
            ClearPendingState(guid);
        }, 1s);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        ObjectGuid const guid = player->GetGUID();
        if (sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
        {
            TeleportOutImmediately(player);
            sPvpveDungeonMgr->OnPlayerLeftMap(player);
        }

        ClearPendingState(guid);
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

        sPvpveDungeonMgr->OnPlayerDeath(player);

        // Do not mark players eliminated on death; they may still receive a
        // resurrection from teammates. Elimination is handled when they release
        // (OnPlayerRepop), the team-wide dead timer expires, or they otherwise
        // leave the dungeon.
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

        if (WorldLocation const* savedLocation = sPvpveDungeonMgr->GetReturnLocation(player->GetGUID()))
        {
            return TeleportDestination
            {
                savedLocation->GetMapId(),
                savedLocation->GetPositionX(),
                savedLocation->GetPositionY(),
                savedLocation->GetPositionZ(),
                savedLocation->GetOrientation()
            };
        }

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

            if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                player->ResurrectPlayer(1.0f);
                player->SpawnCorpseBones();
            }
            else if (!player->IsAlive())
            {
                player->RepopAtGraveyard();
                return;
            }

            TeleportDestination const destination = GetTeleportLocation(player);
            if (player->TeleportTo(destination.MapId, destination.X, destination.Y, destination.Z, destination.O))
                SnapPetToLocation(player, destination.MapId, destination.X, destination.Y, destination.Z, destination.O);
        }, 1s);
    }

    void TeleportOutImmediately(Player* player)
    {
        if (!player)
            return;

        ForceRelease(player);

        TeleportDestination const destination = GetTeleportLocation(player);
        if (player->TeleportTo(destination.MapId, destination.X, destination.Y, destination.Z, destination.O))
            SnapPetToLocation(player, destination.MapId, destination.X, destination.Y, destination.Z, destination.O);
    }

    void ClearPendingState(ObjectGuid const& guid)
    {
        _eliminatedPlayers.erase(guid);
        _pendingTeleport.erase(guid);
    }

    GuidSet _eliminatedPlayers;
    GuidSet _pendingTeleport;
};

namespace
{
    struct PvpveDungeonWorldScript : WorldScript
    {
        PvpveDungeonWorldScript() : WorldScript("pvpve_dungeon_world") {}

        void OnStartup() override
        {
            PvpveDungeonMgr::instance()->LoadConfigFromDB();
        }

        void OnUpdate(uint32 diff) override
        {
            PvpveDungeonMgr::instance()->Update(diff);
        }

        void OnShutdown() override
        {
            PvpveDungeonMgr::instance()->HandleServerShutdown();
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

void ApplyPvpveFfaState(Player* player)
{
    if (!player)
        return;

    player->pvpInfo.IsInFFAPvPArea = true;
    player->UpdatePvPState();
    player->SetPvP(true);

    if (kPvpveFfaAuraSpellId)
    {
        if (!player->HasAura(kPvpveFfaAuraSpellId))
            player->AddAura(kPvpveFfaAuraSpellId, player);

        return;
    }

    if (!player->HasPvpFlag(kPvpveFfaPvpFlag))
        player->SetPvpFlag(kPvpveFfaPvpFlag);
}

void ClearPvpveFfaState(Player* player)
{
    if (!player)
        return;

    player->pvpInfo.IsInFFAPvPArea = false;
    player->UpdatePvPState();

    if (kPvpveFfaAuraSpellId)
    {
        player->RemoveAura(kPvpveFfaAuraSpellId);
        return;
    }

    if (player->HasPvpFlag(kPvpveFfaPvpFlag))
        player->RemovePvpFlag(kPvpveFfaPvpFlag);
}
