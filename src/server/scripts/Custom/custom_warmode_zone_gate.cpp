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
#include "DBCStores.h"
#include "Map.h"
#include "MapManager.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "custom_barracks_hardcore.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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

    // Players who crossed into a refused zone while ON A TAXI, to be settled up
    // the moment they land.
    //
    // The counter is read on every player tick and the set only under the lock,
    // so the overwhelmingly common case - nobody mid-flight anywhere on the
    // realm - costs one relaxed atomic load per player and no contention.
    std::unordered_set<uint64> g_pendingAfterFlight;
    std::atomic<uint32> g_pendingCount{ 0 };

    void MarkPendingAfterFlight(Player const* player)
    {
        std::lock_guard<std::mutex> guard(g_safeLock);
        if (g_pendingAfterFlight.insert(player->GetGUID().GetRawValue()).second)
            g_pendingCount.store(uint32(g_pendingAfterFlight.size()), std::memory_order_relaxed);
    }

    bool TakePendingAfterFlight(Player const* player)
    {
        std::lock_guard<std::mutex> guard(g_safeLock);
        if (!g_pendingAfterFlight.erase(player->GetGUID().GetRawValue()))
            return false;
        g_pendingCount.store(uint32(g_pendingAfterFlight.size()), std::memory_order_relaxed);
        return true;
    }

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
        // A zone where War Mode does not arm has nothing to protect.
        //
        // The whole rule exists because someone who opted in to being attackable
        // should not carry that into a place where the residents did not. Where
        // the flag never arms in the first place, nobody there is attackable by
        // them and they are not attackable either - so refusing the journey buys
        // nothing and costs a great deal, because the capitals and the roads to
        // them are exactly where people need to go.
        //
        // Asked of the FFA ruleset rather than answered here, so the two can
        // never disagree about which zones those are. It says no to every
        // capital and sanctuary (the flag is disarmed inside the walls) and to
        // the starter zones, which it declines to arm at all.
        if (!BarracksHardcore::IsOpenWorldPvpZone(zoneId))
            return false;

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

    // War Mode stops paying experience at the top of a zone's band.
    //
    // The gate turns a flagged player out of any zone they have outlevelled, so
    // the level that carries them past a zone's ceiling is the level that gets
    // them bounced out of the zone they are standing in - by their own
    // experience bar, possibly mid-fight. Stopping experience AT the ceiling lets
    // them stay and fight there at that level for as long as they like; the way
    // to level on is to go somewhere higher, which is what the gate wants from
    // them anyway.
    //
    // Separate from the stock toggle (PLAYER_FLAGS_NO_XP_GAIN) on purpose. Either
    // one stops experience; nothing here ever sets or clears the player's own.
    //
    // Cached from config (WorldScript below): OnUpdate asks this every tick.
    std::atomic<bool> s_zoneCapStopsXp{ true };
    std::atomic<uint32> s_zoneCapAuraSpell{ 0 };

    // Same conditions as ZoneIsBeneath, one level lower: where that would bounce
    // them at level+1, this stops the experience that would take them there.
    // The top band on the realm tops out at the level cap, where there is no
    // experience to stop, so a max-level player is never capped.
    bool ZoneCapStopsXp(Player const* player, uint8* outTop = nullptr)
    {
        if (!s_zoneCapStopsXp.load(std::memory_order_relaxed) || !player)
            return false;

        if (player->IsGameMaster() || player->IsMaxLevel())
            return false;

        if (!BarracksHardcore::IsWarModeOptedIn(player))
            return false;

        Map const* map = player->GetMap();
        if (!map || map->Instanceable())
            return false;

        uint32 const zoneId = player->GetZoneId();
        if (!BarracksHardcore::IsOpenWorldPvpZone(zoneId))
            return false;

        uint8 bottom = 0;
        uint8 top = 0;
        if (!playerbot::GetZoneLevelBand(zoneId, bottom, top))
            return false;

        if (outTop)
            *outTop = top;
        return uint32(player->GetLevel()) >= uint32(top);
    }

    // The aura IS the state: on while capped, off while not, and the message is
    // spoken on the change. No bookkeeping to go stale across a logout - the
    // aura persists with the character, so a login inside a capped zone says
    // nothing new, and one outside it lifts the aura with the "again" line.
    void SyncZoneCapAura(Player* player)
    {
        uint32 const spell = s_zoneCapAuraSpell.load(std::memory_order_relaxed);
        if (!spell || !player || !player->IsInWorld())
            return;

        uint8 top = 0;
        bool const capped = ZoneCapStopsXp(player, &top);
        bool const wearing = player->HasAura(spell);
        if (capped == wearing)
            return;

        ChatHandler handler(player->GetSession());
        if (capped)
        {
            player->AddAura(spell, player);

            char const* zoneName = "this zone";
            if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId()))
                if (char const* name = zone->AreaName[player->GetSession()->GetSessionDbcLocale()])
                    if (*name)
                        zoneName = name;

            handler.PSendSysMessage("You have reached level %u, the top of %s's level range. With War Mode on you no longer gain experience here - travel to a higher-level zone to turn it back on.",
                uint32(top), zoneName);
            return;
        }

        player->RemoveAurasDueToSpell(spell);
        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
            handler.PSendSysMessage("War Mode's experience stop has lifted, but your own experience toggle is still off.");
        else
            handler.PSendSysMessage("You are earning experience again.");
    }
}

