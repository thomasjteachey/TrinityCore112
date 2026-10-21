#include "BattlegroundSRF.h"
#include "BattlegroundSRFBrush.h"

#include "BattlegroundMgr.h"
#include "Creature.h"
#include "GameTime.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "RiftProgression.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
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

// A turret reaches 50 yards - past every class's own range (League's 750 units
// would be 22.5 here, inside a caster's 30 or a hunter's 35, so a ranged champion
// could siege it untouched; owner, 2026-10-20: "at least 50 yards"). Both shot
// spells carry it: Ice Lance 43427 is unlimited, Fireball 22088 100 yards.
float constexpr SRF_TURRET_RANGE        = 50.0f;
float constexpr SRF_MINION_AGGRO_RANGE  = Yards(400.0f);
// With no enemy unit near, a minion takes on an enemy structure in a turret's
// range of it: a wave walking down a lane stops at the first turret standing.
float constexpr SRF_MINION_STRUCTURE_RANGE = Yards(750.0f);
// Further than this from its target, a minion lets it go and takes the closest again.
float constexpr SRF_MINION_LEASH        = Yards(1100.0f);
uint32 constexpr SRF_REPORT_TICK        = 30 * IN_MILLISECONDS;
float constexpr SRF_FOUNTAIN_HEAL_RANGE = Yards(600.0f);
// League's brush (BattlegroundSRFBrush.h): attacking from it reveals for a moment.
uint32 constexpr SRF_BRUSH_TICK         = 250;
uint32 constexpr SRF_BRUSH_REVEAL       = 1500;
// Centurion spell 94142 "Brush": the buff a hidden champion wears, so they can see it.
uint32 constexpr SRF_SPELL_BRUSH        = 94142;
// Centurion spell 94153 "Summoner's Focus": every champion's match aura, +5% hit
// and +3% spell hit, a buff anyone can see.
uint32 constexpr SRF_SPELL_FOCUS        = 94153;
uint32 constexpr SRF_ITEM_SYNC_MS       = 500;
uint32 constexpr SRF_SCORE_TICK         = 2000;    // the scoreboard feed (RIFTSCORE)
uint32 constexpr SRF_MULTIKILL_MS       = 10000;   // League: each kill of a multikill within 10 s of the last...
uint32 constexpr SRF_PENTAKILL_MS       = 30000;   // ...and the pentakill within 30 s of the quadra

// The brush patch in League's grid cell holding (lx, lz), 0 for none.
uint8 BrushCellAt(float lx, float lz)
{
    int32 const ix = int32(std::floor((lx - SRF_BRUSH_MIN_X) / SRF_BRUSH_CELL));
    int32 const iz = int32(std::floor((lz - SRF_BRUSH_MIN_Z) / SRF_BRUSH_CELL));
    if (ix < 0 || iz < 0 || ix >= int32(SRF_BRUSH_WIDTH) || iz >= int32(SRF_BRUSH_DEPTH))
        return 0;
    uint32 const key = uint32(iz) * SRF_BRUSH_WIDTH + uint32(ix);
    auto const itr = std::lower_bound(std::begin(SRF_BRUSH), std::end(SRF_BRUSH), key,
        [](SrfBrushCell const& cell, uint32 k) { return cell.key < k; });
    return itr != std::end(SRF_BRUSH) && itr->key == key ? itr->patch : 0;
}

// The brush patch a champion standing at a world position is in, 0 for none.
// The body, not the point: the centre and four points half a yard out, so a
// champion at a bush's edge - visibly in the leaves - counts as in it. League's
// cells are a yard and a half wide and its bush art spills past their edges;
// one point read "just outside" for someone standing in the brush (2026-10-19,
// 3 cm short of a cell).
float constexpr SRF_BRUSH_BODY = 0.5f / SRF_YARDS_PER_UNIT;
uint8 BrushPatchAt(Position const& pos)
{
    float const lx = SRF_LEAGUE_CENTRE - pos.GetPositionY() / SRF_YARDS_PER_UNIT;
    float const lz = SRF_LEAGUE_CENTRE + pos.GetPositionX() / SRF_YARDS_PER_UNIT;
    float const offsets[5][2] = { { 0, 0 }, { SRF_BRUSH_BODY, 0 }, { -SRF_BRUSH_BODY, 0 }, { 0, SRF_BRUSH_BODY }, { 0, -SRF_BRUSH_BODY } };
    for (auto const& o : offsets)
        if (uint8 const patch = BrushCellAt(lx + o[0], lz + o[1]))
            return patch;
    return 0;
}

// A Rift creature's side, from its faction (a deniable minion's, from its entry).
// -1: neither side's (a jungle monster).
int32 CreatureSide(Creature const* creature)
{
    switch (creature->GetFaction())
    {
        case 1732: return TEAM_ALLIANCE;
        case 1735: return TEAM_HORDE;
        default: break;
    }
    uint32 const entry = creature->GetEntry();
    if (entry >= BG_SRF_ENTRY_ORDER_MELEE && entry <= BG_SRF_ENTRY_CHAOS_CASTER)
        return entry <= BG_SRF_ENTRY_ORDER_CASTER ? TEAM_ALLIANCE : TEAM_HORDE;
    return -1;
}

// The base: the fountain and the shop beside it.
float constexpr SRF_BASE_RANGE          = Yards(1000.0f);
// The shops: League's orderShop01 / chaosShop01 stand outside the walkable area
// (under the boundary embankment), so each is drawn in toward its fountain to the
// nearest spot 4 yards clear of the navigation grid's walls. Facing the middle.
SrfPoint constexpr SRF_SHOP_AT[PVP_TEAMS_COUNT] = { { 322.9f, 171.0f, 682.7f }, { 14151.8f, 179.0f, 14454.3f } };
float constexpr SRF_SHOP_FACING[PVP_TEAMS_COUNT] = { 5.44f, 2.42f };
float constexpr SRF_FOUNTAIN_BURN_RANGE = Yards(700.0f);
// Turret shots as shares of the target's maximum health: minions die to a few
// shots, and a player who stays under a turret takes a little more every shot.
// League's turret shots on minions are shares of their maximum health: a melee
// minion takes 3 shots (45%), a caster 2 (70%).
float constexpr SRF_TURRET_MELEE_SHARE  = 0.45f;
float constexpr SRF_TURRET_CASTER_SHARE = 0.70f;
// On champions: a base share, and League's Warming Up - +50% per champion hit in
// a row, up to +150%, gone 5 s after the last champion hit.
float constexpr SRF_TURRET_PLAYER_SHARE = 0.20f;
// Every turret shot at a quarter less than the shares above (owner, 2026-10-21).
float constexpr SRF_TURRET_DAMAGE_SCALE = 0.75f;
// The pause between one bolt landing and the next leaving the turret.
uint32 constexpr SRF_TURRET_SHOT_GAP = 400;
float constexpr SRF_TURRET_WARMUP_STEP  = 0.50f;
uint8 constexpr SRF_TURRET_WARMUP_MAX   = 3;
uint32 constexpr SRF_TURRET_WARMUP_RESET = 5 * IN_MILLISECONDS;
// A champion damaging an allied champion within 1400 of a turret pulls its fire.
float constexpr SRF_TURRET_CALL_RANGE   = std::max(Yards(1400.0f), SRF_TURRET_RANGE);
// Minions: League acquires within 500; an allied champion under attack calls
// for help from up to 1000 away.
float constexpr SRF_MINION_ACQUIRE_RANGE = Yards(500.0f);
float constexpr SRF_MINION_CALL_RANGE    = Yards(1000.0f);
// Caster minions: League's 550 range, a bolt every 1.6 s.
float constexpr SRF_CASTER_RANGE        = Yards(550.0f);
uint32 constexpr SRF_CASTER_ATTACK_TIME = 1500;    // League's 0.667 attacks a second
// League's rewards, in League gold and experience (wiki "Experience (champion)"
// and "Minion"): a minion's experience goes to the enemy champions within 1500
// units of where it died, the killer always among them; one takes it all, two or
// more split 130% of it. A champion kill's goes to the killer and the allies
// within 1600: 42 to 990 alone, 27.97 to 891 all told when shared, by the victim's
// level, 20% more or less for each full level of difference past the first (never
// under 40%). Jungle camps pay their killer only; the Dragon pays the slaying
// team's living champions near it, Baron the whole team.
float constexpr SRF_XP_RANGE            = Yards(1600.0f);
float constexpr SRF_XP_MINION_RANGE     = Yards(1500.0f);
float constexpr SRF_XP_SHARED_TOTAL     = 1.30f;
float constexpr SRF_XP_MELEE_MINION     = 58.88f;
float constexpr SRF_XP_CASTER_MINION    = 29.44f;
float constexpr SRF_XP_KILL_SOLO_MIN    = 42.0f;
float constexpr SRF_XP_KILL_SOLO_MAX    = 990.0f;
float constexpr SRF_XP_KILL_SHARED_MIN  = 27.97f;
float constexpr SRF_XP_KILL_SHARED_MAX  = 891.0f;
uint32 constexpr SRF_GOLD_MELEE_MINION  = 20;
uint32 constexpr SRF_GOLD_CASTER_MINION = 15;
uint32 constexpr SRF_GOLD_KILL          = 300;
uint32 constexpr SRF_GOLD_FIRST_BLOOD   = 100;     // on top of the kill
uint32 constexpr SRF_GOLD_BOUNTY_MAX    = 1000;    // a shutdown at most
uint32 constexpr SRF_GOLD_STRUCTURE     = 150;     // to every player of the side that destroys it
uint32 constexpr SRF_GOLD_PASSIVE       = 10;      // every SRF_PASSIVE_GOLD_TICK: League's ~1.9 a second
uint32 constexpr SRF_PASSIVE_GOLD_TICK  = 5 * IN_MILLISECONDS;
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
uint32 constexpr SRF_WAVE_START_STAGGER = 792;      // ms between departures: League's 0.792 s

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
    _matchClock = 0;
    _respawnAt.clear();
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

