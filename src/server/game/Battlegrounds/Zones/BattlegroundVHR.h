/*
 * Violet Hold survival battleground
 * Custom TrinityCore 3.3.5 battleground type 105 (map 1608, cloned from map 608)
 *
 * A party of up to ten queues together and fights waves of "Dark <name>" clones
 * released from the Violet Hold's prison cells. Wave 1 fields as many clones as
 * there are players, and the wave grows in two phases. Through
 * BG_VHR_QUARTER_RAMP_LAST_WAVE every cleared wave adds a QUARTER of a clone,
 * expressed as one extra clone carrying the Diminished debuff at 75, 50 or 25
 * stacks before it finally joins at full strength on the fourth step; from the
 * wave after that it is one WHOLE clone per wave and there is no partial one.
 * See BG_VHR_QUARTERS_PER_CLONE for the table. Clearing continues until a wave
 * would need more than BG_VHR_MAX_ENEMIES clones, at which point the party has
 * won - reached after clearing wave 46 with a full ten, 51 with five, 55 solo.
 * Deeper waves also reinforce every clone that is not a copy of a party member
 * (BG_VHR_REINFORCE_FROM_WAVE), while clones OF a party member carry that
 * member's boons.
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
 * 3. The thirty-second gap between waves is a preparation window for the clones
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

#include <unordered_map>
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
    // Denominators for the "X / Y remaining" readouts. Enemies total is THIS
    // WAVE's clone count, not the run-ending BG_VHR_MAX_ENEMIES cap - the
    // top frame is reporting the fight in front of the party, not the run's
    // ceiling. Players total is the party size captured when the gates opened,
    // so it stays put as people fall.
    BG_VHR_WORLDSTATE_ENEMIES_TOTAL   = 9404,
    BG_VHR_WORLDSTATE_PLAYERS_TOTAL   = 9405
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
    BG_VHR_STRING_NEXT_WAVE_COUNTDOWN = 20100,
    // "%s answers the call and joins your side." - Boon of Fellowship ally
    // arrival. See sql/custom/world/2026_08_16_01_world_violet_hold_boons2.sql.
    BG_VHR_STRING_ALLY_JOINED = 20102
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

enum BG_VHR_Creatures
{
    // The boon broker spawns ALONGSIDE the runes above - one broker per rune
    // dropped, in addition to it, never in its place - and, unlike them, is
    // not taken back on a timer: he stands until somebody picks a boon.
    // Several can therefore be up at once, so he gets his own slots rather
    // than sharing the per-drop object slots. When every slot is occupied no
    // further broker comes until one is used.
    // 16: four runes' worth of brokers per wave plus the Hoarder's extra,
    // with room for a couple of waves to go untouched before it fills.
    BG_VHR_CREATURE_BOON_BROKER_1  = 0,
    BG_VHR_CREATURE_BOON_BROKER_MAX = 16,
    BG_VHR_CREATURE_MAX            = 16
};

enum BG_VHR_Constants
{
    // A wave needing more clones than this ends the run as a win.
    BG_VHR_MAX_ENEMIES = 40,

    // Cells hold at most ten players each in the stock instance, so a late
    // wave has to be spread over several. One cell per ten clones, rounded up.
    BG_VHR_CLONES_PER_CELL = 10,

    // Defaults for the gates-closed preparation window and the second the
    // per-second countdown starts at; the live values are
    // Centurion.VioletHold.PrepSeconds / .CountdownFromSeconds (World.cpp),
    // read at use time so `.reload config` applies to the next wave. The
    // window is announced once as it opens, then silently until the
    // countdown second, then every second down to the gates.
    BG_VHR_PREP_MS = 30 * IN_MILLISECONDS,
    BG_VHR_COUNTDOWN_FROM_SECONDS = 15,

    // Alive counts and the wipe check run on this timer rather than every tick.
    BG_VHR_STATE_CHECK_INTERVAL = 500,

    // The fall-through sweep (see BG_VHR_MIN_SAFE_Z) runs on this timer, and -
    // unlike the state check - from the moment the party is put down, not only
    // once the gates open. Half a second of free fall from the floor is well
    // short of the map's MinHeight, so nothing slips past between sweeps.
    BG_VHR_FLOOR_CHECK_INTERVAL = 500,

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
    //   wave 21 party + 5    wave 22 party + 6    wave 23 party + 7 ...
    //
    // The partial clone is held back by the Diminished debuff, whose stack
    // count is the percentage taken off its size, health, damage and healing -
    // so a clone at 25% power carries 75 stacks.
    //
    // That quarter-step ramp only runs to BG_VHR_QUARTER_RAMP_LAST_WAVE. From
    // the wave after it the curve is a WHOLE extra clone per wave and there is
    // no partial one any more, so a deep run climbs to the BG_VHR_MAX_ENEMIES
    // finish in tens of waves rather than hundreds.
    BG_VHR_QUARTERS_PER_CLONE = 4,
    BG_VHR_SPELL_DIMINISHED   = 90201,
    BG_VHR_QUARTER_RAMP_LAST_WAVE = 20,

    // Past this wave every clone that is NOT a copy of a party member walks in
    // carrying BG_VHR_SPELL_REINFORCED at (wave - this) stacks - +5% Stamina
    // each. Party copies are left alone: they already scale with the party.
    BG_VHR_REINFORCE_FROM_WAVE = 30,
    BG_VHR_SPELL_REINFORCED   = 90298,

    // Clearing a wave drops powerups on the floor: one per this many players,
    // rounded UP, so even a solo run gets one and a full ten gets four.
    //
    //   1-3 players -> 1     4-6 -> 2     7-9 -> 3     10 -> 4
    BG_VHR_PLAYERS_PER_BUFF = 3,
    BG_VHR_MAX_WAVE_BUFFS   = 4,

    // How long a dropped powerup survives before it is taken back, in ms. Waves
    // are composed the moment the previous one dies, so this deliberately
    // outlives the preparation window - the party can grab one while
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
    BG_VHR_GO_RECHARGE_BUFF = 300500,

    // Both end-of-run payouts share one shape: the matching arena-win payout,
    // times the wave the run ended on, divided by this. Honor additionally
    // compounds BG_VHR_HONOR_COMPOUND_PERCENT per wave (see
    // GetHonorRewardForRun); gold does not. This is the single knob for the
    // mode's overall generosity rather than a per-wave tweak.
    BG_VHR_REWARD_DIVISOR = 5,
    BG_VHR_HONOR_COMPOUND_PERCENT = 1,

    // Clearing a wave puts every casualty back on their feet at this fraction
    // of health and mana. Being brought back weakened is the cost of dying; the
    // run's difficulty comes from the party getting more fragile as the waves
    // climb, not from anyone sitting the rest of it out.
    BG_VHR_WAVE_END_REZ_PERCENT = 75
};

// Nothing in the Hold is walkable this low. The chamber floor sits at z 38 and
// the bed of Ichoron's flooded cell - the lowest ground anyone can be on - at
// 31, while the map's own MinHeight, where the stock under-map check fires
// (MovementHandler for players, the playerbot lifecycle tick for bots), is
// hundreds of yards further down: someone who had clipped through the floor
// would fall for seconds before the engine noticed, and a clone doing it
// mid-wave would stall the run. Anyone below this has fallen out of the map
// and is put back by RescueFallenPlayers.
constexpr float BG_VHR_MIN_SAFE_Z = 20.0f;

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

// Boon of Fellowship: the party asked for a bot ally. The battleground cannot
// see the bot population, so it publishes the request and the script-side
// driver fulfils it - one clone of a random level-appropriate managed bot,
// created on the HUMAN team next to whoever asked, and reported back with
// NotifyAllySpawned. Allies live for the rest of the run: not torn down with
// the waves, resurrected with the party when a wave falls, never counted as
// party members (wave size, wipe check, broker roster, rune pickup).
struct VhrAllyRequest
{
    uint32 id = 0;
    ObjectGuid requester;
    Position where;
    uint32 partyMinLevel = 0;
    uint32 partyMaxLevel = 0;
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

    // Called by the boon broker's script once a player has taken a boon from
    // him: the broker leaves and his slot is freed. Unknown guids are ignored.
    void ConsumeBoonBroker(ObjectGuid brokerGuid);

    // The human team as it stands - online, real (non-transient) characters,
    // dead or alive. What the broker rolls his offers against.
    void CollectHumanPlayers(std::vector<Player const*>& out) const;

    // Same rule as an arena: you cannot walk back to your own corpse. Being
    // raised by someone else is untouched - Rebirth, soulstones and
    // Reincarnation are the intended way back mid-wave, and
    // ResurrectWaveCasualties is the guaranteed one at the end of it.
    bool AllowsCorpseReclaim() const override { return false; }
    // Puts a fallen player back on valid ground (see GetRescuePosition).
    // Reached from the engine's MinHeight check and, well before that, from
    // the battleground's own BG_VHR_MIN_SAFE_Z sweep.
    bool HandlePlayerUnderMap(Player* player) override;
    void EndBattleground(uint32 winner) override;

    // The enemy side is empty between waves by design; the stock under-population
    // countdown would treat every gap as a team having abandoned the match.
    bool AllowsPrematureFinish() const override { return false; }

    // The wave behind the gates is mid-preparation and its clones are holding
    // the preparation aura and flag this battleground gave them.
    bool OwnsPreparationState(Player const* player) const override;

    // --- script-side clone driver interface -------------------------------
    // The driver polls this every world tick. A non-null return means the wave
    // has been composed and is waiting to be summoned; the driver spawns one
    // clone per sourceGuids entry at the matching spawnPositions entry and then
    // calls NotifyWaveSpawnFulfilled(). Until it does, the preparation timer
    // does not start, so a slow spawn never eats into the players' warning.
    VhrWaveSpawnRequest const* GetPendingWaveSpawnRequest() const;
    void NotifyWaveSpawnFulfilled(uint32 waveNumber, uint32 spawnedCount);

    // Reported per clone as the driver builds the wave, for clones copied from
    // a party member. The pairing is what lets the boons be refreshed when the
    // gates open: the wave is composed the instant the previous one dies -
    // BEFORE the party has even walked to the brokers that drop with it - so a
    // boon taken during the preparation window would otherwise not reach the
    // wave it was bought to fight.
    void NotifyCloneSource(ObjectGuid cloneGuid, ObjectGuid sourceGuid);

    // The team the clones fight on. Humans always hold the other one.
    uint32 GetEnemyTeam() const { return _enemyTeam; }
    uint32 GetHumanTeam() const { return _humanTeam; }
    uint32 GetWaveNumber() const { return _waveNumber; }
    bool IsWavePreparing() const { return _waveState == WaveState::Preparing; }

    // --- run-wide boons ----------------------------------------------------
    // Greed: every broker rolled from now on shows this many extra options.
    uint8 GetBrokerBonusOffers() const { return _brokerBonusOffers; }
    void AddBrokerBonusOffers(uint8 count);
    // Hoarder: this many brokers more than runes after every wave from now on.
    uint8 GetBonusBrokersPerWave() const { return _bonusBrokersPerWave; }
    void AddBonusBrokersPerWave(uint8 count);

    // Fellowship. RequestAlly queues one; the driver polls the queue, and on
    // success calls NotifyAllySpawned. A request that cannot be met right now
    // (no eligible bot online) is left in the queue and retried; DeferAllyRequests
    // is how the driver spaces those retries.
    void RequestAlly(Player const* requester);
    std::vector<VhrAllyRequest> const& GetPendingAllyRequests() const { return _allyRequests; }
    uint32 GetPendingAllyRequestCount() const { return uint32(_allyRequests.size()); }
    bool AreAllyRequestsDue() const { return !_allyRequests.empty() && !_allyRetryMs; }
    void DeferAllyRequests(uint32 ms) { _allyRetryMs = ms; }
    void NotifyAllySpawned(uint32 requestId, ObjectGuid allyGuid);
    uint32 GetAllyCount() const { return uint32(_allies.size()); }
    bool IsAlly(ObjectGuid guid) const { return _allies.find(guid) != _allies.end(); }

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
    uint32 GetPrepWindowMs() const;
    void SetWaveEnemyImmunity(bool immune);
    void CheckRunState();
    void TeleportSurvivorsToGurubashi();

    // Fall-through guard: everyone in the instance below BG_VHR_MIN_SAFE_Z is
    // handed to HandlePlayerUnderMap. Runs on BG_VHR_FLOOR_CHECK_INTERVAL from
    // PostUpdateImpl, countdown included.
    void RescueFallenPlayers();
    // Where someone who has fallen out of the map is put back: the start
    // landing before the gates open, the middle of the chamber floor after,
    // and for a clone still behind its cell door the release point of the
    // nearest cell in use, so a fall cannot let it out early.
    Position GetRescuePosition(Player const* player) const;

    uint32 CountAliveHumans() const;
    // Denominator for the players readout - the party size the run started
    // with, or the live head count before that is fixed.
    uint32 GetPartyStrength() const;
    uint32 CountAliveEnemies() const;
    std::vector<ObjectGuid> GetHumanRoster(bool livingOnly) const;
    uint32 GetEnemyCountForWave(uint32 wave) const;
    // Full-strength clones only, ignoring the partial one.
    uint32 GetFullStrengthCountForWave(uint32 wave) const;
    // Diminished stacks for the wave's partial clone. 0 when the wave is made
    // entirely of full-strength clones: every fourth wave during the quarter
    // ramp, and every wave past BG_VHR_QUARTER_RAMP_LAST_WAVE.
    static uint8 GetPartialCloneStacks(uint32 wave);

    // Re-copy every party member's boons onto the clones made from them, as
    // the cells open. Idempotent: CopyBoonsTo only tops up what is missing.
    void RefreshWaveBoonCopies();

    // Menagerie: summon each holder's guardians as the cells open, take the
    // previous wave's down first (and at the end of the run).
    void SummonMenagerie();
    void DespawnMenagerie();
    // Everyone alive who is not a real party member and needs no rez.
    void ResurrectAllies(float restore);

    // Reward powerups, dropped when a wave is cleared and taken back after
    // BG_VHR_BUFF_LIFETIME_MS.
    // Puts every dead player back on their feet at BG_VHR_WAVE_END_REZ_PERCENT
    // health and mana. Called once as a wave falls, and is the only route back
    // from death in this mode.
    void ResurrectWaveCasualties();

    void SpawnWaveRewardBuffs();
    void DespawnWaveRewardBuffs();
    bool PickBuffPosition(std::vector<Position> const& taken, Position& out) const;
    // First free broker slot, or BG_VHR_CREATURE_BOON_BROKER_MAX when all are
    // standing. Also collects the standing brokers' positions so a new drop
    // keeps its distance from them.
    uint32 FindFreeBoonBrokerSlot(std::vector<Position>& occupied) const;
    bool SpawnBoonBroker(uint32 slot, Position const& spot);

    uint32 GetHonorRewardForRun() const;
    void ModifyEndOfMatchHonorRewards(uint32 winner, uint32 team, uint32& winnerHonor, uint32& loserHonor) const override;
    uint32 GetMoneyRewardForRun() const;
    void ModifyEndOfMatchMoneyRewards(uint32 winner, uint32 team, uint32& winnerMoney, uint32& loserMoney) const override;

    uint32 _humanTeam;
    uint32 _enemyTeam;

    // Party size captured when the gates first open. Every wave is sized from
    // it - a quarter of a clone per wave elapsed up to
    // BG_VHR_QUARTER_RAMP_LAST_WAVE, a whole one per wave after that - so this
    // fixes the whole difficulty curve and must not drift when someone dies or
    // leaves.
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
    uint32 _floorCheckTimerMs;

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

    // Run-wide boon state (see the accessors above).
    uint8 _brokerBonusOffers;
    uint8 _bonusBrokersPerWave;
    std::vector<VhrAllyRequest> _allyRequests;
    uint32 _nextAllyRequestId;
    uint32 _allyRetryMs;
    GuidSet _allies;
    // Menagerie guardians alive for the current wave.
    std::vector<ObjectGuid> _menagerie;
    // clone -> the party member it was copied from, for the current wave only.
    std::vector<std::pair<ObjectGuid, ObjectGuid>> _waveCloneSources;
};

#endif
