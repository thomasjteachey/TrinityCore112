#include "BattlegroundSRF.h"
#include "BattlegroundSRFBrush.h"

#include "BattlegroundMgr.h"
#include "Config.h"
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
#include "RiftLeague.h"
#include "RiftProgression.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace
{
// The Rift's size. League units become yards at RiftLeague::MAP_SCALE / 50, the same mapping
// the map, its terrain and navmesh were converted with (wow/import/rift/scripts/
// rift_scale.py): change one, change both. Everything below is
// League's own numbers - placements in League units, ranges as League has them - so
// the battleground follows the map whatever its size. The League models were
// converted at 1.5; creature_template.scale and the shells' gameobject_template.size
// shrink them to the map (0.75 at 1.125).
float constexpr SRF_YARDS_PER_UNIT = RiftLeague::YARDS_PER_UNIT;
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
// The buff camps, the Dragon and Baron Nashor on League Season 3's clock (the
// mid-2013 wiki infoboxes): 1:55 and 5:00, 2:30 and 6:00, 15:00 and 7:00.
uint32 constexpr SRF_BUFF_CAMP_START = 115 * IN_MILLISECONDS;
uint32 constexpr SRF_BUFF_CAMP_RESPAWN = 5 * MINUTE * IN_MILLISECONDS;

SrfCamp constexpr SRF_CAMPS[BG_SRF_JUNGLE_CAMPS] =
{
    // Order's side of the jungle (0-5), then Chaos's (6-11)
    { SRF_CAMP_BLUE,    SRF_BUFF_CAMP_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_WOLVES,  SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RAPTORS, SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RED,     SRF_BUFF_CAMP_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_KRUGS,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_GROMP,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_BLUE,    SRF_BUFF_CAMP_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_WOLVES,  SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RAPTORS, SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_RED,     SRF_BUFF_CAMP_START, SRF_BUFF_CAMP_RESPAWN },
    { SRF_CAMP_KRUGS,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    { SRF_CAMP_GROMP,   SRF_JUNGLE_START, SRF_SMALL_CAMP_RESPAWN },
    // the river
    { SRF_CAMP_SCUTTLE, 210 * IN_MILLISECONDS, 150 * IN_MILLISECONDS },
    { SRF_CAMP_SCUTTLE, 210 * IN_MILLISECONDS, 150 * IN_MILLISECONDS },
    { SRF_CAMP_DRAGON,  150 * IN_MILLISECONDS, 6 * MINUTE * IN_MILLISECONDS },
    { SRF_CAMP_BARON,   15 * MINUTE * IN_MILLISECONDS, 7 * MINUTE * IN_MILLISECONDS }
};

struct SrfMonster
{
    uint8 camp;
    uint32 entry;
    float x, y, z;   // League units
    float o;         // WoW facing: the MapLocator's local +X (its matrix's first row) - League's
                     // forward for these; the third row put every monster 90 deg off (the owner
                     // measured the Dragon at 5.503074, 2026-10-22)
    uint32 gold;     // League gold, to the killer
    float xp;        // League experience, shared like a minion's
};

// League's MapLocators (OrderBlueBuff1, OrderWolves1-3, OrderVultures1-6, ...,
// DragonEelCamp, BaronEelCamp, Dragon, Baron1), in BgCreatures slot order.
SrfMonster constexpr SRF_MONSTERS[BG_SRF_JUNGLE_MONSTERS] =
{
    {  0, BG_SRF_ENTRY_BLUE_SENTINEL,      3821.5f,  51.9f,  7901.1f, 3.9270f, 100, 120.0f },
    {  1, BG_SRF_ENTRY_GREATER_MURK_WOLF,  3780.6f,  45.0f,  6444.0f, 2.3562f,  55,  55.0f },
    {  1, BG_SRF_ENTRY_MURK_WOLF,          3980.6f,  45.0f,  6444.0f, 2.3562f,  16,  16.0f },
    {  1, BG_SRF_ENTRY_MURK_WOLF,          3730.6f,  45.0f,  6594.0f, 2.3562f,  16,  16.0f },
    {  2, BG_SRF_ENTRY_CRIMSON_RAPTOR,     6706.7f,  50.3f,  5521.0f, 0.0000f,  35,  30.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             6929.6f,  50.3f,  5647.9f, 1.5708f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             7060.5f,  50.3f,  5499.3f, 3.1416f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             6962.7f,  51.1f,  5354.4f, 0.2492f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             6852.2f,  48.1f,  5227.1f, 0.7854f,   9,   9.0f },
    {  2, BG_SRF_ENTRY_RAPTOR,             7106.2f,  48.1f,  5266.1f, 2.3562f,   9,   9.0f },
    {  3, BG_SRF_ENTRY_RED_BRAMBLEBACK,    7765.2f,  50.4f,  4020.2f, 2.3562f, 100, 120.0f },
    {  4, BG_SRF_ENTRY_ANCIENT_KRUG,       8482.5f,  50.7f,  2705.9f, 1.5708f,  60,  60.0f },
    {  4, BG_SRF_ENTRY_KRUG,               8275.5f,  50.7f,  2688.9f, 1.5708f,  40,  40.0f },
    {  5, BG_SRF_ENTRY_GROMP,              2110.6f,  54.0f,  8451.0f, 5.8224f,  80,  90.0f },
    {  6, BG_SRF_ENTRY_BLUE_SENTINEL,     11031.7f,  51.7f,  6990.8f, 0.7854f, 100, 120.0f },
    {  7, BG_SRF_ENTRY_GREATER_MURK_WOLF, 11008.2f,  51.2f,  8387.4f, 5.4978f,  55,  55.0f },
    {  7, BG_SRF_ENTRY_MURK_WOLF,         11058.2f,  51.2f,  8217.4f, 5.4978f,  16,  16.0f },
    {  7, BG_SRF_ENTRY_MURK_WOLF,         10808.2f,  51.2f,  8387.4f, 5.4978f,  16,  16.0f },
    {  8, BG_SRF_ENTRY_CRIMSON_RAPTOR,     7987.0f,  48.8f,  9471.4f, 3.1416f,  35,  30.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7887.0f,  48.8f,  9312.4f, 4.7124f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7757.0f,  48.8f,  9451.4f, 0.0000f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7854.4f,  52.2f,  9610.5f, 3.3824f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7997.2f,  52.1f,  9772.1f, 3.9270f,   9,   9.0f },
    {  8, BG_SRF_ENTRY_RAPTOR,             7724.2f,  52.1f,  9724.1f, 5.4978f,   9,   9.0f },
    {  9, BG_SRF_ENTRY_RED_BRAMBLEBACK,    7101.9f,  51.3f, 10900.5f, 5.4978f, 100, 120.0f },
    { 10, BG_SRF_ENTRY_ANCIENT_KRUG,       6317.1f,  47.6f, 12146.5f, 4.7124f,  60,  60.0f },
    { 10, BG_SRF_ENTRY_KRUG,               6547.1f,  47.6f, 12156.5f, 4.7124f,  40,  40.0f },
    { 11, BG_SRF_ENTRY_GROMP,             12703.6f,  54.0f,  6444.0f, 2.6792f,  80,  90.0f },
    { 12, BG_SRF_ENTRY_RIFT_SCUTTLER,     10500.0f, -62.0f,  5170.0f, 3.9270f,  55, 100.0f },
    { 13, BG_SRF_ENTRY_RIFT_SCUTTLER,      4400.0f, -62.0f,  9600.0f, 0.7854f,  55, 100.0f },
    // Season 3: the Dragon 190 gold to every player of the team and 25 more to
    // its killer, 400 experience shared; Baron 300 gold and 900 experience to
    // every player of the team, nothing more to its killer
    { 14, BG_SRF_ENTRY_DRAGON,             9866.1f, -69.8f,  4414.0f, 5.503074f,  25, 400.0f },
    { 15, BG_SRF_ENTRY_BARON_NASHOR,       5007.1f,  24.4f, 10471.4f, 2.3562f,   0, 900.0f }
};

// League Season 3's camp buffs (the mid-2013 wiki): Crest of the Ancient Golem
// and Blessing of the Lizard Elder last 2:30 and pass to the champion credited
// with killing the holder; Exalted with Baron Nashor lasts 4:00, goes to the
// living of the team and is lost on death - 40 attack damage and ability power,
// 35 before 35:00. What they do is RiftProgression's (SetChampionCarriers,
// CooldownReduction, OnBasicAttackDamage). The Dragon gave no buff then.
int32 constexpr SRF_BUFF_DURATION         = 150 * IN_MILLISECONDS;   // Blue and Red
int32 constexpr SRF_BARON_BUFF_DURATION   = 4 * MINUTE * IN_MILLISECONDS;
int32 constexpr SRF_BARON_POWER           = 40;
int32 constexpr SRF_BARON_POWER_EARLY     = 35;
float constexpr SRF_BARON_EARLY_MINUTES   = 35.0f;
uint32 constexpr SRF_GOLD_BARON_TEAM      = 300;     // to every player of the team
uint32 constexpr SRF_GOLD_DRAGON_TEAM     = 190;     // likewise
// Long enough that the core never brings a corpse back on its own: the
// battleground respawns its camps and inhibitors itself. A fallen turret came
// back five minutes after it was despawned (the core's default delay) - standing,
// at full health, with its slot already counted as destroyed.
uint32 constexpr SRF_CORE_RESPAWN         = DAY;

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

// A turret reaches 45 yards - past every class's own range (League's 750 units
// would be 16.9 here, inside a caster's 30 or a hunter's 35, so a ranged champion
// could siege it untouched; owner, 2026-10-20: "at least 50 yards"). Yards, not
// League units: it does not follow the map's scale (owner, 2026-10-25, shrinking the
// map: "ensure the tower range still stays at 45 yards"). Both shot spells carry
// it: Ice Lance 43427 is unlimited, Fireball 22088 100 yards. Measured centre to
// centre (GetExactDist2d), so the turret's own size does not change it.
float constexpr SRF_TURRET_RANGE        = 45.0f;   // owner, 2026-10-22 (50 before)
float constexpr SRF_MINION_AGGRO_RANGE  = Yards(400.0f);
// With no enemy unit near, a minion takes on an enemy structure in a turret's
// range of it: a wave walking down a lane stops at the first turret standing.
float constexpr SRF_MINION_STRUCTURE_RANGE = Yards(750.0f);
// Further than this from its target, a minion lets it go and takes the closest again.
float constexpr SRF_MINION_LEASH        = Yards(1100.0f);
uint32 constexpr SRF_REPORT_TICK        = 30 * IN_MILLISECONDS;
float constexpr SRF_FOUNTAIN_HEAL_RANGE = Yards(600.0f);
// League's brush (BattlegroundSRFBrush.h): attacking from it reveals for a moment.
uint32 constexpr SRF_BRUSH_TICK         = 250;      // idle combat and the free-turret cache (the brush: every update)
uint32 constexpr SRF_BRUSH_REVEAL       = 1500;
uint32 constexpr SRF_BRUSH_PROJECT_MS   = 500;      // a running client's heartbeat interval: no further than that
// Centurion spell 94142 "Brush": the buff a hidden champion wears, so they can see it.
uint32 constexpr SRF_SPELL_BRUSH        = 94142;
// Centurion spell 94153 "Summoner's Focus": every champion's match aura, +5% hit
// and +3% spell hit, a buff anyone can see. Its third effect shortens the
// champion's ranged class spells by 30% (spell_rift_focus, RiftProgression::RangeMod).
uint32 constexpr SRF_SPELL_FOCUS        = 94153;
uint32 constexpr SRF_ITEM_SYNC_MS       = 500;
uint32 constexpr SRF_SCORE_TICK         = 2000;    // the scoreboard feed (RIFTSCORE)
uint32 constexpr SRF_MAP_TICK           = 1000;    // the map feed (RIFTSTR, RIFTSEEN)
// League's vision, for the map: how far a champion, a minion and a standing
// turret see (1350 / 1200 / 1350 League units, about 30 / 27 / 30 yd on the 1.125 map).
float constexpr SRF_VISION_CHAMPION    = 30.0f;
float constexpr SRF_VISION_MINION      = 25.0f;
float constexpr SRF_VISION_TURRET      = 30.0f;
uint32 constexpr SRF_ASSIST_WINDOW_MS   = 10000;   // League: damage or an ability in the victim's last 10 s
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
    switch (creature->GetEntry())
    {
        case BG_SRF_ENTRY_ORDER_MELEE: case BG_SRF_ENTRY_ORDER_CASTER: case BG_SRF_ENTRY_ORDER_SIEGE: case BG_SRF_ENTRY_ORDER_SUPER:
            return TEAM_ALLIANCE;
        case BG_SRF_ENTRY_CHAOS_MELEE: case BG_SRF_ENTRY_CHAOS_CASTER: case BG_SRF_ENTRY_CHAOS_SIEGE: case BG_SRF_ENTRY_CHAOS_SUPER:
            return TEAM_HORDE;
        default:
            return -1;
    }
}

// The base: the fountain and the shop beside it.
float constexpr SRF_BASE_RANGE          = Yards(1000.0f);
// The shops: League's orderShop01 / chaosShop01 stand outside the walkable area
// (under the boundary embankment), so each is drawn in toward its fountain to the
// nearest spot 4 yards clear of the navigation grid's walls. Facing the middle.
SrfPoint constexpr SRF_SHOP_AT[PVP_TEAMS_COUNT] = { { 322.9f, 171.0f, 682.7f }, { 14151.8f, 179.0f, 14454.3f } };
float constexpr SRF_SHOP_FACING[PVP_TEAMS_COUNT] = { 5.44f, 2.42f };
float constexpr SRF_ARMSMASTER_OFFSET = 4.0f;   // yards to the shopkeeper's left
float constexpr SRF_FOUNTAIN_BURN_RANGE = Yards(700.0f);
// Turret shots are League's (Season 3): the turret's attack damage for its tier
// and the game time (world rift_league_unit), through 30% of the target's armour.
// On champions, League's Warming Up and Heated Up: 105% on the first shot, +25% a
// shot in a row to 225%; a turret that turns on another champion takes it from at
// most 175%, a shot at anything else resets it, and so do 5 s without a champion.
std::array<float, 6> constexpr SRF_TURRET_HEAT = { 1.05f, 1.25f, 1.50f, 1.75f, 2.00f, 2.25f };
uint8 constexpr SRF_TURRET_SWITCH_HEAT = 3;
uint32 constexpr SRF_TURRET_WARMUP_RESET = 5 * IN_MILLISECONDS;
// The pause between one bolt landing and the next leaving the turret.
uint32 constexpr SRF_TURRET_SHOT_GAP = 400;
// A champion damaging an allied champion within 1400 of a turret pulls its fire.
float constexpr SRF_TURRET_CALL_RANGE   = std::max(Yards(1400.0f), SRF_TURRET_RANGE);
// Minions: League acquires within 500; an allied champion under attack calls
// for help from up to 1000 away.
float constexpr SRF_MINION_ACQUIRE_RANGE = Yards(500.0f);
float constexpr SRF_MINION_CALL_RANGE    = Yards(1000.0f);
// Caster minions: League's 550 range, siege minions 450; their attack speed League's (rift_league_unit).
float constexpr SRF_CASTER_RANGE        = Yards(550.0f);
float constexpr SRF_SIEGE_RANGE         = Yards(450.0f);
// League Season 3's Dragon (the mid-2013 wiki): a ranged attack at 350, whose
// burn is 60 true damage over 4 sec (4 ticks) and 20% less attack speed.
float constexpr SRF_DRAGON_RANGE        = Yards(350.0f);
float constexpr SRF_DRAGON_BURN         = 60.0f;            // League's, over the burn's 4 ticks
float constexpr SRF_DRAGON_FIRE_SPEED   = 36.0f;            // yd/s: the Fireball missile (94144's)
// League Season 3's Baron Nashor (the wiki, mid-2013): ranged, 955; Wrath of the Ancients
// 70 magic a bolt; Voracious Corrosion is its physical attack (rift_league_unit's 460);
// Sweeping Blow knocks every champion 600 away; Propelled is Cho'Gath's Rupture (175 round).
// Who Sweeping Blow reaches and the puddle's second are this realm's numbers: the wiki has none.
float constexpr SRF_BARON_RANGE          = Yards(955.0f);
float constexpr SRF_BARON_WRATH          = 70.0f;
float constexpr SRF_BARON_SWEEP_REACH    = Yards(350.0f);   // edge to edge
float constexpr SRF_BARON_SWEEP_DISTANCE = Yards(600.0f);   // from Baron's edge, where they land
float constexpr SRF_BARON_PUDDLE_RADIUS  = Yards(175.0f);
uint32 constexpr SRF_BARON_PUDDLE_MS     = 1000;
float constexpr SRF_KNOCK_GRAVITY        = 19.2911f;         // the client's (Movement::gravity)
float constexpr SRF_SWEEP_SPEED_Z        = 7.0f;             // a 0.73 sec flight
float constexpr SRF_PROPEL_SPEED_Z       = 9.65f;            // a second in the air
// League's minion kinds (Season 3). Waves are three melee and three casters, a
// siege minion every third wave until 35:00 and every second after; a lane whose
// enemy inhibitor is down sends a super minion instead of the siege minion every
// wave, two when all three are down.
enum SrfMinionKind : uint8 { SRF_MINION_MELEE, SRF_MINION_CASTER, SRF_MINION_SIEGE, SRF_MINION_SUPER };
char const* const SRF_MINION_KIND[] = { "melee", "caster", "siege", "super" };
float constexpr SRF_SIEGE_EVERY_SECOND_WAVE_FROM = 35.0f;   // minutes of League's clock
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
float constexpr SRF_XP_SIEGE_MINION     = 92.0f;
float constexpr SRF_XP_SUPER_MINION     = 97.0f;
float constexpr SRF_XP_KILL_SOLO_MIN    = 42.0f;
float constexpr SRF_XP_KILL_SOLO_MAX    = 990.0f;
float constexpr SRF_XP_KILL_SHARED_MIN  = 27.97f;
float constexpr SRF_XP_KILL_SHARED_MAX  = 891.0f;
uint32 constexpr SRF_GOLD_MELEE_MINION  = 20;
uint32 constexpr SRF_GOLD_CASTER_MINION = 15;
uint32 constexpr SRF_GOLD_SIEGE_MINION  = 40;
uint32 constexpr SRF_GOLD_SUPER_MINION  = 27;      // the 2013 infobox's
float const SRF_MINION_XP[] = { SRF_XP_MELEE_MINION, SRF_XP_CASTER_MINION, SRF_XP_SIEGE_MINION, SRF_XP_SUPER_MINION };
uint32 const SRF_MINION_GOLD[] = { SRF_GOLD_MELEE_MINION, SRF_GOLD_CASTER_MINION, SRF_GOLD_SIEGE_MINION, SRF_GOLD_SUPER_MINION };
uint32 constexpr SRF_GOLD_KILL          = 300;
uint32 constexpr SRF_GOLD_FIRST_BLOOD   = 100;     // on top of the kill
uint32 constexpr SRF_GOLD_BOUNTY_MAX    = 1000;    // a shutdown at most
uint32 constexpr SRF_GOLD_STRUCTURE     = 150;     // to every player of the side that destroys it
uint32 constexpr SRF_GOLD_PASSIVE       = 8;       // every SRF_PASSIVE_GOLD_TICK: League's 16 per 10 s (Season 3)
uint32 constexpr SRF_PASSIVE_GOLD_TICK  = 5 * IN_MILLISECONDS;
// Units play League's numbers for their kind and League's game time (RiftLeague,
// world rift_league_unit), restated as the match's scale and League's clock move:
// League's minions grow every 3 minutes, its turrets by the minute, a camp by the
// time it came up. They take the players' level for WoW's own level rules.
uint32 constexpr SRF_STRUCTURE_LEVEL_TICK  = 30 * IN_MILLISECONDS;
// League's clock: its first wave comes at 1:30.
uint32 constexpr SRF_LEAGUE_FIRST_WAVE     = 90 * IN_MILLISECONDS;
uint8 constexpr SRF_UNIT_BASE_LEVEL = 10;          // the match's starting level (RiftProgression)
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
// The spacing follows the map's scale, as the minions do (2.5 and 3 yards on the 1.5 map).
float constexpr SRF_WAVE_FILE_SPACING   = Yards(83.33f);    // between files, across the lane
float constexpr SRF_WAVE_RANK_SPACING   = Yards(100.0f);    // between ranks, along it
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
    _campBuffEnds.clear();
    _jungleTimer = 0;
    _reportTimer = 0;
    _boltsCast = 0;
    _boltsRefused = 0;
    _lastRefusal = 0;
    _unstuckTargets = 0;
    _unstuckPlaced = 0;
    _matchClock = 0;
    _respawnAt.clear();
    _championPet.clear();
    _petRevive.clear();
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
    // back to the match it left picks up where it was. A relog adds the player
    // from inside Player::LoadFromDB, before its spells, talents and gear have
    // loaded: that one is RiftProgression::OnLogin's, once the character is whole
    // (it was dressed in a field kit into slots its own gear then loaded into, and
    // built a match from an empty spellbook - Forcskin, 2026-10-22).
    if (!player->GetSession() || !player->GetSession()->PlayerLoading())
        RiftProgression::Enter(player);
}

