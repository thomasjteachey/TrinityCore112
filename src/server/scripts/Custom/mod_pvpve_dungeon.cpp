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

#include "Group.h"
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
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldSession.h"

#include <algorithm>
#include <ctime>
#include <set>
#include <string>
#include <sstream>
#include <unordered_set>

namespace
{
    char const* const kTemplateQuery = "SELECT Id, MapId, Enabled, MinLevel, MaxLevel, MaxTeams, MinPlayersPerTeam, MaxPlayersPerTeam, MaxRuntimeSecs "
        "FROM pvpve_dungeon_template";

    char const* const kSpawnQuery = "SELECT TemplateId, SpawnIndex, PositionX, PositionY, PositionZ, Orientation "
        "FROM pvpve_dungeon_spawn ORDER BY TemplateId, SpawnIndex";

    constexpr uint32             kPvpveFfaAuraSpellId = 0;
    constexpr UnitPVPStateFlags  kPvpveFfaPvpFlag = UNIT_BYTE2_FLAG_FFA_PVP;
    constexpr uint32             kStockadesMapId = 34;

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
} // anonymous namespace

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
        {
            ChatHandler(session).PSendSysMessage("PvPvE Stockades instance ID: %u", instanceId);

            // Flag the player as locked to this run as soon as they enter the instance so they cannot
            // re-enter it after leaving, even if they were not eliminated inside the run.
            RecordPlayerRunLockout(player->GetGUID(), run->Id, instanceId);
            if (!run->InstanceId)
                run->InstanceId = instanceId;
        }
    }
}

PvpveDungeonMgr* PvpveDungeonMgr::Instance()
{
    static PvpveDungeonMgr instance;
    return &instance;
}

PvpveDungeonMgr::PvpveDungeonMgr()
{
    Reset();
}

void PvpveDungeonMgr::Reset()
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
    _teamEliminationDeadlines.clear();
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

