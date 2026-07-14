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
#include "ObjectGuid.h"
#include "Position.h"

#include <string>

class Battleground;
class Player;
class WorldSession;

namespace playerbot
{
// Obsidian Colosseum "Dark <name>" clone mirror.
//
// When a real human joins an in-progress Obsidian Colosseum (BATTLEGROUND_OBC)
// instance, this manager creates a transient in-memory copy of that human's
// character - gear, active talents, glyphs, level, skills, and spells - on the
// OPPOSITE team of the same instance under normal playerbot PvP control, and
// displays it as "Dark <name>". It has no account or character-database row.
// When the human leaves, logs out, or the match ends, the clone is destroyed.
// A killing blow between counterparts grants the killer Bloodlust (2825) for
// 60 seconds.
//
// Everything lives in the scripts/playerbot lib and is driven by the world
// update tick plus the OnPVPKill / OnLogout player-script hooks, so the
// game-lib BattlegroundOBC class needs no knowledge of it.
class PlayerbotObcCloneManager
{
public:
    static void LoadConfig();

    // Compatibility hook; transient clones cannot survive a process restart.
    static void OnStartupSweep();

    // Destroy transient clones while map and battleground services are still alive.
    static void OnShutdown();

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

    // Create and manage explicitly requested transient copies for private
    // custom games. The source character is never moved or modified.
    static Player* CreateCustomGameClone(Player* source, Battleground* bg, uint32 team, std::string const& displayPrefix);
    static bool QueueCustomGameClone(ObjectGuid sourceGuid, WorldSession* callbackSession, Battleground* bg,
        uint32 team, std::string const& displayPrefix);
    static void DestroyCustomGameClones(uint32 battlegroundInstanceId);

    // Create inert, transient roster previews in a private custom-game lobby.
    // The request key includes the source kind so a playerbot and Dark copy of the
    // same character can be represented independently.
    static bool QueueCustomGameLobbyClone(ObjectGuid sourceGuid, WorldSession* callbackSession, uint32 mapId,
        uint32 lobbyInstanceId, uint32 team, bool isPlayerbot, Position const& position, std::string const& displayPrefix);
    static void SetCustomGameLobbyClonePosition(uint32 lobbyInstanceId, ObjectGuid sourceGuid, uint32 team,
        bool isPlayerbot, Position const& position);
    static void DestroyCustomGameLobbyClone(uint32 lobbyInstanceId, ObjectGuid sourceGuid, uint32 team, bool isPlayerbot);
    static void DestroyCustomGameLobbyClones(uint32 lobbyInstanceId);
};
}

#endif
