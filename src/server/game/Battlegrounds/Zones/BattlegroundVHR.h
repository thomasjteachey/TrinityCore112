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
 * wave after that the partial clone is gone and the wave climbs - slowly at
 * first, faster towards the end - to BG_VHR_MAX_ENEMIES clones on
 * BG_VHR_FINAL_WAVE, whatever the party size. See BG_VHR_QUARTERS_PER_CLONE for
 * the table. Clearing BG_VHR_FINAL_WAVE (25) wins the run.
 *
 * Only so many of a wave's clones fight at full power: four per player
 * (Centurion.VioletHold.FullPowerClonesPerPlayer). The rest are a horde of
 * Diminished clones, so a small party faces the same forty bodies as a full
 * one without facing forty of itself. Past the champion the horde thins out
 * a wave at a time, and the final wave has none - every clone in it is at
 * full power.
 *
 * Wave BG_VHR_MIDBOSS_WAVE is a champion: one oversized clone with a party's
 * worth of health, extra damage and crowd-control resistance, walking out with
 * an escort halfway between one horde clone per player and the wave's regular
 * count, half of it at full power. The waves
 * before the champion and before the final wave drop extra boon brokers.
 * Deeper waves also reinforce every clone that is not a copy of a party member
 * (BG_VHR_REINFORCE_FROM_WAVE), while clones OF a party member carry that
 * member's boons.
 *
 * Not every clone walks out when the gates open. Only as many as the party
 * has players do, plus one more every BG_VHR_RESERVE_PROMOTION_WAVES waves;
 * the rest - the curve's extras - wait in cells of their own. Half of those
 * come out one at a time, one for every clone that dies, and the other half
 * together BG_VHR_TIMED_RESERVE_DELAY_MS after the gates. See VhrCloneRelease.
 *
 * Three things make this different from the other custom battlegrounds here:
 *
 * 1. There is no opposing queue. The enemy side is spawned on demand, so the
 *    match pops the instant a group queues and never waits for a second team.
 *
 * 2. It is an arena in the sense that matters: no spirit guides are placed and
 *    map 1608 deliberately has no graveyard rows, so the dead stay dead until
 *    the wave they fell in is cleared. Then everyone on the party's side is put
 *    back on their feet at full health and mana. Cooldowns are NOT given back.
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
#include "VioletHoldBoons.h"

#include <map>
#include <string>
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
    BG_VHR_STRING_ALLY_JOINED = 20102,
    // "Wave %u is a champion. %u boon brokers answer the call." - cleared the
    // wave before BG_VHR_MIDBOSS_WAVE.
    BG_VHR_STRING_CHAMPION_NEXT = 20103,
    // "%s, champion of the Hold, breaks free!" - the champion's gates open.
    BG_VHR_STRING_CHAMPION_FREE = 20104,
    // "The next wave is the last. %u boon brokers answer the call." - cleared
    // the wave before BG_VHR_FINAL_WAVE.
    BG_VHR_STRING_FINAL_NEXT = 20105,
    // "Wave %u is the Warchief, %s. %u boon brokers answer the call." - the
    // champion wave when the champion is the weekly honor Warchief's clone
    // (PickWarchief).
    BG_VHR_STRING_WARCHIEF_NEXT = 20106
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
    // Boon of Abundance's extra runes, one slot per stack.
    BG_VHR_OBJECT_BUFF_5           = 13,
    BG_VHR_OBJECT_BUFF_6           = 14,
    BG_VHR_OBJECT_BUFF_7           = 15,
    BG_VHR_OBJECT_MAX              = 16
};

enum BG_VHR_Creatures
{
    // The boon broker spawns ALONGSIDE the runes above - one broker per rune
    // dropped, in addition to it, never in its place - and, unlike them, is
    // not taken back on a timer: he stands until somebody picks a boon.
    // Several can therefore be up at once, so he gets his own slots rather
    // than sharing the per-drop object slots. When every slot is occupied no
    // further broker comes until one is used.
    // 32: four runes' worth of brokers per wave plus the Hoarder's extra,
    // with room for a couple of waves to go untouched before it fills - and
    // for the BG_VHR_FINALE_BROKERS drop before the last wave on top of that.
    BG_VHR_CREATURE_BOON_BROKER_1  = 0,
    BG_VHR_CREATURE_BOON_BROKER_MAX = 32,
    BG_VHR_CREATURE_MAX            = 32
};

