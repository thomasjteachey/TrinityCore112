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

#ifndef TRINITY_PLAYERBOT_VHR_WAVE_DRIVER_H
#define TRINITY_PLAYERBOT_VHR_WAVE_DRIVER_H

#include "Define.h"

namespace playerbot
{
// Fulfils BattlegroundVHR wave spawn requests.
//
// The battleground (game lib) decides what a wave contains - which humans the
// clones copy and where each one stands - but cannot summon anything, because
// clone creation lives in PlayerbotObcCloneManager over here in the scripts
// lib. This driver is the bridge: each world tick it visits the live Violet
// Hold instances, and for any with a pending request it creates one "Dark"
// clone per roster entry, moves it onto its cell position, and reports back so
// the battleground can start the preparation window.
//
// Clone teardown is not handled here: the clones are ordinary custom-game
// clones, so the existing DestroyCustomGameClones path cleans an instance up
// when the battleground ends, exactly as it does for private custom games.
class PlayerbotVhrWaveDriver
{
public:
    static void OnWorldUpdate(uint32 diffMs);
};
}

#endif
