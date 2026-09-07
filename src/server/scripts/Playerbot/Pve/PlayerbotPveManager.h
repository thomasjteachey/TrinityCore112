/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITY_PLAYERBOT_PVE_MANAGER_H
#define TRINITY_PLAYERBOT_PVE_MANAGER_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;
class WorldObject;

namespace playerbot
{
struct PveConfig
{
    bool enabled = false;
    float companionFollowDistance = 2.5f;
    float companionAssistRadius = 45.0f;
    uint32 autoReviveSeconds = 30;
    float restHealthPct = 60.0f;
    float restManaPct = 50.0f;
    // Seconds a bot must hold an item taken from a PLAYER's death chest before it
    // may list it. Long enough to corpse-run back and take it off the bot.
    uint32 deathChestAuctionHoldSeconds = 1800;   // 30 minutes
    // Bots drinking for fun. Nothing here touches recovery - a bot only opens a
    // bottle when it needs nothing, and never instead of water.
    bool tavernEnabled = true;
    uint32 tavernMinMinutes = 25;
    uint32 tavernMaxMinutes = 90;
    uint32 tavernMaxDrunk = 100;
    // Fitness to START a fight with a person. The rest thresholds above only
    // decide when to sit down; these decide when a bot is fit to open on
    // somebody, and are deliberately higher.
    float playerEngageMinHealthPct = 85.0f;
    float playerEngageMinManaPct = 80.0f;
    bool autoLearnSpellsOnLevelUp = true;
    bool grindEnabled = false;
    float grindSearchRadius = 60.0f;
    // Navmesh A* queries the whole bot fleet may issue per second. These run
    // on the map-update thread that also resolves human combat, so this is
    // really a cap on how much player-visible latency the bots may cause.
    uint32 pathBudgetPerSecond = 150;
    // Multiplies the anchor price a bot asks for an item. Bots undercut each
    // other by 5% a pass, so an unmultiplied anchor walks the whole market
    // down to the vendor floor; this is the headroom that race runs in.
    float auctionPriceMultiplier = 10.0f;
    // Common materials are listed in respectable lots rather than dribbled
    // out one at a time. Capped by the item's own max stack size.
    uint32 auctionMinTradeGoodStack = 10;
    // Per-unit sell value at or above which a material is worth listing on
    // its own, however few the bot has.
    uint32 auctionValuableUnitCopper = 1000;
    // Multiple of an item's vendor price that the auction ask never goes
    // below - and, being the same number, the market price at which listing
    // stops being worth doing at all.
    float auctionVendorFloorFactor = 1.5f;
    // How far a seller undercuts the standing lot, in copper PER UNIT. A flat
    // step rather than a percentage, so the market drifts down instead of
    // collapsing geometrically.
    uint32 auctionUndercutCopper = 1;
    // How many SLOTS one item id may occupy in a bot's bags before the
    // surplus is thrown away. By slot, not by quantity: one stack of twenty
    // potions is harmless, twenty stacks of one potion is twenty slots.
    uint32 maxSlotsPerItemEntry = 3;
    // The rogue energy consumable, and the energy level below which it is
    // worth drinking mid-fight. 0 disables.
    uint32 thistleTeaItemId = 7676;
    uint32 thistleTeaEnergyBelow = 25;
    float grindWanderRadius = 40.0f;
    uint32 grindMaxLevelAbove = 3;
    uint32 grindMaxLevelBelow = 5;
    // Yards of detour a bot will accept per level of mismatch when choosing
    // something to grind. Nearest-wins alone had bots farming whatever was
    // underfoot; this makes a same-level mob worth walking for.
    float grindLevelMatchYards = 15.0f;
    bool grindAllowElites = false;
    bool lootEnabled = true;
    bool vendorEnabled = true;
    bool questsEnabled = true;
    bool equipUpgradesEnabled = true;
    bool buffsEnabled = true;
    bool talentsEnabled = true;
    bool relocateEnabled = true;
    uint32 relocateDryWanders = 5;
    std::vector<uint32> relocateMaps = { 0, 1 };
    // Open-world safety net: an autonomous PvE bot that never gets this far
    // from one anchor during the timeout is moved to a validated grind spot
    // in the zone it is already in.
    bool stuckRecoveryEnabled = true;
    float stuckRecoveryDistanceYards = 15.0f;
    uint32 stuckRecoverySeconds = 120;
    bool combatDiagnostics = false;
    // Journeys: destinations within this range are WALKED (segmented mmap
    // pathing) instead of teleported; 0 restores teleport-only travel.
    float travelWalkMaxDistance = 900.0f;
    // Real flight-master routes for longer travel (BFS over taxi edges,
    // bot pays the fare); teleport remains the last fallback.
    bool travelUseFlightPaths = true;
    // Bots browse their faction auction house and buy affordable gear
    // upgrades (delivered by mail, which bots now collect).
    bool auctionBuyEnabled = false;
    uint32 auctionBuyBudgetPct = 30;
    // How far BELOW the bot's own level an auction's ITEM LEVEL may sit before
    // the bot stops considering it at all. A hard floor under
    // auctionLevelsBehindPenalty, which only made stale gear score worse and
    // still let it win an empty or nearly worthless slot. Nothing is capped on
    // the way up: gear above the bot's level is a fine thing to buy.
    // 0 disables the floor.
    uint32 auctionMaxItemLevelsBehind = 12;
    uint32 auctionBuyMaxOverpayPct = 1200;
    // How many item levels a bot considers its whole auction budget worth.
    // Higher means more willing to pay for an upgrade.
    uint32 auctionBudgetWorthLevels = 15;
    // Hand bots riding skill and a mount at 40 and 60. They cannot visit a
    // riding trainer or a mount vendor - the trainer catch-up only walks class
    // trainers - so without this they run the entire climb on foot.
    bool grantMounts = true;

