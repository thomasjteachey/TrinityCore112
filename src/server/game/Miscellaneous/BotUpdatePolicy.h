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

#ifndef TRINITYCORE_BOT_UPDATE_POLICY_H
#define TRINITYCORE_BOT_UPDATE_POLICY_H

#include "Define.h"

class Player;

// Per-tick work spent on players nobody is playing. Managed playerbots and
// transient clones run on WorldSessions with no socket, and
// WorldSession::SendPacket drops every packet addressed to one of those before
// anything (scripts included) reads it. Both switches are off unless the realm
// sets them, and both are re-read by `.reload config`.
namespace BotUpdatePolicy
{
    // Loaded from World::LoadConfigSettings.
    void LoadConfig();

    // Centurion.Bots.SkipClientPackets: true for a receiver with no client, so
    // callers skip BUILDING what would only be thrown away - field updates,
    // create blocks, aura lists. Visibility bookkeeping (m_clientGUIDs, and
    // other players' view of this one) is not affected.
    bool SkipsClientPackets(Player const* receiver);

    // Centurion.Bots.GridActivationRange: the radius around a client-less player
    // in which the map keeps creatures and objects updating, on a continent.
    // 0 means "the ordinary player range": the switch is off, the player has a
    // client, the map is an instance, battleground, arena or lobby sub-map, or
    // the configured range is not smaller than the map's own.
    float GetReducedGridActivationRange(Player const* player);
}

#endif
