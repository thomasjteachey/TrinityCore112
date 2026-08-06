/*
 * Tanaris team deathmatch battleground
 * Custom TrinityCore 3.3.5 battleground type 104 (map 1620, cloned from map 1)
 *
 * Straight deathmatch: one point per kill, first side to the kill limit wins.
 *
 * What makes this one different from the other custom battlegrounds here is
 * that it is fought OUTDOORS, on the flat desert shelf in north-west Tanaris,
 * rather than inside a building. There are no walls to keep anyone in, so
 * containment is a coordinate check on a timer (ConfinePlayers) instead of a
 * ring of doors. The gates are start-line dressing; the bounds check is what
 * actually holds the arena together.
 *
 * Map 1620 is a full clone of Kalimdor, so the terrain outside the arena is
 * real ground rather than a hole. Leaving the rectangle is therefore a
 * gameplay problem, not a falling-through-the-world one, and the fix is to
 * slide the player back inside rather than to yank them to a graveyard.
 */

#ifndef __BATTLEGROUNDTRT_H
#define __BATTLEGROUNDTRT_H

#include "Battleground.h"
#include "BattlegroundScore.h"

// Server-side only until matching client WorldStateUI rows exist.
// Blocks of 100 per custom battleground: SCM 9000, BRT 9100, OBC 9200.
enum BG_TRT_WorldStates
{
    BG_TRT_WORLDSTATE_ALLIANCE_SCORE = 9300,
    BG_TRT_WORLDSTATE_HORDE_SCORE    = 9301,
    BG_TRT_WORLDSTATE_MAX_SCORE      = 9302,
    BG_TRT_WORLDSTATE_TIMER_ACTIVE   = 9303,
    BG_TRT_WORLDSTATE_TIMER          = 9304,
    BG_TRT_WORLDSTATE_SHOW           = 9305,
    BG_TRT_WORLDSTATE_MAX_KILLS_UI   = 9306
};

enum BG_TRT_Graveyards
{
    BG_TRT_GY_ALLIANCE_START = 52500,
    BG_TRT_GY_HORDE_START    = 52501,
    BG_TRT_GY_ALLIANCE       = 52502,
    BG_TRT_GY_HORDE          = 52503
};

enum BG_TRT_Creatures
{
    BG_TRT_SPIRIT_ALLIANCE = 0,
    BG_TRT_SPIRIT_HORDE    = 1,
    BG_TRT_CREATURE_MAX    = 2
};

enum BG_TRT_Objects
{
    BG_TRT_OBJECT_GATE_ALLIANCE = 0,
    BG_TRT_OBJECT_GATE_HORDE    = 1,

    // Buff group 1 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_TRT_OBJECT_BUFF1_SPEED   = 2,
    BG_TRT_OBJECT_BUFF1_REGEN   = 3,
    BG_TRT_OBJECT_BUFF1_BERSERK = 4,

    // Buff group 2 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_TRT_OBJECT_BUFF2_SPEED   = 5,
    BG_TRT_OBJECT_BUFF2_REGEN   = 6,
    BG_TRT_OBJECT_BUFF2_BERSERK = 7,

    // Buff group 3 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_TRT_OBJECT_BUFF3_SPEED   = 8,
    BG_TRT_OBJECT_BUFF3_REGEN   = 9,
    BG_TRT_OBJECT_BUFF3_BERSERK = 10,

    // Buff group 4 must stay contiguous in Speed/Regen/Berserk order for m_BuffChange logic.
    BG_TRT_OBJECT_BUFF4_SPEED   = 11,
    BG_TRT_OBJECT_BUFF4_REGEN   = 12,
    BG_TRT_OBJECT_BUFF4_BERSERK = 13,

    BG_TRT_OBJECT_MAX           = 14
};

enum BG_TRT_ObjectEntries
{
    // Same gate the other two custom battlegrounds use, so it is known to
    // open and close correctly through DoorOpen/DoorClose here.
    BG_TRT_OBJECT_GATE_ENTRY = 185483
};

enum BG_TRT_Constants
{
    BG_TRT_KILL_LIMIT        = 30,
    BG_TRT_BUFF_RESPAWN_TIME = 60,
    // Bounds and fall-through are both checked on this timer rather than every
    // tick. Walking out of the arena takes far longer than the interval at any
    // achievable run speed, so nothing can slip past between checks.
    BG_TRT_BOUNDS_CHECK_INTERVAL = 500
};

// The playable rectangle, in world coordinates on map 1620: 230 x 200 yards
// of the level desert shelf, centred on (-8365, -3010).
//
// These bounds are not arbitrary. Sampling the server's own terrain out of
// 16204737.map, the shelf holds z 8.6 - 15.3 across this rectangle, but a mesa
// rises to z 60 just north-west of it and the ground climbs steeply away to the
// south-east. Both would sit on one team's side only, so the rectangle is drawn
// to exclude them and keep the fight on symmetric ground. Widening it without
// re-checking the heightmap would hand the Alliance a hill.
//
// The one feature left inside is a small crater about 15 yards across near the
// centre, bottoming at z 2. That is deliberate: it is central, so it costs
// neither side anything, and it gives an otherwise featureless plain a landmark.
constexpr float BG_TRT_ARENA_MIN_X = -8480.0f;
constexpr float BG_TRT_ARENA_MAX_X = -8250.0f;
constexpr float BG_TRT_ARENA_MIN_Y = -3110.0f;
constexpr float BG_TRT_ARENA_MAX_Y = -2910.0f;

// How far inside the boundary a player is put back. Landing exactly on the
// edge would re-trigger the check on the next pass from any outward drift.
constexpr float BG_TRT_BOUNDS_INSET = 5.0f;

// During the warmup each team is held behind its own gate line, 55 yards from
// its own end of the rectangle and equidistant from the centre.
constexpr float BG_TRT_GATE_LINE_ALLIANCE = -8425.0f;
constexpr float BG_TRT_GATE_LINE_HORDE    = -8305.0f;

// Ground in the arena runs z 8.6 - 15.3, and the crater floor is z 2, so
// anything this far down has fallen out of the world rather than walked
// downhill.
constexpr float BG_TRT_MIN_SAFE_Z = -30.0f;

struct BattlegroundTRTScore final : public BattlegroundScore
{
    explicit BattlegroundTRTScore(ObjectGuid playerGuid, uint32 scoreboardTeamMarker = 0)
        : BattlegroundScore(playerGuid)
    {
        BonusHonor = scoreboardTeamMarker;
    }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class BattlegroundTRT : public Battleground
{
public:
    BattlegroundTRT();
    ~BattlegroundTRT() override = default;

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
    void UpdateTeamScoreWorldStates();
    void AwardPointToTeam(uint32 team);
    void AwardLeavePointIfNeeded(Player const* player, uint32 team);
    void ApplyNonInteractableObjectFlags();
    void SpawnRandomBuffSet(uint32 speedTypeIndex);
    void ConfinePlayers();
    bool ConfineToRegion(Player* player) const;
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
    uint32 _boundsCheckTimer;
};

#endif
