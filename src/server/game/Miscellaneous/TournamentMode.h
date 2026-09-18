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

#ifndef TRINITYCORE_TOURNAMENT_MODE_H
#define TRINITYCORE_TOURNAMENT_MODE_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Optional.h"
#include <array>
#include <string>
#include <utility>
#include <vector>

class Creature;
class Player;
class SpellInfo;
class WorldLocation;
struct ItemTemplate;

// CENTURION runs Barracks+ ("world mode") and Legionnaire+ ("tournament mode")
// characters on one realm. The choice is made at character creation and lives
// in characters.extra_flags (PLAYER_EXTRA_TOURNAMENT_MODE), so no schema change
// is needed and every realm on the shared branch can run this code.
//
// Every rule is inert unless Centurion.Tournament.Enable = 1: a realm that has
// never heard of tournament characters behaves exactly as before, and a
// tournament flag left on a character there does nothing.
namespace Tournament
{
    // Loaded from World::LoadConfigSettings, so `.reload config` applies it.
    void LoadConfig();
    bool IsEnabled();

    // --- who is what ------------------------------------------------------

    // A tournament character (flag set AND the feature enabled).
    bool IsTournamentCharacter(Player const* player);
    // Answers for an OFFLINE character too, from the character cache.
    bool IsTournamentCharacter(ObjectGuid guid);

    // True when exactly one side is a tournament character: the pair may not
    // trade, mail or otherwise hand each other anything.
    bool AreSeparated(Player const* a, Player const* b);
    bool AreSeparated(ObjectGuid a, ObjectGuid b);

    // A tournament character and a world character are only ever enemies on the
    // sand of the Gurubashi Battle Ring, and only while both stand on it. True
    // for such a pair anywhere else, whichever of them happens to be FFA-armed
    // (War Mode, the ring). Battlegrounds and arenas keep their own sides, and a
    // duel is settled before this is asked. WorldObject::GetReactionTo and
    // Unit::BuildValuesUpdate both use it, so the server and the client agree.
    bool AreKeptFromFighting(Player const* a, Player const* b);

    // Battleground/arena queue pool. Tournament characters always queue in the
    // tournament pool. So does every world character from
    // Centurion.Tournament.QueueMinLevel up, because
    // Centurion.Tournament.ForceQueueAtMinLevel leaves them no world queue to
    // join; with that rule off they are back to queueing there only when they
    // opted in.
    bool QueuesInTournamentPool(Player const* player);

    // Why the Battlegrounds-tab toggle cannot be moved, which is also what its
    // tooltip explains. Sent to the client as it stands, so the numbers are
    // part of the protocol.
    enum QueueLockReason : uint8
    {
        QUEUE_LOCK_NONE                 = 0,   // theirs to set
        QUEUE_LOCK_TOURNAMENT_CHARACTER = 1,   // born to the tournament
        QUEUE_LOCK_BELOW_LEVEL          = 2,   // world character under QueueMinLevel
        QUEUE_LOCK_FORCED               = 3,   // world character at QueueMinLevel, ForceQueueAtMinLevel
        QUEUE_LOCK_BUSY                 = 4,   // in a queue or a match right now
        QUEUE_LOCK_DISABLED             = 5    // no character modes on this realm
    };

    // The match rules the tooltip lists, as the bits of the TQUEUEWHY line.
    // Also part of the protocol.
    enum QueueRuleFlags : uint32
    {
        QUEUE_RULE_LOADOUT          = 0x1,   // gear is swapped for the tournament's
        QUEUE_RULE_BAN_CONSUMABLES  = 0x2    // only PvP consumables work inside
    };

    QueueLockReason GetQueueLockReason(Player const* player);
    bool CanToggleTournamentQueue(Player const* player);
    uint32 GetQueueMinLevel();

    // True when a managed world-mode bot could end up in the same match. Bots
    // are world characters, so one at this character's level follows the same
    // pool rule they do; only a tournament character - or a world character who
    // opted into the tournament pool by hand, which needs the forcing rule off -
    // is somewhere the fleet never goes.
    bool IsReachableByWorldBots(Player const* player);

