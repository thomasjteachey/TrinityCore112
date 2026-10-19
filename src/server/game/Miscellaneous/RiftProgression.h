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

#include <string_view>

class Player;
class SpellInfo;

// Summoner's Rift progression: every match starts at level 1 and climbs back to
// the character's real level, the way a League match climbs from 1 to 18.
//
// Nothing about the character is taken away to do it. The level drop is real
// (Player::GiveLevel, borrowed), but the two things a lower level would destroy
// are protected instead of lost:
//
//  - Talents stay learned. InitTalentForLevel would reset and SAVE them the
//    moment the level fell below what they cost, so it is told to stand aside
//    (SuppressTalentReset). Their effects are switched off instead - passive
//    talent auras removed, talent abilities refused - and come back, tier by
//    tier, as the match level earns the points they cost.
//  - Spells stay known. Casting one is refused until it is unlocked (Spell::
//    CheckCast, AdjustCast); a higher rank than the one unlocked casts the
//    unlocked rank instead, so the action bar keeps working.
//
// Each match level hands out skill picks: the player chooses which spell to
// learn or rank up, through the CENTURION_Rift addon (CCGAME "RIFTPICKS" lines
// out, "CCGAMEREQ RIFTPICK:<spell>" back). Picks are spread so that by the real
// level everything the character knows is unlocked.
//
// Rift gold is the character's money slot: the real money is put aside, the
// match is played from a fixed purse, and the real money comes back.
//
// What comes back, and when: the real level, XP and money are written to
// `custom_rift_stash` before anything changes, so leaving by any route -
// the match ending, the leave button, a disconnect, a crash, a restart -
// ends the same way: Leave(), or OnLogin() at the next login, gives it all back.
namespace RiftProgression
{
    // World startup: is `custom_rift_stash` there? A realm without it never
    // enters the progression (the Rift then plays at the character's own level).
    void ProbeStorage();

    // True while this character is playing a Rift match under these rules.
    TC_GAME_API bool IsInMatch(Player const* player);

    // BattlegroundSRF::AddPlayer / RemovePlayer.
    TC_GAME_API void Enter(Player* player);
    TC_GAME_API void Leave(Player* player);

    // WorldSession login, after the character has loaded: restores a character
    // that left a match without Leave() running, or re-applies the rules to one
    // that logged back into the match it left.
    TC_GAME_API void OnLogin(Player* player);

    // Player::InitTalentForLevel: true while talent points must not be reset.
    TC_GAME_API bool SuppressTalentReset(Player const* player);

    // The cast opcode: the rank this cast should use, the spell itself when it is
    // not governed, or nullptr when it is locked.
    TC_GAME_API SpellInfo const* AdjustCast(Player const* player, SpellInfo const* spellInfo);
    // Spell::CheckCast: true when this spell is not unlocked in the match yet.
    TC_GAME_API bool IsSpellLocked(Player const* player, SpellInfo const* spellInfo);

    // Match experience, in League's own units (a melee minion is ~59).
    TC_GAME_API void AddExperience(Player* player, float leagueXp);
    // Rift gold, in League gold (1 League gold = 1 silver).
    TC_GAME_API void AddGold(Player* player, uint32 leagueGold);
    // The League level the character has reached this match (1..18).
    TC_GAME_API uint8 GetLeagueLevel(Player const* player);

    // BattlegroundSRF's update, every few seconds per player: anything the
    // engine re-applied behind the rules' back (a passive talent aura on a form
    // change or a resurrection) is taken off again.
    TC_GAME_API void Maintain(Player* player);

    // CCGAMEREQ "RIFTPICK:<spellId>" (the addon's pick) and "RIFTSTATE:" (the
    // addon asking for the current state, e.g. after /reload).
    TC_GAME_API void HandlePickRequest(Player* player, std::string_view argument);
    TC_GAME_API void HandleStateRequest(Player* player, std::string_view argument);
}

#endif
