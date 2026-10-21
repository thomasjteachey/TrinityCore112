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

#ifndef TRINITYCORE_RIFT_PROGRESSION_H
#define TRINITYCORE_RIFT_PROGRESSION_H

#include "Define.h"

#include <string>
#include <string_view>
#include <vector>

class Player;
class SpellInfo;

// Summoner's Rift progression: every match starts at level 10 - where the first
// talent point comes - and climbs back to the character's real level, the way a
// League match climbs from 1 to 18.
//
// The level drop is real (Player::GiveLevel, borrowed). On the way in:
//
//  - The active spec's talents are written down and reset; the match's talent
//    points are the character's to spend in the ordinary talent frame, one per
//    level. On the way out the match's talents are reset and the real build is
//    learned again, tier by tier. The OTHER spec is never touched: while a
//    character is in a match (or has a stash row) InitTalentForLevel neither
//    resets talents it cannot pay for nor folds the specs together, and the spec
//    cannot be switched (SuppressTalentReset).
//  - Spells stay known. Everything a character of the starting level would know
//    is usable from the start; anything higher is refused until it is unlocked
//    (Spell::CheckCast, AdjustCast), and a higher rank than the one unlocked
//    casts the unlocked rank instead, so the action bar keeps working.
//  - The gear goes aside for the field set (Tournament::ApplyRiftLoadout, the
//    tournament loadout's stash rows and restore routes).
//
// Each match level hands out skill picks: the player chooses which spell to
// learn or rank up, through the CENTURION_Rift addon (CCGAME "RIFTPICKS" lines
// out, "CCGAMEREQ RIFTPICK:<spell>" back). Picks are spread so that by the real
// level everything the character knows is unlocked.
//
// Rift gold is the character's money slot: the real money is put aside, the
// match is played from a fixed purse, and the real money comes back.
//
// What comes back, and when: the real level, XP, money and talent build are
// written to `custom_rift_stash` before anything changes, so leaving by any
// route - the match ending, the leave button, a disconnect, a crash, a restart -
// ends the same way: Leave(), or OnLogin() at the next login, gives it all back.
namespace RiftProgression
{
    // World startup: is `custom_rift_stash` there? A realm without it never
    // enters the progression (the Rift then plays at the character's own level).
    void ProbeStorage();

    // True while this character is playing a Rift match under these rules.
    TC_GAME_API bool IsInMatch(Player const* player);

    // BattlegroundSRF::AddPlayer; Battleground::RemovePlayerAtLeave, before the
    // gear comes back (it needs the real level to be worn).
    TC_GAME_API void Enter(Player* player);
    TC_GAME_API void Leave(Player* player);

    // WorldSession login, after the character has loaded: restores a character
    // that left a match without Leave() running, or re-applies the rules to one
    // that logged back into the match it left.
    TC_GAME_API void OnLogin(Player* player);

    // Player::InitTalentForLevel and ActivateSpec: true while talents must not be
    // reset, the specs folded together or switched.
    TC_GAME_API bool SuppressTalentReset(Player const* player);

    // The cast opcode: the rank this cast should use, the spell itself when it is
    // not governed, or nullptr when it is locked.
    TC_GAME_API SpellInfo const* AdjustCast(Player const* player, SpellInfo const* spellInfo);
    // Spell::CheckCast: true when this spell is not unlocked in the match yet.
    TC_GAME_API bool IsSpellLocked(Player const* player, SpellInfo const* spellInfo);
    // Riding as a match counts it - the shop steed held: 0, 75 (60%) or 150 (100%) -
    // for Plainsrunning's speed (Unit::UpdateSpeed); -1 outside a match.
    TC_GAME_API int32 MatchRidingSkill(Player const* player);
    // Player::SendInitialSpells: whether the client's spellbook lists this spell.
    // In a match only the best unlocked rank of each ability is shown (spells stay
    // known server side, so nothing is written down or relearned); outside one,
    // `active` as always. Enter, a pick and Leave resend the list.
    TC_GAME_API bool ShowsInSpellbook(Player const* player, uint32 spell, bool active);
    // Player::CanEquipItem: in a match a character wears the field kit and what
    // the shop sells, nothing it brought (its own gear is stashed at the door, but
    // a piece left in its bags would otherwise go straight back on). True outside
    // a match, and for the shirt and tabard.
    TC_GAME_API bool MayEquip(Player const* player, uint32 entry, uint8 slot);
    // In a match only shop items can be used.
    TC_GAME_API bool IsItemLocked(Player const* player, uint32 entry);
    // A talent learned in a match puts its old action button back.
    TC_GAME_API void OnTalentLearned(Player* player);

