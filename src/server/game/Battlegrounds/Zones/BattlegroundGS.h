/*
 * Gunship Deathmatch battleground
 * Custom TrinityCore 3.3.5 battleground type 102
 */

#ifndef __BATTLEGROUNDGS_H
#define __BATTLEGROUNDGS_H

#include "Battleground.h"
#include "BattlegroundScore.h"

class Transport;

struct BG_GS_TransportPoint
{
    uint32 TransportEntry;
    Position Offset;
};

enum BG_GS_WorldStates
{
    BG_GS_WORLDSTATE_ALLIANCE_SCORE = 9200,
    BG_GS_WORLDSTATE_HORDE_SCORE    = 9201,
    BG_GS_WORLDSTATE_MAX_SCORE      = 9202,
    BG_GS_WORLDSTATE_TIMER_ACTIVE   = 9203,
    BG_GS_WORLDSTATE_TIMER          = 9204,
    BG_GS_WORLDSTATE_SHOW           = 9205,
    BG_GS_WORLDSTATE_MAX_KILLS_UI   = 9206
};

enum BG_GS_Graveyards
{
    BG_GS_GY_ALLIANCE_START = 52400,
    BG_GS_GY_HORDE_START    = 52401,
    BG_GS_GY_ALLIANCE       = 52402,
    BG_GS_GY_HORDE          = 52403
};

enum BG_GS_Creatures
{
    BG_GS_SPIRIT_ALLIANCE = 0,
    BG_GS_SPIRIT_HORDE    = 1,
    BG_GS_CREATURE_MAX    = 2
};

enum BG_GS_TransportEntries
{
    BG_GS_TRANSPORT_ORGRIM_HAMMER = 300241,
    BG_GS_TRANSPORT_SKYBREAKER    = 300242
};

enum BG_GS_Constants
{
    BG_GS_KILL_LIMIT = 30
};

struct BattlegroundGSScore final : public BattlegroundScore
{
    explicit BattlegroundGSScore(ObjectGuid playerGuid, uint32 scoreboardTeamMarker = 0)
        : BattlegroundScore(playerGuid)
    {
        BonusHonor = scoreboardTeamMarker;
    }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class BattlegroundGS : public Battleground
{
public:
    BattlegroundGS();
    ~BattlegroundGS() override = default;

    void AddPlayer(Player* player) override;
    void RemovePlayer(Player* player, ObjectGuid guid, uint32 team) override;
    void Reset() override;
    bool SetupBattleground() override;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    bool HandlePlayerRepopAtGraveyard(Player* player, bool shouldResurrect) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

private:
    bool SpawnGunshipTransports();
    Transport* GetTransportForEntry(uint32 entry) const;
    BG_GS_TransportPoint const& GetTeamStartPoint(uint32 team) const;
    BG_GS_TransportPoint const& GetTeamGraveyardPoint(uint32 team) const;
    bool TeleportPlayerToTransportPoint(Player* player, BG_GS_TransportPoint const& point, bool resurrectAtTeleport = false);
    bool AddTransportSpiritGuide(uint32 type, Transport* transport, Position const& offset, TeamId teamId);

    void UpdateTeamScoreWorldStates();
    void AwardPointToTeam(uint32 team);
    void AwardLeavePointIfNeeded(Player const* player, uint32 team);
    uint32 GetHonorRewardForTeam() const;
    void ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const override;
    void TrackHumanParticipantAdded(Player const* player, bool isInBattleground);
    void TrackHumanParticipantRemoved(Player const* player, uint32 team);
    void UpdateHumanFaceoffState();

    uint32 _allianceKills;
    uint32 _hordeKills;
    uint32 _allianceHumanParticipants;
    uint32 _hordeHumanParticipants;
    bool _humanFaceoffEverHappened;

    Transport* _skybreaker;
    Transport* _orgrimsHammer;
};

#endif
