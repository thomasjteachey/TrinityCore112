/*
 * Obsidian Colosseum deathmatch + center-flag battleground
 * Custom TrinityCore 3.3.5 battleground type 102 (map 1615, cloned from 615)
 *
 * Deathmatch scoring like Blackrock Throne (1 point per kill), plus a single
 * Eye of the Storm style flag at the center of the arena. Picking the flag up
 * spawns a light beam at the ENEMY base; carrying the flag to that light caps
 * it for bonus points. No flag debuffs of any kind - the dropper can pick the
 * flag right back up.
 */

#ifndef __BATTLEGROUNDOBC_H
#define __BATTLEGROUNDOBC_H

#include "Battleground.h"
#include "BattlegroundScore.h"
#include "Object.h"

enum BG_OBC_WorldStates
{
    BG_OBC_WORLDSTATE_ALLIANCE_SCORE = 9200,
    BG_OBC_WORLDSTATE_HORDE_SCORE    = 9201,
    BG_OBC_WORLDSTATE_MAX_SCORE      = 9202,
    BG_OBC_WORLDSTATE_TIMER_ACTIVE   = 9203,
    BG_OBC_WORLDSTATE_TIMER          = 9204,
    BG_OBC_WORLDSTATE_SHOW           = 9205,
    BG_OBC_WORLDSTATE_MAX_SCORE_UI   = 9206
};

enum BG_OBC_Graveyards
{
    BG_OBC_GY_ALLIANCE_START = 52320,
    BG_OBC_GY_HORDE_START    = 52321,
    BG_OBC_GY_ALLIANCE       = 52322,
    BG_OBC_GY_HORDE          = 52323
};

enum BG_OBC_Creatures
{
    BG_OBC_SPIRIT_ALLIANCE = 0,
    BG_OBC_SPIRIT_HORDE    = 1,
    BG_OBC_CREATURE_MAX    = 2
};

enum BG_OBC_Objects
{
    // Horde-side start gates (X ~3381-3418)
    BG_OBC_OBJECT_GATE_H_1 = 0,
    BG_OBC_OBJECT_GATE_H_2,
    BG_OBC_OBJECT_GATE_H_3,
    BG_OBC_OBJECT_GATE_H_4,
    BG_OBC_OBJECT_GATE_H_5,

    // Alliance-side start gates (X ~3092-3126)
    BG_OBC_OBJECT_GATE_A_1,
    BG_OBC_OBJECT_GATE_A_2,
    BG_OBC_OBJECT_GATE_A_3,
    BG_OBC_OBJECT_GATE_A_4,

    BG_OBC_OBJECT_FLAG,               // clickable flag stand at the arena center

    // Objective light beams. Exactly one is visible while the flag is
    // carried: the light marks the ENEMY base the carrier must reach.
    BG_OBC_OBJECT_LIGHT_ALLIANCE_BASE,
    BG_OBC_OBJECT_LIGHT_HORDE_BASE,

    BG_OBC_OBJECT_MAX
};

enum BG_OBC_ObjectEntries
{
    BG_OBC_GATE_ENTRY         = 185483,
    BG_OBC_FLAG_STAND_ENTRY   = 300206, // OBC Netherstorm Flag (instant GAMEOBJECT_TYPE_FLAGSTAND)
    BG_OBC_FLAG_DROP_ENTRY    = 184142, // Netherstorm Flag (GAMEOBJECT_TYPE_FLAGDROP)
    BG_OBC_LIGHT_BEAM_ENTRY   = 300010  // BG Objective Light Beam
};

enum BG_OBC_Spells
{
    BG_OBC_NETHERSTORM_FLAG_SPELL    = 34976, // carrier visual aura
    BG_OBC_PLAYER_DROPPED_FLAG_SPELL = 34991  // summons the ground flag (184142); NOT a debuff
};

