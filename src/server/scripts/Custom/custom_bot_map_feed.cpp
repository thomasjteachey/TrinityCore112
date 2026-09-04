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

// ---------------------------------------------------------------------------
// Playerbots on a War Mode player's WORLD MAP.
//
// The minimap needed none of this: UNIT_DYNFLAG_TRACK_UNIT (Unit.cpp) marks bots
// as tracked in a War Mode player's copy of the update and the client draws the
// dots itself, exactly as it does for Track Humanoids.
//
// The world map cannot work that way. Blizzard's own party blips are Lua - see
// Blizzard_BattlefieldMinimap.lua:379-385, which reads GetPlayerMapPosition(unit)
// and anchors a texture at (x * width, -y * height) - but that API answers only
// for "player", "party1-4" and "raid1-40". There is no way to make it answer for
// an arbitrary bot, and a raid caps at 40 in any case.
//
// So the DRAWING is copied from Blizzard verbatim and only the POSITIONS come
// from here: the server converts each bot to the same 0..1 space
// GetPlayerMapPosition returns, and the addon anchors textures with the same
// arithmetic. That is "the same way party members work" in every respect the
// client can actually offer.
//
// Sent to anyone who is FFA-armed, covering their own zone only. A zone's worth
// of bots is a few hundred bytes every two seconds, so no map-open handshake is
// worth the core edit it would need - ChatHandler.cpp cannot reach a scripts
// header, and moving this into the game library to fix that would mean a third
// copy of the bot-account predicate.
// ---------------------------------------------------------------------------

#include "custom_barracks_hardcore.h"

#include "Chat.h"
#include "Configuration/Config.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <string>
#include <unordered_map>

namespace
{
    bool s_enabled = false;
    uint32 s_intervalMs = 2000;
    uint32 s_maxPerMessage = 12;

    // No watcher set and no handshake: see the note at the top of the file.
    // The server pushes to everyone who is armed, and the addon draws when the
    // map is open.

