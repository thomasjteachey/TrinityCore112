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

// .tick - the world tick as the resource governor sees it, for a GM.
//
//   .tick              world tick over 10 s and 60 s, where the time goes,
//                      the governor's verdict and the slowest maps
//   .tick watch [sec]  one summary line every few seconds (default 5) until
//                      `.tick watch` again, `.tick watch 0` or logout
//   .tick addon        the same numbers as machine lines, for the GM Panel
//                      addon's Server page
//
// The world tick is the SLOWEST map, not the sum: MapManager::Update waits
// for every scheduled map. So the map table is the answer to "why is it
// slow" - a 40v40 battleground shows up there as its own row.
//
// Everything here runs on the world thread: chat commands are
// PROCESS_THREADUNSAFE, console/SOAP commands run in World::Update, and so
// does the WorldScript that drives the watch. The watcher table needs no lock.

#include "Chat.h"
#include "ChatCommand.h"
#include "Common.h"
#include "GameTime.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Optional.h"
#include "Player.h"
#include "Playerbot/Pvp/PlayerbotResourceGovernor.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "TickStats.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_map>

using namespace Trinity::ChatCommands;

namespace
{
constexpr uint32 ShortWindowMs = 10 * IN_MILLISECONDS;
constexpr uint32 LongWindowMs = 60 * IN_MILLISECONDS;
constexpr size_t MapRowsShown = 8;
constexpr size_t AddonMapRows = 16;
constexpr uint32 DefaultWatchSeconds = 5;
constexpr uint32 MinWatchSeconds = 2;
constexpr uint32 MaxWatchSeconds = 60;

struct TickWatch
{
    uint32 intervalMs = 0;
    uint32 nextMs = 0;
};

std::unordered_map<ObjectGuid, TickWatch> g_TickWatchers;

struct MapHeadcount
{
    uint32 players = 0;
    uint32 bots = 0;
};

// Socketless sessions are managed bots, transient clones and offline clone
// sources - everything that is not a person at a keyboard.
bool IsBot(Player const* player)
{
    return player->GetSession() && player->GetSession()->IsVirtualSession();
}

std::unordered_map<uint64, MapHeadcount> CountHeads(MapHeadcount& total)
{
    std::unordered_map<uint64, MapHeadcount> counts;
    sMapMgr->DoForAllMaps([&](Map* map)
    {
        MapHeadcount& count = counts[(uint64(map->GetId()) << 32) | map->GetInstanceId()];
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* player = itr->GetSource();
            if (!player)
                continue;

            if (IsBot(player))
            {
                ++count.bots;
                ++total.bots;
            }
            else
            {
                ++count.players;
                ++total.players;
            }
        }
    });
    return counts;
}

std::string MapLabel(uint32 mapId, uint32 instanceId)
{
    Map* map = sMapMgr->FindMap(mapId, instanceId);
    std::string label = map ? map->GetMapName() : "map";
    label += " (" + std::to_string(mapId);
    if (instanceId)
        label += " #" + std::to_string(instanceId);
    label += ")";
    return label;
}

char const* PressureName(playerbot::ResourcePressureLevel level)
{
    switch (level)
    {
        case playerbot::ResourcePressureLevel::Hard: return "|cffff4040HARD|r";
        case playerbot::ResourcePressureLevel::Soft: return "|cffffc040SOFT|r";
        default: return "|cff40ff40NORMAL|r";
    }
}

std::string GovernorLine()
{
    playerbot::ResourceGovernorSnapshot const governor = playerbot::ResourceGovernor::GetSnapshot();
    if (!governor.enabled)
        return "Governor: off - bot adds are never throttled.";

    std::string line = fmt::format("Governor: {} (avg {} ms). ", PressureName(governor.level), governor.averageWorldUpdateMs);
    switch (governor.level)
    {
        case playerbot::ResourcePressureLevel::Hard:
            line += "No new bots; if it holds, bots are culled and shed.";
            break;
        case playerbot::ResourcePressureLevel::Soft:
            line += fmt::format("Each match is capped at {} bots.", governor.softMatchBotCap);
            break;
        default:
            line += fmt::format("Unrestricted. Soft at {} ms caps a match at {} bots; hard at {} ms stops bot adds.",
                governor.softUpdateTimeMs, governor.softMatchBotCap, governor.hardUpdateTimeMs);
            break;
    }
    return line;
}