enum BG_VHR_Constants
{
    // The final wave's clone count, for every party size, and the most any
    // wave ever fields. Clearing BG_VHR_FINAL_WAVE wins the run.
    BG_VHR_MAX_ENEMIES = 40,
    BG_VHR_FINAL_WAVE  = 25,

    // Cells hold at most ten players each in the stock instance, so a late
    // wave has to be spread over several. One cell per ten clones, rounded up,
    // counted separately for each VhrCloneRelease group: a held-back clone
    // never shares a cell with one that walks out at the gates.
    BG_VHR_CLONES_PER_CELL = 10,

    // The wave's extras - every clone past the party's own head count - are
    // held back in their cells when the gates open. One more of them joins
    // the opening group on every wave that is a multiple of this one: one
    // from wave 8, two from 16, and so on. Only full-strength clones are moved
    // up; the Diminished partial clone always stays back.
    BG_VHR_RESERVE_PROMOTION_WAVES = 8,

    // How long after the gates open the timed half of the held-back clones
    // comes out, all together.
    BG_VHR_TIMED_RESERVE_DELAY_MS = 30 * IN_MILLISECONDS,

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
    // The partial clone is held back by the Diminished debuff, whose stack
    // count is the percentage taken off its size, health, damage and healing -
    // so a clone at 25% power carries 75 stacks.
    //
    // That quarter-step ramp only runs to BG_VHR_QUARTER_RAMP_LAST_WAVE, where
    // the wave stands at party + 3 and a 50% clone. From the wave after it
    // there is no partial clone any more and the wave climbs to
    // BG_VHR_MAX_ENEMIES on BG_VHR_FINAL_WAVE along a square curve - gentle
    // just past the ramp, steep at the end (GetFullStrengthCountForWave):
    //
    //   solo      wave 16: 5    wave 20: 13    wave 24: 34    wave 25: 40
    //   party 2   wave 16: 6    wave 20: 14    wave 24: 34    wave 25: 40
    //   party 5   wave 16: 9    wave 20: 16    wave 24: 34    wave 25: 40
    //   party 10  wave 16: 14   wave 20: 20    wave 24: 35    wave 25: 40
    //
    // (Wave 15 itself is the champion - see BG_VHR_MIDBOSS_WAVE.) Of those,
    // Centurion.VioletHold.FullPowerClonesPerPlayer per player fight at full
    // power, plus a share of the rest that grows by a tenth each wave past
    // the ramp, so the horde is gone by the final wave
    // (GetFullPowerCountForWave).
    BG_VHR_QUARTERS_PER_CLONE = 4,
    BG_VHR_SPELL_DIMINISHED   = 90201,
    BG_VHR_QUARTER_RAMP_LAST_WAVE = 15,

    // The champion wave: one clone in slot 0 carrying
    // BG_VHR_CHAMPION_REINFORCED_PER_PLAYER Reinforced stacks per player
    // (+5% Stamina each, so about (party size + 1) times a clone's
    // health), BG_VHR_CHAMPION_DAMAGE_BASE + _PER_PLAYER per player
    // stacks of Boon of Might (+1% damage each), BG_VHR_CHAMPION_RESOLVE
    // stacks of Boon of Resolve (1% shorter crowd control each) and
    // BG_VHR_CHAMPION_SCALE times the size, walking out with an escort
    // halfway between one horde clone per player and the wave's regular
    // count (GetChampionEscortCount), half of it at full power.
    BG_VHR_MIDBOSS_WAVE = 15,
    BG_VHR_CHAMPION_REINFORCED_PER_PLAYER = 20,
    BG_VHR_CHAMPION_DAMAGE_BASE = 50,
    BG_VHR_CHAMPION_DAMAGE_PER_PLAYER = 10,
    BG_VHR_CHAMPION_RESOLVE = 50,
    // How far the Warchief's level may sit outside the party's range for the
    // champion to be their clone - the widest ring the wave bots are drawn
    // from. Further out, the champion is an ordinary clone.
    BG_VHR_WARCHIEF_LEVEL_SPAN = 10,