    void LoadBotMapConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.WarMode.BotMap", true);
        s_intervalMs = uint32(std::max(500, sConfigMgr->GetIntDefault("Centurion.WarMode.BotMapIntervalMs", 2000)));
        s_maxPerMessage = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.WarMode.BotMapPerMessage", 12), 1, 40));
    }

    void SendChunk(Player* viewer, std::string const& payload)
    {
        // The same transport the battleground rules already use
        // (Battleground::SendCustomGameRulesTo), so the client needs no new
        // prefix registered - CENTURION_WSGHelper is already listening on
        // CCGAME and simply ignores tags it does not know.
        std::string const message = "CCGAME\tBMAP:" + payload;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty,
            ObjectGuid::Empty, message, 0);
        viewer->SendDirectMessage(&data);
    }

    // One viewer's worth of bots, as 0..1 map coordinates in THEIR current zone.
    void SendBotsTo(Player* viewer)
    {
        Map* map = viewer->FindMap();
        if (!map)
            return;

        uint32 const zoneId = viewer->GetZoneId();

        // Prove the zone is mappable ONCE, with the viewer's own position, before
        // converting anybody. TryMap2ZoneCoordinates rather than the void version:
        // that one returns having changed nothing for an unmapped zone, which
        // would silently plot every bot at four thousand percent.
        {
            float probeX = viewer->GetPositionX();
            float probeY = viewer->GetPositionY();
            if (!TryMap2ZoneCoordinates(probeX, probeY, zoneId))
                return;
        }

        std::string payload;
        uint32 inMessage = 0;
        uint32 sent = 0;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* bot = itr->GetSource();
            if (!bot || bot == viewer || !bot->IsInWorld() || !bot->IsAlive())
                continue;

            if (!BarracksHardcore::IsPlayerbot(bot))
                continue;

            float x = bot->GetPositionX();
            float y = bot->GetPositionY();
            if (!TryMap2ZoneCoordinates(x, y, zoneId))
                continue;

            // Map2ZoneCoordinates yields 0..100 percentages within the zone's
            // rectangle; anything outside it is in a different zone and simply
            // is not on the map being looked at.
            if (x < 0.0f || x > 100.0f || y < 0.0f || y > 100.0f)
                continue;

            // Two decimals is about a pixel on a thousand-pixel map, and keeps
            // the whole record inside a dozen characters.
            payload += Trinity::StringFormat("{}:{:.2f}:{:.2f};", bot->GetName(), x / 100.0f, y / 100.0f);
            ++inMessage;
            ++sent;

            if (inMessage >= s_maxPerMessage)
            {
                SendChunk(viewer, "C" + payload);   // C = continued
                payload.clear();
                inMessage = 0;
            }
        }

        // The LAST message is tagged E, so the addon knows the set is complete
        // and can swap it in rather than drawing a half-built world.
        SendChunk(viewer, "E" + payload);

        TC_LOG_DEBUG("playerbots.hardcore", "BotMap: sent {} bot(s) in zone {} to {}.",
            sent, zoneId, viewer->GetName());
    }

    // Whether this player should be fed at all: War Mode armed, not a bot, and
    // looking at a map. IsFFAPvP is the same signal the minimap tracking uses -
    // see the note in Unit::BuildValuesUpdate for why it is asked this way
    // rather than through the badge aura.
    // How long the feed keeps running after the byte last read as armed.
    //
    // Crossing a SUB-AREA boundary strips the FFA byte for at least a tick:
    // Player::UpdateArea recomputes pvpInfo.IsInFFAPvPArea from the area alone
    // and re-runs UpdatePvPState, and only the next per-player tick of
    // ApplyFfaState puts it back. There is no area-change script hook to do it
    // sooner. Asking the raw byte therefore drops the feed for a second or two
    // every time somebody walks between two sub-areas of the same zone, which
    // reads as every bot icon blinking off the map and back.
    //
    // A short grace covers the gap without changing what the gate MEANS: walk
    // into a capital and disarm properly and the feed still stops, just a couple
    // of seconds later than it used to.
    constexpr uint32 kArmedGraceMs = 5000;
    std::unordered_map<uint64, uint32> s_lastArmedAtMs;

    bool ShouldFeed(Player* player)
    {
        if (!s_enabled || !player || !player->IsInWorld() || !player->GetSession())
            return false;

        if (BarracksHardcore::IsPlayerbot(player))
            return false;

        uint64 const raw = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();

        if (player->IsFFAPvP())
        {
            s_lastArmedAtMs[raw] = nowMs;
            return true;
        }

        auto const itr = s_lastArmedAtMs.find(raw);
        if (itr == s_lastArmedAtMs.end())
            return false;

        if (getMSTimeDiff(itr->second, nowMs) <= kArmedGraceMs)
            return true;

        s_lastArmedAtMs.erase(itr);
        return false;
    }
}

class custom_bot_map_feed_player : public PlayerScript
{
public:
    custom_bot_map_feed_player() : PlayerScript("custom_bot_map_feed_player") {}

    // Nothing to forget: there is no per-player state to keep.
    // The armed-grace stamp is per character and would otherwise outlive
    // every session on a realm that never restarts.
    void OnLogout(Player* player) override
    {
        if (player)
            s_lastArmedAtMs.erase(player->GetGUID().GetRawValue());
    }
};

class custom_bot_map_feed_world : public WorldScript
{
public:
    custom_bot_map_feed_world() : WorldScript("custom_bot_map_feed_world") {}

    void OnConfigLoad(bool /*reload*/) override { LoadBotMapConfig(); }

    void OnUpdate(uint32 diff) override
    {
        if (!s_enabled)
            return;

        _elapsed += diff;
        if (_elapsed < s_intervalMs)
            return;
        _elapsed = 0;

        for (auto const& pair : ObjectAccessor::GetPlayers())
            if (Player* viewer = pair.second)
                if (ShouldFeed(viewer))
                    SendBotsTo(viewer);
    }

private:
    uint32 _elapsed = 0;
};

void AddSC_custom_bot_map_feed()
{
    LoadBotMapConfig();
    new custom_bot_map_feed_player();
    new custom_bot_map_feed_world();
}
