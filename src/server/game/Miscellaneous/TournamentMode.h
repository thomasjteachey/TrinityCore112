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
#include <array>
#include <string>
#include <utility>
#include <vector>

class Creature;
class Player;
class SpellInfo;
class WorldLocation;

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

    // Battleground/arena queue pool. Tournament characters always queue in the
    // tournament pool; world characters only when they opted in (and are at
    // least Centurion.Tournament.QueueMinLevel).
    bool QueuesInTournamentPool(Player const* player);
    bool CanToggleTournamentQueue(Player const* player);
    uint32 GetQueueMinLevel();

    // The Battlegrounds-tab toggles. The client asks with the addon message
    // "CCGAMEREQ\tTQUEUE" (query) or "CCGAMEREQ\tTQUEUE:1" / ":0" (set) and is
    // answered "CCGAME\tTQUEUE:<on>:<locked>" - locked on for tournament
    // characters, locked off below QueueMinLevel. "CCGAMEREQ\tGURUCHEST[:0|1]"
    // is answered "CCGAME\tGURUCHEST:<on>": whether the hourly Gurubashi chest
    // counts the character and pulls it into the Battle Ring (every realm).
    // Returns true when the message was one of these (the caller drops it).
    bool HandleAddonRequest(Player* sender, uint32 lang, std::string const& msg);
    void SendQueueState(Player* player);
    void SendGurubashiChestState(Player* player);

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
    // reach those NPCs. Player::SetPhaseMask adds it; 0 for everyone else.
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
    // Innate eat/drink/bandage spells and maxed crafting professions.
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
