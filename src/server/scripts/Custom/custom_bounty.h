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

#ifndef CUSTOM_BOUNTY_H
#define CUSTOM_BOUNTY_H

#include "Define.h"
#include "ObjectGuid.h"

class Player;

// ---------------------------------------------------------------------------
// Bounty: killing people makes you worth killing.
//
// Every kill you take part in adds a stack of a fifteen minute aura, up to
// fifty. The stacks are a standing offer to the world: playerbots hunt a
// bountied player harder the higher it climbs - from further away, more of them
// per zone, and past a threshold without regard for the aggro budget at all -
// and when you finally die a share of your gold is left in a chest for whoever
// wants it.
//
// It exists to put a natural ceiling on a killing spree. A run that would
// otherwise compound - the strongest player farming the weakest with nothing
// pushing back - instead builds its own opposition, and pays out to the people
// who eventually stop it.
//
// Nothing here arms in the Battle Ring, an FFA area, a battleground or an
// arena. Those are places built for killing each other, and a bounty for doing
// the thing the zone is for would be nonsense.
// ---------------------------------------------------------------------------
namespace Bounty
{
    bool Enabled();

    // The aura's spell id, so callers do not hardcode it.
    uint32 SpellId();

    // Stacks on this player right now, from the registry rather than the aura.
    //
    // The registry is what makes this callable from the world thread: reading a
    // Unit's aura map while the map thread may be writing it is a data race,
    // and the playerbot manager needs this figure once a second from exactly
    // there. The two are written together and the aura is the copy the player
    // sees; when they can disagree at all, this one decides behaviour.
    uint32 GetStacks(ObjectGuid guid);
    uint32 GetStacks(Player const* player);

    // 0.0 at no bounty, 1.0 at the cap. Every escalation below is a straight
    // line along this, so one number tunes how fast the world turns on you.
    float Fraction(uint32 stacks);

    // How much further a bot will come for a bountied player, in yards, added
    // to its ordinary hunt radius.
    float HuntRadiusBonusYards(uint32 stacks);

    // How many more drifters a zone will hold while somebody is worth hunting
    // in it.
    uint32 DrifterZoneBonus(uint32 stacks);

    // Extra bots allowed onto this person over their party's ordinary budget.
    int32 AggroSlotBonus(uint32 stacks);

    // Past this the budget stops applying at all: anyone who wants a piece of
    // them may take one.
    bool IgnoresAggroBudget(uint32 stacks);

    // Past this the fleet stops waiting to get bored and simply keeps sending
    // somebody, on the bounty's clock rather than each bot's own.
    bool IsHuntedRelentlessly(uint32 stacks);

    // How long between those arrivals. Shortens as the bounty climbs, so the
    // stream thickens rather than merely continuing.
    uint32 RelentlessIntervalSeconds(uint32 stacks);

    // Past this the VETERAN pool is preferred - the fleet's high-band bots,
    // rather than whoever happens to be nearest.
    bool DrawsFromVeterans(uint32 stacks);

    // And past this the PvP-only bots are recruited: the fleet held back for
    // battlegrounds, which nothing else pulls into the open world.
    bool DrawsFromPvpBots(uint32 stacks);

    // Coin owed by a death, in copper, taken now and handed to the caller to
    // put in a chest. Zero unless this player died with a bounty and nothing
    // has collected on it yet.
    //
    // Deliberately NOT deducted at the moment of death: a bounty that charged
    // the player before anything existed to receive the gold would burn it on
    // any path that never summons a chest. The debt is recorded at death and
    // settled here, so the failure direction is "nobody lost anything".
    uint32 TakePendingChestGold(Player* victim);
}

#endif // CUSTOM_BOUNTY_H