class warmode_zone_gate_config : public WorldScript
{
public:
    warmode_zone_gate_config() : WorldScript("warmode_zone_gate_config") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        s_zoneCapStopsXp.store(sConfigMgr->GetBoolDefault("Centurion.WarMode.ZoneCapStopsXp", true),
            std::memory_order_relaxed);
        // 0 in the dist: an aura with no client Spell.dbc row is a blank icon.
        // 90718 on B+. The experience stop does not depend on it - only the badge.
        s_zoneCapAuraSpell.store(uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.WarMode.ZoneCapAuraSpell", 0))),
            std::memory_order_relaxed);
    }
};

class warmode_zone_gate : public PlayerScript
{
public:
    warmode_zone_gate() : PlayerScript("warmode_zone_gate") { }

    // Settle up with anyone who was refused a zone while airborne.
    //
    // Deliberately not done at the moment of refusal: see the taxi note in
    // OnUpdateZone. The flight is allowed to finish and the rule is applied to
    // where it actually put them, which is also the only position that is safe
    // to teleport from.
    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        // Every tick, like the notoriety checkpoints: level, zone and the War
        // Mode flag all move it, and a list of those is a list somebody forgets
        // to add to. Cheap - one opt-in lookup, then nothing, for anyone unflagged.
        SyncZoneCapAura(player);

        if (!g_pendingCount.load(std::memory_order_relaxed))
            return;

        if (!player || !player->IsInWorld() || player->IsInFlight())
            return;

        if (!TakePendingAfterFlight(player))
            return;

        if (!GateEnabled() || !player->IsAlive() || player->IsGameMaster())
            return;

        if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
            return;

        if (!BarracksHardcore::IsWarModeOptedIn(player) || !ZoneIsBeneath(player, player->GetZoneId()))
        {
            RememberSafeSpot(player);
            return;
        }

        SendHome(player);
    }

    // The stop. Checked at the award rather than trusted to the aura, so it holds
    // in the tick between a level-up and the badge appearing. Zeroing is enough:
    // Player::GiveXP returns on a withheld award without logging a gain of 0.
    void OnGiveXP(Player* player, uint32& amount, Unit* /*victim*/) override
    {
        if (amount && ZoneCapStopsXp(player))
            amount = 0;
    }

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

        // Never act on somebody the core is already moving. A second teleport
        // on top of one in flight is how the map code walks into RemoveFromGrid's
        // IsInGrid assert.
        if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
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
        bool const refused = BarracksHardcore::IsWarModeOptedIn(player) && ZoneIsBeneath(player, newZone);

        // A taxi is not a journey the passenger is steering.
        //
        // The route was bought at the other end and cannot be altered in the
        // air, so a zone crossed on the way is not a destination anybody chose -
        // and TeleportTo during a flight does not merely redirect it, it ENDS
        // it: the rider is dropped on foot wherever the bounce lands, mount
        // gone, at walking speed, halfway to somewhere they paid to reach.
        // Let the flight finish and apply the rule to where it actually put
        // them. A mid-air position is also useless as a bounce target, so no
        // safe spot is recorded while airborne either.
        if (player->IsInFlight())
        {
            if (refused)
                MarkPendingAfterFlight(player);
            return;
        }

        if (!refused)
        {
            RememberSafeSpot(player);
            return;
        }

        SendHome(player);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        TakePendingAfterFlight(player);

        std::lock_guard<std::mutex> guard(g_safeLock);
        g_lastSafeSpot.erase(player->GetGUID().GetRawValue());
    }

private:
    static void SendHome(Player* player)
    {
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
};

void AddSC_custom_warmode_zone_gate()
{
    new warmode_zone_gate_config();
    new warmode_zone_gate();
}