    // How many bots may PROACTIVELY start a fight with one person at a time,
    // sized by how much of their party is actually standing with them.
    //
    // Bots stay flagged and stay hostile: hostility cannot be made per-observer
    // on this core, so the budget decides only whether a bot CHOOSES to pull.
    // Retaliation is never gated - attacking a bot always gets you a fight.
    // How close an ORDINARY bot must be before it will pick a fight with a
    // person. Guardians keep their own, longer reach - they hold a zone and
    // noticing a visitor is the job - and the teleport-approach logic keeps
    // using the guardian value, because it has to stay above the 210 yard
    // landing distance to stop a just-dropped guardian re-approaching.
    float proactiveHuntYards = 125.0f;

    // How close a person has to be for a bot to count as having had its
    // contact. Being near somebody resets the aggression clock the same way
    // a fight does, so a bot that has been standing beside a player is not
    // instantly overdue the moment they leave.
    float aggressionResetYards = 200.0f;

    bool aggroBudgetEnabled = false;
    uint32 aggroBudgetSolo = 2;
    uint32 aggroBudgetPerExtraMember = 2;
    uint32 aggroBudgetMaxPerPlayer = 10;
    float aggroBudgetPartyRadius = 80.0f;
    // Item levels of value knocked off a purchase for each character level the
    // item sits below the buyer. Without it the scorer weighs item level gained
    // against price and nothing else, so a cheap scrap wins any slot that is
    // empty or nearly worthless - which is how level 40 bots ended up wearing
    // level 10 gear. 0 restores that behaviour.
    float auctionLevelsBehindPenalty = 0.35f;
    // Bots list the gear they cannot use, undercutting the standing price.
    bool auctionSellEnabled = false;
    // Gathering professions: two of herbalism/mining/skinning per bot,
    // auto-learned and ranked, nodes gathered mid-grind, corpses skinned.
    bool professionsEnabled = false;
    // Realm economy switch: off = the free eat/drink spells (L+ style);
    // on = bots buy real food/water/ammo from vendors and consume them (B+).
    bool restUseConsumables = false;
    // Accounts whose bots are PvP-only: no PvE behavior of any kind, they
    // idle at their sanctuary between battleground queues.
    std::vector<uint32> pvpOnlyAccountIds;
    // This percent of the fleet (guid-keyed, deterministic) is reborn at
    // the level cap: full level-1 reset and sent home. 0 disables.
    uint32 rebirthAtMaxLevelPercent = 0;
    // Rebirth into the bot's own zone band instead of all the way to level 1.
    bool rebirthZoneBanded = true;
    // Guild every managed bot is put into. Empty disables it.
    std::string guildName = "AI Uprising";
    // Veterans: how many bots skip the zone cycle, climb to the cap and stay.
    uint32 veteranBotCount = 20;
    // The configured fleet size, so veteranBotCount can be a real count.
    uint32 populationTarget = 256;
    // Characters sitting on the PvP-only accounts, counted at config load.
    uint32 pvpOnlyBotCount = 0;
    // Hardcore realms: bots never join groups - invites are declined and
    // companion summons refused.
    bool declineGroupInvites = false;
    // The hardcore death-chest gameobject: bots loot these like any prize.
    uint32 hardcoreLootChestEntry = 0;
    // How long such a chest stands before despawning - a corpse run is only
    // worth starting while it could still be there.
    uint32 hardcoreChestDespawnSeconds = 600;
    // Zone guardians: this many bots per classic zone are pinned there at
    // the zone's classic level cap with XP gain frozen. 0 disables.
    uint32 zoneGuardiansPerZone = 0;
    // Drifters: bots drawn evenly out of the banded population that drift toward the
    // real people online, so the leveling world looks lived in wherever somebody
    // actually is. Split evenly across everyone online; zero disables it.
    uint32 drifterCount = 0;
    // How long a person must hold a zone before their drifters follow them
    // into it, that being just long enough to ignore somebody running across a
    // border and straight back. A flight path is handled separately and needs no
    // help from this: a person in the air is not counted as being in the zones
    // they pass over at all.
    // Ceiling on drifters in any ONE zone, counted across everyone standing
    // in it. The share is per person and the total is global, so without this
    // five people in one zone draw five shares into it. 0 disables the cap.
    uint32 drifterMaxPerZone = 10;
    // Added to drifterMaxPerZone for every person in the zone beyond the
    // first, so a busier zone holds a bigger crowd rather than a thinner
    // share of the same one.
    uint32 drifterPerExtraPerson = 2;
    // Nobody is left alone for long: a person who has not fought a bot for
    // this many minutes has one dropped at idleProdDropYards and walked in.
    // 0 disables the prod entirely.
    uint32 idleProdAfterMinutes = 15;
    float idleProdDropYards = 210.0f;
    // How long before an unanswered prod is tried again. A prod that never
    // arrives leaves the person just as alone as before.
    uint32 idleProdRetrySeconds = 120;
    uint32 drifterZoneDwellSeconds = 10;
    // Gold handed to a drifter each time it lands somewhere new, paid before
    // the auction sweep is queued. A bot that arrives broke sweeps the auction
    // house and buys nothing, so the purse and the shopping trip have to happen
    // in that order. Counted in GOLD, not copper. Zero pays nothing.
    uint32 drifterTeleportGold = 10;
    // How far above its own level a bot will pick a fight with a person.
    // Four is the orange/red boundary the client draws: a target five or more
    // levels up is painted RED, the standard "you will lose this" signal, and a
    // bot that walks into one is donating a corpse. Everything up to and
    // including orange is fair game. Self-defence ignores this entirely.
    uint32 proactiveMaxLevelsAbove = 4;
    // And the floor under it: how far BELOW a bot a person may be and still be
    // picked on unprompted. Symmetric with the ceiling above by default.
    uint32 proactiveMaxLevelsBelow = 4;
    // How much bounty it takes to waive that floor. A single stack is one kill,
    // which is not yet somebody the realm should be sending a guardian after -
    // the waiver is meant for a person who has made a habit of it. 0 means no
    // amount of bounty ever waives the floor.
    uint32 proactiveBountyStacks = 5;
    // How close a guardian tries to get to a real player. Guardians go to the
    // people rather than waiting to be found: the server already knows where
    // every player is, so this needs no searching. 0 disables the approach and
    // leaves guardians standing their post.
    // Must stay clear of PvePlayerTeleportMinimumDistance (210y): a bot that
    // teleports to a person lands at that range, so a smaller radius than the
    // drop puts it outside its own engage check the instant it arrives and it
    // just stands there.
    float guardianPlayerApproachYards = 225.0f;
    // Minutes without a fight against a real player before a guardian starts
    // landing closer and closer, until it is arriving on top of them.
    uint32 guardianEscalateAfterMinutes = 30;
    // Every bot carries an aggression score of 1-100, normally distributed and
    // stable for the life of the character, biased by class. It decides how
    // long the bot will go without fighting a person before it travels to one
    // and starts a fight: the most aggressive wait AggressionMinMinutes, the
    // most passive wait AggressionMaxMinutes. 0 for both disables the hunt for
    // ordinary bots and leaves it to the zone guardians.
    uint32 aggressionMinMinutes = 5;
    uint32 aggressionMaxMinutes = 90;
    // Losing a fight to a person makes a bot timid: for this long it will not
    // go looking for another one, and it relocates to a grind spot with nobody
    // around rather than staying where it just died. Scaled by the bot's own
    // aggression, so the brave shrug it off and the meek stay away.
    uint32 timidMinutes = 20;
    // How far a fleeing bot's new grind spot must be from the nearest person.
    float timidFleeYards = 500.0f;
};

// Open-world PvE behavior for managed random bots, layered on the existing
// PvP decision engine: companion mode (grouped with a human: follow, assist,
// rest, revive) and grind mode (idle bots fight nearby creatures to level).
// Combat itself reuses PvpCore::BuildClassSpellContext through the PvE
// engagement flag; this manager only owns target selection and the
// out-of-combat life around it.
class PveManager
{
public:
    static void LoadConfig();
    static PveConfig const& GetConfig();

