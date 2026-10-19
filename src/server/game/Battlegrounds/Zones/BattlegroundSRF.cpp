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
#include "SpellDefines.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cmath>
#include <iterator>

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
float constexpr SRF_FOUNTAIN_HEAL_RANGE = Yards(600.0f);
float constexpr SRF_FOUNTAIN_BURN_RANGE = Yards(700.0f);
// Turret shots as shares of the target's maximum health: minions die to a few
// shots, and a player who stays under a turret takes a little more every shot.
float constexpr SRF_TURRET_MINION_SHARE = 0.45f;
float constexpr SRF_TURRET_PLAYER_SHARE = 0.20f;
float constexpr SRF_TURRET_PLAYER_RAMP  = 0.10f;
float constexpr SRF_TURRET_PLAYER_MAX   = 0.60f;
// A shot leaves from the turret's crystal and flies at League's 1200 units a
// second, homing on its target; it lands when it reaches the target's chest.
float constexpr SRF_SHOT_ORIGIN_HEIGHT  = Yards(475.0f);
float constexpr SRF_SHOT_SPEED          = Yards(1200.0f);
float constexpr SRF_SHOT_HIT_DISTANCE   = Yards(75.0f);
float constexpr SRF_SHOT_AIM_HEIGHT     = Yards(60.0f);
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
uint32 constexpr SRF_LANE_SPLINE_ID     = 1;

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
    _shotTimer = 0;
    _fountainTimer = 0;
    _structureTimer = 0;
    _wavesStarted = false;
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
}

void BattlegroundSRF::Reset()
{
    Battleground::Reset();

    _structures.fill(StructureState());
    _turrets.fill(TurretState());
    _minions.clear();
    _shots.clear();
    for (auto& team : _lanePaths)
        for (std::vector<Position>& lane : team)
            lane.clear();
    _waveTimer = 0;
    _minionTimer = 0;
    _turretTimer = 0;
    _shotTimer = 0;
    _fountainTimer = 0;
    _structureTimer = 0;
    _wavesStarted = false;
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
    return true;
}

void BattlegroundSRF::StartingEventOpenDoors()
{
    _wavesStarted = true;
    _waveTimer = BG_SRF_WAVE_INTERVAL - BG_SRF_FIRST_WAVE_TIME;
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

    _turretTimer += diff;
    if (_turretTimer >= BG_SRF_TURRET_TICK)
    {
        _turretTimer = 0;
        UpdateTurrets();
    }

    _shotTimer += diff;
    if (_shotTimer >= BG_SRF_SHOT_TICK)
    {
        UpdateShots(_shotTimer);
        _shotTimer = 0;
    }

    _fountainTimer += diff;
    if (_fountainTimer >= BG_SRF_FOUNTAIN_TICK)
    {
        _fountainTimer = 0;
        UpdateFountains();
    }
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
    if (vulnerable)
    {
        creature->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
        creature->SetImmuneToAll(false);
    }
    else
    {
        creature->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
        creature->SetImmuneToAll(true);
    }
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
                // a small block, melee in front, so the wave does not spawn stacked
                float const ox = float(i % 3) - 1.0f, oy = float(i / 3);
                Position const pos(gate.GetPositionX() + ox * 2.0f, gate.GetPositionY() + oy * 2.0f, gate.GetPositionZ() + 1.0f, facing);
                TempSummon* minion = nexus->SummonCreature(i < BG_SRF_MELEE_PER_WAVE ? melee : caster, pos,
                    TEMPSUMMON_CORPSE_TIMED_DESPAWN, 3s);
                if (!minion)
                    continue;
                minion->SetReactState(REACT_AGGRESSIVE);
                Minion m;
                m.Guid = minion->GetGUID();
                m.Team = team;
                m.Lane = lane;
                _minions.push_back(m);
                spawned = true;
            }
        }
    }

    if (spawned)
        PSendMessageToAll(BG_SRF_STRING_MINIONS_SPAWNED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
}

