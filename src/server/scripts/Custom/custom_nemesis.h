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

#ifndef CUSTOM_NEMESIS_H
#define CUSTOM_NEMESIS_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string_view>
#include <vector>

class GameObject;
class Player;

// The nemesis system, after Shadow of Mordor.
//
// A world bot that kills a person RISES: it becomes a nemesis, takes an
// epithet ("the Baku-Slayer", "Priestbane", "the Terror of Duskwood") and a
// rank, rolls strengths and weaknesses, and remembers who it killed. Every
// person it kills after that is another rank, up to five: a little more damage
// dealt, a little less taken, a little bigger. It taunts the people it meets -
// by name if it has killed them before - and the top nemesis in each zone is
// its Warchief, guarded by bodyguards.
//
// Its traits are hidden until somebody learns them: killing other bots in its
// zone wrings out a secret, and dying to it teaches one of its strengths the
// hard way.
//
// Kill one and it pays coin and seals, everybody it ever killed hears they were
// avenged, and it is gone - usually. Every bot gets up again; what a slain
// nemesis loses is its standing. But now and then one will not let it go: the
// REVENANT. Hours later, at a moment nobody chose, while its slayer is out in
// the open world, it claws its way back - whispering to them as it comes,
// shouting when it arrives, wearing a new scar and a name to match - and hunts
// them alone for a while. Kill it again and it is laid to rest for good, for a
// bigger purse; die to it and it climbs on your corpse; outlast it and it melts
// back into the world, promising to finish this.
//
// And nemeses in the same zone challenge each other for standing: the board
// posts the rumour, the fight is settled off-screen, and anybody who kills one
// of the two first decides it.
//
// GROUPS. A nemesis climbs once a fight, however many of a party it kills in
// it, and the same person cannot feed it a rank again within the hour. Kill
// one as a party and everybody in it close by shares the purse and the
// vengeance, and what one of you learns about its traits, the others standing
// there learn too. A revenant comes for the one who struck the killing blow,
// but the slayer's party may fight it whatever their War Mode (and it them),
// it hits harder for every friend at the slayer's side, and it will not walk
// into a raid.
//
// Everything is a Centurion.Nemesis.* switch, applied on .reload config.
namespace Nemesis
{
    bool Enabled();

    // A revenant out for its slayer's blood. Takes no notice and answers no
    // challenge until that is settled. Any thread.
    bool IsHunting(ObjectGuid bot);

    // --- the board (world thread) -------------------------------------------

    // BBNX after a zone's notices: which of them are nemeses.
    void SendBoardRows(Player* viewer, uint32 zoneId, std::vector<Player*> const& bots);

    // BBNP after a poster: the nemesis line, and what the viewer knows of it.
    void SendPosterLine(Player* viewer, Player* bot);

    // BBNO when a board opens: whether the realm has nemeses at all.
    void SendBoardState(Player* viewer);

    // NEMESES:<zoneId> (the hierarchy and the rumours). True when ours.
    bool HandleRequest(Player* player, std::string_view verb, std::string_view argument);

    // The no-addon gossip.
    void AddGossipOptions(Player* player);
    bool OnGossipSelect(Player* player, GameObject* board, uint32 sender, uint32 action);
}

#endif // CUSTOM_NEMESIS_H