    // Called from the world-update hook on the world thread: finalizes pending
    // companion summons/teleports and drains queued group-invite acceptances,
    // so all group mutations happen on the world thread.
    static void OnWorldUpdate(uint32 diffMs);

    // Called from RandomBotParticipationManager::ProcessPlayerLifecycle on the
    // bot's map-update thread (under the per-map decision lock). Internally
    // cadence-gated; inert inside battlegrounds and duels.
    static void OnPlayerLifecycleTick(Player* player);

    static void OnBotLogout(Player const* player);

    // True while a bot is companion-bound to a human (or mid-summon): such a
    // bot must not be queued for battlegrounds or logged out by the
    // population rebalancer.
    static bool IsExemptFromBattlegroundOrchestration(Player const* player);

    // Master -> bot whisper command surface ("follow", "stay", "attack",
    // "passive", "come", "dismiss"). Returns true when the whisper was a
    // recognized PvE command and has been handled.
    static bool HandleWhisperCommand(Player* sender, Player* botReceiver, std::string const& command);

    // .playerbot pve summon <name>: logs the pool character in if needed,
    // teleports it to the summoner and joins the summoner's group.
    static bool RequestCompanionSummon(Player* summoner, std::string const& characterName, std::string& statusMessage);
    static bool RequestCompanionDismiss(Player* requester, Player* bot, std::string& statusMessage);

