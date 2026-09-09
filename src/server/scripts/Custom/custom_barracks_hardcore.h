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

#ifndef CUSTOM_BARRACKS_HARDCORE_H
#define CUSTOM_BARRACKS_HARDCORE_H

#include "Define.h"

#include <string>

class Player;

namespace BarracksHardcore
{
    // Copper as a player reads it: "12g 40s", with zero parts left out.
    // Exported so a price quoted on a gossip row and a loss reported in chat
    // are written the same way.
    std::string FormatMoney(uint32 copper);

    // Whether this character is one of the fleet, by account id.
    //
    // Exported because the bot account set is configured in exactly one
    // place and a second copy of the answer would drift from it. The bounty
    // ruleset needs it to decide how much of a corpse's gold burns.
    bool IsPlayerbot(Player const* player);

    // Whether this PERSON has armed War Mode.
    //
    // False for every playerbot by construction - a bot has no setting to read -
    // so this answers "is this someone who came here looking for a fight", which
    // is the question the playerbot manager asks before it sends anyone after
    // them. Exported rather than copied because the opt-in set is loaded and
    // maintained in exactly one place.
    bool IsWarModeOptedIn(Player const* player);

    // True when a fight between people can actually happen in this zone.
    //
    // Exported so the playerbot manager asks the same question the FFA ruleset
    // answers, rather than keeping a second copy of the zone table that would
    // drift out of step with this one. A bot that travels to pick a fight in a
    // zone where no fight is possible is not aggressive, it is lost.
    bool IsOpenWorldPvpZone(uint32 zoneId);

    // What level this zone tops out at.
    //
    // The realm's own table, written because AreaTableEntry::ExplorationLevel is
    // ZERO for every zone in this rebuilt DBC - reading that armed nothing,
    // anywhere. Exported so the bounty guards are sized by the same answer the
    // FFA rules use, rather than a second copy that could drift from it.
    // Unknown zones (Outland, Northrend, dungeons) return 60.
    uint8 ZoneTopLevel(uint32 zoneId);

    // Whether the world can produce this item at all: sold by a vendor,
    // dropped by something, or handed to a new character. Fails open when the
    // set has not been built, so nothing is destroyed on a cold cache.
    bool IsObtainableInWorld(uint32 itemId);

    // Fill every empty equipment slot with the white field kit, and replace worn
    // kit that has fallen too far behind the wearer. Gear the character actually
    // earned is never touched.
    //
    // Exported for the zone-band rebirth. That reset strips the bot to re-level
    // it, and the loose-kit sweep then destroys the pieces it just unequipped -
    // correctly, since a kit piece in a bag is normally debris - so a bot that
    // died and was reborn in the same breath came back with whatever the starter
    // outfit gave it and nothing else. Issuing the kit at the END of the reset,
    // at the bot's NEW level, is the order that leaves it dressed.
    void IssueWhiteFieldKit(Player* player);
}

#endif
