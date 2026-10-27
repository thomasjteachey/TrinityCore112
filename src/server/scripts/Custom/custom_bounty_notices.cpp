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

// Bounty-board notices. See custom_bounty_notices.h for what they are; this is
// how they work.
//
// --- threads ----------------------------------------------------------------
//
// Almost everything runs on the WORLD thread: the board's addon requests and
// gossip choices (both PROCESS_THREADUNSAFE), login and logout, the Mok'gora
// answers, and the once-a-second tick, which World::Update runs after every
// map has finished its own update - so it may read and change any player.
//
// Three hooks run on a MAP thread and touch only the players they were handed:
// a death (queued for the tick, because the people owed credit for it may be
// anywhere), the end of a duel (both duelists stand on that map), and the
// board's own click (the person who touched it).
//
// Every container below sits behind one lock, g_lock, and nothing that takes
// another lock ever calls back into this file - PairStance and the playerbot
// manager have locks of their own and know nothing of notices.
//
// --- the wire ---------------------------------------------------------------
//
// Added to the board addon's protocol (custom_bounty_board.cpp has the rest):
//
//   client -> server  CCGAMEREQ\tBOUNTY:TAKE:<guid>:<H|M|R|P>  take a notice (P: raise a posse)
//                     CCGAMEREQ\tBOUNTY:DROP:<H|M|R|P>:<slot>  tear one up
//                     CCGAMEREQ\tBOUNTY:LEDGER:0               ledger + notices
//                     CCGAMEREQ\tBOUNTY:SHOP:0                 the Quartermaster's list
//                     CCGAMEREQ\tBOUNTY:BUY:<itemId>           spend seals on it
//
//   server -> client  BBRX:<zoneId>|<guid>,<flags>,<temper>,<coin>,<area>;...
//                         flags 1 yours, 2 carries your gear, 4 a Mok'gora is out
//                         temper/coin are -1 and area empty below the rank
//                     BBMW:<zoneId>|<guid>,<collectedBy>        the zone's Most Wanted today
//                     BBPO:<guid>|<slot>,<itemId>,<owner>;...  taken from a body
//                     BBPX:<guid>|<hunt>,<mokgora>,<recovery>  may you take it
//                     BBPP:<guid>|<posse>                      ...as a posse
//                     BBMP:<guid>|<multiplier x10>,<collectedBy> it is today's Most Wanted
//                     BBLG:<rank>,<collected>,<next>,<seals>,<hunts>,<mokgora>,
//                          <recovered>,<zonesDone>,<zonesTotal>,0,<maxNotices>,
//                          0,0,0,<features>,<posses>,<rank1>..<rank4> (the zeros were pardons)
//                     BBNA:<kind>,<slot>,<guid>,<level>,<class>,<zoneId>,
//                          <complete>,<expiresIn>,<name>;...
//                     BBNE:<count>
//                     BBSH:<itemId>,<price>,<count>;...         the Quartermaster's list
//                     BBSE:<seals>
//                     BBMS:<1 ok, 0 refused>|<text>

#include "custom_bounty_notices.h"
#include "custom_nemesis.h"

#include "custom_barracks_hardcore.h"
#include "custom_bounty.h"
#include "custom_bounty_board.h"
#include "custom_notoriety.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"