    // Clearing the wave before the champion drops this many brokers on top
    // of the usual ones. Clearing the wave before the final one drops at
    // least BG_VHR_FINALE_BROKERS in all.
    BG_VHR_MIDBOSS_EXTRA_BROKERS = 3,
    BG_VHR_FINALE_BROKERS = 10,

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
    // Boon of Abundance adds a rune per stack on top of that (its brokers do
    // not grow with it), so the drop can be up to seven.
    BG_VHR_MAX_BONUS_RUNES  = 3,

    // The Volatile Core rune's aura (5 sec, then the blast), and how often the
    // party is told where its carriers are (SendBombFeed).
    BG_VHR_SPELL_VOLATILE_CORE = 94311,
    BG_VHR_BOMB_FEED_INTERVAL  = 200,
    BG_VHR_RUNE_SLOTS       = BG_VHR_MAX_WAVE_BUFFS + BG_VHR_MAX_BONUS_RUNES,

    // Below this many people the party gets one extra boon broker per wave. A
    // boon is bought once and carried by one person, so it does not divide the
    // way a wave does: a trio shares what eight would.
    BG_VHR_SHORT_HANDED_PARTY = 4,

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

    // The rune pool, alongside stock Berserking, each a Power Crystal (the
    // spell scripts are in spell_generic.cpp):
    //   Recharge (red)          every cooldown reset
    //   Glacial Stasis (blue)   every enemy within 20 yd frozen for 5 sec
    //   Volatile Core (yellow)  goes off 5 sec after it is taken: a fifth of
    //                           each nearby enemy's health, and knocked back
    //   Bright Reflection (green) a copy of the taker fights beside them
    //                           until the wave falls
    //   Mana Surge (violet)     15 sec of spells that cost no mana and are
    //                           instant (94372 -> its class's 94373-94378).
    //                           Casters only: it is only in the roll while a
    //                           caster spec is in the party
    //                           (VioletHoldBoons::IsCasterSpec)
    // The runes outlive the gate window, so the last three can be held for
    // the fight. Speed is deliberately not in the roll, and neither is
    // Restoration: the party is put back to full as every wave falls.
    BG_VHR_GO_RECHARGE_BUFF          = 300500,
    BG_VHR_GO_GLACIAL_STASIS_BUFF    = 300501,
    BG_VHR_GO_VOLATILE_CORE_BUFF     = 300502,
    BG_VHR_GO_BRIGHT_REFLECTION_BUFF = 300503,
    BG_VHR_GO_MANA_SURGE_BUFF        = 300504,

    // What a rune looks like while it waits for the wave (UpdateRuneDisplays):
    // grey, see-through copies of the rune models (tools/violet_hold/
    // make_dim_rune.py), in GameObjectDisplayInfo and patch-enUS-A. Every
    // crystal rune shares the one crystal; Berserking keeps its own shape.
    BG_VHR_DISPLAY_CRYSTAL_RUNE_LOCKED    = 11204,
    BG_VHR_DISPLAY_BERSERKING_RUNE_LOCKED = 11205,

    // Both end-of-run payouts share one shape: the matching arena-win payout,
    // times the wave the run ended on, divided by this. Honor additionally
    // compounds BG_VHR_HONOR_COMPOUND_PERCENT per wave (see
    // GetHonorRewardForRun); gold does not. This is the single knob for the
    // mode's overall generosity rather than a per-wave tweak.
    BG_VHR_REWARD_DIVISOR = 5,
    BG_VHR_HONOR_COMPOUND_PERCENT = 1
};

// Every group a wave is split into gets cells of its own, and the wave can
// never need more than the room has.
static_assert((BG_VHR_MAX_ENEMIES + BG_VHR_CLONES_PER_CELL - 1) / BG_VHR_CLONES_PER_CELL + 2 <= BG_VHR_OBJECT_CELL_MAX,
    "a full wave split three ways must fit in the Violet Hold's cells");

// Nothing in the Hold is walkable this low. The chamber floor sits at z 38 and
// the bed of Ichoron's flooded cell - the lowest ground anyone can be on - at
// 31, while the map's own MinHeight, where the stock under-map check fires
// (MovementHandler for players, the playerbot lifecycle tick for bots), is
// hundreds of yards further down: someone who had clipped through the floor
// would fall for seconds before the engine noticed, and a clone doing it
// mid-wave would stall the run. Anyone below this has fallen out of the map
// and is put back by RescueFallenPlayers.
constexpr float BG_VHR_MIN_SAFE_Z = 20.0f;