void BattlegroundSRF::RemovePlayer(Player* player, ObjectGuid guid, uint32 /*team*/)
{
    _brush.erase(guid);
    _bots.erase(guid);
    // Gone without a Player (logged out) is OnLogin's to restore.
    if (player)
    {
        player->RemoveAurasDueToSpell(SRF_SPELL_BRUSH);
        player->RemoveAurasDueToSpell(SRF_SPELL_FOCUS);
        if (!player->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            player->RemoveByteFlag(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_AURA_VISION, PLAYER_FIELD_BYTE2_STEALTH);
            player->RemoveVisFlag(UNIT_VIS_FLAGS_CREEP);
        }
        RiftProgression::Leave(player);
    }
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
    _matchClock = 0;
    _respawnAt.clear();
    _stats.clear();
    _firstBlood = false;
    _scoreTimer = 0;
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

    // The shopkeepers, spawned with the match rather than kept in `creature`:
    // a static spawn on a battleground map is no part of the match's own copy.
    Map* map = FindBgMap();
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        SrfPoint const& at = SRF_SHOP_AT[team];
        Position pos = ToWorld(at.x, at.y, at.z, SRF_SHOP_FACING[team]);
        if (map)
        {
            float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 10.0f, true);
            if (ground > INVALID_HEIGHT)
                pos.Relocate(pos.GetPositionX(), pos.GetPositionY(), ground);
        }
        if (!AddCreature(team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_SHOP : BG_SRF_ENTRY_CHAOS_SHOP,
                team == TEAM_ALLIANCE ? BG_SRF_CREATURE_SHOP_ORDER : BG_SRF_CREATURE_SHOP_CHAOS, pos, team))
            TC_LOG_ERROR("bg.battleground", "BattlegroundSRF: failed to spawn the {} shopkeeper.", TeamName(team));
    }

    RefreshVulnerability();
    return true;
}

bool BattlegroundSRF::IsAtBase(Player const* player) const
{
    if (!player || player->GetMapId() != GetMapId())
        return false;
    uint32 const team = GetPlayerTeam(player->GetGUID()) ? GetPlayerTeam(player->GetGUID()) : player->GetTeam();
    return player->GetExactDist2d(FountainPos(GetTeamIndexByTeamId(team))) <= SRF_BASE_RANGE;
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
    AnnounceEvent("WELCOME", TEAM_NEUTRAL);
}

void BattlegroundSRF::PostUpdateImpl(uint32 diff)
{
    // The scoreboard from the moment champions are in, the prep phase included
    // (it only started with the gates: an empty board until then).
    if (GetStatus() == STATUS_WAIT_JOIN || GetStatus() == STATUS_IN_PROGRESS)
    {
        _scoreTimer += diff;
        if (_scoreTimer >= SRF_SCORE_TICK)
        {
            _scoreTimer = 0;
            SendScores();
        }
    }

    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    UpdateRespawns();

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
            AnnounceEvent("INHIBRESPAWN", SlotTeam(slot));
            RefreshVulnerability();
        }
        UpdateStructures();
        if (GetStatus() != STATUS_IN_PROGRESS)
            return;
    }

    if (_wavesStarted)
    {
        _matchClock += diff;
        _waveTimer += diff;
        if (_waveTimer >= WaveInterval())
        {
            _waveTimer -= WaveInterval();
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
    if (_turretTimer >= BG_SRF_TURRET_STEP)
    {
        UpdateTurrets(_turretTimer);
        _turretTimer = 0;
    }

    _fountainTimer += diff;
    if (_fountainTimer >= BG_SRF_FOUNTAIN_TICK)
    {
        _fountainTimer = 0;
        UpdateFountains();
    }

    UpdateProgression(diff);
    Report(diff);

    _brushTimer += diff;
    if (_brushTimer >= SRF_BRUSH_TICK)
    {
        UpdateBrush(_brushTimer);
        ReleaseIdleCombat();
        RefreshFreeTurrets();
        _brushTimer = 0;
    }
}

// Who stands in which brush patch, and whether that hides them. A change is
// pushed to everyone at once (UpdateObjectVisibility both ways), so a champion
// stepping into a bush vanishes from the enemies outside it - and an enemy
// stepping into the same bush sees it again.
void BattlegroundSRF::UpdateBrush(uint32 elapsed)
{
    // League's inventory works from the bags: what a sale, a drop or a
    // destroyed item changed is picked up here (RiftProgression::SyncItems).
    _itemSyncTimer += elapsed;
    bool const syncItems = _itemSyncTimer >= SRF_ITEM_SYNC_MS;
    if (syncItems)
        _itemSyncTimer = 0;

    for (auto const& [guid, bgPlayer] : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || player->GetMapId() != GetMapId())
        {
            _brush.erase(guid);
            continue;
        }
        if (syncItems)
            RiftProgression::SyncItems(player);
        if (player->IsAlive() && !player->HasAura(SRF_SPELL_FOCUS))
            player->AddAura(SRF_SPELL_FOCUS, player);
        BrushState& st = _brush[guid];
        uint8 const patch = player->IsAlive() ? BrushPatchAt(player->GetPosition()) : 0;
        if (patch && AttackOf(player))
            st.RevealMs = SRF_BRUSH_REVEAL;
        else
            st.RevealMs = st.RevealMs > elapsed ? st.RevealMs - elapsed : 0;
        bool const hidden = patch && !st.RevealMs;
        if (patch != st.Patch || hidden != st.Hidden)
        {
            st.Patch = patch;
            st.Hidden = hidden;
            player->UpdateObjectVisibility();
        }
        // In the fountain or not: CENTURION_Rift shows the pick panel only there.
        int8 const atBase = IsAtBase(player) ? 1 : 0;
        if (atBase != st.AtBase)
        {
            st.AtBase = atBase;
            RiftProgression::SendBase(player, atBase != 0);
        }
        // Checked every tick, not only on a change: death or a zone-in can strip it.
        if (hidden != player->HasAura(SRF_SPELL_BRUSH))
        {
            if (hidden)
                player->AddAura(SRF_SPELL_BRUSH, player);
            else
                player->RemoveAurasDueToSpell(SRF_SPELL_BRUSH);
        }
        // And the look of it, both flags HandleModStealth sets: the stealth
        // vision, and the creep flag that draws the model see-through - for the
        // champion and the allies who still see it, as League shows a champion
        // in the brush. The vision flag alone left the model solid. Real stealth
        // keeps its own flags.
        bool const looksHidden = player->HasByteFlag(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_AURA_VISION, PLAYER_FIELD_BYTE2_STEALTH)
            && player->HasByteFlag(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_VIS_FLAG, UNIT_VIS_FLAGS_CREEP);
        if (hidden && !looksHidden)
        {
            player->SetByteFlag(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_AURA_VISION, PLAYER_FIELD_BYTE2_STEALTH);
            player->SetVisFlag(UNIT_VIS_FLAGS_CREEP);
        }
        else if (!hidden && !player->HasAuraType(SPELL_AURA_MOD_STEALTH)
            && (player->HasByteFlag(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_AURA_VISION, PLAYER_FIELD_BYTE2_STEALTH)
                || player->HasByteFlag(UNIT_FIELD_BYTES_1, UNIT_BYTES_1_OFFSET_VIS_FLAG, UNIT_VIS_FLAGS_CREEP)))
        {
            player->RemoveByteFlag(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_AURA_VISION, PLAYER_FIELD_BYTE2_STEALTH);
            player->RemoveVisFlag(UNIT_VIS_FLAGS_CREEP);
        }
    }
}

