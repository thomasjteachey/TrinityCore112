#include "BattlegroundSRF.h"

#include "BattlegroundMgr.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RiftProgression.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace
{
// The Rift's size. League units become yards at SRF_MAP_SCALE / 50, the same mapping
// the map, its terrain and every model were converted with (wow/import/rift/scripts/
// rift_scale.py): change one, change both. Everything below is League's own
// numbers - placements in League units, ranges and speeds as League has them - so
// the battleground follows the map whatever its size.
float constexpr SRF_MAP_SCALE = 1.5f;
float constexpr SRF_YARDS_PER_UNIT = SRF_MAP_SCALE / 50.0f;
float constexpr SRF_LEAGUE_CENTRE = 7400.0f;

constexpr float Yards(float units) { return units * SRF_YARDS_PER_UNIT; }

// League space is x east, y up, z north; WoW is +X north, +Y west, +Z up, the
// map centre on the world origin.
Position ToWorld(float x, float y, float z, float o = 0.0f)
{
    return Position((z - SRF_LEAGUE_CENTRE) * SRF_YARDS_PER_UNIT, -(x - SRF_LEAGUE_CENTRE) * SRF_YARDS_PER_UNIT,
        y * SRF_YARDS_PER_UNIT, o);
}

struct SrfStructure
{
    BG_SRF_StructureKind kind;
    uint8 lane;
    float x, y, z;   // League units
    float o;         // WoW facing, from the placement's forward axis
};

// League's own placements (GdsMapObject transforms in base.materials.bin, read by
// wow/import/rift/scripts/layout_extract.py), in slot order: per lane the outer,
// inner and inhibitor turret and the inhibitor, then the two nexus turrets and
// the nexus.
SrfStructure constexpr SRF_STRUCTURES[PVP_TEAMS_COUNT][BG_SRF_STRUCTURES_PER_TEAM] =
{
    { // Order (Alliance), the south-west base
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,    981.00f,   37.00f,  10441.00f, 6.1959f }, // Turret_T1_L_03
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,   1512.00f,   42.00f,   6699.00f, 6.2657f }, // Turret_T1_L_02
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,   1169.00f,   90.73f,   4287.00f, 0.0524f }, // Turret_T1_C_06
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_TOP,   1169.28f,   94.73f,   3573.22f, 1.5708f }, // Barracks_T1_L1
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   5846.00f,   44.00f,   6396.00f, 5.4629f }, // Turret_T1_C_05
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   5048.00f,   43.00f,   4812.00f, 5.5676f }, // Turret_T1_C_04
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   3651.00f,   91.00f,   3696.00f, 5.4978f }, // Turret_T1_C_03
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_MID,   3203.00f,   92.00f,   3208.00f, 0.7854f }, // Barracks_T1_C1
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,  10504.00f,   41.00f,   1029.00f, 4.7085f }, // Turret_T1_R_03
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,   6919.00f,   43.00f,   1483.00f, 4.6949f }, // Turret_T1_R_02
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,   4281.00f,   86.00f,   1253.00f, 4.6251f }, // Turret_T1_C_07
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_BOT,   3454.88f,   89.00f,   1241.76f, 0.0000f }, // Barracks_T1_R1
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE,   1748.00f,   88.00f,   2270.00f, 5.7596f }, // Turret_T1_C_01
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE,   2177.00f,   89.00f,   1807.00f, 5.2360f }, // Turret_T1_C_02
        { BG_SRF_KIND_NEXUS,     BG_SRF_LANE_NONE,   1551.00f,   86.00f,   1659.00f, 0.7854f }  // HQ_T1
    },
    { // Chaos (Horde), the north-east base
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,   4318.00f,   38.00f,  13875.00f, 2.0071f }, // Turret_T2_L_03
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,   7943.00f,   38.00f,  13411.00f, 1.6598f }, // Turret_T2_L_02
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,  10481.00f,   88.00f,  13650.00f, 1.6252f }, // Turret_T2_L_01
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_TOP,  11261.00f,   88.00f,  13659.96f, 4.7124f }, // Barracks_T2_L1
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   8955.00f,   38.00f,   8510.00f, 2.2965f }, // Turret_T2_C_05
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   9767.00f,   38.00f,  10113.00f, 2.3562f }, // Turret_T2_C_04
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,  11134.00f,   88.00f,  11207.00f, 2.3562f }, // Turret_T2_C_03
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_MID,  11603.07f,   89.00f,  11667.00f, 5.4978f }, // Barracks_T2_C1
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,  13866.00f,   38.00f,   4505.00f, 3.1174f }, // Turret_T2_R_03
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,  13327.00f,   38.00f,   8226.00f, 3.1416f }, // Turret_T2_R_02
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,  13624.00f,   88.00f,  10572.00f, 3.1416f }, // Turret_T2_R_01
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_BOT,  13598.14f,   89.00f,  11316.13f, 0.0000f }, // Barracks_T2_R1
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE,  13052.00f,   88.00f,  12612.00f, 2.6005f }, // Turret_T2_C_01
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE,  12611.00f,   88.00f,  13084.00f, 2.0769f }, // Turret_T2_C_02
        { BG_SRF_KIND_NEXUS,     BG_SRF_LANE_NONE,  13243.00f,   91.00f,  13235.00f, 0.0000f }  // HQ_T2
    }
};

// The jungle. Camps are League's, in BG_SRF_JUNGLE_CAMPS order, with League's
// first spawn (after the gates open) and respawn times.
enum SrfCampKind : uint8
{
    SRF_CAMP_BLUE,
    SRF_CAMP_RED,
    SRF_CAMP_WOLVES,
    SRF_CAMP_RAPTORS,
    SRF_CAMP_KRUGS,
    SRF_CAMP_GROMP,
    SRF_CAMP_SCUTTLE,
    SRF_CAMP_DRAGON,
    SRF_CAMP_BARON
};

struct SrfCamp
{
    SrfCampKind kind;
    uint32 firstSpawn;   // ms after the gates open
    uint32 respawn;      // ms after the camp is cleared
};

uint32 constexpr SRF_JUNGLE_START = 90 * IN_MILLISECONDS;
uint32 constexpr SRF_SMALL_CAMP_RESPAWN = 135 * IN_MILLISECONDS;
uint32 constexpr SRF_BUFF_CAMP_RESPAWN = 5 * MINUTE * IN_MILLISECONDS;

