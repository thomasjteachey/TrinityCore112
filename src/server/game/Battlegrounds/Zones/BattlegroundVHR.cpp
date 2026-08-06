#include "BattlegroundVHR.h"

#include "BattlegroundMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldStatePackets.h"

#include <algorithm>
#include <cmath>

namespace
{
// Cell geometry, taken from the live map 608 gameobject spawns and the boss
// walk-out paths in instance_violet_hold.cpp. The door is where the gate object
// sits; the release point is the first node of that boss's path, which is the
// spot inside the cell the occupant stands on. Spawning on the release point
// rather than the door keeps clones behind the gate while it is shut.
struct VhrCell
{
    uint32 doorEntry;
    Position door;
    float doorRotation2;
    float doorRotation3;
    Position release;
};

VhrCell const kCells[BG_VHR_OBJECT_CELL_MAX] =
{
    // Xevozz - north-east
    { 191556, { 1908.06f, 844.885f, 41.1377f, -0.444615f }, -0.220481f, 0.975391f, { 1908.417f, 845.8502f, 38.71947f, 3.560472f } },
    // Lavanthor - south-west
    { 191566, { 1847.81f, 752.476f, 49.3023f,  2.44346f  },  0.939692f, 0.342021f, { 1844.557f, 748.7083f, 38.74205f, 0.907571f } },
    // Ichoron - south-east
    { 191722, { 1938.43f, 754.695f, 28.7801f,  2.37934f  },  0.928246f, 0.371966f, { 1942.041f, 749.5228f, 30.95229f, 2.251475f } },
    // Moragg - due south
    { 191606, { 1895.07f, 733.715f, 57.6715f,  3.14159f  },  1.0f,      0.000001f, { 1893.895f, 728.1261f, 47.75016f, 1.675516f } },
    // Zuramat - far north-east
    { 191565, { 1931.87f, 859.01f,  54.923f,  -0.698129f }, -0.342019f, 0.939693f, { 1934.151f, 860.9463f, 47.29499f, 3.961897f } },
    // Erekem - north
    { 191564, { 1872.45f, 868.998f, 47.6405f,  0.289969f },  0.144477f, 0.989508f, { 1871.456f, 871.0361f, 43.41524f, 4.345870f } },
    // Erekem's left guard - north-west
    { 191562, { 1854.57f, 860.96f,  47.6405f,  0.546741f },  0.269978f, 0.962866f, { 1853.752f, 862.4528f, 43.41614f, 4.345870f } },
    // Erekem's right guard - north
    { 191563, { 1892.01f, 871.239f, 47.6405f, -0.05735f  }, -0.028671f, 0.999589f, { 1892.418f, 872.2831f, 43.41563f, 4.345870f } }
};

// The prison seal at the west end. Kept shut for the whole run.
constexpr uint32 kMainDoorEntry = 191723;
Position const kMainDoor = { 1822.59f, 803.928f, 44.3647f, 3.14159f };

// Where the party is put down, just inside the seal on the entrance landing.
Position const kPlayerStart = { 1848.03f, 804.62f, 44.07f, 0.027476f };

// Middle of the chamber floor, below the ramp. Anyone loitering on a cell that
// is about to open gets moved here - it is the furthest point from every cell.
Position const kChamberCentre = { 1886.251f, 803.0743f, 38.42326f, 3.211406f };

// Gurubashi arena, where a wiped party is returned to. Kept in step with
// GurubashiGamesmasterLocation in scripts/Custom/custom_game_lobby.cpp, which
// is the same spot every other custom mode drops players back onto.
WorldLocation const kGurubashiReturn(0, -13235.707031f, 214.336441f, 31.276190f, 1.010225f);

// Clones are placed in a ring around the cell's release point so forty of them
// do not end up inside one another.
constexpr float kCloneRingSpacing = 1.6f;

Position ScatterInCell(Position const& release, uint32 indexInCell)
{
    if (!indexInCell)
        return release;

    // Concentric rings of six, eight, ten... starting one spacing out.
    uint32 ring = 1;
    uint32 consumed = 0;
    uint32 slotsInRing = 6;
    while (consumed + slotsInRing < indexInCell)
    {
        consumed += slotsInRing;
        ++ring;
        slotsInRing += 2;
    }

    uint32 const slot = indexInCell - consumed - 1;
    float const angle = (2.0f * float(M_PI) * float(slot)) / float(slotsInRing);
    float const radius = kCloneRingSpacing * float(ring);

    return { release.GetPositionX() + radius * std::cos(angle),
             release.GetPositionY() + radius * std::sin(angle),
             release.GetPositionZ(),
             release.GetOrientation() };
}
}

void BattlegroundVHRScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // no extra custom scoreboard columns
}

BattlegroundVHR::BattlegroundVHR()
{
    BgObjects.resize(BG_VHR_OBJECT_MAX);

    // Arena-style pre-match countdown, not the one-minute battleground stock:
    // the whole party arrives together from one queue group, so there is
    // nobody to wait for.
    StartDelayTimes[BG_STARTING_EVENT_FIRST]  = BG_START_DELAY_30S;
    StartDelayTimes[BG_STARTING_EVENT_SECOND] = BG_START_DELAY_15S;
    StartDelayTimes[BG_STARTING_EVENT_THIRD]  = BG_START_DELAY_10S;
    StartDelayTimes[BG_STARTING_EVENT_FOURTH] = BG_START_DELAY_NONE;

    // Humans always hold Alliance. The queue forces this side regardless of the
    // player's real faction, so a cross-faction party stays together and the
    // clones always have the other side to themselves.
    _humanTeam = ALLIANCE;
    _enemyTeam = HORDE;

    _partySize = 0;
    _waveNumber = 0;
    _highestWaveCleared = 0;
    _waveState = WaveState::NotStarted;
    _prepTimerMs = 0;
    _stateCheckTimerMs = 0;
    _hasPendingRequest = false;
}

void BattlegroundVHR::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());

    Battleground::AddPlayer(player);

    if (!isInBattleground)
    {
        uint32 const scoreboardTeamMarker = (player->GetBGTeam() == HORDE) ? 1u : 0u;
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundVHRScore(player->GetGUID(), scoreboardTeamMarker);
    }

    // A clone summoned into a live wave must not inherit the run's earlier
    // eliminations if its GUID is ever recycled.
    _eliminated.erase(player->GetGUID());

    UpdateScoreWorldStates();
}

void BattlegroundVHR::RemovePlayer(Player* /*player*/, ObjectGuid guid, uint32 /*team*/)
{
    // Leaving is indistinguishable from dying as far as the run is concerned:
    // either way that character is no longer holding the line.
    _eliminated.insert(guid);
    UpdateScoreWorldStates();
}

void BattlegroundVHR::Reset()
{
    Battleground::Reset();

    _partySize = 0;
    _waveNumber = 0;
    _highestWaveCleared = 0;
    _waveState = WaveState::NotStarted;
    _prepTimerMs = 0;
    _stateCheckTimerMs = 0;
    _waveCells.clear();
    _waveEnemies.clear();
    _eliminated.clear();
    _pendingRequest = VhrWaveSpawnRequest();
    _hasPendingRequest = false;
}

bool BattlegroundVHR::SetupBattleground()
{
    for (uint32 i = 0; i < BG_VHR_OBJECT_CELL_MAX; ++i)
    {
        VhrCell const& cell = kCells[i];
        if (!AddObject(i, cell.doorEntry, cell.door.GetPositionX(), cell.door.GetPositionY(), cell.door.GetPositionZ(),
            cell.door.GetOrientation(), 0.0f, 0.0f, cell.doorRotation2, cell.doorRotation3, RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundVHR::SetupBattleground: failed to spawn cell door {} (entry {}).", i, cell.doorEntry);
            return false;
        }
    }

    if (!AddObject(BG_VHR_OBJECT_MAIN_DOOR, kMainDoorEntry, kMainDoor.GetPositionX(), kMainDoor.GetPositionY(),
        kMainDoor.GetPositionZ(), kMainDoor.GetOrientation(), 0.0f, 0.0f, 1.0f, 0.000001f, RESPAWN_IMMEDIATELY))
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundVHR::SetupBattleground: failed to spawn the prison seal (entry {}).", kMainDoorEntry);
        return false;
    }

    // Deliberately no AddSpiritGuide call. Map 1608 also ships without
    // graveyard rows, so a released ghost has nowhere to resurrect: death is
    // final for the rest of the run, which is the whole point of the mode.
    return true;
}

void BattlegroundVHR::StartingEventCloseDoors()
{
    for (uint32 i = 0; i < BG_VHR_OBJECT_CELL_MAX; ++i)
        DoorClose(i);

    DoorClose(BG_VHR_OBJECT_MAIN_DOOR);
}

