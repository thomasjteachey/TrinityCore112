/*
 * Blackrock Throne Murderball / team deathmatch battleground
 * Custom TrinityCore 3.3.5 battleground type 101
 */

#ifndef __BATTROUNDBRT_H
#define __BATTROUNDBRT_H

#include "Battleground.h"
#include "BattlegroundScore.h"

enum BG_BRT_WorldStates
{
    BG_BRT_WORLDSTATE_ALLIANCE_SCORE = 9100,
    BG_BRT_WORLDSTATE_HORDE_SCORE    = 9101,
    BG_BRT_WORLDSTATE_MAX_SCORE      = 9102,
    BG_BRT_WORLDSTATE_TIMER_ACTIVE   = 9103,
    BG_BRT_WORLDSTATE_TIMER          = 9104,
    BG_BRT_WORLDSTATE_SHOW           = 9105,
    BG_BRT_WORLDSTATE_MAX_KILLS_UI   = 9106
};

enum BG_BRT_Graveyards
{
    BG_BRT_GY_ALLIANCE_START = 52300,
    BG_BRT_GY_HORDE_START    = 52301,
    BG_BRT_GY_ALLIANCE_A     = 52302,
    BG_BRT_GY_ALLIANCE_B     = 52303,
    BG_BRT_GY_HORDE_A        = 52304,
    BG_BRT_GY_HORDE_B        = 52305
};

enum BG_BRT_Creatures
{
    BG_BRT_SPIRIT_ALLIANCE_A = 0,
    BG_BRT_SPIRIT_ALLIANCE_B = 1,
    BG_BRT_SPIRIT_HORDE_A    = 2,
    BG_BRT_SPIRIT_HORDE_B    = 3,
    BG_BRT_CREATURE_MAX      = 4
};

enum BG_BRT_Constants
{
    BG_BRT_KILL_LIMIT = 30
};

struct BattlegroundBRTScore final : public BattlegroundScore
{
    explicit BattlegroundBRTScore(ObjectGuid playerGuid, uint32 scoreboardTeamMarker = 0)
        : BattlegroundScore(playerGuid)
    {
        BonusHonor = scoreboardTeamMarker;
    }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class BattlegroundBRT : public Battleground
{
public:
    BattlegroundBRT();
    ~BattlegroundBRT() override = default;

    void AddPlayer(Player* player) override;
    void RemovePlayer(Player* player, ObjectGuid guid, uint32 team) override;
    void Reset() override;
    bool SetupBattleground() override;
    void PostUpdateImpl(uint32 diff) override;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

private:
    WorldSafeLocsEntry const* GetCurrentTeamGraveyard(TeamId teamId) const;
    void UpdateTeamScoreWorldStates();
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
    bool _usePrimaryGraveyard;
    uint32 _graveyardSwapTimer;
};

#endif