SrfCamp constexpr SRF_CAMPS[BG_SRF_JUNGLE_CAMPS] =
{
    // Order's side of the jungle (0-5), then Chaos's (6-11)
    { SRF_CAMP_BLUE,    SRF_JUNGLE_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_WOLVES,  SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RAPTORS, SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RED,     SRF_JUNGLE_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_KRUGS,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_GROMP,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_BLUE,    SRF_JUNGLE_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_WOLVES,  SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RAPTORS, SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RED,     SRF_JUNGLE_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_KRUGS,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_GROMP,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    // the river
    { SRF_CAMP_SCUTTLE, 210 * IN_MILLISECONDS, 150 * IN_MILLISECONDS },
    { SRF_CAMP_SCUTTLE, 210 * IN_MILLISECONDS, 150 * IN_MILLISECONDS },
    { SRF_CAMP_DRAGON,  5 * MINUTE * IN_MILLISECONDS, 5 * MINUTE * IN_MILLISECONDS },
    { SRF_CAMP_BARON,   20 * MINUTE * IN_MILLISECONDS, 6 * MINUTE * IN_MILLISECONDS }
};

struct SrfMonster
{
    uint8 camp;
    uint32 entry;
    float x, y, z;   // League units
    float o;         // WoW facing
    uint32 gold;     // League gold, to the killer
    float xp;        // League experience, shared like a minion's
};

// League's MapLocators (OrderBlueBuff1, OrderWolves1-3, OrderVultures1-6, ...,
// DragonEelCamp, BaronEelCamp, Dragon, Baron1), in BgCreatures slot order.
SrfMonster constexpr SRF_MONSTERS[BG_SRF_JUNGLE_MONSTERS] =
{
    {  0, BG_SRF_ENTRY_BLUE_SENTINEL,      3821.5f,  51.9f,  7901.1f, 5.4978f, 100, 120.0f },
    {  1, BG_SRF_ENTRY_GREATER_MURK_WOLF,  3780.6f,  45.0f,  6444.0f, 3.9270f,  55,  55.0f },
    {  1, BG_SRF_ENTRY_MURK_WOLF,          3980.6f,  45.0f,  6444.0f, 3.9270f,  16,  16.0f },
    {  1, BG_SRF_ENTRY_MURK_WOLF,          3730.6f,  45.0f,  6594.0f, 3.9270f,  16,  16.0f },
    {  2, BG_SRF_ENTRY_CRIMSON_RAPTOR,     6706.7f,  50.3f,  5521.0f, 1.5708f,  35,  30.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             6929.6f,  50.3f,  5647.9f, 3.1416f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             7060.5f,  50.3f,  5499.3f, 4.7124f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             6962.7f,  51.1f,  5354.4f, 1.8200f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             6852.2f,  48.1f,  5227.1f, 2.3562f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             7106.2f,  48.1f,  5266.1f, 3.9270f,   9,   9.0f },
    {  3, BG_SRF_ENTRY_RED_BRAMBLEBACK,    7765.2f,  50.4f,  4020.2f, 3.9270f, 100, 120.0f },
    {  4, BG_SRF_ENTRY_ANCIENT_KRUG,       8482.5f,  50.7f,  2705.9f, 3.1416f,  60,  60.0f },
    {  4, BG_SRF_ENTRY_KRUG,               8275.5f,  50.7f,  2688.9f, 3.1416f,  40,  40.0f },
    {  5, BG_SRF_ENTRY_GROMP,              2110.6f,  54.0f,  8451.0f, 1.1100f,  80,  90.0f },
    {  6, BG_SRF_ENTRY_BLUE_SENTINEL,     11031.7f,  51.7f,  6990.8f, 2.3562f, 100, 120.0f },
    {  7, BG_SRF_ENTRY_GREATER_MURK_WOLF, 11008.2f,  51.2f,  8387.4f, 0.7854f,  55,  55.0f },
    {  7, BG_SRF_ENTRY_MURK_WOLF,         11058.2f,  51.2f,  8217.4f, 0.7854f,  16,  16.0f },
    {  7, BG_SRF_ENTRY_MURK_WOLF,         10808.2f,  51.2f,  8387.4f, 0.7854f,  16,  16.0f },
    {  8, BG_SRF_ENTRY_CRIMSON_RAPTOR,     7987.0f,  48.8f,  9471.4f, 4.7124f,  35,  30.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7887.0f,  48.8f,  9312.4f, 0.0000f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7757.0f,  48.8f,  9451.4f, 1.5708f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7854.4f,  52.2f,  9610.5f, 4.9532f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7997.2f,  52.1f,  9772.1f, 5.4978f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7724.2f,  52.1f,  9724.1f, 0.7854f,   9,   9.0f },
    {  9, BG_SRF_ENTRY_RED_BRAMBLEBACK,    7101.9f,  51.3f, 10900.5f, 0.7854f, 100, 120.0f },
    { 10, BG_SRF_ENTRY_ANCIENT_KRUG,       6317.1f,  47.6f, 12146.5f, 0.0000f,  60,  60.0f },
    { 10, BG_SRF_ENTRY_KRUG,               6547.1f,  47.6f, 12156.5f, 0.0000f,  40,  40.0f },
    { 11, BG_SRF_ENTRY_GROMP,             12703.6f,  54.0f,  6444.0f, 4.2500f,  80,  90.0f },
    { 12, BG_SRF_ENTRY_RIFT_SCUTTLER,     10500.0f, -62.0f,  5170.0f, 5.4978f,  55, 100.0f },
    { 13, BG_SRF_ENTRY_RIFT_SCUTTLER,      4400.0f, -62.0f,  9600.0f, 2.3562f,  55, 100.0f },
    { 14, BG_SRF_ENTRY_DRAGON,             9866.1f, -69.8f,  4414.0f, 0.7854f,  25, 200.0f },
    { 15, BG_SRF_ENTRY_BARON_NASHOR,       5007.1f,  24.4f, 10471.4f, 3.9270f, 300, 600.0f }
};

// League's buff durations, and what the stand-in spells are set to.
int32 constexpr SRF_BUFF_DURATION         = 2 * MINUTE * IN_MILLISECONDS;   // Blue and Red
int32 constexpr SRF_BARON_BUFF_DURATION   = 3 * MINUTE * IN_MILLISECONDS;
float constexpr SRF_BLUE_MANA_PER_TICK    = 0.02f;   // of maximum mana
int32 constexpr SRF_RED_AP_PER_LEVEL      = 3;
int32 constexpr SRF_DRAGON_CRIT_PER_STACK = 2;       // spell and melee crit, percent
int32 constexpr SRF_DRAGON_AP_PER_STACK   = 30;
int32 constexpr SRF_BARON_STATS_PCT       = 15;
uint32 constexpr SRF_GOLD_BARON_TEAM      = 300;     // to every player of the team
float constexpr SRF_BARON_MINION_HEALTH   = 1.5f;
// Long enough that the core never brings a jungle corpse back on its own: the
// battleground respawns its camps itself.
uint32 constexpr SRF_JUNGLE_CORE_RESPAWN  = DAY;

uint8 constexpr SRF_SLOT_NEXUS_TURRET_1 = 12;
uint8 constexpr SRF_SLOT_NEXUS_TURRET_2 = 13;
uint8 constexpr SRF_SLOT_NEXUS          = 14;

struct SrfPoint
{
    float x, y, z;   // League units
};

// League's __NAV_<lane>02..17 lane markers, Order's end first. Chaos minions
// walk the same list backwards.
SrfPoint constexpr SRF_LANE_TOP[] =
{
    {   1508.0f,  133.0f,   3597.0f }, {   1382.0f,   92.0f,   4910.0f }, {   1170.0f,   16.0f,   6715.0f },
    {   1207.0f,   13.0f,   7787.0f }, {   1229.0f,    0.0f,   9072.0f }, {   1285.0f,   11.0f,  10423.0f },
    {   1508.0f,   10.0f,  11601.0f }, {   1968.0f,   10.0f,  12459.0f }, {   2720.0f,   10.0f,  13100.0f },
    {   3486.0f,   10.0f,  13473.0f }, {   4359.0f,   10.0f,  13597.0f }, {   5504.0f,   10.0f,  13608.0f },
    {   6668.0f,   10.0f,  13647.0f }, {   8113.0f,   10.0f,  13665.0f }, {   9889.0f,   91.0f,  13447.0f },
    {  11348.0f,  145.0f,  13344.0f }
};
SrfPoint constexpr SRF_LANE_MID[] =
{
    {   2952.0f,  146.0f,   3474.0f }, {   3856.0f,  107.0f,   4170.0f }, {   4874.0f,   43.0f,   5141.0f },
    {   5375.0f,   58.0f,   5589.0f }, {   5823.0f,   87.0f,   5928.0f }, {   6270.0f,   31.0f,   6366.0f },
    {   6764.0f,   41.0f,   6864.0f }, {   7408.0f,   51.0f,   7405.0f }, {   8079.0f,  147.0f,   8022.0f },
    {   8618.0f,  147.0f,   8647.0f }, {   9149.0f,  147.0f,   9105.0f }, {   9625.0f,  147.0f,   9543.0f },
    {  10193.0f,  147.0f,  10035.0f }, {  10822.0f,  147.0f,  10646.0f }, {  11808.0f,  147.0f,  11479.0f }
};
SrfPoint constexpr SRF_LANE_BOT[] =
{
    {   3471.0f,  114.0f,   1589.0f }, {   4844.0f,  112.0f,   1440.0f }, {   6925.0f,   -7.0f,   1163.0f },
    {   7975.0f,  -30.0f,   1230.0f }, {   8870.0f,    9.0f,   1212.0f }, {  10506.0f,   17.0f,   1301.0f },
    {  11432.0f,   17.0f,   1472.0f }, {  12294.0f,   -3.0f,   1913.0f }, {  13001.0f,   -3.0f,   2628.0f },
    {  13529.0f,   -7.0f,   3694.0f }, {  13751.0f,   -7.0f,   4790.0f }, {  13741.0f,   -7.0f,   5811.0f },
    {  13710.0f,  -13.0f,   7177.0f }, {  13696.0f,  -13.0f,   8608.0f }, {  13364.0f,   89.0f,  10040.0f },
    {  13253.0f,   95.0f,  11334.0f }
};

struct SrfLane
{
    SrfPoint const* points;
    uint8 count;
};
SrfLane constexpr SRF_LANES[BG_SRF_LANE_COUNT] =
{
    { SRF_LANE_TOP, uint8(std::size(SRF_LANE_TOP)) },
    { SRF_LANE_MID, uint8(std::size(SRF_LANE_MID)) },
    { SRF_LANE_BOT, uint8(std::size(SRF_LANE_BOT)) }
};

// Where each lane's minions leave their base (__P_<team>_Spawn_Barracks__L01 /
// C01 / R01).
SrfPoint constexpr SRF_BARRACKS[PVP_TEAMS_COUNT][BG_SRF_LANE_COUNT] =
{
    { {   1109.0f,  124.0f,   2091.0f }, {   2008.0f,  119.0f,   2079.0f }, {   2034.0f,  128.0f,   1171.0f } },
    { {  12800.0f,  128.0f,  13745.0f }, {  12776.0f,  110.0f,  12784.0f }, {  13719.0f,  119.0f,  12845.0f } }
};

// The fountains (__Spawn_T1 / T2), facing the middle. The same spots are
// WorldSafeLocs 52710/52711.
SrfPoint constexpr SRF_FOUNTAIN_AT[PVP_TEAMS_COUNT] = { { 394.0f, 171.0f, 461.0f }, { 14340.0f, 179.0f, 14391.0f } };
float constexpr SRF_FOUNTAIN_FACING[PVP_TEAMS_COUNT] = { 5.4978f, 2.3562f };

float constexpr SRF_TURRET_RANGE        = Yards(750.0f);
float constexpr SRF_MINION_AGGRO_RANGE  = Yards(400.0f);
// With no enemy unit near, a minion takes on an enemy structure in a turret's
// range of it: a wave walking down a lane stops at the first turret standing.
float constexpr SRF_MINION_STRUCTURE_RANGE = Yards(750.0f);
// Further than this from its target, a minion lets it go and takes the closest again.
float constexpr SRF_MINION_LEASH        = Yards(1100.0f);
uint32 constexpr SRF_REPORT_TICK        = 30 * IN_MILLISECONDS;
float constexpr SRF_FOUNTAIN_HEAL_RANGE = Yards(600.0f);
float constexpr SRF_FOUNTAIN_BURN_RANGE = Yards(700.0f);
// Turret shots as shares of the target's maximum health: minions die to a few
// shots, and a player who stays under a turret takes a little more every shot.
float constexpr SRF_TURRET_MINION_SHARE = 0.45f;
float constexpr SRF_TURRET_PLAYER_SHARE = 0.20f;
float constexpr SRF_TURRET_PLAYER_RAMP  = 0.10f;
float constexpr SRF_TURRET_PLAYER_MAX   = 0.60f;
// Caster minions: League's 550 range, a bolt every 1.6 s.
float constexpr SRF_CASTER_RANGE        = Yards(550.0f);
uint32 constexpr SRF_CASTER_ATTACK_TIME = 1600;
// League's rewards (Season 3), in League gold and experience. Experience goes
// to the enemy champions within 1600 units of a minion that dies, shared
// between them.
float constexpr SRF_XP_RANGE            = Yards(1600.0f);
float constexpr SRF_XP_MELEE_MINION     = 58.88f;
float constexpr SRF_XP_CASTER_MINION    = 29.44f;
uint32 constexpr SRF_GOLD_MELEE_MINION  = 20;
uint32 constexpr SRF_GOLD_CASTER_MINION = 15;
uint32 constexpr SRF_GOLD_KILL          = 300;
uint32 constexpr SRF_GOLD_STRUCTURE     = 150;     // to every player of the side that destroys it
uint32 constexpr SRF_GOLD_PASSIVE       = 10;      // every SRF_PASSIVE_GOLD_TICK: League's ~1.9 a second
uint32 constexpr SRF_PASSIVE_GOLD_TICK  = 5 * IN_MILLISECONDS;
float constexpr SRF_XP_KILL_BASE        = 140.0f;
float constexpr SRF_XP_KILL_PER_LEVEL   = 20.0f;   // per League level of the victim
// Units follow the players' level: a level-60 minion would miss nobody and be
// missed by everybody a level-1 match starts with. Minions also grow a little
// with every wave, League's scaling over the course of a match.
float constexpr SRF_MINION_GROWTH_PER_WAVE = 0.03f;
uint32 constexpr SRF_STRUCTURE_LEVEL_TICK  = 30 * IN_MILLISECONDS;
uint32 constexpr SRF_MAINTAIN_TICK         = 3 * IN_MILLISECONDS;
// The denying rule: under this share of its health a minion turns neutral.
float constexpr SRF_DENY_HEALTH_PCT     = 50.0f;
// Hostile to everyone (players, both sides' minions), friendly to none: its own
// side can last-hit it, and it keeps fighting the other - a neutral minion could
// neither attack nor be attacked by a creature, and stood there for good.
uint32 constexpr SRF_FACTION_DENIABLE   = 1620;
float constexpr SRF_FOUNTAIN_HEAL_SHARE = 0.10f;
float constexpr SRF_FOUNTAIN_BURN_SHARE = 0.30f;

// Lane paths: League's markers joined up in steps of SRF_PATH_STEP, each point
// stood on the ground, and walked as one smooth spline of up to SRF_SPLINE_POINTS
// at a time. The next stretch is laid down before the current one runs out, so a
// minion never stops in the middle of its lane.
float constexpr SRF_PATH_STEP           = 6.0f;
uint32 constexpr SRF_SPLINE_POINTS      = 40;
uint32 constexpr SRF_SPLINE_RELAUNCH    = 6;     // points before the end
uint32 constexpr SRF_PATH_LOOKAHEAD     = 25;
// A minion that stands still this long without landing a blow is stuck: on a
// target it cannot get to (it gives that one up for SRF_MINION_IGNORE_MS) or
// on its lane (it is put back on the lane a little further on).
uint32 constexpr SRF_MINION_STUCK_MS    = 4 * IN_MILLISECONDS;
uint32 constexpr SRF_MINION_IGNORE_MS   = 10 * IN_MILLISECONDS;
float constexpr SRF_MINION_STILL_DIST   = 0.5f;
uint32 constexpr SRF_UNSTICK_ADVANCE    = 2;     // path points past where it stands
uint32 constexpr SRF_LANE_SPLINE_ID     = 1;
// A wave walks as League's does, strung out rather than stacked: melee in front,
// casters behind, three files across the lane, each minion leaving the barracks
// a moment after the one before it.
float constexpr SRF_WAVE_FILE_SPACING   = 2.5f;     // yards between files, across the lane
float constexpr SRF_WAVE_RANK_SPACING   = 3.0f;     // yards between ranks, along it
uint32 constexpr SRF_WAVE_START_STAGGER = 600;      // ms between departures

// Its file's offset across the lane: -1, 0 or +1 files from the middle.
float FileOffset(uint8 slot) { return (float(slot % 3) - 1.0f) * SRF_WAVE_FILE_SPACING; }
float RankOffset(uint8 slot) { return float(slot / 3) * SRF_WAVE_RANK_SPACING; }

// The lane point shifted sideways into a minion's file, square to the lane there.
Position InFile(std::vector<Position> const& path, uint32 i, float offset)
{
    Position const& p = path[i];
    if (offset == 0.0f || path.size() < 2)
        return p;
    Position const& a = path[std::min<uint32>(i, uint32(path.size()) - 2)];
    Position const& b = path[std::min<uint32>(i, uint32(path.size()) - 2) + 1];
    float const angle = a.GetAbsoluteAngle(&b) + float(M_PI) / 2.0f;
    return Position(p.GetPositionX() + std::cos(angle) * offset, p.GetPositionY() + std::sin(angle) * offset, p.GetPositionZ());
}

TeamId SlotTeam(uint8 slot) { return slot < BG_SRF_STRUCTURES_PER_TEAM ? TEAM_ALLIANCE : TEAM_HORDE; }
uint8 SlotIndex(uint8 slot) { return slot % BG_SRF_STRUCTURES_PER_TEAM; }
uint8 SlotOf(TeamId team, uint8 index) { return uint8(team) * BG_SRF_STRUCTURES_PER_TEAM + index; }
SrfStructure const& StructureAt(uint8 slot) { return SRF_STRUCTURES[SlotTeam(slot)][SlotIndex(slot)]; }
Position StructurePos(uint8 slot) { SrfStructure const& s = StructureAt(slot); return ToWorld(s.x, s.y, s.z, s.o); }
Position FountainPos(TeamId team)
{
    SrfPoint const& f = SRF_FOUNTAIN_AT[team];
    return ToWorld(f.x, f.y, f.z, SRF_FOUNTAIN_FACING[team]);
}
uint32 TeamValue(TeamId team) { return team == TEAM_ALLIANCE ? ALLIANCE : HORDE; }
TeamId OtherTeam(TeamId team) { return team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE; }
char const* TeamName(TeamId team) { return team == TEAM_ALLIANCE ? "The Alliance" : "The Horde"; }

uint32 StructureEntry(uint8 slot)
{
    bool const order = SlotTeam(slot) == TEAM_ALLIANCE;
    switch (StructureAt(slot).kind)
    {
        case BG_SRF_KIND_INHIBITOR: return order ? BG_SRF_ENTRY_ORDER_INHIBITOR : BG_SRF_ENTRY_CHAOS_INHIBITOR;
        case BG_SRF_KIND_NEXUS:     return order ? BG_SRF_ENTRY_ORDER_NEXUS : BG_SRF_ENTRY_CHAOS_NEXUS;
        default:                    return order ? BG_SRF_ENTRY_ORDER_TURRET : BG_SRF_ENTRY_CHAOS_TURRET;
    }
}
}

void BattlegroundSRFScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra scoreboard columns
}

BattlegroundSRF::BattlegroundSRF()
{
    BgCreatures.resize(BG_SRF_CREATURE_MAX);
    _waveTimer = 0;
    _minionTimer = 0;
    _turretTimer = 0;
    _fountainTimer = 0;
    _structureTimer = 0;
    _passiveGoldTimer = 0;
    _maintainTimer = 0;
    _structureLevelTimer = 0;
    _wavesSpawned = 0;
    _wavesStarted = false;
    _dragonsSlain.fill(0);
    _baronTimer.fill(0);
    _jungleTimer = 0;
    _reportTimer = 0;
    _boltsCast = 0;
    _boltsRefused = 0;
    _lastRefusal = 0;
    _unstuckTargets = 0;
    _unstuckPlaced = 0;
}

void BattlegroundSRF::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundSRFScore(player->GetGUID(), scoreboardTeamMarker);
    }

    // Level 1, a fixed purse, spells and talents to earn back; a player coming
    // back to the match it left picks up where it was.
    RiftProgression::Enter(player);
}

void BattlegroundSRF::RemovePlayer(Player* player, ObjectGuid /*guid*/, uint32 /*team*/)
{
    // Gone without a Player (logged out) is OnLogin's to restore.
    if (player)
        RiftProgression::Leave(player);
}