    // The Battlegrounds-tab toggles. The client asks with the addon message
    // "CCGAMEREQ\tTQUEUE" (query) or "CCGAMEREQ\tTQUEUE:1" / ":0" (set) and is
    // answered "CCGAME\tTQUEUE:<on>:<locked>" - locked on for tournament
    // characters and for world characters from QueueMinLevel up, locked off
    // below it - followed by
    // "CCGAME\tTQUEUEWHY:<QueueLockReason>:<minLevel>:<QueueRuleFlags>", which
    // is what the tooltip reads. Its own line so that a client that predates it
    // keeps matching the TQUEUE line whole. "CCGAMEREQ\tGURUCHEST[:0|1]"
    // is answered "CCGAME\tGURUCHEST:<on>": whether the hourly Gurubashi chest
    // counts the character and pulls it into the Battle Ring (every realm).
    // Returns true when the message was one of these (the caller drops it).
    bool HandleAddonRequest(Player* sender, uint32 lang, std::string const& msg);
    void SendQueueState(Player* player);
    void SendGurubashiChestState(Player* player);

    // The hourly Gurubashi chest's clock, shown beside that toggle. The chest
    // event (scripts/Custom/custom_gurubashi_arena.cpp) writes it - game/ never
    // calls into scripts, so the two meet here. "CCGAMEREQ\tGURUTIMER" is
    // answered "CCGAME\tGURUTIMER:<out>:<seconds>": out = 1 and the seconds until
    // the chest despawns, or out = 0 and the seconds until the next hourly check.
    // Both times are GameTime seconds, 0 for none; a realm whose event never
    // wrote a clock gets no answer, so its timer stays hidden.
    void SetGurubashiChestClock(time_t nextCheck, time_t chestExpiresAt);
    void SendGurubashiChestTimer(Player* player);

    // --- confinement ------------------------------------------------------

    // Battleground and arena maps are always allowed; elsewhere the zone or
    // area must be listed in Centurion.Tournament.AllowedZones.
    bool IsLocationAllowed(uint32 mapId, uint32 zoneId, uint32 areaId);
    bool IsLocationAllowed(uint32 mapId, float x, float y, float z);
    bool HasConfinement();
    bool GetHomeLocation(WorldLocation& out);
    // Sends a tournament character that stands somewhere it may not back home.
    // Returns true when it had to.
    bool EnforceConfinement(Player* player);

    // --- phasing ------------------------------------------------------------

    // Centurion.Tournament.PhaseMask: the phase the tournament NPCs are spawned
    // in. Tournament characters carry it on top of their normal phase, so they
    // see both worlds; world characters never do, so they can neither see nor
    // reach those NPCs. Centurion.Tournament.WorldPhaseMask is the mirror image,
    // carried by world characters only, for world creatures hidden from
    // tournament characters. Player::SetPhaseMask adds whichever applies.
    //
    // A world character also carries one phase per quest listed in
    // Centurion.Tournament.WorldQuestPhases ("quest:phase, ..."), from the
    // moment that quest is rewarded: the world copies of the Legionnaire+ hubs
    // are spawned in those phases, so each hub appears when its teleport is
    // earned and stays hidden before. Tournament characters never carry them.
    uint32 GetExtraPhaseMask(Player const* player);
    // Recomputes the phase from the character's phase auras plus the tournament
    // phase - after a mode change, a `.reload config`, or a login.
    void RefreshPhase(Player* player);
    // True when the character's phase does not match its mode.
    bool IsPhaseStale(Player const* player);

    // --- interaction ------------------------------------------------------

    // Player::GetNPCIfCanInteractWith. npcFlags is what the caller asked the
    // creature to be: auctioneers and flight masters are refused outright, any
    // other NPC must be in Centurion.Tournament.AllowedCreatures (when the list
    // is set). Spirit healers and guides, battlemasters, own pets and anything on
    // a battleground map or in the custom-game lobby stay usable.
    bool CanInteractWithCreature(Player const* player, Creature const* creature, uint32 npcFlags);

    // --- loot and the guild bank ------------------------------------------

