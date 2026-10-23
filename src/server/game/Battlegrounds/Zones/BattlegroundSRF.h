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
 * killer League Season 3's Crest of the Ancient Golem or Blessing of the Lizard
 * Elder (the champion who kills the holder takes it), the Dragon gives gold to its
 * whole team, and Baron Nashor, from 15 minutes, gold, experience and four minutes
 * of Exalted with Baron Nashor to the living of its team. The buffs are the Rift's
 * own spells (94161-94164); RiftProgression makes them do what League's did.
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
#include <unordered_map>
#include <unordered_set>
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
    // League's siege (cannon) and super minions (Season 3: rift_league_unit)
    BG_SRF_ENTRY_ORDER_SIEGE     = 922114,
    BG_SRF_ENTRY_ORDER_SUPER     = 922115,
    BG_SRF_ENTRY_CHAOS_SIEGE     = 922116,
    BG_SRF_ENTRY_CHAOS_SUPER     = 922117,
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
    BG_SRF_ENTRY_BARON_NASHOR      = 922141,
    // The fountain shopkeepers (their stock: import/rift/scripts/rift_shop.py).
    BG_SRF_ENTRY_ORDER_SHOP        = 922150,
    BG_SRF_ENTRY_CHAOS_SHOP        = 922151,
    BG_SRF_ENTRY_ORDER_ARMSMASTER  = 922152,   // npc_rift_armsmaster: the kit's weapons
    BG_SRF_ENTRY_CHAOS_ARMSMASTER  = 922153
};

enum BG_SRF_Spells
{
    // Turret shots and caster minion bolts are stock missile spells - plain school
    // damage, nothing else - so the client draws the flight and the impact and the
    // damage lands on arrival. The damage is set per cast.
    // Centurion custom shots (import/rift/scripts/rift_shot_spells.py): both sides
    // on exact parity - Arcane, unmissable, no resistance - only the missile differs.
    BG_SRF_SPELL_ORDER_TURRET_SHOT = 94143, // "Turret Shot", Ice Lance's missile, 38 yd/s
    BG_SRF_SPELL_CHAOS_TURRET_SHOT = 94144, // "Turret Shot", Fireball's missile, 36 yd/s
    BG_SRF_SPELL_ORDER_CASTER_BOLT = 94145, // "Minion Bolt", Arcane Bolt's missile, 24 yd/s
    BG_SRF_SPELL_CHAOS_CASTER_BOLT = 94146, // "Minion Bolt", Shadow Bolt's missile, 24 yd/s
    // Stand-ins for League's buffs, their values and durations set when cast.
    BG_SRF_SPELL_BLUE_BUFF   = 94161, // "Crest of the Ancient Golem": cooldowns, mana
    BG_SRF_SPELL_RED_BUFF    = 94162, // "Blessing of the Lizard Elder": basic attacks burn and slow
    BG_SRF_SPELL_RED_BURN    = 94163, // the Blessing's burn and slow, on the one struck
    // The Season 3 Dragon (npc_rift_dragon, DragonAttack): its ranged attack - the
    // Chaos turret shot's Fireball missile, renamed - and the burn it leaves.
    BG_SRF_SPELL_DRAGON_BURN = 94168, // "Dragon Fire": true damage (spell_rift_dragon_burn), 20% less attack speed
    BG_SRF_SPELL_DRAGON_FIRE = 94169, // "Dragon Fire": the missile, its damage set per cast
    BG_SRF_SPELL_BARON_BUFF  = 94164, // "Exalted with Baron Nashor": damage, ability power, regeneration
    // The Season 3 Baron Nashor (npc_rift_baron, Baron*): its attacks' missiles, their
    // damage set per cast, and Propelled's puddle and slow.
    BG_SRF_SPELL_BARON_WRATH     = 94177, // "Wrath of the Ancients": magic, a stack of +25% magic damage taken
    BG_SRF_SPELL_BARON_CORROSION = 94178, // "Voracious Corrosion": physical; half damage to Baron after it
    BG_SRF_SPELL_BARON_PUDDLE    = 94179, // "Propelled": the ooze on the ground before the knock-up
    BG_SRF_SPELL_BARON_PROPELLED = 94180  // "Propelled": the slow after it
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
    BG_SRF_CREATURE_SHOP_ORDER,
    BG_SRF_CREATURE_SHOP_CHAOS,
    BG_SRF_CREATURE_JUNGLE_FIRST,
    // per side Blue, Red, Gromp, 3 wolves, 6 raptors, 2 krugs; two scuttlers, Dragon, Baron
    BG_SRF_JUNGLE_MONSTERS = 2 * 14 + 2 + 1 + 1,
    // per side Blue, wolves, raptors, Red, krugs, Gromp; the two scuttlers, Dragon, Baron
    BG_SRF_JUNGLE_CAMPS = 2 * 6 + 2 + 1 + 1,
    BG_SRF_CREATURE_ARMS_ORDER = BG_SRF_CREATURE_JUNGLE_FIRST + BG_SRF_JUNGLE_MONSTERS,
    BG_SRF_CREATURE_ARMS_CHAOS,
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
    BG_SRF_TURRET_TICK           = 1200,  // the shortest time between a turret's shots
    BG_SRF_TURRET_STEP           = 100,   // how often the turrets are looked at
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

class Pet;

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