void BattlegroundSRF::Reset()
{
    Battleground::Reset();

    _structures.fill(StructureState());
    _turrets.fill(TurretState());
    _minions.clear();
    for (auto& team : _lanePaths)
        for (std::vector<Position>& lane : team)
            lane.clear();
    _waveTimer = 0;
    _minionTimer = 0;
    _turretTimer = 0;
    _fountainTimer = 0;
    _structureTimer = 0;
    _passiveGoldTimer = 0;
    _maintainTimer = 0;
    _structureLevelTimer = 0;
    _wavesSpawned = 0;
    _wavesStarted = false;
    _camps.fill(CampState());
    _dragonsSlain.fill(0);
    _baronTimer.fill(0);
    _jungleTimer = 0;
    _reportTimer = 0;
    _boltsCast = 0;
    _boltsRefused = 0;
    _lastRefusal = 0;
    _unstuckTargets = 0;
    _unstuckPlaced = 0;
}

bool BattlegroundSRF::SetupBattleground()
{
    for (uint8 slot = 0; slot < 2 * BG_SRF_STRUCTURES_PER_TEAM; ++slot)
        if (!SpawnStructure(slot))
            return false;

    struct SpiritSpawn
    {
        uint32 type;
        uint32 safeLoc;
        TeamId team;
    };
    SpiritSpawn constexpr spirits[] =
    {
        { BG_SRF_CREATURE_SPIRIT_ORDER, BG_SRF_GY_ORDER, TEAM_ALLIANCE },
        { BG_SRF_CREATURE_SPIRIT_CHAOS, BG_SRF_GY_CHAOS, TEAM_HORDE }
    };
    for (SpiritSpawn const& spirit : spirits)
    {
        WorldSafeLocsEntry const* loc = sWorldSafeLocsStore.LookupEntry(spirit.safeLoc);
        if (!loc)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundSRF::SetupBattleground: WorldSafeLocs {} is missing.", spirit.safeLoc);
            return false;
        }
        if (!AddSpiritGuide(spirit.type, loc->Loc.X, loc->Loc.Y, loc->Loc.Z, FountainPos(spirit.team).GetOrientation(), spirit.team))
            return false;
    }

    RefreshVulnerability();
    return true;
}

bool BattlegroundSRF::SpawnStructure(uint8 slot)
{
    DelCreature(slot);
    if (!AddCreature(StructureEntry(slot), slot, StructurePos(slot), SlotTeam(slot)))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundSRF: failed to spawn structure slot {} (entry {}).", slot, StructureEntry(slot));
        return false;
    }
    _structures[slot] = StructureState();
    _turrets[slot] = TurretState();
    ApplyUnitLevel(GetBGCreature(slot, false), AverageMatchLevel(), 1.0f);
    return true;
}

void BattlegroundSRF::StartingEventOpenDoors()
{
    _wavesStarted = true;
    _waveTimer = BG_SRF_WAVE_INTERVAL - BG_SRF_FIRST_WAVE_TIME;
    for (uint8 camp = 0; camp < BG_SRF_JUNGLE_CAMPS; ++camp)
    {
        _camps[camp].Up = false;
        _camps[camp].Timer = SRF_CAMPS[camp].firstSpawn;
    }
}

void BattlegroundSRF::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    _structureTimer += diff;
    if (_structureTimer >= BG_SRF_STRUCTURE_CHECK_TICK)
    {
        uint32 const elapsed = _structureTimer;
        _structureTimer = 0;
        for (uint8 slot = 0; slot < _structures.size(); ++slot)
        {
            StructureState& st = _structures[slot];
            if (!st.Destroyed || !st.RespawnTimer)
                continue;
            if (st.RespawnTimer > elapsed)
            {
                st.RespawnTimer -= elapsed;
                continue;
            }
            // A fallen inhibitor stands back up.
            SpawnStructure(slot);
            PSendMessageToAll(BG_SRF_STRING_INHIBITOR_RESPAWNED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(SlotTeam(slot)));
            RefreshVulnerability();
        }
        UpdateStructures();
        if (GetStatus() != STATUS_IN_PROGRESS)
            return;
    }

    if (_wavesStarted)
    {
        _waveTimer += diff;
        if (_waveTimer >= BG_SRF_WAVE_INTERVAL)
        {
            _waveTimer -= BG_SRF_WAVE_INTERVAL;
            SpawnWave();
        }
    }

    _minionTimer += diff;
    if (_minionTimer >= BG_SRF_MINION_TICK)
    {
        _minionTimer = 0;
        UpdateMinions();
    }

    _jungleTimer += diff;
    if (_jungleTimer >= BG_SRF_MINION_TICK)
    {
        if (_wavesStarted)
            UpdateJungle(_jungleTimer);
        _jungleTimer = 0;
    }

    _turretTimer += diff;
    if (_turretTimer >= BG_SRF_TURRET_TICK)
    {
        _turretTimer = 0;
        UpdateTurrets();
    }

    _fountainTimer += diff;
    if (_fountainTimer >= BG_SRF_FOUNTAIN_TICK)
    {
        _fountainTimer = 0;
        UpdateFountains();
    }

    UpdateProgression(diff);
    Report(diff);
}

// ---------------------------------------------------------------- structures

// Structures die to players and minions alike, and only a player kill reaches
// HandleKillUnit, so the structures are watched instead.
void BattlegroundSRF::UpdateStructures()
{
    for (uint8 slot = 0; slot < _structures.size(); ++slot)
    {
        if (_structures[slot].Destroyed)
            continue;
        Creature* creature = GetBGCreature(slot, false);
        if (!creature || !creature->IsAlive())
        {
            OnStructureDestroyed(slot);
            if (GetStatus() != STATUS_IN_PROGRESS)
                return;
        }
    }
}

void BattlegroundSRF::OnStructureDestroyed(uint8 slot)
{
    StructureState& st = _structures[slot];
    st.Destroyed = true;
    st.Vulnerable = false;
    if (Map* map = FindBgMap())
        if (Creature* eye = map->GetCreature(_turrets[slot].Eye))
            eye->DespawnOrUnsummon();
    _turrets[slot] = TurretState();

    TeamId const owner = SlotTeam(slot);
    TeamId const destroyer = OtherTeam(owner);
    switch (StructureAt(slot).kind)
    {
        case BG_SRF_KIND_NEXUS:
            EndBattleground(TeamValue(destroyer));
            return;
        case BG_SRF_KIND_INHIBITOR:
            st.RespawnTimer = BG_SRF_INHIBITOR_RESPAWN;
            PSendMessageToAll(BG_SRF_STRING_INHIBITOR_DESTROYED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(destroyer));
            RewardHonorToTeam(BG_SRF_HONOR_INHIBITOR, TeamValue(destroyer));
            break;
        default:
            PSendMessageToAll(BG_SRF_STRING_TURRET_DESTROYED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(destroyer));
            RewardHonorToTeam(BG_SRF_HONOR_TURRET, TeamValue(destroyer));
            break;
    }
    for (Player* player : TeamPlayers(destroyer))
        RiftProgression::AddGold(player, SRF_GOLD_STRUCTURE, true);

    RefreshVulnerability();
}

// League's order: in each lane the outer, inner and inhibitor turret and the
// inhibitor fall in turn; the nexus turrets open while any inhibitor of their
// team is down, the nexus once both nexus turrets are gone.
void BattlegroundSRF::RefreshVulnerability()
{
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        bool anyInhibitorDown = false;
        for (uint8 lane = 0; lane < BG_SRF_LANE_COUNT; ++lane)
        {
            uint8 const first = SlotOf(team, lane * 4);
            for (uint8 tier = 0; tier < 4; ++tier)
                SetVulnerable(first + tier, tier == 0 || _structures[first + tier - 1].Destroyed);
            anyInhibitorDown |= _structures[first + 3].Destroyed;
        }
        SetVulnerable(SlotOf(team, SRF_SLOT_NEXUS_TURRET_1), anyInhibitorDown);
        SetVulnerable(SlotOf(team, SRF_SLOT_NEXUS_TURRET_2), anyInhibitorDown);
        SetVulnerable(SlotOf(team, SRF_SLOT_NEXUS),
            _structures[SlotOf(team, SRF_SLOT_NEXUS_TURRET_1)].Destroyed && _structures[SlotOf(team, SRF_SLOT_NEXUS_TURRET_2)].Destroyed);
    }
}

void BattlegroundSRF::SetVulnerable(uint8 slot, bool vulnerable)
{
    StructureState& st = _structures[slot];
    if (st.Destroyed)
        return;
    st.Vulnerable = vulnerable;

    Creature* creature = GetBGCreature(slot, false);
    if (!creature)
        return;
    // Locked by NON_ATTACKABLE alone: nothing can target it, and it can still
    // target others. Immunity flags would also stop a locked turret firing -
    // a unit immune to PCs or NPCs is no valid attacker of them either.
    creature->SetImmuneToAll(false);
    if (vulnerable)
        creature->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
    else
        creature->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
}

// ------------------------------------------------------------------- minions