    // Player::SendLoot. Outside battlegrounds and arenas a tournament character
    // loots nothing: no corpses, chests, gathering nodes, fishing, skinning,
    // pickpocketing or death chests. Its own items (containers, disenchanting)
    // and player bones stay open, and so do objects listed in
    // Centurion.Tournament.AllowedLootObjects.
    bool CanOpenLoot(Player const* player, ObjectGuid lootGuid);
    // Group loot: whether this member may be handed anything from a corpse or a
    // chest - rolls, round robin, master loot, a share of the coin.
    bool MayReceiveGroupLoot(Player const* player);
    // The guild bank - vault, tabs, money - and repairs paid out of it.
    bool CanUseGuildBank(Player const* player);

    // --- character kit ----------------------------------------------------

    uint8 GetStartLevel();
    // The character-create screen marks a tournament character by sending its
    // name first-letter-lowercase, rest-uppercase ("eLGROM"). Checked on the raw
    // name, before normalizePlayerName.
    bool IsTournamentNameMarker(std::string const& rawName);
    // Innate eat/drink/bandage spells. Tournament characters have no professions.
    // Idempotent; run at every login of a tournament character.
    void ApplyCharacterKit(Player* player);

    // Legionnaire+'s create data for tournament characters, from the world
    // tables playercreateinfo_tournament (start position),
    // playercreateinfo_item_tournament (items; amount -1 drops an item from
    // the starting outfit), playercreateinfo_outfit_tournament (the outfit) and
    // playercreateinfo_spell_custom_tournament
    // (taught at creation and at every login, like playercreateinfo_spell_custom).
    // A table that does not exist leaves that part to the world data, so a realm
    // without them loads nothing and changes nothing.
    struct CreateInfo
    {
        bool HasPosition = false;
        uint32 MapId = 0;
        float PositionX = 0.0f;
        float PositionY = 0.0f;
        float PositionZ = 0.0f;
        float Orientation = 0.0f;

        bool HasItems = false;
        std::vector<std::pair<uint32 /*itemId*/, uint32 /*count*/>> Items;
        std::vector<uint32> RemovedOutfitItems;

        // Legionnaire+'s starting outfit (its CharStartOutfit.dbc, table
        // playercreateinfo_outfit_tournament) with item ids mapped to the tournament
        // copies, one list per gender. Given instead of this realm's DBC outfit;
        // an empty list = the DBC outfit.
        std::array<std::vector<uint32>, 2> Outfit;

        bool HasCustomSpells = false;
        std::vector<uint32> CustomSpells;
    };

    // Startup only (World::SetInitialWorldSettings, after LoadPlayerInfo).
    void LoadCreateInfo();
    // nullptr when no tournament table is loaded or the pair is not creatable.
    CreateInfo const* GetCreateInfo(uint8 race, uint8 playerClass);
    // Where a new tournament character starts: its create data position, else
    // Centurion.Tournament.HomeLocation.
    bool GetStartLocation(uint8 race, uint8 playerClass, WorldLocation& out);
    // The spell list Player::LearnCustomSpells teaches this character, or nullptr
    // for the world list.
    std::vector<uint32> const* GetCustomSpells(Player const* player);

    // The template characters (Startwarrior, Startmage...) hand a new tournament
    // character their spells, talents, action bars and, for hunters, pets -
    // Legionnaire+'s createCopyOfChar, as the characters-DB procedure
    // createTournamentKit. Runs after the create transaction committed; logs and
    // does nothing when the procedure does not exist on this realm.
    void RunKitProcedure(Player const* newCharacter);

    // Legionnaire+ ran with AlwaysMaxWeaponSkill and AlwaysMaxSkillForLevel on;
    // tournament characters follow Centurion.Tournament.AlwaysMaxWeaponSkill and
    // .AlwaysMaxSkillForLevel, everyone else the realm-wide keys.
    bool AlwaysMaxWeaponSkill(Player const* player);
    bool AlwaysMaxSkillForLevel(Player const* player);

    // --- item links -------------------------------------------------------

