#include "BattlegroundSRF.h"

#include "BattlegroundMgr.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
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
struct SrfStructure
{
    BG_SRF_StructureKind kind;
    uint8 lane;
    float x, y, z, o;
};

// League's own placements (GdsMapObject transforms in base.materials.bin, read by
// wow/import/rift/scripts/layout_extract.py), in slot order: per lane the outer,
// inner and inhibitor turret and the inhibitor, then the two nexus turrets and
// the nexus. League names: Turret_T1_L_03 / L_02 / C_06, Barracks_T1_L1 for
// Order's top lane, and so on.
SrfStructure constexpr SRF_STRUCTURES[PVP_TEAMS_COUNT][BG_SRF_STRUCTURES_PER_TEAM] =
{
    { // Order (Alliance), the south-west base
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,   60.820f,  128.380f, 0.740f, 6.1959f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,  -14.020f,  117.760f, 0.840f, 6.2657f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,  -62.260f,  124.620f, 1.815f, 0.0524f },
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_TOP,  -76.536f,  124.614f, 1.895f, 1.5708f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,  -20.080f,   31.080f, 0.880f, 5.4629f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,  -51.760f,   47.040f, 0.860f, 5.5676f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,  -74.080f,   74.980f, 1.820f, 5.4978f },
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_MID,  -83.840f,   83.940f, 1.840f, 0.7854f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT, -127.420f,  -62.080f, 0.820f, 4.7085f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT, -118.340f,    9.620f, 0.860f, 4.6949f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT, -122.940f,   62.380f, 1.720f, 4.6251f },
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_BOT, -123.165f,   78.902f, 1.780f, 0.0000f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE, -102.600f, 113.040f, 1.760f, 5.7596f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE, -111.860f, 104.460f, 1.780f, 5.2360f },
        { BG_SRF_KIND_NEXUS,     BG_SRF_LANE_NONE, -114.820f, 116.980f, 1.720f, 0.7854f }
    },
    { // Chaos (Horde), the north-east base
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,  129.500f,   61.640f, 0.760f, 2.0071f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,  120.220f,  -10.860f, 0.760f, 1.6598f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_TOP,  125.000f,  -61.620f, 1.760f, 1.6252f },
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_TOP,  125.199f,  -77.220f, 1.760f, 4.7124f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   22.200f,  -31.100f, 0.760f, 2.2965f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   54.260f,  -47.340f, 0.760f, 2.3562f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_MID,   76.140f,  -74.680f, 1.760f, 2.3562f },
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_MID,   85.340f,  -84.061f, 1.780f, 5.4978f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,  -57.900f, -129.320f, 0.760f, 3.1174f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,   16.520f, -118.540f, 0.760f, 3.1416f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_BOT,   63.440f, -124.480f, 1.760f, 3.1416f },
        { BG_SRF_KIND_INHIBITOR, BG_SRF_LANE_BOT,   78.323f, -123.963f, 1.780f, 0.0000f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE,  104.240f, -113.040f, 1.760f, 2.6005f },
        { BG_SRF_KIND_TURRET,    BG_SRF_LANE_NONE,  113.680f, -104.220f, 1.760f, 2.0769f },
        { BG_SRF_KIND_NEXUS,     BG_SRF_LANE_NONE,  116.700f, -116.860f, 1.820f, 0.0000f }
    }
};

uint8 constexpr SRF_SLOT_NEXUS_TURRET_1 = 12;
uint8 constexpr SRF_SLOT_NEXUS_TURRET_2 = 13;
uint8 constexpr SRF_SLOT_NEXUS          = 14;

struct SrfPoint
{
    float x, y;
};

// League's __NAV_<lane>02..17 lane markers, Order's end first. Chaos minions
// walk the same list backwards.
SrfPoint constexpr SRF_LANE_TOP[] =
{
    { -76.06f, 117.84f }, { -49.80f, 120.36f }, { -13.70f, 124.60f }, {   7.74f, 123.86f },
    {  33.44f, 123.42f }, {  60.46f, 122.30f }, {  84.02f, 117.84f }, { 101.18f, 108.64f },
    { 114.00f,  93.60f }, { 121.46f,  78.28f }, { 123.94f,  60.82f }, { 124.16f,  37.92f },
    { 124.94f,  14.64f }, { 125.30f, -14.26f }, { 120.94f, -49.78f }, { 118.88f, -78.96f }
};
SrfPoint constexpr SRF_LANE_MID[] =
{
    { -78.52f,  88.96f }, { -64.60f,  70.88f }, { -45.18f,  50.52f }, { -36.22f,  40.50f },
    { -29.44f,  31.54f }, { -20.68f,  22.60f }, { -10.72f,  12.72f }, {   0.10f,  -0.16f },
    {  12.44f, -13.58f }, {  24.94f, -24.36f }, {  34.10f, -34.98f }, {  42.86f, -44.50f },
    {  52.70f, -55.86f }, {  64.92f, -68.44f }, {  81.58f, -88.16f }
};
SrfPoint constexpr SRF_LANE_BOT[] =
{
    { -116.22f,  78.58f }, { -119.20f,  51.12f }, { -124.74f,   9.50f }, { -123.40f, -11.50f },
    { -123.76f, -29.40f }, { -121.98f, -62.12f }, { -118.56f, -80.64f }, { -109.74f, -97.88f },
    {  -95.44f, -112.02f }, { -74.12f, -122.58f }, { -52.20f, -127.02f }, { -31.78f, -126.82f },
    {   -4.46f, -126.20f }, {  24.16f, -125.92f }, {  52.80f, -119.28f }, {  78.68f, -117.06f }
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
    { { -106.18f,  125.82f }, { -106.42f,  107.84f }, { -124.58f,  107.32f } },
    { {  126.90f, -108.00f }, {  107.68f, -107.52f }, {  108.90f, -126.38f } }
};

