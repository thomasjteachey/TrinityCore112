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

#ifndef TRINITY_PLAYERBOT_BG_FILL_DRIVER_H
#define TRINITY_PLAYERBOT_BG_FILL_DRIVER_H

#include "Define.h"

namespace playerbot
{
// Pads public battlegrounds with transient clones of the managed playerbots.
//
// The bots themselves stay out in the world. What enters a Warsong Gulch or a
// Battle for Gilneas is an in-memory copy of one of them - same gear, talents
// and level, made by PlayerbotObcCloneManager exactly as the Violet Hold waves
// and the custom-game rosters are - and the copy is thrown away when the match
// is over. Real players come first at every turn: a person queuing alone gets a
// match after BattlegroundMgr::BotFillPolicy's wait (the queue's
// TryStartBotFilledMatch), a person invited into a full match displaces a clone
// (the queue's ordinary bot-displacement path, extended to transient clones by
// Battleground::IsBotFillMatch), a person leaving has the seat refilled here,
// and the last person leaving ends the match through the stock no-humans rule.
//
// Each half-second the driver visits every live public battleground of an
// enabled type and, once at least one real player has actually entered, tops
// each team up to the configured size with clones drawn from bots whose level
// fits the bracket: online bots first, then offline characters from the bot
// accounts (loaded in the background), then as a last resort "Dark" mirrors of
// the people in the match. Teams over the target - a person was admitted, or
// the cap was lowered - shed clones, dead ones first. The resource governor
// gates every addition and can shed clones under sustained pressure.
class PlayerbotBgFillDriver
{
public:
    // Reads Playerbot.BgFill.* and installs the queue-side policy into
    // BattlegroundMgr. Safe on .reload config.
    static void LoadConfig();

    static void OnWorldUpdate(uint32 diffMs);
};
}

#endif
