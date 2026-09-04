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

 // Fleet telemetry for the GM stats addon (CENTURION_BotStats).
 //
 // GM ONLY, and deliberately so: this is a live readout of where every bot is,
 // how aggressive it is and how much it is carrying - a map of the fleet that
 // an ordinary player has no business holding. The gate is IsGameMaster(), the
 // same test the whisper diagnostics use, checked on every push rather than at
 // handshake time so dropping GM mode stops the feed immediately.
 //
 // Transport is the CCGAME addon whisper the bot map already uses
 // (custom_bot_map_feed.cpp), so no new client prefix has to be registered.
 // Three record types, each its own tag, so the addon can redraw one panel
 // without waiting for the others:
 //
 //   BSTA:<bots>|<combat>|<dead>|<travelling>|<timid>|<avgAggr>|<goldK>|<auctions>
 //   BSTZ:<zoneId>,<count>,<avgLevel>,<avgAggr>,<timid>,<goldK>;...
 //   BSTL:<levelBand>,<count>,<avgAggr>,<timid>,<goldK>;...
 //
 // Aggregation happens here rather than in Lua: a 256-bot fleet is a lot of
 // rows to push at a client every few seconds, and the addon only ever draws
 // the totals.

#include "AuctionHouseMgr.h"
#include "DBCStores.h"
#include "Chat.h"
#include "Configuration/Config.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    bool s_enabled = true;
    uint32 s_intervalMs = 3000;

    void LoadBotStatsConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.BotStats.Feed", true);
        s_intervalMs = uint32(std::max(1000, sConfigMgr->GetIntDefault("Centurion.BotStats.IntervalMs", 3000)));
    }

    void SendTagged(Player* viewer, std::string const& tag, std::string const& payload)
    {
        std::string const message = "CCGAME\t" + tag + ":" + payload;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty,
            ObjectGuid::Empty, message, 0);
        viewer->SendDirectMessage(&data);
    }

    // Gold, rounded to whole gold. Copper resolution across a whole fleet is
    // noise, and the addon draws it on an axis.
    uint32 ToGold(uint64 copper)
    {
        return uint32(copper / 10000u);
    }

    // The 3.3.5 client has no zone-id to name lookup, so the name travels with
    // the row. Commas, semicolons and pipes are the record separators, so they
    // are scrubbed rather than escaped - no zone name contains one.
    std::string ZoneName(uint32 zoneId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        char const* name = area ? area->AreaName[LOCALE_enUS] : nullptr;
        if (!name || !*name)
            return "Zone " + std::to_string(zoneId);

        std::string out(name);
        for (char& c : out)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';
        return out;
    }

    struct Bucket
    {
        uint32 Count = 0;
        uint64 LevelSum = 0;
        uint64 AggrSum = 0;
        uint32 Timid = 0;
        uint64 Copper = 0;
    };

    // Auction listings, counted once per push rather than per viewer.
    uint32 CountAuctions()
    {
        uint32 total = 0;
        for (uint8 houseId : { uint8(AUCTIONHOUSE_ALLIANCE), uint8(AUCTIONHOUSE_HORDE), uint8(AUCTIONHOUSE_NEUTRAL) })
            if (AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId))
                total += house->Getcount();
        return total;
    }

    void PushTo(Player* viewer, std::vector<playerbot::PveManager::BotStatsRow> const& rows, uint32 auctions)
    {
        Bucket all;
        uint32 inCombat = 0, dead = 0, travelling = 0;
        std::map<uint32, Bucket> byZone;
        std::map<uint32, Bucket> byBand;

        for (auto const& r : rows)
        {
            all.Count++;
            all.LevelSum += r.Level;
            all.AggrSum += r.Aggression;
            all.Copper += r.MoneyCopper;
            if (r.TimidSeconds)
                all.Timid++;
            if (r.InCombat)
                inCombat++;
            if (r.Dead)
                dead++;
            if (r.Travelling)
                travelling++;

            // Ten-level bands, which is how the zones are laid out anyway.
            uint32 const band = uint32(r.Level / 10) * 10;
            for (Bucket* b : { &byZone[r.ZoneId], &byBand[band] })
            {
                b->Count++;
                b->LevelSum += r.Level;
                b->AggrSum += r.Aggression;
                b->Copper += r.MoneyCopper;
                if (r.TimidSeconds)
                    b->Timid++;
            }
        }

        {
            std::ostringstream out;
            out << all.Count << '|' << inCombat << '|' << dead << '|' << travelling << '|'
                << all.Timid << '|' << (all.Count ? uint32(all.AggrSum / all.Count) : 0u) << '|'
                << ToGold(all.Copper) << '|' << auctions;
            SendTagged(viewer, "BSTA", out.str());
        }

        // Zones and bands are chunked: a whisper is capped well below what a
        // hundred zones would need, and the addon rebuilds its table from
        // whatever arrives before the next aggregate.
        auto flush = [&](char const* tag, std::map<uint32, Bucket> const& src, bool named)
        {
            std::ostringstream out;
            uint32 inMessage = 0;
            for (auto const& entry : src)
            {
                Bucket const& b = entry.second;
                if (!b.Count)
                    continue;

                out << entry.first << ',';
                if (named)
                    out << ZoneName(entry.first) << ',';
                out << b.Count << ',' << uint32(b.LevelSum / b.Count) << ','
                    << uint32(b.AggrSum / b.Count) << ',' << b.Timid << ',' << ToGold(b.Copper) << ';';

                // Named rows are far longer, so fewer fit inside one whisper.
                if (++inMessage >= (named ? 6u : 14u))
                {
                    SendTagged(viewer, tag, out.str());
                    out.str(std::string());
                    out.clear();
                    inMessage = 0;
                }
            }
            if (inMessage)
                SendTagged(viewer, tag, out.str());
        };

        flush("BSTZ", byZone, true);
        flush("BSTL", byBand, false);
    }

    class centurion_bot_stats_feed : public WorldScript
    {
    public:
        centurion_bot_stats_feed() : WorldScript("centurion_bot_stats_feed") { }

        void OnConfigLoad(bool /*reload*/) override
        {
            LoadBotStatsConfig();
        }

        void OnUpdate(uint32 diff) override
        {
            if (!s_enabled)
                return;

            _timer += diff;
            if (_timer < s_intervalMs)
                return;
            _timer = 0;

            // Collect the roster ONCE however many GMs are watching, then hand
            // each of them the same aggregate. Walking the fleet per viewer is
            // what would make this expensive.
            std::vector<Player*> watchers;
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* player = pair.second;
                if (player && player->IsInWorld() && player->IsGameMaster() && player->GetSession())
                    watchers.push_back(player);
            }

            if (watchers.empty())
                return;

            std::vector<playerbot::PveManager::BotStatsRow> rows;
            playerbot::PveManager::CollectBotStats(rows);
            uint32 const auctions = CountAuctions();

            for (Player* watcher : watchers)
                PushTo(watcher, rows, auctions);
        }

    private:
        uint32 _timer = 0;
    };
}

void AddSC_centurion_bot_stats_feed()
{
    new centurion_bot_stats_feed();
}
