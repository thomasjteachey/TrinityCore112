/*
 * Summoner's Rift
 * Custom TrinityCore 3.3.5 battleground type 107 on map 1690 (SummonersRift),
 * League of Legends' map imported as a WMO (wow/import/rift). A MOBA: two bases,
 * three lanes, the first team to destroy the other's nexus wins.
 *
 * Alliance plays Order (the south-west base), Horde plays Chaos. Every position
 * is League's own map object placement, converted the way the map geometry was
 * (50 League units to the yard, the map centre on the world origin).
 *
 * Structures are the Goblin Workshop's hybrid: an attackable creature
 * (npc_rts_building, rooted, passive) that summons its own invisible collision
 * shell. A structure cannot be damaged until the one before it falls: outer
 * turret, inner turret, inhibitor turret, inhibitor, in each lane; both nexus
 * turrets open once any one of the team's inhibitors is down, and the nexus once
 * both nexus turrets are. Turrets fire on the battleground's clock, minions
 * before players, the way League's do: a glowing orb (a creature wearing League's
 * own shot sprites) leaves the turret's crystal, homes on its target and lands
 * with a burst, and only then deals its damage. A fallen inhibitor stands back up
 * after five minutes.
 *
 * Minion waves leave both bases every 30 seconds down all three lanes, walking
 * League's own lane waypoints as one smooth path, and fight whatever enemy they
 * meet. Positions, ranges and speeds are League's, scaled by SRF_MAP_SCALE (the
 * size the map was converted at). The fountains
 * heal their own team and burn the other.
 *
 * Units, structures and the shot effects are League's own models and textures,
 * converted by wow/import/rift/scripts. This is still the skeleton: caster minions
 * fight in melee, there are no super minions, jungle camps, gold or items yet.
 */

#ifndef __BATTLEGROUNDSRF_H
#define __BATTLEGROUNDSRF_H

#include "Battleground.h"
#include "BattlegroundScore.h"
#include "Object.h"

#include <array>
#include <vector>

enum BG_SRF_Graveyards
{
    // WorldSafeLocs.dbc: the fountains, which are the starts and the graveyards.
    BG_SRF_GY_ORDER = 52710,
    BG_SRF_GY_CHAOS = 52711
};

enum BG_SRF_CreatureEntries
{
    // creature_template. Structures clone the Goblin Workshop (900116) and pair
    // with their collision shells (gameobjects 300600-300605) in rts_building.cpp.
    BG_SRF_ENTRY_ORDER_TURRET    = 922100,
    BG_SRF_ENTRY_CHAOS_TURRET    = 922101,
    BG_SRF_ENTRY_ORDER_INHIBITOR = 922102,
    BG_SRF_ENTRY_CHAOS_INHIBITOR = 922103,
    BG_SRF_ENTRY_ORDER_NEXUS     = 922104,
    BG_SRF_ENTRY_CHAOS_NEXUS     = 922105,
    BG_SRF_ENTRY_ORDER_MELEE     = 922110,
    BG_SRF_ENTRY_ORDER_CASTER    = 922111,
    BG_SRF_ENTRY_CHAOS_MELEE     = 922112,
    BG_SRF_ENTRY_CHAOS_CASTER    = 922113,
    // The turret shot's orb and its landing burst: effect-only creatures.
    BG_SRF_ENTRY_ORDER_SHOT        = 922120,
    BG_SRF_ENTRY_CHAOS_SHOT        = 922121,
    BG_SRF_ENTRY_ORDER_SHOT_IMPACT = 922122,
    BG_SRF_ENTRY_CHAOS_SHOT_IMPACT = 922123
};

enum BG_SRF_Spells
{
    // "dmg": instant school damage with no visual of its own, cast when a turret
    // shot lands. The damage is set per shot.
    BG_SRF_SPELL_TURRET_SHOT = 7482
};

enum BG_SRF_TrinityStrings
{
    BG_SRF_STRING_TURRET_DESTROYED    = 20130, // "%s has destroyed a turret!"
    BG_SRF_STRING_INHIBITOR_DESTROYED = 20131, // "%s has destroyed an inhibitor!"
    BG_SRF_STRING_INHIBITOR_RESPAWNED = 20132, // "%s's inhibitor is respawning."
    BG_SRF_STRING_MINIONS_SPAWNED     = 20133  // "Minions have spawned!"
};

enum BG_SRF_Lanes
{
    BG_SRF_LANE_TOP = 0,
    BG_SRF_LANE_MID = 1,
    BG_SRF_LANE_BOT = 2,
    BG_SRF_LANE_COUNT,
    BG_SRF_LANE_NONE = BG_SRF_LANE_COUNT
};

enum BG_SRF_StructureKind
{
    BG_SRF_KIND_TURRET,
    BG_SRF_KIND_INHIBITOR,
    BG_SRF_KIND_NEXUS
};