void BattlegroundSRF::SpawnWave()
{
    bool spawned = false;
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        Creature* nexus = GetBGCreature(SlotOf(team, SRF_SLOT_NEXUS), false);
        if (!nexus)
            continue;

        size_t const alive = std::count_if(_minions.begin(), _minions.end(), [team](Minion const& m) { return m.Team == team; });
        if (alive >= BG_SRF_MAX_MINIONS_PER_TEAM)
            continue;

        uint32 const melee = team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_MELEE : BG_SRF_ENTRY_CHAOS_MELEE;
        uint32 const caster = team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_CASTER : BG_SRF_ENTRY_CHAOS_CASTER;
        for (uint8 lane = 0; lane < BG_SRF_LANE_COUNT; ++lane)
        {
            std::vector<Position> const& path = LanePath(team, lane);
            if (path.size() < 2)
                continue;
            Position const gate = path[0];
            float const facing = gate.GetAbsoluteAngle(&path[1]);
            for (uint8 i = 0; i < BG_SRF_MELEE_PER_WAVE + BG_SRF_CASTERS_PER_WAVE; ++i)
            {
                // In its file across the lane and its rank behind the gate: melee
                // (slots 0-2) in front, casters (3-5) behind.
                float const back = facing + float(M_PI);
                Position const filed = InFile(path, 0, FileOffset(i));
                Position const pos(filed.GetPositionX() + std::cos(back) * RankOffset(i),
                    filed.GetPositionY() + std::sin(back) * RankOffset(i), gate.GetPositionZ() + 1.0f, facing);
                TempSummon* minion = nexus->SummonCreature(i < BG_SRF_MELEE_PER_WAVE ? melee : caster, pos,
                    TEMPSUMMON_CORPSE_TIMED_DESPAWN, 3s);
                if (!minion)
                    continue;
                // The battleground picks every minion's target (FindMinionTarget):
                // left to aggro on sight, a wave would turn on its own deniable
                // minions, which are hostile to everyone.
                minion->SetReactState(REACT_DEFENSIVE);
                // Baron's empowerment: the team's minions come out tougher while it lasts.
                float const empowered = _baronTimer[team] ? SRF_BARON_MINION_HEALTH : 1.0f;
                ApplyUnitLevel(minion, AverageMatchLevel(), (1.0f + SRF_MINION_GROWTH_PER_WAVE * float(_wavesSpawned)) * empowered);
                Minion m;
                m.Guid = minion->GetGUID();
                m.Team = team;
                m.Lane = lane;
                m.Caster = i >= BG_SRF_MELEE_PER_WAVE;
                m.Slot = i;
                m.StartDelay = uint32(i) * SRF_WAVE_START_STAGGER;
                _minions.push_back(m);
                spawned = true;
            }
        }
    }

    if (spawned)
    {
        ++_wavesSpawned;
        PSendMessageToAll(BG_SRF_STRING_MINIONS_SPAWNED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
    }
}

Unit* BattlegroundSRF::FindMinionTarget(Creature* minion, TeamId team, float range, ObjectGuid ignore) const
{
    // League's rule: the closest enemy, whatever it is - minion, champion or
    // structure - measured edge to edge. A structure is in reach from further
    // off (a wave walking up to a turret stops at it).
    Map* map = minion->GetMap();
    Unit* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    auto consider = [&](Unit* unit, float reach)
    {
        if (!unit || !unit->IsAlive() || unit->GetGUID() == ignore || !minion->IsValidAttackTarget(unit))
            return;
        float const dist = minion->GetExactDist2d(unit) - unit->GetCombatReach();
        if (dist <= reach && dist < bestDist)
        {
            best = unit;
            bestDist = dist;
        }
    };

    for (Minion const& other : _minions)
        if (other.Team != team)
            consider(map->GetCreature(other.Guid), range);

    TeamId const enemy = OtherTeam(team);
    for (auto const& [guid, bgPlayer] : GetPlayers())
        if (bgPlayer.Team == TeamValue(enemy))
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                if (player->GetMap() == map)
                    consider(player, range);

    float const structureReach = std::max(range, SRF_MINION_STRUCTURE_RANGE);
    for (uint8 i = 0; i < BG_SRF_STRUCTURES_PER_TEAM; ++i)
    {
        uint8 const slot = SlotOf(enemy, i);
        if (_structures[slot].Vulnerable && !_structures[slot].Destroyed)
            consider(map->GetCreature(BgCreatures[slot]), structureReach);
    }
    return best;
}

// League's markers, joined up and stood on the ground: barracks, the lane in the
// team's walking direction, then the enemy nexus. Built once per match, on first
// use, because it needs the map's heights.
std::vector<Position> const& BattlegroundSRF::LanePath(TeamId team, uint8 lane)
{
    std::vector<Position>& path = _lanePaths[team][lane];
    if (!path.empty())
        return path;
    Map* map = FindBgMap();
    if (!map)
        return path;

    std::vector<Position> nodes;
    SrfPoint const& gate = SRF_BARRACKS[team][lane];
    nodes.push_back(ToWorld(gate.x, gate.y, gate.z));
    SrfLane const& l = SRF_LANES[lane];
    for (uint8 i = 0; i < l.count; ++i)
    {
        SrfPoint const& p = l.points[team == TEAM_ALLIANCE ? i : l.count - 1 - i];
        nodes.push_back(ToWorld(p.x, p.y, p.z));
    }
    nodes.push_back(StructurePos(SlotOf(OtherTeam(team), SRF_SLOT_NEXUS)));

    path.push_back(nodes[0]);
    for (size_t n = 1; n < nodes.size(); ++n)
    {
        Position const& a = nodes[n - 1];
        Position const& b = nodes[n];
        uint32 const steps = std::max<uint32>(1, uint32(std::ceil(a.GetExactDist2d(&b) / SRF_PATH_STEP)));
        for (uint32 k = 1; k <= steps; ++k)
        {
            float const f = float(k) / float(steps);
            float const x = a.GetPositionX() + (b.GetPositionX() - a.GetPositionX()) * f;
            float const y = a.GetPositionY() + (b.GetPositionY() - a.GetPositionY()) * f;
            float z = a.GetPositionZ() + (b.GetPositionZ() - a.GetPositionZ()) * f;
            float const ground = map->GetHeight(x, y, z + 10.0f, true);
            if (ground > INVALID_HEIGHT)
                z = ground;
            path.emplace_back(x, y, z);
        }
    }
    return path;
}

void BattlegroundSRF::UpdateMinions()
{
    Map* map = FindBgMap();
    if (!map)
        return;

    for (auto it = _minions.begin(); it != _minions.end();)
    {
        Creature* creature = map->GetCreature(it->Guid);
        if (!creature || !creature->IsAlive())
        {
            OnMinionDied(*it, creature);
            it = _minions.erase(it);
            continue;
        }

        Minion& m = *it;
        ++it;

        // The denying rule: a minion close to death is fair game for its own side.
        if (!m.Denied && GetCustomRules().RiftDenying && creature->GetHealthPct() < SRF_DENY_HEALTH_PCT)
        {
            m.Denied = true;
            creature->SetFaction(SRF_FACTION_DENIABLE);
        }

        if (m.IgnoreMs)
        {
            m.IgnoreMs = m.IgnoreMs > BG_SRF_MINION_TICK ? m.IgnoreMs - BG_SRF_MINION_TICK : 0;
            if (!m.IgnoreMs)
                m.Ignore.Clear();
        }

        // Standing still is only right for a minion that is landing blows, or
        // one still waiting at the barracks. Anything else for SRF_MINION_STUCK_MS
        // is a minion stuck - and the core's own answer, evading after a
        // chase with no path, only had the battleground hand it the same target.
        if (creature->GetExactDist2d(&m.LastPos) > SRF_MINION_STILL_DIST)
        {
            m.LastPos = creature->GetPosition();
            m.StillMs = 0;
        }
        else if (!m.StartDelay)
            m.StillMs += BG_SRF_MINION_TICK;
        if (Unit* victim = creature->GetVictim())
            if (m.Caster ? creature->GetExactDist2d(victim) <= SRF_CASTER_RANGE : creature->IsWithinMeleeRange(victim))
                m.StillMs = 0;
        if (m.StillMs >= SRF_MINION_STUCK_MS && UnstickMinion(creature, m))
            continue;

        // A minion never walks back after a fight: its home is wherever it is
        // standing, so leaving combat costs nothing and it carries on down the lane.
        // A minion keeps its target until it dies or gets away - no threat,
        // nothing that hits it pulls it off.
        if (Unit* victim = creature->GetVictim())
        {
            if (creature->GetExactDist2d(victim) - victim->GetCombatReach() <= SRF_MINION_LEASH)
            {
                m.Walking = false;
                if (m.Caster)
                    UpdateCasterAttack(creature, m, victim);
                continue;
            }
            creature->AI()->SetGUID(ObjectGuid::Empty, 0);
        }
        if (!creature->IsInEvadeMode())
            creature->SetHomePosition(creature->GetPosition());

        if (Unit* target = FindMinionTarget(creature, m.Team, SRF_MINION_AGGRO_RANGE, m.Ignore))
        {
            creature->AI()->SetGUID(target->GetGUID(), m.Caster ? 1 : 0);
            m.Target = target->GetGUID();
            m.Walking = false;
            continue;
        }
        m.Target.Clear();

        // Still waiting its turn to leave the barracks.
        if (m.StartDelay)
        {
            m.StartDelay = m.StartDelay > BG_SRF_MINION_TICK ? m.StartDelay - BG_SRF_MINION_TICK : 0;
            continue;
        }

        std::vector<Position> const& path = LanePath(m.Team, m.Lane);
        if (path.size() < 2)
            continue;

        // How far along the lane it is: the nearest point a little ahead of where
        // it last was, so a minion pulled sideways by a fight rejoins further on
        // rather than walking back.
        uint32 best = m.PathIndex;
        float bestDist = creature->GetExactDist2dSq(&path[best]);
        uint32 const last = std::min<uint32>(uint32(path.size()), m.PathIndex + SRF_PATH_LOOKAHEAD);
        for (uint32 i = m.PathIndex + 1; i < last; ++i)
        {
            float const d = creature->GetExactDist2dSq(&path[i]);
            if (d < bestDist)
            {
                best = i;
                bestDist = d;
            }
        }
        m.PathIndex = best;
        if (m.PathIndex + 1 >= path.size())
            continue;   // at the enemy nexus

        bool const onSpline = m.Walking && !creature->movespline->Finalized()
            && creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE;
        if (onSpline && m.PathIndex + SRF_SPLINE_RELAUNCH < m.SplineEnd)
            continue;

        std::vector<Position> points;
        points.push_back(creature->GetPosition());
        uint32 const end = std::min<uint32>(uint32(path.size()), m.PathIndex + 1 + SRF_SPLINE_POINTS);
        for (uint32 i = m.PathIndex + 1; i < end; ++i)
            points.push_back(InFile(path, i, FileOffset(m.Slot)));
        creature->GetMotionMaster()->MoveSmoothPath(SRF_LANE_SPLINE_ID, points.data(), points.size(), false);
        m.Walking = true;
        m.SplineEnd = end - 1;
    }
}