void BattlegroundSRF::RemovePlayer(Player* player, ObjectGuid guid, uint32 /*team*/)
{
    _brush.erase(guid);
    _bots.erase(guid);
    _championPet.erase(guid);
    _petRevive.erase(guid);
    if (player)
    {
        player->RemoveAurasDueToSpell(SRF_SPELL_BRUSH);
        player->RemoveAurasDueToSpell(SRF_SPELL_FOCUS);
        if (!player->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            player->RemoveByteFlag(PLAYER_FIELD_BYTES2, PLAYER_FIELD_BYTES_2_OFFSET_AURA_VISION, PLAYER_FIELD_BYTE2_STEALTH);
            player->RemoveVisFlag(UNIT_VIS_FLAGS_CREEP);
        }
    }
    // No RiftProgression::Leave here. A real leave runs it from
    // Battleground::RemovePlayerAtLeave, before this. The other caller is
    // Battleground::EventPlayerLoggedOut - a logout, a crash - which hands a live
    // Player while the character stays in the match: leaving here gave the real
    // level, talents and money back and dropped the match, so a relog moments
    // later was dressed as a NEW champion, during LoadFromDB, before its spells
    // and talents had loaded - level 10, nothing to cast, and the stash written
    // over with an empty build (Forcskin, 2026-10-22). The match now stays as it
    // was: a relog picks it up (Enter's "back in the match" path), and one that
    // comes back after the match let it go is OnLogin's to restore.
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
    _campBuffEnds.clear();
    _jungleTimer = 0;
    _reportTimer = 0;
    _boltsCast = 0;
    _boltsRefused = 0;
    _lastRefusal = 0;
    _unstuckTargets = 0;
    _unstuckPlaced = 0;
    _matchClock = 0;
    _combatLevel = SRF_UNIT_BASE_LEVEL;
    _unitScale = 0.0f;
    _respawnAt.clear();
    _championPet.clear();
    _petRevive.clear();
    _ringsSentAt.clear();
    _contributions.clear();
    _stats.clear();
    _firstBlood = false;
    _scoreTimer = 0;
    _mapTimer = 0;
}