    // Centurion carries most gear twice: the world-mode (Barracks+) item and the
    // tournament-mode (Legionnaire+) copy in the 200000 range. Cryptstalker
    // Wristguards 22443 and Crunched Cryptstalker Wristguards 201978 are the same
    // bracers in two modes; Beaststalker's Mantle 16679 and 200487 share even the
    // name. Nothing tied them together - `zz_tmode_item_map` only records which
    // row each copy was made from, which is provenance and not equivalence (it
    // also pairs the 61 Legionnaire+ items that squat on a stock id, where the
    // world side is a different item entirely).
    //
    // The world table `item_tournament_link` is the equivalence, and this is the
    // lookup over it, both ways. Knowing the pair changes nothing by itself: no
    // item is substituted, hidden or rewritten anywhere in core. It is the
    // groundwork for swapping a character's gear between the two modes.
    struct ItemLink
    {
        uint32 TournamentEntry = 0;
        uint32 WorldEntry = 0;    // 0 = the tournament item stands for nothing in
                                  // world mode: weapon boxes, the marks, the Mark
                                  // Transmuter - Legionnaire+ items whose source
                                  // id holds an unrelated item on the world side
        bool Crunched = false;    // the tournament side was retuned - a lower item
                                  // level and stats, named "Crunched <name>"
    };

    // Startup, after the item templates are in, and `.tournament reloaditems`.
    // A realm without the table loads nothing and every lookup below says "no
    // link", so the branch's other realms are unaffected.
    void LoadItemLinks();
    uint32 GetItemLinkCount();

    // Either side of a pair; empty when the entry is in no pair at all. Returned
    // by value: a reload may replace the store while a caller holds this.
    Optional<ItemLink> GetItemLink(uint32 entry);
    // True for an item that belongs to the tournament side, twin or no twin.
    bool IsTournamentItem(uint32 entry);
    // The other side, or 0 when this item does not have one.
    uint32 GetTournamentItem(uint32 worldEntry);
    uint32 GetWorldItem(uint32 tournamentEntry);
    // The counterpart of whichever side the entry is on, 0 when it has none.
    uint32 GetCounterpartItem(uint32 entry);
    // What a character in this mode holds in place of `entry`: the item itself
    // when it already belongs to that side, its counterpart when it has one, and
    // 0 when the item cannot cross - a tournament-only item asked for in world
    // mode, or a world item with no tournament copy (most of them: 2,999 of the
    // realm's 27,334 equippable world items have one). What 0 means - leave the
    // item alone, refuse it, strip it - is the caller's to decide.
    uint32 GetItemForMode(uint32 entry, bool tournamentMode);

    // --- the battleground loadout -----------------------------------------

    // Stepping into a TOURNAMENT-POOL battleground or arena normalises what a
    // character fights in, and leaving - by any route, chosen or not - gives it
    // everything back. Miscellaneous/TournamentLoadout.cpp holds the rules and
    // the reasoning; in short, each equippable item it owns becomes its
    // tournament twin, or the same slot from its class's starter template, or a
    // field kit piece, or nothing, and the originals are kept aside meanwhile.
    //
    // Every move is written to `character_tournament_loadout` as it is made, so
    // a logout, a disconnect, a crash or a restart mid-match is just a slower
    // way out: the next login hands the character its own gear back.
    void LoadLoadoutConfig();   // from LoadConfig, so `.reload config` applies
    void LoadLoadoutData();     // startup (after the item templates) and `.tournament reloaditems`

    // Which of these rules a match is actually run under, for the tooltip that
    // promises them (TQUEUEWHY, below).
    bool IsBgLoadoutEnabled();          // Centurion.Tournament.BgLoadout
    bool AreMatchConsumablesBanned();   // Centurion.Tournament.BgBanConsumables

    void ApplyBattlegroundLoadout(Player* player);    // Battleground::AddPlayer
    void RestoreBattlegroundLoadout(Player* player);  // Battleground::RemovePlayerAtLeave
    void RestoreLoadoutAfterLogin(Player* player);    // the crash and logout route
    bool HasBattlegroundLoadout(Player const* player);

    // Tournament gear never leaves a tournament match on a world-mode character.
    // The rows are the bookkeeping; this is the guarantee that does not depend on
    // them - every tournament item a world character holds is taken off it on the
    // way out and again at every login, which is where a realm that was killed
    // outright is caught. Returns how many were taken. A tournament character
    // keeps its gear and a Game Master is left alone.
    uint32 SweepTournamentItems(Player* player);

