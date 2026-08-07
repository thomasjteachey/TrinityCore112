/*
 * Violet Hold survival battleground
 * Custom TrinityCore 3.3.5 battleground type 105 (map 1608, cloned from map 608)
 *
 * A party of up to ten queues together and fights waves of "Dark <name>" clones
 * released from the Violet Hold's prison cells. Wave 1 fields as many clones as
 * there are players; every cleared wave adds a QUARTER of a clone, expressed as
 * one extra clone carrying the Diminished debuff at 75, 50 or 25 stacks before
 * it finally joins at full strength on the fourth step. See
 * BG_VHR_QUARTERS_PER_CLONE for the table. Clearing continues until a wave would
 * need more than BG_VHR_MAX_ENEMIES clones, at which point the party has won -
 * which is not expected to happen.
 *
 * Three things make this different from the other custom battlegrounds here:
 *
 * 1. There is no opposing queue. The enemy side is spawned on demand, so the
 *    match pops the instant a group queues and never waits for a second team.
 *
 * 2. It is an arena in the sense that matters: no spirit guides are placed and
 *    map 1608 deliberately has no graveyard rows, so the dead stay dead for the
 *    rest of the run. Between waves the survivors are NOT healed, resurrected,
 *    mana-restored, or given their cooldowns back. Attrition across waves is the
 *    entire difficulty curve, so anything that undoes it has to stay out.
 *
 * 3. The ten-second gap between waves is a preparation window for the clones
 *    only. They get Arena Preparation - which is also what drives the playerbot
 *    self-buff logic in SelectPreparationBuffSpell - while the players get
 *    nothing but a warning about what is about to walk out of the cell.
 *
 * The clones themselves are created by PlayerbotObcCloneManager, which lives in
 * the scripts library and cannot be called from here. This class therefore only
 * decides what a wave should contain and publishes that as a spawn request; the
 * script-side driver polls it and does the summoning.
 */

#ifndef __BATTLEGROUNDVHR_H
#define __BATTLEGROUNDVHR_H

#include "Battleground.h"
#include "BattlegroundScore.h"
#include "Common.h"
#include "Position.h"

#include <vector>

// Blocks of 100 per custom battleground: SCM 9000, BRT 9100, OBC 9200,
// TRT 9300. Matching WorldStateUI rows are required or the client shows
// nothing, however correct the values sent here are.
enum BG_VHR_WorldStates
{
    BG_VHR_WORLDSTATE_SHOW            = 9400,
    BG_VHR_WORLDSTATE_PLAYERS_ALIVE   = 9401,
    BG_VHR_WORLDSTATE_ENEMIES_ALIVE   = 9402,
    BG_VHR_WORLDSTATE_WAVE            = 9403,
    BG_VHR_WORLDSTATE_ENEMIES_MAX     = 9404
};

enum BG_VHR_BroadcastTexts
{
    BG_VHR_TEXT_NEXT_WAVE_IN_TEN_SECONDS = 910079
};

enum BG_VHR_TrinityStrings
{
    // "Next wave in %u..." - formatted per second and sent as a raid-boss
    // emote, which the client draws in the centre of the screen. See
    // sql/custom/world/2026_08_06_04_world_violet_hold_countdown_text.sql.
    BG_VHR_STRING_NEXT_WAVE_COUNTDOWN = 20100
};

enum BG_VHR_Objects
{
    BG_VHR_OBJECT_CELL_XEVOZZ      = 0,
    BG_VHR_OBJECT_CELL_LAVANTHOR   = 1,
    BG_VHR_OBJECT_CELL_ICHORON     = 2,
    BG_VHR_OBJECT_CELL_MORAGG      = 3,
    BG_VHR_OBJECT_CELL_ZURAMAT     = 4,
    BG_VHR_OBJECT_CELL_EREKEM      = 5,
    BG_VHR_OBJECT_CELL_GUARD_LEFT  = 6,
    BG_VHR_OBJECT_CELL_GUARD_RIGHT = 7,
    BG_VHR_OBJECT_CELL_MAX         = 8,