bool BattlegroundSRF::SetupBattleground()
{
    // League's numbers as the tables hold them now: an edit plays from the next match.
    RiftLeague::Load();
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

        // The armsmaster (the kit's weapons) beside the shopkeeper, facing the same way.
        float const side = pos.GetOrientation() + float(M_PI) / 2.0f;
        Position arms(pos.GetPositionX() + SRF_ARMSMASTER_OFFSET * std::cos(side), pos.GetPositionY() + SRF_ARMSMASTER_OFFSET * std::sin(side),
            pos.GetPositionZ(), pos.GetOrientation());
        if (map)
        {
            float const ground = map->GetHeight(arms.GetPositionX(), arms.GetPositionY(), arms.GetPositionZ() + 10.0f, true);
            if (ground > INVALID_HEIGHT)
                arms.Relocate(arms.GetPositionX(), arms.GetPositionY(), ground);
        }
        if (!AddCreature(team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_ARMSMASTER : BG_SRF_ENTRY_CHAOS_ARMSMASTER,
                team == TEAM_ALLIANCE ? BG_SRF_CREATURE_ARMS_ORDER : BG_SRF_CREATURE_ARMS_CHAOS, arms, team))
            TC_LOG_ERROR("bg.battleground", "BattlegroundSRF: failed to spawn the {} armsmaster.", TeamName(team));
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
    if (!AddCreature(StructureEntry(slot), slot, StructurePos(slot), SlotTeam(slot), SRF_CORE_RESPAWN))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundSRF: failed to spawn structure slot {} (entry {}).", slot, StructureEntry(slot));
        return false;
    }
    _structures[slot] = StructureState();
    _turrets[slot] = TurretState();
    ApplyLeagueStats(GetBGCreature(slot, false), StructureKind(slot), LeagueMinutes(), 1.0f, _structures[slot].Backdoor);
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
        SyncChampions(diff);
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

    _mapTimer += diff;
    if (_mapTimer >= SRF_MAP_TICK)
    {
        _mapTimer = 0;
        SendMap();
    }

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

    // The brush every update (owner, 2026-10-23: "it takes a second for it to register"
    // going in and coming out - it ran every 250 ms on positions up to half a second old).
    UpdateBrush(diff);
    _brushTimer += diff;
    if (_brushTimer >= SRF_BRUSH_TICK)
    {
        ReleaseIdleCombat();
        RefreshFreeTurrets();
        _brushTimer = 0;
    }
}

// Who stands in which brush patch, and whether that hides them. A change is
// pushed to everyone at once (UpdateObjectVisibility both ways), so a champion
// stepping into a bush vanishes from the enemies outside it - and an enemy
// stepping into the same bush sees it again.
// League's inventory works from the bags: what a sale, a drop or a destroyed item
// changed is picked up here (RiftProgression::SyncItems), at the match's scale and
// level (RiftLeague). From the moment champions are in, as League's shop: boots
// bought at the fountain before the gates opened did nothing until they did.
void BattlegroundSRF::SyncChampions(uint32 diff)
{
    _itemSyncTimer += diff;
    if (_itemSyncTimer < SRF_ITEM_SYNC_MS)
        return;
    _itemSyncTimer = 0;
    _combatLevel = AverageMatchLevel();
    float const scale = RiftLeague::Scale(AverageLeagueLevel());
    _unitScale = RiftLeague::Scale(UnitLeagueLevel());
    if (GetStatus() == STATUS_IN_PROGRESS)
        UpdateBackdoor();
    for (auto const& [guid, bgPlayer] : GetPlayers())
        if (Player* player = ObjectAccessor::FindPlayer(guid); player && player->GetMapId() == GetMapId())
        {
            RiftProgression::SetMatchScale(player, scale, _combatLevel);
            RiftProgression::SyncItems(player);
        }
}

// Where a champion is now, as near as the server can tell. A client running straight
// reports its position only with its heartbeat, twice a second, so between reports the
// last one is carried on along the way it is moving (its heading and strafe flags) at
// its speed, for as long as no new one came - half a second at most. A stop is reported
// at once, so a champion that stopped is never carried on.
static Position ProjectedPosition(Player const* player, uint32 unchangedMs)
{
    Position pos = player->GetPosition();
    uint32 const flags = player->GetUnitMovementFlags();
    if (!unchangedMs || (flags & (MOVEMENTFLAG_ROOT | MOVEMENTFLAG_FALLING)))
        return pos;
    float ahead = 0.0f, left = 0.0f;
    if (flags & MOVEMENTFLAG_FORWARD)
        ahead += 1.0f;
    if (flags & MOVEMENTFLAG_BACKWARD)
        ahead -= 1.0f;
    if (flags & MOVEMENTFLAG_STRAFE_LEFT)
        left += 1.0f;
    if (flags & MOVEMENTFLAG_STRAFE_RIGHT)
        left -= 1.0f;
    float const length = std::sqrt(ahead * ahead + left * left);
    if (length < 0.5f)
        return pos;
    UnitMoveType const type = ahead < 0.0f ? MOVE_RUN_BACK : (flags & MOVEMENTFLAG_WALKING) ? MOVE_WALK : MOVE_RUN;
    float const distance = player->GetSpeed(type) * float(std::min(unchangedMs, SRF_BRUSH_PROJECT_MS)) / 1000.0f;
    float const o = pos.GetOrientation();
    ahead /= length;
    left /= length;
    pos.Relocate(pos.GetPositionX() + (ahead * std::cos(o) - left * std::sin(o)) * distance,
        pos.GetPositionY() + (ahead * std::sin(o) + left * std::cos(o)) * distance, pos.GetPositionZ());
    return pos;
}