// How much bigger the champion stands than the clone it was made from.
constexpr float BG_VHR_CHAMPION_SCALE = 1.5f;

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

// When a wave clone joins the fight. The opening group is the party's head
// count plus the extras promoted so far (BG_VHR_RESERVE_PROMOTION_WAVES); every
// other clone is held back, and the held-back clones are split in half - the
// odd one goes to OnEnemyDeath. They wait behind the shut doors of cells of
// their own, still under the preparation hold, and a cell opens the moment the
// first clone in it is let out.
enum class VhrCloneRelease : uint8
{
    WithGates = 0,
    // One comes out each time a clone of the wave dies.
    OnEnemyDeath = 1,
    // All come out together BG_VHR_TIMED_RESERVE_DELAY_MS after the gates.
    AfterDelay = 2
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
    // A custom game's host fixed the bots' levels (BattlegroundCustomRules
    // VhrBotMinLevel/MaxLevel). When set, BotSourced waves take only bots
    // inside this range, from either bot pool, and never widen it; with none
    // in range the wave falls back to the party picks as usual.
    uint32 hostBotMinLevel = 0;
    uint32 hostBotMaxLevel = 0;
    std::vector<ObjectGuid> sourceGuids;
    std::vector<Position> spawnPositions;

    // The champion wave's slot 0 is a clone of this character - the weekly
    // honor Warchief - whatever the composition. The driver keeps it in slot 0
    // when it re-sources a wave from the bots, loads it if it is offline and
    // names the clone "Warchief <name>". Empty on every other wave, and on the
    // champion wave when there is no usable Warchief (BattlegroundVHR::PickWarchief).
    ObjectGuid championSource;

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
    // Bright Reflection rune: the ally is a copy of THIS character (the one
    // who took the rune) rather than a bot, named "Bright <name>", and it is
    // fleeting - dismissed as the wave it joined falls instead of staying for
    // the run. Fleeting allies never count toward Fellowship's stacks.
    ObjectGuid source;
    bool fleeting = false;
    // A reflection's name over its head is this + the source's name.
    std::string namePrefix = "Bright ";
    // Diminished stacks the copy is seated with (0 = full power). Boon of
    // Multitudes' copies fight at 25%.
    uint8 diminishedStacks = 0;
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

    // The Hold is a last stand. Dying in it is meant to be final for the round -
    // no graveyard, no spirit guide, no timer counting down to a release that
    // leads nowhere - which is the arena contract, not the battleground one.
    bool UsesArenaDeathRules() const override { return true; }
    void HandleKillPlayer(Player* victim, Player* killer) override;
    void HandlePlayerResurrect(Player* player) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;

    // Wave-clear rewards belong to the party. The clones are Players on the
    // enemy team and would otherwise walk over them on the way in.
    bool CanPickUpPowerup(Player const* player) const override;

    // A rune taken stays taken for the rest of its drop. The stock respawn
    // (Battleground::HandleTriggerBuff, 3 minutes) only never showed here
    // while runes expired after a minute; untimed, it would hand a long wave
    // the same rune twice.
    uint32 GetBuffRespawnTime(uint32 /*type*/) const override { return RESPAWN_ONE_DAY; }

    // A party member just took a rune whose spell is runeSpellId (called by
    // the rune trap, GameObject.cpp). With Boon of Communion the rest of the
    // living party gets the same rune.
    void OnRuneTaken(Player* taker, uint32 runeSpellId);

    // Called by the boon broker's script once a player has taken a boon from
    // him: the broker leaves and his slot is freed. Unknown guids are ignored.
    void ConsumeBoonBroker(ObjectGuid brokerGuid);

    // The human team as it stands - online, real (non-transient) characters,
    // dead or alive. What the broker rolls his offers against.
    void CollectHumanPlayers(std::vector<Player const*>& out) const;

