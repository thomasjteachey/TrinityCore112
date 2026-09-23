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

#ifndef TRINITYCORE_MOKGORA_H
#define TRINITYCORE_MOKGORA_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

class Player;

enum DuelCompleteType : uint8;

// Mok'gora - the duel to the death, as Hardcore realms play it.
//
// You walk up to someone, challenge them, and a box asks them - in as many
// words - whether they are willing to die. If they say yes the two of you fight
// an ordinary duel with one rule removed: the blow that would normally stop at
// one health is allowed to land. Anything goes. Potions, engineering, pets,
// bandages, a cliff - there is no honour clause, only a corpse.
//
// It is available to every character on the realm, not only to those who took
// the Hardcore mode. What losing COSTS you is what differs: a Hardcore
// character who dies here is gone for good (the challenge-mode hooks already
// treat any PvP death that way, so nothing special happens here for them),
// while everyone else takes a real death - corpse, spirit, the run back and
// resurrection sickness. There is no switch to make it permanent for everyone,
// because that is not what the feature does: Blizzard's own warning reads
// "death is permanent on Hardcore Realms", not "this duel is permanent". The
// duel supplies a corpse; the character's own ruleset decides what a corpse
// means.
//
// Every rule below is inert unless Centurion.Mokgora.Enable = 1, so a realm on
// the shared branch that has never heard of Mok'gora behaves exactly as before.
//
// --- the wire ------------------------------------------------------------
//
// The client half is the CENTURION_Mokgora addon. It talks on the realm's
// established addon channel: CCGAMEREQ from the client, CCGAME back. It is NOT
// required - every request has a spoken ".mokgora ..." equivalent and every
// answer is also written to the chat frame - but without it the challenged
// player reads the warning as text rather than as a box they have to click.
//
//   client -> server   CCGAMEREQ\tMOKGORA:CHALLENGE[:<name>]   (no name = target)
//                      CCGAMEREQ\tMOKGORA:ACCEPT
//                      CCGAMEREQ\tMOKGORA:DECLINE
//                      CCGAMEREQ\tMOKGORA:WITHDRAW
//                      CCGAMEREQ\tMOKGORA:RULES
//
//   server -> client   CCGAME\tMOKGORA:RULES:<on>:<range>:<secs>:<ring>:<cowarddays>
//                      CCGAME\tMOKGORA:OFFER:<who>:<level>:<class>:<seconds>
//                      CCGAME\tMOKGORA:SENT:<who>:<seconds>
//                      CCGAME\tMOKGORA:CLOSE:<reason>
//                      CCGAME\tMOKGORA:INCOMING:<who>       (auto-accept the duel)
//                      CCGAME\tMOKGORA:BEGIN:<who>
//                      CCGAME\tMOKGORA:END:<winner>:<loser>:<how>
//
// Every field is ":"-separated and no field can contain a colon: character
// names cannot, and the rest are numbers or one of a fixed set of words.
namespace Mokgora
{
    // Loaded from World::LoadConfigSettings, so `.reload config` applies it.
    void LoadConfig();
    bool IsEnabled();

    // How far apart the two may stand when the challenge is issued. You say
    // this to someone's face.
    float ChallengeRange();

    // The fighting ground: how far either of them may stray from the flag
    // planted between them, and how long they have to come back before it
    // counts as running. Read by Player::CheckDuelDistance in place of the
    // stock 50 yards and 10 seconds, and only for a Mok'gora - an ordinary
    // duel on the same realm keeps the numbers it always had.
    float BoundsYards();
    uint32 BoundsGraceSeconds();

    // Coward! - three days of -20% attributes, damage, armour and resistance,
    // worn by anyone who leaves the ring or logs out of it. 0 disables it.
    uint32 CowardSpell();

    // A bot never issues a Mok'gora, and takes up any a person throws at it on
    // the spot, with no offer box. game/ has no way to recognise one - the
    // account set lives in the Hardcore ruleset over in scripts/ - so the
    // Mok'gora script installs the answer here at startup. Until it does, only
    // virtual sessions count as bots.
    using PlayerPredicate = bool(*)(Player const*);
    void SetBotPredicate(PlayerPredicate predicate);

    // --- state ------------------------------------------------------------

    // This character's duel is a Mok'gora. True from the moment the flag is
    // planted until the duel is torn down - which INCLUDES the duel-end hook,
    // where the outcome is known but `Player::duel` still stands. That is
    // deliberate: it is the only way a caller in that hook can tell a Mok'gora
    // from an ordinary duel that happened to end the same way.
    bool IsMokgoraDuel(Player const* player);

    // This character is fighting one right now, blows actually landing.
    bool IsFighting(Player const* player);

    // This character has an offer outstanding - either one they made or one
    // they have been asked to answer.
    bool HasPendingOffer(Player const* player);

    // --- the challenge ----------------------------------------------------

    // Issue, answer or withdraw. Each one speaks to the player itself about
    // what happened, and each returns false only when it had nothing to do.
    // `targetName` empty means "whoever I have targeted".
    bool Challenge(Player* challenger, std::string const& targetName);
    bool Accept(Player* target);
    bool Decline(Player* target);
    bool Withdraw(Player* challenger);

    // --- hooks ------------------------------------------------------------

    // CMSG_MESSAGECHAT, before the command parser. Returns true when the
    // message was ours and the caller should drop it. Handles both the addon
    // protocol and the spoken ".mokgora ..." fallback, the second of which has
    // to live here because regular accounts may not run chat commands at all.
    bool HandleChatMessage(Player* sender, uint32 type, uint32 lang, std::string const& msg);

    // Player::DuelComplete, through the script hook. `winner` and `loser` are
    // as the core names them - for DUEL_INTERRUPTED neither of them really won.
    void OnDuelEnd(Player* winner, Player* loser, DuelCompleteType type);

    // Player::Update, when the countdown runs out and the fight is really on.
    void OnDuelStart(Player* first, Player* second);

    // WorldSession::LogoutPlayer, BEFORE the character is saved, so that a
    // forfeit is written to the database rather than lost with the session.
    void OnLogout(Player* player);

    void OnLogin(Player* player);

    // Once a second from World::Update: expires offers nobody answered.
    void Update();

    // For `.mokgora why`, the GM-side account of which gate said no.
    std::string Explain(Player* challenger, Player* target);
}

#endif // TRINITYCORE_MOKGORA_H