void BattlegroundSRF::UpdateBrush(uint32 elapsed)
{
    for (auto const& [guid, bgPlayer] : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || player->GetMapId() != GetMapId())
        {
            _brush.erase(guid);
            continue;
        }
        if (player->IsAlive() && !player->HasAura(SRF_SPELL_FOCUS))
            player->AddAura(SRF_SPELL_FOCUS, player);
        BrushState& st = _brush[guid];
        if (std::fabs(player->GetPositionX() - st.LastX) > 0.01f || std::fabs(player->GetPositionY() - st.LastY) > 0.01f)
        {
            st.LastX = player->GetPositionX();
            st.LastY = player->GetPositionY();
            st.UnchangedMs = 0;
        }
        else
            st.UnchangedMs = std::min(st.UnchangedMs + elapsed, SRF_BRUSH_PROJECT_MS);
        uint8 const patch = player->IsAlive() ? BrushPatchAt(ProjectedPosition(player, st.UnchangedMs)) : 0;
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
            continue;
        }
        // A rise of 5% or more: nothing should ever do it (a heal is refused,
        // RtsBuildingAI::HealReceived; ApplyLeagueStats keeps the share).
        StructureState& st = _structures[slot];
        uint32 const health = creature->GetHealth(), maxHealth = creature->GetMaxHealth();
        if (st.LastHealth && maxHealth && health > st.LastHealth && (health - st.LastHealth) * 20 >= maxHealth)
            TC_LOG_INFO("bg.battleground", "Summoner's Rift {}: structure slot {} ({}) health ROSE {}/{} -> {}/{}, {}attackable, in combat {}, level {}.",
                GetInstanceID(), uint32(slot), creature->GetGUID().ToString(), st.LastHealth, st.LastMaxHealth, health, maxHealth,
                creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) ? "NOT " : "", creature->IsInCombat() ? "yes" : "no", uint32(creature->GetLevel()));
        st.LastHealth = health;
        st.LastMaxHealth = maxHealth;
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

        uint32 const entries[] = {
            uint32(team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_MELEE : BG_SRF_ENTRY_CHAOS_MELEE),
            uint32(team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_CASTER : BG_SRF_ENTRY_CHAOS_CASTER),
            uint32(team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_SIEGE : BG_SRF_ENTRY_CHAOS_SIEGE),
            uint32(team == TEAM_ALLIANCE ? BG_SRF_ENTRY_ORDER_SUPER : BG_SRF_ENTRY_CHAOS_SUPER) };
        // League's wave (SrfMinionKind): supers lead, then the melee, the siege
        // minion and the casters.
        bool const siegeWave = (_wavesSpawned + 1) % (LeagueMinutes() >= SRF_SIEGE_EVERY_SECOND_WAVE_FROM ? 2 : 3) == 0;
        TeamId const enemy = team == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
        uint8 inhibitorsDown = 0;
        for (uint8 lane = 0; lane < BG_SRF_LANE_COUNT; ++lane)
            inhibitorsDown += _structures[SlotOf(enemy, lane * 4 + 3)].Destroyed ? 1 : 0;
        for (uint8 lane = 0; lane < BG_SRF_LANE_COUNT; ++lane)
        {
            std::vector<Position> const& path = LanePath(team, lane);
            if (path.size() < 2)
                continue;
            Position const gate = path[0];
            float const facing = gate.GetAbsoluteAngle(&path[1]);
            std::vector<uint8> wave;
            uint8 const supers = _structures[SlotOf(enemy, lane * 4 + 3)].Destroyed ? (inhibitorsDown == BG_SRF_LANE_COUNT ? 2 : 1) : 0;
            wave.insert(wave.end(), supers, SRF_MINION_SUPER);
            wave.insert(wave.end(), BG_SRF_MELEE_PER_WAVE, SRF_MINION_MELEE);
            if (!supers && siegeWave)
                wave.push_back(SRF_MINION_SIEGE);
            wave.insert(wave.end(), BG_SRF_CASTERS_PER_WAVE, SRF_MINION_CASTER);
            for (uint8 i = 0; i < wave.size(); ++i)
            {
                uint8 const kind = wave[i];
                // In its file across the lane and its rank behind the gate: melee
                // (slots 0-2) in front, casters (3-5) behind.
                float const back = facing + float(M_PI);
                Position const filed = InFile(path, 0, FileOffset(i));
                Position const pos(filed.GetPositionX() + std::cos(back) * RankOffset(i),
                    filed.GetPositionY() + std::sin(back) * RankOffset(i), gate.GetPositionZ() + 1.0f, facing);
                TempSummon* minion = nexus->SummonCreature(entries[kind], pos,
                    TEMPSUMMON_CORPSE_TIMED_DESPAWN, 3s);
                if (!minion)
                    continue;
                // The battleground picks every minion's target (FindMinionTarget):
                // left to aggro on sight, a wave would turn on its own deniable
                // minions, which are hostile to everyone.
                minion->SetReactState(REACT_DEFENSIVE);
                float const minutes = LeagueMinutes();
                ApplyLeagueStats(minion, SRF_MINION_KIND[kind], minutes, 1.0f, false);
                Minion m;
                m.SpawnMinutes = minutes;
                m.HealthScale = 1.0f;
                m.Guid = minion->GetGUID();
                m.Team = team;
                m.Lane = lane;
                m.Kind = kind;
                m.Caster = kind == SRF_MINION_CASTER || kind == SRF_MINION_SIEGE;
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
// A ranged champion stands back at its reach behind the wave, as League's carries
// and mages do: 5 yards behind the front put the enemy wave inside a hunter's
// minimum range, so it stepped back to shoot, was ordered back to the post, and
// stepped back again (Aerendil Trapfang's hiccup walk, 2026-10-22) until the
// terrain behind it held it. Its fighting circle grows by the same.
float constexpr SRF_BOT_BEHIND_WAVE_RANGED = 18.0f;
// Walking out to the post it is given nothing to fight beyond its own circle:
// ordered onto a minion 390 yards off, a bot re-aimed at a moving target every
// tick instead of walking the lane.
float constexpr SRF_BOT_POST_NEAR     = 2.0f * SRF_BOT_ENGAGE_RANGE;
// The current target keeps this much score over an equal one, so a bot does not
// flip between two minions every time the orders are worked out.
int32 constexpr SRF_BOT_TARGET_KEEP   = 5;
// The jungler ganks an enemy champion this close, off its turrets, that is hurt
// or already fighting one of the team; between camps it goes where a fight is.
float constexpr SRF_BOT_GANK_RANGE    = 60.0f;
float constexpr SRF_BOT_GANK_HURT_PCT = 60.0f;
float constexpr SRF_BOT_FIGHT_RANGE   = 30.0f;
// The support walks this far behind its carry, along the lane.
float constexpr SRF_BOT_SUPPORT_BEHIND = 4.0f;

namespace
{
    // Fights from range: the hunter's bow, the cloth casters' spells.
    bool FightsAtRange(Player const* bot)
    {
        switch (bot->GetClass())
        {
            case CLASS_HUNTER:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
            case CLASS_PRIEST:
                return true;
            default:
                return false;
        }
    }
}
uint32 constexpr SRF_BOT_SHOP_GOLD    = 1300;     // League gold that sends it home to shop
uint32 constexpr SRF_BOT_SHOP_WAIT_MS = 7000;     // at the fountain this long with its gold untouched: nothing to buy
uint32 constexpr SRF_BOT_SHOP_MORE    = 500;      // League gold more it needs before the next trip home after that
uint32 constexpr SRF_SPELL_RECALL     = 94120;    // League's recall (RiftProgression teaches it)

namespace
{
    // How well a champion fits each of League's positions, by what its class and
    // spec build toward (RiftProgression::BuildRole: melee, ranged, caster,
    // healer, tank) and the class's own leaning.
    //                                      top  jungle  mid  carry  support
    uint8 constexpr SRF_ROLE_FIT[5][5] = { {  8,    9,     3,    2,     1 },    // melee
                                           {  2,    4,     5,   10,     2 },    // ranged
                                           {  3,    2,    10,    4,     5 },    // caster
                                           {  2,    1,     4,    1,    10 },    // healer
                                           { 10,    8,     1,    1,     6 } };  // tank
    // A team short of five leaves the support open first, then the jungle.
    uint8 constexpr SRF_ROLE_PRIORITY[] = { 2 /*mid*/, 0 /*top*/, 3 /*carry*/, 1 /*jungle*/, 4 /*support*/ };

    int32 RoleFit(Player const* player, uint8 role)
    {
        int32 score = SRF_ROLE_FIT[std::min<uint8>(RiftProgression::BuildRole(player), 4)][role];
        switch (player->GetClass())
        {
            case CLASS_ROGUE:        if (role == 1) score += 2; break;   // the jungle
            case CLASS_DRUID:        if (role == 1) score += 1; break;
            case CLASS_WARRIOR:
            case CLASS_DEATH_KNIGHT: if (role == 0) score += 2; break;   // top
            case CLASS_PALADIN:      if (role == 0) score += 1; break;
            case CLASS_HUNTER:       if (role == 3) score += 2; break;   // the carry
            default: break;
        }
        return score;
    }

    uint8 LaneOfRole(uint8 role)
    {
        switch (role)
        {
            case 0:  return BG_SRF_LANE_TOP;
            case 1:  return BG_SRF_LANE_NONE;
            case 2:  return BG_SRF_LANE_MID;
            default: return BG_SRF_LANE_BOT;   // the carry and its support
        }
    }
}

// Every champion of a team at once, when its match has started: the hand-out of
// the positions with the best fit overall, over every way of handing them out.
// The humans are in it too - a human hunter will carry whatever it is told - so
// the bots take what the humans leave.
void BattlegroundSRF::AssignRoles(TeamId team)
{
    _rolesAssigned[team] = true;
    std::vector<Player*> members = TeamPlayers(team);
    std::sort(members.begin(), members.end(), [](Player const* a, Player const* b) { return a->GetGUID() < b->GetGUID(); });
    size_t const n = std::min<size_t>(members.size(), SRF_ROLE_COUNT);
    if (!n)
        return;
    std::vector<uint8> roles(SRF_ROLE_PRIORITY, SRF_ROLE_PRIORITY + n);
    std::sort(roles.begin(), roles.end());
    std::vector<std::array<int32, SRF_ROLE_COUNT>> fit(n);
    for (size_t i = 0; i < n; ++i)
        for (uint8 r = 0; r < SRF_ROLE_COUNT; ++r)
            fit[i][r] = RoleFit(members[i], r);
    std::vector<uint8> best = roles;
    int32 bestScore = -1;
    do
    {
        int32 score = 0;
        for (size_t i = 0; i < n; ++i)
            score += fit[i][roles[i]];
        if (score > bestScore)
        {
            bestScore = score;
            best = roles;
        }
    } while (std::next_permutation(roles.begin(), roles.end()));
    for (size_t i = 0; i < members.size(); ++i)
        _roles[members[i]->GetGUID()] = i < n ? best[i] : uint8(SRF_ROLE_MID);
}

uint8 BattlegroundSRF::RoleOf(ObjectGuid guid) const
{
    auto const itr = _roles.find(guid);
    return itr != _roles.end() ? itr->second : uint8(SRF_ROLE_MID);
}

// The team's carry, alive, for its support to walk behind.
Player* BattlegroundSRF::TeamCarry(TeamId team) const
{
    for (Player* player : TeamPlayers(team))
        if (player->IsAlive() && RoleOf(player->GetGUID()) == SRF_ROLE_CARRY)
            return player;
    return nullptr;
}

BattlegroundSRF::BotState& BattlegroundSRF::BotOf(Player* bot)
{
    BotState& st = _bots[bot->GetGUID()];
    if (st.Assigned)
        return st;
    TeamId const team = GetTeamIndexByTeamId(GetPlayerTeam(bot->GetGUID()));
    if (!_rolesAssigned[team])
        AssignRoles(team);
    // One that came in after (a seat refilled): the best of the positions free.
    if (_roles.find(bot->GetGUID()) == _roles.end())
    {
        std::array<bool, SRF_ROLE_COUNT> held = {};
        for (Player* player : TeamPlayers(team))
            if (player != bot)
                held[RoleOf(player->GetGUID())] = true;
        uint8 role = SRF_ROLE_MID;
        int32 bestFit = -1;
        for (uint8 r = 0; r < SRF_ROLE_COUNT; ++r)
            if (!held[r] && RoleFit(bot, r) > bestFit)
            {
                bestFit = RoleFit(bot, r);
                role = r;
            }
        _roles[bot->GetGUID()] = role;
    }
    st.Role = RoleOf(bot->GetGUID());
    st.Lane = LaneOfRole(st.Role);
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
    // `out` is the bot's own last orders (GetBotOrders): what it was fighting goes first
    ObjectGuid const current = out.Target;
    out = BattlegroundBotOrders();

    // Home: to heal, or to spend. A trip home that buys nothing - the build is done,
    // or nothing fits - sends it back out until it has SRF_BOT_SHOP_MORE more:
    // Tinkerton stood at the fountain with 1515 gold for the rest of a match.
    uint32 const money = bot->GetMoney();
    if (IsAtBase(bot) && money >= std::max(SRF_BOT_SHOP_GOLD * GOLD, st.ShopBar))
    {
        uint32 const now = GameTime::GetGameTimeMS();
        if (!st.ShopSince || money < st.ShopGold)
        {
            st.ShopSince = now;
            st.ShopGold = money;
        }
        else if (now - st.ShopSince >= SRF_BOT_SHOP_WAIT_MS)
        {
            st.ShopBar = money + SRF_BOT_SHOP_MORE * GOLD;
            st.ShopSince = 0;
        }
    }
    else
        st.ShopSince = 0;
    bool const wantsShop = money >= std::max(SRF_BOT_SHOP_GOLD * GOLD, st.ShopBar);
    float const pct = bot->GetHealthPct();
    if (pct < SRF_BOT_RETREAT_PCT)
        st.Retreating = true;
    else if (st.Retreating && pct >= SRF_BOT_RETURN_PCT && !(IsAtBase(bot) && wantsShop))
        st.Retreating = false;
    if (!st.Retreating && wantsShop && !bot->IsInCombat() && !IsAtBase(bot))
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

    // The jungler: a gank first - an enemy champion near, hurt or in a fight with
    // the team, and off its turrets - then the next camp that is up, its own side
    // first, and with none up the lane where a fight is (the middle with none).
    uint8 lane = st.Lane;
    if (st.Lane == BG_SRF_LANE_NONE)
    {
        Player* prey = nullptr;
        for (Player* enemy : TeamPlayers(OtherTeam(team)))
        {
            if (!enemy->IsAlive() || enemy->GetExactDist2d(bot) > SRF_BOT_GANK_RANGE || !bot->IsValidAttackTarget(enemy)
                || UnderFreeEnemyTurret(team, enemy->GetPosition(), 3.0f))
                continue;
            bool fighting = enemy->GetHealthPct() < SRF_BOT_GANK_HURT_PCT;
            for (Player* ally : TeamPlayers(team))
                if (!fighting && ally != bot && ally->IsAlive() && ally->GetExactDist2d(enemy) < SRF_BOT_FIGHT_RANGE)
                    fighting = true;
            if (fighting && (!prey || enemy->GetHealthPct() < prey->GetHealthPct()))
                prey = enemy;
        }
        if (prey)
        {
            out.Post = prey->GetPosition();
            out.Target = prey->GetGUID();
            return true;
        }
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
            if (best->GetExactDist2d(bot) <= SRF_BOT_POST_NEAR || best->GetVictim() == bot)
                out.Target = best->GetGUID();
            return true;
        }
        // nothing up: the lane with a fight in it, else the middle - for now; the
        // jungler stays the jungler (it used to be sent mid for the rest of the match)
        lane = BG_SRF_LANE_MID;
        for (Player* ally : TeamPlayers(team))
        {
            uint8 const role = RoleOf(ally->GetGUID());
            if (ally == bot || !ally->IsAlive() || role == SRF_ROLE_JUNGLE)
                continue;
            bool fight = false;
            for (Player* enemy : TeamPlayers(OtherTeam(team)))
                if (enemy->IsAlive() && enemy->GetExactDist2d(ally) < SRF_BOT_FIGHT_RANGE)
                    fight = true;
            if (fight)
            {
                lane = LaneOfRole(role);
                break;
            }
        }
    }

    // Laning: just behind the frontmost allied minion of its lane, or at its
    // outermost standing turret while there is none.
    std::vector<Position> const& path = LanePath(team, lane);
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
        if (m.Team != team || m.Lane != lane)
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
        front = int32(LaneIndexNear(team, lane, *clashAt));
    if (front < 0)
    {
        front = 0;
        for (uint8 i = 0; i < BG_SRF_STRUCTURES_PER_TEAM; ++i)
        {
            uint8 const slot = SlotOf(team, i);
            if (!_structures[slot].Destroyed && StructureAt(slot).kind == BG_SRF_KIND_TURRET && StructureAt(slot).lane == lane)
                front = std::max<int32>(front, int32(LaneIndexNear(team, lane, StructurePos(slot))));
        }
    }
    front = std::min<int32>(front, int32(path.size()) - 1);
    // back along the path until it is its distance behind it, and out of free turret reach
    bool const ranged = FightsAtRange(bot);
    float const behind = ranged ? SRF_BOT_BEHIND_WAVE_RANGED : SRF_BOT_BEHIND_WAVE;
    float const engage = SRF_BOT_ENGAGE_RANGE + (ranged ? SRF_BOT_BEHIND_WAVE_RANGED - SRF_BOT_BEHIND_WAVE : 0.0f);
    int32 post = front;
    float walked = 0.0f;
    while (post > 0 && walked < behind)
    {
        walked += path[post].GetExactDist2d(&path[post - 1]);
        --post;
    }
    while (post > 0 && UnderFreeEnemyTurret(team, path[post], 3.0f))
        --post;
    out.Post = path[post];

    // The support: a few yards behind its carry, wherever the carry is in the lane.
    bool const support = st.Role == SRF_ROLE_SUPPORT;
    if (support)
        if (Player* carry = TeamCarry(team); carry && carry != bot && carry->GetExactDist2d(&out.Post) < SRF_BOT_GANK_RANGE)
        {
            int32 at = int32(LaneIndexNear(team, lane, carry->GetPosition()));
            float back = 0.0f;
            while (at > 0 && back < SRF_BOT_SUPPORT_BEHIND)
            {
                back += path[at].GetExactDist2d(&path[at - 1]);
                --at;
            }
            out.Post = path[at];
        }

    // What to hit near the post: an enemy minion low enough to last-hit, then any
    // enemy minion, then an enemy champion in reach, then a turret the minions tank.
    Unit* best = nullptr;
    int32 bestScore = 0;
    bool const atPost = bot->GetExactDist2d(&out.Post) <= SRF_BOT_POST_NEAR;
    auto consider = [&](Unit* unit, int32 score)
    {
        if (!unit || !unit->IsAlive() || !bot->IsValidAttackTarget(unit))
            return;
        // in reach of the bot itself, or of the post once it is there
        if ((unit->GetExactDist2d(bot) > engage && (!atPost || unit->GetExactDist2d(&out.Post) > engage))
            || !BotMayAttack(bot, unit))
            return;
        if (unit->GetGUID() == current)
            score += SRF_BOT_TARGET_KEEP;
        if (score > bestScore || (score == bestScore && best && unit->GetExactDist2d(bot) < best->GetExactDist2d(bot)))
        {
            best = unit;
            bestScore = score;
        }
    };
    // A support leaves the last hits to its carry: a low minion is not its.
    for (Minion const& m : _minions)
        if (m.Team != team)
            if (Creature* minion = map->GetCreature(m.Guid))
                consider(minion, support ? (minion->GetHealthPct() < 35.0f ? 0 : 5) : (minion->GetHealthPct() < 35.0f ? 40 : 20));
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

// League's cadence (Season 3): a wave every 30 s, all match.
uint32 BattlegroundSRF::WaveInterval() const
{
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
            if (m.Caster ? creature->GetExactDist2d(victim) <= (m.Kind == SRF_MINION_SIEGE ? SRF_SIEGE_RANGE : SRF_CASTER_RANGE) : creature->IsWithinMeleeRange(victim))
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
    AwardMinionExperience(enemy, where, SRF_MINION_XP[minion.Kind], minion.Killer);
    if (!GetCustomRules().RiftLastHitGold && !minion.Paid && corpse)
        AwardGold(enemy, corpse, SRF_MINION_GOLD[minion.Kind]);
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
    RiftProgression::OnUnitKill(killer);
    if (GetCustomRules().RiftLastHitGold)
        RiftProgression::AddGold(killer, SRF_MINION_GOLD[it->Kind], true);
}

void BattlegroundSRF::RecordChampionContribution(Player* attacker, Player* victim)
{
    if (!attacker || !victim || attacker == victim || GetStatus() != STATUS_IN_PROGRESS)
        return;
    if (GetPlayerTeam(attacker->GetGUID()) == GetPlayerTeam(victim->GetGUID()))
        return;
    _contributions[victim->GetGUID()][attacker->GetGUID()] = _matchClock;
}

void BattlegroundSRF::HandleKillPlayer(Player* victim, Player* killer)
{
    Battleground::HandleKillPlayer(victim, killer);
    if (!victim || GetStatus() != STATUS_IN_PROGRESS)
        return;
    // League's kill credit: the enemy champions who damaged the victim or used an
    // ability on it in its last 10 s. A death a turret, a minion or a monster
    // dealt goes to the latest of them; only with none is it "Executed".
    std::vector<std::pair<uint32, ObjectGuid>> recent;
    if (auto const hits = _contributions.find(victim->GetGUID()); hits != _contributions.end())
    {
        for (auto const& [guid, at] : hits->second)
            if (_matchClock - at <= SRF_ASSIST_WINDOW_MS)
                recent.emplace_back(at, guid);
        _contributions.erase(hits);
    }
    std::sort(recent.begin(), recent.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    if (!killer || killer == victim || GetPlayerTeam(killer->GetGUID()) == GetPlayerTeam(victim->GetGUID()))
    {
        killer = nullptr;
        for (auto const& [at, guid] : recent)
            if (Player* credited = ObjectAccessor::FindPlayer(guid); credited && credited->GetBattleground() == this)
            {
                killer = credited;
                break;
            }
    }
    RiftProgression::OnDeath(victim);
    if (killer && killer != victim)
        RiftProgression::OnUnitKill(killer);
    TeamId const victimTeam = GetTeamIndexByTeamId(GetPlayerTeam(victim->GetGUID()));
    PassCampBuffs(victim, killer && killer != victim && GetTeamIndexByTeamId(GetPlayerTeam(killer->GetGUID())) != victimTeam ? killer : nullptr);
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

    // Assists: League's - every other champion of the killer's side who damaged
    // the victim or used an ability on it in its last 10 s. They share half the
    // kill's gold, as in League.
    std::vector<Player*> assists;
    for (auto const& [at, guid] : recent)
        if (guid != killer->GetGUID())
            if (Player* ally = ObjectAccessor::FindPlayer(guid); ally && ally->GetBattleground() == this)
                assists.push_back(ally);
    for (Player* ally : assists)
    {
        ++_stats[ally->GetGUID()].Assists;
        RiftProgression::AddGold(ally, std::max<uint32>(1, gold / 2 / uint32(assists.size())), true);
        RiftProgression::OnChampionTakedown(ally, false);
    }
    RiftProgression::AddGold(killer, gold, true);
    RiftProgression::OnChampionTakedown(killer, true);
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
// The spirit guide's "Resurrection in" window counts the champion's own timer.
uint32 BattlegroundSRF::GetTimeUntilResurrectionFor(Player const* player) const
{
    auto const itr = player ? _respawnAt.find(player->GetGUID()) : _respawnAt.end();
    if (itr == _respawnAt.end())
        return 0;
    uint32 const now = GameTime::GetGameTimeMS();
    return itr->second > now ? itr->second - now : 0;
}

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
            {
                // back by some other resurrection (Reincarnation, a priest's): its pet too
                _respawnAt.erase(itr);
                if (uint32 const number = _championPet[guid])
                    _petRevive[guid] = { number, now, false };
            }
            else if (!_petRevive.count(guid))
            {
                // the pet out now, alive or dead - a summon that runs out (an Infernal) is not kept
                Pet* pet = player->GetPet();
                _championPet[guid] = pet && !pet->isTemporarySummoned() && pet->GetCharmInfo() ? pet->GetCharmInfo()->GetPetNumber() : 0;
            }
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
        if (uint32 const number = _championPet[guid])
            _petRevive[guid] = { number, now, false };
    }
    RevivePets();
}

// A champion back from the dead brings back the pet it had out (owner, 2026-10-23:
// "rezzing should also rez pets"): called from its stable once the champion stands at
// the fountain - LoadPetFromDB answers a moment later - then stood up, alive, at full
// health and power, as the arena brings a pet back
// (Player::ResummonPetTemporaryUnSummonedIfAny). One the champion called itself first
// is stood up the same way.
void BattlegroundSRF::RevivePets()
{
    uint32 constexpr SRF_PET_REVIVE_GIVE_UP_MS = 10 * IN_MILLISECONDS;
    uint32 const now = GameTime::GetGameTimeMS();
    for (auto itr = _petRevive.begin(); itr != _petRevive.end();)
    {
        Player* player = ObjectAccessor::FindPlayer(itr->first);
        PetRevive& revive = itr->second;
        if (!player || player->GetBattleground() != this || !player->IsAlive() || now - revive.Since > SRF_PET_REVIVE_GIVE_UP_MS)
        {
            itr = _petRevive.erase(itr);
            continue;
        }
        if (player->IsBeingTeleported())
        {
            ++itr;
            continue;
        }
        Pet* pet = player->GetPet();
        if (!pet)
        {
            if (!revive.Loading)
            {
                revive.Loading = true;
                Pet* loading = new Pet(player);
                if (!loading->LoadPetFromDB(player, 0, revive.Number, false))
                {
                    delete loading;
                    itr = _petRevive.erase(itr);
                    continue;
                }
            }
            ++itr;
            continue;
        }
        if (!pet->IsInWorld())
        {
            ++itr;
            continue;
        }
        if (!pet->IsAlive())
        {
            pet->ReplaceAllDynamicFlags(UNIT_DYNFLAG_NONE);
            pet->RemoveUnitFlag(UNIT_FLAG_SKINNABLE);
            pet->setDeathState(ALIVE);
            pet->ClearUnitState(UNIT_STATE_ALL_ERASABLE);
        }
        pet->SetFullHealth();
        for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
            if (Powers const power = Powers(i); power != POWER_HAPPINESS && pet->GetMaxPower(power))
                pet->SetPower(power, pet->GetMaxPower(power));
        itr = _petRevive.erase(itr);
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
// The level is the champion's WoW level in the match, not League's 1-18 (owner,
// 2026-10-23: "have the scorescreen display the wow level of the player").
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
                GetPlayerDisplayName(player), uint32(team), uint32(player->GetClass()), uint32(player->GetLevel()),
                st.Kills, st.Deaths, st.Assists, st.Minions, items));
        }
    for (std::string const& line : lines)
        Announce(line);
}

