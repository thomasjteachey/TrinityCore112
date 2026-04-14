/*
 * Scarlet Chapel Murderball / team deathmatch battleground
 * Custom TrinityCore 3.3.5 battleground type 100
 */

#ifndef __BATTLEGROUNDSCM_H
#define __BATTLEGROUNDSCM_H

#include "Battleground.h"
#include "BattlegroundScore.h"

// These world-state IDs are server-side only until you add matching client UI data.
// Pick different values if they collide with other custom states in your client.
enum BG_SCM_WorldStates
{
    BG_SCM_WORLDSTATE_ALLIANCE_SCORE = 9000,
    BG_SCM_WORLDSTATE_HORDE_SCORE    = 9001,
    BG_SCM_WORLDSTATE_MAX_SCORE      = 9002,
    BG_SCM_WORLDSTATE_TIMER_ACTIVE   = 9003,
    BG_SCM_WORLDSTATE_TIMER          = 9004,
    BG_SCM_WORLDSTATE_SHOW           = 9005
};

enum BG_SCM_Graveyards
{
    BG_SCM_GY_ALLIANCE_START = 51890,
    BG_SCM_GY_HORDE_START    = 51891,
    BG_SCM_GY_ALLIANCE_A     = 51892,
    BG_SCM_GY_HORDE_A        = 51893,
    BG_SCM_GY_ALLIANCE_B     = 51894,
    BG_SCM_GY_HORDE_B        = 51895
};

enum BG_SCM_Creatures
{
    BG_SCM_SPIRIT_ALLIANCE_A = 0,
    BG_SCM_SPIRIT_ALLIANCE_B = 1,
    BG_SCM_SPIRIT_HORDE_A    = 2,
    BG_SCM_SPIRIT_HORDE_B    = 3,
    BG_SCM_CREATURE_MAX      = 4
};

enum BG_SCM_Objects
{
    BG_SCM_OBJECT_STATUE_1        = 0,
    BG_SCM_OBJECT_DOOR_ALLIANCE   = 1,
    BG_SCM_OBJECT_DOOR_HORDE      = 2,
    BG_SCM_OBJECT_STATUE_2        = 3,
    BG_SCM_OBJECT_STATUE_3        = 4,
    BG_SCM_OBJECT_STATUE_4        = 5,

    // Buff group 1 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_SCM_OBJECT_BUFF1_SPEED     = 6,
    BG_SCM_OBJECT_BUFF1_REGEN     = 7,
    BG_SCM_OBJECT_BUFF1_BERSERK   = 8,

    // Buff group 2 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_SCM_OBJECT_BUFF2_SPEED     = 9,
    BG_SCM_OBJECT_BUFF2_REGEN     = 10,
    BG_SCM_OBJECT_BUFF2_BERSERK   = 11,

    BG_SCM_OBJECT_MAX             = 12
};

enum BG_SCM_ObjectEntries
{
    BG_SCM_OBJECT_STATUE_ENTRY        = 183491,
    BG_SCM_OBJECT_DOOR_ALLIANCE_ENTRY = 300002,
    BG_SCM_OBJECT_DOOR_HORDE_ENTRY    = 300001
};

enum BG_SCM_Constants
{
    BG_SCM_KILL_LIMIT        = 50,
    BG_SCM_BUFF_RESPAWN_TIME = 60
};

struct BattlegroundSCMScore final : public BattlegroundScore
{
    explicit BattlegroundSCMScore(ObjectGuid playerGuid) : BattlegroundScore(playerGuid) { }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class BattlegroundSCM : public Battleground
{
public:
    BattlegroundSCM();
    ~BattlegroundSCM() override = default;

    void AddPlayer(Player* player) override;
    void Reset() override;
    bool SetupBattleground() override;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

private:
    WorldSafeLocsEntry const* GetRandomTeamGraveyard(TeamId teamId) const;
    void UpdateTeamScoreWorldStates();
    void ApplyNonInteractableObjectFlags();
    void SpawnRandomBuffSet(uint32 speedTypeIndex);

    uint32 _allianceKills;
    uint32 _hordeKills;
};

#endif
