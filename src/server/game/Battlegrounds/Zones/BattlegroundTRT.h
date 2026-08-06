/*
 * Tanaris team deathmatch battleground
 * Custom TrinityCore 3.3.5 battleground type 104 (map 1620, cloned from map 1)
 *
 * Straight deathmatch: one point per kill, first side to the kill limit wins.
 * This is the baseline the RTS layer will be built on top of.
 *
 * What makes this one different from the other custom battlegrounds here is
 * that it is fought OUTDOORS across the whole Tanaris zone rather than inside a
 * building. There are no walls, so containment is a coordinate check on a timer
 * (ConfinePlayers) instead of a ring of doors.
 *
 * Map 1620 ships its own copy of the Tanaris tiles plus a two-tile horizon
 * ring, so the terrain outside the playable bounds is real ground rather than a
 * hole. Leaving the bounds is therefore a gameplay problem, not a
 * falling-through-the-world one, and the fix is to slide the player back inside
 * rather than yank them to a graveyard.
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

enum BG_TRT_Constants
{
    BG_TRT_KILL_LIMIT = 30,
    // Bounds and fall-through are both checked on this timer rather than every
    // tick. Crossing the boundary takes far longer than the interval at any
    // achievable run speed, so nothing can slip past between checks.
    BG_TRT_BOUNDS_CHECK_INTERVAL = 500
};

// The playable region, in world coordinates on map 1620: the whole Tanaris
// zone, which is the ground the RTS layer is meant to be fought over.
//
// These are the extents of the tiles that actually contain Tanaris areas
// (server grid x 43-51, y 34-42). The client patch ships a further two-tile
// ring beyond this in every direction, so a player pushed back at the edge is
// always standing on real terrain and always has terrain on the horizon.
constexpr float BG_TRT_FIELD_MIN_X = -10700.0f;
constexpr float BG_TRT_FIELD_MAX_X =  -5850.0f;
constexpr float BG_TRT_FIELD_MIN_Y =  -5900.0f;
constexpr float BG_TRT_FIELD_MAX_Y =  -1030.0f;

// How far inside the boundary a player is put back. Landing exactly on the
// edge would re-trigger the check on the next pass from any outward drift.
constexpr float BG_TRT_BOUNDS_INSET = 10.0f;

// Before the match starts each team is held near its own spawn. There are no
// start gates on an open zone, so this radius is what keeps the two sides
// apart during the warmup.
constexpr float BG_TRT_WARMUP_RADIUS = 60.0f;

// Tanaris drops well below sea level in places -- Zul'Farrak and the sunken
// ground near the Gaping Chasm sit around z -270 -- so the fall-through guard
// has to sit below those to avoid teleporting players out of legitimate
// terrain. The map's own MinHeight is -500.
constexpr float BG_TRT_MIN_SAFE_Z = -400.0f;

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

private:
    void UpdateTeamScoreWorldStates();
    void AwardPointToTeam(uint32 team);
    void AwardLeavePointIfNeeded(Player const* player, uint32 team);
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
