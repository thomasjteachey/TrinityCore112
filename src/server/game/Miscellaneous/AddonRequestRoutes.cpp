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

#include "AddonRequestRoutes.h"
#include "SharedDefines.h"
#include "Util.h"

#include <utility>
#include <vector>

namespace
{
    // A handful of verbs at most, so a vector walked in order beats a map.
    std::vector<std::pair<std::string, AddonRequestRoutes::Handler>>& Routes()
    {
        static std::vector<std::pair<std::string, AddonRequestRoutes::Handler>> routes;
        return routes;
    }
}

void AddonRequestRoutes::Register(std::string_view verb, Handler handler)
{
    Routes().emplace_back(std::string(verb), handler);
}

bool AddonRequestRoutes::Dispatch(Player* sender, uint32 lang, std::string const& msg)
{
    static constexpr std::string_view Prefix = "CCGAMEREQ\t";
    if (!sender || lang != LANG_ADDON || !StringStartsWith(msg, Prefix))
        return false;

    std::string_view const body = std::string_view(msg).substr(Prefix.size());
    for (auto const& [verb, handler] : Routes())
    {
        // The verb and its colon exactly, so BOUNTY never answers BOUNTYX.
        if (body.size() <= verb.size() || body[verb.size()] != ':' || !StringStartsWith(body, verb))
            continue;

        handler(sender, body.substr(verb.size() + 1));
        return true;
    }

    return false;
}