void BattlegroundVHR::StartingEventOpenDoors()
{
    // The prison seal stays shut - there is nowhere to go but the chamber.
    // Only the cells are managed from here, and they open one wave at a time.

    // Fix the difficulty curve to whoever actually made it to the start.
    _partySize = CountAliveHumans();
    if (!_partySize)
        _partySize = 1;

    BeginWave();
}

uint32 BattlegroundVHR::GetEnemyCountForWave(uint32 wave) const
{
    if (!wave)
        return 0;

    return _partySize + wave - 1;
}

void BattlegroundVHR::BeginWave()
{
    uint32 const nextWave = _waveNumber + 1;
    uint32 const enemyCount = GetEnemyCountForWave(nextWave);

    // Surviving a wave that would need more than the cap is the win condition.
    if (enemyCount > BG_VHR_MAX_ENEMIES)
    {
        EndBattleground(_humanTeam);
        return;
    }

    _waveNumber = nextWave;
    _waveEnemies.clear();

    ComposeWave(enemyCount);

    _waveState = WaveState::AwaitingSpawn;
    _prepTimerMs = BG_VHR_PREP_MS;

    for (auto const& itr : GetPlayers())
        if (Player* player = ObjectAccessor::FindPlayer(itr.first))
            if (player->GetBGTeam() == _humanTeam && player->GetSession())
                player->GetSession()->SendAreaTriggerMessage("Wave %u - %u incoming!", _waveNumber, enemyCount);

    UpdateScoreWorldStates();
}

// Decide who this wave is made of and where each clone is put down, then
// publish it for the script-side driver. Nothing is summoned here.
void BattlegroundVHR::ComposeWave(uint32 enemyCount)
{
    _pendingRequest = VhrWaveSpawnRequest();
    _pendingRequest.waveNumber = _waveNumber;
    _pendingRequest.enemyTeam = _enemyTeam;

    // Sources are drawn from the living. A dead player's likeness should not
    // keep walking out of the cells after they have been eliminated.
    std::vector<ObjectGuid> roster = GetHumanRoster(true);
    if (roster.empty())
    {
        // Everyone is already down; the wipe check will end the run shortly.
        _hasPendingRequest = false;
        return;
    }

    // The party's level range rides along so the bot driver can pick fair
    // opponents for BotSourced waves.
    uint32 minLevel = 0, maxLevel = 0;
    for (ObjectGuid guid : roster)
        if (Player const* member = ObjectAccessor::FindPlayer(guid))
        {
            uint32 const level = member->GetLevel();
            if (!minLevel || level < minLevel)
                minLevel = level;
            if (level > maxLevel)
                maxLevel = level;
        }
    _pendingRequest.partyMinLevel = minLevel;
    _pendingRequest.partyMaxLevel = maxLevel;

    // Party-mirror waves are the seasoning, not the meal: most waves are
    // drawn from the playerbot population, with the party flavours rolled
    // first so they keep their exact advertised odds.
    VhrWaveComposition composition = VhrWaveComposition::BotSourced;
    if (urand(0, 999) < BG_VHR_MONO_WAVE_CHANCE_PERMILLE)
        composition = VhrWaveComposition::SingleSource;
    else if (urand(0, 999) < BG_VHR_ROSTER_WAVE_CHANCE_PERMILLE)
        composition = VhrWaveComposition::FullRoster;
    else if (urand(0, 99) < sWorld->getIntConfig(CONFIG_CENTURION_VHR_PARTY_WAVE_CHANCE))
        composition = VhrWaveComposition::RandomPerSlot;

    _pendingRequest.composition = composition;
    _pendingRequest.sourceGuids.reserve(enemyCount);

    switch (composition)
    {
        case VhrWaveComposition::SingleSource:
        {
            // One unlucky player, over and over.
            ObjectGuid const source = roster[urand(0, uint32(roster.size()) - 1)];
            _pendingRequest.sourceGuids.assign(enemyCount, source);
            break;
        }
        case VhrWaveComposition::FullRoster:
        {
            // The whole party in order, looped until the wave is full, so
            // everyone is represented as evenly as the count allows.
            for (uint32 i = 0; i < enemyCount; ++i)
                _pendingRequest.sourceGuids.push_back(roster[i % roster.size()]);
            break;
        }
        case VhrWaveComposition::RandomPerSlot:
        case VhrWaveComposition::BotSourced: // party picks are only the fallback
        default:
        {
            for (uint32 i = 0; i < enemyCount; ++i)
                _pendingRequest.sourceGuids.push_back(roster[urand(0, uint32(roster.size()) - 1)]);
            break;
        }
    }

    // Spread the wave over as many cells as it needs, starting from a different
    // cell each wave so consecutive waves do not pour out of the same door.
    uint32 const cellsNeeded = std::min<uint32>(BG_VHR_OBJECT_CELL_MAX,
        (enemyCount + BG_VHR_CLONES_PER_CELL - 1) / BG_VHR_CLONES_PER_CELL);

    _waveCells.clear();
    for (uint32 i = 0; i < cellsNeeded; ++i)
        _waveCells.push_back((_waveNumber - 1 + i) % BG_VHR_OBJECT_CELL_MAX);

    ClearPlayersFromSpawn();

    _pendingRequest.spawnPositions.reserve(enemyCount);
    std::vector<uint32> perCellCount(cellsNeeded, 0);
    for (uint32 i = 0; i < enemyCount; ++i)
    {
        uint32 const cellSlot = i % cellsNeeded;
        VhrCell const& cell = kCells[_waveCells[cellSlot]];
        _pendingRequest.spawnPositions.push_back(ScatterInCell(cell.release, perCellCount[cellSlot]++));
    }

    for (uint32 cellIndex : _waveCells)
        DoorClose(cellIndex);

    _hasPendingRequest = true;
}

