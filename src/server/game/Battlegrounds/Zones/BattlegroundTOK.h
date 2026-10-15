/*
 * Temple of Kotmogu
 * Custom TrinityCore 3.3.5 battleground type 106 on map 998 (ValleyOfPower).
 *
 * The terrain, orb models, icons and orb auras come from the Ascension client
 * data; the rules are retail Mists of Pandaria's. Four Orbs of Power sit on
 * pedestals in the corners of the temple courtyard. Clicking one picks it up;
 * while held it earns the carrier's team victory points every tick - more the
 * closer to the sunken centre of the temple the carrier stands - and killing
 * blows score too. First team to 1600 wins.
 *
 * Holding an orb is a stacking debuff (more damage taken, less healing taken,
 * more damage done) that grows every ten seconds, so an orb cannot be parked
 * forever. A carrier who dies, leaves, mounts, turns immune or otherwise loses
 * the orb aura sends the orb straight back to its pedestal.
 */

#ifndef __BATTLEGROUNDTOK_H
#define __BATTLEGROUNDTOK_H

#include "Battleground.h"
#include "BattlegroundScore.h"
#include "Object.h"

#include <array>

enum BG_TOK_WorldStates
{
    BG_TOK_WORLDSTATE_ALLIANCE_SCORE = 9500,
    BG_TOK_WORLDSTATE_HORDE_SCORE    = 9501,
    BG_TOK_WORLDSTATE_MAX_SCORE      = 9502,
    // StateVariable of both WorldStateUI score rows: 1 shows them, 0 hides.
    BG_TOK_WORLDSTATE_SHOW           = 9503,
    BG_TOK_WORLDSTATE_ALLIANCE_ORBS  = 9504,
    BG_TOK_WORLDSTATE_HORDE_ORBS     = 9505
};

enum BG_TOK_Graveyards
{
    // WorldSafeLocs.dbc. The starts are Ascension's 5978/5979 and the field
    // graveyards its 6034-6037 (retail's own ids), renumbered into this realm's
    // custom block in the same order. The team each one serves is the one its
    // source row names; both of a team's field graveyards stand on the far
    // side of the temple from its start.
    BG_TOK_GY_ALLIANCE_START = 52700,
    BG_TOK_GY_HORDE_START    = 52701,
    BG_TOK_GY_ALLIANCE_1     = 52702,
    BG_TOK_GY_HORDE_1        = 52703,
    BG_TOK_GY_ALLIANCE_2     = 52704,
    BG_TOK_GY_HORDE_2        = 52705
};

enum BG_TOK_Orbs
{
    BG_TOK_ORB_BLUE   = 0,
    BG_TOK_ORB_PURPLE = 1,
    BG_TOK_ORB_GREEN  = 2,
    BG_TOK_ORB_ORANGE = 3,
    BG_TOK_ORB_COUNT  = 4
};

enum BG_TOK_Creatures
{
    BG_TOK_SPIRIT_ALLIANCE_1 = 0,
    BG_TOK_SPIRIT_ALLIANCE_2,
    BG_TOK_SPIRIT_HORDE_1,
    BG_TOK_SPIRIT_HORDE_2,
    BG_TOK_CREATURE_MAX
};

enum BG_TOK_Objects
{
    // The orb itself, one per pedestal, indexed by BG_TOK_Orbs.
    BG_TOK_OBJECT_ORB_FIRST  = 0,
    // The coloured light column standing over each waiting orb. The balls
    // share one neutral texture, so the column is what tells them apart.
    BG_TOK_OBJECT_BEAM_FIRST = BG_TOK_OBJECT_ORB_FIRST + BG_TOK_ORB_COUNT,
    BG_TOK_OBJECT_MAX        = BG_TOK_OBJECT_BEAM_FIRST + BG_TOK_ORB_COUNT
};

enum BG_TOK_ObjectEntries
{
    // gameobject_template. Orbs are lockless GAMEOBJECT_TYPE_FLAGSTAND, so a
    // click reaches EventPlayerClickedOnFlag directly; beams are scenery.
    BG_TOK_ENTRY_ORB_FIRST  = 300220,
    BG_TOK_ENTRY_BEAM_FIRST = 300224
};

