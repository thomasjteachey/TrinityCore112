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
 * before players, the way League's do, with a stock missile spell (Ice Lance for
 * Order, Fireball for Chaos) whose damage lands when it arrives. A fallen inhibitor stands back up
 * after five minutes.
 *
 * Minion waves leave both bases every 30 seconds down all three lanes, walking
 * League's own lane waypoints as one smooth path, and fight whatever enemy they
 * meet. Positions, ranges and speeds are League's, scaled by SRF_MAP_SCALE (the
 * size the map was converted at). The fountains
 * heal their own team and burn the other.
 *
 * The jungle is League's: its camps at League's own spots, neutral to both teams,
 * fighting as one camp and leashing home, first up 90 seconds after the gates open
 * and back a League respawn time after they are cleared. The buff camps give their
 * killer a buff, the Dragon gives its whole team a stack of the Dragonslayer's cry
 * for the rest of the match, and Baron Nashor, from 20 minutes, gives the living
 * members of its team three minutes of Zandalar's spirit and their minions more
 * health. League's buffs are stood in for by stock spells with the values set here.
 *
 * Units, structures and the shot effects are League's own models and textures,
 * converted by wow/import/rift/scripts.
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
    // A turret's eye: an invisible caster hovering over its crystal. Turret shots
    // leave from it - clear of the turret's own collision shell, which blocks a
    // shot's line of sight from inside, and from where the shot should start.
    BG_SRF_ENTRY_TURRET_EYE      = 922120,
    // The jungle: neutral monsters, attackable by both teams.
    BG_SRF_ENTRY_BLUE_SENTINEL     = 922130,
    BG_SRF_ENTRY_RED_BRAMBLEBACK   = 922131,
    BG_SRF_ENTRY_GREATER_MURK_WOLF = 922132,
    BG_SRF_ENTRY_MURK_WOLF         = 922133,
    BG_SRF_ENTRY_CRIMSON_RAPTOR    = 922134,
    BG_SRF_ENTRY_RAPTOR            = 922135,
    BG_SRF_ENTRY_ANCIENT_KRUG      = 922136,
    BG_SRF_ENTRY_KRUG              = 922137,
    BG_SRF_ENTRY_GROMP             = 922138,
    BG_SRF_ENTRY_RIFT_SCUTTLER     = 922139,
    BG_SRF_ENTRY_DRAGON            = 922140,
    BG_SRF_ENTRY_BARON_NASHOR      = 922141
};

enum BG_SRF_Spells
{
    // Turret shots and caster minion bolts are stock missile spells - plain school
    // damage, nothing else - so the client draws the flight and the impact and the
    // damage lands on arrival. The damage is set per cast.
    BG_SRF_SPELL_ORDER_TURRET_SHOT = 43427, // "Ice Lance", 38 yd/s
    BG_SRF_SPELL_CHAOS_TURRET_SHOT = 22088, // "Fireball", 36 yd/s
    BG_SRF_SPELL_ORDER_CASTER_BOLT = 13748, // "Arcane Bolt", 24 yd/s
    BG_SRF_SPELL_CHAOS_CASTER_BOLT = 9613,  // "Shadow Bolt", 24 yd/s
    // Stand-ins for League's buffs, their values and durations set when cast.
    BG_SRF_SPELL_BLUE_BUFF   = 18194, // "Mana Regeneration": periodic mana
    BG_SRF_SPELL_RED_BUFF    = 22817, // "Fengus' Ferocity": attack power
    BG_SRF_SPELL_DRAGON_BUFF = 22888, // "Rallying Cry of the Dragonslayer": crit, attack power
    BG_SRF_SPELL_BARON_BUFF  = 24425  // "Spirit of Zandalar": speed, all stats
};

enum BG_SRF_TrinityStrings
{
    BG_SRF_STRING_TURRET_DESTROYED    = 20130, // "%s has destroyed a turret!"
    BG_SRF_STRING_INHIBITOR_DESTROYED = 20131, // "%s has destroyed an inhibitor!"
    BG_SRF_STRING_INHIBITOR_RESPAWNED = 20132, // "%s's inhibitor is respawning."
    BG_SRF_STRING_MINIONS_SPAWNED     = 20133, // "Minions have spawned!"
    BG_SRF_STRING_DRAGON_SPAWNED      = 20134, // "The Dragon has spawned!"
    BG_SRF_STRING_BARON_SPAWNED       = 20135, // "Baron Nashor has spawned!"
    BG_SRF_STRING_DRAGON_SLAIN        = 20136, // "%s has slain the Dragon!"
    BG_SRF_STRING_BARON_SLAIN         = 20137  // "%s has slain Baron Nashor!"
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
// After them the spirit guides and the jungle's monsters (SRF_MONSTERS order).
enum BG_SRF_StructureSlots
{
    BG_SRF_STRUCTURES_PER_TEAM = 3 * 4 + 3,
    BG_SRF_CREATURE_SPIRIT_ORDER = 2 * BG_SRF_STRUCTURES_PER_TEAM,
    BG_SRF_CREATURE_SPIRIT_CHAOS,
    BG_SRF_CREATURE_JUNGLE_FIRST,
    // per side Blue, Red, Gromp, 3 wolves, 6 raptors, 2 krugs; two scuttlers, Dragon, Baron
    BG_SRF_JUNGLE_MONSTERS = 2 * 14 + 2 + 1 + 1,
    // per side Blue, wolves, raptors, Red, krugs, Gromp; the two scuttlers, Dragon, Baron
    BG_SRF_JUNGLE_CAMPS = 2 * 6 + 2 + 1 + 1,
    BG_SRF_CREATURE_MAX = BG_SRF_CREATURE_JUNGLE_FIRST + BG_SRF_JUNGLE_MONSTERS
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
    void RemovePlayer(Player* player, ObjectGuid guid, uint32 team) override;
    void Reset() override;
    bool SetupBattleground() override;
    void PostUpdateImpl(uint32 diff) override;
    void StartingEventOpenDoors() override;

