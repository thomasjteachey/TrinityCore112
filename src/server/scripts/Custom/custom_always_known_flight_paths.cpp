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

// Every character knows the flight paths listed in
// Centurion.FlightPaths.AlwaysKnown, granted at every login, so new and
// existing characters alike have them. Centurion is a one-team realm whose
// TaxiNodes mounts let any flight master fly to any node, so both old
// factions' capitals are useful to everyone.
//
// The list is config: edit the key and run .reload config, and it applies from
// each character's next login. Empty turns it off. The grant is silent (no
// "New flight path discovered!"), and the next save writes the nodes to
// characters.taximask like any discovered path.

#include "Config.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Util.h"
#include <algorithm>
#include <string>
#include <vector>

namespace
{
    std::vector<uint32> s_alwaysKnownNodes;

    // Only nodes in sTaxiNodesMask can be kept: PlayerTaxi::LoadTaxiMask drops
    // every other bit at login.
    bool IsKeepableTaxiNode(uint32 node)
    {
        if (!node || (node - 1) / 32 >= TaxiMaskSize || !sTaxiNodesStore.LookupEntry(node))
            return false;
        return (sTaxiNodesMask[(node - 1) / 32] & (1u << ((node - 1) % 32))) != 0;
    }

    // "2 6 11 22 23 27" or "2,6,11,22,23,27"
    void LoadAlwaysKnownNodes()
    {
        std::string raw = sConfigMgr->GetStringDefault("Centurion.FlightPaths.AlwaysKnown", "");
        std::replace(raw.begin(), raw.end(), ',', ' ');

        std::vector<uint32> loaded;
        for (std::string_view token : Trinity::Tokenize(raw, ' ', false))
        {
            Optional<uint32> const node = Trinity::StringTo<uint32>(token);
            if (!node || !IsKeepableTaxiNode(*node))
            {
                TC_LOG_ERROR("server.loading",
                    "Centurion.FlightPaths.AlwaysKnown: ignoring '{}', not a flight path node id.", token);
                continue;
            }
            loaded.push_back(*node);
        }

        s_alwaysKnownNodes = std::move(loaded);
        TC_LOG_INFO("server.loading", "Centurion.FlightPaths.AlwaysKnown: {} flight path(s) known by every character.",
            s_alwaysKnownNodes.size());
    }
}

class custom_always_known_flight_paths_world : public WorldScript
{
public:
    custom_always_known_flight_paths_world() : WorldScript("custom_always_known_flight_paths_world") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadAlwaysKnownNodes();
    }
};

class custom_always_known_flight_paths_player : public PlayerScript
{
public:
    custom_always_known_flight_paths_player() : PlayerScript("custom_always_known_flight_paths_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        for (uint32 node : s_alwaysKnownNodes)
            player->m_taxi.SetTaximaskNode(node);
    }
};

void AddSC_custom_always_known_flight_paths()
{
    new custom_always_known_flight_paths_world();
    new custom_always_known_flight_paths_player();
}