    // League's death timers: no spirit-guide waves - every champion comes back
    // at its fountain on its own clock (UpdateRespawns).
    uint32 GetResurrectionInterval() const override;

    // At its own base - the fountain and its shop - the one place a character
    // spends skill picks and talent points (RiftProgression).
    bool IsAtBase(Player const* player) const;

    // League's bots, for the playerbot battleground strategy: where a bot should
    // be and what it should hit, by League's laning rules - its own lane (or the
    // jungle), just behind its own minion wave, never under a standing enemy
    // turret unless allied minions are taking that turret's fire, back to the
    // fountain to heal and shop when low or rich.
    bool GetBotOrders(Player* bot, BattlegroundBotOrders& out) override;
    // False when attacking `target` would take `bot` under a standing enemy turret
    // that is free to shoot it - League's "no tower diving".
    bool BotMayAttack(Player* bot, Unit* target) const;

    // League's brush: a champion in a brush patch is unseen by every enemy - player,
    // minion, turret - outside that patch, until it attacks.
    bool HidesFrom(Player const* target, WorldObject const* seer) const override;
    // League's kill credit and assists: an enemy champion's damage or ability on
    // `victim` (custom_rift_progression.cpp's hooks). Within 10 s of a death it
    // makes an assist, and the latest one takes the kill a turret, a minion or a
    // monster finished.
    void RecordChampionContribution(Player* attacker, Player* victim);

    // Minions, turrets and camps hit bots and players alike, and take the same from both.
    bool UsesBotCreatureScaling() const override { return false; }
    // League's combat (Battleground::UsesLeagueCombat), armour read at the
    // champions' average level - the one their League armour is converted at.
    bool UsesLeagueCombat() const override { return true; }
    // League's death timers (DeathTimer, UpdateRespawns): no spirit-guide waves.
    bool UsesSpiritGuideWaves() const override { return false; }
    uint32 GetTimeUntilResurrectionFor(Player const* player) const override;
    uint8 GetLeagueCombatLevel() const override { return _combatLevel; }
    // A League match runs two or three Warsongs: Centurion.Rift.FlatHonorMultiplier
    // until the hourly payout has the Rift's own medians (owner 2026-10-22: "2x the
    // honor for now", then 3v3 arena's honor an hour).
    float FlatHonorMultiplier() const override;
    // The Season 3 Dragon's basic attack, timed by its AI (npc_rift_dragon): reach
    // and the shot itself.
    float DragonRange() const;
    void DragonAttack(Creature* dragon, Unit* victim);
    // The Season 3 Baron Nashor's, timed by its AI (npc_rift_baron): reach, a bolt of
    // Wrath of the Ancients, Voracious Corrosion, Sweeping Blow (every champion close
    // by knocked away) and Propelled (the puddle under `victim` now, the knock-up and
    // slow of whoever stands in it a second later).
    float BaronRange() const;
    float BaronSweepReach() const;
    void BaronWrath(Creature* baron, Unit* victim);
    void BaronCorrosion(Creature* baron, Unit* victim);
    void BaronSweep(Creature* baron);
    void BaronPropel(Creature* baron, Unit* victim);
    // League's lobby: a seat is taken before the match, never during it (owner
    // 2026-10-22: "like arenas ... not join late").
    bool TakesSeatsInPreparationOnly() const override { return true; }
    // Its own field kit, talent points from level 10 and shop (RiftProgression):
    // none of the tournament's rules, from the tournament queue too - the custom
    // game never had them (owner 2026-10-22: "you cannot spend points in a
    // tournament match wtf").
    bool PlaysByTournamentRules() const override { return false; }
    void EndBattleground(uint32 winner) override;

private:
    struct StructureState
    {
        bool Destroyed = false;
        bool Vulnerable = false;
        uint32 RespawnTimer = 0;   // inhibitors only
        bool Backdoor = false;     // League's backdoor plating on: no enemy minion near
        // what UpdateStructures last saw, to log a structure's health rising (nothing heals
        // one in League: the mid Chaos turret was seen going from 1 health to half, 2026-10-22)
        uint32 LastHealth = 0;
        uint32 LastMaxHealth = 0;
    };

