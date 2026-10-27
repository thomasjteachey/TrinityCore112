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

// Inn bounty boards: the fleet as a player gets to read it.
//
// Every inn in a contested zone has a board. Reading it shows the bots in that
// zone - and, a click away, in every other contested zone - with exactly what
// killing each one would win: the gear the full-loot cache would take off the
// corpse, the spec behind it, and how many people it has killed.
//
// It is the GM Bot Stats window cut down to what a death actually pays out.
// Where a bot stands, how aggressive or timid it is and the coin it carries
// stay GM-only (custom_bot_stats_feed.cpp): a board names the zone and nothing
// finer.
//
// --- the wire -------------------------------------------------------------
//
// The client half is the CENTURION_BountyBoard addon. Clicking a board opens it
// on the board's zone; everything on it is then asked for over the realm's
// addon channel - CCGAMEREQ from the client, CCGAME back.
//
//   server -> client  BBOP:<zoneId>                          a board was clicked
//   client -> server  CCGAMEREQ\tBOUNTY:OPEN:<zoneId>        zone list + that zone
//                     CCGAMEREQ\tBOUNTY:ZONE:<zoneId>        another contested zone
//                     CCGAMEREQ\tBOUNTY:BOT:<guid>           one notice in full
//   server -> client  BBZL:<zoneId>,<count>,<name>;...       zones with notices (chunked)
//                     BBZE:<zones>                           end of the zone list
//                     BBRS:<zoneId>|<notice>;...             one zone's notices (chunked)
//                     BBRE:<zoneId>|<count>                  end of them
//                     BBPI:<guid>|<spoil>;...                one bot's spoils (chunked)
//                     BBPT:<guid>|<t1>,<t2>,<t3>|          the talent split - nothing finer
//                     BBPE:<guid>|<1 still here, 0 gone>
//                     BBCL:far                               walked away from the board
//
//   notice = <guid>,<level>,<race>,<class>,<t1>,<t2>,<t3>,<kills>,<spoils>,<id>:<quality>/...,<name>
//   spoil  = <slot or -1 carried>,<id>,<quality>,<ilvl>,<enchant>,<gem1>,<gem2>,<gem3>,<randomProp>,<seed>
//
// The click does not carry the data itself because CMSG_GAMEOBJ_USE is handled
// in place, on whichever map thread holds the board, and a notice reads bots on
// every continent. Chat is PROCESS_THREADUNSAFE - the world thread, where
// reading any player is as safe as the .botstats commands already make it.
//
// Without the addon a board is still a gossip menu: "Read the notices" writes
// them into the chat frame, item links and all.
//
// --- lifetime player kills ------------------------------------------------
//
// Each notice says how many people the bot has killed. The stock lifetime
// honorable-kill counter cannot say it: Player::RewardHonor refuses every kill
// between members of the same team, and on this one-team realm that is every
// open-world kill there is - the world fleet reads zero across the board. So
// killing blows on players are counted here, in character_player_kills, for
// every character. A pet's or a totem's blow is its master's.

#include "custom_barracks_hardcore.h"
#include "custom_bounty_board.h"
#include "custom_bounty_notices.h"
#include "custom_bounty_writs.h"
#include "custom_nemesis.h"