// Anyone standing on a cell that is about to release is moved to the middle of
// the chamber. Checked once per wave, as the cells are chosen.
void BattlegroundVHR::ClearPlayersFromSpawn()
{
    float const radiusSq = float(BG_VHR_SPAWN_CLEAR_RADIUS) * float(BG_VHR_SPAWN_CLEAR_RADIUS);

    for (auto const& itr : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(itr.first);
        if (!player || player->GetBGTeam() != _humanTeam || player->IsBeingTeleported())
            continue;

        for (uint32 cellIndex : _waveCells)
        {
            Position const& release = kCells[cellIndex].release;
            float const dx = player->GetPositionX() - release.GetPositionX();
            float const dy = player->GetPositionY() - release.GetPositionY();
            float const dz = player->GetPositionZ() - release.GetPositionZ();

            if ((dx * dx + dy * dy + dz * dz) > radiusSq)
                continue;

            player->TeleportTo(GetMapId(), kChamberCentre.GetPositionX(), kChamberCentre.GetPositionY(),
                kChamberCentre.GetPositionZ(), kChamberCentre.GetOrientation());
            break;
        }
    }
}

VhrWaveSpawnRequest const* BattlegroundVHR::GetPendingWaveSpawnRequest() const
{
    return _hasPendingRequest ? &_pendingRequest : nullptr;
}

void BattlegroundVHR::NotifyWaveSpawnFulfilled(uint32 waveNumber, uint32 /*spawnedCount*/)
{
    // A late callback from a wave that has already been cleared must not
    // restart the preparation window.
    if (!_hasPendingRequest || waveNumber != _waveNumber)
        return;

    _hasPendingRequest = false;

    // Record the wave's clones by reading the roster back out: the driver
    // spawned them straight into this battleground, so they are already in
    // m_Players on the enemy side.
    _waveEnemies.clear();
    for (auto const& itr : GetPlayers())
        if (itr.second.Team == _enemyTeam && _eliminated.find(itr.first) == _eliminated.end())
            _waveEnemies.push_back(itr.first);

    if (_waveNumber == 1)
    {
        // The match countdown was the first wave's warning; making the party
        // stand through a second ten-second wait on top of it kills the
        // opening. The cells open the moment the clones exist, unbuffed -
        // wave one is the easiest fight of the run anyway.
        OpenWaveCell();
        _waveState = WaveState::Fighting;
    }
    else
    {
        ApplyPreparationToWave();
        _waveState = WaveState::Preparing;
        _prepTimerMs = BG_VHR_PREP_MS;
    }

    UpdateScoreWorldStates();
}

// The clones' ten seconds. Arena Preparation is what the playerbot core keys
// its pre-gates buffing off (SelectPreparationBuffSpell), so applying it here
// is what makes a wave walk out already buffed, exactly as at an arena start.
void BattlegroundVHR::ApplyPreparationToWave()
{
    for (ObjectGuid guid : _waveEnemies)
    {
        Player* enemy = ObjectAccessor::FindPlayer(guid);
        if (!enemy)
            continue;

        enemy->CastSpell(enemy, SPELL_ARENA_PREPARATION, true);
        enemy->ResetAllPowers();
    }

    SetWaveEnemyImmunity(true);
}