void PvpveDungeonMgr::PurgeDungeonInstances()
{
    if (_templates.empty())
        return;

    std::set<uint32> mapIds;
    for (auto const& pair : _templates)
        mapIds.insert(pair.second.MapId);

    if (mapIds.empty())
        return;

    std::ostringstream mapList;
    for (auto itr = mapIds.begin(); itr != mapIds.end(); ++itr)
    {
        if (itr != mapIds.begin())
            mapList << ",";

        mapList << *itr;
    }

    std::string query = "SELECT id FROM instance WHERE map IN (" + mapList.str() + ")";
    QueryResult result = CharacterDatabase.Query(query.c_str());
    if (!result)
        return;

    do
    {
        uint32 const instanceId = result->Fetch()[0].GetUInt32();
        sInstanceSaveMgr->RemoveInstanceSave(instanceId);
        sInstanceSaveMgr->DeleteInstanceFromDB(instanceId);
        TC_LOG_INFO("server.custom", "PvpveDungeonMgr: purged stale PvPvE instance {}.", instanceId);
    } while (result->NextRow());
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
            if (lockoutItr != _playerRunLockouts.end() && lockoutItr->second.RunId == preferredRunId)
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
            if (lockoutItr != _playerRunLockouts.end() && lockoutItr->second.RunId == preferredRunId)
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

            std::unordered_set<uint32> memberInstanceLocks;
            for (ObjectGuid const& memberGuid : queueItr->second.Members)
            {
                if (Player* member = ObjectAccessor::FindPlayer(memberGuid))
                {
                    if (InstancePlayerBind* bind = member->GetBoundInstance(dungeonTemplate->MapId, member->GetDifficulty(false)))
                    {
                        if (InstanceSave* save = bind->save)
                            memberInstanceLocks.insert(save->GetInstanceId());
                    }
                }
            }

            auto const runEligible = [&](PvpveDungeonRun& candidate)
            {
                if (candidate.TemplateId != dungeonTemplate->Id)
                    return false;

                if (candidate.Completed)
                    return false;

                if (dungeonTemplate->MaxTeams && candidate.Teams.size() >= dungeonTemplate->MaxTeams)
                    return false;

                std::vector<SpawnPoint> const* candidateSpawns = GetSpawnPoints(candidate.TemplateId);
                if (!candidateSpawns || candidateSpawns->empty())
                    return false;

                std::set<uint8> usedIndices = candidate.UsedSpawnIndices;
                if (usedIndices.empty())
                {
                    for (uint64 cid : candidate.Teams)
                    {
                        if (PvpveTeam* t = GetTeam(cid))
                        {
                            if (t->Eliminated)
                                continue;

                            usedIndices.insert(t->SpawnIndex);
                        }
                    }
                }

                if (usedIndices.size() >= candidateSpawns->size())
                    return false;

                bool const memberHasLockout = std::any_of(queueItr->second.Members.begin(), queueItr->second.Members.end(),
                    [this, &candidate](ObjectGuid const& guid)
                {
                    auto lockoutItr = _playerRunLockouts.find(guid);
                    if (lockoutItr == _playerRunLockouts.end())
                        return false;

                    PlayerRunLockout const& lockout = lockoutItr->second;
                    if (lockout.RunId && lockout.RunId == candidate.Id)
                        return true;

                    if (lockout.InstanceId && candidate.InstanceId && lockout.InstanceId == candidate.InstanceId)
                        return true;

                    return false;
                });

                if (!memberInstanceLocks.empty() && candidate.InstanceId && memberInstanceLocks.count(candidate.InstanceId))
                    return false;

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

uint64 PvpveDungeonMgr::GetTeamIdForPlayer(ObjectGuid const& guid) const
{
    auto itr = _playerToTeam.find(guid);
    if (itr == _playerToTeam.end())
        return 0;

    return itr->second;
}

bool PvpveDungeonMgr::PickSpawnIndex(PvpveDungeonRun const& run, uint8& outIndex)
{
    if (run.BossDefeated)
    {
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: spawn points locked for run {}; boss already defeated.", run.Id);
        return false;
    }

    auto spawnList = GetSpawnPoints(run.TemplateId);
    if (!spawnList || spawnList->empty())
        return false;

    std::set<uint8> usedIndices = run.UsedSpawnIndices;
    if (usedIndices.empty())
    {
        for (uint64 teamId : run.Teams)
        {
            if (PvpveTeam* team = GetTeam(teamId))
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

bool PvpveDungeonMgr::UnlockTeamTeleport(uint64 teamId)
{
    PvpveTeam* team = GetTeam(teamId);
    if (!team || team->TeleportUnlockedOnKill)
        return false;

    PvpveDungeonRun* run = GetRunForTeam(teamId);
    if (!run || run->Finished)
        return false;

    DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run->TemplateId);
    if (!dungeonTemplate || dungeonTemplate->MapId != kStockadesMapId)
        return false;

    team->TeleportUnlockedOnKill = true;

    for (ObjectGuid const& memberGuid : team->Members)
    {
        if (Player* member = ObjectAccessor::FindPlayer(memberGuid))
        {
            if (member->GetMapId() != dungeonTemplate->MapId)
                continue;

            if (WorldSession* session = member->GetSession())
                session->SendNotification("Your team has slain an enemy inside the Stockades. Teleport spells can now return you to the city.");
        }
    }

    return true;
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
    // Important: we only end a run automatically once all teams are *marked eliminated*.
    // We do NOT prematurely end just because nobody is currently alive or on the map ?
    // elimination (death + timer, or explicitly leaving the map) is what flips the switch.

    if (run.Completed || run.Finished)
        return;

    if (run.Teams.empty())
        return;

    uint32 remainingTeams = 0;
    for (uint64 teamId : run.Teams)
    {
        PvpveTeam* team = GetTeam(teamId);
        if (!team)
            continue;

        if (!team->Eliminated)
            ++remainingTeams;
    }

    if (remainingTeams > 0)
        return;

    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: ending run {} because all teams have been eliminated.", run.Id);
    FinishRun(run, 0);
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

    uint32 const runInstanceId = run->InstanceId ? run->InstanceId : (run->InstanceMap ? run->InstanceMap->GetInstanceId() : 0u);
    for (ObjectGuid const& memberGuid : team->Members)
        RecordPlayerRunLockout(memberGuid, run->Id, runInstanceId);

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

    // Leaving the map is an elimination event.
    OnPlayerEliminated(player);

    // Detach the player from this run?s bookkeeping.
    auto playerListItr = std::find(run->Players.begin(), run->Players.end(), guid);
    if (playerListItr != run->Players.end())
        run->Players.erase(playerListItr);

    run->PlayerSpawns.erase(guid);
    _playerToRun.erase(runItr);

    _playerToTeam.erase(guid);
    ClearReturnLocation(guid);

    // Always unbind the player AND their group from this dungeon instance so we
    // never get shoved back into the same physical Stockades run.
    if (DungeonTemplate const* dungeonTemplate = GetDungeonTemplate(run->TemplateId))
    {
        Difficulty const diff = player->GetDifficulty(false);

        // Personal bind
        player->UnbindInstance(dungeonTemplate->MapId, diff);

        // Group bind ? this is what was causing ?invade same person twice / on their spawn?
        if (Group* group = player->GetGroup())
            group->UnbindInstance(dungeonTemplate->MapId, uint8(diff), false);
    }

    // Lockout logic: they may not re-enter THIS run again while it is active.
    uint32 const runInstanceId =
        run->InstanceId ? run->InstanceId :
        (run->InstanceMap ? run->InstanceMap->GetInstanceId() : 0u);

    if (run->Finished)
        _playerRunLockouts.erase(guid);
    else
        RecordPlayerRunLockout(guid, run->Id, runInstanceId);

    EvaluateRunState(*run);

    if (run->Finished && run->Players.empty())
        CleanupRun(run->Id);

    if (hasSavedLocation && player->IsInWorld())
    {
        player->TeleportTo(savedLocation.GetMapId(),
            savedLocation.GetPositionX(), savedLocation.GetPositionY(),
            savedLocation.GetPositionZ(), savedLocation.GetOrientation());
    }
}

WorldLocation const* PvpveDungeonMgr::GetReturnLocation(ObjectGuid const& guid) const
{
    auto itr = _playerReturnLocations.find(guid);
    if (itr == _playerReturnLocations.end())
        return nullptr;

    return &itr->second;
}

void PvpveDungeonMgr::RecordPlayerRunLockout(ObjectGuid const& guid, uint64 runId, uint32 instanceId)
{
    if (!guid || !runId)
        return;

    _playerRunLockouts[guid] = PlayerRunLockout{ runId, instanceId };
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
    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: boss defeated for run {}; locking further spawns.", runId);

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

    TC_LOG_INFO("server.custom", "PvpveDungeonMgr: boss defeated for run {} (credit team: {}).", runId, preferredWinner);
    FinishRun(*run, preferredWinner);
}

void PvpveDungeonMgr::FinishRun(PvpveDungeonRun& run, uint64 preferredWinner)
{
    if (run.Finished)
        return;

    run.Finished = true;
    run.Completed = true;

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

    if (run.Players.empty())
        CleanupRun(run.Id);
}

void PvpveDungeonMgr::ClearRunLockouts(uint64 runId)
{
    for (auto itr = _playerRunLockouts.begin(); itr != _playerRunLockouts.end();)
    {
        if (itr->second.RunId == runId)
            itr = _playerRunLockouts.erase(itr);
        else
            ++itr;
    }
}

void PvpveDungeonMgr::CleanupRun(uint64 runId)
{
    auto runItr = _runs.find(runId);
    if (runItr == _runs.end())
        return;

    PvpveDungeonRun const& run = runItr->second;

    for (ObjectGuid const& guid : run.Players)
    {
        _playerToRun.erase(guid);
        _playerToTeam.erase(guid);
        _queuedPlayers.erase(guid);
        _playerReturnLocations.erase(guid);
    }

    for (uint64 teamId : run.Teams)
    {
        _teamEliminationDeadlines.erase(teamId);
        _teams.erase(teamId);
    }

    ClearRunLockouts(runId);

    if (run.InstanceId)
    {
        sInstanceSaveMgr->RemoveInstanceSave(run.InstanceId);
        sInstanceSaveMgr->DeleteInstanceFromDB(run.InstanceId);
    }

    _runs.erase(runItr);
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
        TC_LOG_WARN("server.custom", "PvpveDungeonMgr: run {} exceeded max runtime ({}s / {}s) but will remain active until the boss is defeated.",
            run.Id, elapsed, dungeonTemplate->MaxRuntimeSecs);
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
    TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: status @{} queueTeams={} queuedPlayers={} runs={} activeRuns={}",
        now, _queue.size(), _queuedPlayers.size(), _runs.size(), CountActiveRuns());

    for (auto const& runPair : _runs)
    {
        PvpveDungeonRun const& run = runPair.second;
        TC_LOG_DEBUG("server.custom", "PvpveDungeonMgr: run {} template {} teams={} players={} completed={} finished={} startTime={}",
            run.Id, run.TemplateId, run.Teams.size(), run.Players.size(), run.Completed, run.Finished, uint64(run.StartTime));
    }
}

namespace
{
    struct TeleportDestination
    {
        uint32 MapId;
        float  X;
        float  Y;
        float  Z;
        float  O;
    };

    TeleportDestination const kAllianceTeleportDestination{ 0, -8833.38f, 628.62f, 94.0066f, 1.0646f };
    TeleportDestination const kHordeTeleportDestination{ 1, 1633.33f, -4439.09f, 15.999f, 5.3178f };

    using TeleportSpellSet = std::unordered_set<uint32>;

    TeleportSpellSet const kMageTeleportSpellIds
    {
        // Classic-era mage teleports
        3561,  // Teleport: Stormwind
        3562,  // Teleport: Ironforge
        3563,  // Teleport: Undercity
        3565,  // Teleport: Darnassus
        3566,  // Teleport: Thunder Bluff
        3567,  // Teleport: Orgrimmar

        // Expanded capital teleports (in case the core has them enabled)
        11416, // Teleport: Ironforge
        11417, // Teleport: Orgrimmar
        11418, // Teleport: Undercity
        11419, // Teleport: Darnassus
        11420, // Teleport: Thunder Bluff
        32271, // Teleport: Exodar
        32272  // Teleport: Silvermoon
    };

    bool IsRogueVanish(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        if (spellInfo->SpellFamilyName != SPELLFAMILY_ROGUE)
            return false;

        if (!(spellInfo->SpellFamilyFlags[0] & SPELLFAMILYFLAG_ROGUE_VANISH))
            return false;

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if (!effect.IsEffectValid())
                continue;

            if (effect.ApplyAuraName == SPELL_AURA_MOD_STEALTH)
                return true;
        }

        return false;
    }

    bool IsStockadesMageTeleport(uint32 spellId)
    {
        return kMageTeleportSpellIds.find(spellId) != kMageTeleportSpellIds.end();
    }
}

class PvpveDungeonPlayerScript : public PlayerScript
{
public:
    PvpveDungeonPlayerScript() : PlayerScript("pvpve_dungeon_player") {}

    void OnSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!player || !spell)
            return;

        if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
            return;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo)
            return;

        PvpveDungeonRun* run = sPvpveDungeonMgr->GetRunForPlayer(player->GetGUID());
        if (!run || run->BossDefeated || run->Finished)
            return;

        if (player->GetMapId() == kStockadesMapId && IsRogueVanish(spellInfo))
            player->GetCombatManager().EndAllPvECombat();

        uint64 const teamId = sPvpveDungeonMgr->GetTeamIdForPlayer(player->GetGUID());
        PvpveTeam* team = sPvpveDungeonMgr->GetTeam(teamId);
        if (team && team->TeleportUnlockedOnKill)
            return;

        DungeonTemplate const* dungeonTemplate = sPvpveDungeonMgr->GetDungeonTemplate(run->TemplateId);
        if (!dungeonTemplate || dungeonTemplate->MapId != kStockadesMapId)
            return;

        if (!IsStockadesMageTeleport(spellInfo->Id))
            return;

        spell->SendCastResult(SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW);
        spell->cancel();

        if (WorldSession* session = player->GetSession())
            session->SendNotification("You cannot teleport out until the Stockades boss has been defeated.");
    }

    void OnPVPKill(Player* killer, Player* killed) override
    {
        if (killer && killed && killer->GetMapId() == kStockadesMapId)
        {
            PvpveDungeonRun* killerRun = sPvpveDungeonMgr->GetRunForPlayer(killer->GetGUID());
            PvpveDungeonRun* victimRun = sPvpveDungeonMgr->GetRunForPlayer(killed->GetGUID());

            if (killerRun && killerRun == victimRun)
            {
                uint64 const killerTeamId = sPvpveDungeonMgr->GetTeamIdForPlayer(killer->GetGUID());
                uint64 const victimTeamId = sPvpveDungeonMgr->GetTeamIdForPlayer(killed->GetGUID());

                if (killerTeamId && victimTeamId && killerTeamId != victimTeamId)
                    sPvpveDungeonMgr->UnlockTeamTeleport(killerTeamId);
            }
        }

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

    void EnsureAliveForTeleport(Player* player)
    {
        if (!player)
            return;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        {
            player->ResurrectPlayer(1.0f);
            player->SpawnCorpseBones();
        }
        else if (!player->IsAlive())
        {
            player->ResurrectPlayer(1.0f);
        }
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

            if (PvpveDungeonRun* run = sPvpveDungeonMgr->GetRunForPlayer(guid))
            {
                uint32 instanceId = run->InstanceId;
                if (!instanceId)
                {
                    instanceId = player->GetInstanceId();
                    if (!instanceId)
                        if (Map* map = player->GetMap())
                            instanceId = map->GetInstanceId();
                }

                if (instanceId)
                {
                    sPvpveDungeonMgr->RecordPlayerRunLockout(guid, run->Id, instanceId);
                    if (!run->InstanceId)
                        run->InstanceId = instanceId;
                }
            }

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
        // Actual elimination is handled on release / timeout / leaving map.
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

            // They have chosen to release at this point; now we kick them out and rez them.
            EnsureAliveForTeleport(player);

            TeleportDestination const destination = GetTeleportLocation(player);
            if (player->TeleportTo(destination.MapId, destination.X, destination.Y, destination.Z, destination.O))
                SnapPetToLocation(player, destination.MapId, destination.X, destination.Y, destination.Z, destination.O);
        }, 1s);
    }

    void TeleportOutImmediately(Player* player)
    {
        if (!player)
            return;

        EnsureAliveForTeleport(player);

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
            PvpveDungeonMgr::instance()->Reset();
            PvpveDungeonMgr::instance()->LoadConfigFromDB();
            PvpveDungeonMgr::instance()->PurgeDungeonInstances();
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