// A minion stuck on a target gives it up for a while and goes on with whatever
// else is near, or down its lane; one stuck on the lane itself is put back on it
// a couple of points further on - a declared way out, not a guess at the ground.
bool BattlegroundSRF::UnstickMinion(Creature* creature, Minion& m)
{
    m.StillMs = 0;
    m.LastPos = creature->GetPosition();

    Unit* victim = creature->GetVictim();
    ObjectGuid const target = victim ? victim->GetGUID() : m.Target;
    if (!target.IsEmpty())
    {
        if (_unstuckTargets + _unstuckPlaced < 3)
            TC_LOG_INFO("bg.battleground", "Summoner's Rift {}: minion {} (lane {}, point {}) at {:.1f} {:.1f} {:.1f} cannot get to {} - leaving it alone.",
                GetInstanceID(), creature->GetEntry(), m.Lane, m.PathIndex, creature->GetPositionX(), creature->GetPositionY(),
                creature->GetPositionZ(), target.ToString());
        m.Ignore = target;
        m.IgnoreMs = SRF_MINION_IGNORE_MS;
        m.Target.Clear();
        m.Walking = false;
        creature->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_NO_PATH);
        ++_unstuckTargets;
        return true;
    }

    std::vector<Position> const& path = LanePath(m.Team, m.Lane);
    if (path.size() < 2 || m.PathIndex + 1 >= path.size())
        return false;   // at the enemy nexus: nowhere further to go
    uint32 const to = std::min<uint32>(uint32(path.size()) - 1, m.PathIndex + SRF_UNSTICK_ADVANCE);
    Position const at = InFile(path, to, FileOffset(m.Slot));
    float const facing = path[std::min<uint32>(uint32(path.size()) - 1, to + 1)].GetAbsoluteAngle(&path[to]) + float(M_PI);
    if (_unstuckTargets + _unstuckPlaced < 3)
        TC_LOG_INFO("bg.battleground", "Summoner's Rift {}: minion {} (lane {}, point {}) stuck at {:.1f} {:.1f} {:.1f} - back on the lane at point {}.",
            GetInstanceID(), creature->GetEntry(), m.Lane, m.PathIndex, creature->GetPositionX(), creature->GetPositionY(),
            creature->GetPositionZ(), to);
    creature->GetMotionMaster()->Clear();
    creature->NearTeleportTo(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ() + 0.5f, facing);
    m.PathIndex = to;
    m.Walking = false;
    m.LastPos = at;
    ++_unstuckPlaced;
    return true;
}

void BattlegroundSRF::DespawnMinions()
{
    if (Map* map = FindBgMap())
        for (Minion const& m : _minions)
            if (Creature* creature = map->GetCreature(m.Guid))
                creature->DespawnOrUnsummon();
    _minions.clear();
}

// --------------------------------------------------------------- progression

std::vector<Player*> BattlegroundSRF::TeamPlayers(TeamId team) const
{
    std::vector<Player*> out;
    for (auto const& [guid, bgPlayer] : GetPlayers())
        if (bgPlayer.Team == TeamValue(team))
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                if (player->GetMapId() == GetMapId())
                    out.push_back(player);
    return out;
}

std::vector<Player*> BattlegroundSRF::TeamPlayersNear(TeamId team, WorldObject const* where, float range) const
{
    std::vector<Player*> out;
    for (Player* player : TeamPlayers(team))
        if (player->IsAlive() && player->GetExactDist2d(where) <= range)
            out.push_back(player);
    return out;
}

// League shares experience between the champions near what died; with the
// shared-experience rule the whole team earns all of it, as one.
void BattlegroundSRF::AwardExperience(TeamId team, WorldObject const* where, float leagueXp)
{
    if (GetCustomRules().RiftSharedExperience)
    {
        for (Player* player : TeamPlayers(team))
            RiftProgression::AddExperience(player, leagueXp);
        return;
    }
    std::vector<Player*> const near = TeamPlayersNear(team, where, SRF_XP_RANGE);
    for (Player* player : near)
        RiftProgression::AddExperience(player, leagueXp / float(near.size()));
}

void BattlegroundSRF::AwardGold(TeamId team, WorldObject const* where, uint32 leagueGold)
{
    std::vector<Player*> const near = TeamPlayersNear(team, where, SRF_XP_RANGE);
    if (near.empty())
        return;
    uint32 const share = std::max<uint32>(1, leagueGold / uint32(near.size()));
    for (Player* player : near)
        RiftProgression::AddGold(player, share, true);
}

void BattlegroundSRF::OnMinionDied(Minion const& minion, Creature const* corpse)
{
    if (!corpse || GetStatus() != STATUS_IN_PROGRESS)
        return;
    TeamId const enemy = OtherTeam(minion.Team);
    // A denied minion is worth nothing to anybody.
    if (minion.Denied && minion.Paid)
        return;
    AwardExperience(enemy, corpse, minion.Caster ? SRF_XP_CASTER_MINION : SRF_XP_MELEE_MINION);
    if (!GetCustomRules().RiftLastHitGold && !minion.Paid)
        AwardGold(enemy, corpse, minion.Caster ? SRF_GOLD_CASTER_MINION : SRF_GOLD_MELEE_MINION);
}

void BattlegroundSRF::HandleKillUnit(Creature* creature, Player* killer)
{
    if (!creature || !killer || GetStatus() != STATUS_IN_PROGRESS)
        return;
    if (HandleJungleKill(creature, killer))
        return;
    auto const it = std::find_if(_minions.begin(), _minions.end(), [creature](Minion const& m) { return m.Guid == creature->GetGUID(); });
    if (it == _minions.end())
        return;
    // Marked paid either way: the last hit's gold is the killer's, or - on its
    // own side - a deny, worth nothing at all.
    it->Paid = true;
    if (GetTeamIndexByTeamId(GetPlayerTeam(killer->GetGUID())) == it->Team)
    {
        it->Denied = true;
        return;
    }
    if (GetCustomRules().RiftLastHitGold)
        RiftProgression::AddGold(killer, it->Caster ? SRF_GOLD_CASTER_MINION : SRF_GOLD_MELEE_MINION, true);
}

void BattlegroundSRF::HandleKillPlayer(Player* victim, Player* killer)
{
    Battleground::HandleKillPlayer(victim, killer);
    if (!victim || !killer || killer == victim || GetStatus() != STATUS_IN_PROGRESS)
        return;
    TeamId const killerTeam = GetTeamIndexByTeamId(GetPlayerTeam(killer->GetGUID()));
    if (killerTeam == GetTeamIndexByTeamId(GetPlayerTeam(victim->GetGUID())))
        return;
    RiftProgression::AddGold(killer, SRF_GOLD_KILL, true);
    AwardExperience(killerTeam, victim, SRF_XP_KILL_BASE + SRF_XP_KILL_PER_LEVEL * float(RiftProgression::GetLeagueLevel(victim)));
}

uint8 BattlegroundSRF::AverageMatchLevel() const
{
    uint32 total = 0, count = 0;
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        for (Player* player : TeamPlayers(team))
        {
            total += player->GetLevel();
            ++count;
        }
    return count ? uint8(std::clamp<uint32>((total + count / 2) / count, 1, DEFAULT_MAX_LEVEL)) : 1;
}