    struct Minion
    {
        ObjectGuid Guid;
        TeamId Team = TEAM_ALLIANCE;
        uint8 Lane = BG_SRF_LANE_TOP;
        uint32 PathIndex = 0;      // how far along its lane path (LanePath) it has come
        uint32 SplineEnd = 0;      // the last path point of the spline it is walking
        bool Walking = false;
        bool Caster = false;       // fights at range: a caster or a siege minion
        uint8 Kind = 0;            // SRF_MINION_MELEE / CASTER / SIEGE / SUPER
        uint8 Slot = 0;            // its place in the wave: rank (melee first) and file
        uint32 AttackTimer = 0;    // casters: ms until the next bolt
        uint32 StartDelay = 0;     // ms before it leaves the barracks
        bool Denied = false;       // turned neutral under half health (the denying rule)
        bool Paid = false;         // its gold went to a last hit already
        Position LastPos;          // where it stood at the last tick that moved it
        uint32 StillMs = 0;        // how long it has stood there without fighting
        Position Pos;              // where it stood at the last tick: its death is paid out there
        ObjectGuid Killer;         // the champion that last-hit it, if one did
        ObjectGuid Target;         // the target the battleground last sent it after
        ObjectGuid Ignore;         // a target it could not get to, left alone for a while
        uint32 IgnoreMs = 0;
        float SpawnMinutes = 0.0f; // League's clock when it came out: its League numbers
        float HealthScale = 1.0f;  // Baron's empowerment when it came out
    };

    struct CampState
    {
        bool Up = false;
        uint32 Timer = 0;          // ms until it spawns, while it is not up
        // League's leash: the camp's patience (0..100), shared by all its monsters
        float Patience = 100.0f;
        uint32 PauseMs = 0;        // a hit inside the leash stops the drain this long
        std::unordered_map<ObjectGuid, uint32> LastHealth;   // to tell a hit from the last tick
        float SpawnMinutes = 0.0f; // League's clock when it came up: its League numbers
    };
    void UpdateLeash(uint8 camp, uint32 elapsed);

    struct TurretState
    {
        ObjectGuid Target;
        ObjectGuid Eye;
        uint8 WarmStacks = 0;          // League's heat: champion hits in a row (SRF_TURRET_HEAT)
        ObjectGuid LastChampion;       // the champion it last shot
        ObjectGuid Warned;             // the champion told it is the target (RIFTTURRET)
        uint32 SinceChampionShot = 0;  // ms since the last shot at a champion
        uint32 Cooldown = 0;           // ms before it may fire: its last bolt lands first
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

    void UpdateTurrets(uint32 elapsed);
    Unit* PickTurretTarget(Creature* turret, TeamId team) const;
    Unit* TurretCallForHelp(Creature* turret, TeamId team) const;
    // League's targeting: who a unit is attacking, and how much a minion wants a target.
    Unit* AttackOf(Unit* unit) const;
    bool IsAlly(Unit* unit, TeamId team) const;
    // A hunter's or warlock's pet: an enemy champion's pet is fought like a minion.
    Pet* EnemyPetOf(Player* player) const;
    bool IsChampionPet(Unit* unit) const;
    // League has no combat state to hold: Rift units let go of players they are
    // no longer fighting (see UpdateBrush's tick).
    void ReleaseIdleCombat();
    uint8 MinionPriority(Creature* minion, TeamId team, Unit* target) const;
    uint32 WaveInterval() const;
    void FireBolt(Unit* source, Unit* target, uint32 spellId, int32 damage);
    Creature* TurretEye(uint8 slot, Creature* turret);
    void Report(uint32 diff);
    uint32 DeathTimer(Player const* player) const;
    void UpdateRespawns();
    void UpdateCasterAttack(Creature* caster, Minion& minion, Unit* victim);