#include "Bag.h"
#include "Chat.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MapManager.h"
#include "Miscellaneous/AddonRequestRoutes.h"
#include "ObjectMgr.h"
#include "Miscellaneous/Surnames.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    bool s_enabled = true;
    uint32 s_boardEntry = 900310;
    float s_reachYards = 15.0f;
    uint32 s_gossipTextId = 900310;

    void LoadBountyBoardConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.BountyBoard.Enable", true);
        s_boardEntry = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.GameObjectEntry", 900310)));
        s_reachYards = std::max(5.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.ReachYards", 15.0f));
        s_gossipTextId = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.GossipTextId", 900310)));
    }

    // --------------------------------------------------------------- kills

    // Probed once at startup: the first write to a missing table would abort the
    // worldserver, and a realm on this branch may not have it yet.
    std::atomic<bool> s_killTableExists{ false };

    // Written from map threads (a death is), read from the world thread (a board
    // is), so the cache keeps its own lock.
    std::mutex s_killsLock;
    std::unordered_map<ObjectGuid::LowType, uint32> s_lifetimeKills;

    uint32 LifetimeKills(ObjectGuid::LowType guid)
    {
        std::lock_guard<std::mutex> guard(s_killsLock);
        auto const itr = s_lifetimeKills.find(guid);
        return itr != s_lifetimeKills.end() ? itr->second : 0;
    }

    // --------------------------------------------------------------- the wire

    void SendTagged(Player* viewer, std::string const& tag, std::string const& payload)
    {
        std::string const message = "CCGAME\t" + tag + ":" + payload;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty,
            ObjectGuid::Empty, message, 0);
        viewer->SendDirectMessage(&data);
    }

    // Rows packed into as few whispers as will hold them. An addon message tops
    // out at 255 bytes with its prefix, so a line goes out before the next row
    // would push it past that, rather than after a fixed number of rows - a
    // notice with a long surname and five spoils is three times a short one.
    class Chunker
    {
    public:
        Chunker(Player* viewer, std::string tag, std::string head)
            : _viewer(viewer), _tag(std::move(tag)), _head(std::move(head)) { }

        void Add(std::string const& row)
        {
            if (!_body.empty() && _head.size() + _body.size() + row.size() + 1 > MaxPayload)
                Flush();
            _body += row;
            _body += ';';
        }

        void Flush()
        {
            if (_body.empty())
                return;
            SendTagged(_viewer, _tag, _head + _body);
            _body.clear();
        }

    private:
        // 255, less "CCGAME\t", a four letter tag and its colon, and some slack.
        static constexpr size_t MaxPayload = 236;

        Player* _viewer;
        std::string _tag;
        std::string _head;
        std::string _body;
    };

    // Commas, semicolons and pipes are the record separators. No zone or
    // character name contains one; scrubbing costs nothing and keeps it so.
    std::string Scrubbed(std::string text)
    {
        for (char& c : text)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';
        return text;
    }

    std::string ZoneName(uint32 zoneId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        char const* name = area ? area->AreaName[LOCALE_enUS] : nullptr;
        if (!name || !*name)
            return "Zone " + std::to_string(zoneId);
        return Scrubbed(name);
    }

    // --------------------------------------------------------------- who is on a board

    // Zones a board stands in. Read from the spawns at startup, and added to by
    // any board clicked since - one placed by hand while the realm runs. Written
    // from map threads (a click is), read from the world thread.
    std::mutex s_boardZonesLock;
    std::unordered_set<uint32> s_boardZones;

    void AddBoardZone(uint32 zoneId)
    {
        if (!zoneId)
            return;
        std::lock_guard<std::mutex> guard(s_boardZonesLock);
        s_boardZones.insert(zoneId);
    }

    bool HasABoard(uint32 zoneId)
    {
        std::lock_guard<std::mutex> guard(s_boardZonesLock);
        return s_boardZones.count(zoneId) != 0;
    }

    // Contested as Blizzard drew it: a top-level zone on the two old continents
    // that AreaTable gives to neither faction. On this realm every faction's home
    // zones read Alliance, so this is Stranglethorn, Tanaris, the Plaguelands and
    // the rest - never a starter zone or a capital. A zone a board was put up in
    // counts too, whatever AreaTable says of it: a board there is the owner
    // saying it is hunting ground, and one that lists nobody reads as broken.
    bool IsContestedZone(uint32 zoneId)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
        if (!zone || zone->ParentAreaID || (zone->Flags & (AREA_FLAG_CAPITAL | AREA_FLAG_SANCTUARY)))
            return false;
        if (HasABoard(zoneId))
            return true;
        return zone->FactionGroupMask == AREATEAM_NONE && (zone->ContinentID == 0 || zone->ContinentID == 1);
    }

    // The world fleet, alive and standing in a contested zone. A dead bot has
    // just paid out whatever it carried, and a bot in a battleground is not out
    // there to be found.
    bool IsOnTheBoard(Player const* player)
    {
        if (!player || !player->IsInWorld() || !player->IsAlive() || player->IsGameMaster())
            return false;

        if (!BarracksHardcore::IsPlayerbot(player))
            return false;

        WorldSession const* session = player->GetSession();
        if (!session || session->IsTransientPlayerSession())
            return false;

        if (player->InBattleground() || player->InArena())
            return false;

        return IsContestedZone(player->GetZoneId());
    }

    // Highest level first: the notice worth reading tops the board.
    std::vector<Player*> BotsIn(uint32 zoneId)
    {
        std::vector<Player*> bots;
        for (auto const& pair : ObjectAccessor::GetPlayers())
            if (IsOnTheBoard(pair.second) && pair.second->GetZoneId() == zoneId)
                bots.push_back(pair.second);

        std::sort(bots.begin(), bots.end(), [](Player const* a, Player const* b)
        {
            if (a->GetLevel() != b->GetLevel())
                return a->GetLevel() > b->GetLevel();
            return a->GetGUID().GetCounter() < b->GetGUID().GetCounter();
        });
        return bots;
    }

    std::string NoticeName(Player const* bot)
    {
        return Scrubbed(Surnames::Decorated(bot->GetGUID(), bot->GetName()));
    }

    // --------------------------------------------------------------- what a notice says

    struct Spoil
    {
        Item const* Piece = nullptr;
        int32 Slot = -1;          // the equipment slot, or -1 in the bags
    };

    // What the cache would take, by the cache's own rule.
    std::vector<Spoil> SpoilsOf(Player const* bot)
    {
        std::vector<Spoil> spoils;
        if (!BarracksHardcore::IsFullLootEnabled())
            return spoils;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (BarracksHardcore::IsStakedOnDeath(item->GetTemplate(), false))
                    spoils.push_back({ item, int32(slot) });

        auto const carried = [&](Item const* item)
        {
            if (item && BarracksHardcore::IsStakedOnDeath(item->GetTemplate(), true))
                spoils.push_back({ item, -1 });
        };

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            carried(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = bot->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    carried(bag->GetItemByPos(uint8(i)));

        // Best first, so the five a notice has room for are the five worth
        // having. Ties fall to the worn piece and then the item id, which keeps
        // the order the same from one reading of the board to the next.
        std::sort(spoils.begin(), spoils.end(), [](Spoil const& a, Spoil const& b)
        {
            ItemTemplate const* pa = a.Piece->GetTemplate();
            ItemTemplate const* pb = b.Piece->GetTemplate();
            if (pa->Quality != pb->Quality)
                return pa->Quality > pb->Quality;
            if (pa->ItemLevel != pb->ItemLevel)
                return pa->ItemLevel > pb->ItemLevel;
            if ((a.Slot >= 0) != (b.Slot >= 0))
                return a.Slot >= 0;
            return pa->ItemId < pb->ItemId;
        });
        return spoils;
    }

    // Points per tree and which one leads - the spec. The talents themselves
    // are not on a notice: that was more than a wanted poster should know.
    struct TalentSplit
    {
        uint32 Points[3] = { 0, 0, 0 };
        uint32 MainTree = 0;
    };

    TalentSplit ReadTalents(Player const* bot)
    {
        TalentSplit split;
        uint32 const classMask = 1 << (bot->GetClass() - 1);
        uint8 const spec = uint8(bot->GetActiveSpec());

        for (TalentEntry const* talent : sTalentStore)
        {
            if (!talent)
                continue;

            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TabID);
            if (!tab || !(tab->ClassMask & classMask) || tab->OrderIndex > 2)
                continue;

            // Ranks are cumulative, so the top one known is the points spent.
            for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (!talent->SpellRank[rank] || !bot->HasTalent(talent->SpellRank[rank], spec))
                    continue;

                split.Points[tab->OrderIndex] += uint32(rank) + 1;
                break;
            }
        }

        for (uint32 tree = 1; tree < 3; ++tree)
            if (split.Points[tree] > split.Points[split.MainTree])
                split.MainTree = tree;

        return split;
    }

    // --------------------------------------------------------------- answers

    void SendZoneList(Player* viewer)
    {
        std::map<uint32, uint32> counts;
        for (auto const& pair : ObjectAccessor::GetPlayers())
            if (IsOnTheBoard(pair.second))
                ++counts[pair.second->GetZoneId()];

        Chunker out(viewer, "BBZL", "");
        for (auto const& [zoneId, count] : counts)
            out.Add(Trinity::StringFormat("{},{},{}", zoneId, count, ZoneName(zoneId)));
        out.Flush();

        SendTagged(viewer, "BBZE", std::to_string(counts.size()));
    }

    void SendNotices(Player* viewer, uint32 zoneId)
    {
        std::string const head = std::to_string(zoneId) + "|";
        std::vector<Player*> const bots = IsContestedZone(zoneId) ? BotsIn(zoneId) : std::vector<Player*>();

        Chunker out(viewer, "BBRS", head);
        for (Player const* bot : bots)
        {
            TalentSplit const talents = ReadTalents(bot);
            std::vector<Spoil> const spoils = SpoilsOf(bot);
            ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();

            std::ostringstream row;
            row << guid << ',' << uint32(bot->GetLevel()) << ',' << uint32(bot->GetRace()) << ','
                << uint32(bot->GetClass()) << ',' << talents.Points[0] << ',' << talents.Points[1] << ','
                << talents.Points[2] << ',' << LifetimeKills(guid) << ',' << spoils.size() << ',';
            for (size_t i = 0; i < spoils.size() && i < 5; ++i)
                row << (i ? "/" : "") << spoils[i].Piece->GetEntry() << ':'
                    << spoils[i].Piece->GetTemplate()->Quality;
            row << ',' << NoticeName(bot);

            out.Add(row.str());
        }
        out.Flush();

        // What this reader in particular may see of them: their own notices,
        // gear off their body, and what their Ledger rank reads. Before the
        // end marker, so the addon has it when it draws.
        BountyNotices::SendBoardExtras(viewer, zoneId, bots);
        Nemesis::SendBoardRows(viewer, zoneId, bots);
        BountyWrits::SendZoneWrits(viewer, zoneId, bots);

        SendTagged(viewer, "BBRE", head + std::to_string(bots.size()));
    }

    void SendPoster(Player* viewer, ObjectGuid::LowType guidLow)
    {
        std::string const head = std::to_string(guidLow) + "|";

        // Only a bot that is still on a board. One that died or walked out of
        // the contested zones since the board was read says so, rather than
        // handing over a list of gear it no longer has.
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guidLow));
        if (!IsOnTheBoard(bot))
        {
            SendTagged(viewer, "BBPE", head + "0");
            return;
        }

        // Every field the item link needs, so the tooltip is the piece itself -
        // suffix, enchant and gems - which is what the cache now hands over.
        Chunker items(viewer, "BBPI", head);
        for (Spoil const& spoil : SpoilsOf(bot))
        {
            Item const* item = spoil.Piece;
            ItemTemplate const* proto = item->GetTemplate();
            items.Add(Trinity::StringFormat("{},{},{},{},{},{},{},{},{},{}",
                spoil.Slot, proto->ItemId, proto->Quality, proto->ItemLevel,
                item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT),
                item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT),
                item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_2),
                item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_3),
                item->GetItemRandomPropertyId(), item->GetItemSuffixFactor()));
        }
        items.Flush();

        // The trailing pipe is where the talent list used to follow; the first
        // addon release still expects the separator.
        TalentSplit const talents = ReadTalents(bot);
        SendTagged(viewer, "BBPT", Trinity::StringFormat("{}{},{},{}|", head,
            talents.Points[0], talents.Points[1], talents.Points[2]));

        // Whose body each piece came off, and which notices this reader may
        // take on the bot.
        BountyNotices::SendPosterExtras(viewer, bot);
        Nemesis::SendPosterLine(viewer, bot);
        BountyWrits::SendPosterWrit(viewer, bot);

        SendTagged(viewer, "BBPE", head + "1");
    }

    // A board answers only somebody standing at one, the way a vendor's window
    // closes when you walk off - and the fleet's gear is not something to be
    // read from anywhere in the world through a hand-written addon message.
    bool IsAtABoard(Player const* player)
    {
        return player->FindNearestGameObject(s_boardEntry, s_reachYards) != nullptr;
    }

    // A few requests a second is all the addon ever makes; more is somebody
    // replaying messages. World thread only, like everything the chat handler
    // calls, so no lock.
    struct RequestBudget
    {
        float Tokens = 8.0f;
        uint32 LastMs = 0;
    };
    std::unordered_map<ObjectGuid::LowType, RequestBudget> s_budgets;

    bool TakeRequest(Player const* player)
    {
        uint32 const now = getMSTime();
        RequestBudget& budget = s_budgets[player->GetGUID().GetCounter()];
        if (budget.LastMs)
            budget.Tokens = std::min(8.0f, budget.Tokens + float(getMSTimeDiff(budget.LastMs, now)) / 250.0f);
        budget.LastMs = now;

        if (budget.Tokens < 1.0f)
            return false;

        budget.Tokens -= 1.0f;
        return true;
    }

    void HandleBoardRequest(Player* player, std::string_view argument)
    {
        if (!s_enabled || !player->IsInWorld())
            return;

        if (!IsAtABoard(player))
        {
            SendTagged(player, "BBCL", "far");
            return;
        }

        if (!TakeRequest(player))
            return;

        size_t const colon = argument.find(':');
        if (colon == std::string_view::npos)
            return;

        std::string_view const verb = argument.substr(0, colon);

        // The nemesis hierarchy (custom_nemesis.cpp), then taking, tearing up
        // and paying for notices (custom_bounty_notices.cpp).
        if (Nemesis::HandleRequest(player, verb, argument.substr(colon + 1)))
            return;
        if (BountyWrits::HandleRequest(player, verb, argument.substr(colon + 1)))
            return;
        if (BountyNotices::HandleRequest(player, verb, argument.substr(colon + 1)))
            return;

        Optional<uint32> const id = Trinity::StringTo<uint32>(argument.substr(colon + 1));
        if (!id)
            return;

        if (verb == "OPEN")
        {
            SendZoneList(player);
            SendNotices(player, *id);
            BountyNotices::SendLedgerAndNotices(player);
            Nemesis::SendBoardState(player);
            BountyWrits::SendBoardState(player);
        }
        else if (verb == "ZONE")
            SendNotices(player, *id);
        else if (verb == "BOT")
            SendPoster(player, *id);
    }

    // --------------------------------------------------------------- no addon

    char const* const SpecNames[12][3] =
    {
        { "", "", "" },
        { "Arms", "Fury", "Protection" },
        { "Holy", "Protection", "Retribution" },
        { "Beast Mastery", "Marksmanship", "Survival" },
        { "Assassination", "Combat", "Subtlety" },
        { "Discipline", "Holy", "Shadow" },
        { "Blood", "Frost", "Unholy" },
        { "Elemental", "Enhancement", "Restoration" },
        { "Arcane", "Fire", "Frost" },
        { "Affliction", "Demonology", "Destruction" },
        { "", "", "" },
        { "Balance", "Feral", "Restoration" },
    };

    std::string ItemLink(Item const* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        std::string name = proto->Name1;

        int32 const property = item->GetItemRandomPropertyId();
        if (property > 0)
        {
            if (ItemRandomPropertiesEntry const* entry = sItemRandomPropertiesStore.LookupEntry(property))
                if (entry->Name[LOCALE_enUS] && *entry->Name[LOCALE_enUS])
                    name = name + " " + entry->Name[LOCALE_enUS];
        }
        else if (property < 0)
        {
            if (ItemRandomSuffixEntry const* entry = sItemRandomSuffixStore.LookupEntry(uint32(-property)))
                if (entry->Name[LOCALE_enUS] && *entry->Name[LOCALE_enUS])
                    name = name + " " + entry->Name[LOCALE_enUS];
        }

        return Trinity::StringFormat("|c{:08x}|Hitem:{}:{}:{}:{}:{}:0:{}:{}:0|h[{}]|h|r",
            ItemQualityColors[std::min<uint32>(proto->Quality, MAX_ITEM_QUALITY - 1)], proto->ItemId,
            item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT), item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT),
            item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_2), item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_3),
            property, item->GetItemSuffixFactor(), name);
    }

    void WriteNoticesToChat(Player* player, uint32 zoneId)
    {
        ChatHandler handler(player->GetSession());
        std::vector<Player*> const bots = BotsIn(zoneId);
        if (bots.empty())
        {
            handler.SendSysMessage(Trinity::StringFormat("No notices are posted for {}.", ZoneName(zoneId)));
            return;
        }

        handler.SendSysMessage(Trinity::StringFormat("|cffffd200Wanted in {}:|r", ZoneName(zoneId)));
        for (Player const* bot : bots)
        {
            TalentSplit const talents = ReadTalents(bot);
            std::vector<Spoil> const spoils = SpoilsOf(bot);
            uint8 const cls = bot->GetClass();
            ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(cls);

            std::string line = Trinity::StringFormat("[{}] {}, {} {} - {} player kills.", uint32(bot->GetLevel()),
                NoticeName(bot), cls < 12 ? SpecNames[cls][talents.MainTree] : "",
                classEntry ? classEntry->Name[LOCALE_enUS] : "", LifetimeKills(bot->GetGUID().GetCounter()));

            if (spoils.empty())
                line += " Carries nothing worth the taking.";
            else
            {
                line += " Spoils:";
                for (size_t i = 0; i < spoils.size() && i < 3; ++i)
                    line += " " + ItemLink(spoils[i].Piece);
                if (spoils.size() > 3)
                    line += Trinity::StringFormat(" and {} more", spoils.size() - 3);
            }
            handler.SendSysMessage(line);
        }
    }
}

