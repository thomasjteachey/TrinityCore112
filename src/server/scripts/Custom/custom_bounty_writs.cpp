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

// Vengeance writs. See custom_bounty_writs.h for what they are; this is how.
//
// --- threads ----------------------------------------------------------------
//
// One lock, g_lock, over the recent killers and the writs. A death (map
// thread) records who did it and hands any writ on the dead to whoever earned
// it, with both of them in hand. The board's requests, the gossip and the
// expiry tick run on the world thread. Touching a board (map thread) pays the
// person who touched it. Nothing here calls out while holding g_lock except
// the database's own async queue.
//
// --- the wire (the board addon) ----------------------------------------------
//
//   client -> server  CCGAMEREQ\tBOUNTY:WRITS:0                 the page
//                     CCGAMEREQ\tBOUNTY:WRIT:<guid>:<copper>    post one
//
//   server -> client  BBWO:<1|0>                                writs on this realm
//                     BBWZ:<zoneId>|<guid>,<copper>;...         the price on a zone's bots
//                     BBPW:<guid>|<copper>,<count>              the price on one poster
//                     BBWA:<feePct>,<hours>,<copper>,<copper>...  the page: amounts on offer
//                     BBWK:<guid>,<bot>,<secondsAgo>,<name>;... who killed you lately
//                     BBWL:<guid>,<bot>,<copper>,<count>,<secondsLeft>,<name>;...
//                     BBWC:<copper>,<count>                     earned, not yet collected
//                     BBWE:<count>                              end of the page

#include "custom_bounty_writs.h"

#include "custom_barracks_hardcore.h"
#include "custom_bounty_board.h"