    void UpdateFountains();

    void SpawnCamp(uint8 camp);
    void UpdateJungle(uint32 elapsed);
    bool HandleJungleKill(Creature* creature, Player* killer);
    void ApplyBuff(Player* player, uint32 spellId, int32 bp0, int32 bp1, int32 bp2, int32 durationMs);
    void ApplyCampBuff(Player* player, uint32 spellId);
    void PassCampBuffs(Player* victim, Player* heir);

    // Progression (RiftProgression): experience and gold, and keeping the
    // units' levels with the players'.
    std::vector<Player*> TeamPlayers(TeamId team) const;
    std::vector<Player*> TeamPlayersNear(TeamId team, WorldObject const* where, float range) const;
    void AwardExperience(TeamId team, WorldObject const* where, float leagueXp);
    // League's experience rules (wiki "Experience (champion)"): a minion's to every
    // enemy champion within 1500 units of where it died - whoever killed it, and
    // the killer always - one champion taking all of it, two or more splitting
    // 130% of it; a champion kill's to the killer and the allies within 1600.
    void AwardMinionExperience(TeamId team, Position const& where, float leagueXp, ObjectGuid killer);
    void AwardKillExperience(TeamId team, Player* victim, Player* killer);
    void AwardGold(TeamId team, WorldObject const* where, uint32 leagueGold);
    void OnMinionDied(Minion const& minion, Creature const* corpse);
    uint8 AverageMatchLevel() const;
    uint8 UnitLevel() const;
    // League's numbers (RiftLeague): its clock, the levels the conversion reads,
    // and a unit's League stats put on it.
    float LeagueMinutes() const;
    float AverageLeagueLevel() const;
    float UnitLeagueLevel() const;
    float UnitScale() const;
    char const* StructureKind(uint8 slot) const;
    static char const* MonsterKind(uint32 entry);
    void ApplyLeagueStats(Creature* creature, char const* kind, float minutes, float healthScale, bool backdoor);
    float LeagueAttackDamage(char const* kind, float minutes) const;
    void UpdateBackdoor();
    uint8 _combatLevel = 10;       // the champions' average level, refreshed with the item tick
    float _unitScale = 0.0f;       // RiftLeague::Scale at the units' League level, likewise
    void UpdateProgression(uint32 diff);
    void SyncChampions(uint32 diff);

    std::array<StructureState, 2 * BG_SRF_STRUCTURES_PER_TEAM> _structures;
    std::array<TurretState, 2 * BG_SRF_STRUCTURES_PER_TEAM> _turrets;
    std::vector<Minion> _minions;
    std::array<std::array<std::vector<Position>, BG_SRF_LANE_COUNT>, PVP_TEAMS_COUNT> _lanePaths;
    std::array<CampState, BG_SRF_JUNGLE_CAMPS> _camps;
    // When each champion's Crest and Blessing end (game ms), for PassCampBuffs.
    std::unordered_map<ObjectGuid, std::array<uint32, 2>> _campBuffEnds;
    uint32 _jungleTimer;
    // What the match did, for the periodic log line (Report).
    uint32 _reportTimer;
    uint32 _boltsCast;
    uint32 _boltsRefused;
    uint32 _lastRefusal;
    uint32 _matchClock;            // ms since the gates opened (wave cadence)
    std::unordered_map<ObjectGuid, uint32> _respawnAt;   // game time (ms) a dead champion is back

    struct BrushState
    {
        uint8 Patch = 0;           // the brush patch it stands in, 0 = none
        uint32 RevealMs = 0;       // attacked from the brush: seen until this runs out
        bool Hidden = false;
        int8 AtBase = -1;          // in its fountain at the last tick (-1: not known yet)
    };
    std::unordered_map<ObjectGuid, BrushState> _brush;
    uint32 _itemSyncTimer = 0;

