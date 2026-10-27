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

#ifndef CUSTOM_BOUNTY_WRITS_H
#define CUSTOM_BOUNTY_WRITS_H

#include "Define.h"

#include <string_view>
#include <vector>

class GameObject;
class Player;

// Vengeance writs: a price on whoever killed you.
//
// Killed out in the open world - by a world bot or by another player - you
// have an hour to walk up to any bounty board and put gold on your killer's
// head. The board holds the gold. Whoever kills the target next earns it and
// collects it at any board - but not on the poster's own account, and not while
// grouped with the target. A writ nobody earns within a day goes back to its
// poster by mail, less the board's fee. Posters stay anonymous; a player with a
// price on their head is told how much, never by whom.
//
// Everything is a Centurion.BountyBoard.Writs.* switch, read on .reload config.
namespace BountyWrits
{
    bool Enabled();

    // --- the board (world thread) -------------------------------------------

    // BBWO when a board opens: whether writs are on at all.
    void SendBoardState(Player* viewer);

    // BBWZ with a zone's notices: the price on each bot posted there.
    void SendZoneWrits(Player* viewer, uint32 zoneId, std::vector<Player*> const& bots);

    // BBPW with a poster: the price on that bot.
    void SendPosterWrit(Player* viewer, Player* bot);

    // WRITS:0 (the page) and WRIT:<guid>:<copper> (put a price on a killer).
    // True when the verb was one of these.
    bool HandleRequest(Player* player, std::string_view verb, std::string_view argument);

    // --- the board's own click (map thread) ---------------------------------

    // Touching a board pays out every writ the player has earned.
    void OnBoardTouched(Player* player);

    // The no-addon gossip.
    void AddGossipOptions(Player* player);
    // World thread (CMSG_GOSSIP_SELECT_OPTION). True when it was ours.
    bool OnGossipSelect(Player* player, GameObject* board, uint32 sender, uint32 action);
}

#endif // CUSTOM_BOUNTY_WRITS_H
