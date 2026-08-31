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

namespace BarracksHardcore
{
    // True when a fight between people can actually happen in this zone.
    //
    // Exported so the playerbot manager asks the same question the FFA ruleset
    // answers, rather than keeping a second copy of the zone table that would
    // drift out of step with this one. A bot that travels to pick a fight in a
    // zone where no fight is possible is not aggressive, it is lost.
    bool IsOpenWorldPvpZone(uint32 zoneId);

    // Whether the world can produce this item at all: sold by a vendor,
    // dropped by something, or handed to a new character. Fails open when the
    // set has not been built, so nothing is destroyed on a cold cache.
    bool IsObtainableInWorld(uint32 itemId);
}

#endif
