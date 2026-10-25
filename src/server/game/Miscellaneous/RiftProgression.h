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
#include <utility>
#include <vector>

class AuraEffect;
class Player;
class SpellInfo;
class Unit;
struct SpellModifier;

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
// out, "CCGAMEREQ RIFTPICK:<spell>" back). One pick a level, as League's skill
// points: a level-60 mage has 46 picks by 55 for its 106 rank steps, so what to
// learn and what to rank up is the player's choice (owner, 2026-10-22: keep it).
// A rogue's poison coats are picks too, each rank offered from the level the
// classic poison trainers taught it (AddPoisonChains).
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
    // A world character in a match: its skills follow the tournament's rules there
    // (Tournament::AlwaysMaxWeaponSkill, AlwaysMaxSkillForLevel), its real ones come back
    // on the way out. Safe to ask from anywhere: it takes no lock but its own.
    TC_GAME_API bool UsesMatchSkills(Player const* player);

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
    // A talent learned in a match puts its old action button back, and a spell it
    // gives that is on no button goes on a free one.
    TC_GAME_API void OnTalentLearned(Player* player, uint32 talentSpell);
    // The armsmaster at each fountain (npc_rift_armsmaster): the field kit's
    // weapon shapes this champion can take now (Tournament::RiftWeaponShape
    // values), the weapon types for one (ITEM_SUBCLASS_WEAPON_*), its current
    // choice, and the choice itself - the hands dressed at once, and kept for its
    // matches (`custom_rift_weapons`). Names for the menu.
    TC_GAME_API std::vector<uint8> WeaponShapes(Player* player);
    TC_GAME_API std::vector<uint32> WeaponTypes(Player* player, uint8 shape);
    TC_GAME_API std::pair<uint8, int32> CurrentWeapons(Player* player);
    TC_GAME_API bool ChooseWeapons(Player* player, uint8 shape, int32 subClass);
    TC_GAME_API char const* WeaponShapeName(uint8 shape);
    TC_GAME_API char const* WeaponTypeName(uint32 subClass);
    // CMSG_SET_ACTION_BUTTON emptying `button`: false when the client's "empty"
    // answers bars sent before the server filled the button, moments ago - the
    // client clears every empty and unknown button of each SMSG_ACTION_BUTTONS
    // and says so (PublishBars).
    TC_GAME_API bool KeepsButton(Player* player, uint8 button);
    // Player::LearnSpell: whether learning a spell the talent reset put aside
    // learns its disabled higher ranks back too. Not in a match: Trueshot
    // Aura's talent handed back ranks 2 and 3 there, which are skill picks.
    TC_GAME_API bool LearnsHigherRanks(Player const* player);

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
    // BattlegroundSRF, as the champions level: the match's League -> WoW scale and
    // the level armour is read at (RiftLeague). SyncItems converts the
    // champion's League stats at these.
    TC_GAME_API void SetMatchScale(Player* player, float scale, uint8 combatLevel);
    // That scale: a League number times it is the WoW one (0 outside a match).
    TC_GAME_API float MatchScale(Player const* player);
    // What a champion's class and spec build toward - the bots' item lists:
    // 0 melee, 1 ranged, 2 caster, 3 healer, 4 tank (its real talents' main
    // tree). BattlegroundSRF hands out League's positions by it.
    TC_GAME_API uint8 BuildRole(Player const* player);
    // A basic attack at range, as League counts one: the repeating shot (Auto
    // Shot, a wand's Shoot) or a warrior's or rogue's Shoot and Throw.
    TC_GAME_API bool IsRangedBasicAttack(SpellInfo const* info);
    // League's life steal (Vampiric Scepter, the Bloodthirster): Unit's melee and
    // spell damage paths, after a champion's basic attack (`spell` null for a
    // swing) has dealt `damage`. Nothing from a structure.
    TC_GAME_API void OnBasicAttackDamage(Unit* attacker, Unit* victim, uint32 damage, SpellInfo const* spell);
    // The Blessing of the Lizard Elder's slow and burn on whoever a champion's
    // basic attack struck (OnBasicAttackDamage).
    TC_GAME_API void LizardBurn(Player* attacker, Unit* victim, bool ranged);
    // League's cooldown reduction on the champion's own class spells - the items',
    // the Crest of the Ancient Golem's and Mejai's at 20 stacks, 40% at most - is a
    // real SPELLMOD_COOLDOWN pct modifier, so the client works the shortened
    // cooldowns out itself - tooltips, timers, its "not ready" check - as Violet
    // Hold's Boon of Alacrity does. The carrier's aura script (94170, spell_rift_cdr)
    // builds it here (DoEffectCalcSpellMod), from its amount, on a mask that gives
    // each class spell one bit (the client adds a modifier once per bit).
    TC_GAME_API void CooldownMod(AuraEffect const* carrier, SpellModifier*& mod);
    // The match's shorter ranges: Summoner's Focus (94153, spell_rift_focus) keeps a
    // SPELLMOD_RANGE pct modifier of its third effect's amount (-30) on every
    // champion, over the class spells cast at a range - melee spells keep theirs.
    // The client is told it like any modifier (Player::AddSpellMod), so its own
    // range checks and the bars' out-of-range red agree with the server's.
    TC_GAME_API void RangeMod(AuraEffect const* carrier, SpellModifier*& mod);
    // What SpellHistory::StartCooldown still takes off itself: the share, 0 for
    // none, for the few class spells the modifier cannot reach. `clientMisreads`
    // is set when the client's own cooldown will be wrong (a spell left out, or
    // one with two bits on the mask), so it is re-told after SMSG_SPELL_GO.
    TC_GAME_API float CooldownReduction(Player const* player, SpellInfo const* spellInfo, bool* clientMisreads = nullptr);
    // League's flat magic penetration (runes, Sorcerer's Shoes), in League's
    // points: Unit::CalculateAverageResistReduction takes it off the target's
    // magic resist, converted back (RiftLeague::FromWowResist). 0 outside a match.
    TC_GAME_API float MagicPenetration(Player const* player);
    // The magic resist the champion's shop items give, in League's points - not its class's
    // base or its rune page. Under League's rules only this can make a no-damage spell (a
    // Polymorph, a Fear) miss outright (WorldObject::MagicSpellHitResult). 0 outside a match.
    TC_GAME_API float GearMagicResist(Player const* player);
    // BattlegroundSRF: a champion killed a unit (minion, monster, champion), and a
    // champion died - the Bloodthirster's stacks, Seeker's Armguard's, Greed's gold,
    // Mejai's and the Sword of the Occult's losses.
    TC_GAME_API void OnUnitKill(Player* player);
    TC_GAME_API void OnDeath(Player* player);
    // BattlegroundSRF: a champion's kill (`kill`) or assist on an enemy champion -
    // Mejai's Soulstealer and Sword of the Occult (2 stacks, 1), Athene's mana.
    TC_GAME_API void OnChampionTakedown(Player* player, bool kill);
    // BattlegroundSRF's passive gold tick: the items' gold every 10 sec (Philosopher's
    // Stone, Kage's Lucky Pick, Avarice Blade) for `diff` ms.
    TC_GAME_API void ItemGoldTick(Player* player, uint32 diff);

    // League's item effects the core asks for, 0 outside the Rift: flat armor
    // penetration in League's points (Unit::CalcArmorReducedDamage), percent magic
    // penetration (before the flat: Unit::CalculateAverageResistReduction), percent
    // off slows (Unit::UpdateSpeed) and tenacity, percent off a crowd-control spell's
    // duration (WorldObject::ModSpellDuration).
    TC_GAME_API float ArmorPenetration(Player const* player);
    TC_GAME_API float MagicPenetrationPct(Player const* player);
    TC_GAME_API float SlowResist(Player const* player);
    TC_GAME_API float Tenacity(Player const* player, SpellInfo const* spellInfo, uint32 effectMask);
    // And what custom_rift_items.cpp's hooks need, in League's numbers (Scale makes
    // them WoW's): basic attacks blocked, health on hit, monsters maimed and butchered,
    // healing amplified, regeneration after a champion's damage, Cold Steel, spell vamp.
    struct ItemHooks
    {
        bool InMatch = false;
        float BasicBlockPct = 0.0f, ChampionBasicBlock = 0.0f, OnHitHeal = 0.0f, Maim = 0.0f, Butcher = 0.0f;
        float HealAmp = 0.0f, ChampionHitRegen = 0.0f, ColdSteelAs = 0.0f, ColdSteelMs = 0.0f, SpellVamp = 0.0f;
        float Scale = 1.0f;
    };
    TC_GAME_API ItemHooks ItemHooksOf(Player const* player);
    // Spell vamp: an ability's damage back as health (a third of it for an area one),
    // from OnBasicAttackDamage and the periodic damage hook.
    TC_GAME_API void SpellVamp(Player* player, Unit* victim, uint32 damage, SpellInfo const* spell);
    // League's base attack damage at the champion's League level (no runes, no
    // items) in WoW's numbers: what Sheen's and Trinity Force's Spellblade add.
    TC_GAME_API float BaseAttackDamage(Player const* player);

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
    // CCGAMEREQ "RIFTBUY:<entry>": the shop's purchase, at the fountain (a 3.3.5
    // vendor lists 150 items at most, League's shop more).
    TC_GAME_API void HandleBuyRequest(Player* player, std::string_view argument);
    // Tells CENTURION_Rift whether the character is in its fountain ("RIFTBASE:1"
    // / "RIFTBASE:0"): the pick panel only shows there, and walking in opens the
    // talent frame when there are points to spend. BattlegroundSRF sends it on
    // every change.
    TC_GAME_API void SendBase(Player* player, bool atBase);
}

#endif