#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CombatManager.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "Miscellaneous/Mokgora.h"
#include "Miscellaneous/PairStance.h"
#include "Miscellaneous/TournamentMode.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    enum class Kind : uint8
    {
        Hunt     = 1,
        Mokgora  = 2,
        Recovery = 3,
        Posse    = 4,   // a party's notice on a zone's Warchief
    };

    constexpr uint8 kSlots = 2;
    constexpr uint8 kTopRank = 4;

    char const* const RankNames[kTopRank + 1] = { "Unranked", "Tracker", "Bloodhound", "Manhunter", "Headsman" };

    // ---------------------------------------------------------------- config

    // Off unless the realm's config says otherwise: the quests, items and titles
    // a realm needs are data, and a realm that has the build without them must
    // not offer notices it cannot give.
    bool s_enabled = false;
    uint8 s_minLevel = 10;
    // [kind - 1][slot]. Two of each, so a Headsman can carry two at once and
    // somebody robbed by two bots can go after both.
    uint32 s_questIds[4][kSlots] = { { 60010, 60013 }, { 60011, 60014 }, { 60012, 60015 }, { 60016, 60017 } };
    uint32 s_noticeMinutes = 120;
    uint32 s_hintSeconds = 180;
    uint32 s_quarryGoneMinutes = 20;
    // No notice on anybody more than this many levels above you, and none on
    // anybody grey to you - a notice is a fight, not a chore.
    uint32 s_levelsAbove = 5;
    uint32 s_feudMinutes = 10;
    // A hunted bot this aggressive or more calls for help; twice as aggressive
    // past the second line.
    uint32 s_escortTemper = 60;
    uint32 s_escortTemperTwo = 80;
    uint32 s_escortMax = 2;
    float s_escortReachYards = 400.0f;
    // Somebody grouped with whoever landed the blow, and standing this close to
    // it, shares the credit.
    float s_creditYards = 100.0f;

    uint32 s_moneyBase = 2000;
    uint32 s_moneyPerLevel = 150;
    uint32 s_moneyPerKill = 300;
    uint32 s_killCap = 20;
    float s_huntBubbles = 2.0f;
    float s_mokgoraBubbles = 3.0f;
    float s_recoveryBubbles = 0.0f;
    float s_mokgoraMoney = 1.5f;
    float s_recoveryMoney = 0.5f;
    // At the level cap there is no experience to give, so the coin grows.
    float s_cappedMoney = 1.5f;
    uint32 s_sealsHunt = 1;
    uint32 s_sealsMokgora = 2;
    uint32 s_sealsRecovery = 1;

    std::vector<uint32> s_rankAt = { 5, 15, 35, 75 };

    uint32 s_keepsakeItem = 4486;
    uint32 s_tabardItem = 3557;
    uint32 s_headsmanTitle = 210;
    uint32 s_relentlessTitle = 211;

    uint32 s_sealsPosse = 3;
    float s_posseMoney = 2.0f;
    float s_posseBubbles = 3.0f;
    uint32 s_posseMinGroup = 2;

    float s_mostWantedMultiplier = 3.0f;
    uint32 s_mostWantedResetHour = 6;
    uint32 s_mostWantedMissingMinutes = 30;

    // The Quartermaster: what seals buy. Centurion.BountyBoard.Notices.Shop.Items,
    // "itemId:price[:count]" comma-separated.
    struct ShopItem
    {
        uint32 ItemId = 0;
        uint32 Price = 0;
        uint32 Count = 1;
    };
    std::vector<ShopItem> s_shop;
    constexpr char const* kDefaultShop = "929:1:5,1710:1:5,3928:2:5,13446:3:5,5634:2,3387:4,13452:4,13510:15,"
        "8529:2:5,8485:10,11023:10,23192:25,1973:25,19872:150";

    // Every part of the feature can be switched off on its own, live, with
    // .reload config. A part switched off stops at once: its notices freeze
    // (still in the log, nothing hunts, nothing expires, nothing pays), what it
    // put in force on the bots - truces, feuds, errands, guards - is taken back,
    // and switching it on again puts all of that back for whoever is online.
    struct Switches
    {
        bool Hunt = true;               // hunt notices can be taken and run
        bool HuntRequireWarMode = true; // ...by War Mode players only
        bool HuntEscorts = true;        // an aggressive quarry calls friends
        bool Mokgora = true;            // Mok'gora notices
        bool MokgoraTruce = true;       // the challenger is not red on its way
        bool MokgoraFeud = true;        // a refusal turns the bot on you
        bool Recovery = true;           // recovery notices
        bool RecoveryFeud = true;       // ...make the thief hostile whatever War Mode says
        bool ShowTakenFrom = true;      // the board marks gear taken off a body, and whose
        bool Hints = true;              // "word on the road" while hunting
        bool Ledger = true;             // ranks advance; off freezes them and their perks
        bool PerkArea = true;           // Tracker reads where in a zone
        bool PerkTemper = true;         // Bloodhound reads temperament
        bool PerkCoin = true;           // Manhunter reads coin
        bool PerkTwoNotices = true;     // Headsman carries two
        bool Keepsakes = true;          // the readable poster on hand-in
        bool KeepsakesByMail = true;    // ...sent to the mailbox, never into the bags
        bool Tabard = true;             // the Headsman's tabard
        bool Titles = true;             // Headsman and the Relentless
        bool Posse = true;              // posse notices on a zone's Warchief
        bool PosseFeud = true;          // ...that the whole party may fight, War Mode or not
        bool MostWanted = true;         // a pinned bot a day in every zone, paying more
        bool Shop = true;               // seals buy things from the Quartermaster
        bool RewardMoney = true;
        bool RewardExperience = true;
        bool RewardSeals = true;
        bool Gossip = true;             // the no-addon board menu
        bool Stances = true;            // the truce/feud rule in the core (PairStance)
        bool GroupStances = true;       // ...covering the holder's whole party or raid
        bool Dispatch = true;           // a bot on a notice errand may be landed near you
    };
    Switches s_on;

    // Gossip senders for the no-addon menu. Board-specific, well clear of
    // GOSSIP_SENDER_MAIN and anything a stock menu uses.
    constexpr uint32 kSenderMenu = 900310;
    constexpr uint32 kSenderTakeHunt = 900311;
    constexpr uint32 kSenderTakeMokgora = 900312;
    constexpr uint32 kSenderTakeRecovery = 900313;
    constexpr uint32 kSenderLedger = 900314;
    constexpr uint32 kSenderShop = 900315;
    constexpr uint32 kSenderBuy = 900316;
    constexpr uint32 kSenderTakePosse = 900317;

    void LoadNoticeConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.BountyBoard.Notices.Enable", false);
        s_minLevel = uint8(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.MinLevel", 10), 1, 80));

        static char const* const kindKeys[4] = { "Hunt", "Mokgora", "Recovery", "Posse" };
        uint32 const defaults[4][kSlots] = { { 60010, 60013 }, { 60011, 60014 }, { 60012, 60015 }, { 60016, 60017 } };
        for (uint8 k = 0; k < 4; ++k)
            for (uint8 s = 0; s < kSlots; ++s)
                s_questIds[k][s] = uint32(std::max(0, sConfigMgr->GetIntDefault(
                    Trinity::StringFormat("Centurion.BountyBoard.Notices.{}Quest{}", kindKeys[k], s + 1), int32(defaults[k][s]))));

        s_noticeMinutes = uint32(std::max(5, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.Minutes", 120)));
        s_hintSeconds = uint32(std::max(30, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.HintSeconds", 180)));
        s_quarryGoneMinutes = uint32(std::max(1, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.QuarryGoneMinutes", 20)));
        s_levelsAbove = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.LevelsAbove", 5)));
        s_feudMinutes = uint32(std::max(1, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.FeudMinutes", 10)));
        s_escortTemper = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.EscortTemper", 60), 0, 101));
        s_escortTemperTwo = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.EscortTemperTwo", 80), 0, 101));
        s_escortMax = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.EscortMax", 2), 0, 5));
        s_escortReachYards = std::max(50.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.EscortReachYards", 400.0f));
        s_creditYards = std::max(10.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.CreditYards", 100.0f));

        s_moneyBase = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.MoneyBase", 2000)));
        s_moneyPerLevel = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.MoneyPerLevel", 150)));
        s_moneyPerKill = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.MoneyPerKill", 300)));
        s_killCap = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.KillCap", 20)));
        s_huntBubbles = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.HuntBubbles", 2.0f));
        s_mokgoraBubbles = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.MokgoraBubbles", 3.0f));
        s_recoveryBubbles = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.RecoveryBubbles", 0.0f));
        s_mokgoraMoney = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.MokgoraMoney", 1.5f));
        s_recoveryMoney = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.RecoveryMoney", 0.5f));
        s_cappedMoney = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.CappedMoney", 1.5f));
        s_sealsHunt = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.SealsHunt", 1)));
        s_sealsMokgora = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.SealsMokgora", 2)));
        s_sealsRecovery = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.SealsRecovery", 1)));
        s_sealsPosse = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.SealsPosse", 3)));
        s_posseMoney = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.PosseMoney", 2.0f));
        s_posseBubbles = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.PosseBubbles", 3.0f));
        s_posseMinGroup = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.Posse.MinGroup", 2), 1, 40));
        s_mostWantedMultiplier = std::clamp(sConfigMgr->GetFloatDefault("Centurion.BountyBoard.Notices.MostWanted.Multiplier", 3.0f), 1.0f, 20.0f);
        s_mostWantedResetHour = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.MostWanted.ResetHour", 6), 0, 23));
        s_mostWantedMissingMinutes = uint32(std::max(5, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.MostWanted.MissingMinutes", 30)));

        s_shop.clear();
        std::string const shop = sConfigMgr->GetStringDefault("Centurion.BountyBoard.Notices.Shop.Items", kDefaultShop);
        for (std::string_view entry : Trinity::Tokenize(shop, ',', false))
        {
            std::vector<std::string_view> const parts = Trinity::Tokenize(entry, ':', false);
            if (parts.size() < 2)
                continue;
            Optional<uint32> const itemId = Trinity::StringTo<uint32>(parts[0]);
            Optional<uint32> const price = Trinity::StringTo<uint32>(parts[1]);
            Optional<uint32> const count = parts.size() > 2 ? Trinity::StringTo<uint32>(parts[2]) : Optional<uint32>(1);
            if (itemId && *itemId && price && count && *count)
                s_shop.push_back({ *itemId, *price, *count });
        }

        s_rankAt.clear();
        std::string const rankAt = sConfigMgr->GetStringDefault("Centurion.BountyBoard.Notices.RankAt", "5,15,35,75");
        for (std::string_view piece : Trinity::Tokenize(rankAt, ',', false))
            if (Optional<uint32> value = Trinity::StringTo<uint32>(piece))
                s_rankAt.push_back(*value);
        std::sort(s_rankAt.begin(), s_rankAt.end());
        if (s_rankAt.size() > kTopRank)
            s_rankAt.resize(kTopRank);

        s_keepsakeItem = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.KeepsakeItem", 4486)));
        s_tabardItem = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.TabardItem", 3557)));
        s_headsmanTitle = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.HeadsmanTitle", 210)));
        s_relentlessTitle = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.BountyBoard.Notices.RelentlessTitle", 211)));


        auto const on = [](char const* key) { return sConfigMgr->GetBoolDefault(
            Trinity::StringFormat("Centurion.BountyBoard.Notices.{}", key), true); };
        s_on.Hunt = on("Hunt.Enable");
        s_on.HuntRequireWarMode = on("Hunt.RequireWarMode");
        s_on.HuntEscorts = on("Hunt.Escorts.Enable");
        s_on.Mokgora = on("Mokgora.Enable");
        s_on.MokgoraTruce = on("Mokgora.Truce.Enable");
        s_on.MokgoraFeud = on("Mokgora.FeudOnRefusal.Enable");
        s_on.Recovery = on("Recovery.Enable");
        s_on.RecoveryFeud = on("Recovery.Feud.Enable");
        s_on.ShowTakenFrom = on("ShowTakenFrom.Enable");
        s_on.Hints = on("Hints.Enable");
        s_on.Ledger = on("Ledger.Enable");
        s_on.PerkArea = on("Ledger.Perk.Area.Enable");
        s_on.PerkTemper = on("Ledger.Perk.Temper.Enable");
        s_on.PerkCoin = on("Ledger.Perk.Coin.Enable");
        s_on.PerkTwoNotices = on("Ledger.Perk.TwoNotices.Enable");
        s_on.Keepsakes = on("Keepsakes.Enable");
        s_on.KeepsakesByMail = on("Keepsakes.Mail.Enable");
        s_on.Tabard = on("Tabard.Enable");
        s_on.Titles = on("Titles.Enable");
        s_on.Posse = on("Posse.Enable");
        s_on.PosseFeud = on("Posse.Feud.Enable");
        s_on.MostWanted = on("MostWanted.Enable");
        s_on.Shop = on("Shop.Enable");
        s_on.RewardMoney = on("Reward.Money.Enable");
        s_on.RewardExperience = on("Reward.Experience.Enable");
        s_on.RewardSeals = on("Reward.Seals.Enable");
        s_on.Gossip = on("Gossip.Enable");
        s_on.Stances = on("Stances.Enable");
        s_on.GroupStances = on("Group.Stances.Enable");
        s_on.Dispatch = on("Dispatch.Enable");
    }

    // A kind of notice is running: the whole feature, and that kind.
    bool KindOn(Kind kind)
    {
        if (!s_enabled)
            return false;
        switch (kind)
        {
            case Kind::Hunt:     return s_on.Hunt;
            case Kind::Mokgora:  return s_on.Mokgora;
            case Kind::Recovery: return s_on.Recovery;
            default:             return s_on.Posse;
        }
    }

    // The Ledger rank as its perks read it: with the Ledger switched off nobody
    // reads anything extra off the board.
    bool PerkOn(uint8 rank, uint8 needed, bool perk)
    {
        return s_on.Ledger && perk && rank >= needed;
    }

    // BBLG's last field: which parts the addon should draw.
    uint32 FeatureBits()
    {
        // 2 was pardons, gone: an older addon reads it as "the board sells none".
        return (s_on.Ledger ? 1u : 0u) | (s_on.PerkArea ? 4u : 0u) |
            (s_on.PerkTemper ? 8u : 0u) | (s_on.PerkCoin ? 16u : 0u) | (s_on.PerkTwoNotices ? 32u : 0u) |
            (s_on.Titles ? 64u : 0u) | (s_on.ShowTakenFrom ? 128u : 0u) | (s_on.Shop ? 256u : 0u) |
            (s_on.MostWanted ? 1024u : 0u) | (KindOn(Kind::Posse) ? 2048u : 0u);
    }

    uint32 QuestFor(Kind kind, uint8 slot)
    {
        return slot < kSlots ? s_questIds[uint8(kind) - 1][slot] : 0;
    }

    char KindCode(Kind kind)
    {
        switch (kind)
        {
            case Kind::Hunt:     return 'H';
            case Kind::Mokgora:  return 'M';
            case Kind::Recovery: return 'R';
            default:             return 'P';
        }
    }

    bool KindFromCode(char code, Kind& out)
    {
        switch (code)
        {
            case 'H': out = Kind::Hunt;     return true;
            case 'M': out = Kind::Mokgora;  return true;
            case 'R': out = Kind::Recovery; return true;
            case 'P': out = Kind::Posse;    return true;
            default:  return false;
        }
    }

    // ---------------------------------------------------------------- state

    struct Notice
    {
        uint32 Id = 0;                 // in-memory handle, never persisted
        ObjectGuid Holder;
        Kind Type = Kind::Hunt;
        uint8 Slot = 0;
        ObjectGuid Quarry;
        std::string QuarryName;
        uint8 QuarryLevel = 0;
        uint8 QuarryClass = 0;
        uint8 QuarryRace = 0;
        uint32 QuarryKills = 0;        // lifetime player kills when it was taken
        uint32 ZoneId = 0;             // where the quarry was posted
        time_t TakenAt = 0;
        time_t ExpiresAt = 0;
        bool Complete = false;

        // Not persisted: rebuilt at login.
        bool Live = false;             // holder online, holds/stances in force
        time_t NextHintAt = 0;
        uint32 LastHintZone = 0;
        time_t QuarryMissingSince = 0;
        time_t NextGearCheckAt = 0;
        bool Challenged = false;
        bool Dueling = false;
        time_t NextChallengeTryAt = 0;
        time_t NextNagAt = 0;
        time_t WaitingSince = 0;       // a challenger standing there, not yet able to ask
        bool WaitLogged = false;
        std::vector<ObjectGuid> Escorts;
    };

    struct Ledger
    {
        uint32 Collected = 0;
        uint32 Hunts = 0;
        uint32 Mokgora = 0;
        uint32 Recovered = 0;
        uint32 Seals = 0;
        uint32 Posses = 0;
        std::set<uint32> Zones;
    };

    // A refused Mok'gora: the bot comes after the person, whatever War Mode
    // says, until one of them dies or the time runs out.
    struct Feud
    {
        ObjectGuid Bot;
        ObjectGuid Human;
        time_t Until = 0;
    };

    struct DeathEvent
    {
        ObjectGuid Victim;
        std::vector<ObjectGuid> Participants;
        uint32 MapId = 0;
        float X = 0.0f;
        float Y = 0.0f;
    };

    std::mutex g_lock;
    std::vector<Notice> g_notices;
    std::unordered_map<uint64, Ledger> g_ledgers;
    std::vector<Feud> g_feuds;
    std::vector<DeathEvent> g_deaths;
    uint32 g_nextId = 0;
    std::atomic<bool> g_any{ false };
    bool s_tablesReady = false;

    void UpdateAnyLocked()
    {
        g_any.store(!g_notices.empty() || !g_feuds.empty() || !g_deaths.empty(), std::memory_order_relaxed);
    }

    Notice* FindLocked(uint32 id)
    {
        for (Notice& notice : g_notices)
            if (notice.Id == id)
                return &notice;
        return nullptr;
    }

    // ---------------------------------------------------------------- words

    std::string Banner(std::string const& text)
    {
        return "|cffd4a017[Bounty Board]|r " + text;
    }

    void Tell(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(Banner(text).c_str());
    }

    // The board addon reads this one; everybody reads the chat line.
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
            out += Trinity::StringFormat("{}g ", gold);
        if (silver || gold)
            out += Trinity::StringFormat("{}s ", silver);
        out += Trinity::StringFormat("{}c", rest);
        return out;
    }

    std::string AreaName(uint32 areaId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        char const* name = area ? area->AreaName[LOCALE_enUS] : nullptr;
        return (name && *name) ? std::string(name) : std::string();
    }

    std::string ClassWord(uint8 cls)
    {
        ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(cls);
        return (entry && entry->Name[LOCALE_enUS]) ? std::string(entry->Name[LOCALE_enUS]) : std::string();
    }

    std::string RaceWord(uint8 race)
    {
        ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race);
        return (entry && entry->Name[LOCALE_enUS]) ? std::string(entry->Name[LOCALE_enUS]) : std::string();
    }

    // Commas, semicolons and pipes separate records on the wire.
    std::string Scrubbed(std::string text)
    {
        for (char& c : text)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';
        return text;
    }

    // Rows packed into as few whispers as will hold them (see the board's own).
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
            BountyBoard::SendTagged(_viewer, _tag, _head + _body);
            _body.clear();
        }

    private:
        static constexpr size_t MaxPayload = 236;
        Player* _viewer;
        std::string _tag;
        std::string _head;
        std::string _body;
    };

    // ---------------------------------------------------------------- ledger

    uint8 RankFor(uint32 collected)
    {
        uint8 rank = 0;
        for (uint32 threshold : s_rankAt)
            if (collected >= threshold)
                ++rank;
        return std::min(rank, kTopRank);
    }

    uint32 NextRankAt(uint32 collected)
    {
        for (uint32 threshold : s_rankAt)
            if (collected < threshold)
                return threshold;
        return 0;
    }

    uint32 MaxNotices(uint8 rank)
    {
        return PerkOn(rank, kTopRank, s_on.PerkTwoNotices) ? 2 : 1;
    }

    Ledger LedgerCopy(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto const itr = g_ledgers.find(guid.GetRawValue());
        return itr != g_ledgers.end() ? itr->second : Ledger();
    }

    uint8 RankOf(ObjectGuid guid)
    {
        return RankFor(LedgerCopy(guid).Collected);
    }

    void SaveLedger(ObjectGuid guid, Ledger const& ledger)
    {
        if (!s_tablesReady)
            return;

        CharacterDatabase.PExecute(
            "REPLACE INTO character_bounty_ledger (guid, collected, hunts, mokgora, recovered, seals, pardonAt, posses) "
            "VALUES ({}, {}, {}, {}, {}, {}, 0, {})",
            guid.GetCounter(), ledger.Collected, ledger.Hunts, ledger.Mokgora, ledger.Recovered, ledger.Seals,
            ledger.Posses);
    }

    void LoadLedger(Player* player)
    {
        if (!s_tablesReady)
            return;

        Ledger ledger;
        ObjectGuid::LowType const low = player->GetGUID().GetCounter();
        if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT collected, hunts, mokgora, recovered, seals, posses FROM character_bounty_ledger WHERE guid = {}", low))
        {
            Field* fields = result->Fetch();
            ledger.Collected = fields[0].GetUInt32();
            ledger.Hunts = fields[1].GetUInt32();
            ledger.Mokgora = fields[2].GetUInt32();
            ledger.Recovered = fields[3].GetUInt32();
            ledger.Seals = fields[4].GetUInt32();
            ledger.Posses = fields[5].GetUInt32();
        }

        if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT zoneId FROM character_bounty_ledger_zone WHERE guid = {}", low))
        {
            do
                ledger.Zones.insert(result->Fetch()[0].GetUInt32());
            while (result->NextRow());
        }

        std::lock_guard<std::mutex> guard(g_lock);
        g_ledgers[player->GetGUID().GetRawValue()] = std::move(ledger);
    }

    std::pair<uint32, uint32> ZoneProgress(Ledger const& ledger)
    {
        std::vector<uint32> const zones = BountyBoard::BoardZones();
        uint32 done = 0;
        for (uint32 zone : zones)
            if (ledger.Zones.count(zone))
                ++done;
        return { done, uint32(zones.size()) };
    }

    void GrantTitle(Player* player, uint32 titleId, char const* what)
    {
        if (!titleId)
            return;

        CharTitlesEntry const* title = sCharTitlesStore.LookupEntry(titleId);
        if (!title)
        {
            TC_LOG_ERROR("scripts", "Bounty notices: title {} ({}) is not in CharTitles.dbc - nothing granted to {}.",
                titleId, what, player->GetName());
            return;
        }

        if (player->HasTitle(title))
            return;

        player->SetTitle(title);
        Tell(player, Trinity::StringFormat("You have earned a title: {}.", what));
    }

    // Owed once each: the title by rank and by every zone. Asked at login too,
    // so a title that reached the realm's data after it was earned still lands.
    void GrantEarnedTitles(Player* player, Ledger const& ledger)
    {
        if (!s_enabled || !s_on.Titles || !s_on.Ledger)
            return;

        if (RankFor(ledger.Collected) >= kTopRank)
            GrantTitle(player, s_headsmanTitle, "Headsman");

        auto const [done, total] = ZoneProgress(ledger);
        if (total && done >= total)
            GrantTitle(player, s_relentlessTitle, "the Relentless");
    }

    void MailItem(Player* player, Item* item, GameObject* from, std::string const& subject, std::string const& body)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->SaveToDB(trans);
        MailSender const sender = from ? MailSender(from) : MailSender(MAIL_NORMAL, 0);
        MailDraft(subject, body).AddItem(item).SendMailTo(trans, MailReceiver(player), sender);
        CharacterDatabase.CommitTransaction(trans);
    }

    void GiveOrMail(Player* player, Item* item, GameObject* from, char const* subject, char const* body)
    {
        ItemPosCountVec dest;
        if (player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
        {
            player->StoreItem(dest, item, true);
            player->SendNewItem(item, 1, true, false);
            return;
        }

        MailItem(player, item, from, subject, body);
        Tell(player, "Your bags are full. It has been sent to your mailbox.");
    }

    // ---------------------------------------------------------------- persistence

    void SaveNotice(Notice const& notice)
    {
        if (!s_tablesReady)
            return;

        std::string name = notice.QuarryName;
        CharacterDatabase.EscapeString(name);
        CharacterDatabase.PExecute(
            "REPLACE INTO character_bounty_notice (guid, kind, slot, quarry, quarryName, quarryLevel, quarryClass, "
            "quarryRace, quarryKills, zoneId, takenAt, expiresAt, complete) "
            "VALUES ({}, {}, {}, {}, '{}', {}, {}, {}, {}, {}, {}, {}, {})",
            notice.Holder.GetCounter(), uint32(notice.Type), uint32(notice.Slot), notice.Quarry.GetCounter(), name,
            uint32(notice.QuarryLevel), uint32(notice.QuarryClass), uint32(notice.QuarryRace), notice.QuarryKills,
            notice.ZoneId, uint32(notice.TakenAt), uint32(notice.ExpiresAt), notice.Complete ? 1 : 0);
    }

    void DeleteNotice(Notice const& notice)
    {
        if (!s_tablesReady)
            return;

        CharacterDatabase.PExecute("DELETE FROM character_bounty_notice WHERE guid = {} AND kind = {} AND slot = {}",
            notice.Holder.GetCounter(), uint32(notice.Type), uint32(notice.Slot));
    }

    // ---------------------------------------------------------------- holds

    bool QuarryNamedElsewhereLocked(ObjectGuid bot, uint32 exceptId)
    {
        for (Notice const& other : g_notices)
            if (other.Id != exceptId && other.Live && !other.Complete && other.Quarry == bot && KindOn(other.Type))
                return true;
        for (Feud const& feud : g_feuds)
            if (feud.Bot == bot)
                return true;
        return false;
    }

    bool InFeudLocked(ObjectGuid bot, ObjectGuid human)
    {
        for (Feud const& feud : g_feuds)
            if (feud.Bot == bot && feud.Human == human)
                return true;
        return false;
    }

    // A notice's stance, shared with the holder's party or raid when the switch
    // says so: friends can help with a feud whatever their War Mode, and nobody
    // in the party lands the first blow on a challenger walking up in truce.
    void PutStance(ObjectGuid bot, ObjectGuid human, PairStance::Stance stance)
    {
        if (s_on.GroupStances)
            PairStance::SetShared(bot, human, stance);
        else
            PairStance::Set(bot, human, stance);
    }

    // Make the bots match the switches for one live notice: whatever it should
    // have in force right now - the quarry kept in the world, a Mok'gora
    // challenger's truce and walk, a recovery's feud, a hunted bot's guards -
    // is put in force, and whatever it should not have is taken out. Run when a
    // notice goes live and again after every .reload config, so it has to be
    // safe to run any number of times.
    void ReconcileLocked(Notice& notice)
    {
        if (!notice.Live)
            return;

        bool const on = KindOn(notice.Type) && !notice.Complete;

        if (on)
            playerbot::PveManager::SetNoticeHeld(notice.Quarry, true);
        else if (!QuarryNamedElsewhereLocked(notice.Quarry, notice.Id))
            playerbot::PveManager::SetNoticeHeld(notice.Quarry, false);

        // Guards stay only with a running hunt that still wants them. They are
        // not called again when switched back on: a bot calls for help when it
        // hears it is wanted, not when the config changes.
        if (!(on && notice.Type == Kind::Hunt && s_on.HuntEscorts))
        {
            for (ObjectGuid escort : notice.Escorts)
            {
                playerbot::PveManager::ClearNoticeErrand(escort);
                playerbot::PveManager::SetNoticeHeld(escort, false);
            }
            notice.Escorts.clear();
        }

        // A feud owns the pair's stance and the bot's errand.
        if (InFeudLocked(notice.Quarry, notice.Holder))
            return;

        switch (notice.Type)
        {
            case Kind::Mokgora:
                if (on && s_on.MokgoraTruce && !notice.Dueling)
                    PutStance(notice.Quarry, notice.Holder, PairStance::Stance::Truce);
                else
                    PairStance::Clear(notice.Quarry, notice.Holder);

                if (on && !notice.Dueling)
                    playerbot::PveManager::SetNoticeErrand(notice.Quarry,
                        playerbot::PveManager::NoticeErrandKind::Approach, notice.Holder);
                else
                    playerbot::PveManager::ClearNoticeErrand(notice.Quarry);
                break;
            case Kind::Recovery:
                if (on && s_on.RecoveryFeud)
                    PutStance(notice.Quarry, notice.Holder, PairStance::Stance::Feud);
                else
                    PairStance::Clear(notice.Quarry, notice.Holder);
                break;
            case Kind::Hunt:
                // With hunts open to anybody, somebody without War Mode could
                // not land a blow on what they took the notice for, so the pair
                // feuds. With War Mode required there is nothing to add.
                if (on && !s_on.HuntRequireWarMode)
                    PutStance(notice.Quarry, notice.Holder, PairStance::Stance::Feud);
                else
                    PairStance::Clear(notice.Quarry, notice.Holder);
                break;
            case Kind::Posse:
                // The whole party may fight the Warchief, War Mode or not (and
                // it them): shared with the holder's group, and every member of
                // a posse holds a notice of their own anyway.
                if (on && s_on.PosseFeud)
                    PutStance(notice.Quarry, notice.Holder, PairStance::Stance::Feud);
                else
                    PairStance::Clear(notice.Quarry, notice.Holder);
                break;
        }
    }

    void ApplyLiveLocked(Notice& notice)
    {
        notice.Live = true;
        ReconcileLocked(notice);
    }

    // And everything it took out again: the holder logged out, or the notice
    // ended. The quarry stays held while anybody else's notice or a feud names
    // it, and a stance stays when a feud has just replaced it.
    void ReleaseLiveLocked(Notice& notice)
    {
        if (!notice.Live)
            return;
        notice.Live = false;

        for (ObjectGuid escort : notice.Escorts)
        {
            playerbot::PveManager::ClearNoticeErrand(escort);
            playerbot::PveManager::SetNoticeHeld(escort, false);
        }
        notice.Escorts.clear();

        bool const feud = InFeudLocked(notice.Quarry, notice.Holder);

        if (notice.Type == Kind::Mokgora && !feud)
            playerbot::PveManager::ClearNoticeErrand(notice.Quarry);

        if (!feud)
            PairStance::Clear(notice.Quarry, notice.Holder);

        if (!QuarryNamedElsewhereLocked(notice.Quarry, notice.Id))
            playerbot::PveManager::SetNoticeHeld(notice.Quarry, false);
    }

    void StartFeudLocked(ObjectGuid bot, ObjectGuid human)
    {
        for (Feud& feud : g_feuds)
            if (feud.Bot == bot && feud.Human == human)
            {
                feud.Until = GameTime::GetGameTime() + time_t(s_feudMinutes) * MINUTE;
                return;
            }

        Feud feud;
        feud.Bot = bot;
        feud.Human = human;
        feud.Until = GameTime::GetGameTime() + time_t(s_feudMinutes) * MINUTE;
        g_feuds.push_back(feud);

        PutStance(bot, human, PairStance::Stance::Feud);
        playerbot::PveManager::SetNoticeHeld(bot, true);
        playerbot::PveManager::SetNoticeErrand(bot, playerbot::PveManager::NoticeErrandKind::Pursue, human);
    }

    void EndFeudLocked(std::vector<Feud>::iterator itr)
    {
        ObjectGuid const bot = itr->Bot;
        ObjectGuid const human = itr->Human;
        g_feuds.erase(itr);

        PairStance::Clear(bot, human);
        playerbot::PveManager::ClearNoticeErrand(bot);
        if (!QuarryNamedElsewhereLocked(bot, 0))
            playerbot::PveManager::SetNoticeHeld(bot, false);
    }

    bool FeudsOn()
    {
        return s_enabled && s_on.Mokgora && s_on.MokgoraFeud;
    }

    // What a refused Mok'gora costs, said as it stands on this realm.
    std::string RefusalText(char const* what, std::string const& name)
    {
        if (FeudsOn())
            return Trinity::StringFormat("{} {} takes it as an insult - and is coming for you.", what, name);
        return Trinity::StringFormat("{} {} walks away.", what, name);
    }

    // Everything after a .reload config: feuds end if they were switched off,
    // and every live notice is made to match its switches again.
    void ReconcileAllLocked()
    {
        if (!FeudsOn())
            while (!g_feuds.empty())
                EndFeudLocked(g_feuds.begin());

        // Shared with the group or not, as the switch now says.
        for (Feud const& feud : g_feuds)
            PutStance(feud.Bot, feud.Human, PairStance::Stance::Feud);

        for (Notice& notice : g_notices)
            ReconcileLocked(notice);
    }

    // ---------------------------------------------------------------- the quest

    // Out of the log in a way the client sees, whatever its status. See
    // Notoriety::VoidContract for why each line is here.
    void RemoveQuestFromLog(Player* holder, uint32 questId, bool failed)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!holder || !quest || holder->GetQuestStatus(questId) == QUEST_STATUS_NONE)
            return;

        if (failed)
            holder->SendQuestFailed(questId);

        if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED))
            holder->RemoveTimedQuest(questId);

        holder->TakeQuestSourceItem(questId, false);
        holder->AbandonQuest(questId);

        uint16 const logSlot = holder->FindQuestSlot(questId);
        if (logSlot < MAX_QUEST_LOG_SIZE)
            holder->SetQuestSlot(logSlot, 0);

        holder->RemoveActiveQuest(questId);
        holder->RemoveRewardedQuest(questId);
    }

    void SendNoticeList(Player* viewer);

    // The quarry is down (or beaten in the ring): the quest completes, and the
    // notice waits to be handed in at any board.
    //
    // Either thread, with the holder in hand: the tick, or a duel's end where
    // the holder is one of the two duelists.
    void CompleteNotice(uint32 id, Player* holder, std::string const& how)
    {
        Notice copy;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            Notice* notice = FindLocked(id);
            if (!notice || notice->Complete)
                return;

            ReleaseLiveLocked(*notice);
            notice->Complete = true;
            notice->Live = true;
            copy = *notice;
        }

        SaveNotice(copy);

        if (!holder)
            return;

        uint32 const questId = QuestFor(copy.Type, copy.Slot);
        if (holder->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            holder->AreaExploredOrEventHappens(questId);

        Tell(holder, Trinity::StringFormat("{} Touch any bounty board to collect.", how));
        SendNoticeList(holder);
    }

    enum class EndReason : uint8
    {
        Abandoned,
        Expired,
        Gone,
        GearGone,
        Lost,
        Refused,      // a Mok'gora turned down: the bot takes it personally
    };

    // The notice is over without a collection. The holder must be in hand if
    // they are online at all - the tick, a request, or a duel's end.
    void EndNotice(uint32 id, Player* holder, EndReason reason, std::string const& text)
    {
        Notice copy;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = std::find_if(g_notices.begin(), g_notices.end(), [id](Notice const& n) { return n.Id == id; });
            if (itr == g_notices.end())
                return;

            copy = *itr;

            // The feud first, so the release below leaves its stance and its
            // hold in place rather than tearing them down and putting them back.
            if (reason == EndReason::Refused && FeudsOn())
                StartFeudLocked(copy.Quarry, copy.Holder);

            ReleaseLiveLocked(*itr);
            g_notices.erase(itr);
            UpdateAnyLocked();
        }

        // A challenge still waiting for an answer goes with the notice. Never
        // from a duel's end (EndReason::Lost): that is a map thread, the offers
        // are world-thread only, and once the flag is planted there is no offer
        // left to take back anyway.
        if (copy.Type == Kind::Mokgora && reason != EndReason::Lost)
            Mokgora::CancelBotChallenge(copy.Quarry);

        DeleteNotice(copy);

        if (holder)
        {
            RemoveQuestFromLog(holder, QuestFor(copy.Type, copy.Slot),
                reason != EndReason::Abandoned);
            if (!text.empty())
                Tell(holder, text);
            SendNoticeList(holder);
        }

        TC_LOG_INFO("scripts", "Bounty notices: {}'s {} notice on {} ended ({}).",
            holder ? holder->GetName() : copy.Holder.ToString(), KindCode(copy.Type), copy.QuarryName, uint32(reason));
    }

    // ---------------------------------------------------------------- reading the quarry

    // Somebody's death-chest gear this bot is still holding back from the
    // auction house for them.
    struct TakenPiece
    {
        int32 Slot = -1;
        uint32 ItemId = 0;
        ObjectGuid Owner;
    };

    template <typename Fn>
    void ForEachCarried(Player* bot, Fn&& fn)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item, int32(slot));

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item, -1);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    if (Item* item = bag->GetItemByPos(uint8(i)))
                        fn(item, -1);
    }

    std::vector<TakenPiece> TakenFromBodies(Player* bot)
    {
        std::vector<TakenPiece> pieces;
        ForEachCarried(bot, [&](Item* item, int32 slot)
        {
            ObjectGuid owner;
            if (playerbot::PveManager::GetChestLootOwner(item->GetGUID(), owner))
                pieces.push_back({ slot, item->GetEntry(), owner });
        });
        return pieces;
    }

    bool CarriesGearOf(Player* bot, ObjectGuid owner)
    {
        bool found = false;
        ForEachCarried(bot, [&](Item* item, int32 /*slot*/)
        {
            ObjectGuid from;
            if (!found && playerbot::PveManager::GetChestLootOwner(item->GetGUID(), from) && from == owner)
                found = true;
        });
        return found;
    }

    // A world bot a recovery notice may name: any one, wherever it stands - the
    // gear is yours and there is no "fair fight" rule about taking it back.
    bool IsWorldBot(Player const* bot)
    {
        if (!bot || !bot->IsInWorld() || bot->IsGameMaster() || !BarracksHardcore::IsPlayerbot(bot))
            return false;
        WorldSession const* session = bot->GetSession();
        return session && !session->IsTransientPlayerSession() && !bot->InBattleground() && !bot->InArena();
    }

    // ---------------------------------------------------------------- taking a notice

    // Why this person may not take this kind of notice on this bot right now,
    // as a code for the addon; "ok" when they may. `slot` gets the free slot.
    // People in the player's party or raid, the player included. Bots may ride
    // along but do not count. World thread.
    uint32 PosseSize(Player* player)
    {
        Group* group = player->GetGroup();
        if (!group)
            return 1;

        uint32 people = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
            if (Player* member = ref->GetSource(); member && !BarracksHardcore::IsPlayerbot(member))
                ++people;
        return std::max<uint32>(people, 1);
    }

    std::string WhyNot(Player* player, Player* bot, Kind kind, uint8& slot)
    {
        slot = 0;

        if (!KindOn(kind) || !s_tablesReady || !sObjectMgr->GetQuestTemplate(QuestFor(kind, 0)))
            return "off";

        if (player->GetLevel() < s_minLevel)
            return "min";

        if (Tournament::IsTournamentCharacter(player))
            return "tourn";

        if (kind == Kind::Recovery ? !IsWorldBot(bot) : !BountyBoard::IsOnTheBoard(bot))
            return "gone";

        bool const levelOk = bot->GetLevel() > Trinity::XP::GetGrayLevel(player->GetLevel()) &&
            uint32(bot->GetLevel()) <= uint32(player->GetLevel()) + s_levelsAbove;

        switch (kind)
        {
            case Kind::Hunt:
                if (s_on.HuntRequireWarMode && !BarracksHardcore::IsWarModeOptedIn(player))
                    return "wm";
                if (!levelOk)
                    return "level";
                break;
            case Kind::Mokgora:
                if (!Mokgora::IsEnabled())
                    return "off";
                if (!levelOk)
                    return "level";
                break;
            case Kind::Recovery:
                if (!CarriesGearOf(bot, player->GetGUID()))
                    return "none";
                break;
            case Kind::Posse:
                // The zone's Warchief only, and only for a party: a posse is
                // the board's group content. No War Mode needed - the notice
                // makes it fair game to the whole party.
                if (!Nemesis::IsWarchief(bot->GetGUID()))
                    return "chief";
                if (PosseSize(player) < s_posseMinGroup)
                    return "group";
                if (!levelOk)
                    return "level";
                break;
        }

        // Back from the grave for whoever put it there: no notice names it
        // until that is settled - it has an errand and a stance of its own.
        if (Nemesis::IsHunting(bot->GetGUID()))
            return "grave";

        uint8 const rank = RankOf(player->GetGUID());
        {
            std::lock_guard<std::mutex> guard(g_lock);

            uint32 contracts = 0;           // hunts and Mok'goras share a limit
            bool slotTaken[kSlots] = { false, false };
            for (Notice const& notice : g_notices)
            {
                if (notice.Holder == player->GetGUID())
                {
                    if (notice.Quarry == bot->GetGUID())
                        return "held";
                    if (notice.Type != Kind::Recovery)
                        ++contracts;
                    if (notice.Type == kind && notice.Slot < kSlots)
                        slotTaken[notice.Slot] = true;
                }
                else if (kind == Kind::Mokgora && notice.Type == Kind::Mokgora && notice.Quarry == bot->GetGUID())
                    return "taken";
            }

            // A bot already set on somebody cannot walk to a second person.
            if (kind == Kind::Mokgora)
                for (Feud const& feud : g_feuds)
                    if (feud.Bot == bot->GetGUID())
                        return "taken";

            if (kind != Kind::Recovery && contracts >= MaxNotices(rank))
                return "full";

            uint8 free = kSlots;
            for (uint8 s = 0; s < kSlots; ++s)
                if (!slotTaken[s] && sObjectMgr->GetQuestTemplate(QuestFor(kind, s)))
                {
                    free = s;
                    break;
                }
            if (free == kSlots)
                return "full";
            slot = free;
        }

        Quest const* quest = sObjectMgr->GetQuestTemplate(QuestFor(kind, slot));
        if (!quest || !player->CanAddQuest(quest, false))
            return "quest";

        return "ok";
    }

    std::string WhyNotText(std::string const& code)
    {
        if (code == "off")   return "The board is not taking notices just now.";
        if (code == "min")   return Trinity::StringFormat("Come back at level {}. The board does not deal with greenhorns.", uint32(s_minLevel));
        if (code == "tourn") return "Tournament fighters have no business with the board.";
        if (code == "gone")  return "That notice has been torn down - whoever it named is dead, or has moved on.";
        if (code == "held")  return "You already carry a notice on them.";
        if (code == "wm")    return "Hunting is War Mode business. Mok'gora is open to anyone.";
        if (code == "level") return "Not a fair fight. Pick someone nearer your own level.";
        if (code == "full")  return "You carry as many notices as the board will trust you with. Finish one first.";
        if (code == "taken") return "Somebody else's challenge is already on its way to them.";
        if (code == "grave") return "That one has crawled out of a grave for somebody. The board will not post it until that is settled.";
        if (code == "chief") return "Only a zone's Warchief draws a posse.";
        if (code == "group") return Trinity::StringFormat("A posse needs at least {} of you in a party. Group up first.", s_posseMinGroup);
        if (code == "none")  return "They carry nothing of yours.";
        if (code == "quest") return "Your quest log is full.";
        return "The board will not give you that notice.";
    }

    // The bodyguards an aggressive bot calls when it hears it is wanted: world
    // bots near it in the same zone, near its level, not busy with anything a
    // notice already asked of them. World thread.
    std::vector<ObjectGuid> PickEscorts(Player* quarry, uint32 wanted)
    {
        std::vector<std::pair<float, ObjectGuid>> candidates;
        if (!wanted)
            return {};

        for (auto const& [guid, other] : ObjectAccessor::GetPlayers())
        {
            if (other == quarry || !BountyBoard::IsOnTheBoard(other) || other->IsInCombat())
                continue;
            if (other->GetMapId() != quarry->GetMapId() || other->GetZoneId() != quarry->GetZoneId())
                continue;
            if (std::abs(int32(other->GetLevel()) - int32(quarry->GetLevel())) > 6)
                continue;
            if (playerbot::PveManager::IsNoticeHeld(other->GetGUID()))
                continue;

            float const distance = other->GetDistance(quarry);
            if (distance > s_escortReachYards)
                continue;

            candidates.emplace_back(distance, other->GetGUID());
        }

        std::sort(candidates.begin(), candidates.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });

        std::vector<ObjectGuid> chosen;
        for (auto const& [distance, guid] : candidates)
        {
            if (chosen.size() >= wanted)
                break;
            chosen.push_back(guid);
        }
        return chosen;
    }

    void SendFirstHint(Player* holder, Notice& notice, Player* quarry);

    // A duel request to or from a bot that is still unanswered on a second look
    // at least five seconds later. The fleet answers one on the spot (accepts,
    // or refuses out loud - playerbot_loader's OnDuelRequest), so one still open
    // is stuck, and it holds both sides "in a duel" for good: a Mok'gora
    // challenger stood beside its person waiting for it to end, and nothing
    // ended it until one of the two died. World thread.
    void ClearStuckDuelRequest(Player* player, time_t now)
    {
        static std::unordered_map<uint64, std::pair<uint64, time_t>> seen;   // player -> (opponent, first seen)
        uint64 const key = player->GetGUID().GetRawValue();

        Player* opponent = player->duel ? player->duel->Opponent : nullptr;
        bool const stuckShape = opponent && player->duel->State == DUEL_STATE_CHALLENGED &&
            (BarracksHardcore::IsPlayerbot(player) || BarracksHardcore::IsPlayerbot(opponent));
        if (!stuckShape)
        {
            seen.erase(key);
            return;
        }

        uint64 const other = opponent->GetGUID().GetRawValue();
        auto const itr = seen.find(key);
        if (itr == seen.end() || itr->second.first != other)
        {
            seen[key] = { other, now };
            return;
        }
        if (now - itr->second.second < 5)
            return;

        seen.erase(itr);
        TC_LOG_INFO("scripts", "Bounty notices: cleared a duel request between {} and {} that was never answered.",
            player->GetName(), opponent->GetName());
        player->DuelComplete(DUEL_INTERRUPTED);
    }

    // Into the quest log, the registry and the table, and in force on the bots.
    // The new notice's id, or 0 when the quest would not go in. World thread.
    uint32 AddNoticeFor(Player* player, Player* bot, Kind kind, uint8 slot, std::vector<ObjectGuid> const& escorts)
    {
        uint32 const questId = QuestFor(kind, slot);
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return 0;
        player->AddQuestAndCheckCompletion(quest, nullptr);
        if (player->GetQuestStatus(questId) == QUEST_STATUS_NONE)
            return 0;

        time_t const now = GameTime::GetGameTime();

        Notice notice;
        notice.Holder = player->GetGUID();
        notice.Type = kind;
        notice.Slot = slot;
        notice.Quarry = bot->GetGUID();
        notice.QuarryName = BountyBoard::NoticeName(bot);
        notice.QuarryLevel = bot->GetLevel();
        notice.QuarryClass = bot->GetClass();
        notice.QuarryRace = bot->GetRace();
        notice.QuarryKills = BountyBoard::LifetimeKills(bot->GetGUID().GetCounter());
        notice.ZoneId = bot->GetZoneId();
        notice.TakenAt = now;
        notice.ExpiresAt = now + time_t(s_noticeMinutes) * MINUTE;

        uint32 id = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            notice.Id = ++g_nextId;
            id = notice.Id;
            g_notices.push_back(notice);
            Notice& stored = g_notices.back();
            ApplyLiveLocked(stored);

            for (ObjectGuid escort : escorts)
            {
                playerbot::PveManager::SetNoticeErrand(escort, playerbot::PveManager::NoticeErrandKind::Escort, stored.Quarry);
                playerbot::PveManager::SetNoticeHeld(escort, true);
                stored.Escorts.push_back(escort);
            }
            UpdateAnyLocked();
        }

        SaveNotice(notice);
        return id;
    }

    void TakeNotice(Player* player, ObjectGuid::LowType botLow, Kind kind)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botLow));

        uint8 slot = 0;
        std::string const code = bot ? WhyNot(player, bot, kind, slot) : std::string("gone");
        if (code != "ok")
        {
            Result(player, false, WhyNotText(code));
            return;
        }

        // The hunted hear it. An aggressive bot does not wait to be found alone.
        std::vector<ObjectGuid> escorts;
        uint8 const temper = playerbot::PveManager::GetBotTemperament(bot);
        if (kind == Kind::Hunt && s_on.HuntEscorts && temper >= s_escortTemper)
            escorts = PickEscorts(bot, std::min<uint32>(s_escortMax, temper >= s_escortTemperTwo ? 2 : 1));

        uint32 const id = AddNoticeFor(player, bot, kind, slot, escorts);
        if (!id)
        {
            Result(player, false, "The notice would not go into your quest log.");
            return;
        }

        std::string const name = BountyBoard::NoticeName(bot);
        std::string const who = Trinity::StringFormat("{} (level {} {})", name, uint32(bot->GetLevel()),
            ClassWord(bot->GetClass()));

        switch (kind)
        {
            case Kind::Hunt:
                Result(player, true, Trinity::StringFormat("You tear down the notice on {}. Kill them, then touch any bounty board.", who));
                if (!escorts.empty())
                    Tell(player, Trinity::StringFormat("Word travels fast. {} has heard you are coming, and {} at their side.",
                        name, escorts.size() == 1 ? "a friend now stands" : "two friends now stand"));
                break;
            case Kind::Mokgora:
                Result(player, true, Trinity::StringFormat("You tear down the notice on {}. They will come to you and "
                    "challenge you to Mok'gora - be somewhere a duel is allowed, and answer when they ask. "
                    "Refuse, and they will come for you anyway.", who));
                break;
            case Kind::Recovery:
                Result(player, true, Trinity::StringFormat("You tear down the notice on {}, who carries what was taken "
                    "from your body. Until it is back in your hands, they are your enemy and you are theirs - "
                    "War Mode or not.", who));
                break;
            case Kind::Posse:
            {
                // The whole party rides: everybody in it here in the zone who
                // may take the notice gets their own copy, and their own pay.
                std::string const zone = BountyBoard::ZoneName(bot->GetZoneId());
                uint32 riders = 0;
                if (Group* group = player->GetGroup())
                    for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
                    {
                        Player* member = ref->GetSource();
                        if (!member || member == player || !member->IsInWorld() || BarracksHardcore::IsPlayerbot(member) ||
                            member->GetZoneId() != player->GetZoneId())
                            continue;

                        uint8 memberSlot = 0;
                        std::string const memberCode = WhyNot(member, bot, Kind::Posse, memberSlot);
                        if (memberCode == "held")
                            continue;   // already riding
                        if (memberCode != "ok" || !AddNoticeFor(member, bot, Kind::Posse, memberSlot, {}))
                        {
                            Tell(member, Trinity::StringFormat("{} raised a posse against {}, but you cannot ride with it: {}",
                                player->GetName(), name, memberCode == "ok" ? std::string("your quest log is full.")
                                    : WhyNotText(memberCode)));
                            continue;
                        }

                        ++riders;
                        Tell(member, Trinity::StringFormat("{} raised a posse against {}, Warchief of {}. You ride with it: "
                            "bring it down together, then touch any bounty board.", player->GetName(), who, zone));
                        SendNoticeList(member);
                    }

                Result(player, true, Trinity::StringFormat("You raise a posse against {}, Warchief of {}{}. Bring it down "
                    "together, then touch any bounty board.{}", who, zone,
                    riders ? Trinity::StringFormat(" - {} of your party ride{} with you", riders, riders == 1 ? "s" : "")
                           : std::string(),
                    s_on.PosseFeud ? " It is fair game to every one of you, War Mode or not." : ""));
                break;
            }
        }

        if (kind != Kind::Mokgora && s_on.Hints)
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (Notice* stored = FindLocked(id))
                SendFirstHint(player, *stored, bot);
        }

        SendNoticeList(player);

        TC_LOG_INFO("scripts", "Bounty notices: {} took a {} notice on {} (level {}, temper {}, {} escort(s)).",
            player->GetName(), KindCode(kind), name, uint32(bot->GetLevel()), uint32(temper), escorts.size());
    }

    // ---------------------------------------------------------------- hints

    // Where the quarry is, as much as the holder's rank lets them be told: the
    // zone always, the spot within it from Tracker up. Called with g_lock held.
    void SendFirstHint(Player* holder, Notice& notice, Player* quarry)
    {
        if (!holder || !quarry)
            return;

        uint8 rank = 0;
        if (auto const itr = g_ledgers.find(holder->GetGUID().GetRawValue()); itr != g_ledgers.end())
            rank = RankFor(itr->second.Collected);

        uint32 const zone = quarry->GetZoneId();
        std::string const zoneName = BountyBoard::ZoneName(zone);
        std::string const area = AreaName(quarry->GetAreaId());

        if (PerkOn(rank, 1, s_on.PerkArea) && !area.empty() && quarry->GetAreaId() != zone)
            Tell(holder, Trinity::StringFormat("Word on the road: {} was last seen near {}, {}.", notice.QuarryName, area, zoneName));
        else
            Tell(holder, Trinity::StringFormat("Word on the road: {} is somewhere in {}.", notice.QuarryName, zoneName));

        notice.LastHintZone = zone;
        notice.NextHintAt = GameTime::GetGameTime() + time_t(s_hintSeconds);
    }

    // ---------------------------------------------------------------- the addon

    void SendNoticeList(Player* viewer)
    {
        if (!viewer)
            return;

        std::vector<Notice> mine;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Notice const& notice : g_notices)
                if (notice.Holder == viewer->GetGUID())
                    mine.push_back(notice);
        }

        time_t const now = GameTime::GetGameTime();
        Chunker out(viewer, "BBNA", "");
        for (Notice const& notice : mine)
            out.Add(Trinity::StringFormat("{},{},{},{},{},{},{},{},{}", KindCode(notice.Type), uint32(notice.Slot),
                notice.Quarry.GetCounter(), uint32(notice.QuarryLevel), uint32(notice.QuarryClass), notice.ZoneId,
                notice.Complete ? 1 : 0, notice.Complete ? 0 : uint32(std::max<time_t>(0, notice.ExpiresAt - now)),
                Scrubbed(notice.QuarryName)));
        out.Flush();
        BountyBoard::SendTagged(viewer, "BBNE", std::to_string(mine.size()));
    }

    void SendLedger(Player* viewer)
    {
        Ledger const ledger = LedgerCopy(viewer->GetGUID());
        uint8 const rank = RankFor(ledger.Collected);
        auto const [done, total] = ZoneProgress(ledger);

        // Fields 10 and 12-14 were pardons: zero, so an older addon still reads the
        // rest where it expects it. Then the four rank thresholds, so the Ledger
        // can say what each rank takes.
        std::string thresholds;
        for (uint8 i = 0; i < kTopRank; ++i)
            thresholds += Trinity::StringFormat(",{}", i < s_rankAt.size() ? s_rankAt[i] : 0u);
        BountyBoard::SendTagged(viewer, "BBLG", Trinity::StringFormat("{},{},{},{},{},{},{},{},{},0,{},0,0,0,{},{}{}",
            uint32(rank), ledger.Collected, NextRankAt(ledger.Collected), ledger.Seals, ledger.Hunts, ledger.Mokgora,
            ledger.Recovered, done, total, MaxNotices(rank), FeatureBits(), ledger.Posses, thresholds));
    }

    // ---------------------------------------------------------------- Most Wanted

    // Every board zone pins one bot a day: the one with the most player kills
    // this week (its lifetime count breaks a tie). The first notice collected on
    // it that day pays the multiplier, and the collector's name is stamped
    // across its poster until the day turns.
    struct MostWantedPin
    {
        uint32 Day = 0;
        ObjectGuid Bot;
        std::string Name;
        std::string CollectedBy;
        time_t MissingSince = 0;   // runtime only
    };
    std::unordered_map<uint32, MostWantedPin> g_mostWanted;               // zone -> today's pin, under g_lock
    std::unordered_map<uint32, std::map<uint32, uint32>> g_botDayKills;   // bot low guid -> day -> kills, under g_lock

    // The board's day, turning over at MostWanted.ResetHour (server clock, UTC).
    uint32 DayIndex(time_t at)
    {
        return uint32(std::max<time_t>(0, at - time_t(s_mostWantedResetHour) * HOUR) / DAY);
    }

    uint32 WeekKillsLocked(uint32 botLow, uint32 today)
    {
        auto const itr = g_botDayKills.find(botLow);
        if (itr == g_botDayKills.end())
            return 0;
        uint32 kills = 0;
        for (auto const& [day, count] : itr->second)
            if (day + 7 > today)
                kills += count;
        return kills;
    }

    // A world bot has killed a person. Any thread.
    void RecordBotKill(ObjectGuid::LowType botLow)
    {
        uint32 const today = DayIndex(GameTime::GetGameTime());
        {
            std::lock_guard<std::mutex> guard(g_lock);
            std::map<uint32, uint32>& days = g_botDayKills[botLow];
            ++days[today];
            while (!days.empty() && days.begin()->first + 7 <= today)
                days.erase(days.begin());
        }
        if (s_tablesReady)
            CharacterDatabase.PExecute("INSERT INTO character_bounty_bot_kills (bot, day, kills) VALUES ({}, {}, 1) "
                "ON DUPLICATE KEY UPDATE kills = kills + 1", botLow, today);
    }

    void SaveMostWanted(uint32 zoneId, MostWantedPin const& pin)
    {
        if (!s_tablesReady)
            return;
        std::string name = pin.Name;
        std::string by = pin.CollectedBy;
        CharacterDatabase.EscapeString(name);
        CharacterDatabase.EscapeString(by);
        CharacterDatabase.PExecute("REPLACE INTO character_bounty_mostwanted (zoneId, day, bot, name, collectedBy) "
            "VALUES ({}, {}, {}, '{}', '{}')", zoneId, pin.Day, pin.Bot.GetCounter(), name, by);
    }

    // World thread, at startup with the tables in place.
    void LoadMostWanted()
    {
        uint32 const today = DayIndex(GameTime::GetGameTime());
        CharacterDatabase.DirectExecute(Trinity::StringFormat(
            "DELETE FROM character_bounty_bot_kills WHERE day + 7 <= {}", today).c_str());

        std::lock_guard<std::mutex> guard(g_lock);
        g_botDayKills.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT bot, day, kills FROM character_bounty_bot_kills"))
        {
            do
            {
                Field* f = result->Fetch();
                g_botDayKills[f[0].GetUInt32()][f[1].GetUInt32()] = f[2].GetUInt32();
            } while (result->NextRow());
        }

        g_mostWanted.clear();
        if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT zoneId, day, bot, name, collectedBy FROM character_bounty_mostwanted WHERE day = {}", today))
        {
            do
            {
                Field* f = result->Fetch();
                MostWantedPin pin;
                pin.Day = f[1].GetUInt32();
                if (uint32 const bot = f[2].GetUInt32())
                    pin.Bot = ObjectGuid::Create<HighGuid::Player>(bot);
                pin.Name = f[3].GetString();
                pin.CollectedBy = f[4].GetString();
                g_mostWanted[f[0].GetUInt32()] = pin;
            } while (result->NextRow());
        }
    }

    // World thread, once a minute. A pin whose bot has been off the board for
    // MostWanted.MissingMinutes moves on to somebody else - unless somebody has
    // already collected on it: that one stays, stamped, until the day turns.
    void TickMostWanted()
    {
        if (!s_enabled || !s_on.MostWanted || !s_tablesReady)
            return;

        time_t const now = GameTime::GetGameTime();
        uint32 const today = DayIndex(now);

        struct Candidate
        {
            ObjectGuid Guid;
            std::string Name;
            uint32 Lifetime = 0;
        };
        std::unordered_map<uint32, std::vector<Candidate>> byZone;
        for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
            if (BountyBoard::IsOnTheBoard(bot))
                byZone[bot->GetZoneId()].push_back({ bot->GetGUID(), BountyBoard::NoticeName(bot),
                    BountyBoard::LifetimeKills(bot->GetGUID().GetCounter()) });
        std::vector<uint32> const zones = BountyBoard::BoardZones();

        std::vector<std::pair<uint32, MostWantedPin>> saves;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (uint32 zoneId : zones)
            {
                MostWantedPin& pin = g_mostWanted[zoneId];
                std::vector<Candidate> const& bots = byZone[zoneId];

                ObjectGuid skip;
                if (pin.Day == today && !pin.Bot.IsEmpty())
                {
                    if (!pin.CollectedBy.empty())
                        continue;
                    bool const here = std::any_of(bots.begin(), bots.end(),
                        [&pin](Candidate const& c) { return c.Guid == pin.Bot; });
                    if (here)
                    {
                        pin.MissingSince = 0;
                        continue;
                    }
                    if (!pin.MissingSince)
                    {
                        pin.MissingSince = now;
                        continue;
                    }
                    if (now - pin.MissingSince < time_t(s_mostWantedMissingMinutes) * MINUTE)
                        continue;
                    skip = pin.Bot;   // gone to ground: pin somebody else
                }

                Candidate const* best = nullptr;
                uint32 bestWeek = 0;
                for (Candidate const& c : bots)
                {
                    if (c.Guid == skip)
                        continue;
                    uint32 const week = WeekKillsLocked(c.Guid.GetCounter(), today);
                    if (!week && !c.Lifetime)
                        continue;   // nobody is Most Wanted for killing nobody
                    if (!best || week > bestWeek || (week == bestWeek && c.Lifetime > best->Lifetime))
                    {
                        best = &c;
                        bestWeek = week;
                    }
                }

                if (!best)
                {
                    if (pin.Day != today || !pin.Bot.IsEmpty())
                    {
                        pin = MostWantedPin();
                        pin.Day = today;
                        saves.emplace_back(zoneId, pin);
                    }
                    continue;
                }

                pin.Day = today;
                pin.Bot = best->Guid;
                pin.Name = best->Name;
                pin.CollectedBy.clear();
                pin.MissingSince = 0;
                saves.emplace_back(zoneId, pin);
            }
        }

        for (auto const& [zoneId, pin] : saves)
        {
            SaveMostWanted(zoneId, pin);
            if (!pin.Bot.IsEmpty())
                TC_LOG_INFO("scripts", "Bounty notices: {} is Most Wanted in {} today.", pin.Name, BountyBoard::ZoneName(zoneId));
        }
    }

    // The first notice collected on today's pin takes the stamp. True when this
    // one did. Any thread.
    bool ClaimMostWanted(ObjectGuid bot, std::string const& by)
    {
        uint32 const today = DayIndex(GameTime::GetGameTime());
        uint32 zone = 0;
        MostWantedPin claimed;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (auto& [zoneId, pin] : g_mostWanted)
                if (pin.Day == today && pin.Bot == bot && pin.CollectedBy.empty())
                {
                    pin.CollectedBy = by;
                    zone = zoneId;
                    claimed = pin;
                    break;
                }
        }
        if (!zone)
            return false;
        SaveMostWanted(zone, claimed);
        return true;
    }

    // ---------------------------------------------------------------- collecting

    void WriteKeepsake(Player* player, Notice const& notice, GameObject* board)
    {
        if (!s_keepsakeItem || !sObjectMgr->GetItemTemplate(s_keepsakeItem))
            return;

        Item* item = Item::CreateItem(s_keepsakeItem, 1, player);
        if (!item)
            return;

        time_t const now = GameTime::GetGameTime();
        tm stamp{};
        localtime_r(&now, &stamp);
        static char const* const months[12] = { "January", "February", "March", "April", "May", "June", "July",
            "August", "September", "October", "November", "December" };

        char const* how = notice.Type == Kind::Mokgora ? "Slain in Mok'gora."
            : notice.Type == Kind::Recovery ? "What was taken, taken back."
            : notice.Type == Kind::Posse ? "Brought down by a posse."
            : "Hunted down.";

        item->SetText(Trinity::StringFormat(
            "WANTED\n\n{}\nLevel {} {} {}\nPlayer kills: {}\n\n{}\n\nCollected by {} in {},\n{} {} {}.",
            notice.QuarryName, uint32(notice.QuarryLevel), RaceWord(notice.QuarryRace), ClassWord(notice.QuarryClass),
            notice.QuarryKills, how, player->GetName(), BountyBoard::ZoneName(notice.ZoneId),
            stamp.tm_mday, months[std::clamp(stamp.tm_mon, 0, 11)], stamp.tm_year + 1900));
        item->SetFlag(ITEM_FIELD_FLAGS, ITEM_FLAG_MAIL_TEXT_MASK);

        // A trophy, not something to carry about: straight to the mailbox, where
        // it waits until the player wants it on a wall (or in a bank).
        if (s_on.KeepsakesByMail)
        {
            MailItem(player, item, board, Trinity::StringFormat("Collected: {}", notice.QuarryName),
                "The board's keeper keeps a copy of every notice it pays out on. This one is yours, for your wall.");
            Tell(player, Trinity::StringFormat("The notice on {} is in your mailbox, for your wall.", notice.QuarryName));
        }
        else
            GiveOrMail(player, item, board, "A notice for your wall",
                "You collected this notice with your bags full. The board's keeper sent it on.");
    }

    void PayOut(Player* player, Notice const& notice, GameObject* board)
    {
        uint32 money = s_moneyBase + s_moneyPerLevel * notice.QuarryLevel +
            s_moneyPerKill * std::min(notice.QuarryKills, s_killCap);

        float bubbles = s_huntBubbles;
        uint32 seals = s_sealsHunt;
        switch (notice.Type)
        {
            case Kind::Mokgora:
                money = uint32(float(money) * s_mokgoraMoney);
                bubbles = s_mokgoraBubbles;
                seals = s_sealsMokgora;
                break;
            case Kind::Recovery:
                money = uint32(float(money) * s_recoveryMoney);
                bubbles = s_recoveryBubbles;
                seals = s_sealsRecovery;
                break;
            case Kind::Posse:
                money = uint32(float(money) * s_posseMoney);
                bubbles = s_posseBubbles;
                seals = s_sealsPosse;
                break;
            default:
                break;
        }

        // Today's Most Wanted: the first notice collected on it pays the
        // multiplier, and the collector's name goes across its poster.
        bool const mostWanted = s_on.MostWanted && notice.Type != Kind::Recovery &&
            ClaimMostWanted(notice.Quarry, player->GetName());
        if (mostWanted)
        {
            money = uint32(float(money) * s_mostWantedMultiplier);
            seals = uint32(std::ceil(float(seals) * s_mostWantedMultiplier));
            bubbles *= std::min(2.0f, s_mostWantedMultiplier);
        }

        uint32 xp = 0;
        if (player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            money = uint32(float(money) * s_cappedMoney);
        else
            xp = uint32(float(sObjectMgr->GetXPForLevel(player->GetLevel())) * bubbles / 20.0f);

        if (!s_on.RewardMoney)
            money = 0;
        if (!s_on.RewardExperience)
            xp = 0;
        if (!s_on.RewardSeals)
            seals = 0;

        if (money)
            player->ModifyMoney(int64(money));
        if (xp)
            player->GiveXP(xp, nullptr);

        // The ledger. The zone counts only if a board stands in it - the set
        // that earns a title is "every zone with a board". With the Ledger
        // switched off nothing is counted, so ranks and the zone set stand
        // still; seals are their own switch.
        Ledger after;
        uint8 rankBefore = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            Ledger& ledger = g_ledgers[player->GetGUID().GetRawValue()];
            rankBefore = RankFor(ledger.Collected);
            if (s_on.Ledger)
            {
                ++ledger.Collected;
                switch (notice.Type)
                {
                    case Kind::Hunt:     ++ledger.Hunts;     break;
                    case Kind::Mokgora:  ++ledger.Mokgora;   break;
                    case Kind::Recovery: ++ledger.Recovered; break;
                    case Kind::Posse:    ++ledger.Posses;    break;
                }
                ledger.Zones.insert(notice.ZoneId);
            }
            ledger.Seals += seals;
            after = ledger;
        }

        SaveLedger(player->GetGUID(), after);
        if (s_tablesReady && s_on.Ledger && notice.ZoneId)
            CharacterDatabase.PExecute(
                "INSERT INTO character_bounty_ledger_zone (guid, zoneId, collected) VALUES ({}, {}, 1) "
                "ON DUPLICATE KEY UPDATE collected = collected + 1",
                player->GetGUID().GetCounter(), notice.ZoneId);

        std::vector<std::string> paidParts;
        if (money)
            paidParts.push_back(MoneyText(money));
        if (xp)
            paidParts.push_back(Trinity::StringFormat("{} experience", xp));
        if (seals)
            paidParts.push_back(Trinity::StringFormat("{} seal{}", seals, seals == 1 ? "" : "s"));
        std::string paid;
        for (std::string const& part : paidParts)
            paid += (paid.empty() ? "" : ", ") + part;
        Tell(player, paid.empty()
            ? Trinity::StringFormat("You hand in the notice on {}.", notice.QuarryName)
            : Trinity::StringFormat("You hand in the notice on {}: {}.", notice.QuarryName, paid));

        if (mostWanted)
            Tell(player, Trinity::StringFormat("Most Wanted! The board pays {:g} times over for {} today, and your name "
                "goes across the poster.", s_mostWantedMultiplier, notice.QuarryName));

        if (s_on.Keepsakes)
            WriteKeepsake(player, notice, board);

        uint8 const rankAfter = RankFor(after.Collected);
        if (s_on.Ledger && rankAfter > rankBefore)
        {
            Tell(player, Trinity::StringFormat("The Hunter's Ledger writes you up a rank: {}.", RankNames[rankAfter]));
            switch (rankAfter)
            {
                case 1:
                    if (s_on.PerkArea)
                        Tell(player, "Notices now tell you where in a zone their quarry was last seen.");
                    break;
                case 2:
                    if (s_on.PerkTemper)
                        Tell(player, "Notices now tell you how ready their quarry is for a fight.");
                    break;
                case 3:
                    if (s_on.PerkCoin)
                        Tell(player, "Notices now tell you how much coin their quarry carries.");
                    break;
                default:
                    break;
            }

            if (rankAfter >= kTopRank)
            {
                if (s_on.PerkTwoNotices)
                    Tell(player, "You may now carry two notices at once.");
                if (s_on.Tabard && s_tabardItem && sObjectMgr->GetItemTemplate(s_tabardItem))
                    if (Item* tabard = Item::CreateItem(s_tabardItem, 1, player))
                        GiveOrMail(player, tabard, board, "The Headsman's tabard",
                            "Wear it. Everybody on the boards will know what it means.");
            }
        }

        GrantEarnedTitles(player, after);

        TC_LOG_INFO("scripts", "Bounty notices: {} collected a {} notice on {} for {}c, {} xp, {} seal(s); ledger {} ({}).",
            player->GetName(), KindCode(notice.Type), notice.QuarryName, money, xp, seals, after.Collected, RankNames[rankAfter]);
    }

    // ---------------------------------------------------------------- the Quartermaster

    // Seals are the board's own coin. Every notice pays some (a posse and a Most
    // Wanted more), a slain nemesis pays by its rank, and the board's
    // Quartermaster takes them for what is on its list (Shop.Items).
    ShopItem const* FindShopItem(uint32 itemId)
    {
        for (ShopItem const& item : s_shop)
            if (item.ItemId == itemId)
                return &item;
        return nullptr;
    }

    void SendShop(Player* viewer)
    {
        if (!s_enabled || !s_on.Shop || !viewer)
            return;

        Chunker out(viewer, "BBSH", "");
        for (ShopItem const& item : s_shop)
            if (sObjectMgr->GetItemTemplate(item.ItemId))
                out.Add(Trinity::StringFormat("{},{},{}", item.ItemId, item.Price, item.Count));
        out.Flush();
        BountyBoard::SendTagged(viewer, "BBSE", std::to_string(LedgerCopy(viewer->GetGUID()).Seals));
    }

    void Buy(Player* player, uint32 itemId, GameObject* board)
    {
        if (!s_enabled || !s_on.Shop || !s_tablesReady)
        {
            Result(player, false, "The Quartermaster is not trading just now.");
            return;
        }

        ShopItem const* entry = FindShopItem(itemId);
        ItemTemplate const* proto = entry ? sObjectMgr->GetItemTemplate(entry->ItemId) : nullptr;
        if (!proto)
        {
            Result(player, false, "The Quartermaster has nothing like that on the list.");
            return;
        }

        uint32 const price = entry->Price;
        uint32 const count = std::clamp<uint32>(entry->Count, 1, std::max<uint32>(1, proto->GetMaxStackSize()));

        Ledger after;
        uint32 had = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            Ledger& ledger = g_ledgers[player->GetGUID().GetRawValue()];
            had = ledger.Seals;
            if (ledger.Seals >= price)
                ledger.Seals -= price;
            after = ledger;
        }
        if (had < price)
        {
            Result(player, false, Trinity::StringFormat("That costs {} seal{}, and you have {}. Every notice you collect "
                "pays more.", price, price == 1 ? "" : "s", had));
            return;
        }

        Item* item = Item::CreateItem(entry->ItemId, count, player);
        if (!item)
        {
            // Nothing was handed over, so nothing is kept.
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_ledgers[player->GetGUID().GetRawValue()].Seals += price;
            }
            Result(player, false, "The Quartermaster could not lay hands on one. Nothing was taken.");
            return;
        }

        SaveLedger(player->GetGUID(), after);
        GiveOrMail(player, item, board, "From the Quartermaster", "Paid for in seals at the bounty board.");
        Result(player, true, Trinity::StringFormat("You trade {} seal{} for {}{}.", price, price == 1 ? "" : "s",
            count > 1 ? Trinity::StringFormat("{} x ", count) : std::string(), proto->Name1));
        SendLedger(player);
        SendShop(player);

        TC_LOG_INFO("scripts", "Bounty notices: {} bought {} x{} for {} seal(s), {} left.", player->GetName(),
            entry->ItemId, count, price, after.Seals);
    }

    void GossipShopMenu(Player* player, GameObject* board)
    {
        ClearGossipMenuFor(player);
        uint32 const seals = LedgerCopy(player->GetGUID()).Seals;
        uint32 shown = 0;
        for (ShopItem const& item : s_shop)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.ItemId);
            if (!proto || shown >= 15)
                continue;
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, Trinity::StringFormat("{}{} - {} seal{}{}",
                item.Count > 1 ? Trinity::StringFormat("{} x ", item.Count) : std::string(), proto->Name1, item.Price,
                item.Price == 1 ? "" : "s", seals < item.Price ? " (not enough)" : ""), kSenderBuy, item.ItemId);
            ++shown;
        }
        Tell(player, Trinity::StringFormat("You have {} seal{}.", seals, seals == 1 ? "" : "s"));
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, board->GetGUID());
    }

    // The core follows shared stances onto their groups and re-sends whatever
    // colour changed (PairStance::Update). World thread.
    void RefreshStaleViews()
    {
        PairStance::Update();
    }

    // A quarry is down. Whoever holds a hunt or recovery notice on it and had a
    // hand in the kill - or stood near it with somebody who did, in their group
    // - completes it. Everybody else's notice stands: the bot will get up again.
    void HandleDeath(DeathEvent const& death)
    {
        std::vector<std::pair<uint32, ObjectGuid>> candidates;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Notice const& notice : g_notices)
                if (notice.Quarry == death.Victim && !notice.Complete && notice.Type != Kind::Mokgora &&
                    KindOn(notice.Type))
                    candidates.emplace_back(notice.Id, notice.Holder);

            // A feud lasts until one of the two is dead.
            for (auto itr = g_feuds.begin(); itr != g_feuds.end(); )
            {
                if (itr->Bot == death.Victim || itr->Human == death.Victim)
                {
                    EndFeudLocked(itr);
                    itr = g_feuds.begin();
                    continue;
                }
                ++itr;
            }
            UpdateAnyLocked();
        }

        for (auto const& [id, holderGuid] : candidates)
        {
            Player* holder = ObjectAccessor::FindPlayer(holderGuid);
            if (!holder)
                continue;

            bool credited = std::find(death.Participants.begin(), death.Participants.end(), holderGuid) !=
                death.Participants.end();

            if (!credited && holder->GetMapId() == death.MapId &&
                holder->GetExactDist2d(death.X, death.Y) <= s_creditYards)
                if (Group const* group = holder->GetGroup())
                    for (ObjectGuid participant : death.Participants)
                        if (group->IsMember(participant))
                        {
                            credited = true;
                            break;
                        }

            if (!credited)
                continue;

            std::string name;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                if (Notice const* notice = FindLocked(id))
                    name = notice->QuarryName;
            }
            CompleteNotice(id, holder, Trinity::StringFormat("{} is dead.", name));
        }
    }

    void Tick()
    {
        if (!g_any.load(std::memory_order_relaxed))
            return;

        std::vector<DeathEvent> deaths;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            deaths.swap(g_deaths);
        }
        for (DeathEvent const& death : deaths)
            HandleDeath(death);

        time_t const now = GameTime::GetGameTime();

        // Feuds that ran out of time.
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (auto itr = g_feuds.begin(); itr != g_feuds.end(); )
            {
                if (itr->Until <= now)
                {
                    EndFeudLocked(itr);
                    itr = g_feuds.begin();
                    continue;
                }
                ++itr;
            }
        }

        std::vector<Notice> snapshot;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            snapshot = g_notices;
            UpdateAnyLocked();
        }

        for (Notice const& notice : snapshot)
        {
            if (!notice.Live)
                continue;

            Player* holder = ObjectAccessor::FindPlayer(notice.Holder);
            if (!holder || !holder->IsInWorld())
                continue;

            uint32 const questId = QuestFor(notice.Type, notice.Slot);

            // Torn up from the quest log. For a Mok'gora that is a refusal like
            // any other: the bot was promised a fight.
            if (holder->GetQuestStatus(questId) == QUEST_STATUS_NONE && now > notice.TakenAt + 2)
            {
                if (notice.Type == Kind::Mokgora && !notice.Complete)
                    EndNotice(notice.Id, holder, EndReason::Refused, RefusalText("You tore up the Mok'gora notice.",
                        notice.QuarryName));
                else
                    EndNotice(notice.Id, holder, EndReason::Abandoned, Trinity::StringFormat(
                        "You tear up the notice on {}.", notice.QuarryName));
                continue;
            }

            // A kind switched off is frozen: nothing expires, hunts or pays
            // until it is switched on again.
            if (notice.Complete || !KindOn(notice.Type))
                continue;

            if (now >= notice.ExpiresAt && !notice.Challenged && !notice.Dueling)
            {
                EndNotice(notice.Id, holder, EndReason::Expired, Trinity::StringFormat(
                    "The notice on {} has run its time, and the board takes it back.", notice.QuarryName));
                continue;
            }

            Player* quarry = ObjectAccessor::FindConnectedPlayer(notice.Quarry);
            if (!quarry || !quarry->IsInWorld())
            {
                bool lapse = false;
                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    if (Notice* stored = FindLocked(notice.Id))
                    {
                        if (!stored->QuarryMissingSince)
                            stored->QuarryMissingSince = now;
                        else if (now - stored->QuarryMissingSince >= time_t(s_quarryGoneMinutes) * MINUTE)
                            lapse = true;
                    }
                }
                if (lapse)
                    EndNotice(notice.Id, holder, EndReason::Gone, Trinity::StringFormat(
                        "{} has gone to ground. The notice lapses.", notice.QuarryName));
                continue;
            }

            {
                std::lock_guard<std::mutex> guard(g_lock);
                if (Notice* stored = FindLocked(notice.Id))
                    stored->QuarryMissingSince = 0;
            }

            switch (notice.Type)
            {
                case Kind::Hunt:
                case Kind::Recovery:
                case Kind::Posse:
                {
                    // Gone from its packs - listed, sold, thrown away - and
                    // there is nothing left to take back.
                    if (notice.Type == Kind::Recovery && now >= notice.NextGearCheckAt)
                    {
                        if (!CarriesGearOf(quarry, holder->GetGUID()))
                        {
                            EndNotice(notice.Id, holder, EndReason::GearGone, Trinity::StringFormat(
                                "Whatever {} took from you is gone from their packs. The notice lapses.", notice.QuarryName));
                            break;
                        }

                        std::lock_guard<std::mutex> guard(g_lock);
                        if (Notice* stored = FindLocked(notice.Id))
                            stored->NextGearCheckAt = now + 15;
                    }

                    // Word on the road: on the hint clock with a rank, and the
                    // moment it changes zone without one.
                    uint8 const rank = RankOf(holder->GetGUID());
                    bool const due = (PerkOn(rank, 1, s_on.PerkArea) && now >= notice.NextHintAt) ||
                        quarry->GetZoneId() != notice.LastHintZone;
                    if (due && s_on.Hints && quarry->IsAlive())
                    {
                        std::lock_guard<std::mutex> guard(g_lock);
                        if (Notice* stored = FindLocked(notice.Id))
                            SendFirstHint(holder, *stored, quarry);
                    }
                    break;
                }
                case Kind::Mokgora:
                {
                    if (notice.Challenged || notice.Dueling || now < notice.NextChallengeTryAt)
                        break;
                    if (!playerbot::PveManager::IsNoticeErrandArrived(notice.Quarry))
                        break;

                    // A duel request a bot never answered holds both sides "in a
                    // duel" for good, and the challenger would stand there for it.
                    ClearStuckDuelRequest(holder, now);
                    ClearStuckDuelRequest(quarry, now);

                    // Somebody already in a duel, or holding another box, is
                    // asked again in a moment rather than talked over - and told
                    // what the bot standing beside them is waiting for.
                    std::string wait;
                    if (holder->duel)
                        wait = holder->duel->State == DUEL_STATE_CHALLENGED
                            ? "you have a duel request out - finish or cancel it" : "you are in a duel";
                    else if (quarry->duel)
                        wait = "it is caught up in a duel of its own";
                    else if (Mokgora::HasPendingOffer(holder))
                        wait = "you have another Mok'gora to answer first";
                    else if (Mokgora::HasPendingOffer(quarry))
                        wait = "it is answering another challenge";
                    bool const busy = !wait.empty() || !holder->IsAlive();

                    std::string why;
                    bool const issued = !busy && Mokgora::ChallengeFromBot(quarry, holder, why);

                    std::lock_guard<std::mutex> guard(g_lock);
                    Notice* stored = FindLocked(notice.Id);
                    if (!stored)
                        break;

                    if (issued)
                    {
                        stored->Challenged = true;
                        stored->WaitingSince = 0;
                        stored->WaitLogged = false;
                        quarry->Say(Trinity::StringFormat("{}. You took my notice off the board - now take my challenge. Mok'gora.",
                            holder->GetName()), LANG_UNIVERSAL);
                    }
                    else
                    {
                        stored->NextChallengeTryAt = now + 5;
                        std::string const reason = busy ? wait : why;
                        if (!reason.empty() && now >= stored->NextNagAt)
                        {
                            stored->NextNagAt = now + 30;
                            Tell(holder, busy
                                ? Trinity::StringFormat("{} has come for you, and waits: {}.", notice.QuarryName, reason)
                                : Trinity::StringFormat("{} has come for you, but cannot challenge you here: {}",
                                    notice.QuarryName, reason));
                        }

                        // Said in the log once a wait runs long, so the next report
                        // of a challenger that "just follows" can be read back.
                        if (!stored->WaitingSince)
                            stored->WaitingSince = now;
                        else if (!stored->WaitLogged && now - stored->WaitingSince >= 60)
                        {
                            stored->WaitLogged = true;
                            TC_LOG_INFO("scripts", "Bounty notices: {} has waited {}s to challenge {}: {}.",
                                notice.QuarryName, uint32(now - stored->WaitingSince), holder->GetName(),
                                !reason.empty() ? reason : (holder->IsAlive() ? "no reason given" : "they are dead"));
                        }
                    }
                    break;
                }
            }
        }
    }

    // ---------------------------------------------------------------- Mok'gora answers

    // World thread, from Mokgora::Accept/Decline/Update/OnLogout.
    void OnBotChallengeAnswer(ObjectGuid bot, ObjectGuid target, Mokgora::BotChallengeAnswer answer)
    {
        uint32 id = 0;
        std::string name;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Notice& notice : g_notices)
            {
                if (notice.Type != Kind::Mokgora || notice.Quarry != bot || notice.Holder != target || notice.Complete)
                    continue;

                id = notice.Id;
                name = notice.QuarryName;
                switch (answer)
                {
                    case Mokgora::BotChallengeAnswer::Accepted:
                        // The truce has done its job. The duel is fought under
                        // the ordinary rules, exactly like a Mok'gora a person
                        // throws at a bot; an interrupted one puts it back.
                        notice.Dueling = true;
                        playerbot::PveManager::ClearNoticeErrand(bot);
                        PairStance::Clear(bot, target);
                        break;
                    case Mokgora::BotChallengeAnswer::Gone:
                        notice.Challenged = false;
                        notice.NextChallengeTryAt = GameTime::GetGameTime() + 10;
                        break;
                    default:
                        break;
                }
                break;
            }
        }

        if (!id)
            return;

        if (answer == Mokgora::BotChallengeAnswer::Declined || answer == Mokgora::BotChallengeAnswer::Lapsed)
            EndNotice(id, ObjectAccessor::FindPlayer(target), EndReason::Refused,
                RefusalText("You did not answer the Mok'gora. The notice is failed.", name));
    }

    // ---------------------------------------------------------------- the gossip fallback

    void GossipTakeMenu(Player* player, GameObject* board)
    {
        ClearGossipMenuFor(player);

        uint32 shown = 0;
        bool const hunts = KindOn(Kind::Hunt) &&
            (!s_on.HuntRequireWarMode || BarracksHardcore::IsWarModeOptedIn(player));
        bool const challenges = KindOn(Kind::Mokgora);

        // Anybody, anywhere, carrying your gear first: that is the notice the
        // reader most wants and can most easily miss.
        for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
        {
            if (shown >= 6 || !KindOn(Kind::Recovery))
                break;
            if (!IsWorldBot(bot) || !CarriesGearOf(bot, player->GetGUID()))
                continue;
            AddGossipItemFor(player, GOSSIP_ICON_TABARD, Trinity::StringFormat("Take back your gear: {} (level {} {}, {})",
                BountyBoard::NoticeName(bot), uint32(bot->GetLevel()), ClassWord(bot->GetClass()),
                BountyBoard::ZoneName(bot->GetZoneId())), kSenderTakeRecovery, bot->GetGUID().GetCounter());
            ++shown;
        }

        std::vector<Player*> local;
        for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
            if (BountyBoard::IsOnTheBoard(bot) && bot->GetZoneId() == board->GetZoneId())
                local.push_back(bot);
        std::sort(local.begin(), local.end(), [](Player const* a, Player const* b) { return a->GetLevel() > b->GetLevel(); });

        for (Player* bot : local)
        {
            if (shown >= 14)
                break;

            std::string const who = Trinity::StringFormat("{} (level {} {})", BountyBoard::NoticeName(bot),
                uint32(bot->GetLevel()), ClassWord(bot->GetClass()));
            if (hunts)
            {
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Hunt " + who, kSenderTakeHunt, bot->GetGUID().GetCounter());
                ++shown;
            }
            if (challenges)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Mok'gora: " + who, kSenderTakeMokgora, bot->GetGUID().GetCounter());
                ++shown;
            }
            if (KindOn(Kind::Posse) && Nemesis::IsWarchief(bot->GetGUID()))
            {
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Raise a posse against the Warchief: " + who, kSenderTakePosse,
                    bot->GetGUID().GetCounter());
                ++shown;
            }
        }

        if (!shown)
            Tell(player, "There are no notices you can take here.");

        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, board->GetGUID());
    }

    void GossipLedger(Player* player)
    {
        Ledger const ledger = LedgerCopy(player->GetGUID());
        uint8 const rank = RankFor(ledger.Collected);
        auto const [done, total] = ZoneProgress(ledger);

        Tell(player, Trinity::StringFormat("Hunter's Ledger: {} - {} notice{} collected{}.", RankNames[rank],
            ledger.Collected, ledger.Collected == 1 ? "" : "s",
            NextRankAt(ledger.Collected) ? Trinity::StringFormat(" (next rank at {})", NextRankAt(ledger.Collected)) : ""));
        Tell(player, Trinity::StringFormat("Hunts {}, Mok'gora {}, recovered {}, posses {}. Seals: {} (spend them with "
            "the Quartermaster). Zones: {} of {}.", ledger.Hunts, ledger.Mokgora, ledger.Recovered, ledger.Posses,
            ledger.Seals, done, total));

        std::vector<Notice> mine;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Notice const& notice : g_notices)
                if (notice.Holder == player->GetGUID())
                    mine.push_back(notice);
        }

        for (Notice const& notice : mine)
            Tell(player, Trinity::StringFormat("Notice: {} on {} (level {}){}.",
                notice.Type == Kind::Hunt ? "hunt" : notice.Type == Kind::Mokgora ? "Mok'gora"
                    : notice.Type == Kind::Posse ? "posse" : "recovery",
                notice.QuarryName, uint32(notice.QuarryLevel), notice.Complete ? " - done, hand it in" : ""));
    }
}