    // Same rule as an arena: you cannot walk back to your own corpse. Being
    // raised by someone else is untouched - Rebirth, soulstones and
    // Reincarnation are the intended way back mid-wave, and
    // RestorePartyAfterWave is the guaranteed one at the end of it.
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
    // the preparation aura and flag this battleground gave them - or the wave
    // is still being seated, and the clones already in are behind the same
    // shut doors waiting for it - or the gates are open and this clone is one
    // of the held-back ones (VhrCloneRelease). In every case the playerbot
    // lifecycle holds them where they stand until they are let out.
    bool OwnsPreparationState(Player const* player) const override;

    // Every cell door still shut, and the prison seal. The navmesh runs through
    // the doorways (the stock bosses walk out of their cells), so without these
    // a kiting bot retreated through a shut door and stood in a cell nobody
    // could reach or shoot into.
    void GetStandingGates(GuidVector& gates) const override;

    // --- script-side clone driver interface -------------------------------
    // The driver polls this every world tick. A non-null return means the wave
    // has been composed and is waiting to be summoned; the driver spawns one
    // clone per sourceGuids entry at the matching spawnPositions entry and then
    // calls NotifyWaveSpawnFulfilled(). Until it does, the preparation timer
    // does not start, so a slow spawn never eats into the players' warning.
    VhrWaveSpawnRequest const* GetPendingWaveSpawnRequest() const;
    void NotifyWaveSpawnFulfilled(uint32 waveNumber, uint32 spawnedCount);

    // Reported for every clone the driver seats, with the index of the
    // request slot it fills. The slot decides when the clone is let out
    // (VhrCloneRelease) - the driver does not need to know. A clone that is
    // never reported walks out with the gates.
    void NotifyCloneSeated(uint32 waveNumber, uint32 slot, ObjectGuid cloneGuid);

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
    // Cells open and the wave out. The runes can only be taken now
    // (GameObject::Update's battleground trap branch), not in the lull
    // between waves.
    bool IsWaveLive() const { return _waveState == WaveState::Fighting; }

    // --- run-wide boons ----------------------------------------------------
    // Greed: every broker rolled from now on shows this many extra options.
    uint8 GetBrokerBonusOffers() const { return _brokerBonusOffers; }
    void AddBrokerBonusOffers(uint8 count);
    // Hoarder: this many brokers more than runes after every wave from now on.
    uint8 GetBonusBrokersPerWave() const { return _bonusBrokersPerWave; }
    void AddBonusBrokersPerWave(uint8 count);
    // Abundance: this many runes more after every wave from now on.
    uint8 GetBonusRunesPerWave() const { return _bonusRunesPerWave; }
    void AddBonusRunesPerWave(uint8 count);

    // Fellowship. RequestAlly queues one; the driver polls the queue, and on
    // success calls NotifyAllySpawned. A request that cannot be met right now
    // (no eligible bot online) is left in the queue and retried; DeferAllyRequests
    // is how the driver spaces those retries.
    void RequestAlly(Player const* requester);
    // Bright Reflection rune: a copy of the taker fights beside them until
    // the wave falls (VhrAllyRequest::fleeting). Boon of Multitudes asks for
    // the same thing, smaller and under another name.
    // False when the requester cannot have one (not the party, or an ally).
    bool RequestReflection(Player const* requester, std::string const& namePrefix = "Bright ", uint8 diminishedStacks = 0);

    // Boons only one member of a run may ever take (VioletHoldBoons::
    // IsRunUnique), by Boon index. Claimed when taken; never released.
    bool IsRunUniqueBoonClaimed(uint8 boonIndex) const { return boonIndex < 64 && (_claimedRunUniqueBoons & (uint64(1) << boonIndex)); }
    // Once-per-wave class boons (VioletHoldClassBoons: Ancestors, Wellspring):
    // true the first time `guid` spends boon `boonSpellId` this wave.
    bool ClaimOncePerWave(uint32 boonSpellId, ObjectGuid guid) { return _oncePerWave[boonSpellId].insert(guid).second; }
    // Per-wave tallies (Boon of Atonement's heals): adds one and returns the
    // new count. Cleared with the once-per-wave claims.
    uint32 CountPerWave(uint32 boonSpellId, ObjectGuid guid) { return ++_perWaveCounts[boonSpellId][guid]; }
    void ClaimRunUniqueBoon(uint8 boonIndex) { if (boonIndex < 64) _claimedRunUniqueBoons |= uint64(1) << boonIndex; }
    std::vector<VhrAllyRequest> const& GetPendingAllyRequests() const { return _allyRequests; }
    // Fellowship's own requests and allies only - the reflections are not its.
    uint32 GetPendingAllyRequestCount() const;
    bool AreAllyRequestsDue() const { return !_allyRequests.empty() && !_allyRetryMs; }
    void DeferAllyRequests(uint32 ms) { _allyRetryMs = ms; }
    void NotifyAllySpawned(uint32 requestId, ObjectGuid allyGuid);
    uint32 GetAllyCount() const { return uint32(_allies.size() - _fleetingAllies.size()); }
    bool IsAlly(ObjectGuid guid) const { return _allies.find(guid) != _allies.end(); }
    // A reflection (Bright Reflection rune, Boon of Multitudes) - here for the
    // wave only.
    bool IsFleetingAlly(ObjectGuid guid) const { return _fleetingAllies.find(guid) != _fleetingAllies.end(); }