    void HandleKillUnit(Creature* creature, Player* killer) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;

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
        bool Caster = false;
        uint8 Slot = 0;            // its place in the wave: rank (melee first) and file
        uint32 AttackTimer = 0;    // casters: ms until the next bolt
        uint32 StartDelay = 0;     // ms before it leaves the barracks
        bool Denied = false;       // turned neutral under half health (the denying rule)
        bool Paid = false;         // its gold went to a last hit already
        Position LastPos;          // where it stood at the last tick that moved it
        uint32 StillMs = 0;        // how long it has stood there without fighting
        ObjectGuid Target;         // the target the battleground last sent it after
        ObjectGuid Ignore;         // a target it could not get to, left alone for a while
        uint32 IgnoreMs = 0;
    };

    struct CampState
    {
        bool Up = false;
        uint32 Timer = 0;          // ms until it spawns, while it is not up
    };

    struct TurretState
    {
        ObjectGuid Target;
        ObjectGuid Eye;
        uint8 ConsecutiveHits = 0;
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
    Unit* FindMinionTarget(Creature* minion, TeamId team, float range, ObjectGuid ignore = ObjectGuid::Empty) const;
    bool UnstickMinion(Creature* creature, Minion& m);

    void UpdateTurrets();
    Unit* PickTurretTarget(Creature* turret, TeamId team) const;
    void FireBolt(Unit* source, Unit* target, uint32 spellId, int32 damage);
    Creature* TurretEye(uint8 slot, Creature* turret);
    void Report(uint32 diff);
    void UpdateCasterAttack(Creature* caster, Minion& minion, Unit* victim);

    void UpdateFountains();

    void SpawnCamp(uint8 camp);
    void UpdateJungle(uint32 elapsed);
    bool HandleJungleKill(Creature* creature, Player* killer);
    void ApplyBuff(Player* player, uint32 spellId, int32 bp0, int32 bp1, int32 bp2, int32 durationMs);
    void ApplyDragonBuff(Player* player, TeamId team);
    void MaintainObjectiveBuffs();

    // Progression (RiftProgression): experience and gold, and keeping the
    // units' levels with the players'.
    std::vector<Player*> TeamPlayers(TeamId team) const;
    std::vector<Player*> TeamPlayersNear(TeamId team, WorldObject const* where, float range) const;
    void AwardExperience(TeamId team, WorldObject const* where, float leagueXp);
    void AwardGold(TeamId team, WorldObject const* where, uint32 leagueGold);
    void OnMinionDied(Minion const& minion, Creature const* corpse);
    uint8 AverageMatchLevel() const;
    void ApplyUnitLevel(Creature* creature, uint8 level, float healthScale);
    void UpdateProgression(uint32 diff);

    std::array<StructureState, 2 * BG_SRF_STRUCTURES_PER_TEAM> _structures;
    std::array<TurretState, 2 * BG_SRF_STRUCTURES_PER_TEAM> _turrets;
    std::vector<Minion> _minions;
    std::array<std::array<std::vector<Position>, BG_SRF_LANE_COUNT>, PVP_TEAMS_COUNT> _lanePaths;
    std::array<CampState, BG_SRF_JUNGLE_CAMPS> _camps;
    std::array<uint8, PVP_TEAMS_COUNT> _dragonsSlain;
    std::array<uint32, PVP_TEAMS_COUNT> _baronTimer;   // ms of Baron's empowerment left
    uint32 _jungleTimer;
    // What the match did, for the periodic log line (Report).
    uint32 _reportTimer;
    uint32 _boltsCast;
    uint32 _boltsRefused;
    uint32 _lastRefusal;
    uint32 _unstuckTargets;        // since the last report: unreachable targets given up
    uint32 _unstuckPlaced;         // since the last report: minions put back on their lane
    uint32 _waveTimer;
    uint32 _minionTimer;
    uint32 _turretTimer;
    uint32 _fountainTimer;
    uint32 _structureTimer;
    uint32 _passiveGoldTimer;
    uint32 _maintainTimer;
    uint32 _structureLevelTimer;
    uint32 _wavesSpawned;
    bool _wavesStarted;
};

#endif