// The world map and the battlefield map (CENTURION_Rift's RiftMap.lua), once a
// second: every structure with its health - RIFTSTR:<slot>,<kind>,<x>,<y>,<health
// %>;... in chunks under the 255-character addon line - and the enemy champions
// the player's team can see, RIFTSEEN:<guid>,<class>,<x>,<y>;...
// RIFTRING:<slot>:<enemy 0/1>:<x>,<y>,<z>:<dz1>,...,<dz36> - a turret's reach
// (SRF_TURRET_RANGE) as 36 ground heights round it, in tenths of a yard off the
// turret's own: the dots CENTURION_Rift draws follow the ground.
void BattlegroundSRF::SendRings(Player* player)
{
    Map* map = FindBgMap();
    if (!map)
        return;
    uint32 const team = GetPlayerTeam(player->GetGUID()) ? GetPlayerTeam(player->GetGUID()) : player->GetTeam();
    TeamId const own = GetTeamIndexByTeamId(team);
    uint8 constexpr POINTS = 36;
    for (uint8 slot = 0; slot < _structures.size(); ++slot)
    {
        if (StructureAt(slot).kind != BG_SRF_KIND_TURRET)
            continue;
        Creature* turret = GetBGCreature(slot, false);
        if (!turret)
            continue;
        float const cx = turret->GetPositionX(), cy = turret->GetPositionY(), cz = turret->GetPositionZ();
        std::string heights;
        for (uint8 i = 0; i < POINTS; ++i)
        {
            float const a = 2.0f * float(M_PI) * float(i) / float(POINTS);
            float const x = cx + std::cos(a) * SRF_TURRET_RANGE, y = cy + std::sin(a) * SRF_TURRET_RANGE;
            float z = map->GetHeight(turret->GetPhaseMask(), x, y, cz + 15.0f, true, 60.0f);
            if (z <= INVALID_HEIGHT)
                z = cz;
            if (i)
                heights += ',';
            heights += std::to_string(std::lround((z - cz) * 10.0f));
        }
        RiftProgression::Tell(player, Trinity::StringFormat("RIFTRING:{}:{}:{},{},{}:{}", slot, SlotTeam(slot) != own ? 1 : 0,
            std::lround(cx), std::lround(cy), std::lround(cz), heights));
    }
}