bool BattlegroundSRF::HidesFrom(Player const* target, WorldObject const* seer) const
{
    auto const itr = _brush.find(target->GetGUID());
    if (itr == _brush.end() || !itr->second.Hidden)
        return false;
    uint32 const targetTeam = GetPlayerTeam(target->GetGUID());
    if (Player const* player = seer->ToPlayer())
    {
        if (player->IsGameMaster() || GetPlayerTeam(player->GetGUID()) == targetTeam)
            return false;
        auto const seen = _brush.find(player->GetGUID());
        return seen == _brush.end() || seen->second.Patch != itr->second.Patch;
    }
    if (Creature const* creature = seer->ToCreature())
    {
        // the enemy side's minions, turrets and their eyes; never a jungle monster
        int32 const side = CreatureSide(creature);
        return side >= 0 && TeamValue(TeamId(side)) != targetTeam;
    }
    return false;
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
    {
        if (Creature* eye = map->GetCreature(_turrets[slot].Eye))
            eye->DespawnOrUnsummon();
        // The models have no death: a fallen structure stood there as if alive.
        // It goes, as League's crumble (an inhibitor comes back by SpawnStructure).
        if (Creature* corpse = map->GetCreature(BgCreatures[slot]))
            corpse->DespawnOrUnsummon(1500ms);
    }
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
            AnnounceEvent("INHIB", destroyer);
            RewardHonorToTeam(BG_SRF_HONOR_INHIBITOR, TeamValue(destroyer));
            break;
        default:
            PSendMessageToAll(BG_SRF_STRING_TURRET_DESTROYED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(destroyer));
            AnnounceEvent("TURRET", destroyer);
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
        // League announces the first wave only ("Minions have spawned!"), not
        // every one - every 30 seconds in the chat was noise.
        if (!_wavesSpawned)
        {
            PSendMessageToAll(BG_SRF_STRING_MINIONS_SPAWNED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
            AnnounceEvent("MINIONS", TEAM_NEUTRAL);
        }
        ++_wavesSpawned;
    }
}

Unit* BattlegroundSRF::FindMinionTarget(Creature* minion, TeamId team, float range, ObjectGuid ignore) const
{
    // League's priority list (MinionPriority), closest first within a tier,
    // measured edge to edge. A structure is in reach from further off: a wave
    // walking up to a turret stops at it.
    Map* map = minion->GetMap();
    Unit* best = nullptr;
    uint8 bestTier = 0xFF;
    float bestDist = std::numeric_limits<float>::max();
    auto consider = [&](Unit* unit, float reach)
    {
        if (!unit || !unit->IsAlive() || unit->GetGUID() == ignore || !minion->IsValidAttackTarget(unit))
            return;
        float const dist = minion->GetExactDist2d(unit) - unit->GetCombatReach();
        if (dist > reach)
            return;
        uint8 const tier = MinionPriority(minion, team, unit);
        if (tier < bestTier || (tier == bestTier && dist < bestDist))
        {
            best = unit;
            bestTier = tier;
            bestDist = dist;
        }
    };

    float const acquire = std::max(range, SRF_MINION_ACQUIRE_RANGE);
    for (Minion const& other : _minions)
        if (other.Team != team)
            consider(map->GetCreature(other.Guid), acquire);

    TeamId const enemy = OtherTeam(team);
    for (auto const& [guid, bgPlayer] : GetPlayers())
        if (bgPlayer.Team == TeamValue(enemy))
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                if (player->GetMap() == map)
                {
                    consider(player, acquire);
                    consider(EnemyPetOf(player), acquire);
                }

    float const structureReach = std::max(acquire, SRF_MINION_STRUCTURE_RANGE);
    for (uint8 i = 0; i < BG_SRF_STRUCTURES_PER_TEAM; ++i)
    {
        uint8 const slot = SlotOf(enemy, i);
        if (_structures[slot].Vulnerable && !_structures[slot].Destroyed)
            consider(map->GetCreature(BgCreatures[slot]), structureReach);
    }
    return best;
}

// Who a unit is attacking: a creature's victim, a turret's target, a player's
// victim - or, for a caster, the hostile unit it has selected while fighting.
Unit* BattlegroundSRF::AttackOf(Unit* unit) const
{
    if (!unit)
        return nullptr;
    if (Creature* creature = unit->ToCreature())
    {
        for (uint8 slot = 0; slot < _structures.size(); ++slot)
            if (BgCreatures[slot] == creature->GetGUID())
                return !_turrets[slot].Target.IsEmpty() ? ObjectAccessor::GetUnit(*creature, _turrets[slot].Target) : nullptr;
        return creature->GetVictim();
    }
    Player* player = unit->ToPlayer();
    if (!player)
        return nullptr;
    if (Unit* victim = player->GetVictim())
        return victim;
    if (!player->IsInCombat())
        return nullptr;
    Unit* selected = player->GetSelectedUnit();
    return selected && selected->IsAlive() && player->IsValidAttackTarget(selected) ? selected : nullptr;
}

bool BattlegroundSRF::IsAlly(Unit* unit, TeamId team) const
{
    if (!unit)
        return false;
    if (Player* player = unit->ToPlayer())
        return GetPlayerTeam(player->GetGUID()) == TeamValue(team);
    for (Minion const& m : _minions)
        if (m.Guid == unit->GetGUID())
            return m.Team == team;
    return false;
}

Pet* BattlegroundSRF::EnemyPetOf(Player* player) const
{
    Pet* pet = player ? player->GetPet() : nullptr;
    return pet && pet->IsAlive() && pet->GetMap() == player->GetMap() ? pet : nullptr;
}

bool BattlegroundSRF::IsChampionPet(Unit* unit) const
{
    if (!unit || !unit->IsPet())
        return false;
    Unit* owner = unit->GetOwner();
    return owner && owner->GetTypeId() == TYPEID_PLAYER;
}

// A minion, turret or turret eye that is in combat with a player it is not
// fighting - not its target, and not attacked by that player - lets go of
// them. The stock rule keeps a player in combat for as long as any creature
// holds them, and Rift units never evade, so one turret shot or a minion that
// had moved on kept a champion in combat until that unit died.
void BattlegroundSRF::ReleaseIdleCombat()
{
    Map* map = FindBgMap();
    if (!map)
        return;
    std::vector<CombatReference*> drop;
    auto release = [&](Creature* creature, ObjectGuid target)
    {
        if (!creature)
            return;
        drop.clear();
        for (auto const& [guid, ref] : creature->GetCombatManager().GetPvECombatRefs())
        {
            if (!guid.IsPlayer() || guid == target)
                continue;
            Unit* other = ref->GetOther(creature);
            if (other && AttackOf(other) == creature)
                continue;
            drop.push_back(ref);
        }
        for (CombatReference* ref : drop)
            ref->EndCombat();
    };

    for (Minion const& m : _minions)
        if (Creature* creature = map->GetCreature(m.Guid))
            release(creature, creature->GetVictim() ? creature->GetVictim()->GetGUID() : ObjectGuid::Empty);
    for (uint8 slot = 0; slot < _structures.size(); ++slot)
    {
        Creature* structure = map->GetCreature(BgCreatures[slot]);
        ObjectGuid const target = _turrets[slot].Target;
        release(structure, target);
        if (!_turrets[slot].Eye.IsEmpty())
            release(map->GetCreature(_turrets[slot].Eye), target);
    }
}

// ------------------------------------------------------------------ bots
//
// League's bots: one to each lane, the bottom lane taking a second (carry and
// support) and a fourth or later bot jungling; a bot holds just behind its
// frontmost allied minion, last-hits the enemy wave, fights an enemy champion
// that comes into its lane, and keeps out of a standing enemy turret's reach
// unless allied minions are under it taking the shots. Low on health (30%) it
// goes home and stays until healed (90%); rich enough for its next item it goes
// home to shop (RiftProgression::Maintain spends for it there).