std::string WatchLine()
{
    TickStats::WorldWindow const tick = TickStats::GetWorldWindow(ShortWindowMs);
    playerbot::ResourceGovernorSnapshot const governor = playerbot::ResourceGovernor::GetSnapshot();

    std::string line = fmt::format("[tick] {} ms | 10s avg {} p95 {} max {}", tick.lastMs, tick.avgMs, tick.p95Ms, tick.maxMs);
    if (governor.enabled)
        line += fmt::format(" | gov {} {}", PressureName(governor.level), governor.averageWorldUpdateMs);

    std::vector<TickStats::MapTiming> const maps = TickStats::GetMapTimings(ShortWindowMs);
    for (size_t i = 0; i < std::min<size_t>(maps.size(), 2); ++i)
    {
        Map* map = sMapMgr->FindMap(maps[i].mapId, maps[i].instanceId);
        line += fmt::format("{} {} {}/{}", i ? "," : " | slow:", map ? map->GetMapName() : "map", maps[i].avgMs, maps[i].maxMs);
    }
    return line;
}
}

class PlayerbotTickCommandScript final : public CommandScript
{
public:
    PlayerbotTickCommandScript() : CommandScript("PlayerbotTickCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable tickTable =
        {
            { "",      HandleTickCommand,      rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "watch", HandleTickWatchCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "addon", HandleTickAddonCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "tick", tickTable },
        };

        return commandTable;
    }

    static bool HandleTickCommand(ChatHandler* handler)
    {
        TickStats::WorldWindow const shortWindow = TickStats::GetWorldWindow(ShortWindowMs);
        TickStats::WorldWindow const longWindow = TickStats::GetWorldWindow(LongWindowMs);
        if (!shortWindow.ticks)
        {
            handler->SendSysMessage("No world ticks recorded yet.");
            return true;
        }

        handler->PSendSysMessage("World tick: last %u ms", shortWindow.lastMs);
        handler->PSendSysMessage("  10 s: avg %u, p95 %u, max %u ms (%u ticks)",
            shortWindow.avgMs, shortWindow.p95Ms, shortWindow.maxMs, shortWindow.ticks);
        handler->PSendSysMessage("  %u s: avg %u, p95 %u, max %u ms (%u ticks)",
            std::max<uint32>(longWindow.spanMs / IN_MILLISECONDS, 1), longWindow.avgMs, longWindow.p95Ms, longWindow.maxMs, longWindow.ticks);
        handler->PSendSysMessage("  work per tick (10 s avg): maps %u, sessions %u, battlegrounds %u, scripts %u ms",
            shortWindow.mapsMs, shortWindow.sessionsMs, shortWindow.battlegroundsMs, shortWindow.scriptsMs);
        handler->SendSysMessage(GovernorLine());

        MapHeadcount total;
        std::unordered_map<uint64, MapHeadcount> const heads = CountHeads(total);
        handler->PSendSysMessage("Online: %u players, %u bots", total.players, total.bots);

        std::vector<TickStats::MapTiming> const maps = TickStats::GetMapTimings(ShortWindowMs);
        if (maps.empty())
        {
            handler->SendSysMessage("No map updates recorded (MapUpdate.Threads = 0 records none).");
            return true;
        }

        handler->SendSysMessage("Slowest maps, 10 s (avg / max ms):");
        for (size_t i = 0; i < std::min(maps.size(), MapRowsShown); ++i)
        {
            TickStats::MapTiming const& timing = maps[i];
            auto const headItr = heads.find((uint64(timing.mapId) << 32) | timing.instanceId);
            MapHeadcount const count = headItr != heads.end() ? headItr->second : MapHeadcount();
            handler->PSendSysMessage("  %u. %s: %u / %u - %u players, %u bots",
                uint32(i + 1), MapLabel(timing.mapId, timing.instanceId), timing.avgMs, timing.maxMs, count.players, count.bots);
        }
        return true;
    }