    // Boon of Focus Fire: `attacker` (a player on the party's side) hit
    // `victim` at `nowMs`. Forgotten when the wave ends.
    void NoteHit(ObjectGuid victim, ObjectGuid attacker, uint32 nowMs) { _recentHits[victim][attacker] = nowMs; }
    // Whether anyone on the party's side but `holder` hit `victim` within the
    // last `windowMs`.
    bool WasHitByOtherAlly(ObjectGuid victim, ObjectGuid holder, uint32 nowMs, uint32 windowMs) const;
    // Reflections whose wave has fallen, for the driver to take down. Empties
    // the list.
    std::vector<ObjectGuid> TakeAlliesToDismiss();

    // Boon of the Undying: `player` is Undying for `ms` from now. When the
    // clock runs out UpdateUndying ends the state and kills them (Phoenix
    // permitting); the wave falling first ends it without a death.
    void StartUndying(Player const* player, uint32 ms);

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
    // Takes the hold off everything that walks out with the gates. The
    // held-back clones keep it (and get it here if the wave had no window).
    void ReleaseWaveFromPreparation();
    // Centre-screen "Next wave in N..." notification, once per whole second.
    void AnnounceWaveCountdown();
    uint32 GetPrepWindowMs() const;
    // One clone into, or out of, the state the cells keep it in: preparation
    // aura and flag, fresh powers, and untouchable from outside.
    void HoldWaveClone(Player* enemy);
    void FreeWaveClone(Player* enemy);
    void CheckRunState();

    // The held-back clones (VhrCloneRelease) still waiting in their cells.
    struct VhrHeldClone
    {
        ObjectGuid guid;
        uint32 cell = 0;
        VhrCloneRelease release = VhrCloneRelease::WithGates;
    };
    bool IsHeldClone(ObjectGuid guid) const;
    // Lets one out: opens its cell (a no-op if already open), frees it, and
    // tops up the boons of a party copy. Unknown or vanished clones are just
    // dropped from the list.
    void ReleaseHeldClone(size_t index);
    // The first one waiting on `release`, if any.
    bool ReleaseNextHeldClone(VhrCloneRelease release);
    void ReleaseHeldClones(VhrCloneRelease release);

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
    // Held-back clones are alive and part of the wave - it is not cleared
    // while any are still in their cells - but not yet in the fight.
    uint32 CountAliveEnemies(bool includeHeld = true) const;
    std::vector<ObjectGuid> GetHumanRoster(bool livingOnly) const;
    uint32 GetEnemyCountForWave(uint32 wave) const;
    // Full-strength clones only, ignoring the partial one.
    uint32 GetFullStrengthCountForWave(uint32 wave) const;
    // The clones that walk out as the gates open: the party's head count plus
    // one per BG_VHR_RESERVE_PROMOTION_WAVES waves, never more than the wave's
    // full-strength clones. Everything past it is held back.
    uint32 GetOpeningCountForWave(uint32 wave) const;
    // Diminished stacks for the wave's partial clone. 0 when the wave is made
    // entirely of full-strength clones: every fourth wave during the quarter
    // ramp, the champion wave, and every wave past BG_VHR_QUARTER_RAMP_LAST_WAVE.
    static uint8 GetPartialCloneStacks(uint32 wave);
    // How many of the wave's clones fight at full power; every clone past it
    // is horde (GetHordeCloneStacks). Past the ramp the horde is promoted to
    // full power a step per wave; all of them on the final wave.
    uint32 GetFullPowerCountForWave(uint32 wave) const;
    // How many clones walk out with the wave-15 champion.
    uint32 GetChampionEscortCount() const;
    static uint8 GetHordeCloneStacks();
    // The champion wave's slot-0 clone grows into the champion as the wave is
    // fulfilled. Announced when its gates open.
    void MakeChampion(Player* champion);
    // Who the champion wave clones: the weekly honor Warchief, when that
    // character still exists and its level is within BG_VHR_WARCHIEF_LEVEL_SPAN
    // of the party's. Nobody otherwise, and the wave is the plain champion.
    void PickWarchief();
    // The champion's boons: at least its own Might and Resolve, whatever the
    // party member it copies was handed (RefreshWaveBoonCopies).
    void EmpowerChampion(Player* champion);