float constexpr SRF_BOT_RETREAT_PCT   = 30.0f;
float constexpr SRF_BOT_RETURN_PCT    = 90.0f;
float constexpr SRF_BOT_ENGAGE_RANGE  = 22.0f;    // yards round its post (or itself) it fights in
float constexpr SRF_BOT_CLASH_RANGE   = 30.0f;    // an enemy minion this close to an allied one is a fight
float constexpr SRF_BOT_BEHIND_WAVE   = 5.0f;     // yards it keeps behind its own wave
uint32 constexpr SRF_BOT_SHOP_GOLD    = 1300;     // League gold that sends it home to shop
uint32 constexpr SRF_SPELL_RECALL     = 94120;    // League's recall (RiftProgression teaches it)

BattlegroundSRF::BotState& BattlegroundSRF::BotOf(Player* bot)
{
    BotState& st = _bots[bot->GetGUID()];
    if (st.Assigned)
        return st;
    // League's laning: top, mid and bottom first, bottom twice, then the jungle.
    TeamId const team = GetTeamIndexByTeamId(GetPlayerTeam(bot->GetGUID()));
    uint8 counts[BG_SRF_LANE_COUNT + 1] = {};
    for (auto const& [guid, other] : _bots)
        if (other.Assigned && guid != bot->GetGUID())
            if (GetTeamIndexByTeamId(GetPlayerTeam(guid)) == team)
                ++counts[other.Lane];
    uint8 const order[] = { BG_SRF_LANE_MID, BG_SRF_LANE_TOP, BG_SRF_LANE_BOT, BG_SRF_LANE_NONE, BG_SRF_LANE_BOT };
    uint8 const quota[BG_SRF_LANE_COUNT + 1] = { 1, 1, 2, 1 };    // top, mid, bot, jungle
    st.Lane = BG_SRF_LANE_MID;
    for (uint8 lane : order)
        if (counts[lane] < quota[lane])
        {
            st.Lane = lane;
            break;
        }
    st.Assigned = true;
    return st;
}

// A standing enemy turret whose reach `pos` is in (plus `margin`) and that is
// free to shoot a champion: no allied minion of `team` inside its reach.
void BattlegroundSRF::RefreshFreeTurrets()
{
    Map* map = FindBgMap();
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        std::vector<Position>& out = _freeTurrets[team];
        out.clear();
        if (!map)
            continue;
        TeamId const enemy = OtherTeam(team);
        for (uint8 i = 0; i < BG_SRF_STRUCTURES_PER_TEAM; ++i)
        {
            uint8 const slot = SlotOf(enemy, i);
            if (_structures[slot].Destroyed || StructureAt(slot).kind != BG_SRF_KIND_TURRET)
                continue;
            Creature* turret = map->GetCreature(BgCreatures[slot]);
            if (!turret || !turret->IsAlive())
                continue;
            bool tanked = false;
            for (Minion const& m : _minions)
                if (m.Team == team && turret->GetExactDist2d(&m.Pos) <= SRF_TURRET_RANGE)
                {
                    tanked = true;
                    break;
                }
            if (!tanked)
                out.push_back(turret->GetPosition());
        }
    }
}

bool BattlegroundSRF::UnderFreeEnemyTurret(TeamId team, Position const& pos, float margin) const
{
    for (Position const& turret : _freeTurrets[team])
        if (turret.GetExactDist2d(&pos) <= SRF_TURRET_RANGE + margin)
            return true;
    return false;
}