    // The gear is handed back while the character is on its way out of the
    // battleground map, and a client mid-world-port can keep showing the weapon
    // it just put down. Called every update by the tournament player script; it
    // returns at once unless that character is waiting for exactly this.
    void RefreshLoadoutVisuals(Player* player);
    // True while this thread is dressing or undressing that character: the
    // battleground armour lock (Player.cpp) stands aside for the swap, which is
    // the one pass that has to change every slot.
    bool IsLoadoutSwapInProgress(Player const* player);
    // The starter-template item a class wears in an equipment slot, 0 for none.
    uint32 GetLoadoutTemplateItem(uint8 playerClass, uint8 slot);

    // Inside a tournament match only the tournament PvP consumables work
    // (Centurion.Tournament.BgConsumables - what Jazzik sells), plus what a
    // character brought of its own: food, drink and bandages, and anything a
    // class conjured - healthstones, mana gems, soulstones, conjured food and
    // water. Asked by the use handler about anything a character tries to eat,
    // drink, quaff or throw.
    bool IsConsumableAllowedInMatch(Player const* player, ItemTemplate const* proto);

    // Eat, drink and bandage out of your own bags for free. The tournament hands
    // those out as spells, so a character using its own food, drink or bandage
    // in a tournament match spends nothing: Spell::TakeCastItem asks before it
    // takes a charge or the item. Only what an arena already allows (the arena
    // flag, a conjured consumable, a real First Aid bandage) and only that
    // family - a healthstone is not a meal.
    bool KeepsCastItem(Player const* player, ItemTemplate const* proto);

    // Centurion.Tournament.InnateSpells as spell -> class mask (0 = every
    // class): the eat/drink/bandage a tournament character knows without being
    // taught, lent to a world-mode character for the length of a match.
    std::vector<std::pair<uint32, uint32>> GetInnateSpells();

    // --- Legionnaire+ realm rules, per character --------------------------

    // Legionnaire+ ran these realm-wide; on a mixed realm tournament characters
    // keep them (Centurion.Tournament.*) and world characters follow the realm.
    // Level from which resurrection sickness applies (Death.SicknessLevel; L+ 61).
    int32 GetDeathSicknessLevel(Player const* player);
    // Whether ranged attacks use up ammunition (Centurion.Classic.ConsumeAmmo; L+ off).
    bool ConsumesAmmo(Player const* player);
    // Whether a hunter pet's happiness decays (Centurion.Classic.PetHappinessDecay; L+ off).
    bool PetHappinessDecays(Player const* owner);
    // ResetDuelCooldowns / ResetDuelHealthMana (L+ on): a duel with a tournament
    // character on either side follows the tournament setting, for both duelists.
    bool ResetsDuelCooldowns(Player const* a, Player const* b);
    bool ResetsDuelHealthMana(Player const* a, Player const* b);
    // The honor cap. MaxHonorPoints and its ConditionalMaxHonorPoints steps (a
    // spell known, a quest done) are Legionnaire+'s ladder and bind tournament
    // characters; a world character holds up to
    // Centurion.Tournament.WorldMaxHonorPoints instead. 0 = the ladder applies
    // (a tournament character, modes off, or the key set to 0).
    uint32 GetWorldMaxHonorPoints(Player const* player);

    // --- all characters ---------------------------------------------------

    // Battlegrounds, arenas and duels in progress (Centurion.Pvp.WaiveReagentsAndAmmo):
    // reagents are neither consumed nor required there, and ammunition is not
    // consumed (it must still be equipped).
    bool IsFreeReagentContext(Player const* player);
    // Whether the character's reagents are waived right now: in that context, and
    // anywhere for a tournament character (Centurion.Tournament.WaiveReagents).
    bool HasReagentWaiver(Player const* player);
    // The reagent half, per spell: combat spells only - never crafting,
    // enchanting, item creation, portals, teleports or summons.
    bool IsReagentWaived(Player const* player, SpellInfo const* spellInfo);

    // Standard refusal text, sent as a notification.
    void SendRefusal(Player const* player, char const* what);
}

#endif // TRINITYCORE_TOURNAMENT_MODE_H