    // The prison seal at the west end of the chamber. Held shut for the whole
    // run so the party cannot retreat up the entrance ramp.
    BG_VHR_OBJECT_MAIN_DOOR        = 8,

    // Reward powerups dropped on the floor when a wave is cleared: one per
    // three players, rounded UP, so a full party of ten needs four slots.
    // Reused every wave - the previous one is deleted and a fresh object
    // created at a newly chosen spot, so these are not fixed positions.
    BG_VHR_OBJECT_BUFF_1           = 9,
    BG_VHR_OBJECT_BUFF_2           = 10,
    BG_VHR_OBJECT_BUFF_3           = 11,
    BG_VHR_OBJECT_BUFF_4           = 12,
    BG_VHR_OBJECT_MAX              = 13
};

enum BG_VHR_Constants
{
    // A wave needing more clones than this ends the run as a win.
    BG_VHR_MAX_ENEMIES = 40,

    // Cells hold at most ten players each in the stock instance, so a late
    // wave has to be spread over several. One cell per ten clones, rounded up.
    BG_VHR_CLONES_PER_CELL = 10,

    // The gates-closed preparation window, in milliseconds.
    BG_VHR_PREP_MS = 10 * IN_MILLISECONDS,

    // Alive counts and the wipe check run on this timer rather than every tick.
    BG_VHR_STATE_CHECK_INTERVAL = 500,

    // Chance per wave of each special composition, in tenths of a percent, so
    // 25 is 2.5%. Rolled against urand(0, 999) in the order declared here.
    BG_VHR_MONO_WAVE_CHANCE_PERMILLE   = 25,
    BG_VHR_ROSTER_WAVE_CHANCE_PERMILLE = 25,

    // How close a player has to be to a cell's release point to be pushed off
    // it when that cell is chosen. Checked once, as the wave is composed.
    BG_VHR_SPAWN_CLEAR_RADIUS = 15,

    // The difficulty curve advances a QUARTER of a clone per wave, not a whole
    // one. Each step either strengthens the wave's partial clone or, on the
    // fourth step, promotes it to a full one and starts a new partial:
    //
    //   wave 1  party                      wave 5  party + 1
    //   wave 2  party + a 25%-power clone   wave 6  party + 1 + a 25% clone
    //   wave 3  party + a 50%-power clone   wave 7  party + 1 + a 50% clone
    //   wave 4  party + a 75%-power clone   ...
    //
    // The partial clone is held back by the Diminished debuff, whose stack
    // count is the percentage taken off its size, health, damage and healing -
    // so a clone at 25% power carries 75 stacks.
    BG_VHR_QUARTERS_PER_CLONE = 4,
    BG_VHR_SPELL_DIMINISHED   = 90201,

    // Clearing a wave drops powerups on the floor: one per this many players,
    // rounded UP, so even a solo run gets one and a full ten gets four.
    //
    //   1-3 players -> 1     4-6 -> 2     7-9 -> 3     10 -> 4
    BG_VHR_PLAYERS_PER_BUFF = 3,
    BG_VHR_MAX_WAVE_BUFFS   = 4,

    // How long a dropped powerup survives before it is taken back, in ms. Waves
    // are composed the moment the previous one dies, so this deliberately
    // outlives the ten-second preparation window - the party can grab one while
    // the next wave is still behind its cell door.
    BG_VHR_BUFF_LIFETIME_MS = 60 * IN_MILLISECONDS,