    // League's inventory: shop items are not worn but held (six item slots and a
    // trinket slot, drawn by CENTURION_Rift); what the held ones give is put on
    // the champion as auras. SyncItems re-reads the bags (BattlegroundSRF, twice
    // a second, and after a purchase); HasRoomFor refuses a purchase with the
    // slots full, the components going into it counted out.
    TC_GAME_API void SyncItems(Player* player);
    // The shop items a champion holds, in bag order (CENTURION_Rift's slot order),
    // for the scoreboard.
    TC_GAME_API std::vector<uint32> HeldShopItems(Player const* player);
    // One CCGAME line to a real client (not a bot): the Rift's announcer and
    // scoreboard feed (BattlegroundSRF).
    TC_GAME_API void Tell(Player* player, std::string const& line);
    TC_GAME_API bool HasRoomFor(Player const* player, uint32 entry, std::vector<uint32> const& consumed);

    // Match experience, in League's own units (a melee minion is ~59).
    TC_GAME_API void AddExperience(Player* player, float leagueXp);
    // Rift gold, in League gold (1 League gold = 1 gold). With popup, the
    // CENTURION_Rift addon floats "+N gold" over the character (RIFTGOLD line).
    TC_GAME_API void AddGold(Player* player, uint32 leagueGold, bool popup = false);
    // League's build paths (world `rift_item_recipe`): an item bought while the
    // character holds some of its components costs its price less theirs - all
    // the way down the tree, a missing component priced the same way - and the
    // components held go into it. `price` is the vendor's price in copper; the
    // result is what this character pays, and `consumed` the item entries to take
    // once the purchase has gone through (Player::BuyItemFromVendorSlot).
    TC_GAME_API uint32 ShopPrice(Player* player, uint32 item, uint32 price, std::vector<uint32>& consumed);
    TC_GAME_API void TakeComponents(Player* player, std::vector<uint32> const& consumed);
    // The League level the character has reached this match (1..18).
    TC_GAME_API uint8 GetLeagueLevel(Player const* player);

    // Skill picks and talent points are spent at the character's own base only
    // (Player::LearnTalent, HandlePickRequest). True outside a match. When it is
    // false and `tell` is set, the character is told where to go.
    TC_GAME_API bool CanSpendPoints(Player* player, bool tell);

    // Player::GiveLevel, whatever called it: a level above the match level
    // becomes the match level, with the picks of every level gained.
    TC_GAME_API void OnLevelChanged(Player* player);

    // BattlegroundSRF's update, every few seconds per player (nothing to do at
    // present: the match's talents are the character's own).
    TC_GAME_API void Maintain(Player* player);

    // CCGAMEREQ "RIFTPICK:<spellId>" (the addon's pick) and "RIFTSTATE:" (the
    // addon asking for the current state, e.g. after /reload).
    TC_GAME_API void HandlePickRequest(Player* player, std::string_view argument);
    TC_GAME_API void HandleStateRequest(Player* player, std::string_view argument);
    // Tells CENTURION_Rift whether the character is in its fountain ("RIFTBASE:1"
    // / "RIFTBASE:0"): the pick panel only shows there, and walking in opens the
    // talent frame when there are points to spend. BattlegroundSRF sends it on
    // every change.
    TC_GAME_API void SendBase(Player* player, bool atBase);
}

#endif