// The fountains (__Spawn_T1 / T2). The same spots are WorldSafeLocs 52710/52711.
Position const SRF_FOUNTAIN[PVP_TEAMS_COUNT] =
{
    { -138.78f,  140.12f, 3.42f, 5.4978f },
    {  139.82f, -138.80f, 3.58f, 2.3562f }
};

// Lanes and fountains sit within a couple of yards of this height; the path
// generator snaps a point onto the navmesh below it.
float constexpr SRF_LANE_Z = 1.0f;

float constexpr SRF_TURRET_RANGE        = 15.0f;   // League's 750 units
float constexpr SRF_MINION_AGGRO_RANGE  = 8.0f;
float constexpr SRF_WAYPOINT_REACHED    = 3.0f;
float constexpr SRF_FOUNTAIN_HEAL_RANGE = 12.0f;
float constexpr SRF_FOUNTAIN_BURN_RANGE = 14.0f;
// Turret shots as shares of the target's maximum health: minions die to a few
// shots, and a player who stays under a turret takes a little more every shot.
float constexpr SRF_TURRET_MINION_SHARE = 0.45f;
float constexpr SRF_TURRET_PLAYER_SHARE = 0.20f;
float constexpr SRF_TURRET_PLAYER_RAMP  = 0.10f;
float constexpr SRF_TURRET_PLAYER_MAX   = 0.60f;
// A shot leaves from the turret's crystal and flies at League's 1200 units a
// second, homing on its target; it lands when it reaches the target's chest.
float constexpr SRF_SHOT_ORIGIN_HEIGHT  = 9.5f;
float constexpr SRF_SHOT_SPEED          = 24.0f;
float constexpr SRF_SHOT_HIT_DISTANCE   = 1.5f;
float constexpr SRF_SHOT_AIM_HEIGHT     = 1.2f;
float constexpr SRF_FOUNTAIN_HEAL_SHARE = 0.10f;
float constexpr SRF_FOUNTAIN_BURN_SHARE = 0.30f;

TeamId SlotTeam(uint8 slot) { return slot < BG_SRF_STRUCTURES_PER_TEAM ? TEAM_ALLIANCE : TEAM_HORDE; }
uint8 SlotIndex(uint8 slot) { return slot % BG_SRF_STRUCTURES_PER_TEAM; }
uint8 SlotOf(TeamId team, uint8 index) { return uint8(team) * BG_SRF_STRUCTURES_PER_TEAM + index; }
SrfStructure const& StructureAt(uint8 slot) { return SRF_STRUCTURES[SlotTeam(slot)][SlotIndex(slot)]; }
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

// The lane's index-th point in the team's walking direction, then the enemy nexus.
Position LanePoint(TeamId team, uint8 lane, uint8 index)
{
    SrfLane const& l = SRF_LANES[lane];
    if (index >= l.count)
    {
        SrfStructure const& nexus = SRF_STRUCTURES[OtherTeam(team)][SRF_SLOT_NEXUS];
        return Position(nexus.x, nexus.y, SRF_LANE_Z);
    }
    SrfPoint const& p = l.points[team == TEAM_ALLIANCE ? index : l.count - 1 - index];
    return Position(p.x, p.y, SRF_LANE_Z);
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
        if (!AddSpiritGuide(spirit.type, loc->Loc.X, loc->Loc.Y, loc->Loc.Z, SRF_FOUNTAIN[spirit.team].GetOrientation(), spirit.team))
            return false;
    }

    RefreshVulnerability();
    return true;
}

bool BattlegroundSRF::SpawnStructure(uint8 slot)
{
    SrfStructure const& s = StructureAt(slot);
    DelCreature(slot);
    if (!AddCreature(StructureEntry(slot), slot, s.x, s.y, s.z, s.o, SlotTeam(slot)))
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
            SrfPoint const& gate = SRF_BARRACKS[team][lane];
            Position const first = LanePoint(team, lane, 0);
            float const facing = std::atan2(first.GetPositionY() - gate.y, first.GetPositionX() - gate.x);
            for (uint8 i = 0; i < BG_SRF_MELEE_PER_WAVE + BG_SRF_CASTERS_PER_WAVE; ++i)
            {
                // a small block, melee in front, so the wave does not spawn stacked
                float const ox = float(i % 3) - 1.0f, oy = float(i / 3);
                Position const pos(gate.x + ox * 1.5f, gate.y + oy * 1.5f, SRF_LANE_Z + 1.5f, facing);
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

        Position dest = LanePoint(m.Team, m.Lane, m.Waypoint);
        while (m.Waypoint < SRF_LANES[m.Lane].count && creature->GetExactDist2d(dest) < SRF_WAYPOINT_REACHED)
            dest = LanePoint(m.Team, m.Lane, ++m.Waypoint);

        if (!m.Walking || creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            creature->GetMotionMaster()->MovePoint(1, dest);
            m.Walking = true;
        }
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
        if (player->GetExactDist2d(SRF_FOUNTAIN[team]) <= SRF_FOUNTAIN_HEAL_RANGE)
        {
            player->ModifyHealth(int32(player->GetMaxHealth() * SRF_FOUNTAIN_HEAL_SHARE));
            Powers const power = player->GetPowerType();
            player->ModifyPower(power, int32(player->GetMaxPower(power) * SRF_FOUNTAIN_HEAL_SHARE));
        }
        else if (player->GetExactDist2d(SRF_FOUNTAIN[OtherTeam(team)]) <= SRF_FOUNTAIN_BURN_RANGE)
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
    Position const& fountain = SRF_FOUNTAIN[team];
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