// Structure slots in BgCreatures, per team: three lanes of outer, inner and
// inhibitor turret plus the inhibitor, then the two nexus turrets and the nexus.
enum BG_SRF_StructureSlots
{
    BG_SRF_STRUCTURES_PER_TEAM = 3 * 4 + 3,
    BG_SRF_CREATURE_SPIRIT_ORDER = 2 * BG_SRF_STRUCTURES_PER_TEAM,
    BG_SRF_CREATURE_SPIRIT_CHAOS,
    BG_SRF_CREATURE_MAX
};

enum BG_SRF_Constants
{
    BG_SRF_FIRST_WAVE_TIME       = 15 * IN_MILLISECONDS,
    BG_SRF_WAVE_INTERVAL         = 30 * IN_MILLISECONDS,
    BG_SRF_MELEE_PER_WAVE        = 3,
    BG_SRF_CASTERS_PER_WAVE      = 3,
    // A wave that cannot be fought off must not pile up without end.
    BG_SRF_MAX_MINIONS_PER_TEAM  = 90,
    BG_SRF_MINION_TICK           = 500,
    BG_SRF_TURRET_TICK           = 1200,
    BG_SRF_SHOT_TICK             = 100,
    BG_SRF_SHOT_LIFETIME         = 4 * IN_MILLISECONDS,
    BG_SRF_FOUNTAIN_TICK         = 1 * IN_MILLISECONDS,
    BG_SRF_STRUCTURE_CHECK_TICK  = 500,
    BG_SRF_INHIBITOR_RESPAWN     = 5 * MINUTE * IN_MILLISECONDS,
    // Honor per structure destroyed, paid to the destroying team.
    BG_SRF_HONOR_TURRET          = 10,
    BG_SRF_HONOR_INHIBITOR       = 15
};

struct BattlegroundSRFScore final : public BattlegroundScore
{
    explicit BattlegroundSRFScore(ObjectGuid playerGuid, uint32 scoreboardTeamMarker = 0)
        : BattlegroundScore(playerGuid)
    {
        BonusHonor = scoreboardTeamMarker;
    }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class BattlegroundSRF : public Battleground
{
public:
    BattlegroundSRF();
    ~BattlegroundSRF() override = default;

    void AddPlayer(Player* player) override;
    void Reset() override;
    bool SetupBattleground() override;
    void PostUpdateImpl(uint32 diff) override;
    void StartingEventOpenDoors() override;

    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

private:
    struct StructureState
    {
        bool Destroyed = false;
        bool Vulnerable = false;
        uint32 RespawnTimer = 0;   // inhibitors only
    };

    struct Minion
    {
        ObjectGuid Guid;
        TeamId Team = TEAM_ALLIANCE;
        uint8 Lane = BG_SRF_LANE_TOP;
        uint32 PathIndex = 0;      // how far along its lane path (LanePath) it has come
        uint32 SplineEnd = 0;      // the last path point of the spline it is walking
        bool Walking = false;
    };

    struct TurretState
    {
        ObjectGuid Target;
        uint8 ConsecutiveHits = 0;
    };

    // A turret shot in flight: the orb creature, who fired it and at whom.
    struct Shot
    {
        ObjectGuid Orb;
        ObjectGuid Target;
        uint8 TurretSlot = 0;
        int32 Damage = 0;
        uint32 Age = 0;
    };

    bool SpawnStructure(uint8 slot);
    void UpdateStructures();
    void RefreshVulnerability();
    void SetVulnerable(uint8 slot, bool vulnerable);
    void OnStructureDestroyed(uint8 slot);

    void SpawnWave();
    std::vector<Position> const& LanePath(TeamId team, uint8 lane);
    void UpdateMinions();
    void DespawnMinions();
    Unit* FindMinionTarget(Creature* minion, TeamId team, float range) const;

    void UpdateTurrets();
    Unit* PickTurretTarget(Creature* turret, TeamId team) const;
    void FireShot(uint8 slot, Creature* turret, Unit* target, int32 damage);
    void UpdateShots(uint32 elapsed);
    void DespawnShots();

    void UpdateFountains();

    std::array<StructureState, 2 * BG_SRF_STRUCTURES_PER_TEAM> _structures;
    std::array<TurretState, 2 * BG_SRF_STRUCTURES_PER_TEAM> _turrets;
    std::vector<Minion> _minions;
    std::array<std::array<std::vector<Position>, BG_SRF_LANE_COUNT>, PVP_TEAMS_COUNT> _lanePaths;
    std::vector<Shot> _shots;
    uint32 _waveTimer;
    uint32 _minionTimer;
    uint32 _turretTimer;
    uint32 _shotTimer;
    uint32 _fountainTimer;
    uint32 _structureTimer;
    bool _wavesStarted;
};

#endif