    // Ideal spacing for a dropped powerup: this far from any living player, so
    // it has to be walked to rather than stood on, and this far from a cell
    // release point so it never lands in a gate that is about to open.
    //
    // These are the FIRST pass only. PickBuffPosition walks a relaxation ladder
    // and gives ground back on each attempt rather than failing - a wave that
    // earns a reward always gets one, even if it ends up in someone's lap.
    BG_VHR_BUFF_MIN_PLAYER_DISTANCE = 20,
    BG_VHR_BUFF_MIN_CELL_DISTANCE = 12,
    BG_VHR_BUFF_MIN_DROP_DISTANCE = 12,

    // The custom cooldown-reset rune, alongside stock Restoration and
    // Berserking. Speed is deliberately not in the roll.
    BG_VHR_GO_RECHARGE_BUFF = 300500
};

// Which characters a wave's clones are copied from. Every wave is made of
// clones either way; these only change how the sources are chosen.
enum class VhrWaveComposition : uint8
{
    // Party flavour: each slot draws its own source from the party at random,
    // so the wave is an uneven mix of the players' own reflections. Rolled at
    // Centurion.VioletHold.PartyWaveChancePercent per wave.
    RandomPerSlot = 0,

    // 2.5% - one player is picked and the whole wave is copies of them.
    // Fifteen Dark Elgroms.
    SingleSource = 1,

    // 2.5% - the living roster in order, repeated until the wave is full, so
    // every player is represented as evenly as the count allows.
    FullRoster = 2,

    // The common case: the wave is drawn from the server's playerbot
    // population instead of the party. sourceGuids still holds party picks as
    // a fallback; the driver swaps in level-appropriate bots when it can find
    // any, and uses the fallback when it cannot (an empty bot roster must
    // never stall a wave).
    BotSourced = 3
};

// Published by the battleground and fulfilled by the script-side clone driver.
// sourceGuids is already expanded to one entry per clone, in spawn order, so
// the driver needs to know nothing about how a composition was chosen -
// except for BotSourced, where it re-sources the same count from the bot
// population using the party level range below to pick fair opponents.
struct VhrWaveSpawnRequest
{
    uint32 waveNumber = 0;
    uint32 enemyTeam = 0;
    VhrWaveComposition composition = VhrWaveComposition::RandomPerSlot;
    uint32 partyMinLevel = 0;
    uint32 partyMaxLevel = 0;
    std::vector<ObjectGuid> sourceGuids;
    std::vector<Position> spawnPositions;

    // Diminished stacks per clone, parallel to sourceGuids. 0 means a clone at
    // full strength, which is every entry except - on three waves in four - the
    // last one. Kept as a per-slot vector rather than a single "the last one is
    // weak" flag so the driver never has to know which slot is special.
    std::vector<uint8> diminishedStacks;
};

struct BattlegroundVHRScore final : public BattlegroundScore
{
    explicit BattlegroundVHRScore(ObjectGuid playerGuid, uint32 scoreboardTeamMarker = 0)
        : BattlegroundScore(playerGuid)
    {
        BonusHonor = scoreboardTeamMarker;
    }

protected:
    void BuildObjectivesBlock(WorldPacket& data) final override;
};

class TC_GAME_API BattlegroundVHR : public Battleground
{
public:
    BattlegroundVHR();
    ~BattlegroundVHR() override = default;

    void AddPlayer(Player* player) override;
    void RemovePlayer(Player* player, ObjectGuid guid, uint32 team) override;
    void Reset() override;
    bool SetupBattleground() override;
    void PostUpdateImpl(uint32 diff) override;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;
    void HandlePlayerResurrect(Player* player) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;

    // Wave-clear rewards belong to the party. The clones are Players on the
    // enemy team and would otherwise walk over them on the way in.
    bool CanPickUpPowerup(Player const* player) const override;
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

    // The enemy side is empty between waves by design; the stock under-population
    // countdown would treat every gap as a team having abandoned the match.
    bool AllowsPrematureFinish() const override { return false; }