    // League's announcer and scoreboard: CENTURION_Rift draws the banners from
    // RIFTANN lines and the Tab scoreboard from RIFTSCORE lines.
    struct ChampionStats
    {
        uint32 Kills = 0;
        uint32 Deaths = 0;
        uint32 Assists = 0;
        uint32 Minions = 0;        // League's CS: minions and monsters last-hit
        uint32 Streak = 0;         // kills since its last death
        uint32 DeathStreak = 0;    // deaths since its last kill
        uint32 Multi = 0;          // kills in the running multikill window
        uint32 LastKillMs = 0;     // _matchClock at its last kill
    };
    std::unordered_map<ObjectGuid, ChampionStats> _stats;
    // victim -> enemy champion -> _matchClock of its last damage or ability on it
    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> _contributions;
    bool _firstBlood = false;
    uint32 _scoreTimer = 0;
    uint32 _mapTimer = 0;
    void Announce(std::string const& line);
    void AnnounceEvent(char const* kind, TeamId team, uint32 arg = 0);
    void CheckAce(TeamId deadTeam);
    void SendScores();
    void SendMap();
    // League's turret range rings: each turret's reach as a ring of ground heights,
    // sent to each champion every minute (a /reload loses them; RIFTRING lines,
    // CENTURION_Rift draws the dots).
    void SendRings(Player* player);
    std::unordered_map<ObjectGuid, uint32> _ringsSentAt;   // _matchClock of the last send
    bool SeenBy(TeamId team, Player* enemy);
    // League's gold for a champion kill, before first blood's extra: the
    // victim's bounty for its streak, less for a champion that keeps dying.
    uint32 KillBounty(uint32 victimStreak, uint32 victimDeathStreak) const;

    // League's positions. Each team's are handed out when its match starts - the
    // humans too, by what their class and spec fit, so the bots take the rest
    // (AssignRoles) - and a bot plays its own loosely: the support walks behind
    // its carry and leaves it the last hits, the jungler clears camps and ganks
    // (ComputeBotOrders).
    enum SrfRole : uint8 { SRF_ROLE_TOP, SRF_ROLE_JUNGLE, SRF_ROLE_MID, SRF_ROLE_CARRY, SRF_ROLE_SUPPORT, SRF_ROLE_COUNT };
    std::unordered_map<ObjectGuid, uint8> _roles;       // every champion's, humans' as guessed
    std::array<bool, PVP_TEAMS_COUNT> _rolesAssigned = { false, false };
    void AssignRoles(TeamId team);
    uint8 RoleOf(ObjectGuid guid) const;
    Player* TeamCarry(TeamId team) const;

    struct BotState
    {
        uint8 Lane = BG_SRF_LANE_NONE;
        uint8 Role = SRF_ROLE_MID;
        bool Assigned = false;
        bool Retreating = false;
        uint8 Camp = 0;            // the jungler's next camp
        BattlegroundBotOrders Orders;   // the last orders, reused for SRF_BOT_ORDERS_MS
        bool HasOrders = false;
        uint32 OrdersAt = 0;            // GameTime ms they were worked out
        uint32 ShopBar = 0;             // copper the next trip home to shop waits for (raised when a trip buys nothing)
        uint32 ShopSince = 0;           // GameTime ms it has stood at the fountain with the gold to shop
        uint32 ShopGold = 0;            // its money then
    };
    // Per team: the standing enemy turrets free to shoot one of its champions (no
    // minion of the team inside their reach), refreshed with the brush tick.
    std::array<std::vector<Position>, PVP_TEAMS_COUNT> _freeTurrets;
    void RefreshFreeTurrets();
    bool ComputeBotOrders(Player* bot, BotState& st, BattlegroundBotOrders& out);
    std::unordered_map<ObjectGuid, BotState> _bots;
    BotState& BotOf(Player* bot);
    bool UnderFreeEnemyTurret(TeamId team, Position const& pos, float margin) const;
    uint32 LaneIndexNear(TeamId team, uint8 lane, Position const& pos);
    uint32 _brushTimer = 0;
    void UpdateBrush(uint32 elapsed);
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