#include "Chat.h"
#include "CombatManager.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // ---------------------------------------------------------------- switches

    bool s_enabled = false;
    uint32 s_windowMinutes = 60;     // how long after a death its killer may be posted
    uint32 s_hours = 24;             // how long a writ stands before it goes back
    uint32 s_feePct = 10;            // what the board keeps of a writ nobody earned
    uint32 s_maxPerPoster = 3;       // open writs one person may have out at once
    bool s_tellTarget = true;        // a person with a price on their head is told
    bool s_gossip = true;
    std::vector<uint32> s_amounts = { 10000, 50000, 250000, 1000000 };

    void LoadConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.BountyBoard.Writs.Enable", false);
        s_windowMinutes = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Writs.WindowMinutes", 60), 1, 24 * 60));
        s_hours = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Writs.Hours", 24), 1, 24 * 14));
        s_feePct = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Writs.FeePct", 10), 0, 100));
        s_maxPerPoster = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Writs.MaxPerPoster", 3), 1, 50));
        s_tellTarget = sConfigMgr->GetBoolDefault("Centurion.BountyBoard.Writs.TellTarget", true);
        s_gossip = sConfigMgr->GetBoolDefault("Centurion.BountyBoard.Writs.Gossip.Enable", true);

        s_amounts.clear();
        std::string const amounts = sConfigMgr->GetStringDefault("Centurion.BountyBoard.Writs.Amounts", "10000,50000,250000,1000000");
        for (std::string_view entry : Trinity::Tokenize(amounts, ',', false))
            if (Optional<uint32> const copper = Trinity::StringTo<uint32>(entry); copper && *copper)
                s_amounts.push_back(*copper);
        std::sort(s_amounts.begin(), s_amounts.end());
        s_amounts.erase(std::unique(s_amounts.begin(), s_amounts.end()), s_amounts.end());
        if (s_amounts.size() > 6)
            s_amounts.resize(6);
        if (s_amounts.empty())
            s_amounts.push_back(10000);
    }

    // ---------------------------------------------------------------- state

    struct Killer
    {
        ObjectGuid Guid;
        std::string Name;
        bool Bot = false;
        time_t At = 0;
    };

    struct Writ
    {
        uint32 Id = 0;
        ObjectGuid Poster;
        std::string PosterName;
        uint32 PosterAccount = 0;
        ObjectGuid Target;
        std::string TargetName;
        bool TargetBot = false;
        uint32 Amount = 0;
        time_t CreatedAt = 0;
        time_t ExpiresAt = 0;
        ObjectGuid ClaimedBy;
        std::string ClaimedByName;
        time_t ClaimedAt = 0;

        bool Open() const { return ClaimedBy.IsEmpty(); }
    };

    std::mutex g_lock;
    std::unordered_map<uint64, std::vector<Killer>> g_killers;   // victim -> recent killers, newest last
    std::vector<Writ> g_writs;
    std::atomic<bool> g_anyWrit{ false };
    uint32 g_nextId = 0;
    bool s_tablesReady = false;

    std::mutex g_outboxLock;
    std::vector<std::pair<ObjectGuid, std::string>> g_outbox;

    constexpr uint32 kSenderPost = 900330;
    constexpr uint32 kSenderList = 900331;

    void UpdateAnyLocked()
    {
        g_anyWrit.store(!g_writs.empty(), std::memory_order_relaxed);
    }

    bool On()
    {
        return s_enabled && s_tablesReady;
    }

    // ---------------------------------------------------------------- words

    void Tell(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(("|cffc8a050[Writ]|r " + text).c_str());
    }

    void TellLater(ObjectGuid who, std::string const& text)
    {
        std::lock_guard<std::mutex> guard(g_outboxLock);
        g_outbox.emplace_back(who, text);
    }

    void Result(Player* player, bool ok, std::string const& text)
    {
        BountyBoard::SendTagged(player, "BBMS", Trinity::StringFormat("{}|{}", ok ? 1 : 0, text));
        Tell(player, text);
    }

    std::string MoneyText(uint32 copper)
    {
        uint32 const gold = copper / 10000;
        uint32 const silver = (copper % 10000) / 100;
        uint32 const rest = copper % 100;
        std::string out;
        if (gold)
            out += Trinity::StringFormat("{}g", gold);
        if (silver)
            out += Trinity::StringFormat("{}{}s", out.empty() ? "" : " ", silver);
        if (rest || out.empty())
            out += Trinity::StringFormat("{}{}c", out.empty() ? "" : " ", rest);
        return out;
    }

    std::string Scrubbed(std::string text)
    {
        for (char& c : text)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';
        return text;
    }

    // ---------------------------------------------------------------- who is who

    bool IsPerson(Player const* player)
    {
        if (!player || player->IsGameMaster() || BarracksHardcore::IsPlayerbot(player))
            return false;
        WorldSession const* session = player->GetSession();
        return session && !session->IsTransientPlayerSession() && !session->IsVirtualSession();
    }

    bool IsWorldBot(Player const* player)
    {
        if (!player || !BarracksHardcore::IsPlayerbot(player))
            return false;
        WorldSession const* session = player->GetSession();
        return session && !session->IsTransientPlayerSession();
    }

    // The open world: no dungeon, battleground or arena, and not the Gurubashi
    // ring, where killing is what everybody came for.
    bool InOpenWorld(Player const* player)
    {
        Map const* map = player ? player->FindMap() : nullptr;
        return map && !map->Instanceable() && !player->pvpInfo.IsInFFAPvPAreaByMap;
    }

    // ---------------------------------------------------------------- persistence

    void SaveWrit(Writ const& writ)
    {
        if (!s_tablesReady)
            return;
        std::string poster = writ.PosterName;
        std::string target = writ.TargetName;
        std::string claimed = writ.ClaimedByName;
        CharacterDatabase.EscapeString(poster);
        CharacterDatabase.EscapeString(target);
        CharacterDatabase.EscapeString(claimed);
        CharacterDatabase.PExecute(
            "REPLACE INTO character_bounty_writ (id, poster, posterName, posterAccount, target, targetName, targetBot, "
            "amount, createdAt, expiresAt, claimedBy, claimedByName, claimedAt) "
            "VALUES ({}, {}, '{}', {}, {}, '{}', {}, {}, {}, {}, {}, '{}', {})",
            writ.Id, writ.Poster.GetCounter(), poster, writ.PosterAccount, writ.Target.GetCounter(), target,
            writ.TargetBot ? 1 : 0, writ.Amount, uint32(writ.CreatedAt), uint32(writ.ExpiresAt),
            writ.ClaimedBy.GetCounter(), claimed, uint32(writ.ClaimedAt));
    }

    void DeleteWrit(uint32 id)
    {
        if (s_tablesReady)
            CharacterDatabase.PExecute("DELETE FROM character_bounty_writ WHERE id = {}", id);
    }

    // ---------------------------------------------------------------- the page

    // A small chunker: rows packed into as few addon whispers as will hold them.
    class Chunker
    {
    public:
        Chunker(Player* viewer, std::string tag, std::string head)
            : _viewer(viewer), _tag(std::move(tag)), _head(std::move(head)) { }

        void Add(std::string const& row)
        {
            if (!_body.empty() && _head.size() + _body.size() + row.size() + 1 > 236)
                Flush();
            _body += row;
            _body += ';';
        }

        void Flush()
        {
            if (_body.empty())
                return;
            BountyBoard::SendTagged(_viewer, _tag, _head + _body);
            _body.clear();
        }

    private:
        Player* _viewer;
        std::string _tag;
        std::string _head;
        std::string _body;
    };

    // Who killed this person inside the window, newest first, one row each.
    std::vector<Killer> RecentKillersLocked(ObjectGuid victim, time_t now)
    {
        std::vector<Killer> out;
        auto const itr = g_killers.find(victim.GetRawValue());
        if (itr == g_killers.end())
            return out;
        for (auto k = itr->second.rbegin(); k != itr->second.rend(); ++k)
            if (now - k->At <= time_t(s_windowMinutes) * MINUTE &&
                std::none_of(out.begin(), out.end(), [&k](Killer const& seen) { return seen.Guid == k->Guid; }))
                out.push_back(*k);
        return out;
    }

    void SendPage(Player* viewer)
    {
        if (!On() || !viewer)
            return;

        time_t const now = GameTime::GetGameTime();

        std::string amounts = Trinity::StringFormat("{},{}", s_feePct, s_hours);
        for (uint32 copper : s_amounts)
            amounts += Trinity::StringFormat(",{}", copper);
        BountyBoard::SendTagged(viewer, "BBWA", amounts);

        struct Row
        {
            ObjectGuid Target;
            std::string Name;
            bool Bot = false;
            uint32 Total = 0;
            uint32 Count = 0;
            time_t Soonest = 0;
        };
        std::vector<Killer> killers;
        std::vector<Row> rows;
        uint32 earned = 0;
        uint32 earnedCount = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            killers = RecentKillersLocked(viewer->GetGUID(), now);

            std::map<uint64, Row> byTarget;
            for (Writ const& writ : g_writs)
            {
                if (!writ.Open())
                {
                    if (writ.ClaimedBy == viewer->GetGUID())
                    {
                        earned += writ.Amount;
                        ++earnedCount;
                    }
                    continue;
                }
                Row& row = byTarget[writ.Target.GetRawValue()];
                row.Target = writ.Target;
                row.Name = writ.TargetName;
                row.Bot = writ.TargetBot;
                row.Total += writ.Amount;
                ++row.Count;
                if (!row.Soonest || writ.ExpiresAt < row.Soonest)
                    row.Soonest = writ.ExpiresAt;
            }
            for (auto& [raw, row] : byTarget)
                rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.Total > b.Total; });
        if (rows.size() > 10)
            rows.resize(10);

        Chunker killerRows(viewer, "BBWK", "");
        for (Killer const& k : killers)
            killerRows.Add(Trinity::StringFormat("{},{},{},{}", k.Guid.GetCounter(), k.Bot ? 1 : 0,
                uint32(std::max<time_t>(0, now - k.At)), Scrubbed(k.Name)));
        killerRows.Flush();

        Chunker writRows(viewer, "BBWL", "");
        for (Row const& row : rows)
            writRows.Add(Trinity::StringFormat("{},{},{},{},{},{}", row.Target.GetCounter(), row.Bot ? 1 : 0, row.Total,
                row.Count, uint32(std::max<time_t>(0, row.Soonest - now)), Scrubbed(row.Name)));
        writRows.Flush();

        BountyBoard::SendTagged(viewer, "BBWC", Trinity::StringFormat("{},{}", earned, earnedCount));
        BountyBoard::SendTagged(viewer, "BBWE", std::to_string(rows.size()));
    }

    // ---------------------------------------------------------------- posting one

    void Post(Player* player, ObjectGuid::LowType targetLow, uint32 amount)
    {
        if (!On())
        {
            Result(player, false, "The board is not taking writs just now.");
            return;
        }
        if (std::find(s_amounts.begin(), s_amounts.end(), amount) == s_amounts.end())
            return;

        time_t const now = GameTime::GetGameTime();
        Killer killer;
        uint32 open = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Killer const& k : RecentKillersLocked(player->GetGUID(), now))
                if (k.Guid.GetCounter() == targetLow)
                {
                    killer = k;
                    break;
                }
            for (Writ const& writ : g_writs)
                if (writ.Open() && writ.Poster == player->GetGUID())
                    ++open;
        }

        if (killer.Guid.IsEmpty())
        {
            Result(player, false, Trinity::StringFormat("You can only put a price on somebody who killed you out in the "
                "world in the last {} minutes.", s_windowMinutes));
            return;
        }
        if (open >= s_maxPerPoster)
        {
            Result(player, false, Trinity::StringFormat("You already have {} writ{} out. The board will hold no more of "
                "yours until one is earned or comes back.", open, open == 1 ? "" : "s"));
            return;
        }
        if (!player->HasEnoughMoney(amount))
        {
            Result(player, false, Trinity::StringFormat("A writ of {} needs {} in your purse.", MoneyText(amount), MoneyText(amount)));
            return;
        }

        player->ModifyMoney(-int64(amount));

        Writ writ;
        writ.Poster = player->GetGUID();
        writ.PosterName = player->GetName();
        writ.PosterAccount = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
        writ.Target = killer.Guid;
        writ.TargetName = killer.Name;
        writ.TargetBot = killer.Bot;
        writ.Amount = amount;
        writ.CreatedAt = now;
        writ.ExpiresAt = now + time_t(s_hours) * HOUR;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            writ.Id = ++g_nextId;
            g_writs.push_back(writ);
            UpdateAnyLocked();
        }
        SaveWrit(writ);

        Result(player, true, Trinity::StringFormat("You put {} on {}'s head. Whoever kills them next collects it at any "
            "board. If nobody has within {} hours, it comes back to you by mail, less the board's {}%.",
            MoneyText(amount), killer.Name, s_hours, s_feePct));

        if (s_tellTarget && !killer.Bot)
            TellLater(killer.Guid, Trinity::StringFormat("Somebody has put {} on your head at a bounty board. Whoever "
                "kills you next collects it.", MoneyText(amount)));

        SendPage(player);

        TC_LOG_INFO("scripts", "Bounty writs: {} put {}c on {} ({}).", player->GetName(), amount, killer.Name,
            killer.Bot ? "bot" : "player");
    }

    // ---------------------------------------------------------------- the tick

    void FlushOutbox()
    {
        std::vector<std::pair<ObjectGuid, std::string>> outbox;
        {
            std::lock_guard<std::mutex> guard(g_outboxLock);
            outbox.swap(g_outbox);
        }
        for (auto const& [who, text] : outbox)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(who))
                Tell(player, text);
    }

    // A writ nobody earned goes back to its poster, less the board's fee.
    void TickExpiry()
    {
        if (!g_anyWrit.load(std::memory_order_relaxed) || !s_tablesReady)
            return;

        time_t const now = GameTime::GetGameTime();
        std::vector<Writ> expired;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (auto itr = g_writs.begin(); itr != g_writs.end(); )
            {
                if (itr->Open() && itr->ExpiresAt <= now)
                {
                    expired.push_back(*itr);
                    itr = g_writs.erase(itr);
                }
                else
                    ++itr;
            }

            // And the killers nobody can post any more.
            for (auto itr = g_killers.begin(); itr != g_killers.end(); )
            {
                std::vector<Killer>& list = itr->second;
                list.erase(std::remove_if(list.begin(), list.end(), [now](Killer const& k)
                    { return now - k.At > time_t(s_windowMinutes) * MINUTE; }), list.end());
                itr = list.empty() ? g_killers.erase(itr) : std::next(itr);
            }
            UpdateAnyLocked();
        }

        for (Writ const& writ : expired)
        {
            uint32 const refund = uint32(uint64(writ.Amount) * (100 - s_feePct) / 100);
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            MailDraft draft("Your writ was not earned", Trinity::StringFormat("Nobody collected the {} you put on {}'s "
                "head. The board keeps {}% for its trouble.", MoneyText(writ.Amount), writ.TargetName, s_feePct));
            if (refund)
                draft.AddMoney(refund);
            draft.SendMailTo(trans, MailReceiver(writ.Poster.GetCounter()), MailSender(MAIL_NORMAL, 0));
            CharacterDatabase.CommitTransaction(trans);
            DeleteWrit(writ.Id);
            TellLater(writ.Poster, Trinity::StringFormat("Your writ on {} ran out unearned. {} is in your mailbox.",
                writ.TargetName, MoneyText(refund)));
            TC_LOG_INFO("scripts", "Bounty writs: the writ on {} by {} ran out; {}c back.", writ.TargetName,
                writ.PosterName, refund);
        }
    }

    // ---------------------------------------------------------------- the gossip

    void GossipList(Player* player)
    {
        time_t const now = GameTime::GetGameTime();
        std::map<std::string, std::pair<uint32, uint32>> byName;   // name -> (total, count)
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Writ const& writ : g_writs)
                if (writ.Open() && writ.ExpiresAt > now)
                {
                    auto& entry = byName[writ.TargetName];
                    entry.first += writ.Amount;
                    ++entry.second;
                }
        }
        if (byName.empty())
        {
            Tell(player, "Nobody has a price on their head just now.");
            return;
        }
        for (auto const& [name, entry] : byName)
            Tell(player, Trinity::StringFormat("{}: {} ({} writ{}).", name, MoneyText(entry.first), entry.second,
                entry.second == 1 ? "" : "s"));
    }
}