void BattlegroundSRF::SendMap()
{
    std::vector<std::string> structures;
    std::string chunk;
    uint32 inChunk = 0;
    for (uint8 slot = 0; slot < _structures.size(); ++slot)
    {
        uint32 health = 0;
        if (!_structures[slot].Destroyed)
            if (Creature* creature = GetBGCreature(slot, false))
                if (creature->IsAlive())
                    health = std::max<uint32>(1, uint32(creature->GetHealthPct() + 0.5f));
        Position const pos = StructurePos(slot);
        chunk += Trinity::StringFormat("{}{},{},{},{},{}", inChunk ? ";" : "", uint32(slot), uint32(StructureAt(slot).kind),
            int32(std::lround(pos.GetPositionX())), int32(std::lround(pos.GetPositionY())), health);
        if (++inChunk == 8)
        {
            structures.push_back("RIFTSTR:" + chunk);
            chunk.clear();
            inChunk = 0;
        }
    }
    if (inChunk)
        structures.push_back("RIFTSTR:" + chunk);

    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        std::string seen;
        for (Player* enemy : TeamPlayers(OtherTeam(team)))
            if (enemy->IsAlive() && SeenBy(team, enemy))
                seen += Trinity::StringFormat("{}{},{},{},{}", seen.empty() ? "" : ";", enemy->GetGUID().GetCounter(),
                    uint32(enemy->GetClass()), int32(std::lround(enemy->GetPositionX())), int32(std::lround(enemy->GetPositionY())));
        for (Player* player : TeamPlayers(team))
        {
            for (std::string const& line : structures)
                RiftProgression::Tell(player, line);
            RiftProgression::Tell(player, "RIFTSEEN:" + seen);
            auto const sent = _ringsSentAt.find(player->GetGUID());
            if (sent == _ringsSentAt.end() || _matchClock - sent->second >= MINUTE * IN_MILLISECONDS)
            {
                SendRings(player);
                _ringsSentAt[player->GetGUID()] = _matchClock;
            }
        }
    }
}

// League's vision: an enemy champion near an allied champion, minion or
// standing turret, and not hidden from it in brush (HidesFrom).
bool BattlegroundSRF::SeenBy(TeamId team, Player* enemy)
{
    for (Player* ally : TeamPlayers(team))
        if (ally->IsAlive() && ally->GetExactDist2d(enemy) <= SRF_VISION_CHAMPION && !HidesFrom(enemy, ally))
            return true;
    Map* map = enemy->GetMap();
    for (Minion const& m : _minions)
        if (m.Team == team)
            if (Creature* minion = map->GetCreature(m.Guid))
                if (minion->IsAlive() && minion->GetExactDist2d(enemy) <= SRF_VISION_MINION && !HidesFrom(enemy, minion))
                    return true;
    for (uint8 slot = 0; slot < _structures.size(); ++slot)
        if (SlotTeam(slot) == team && !_structures[slot].Destroyed && StructureAt(slot).kind == BG_SRF_KIND_TURRET)
            if (Creature* turret = GetBGCreature(slot, false))
                if (turret->IsAlive() && turret->GetExactDist2d(enemy) <= SRF_VISION_TURRET && !HidesFrom(enemy, turret))
                    return true;
    return false;
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

// Minions, monsters and structures level with the champions: from the match's
// starting level, Centurion.Rift.UnitLevelRate of the way the champions' average
// has come (1.0: the average itself; .reload config takes a change).
uint8 BattlegroundSRF::UnitLevel() const
{
    float const rate = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Rift.UnitLevelRate", 1.0f));
    float const average = float(AverageMatchLevel());
    float const base = std::min(average, float(SRF_UNIT_BASE_LEVEL));
    return uint8(std::clamp(std::lround(base + (average - base) * rate), 1L, long(DEFAULT_MAX_LEVEL)));
}

// League's clock, in minutes: its first wave at 1:30, ours BG_SRF_FIRST_WAVE_TIME
// after the gates open.
float BattlegroundSRF::LeagueMinutes() const
{
    return float(_matchClock + SRF_LEAGUE_FIRST_WAVE - BG_SRF_FIRST_WAVE_TIME) / float(MINUTE * IN_MILLISECONDS);
}

// The champions' average League level (RiftProgression), 1..18.
float BattlegroundSRF::AverageLeagueLevel() const
{
    uint32 total = 0, count = 0;
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        for (Player* player : TeamPlayers(team))
        {
            total += RiftProgression::GetLeagueLevel(player);
            ++count;
        }
    return count ? float(total) / float(count) : 1.0f;
}

// The League level the units are converted at: Centurion.Rift.UnitLevelRate of
// the way from level 1 to the champions' average (1.0: the champions' own, so a
// unit's League numbers meet the champions' at the same scale).
float BattlegroundSRF::UnitLeagueLevel() const
{
    float const rate = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Rift.UnitLevelRate", 1.0f));
    return 1.0f + (AverageLeagueLevel() - 1.0f) * rate;
}

// The units' scale, kept with the item tick (worked out here before the first).
float BattlegroundSRF::UnitScale() const
{
    return _unitScale > 0.0f ? _unitScale : RiftLeague::Scale(UnitLeagueLevel());
}

char const* BattlegroundSRF::StructureKind(uint8 slot) const
{
    uint8 const index = SlotIndex(slot);
    if (index == SRF_SLOT_NEXUS)
        return "nexus";
    if (index == SRF_SLOT_NEXUS_TURRET_1 || index == SRF_SLOT_NEXUS_TURRET_2)
        return "turret_nexus";
    switch (index % 4)
    {
        case 0: return "turret_outer";
        case 1: return "turret_inner";
        case 2: return "turret_inhibitor";
        default: return "inhibitor";
    }
}

// The jungle's monsters, by the Rift's creature: each plays the Season 3 camp
// that stood in its place (world rift_league_unit says which).
char const* BattlegroundSRF::MonsterKind(uint32 entry)
{
    switch (entry)
    {
        case BG_SRF_ENTRY_BLUE_SENTINEL:     return "blue_sentinel";
        case BG_SRF_ENTRY_RED_BRAMBLEBACK:   return "red_brambleback";
        case BG_SRF_ENTRY_GREATER_MURK_WOLF: return "greater_murk_wolf";
        case BG_SRF_ENTRY_MURK_WOLF:         return "murk_wolf";
        case BG_SRF_ENTRY_CRIMSON_RAPTOR:    return "crimson_raptor";
        case BG_SRF_ENTRY_RAPTOR:            return "raptor";
        case BG_SRF_ENTRY_ANCIENT_KRUG:      return "ancient_krug";
        case BG_SRF_ENTRY_KRUG:              return "krug";
        case BG_SRF_ENTRY_GROMP:             return "gromp";
        case BG_SRF_ENTRY_RIFT_SCUTTLER:     return "rift_scuttler";
        case BG_SRF_ENTRY_DRAGON:            return "dragon";
        case BG_SRF_ENTRY_BARON_NASHOR:      return "baron";
        default:                             return "";
    }
}

// A unit takes League's stats for its kind at `minutes` on League's clock, in
// WoW's numbers at the match's conversion (RiftLeague): health (keeping the share
// it had, times `healthScale`), armour, resistances, and its attack's damage and
// speed. And the players' level, for WoW's own level rules. `backdoor`: League's
// backdoor plating, a structure with no enemy minion near.
void BattlegroundSRF::ApplyLeagueStats(Creature* creature, char const* kind, float minutes, float healthScale, bool backdoor)
{
    if (!creature || !creature->IsAlive())
        return;
    float const pct = creature->GetHealthPct();
    uint8 const level = UnitLevel();
    if (creature->GetLevel() != level)
        creature->SetLevel(level);

    std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
    RiftLeague::UnitStats const* unit = RiftLeague::FindUnit(*tables, kind);
    if (!unit)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundSRF: rift_league_unit has no `{}` (entry {}); it keeps its template stats.",
            kind, creature->GetEntry());
        creature->UpdateLevelDependantStats();
        creature->SetHealth(std::max<uint32>(1, uint32(float(creature->GetMaxHealth()) * pct / 100.0f)));
        return;
    }
    float const scale = UnitScale();
    RiftLeague::UnitAt const at = RiftLeague::At(*unit, minutes);

    uint32 const health = std::max<uint32>(1, uint32(std::lround(scale * at.Health * healthScale)));
    creature->SetCreateHealth(health);
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(health));
    creature->UpdateMaxHealth();

    float const armor = at.Armor + (backdoor ? unit->BackdoorArmor : 0.0f);
    creature->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, RiftLeague::ToWowArmor(armor, _combatLevel));
    creature->UpdateArmor();
    float const resistance = RiftLeague::ToWowResist(at.Resist + (backdoor ? unit->BackdoorResist : 0.0f));
    for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
    {
        creature->SetStatFlatModifier(UnitMods(UNIT_MOD_RESISTANCE_START + school), BASE_VALUE, resistance);
        creature->UpdateResistances(school);
    }

    // Creature::CalculateMinMaxDamage: weapon x DamageModifier x the attack time
    // in seconds, plus attack power, which a League unit has none of.
    uint32 const attackTime = uint32(std::lround(1000.0f / unit->AttackSpeed));
    creature->SetAttackTime(BASE_ATTACK, attackTime);
    float const modDamage = std::max(0.01f, creature->GetCreatureTemplate()->ModDamage);
    float const weapon = scale * at.Damage / (modDamage * float(attackTime) / 1000.0f);
    creature->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, weapon);
    creature->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, weapon);
    creature->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER, BASE_VALUE, 0.0f);
    creature->UpdateAttackPowerAndDamage();

    creature->SetHealth(std::max<uint32>(1, uint32(float(creature->GetMaxHealth()) * pct / 100.0f)));
}