Unit* BattlegroundSRF::FindMinionTarget(Creature* minion, TeamId team, float range) const
{
    Map* map = minion->GetMap();
    Unit* best = nullptr;
    float bestDist = range;
    auto consider = [&](Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !minion->IsValidAttackTarget(unit))
            return;
        float const dist = minion->GetExactDist2d(unit) - unit->GetCombatReach();
        if (dist < bestDist)
        {
            best = unit;
            bestDist = dist;
        }
    };

    for (Minion const& other : _minions)
        if (other.Team != team)
            consider(map->GetCreature(other.Guid));

    TeamId const enemy = OtherTeam(team);
    for (uint8 i = 0; i < BG_SRF_STRUCTURES_PER_TEAM; ++i)
    {
        uint8 const slot = SlotOf(enemy, i);
        if (_structures[slot].Vulnerable && !_structures[slot].Destroyed)
            consider(map->GetCreature(BgCreatures[slot]));
    }

    for (auto const& [guid, bgPlayer] : GetPlayers())
        if (bgPlayer.Team == TeamValue(enemy))
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                if (player->GetMap() == map)
                    consider(player);

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
            it = _minions.erase(it);
            continue;
        }

        Minion& m = *it;
        ++it;

        // A minion never walks back after a fight: its home is wherever it is
        // standing, so leaving combat costs nothing and it carries on down the lane.
        if (creature->IsInCombat() && creature->GetVictim())
        {
            m.Walking = false;
            continue;
        }
        if (!creature->IsInEvadeMode())
            creature->SetHomePosition(creature->GetPosition());

        if (Unit* target = FindMinionTarget(creature, m.Team, SRF_MINION_AGGRO_RANGE))
        {
            creature->AI()->AttackStart(target);
            m.Walking = false;
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
            points.push_back(path[i]);
        creature->GetMotionMaster()->MoveSmoothPath(SRF_LANE_SPLINE_ID, points.data(), points.size(), false);
        m.Walking = true;
        m.SplineEnd = end - 1;
    }
}

void BattlegroundSRF::DespawnMinions()
{
    if (Map* map = FindBgMap())
        for (Minion const& m : _minions)
            if (Creature* creature = map->GetCreature(m.Guid))
                creature->DespawnOrUnsummon();
    _minions.clear();
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
        FireShot(slot, turret, target, std::max<int32>(1, int32(target->GetMaxHealth() * share)));
    }
}

void BattlegroundSRF::FireShot(uint8 slot, Creature* turret, Unit* target, int32 damage)
{
    bool const order = SlotTeam(slot) == TEAM_ALLIANCE;
    Position const from(turret->GetPositionX(), turret->GetPositionY(), turret->GetPositionZ() + SRF_SHOT_ORIGIN_HEIGHT);
    TempSummon* orb = turret->SummonCreature(order ? BG_SRF_ENTRY_ORDER_SHOT : BG_SRF_ENTRY_CHAOS_SHOT, from,
        TEMPSUMMON_TIMED_DESPAWN, Milliseconds(BG_SRF_SHOT_LIFETIME));
    if (!orb)
    {
        // No orb, no flight: the shot still lands.
        turret->CastSpell(target, BG_SRF_SPELL_TURRET_SHOT, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(damage));
        return;
    }
    orb->SetDisableGravity(true);
    orb->SetSpeedRate(MOVE_RUN, SRF_SHOT_SPEED / baseMoveSpeed[MOVE_RUN]);
    orb->GetMotionMaster()->MovePoint(1, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ() + SRF_SHOT_AIM_HEIGHT, false);

    Shot shot;
    shot.Orb = orb->GetGUID();
    shot.Target = target->GetGUID();
    shot.TurretSlot = slot;
    shot.Damage = damage;
    _shots.push_back(shot);
}

// Shots home on their target and land on arrival: the damage, from the turret,
// and League's burst where it hits. A target that dies or leaves first is missed.
void BattlegroundSRF::UpdateShots(uint32 elapsed)
{
    Map* map = FindBgMap();
    if (!map)
        return;

    for (auto it = _shots.begin(); it != _shots.end();)
    {
        Shot& shot = *it;
        shot.Age += elapsed;
        Creature* orb = map->GetCreature(shot.Orb);
        Unit* target = orb ? ObjectAccessor::GetUnit(*orb, shot.Target) : nullptr;
        if (!orb || !target || !target->IsAlive() || target->GetMap() != map || shot.Age >= BG_SRF_SHOT_LIFETIME)
        {
            if (orb)
                orb->DespawnOrUnsummon();
            it = _shots.erase(it);
            continue;
        }

        Position const aim(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ() + SRF_SHOT_AIM_HEIGHT);
        if (orb->GetExactDist(aim) > SRF_SHOT_HIT_DISTANCE)
        {
            orb->GetMotionMaster()->MovePoint(1, aim, false);
            ++it;
            continue;
        }

        bool const order = SlotTeam(shot.TurretSlot) == TEAM_ALLIANCE;
        if (Creature* turret = GetBGCreature(shot.TurretSlot, false))
            if (turret->IsAlive())
                turret->CastSpell(target, BG_SRF_SPELL_TURRET_SHOT, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(shot.Damage));
        orb->SummonCreature(order ? BG_SRF_ENTRY_ORDER_SHOT_IMPACT : BG_SRF_ENTRY_CHAOS_SHOT_IMPACT, aim,
            TEMPSUMMON_TIMED_DESPAWN, 450ms);
        orb->DespawnOrUnsummon();
        it = _shots.erase(it);
    }
}

void BattlegroundSRF::DespawnShots()
{
    if (Map* map = FindBgMap())
        for (Shot const& shot : _shots)
            if (Creature* orb = map->GetCreature(shot.Orb))
                orb->DespawnOrUnsummon();
    _shots.clear();
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
    DespawnShots();
    Battleground::EndBattleground(winner);
}
