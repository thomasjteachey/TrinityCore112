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

#ifndef TRINITYCORE_VIOLET_HOLD_BARS_H
#define TRINITYCORE_VIOLET_HOLD_BARS_H

class Player;

// A character leaves a Violet Hold run with the action bars it walked in with.
//
// A run changes the bars in ways nobody chose to keep: the broker's class
// spells, legendaries and healthstone go on them and vanish again when the
// boons are stripped, a talent bought on a borrowed level comes off with the
// level, and whatever the player moved around to fight the waves stays moved.
// So the bars are written down at the door and put back, button for button, on
// the way out - what the Summoner's Rift does for its match (RiftProgression).
//
// The row in `custom_violet_hold_bars` is the record that survives a restart. A
// logout, a disconnect, a crash or a restart in the middle of a run is just a
// slower way out: the next login that does not put the character back into its
// run gives the bars back instead. Giving them back and deleting the row are one
// transaction, so a crash right after a character leaves cannot lose them
// either (the .cpp says why the order holds). A realm without the table never
// queries it.
//
// The row holds the bars of the spec the character walked in on. One that
// swapped specs inside gets that spec's bars written back to the database,
// where the swap left them; the spec it is on keeps what it has.
namespace VioletHoldBars
{
    // World::SetInitialWorldSettings. Probed once: a query against a table that
    // does not exist is fatal, and this branch is shared.
    void ProbeStorage();

    // BattlegroundVHR::AddPlayer, the first time a character steps in. Bars
    // already kept aside for it were never given back - they are the real ones
    // - and are kept.
    void Stash(Player* player);

    // BattlegroundVHR::RemovePlayer, on every way out but a logout, BEFORE the
    // boons are stripped: the client has to be handed the bars while it still
    // knows every spell on them, and be told which spells are gone after.
    void Restore(Player* player);

    // Player::LoadFromDB, right after the bars are read: a character that is
    // not going back into its run gets the bars it walked in with, in time for
    // them to go out with the initial packets. One that is going back has its
    // row taken up again, in case a restart forgot it.
    void RestoreAtLogin(Player* player);
}

#endif // TRINITYCORE_VIOLET_HOLD_BARS_H
