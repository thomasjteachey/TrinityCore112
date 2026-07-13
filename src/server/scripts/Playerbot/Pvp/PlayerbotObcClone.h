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

#ifndef TRINITY_PLAYERBOT_OBC_CLONE_H
#define TRINITY_PLAYERBOT_OBC_CLONE_H

#include "Define.h"

class Player;

namespace playerbot
{
// Obsidian Colosseum "Dark <name>" clone mirror.
//
// When a real human joins an in-progress Obsidian Colosseum (BATTLEGROUND_OBC)
// instance, this manager provisions a throwaway copy of that human's character
// - identical gear, talents, level, spells - onto a dedicated bot account, logs
// it in on the OPPOSITE team of the same instance under normal playerbot PvP
// control, and displays it as "Dark <name>". When the human leaves the
// battleground (or logs out, or the match ends) the clone is removed and its
// throwaway character is hard-deleted. A killing blow between a human and their
// own clone grants the killer Bloodlust (2825) for 30 seconds.
//
// Everything lives in the scripts/playerbot lib and is driven by the world
// update tick plus the OnPVPKill / OnLogout player-script hooks, so the
// game-lib BattlegroundOBC class needs no knowledge of it.
class PlayerbotObcCloneManager
{
public:
    static void LoadConfig();

    // Delete any clone characters left over from a previous run (e.g. a crash
    // mid-match). Safe because the dedicated clone account only ever holds
    // ephemeral clones.
    static void OnStartupSweep();

    // World-thread tick: provision clones for un-mirrored humans and tear down
    // clones whose human counterpart has left.
    static void OnWorldUpdate(uint32 diffMs);

    // Grant Bloodlust to the killer when a human and their own clone trade a
    // killing blow.
    static void OnPvpKill(Player* killer, Player* killed);

    // Tear down a human's clone immediately when the human logs out.
    static void OnPlayerLogout(Player const* player);

    // True while the given player is a live Obsidian Colosseum clone.
    static bool IsActiveClone(Player const* player);
};
}

#endif