enum BG_OBC_FlagState
{
    BG_OBC_FLAG_STATE_ON_BASE = 0,
    BG_OBC_FLAG_STATE_ON_PLAYER,
    BG_OBC_FLAG_STATE_ON_GROUND,
    BG_OBC_FLAG_STATE_WAIT_RESPAWN
};

enum BG_OBC_Sounds
{
    BG_OBC_SOUND_FLAG_PICKED_UP_ALLIANCE = 8212,
    BG_OBC_SOUND_FLAG_CAPTURED_HORDE     = 8213,
    BG_OBC_SOUND_FLAG_PICKED_UP_HORDE    = 8174,
    BG_OBC_SOUND_FLAG_CAPTURED_ALLIANCE  = 8173,
    BG_OBC_SOUND_FLAG_RESET              = 8192
};

enum BG_OBC_BroadcastTexts
{
    BG_OBC_TEXT_TAKEN_FLAG             = 18359,
    BG_OBC_TEXT_FLAG_DROPPED           = 18361,
    BG_OBC_TEXT_FLAG_RESET             = 18364,
    BG_OBC_TEXT_ALLIANCE_CAPTURED_FLAG = 18375,
    BG_OBC_TEXT_HORDE_CAPTURED_FLAG    = 18384
};

enum BG_OBC_Constants
{
    BG_OBC_SCORE_LIMIT        = 30,
    BG_OBC_POINTS_PER_KILL    = 1,
    BG_OBC_POINTS_PER_CAPTURE = 2,
    BG_OBC_FLAG_RESPAWN_TIME  = (8 * IN_MILLISECONDS)
};

float constexpr BG_OBC_CAPTURE_RADIUS = 10.0f;

struct BattlegroundOBCScore final : public BattlegroundScore
{
    explicit BattlegroundOBCScore(ObjectGuid playerGuid, uint32 scoreboardTeamMarker = 0)
        : BattlegroundScore(playerGuid)
    {
        BonusHonor = scoreboardTeamMarker;
    }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class BattlegroundOBC : public Battleground
{
public:
    BattlegroundOBC();
    ~BattlegroundOBC() override = default;

    void AddPlayer(Player* player) override;
    void RemovePlayer(Player* player, ObjectGuid guid, uint32 team) override;
    void Reset() override;
    bool SetupBattleground() override;
    void PostUpdateImpl(uint32 diff) override;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    void EventPlayerClickedOnFlag(Player* player, GameObject* target_obj) override;
    void EventPlayerDroppedFlag(Player* player) override;
    ObjectGuid GetFlagPickerGUID(int32 /*team*/ = -1) const override { return _flagCarrierGuid; }
    void SetDroppedFlagGUID(ObjectGuid guid, int32 /*team*/ = -1) override { _droppedFlagGuid = guid; }

    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

private:
    bool IsFlagPickedup() const { return !_flagCarrierGuid.IsEmpty(); }
    void SetFlagPicker(ObjectGuid guid) { _flagCarrierGuid = guid; }
    void EventPlayerCapturedFlag(Player* player);
    void RespawnFlag();
    void RespawnFlagAfterDrop();
    void UpdateObjectiveLights();
    void UpdateTeamScoreWorldStates();
    void AwardPointsToTeam(uint32 team, uint32 points);
    void AwardLeavePointIfNeeded(Player const* player, uint32 team);
    uint32 GetHonorRewardForTeam() const;
    void ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const override;
    void TrackHumanParticipantAdded(Player const* player, bool isInBattleground);
    void TrackHumanParticipantRemoved(Player const* player, uint32 team);
    void UpdateHumanFaceoffState();
    void ApplyNonInteractableObjectFlags();

    uint32 _allianceScore;
    uint32 _hordeScore;
    uint32 _allianceHumanParticipants;
    uint32 _hordeHumanParticipants;
    bool _humanFaceoffEverHappened;
    uint8 _flagState;
    int32 _flagResetTimer;
    ObjectGuid _flagCarrierGuid;
    ObjectGuid _droppedFlagGuid;
};

#endif
