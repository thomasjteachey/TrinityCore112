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

#ifndef TRINITYCORE_ALL_STAR_H
#define TRINITYCORE_ALL_STAR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

class Player;
class WorldSession;

// All-Star arena (BATTLEGROUND_ASA, owner 2026-10-28): "when you die, you will
// switch to a random other class on your account, until you die with all 9
// classes. if you do not have a level 60 character with that class, it gives
// you a default loadout", then "it should BE your other character", "the order
// in which you switch characters should be the order they are in your
// character list", and "as soon as you die, you need to immediately spawn in as
// your new class ... maybe it shouldn't be a literal login".
//
// Every player has four lives (Centurion.AllStar.Lives; owner 2026-10-29:
// "change all star mode to 4 lives"), each a different class. Life one is the
// character they queued with. Lives two to four are what the player picked on
// the Arena Master's All-Star page (owner: "allow players to rearrange what
// characters they pick and in what order"): any level-60 character of the
// account, or a class's standard kit, in the order they set; it is kept per
// character (character_allstar_lineup) and checked again when the match reads
// it. A life left on "Random" is filled from the rest as before: the account's
// character list, top to bottom (CharacterScreen::GetListPosition), each
// level-60 character of a class not in the lineup yet, then - for every class
// the account has no level-60 of - a random level-60 playerbot character of
// that class (a stand-in; bots only), or that class's tournament starter
// template (Startwarrior and the rest, Centurion.AllStar.TemplateCharacters).
// With Centurion.AllStar.ShuffleLives (the default) that rest is taken in a
// random order.
//
// A death is a death for four seconds (Centurion.AllStar.RespawnDelay); then the
// player stands up where they fell as the next character of the lineup. The swap is made IN PLACE, on the live character -
// same guid, same session, same seat - so there is no loading screen: what the
// next character is made of - its name, race and looks, spells, talents,
// glyphs, skills, action bars, gear, ammo and hunter pet - is read off its rows
// into a Kit ahead of time, and the Kit is put on the player. Everyone in the
// match is told the new name, race and class (the name query answer for the
// player's guid), so it reads as that character; a template keeps the player's
// own name. The player's own state is held aside in memory (PersonaStash) and
// put back when the match lets go of them.
//
// Nothing of a persona is ever written down. While a persona is on, every route
// that saves a character - Player::SaveToDB, SaveInventoryAndGoldToDB, the pet
// saves, ResetTalents, ActivateSpec - leaves the character's rows alone, so the
// database keeps what the character was the moment it first changed class (it
// is saved once, just before). A crash or a restart mid-match therefore loses
// nothing but what the match itself paid since then; the next login is the real
// character, and the tournament loadout's own rows give its gear back as ever.
// Every other way out - the match ending, the leave button, a logout, a
// disconnect - restores the stash first (Battleground::RemovePlayerAtLeave,
// WorldSession::LogoutPlayer) and only then lets the save happen. What the
// match pays (honor, kills) belongs to the character that queued; its end-of-
// match honor and Mark of Honor wait for that restore (DeferMatchReward). The
// honor is Buffle Shuffle 3v3's payout (BattlegroundMgr::GetHonorPayoutKey)
// times the player's lives, split evenly between the lives played
// (Arena::ShareEndOfMatchHonor); another character's share is paid at its
// next login (QueueLifeHonor).
//
// Bots (the transient copies that fill a match) play by the same rules, every
// class after their first from the starter templates: a copy has no character
// list of its own, and it never saves anyway.
namespace AllStar
{
    // ------------------------------------------------------------------ config
    // Centurion.AllStar.*, read in World::LoadConfigSettings (.reload config).
    void LoadConfig();
    bool IsEnabled();
    uint32 GetRespawnDelayMs();     // death -> back at the start as the next character
    uint32 GetTimeLimitMs();        // from the gates opening; then the most lives left wins
    uint32 GetLives();              // lives a player has (Centurion.AllStar.Lives, 1-9)
    // The tournament starter template that stands in for a class nobody has.
    ObjectGuid::LowType GetTemplateCharacter(uint8 playerClass);