void BattlegroundVHR::ReleaseWaveFromPreparation()
{
    for (ObjectGuid guid : _waveEnemies)
    {
        Player* enemy = ObjectAccessor::FindPlayer(guid);
        if (!enemy)
            continue;

        enemy->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
        enemy->RemoveAurasDueToSpell(SPELL_INSTANT_CAST);
        enemy->ResetAllPowers();
    }

    SetWaveEnemyImmunity(false);
}

// Nothing may reach through a shut cell in either direction. The gate is solid,
// but area effects and targeting are not stopped by geometry alone.
void BattlegroundVHR::SetWaveEnemyImmunity(bool immune)
{
    for (ObjectGuid guid : _waveEnemies)
    {
        Player* enemy = ObjectAccessor::FindPlayer(guid);
        if (!enemy)
            continue;

        enemy->SetImmuneToAll(immune);

        if (immune)
            enemy->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE));
        else
            enemy->RemoveUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE));
    }
}

void BattlegroundVHR::OpenWaveCell()
{
    for (uint32 cellIndex : _waveCells)
        DoorOpen(cellIndex);

    ReleaseWaveFromPreparation();
}

void BattlegroundVHR::CompleteWave()
{
    _highestWaveCleared = _waveNumber;
    BeginWave();
}

void BattlegroundVHR::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    switch (_waveState)
    {
        case WaveState::AwaitingSpawn:
            // Waiting on the script-side driver. If the roster was empty when
            // the wave was composed there is nothing coming, so fall through to
            // the wipe check below.
            break;
        case WaveState::Preparing:
            if (_prepTimerMs <= diff)
            {
                _prepTimerMs = 0;
                OpenWaveCell();
                _waveState = WaveState::Fighting;
            }
            else
                _prepTimerMs -= diff;
            break;
        default:
            break;
    }

    _stateCheckTimerMs += diff;
    if (_stateCheckTimerMs < BG_VHR_STATE_CHECK_INTERVAL)
        return;

    _stateCheckTimerMs = 0;
    CheckRunState();
    UpdateScoreWorldStates();
}

void BattlegroundVHR::CheckRunState()
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    // The party is gone: the run is over regardless of what the wave is doing.
    if (!CountAliveHumans())
    {
        EndBattleground(_enemyTeam);
        return;
    }

    // A wave is only cleared once it is actually on the field. Checking during
    // AwaitingSpawn would see an empty enemy side and skip straight past it.
    if (_waveState == WaveState::Fighting && !CountAliveEnemies())
        CompleteWave();
}

uint32 BattlegroundVHR::CountAliveHumans() const
{
    uint32 alive = 0;
    for (auto const& itr : GetPlayers())
    {
        if (itr.second.Team != _humanTeam || itr.second.OfflineRemoveTime)
            continue;

        if (_eliminated.find(itr.first) != _eliminated.end())
            continue;

        ++alive;
    }

    return alive;
}

uint32 BattlegroundVHR::CountAliveEnemies() const
{
    uint32 alive = 0;
    for (auto const& itr : GetPlayers())
    {
        if (itr.second.Team != _enemyTeam || itr.second.OfflineRemoveTime)
            continue;

        if (_eliminated.find(itr.first) != _eliminated.end())
            continue;

        ++alive;
    }

    return alive;
}

std::vector<ObjectGuid> BattlegroundVHR::GetHumanRoster(bool livingOnly) const
{
    std::vector<ObjectGuid> roster;

    for (auto const& itr : GetPlayers())
    {
        if (itr.second.Team != _humanTeam || itr.second.OfflineRemoveTime)
            continue;

        if (livingOnly && _eliminated.find(itr.first) != _eliminated.end())
            continue;

        // Clones are copied from real characters only; a clone of a clone would
        // compound any drift in the copy.
        Player const* player = ObjectAccessor::FindPlayer(itr.first);
        if (!player || !player->GetSession() || player->GetSession()->IsTransientPlayerSession())
            continue;

        roster.push_back(itr.first);
    }

    return roster;
}

void BattlegroundVHR::HandleKillPlayer(Player* victim, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !victim)
        return;

    _eliminated.insert(victim->GetGUID());

    Battleground::HandleKillPlayer(victim, killer);

    UpdateScoreWorldStates();
    CheckRunState();
}