    // --- script-side clone driver interface -------------------------------
    // The driver polls this every world tick. A non-null return means the wave
    // has been composed and is waiting to be summoned; the driver spawns one
    // clone per sourceGuids entry at the matching spawnPositions entry and then
    // calls NotifyWaveSpawnFulfilled(). Until it does, the preparation timer
    // does not start, so a slow spawn never eats into the players' warning.
    VhrWaveSpawnRequest const* GetPendingWaveSpawnRequest() const;
    void NotifyWaveSpawnFulfilled(uint32 waveNumber, uint32 spawnedCount);

    // The team the clones fight on. Humans always hold the other one.
    uint32 GetEnemyTeam() const { return _enemyTeam; }
    uint32 GetHumanTeam() const { return _humanTeam; }
    uint32 GetWaveNumber() const { return _waveNumber; }
    bool IsWavePreparing() const { return _waveState == WaveState::Preparing; }

private:
    enum class WaveState : uint8
    {
        NotStarted = 0,   // battleground countdown has not finished yet
        AwaitingSpawn,    // request published, clones not yet in the world
        Preparing,        // cells shut, clones buffing, players warned
        Fighting          // cells open, wave live
    };

    void BeginWave();
    void ComposeWave(uint32 enemyCount);
    void OpenWaveCell();
    void CompleteWave();
    void UpdateScoreWorldStates();
    void ClearPlayersFromSpawn();
    void ApplyPreparationToWave();
    void ReleaseWaveFromPreparation();
    // Centre-screen "Next wave in N..." notification, once per whole second.
    void AnnounceWaveCountdown();
    void SetWaveEnemyImmunity(bool immune);
    void CheckRunState();
    void TeleportSurvivorsToGurubashi();

    uint32 CountAliveHumans() const;
    uint32 CountAliveEnemies() const;
    std::vector<ObjectGuid> GetHumanRoster(bool livingOnly) const;
    uint32 GetEnemyCountForWave(uint32 wave) const;
    // Full-strength clones only, ignoring the partial one.
    uint32 GetFullStrengthCountForWave(uint32 wave) const;
    // Diminished stacks for the wave's partial clone, 0 when the wave is made
    // entirely of full-strength clones (every fourth wave).
    static uint8 GetPartialCloneStacks(uint32 wave);

    // Reward powerups, dropped when a wave is cleared and taken back after
    // BG_VHR_BUFF_LIFETIME_MS.
    void SpawnWaveRewardBuffs();
    void DespawnWaveRewardBuffs();
    bool PickBuffPosition(std::vector<Position> const& taken, Position& out) const;

    uint32 GetHonorRewardForRun() const;
    void ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const override;

    uint32 _humanTeam;
    uint32 _enemyTeam;

    // Party size captured when the gates first open. Wave N fields _partySize
    // full clones plus a quarter of a clone per wave elapsed, so this fixes the
    // whole difficulty curve and must not drift when someone dies or leaves.
    uint32 _partySize;

    // Counts down while dropped powerups are on the floor; 0 means none are
    // out. Reset each time a wave is cleared, so a fresh drop always gets the
    // full lifetime even if the previous one had not expired.
    uint32 _buffLifetimeMs;

    uint32 _waveNumber;
    uint32 _highestWaveCleared;
    WaveState _waveState;
    uint32 _prepTimerMs;
    // Last whole second announced during the prep window, so the countdown
    // fires once per second instead of once per tick.
    uint32 _lastCountdownSecond;
    uint32 _stateCheckTimerMs;

    // Cells in use by the current wave, as BG_VHR_OBJECT_* indices.
    std::vector<uint32> _waveCells;
    // Clones belonging to the current wave, in spawn order.
    std::vector<ObjectGuid> _waveEnemies;
    // Everyone eliminated so far. Counting survivors from the roster minus this
    // set is steadier than asking the object accessor mid-death-transition,
    // which is the same problem Arena solves for its custom-game clones.
    GuidSet _eliminated;

    VhrWaveSpawnRequest _pendingRequest;
    bool _hasPendingRequest;
};

#endif
