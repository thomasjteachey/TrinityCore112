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
 // an ordinary player has no business holding. The gate is the RBAC GM command
 // permission - the ACCOUNT, not the .gm toggle, which a GM turns off constantly
 // just to play - checked on every push rather than once at handshake.
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
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
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

using namespace Trinity::ChatCommands;

namespace
{
    bool s_enabled = true;
    uint32 s_intervalMs = 3000;
    uint32 s_rosterIntervalMs = 15000;

    void LoadBotStatsConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.BotStats.Feed", true);
        s_intervalMs = uint32(std::max(1000, sConfigMgr->GetIntDefault("Centurion.BotStats.IntervalMs", 3000)));
        s_rosterIntervalMs = uint32(std::max(5000, sConfigMgr->GetIntDefault("Centurion.BotStats.RosterIntervalMs", 15000)));
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
    //
    // The three house ids are NOT three houses. With cross-faction auctioning
    // on - which this realm runs, being effectively one faction anyway -
    // GetAuctionsMapByHouseId returns &mNeutralAuctions for every id, so
    // summing the three counted the same listings three times and reported
    // 22,317 against the 7,442 the auction window itself was showing. Distinct
    // pointers, not distinct ids.
    uint32 CountAuctions()
    {
        AuctionHouseObject const* counted[3] = { };
        uint8 distinct = 0;
        uint32 total = 0;

        for (uint8 houseId : { uint8(AUCTIONHOUSE_ALLIANCE), uint8(AUCTIONHOUSE_HORDE), uint8(AUCTIONHOUSE_NEUTRAL) })
        {
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            if (!house)
                continue;

            bool seen = false;
            for (uint8 i = 0; i < distinct; ++i)
                if (counted[i] == house)
                    seen = true;

            if (seen)
                continue;

            counted[distinct++] = house;
            total += house->Getcount();
        }
        return total;
    }

    // A real Game Master, not somebody with .gm mode switched on. IsGameMaster()
    // is the toggle, which a GM turns off constantly just to play - and the feed
    // going dark every time they did was never the intent. This is the account
    // permission, the same test the bot whisper diagnostics use.
    bool IsGameMasterAccount(Player* player)
    {
        // Non-const: HasPermission loads the account's RBAC data on first ask.
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && session->HasPermission(rbac::RBAC_PERM_COMMAND_GM);
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

    // The per-bot roster, so the addon can drill from a zone down to the names
    // standing in it.
    //
    // Pushed on its own slower timer rather than answered on request: 3.3.5 has
    // no clean client-to-server addon channel that does not mean editing the
    // core's chat handler, and a fleet this size is only about forty whispers.
    // Sending it unasked also means the drill-down is instant and filtering is
    // local - the addon already holds every row it could want to show.
    //
    // Flags are a bitfield rather than four columns because the whisper is the
    // scarce thing here: 1 combat, 2 dead, 4 travelling, 8 PvP-only.
    void PushRoster(Player* viewer, std::vector<playerbot::PveManager::BotStatsRow> const& rows)
    {
        std::ostringstream out;
        uint32 inMessage = 0;

        for (auto const& r : rows)
        {
            uint32 flags = 0;
            if (r.InCombat)   flags |= 1;
            if (r.Dead)       flags |= 2;
            if (r.Travelling) flags |= 4;
            if (r.PvpOnly)    flags |= 8;

            out << r.Name << ',' << uint32(r.Level) << ',' << uint32(r.Class) << ','
                << uint32(r.Spec) << ',' << r.ZoneId << ',' << uint32(r.Aggression) << ','
                << r.TimidSeconds << ',' << ToGold(r.MoneyCopper) << ','
                << uint32(r.HealthPct) << ',' << uint32(r.PowerPct) << ','
                << r.ItemLevel << ',' << uint32(r.WornCount) << ',' << uint32(r.GreenPlus) << ','
                << flags << ';';

            if (++inMessage >= 4)
            {
                SendTagged(viewer, "BSTI", out.str());
                out.str(std::string());
                out.clear();
                inMessage = 0;
            }
        }

        if (inMessage)
            SendTagged(viewer, "BSTI", out.str());

        // Tells the addon the sweep is complete, so it can swap the new roster
        // in whole instead of drawing a half-arrived one.
        SendTagged(viewer, "BSTE", std::to_string(rows.size()));
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
                if (player && player->IsInWorld() && IsGameMasterAccount(player))
                    watchers.push_back(player);
            }

            if (watchers.empty())
                return;

            std::vector<playerbot::PveManager::BotStatsRow> rows;
            playerbot::PveManager::CollectBotStats(rows);
            uint32 const auctions = CountAuctions();

            for (Player* watcher : watchers)
                PushTo(watcher, rows, auctions);

            // The roster is forty-odd whispers, so it goes at its own pace.
            _rosterTimer += s_intervalMs;
            if (_rosterTimer >= s_rosterIntervalMs)
            {
                _rosterTimer = 0;
                for (Player* watcher : watchers)
                    PushRoster(watcher, rows);
            }
        }