// What the notices need to know about the board, answered by the board's own
// rules (custom_bounty_board.h).
namespace BountyBoard
{
    bool IsOnTheBoard(Player const* bot)
    {
        return ::IsOnTheBoard(bot);
    }

    uint32 LifetimeKills(ObjectGuid::LowType guid)
    {
        return ::LifetimeKills(guid);
    }

    std::string NoticeName(Player const* bot)
    {
        return ::NoticeName(bot);
    }

    std::string ZoneName(uint32 zoneId)
    {
        return ::ZoneName(zoneId);
    }

    std::vector<uint32> BoardZones()
    {
        std::lock_guard<std::mutex> guard(s_boardZonesLock);
        std::vector<uint32> zones(s_boardZones.begin(), s_boardZones.end());
        std::sort(zones.begin(), zones.end());
        return zones;
    }

    bool IsAtABoard(Player const* player)
    {
        return player && ::IsAtABoard(player);
    }

    void SendTagged(Player* viewer, std::string const& tag, std::string const& payload)
    {
        if (viewer)
            ::SendTagged(viewer, tag, payload);
    }
}

class centurion_bounty_board_world : public WorldScript
{
public:
    centurion_bounty_board_world() : WorldScript("centurion_bounty_board_world") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadBountyBoardConfig();
    }

    void OnStartup() override
    {
        LoadBountyBoardConfig();

        // The zone each board stands in, worked out the way a player's own zone
        // is - the spawn row's zoneId is 0 for a board placed in game.
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGameObjectData())
            if (data.id == s_boardEntry)
                AddBoardZone(sMapMgr->GetZoneId(data.phaseMask, data.mapId, data.spawnPoint));
        TC_LOG_INFO("server.loading", "Bounty boards: boards stand in {} zones.", s_boardZones.size());

        s_killTableExists = bool(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_player_kills'"));
        if (!s_killTableExists)
        {
            TC_LOG_ERROR("server.loading", "Bounty boards: table character_player_kills is missing, so player kills are not counted.");
            return;
        }

        std::lock_guard<std::mutex> guard(s_killsLock);
        s_lifetimeKills.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT guid, kills FROM character_player_kills"))
        {
            do
            {
                Field* fields = result->Fetch();
                s_lifetimeKills[fields[0].GetUInt32()] = fields[1].GetUInt32();
            } while (result->NextRow());
        }
        TC_LOG_INFO("server.loading", "Bounty boards: lifetime player kills loaded for {} characters.", s_lifetimeKills.size());
    }
};

