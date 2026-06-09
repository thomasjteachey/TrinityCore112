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

enum BG_BRT_Objects
{
    BG_BRT_OBJECT_ALLIANCE_GATE_LEFT = 0,
    BG_BRT_OBJECT_ALLIANCE_GATE_RIGHT,
    BG_BRT_OBJECT_HORDE_GATE,
    BG_BRT_OBJECT_GHOST_WALL_CENTER,
    BG_BRT_OBJECT_IMPERIAL_THRONE,

    // Buff group 1 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_BRT_OBJECT_BUFF1_SPEED,
    BG_BRT_OBJECT_BUFF1_REGEN,
    BG_BRT_OBJECT_BUFF1_BERSERK,

    // Buff group 2 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_BRT_OBJECT_BUFF2_SPEED,
    BG_BRT_OBJECT_BUFF2_REGEN,
    BG_BRT_OBJECT_BUFF2_BERSERK,

    // Buff group 3 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_BRT_OBJECT_BUFF3_SPEED,
    BG_BRT_OBJECT_BUFF3_REGEN,
    BG_BRT_OBJECT_BUFF3_BERSERK,

    // Buff group 4 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_BRT_OBJECT_BUFF4_SPEED,
    BG_BRT_OBJECT_BUFF4_REGEN,
    BG_BRT_OBJECT_BUFF4_BERSERK,

    BG_BRT_OBJECT_TRAMPOLINE_1,
    BG_BRT_OBJECT_TRAMPOLINE_2,
    BG_BRT_OBJECT_TRAMPOLINE_3,
    BG_BRT_OBJECT_TRAMPOLINE_4,

    BG_BRT_OBJECT_MAX
};

enum BG_BRT_ObjectEntries
{
    BG_BRT_OBJECT_ALLIANCE_GATE_ENTRY = 185483,
    BG_BRT_OBJECT_HORDE_GATE_ENTRY = 170575,
    BG_BRT_OBJECT_GHOST_WALL_ENTRY = 183491,
    BG_BRT_OBJECT_IMPERIAL_THRONE_ENTRY = 170592,
    BG_BRT_OBJECT_TRAMPOLINE_1_ENTRY = 300003,
    BG_BRT_OBJECT_TRAMPOLINE_2_ENTRY = 300004,
    BG_BRT_OBJECT_TRAMPOLINE_3_ENTRY = 300005,
    BG_BRT_OBJECT_TRAMPOLINE_4_ENTRY = 300006
};

enum BG_BRT_Constants
{
    BG_BRT_KILL_LIMIT = 30,
    BG_BRT_BUFF_RESPAWN_TIME = 60
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
    uint32 GetBuffRespawnTime(uint32 type) const override;

private:
    WorldSafeLocsEntry const* GetCurrentTeamGraveyard(TeamId teamId) const;
    void UpdateTeamScoreWorldStates();
    void AwardPointToTeam(uint32 team);
    void AwardLeavePointIfNeeded(Player const* player, uint32 team);
    uint32 GetHonorRewardForTeam() const;
    void ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const override;
    void TrackHumanParticipantAdded(Player const* player, bool isInBattleground);
    void TrackHumanParticipantRemoved(Player const* player, uint32 team);
    void UpdateHumanFaceoffState();
    void ApplyNonInteractableObjectFlags();
    void SpawnRandomBuffSet(uint32 speedTypeIndex);

    uint32 _allianceKills;
    uint32 _hordeKills;
    uint32 _allianceHumanParticipants;
    uint32 _hordeHumanParticipants;
    bool _humanFaceoffEverHappened;
    bool _usePrimaryGraveyard;
    uint32 _graveyardSwapTimer;
};

#endif