uint32 BattlegroundSRF::LaneIndexNear(TeamId team, uint8 lane, Position const& pos)
{
    std::vector<Position> const& path = LanePath(team, lane);
    uint32 best = 0;
    float bestDist = std::numeric_limits<float>::max();
    for (uint32 i = 0; i < path.size(); ++i)
    {
        float const d = path[i].GetExactDist2d(&pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

bool BattlegroundSRF::BotMayAttack(Player* bot, Unit* target) const
{
    if (!bot || !target)
        return false;
    TeamId const team = GetTeamIndexByTeamId(GetPlayerTeam(bot->GetGUID()));
    // a turret itself may be hit while minions tank it - that is UnderFreeEnemyTurret's own test
    return !UnderFreeEnemyTurret(team, target->GetPosition(), bot->GetCombatReach());
}

// Asked twice a bot tick (the decision and the action); worked out at most
// every SRF_BOT_ORDERS_MS.
uint32 constexpr SRF_BOT_ORDERS_MS = 250;

bool BattlegroundSRF::GetBotOrders(Player* bot, BattlegroundBotOrders& out)
{
    if (!bot || !bot->IsAlive() || GetStatus() != STATUS_IN_PROGRESS)
        return false;
    BotState& st = BotOf(bot);
    uint32 const now = GameTime::GetGameTimeMS();
    if (st.HasOrders && now - st.OrdersAt < SRF_BOT_ORDERS_MS)
    {
        out = st.Orders;
        return true;
    }
    st.HasOrders = ComputeBotOrders(bot, st, st.Orders);
    st.OrdersAt = now;
    out = st.Orders;
    return st.HasOrders;
}

bool BattlegroundSRF::ComputeBotOrders(Player* bot, BotState& st, BattlegroundBotOrders& out)
{
    Map* map = FindBgMap();
    if (!map)
        return false;
    TeamId const team = GetTeamIndexByTeamId(GetPlayerTeam(bot->GetGUID()));
    out = BattlegroundBotOrders();

    // Home: to heal, or to spend.
    float const pct = bot->GetHealthPct();
    if (pct < SRF_BOT_RETREAT_PCT)
        st.Retreating = true;
    else if (st.Retreating && pct >= SRF_BOT_RETURN_PCT && !(IsAtBase(bot) && bot->GetMoney() >= SRF_BOT_SHOP_GOLD * GOLD))
        st.Retreating = false;
    if (!st.Retreating && bot->GetMoney() >= SRF_BOT_SHOP_GOLD * GOLD && !bot->IsInCombat() && !IsAtBase(bot))
        st.Retreating = true;
    if (st.Retreating)
    {
        out.Retreat = true;
        out.Post = FountainPos(team);
        // Recall when nobody hostile is close and nothing is hitting it.
        // Recall when nothing hostile is close: no enemy champion within 30
        // yards and no enemy minion within 20 - a bot used to stand channelling
        // right beside a minion fight, which reads as standing idle. Until then
        // it walks home.
        bool threatened = bot->IsInCombat();
        if (!threatened)
            for (Player* enemy : TeamPlayers(OtherTeam(team)))
                if (enemy->IsAlive() && enemy->GetExactDist2d(bot) < 30.0f)
                {
                    threatened = true;
                    break;
                }
        if (!threatened)
            for (Minion const& m : _minions)
                if (m.Team != team && bot->GetExactDist2d(&m.Pos) < 20.0f)
                {
                    threatened = true;
                    break;
                }
        out.RecallSpell = !threatened && !IsAtBase(bot) && !bot->HasUnitState(UNIT_STATE_CASTING) ? SRF_SPELL_RECALL : 0;
        return true;
    }

    // The jungler: the next camp that is up, its own side first.
    if (st.Lane == BG_SRF_LANE_NONE)
    {
        for (uint8 tries = 0; tries < BG_SRF_JUNGLE_CAMPS; ++tries)
        {
            uint8 const camp = (st.Camp + tries) % BG_SRF_JUNGLE_CAMPS;
            if (!_camps[camp].Up || SRF_CAMPS[camp].kind == SRF_CAMP_BARON)
                continue;
            Creature* best = nullptr;
            for (uint8 i = 0; i < BG_SRF_JUNGLE_MONSTERS; ++i)
                if (SRF_MONSTERS[i].camp == camp)
                    if (Creature* monster = map->GetCreature(BgCreatures[BG_SRF_CREATURE_JUNGLE_FIRST + i]))
                        if (monster->IsAlive() && (!best || monster->GetExactDist2d(bot) < best->GetExactDist2d(bot)))
                            best = monster;
            if (!best)
                continue;
            st.Camp = camp;
            out.Post = best->GetPosition();
            out.Target = best->GetGUID();
            return true;
        }
        // nothing up: help the middle lane
        st.Lane = BG_SRF_LANE_MID;
    }

    // Laning: just behind the frontmost allied minion of its lane, or at its
    // outermost standing turret while there is none.
    std::vector<Position> const& path = LanePath(team, st.Lane);
    if (path.empty())
        return false;
    // The front is where the waves meet: the allied minion of this lane closest
    // to an enemy minion (within SRF_BOT_CLASH_RANGE). Only with no fight in the
    // lane is it the minion furthest down the path - one that wandered ahead
    // used to drag the post away from the fight and leave the bot idle.
    int32 front = -1;
    float clash = SRF_BOT_CLASH_RANGE;
    Position const* clashAt = nullptr;
    for (Minion const& m : _minions)
    {
        if (m.Team != team || m.Lane != st.Lane)
            continue;
        front = std::max<int32>(front, int32(m.PathIndex));
        for (Minion const& e : _minions)
            if (e.Team != team)
                if (float const d = m.Pos.GetExactDist2d(&e.Pos); d < clash)
                {
                    clash = d;
                    clashAt = &m.Pos;
                }
    }
    if (clashAt)
        front = int32(LaneIndexNear(team, st.Lane, *clashAt));
    if (front < 0)
    {
        front = 0;
        for (uint8 i = 0; i < BG_SRF_STRUCTURES_PER_TEAM; ++i)
        {
            uint8 const slot = SlotOf(team, i);
            if (!_structures[slot].Destroyed && StructureAt(slot).kind == BG_SRF_KIND_TURRET && StructureAt(slot).lane == st.Lane)
                front = std::max<int32>(front, int32(LaneIndexNear(team, st.Lane, StructurePos(slot))));
        }
    }
    front = std::min<int32>(front, int32(path.size()) - 1);
    // back along the path until SRF_BOT_BEHIND_WAVE behind it, and out of free turret reach
    int32 post = front;
    float walked = 0.0f;
    while (post > 0 && walked < SRF_BOT_BEHIND_WAVE)
    {
        walked += path[post].GetExactDist2d(&path[post - 1]);
        --post;
    }
    while (post > 0 && UnderFreeEnemyTurret(team, path[post], 3.0f))
        --post;
    out.Post = path[post];

    // What to hit near the post: an enemy minion low enough to last-hit, then any
    // enemy minion, then an enemy champion in reach, then a turret the minions tank.
    Unit* best = nullptr;
    int32 bestScore = 0;
    auto consider = [&](Unit* unit, int32 score)
    {
        if (!unit || !unit->IsAlive() || !bot->IsValidAttackTarget(unit))
            return;
        // in reach of the post, or of the bot itself
        if ((unit->GetExactDist2d(&out.Post) > SRF_BOT_ENGAGE_RANGE && unit->GetExactDist2d(bot) > SRF_BOT_ENGAGE_RANGE)
            || !BotMayAttack(bot, unit))
            return;
        if (score > bestScore || (score == bestScore && best && unit->GetExactDist2d(bot) < best->GetExactDist2d(bot)))
        {
            best = unit;
            bestScore = score;
        }
    };
    for (Minion const& m : _minions)
        if (m.Team != team)
            if (Creature* minion = map->GetCreature(m.Guid))
                consider(minion, minion->GetHealthPct() < 35.0f ? 40 : 20);
    for (Player* enemy : TeamPlayers(OtherTeam(team)))
        consider(enemy, enemy->GetHealthPct() < 40.0f ? 50 : 30);
    for (uint8 i = 0; i < BG_SRF_STRUCTURES_PER_TEAM; ++i)
    {
        uint8 const slot = SlotOf(OtherTeam(team), i);
        if (_structures[slot].Vulnerable && !_structures[slot].Destroyed)
            consider(map->GetCreature(BgCreatures[slot]), 10);
    }
    if (best)
        out.Target = best->GetGUID();
    return true;
}

// League's minion target priority, 1 highest:
//   1 an enemy champion attacking an allied champion (the ally within 1000)
//   2 an enemy minion attacking an allied champion
//   3 an enemy minion attacking an allied minion
//   4 an enemy turret attacking an allied minion
//   5 the closest enemy minion
//   6 the closest enemy champion
//   7 a structure
uint8 BattlegroundSRF::MinionPriority(Creature* minion, TeamId team, Unit* target) const
{
    Unit* attacking = AttackOf(target);
    bool const alliedChampion = attacking && attacking->GetTypeId() == TYPEID_PLAYER && IsAlly(attacking, team)
        && minion->GetExactDist2d(attacking) <= SRF_MINION_CALL_RANGE;
    bool const alliedMinion = attacking && attacking->GetTypeId() == TYPEID_UNIT && IsAlly(attacking, team);

    if (target->GetTypeId() == TYPEID_PLAYER)
        return alliedChampion ? 1 : 6;
    for (Minion const& m : _minions)
        if (m.Guid == target->GetGUID())
            return alliedChampion ? 2 : alliedMinion ? 3 : 5;
    if (IsChampionPet(target))
        return alliedChampion ? 2 : alliedMinion ? 3 : 5;
    // a structure: a turret firing on an allied minion comes before the minions
    for (uint8 slot = 0; slot < _structures.size(); ++slot)
        if (BgCreatures[slot] == target->GetGUID())
            return StructureAt(slot).kind == BG_SRF_KIND_TURRET && alliedMinion ? 4 : 7;
    return 7;
}

// League's cadence: a wave every 30 s, every 25 s from 14:00, every 20 s from 30:00.
uint32 BattlegroundSRF::WaveInterval() const
{
    if (_matchClock >= 30 * MINUTE * IN_MILLISECONDS)
        return 20 * IN_MILLISECONDS;
    if (_matchClock >= 14 * MINUTE * IN_MILLISECONDS)
        return 25 * IN_MILLISECONDS;
    return BG_SRF_WAVE_INTERVAL;
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
        m.Pos = creature->GetPosition();

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
                // League: a minion switches only to a higher priority target -
                // and one hitting a tower ignores calls for help about champions.
                uint8 const current = MinionPriority(creature, m.Team, victim);
                if (current > 1)
                    if (Unit* better = FindMinionTarget(creature, m.Team, SRF_MINION_AGGRO_RANGE, m.Ignore))
                    {
                        uint8 const tier = MinionPriority(creature, m.Team, better);
                        bool const onTower = current == 4 || current == 7;
                        if (tier < current && !(onTower && (tier == 1 || better->GetTypeId() == TYPEID_PLAYER)))
                        {
                            creature->AI()->SetGUID(better->GetGUID(), m.Caster ? 1 : 0);
                            m.Target = better->GetGUID();
                            victim = better;
                        }
                    }
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
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;
    TeamId const enemy = OtherTeam(minion.Team);
    // A denied minion is worth nothing to anybody.
    if (minion.Denied && minion.Paid)
        return;
    // Where it stood at the last tick: a minion's corpse can be gone before the
    // tick that notices it died, and the experience was lost with it.
    Position const where = corpse ? corpse->GetPosition() : minion.Pos;
    AwardMinionExperience(enemy, where, minion.Caster ? SRF_XP_CASTER_MINION : SRF_XP_MELEE_MINION, minion.Killer);
    if (!GetCustomRules().RiftLastHitGold && !minion.Paid && corpse)
        AwardGold(enemy, corpse, minion.Caster ? SRF_GOLD_CASTER_MINION : SRF_GOLD_MELEE_MINION);
}

void BattlegroundSRF::AwardMinionExperience(TeamId team, Position const& where, float leagueXp, ObjectGuid killer)
{
    if (GetCustomRules().RiftSharedExperience)
    {
        for (Player* player : TeamPlayers(team))
            RiftProgression::AddExperience(player, leagueXp);
        return;
    }
    std::vector<Player*> share;
    for (Player* player : TeamPlayers(team))
        if (player->GetGUID() == killer || (player->IsAlive() && player->GetExactDist2d(where) <= SRF_XP_MINION_RANGE))
            share.push_back(player);
    if (share.empty())
        return;
    float const each = share.size() == 1 ? leagueXp : leagueXp * SRF_XP_SHARED_TOTAL / float(share.size());
    for (Player* player : share)
        RiftProgression::AddExperience(player, each);
}

void BattlegroundSRF::AwardKillExperience(TeamId team, Player* victim, Player* killer)
{
    std::vector<Player*> share;
    for (Player* player : TeamPlayers(team))
        if (player == killer || (player->IsAlive() && player->GetExactDist2d(victim) <= SRF_XP_RANGE))
            share.push_back(player);
    if (share.empty())
        return;
    uint8 const victimLevel = RiftProgression::GetLeagueLevel(victim);
    float const t = float(std::clamp<uint8>(victimLevel, 1, 18) - 1) / 17.0f;
    float const each = share.size() == 1
        ? SRF_XP_KILL_SOLO_MIN + (SRF_XP_KILL_SOLO_MAX - SRF_XP_KILL_SOLO_MIN) * t
        : (SRF_XP_KILL_SHARED_MIN + (SRF_XP_KILL_SHARED_MAX - SRF_XP_KILL_SHARED_MIN) * t) / float(share.size());
    for (Player* player : share)
    {
        // 20% less (higher) or more (lower) per full level of difference past the first
        int32 const diff = int32(RiftProgression::GetLeagueLevel(player)) - int32(victimLevel);
        float mult = 1.0f;
        if (diff >= 2)
            mult = std::max(0.4f, 1.0f - 0.2f * float(diff - 1));
        else if (diff <= -2)
            mult = 1.0f + 0.2f * float(-diff - 1);
        RiftProgression::AddExperience(player, each * mult);
    }
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
    it->Killer = killer->GetGUID();
    if (GetTeamIndexByTeamId(GetPlayerTeam(killer->GetGUID())) == it->Team)
    {
        it->Denied = true;
        return;
    }
    ++_stats[killer->GetGUID()].Minions;
    if (GetCustomRules().RiftLastHitGold)
        RiftProgression::AddGold(killer, it->Caster ? SRF_GOLD_CASTER_MINION : SRF_GOLD_MELEE_MINION, true);
}

void BattlegroundSRF::HandleKillPlayer(Player* victim, Player* killer)
{
    Battleground::HandleKillPlayer(victim, killer);
    if (!victim || GetStatus() != STATUS_IN_PROGRESS)
        return;
    TeamId const victimTeam = GetTeamIndexByTeamId(GetPlayerTeam(victim->GetGUID()));
    // League's death timer, the champion's own; CENTURION_Rift counts it down.
    uint32 const respawn = DeathTimer(victim);
    _respawnAt[victim->GetGUID()] = GameTime::GetGameTimeMS() + respawn;
    RiftProgression::Tell(victim, Trinity::StringFormat("RIFTDEATH:{}", respawn));
    ChampionStats& vs = _stats[victim->GetGUID()];
    uint32 const victimStreak = vs.Streak;
    uint32 const victimDeaths = vs.DeathStreak;
    ++vs.Deaths;
    ++vs.DeathStreak;
    vs.Streak = 0;
    vs.Multi = 0;

    // No champion to credit - a turret, a minion, a monster: League's "Executed".
    if (!killer || killer == victim || GetTeamIndexByTeamId(GetPlayerTeam(killer->GetGUID())) == victimTeam)
    {
        Announce(Trinity::StringFormat("RIFTANN:EXECUTED::{}:{}:0::{}:0:{}", GetPlayerDisplayName(victim), uint32(OtherTeam(victimTeam)),
            uint32(victim->GetClass()), victim->GetGUID().GetCounter()));
        CheckAce(victimTeam);
        return;
    }

    TeamId const killerTeam = OtherTeam(victimTeam);
    ChampionStats& ks = _stats[killer->GetGUID()];
    bool const firstBlood = !_firstBlood;
    _firstBlood = true;
    uint32 const gold = KillBounty(victimStreak, victimDeaths) + (firstBlood ? SRF_GOLD_FIRST_BLOOD : 0);
    ++ks.Kills;
    ++ks.Streak;
    ks.DeathStreak = 0;
    uint32 const window = ks.Multi == 4 ? SRF_PENTAKILL_MS : SRF_MULTIKILL_MS;
    ks.Multi = ks.Multi && _matchClock - ks.LastKillMs <= window ? ks.Multi + 1 : 1;
    ks.LastKillMs = _matchClock;

    // Assists: League's are the allies who damaged the victim in its last 10 s;
    // here, the allies fighting next to the killer when it fell (the experience
    // range). They share half the kill's gold, as in League.
    std::vector<Player*> assists;
    for (Player* ally : TeamPlayers(killerTeam))
        if (ally != killer && ally->IsAlive() && ally->IsInCombat() && ally->GetExactDist2d(victim) <= SRF_XP_RANGE)
            assists.push_back(ally);
    for (Player* ally : assists)
    {
        ++_stats[ally->GetGUID()].Assists;
        RiftProgression::AddGold(ally, std::max<uint32>(1, gold / 2 / uint32(assists.size())), true);
    }
    RiftProgression::AddGold(killer, gold, true);
    AwardKillExperience(killerTeam, victim, killer);

    // One announcement a kill, League's order: a multikill, a shutdown, first
    // blood, a spree, or the kill itself.
    static char const* const MULTI[] = { "DOUBLE", "TRIPLE", "QUADRA", "PENTA" };
    char const* kind = "KILL";
    uint32 arg = gold;
    if (ks.Multi >= 2)
        kind = MULTI[std::min<uint32>(ks.Multi, 5) - 2];
    else if (victimStreak >= 3)
        kind = "SHUTDOWN";
    else if (firstBlood)
        kind = "FIRSTBLOOD";
    else if (ks.Streak >= 3)
    {
        kind = "SPREE";
        arg = ks.Streak;
    }
    // Names as players see them - "<name> <surname>", a bot by its display name,
    // not its internal one - and the two guids, which is how CENTURION_Rift
    // knows the line is about its own player (a name compared as text missed it).
    Announce(Trinity::StringFormat("RIFTANN:{}:{}:{}:{}:{}:{}:{}:{}:{}", kind, GetPlayerDisplayName(killer), GetPlayerDisplayName(victim),
        uint32(killerTeam), arg, uint32(killer->GetClass()), uint32(victim->GetClass()), killer->GetGUID().GetCounter(),
        victim->GetGUID().GetCounter()));
    CheckAce(victimTeam);
}

// League's death timer: the base respawn wait for the champion's level, longer
// the later the match (the time impact factor: +0.425% every 30 sec from 15:00,
// +0.30% from 30:00, +1.45% from 45:00, at most +50%).
uint32 BattlegroundSRF::DeathTimer(Player const* player) const
{
    static float constexpr BASE_RESPAWN_WAIT[18] =
        { 10.0f, 10.0f, 12.0f, 12.0f, 14.0f, 16.0f, 20.0f, 25.0f, 28.0f, 32.5f, 35.0f, 37.5f, 40.0f, 42.5f, 45.0f, 47.5f, 50.0f, 52.5f };
    uint8 const level = std::clamp<uint8>(RiftProgression::GetLeagueLevel(player), 1, 18);
    uint32 const halfMinutes = _matchClock / (30 * IN_MILLISECONDS);
    float impact = 0.0f;
    if (halfMinutes > 30)
        impact += 0.425f * float(std::min<uint32>(halfMinutes, 60) - 30);
    if (halfMinutes > 60)
        impact += 0.30f * float(std::min<uint32>(halfMinutes, 90) - 60);
    if (halfMinutes > 90)
        impact += 1.45f * float(halfMinutes - 90);
    impact = std::min(impact, 50.0f);
    return uint32(BASE_RESPAWN_WAIT[level - 1] * (1.0f + impact / 100.0f) * float(IN_MILLISECONDS));
}

// The spirit guides never call a wave: UpdateRespawns brings each champion back.
uint32 BattlegroundSRF::GetResurrectionInterval() const
{
    return 24 * HOUR * IN_MILLISECONDS;
}

// A champion whose timer has run out is back at its fountain - released or not,
// full health and mana, as in League. One found dead with no timer (back from a
// disconnect, say) gets one now: with no waves it would never come back.
void BattlegroundSRF::UpdateRespawns()
{
    uint32 const now = GameTime::GetGameTimeMS();
    for (auto const& [guid, bgPlayer] : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || player->GetBattleground() != this)
            continue;
        auto const itr = _respawnAt.find(guid);
        if (player->IsAlive())
        {
            if (itr != _respawnAt.end())
                _respawnAt.erase(itr);
            continue;
        }
        if (itr == _respawnAt.end())
        {
            uint32 const respawn = DeathTimer(player);
            _respawnAt[guid] = now + respawn;
            RiftProgression::Tell(player, Trinity::StringFormat("RIFTDEATH:{}", respawn));
            continue;
        }
        if (now < itr->second)
            continue;
        _respawnAt.erase(itr);
        RemovePlayerFromResurrectQueue(player->GetGUID());
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones(false);
        Position const at = FountainPos(GetTeamIndexByTeamId(GetPlayerTeam(player->GetGUID())));
        player->NearTeleportTo(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), at.GetOrientation());
    }
}

// League's bounty (Classic): 300 a kill; a victim on a spree is worth more - 150
// at 3 kills, 150 more each kill after, to 1000 in all; one that keeps dying is
// worth less - 15% less each death in a row past the first, to a fifth.
uint32 BattlegroundSRF::KillBounty(uint32 victimStreak, uint32 victimDeathStreak) const
{
    if (victimStreak >= 3)
        return std::min<uint32>(SRF_GOLD_KILL + 150 * (victimStreak - 2), SRF_GOLD_BOUNTY_MAX);
    if (victimDeathStreak >= 1)
        return uint32(float(SRF_GOLD_KILL) * std::max(0.2f, 1.0f - 0.15f * float(victimDeathStreak)));
    return SRF_GOLD_KILL;
}

void BattlegroundSRF::Announce(std::string const& line)
{
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        for (Player* player : TeamPlayers(team))
            RiftProgression::Tell(player, line);
}

// A structure, a monster, the waves, the end: RIFTANN:<kind>:::<team>:<arg>.
void BattlegroundSRF::AnnounceEvent(char const* kind, TeamId team, uint32 arg)
{
    Announce(Trinity::StringFormat("RIFTANN:{}:::{}:{}::", kind, uint32(team), arg));
}

// Ace: every champion of a team is dead.
void BattlegroundSRF::CheckAce(TeamId deadTeam)
{
    std::vector<Player*> const team = TeamPlayers(deadTeam);
    if (team.size() < 2)
        return;
    for (Player* player : team)
        if (player->IsAlive())
            return;
    AnnounceEvent("ACE", OtherTeam(deadTeam));
}

// The Tab scoreboard: one line per champion to everyone, held items in slot order.
void BattlegroundSRF::SendScores()
{
    std::vector<std::string> lines;
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        for (Player* player : TeamPlayers(team))
        {
            ChampionStats const& st = _stats[player->GetGUID()];
            std::string items;
            for (uint32 entry : RiftProgression::HeldShopItems(player))
                items += (items.empty() ? "" : ",") + std::to_string(entry);
            lines.push_back(Trinity::StringFormat("RIFTSCORE:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}", player->GetGUID().GetCounter(),
                GetPlayerDisplayName(player), uint32(team), uint32(player->GetClass()), uint32(RiftProgression::GetLeagueLevel(player)),
                st.Kills, st.Deaths, st.Assists, st.Minions, items));
        }
    for (std::string const& line : lines)
        Announce(line);
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

// League's turret priority: melee minions, then casters and champions' pets,
// then champions, closest first within each; the target is held while it stays
// in range.
Unit* BattlegroundSRF::PickTurretTarget(Creature* turret, TeamId team) const
{
    Map* map = turret->GetMap();
    Unit* best = nullptr;
    uint8 bestTier = 0xFF;
    float bestDist = std::numeric_limits<float>::max();
    auto consider = [&](Unit* unit, uint8 tier)
    {
        if (!unit || !unit->IsAlive() || !turret->IsValidAttackTarget(unit))
            return;
        float const dist = turret->GetExactDist2d(unit);
        if (dist > SRF_TURRET_RANGE)
            return;
        if (tier < bestTier || (tier == bestTier && dist < bestDist))
        {
            best = unit;
            bestTier = tier;
            bestDist = dist;
        }
    };

    for (Minion const& m : _minions)
        if (m.Team != team)
            consider(map->GetCreature(m.Guid), m.Caster ? 2 : 1);

    for (auto const& [guid, bgPlayer] : GetPlayers())
        if (bgPlayer.Team == TeamValue(OtherTeam(team)))
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                if (player->GetMap() == map)
                {
                    consider(player, 3);
                    consider(EnemyPetOf(player), 2);
                }
    return best;
}

// League's call for help: an enemy champion damaging an allied champion within
// 1400 of the turret draws its fire.
Unit* BattlegroundSRF::TurretCallForHelp(Creature* turret, TeamId team) const
{
    Map* map = turret->GetMap();
    for (auto const& [guid, bgPlayer] : GetPlayers())
    {
        if (bgPlayer.Team != TeamValue(OtherTeam(team)))
            continue;
        Player* enemy = ObjectAccessor::FindPlayer(guid);
        if (!enemy || enemy->GetMap() != map || !enemy->IsAlive() || turret->GetExactDist2d(enemy) > SRF_TURRET_RANGE
            || !turret->IsValidAttackTarget(enemy))
            continue;
        Unit* victim = AttackOf(enemy);
        if (victim && victim->GetTypeId() == TYPEID_PLAYER && IsAlly(victim, team) && turret->GetExactDist2d(victim) <= SRF_TURRET_CALL_RANGE)
            return enemy;
    }
    return nullptr;
}

// A turret fires every BG_SRF_TURRET_TICK, and never before its last bolt has
// landed: at 50 yd a bolt flies longer than that, so it landed just as the next
// one left - a champion died "before the shot came out" (owner, 2026-10-21).
void BattlegroundSRF::UpdateTurrets(uint32 elapsed)
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
        ts.SinceChampionShot += elapsed;
        if (ts.SinceChampionShot >= SRF_TURRET_WARMUP_RESET)
            ts.WarmStacks = 0;
        if (ts.Cooldown > elapsed)
        {
            ts.Cooldown -= elapsed;
            continue;
        }
        ts.Cooldown = 0;
        Unit* target = !ts.Target.IsEmpty() ? ObjectAccessor::GetUnit(*turret, ts.Target) : nullptr;
        if (!target || !target->IsAlive() || target->GetMap() != map
            || turret->GetExactDist2d(target) > SRF_TURRET_RANGE || !turret->IsValidAttackTarget(target))
        {
            target = PickTurretTarget(turret, team);
            ts.Target = target ? target->GetGUID() : ObjectGuid::Empty;
        }
        // Not already on a champion: a champion hitting an allied one takes its fire.
        if (!target || target->GetTypeId() != TYPEID_PLAYER)
            if (Unit* caller = TurretCallForHelp(turret, team))
            {
                target = caller;
                ts.Target = caller->GetGUID();
            }

        if (!target)
            continue;

        float share = SRF_TURRET_MELEE_SHARE;
        if (target->GetTypeId() == TYPEID_PLAYER)
        {
            share = SRF_TURRET_PLAYER_SHARE * (1.0f + SRF_TURRET_WARMUP_STEP * float(ts.WarmStacks));
            ts.WarmStacks = std::min<uint8>(ts.WarmStacks + 1, SRF_TURRET_WARMUP_MAX);
            ts.SinceChampionShot = 0;
        }
        else if (IsChampionPet(target))
            share = SRF_TURRET_PLAYER_SHARE;
        else
            for (Minion const& m : _minions)
                if (m.Guid == target->GetGUID())
                {
                    share = m.Caster ? SRF_TURRET_CASTER_SHARE : SRF_TURRET_MELEE_SHARE;
                    break;
                }
        Creature* eye = TurretEye(slot, turret);
        Unit* source = eye ? static_cast<Unit*>(eye) : turret;
        uint32 const shot = team == TEAM_ALLIANCE ? BG_SRF_SPELL_ORDER_TURRET_SHOT : BG_SRF_SPELL_CHAOS_TURRET_SHOT;
        FireBolt(source, target, shot, std::max<int32>(1, int32(target->GetMaxHealth() * share * SRF_TURRET_DAMAGE_SCALE)));
        uint32 flight = 0;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(shot))
            if (info->Speed > 0.0f)
                flight = uint32(source->GetDistance(target) / info->Speed * float(IN_MILLISECONDS));
        ts.Cooldown = std::max<uint32>(BG_SRF_TURRET_TICK, flight + SRF_TURRET_SHOT_GAP);
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
    // Four levels over its turret, kept there as the match levels: a spell from a
    // lower-level caster misses and is partly resisted the further the victim is
    // above it (Unit::MagicSpellHitResult, level resistance), so an eye stuck at
    // the level it was summoned at lost most of its shots on the minions and
    // champions that had levelled past it. Four over makes the hit chance 100%
    // and leaves no level resistance - League's turrets never miss.
    uint8 const eyeLevel = uint8(std::min<uint32>(turret->GetLevel() + 4, STRONG_MAX_LEVEL));
    TurretState& ts = _turrets[slot];
    if (Creature* eye = !ts.Eye.IsEmpty() ? turret->GetMap()->GetCreature(ts.Eye) : nullptr)
        if (eye->IsAlive())
        {
            if (eye->GetLevel() != eyeLevel)
                eye->SetLevel(eyeLevel);
            return eye;
        }

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
    eye->SetLevel(eyeLevel);
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
    // Each bot's orders, so a bot standing idle can be read off the log.
    for (auto& [guid, st] : _bots)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || bot->GetMapId() != GetMapId())
            continue;
        Unit* ordered = !st.Orders.Target.IsEmpty() ? ObjectAccessor::GetUnit(*bot, st.Orders.Target) : nullptr;
        Unit* victim = bot->GetVictim();
        TC_LOG_INFO("bg.battleground", "Summoner's Rift {} bot {} (class {}): lane {}, {}{}, {:.0f} yd from post, ordered {}, victim {}, hp {:.0f}%, casting {}.",
            GetInstanceID(), bot->GetName(), uint32(bot->GetClass()), uint32(st.Lane),
            st.HasOrders ? "orders" : "NO ORDERS", st.Orders.Retreat ? " (retreating)" : "",
            bot->GetExactDist2d(&st.Orders.Post), ordered ? ordered->GetName() : std::string("nothing"),
            victim ? victim->GetName() : std::string("nothing"), bot->GetHealthPct(),
            bot->HasUnitState(UNIT_STATE_CASTING) ? "yes" : "no");
    }
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
        if (SRF_CAMPS[camp].kind != SRF_CAMP_BARON)
            UpdateLeash(camp, elapsed);
        if (victim && SRF_CAMPS[camp].kind != SRF_CAMP_SCUTTLE)
            for (Creature* creature : alive)
                if (!creature->IsInCombat() && creature->IsValidAttackTarget(victim))
                    creature->AI()->AttackStart(victim);
    }
}

