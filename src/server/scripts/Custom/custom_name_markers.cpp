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

// The World / Tournament / Bot marker over a player's name - the server half.
//
// The client-tweaks DLL draws the word above the 3D name; all it needs from
// here is to know which of the three a unit is. It reads that off the unit's
// aura list, so every player carries exactly one of:
//
//   92010 World Character
//   92011 Tournament Character
//   92012 Playerbot
//
// All three are clones of Ghostwalk 90218: a dummy aura, hidden from the buff
// bar (ATTR1 0x10000000), not cancellable, kept through death (ATTR3 0x100000)
// and through the arena's aura strip (ATTR4 0x200000). No packet, no name
// games; a client without the DLL sees nothing at all.
//
// Bot beats the character's mode - a bot is a bot whichever pool it plays in.
// "Bot" is a session with no socket, which covers the persistent bots, the
// battleground fill clones and the transient bounty hunters alike, plus
// anything on a playerbot account. The clones are only reachable through the
// maps - see SweepMs.
//
// Config (worldserver.conf, `.reload config`):
//   Centurion.NameMarkers.Enable = 1
// Turning it off strips the auras on the next sweep.

#include "Config.h"
#include "Map.h"
#include "MapManager.h"
#include "Miscellaneous/TournamentMode.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "custom_barracks_hardcore.h"

namespace
{
    constexpr uint32 MarkerWorld      = 92010;
    constexpr uint32 MarkerTournament = 92011;
    constexpr uint32 MarkerBot        = 92012;
    constexpr uint32 Markers[] = { MarkerWorld, MarkerTournament, MarkerBot };

    // For real players the login hook does the work and this is a safety net.
    // For the battleground fill clones it IS the mechanism: they are built on
    // in-memory sessions that never reach the world's session list and never
    // log in, so the only way to find them is to walk the maps they stand on.
    // Five seconds lands the aura well inside a battleground's preparation.
    constexpr uint32 SweepMs = 5 * IN_MILLISECONDS;

    bool Enabled()
    {
        return sConfigMgr->GetBoolDefault("Centurion.NameMarkers.Enable", false) && Tournament::IsEnabled();
    }

    uint32 MarkerFor(Player const* player)
    {
        WorldSession const* session = player->GetSession();
        if ((session && session->IsVirtualSession()) || BarracksHardcore::IsPlayerbot(player))
            return MarkerBot;
        return Tournament::IsTournamentCharacter(player) ? MarkerTournament : MarkerWorld;
    }

    void Ensure(Player* player, bool enabled)
    {
        if (!player || !player->IsInWorld())
            return;

        uint32 const wanted = enabled ? MarkerFor(player) : 0;
        for (uint32 marker : Markers)
            if (marker != wanted && player->HasAura(marker))
                player->RemoveAurasDueToSpell(marker);

        if (wanted && !player->HasAura(wanted))
            player->AddAura(wanted, player);
    }
}

class name_markers_playerscript : public PlayerScript
{
public:
    name_markers_playerscript() : PlayerScript("name_markers_playerscript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (Enabled())
            Ensure(player, true);
    }
};

// Script objects are singletons, so the timer belongs on the WorldScript.
class name_markers_worldscript : public WorldScript
{
public:
    name_markers_worldscript() : WorldScript("name_markers_worldscript") { }

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < SweepMs)
            return;
        _timer = 0;

        bool const enabled = Enabled();

        // Once off, one sweep strips what was handed out, then it goes quiet.
        if (!enabled && _cleared)
            return;

        // Every player on every map, whatever kind of session is behind it.
        // Runs after sMapMgr->Update has waited for the map threads.
        sMapMgr->DoForAllMaps([enabled](Map* map)
        {
            Map::PlayerList const& players = map->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                Ensure(itr->GetSource(), enabled);
        });

        _cleared = !enabled;
    }

private:
    uint32 _timer = 0;
    bool _cleared = false;
};

void AddSC_custom_name_markers()
{
    new name_markers_playerscript();
    new name_markers_worldscript();
}
