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

#ifndef CUSTOM_BOUNTY_BOARD_H
#define CUSTOM_BOUNTY_BOARD_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;

// What the inn boards know, for the notices that are taken off them
// (custom_bounty_notices.cpp). Everything here is answered the way the board
// answers it - same rule for who is posted, same kill count, same wire.
namespace BountyBoard
{
    // Posted on a board right now: a world bot, alive, out in a contested zone.
    // World thread, like every board read.
    bool IsOnTheBoard(Player const* bot);

    // Killing blows on players over the character's whole life on the realm.
    uint32 LifetimeKills(ObjectGuid::LowType guid);

    // The name as a notice prints it - with the surname, if the bot has one.
    std::string NoticeName(Player const* bot);

    std::string ZoneName(uint32 zoneId);

    // Every zone a board stands in. "Every contested zone" for the keepsake set.
    std::vector<uint32> BoardZones();

    // Standing within reach of a board.
    bool IsAtABoard(Player const* player);

    // CCGAME\t<tag>:<payload>, to the board addon.
    void SendTagged(Player* viewer, std::string const& tag, std::string const& payload);
}

#endif // CUSTOM_BOUNTY_BOARD_H