void BattlegroundVHR::UpdateScoreWorldStates()
{
    UpdateWorldState(BG_VHR_WORLDSTATE_SHOW, 1);
    UpdateWorldState(BG_VHR_WORLDSTATE_PLAYERS_ALIVE, CountAliveHumans());
    UpdateWorldState(BG_VHR_WORLDSTATE_ENEMIES_ALIVE, CountAliveEnemies());
    UpdateWorldState(BG_VHR_WORLDSTATE_WAVE, _waveNumber);
    UpdateWorldState(BG_VHR_WORLDSTATE_ENEMIES_MAX, BG_VHR_MAX_ENEMIES);
}

void BattlegroundVHR::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_VHR_WORLDSTATE_SHOW, 1);
    packet.Worldstates.emplace_back(BG_VHR_WORLDSTATE_PLAYERS_ALIVE, CountAliveHumans());
    packet.Worldstates.emplace_back(BG_VHR_WORLDSTATE_ENEMIES_ALIVE, CountAliveEnemies());
    packet.Worldstates.emplace_back(BG_VHR_WORLDSTATE_WAVE, _waveNumber);
    packet.Worldstates.emplace_back(BG_VHR_WORLDSTATE_ENEMIES_MAX, BG_VHR_MAX_ENEMIES);
}

// No graveyards are defined for map 1608 and no spirit guide is placed, so this
// only ever answers the "where is my corpse" question with the start landing.
// Returning a location does not make anyone resurrectable.
WorldSafeLocsEntry const* BattlegroundVHR::GetClosestGraveyard(Player* /*player*/)
{
    return nullptr;
}

bool BattlegroundVHR::HandlePlayerUnderMap(Player* player)
{
    if (!player)
        return false;

    player->TeleportTo(GetMapId(), kChamberCentre.GetPositionX(), kChamberCentre.GetPositionY(),
        kChamberCentre.GetPositionZ(), kChamberCentre.GetOrientation());
    return true;
}

// Honor is the standard arena win payout, multiplied by the deepest wave the
// party fully cleared, compounded ten percent per wave on top. Wiping on wave
// one pays nothing: the multiplier is waves *cleared*, not waves reached.
uint32 BattlegroundVHR::GetHonorRewardForRun() const
{
    if (!_highestWaveCleared)
        return 0;

    // Mirrors the arena branch of Battleground::EndBattleground so the base
    // tracks the same configuration the real arenas pay out on.
    float const arenaMultiplier = sWorld->getFloatConfig(CONFIG_CENTURION_BG_ARENA_REWARD_MULTIPLIER);
    uint32 const arenaWinHonor = uint32((sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_WINNER)
        + sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) * 3) * arenaMultiplier);

    double const compounded = std::pow(1.10, double(_highestWaveCleared) - 1.0);
    return uint32(double(arenaWinHonor) * double(_highestWaveCleared) * compounded);
}

void BattlegroundVHR::ModifyEndOfMatchHonorRewards(uint32 /*winner*/, uint32 team, uint32& winnerHonor, uint32& loserHonor) const
{
    // The clones are transient sessions and are skipped by the reward loop
    // anyway; only the party is paid, and the same amount whether the run ended
    // in a wipe or the improbable clear.
    if (team != _humanTeam)
    {
        winnerHonor = 0;
        loserHonor = 0;
        return;
    }

    uint32 const reward = GetHonorRewardForRun();
    winnerHonor = reward;
    loserHonor = reward;
}

// Rewrite where leaving this battleground drops the party. The base exit path
// reads the stored entry point in RemovePlayerAtLeave, well after this runs, so
// overriding it here is enough - no second teleport, and the usual mount, taxi
// and group restores still happen on the way out.
void BattlegroundVHR::TeleportSurvivorsToGurubashi()
{
    for (auto const& itr : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(itr.first);
        if (!player || player->GetBGTeam() != _humanTeam)
            continue;

        WorldSession const* session = player->GetSession();
        if (!session || session->IsTransientPlayerSession())
            continue;

        player->SetBattlegroundEntryPoint(kGurubashiReturn);
    }
}

void BattlegroundVHR::EndBattleground(uint32 winner)
{
    UpdateScoreWorldStates();

    // Only a wipe sends the party to Gurubashi. Surviving all forty leaves the
    // normal entry point alone, so a winning party returns where they queued.
    if (winner == _enemyTeam)
        TeleportSurvivorsToGurubashi();

    Battleground::EndBattleground(winner);
}
