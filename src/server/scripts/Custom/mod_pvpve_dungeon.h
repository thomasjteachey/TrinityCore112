#ifndef MOD_PVPVE_DUNGEON_H
#define MOD_PVPVE_DUNGEON_H

#include "ObjectGuid.h"

#include <cstdint>
#include <ctime>
#include <map>
#include <vector>

class Player;
class Map;
class Group;
class Creature;
class WorldPacket;
class WorldSession;

struct DungeonTemplate
{
    uint32 Id = 0;
    uint32 MapId = 0;
    bool Enabled = false;
    uint8 MinLevel = 1;
    uint8 MaxLevel = 60;
    uint8 MaxTeams = 1;
    uint8 MinPlayers = 0;
    uint8 MaxPlayers = 0;
};

struct SpawnPoint
{
    uint32 TemplateId = 0;
    uint8 Index = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float O = 0.0f;
};

struct PvpveTeam
{
    uint64 Id = 0;
    uint32 TemplateId = 0;
    std::vector<ObjectGuid> Members;
    time_t CreatedTime = 0;
    uint8 SpawnIndex = 0;
    bool Ready = false;
};

struct PvpveDungeonRun
{
    uint64 Id = 0;
    uint32 TemplateId = 0;
    Map* InstanceMap = nullptr;
    ObjectGuid GroupGuid;
    time_t CreatedTime = 0;
    time_t StartTime = 0;
    bool Active = false;
    bool Completed = false;
    std::vector<ObjectGuid> Players;
    std::map<ObjectGuid, uint8> PlayerSpawns;
    std::vector<uint64> Teams;
};

struct QueuedTeam
{
    uint64 TeamId = 0;
    uint32 TemplateId = 0;
    time_t QueueTime = 0;
    std::vector<ObjectGuid> Members;
    bool Ready = false;
};

class PvpveDungeonMgr
{
public:
    static PvpveDungeonMgr* Instance();
    static PvpveDungeonMgr* instance()
    {
        return Instance();
    }

    void LoadConfigFromDB();

    const DungeonTemplate* GetDungeonTemplate(uint32 templateId) const;
    const std::vector<SpawnPoint>* GetSpawnPoints(uint32 templateId) const;

    uint64 CreateTeam(std::vector<Player*> const& players, uint32 templateId);
    void RemoveTeam(uint64 teamId);
    void QueueTeam(uint64 teamId);
    bool QueueTeam(uint32 templateId, std::vector<ObjectGuid> const& memberGuids);
    void CancelQueue(uint64 teamId);

    void Update(uint32 diff);

    void OnPlayerEnterDungeon(Player* player);
    void OnPlayerLeaveDungeon(Player* player);
    void OnPlayerDeath(Player* player);

    PvpveDungeonRun* GetRun(uint64 runId);
    PvpveTeam* GetTeam(uint64 teamId);
    PvpveDungeonRun* GetRunForPlayer(ObjectGuid const& guid);

    void Reset();

private:
    PvpveDungeonMgr();

    void StartNextRun();
    void AssignTeamToRun(PvpveDungeonRun& run, QueuedTeam const& queued);
    uint8 PickSpawnIndex(uint32 templateId);
    void CleanupRun(uint64 runId);
    void CleanupPlayer(ObjectGuid const& guid);
    void OnInstanceCreated(Map* map);
    void OnPlayerEliminated(Player* player);
    void OnPlayerLeftMap(Player* player);

    using QueueContainer = std::map<uint64, QueuedTeam>;
    using RunContainer = std::map<uint64, PvpveDungeonRun>;
    using TeamContainer = std::map<uint64, PvpveTeam>;
    using PlayerRunMap = std::map<ObjectGuid, uint64>;
    using PlayerTeamMap = std::map<ObjectGuid, uint64>;
    using TemplateContainer = std::map<uint32, DungeonTemplate>;
    using SpawnContainer = std::map<uint32, std::vector<SpawnPoint>>;

    QueueContainer _queue;
    RunContainer _runs;
    TeamContainer _teams;
    PlayerRunMap _playerToRun;
    PlayerTeamMap _playerToTeam;
    TemplateContainer _templates;
    SpawnContainer _spawns;
    uint64 _nextRunId = 1;
    uint64 _nextTeamId = 1;
};

#define sPvpveDungeonMgr PvpveDungeonMgr::Instance()

#endif // MOD_PVPVE_DUNGEON_H