// A unit takes the players' level (and so their hit and miss odds) and keeps
// the share of its health it had.
void BattlegroundSRF::ApplyUnitLevel(Creature* creature, uint8 level, float healthScale)
{
    if (!creature || !creature->IsAlive())
        return;
    float const pct = creature->GetHealthPct();
    creature->SetLevel(level);
    creature->UpdateLevelDependantStats();
    if (healthScale != 1.0f)
        creature->SetMaxHealth(uint32(float(creature->GetMaxHealth()) * healthScale));
    creature->SetHealth(std::max<uint32>(1, uint32(float(creature->GetMaxHealth()) * pct / 100.0f)));
}

void BattlegroundSRF::UpdateProgression(uint32 diff)
{
    _passiveGoldTimer += diff;
    if (_passiveGoldTimer >= SRF_PASSIVE_GOLD_TICK)
    {
        _passiveGoldTimer -= SRF_PASSIVE_GOLD_TICK;
        if (_wavesSpawned)
            for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
                for (Player* player : TeamPlayers(team))
                    RiftProgression::AddGold(player, SRF_GOLD_PASSIVE);
    }

    _maintainTimer += diff;
    if (_maintainTimer >= SRF_MAINTAIN_TICK)
    {
        _maintainTimer = 0;
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            for (Player* player : TeamPlayers(team))
                RiftProgression::Maintain(player);
        MaintainObjectiveBuffs();
    }

    _structureLevelTimer += diff;
    if (_structureLevelTimer >= SRF_STRUCTURE_LEVEL_TICK)
    {
        _structureLevelTimer = 0;
        uint8 const level = AverageMatchLevel();
        for (uint8 slot = 0; slot < _structures.size(); ++slot)
            if (!_structures[slot].Destroyed)
                if (Creature* creature = GetBGCreature(slot, false))
                    if (creature->GetLevel() != level)
                        ApplyUnitLevel(creature, level, 1.0f);
    }
}

// ------------------------------------------------------------------- turrets

// League's rule, simplified: minions first, then players, holding a target while
// it stays in range.
Unit* BattlegroundSRF::PickTurretTarget(Creature* turret, TeamId team) const
{
    Map* map = turret->GetMap();
    Unit* best = nullptr;
    float bestDist = SRF_TURRET_RANGE;
    auto consider = [&](Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !turret->IsValidAttackTarget(unit))
            return;
        float const dist = turret->GetExactDist2d(unit);
        if (dist <= bestDist)
        {
            best = unit;
            bestDist = dist;
        }
    };

    for (Minion const& m : _minions)
        if (m.Team != team)
            consider(map->GetCreature(m.Guid));
    if (best)
        return best;

    for (auto const& [guid, bgPlayer] : GetPlayers())
        if (bgPlayer.Team == TeamValue(OtherTeam(team)))
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                if (player->GetMap() == map)
                    consider(player);
    return best;
}

void BattlegroundSRF::UpdateTurrets()
{
    Map* map = FindBgMap();
    if (!map)
        return;

    for (uint8 slot = 0; slot < _structures.size(); ++slot)
    {
        if (_structures[slot].Destroyed || StructureAt(slot).kind != BG_SRF_KIND_TURRET)
            continue;
        Creature* turret = GetBGCreature(slot, false);
        if (!turret || !turret->IsAlive())
            continue;

        TeamId const team = SlotTeam(slot);
        TurretState& ts = _turrets[slot];
        Unit* target = !ts.Target.IsEmpty() ? ObjectAccessor::GetUnit(*turret, ts.Target) : nullptr;
        if (!target || !target->IsAlive() || target->GetMap() != map
            || turret->GetExactDist2d(target) > SRF_TURRET_RANGE || !turret->IsValidAttackTarget(target))
        {
            target = PickTurretTarget(turret, team);
            ts.Target = target ? target->GetGUID() : ObjectGuid::Empty;
            ts.ConsecutiveHits = 0;
        }
        if (!target)
            continue;

        float share = SRF_TURRET_MINION_SHARE;
        if (target->GetTypeId() == TYPEID_PLAYER)
        {
            share = std::min(SRF_TURRET_PLAYER_SHARE + SRF_TURRET_PLAYER_RAMP * ts.ConsecutiveHits, SRF_TURRET_PLAYER_MAX);
            ++ts.ConsecutiveHits;
        }
        Creature* eye = TurretEye(slot, turret);
        FireBolt(eye ? static_cast<Unit*>(eye) : turret, target, team == TEAM_ALLIANCE ? BG_SRF_SPELL_ORDER_TURRET_SHOT : BG_SRF_SPELL_CHAOS_TURRET_SHOT,
            std::max<int32>(1, int32(target->GetMaxHealth() * share)));
    }
}

void BattlegroundSRF::FireBolt(Unit* source, Unit* target, uint32 spellId, int32 damage)
{
    SpellCastResult const result = source->CastSpell(target, spellId, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(damage));
    if (result == SPELL_CAST_OK)
        ++_boltsCast;
    else
    {
        ++_boltsRefused;
        _lastRefusal = uint32(result);
    }
}

// The turret's eye, summoned on first use: just over the top of the turret,
// with the turret's faction and level, unattackable and unselectable.
Creature* BattlegroundSRF::TurretEye(uint8 slot, Creature* turret)
{
    TurretState& ts = _turrets[slot];
    if (Creature* eye = !ts.Eye.IsEmpty() ? turret->GetMap()->GetCreature(ts.Eye) : nullptr)
        if (eye->IsAlive())
            return eye;

    Position const top(turret->GetPositionX(), turret->GetPositionY(),
        turret->GetPositionZ() + turret->GetCollisionHeight() + 1.0f, turret->GetOrientation());
    TempSummon* eye = turret->SummonCreature(BG_SRF_ENTRY_TURRET_EYE, top, TEMPSUMMON_MANUAL_DESPAWN);
    if (!eye)
        return nullptr;
    eye->SetFaction(turret->GetFaction());
    eye->SetImmuneToAll(false);
    eye->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE));
    eye->SetDisableGravity(true);
    eye->SetReactState(REACT_PASSIVE);
    eye->SetLevel(turret->GetLevel());
    ts.Eye = eye->GetGUID();
    return eye;
}

// Every half minute, one line on what the match is doing - so a report from the
// field ("the turrets do not fire", "no jungle") can be read off the log.
void BattlegroundSRF::Report(uint32 diff)
{
    _reportTimer += diff;
    if (_reportTimer < SRF_REPORT_TICK)
        return;
    _reportTimer = 0;
    uint32 campsUp = 0;
    for (CampState const& cs : _camps)
        campsUp += cs.Up ? 1 : 0;
    TC_LOG_INFO("bg.battleground", "Summoner's Rift {}: {} players, {} minions, {} waves, bolts {} cast / {} refused (last refusal {}), camps up {}/{}, unstuck {} from a target / {} onto the lane.",
        GetInstanceID(), GetPlayersSize(), _minions.size(), _wavesSpawned, _boltsCast, _boltsRefused, _lastRefusal,
        campsUp, uint32(BG_SRF_JUNGLE_CAMPS), _unstuckTargets, _unstuckPlaced);
    _boltsCast = 0;
    _boltsRefused = 0;
    _unstuckTargets = 0;
    _unstuckPlaced = 0;
}

// A caster minion fights at range: it holds at League's 550 units and sends a
// bolt every 1.6 seconds, carrying its own melee damage, instead of walking up.
void BattlegroundSRF::UpdateCasterAttack(Creature* caster, Minion& minion, Unit* victim)
{
    if (caster->GetExactDist2d(victim) > SRF_CASTER_RANGE)
    {
        if (caster->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
            caster->GetMotionMaster()->MoveChase(victim, SRF_CASTER_RANGE * 0.8f);
        return;
    }
    if (caster->isMoving())
        caster->StopMoving();
    caster->SetFacingToObject(victim);

    minion.AttackTimer = minion.AttackTimer > BG_SRF_MINION_TICK ? minion.AttackTimer - BG_SRF_MINION_TICK : 0;
    if (minion.AttackTimer)
        return;
    minion.AttackTimer = SRF_CASTER_ATTACK_TIME;

    float const low = caster->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE);
    float const high = caster->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE);
    bool const order = minion.Team == TEAM_ALLIANCE;
    FireBolt(caster, victim, order ? BG_SRF_SPELL_ORDER_CASTER_BOLT : BG_SRF_SPELL_CHAOS_CASTER_BOLT,
        std::max<int32>(1, int32((low + high) / 2.0f)));
}

// -------------------------------------------------------------------- jungle