    // Re-copy every party member's boons onto the clones made from them, as
    // the cells open - or onto just `onlyClone`, as a held-back one comes out.
    // Idempotent: CopyBoonsTo only tops up what is missing.
    void RefreshWaveBoonCopies(ObjectGuid onlyClone = ObjectGuid::Empty);

    // Menagerie: summon each holder's guardians as the cells open, take the
    // previous wave's down first (and at the end of the run).
    void SummonMenagerie();
    void DespawnMenagerie();
    // Centurion.VioletHold.MenagerieTrace: log each guardian's combat state
    // and the verdict on its owner's target, whenever either changes.
    void TraceMenagerie();
    // RestorePartyAfterWave for the Fellowship allies, who are not on the
    // human roster.
    void RestoreAllies();
    // As a wave falls its reflections go, alive or not, before anyone is
    // restored.
    void DismissFleetingAllies();
    // Boon of the Undying: count the clocks down and kill whoever's ran out.
    void UpdateUndying(uint32 diff);
    // End every Undying state without a death (the wave fell, the run ended)
    // - or just `only`'s (they left).
    void EndUndying(ObjectGuid only = ObjectGuid::Empty);

    // Called once as a wave falls: the party's dead stand back up, and every
    // player on its side - and their pets - is put at full health and mana.
    // This is the only route back from death in this mode.
    void RestorePartyAfterWave();

    // Reward powerups, dropped when a wave is cleared and taken back when the
    // next wave is cleared (the drop that replaces them).
    // Returns how many boon brokers arrived with the drop.
    uint32 SpawnWaveRewardBuffs();
    void DespawnWaveRewardBuffs();

    // The runes still on the floor, for the CENTURION_VioletHold addon, which
    // names each one over it (through the client DLL's world-point
    // projection). One CCGAME line carries the whole state:
    //   VHRUNE:0;<x>,<y>,<z>,<entry>;...   ("VHRUNE:0" alone = none)
    // where entry is the rune's gameobject entry, which the addon names. The
    // 0 was the milliseconds left when runes expired; they no longer do.
    // sent whenever the set of runes changes, and to anyone joining the run.
    uint8 GetStandingRuneMask() const;
    std::string BuildRuneFeed() const;
    void SendRuneFeed(Player* player = nullptr);
    void SendRuneFeedIfChanged();
    // Every standing rune in its locked (grey, see-through) or its own look,
    // by IsWaveLive. Only touches a rune whose look is wrong.
    void UpdateRuneDisplays();
    // Who is carrying a Volatile Core, and where, so the addon can draw the
    // blast's reach round them: "VHBOMB;<guid>,<x>,<y>,<z>;..." to the party
    // every BG_VHR_BOMB_FEED_INTERVAL while anyone is, then a bare "VHBOMB"
    // once when nobody is any more.
    void SendBombFeed();
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
    // A run is already paid by the wave it reached, which is its length.
    bool PaysHourlyHonor() const override { return false; }

    uint32 _humanTeam;
    uint32 _enemyTeam;

    // Party size captured when the gates first open. Every wave is sized from
    // it - a quarter of a clone per wave elapsed up to
    // BG_VHR_QUARTER_RAMP_LAST_WAVE, then an even climb to the final wave - so this
    // fixes the whole difficulty curve and must not drift when someone dies or
    // leaves.
    uint32 _partySize;