    // The nine classes a lineup draws from, in the order the templates fill
    // the end of one.
    std::array<uint8, 9> constexpr CLASS_ORDER = { 1, 2, 3, 4, 5, 7, 8, 9, 11 };
    // Hidden, on every seat for its match: its stack count is the lives left,
    // which the name tag shows beside the World/Tournament/Bot marker. A
    // spell_dbc row (stackable clone of the 92011 Tournament Character marker).
    uint32 constexpr LIVES_AURA = 92013;
    bool IsPlayableClass(uint8 playerClass);
    char const* ClassName(uint8 playerClass);

    // ------------------------------------------------------------------ kits
    // Everything one character's class is made of, as read off its rows. Kits
    // are loaded on the world thread (Update drains the callbacks) and read
    // from any thread afterwards; a Kit never changes once it is published.
    struct Kit;
    using KitPtr = std::shared_ptr<Kit const>;

    // Ask for a character's kit. Cheap to repeat: a kit already loaded or on
    // its way is not asked for twice. Template kits are kept for the life of
    // the server; a player's own are dropped with ForgetKit when the match ends.
    void RequestKit(ObjectGuid::LowType source);
    // nullptr while the kit is still loading or could not be read.
    KitPtr FindKit(ObjectGuid::LowType source);
    // True when the kit was asked for and could not be built (no such row,
    // not one of the nine classes): the caller falls back to the template.
    bool KitFailed(ObjectGuid::LowType source);
    void ForgetKit(ObjectGuid::LowType source);
    uint8 GetKitClass(Kit const& kit);
    std::string const& GetKitName(Kit const& kit);
    bool IsTemplateKit(Kit const& kit);

    // ---------------------------------------------------------------- lineups
    struct LineupEntry
    {
        uint8 Class = 0;
        ObjectGuid::LowType Source = 0;     // 0 for the character that queued
        bool Template = false;
    };
    using Lineup = std::vector<LineupEntry>;

    // A player's lives: the character that queued, then their picks, then the
    // rest of their account's character list. Asked once per seat; `playing`
    // is the character that queued (life one).
    void RequestLineup(uint32 accountId, ObjectGuid::LowType playing, uint8 playingClass);
    // false while the answer is on its way.
    bool FindLineup(uint32 accountId, ObjectGuid::LowType playing, Lineup& out);
    void ForgetLineup(uint32 accountId, ObjectGuid::LowType playing);
    // A bot's: its own class, then stand-ins/templates for the rest (in a
    // random order with Centurion.AllStar.ShuffleLives).
    Lineup BuildBotLineup(uint8 playingClass);
    // The Arena Master's All-Star page - the lives this character plays and
    // what it can pick for them:
    //   AM:LU:BEGIN:<shuffled>:<lives>:<class>:<name>   life one
    //   AM:LU:O:c<guid>:<class>:<name>                  a character it can pick
    //   AM:LU:P:<pick>,<pick>,...                       lives two on: c<guid>,
    //                                                   s<class> (standard kit)
    //                                                   or - (random)
    //   AM:LU:END
    // Any thread; answered from Update. The page sends CCGAMEREQ
    // ALLSTAR:PICK:<pick>,<pick>,... back, answered with the list again.
    void SendMasterLineup(Player* player);

    // World::Update: hands finished loads to the waiting matches.
    void Update();

    // --------------------------------------------------------------- personas
    struct PersonaStash;
    struct PersonaStashDeleter { TC_GAME_API void operator()(PersonaStash* stash) const; };
    class PersonaAccess;            // the friend of Player that does the swapping