void BattlegroundSRF::SpawnCamp(uint8 camp)
{
    Map* map = FindBgMap();
    uint32 spawned = 0;
    Position where;
    for (uint8 i = 0; i < BG_SRF_JUNGLE_MONSTERS; ++i)
    {
        SrfMonster const& m = SRF_MONSTERS[i];
        if (m.camp != camp)
            continue;
        uint32 const slot = BG_SRF_CREATURE_JUNGLE_FIRST + i;
        DelCreature(slot);
        Position pos = ToWorld(m.x, m.y, m.z, m.o);
        if (map)
        {
            float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 10.0f, true);
            if (ground > INVALID_HEIGHT)
                pos.Relocate(pos.GetPositionX(), pos.GetPositionY(), ground);
        }
        Creature* creature = AddCreature(m.entry, slot, pos, TEAM_NEUTRAL, SRF_JUNGLE_CORE_RESPAWN);
        if (!creature)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundSRF: failed to spawn jungle monster {} (entry {}).", i, m.entry);
            continue;
        }
        // Neutral until struck; the scuttler never fights back.
        creature->SetReactState(SRF_CAMPS[camp].kind == SRF_CAMP_SCUTTLE ? REACT_PASSIVE : REACT_DEFENSIVE);
        ApplyUnitLevel(creature, AverageMatchLevel(), 1.0f);
        where = creature->GetPosition();
        ++spawned;
    }
    _camps[camp].Up = true;
    TC_LOG_INFO("bg.battleground", "Summoner's Rift {}: camp {} is up, {} monster(s), one at {:.1f} {:.1f} {:.1f}.",
        GetInstanceID(), uint32(camp), spawned, where.GetPositionX(), where.GetPositionY(), where.GetPositionZ());
    if (SRF_CAMPS[camp].kind == SRF_CAMP_DRAGON)
        PSendMessageToAll(BG_SRF_STRING_DRAGON_SPAWNED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
    else if (SRF_CAMPS[camp].kind == SRF_CAMP_BARON)
        PSendMessageToAll(BG_SRF_STRING_BARON_SPAWNED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
}

// Camps come up on their timers, fight as one - a monster struck pulls its
// campmates in - and start their respawn timer once every monster is dead.
// Leashing home is the creature's own: it gives up a target that leaves its
// camp's threat radius.
void BattlegroundSRF::UpdateJungle(uint32 elapsed)
{
    for (uint32& left : _baronTimer)
        left = left > elapsed ? left - elapsed : 0;

    Map* map = FindBgMap();
    if (!map)
        return;

    for (uint8 camp = 0; camp < BG_SRF_JUNGLE_CAMPS; ++camp)
    {
        CampState& cs = _camps[camp];
        if (!cs.Up)
        {
            if (cs.Timer > elapsed)
                cs.Timer -= elapsed;
            else
                SpawnCamp(camp);
            continue;
        }

        std::vector<Creature*> alive;
        Unit* victim = nullptr;
        for (uint8 i = 0; i < BG_SRF_JUNGLE_MONSTERS; ++i)
        {
            if (SRF_MONSTERS[i].camp != camp)
                continue;
            Creature* creature = map->GetCreature(BgCreatures[BG_SRF_CREATURE_JUNGLE_FIRST + i]);
            if (!creature || !creature->IsAlive())
                continue;
            alive.push_back(creature);
            if (!victim && creature->IsInCombat() && creature->GetVictim())
                victim = creature->GetVictim();
        }
        if (alive.empty())
        {
            cs.Up = false;
            cs.Timer = SRF_CAMPS[camp].respawn;
            continue;
        }
        if (victim && SRF_CAMPS[camp].kind != SRF_CAMP_SCUTTLE)
            for (Creature* creature : alive)
                if (!creature->IsInCombat() && creature->IsValidAttackTarget(victim))
                    creature->AI()->AttackStart(victim);
    }
}

// The killer takes the gold (League's last hit) and its side the experience; the
// buff camps buff their killer, the Dragon and Baron the killer's whole team.
bool BattlegroundSRF::HandleJungleKill(Creature* creature, Player* killer)
{
    uint8 index = 0;
    while (index < BG_SRF_JUNGLE_MONSTERS && BgCreatures[BG_SRF_CREATURE_JUNGLE_FIRST + index] != creature->GetGUID())
        ++index;
    if (index == BG_SRF_JUNGLE_MONSTERS)
        return false;

    SrfMonster const& m = SRF_MONSTERS[index];
    TeamId const team = GetTeamIndexByTeamId(GetPlayerTeam(killer->GetGUID()));
    RiftProgression::AddGold(killer, m.gold, true);
    AwardExperience(team, creature, m.xp);

    switch (SRF_CAMPS[m.camp].kind)
    {
        case SRF_CAMP_BLUE:
            ApplyBuff(killer, BG_SRF_SPELL_BLUE_BUFF,
                std::max<int32>(1, int32(float(killer->GetMaxPower(POWER_MANA)) * SRF_BLUE_MANA_PER_TICK)), 0, 0, SRF_BUFF_DURATION);
            break;
        case SRF_CAMP_RED:
            ApplyBuff(killer, BG_SRF_SPELL_RED_BUFF, SRF_RED_AP_PER_LEVEL * int32(killer->GetLevel()), 0, 0, SRF_BUFF_DURATION);
            break;
        case SRF_CAMP_DRAGON:
            ++_dragonsSlain[team];
            for (Player* player : TeamPlayers(team))
                ApplyDragonBuff(player, team);
            PSendMessageToAll(BG_SRF_STRING_DRAGON_SLAIN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(team));
            break;
        case SRF_CAMP_BARON:
            _baronTimer[team] = SRF_BARON_BUFF_DURATION;
            for (Player* player : TeamPlayers(team))
            {
                RiftProgression::AddGold(player, SRF_GOLD_BARON_TEAM, true);
                // League's Hand of Baron: the living only, and lost on death.
                if (player->IsAlive())
                    ApplyBuff(player, BG_SRF_SPELL_BARON_BUFF, 0, 0, SRF_BARON_STATS_PCT, SRF_BARON_BUFF_DURATION);
            }
            PSendMessageToAll(BG_SRF_STRING_BARON_SLAIN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(team));
            break;
        default:
            break;
    }
    return true;
}

// A stock spell with League's values: base points 0 keep the spell's own.
void BattlegroundSRF::ApplyBuff(Player* player, uint32 spellId, int32 bp0, int32 bp1, int32 bp2, int32 durationMs)
{
    player->RemoveAurasDueToSpell(spellId);
    CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
    if (bp0)
        args.AddSpellMod(SPELLVALUE_BASE_POINT0, bp0);
    if (bp1)
        args.AddSpellMod(SPELLVALUE_BASE_POINT1, bp1);
    if (bp2)
        args.AddSpellMod(SPELLVALUE_BASE_POINT2, bp2);
    player->CastSpell(player, spellId, args);
    if (durationMs > 0)
        if (Aura* aura = player->GetAura(spellId))
        {
            aura->SetMaxDuration(durationMs);
            aura->SetDuration(durationMs);
        }
}

// The Dragonslayer's cry grows with every dragon the team has slain and lasts
// the match (the spell's own two hours).
void BattlegroundSRF::ApplyDragonBuff(Player* player, TeamId team)
{
    int32 const stacks = _dragonsSlain[team];
    if (!stacks || !player->IsAlive())
        return;
    ApplyBuff(player, BG_SRF_SPELL_DRAGON_BUFF, SRF_DRAGON_CRIT_PER_STACK * stacks, SRF_DRAGON_AP_PER_STACK * stacks,
        SRF_DRAGON_CRIT_PER_STACK * stacks, 0);
}

// Dying strips the stand-in auras, but League's dragon buffs are kept for the
// match: put them back on whoever lacks them.
void BattlegroundSRF::MaintainObjectiveBuffs()
{
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        if (_dragonsSlain[team])
            for (Player* player : TeamPlayers(team))
                if (player->IsAlive() && !player->HasAura(BG_SRF_SPELL_DRAGON_BUFF))
                    ApplyDragonBuff(player, team);
}

// ----------------------------------------------------------------- fountains

void BattlegroundSRF::UpdateFountains()
{
    for (auto const& [guid, bgPlayer] : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsAlive() || player->GetMapId() != GetMapId())
            continue;

        TeamId const team = GetTeamIndexByTeamId(bgPlayer.Team);
        if (player->GetExactDist2d(FountainPos(team)) <= SRF_FOUNTAIN_HEAL_RANGE)
        {
            player->ModifyHealth(int32(player->GetMaxHealth() * SRF_FOUNTAIN_HEAL_SHARE));
            Powers const power = player->GetPowerType();
            player->ModifyPower(power, int32(player->GetMaxPower(power) * SRF_FOUNTAIN_HEAL_SHARE));
        }
        else if (player->GetExactDist2d(FountainPos(OtherTeam(team))) <= SRF_FOUNTAIN_BURN_RANGE)
            player->EnvironmentalDamage(DAMAGE_FIRE, uint32(player->GetMaxHealth() * SRF_FOUNTAIN_BURN_SHARE));
    }
}

// --------------------------------------------------------------------- misc

WorldSafeLocsEntry const* BattlegroundSRF::GetClosestGraveyard(Player* player)
{
    if (!player)
        return nullptr;
    uint32 const team = GetPlayerTeam(player->GetGUID()) ? GetPlayerTeam(player->GetGUID()) : player->GetTeam();
    return sWorldSafeLocsStore.LookupEntry(team == HORDE ? BG_SRF_GY_CHAOS : BG_SRF_GY_ORDER);
}

bool BattlegroundSRF::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;
    TeamId const team = GetTeamIndexByTeamId(GetPlayerTeam(player->GetGUID()) ? GetPlayerTeam(player->GetGUID()) : player->GetTeam());
    Position const fountain = FountainPos(team);
    player->TeleportTo(GetMapId(), fountain.GetPositionX(), fountain.GetPositionY(), fountain.GetPositionZ() + 1.0f, fountain.GetOrientation());
    return true;
}

void BattlegroundSRF::EndBattleground(uint32 winner)
{
    _wavesStarted = false;
    DespawnMinions();
    Battleground::EndBattleground(winner);
}