    // The GM Panel's Server page, asked over the core's addon command channel,
    // which hands every system message back as one "m" line:
    //
    //   T1|last|avg10|p95_10|max10|ticks10|avg60|p95_60|max60|ticks60|span60s|maps|sessions|bgs|scripts
    //   T2|enabled|level|emaMs|softMs|hardMs|softMatchBotCap|maxTotalCustomMatchBots
    //   T3|players|bots
    //   TM|mapId|instanceId|name|avg|max|last|players|bots     (slowest first)
    //   TE
    static bool HandleTickAddonCommand(ChatHandler* handler)
    {
        TickStats::WorldWindow const shortWindow = TickStats::GetWorldWindow(ShortWindowMs);
        TickStats::WorldWindow const longWindow = TickStats::GetWorldWindow(LongWindowMs);
        handler->PSendSysMessage("T1|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u",
            shortWindow.lastMs, shortWindow.avgMs, shortWindow.p95Ms, shortWindow.maxMs, shortWindow.ticks,
            longWindow.avgMs, longWindow.p95Ms, longWindow.maxMs, longWindow.ticks, longWindow.spanMs / IN_MILLISECONDS,
            shortWindow.mapsMs, shortWindow.sessionsMs, shortWindow.battlegroundsMs, shortWindow.scriptsMs);

        playerbot::ResourceGovernorSnapshot const governor = playerbot::ResourceGovernor::GetSnapshot();
        handler->PSendSysMessage("T2|%u|%u|%u|%u|%u|%u|%u", governor.enabled ? 1 : 0, uint32(governor.level),
            governor.averageWorldUpdateMs, governor.softUpdateTimeMs, governor.hardUpdateTimeMs,
            governor.softMatchBotCap, governor.maxTotalCustomMatchBots);

        MapHeadcount total;
        std::unordered_map<uint64, MapHeadcount> const heads = CountHeads(total);
        handler->PSendSysMessage("T3|%u|%u", total.players, total.bots);

        std::vector<TickStats::MapTiming> const maps = TickStats::GetMapTimings(ShortWindowMs);
        for (size_t i = 0; i < std::min(maps.size(), AddonMapRows); ++i)
        {
            TickStats::MapTiming const& timing = maps[i];
            Map* map = sMapMgr->FindMap(timing.mapId, timing.instanceId);
            std::string name = map ? map->GetMapName() : "";
            std::replace(name.begin(), name.end(), '|', '/');
            auto const headItr = heads.find((uint64(timing.mapId) << 32) | timing.instanceId);
            MapHeadcount const count = headItr != heads.end() ? headItr->second : MapHeadcount();
            handler->PSendSysMessage("TM|%u|%u|%s|%u|%u|%u|%u|%u", timing.mapId, timing.instanceId, name,
                timing.avgMs, timing.maxMs, timing.lastMs, count.players, count.bots);
        }

        handler->SendSysMessage("TE");
        return true;
    }

    static bool HandleTickWatchCommand(ChatHandler* handler, Optional<uint32> seconds)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        ObjectGuid const guid = player->GetGUID();
        bool const watching = g_TickWatchers.count(guid) != 0;
        if ((!seconds && watching) || (seconds && !*seconds))
        {
            g_TickWatchers.erase(guid);
            handler->SendSysMessage("Tick watch off.");
            return true;
        }

        uint32 const interval = std::clamp<uint32>(seconds.value_or(DefaultWatchSeconds), MinWatchSeconds, MaxWatchSeconds);
        g_TickWatchers[guid] = { interval * IN_MILLISECONDS, GameTime::GetGameTimeMS() };
        handler->PSendSysMessage("Tick watch on: every %u s. `.tick watch` again to stop.", interval);
        return true;
    }
};

class PlayerbotTickWatchWorldScript final : public WorldScript
{
public:
    PlayerbotTickWatchWorldScript() : WorldScript("PlayerbotTickWatchWorldScript") { }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (g_TickWatchers.empty())
            return;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        std::string line;
        for (auto itr = g_TickWatchers.begin(); itr != g_TickWatchers.end();)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(itr->first);
            if (!player || !player->GetSession())
            {
                itr = g_TickWatchers.erase(itr);
                continue;
            }

            if (int32(nowMs - itr->second.nextMs) >= 0)
            {
                if (line.empty())
                    line = WatchLine();
                ChatHandler(player->GetSession()).SendSysMessage(line);
                itr->second.nextMs = nowMs + itr->second.intervalMs;
            }
            ++itr;
        }
    }
};

void AddPlayerbotTickCommandScripts()
{
    new PlayerbotTickCommandScript();
    new PlayerbotTickWatchWorldScript();
}