enum BG_TOK_Spells
{
    // "Orb of Power: <colour>": the visible stacking debuff. Ascension 844536,
    // 844538, 844540 and 844542, renumbered.
    BG_TOK_SPELL_ORB_STACK_FIRST  = 92300,
    // The hidden holder aura that adds one stack every ten seconds. Ascension
    // 844537, 844539, 844541 and 844546, renumbered. Its presence is what makes
    // a player the carrier: when anything strips it, the orb goes home.
    BG_TOK_SPELL_ORB_HOLDER_FIRST = 92304
};

enum BG_TOK_TrinityStrings
{
    BG_TOK_STRING_ORB_TAKEN    = 20120, // "%s has taken the %s!"
    BG_TOK_STRING_ORB_RETURNED = 20121  // "The %s has returned to the temple."
};

enum BG_TOK_Sounds
{
    BG_TOK_SOUND_ORB_PICKED_UP_ALLIANCE = 8212,
    BG_TOK_SOUND_ORB_PICKED_UP_HORDE    = 8174,
    BG_TOK_SOUND_ORB_RETURNED           = 8192
};

enum BG_TOK_Constants
{
    BG_TOK_MAX_TEAM_SCORE         = 1600,
    // Retail: three points a tick outside the courtyard, four on its raised
    // floor, five down in the sunken centre.
    BG_TOK_POINTS_OUTSIDE         = 3,
    BG_TOK_POINTS_COURTYARD       = 4,
    BG_TOK_POINTS_CENTER          = 5,
    BG_TOK_POINTS_PER_KILL        = 10,
    BG_TOK_POINT_TICK_TIME        = (5 * IN_MILLISECONDS),
    BG_TOK_CARRIER_CHECK_TIME     = 250,
    // Same honor cadence as Eye of the Storm, the other 1600-point race.
    BG_TOK_HONOR_TICKS            = 260,
    BG_TOK_HONOR_TICKS_WEEKEND    = 160
};

struct BattlegroundTOKScore final : public BattlegroundScore
{
    explicit BattlegroundTOKScore(ObjectGuid playerGuid, uint32 scoreboardTeamMarker = 0)
        : BattlegroundScore(playerGuid)
    {
        BonusHonor = scoreboardTeamMarker;
    }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class BattlegroundTOK : public Battleground
{
public:
    BattlegroundTOK();
    ~BattlegroundTOK() override = default;

    void AddPlayer(Player* player) override;
    void RemovePlayer(Player* player, ObjectGuid guid, uint32 team) override;
    void Reset() override;
    bool SetupBattleground() override;
    void PostUpdateImpl(uint32 diff) override;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    void EventPlayerClickedOnFlag(Player* player, GameObject* target_obj) override;
    void EventPlayerDroppedFlag(Player* player) override;

    // One carrier per team for the battlefield map's carrier markers; the full
    // set is GetObjectiveCarriers.
    ObjectGuid GetFlagPickerGUID(int32 team = -1) const override;
    void GetObjectiveCarriers(std::vector<ObjectGuid>& carriers) const override;
    // Playerbot contract: the nearest orb still on its pedestal, and for a
    // carrier the sunken centre, where an orb is worth the most.
    ObjectGuid GetFlagPickupGUID(ObjectGuid playerGuid) const override;
    bool GetFlagCapturePosition(ObjectGuid carrierGuid, Position& position) const override;

    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

private:
    struct OrbState
    {
        ObjectGuid Carrier;
        uint32 CarrierTeam = 0;
    };

    int32 FindOrbHeldBy(ObjectGuid guid) const;
    uint32 CountOrbsHeldBy(uint32 team) const;
    void ReturnOrb(uint8 orb, Player* carrier, bool announce);
    void ShowOrbOnPedestal(uint8 orb, bool show);
    void ValidateCarriers();
    void AwardCarrierPoints();
    void AddPoints(uint32 team, uint32 points);
    void UpdateScoreWorldStates();
    uint32 GetPointsAtCarrierPosition(Player const* carrier) const;

    std::array<OrbState, BG_TOK_ORB_COUNT> _orbs;
    uint32 _pointTickTimer;
    uint32 _carrierCheckTimer;
    uint32 _honorScoreTicks[PVP_TEAMS_COUNT];
    uint32 _honorTicks;
};

#endif