namespace BountyNotices
{
    bool Enabled()
    {
        return s_enabled;
    }

    void SendBoardExtras(Player* viewer, uint32 zoneId, std::vector<Player*> const& bots)
    {
        if (!s_enabled || !viewer)
            return;

        uint8 const rank = RankOf(viewer->GetGUID());

        std::unordered_set<uint64> mine;
        std::unordered_set<uint64> challenged;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Notice const& notice : g_notices)
            {
                if (notice.Holder == viewer->GetGUID())
                    mine.insert(notice.Quarry.GetRawValue());
                else if (notice.Type == Kind::Mokgora && !notice.Complete)
                    challenged.insert(notice.Quarry.GetRawValue());
            }
        }

        Chunker out(viewer, "BBRX", std::to_string(zoneId) + "|");
        for (Player* bot : bots)
        {
            uint32 flags = 0;
            if (mine.count(bot->GetGUID().GetRawValue()))
                flags |= 1;
            if (s_on.ShowTakenFrom && CarriesGearOf(bot, viewer->GetGUID()))
                flags |= 2;
            if (KindOn(Kind::Mokgora) && challenged.count(bot->GetGUID().GetRawValue()))
                flags |= 4;

            bool const area = PerkOn(rank, 1, s_on.PerkArea);
            bool const temper = PerkOn(rank, 2, s_on.PerkTemper);
            bool const coin = PerkOn(rank, 3, s_on.PerkCoin);
            if (!flags && !area && !temper && !coin)
                continue;

            int32 const temperValue = temper ? int32(playerbot::PveManager::GetBotTemperament(bot)) : -1;
            int64 const coinValue = coin ? int64(bot->GetMoney()) : -1;
            std::string const areaName = area && bot->GetAreaId() != bot->GetZoneId()
                ? Scrubbed(AreaName(bot->GetAreaId())) : std::string();

            out.Add(Trinity::StringFormat("{},{},{},{},{}", bot->GetGUID().GetCounter(), flags, temperValue,
                coinValue, areaName));
        }
        out.Flush();

        // The zone's Most Wanted today, with the collector's name once taken.
        if (s_on.MostWanted)
        {
            uint32 const today = DayIndex(GameTime::GetGameTime());
            std::string line;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                auto const itr = g_mostWanted.find(zoneId);
                if (itr != g_mostWanted.end() && itr->second.Day == today && !itr->second.Bot.IsEmpty())
                    line = Trinity::StringFormat("{}|{},{}", zoneId, itr->second.Bot.GetCounter(),
                        Scrubbed(itr->second.CollectedBy));
            }
            if (!line.empty())
                BountyBoard::SendTagged(viewer, "BBMW", line);
        }
    }

    void SendPosterExtras(Player* viewer, Player* bot)
    {
        if (!s_enabled || !viewer || !bot)
            return;

        std::string const head = std::to_string(bot->GetGUID().GetCounter()) + "|";

        Chunker owners(viewer, "BBPO", head);
        for (TakenPiece const& piece : s_on.ShowTakenFrom ? TakenFromBodies(bot) : std::vector<TakenPiece>())
        {
            std::string owner;
            if (!sCharacterCache->GetCharacterNameByGuid(piece.Owner, owner))
                continue;
            owners.Add(Trinity::StringFormat("{},{},{}", piece.Slot, piece.ItemId, Scrubbed(owner)));
        }
        owners.Flush();

        uint8 slot = 0;
        BountyBoard::SendTagged(viewer, "BBPX", Trinity::StringFormat("{}{},{},{}", head,
            WhyNot(viewer, bot, Kind::Hunt, slot), WhyNot(viewer, bot, Kind::Mokgora, slot),
            WhyNot(viewer, bot, Kind::Recovery, slot)));
        BountyBoard::SendTagged(viewer, "BBPP", head + WhyNot(viewer, bot, Kind::Posse, slot));

        if (s_on.MostWanted)
        {
            uint32 const today = DayIndex(GameTime::GetGameTime());
            bool pinned = false;
            std::string by;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                for (auto const& [zoneId, pin] : g_mostWanted)
                    if (pin.Day == today && pin.Bot == bot->GetGUID())
                    {
                        pinned = true;
                        by = pin.CollectedBy;
                        break;
                    }
            }
            if (pinned)
                BountyBoard::SendTagged(viewer, "BBMP", Trinity::StringFormat("{}{},{}", head,
                    uint32(std::lround(s_mostWantedMultiplier * 10.0f)), Scrubbed(by)));
        }
    }

    void SendLedgerAndNotices(Player* viewer)
    {
        if (!s_enabled || !viewer)
            return;

        SendLedger(viewer);
        SendNoticeList(viewer);
    }

    bool HandleRequest(Player* player, std::string_view verb, std::string_view argument)
    {
        if (verb == "TAKE")
        {
            // <guid>:<H|M|R>
            size_t const colon = argument.find(':');
            Optional<uint32> const guid = Trinity::StringTo<uint32>(argument.substr(0, colon));
            Kind kind = Kind::Hunt;
            if (!guid || colon == std::string_view::npos || colon + 1 >= argument.size() ||
                !KindFromCode(argument[colon + 1], kind))
                return true;

            TakeNotice(player, *guid, kind);
            return true;
        }

        if (verb == "DROP")
        {
            // <H|M|R>:<slot>
            Kind kind = Kind::Hunt;
            if (argument.size() < 3 || !KindFromCode(argument[0], kind))
                return true;
            Optional<uint32> const slot = Trinity::StringTo<uint32>(argument.substr(2));
            if (!slot)
                return true;

            uint32 id = 0;
            std::string name;
            bool complete = false;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                for (Notice const& notice : g_notices)
                    if (notice.Holder == player->GetGUID() && notice.Type == kind && notice.Slot == *slot)
                    {
                        id = notice.Id;
                        name = notice.QuarryName;
                        complete = notice.Complete;
                        break;
                    }
            }
            if (!id)
                return true;

            if (complete)
            {
                Result(player, false, "That notice is done. Touch the board to hand it in.");
                return true;
            }

            if (kind == Kind::Mokgora)
                EndNotice(id, player, EndReason::Refused, RefusalText("You tear up the Mok'gora notice.", name));
            else
                EndNotice(id, player, EndReason::Abandoned, Trinity::StringFormat("You tear up the notice on {}.", name));
            return true;
        }

        if (verb == "LEDGER")
        {
            SendLedgerAndNotices(player);
            return true;
        }

        if (verb == "SHOP")
        {
            SendShop(player);
            return true;
        }

        if (verb == "BUY")
        {
            if (Optional<uint32> const itemId = Trinity::StringTo<uint32>(argument))
                Buy(player, *itemId, nullptr);
            return true;
        }

        return false;
    }

    void OnBoardTouched(Player* player, GameObject* board)
    {
        if (!s_enabled || !player || BarracksHardcore::IsPlayerbot(player))
            return;

        std::vector<Notice> done;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (auto itr = g_notices.begin(); itr != g_notices.end(); )
            {
                if (itr->Holder == player->GetGUID() && itr->Complete)
                {
                    done.push_back(*itr);
                    itr = g_notices.erase(itr);
                }
                else
                    ++itr;
            }
            UpdateAnyLocked();
        }

        for (Notice const& notice : done)
        {
            uint32 const questId = QuestFor(notice.Type, notice.Slot);
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);

            if (quest && player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                player->CompleteQuest(questId);

            if (quest && player->CanRewardQuest(quest, false))
                player->RewardQuest(quest, 0, board, true);
            else
                RemoveQuestFromLog(player, questId, false);

            DeleteNotice(notice);
            PayOut(player, notice, board);
        }

        if (!done.empty())
            SendNoticeList(player);
    }

    bool AddSeals(Player* player, uint32 seals)
    {
        if (!s_enabled || !s_tablesReady || !player || !seals)
            return false;

        Ledger after;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            Ledger& ledger = g_ledgers[player->GetGUID().GetRawValue()];
            ledger.Seals += seals;
            after = ledger;
        }
        SaveLedger(player->GetGUID(), after);
        return true;
    }

    void AddGossipOptions(Player* player)
    {
        if (!s_enabled || !s_on.Gossip || !s_tablesReady || !player)
            return;

        if (KindOn(Kind::Hunt) || KindOn(Kind::Mokgora) || KindOn(Kind::Recovery) || KindOn(Kind::Posse))
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Take a notice down.", kSenderMenu, 0);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "My notices and the Hunter's Ledger.", kSenderLedger, 0);

        if (s_on.Shop && !s_shop.empty())
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, Trinity::StringFormat("Spend seals with the Quartermaster. "
                "(You have {}.)", LedgerCopy(player->GetGUID()).Seals), kSenderShop, 0);
    }

    bool OnGossipSelect(Player* player, GameObject* board, uint32 sender, uint32 action)
    {
        if (!s_enabled || !s_on.Gossip)
            return false;

        switch (sender)
        {
            case kSenderMenu:
                GossipTakeMenu(player, board);
                return true;
            case kSenderTakeHunt:
                CloseGossipMenuFor(player);
                TakeNotice(player, action, Kind::Hunt);
                return true;
            case kSenderTakeMokgora:
                CloseGossipMenuFor(player);
                TakeNotice(player, action, Kind::Mokgora);
                return true;
            case kSenderTakeRecovery:
                CloseGossipMenuFor(player);
                TakeNotice(player, action, Kind::Recovery);
                return true;
            case kSenderLedger:
                CloseGossipMenuFor(player);
                GossipLedger(player);
                return true;
            case kSenderShop:
                GossipShopMenu(player, board);
                return true;
            case kSenderBuy:
                CloseGossipMenuFor(player);
                Buy(player, action, board);
                return true;
            case kSenderTakePosse:
                CloseGossipMenuFor(player);
                TakeNotice(player, action, Kind::Posse);
                return true;
            default:
                return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
class custom_bounty_notices_world : public WorldScript
{
public:
    custom_bounty_notices_world() : WorldScript("custom_bounty_notices_world") { }

    // Every switch takes effect here, on a running realm: .reload config.
    void OnConfigLoad(bool /*reload*/) override
    {
        LoadNoticeConfig();
        ApplySwitches();
    }

    static void ApplySwitches()
    {
        // The core rule and the dispatch are switched by their owners; the
        // notices only say what they want.
        PairStance::SetEnabled(s_on.Stances);
        playerbot::PveManager::SetNoticeDispatchAllowed(s_enabled && s_on.Dispatch);

        std::lock_guard<std::mutex> guard(g_lock);
        ReconcileAllLocked();
        UpdateAnyLocked();
    }

    void OnStartup() override
    {
        LoadNoticeConfig();

        // Created here rather than shipped as a migration: a realm on this
        // branch without them would otherwise abort on the first query.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bounty_notice ("
            "guid INT UNSIGNED NOT NULL,"
            "kind TINYINT UNSIGNED NOT NULL,"
            "slot TINYINT UNSIGNED NOT NULL,"
            "quarry INT UNSIGNED NOT NULL,"
            "quarryName VARCHAR(64) NOT NULL DEFAULT '',"
            "quarryLevel TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "quarryClass TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "quarryRace TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "quarryKills INT UNSIGNED NOT NULL DEFAULT 0,"
            "zoneId INT UNSIGNED NOT NULL DEFAULT 0,"
            "takenAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "expiresAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "complete TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (guid, kind, slot)"
            ") ENGINE=InnoDB");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bounty_ledger ("
            "guid INT UNSIGNED NOT NULL PRIMARY KEY,"
            "collected INT UNSIGNED NOT NULL DEFAULT 0,"
            "hunts INT UNSIGNED NOT NULL DEFAULT 0,"
            "mokgora INT UNSIGNED NOT NULL DEFAULT 0,"
            "recovered INT UNSIGNED NOT NULL DEFAULT 0,"
            "seals INT UNSIGNED NOT NULL DEFAULT 0,"
            "pardonAt INT UNSIGNED NOT NULL DEFAULT 0"
            ") ENGINE=InnoDB");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bounty_ledger_zone ("
            "guid INT UNSIGNED NOT NULL,"
            "zoneId INT UNSIGNED NOT NULL,"
            "collected INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (guid, zoneId)"
            ") ENGINE=InnoDB");

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bounty_mostwanted ("
            "zoneId INT UNSIGNED NOT NULL PRIMARY KEY,"
            "day INT UNSIGNED NOT NULL DEFAULT 0,"
            "bot INT UNSIGNED NOT NULL DEFAULT 0,"
            "name VARCHAR(64) NOT NULL DEFAULT '',"
            "collectedBy VARCHAR(32) NOT NULL DEFAULT ''"
            ") ENGINE=InnoDB");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bounty_bot_kills ("
            "bot INT UNSIGNED NOT NULL,"
            "day INT UNSIGNED NOT NULL,"
            "kills INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (bot, day)"
            ") ENGINE=InnoDB");

        // A ledger from before posses has no column for them.
        char const* const posseColumn = "SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND "
            "TABLE_NAME = 'character_bounty_ledger' AND COLUMN_NAME = 'posses'";
        if (!CharacterDatabase.Query(posseColumn))
            CharacterDatabase.DirectExecute("ALTER TABLE character_bounty_ledger ADD COLUMN posses INT UNSIGNED NOT NULL DEFAULT 0");

        // Checked, not assumed: DirectExecute does not say whether it worked,
        // and a query against a missing table (or column) takes the whole realm
        // down.
        s_tablesReady = bool(CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
            "('character_bounty_notice', 'character_bounty_ledger', 'character_bounty_ledger_zone', "
            "'character_bounty_mostwanted', 'character_bounty_bot_kills') HAVING COUNT(*) = 5")) &&
            bool(CharacterDatabase.Query(posseColumn));
        if (!s_tablesReady)
            TC_LOG_ERROR("server.loading", "Bounty notices: the notice and ledger tables are missing - notices are off.");
        else
            LoadMostWanted();

        for (uint8 k = 0; k < 4; ++k)
            for (uint8 s = 0; s < kSlots; ++s)
                if (s_questIds[k][s] && !sObjectMgr->GetQuestTemplate(s_questIds[k][s]))
                    TC_LOG_ERROR("server.loading", "Bounty notices: quest {} is not in quest_template - that notice "
                        "cannot be taken.", s_questIds[k][s]);

        Mokgora::SetBotChallengeListener(&OnBotChallengeAnswer);

        TC_LOG_INFO("server.loading", "Bounty notices: {} ({} titles, keepsake {}, tabard {}).",
            s_enabled && s_tablesReady ? "on" : "OFF", s_headsmanTitle && s_relentlessTitle ? 2 : 0,
            s_keepsakeItem, s_tabardItem);
    }

    void OnUpdate(uint32 diff) override
    {
        _sinceTick += diff;
        if (_sinceTick < 1000)
            return;
        _sinceTick = 0;

        // Views refresh whatever the switches say: switching the feature off
        // is itself a change somebody's client has to be told about.
        RefreshStaleViews();
        if (s_enabled)
            Tick();

        // The Most Wanted pins, once a minute (the first a minute after start,
        // with the fleet logged back in).
        _sinceMostWanted += 1000;
        if (_sinceMostWanted >= 60000)
        {
            _sinceMostWanted = 0;
            TickMostWanted();
        }
    }

private:
    uint32 _sinceTick = 0;
    uint32 _sinceMostWanted = 0;
};