    // .playerbot pve reset [percent]: strips that share of the online
    // managed bots (companions excluded) back to freshly created level-1
    // characters and ports them to their racial starting spots.
    static uint32 ResetBotsToLevelOne(uint8 percent);
    // Send every managed bot to its assigned zone immediately.
    static uint32 RelocateBotsToHomeZones();
    // Wipe every auction in every house, escrowed items included. World
    // thread only - it mutates the live auction maps.
    static uint32 ClearAuctionHouse();
    // Wipe and re-spend every online managed bot's talents so existing bots
    // conform to the donor builds. Unlike the level-1 reset this deliberately
    // includes guardians and PvP-only bots: a build is a build wherever the
    // bot happens to be posted.
    static uint32 RespecBotsToDonorBuilds();

    // True for bots on Playerbot.Pve.PvpOnlyAccountIds: they skip every
    // PvE system and only answer the battleground orchestration.
    static bool IsPvpOnlyBot(Player const* player);

    // Whether this bot may pick this person as a target ON ITS OWN INITIATIVE.
    //
    // NOT a question about attackability - the hardcore pseudo-faction makes an
    // armed bot and any real player mutually attackable, and that is left alone
    // so a bot can always defend itself and anyone can always start a fight with
    // one. This asks the narrower question: may the bot start it?
    //
    // No unless the person armed War Mode, or carries a bounty. Consent is
    // assumed inside a battleground or arena (you queued for it) and in a duel
    // with the opponent who accepted. Answered from the same once-a-second
    // snapshot the rest of the proactive machinery reads, so it costs one short
    // scan and never a database or config lookup.
    //
    // Exported because the PvP class engine has its own fallback target scan
    // that runs outside the manager's engagement, and it must ask the same
    // question rather than keep a second answer.
    static bool MayProactivelyEngage(Player const* bot, Player const* human);