// League's damage a hit for a unit of `kind` at `minutes`, in WoW's numbers.
float BattlegroundSRF::LeagueAttackDamage(char const* kind, float minutes) const
{
    std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
    RiftLeague::UnitStats const* unit = RiftLeague::FindUnit(*tables, kind);
    return unit ? UnitScale() * RiftLeague::At(*unit, minutes).Damage : 0.0f;
}

// League's backdoor plating: a turret with no enemy minion within its reach
// takes its bonus armour and magic resist (rift_league_unit backdoor_*), put
// on as the state changes.
void BattlegroundSRF::UpdateBackdoor()
{
    std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
    for (uint8 slot = 0; slot < _structures.size(); ++slot)
    {
        StructureState& st = _structures[slot];
        if (st.Destroyed)
            continue;
        RiftLeague::UnitStats const* unit = RiftLeague::FindUnit(*tables, StructureKind(slot));
        if (!unit || (unit->BackdoorArmor <= 0.0f && unit->BackdoorResist <= 0.0f))
            continue;
        Creature* creature = GetBGCreature(slot, false);
        if (!creature || !creature->IsAlive())
            continue;
        TeamId const team = SlotTeam(slot);
        bool const covered = std::any_of(_minions.begin(), _minions.end(), [&](Minion const& m)
        {
            return m.Team != team && creature->GetExactDist2d(&m.Pos) <= SRF_TURRET_RANGE;
        });
        if (covered == !st.Backdoor)
            continue;
        st.Backdoor = !covered;
        ApplyLeagueStats(creature, StructureKind(slot), LeagueMinutes(), 1.0f, st.Backdoor);
    }
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
                {
                    RiftProgression::AddGold(player, SRF_GOLD_PASSIVE);
                    RiftProgression::ItemGoldTick(player, SRF_PASSIVE_GOLD_TICK);   // gold-per-10 items
                }
    }

    _maintainTimer += diff;
    if (_maintainTimer >= SRF_MAINTAIN_TICK)
    {
        _maintainTimer = 0;
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            for (Player* player : TeamPlayers(team))
                RiftProgression::Maintain(player);
    }

    _structureLevelTimer += diff;
    if (_structureLevelTimer >= SRF_STRUCTURE_LEVEL_TICK)
    {
        _structureLevelTimer = 0;
        // League's turrets grow with the clock; minions and monsters keep the
        // League numbers they came out with, restated at the match's scale.
        float const minutes = LeagueMinutes();
        for (uint8 slot = 0; slot < _structures.size(); ++slot)
            if (!_structures[slot].Destroyed)
                ApplyLeagueStats(GetBGCreature(slot, false), StructureKind(slot), minutes, 1.0f, _structures[slot].Backdoor);
        if (Map* map = FindBgMap())
        {
            for (Minion const& m : _minions)
                if (Creature* creature = map->GetCreature(m.Guid))
                    if (!m.Denied)
                        ApplyLeagueStats(creature, SRF_MINION_KIND[m.Kind], m.SpawnMinutes, m.HealthScale, false);
            for (uint8 i = 0; i < BG_SRF_JUNGLE_MONSTERS; ++i)
                if (_camps[SRF_MONSTERS[i].camp].Up)
                    if (Creature* creature = GetBGCreature(BG_SRF_CREATURE_JUNGLE_FIRST + i, false))
                        ApplyLeagueStats(creature, MonsterKind(SRF_MONSTERS[i].entry), _camps[SRF_MONSTERS[i].camp].SpawnMinutes, 1.0f, false);
        }
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

    // League's turret priority: siege and super minions, then melee, then casters
    // (and champions' pets), then champions.
    for (Minion const& m : _minions)
        if (m.Team != team)
            consider(map->GetCreature(m.Guid), m.Kind == SRF_MINION_SIEGE || m.Kind == SRF_MINION_SUPER ? 0
                : m.Kind == SRF_MINION_CASTER ? 2 : 1);

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
    std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
    float const scale = UnitScale();
    float const minutes = LeagueMinutes();

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
        {
            ts.Warned.Clear();
            continue;
        }

        // League's warning: a champion hears it the moment a turret turns on it
        // (CENTURION_Rift plays the sound), once for each time it does.
        if (target->GetTypeId() == TYPEID_PLAYER)
        {
            if (ts.Warned != target->GetGUID())
            {
                ts.Warned = target->GetGUID();
                RiftProgression::Tell(target->ToPlayer(), "RIFTTURRET:1");
            }
        }
        else
            ts.Warned.Clear();

        RiftLeague::UnitStats const* stats = RiftLeague::FindUnit(*tables, StructureKind(slot));
        if (!stats)
            continue;
        float damage = scale * RiftLeague::At(*stats, minutes).Damage;
        if (target->GetTypeId() == TYPEID_PLAYER)
        {
            if (ts.LastChampion != target->GetGUID() && ts.WarmStacks > SRF_TURRET_SWITCH_HEAT)
                ts.WarmStacks = SRF_TURRET_SWITCH_HEAT;
            damage *= SRF_TURRET_HEAT[ts.WarmStacks];
            ts.WarmStacks = std::min<uint8>(ts.WarmStacks + 1, uint8(SRF_TURRET_HEAT.size() - 1));
            ts.LastChampion = target->GetGUID();
            ts.SinceChampionShot = 0;
        }
        else
        {
            ts.WarmStacks = 0;
            for (Minion const& m : _minions)
                if (m.Guid == target->GetGUID())
                {
                    if (RiftLeague::UnitStats const* victim = RiftLeague::FindUnit(*tables, SRF_MINION_KIND[m.Kind]))
                        damage *= victim->TurretDamageTaken;
                    break;
                }
        }
        damage *= RiftLeague::ArmorMultiplier(float(target->GetArmor()) * (1.0f - stats->ArmorPenPct / 100.0f), _combatLevel);

        Creature* eye = TurretEye(slot, turret);
        Unit* source = eye ? static_cast<Unit*>(eye) : turret;
        uint32 const shot = team == TEAM_ALLIANCE ? BG_SRF_SPELL_ORDER_TURRET_SHOT : BG_SRF_SPELL_CHAOS_TURRET_SHOT;
        uint32 flight = 0;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(shot))
            if (info->Speed > 0.0f)
                flight = uint32(source->GetDistance(target) / info->Speed * float(IN_MILLISECONDS));
        ts.Cooldown = std::max<uint32>(BG_SRF_TURRET_TICK, flight + SRF_TURRET_SHOT_GAP);
        // A shot held back so the last one lands first deals the time it waited:
        // the turret keeps League's damage a second.
        damage *= std::max(1.0f, float(ts.Cooldown) * stats->AttackSpeed / 1000.0f);
        FireBolt(source, target, shot, std::max<int32>(1, int32(std::lround(damage))));
    }
}