    // A drop of runes is on the floor. Runes have no timer: they stay through
    // the next wave until the drop after it replaces them, and one that is
    // taken does not come back (GetBuffRespawnTime).
    bool _runesOut;

    uint32 _waveNumber;
    uint32 _highestWaveCleared;
    WaveState _waveState;
    uint32 _prepTimerMs;
    // Last whole second announced during the prep window, so the countdown
    // fires once per second instead of once per tick.
    uint32 _lastCountdownSecond;
    uint32 _stateCheckTimerMs;
    uint32 _floorCheckTimerMs;

    // Cells in use by the current wave, as BG_VHR_OBJECT_* indices - every
    // group's, the held-back ones included.
    std::vector<uint32> _waveCells;
    // Per request slot, as composed: which cell it is in and when it comes
    // out. Parallel to the published request's sourceGuids.
    struct VhrWaveSlot
    {
        uint32 cell = 0;
        VhrCloneRelease release = VhrCloneRelease::WithGates;
    };
    std::vector<VhrWaveSlot> _waveSlots;
    // (slot, clone) as the driver reports them, until the wave is fulfilled.
    std::vector<std::pair<uint32, ObjectGuid>> _waveSeatedSlots;
    // The held-back clones still in their cells, in slot order.
    std::vector<VhrHeldClone> _heldClones;
    // Counts down from the gates to the AfterDelay release; 0 when there is
    // nothing waiting on it.
    uint32 _timedReleaseMs;
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
    uint8 _bonusRunesPerWave = 0;
    // Fraction of a boon broker a big party has banked toward its next extra
    // one (Centurion.VioletHold.BrokersPerExtraPlayer).
    float _bigPartyBrokerCarry = 0.0f;
    std::vector<VhrAllyRequest> _allyRequests;
    uint32 _nextAllyRequestId;
    uint32 _allyRetryMs;
    GuidSet _allies;
    // The reflections among _allies, and those waiting for the driver to
    // take them down.
    GuidSet _fleetingAllies;
    std::vector<ObjectGuid> _alliesToDismiss;
    // Run-unique boons already taken (bit = Boon index).
    uint64 _claimedRunUniqueBoons = 0;
    // Boon of Multitudes copies called this wave (MULTITUDES_MAX_PER_WAVE).
    uint32 _multitudesThisWave = 0;
    // Once-per-wave class boons (Ancestors, Wellspring): boon spell -> who
    // already spent it this wave. Cleared in CompleteWave.
    std::unordered_map<uint32, GuidSet> _oncePerWave;
    std::unordered_map<uint32, std::unordered_map<ObjectGuid, uint32>> _perWaveCounts;
    // Boon of Focus Fire: victim -> party-side attacker -> game time (ms) of
    // the last hit. Cleared every wave.
    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> _recentHits;
    // Boon of Inheritance: stacks lent this wave, taken back in CompleteWave.
    std::vector<VioletHoldBoons::InheritedStacks> _inherited;
    // Boon of the Pack Leader's refresh clock.
    uint32 _packLeaderTimerMs = 0;
    // Boon of the Undying: who is Undying, and the milliseconds they have left.
    std::unordered_map<ObjectGuid, uint32> _undying;
    // Menagerie guardians alive for the current wave.
    std::vector<ObjectGuid> _menagerie;
    // MenagerieTrace: the last line logged per guardian, and its sampling clock.
    std::map<ObjectGuid, std::string> _menagerieTraceLast;
    uint32 _menagerieTraceTimerMs = 0;
    // clone -> the party member it was copied from, for the current wave only.
    std::vector<std::pair<ObjectGuid, ObjectGuid>> _waveCloneSources;
    // The champion of the current wave, if it is the champion wave.
    ObjectGuid _championGuid;
    // The Warchief the champion wave clones (PickWarchief), and their name.
    ObjectGuid _warchiefGuid;
    std::string _warchiefName;
    // Which rune slots the last rune feed reported standing.
    uint8 _runeFeedMask = 0;
    bool _runeFeedLocked = false;      // what the last feed said about IsWaveLive
    // The bomb feed's clock, and whether its last line named anybody.
    uint32 _bombFeedTimerMs = 0;
    bool _bombFeedSent = false;
};

#endif