namespace BountyWrits
{
    bool Enabled()
    {
        return On();
    }

    void SendBoardState(Player* viewer)
    {
        if (viewer)
            BountyBoard::SendTagged(viewer, "BBWO", On() ? "1" : "0");
    }

    void SendZoneWrits(Player* viewer, uint32 zoneId, std::vector<Player*> const& bots)
    {
        if (!On() || !viewer || !g_anyWrit.load(std::memory_order_relaxed))
            return;

        std::unordered_map<uint64, uint32> prices;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Writ const& writ : g_writs)
                if (writ.Open() && writ.TargetBot)
                    prices[writ.Target.GetRawValue()] += writ.Amount;
        }
        if (prices.empty())
            return;

        Chunker out(viewer, "BBWZ", std::to_string(zoneId) + "|");
        for (Player* bot : bots)
            if (auto const itr = prices.find(bot->GetGUID().GetRawValue()); itr != prices.end())
                out.Add(Trinity::StringFormat("{},{}", bot->GetGUID().GetCounter(), itr->second));
        out.Flush();
    }

    void SendPosterWrit(Player* viewer, Player* bot)
    {
        if (!On() || !viewer || !bot || !g_anyWrit.load(std::memory_order_relaxed))
            return;

        uint32 total = 0;
        uint32 count = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Writ const& writ : g_writs)
                if (writ.Open() && writ.Target == bot->GetGUID())
                {
                    total += writ.Amount;
                    ++count;
                }
        }
        if (count)
            BountyBoard::SendTagged(viewer, "BBPW", Trinity::StringFormat("{}|{},{}", bot->GetGUID().GetCounter(), total, count));
    }

    bool HandleRequest(Player* player, std::string_view verb, std::string_view argument)
    {
        if (verb == "WRITS")
        {
            SendPage(player);
            return true;
        }

        if (verb == "WRIT")
        {
            // <guid>:<copper>
            size_t const colon = argument.find(':');
            if (colon == std::string_view::npos)
                return true;
            Optional<uint32> const guid = Trinity::StringTo<uint32>(argument.substr(0, colon));
            Optional<uint32> const copper = Trinity::StringTo<uint32>(argument.substr(colon + 1));
            if (guid && copper)
                Post(player, *guid, *copper);
            return true;
        }

        return false;
    }

    void OnBoardTouched(Player* player)
    {
        if (!On() || !player || !g_anyWrit.load(std::memory_order_relaxed))
            return;

        std::vector<Writ> earned;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (auto itr = g_writs.begin(); itr != g_writs.end(); )
            {
                if (!itr->Open() && itr->ClaimedBy == player->GetGUID())
                {
                    earned.push_back(*itr);
                    itr = g_writs.erase(itr);
                }
                else
                    ++itr;
            }
            UpdateAnyLocked();
        }
        if (earned.empty())
            return;

        uint64 total = 0;
        for (Writ const& writ : earned)
        {
            total += writ.Amount;
            DeleteWrit(writ.Id);
        }
        player->ModifyMoney(int64(total));
        Tell(player, Trinity::StringFormat("You collect {} writ{}: {}.", earned.size(), earned.size() == 1 ? "" : "s",
            MoneyText(uint32(std::min<uint64>(total, std::numeric_limits<uint32>::max())))));
        TC_LOG_INFO("scripts", "Bounty writs: {} collected {} writ(s) for {}c.", player->GetName(), earned.size(), total);
    }

    void AddGossipOptions(Player* player)
    {
        if (!On() || !s_gossip || !player)
            return;

        std::vector<Killer> killers;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            killers = RecentKillersLocked(player->GetGUID(), GameTime::GetGameTime());
        }
        uint32 shown = 0;
        for (Killer const& k : killers)
        {
            if (shown >= 3)
                break;
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, Trinity::StringFormat("Put {} on {}'s head. (They killed you.)",
                MoneyText(s_amounts.front()), k.Name), kSenderPost, k.Guid.GetCounter());
            ++shown;
        }
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Who has a price on their head?", kSenderList, 0);
    }

    bool OnGossipSelect(Player* player, GameObject* /*board*/, uint32 sender, uint32 action)
    {
        if (!On() || !s_gossip)
            return false;

        switch (sender)
        {
            case kSenderPost:
                CloseGossipMenuFor(player);
                Post(player, action, s_amounts.front());
                return true;
            case kSenderList:
                CloseGossipMenuFor(player);
                GossipList(player);
                return true;
            default:
                return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
class custom_bounty_writs_world : public WorldScript
{
public:
    custom_bounty_writs_world() : WorldScript("custom_bounty_writs_world") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        LoadConfig();

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bounty_writ ("
            "id INT UNSIGNED NOT NULL PRIMARY KEY,"
            "poster INT UNSIGNED NOT NULL DEFAULT 0,"
            "posterName VARCHAR(32) NOT NULL DEFAULT '',"
            "posterAccount INT UNSIGNED NOT NULL DEFAULT 0,"
            "target INT UNSIGNED NOT NULL DEFAULT 0,"
            "targetName VARCHAR(64) NOT NULL DEFAULT '',"
            "targetBot TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "amount INT UNSIGNED NOT NULL DEFAULT 0,"
            "createdAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "expiresAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "claimedBy INT UNSIGNED NOT NULL DEFAULT 0,"
            "claimedByName VARCHAR(32) NOT NULL DEFAULT '',"
            "claimedAt INT UNSIGNED NOT NULL DEFAULT 0"
            ") ENGINE=InnoDB");

        // Checked, not assumed: a query against a missing table takes the realm down.
        s_tablesReady = bool(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_bounty_writ'"));
        if (!s_tablesReady)
        {
            TC_LOG_ERROR("server.loading", "Bounty writs: the writ table is missing - writs are off.");
            return;
        }

        std::lock_guard<std::mutex> guard(g_lock);
        g_writs.clear();
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT id, poster, posterName, posterAccount, target, targetName, targetBot, amount, createdAt, expiresAt, "
                "claimedBy, claimedByName, claimedAt FROM character_bounty_writ"))
        {
            do
            {
                Field* f = result->Fetch();
                Writ writ;
                writ.Id = f[0].GetUInt32();
                writ.Poster = ObjectGuid::Create<HighGuid::Player>(f[1].GetUInt32());
                writ.PosterName = f[2].GetString();
                writ.PosterAccount = f[3].GetUInt32();
                writ.Target = ObjectGuid::Create<HighGuid::Player>(f[4].GetUInt32());
                writ.TargetName = f[5].GetString();
                writ.TargetBot = f[6].GetUInt8() != 0;
                writ.Amount = f[7].GetUInt32();
                writ.CreatedAt = time_t(f[8].GetUInt32());
                writ.ExpiresAt = time_t(f[9].GetUInt32());
                if (uint32 const claimer = f[10].GetUInt32())
                    writ.ClaimedBy = ObjectGuid::Create<HighGuid::Player>(claimer);
                writ.ClaimedByName = f[11].GetString();
                writ.ClaimedAt = time_t(f[12].GetUInt32());
                g_nextId = std::max(g_nextId, writ.Id);
                g_writs.push_back(writ);
            } while (result->NextRow());
        }
        UpdateAnyLocked();

        TC_LOG_INFO("server.loading", "Bounty writs: {} - {} writ(s) on the boards.", On() ? "on" : "OFF", g_writs.size());
    }

    void OnUpdate(uint32 diff) override
    {
        _sinceOutbox += diff;
        _sinceExpiry += diff;
        if (_sinceOutbox >= 1000)
        {
            _sinceOutbox = 0;
            FlushOutbox();
        }
        if (_sinceExpiry >= 60000)
        {
            _sinceExpiry = 0;
            TickExpiry();
        }
    }

private:
    uint32 _sinceOutbox = 0;
    uint32 _sinceExpiry = 0;
};

