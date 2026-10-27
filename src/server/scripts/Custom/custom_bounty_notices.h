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

#ifndef CUSTOM_BOUNTY_NOTICES_H
#define CUSTOM_BOUNTY_NOTICES_H

#include "Define.h"

#include <string_view>
#include <vector>

class GameObject;
class Player;

// Notices: what you can do with a bounty board besides read it.
//
// A notice on the board names a world bot. Take it and it becomes a quest in
// your log, in one of three kinds:
//
//   HUNT       Find the bot and kill it. War Mode only - the bot is fair game
//              to you exactly as it already is, the notice only tells you where
//              to look. An aggressive bot hears it is wanted and calls a friend
//              or two to its side.
//
//   MOK'GORA   Open to everyone. The bot comes to YOU - not red, not
//              attackable, not attacking - and challenges you to a duel to the
//              death. Win it and the notice is yours. Refuse it, let it lapse,
//              or tear the notice up, and the quest fails and the bot takes it
//              personally: it is hostile to you, War Mode or not, and it comes
//              after you.
//
//   RECOVERY   The bot is carrying gear it took off your body. Free, open to
//              everyone: for as long as it still holds your gear, the bot is
//              hostile to you and you to it, War Mode or not, so you can go
//              and take it back off its corpse.
//
// A finished notice is handed in by touching any board, which pays coin,
// experience and seals, and gives you the poster to keep. Collected notices
// climb the Hunter's Ledger - Tracker, Bloodhound, Manhunter, Headsman - and
// each rank reads more off the board. Seals buy pardons: notoriety struck off
// at a board.
namespace BountyNotices
{
    bool Enabled();

    // --- the board, world thread --------------------------------------------

    // After a zone's notices: what this viewer in particular may see of them -
    // their own notices, gear taken from their body, and what their Ledger
    // rank lets them read (where, temperament, coin).
    void SendBoardExtras(Player* viewer, uint32 zoneId, std::vector<Player*> const& bots);

    // After one poster: whose body each piece came off, and whether this
    // viewer may take each kind of notice on it.
    void SendPosterExtras(Player* viewer, Player* bot);

    // The viewer's Ledger and current notices, sent when a board opens.
    void SendLedgerAndNotices(Player* viewer);

    // TAKE, DROP, LEDGER and PARDON. True when the verb was one of ours.
    bool HandleRequest(Player* player, std::string_view verb, std::string_view argument);

    // --- the board's own click, map thread ----------------------------------

    // Hands in every finished notice the player carries. Called before the
    // board opens, so touching a board is the turn-in.
    void OnBoardTouched(Player* player, GameObject* board);

    // The gossip fallback for a client without the addon.
    void AddGossipOptions(Player* player);
    // World thread (CMSG_GOSSIP_SELECT_OPTION). True when it was ours.
    bool OnGossipSelect(Player* player, GameObject* board, uint32 sender, uint32 action);
}

#endif // CUSTOM_BOUNTY_NOTICES_H