class centurion_bounty_board_kills : public PlayerScript
{
public:
    centurion_bounty_board_kills() : PlayerScript("centurion_bounty_board_kills") { }

    void OnPlayerJustDied(Player* victim, Unit* killer) override
    {
        if (!victim || !killer || !s_killTableExists)
            return;

        // A pet's, a totem's or a mind-controlled creature's killing blow
        // belongs to the player behind it.
        Player* credited = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!credited || credited == victim)
            return;

        // A transient copy - battleground fill, an Obsidian Colosseum mirror, a
        // bounty hunter - runs under a guid that is not a character, so it has
        // no lifetime to add to.
        WorldSession const* session = credited->GetSession();
        if (!session || session->IsTransientPlayerSession())
            return;

        ObjectGuid::LowType const guid = credited->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> guard(s_killsLock);
            ++s_lifetimeKills[guid];
        }

        CharacterDatabase.PExecute(
            "INSERT INTO character_player_kills (guid, kills) VALUES ({}, 1) ON DUPLICATE KEY UPDATE kills = kills + 1",
            guid);
    }
};

class go_centurion_bounty_board : public GameObjectScript
{
public:
    go_centurion_bounty_board() : GameObjectScript("go_centurion_bounty_board") { }

    struct go_centurion_bounty_boardAI : public GameObjectAI
    {
        go_centurion_bounty_boardAI(GameObject* go) : GameObjectAI(go) { }