class custom_bounty_notices_player : public PlayerScript
{
public:
    custom_bounty_notices_player() : PlayerScript("custom_bounty_notices_player") { }

    // Loaded whether or not the feature is switched on, so switching it on
    // with .reload config picks up everybody already online.
    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (!s_tablesReady || !player || BarracksHardcore::IsPlayerbot(player))
            return;

        LoadLedger(player);

        time_t const now = GameTime::GetGameTime();
        std::vector<Notice> loaded;
        std::vector<Notice> stale;

        if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT kind, slot, quarry, quarryName, quarryLevel, quarryClass, quarryRace, quarryKills, zoneId, "
                "takenAt, expiresAt, complete FROM character_bounty_notice WHERE guid = {}", player->GetGUID().GetCounter()))
        {
            do
            {
                Field* fields = result->Fetch();
                Notice notice;
                notice.Holder = player->GetGUID();
                notice.Type = Kind(std::clamp<uint8>(fields[0].GetUInt8(), 1, 3));
                notice.Slot = fields[1].GetUInt8();
                notice.Quarry = ObjectGuid::Create<HighGuid::Player>(fields[2].GetUInt32());
                notice.QuarryName = fields[3].GetString();
                notice.QuarryLevel = fields[4].GetUInt8();
                notice.QuarryClass = fields[5].GetUInt8();
                notice.QuarryRace = fields[6].GetUInt8();
                notice.QuarryKills = fields[7].GetUInt32();
                notice.ZoneId = fields[8].GetUInt32();
                notice.TakenAt = time_t(fields[9].GetUInt32());
                notice.ExpiresAt = time_t(fields[10].GetUInt32());
                notice.Complete = fields[11].GetUInt8() != 0;

                QuestStatus const status = player->GetQuestStatus(QuestFor(notice.Type, notice.Slot));
                bool const inLog = status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE;
                if (!inLog || (!notice.Complete && now >= notice.ExpiresAt))
                    stale.push_back(notice);
                else
                    loaded.push_back(notice);
            } while (result->NextRow());
        }

        for (Notice const& notice : stale)
        {
            DeleteNotice(notice);
            RemoveQuestFromLog(player, QuestFor(notice.Type, notice.Slot), false);
            if (!notice.Complete)
                Tell(player, Trinity::StringFormat("While you were away the notice on {} ran its time.", notice.QuarryName));
        }

        // And the other way round: a notice quest in the log with no notice
        // behind it (a crash between the two writes) has nothing that could
        // ever complete it.
        for (uint8 k = 0; k < 3; ++k)
            for (uint8 s = 0; s < kSlots; ++s)
            {
                uint32 const questId = s_questIds[k][s];
                QuestStatus const status = player->GetQuestStatus(questId);
                if (!questId || (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE))
                    continue;

                bool const backed = std::any_of(loaded.begin(), loaded.end(), [&](Notice const& notice)
                    { return QuestFor(notice.Type, notice.Slot) == questId; });
                if (!backed)
                    RemoveQuestFromLog(player, questId, false);
            }

        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Notice& notice : loaded)
            {
                notice.Id = ++g_nextId;
                g_notices.push_back(notice);
                ApplyLiveLocked(g_notices.back());
            }
            UpdateAnyLocked();
        }

        GrantEarnedTitles(player, LedgerCopy(player->GetGUID()));
    }

    void OnLogout(Player* player) override
    {
        if (!player || !g_any.load(std::memory_order_relaxed))
        {
            if (player)
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_ledgers.erase(player->GetGUID().GetRawValue());
            }
            return;
        }

        std::lock_guard<std::mutex> guard(g_lock);

        // A person: their notices wait in the database, and nothing they put in
        // force stays in force without them.
        for (Notice& notice : g_notices)
            if (notice.Holder == player->GetGUID())
            {
                ReleaseLiveLocked(notice);
                notice.Challenged = false;
                notice.Dueling = false;
            }

        g_notices.erase(std::remove_if(g_notices.begin(), g_notices.end(),
            [&](Notice const& notice) { return notice.Holder == player->GetGUID(); }), g_notices.end());

        // Either side of a feud leaving ends it.
        for (auto itr = g_feuds.begin(); itr != g_feuds.end(); )
        {
            if (itr->Bot == player->GetGUID() || itr->Human == player->GetGUID())
            {
                EndFeudLocked(itr);
                itr = g_feuds.begin();
                continue;
            }
            ++itr;
        }

        g_ledgers.erase(player->GetGUID().GetRawValue());
        UpdateAnyLocked();
    }

    // Map thread: who had a hand in it is read off the corpse here, while the
    // combat references still stand; what that means for anybody's notice is
    // the tick's business, because the people owed credit may be anywhere.
    void OnPlayerJustDied(Player* victim, Unit* killer) override
    {
        // Every person a world bot kills counts toward that bot's week, for the
        // Most Wanted - whether or not any notice names it.
        if (s_enabled && s_on.MostWanted && victim && killer && !BarracksHardcore::IsPlayerbot(victim) &&
            !victim->IsGameMaster())
            if (Player* bot = killer->GetCharmerOrOwnerPlayerOrPlayerItself(); bot && bot != victim && IsWorldBot(bot))
                RecordBotKill(bot->GetGUID().GetCounter());

        if (!s_enabled || !victim || !g_any.load(std::memory_order_relaxed))
            return;

        {
            std::lock_guard<std::mutex> guard(g_lock);
            bool named = false;
            for (Notice const& notice : g_notices)
                if (notice.Quarry == victim->GetGUID())
                {
                    named = true;
                    break;
                }
            for (Feud const& feud : g_feuds)
                if (feud.Bot == victim->GetGUID() || feud.Human == victim->GetGUID())
                {
                    named = true;
                    break;
                }
            if (!named)
                return;
        }

        DeathEvent death;
        death.Victim = victim->GetGUID();
        death.MapId = victim->GetMapId();
        death.X = victim->GetPositionX();
        death.Y = victim->GetPositionY();

        std::unordered_set<uint64> seen;
        auto const consider = [&](Unit* unit)
        {
            if (!unit)
                return;
            Player* person = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (!person || person == victim)
                return;
            if (seen.insert(person->GetGUID().GetRawValue()).second)
                death.Participants.push_back(person->GetGUID());
        };

        CombatManager const& combat = victim->GetCombatManager();
        for (auto const& reference : combat.GetPvPCombatRefs())
            consider(reference.second->GetOther(victim));
        for (auto const& reference : combat.GetPvECombatRefs())
            consider(reference.second->GetOther(victim));
        consider(killer);

        std::lock_guard<std::mutex> guard(g_lock);
        g_deaths.push_back(std::move(death));
        UpdateAnyLocked();
    }

    // Map thread, both duelists in hand. Only a Mok'gora a notice set up.
    void OnDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        if (!s_enabled || !winner || !loser || !g_any.load(std::memory_order_relaxed) || !Mokgora::IsMokgoraDuel(loser))
            return;

        uint32 id = 0;
        ObjectGuid holderGuid;
        std::string name;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Notice& notice : g_notices)
            {
                if (notice.Type != Kind::Mokgora || notice.Complete || !notice.Dueling)
                    continue;
                bool const pair = (notice.Holder == winner->GetGUID() && notice.Quarry == loser->GetGUID()) ||
                    (notice.Holder == loser->GetGUID() && notice.Quarry == winner->GetGUID());
                if (!pair)
                    continue;

                id = notice.Id;
                holderGuid = notice.Holder;
                name = notice.QuarryName;

                // Somebody else decided it. The challenger tries again.
                if (type == DUEL_INTERRUPTED)
                {
                    notice.Dueling = false;
                    notice.Challenged = false;
                    notice.NextChallengeTryAt = GameTime::GetGameTime() + 30;
                    PutStance(notice.Quarry, notice.Holder, PairStance::Stance::Truce);
                    playerbot::PveManager::SetNoticeErrand(notice.Quarry,
                        playerbot::PveManager::NoticeErrandKind::Approach, notice.Holder);
                }
                break;
            }
        }

        if (!id)
            return;

        Player* holder = holderGuid == winner->GetGUID() ? winner : loser;

        if (type == DUEL_INTERRUPTED)
        {
            Tell(holder, Trinity::StringFormat("Somebody else settled that. {} will challenge you again.", name));
            return;
        }

        if (holder == winner)
            CompleteNotice(id, holder, Trinity::StringFormat("{} lies dead at your feet.", name));
        else
            EndNotice(id, holder, EndReason::Lost, type == DUEL_FLED
                ? Trinity::StringFormat("You ran from {}. The notice burns with your name on it.", name)
                : Trinity::StringFormat("{} won the Mok'gora. The notice goes back on the board.", name));
    }
};

void AddSC_custom_bounty_notices()
{
    LoadNoticeConfig();
    new custom_bounty_notices_world();
    new custom_bounty_notices_player();
}