class custom_bounty_writs_player : public PlayerScript
{
public:
    custom_bounty_writs_player() : PlayerScript("custom_bounty_writs_player") { }

    // Map thread, before the combat references are torn down.
    void OnPlayerJustDied(Player* victim, Unit* killer) override
    {
        if (!On() || !victim)
            return;

        time_t const now = GameTime::GetGameTime();
        Player* by = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;

        // A person killed out in the world: who did it, for the next hour.
        if (IsPerson(victim) && by && by != victim && InOpenWorld(victim) && (IsPerson(by) || IsWorldBot(by)))
        {
            Killer k;
            k.Guid = by->GetGUID();
            k.Name = IsWorldBot(by) ? BountyBoard::NoticeName(by) : by->GetName();
            k.Bot = IsWorldBot(by);
            k.At = now;
            std::lock_guard<std::mutex> guard(g_lock);
            std::vector<Killer>& list = g_killers[victim->GetGUID().GetRawValue()];
            list.push_back(k);
            if (list.size() > 5)
                list.erase(list.begin());
        }

        // A writ's target down: whoever killed them earns it - the killing blow
        // first, then anybody else who was in the fight - unless they are on the
        // poster's own account, or grouped with the target.
        if (!g_anyWrit.load(std::memory_order_relaxed))
            return;

        std::vector<Player*> candidates;
        std::unordered_set<uint64> seen;
        auto const consider = [&](Unit* unit)
        {
            Player* person = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (person && person != victim && IsPerson(person) && person->IsInMap(victim) &&
                seen.insert(person->GetGUID().GetRawValue()).second)
                candidates.push_back(person);
        };
        consider(killer);
        CombatManager const& combat = victim->GetCombatManager();
        for (auto const& reference : combat.GetPvPCombatRefs())
            consider(reference.second->GetOther(victim));
        if (candidates.empty())
            return;

        struct Earned
        {
            Player* By;
            uint32 Amount;
            ObjectGuid Poster;
            std::string Target;
        };
        std::vector<Earned> earned;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Writ& writ : g_writs)
            {
                if (!writ.Open() || writ.Target != victim->GetGUID())
                    continue;
                for (Player* candidate : candidates)
                {
                    uint32 const account = candidate->GetSession() ? candidate->GetSession()->GetAccountId() : 0;
                    if (account == writ.PosterAccount || candidate->IsInSameGroupWith(victim))
                        continue;
                    writ.ClaimedBy = candidate->GetGUID();
                    writ.ClaimedByName = candidate->GetName();
                    writ.ClaimedAt = now;
                    SaveWrit(writ);
                    earned.push_back({ candidate, writ.Amount, writ.Poster, writ.TargetName });
                    break;
                }
            }
        }

        for (Earned const& e : earned)
        {
            Tell(e.By, Trinity::StringFormat("You earned the writ on {}: {}. Collect it at any bounty board.", e.Target,
                MoneyText(e.Amount)));
            TellLater(e.Poster, Trinity::StringFormat("Your writ on {} was earned by {}.", e.Target, e.By->GetName()));
            TC_LOG_INFO("scripts", "Bounty writs: {} earned {}c on {}.", e.By->GetName(), e.Amount, e.Target);
        }
    }
};

void AddSC_custom_bounty_writs()
{
    LoadConfig();
    new custom_bounty_writs_world();
    new custom_bounty_writs_player();
}