        // Map thread: say which zone this board answers for and nothing about
        // the fleet - the addon asks for that over chat. The gossip menu is for
        // a client without it; the addon closes it as it opens the board.
        bool OnGossipHello(Player* player) override
        {
            ClearGossipMenuFor(player);
            AddBoardZone(me->GetZoneId());
            if (s_enabled)
            {
                // Touching a board hands in every finished notice - before the
                // board opens, so the addon reads the ledger as it now stands.
                BountyNotices::OnBoardTouched(player, me);
                BountyWrits::OnBoardTouched(player);

                SendTagged(player, "BBOP", std::to_string(me->GetZoneId()));
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Read the notices.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);
                BountyNotices::AddGossipOptions(player);
                Nemesis::AddGossipOptions(player);
                BountyWrits::AddGossipOptions(player);
            }
            SendGossipMenuFor(player, s_gossipTextId, me->GetGUID());
            return true;
        }

        // CMSG_GOSSIP_SELECT_OPTION is PROCESS_THREADUNSAFE, so this runs on the
        // world thread and may read the whole fleet.
        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 const sender = GetGossipSenderFor(player, gossipListId);
            uint32 const action = GetGossipActionFor(player, gossipListId);

            // The notices' own options: they close the menu or open the next.
            if (s_enabled && BountyNotices::OnGossipSelect(player, me, sender, action))
                return true;
            if (s_enabled && Nemesis::OnGossipSelect(player, me, sender, action))
                return true;
            if (s_enabled && BountyWrits::OnGossipSelect(player, me, sender, action))
                return true;

            CloseGossipMenuFor(player);
            if (s_enabled && action == GOSSIP_ACTION_INFO_DEF + 1)
                WriteNoticesToChat(player, me->GetZoneId());
            return true;
        }
    };

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new go_centurion_bounty_boardAI(go);
    }
};

void AddSC_custom_bounty_board()
{
    new centurion_bounty_board_world();
    new centurion_bounty_board_kills();
    new go_centurion_bounty_board();
    AddonRequestRoutes::Register("BOUNTY", HandleBoardRequest);
}