// League's leash (wiki "Monster"): a camp has patience, shared by its monsters.
// It drains while any of them is outside its leash or cannot find its target -
// faster the further out, and faster still while nobody is hitting it - and a
// hit inside the leash gives some back and holds the drain for 1.5 seconds. Out
// of patience, the camp resets: home, healed, deaf to attackers until there
// (EnterEvadeMode). Back home, patience refills over a couple of seconds.
// Baron is stationary and has no leash.
float constexpr SRF_LEASH_RANGE         = Yards(800.0f);    // from each monster's own spawn
float constexpr SRF_PATIENCE_DRAIN      = 20.0f;            // a second, just outside the leash
float constexpr SRF_PATIENCE_PER_YARD   = 4.0f;             // more a second per yard further out
float constexpr SRF_PATIENCE_UNHIT      = 1.5f;             // x drain while nobody hits it
float constexpr SRF_PATIENCE_HIT_REFILL = 25.0f;
uint32 constexpr SRF_PATIENCE_HIT_PAUSE = 1500;
float constexpr SRF_PATIENCE_RECOVER    = 50.0f;            // a second, out of combat

void BattlegroundSRF::UpdateLeash(uint8 camp, uint32 elapsed)
{
    Map* map = FindBgMap();
    if (!map)
        return;
    CampState& cs = _camps[camp];
    bool fighting = false;
    bool hitInside = false;
    bool hit = false;
    float worstOut = 0.0f;     // yards beyond the leash; > 0 drains
    std::vector<Creature*> engaged;
    std::vector<Position> homes;
    std::vector<bool> hits;
    for (uint8 i = 0; i < BG_SRF_JUNGLE_MONSTERS; ++i)
    {
        SrfMonster const& m = SRF_MONSTERS[i];
        if (m.camp != camp)
            continue;
        Creature* creature = map->GetCreature(BgCreatures[BG_SRF_CREATURE_JUNGLE_FIRST + i]);
        if (!creature || !creature->IsAlive())
            continue;
        uint32& last = cs.LastHealth[creature->GetGUID()];
        bool const wasHit = last && creature->GetHealth() < last;
        last = creature->GetHealth();
        if (!creature->IsInCombat() || creature->IsInEvadeMode())
            continue;
        fighting = true;
        engaged.push_back(creature);
        homes.push_back(ToWorld(m.x, m.y, m.z, m.o));
        hits.push_back(wasHit);
    }

    // League's aggro: no threat. A monster stays on its target, however hard
    // someone else hits it, until that target is lost - dead, unseen (the
    // brush), or out past the leash. Then it turns on the nearest champion
    // still inside the leash that has fought the camp (the camp fights as one,
    // so a hit on any of it counts), and only with nobody left does the
    // patience below run down. TrinityCore's threat would have stayed on the
    // champion who ran.
    std::vector<Unit*> fought;
    for (Creature* creature : engaged)
        for (ThreatReference const* ref : creature->GetThreatManager().GetUnsortedThreatList())
            if (Unit* unit = ref->GetVictim())
                if (std::find(fought.begin(), fought.end(), unit) == fought.end())
                    fought.push_back(unit);

    for (size_t n = 0; n < engaged.size(); ++n)
    {
        Creature* creature = engaged[n];
        Position const& home = homes[n];
        auto const lost = [&](Unit* unit)
        {
            return !unit || !unit->IsAlive() || !creature->CanSeeOrDetect(unit)
                || !creature->IsValidAttackTarget(unit) || unit->GetExactDist2d(&home) > SRF_LEASH_RANGE + 3.0f;
        };
        Unit* victim = creature->GetVictim();
        if (lost(victim))
        {
            Unit* next = nullptr;
            for (Unit* unit : fought)
                if (unit != victim && !lost(unit) && (!next || creature->GetExactDist2d(unit) < creature->GetExactDist2d(next)))
                    next = unit;
            if (next)
            {
                creature->GetThreatManager().AddThreat(next, 0.0f, nullptr, true, true);
                creature->GetThreatManager().FixateTarget(next);
                if (CreatureAI* ai = creature->AI())
                    ai->AttackStart(next);
                victim = next;
            }
        }
        else if (creature->GetThreatManager().GetFixateTarget() != victim)
            creature->GetThreatManager().FixateTarget(victim);

        float out = creature->GetExactDist2d(&home) - SRF_LEASH_RANGE;
        bool const wasHit = hits[n];
        // a target it cannot find (gone, hidden in the brush, out of reach of the leash) counts as outside
        if (lost(victim))
            out = std::max(out, 0.5f);
        worstOut = std::max(worstOut, out);
        if (wasHit)
        {
            hit = true;
            if (out <= 0.0f)
                hitInside = true;
        }
    }

    float const seconds = float(elapsed) / 1000.0f;
    if (!fighting)
    {
        cs.Patience = std::min(100.0f, cs.Patience + SRF_PATIENCE_RECOVER * seconds);
        cs.PauseMs = 0;
        return;
    }
    if (hitInside)
    {
        cs.Patience = std::min(100.0f, cs.Patience + SRF_PATIENCE_HIT_REFILL);
        cs.PauseMs = SRF_PATIENCE_HIT_PAUSE;
    }
    if (cs.PauseMs)
        cs.PauseMs = cs.PauseMs > elapsed ? cs.PauseMs - elapsed : 0;
    else if (worstOut > 0.0f)
    {
        float rate = SRF_PATIENCE_DRAIN + SRF_PATIENCE_PER_YARD * worstOut;
        if (!hit)
            rate *= SRF_PATIENCE_UNHIT;
        cs.Patience -= rate * seconds;
    }
    if (cs.Patience > 0.0f)
        return;

    // Out of patience: the whole camp goes home.
    cs.Patience = 100.0f;
    cs.PauseMs = 0;
    for (Creature* creature : engaged)
    {
        creature->GetThreatManager().ClearFixate();
        if (CreatureAI* ai = creature->AI())
            ai->EnterEvadeMode(CreatureAI::EVADE_REASON_BOUNDARY);
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
    ++_stats[killer->GetGUID()].Minions;      // League's CS counts monsters too
    switch (SRF_CAMPS[m.camp].kind)
    {
        case SRF_CAMP_BARON:
            for (Player* player : TeamPlayers(team))
                RiftProgression::AddExperience(player, m.xp);
            break;
        case SRF_CAMP_DRAGON:
            AwardExperience(team, creature, m.xp);
            break;
        default:
            RiftProgression::AddExperience(killer, m.xp);
            break;
    }

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
            AnnounceEvent("DRAGON", team);
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
            AnnounceEvent("BARON", team);
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
    // the last scoreboard, then Victory / Defeat
    SendScores();
    AnnounceEvent("END", winner == ALLIANCE ? TEAM_ALLIANCE : winner == HORDE ? TEAM_HORDE : TEAM_NEUTRAL);
    _wavesStarted = false;
    DespawnMinions();
    Battleground::EndBattleground(winner);
}
