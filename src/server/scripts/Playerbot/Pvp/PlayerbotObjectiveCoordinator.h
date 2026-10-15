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

#ifndef TRINITY_PLAYERBOT_OBJECTIVE_COORDINATOR_H
#define TRINITY_PLAYERBOT_OBJECTIVE_COORDINATOR_H

#include "Battleground.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"

#include <string>
#include <vector>

class Map;
class Player;
class Unit;

namespace playerbot
{
// Team play for what a battleground asks of its players beyond bases and the
// two-flag game: gates only siege damage brings down (Strand of the Ancients,
// Isle of Conquest), a commander whose death decides the match (Alterac
// Valley, Isle of Conquest), and the one neutral flag of a base-capture
// battleground (Eye of the Storm). The battleground describes all of it
// through its objective virtuals; nothing here knows which map it is on.
//
// Like the base plan (NodeCoordinator) this is kept per team per match and
// rebuilt lazily from the bots' own decision ticks. It runs FIRST: the bots it
// gives a duty are left out of the base plan, and everybody else is shared
// over the bases exactly as before. A battleground with none of these
// objectives - Arathi Basin, Warsong Gulch - never gives anybody a duty.
//
// The duties, in the order bodies are handed out:
//   - AssaultBoss: half the team goes for an enemy commander the battleground
//     calls engageable. It is how these matches are won.
//   - DriveSiege / CarryCharge: half the team (all of it on a side with no
//     bases to play) works on the breachable enemy gate - one driver per free
//     siege vehicle first, the rest fetching charges.
//   - GuardGate: a gate of ours under siege pulls its defenders off whatever
//     else they were doing; a side with no bases to play guards all of them.
//   - FlagRunner / FlagHunt: one bot takes the neutral flag, two run down an
//     enemy carrying it. The rest stay on the bases, which is where that
//     flag gets scored.
enum class ObjectiveDuty : uint8
{
    None = 0,
    CarryCharge,
    DriveSiege,
    GuardGate,
    AssaultBoss,
    FlagRunner,
    FlagHunt
};

char const* GetObjectiveDutyName(ObjectiveDuty duty);

// The seat that drives a vehicle (VEHICLE_SEAT_FLAG_CAN_CONTROL), or -1. Not
// every vehicle keeps its driver in seat 0.
int8 GetVehicleControlSeat(Unit const* vehicle);

struct ObjectiveBotOrders
{
    ObjectiveDuty duty = ObjectiveDuty::None;

    // CarryCharge, DriveSiege, GuardGate: the gate and the ground in front of it.
    uint32 siegeId = 0;
    ObjectGuid gateGuid;
    Position gateLocation;

    // CarryCharge: the charge, and the objects handing one out.
    uint32 chargeItemId = 0;
    GuidVector chargeSources;

    // DriveSiege: the vehicle this bot was given.
    ObjectGuid vehicleGuid;

    // AssaultBoss.
    ObjectGuid bossGuid;
    bool bossHasApproach = false;
    Position bossApproach;

    // FlagRunner: the flag to take. FlagHunt: the enemy carrying it.
    ObjectGuid flagGuid;
    ObjectGuid flagCarrierGuid;
};

class ObjectiveCoordinator
{
public:
    // True for a battleground that declares a siege, a commander, or a
    // neutral flag in a base-capture match.
    static bool HasObjectives(Battleground const* battleground);

    // This bot's duty. False, with duty None, when it has none.
    static bool GetOrders(Player const* bot, ObjectiveBotOrders& orders);

    // Whether the duty plan has claimed this bot, so the base plan can leave
    // it out. Brings the duty plan up to date first, so the two plans built in
    // the same tick agree about who is where.
    static bool IsOnDuty(Battleground* battleground, Map* map, TeamId team, ObjectGuid referenceGuid,
        ObjectGuid botGuid);

    // One line per duty plus a team summary, for .playerbot pvp duties.
    static std::vector<std::string> DescribeTeams(Player const* observer);

    // A vehicle that could not be driven to its gate is left alone for a while,
    // so the plan neither keeps its driver in it nor sends the next bot to it.
    static void GiveUpVehicle(ObjectGuid vehicleGuid);

    // Playerbot.BgObjectives.* - read on the world thread at config load, never
    // from a map thread.
    static void LoadConfig();
};
}

#endif
