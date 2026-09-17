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

#ifndef TRINITY_PLAYERBOT_CTF_COORDINATOR_H
#define TRINITY_PLAYERBOT_CTF_COORDINATOR_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <string>
#include <vector>

class Battleground;
class Player;

namespace playerbot
{
// Team play for the two-flag capture the flag battlegrounds, Warsong Gulch and
// Twin Peaks.
//
// Each team names ONE flag runner: the best living bot by class - any druid,
// then a protection warrior, then any mage, then a hunter, then anybody - and
// the next in line takes over the moment the runner dies. Only the runner
// crosses the map for the enemy flag. Everyone else takes a flag only when
// already standing near it, returns their own dropped flag, and otherwise
// escorts the runner, holds the flag room, or fights. A carrier that is a worse
// runner and meets the runner with no enemy in sight drops the flag at the
// runner's feet.
//
// The plan is kept per team per battleground instance and rebuilt lazily from
// the bots' own decision ticks. All bots of one instance tick on that map's
// update thread under the per-map decision lock, so a plan's contents are only
// touched by one thread at a time; the container itself follows the cross-map
// rules in PlayerbotSharedStateGuard.h.
enum class CtfRole : uint8
{
    None = 0,
    Carrier,    // holds the enemy flag
    Runner,     // the designated flag runner, not carrying
    Escort,     // stays with our carrier or the runner
    Defender,   // holds our flag room, hunts the enemy carrier
    Midfield    // fights wherever the fight is
};

char const* GetCtfRoleName(CtfRole role);

struct CtfBotOrders
{
    CtfRole role = CtfRole::None;
    bool isDesignatedRunner = false;

    ObjectGuid runnerGuid;
    ObjectGuid teamCarrierGuid;   // our side's holder of the enemy flag, possibly this bot
    ObjectGuid enemyCarrierGuid;  // the enemy holding our flag

    bool enemyFlagPickable = false;  // on its stand or lying on the ground
    bool ownFlagAtBase = false;
    Position ownFlagStand;
    Position enemyFlagStand;

    // The flag object this bot should walk to and use, or empty.
    ObjectGuid pickupGuid;
    bool pickupIsReturn = false;         // our own dropped flag
    bool pickupIsOpportunistic = false;  // allowed only because the bot is near it
    bool pickupNearby = false;           // close enough to click now

    // Carrying, our flag is away, and the bot is already in its own flag room:
    // it can stand and fight there instead of guarding a run it cannot finish.
    bool carrierHolding = false;

    bool handoffGive = false;
    bool handoffReceive = false;
    ObjectGuid handoffPartnerGuid;
};

class CtfCoordinator
{
public:
    static bool IsTwoFlagCtf(Battleground const* battleground);

    // This bot's orders. False, with role None, outside an in-progress Warsong
    // Gulch or Twin Peaks match.
    static bool GetOrders(Player const* bot, CtfBotOrders& orders);

    // True for the runner a flag was just dropped for. That flag is kept for
    // the runner for a few seconds (GetOrders offers it to nobody else), and
    // the runner picks it up without the usual dropped-flag hesitation.
    static bool IsHandoffReceiverFor(Player const* bot, ObjectGuid const& flagGuid);

    // The giving carrier calls this once it stands next to the runner. The flag
    // goes down through the battleground's own drop event and is reserved for
    // the runner. True when the flag was dropped.
    static bool DropFlagForHandoff(Player* giver);

    // One line per team, for .playerbot pvp ctf.
    static std::vector<std::string> DescribeTeams(Player const* observer);
};
}

#endif