    // A validated patch of ground between minYards and maxYards of (fromX, fromY),
    // preferring preferredZoneId and falling back to the rest of the map.
    //
    // Answered entirely from the grind-spot cache, which is built once from
    // creature spawn CLUSTERS - so every candidate is somewhere three or more
    // creatures already stand. That makes it reachable and on the ground by
    // construction, and it costs ZERO pathfinding queries: no navmesh, no
    // PathGenerator, nothing that could compete with the fleet's path budget.
    //
    // Returns false rather than building the cache: the build stamps a zone id
    // on ~5900 points via sMapMgr->GetZoneId and must never run inside a map
    // thread answering a player's gossip click.
    //
    // seed makes the choice deterministic per caller, so asking twice for the
    // same contract gives the same place.
    static bool PickGroundSpotInBand(uint32 mapId, uint32 preferredZoneId, float fromX, float fromY,
        float minYards, float maxYards, uint32 seed,
        float& outX, float& outY, float& outZ, uint32& outZoneId);

    // Whether a real person is close enough to see something happen at this
    // spot. Bots do not count as witnesses, GMs do not either, and DEAD players
    // DO - a ghost is still somebody looking at the world.
    //
    // Exported because the hardcore ruleset asks the same question about a
    // bot's death chest that the remote chest looting asks about a chest.
    static bool AnyPersonWithin(WorldObject const* of, float yards);

    // One row per managed bot currently online, for the GM stats addon.
    //
    // The manager owns the per-bot state and its lock, so it does the reading;
    // callers get a flat snapshot they can aggregate however they like without
    // touching the live map.
    struct BotStatsRow
    {
        std::string Name;
        uint32 ZoneId = 0;
        uint32 MapId = 0;
        uint32 MoneyCopper = 0;
        uint8 Level = 0;
        uint8 Class = 0;
        uint8 Aggression = 0;
        uint8 Spec = 0;            // EquipProfileIndex: 0/1/2, named client-side
        uint8 HealthPct = 0;
        uint8 PowerPct = 0;        // mana, when the class has any
        uint16 ItemLevel = 0;      // mean of equipped, 0 when naked
        uint32 DisplayId = 0;      // for the client's model view
        uint8 WornCount = 0;       // equipped pieces
        uint8 GreenPlus = 0;       // of those, uncommon or better
        uint16 TimidSeconds = 0;   // 0 when not timid
        bool InCombat = false;
        bool Travelling = false;
        bool PvpOnly = false;
        bool Dead = false;
        // Which population this bot belongs to, resolved server-side because
        // every input is server-side: the guardian post table, the veteran hash
        // and its config divisor, the PvP-only account list, the drifter roster
        // and the companion's master. 0 local, 1 guardian, 2 veteran, 3 pvp,
        // 4 drifter, 5 companion. Named client-side, like Spec.
        uint8 Role = 0;
    };
    static void CollectBotStats(std::vector<BotStatsRow>& out);

    // Config-gated trainer-spell catch-up whenever a managed bot levels.
    static void OnManagedBotLevelChanged(Player* player, uint8 oldLevel);

    static std::string BuildStatusLine(Player const* bot);
};
}

#endif