    // Put a kit on a live, alive character. The first one saves the character
    // and puts its own class aside; later ones replace the persona before.
    // Returns false (and changes nothing) when the kit cannot be worn.
    bool ApplyPersona(Player* player, KitPtr const& kit);
    // Put the character's own class back. Safe to call on anybody: a no-op
    // without a persona. Battleground::RemovePlayerAtLeave and
    // WorldSession::LogoutPlayer call it before anything is saved.
    void RestorePersona(Player* player);
    // The class the character really is (its own, persona or not).
    uint8 GetRealClass(Player const* player);
    // The name the persona goes by, family name included ("" without one, or
    // for a template).
    std::string GetPersonaName(Player const* player);
    bool IsPersonaFromTemplate(Player const* player);

    // WorldSession::SendNameQueryOpcode: what to answer for a guid wearing a
    // persona - its source's name (with its family name), race, gender and
    // class. false for everybody else, who are answered as ever. The player's
    // own client is never given it: Centurion addons check the server's
    // whispers against UnitName("player"). Safe from any thread.
    // `viewer`: the character being answered, remembered so the real name
    // reaches it when the persona comes off, wherever it is by then.
    bool GetPersonaNameData(ObjectGuid guid, std::string& name, uint8& race, uint8& gender, uint8& playerClass,
        ObjectGuid viewer = ObjectGuid::Empty);
    // A scoreboard line for a life that is over (Arena::KeepAllStarPastLife):
    // a guid of its own, which GetPersonaNameData answers until it is forgotten.
    ObjectGuid::LowType constexpr PAST_LIFE_GUID_BASE = 0xF0000000;
    ObjectGuid NamePastLife(std::string const& name, uint8 race, uint8 gender, uint8 playerClass);
    void ForgetPastLife(ObjectGuid guid);

    // True while ApplyPersona/RestorePersona is moving gear: the battleground
    // gear lock stands aside for it, the way it does for the tournament loadout.
    bool IsSwapInProgress(Player const* player);
    // MiscHandler: for a few seconds after a swap every action button change
    // the client sends is ignored. They are its answers to the swap itself -
    // "empty" for each button of the bars packet it has not caught up with,
    // and a button moved up a rank for each SMSG_LEARNED_SPELL - and taken as
    // the player's they rewrote bars, the real ones included.
    bool IgnoresClientButtons(Player const* player);
    // MiscHandler, CMSG_UPDATE_ACCOUNT_DATA: true when the upload is the
    // client's macros while it holds a borrowed character's in this
    // character's slots - they are not stored, and the real ones are kept a
    // second newer than the client's copy so its next login downloads them.
    bool KeepsAccountData(WorldSession* session, uint32 type, uint32 clientTime);
    // CENTURION_AllStar's own lives counter:
    // ALLSTAR:LIVES:<left>:<total>:<class>:<name of the life being played>.
    void SendLives(Player* player, uint32 left, uint32 total);
    // A persona's hunter pet arrives a moment after the persona (LoadPetFromDB
    // answers asynchronously): its trained spells are taught then. Called from
    // the match's update.
    void MaintainPersona(Player* player);

    // ------------------------------------------------------------- rewards
    // Battleground::EndBattleground, for a character still wearing a persona:
    // its end-of-match honor and (on a win) the restored Mark of Honor are paid
    // when RestorePersona puts the character back, not now. Worn, the bags are
    // the persona's copies - anything stored there is destroyed with them, the
    // Warchief's Socks the honor brings included - its class picks which
    // depleted mark is eligible, and its spells decide the honor cap. False
    // without a persona: the caller pays as usual.
    bool DeferMatchReward(Player* player, uint32 honor, bool restoreMark);
    // Owner 2026-10-30: "grant honor for every character that participated
    // (respecting honor caps)". A character of the account's share of the
    // honor for playing one of a player's lives, held in character_allstar_honor and paid
    // at its next login (PayQueuedLifeHonor) through Player::RewardHonor, so
    // its own honor cap, Rate.Honor and honor mirror apply as to any award.
    // `account` must own the character; a row for anyone else is not written.
    void QueueLifeHonor(ObjectGuid::LowType character, uint32 account, uint32 honor);
    // WorldSession::HandlePlayerLogin: pays what QueueLifeHonor held.
    void PayQueuedLifeHonor(Player* player);
}

#endif // TRINITYCORE_ALL_STAR_H