    private:
        uint32 _timer = 0;
        uint32 _rosterTimer = 0;
    };
}

// The gear and talent panel's data source.
//
// Answered on DEMAND rather than streamed, because a full equipment list for
// 255 bots every few seconds is an order of magnitude more than the rest of the
// feed put together, and the window only ever looks at one bot.
//
// A GM chat command IS the inbound channel. 3.3.5 has no client-to-server addon
// channel that does not mean editing the core's chat handler, but a slash
// command is already parsed out of chat and already carries an RBAC gate - so
// the addon simply sends ".botstats gear <name>" and reads the reply off the
// same CCGAME whisper as everything else.
namespace
{
    void SendGearTo(Player* viewer, Player* bot)
    {
        std::ostringstream out;
        out << bot->GetName() << '|';

        uint32 inMessage = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                continue;

            out << uint32(slot) << ',' << proto->ItemId << ',' << proto->Quality << ','
                << proto->ItemLevel << ';';

            // Item ids are short, but a full set still overruns one whisper.
            if (++inMessage >= 6)
            {
                SendTagged(viewer, "BSTG", out.str());
                out.str(std::string());
                out.clear();
                out << bot->GetName() << '|';
                inMessage = 0;
            }
        }

        if (inMessage)
            SendTagged(viewer, "BSTG", out.str());

        // Points per talent tree, in OrderIndex order so the client can name
        // them positionally the way the talent frame does.
        uint32 points[3] = { 0, 0, 0 };
        uint32 const classMask = 1 << (bot->GetClass() - 1);
        uint8 const spec = uint8(bot->GetActiveSpec());

        for (TalentEntry const* talent : sTalentStore)
        {
            if (!talent)
                continue;

            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TabID);
            if (!tab || !(tab->ClassMask & classMask) || tab->OrderIndex > 2)
                continue;

            // Highest rank the bot actually has; ranks are cumulative, so the
            // top one it knows IS the number of points sunk into that talent.
            for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (talent->SpellRank[rank] && bot->HasTalent(talent->SpellRank[rank], spec))
                {
                    points[tab->OrderIndex] += uint32(rank) + 1;
                    break;
                }
            }
        }

        std::ostringstream tal;
        tal << bot->GetName() << '|' << points[0] << ',' << points[1] << ',' << points[2];
        SendTagged(viewer, "BSTT", tal.str());
    }

    class centurion_bot_stats_commands : public CommandScript
    {
    public:
        centurion_bot_stats_commands() : CommandScript("centurion_bot_stats_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable botStatsTable =
            {
                { "gear", HandleBotStatsGear, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "botstats", botStatsTable },
            };
            return commandTable;
        }

        static bool HandleBotStatsGear(ChatHandler* handler, std::string_view botName)
        {
            Player* viewer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!viewer || botName.empty())
                return false;

            // By name and across the whole realm: the window lists bots the
            // asker cannot see and often is not on the same continent as.
            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            if (!bot || !bot->IsInWorld())
            {
                handler->PSendSysMessage("botstats: no bot named %s is online.", std::string(botName).c_str());
                return true;
            }

            SendGearTo(viewer, bot);
            return true;
        }
    };
}

void AddSC_centurion_bot_stats_feed()
{
    new centurion_bot_stats_feed();
    new centurion_bot_stats_commands();
}
