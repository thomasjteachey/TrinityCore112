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

#ifndef TRINITY_PLAYERBOT_NODE_COORDINATOR_H
#define TRINITY_PLAYERBOT_NODE_COORDINATOR_H

#include "Battleground.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <string>
#include <vector>

class Player;
class WorldObject;

namespace playerbot
{
// Team play for the base-capture battlegrounds: Arathi Basin and The Battle
// for Gilneas, and any other battleground that answers GetDynamicNodeCount.
//
// Battleground::GetNodeObjective hands every bot a base from a hash of its own
// guid: a fixed third of the team defends, the rest attacks, each one spread
// evenly over whatever bases are in that category, and neither the distance to
// the base nor the enemies standing on it are looked at. One player could
// therefore walk up to any base, meet the single bot the hash had parked
// there, and take it - and nobody came back, because the bots that were not
// in the defending third never looked at a base they already held.
//
// This coordinator plans for the TEAM instead. Every base gets a live score
// and a quota of bodies from its own state:
//
//   - a base of ours that is being taken outranks everything on the map, and
//     wants two bots plus one per enemy standing on it;
//   - a base of ours with enemies on it climbs by sixty a head, so a lone
//     attacker pulls the two nearest bots off whatever they were doing, and
//     one still capping for us is held the same way: it is the more fragile
//     of the two, since one click ends the minute it has been standing there;
//   - a base the enemy holds wants a squad of at least three, so the bots
//     arrive together rather than feeding in one at a time.
//
// A FREE base is read off the starting gates instead. One bot is sent to
// claim the base only we can reach first, none is walked across the map to
// the one only they can, and the whole rest of the team is shared over the
// bases in between - the midfield, which is where these matches are decided.
// Home ground is geometry, not a table of base names: a base counts as one
// side's when it is within three quarters of the distance from that side's
// gate. In Arathi Basin that is the Stables and the Farm and nothing else.
//
// Bodies are then handed out nearest-first, quotas before spares, with the
// base a bot already had worth a discount so assignments do not churn. Only
// LIVING bots fill a quota: killing a base's defenders re-opens it and pulls
// the next nearest bots in, which is the behaviour a solo capper never met.
// A cap on how much of the team may sit on defense keeps a losing side
// attacking - bases, not kills, win these matches.
//
// The plan is kept per team per battleground instance and rebuilt lazily from
// the bots' own decision ticks, exactly as CtfCoordinator does for the
// two-flag battlegrounds; see PlayerbotSharedStateGuard.h for the locking.
enum class NodeRole : uint8
{
    None = 0,
    Recapture,  // ours, being taken: get back and re-click the banner
    Defend,     // ours: sit on it
    Claim,      // free: take it
    Assault,    // part of the squad sent at one enemy-held base
    Roam        // no base wants a body: fight where the fight is
};

char const* GetNodeRoleName(NodeRole role);

struct NodeBotOrders
{
    NodeRole role = NodeRole::None;
    bool hasNode = false;
    uint32 nodeId = 0;
    BattlegroundNodeStatus status = BattlegroundNodeStatus::Neutral;
    Position location;
    ObjectGuid bannerGuid;

    // The banner has to be clicked: the base is not ours, or it is ours and
    // the enemy is taking it.
    bool interact = false;

    // Hold this base rather than follow the fight across the map. A bot
    // dragged past leashRange walks back to the banner instead of chasing -
    // pulling the guard off a base was the other half of the solo cap.
    bool leash = false;
    float leashRange = 0.0f;

    // Held ground, measured from the banner exactly as the leash is. A leashed
    // bot picks its fights among enemies inside engageRange and follows one no
    // further than pursueRange, both short of the leash, so a chase ends before
    // the leash has anything to walk it back from.
    float engageRange = 0.0f;
    float pursueRange = 0.0f;

    uint32 enemiesAtNode = 0;
};

class NodeCoordinator
{
public:
    // True for an in-progress battleground that has capturable bases.
    static bool IsNodeBattleground(Battleground const* battleground);

    // This bot's orders. False, with role None, outside such a match.
    static bool GetOrders(Player const* bot, NodeBotOrders& orders);

    // True when the bot is leashed to a base and the target stands past its
    // pursueRange. Every approach toward a target asks this, so the spell layer
    // cannot walk a holder off its base either.
    static bool IsBeyondHeldGround(Player const* bot, WorldObject const* target);

    // One line per base plus a team summary, for .playerbot pvp nodes.
    static std::vector<std::string> DescribeTeams(Player const* observer);
};
}

#endif
