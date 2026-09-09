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

// War Mode does not travel downhill.
//
// Somebody who has opted in to being attackable may not carry that into a zone
// beneath them. The whole point of the opt-in is a fight; a level 50 in Westfall
// is not offering one, and the people who live there did not agree to it.
//
// Enforced on ARRIVAL rather than on departure, because there is nowhere else to
// stand. There is no script hook on the flight master, the hearthstone or a
// portal, so "refuse the journey" is not available without editing every travel
// path in the core - and a rule that only covered some of them would just teach
// people which one still worked. OnUpdateZone catches all of them at once, at
// the cost of the player crossing the line before being turned around.
//
// The bands are the realm's own, shared with the drifter draft and the zone-band
// addon rather than copied - see playerbot::GetZoneLevelBand. A zone with no
// band has no opinion: cities, instances and battlegrounds are all unbanded and
// are all places a War Mode player is entitled to be.

#include "ScriptMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Map.h"
#include "MapManager.h"
#include "Player.h"
#include "World.h"
#include "custom_barracks_hardcore.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"

#include <mutex>
#include <unordered_map>

namespace
{
    // Where each player last stood in a zone they were allowed to be in.
    //
    // Recorded at every zone change that is NOT refused, so the bounce has
    // somewhere to put them that they have actually occupied. In-memory only on
    // purpose: a receipt that survives a restart would be pointing at a position
    // hours old, and the home bind is a better answer than a stale one.
    struct SafeSpot
    {
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
    };

    // The floor under every decision this script makes.
    //
    // A gate that turns people around needs somewhere to turn them around TO,
    // and it has to be somewhere the gate itself will not object to on arrival -
    // otherwise the bounce lands in a refused zone, which bounces again. A
    // capital has no level band at all, so it is allowed to everybody at every
    // level by construction, and it is the one destination that cannot fail.
    constexpr uint32 kFallbackMapId = 1;
    constexpr float kFallbackX = 1629.36f;
    constexpr float kFallbackY = -4373.39f;
    constexpr float kFallbackZ = 31.2564f;
    constexpr float kFallbackO = 3.54839f;

    std::mutex g_safeLock;
    std::unordered_map<uint64, SafeSpot> g_lastSafeSpot;

    bool GateEnabled()
    {
        return sConfigMgr->GetBoolDefault("Centurion.WarMode.BlockLowerZones", true);
    }

    void RememberSafeSpot(Player const* player)
    {
        SafeSpot spot;
        spot.MapId = player->GetMapId();
        spot.ZoneId = player->GetZoneId();
        spot.X = player->GetPositionX();
        spot.Y = player->GetPositionY();
        spot.Z = player->GetPositionZ();
        spot.O = player->GetOrientation();

        std::lock_guard<std::mutex> guard(g_safeLock);
        g_lastSafeSpot[player->GetGUID().GetRawValue()] = spot;
    }

    // The zone is beneath this player if its band has a ceiling and the player
    // is over it. Above the top, not above the bottom: a level 35 in a 30-40
    // zone is still doing that zone's content.
    bool ZoneIsBeneath(Player const* player, uint32 zoneId)
    {
        uint8 bottom = 0;
        uint8 top = 0;
        if (!playerbot::GetZoneLevelBand(zoneId, bottom, top))
            return false;

        return uint32(player->GetLevel()) > uint32(top);
    }

    // Cities, instances and battlegrounds have no entry in the band table, so
    // ZoneIsBeneath answers false for them and they are open to everybody. That
    // is deliberate and it is what makes the fallback below safe: no capital is
    // ever refused, whatever level you are.
    bool DestinationIsAllowed(Player const* player, uint32 mapId, float x, float y, float z)
    {
        uint32 const zoneId = sMapMgr->GetZoneId(player->GetPhaseMask(), mapId, x, y, z);
        if (!zoneId)
            return false;

        return !ZoneIsBeneath(player, zoneId);
    }
}

class warmode_zone_gate : public PlayerScript
{
public:
    warmode_zone_gate() : PlayerScript("warmode_zone_gate") { }

    void OnUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (!GateEnabled() || !player || !player->IsInWorld())
            return;

        // A ghost is running to a corpse that is already lying in the zone.
        // Turning them around would leave the corpse unreachable, so death
        // suspends the rule rather than trapping them outside it.
        if (!player->IsAlive())
            return;

        if (player->IsGameMaster())
            return;

        // Instances, battlegrounds and arenas are not the open world, and the
        // band table has no entry for them anyway.
        Map const* map = player->GetMap();
        if (map && map->Instanceable())
        {
            RememberSafeSpot(player);
            return;
        }

        // IsWarModeOptedIn already answers false for a bot and for anyone with
        // the system switched off, so this is the whole gate.
        if (!BarracksHardcore::IsWarModeOptedIn(player) || !ZoneIsBeneath(player, newZone))
        {
            RememberSafeSpot(player);
            return;
        }

        SafeSpot spot;
        bool haveSpot = false;
        {
            std::lock_guard<std::mutex> guard(g_safeLock);
            auto const itr = g_lastSafeSpot.find(player->GetGUID().GetRawValue());
            if (itr != g_lastSafeSpot.end() && itr->second.MapId == player->GetMapId())
            {
                spot = itr->second;
                haveSpot = true;
            }
        }

        ChatHandler handler(player->GetSession());
        handler.PSendSysMessage("You cannot travel here with War Mode on: this zone is below your level.");

        // EVERY destination is checked before it is used, including the ones
        // that look obviously safe.
        //
        // This gate fires on arrival, and logging in IS an arrival - so a
        // destination that is itself refused does not merely fail, it bounces
        // again on landing, forever. Somebody whose hearthstone was set in
        // Goldshire could not log in at all: no remembered position existed yet,
        // so the old code fell back to the home bind, and the home bind was the
        // very zone it was throwing them out of. The rule turned a hearthstone
        // into a locked account.
        //
        // A remembered spot is allowed at the moment it is recorded, but levels
        // go up and bands do not move, so it can be refused later. Re-ask.
        if (haveSpot && !ZoneIsBeneath(player, spot.ZoneId))
        {
            player->TeleportTo(spot.MapId, spot.X, spot.Y, spot.Z, spot.O);
            return;
        }

        // The home inn is where they chose to be, so it is tried before anything
        // is imposed on them - but only if they are allowed to stand in it.
        if (DestinationIsAllowed(player, player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ))
        {
            handler.PSendSysMessage("Returning you to your home inn.");
            player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ, player->GetOrientation());
            return;
        }

        // Nowhere they have been is open to them. A capital always is.
        handler.PSendSysMessage("Your hearth is below your level too. Sending you to Orgrimmar.");
        player->TeleportTo(kFallbackMapId, kFallbackX, kFallbackY, kFallbackZ, kFallbackO);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        std::lock_guard<std::mutex> guard(g_safeLock);
        g_lastSafeSpot.erase(player->GetGUID().GetRawValue());
    }
};

void AddSC_custom_warmode_zone_gate()
{
    new warmode_zone_gate();
}