void BattlegroundSRF::FireBolt(Unit* source, Unit* target, uint32 spellId, int32 damage)
{
    // a base point: the shot rows' die adds 1 (SpellEffectInfo::CalcValue)
    SpellCastResult const result = source->CastSpell(target, spellId, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(std::max(0, damage - 1)));
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
        static char const* const ROLE_NAMES[SRF_ROLE_COUNT] = { "top", "jungle", "mid", "carry", "support" };
        TC_LOG_INFO("bg.battleground", "Summoner's Rift {} bot {} (class {}): {}, lane {}, {}{}, at {:.0f} {:.0f}, {:.0f} yd from post, ordered {}, victim {}, hp {:.0f}%, casting {}.",
            GetInstanceID(), bot->GetName(), uint32(bot->GetClass()), st.Assigned ? ROLE_NAMES[st.Role % SRF_ROLE_COUNT] : "unassigned", uint32(st.Lane),
            st.HasOrders ? "orders" : "NO ORDERS", st.Orders.Retreat ? " (retreating)" : "", bot->GetPositionX(), bot->GetPositionY(),
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
// bolt at League's attack speed with League's damage, through the target's
// armour - a caster minion's attack is physical - instead of walking up.
void BattlegroundSRF::UpdateCasterAttack(Creature* caster, Minion& minion, Unit* victim)
{
    float const range = minion.Kind == SRF_MINION_SIEGE ? SRF_SIEGE_RANGE : SRF_CASTER_RANGE;
    if (caster->GetExactDist2d(victim) > range)
    {
        if (caster->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
            caster->GetMotionMaster()->MoveChase(victim, range * 0.8f);
        return;
    }
    if (caster->isMoving())
        caster->StopMoving();
    caster->SetFacingToObject(victim);

    minion.AttackTimer = minion.AttackTimer > BG_SRF_MINION_TICK ? minion.AttackTimer - BG_SRF_MINION_TICK : 0;
    if (minion.AttackTimer)
        return;
    minion.AttackTimer = caster->GetAttackTime(BASE_ATTACK);

    float const damage = LeagueAttackDamage(SRF_MINION_KIND[minion.Kind], minion.SpawnMinutes)
        * RiftLeague::ArmorMultiplier(float(victim->GetArmor()), _combatLevel);
    bool const order = minion.Team == TEAM_ALLIANCE;
    FireBolt(caster, victim, order ? BG_SRF_SPELL_ORDER_CASTER_BOLT : BG_SRF_SPELL_CHAOS_CASTER_BOLT,
        std::max<int32>(1, int32(std::lround(damage))));
}

// -------------------------------------------------------------------- jungle

void BattlegroundSRF::SpawnCamp(uint8 camp)
{
    Map* map = FindBgMap();
    uint32 spawned = 0;
    Position where;
    // League's camps take their stats from the time they come up.
    _camps[camp].SpawnMinutes = LeagueMinutes();
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
        Creature* creature = AddCreature(m.entry, slot, pos, TEAM_NEUTRAL, SRF_CORE_RESPAWN);
        if (!creature)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundSRF: failed to spawn jungle monster {} (entry {}).", i, m.entry);
            continue;
        }
        // Neutral until struck; the scuttler never fights back.
        creature->SetReactState(SRF_CAMPS[camp].kind == SRF_CAMP_SCUTTLE ? REACT_PASSIVE : REACT_DEFENSIVE);
        ApplyLeagueStats(creature, MonsterKind(m.entry), _camps[camp].SpawnMinutes, 1.0f, false);
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
// Baron is stationary (League: he never leaves his pit - creature_template_movement
// Rooted on 922141) and has no leash. The S3 Dragon walks and can be pulled out.
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
    RiftProgression::OnUnitKill(killer);
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
            ApplyCampBuff(killer, BG_SRF_SPELL_BLUE_BUFF);
            break;
        case SRF_CAMP_RED:
            ApplyCampBuff(killer, BG_SRF_SPELL_RED_BUFF);
            break;
        case SRF_CAMP_DRAGON:
            for (Player* player : TeamPlayers(team))
                RiftProgression::AddGold(player, SRF_GOLD_DRAGON_TEAM, true);
            PSendMessageToAll(BG_SRF_STRING_DRAGON_SLAIN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(team));
            AnnounceEvent("DRAGON", team);
            break;
        case SRF_CAMP_BARON:
        {
            int32 const power = LeagueMinutes() < SRF_BARON_EARLY_MINUTES ? SRF_BARON_POWER_EARLY : SRF_BARON_POWER;
            for (Player* player : TeamPlayers(team))
            {
                RiftProgression::AddGold(player, SRF_GOLD_BARON_TEAM, true);
                // Exalted with Baron Nashor: the living only, and lost on death.
                if (player->IsAlive())
                    ApplyBuff(player, BG_SRF_SPELL_BARON_BUFF, power, 0, 0, SRF_BARON_BUFF_DURATION);
            }
            PSendMessageToAll(BG_SRF_STRING_BARON_SLAIN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, TeamName(team));
            AnnounceEvent("BARON", team);
            break;
        }
        default:
            break;
    }
    return true;
}

// A stock spell with League's values (the amounts; 0 keeps the spell's own). The
// rows roll a die of 1 on top of their base points (SpellEffectInfo::CalcValue),
// so a base point is the amount less 1 - passed whole, Baron's 40 came out 41.
void BattlegroundSRF::ApplyBuff(Player* player, uint32 spellId, int32 bp0, int32 bp1, int32 bp2, int32 durationMs)
{
    player->RemoveAurasDueToSpell(spellId);
    CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
    if (bp0)
        args.AddSpellMod(SPELLVALUE_BASE_POINT0, bp0 - 1);
    if (bp1)
        args.AddSpellMod(SPELLVALUE_BASE_POINT1, bp1 - 1);
    if (bp2)
        args.AddSpellMod(SPELLVALUE_BASE_POINT2, bp2 - 1);
    player->CastSpell(player, spellId, args);
    if (durationMs > 0)
        if (Aura* aura = player->GetAura(spellId))
        {
            aura->SetMaxDuration(durationMs);
            aura->SetDuration(durationMs);
        }
}

// A buff camp's buff, its whole 2:30. When it ends is written down: dying takes
// the aura off before HandleKillPlayer hears of it, and the killer inherits it.
void BattlegroundSRF::ApplyCampBuff(Player* player, uint32 spellId)
{
    ApplyBuff(player, spellId, 0, 0, 0, SRF_BUFF_DURATION);
    _campBuffEnds[player->GetGUID()][spellId == BG_SRF_SPELL_BLUE_BUFF ? 0 : 1] = GameTime::GetGameTimeMS() + SRF_BUFF_DURATION;
}

// League Season 3: the champion credited with a kill takes the victim's Crest
// and Blessing, each for a whole 2:30 again; with none to credit they are lost.
void BattlegroundSRF::PassCampBuffs(Player* victim, Player* heir)
{
    auto const held = _campBuffEnds.find(victim->GetGUID());
    if (held == _campBuffEnds.end())
        return;
    std::array<uint32, 2> const ends = held->second;
    _campBuffEnds.erase(held);
    if (!heir || !heir->IsAlive())
        return;
    uint32 const now = GameTime::GetGameTimeMS();
    if (ends[0] > now)
        ApplyCampBuff(heir, BG_SRF_SPELL_BLUE_BUFF);
    if (ends[1] > now)
        ApplyCampBuff(heir, BG_SRF_SPELL_RED_BUFF);
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

float BattlegroundSRF::DragonRange() const
{
    return SRF_DRAGON_RANGE;
}

// The fire goes through the target's armour, as a basic attack does, and on
// landing it burns: put on, or already there, made to last its 4 sec again
// without restarting the ticks (the wiki: it "refreshes as Dragon attacks").
void BattlegroundSRF::DragonAttack(Creature* dragon, Unit* victim)
{
    if (!dragon || !victim)
        return;
    float const hit = (dragon->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE) + dragon->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE)) / 2.0f;
    float const damage = hit * RiftLeague::ArmorMultiplier(float(victim->GetArmor()), _combatLevel);
    FireBolt(dragon, victim, BG_SRF_SPELL_DRAGON_FIRE, std::max<int32>(1, int32(std::lround(damage))));

    int32 const tick = std::max(1, int32(std::lround(UnitScale() * SRF_DRAGON_BURN / 4.0f)));
    uint32 const flight = std::max<uint32>(1, uint32(dragon->GetExactDist(victim) / SRF_DRAGON_FIRE_SPEED * 1000.0f));
    ObjectGuid const victimGuid = victim->GetGUID();
    dragon->m_Events.AddEventAtOffset([dragon, victimGuid, tick]()
    {
        Unit* struck = ObjectAccessor::GetUnit(*dragon, victimGuid);
        if (!struck || !struck->IsAlive() || !dragon->IsAlive())
            return;
        if (Aura* burn = struck->GetAura(BG_SRF_SPELL_DRAGON_BURN, dragon->GetGUID()))
        {
            burn->SetDuration(burn->GetMaxDuration());
            return;
        }
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellMod(SPELLVALUE_BASE_POINT2, tick - 1);   // the row's die adds 1
        dragon->CastSpell(struck, BG_SRF_SPELL_DRAGON_BURN, args);
    }, Milliseconds(flight));
}

float BattlegroundSRF::BaronRange() const
{
    return SRF_BARON_RANGE;
}

float BattlegroundSRF::BaronSweepReach() const
{
    return SRF_BARON_SWEEP_REACH;
}

// One bolt of the stream: League's 70 magic at the match's scale. The bolt's own
// second effect is the stack; League's magic resist takes its share as for any
// spell in the Rift (Unit::CalculateAverageResistReduction).
void BattlegroundSRF::BaronWrath(Creature* baron, Unit* victim)
{
    if (!baron || !victim)
        return;
    FireBolt(baron, victim, BG_SRF_SPELL_BARON_WRATH, std::max<int32>(1, int32(std::lround(UnitScale() * SRF_BARON_WRATH))));
}

// Its physical attack, League's 460 (the weapon ApplyLeagueStats gave it); the
// target's League armor takes its share (a physical spell: Unit::CalcArmorReducedDamage).
void BattlegroundSRF::BaronCorrosion(Creature* baron, Unit* victim)
{
    if (!baron || !victim)
        return;
    float const hit = (baron->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE) + baron->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE)) / 2.0f;
    FireBolt(baron, victim, BG_SRF_SPELL_BARON_CORROSION, std::max<int32>(1, int32(std::lround(hit))));
}

// Sweeping Blow: every champion close by, out to 600 League units from Baron.
void BattlegroundSRF::BaronSweep(Creature* baron)
{
    if (!baron || !baron->IsAlive())
        return;
    float const flight = 2.0f * SRF_SWEEP_SPEED_Z / SRF_KNOCK_GRAVITY;
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        for (Player* player : TeamPlayers(team))
        {
            if (!player->IsAlive() || !baron->IsValidAttackTarget(player))
                continue;
            float const gap = baron->GetExactDist2d(player) - baron->GetCombatReach() - player->GetCombatReach();
            if (gap > SRF_BARON_SWEEP_REACH)
                continue;
            float const push = std::max(1.0f, SRF_BARON_SWEEP_DISTANCE - std::max(0.0f, gap));
            player->KnockbackFrom(baron->GetPositionX(), baron->GetPositionY(), push / flight, SRF_SWEEP_SPEED_Z);
        }
}

// Propelled: the ooze shows under the target, and a second later whoever stands in
// it is thrown up and slowed - there is time to step out (the wiki: "easy to spot
// and subsequently dodge"). The marker is a turret eye, invisible, for the puddle.
void BattlegroundSRF::BaronPropel(Creature* baron, Unit* victim)
{
    if (!baron || !victim)
        return;
    Position const spot = victim->GetPosition();
    if (TempSummon* marker = baron->SummonCreature(BG_SRF_ENTRY_TURRET_EYE, spot, TEMPSUMMON_TIMED_DESPAWN, Milliseconds(SRF_BARON_PUDDLE_MS + 500)))
    {
        marker->SetFaction(baron->GetFaction());
        marker->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE));
        marker->AddAura(BG_SRF_SPELL_BARON_PUDDLE, marker);
    }
    ObjectGuid const baronGuid = baron->GetGUID();
    baron->m_Events.AddEventAtOffset([baron, baronGuid, spot]()
    {
        // the match from Baron's map, not `this`: the event outlives nothing, but says so
        BattlegroundMap* map = baron->GetMap() ? baron->GetMap()->ToBattlegroundMap() : nullptr;
        Battleground* bg = map ? map->GetBG() : nullptr;
        if (!bg || bg->GetTypeID(true) != BATTLEGROUND_SRF || !baron->IsAlive() || baron->GetGUID() != baronGuid)
            return;
        BattlegroundSRF const* rift = static_cast<BattlegroundSRF const*>(bg);
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            for (Player* player : rift->TeamPlayers(team))
            {
                if (!player->IsAlive() || !baron->IsValidAttackTarget(player)
                    || player->GetExactDist2d(&spot) > SRF_BARON_PUDDLE_RADIUS + player->GetCombatReach())
                    continue;
                player->KnockbackFrom(spot.GetPositionX(), spot.GetPositionY(), 0.0f, SRF_PROPEL_SPEED_Z);
                baron->CastSpell(player, BG_SRF_SPELL_BARON_PROPELLED, true);
            }
    }, Milliseconds(SRF_BARON_PUDDLE_MS));
}

float BattlegroundSRF::FlatHonorMultiplier() const
{
    return std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Rift.FlatHonorMultiplier", 2.0f));
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
