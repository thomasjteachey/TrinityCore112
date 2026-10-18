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

#ifndef TRINITYCORE_ADDON_REQUEST_ROUTES_H
#define TRINITYCORE_ADDON_REQUEST_ROUTES_H

#include "Define.h"

#include <string>
#include <string_view>

class Player;

// Addon requests whose answer lives in the script library.
//
// A Centurion addon asks the server for something by whispering itself on the
// CCGAMEREQ prefix: "CCGAMEREQ\t<VERB>:<argument>". The verbs the core owns -
// Mok'gora, the tournament toggles, the battleground feeds - are answered right
// in WorldSession::HandleMessagechatOpcode. A verb that needs the playerbot
// fleet or the hardcore rules cannot be, because game/ never links against
// scripts/. So the script registers its verb here when it loads, and the chat
// handler hands the request over.
//
// Handlers run on the world thread: CMSG_MESSAGECHAT is PROCESS_THREADUNSAFE,
// so a handler may read any player on any map, exactly as a chat command can.
namespace AddonRequestRoutes
{
    // The argument is everything after "<VERB>:".
    using Handler = void (*)(Player* sender, std::string_view argument);

    // Called from a script's AddSC_, while the server is still loading and no
    // session exists. Registration is not locked for that reason.
    TC_GAME_API void Register(std::string_view verb, Handler handler);

    // True when the message was a CCGAMEREQ line for a registered verb. It has
    // been answered and must not travel any further as chat.
    bool Dispatch(Player* sender, uint32 lang, std::string const& msg);
}

#endif
