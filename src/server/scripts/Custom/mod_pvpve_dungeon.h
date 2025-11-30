#ifndef MOD_PVPVE_DUNGEON_H
#define MOD_PVPVE_DUNGEON_H

#include "ObjectGuid.h"
#include "Position.h"

#include <cstdint>
#include <ctime>
#include <map>
#include <vector>
#include <set>

class Player;
class Map;
class Creature;
class WorldPacket;
class WorldSession;
class PvpveDungeonPlayerScript;

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
    uint32 MaxRuntimeSecs = 0;
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
    bool Eliminated = false;
    bool TeleportUnlockedOnKill = false;
};

class PvpveDungeonInstance
{
public:
    virtual ~PvpveDungeonInstance() = default;
    virtual void OnPvpveRunFinished(uint32 runId, PvpveTeam const& winningTeam) = 0;
};

struct PvpveDungeonRun
{
    uint64 Id = 0;
    uint32 TemplateId = 0;
    Map* InstanceMap = nullptr;
    uint32 InstanceId = 0;
    PvpveDungeonInstance* InstanceScript = nullptr;
    time_t CreatedTime = 0;
    time_t StartTime = 0;
    bool Active = false;
    bool Completed = false;
    bool Finished = false;
    bool BossDefeated = false;
    bool TimeoutWarningSent = false;
    std::vector<ObjectGuid> Players;
    std::map<ObjectGuid, uint8> PlayerSpawns;
    std::vector<uint64> Teams;
    std::set<uint8> UsedSpawnIndices;
};

struct QueuedTeam
{
    uint64 TeamId = 0;
    uint32 TemplateId = 0;
    time_t QueueTime = 0;
    std::vector<ObjectGuid> Members;
    bool Ready = false;
    uint64 PreferredRunId = 0;
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
    bool QueueTeam(uint64 teamId, uint64 preferredRunId = 0);
    bool QueueTeam(uint32 templateId, std::vector<ObjectGuid> const& memberGuids, uint64 preferredRunId = 0);
    void CancelQueue(uint64 teamId);

    void Update(uint32 diff);

    void OnPlayerEnterDungeon(Player* player);
    void OnPlayerLeaveDungeon(Player* player);
    void OnPlayerDeath(Player* player);
    void OnInstanceCreated(uint32 templateId, uint64 runId, uint32 instanceId);
    void OnPlayerEnteredInstance(Player* player, PvpveDungeonInstance* instanceScript);
    void OnBossDefeated(uint64 runId, ObjectGuid const& creditGuid);
    bool IsPlayerInPvpveRun(ObjectGuid const& guid) const;
    bool IsPlayerInPvpveRun(Player const* player) const;
    bool IsPvpveDungeonMap(uint32 mapId) const;
    WorldLocation const* GetReturnLocation(ObjectGuid const& guid) const;
    void RecordPlayerRunLockout(ObjectGuid const& guid, uint64 runId, uint32 instanceId);

    PvpveDungeonRun* GetRun(uint64 runId);
    PvpveTeam* GetTeam(uint64 teamId);
    PvpveDungeonRun* GetRunForPlayer(ObjectGuid const& guid);
    PvpveDungeonRun* GetRunForTeam(uint64 teamId);
    uint64 GetTeamIdForPlayer(ObjectGuid const& guid) const;

    void Reset();
    void PurgeDungeonInstances();
    void HandleServerShutdown();

    bool UnlockTeamTeleport(uint64 teamId);

private:
    PvpveDungeonMgr();

    void StartNextRun();
    void AssignTeamToRun(PvpveDungeonRun& run, QueuedTeam const& queued);
    bool PickSpawnIndex(PvpveDungeonRun const& run, uint8& outIndex);
    void CleanupRun(uint64 runId);
    void CleanupPlayer(ObjectGuid const& guid);
    void OnPlayerEliminated(Player* player);
    void OnPlayerLeftMap(Player* player);
    void EvaluateRunState(PvpveDungeonRun& run);
    bool TeamHasActiveMembers(PvpveTeam const& team, DungeonTemplate const* dungeonTemplate) const;
    void FinishRun(PvpveDungeonRun& run, uint64 preferredWinner = 0);
    void TrackQueuedMembers(std::vector<ObjectGuid> const& members);
    void UntrackQueuedMembers(std::vector<ObjectGuid> const& members);
    void CheckRunRuntime(PvpveDungeonRun& run, time_t now);
    void LogQueueStats(time_t now) const;
    uint32 CountActiveRuns() const;
    void ClearRunLockouts(uint64 runId);
    void StoreReturnLocation(Player* player);
    void ClearReturnLocation(ObjectGuid const& guid);
    void ProcessTeamEliminationTimers(time_t now);
    void ForceEliminateTeam(PvpveTeam& team, PvpveDungeonRun& run);
    void MaintainActivePlayerPvpState();

    struct PlayerRunLockout
    {
        uint64 RunId = 0;
        uint32 InstanceId = 0;
    };

    using QueueContainer = std::map<uint64, QueuedTeam>;
    using RunContainer = std::map<uint64, PvpveDungeonRun>;
    using TeamContainer = std::map<uint64, PvpveTeam>;
    using PlayerRunMap = std::map<ObjectGuid, uint64>;
    using PlayerTeamMap = std::map<ObjectGuid, uint64>;
    using PlayerLockoutMap = std::map<ObjectGuid, PlayerRunLockout>;
    using TemplateContainer = std::map<uint32, DungeonTemplate>;
    using SpawnContainer = std::map<uint32, std::vector<SpawnPoint>>;
    using PlayerLocationMap = std::map<ObjectGuid, WorldLocation>;

    QueueContainer _queue;
    RunContainer _runs;
    TeamContainer _teams;
    PlayerRunMap _playerToRun;
    PlayerTeamMap _playerToTeam;
    PlayerLockoutMap _playerRunLockouts;
    TemplateContainer _templates;
    SpawnContainer _spawns;
    GuidSet _queuedPlayers;
    PlayerLocationMap _playerReturnLocations;
    std::map<uint64, time_t> _teamEliminationDeadlines;
    uint64 _nextRunId = 1;
    uint64 _nextTeamId = 1;
    time_t _lastStatsLog = 0;

    friend class PvpveDungeonPlayerScript;
};

#define sPvpveDungeonMgr PvpveDungeonMgr::Instance()

void ApplyPvpveFfaState(Player* player);
void ClearPvpveFfaState(Player* player);

#endif // MOD_PVPVE_DUNGEON_H
