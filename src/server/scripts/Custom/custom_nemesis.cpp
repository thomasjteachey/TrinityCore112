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

// The nemesis system. See custom_nemesis.h for what it is; this is how.
//
// --- threads ----------------------------------------------------------------
//
// One registry behind one shared_mutex. Map threads READ it on every hit
// between a bot and a person (power and traits), and WRITE it on a death with
// the dying and the killing players in hand. The world thread (the tick, the
// board, login and logout) owns everything that reaches across maps: returns,
// struggles, Warchiefs and their guards, scale and immunities, and any message
// for somebody who is not standing where it happened - those are queued.
//
// A hit between two units that are not one bot and one person never takes the
// lock: the test is two pointer walks and an atomic.
//
// A revenant's hunt is world-thread business - TickRevenants picks the moment,
// TickHunts stages the approach and ends it - except that a death on a map
// thread can end it too (revenge, or laid to rest), with both bodies in hand.
//
// --- the wire (the board addon) ----------------------------------------------
//
//   server -> client  BBNO:<1|0>                                nemeses on this realm
//                     BBNX:<zoneId>|<guid>,<rank>,<warchief>,<scars>,<epithet>;...
//                     BBNP:<guid>|<rank>,<warchief>,<scars>,<kills>,<knownS>,<knownW>,
//                          <countS>,<countW>|<epithet>|<victim/victim xN/...>
//                     BBHR:<zone>|<guid>,<rank>,<warchief>,<scars>,<kills>,<zoneId>,
//                          <fallen>,<knownS>,<knownW>,<countS>,<countW>,<level>,<class>,
//                          <online>,<epithet>,<name>;...
//                     BBHE:<zone>|<count>
//                     BBRU:<secondsAgo>,<zoneId>,<text>;...      rumours, newest first
//                     BBRZ:<count>
//   client -> server  CCGAMEREQ\tBOUNTY:NEMESES:<zoneId, 0 every zone>
//
// Trait masks are bit N for trait N in the order of the tables below; the
// addon carries the same tables.

#include "custom_nemesis.h"

#include "custom_barracks_hardcore.h"
#include "custom_bounty_board.h"
#include "custom_bounty_notices.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"

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
#include "Log.h"
#include "Map.h"
#include "Miscellaneous/PairStance.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Unit.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // ---------------------------------------------------------------- traits

    enum StrengthBit : uint8
    {
        S_IRON_HIDE = 0,
        S_SPELLWARD,
        S_FEARLESS,
        S_UNSTOPPABLE,
        S_UNBOUND,
        S_UNBOWED,
        S_EXECUTIONER,
        S_BERSERKER,
        S_BODYGUARD,
        S_COUNT
    };

    enum WeaknessBit : uint8
    {
        W_THIN_SKINNED = 0,
        W_SPELL_SICK,
        W_FIRE,
        W_FROST,
        W_SHADOW,
        W_HOLY,
        W_NATURE,
        W_ARCANE,
        W_FALTERS,
        W_CLUMSY,
        W_COUNT
    };

    struct TraitText
    {
        char const* Name;
        char const* Effect;
        char const* Epithet;      // a strength can name its owner
    };

    TraitText const Strengths[S_COUNT] =
    {
        { "Iron Hide",   "takes 20% less physical damage",                     "Iron-Hide" },
        { "Spellward",   "takes 20% less magic damage",                        "the Spellbreaker" },
        { "Fearless",    "cannot be feared",                                   "the Fearless" },
        { "Unstoppable", "cannot be stunned",                                  "the Unstoppable" },
        { "Unbound",     "cannot be rooted or slowed",                         "the Unchained" },
        { "Unbowed",     "cannot be silenced or polymorphed",                  "the Unbowed" },
        { "Executioner", "deals 25% more damage to anyone below 35% health",   "the Executioner" },
        { "Berserker",   "deals 20% more damage while below half health",      "the Berserker" },
        { "Bodyguard",   "never walks alone",                                  "the Pack-Leader" },
    };

    TraitText const Weaknesses[W_COUNT] =
    {
        { "Thin-skinned",  "takes 20% more physical damage",            nullptr },
        { "Spell-sick",    "takes 20% more magic damage",               nullptr },
        { "Fire-scarred",  "takes 30% more fire damage",                nullptr },
        { "Frostbitten",   "takes 30% more frost damage",               nullptr },
        { "Shadow-shy",    "takes 30% more shadow damage",              nullptr },
        { "Light-blind",   "takes 30% more holy damage",                nullptr },
        { "Venom-weak",    "takes 30% more nature damage",              nullptr },
        { "Arcane-addled", "takes 30% more arcane damage",              nullptr },
        { "Falters",       "takes 30% more damage below 30% health",    nullptr },
        { "Clumsy",        "deals 10% less damage",                     nullptr },
    };

    // ---------------------------------------------------------------- switches

    struct Config
    {
        bool Enabled = false;
        uint8 MaxRank = 5;
        uint32 MaxLevelGap = 10;          // no rising off a victim this far below
        uint32 ClimbFightSeconds = 120;   // one rank a fight, however many of a party it kills in it
        uint32 SameVictimMinutes = 60;    // and never off the same person again this soon
        bool GroupCredit = true;          // a party close by shares a nemesis's purse and vengeance
        float CreditYards = 100.0f;
        bool GroupIntel = true;           // ...and what one of them learns of its traits
        bool Power = true;
        uint32 DamagePerRank = 5;         // % more damage dealt to people per rank
        uint32 ResistPerRank = 5;         // % less damage taken from people per rank
        bool Scale = true;
        float ScalePerRank = 0.04f;
        float ScaleWarchief = 0.06f;
        bool Traits = true;
        bool Intel = true;
        uint32 IntelZoneKillChance = 100; // % chance a bot killed in a nemesis's zone talks
        bool IntelOnDeath = true;         // dying to one teaches a strength
        bool Taunts = true;
        uint32 TauntCooldownMinutes = 10;
        // The Revenant. Every bot gets up again; what a slain nemesis loses is
        // its standing. Now and then one refuses to: hours later, at a moment
        // nobody chose, it comes back - for the one who put it down, and only
        // them.
        bool Returns = true;
        uint32 ReturnChance = 35;          // % a slain nemesis comes back at all
        uint32 ReturnChancePerRank = 5;    // the strong ones do not stay down
        uint32 ReturnChanceLossPerScar = 15;
        uint32 ReturnMinHours = 1;
        uint32 ReturnMaxHours = 8;
        uint32 ReturnStrikeChance = 5;     // % per 5 s once its slayer is out in the world
        uint32 ReturnGiveUpDays = 7;       // its slayer never came back out: it stays down
        uint32 VendettaMinutes = 20;
        uint32 VendettaDamage = 10;        // % more damage to its slayer while hunting them
        uint32 RevengeRanks = 2;           // ranks gained for killing its slayer
        uint32 RestRewardMultiplier = 2;   // laying it to rest for good pays more
        uint32 RevenantVisual = 35847;     // Ghost Visual Red - looks only, 0 for none
        uint32 RiseSound = 5174;           // Moment-Haunted02, to the slayer only
        uint32 ArriveSound = 4795;         // G_GraveBurstDespawn, around the revenant
        bool HuntGroupStance = true;       // fair game to its slayer's whole party, War Mode or not
        bool HuntDispatch = true;          // landed near its slayer when it is far away
        bool StrikeInRaid = false;         // it does not walk into an army
        uint32 PowerPerAlly = 10;          // % harder, both ways, per friend at the slayer's side
        uint32 MaxAllies = 4;
        float AllyYards = 40.0f;
        bool Struggles = true;
        uint32 StruggleCheckMinutes = 60;
        uint32 StruggleChance = 25;
        uint32 StruggleLeadMinutes = 30;
        bool Warchiefs = true;
        uint8 WarchiefMinRank = 3;
        uint32 WarchiefGuards = 2;
        uint32 WarchiefPowerBonus = 5;
        bool Guards = true;
        bool Yell = true;
        bool Whisper = true;
        bool Board = true;
        bool Gossip = true;
        bool Rewards = true;
        uint32 MoneyPerRank = 5000;
        uint32 SealsPerRank = 1;
        uint32 WarchiefRewardMultiplier = 2;
        bool Vengeance = true;
        uint32 FallenHistoryDays = 7;
    };
    Config s_cfg;

    void LoadConfig()
    {
        auto const flag = [](char const* key, bool def) { return sConfigMgr->GetBoolDefault(
            Trinity::StringFormat("Centurion.Nemesis.{}", key), def); };
        auto const num = [](char const* key, int32 def, int32 lo, int32 hi) { return uint32(std::clamp(
            sConfigMgr->GetIntDefault(Trinity::StringFormat("Centurion.Nemesis.{}", key), def), lo, hi)); };
        auto const real = [](char const* key, float def, float lo, float hi) { return std::clamp(
            sConfigMgr->GetFloatDefault(Trinity::StringFormat("Centurion.Nemesis.{}", key), def), lo, hi); };

        s_cfg.Enabled = flag("Enable", false);
        s_cfg.MaxRank = uint8(num("MaxRank", 5, 1, 10));
        s_cfg.MaxLevelGap = num("Promote.MaxLevelGap", 10, 0, 80);
        s_cfg.ClimbFightSeconds = num("Promote.FightSeconds", 120, 0, 3600);
        s_cfg.SameVictimMinutes = num("Promote.SameVictimMinutes", 60, 0, 24 * 60);
        s_cfg.GroupCredit = flag("Group.Credit", true);
        s_cfg.CreditYards = real("Group.CreditYards", 100.0f, 10.0f, 500.0f);
        s_cfg.GroupIntel = flag("Group.Intel", true);
        s_cfg.Power = flag("Power.Enable", true);
        s_cfg.DamagePerRank = num("Power.DamagePerRank", 5, 0, 50);
        s_cfg.ResistPerRank = num("Power.ResistPerRank", 5, 0, 15);
        s_cfg.Scale = flag("Scale.Enable", true);
        s_cfg.ScalePerRank = real("Scale.PerRank", 0.04f, 0.0f, 0.2f);
        s_cfg.ScaleWarchief = real("Scale.Warchief", 0.06f, 0.0f, 0.5f);
        s_cfg.Traits = flag("Traits.Enable", true);
        s_cfg.Intel = flag("Intel.Enable", true);
        s_cfg.IntelZoneKillChance = num("Intel.ZoneKillChance", 100, 0, 100);
        s_cfg.IntelOnDeath = flag("Intel.OnDeath", true);
        s_cfg.Taunts = flag("Taunts.Enable", true);
        s_cfg.TauntCooldownMinutes = num("Taunts.CooldownMinutes", 10, 1, 1440);
        s_cfg.Returns = flag("Return.Enable", true);
        s_cfg.ReturnChance = num("Return.Chance", 35, 0, 100);
        s_cfg.ReturnChancePerRank = num("Return.ChancePerRank", 5, 0, 50);
        s_cfg.ReturnChanceLossPerScar = num("Return.ChanceLossPerScar", 15, 0, 100);
        s_cfg.ReturnMinHours = num("Return.MinHours", 1, 0, 24 * 30);
        s_cfg.ReturnMaxHours = std::max(s_cfg.ReturnMinHours, num("Return.MaxHours", 8, 0, 24 * 30));
        s_cfg.ReturnStrikeChance = num("Return.StrikeChance", 5, 1, 100);
        s_cfg.ReturnGiveUpDays = num("Return.GiveUpDays", 7, 1, 365);
        s_cfg.VendettaMinutes = num("Return.VendettaMinutes", 20, 1, 24 * 60);
        s_cfg.VendettaDamage = num("Return.VendettaDamage", 10, 0, 100);
        s_cfg.RevengeRanks = num("Return.RevengeRanks", 2, 0, 10);
        s_cfg.RestRewardMultiplier = num("Return.RestRewardMultiplier", 2, 1, 10);
        s_cfg.RevenantVisual = num("Return.VisualSpell", 35847, 0, 1000000);
        s_cfg.RiseSound = num("Return.Sound.Rise", 5174, 0, 1000000);
        s_cfg.ArriveSound = num("Return.Sound.Arrive", 4795, 0, 1000000);
        s_cfg.HuntGroupStance = flag("Return.GroupStance", true);
        s_cfg.HuntDispatch = flag("Return.Dispatch", true);
        s_cfg.StrikeInRaid = flag("Return.StrikeInRaid", false);
        s_cfg.PowerPerAlly = num("Return.PowerPerAlly", 10, 0, 50);
        s_cfg.MaxAllies = num("Return.MaxAllies", 4, 0, 39);
        s_cfg.AllyYards = real("Return.AllyYards", 40.0f, 5.0f, 200.0f);
        s_cfg.Struggles = flag("Struggles.Enable", true);
        s_cfg.StruggleCheckMinutes = num("Struggles.CheckMinutes", 60, 1, 24 * 60);
        s_cfg.StruggleChance = num("Struggles.Chance", 25, 0, 100);
        s_cfg.StruggleLeadMinutes = num("Struggles.LeadMinutes", 30, 1, 24 * 60);
        s_cfg.Warchiefs = flag("Warchief.Enable", true);
        s_cfg.WarchiefMinRank = uint8(num("Warchief.MinRank", 3, 1, 10));
        s_cfg.WarchiefGuards = num("Warchief.Guards", 2, 0, 5);
        s_cfg.WarchiefPowerBonus = num("Warchief.PowerBonus", 5, 0, 25);
        s_cfg.Guards = flag("Guards.Enable", true);
        s_cfg.Yell = flag("Announce.Yell", true);
        s_cfg.Whisper = flag("Announce.Whisper", true);
        s_cfg.Board = flag("Board.Enable", true);
        s_cfg.Gossip = flag("Gossip.Enable", true);
        s_cfg.Rewards = flag("Rewards.Enable", true);
        s_cfg.MoneyPerRank = num("Rewards.MoneyPerRank", 5000, 0, 10000000);
        s_cfg.SealsPerRank = num("Rewards.SealsPerRank", 1, 0, 100);
        s_cfg.WarchiefRewardMultiplier = num("Rewards.WarchiefMultiplier", 2, 1, 10);
        s_cfg.Vengeance = flag("Vengeance.Enable", true);
        s_cfg.FallenHistoryDays = num("FallenHistoryDays", 7, 0, 365);
    }

    // ---------------------------------------------------------------- state

    struct Record
    {
        ObjectGuid Bot;
        std::string Name;          // the bot's own name, cached for when it is offline
        uint8 Level = 0;
        uint8 Class = 0;
        uint8 Rank = 1;
        std::string Epithet;
        uint32 Kills = 0;
        uint32 Strengths = 0;      // bit masks over the trait tables
        uint32 Weaknesses = 0;
        uint8 Scars = 0;
        uint32 ZoneId = 0;         // last seen in
        time_t RisenAt = 0;
        bool Fallen = false;
        time_t FallenAt = 0;
        time_t ReturnAt = 0;       // 0 with Fallen: gone for good
        ObjectGuid SlainBy;
        std::string SlainByName;

        // Runtime only.
        bool Warchief = false;
        uint32 AppliedImmunities = 0;   // mechanic bits put on the bot
        bool ScaleApplied = false;
        std::vector<ObjectGuid> Guards;
        time_t ClimbedAt = 0;           // the last rank it took off a kill

        // A revenant's hunt, while it lasts. A restart ends it.
        ObjectGuid Hunting;             // its slayer
        std::string HuntingName;
        time_t HuntUntil = 0;
        uint8 HuntStage = 0;            // 0 risen, 1 closing in, 2 upon them
        uint32 HuntAllies = 0;          // friends at the slayer's side, for its power
        std::vector<ObjectGuid> HuntParty;   // who those friends are
        uint32 HuntVisual = 0;          // the look it was given, to take off again
    };

    struct Victim
    {
        ObjectGuid Player;
        std::string Name;
        uint32 Times = 0;
        time_t LastAt = 0;
    };

    struct Rumour
    {
        time_t At = 0;
        uint32 ZoneId = 0;
        std::string Text;
    };

    struct Struggle
    {
        ObjectGuid Challenger;
        ObjectGuid Defender;
        uint32 ZoneId = 0;
        time_t ResolveAt = 0;
    };

    struct KnownTraits
    {
        uint32 Strengths = 0;
        uint32 Weaknesses = 0;
    };

    std::shared_mutex g_lock;
    std::unordered_map<uint64, Record> g_records;                                   // bot -> record
    std::unordered_map<uint64, std::vector<Victim>> g_victims;                      // bot -> who it killed
    std::unordered_map<uint64, std::unordered_map<uint64, KnownTraits>> g_intel;    // person -> bot -> known
    std::deque<Rumour> g_rumours;                                                   // newest first
    std::vector<Struggle> g_struggles;
    std::atomic<bool> g_anyActive{ false };
    std::atomic<bool> g_anyHunt{ false };
    bool s_tablesReady = false;

    // Messages for people who were not standing where it happened.
    std::mutex g_outboxLock;
    std::vector<std::pair<ObjectGuid, std::string>> g_outbox;

    // Taunt clocks: (bot, person) and per bot.
    std::mutex g_tauntLock;
    std::unordered_map<uint64, time_t> g_pairTauntAt;
    std::unordered_map<uint64, time_t> g_botTauntAt;

    constexpr size_t kMaxRumours = 40;

    bool Active(Record const& record)
    {
        return !record.Fallen && record.Rank > 0;
    }

    void UpdateAnyLocked()
    {
        bool any = false;
        bool hunt = false;
        for (auto const& [raw, record] : g_records)
        {
            any = any || Active(record);
            hunt = hunt || !record.Hunting.IsEmpty();
        }
        g_anyActive.store(any, std::memory_order_relaxed);
        g_anyHunt.store(hunt, std::memory_order_relaxed);
    }

    bool On()
    {
        return s_cfg.Enabled && s_tablesReady;
    }

    // ---------------------------------------------------------------- words

    std::string Banner(std::string const& text)
    {
        return "|cffc41f3b[Nemesis]|r " + text;
    }

    void Tell(Player* player, std::string const& text)
    {
        if (player && player->GetSession() && s_cfg.Whisper)
            ChatHandler(player->GetSession()).SendSysMessage(Banner(text).c_str());
    }

    void TellLater(ObjectGuid who, std::string const& text)
    {
        if (!s_cfg.Whisper)
            return;
        std::lock_guard<std::mutex> guard(g_outboxLock);
        g_outbox.emplace_back(who, text);
    }

    void Shout(Player* bot, std::string const& text)
    {
        if (bot && s_cfg.Yell && bot->IsInWorld())
            bot->Yell(text, LANG_UNIVERSAL);
    }

    std::string Scrubbed(std::string text)
    {
        for (char& c : text)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';
        return text;
    }

    std::string Roman(uint8 rank)
    {
        static char const* const numerals[] = { "0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X" };
        return rank <= 10 ? numerals[rank] : std::to_string(rank);
    }

    std::string Display(Record const& record)
    {
        if (record.Epithet.empty())
            return record.Name;
        bool const spaced = record.Epithet.rfind("the ", 0) == 0 || record.Epithet.rfind("of ", 0) == 0;
        return record.Name + (spaced ? " " : ", ") + record.Epithet;
    }

    std::string ClassWord(uint8 cls)
    {
        ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(cls);
        return (entry && entry->Name[LOCALE_enUS]) ? std::string(entry->Name[LOCALE_enUS]) : std::string();
    }

    std::string TraitLine(char const* kind, TraitText const& trait)
    {
        return Trinity::StringFormat("{} {} - {}", kind, trait.Name, trait.Effect);
    }

    // ---------------------------------------------------------------- epithets

    std::string RiseEpithet(Player const* victim, Record const& record)
    {
        std::string const who = victim ? victim->GetName() : "Nobody";
        uint32 const roll = urand(0, 99);

        if (roll < 40)
        {
            switch (urand(0, 3))
            {
                case 0:  return "the " + who + "-Slayer";
                case 1:  return who + "bane";
                case 2:  return "the " + who + "-Breaker";
                default: return "the Bane of " + who;
            }
        }

        if (roll < 70 && victim)
        {
            switch (victim->GetClass())
            {
                case CLASS_WARRIOR:      return "the Shield-Breaker";
                case CLASS_PALADIN:      return "the Light-Snuffer";
                case CLASS_HUNTER:       return "the Hound-Killer";
                case CLASS_ROGUE:        return "the Knife-Catcher";
                case CLASS_PRIEST:       return "the Priest-Flayer";
                case CLASS_DEATH_KNIGHT: return "the Grave-Robber";
                case CLASS_SHAMAN:       return "the Totem-Smasher";
                case CLASS_MAGE:         return "the Mage-Eater";
                case CLASS_WARLOCK:      return "the Soul-Taker";
                case CLASS_DRUID:        return "the Bear-Skinner";
                default: break;
            }
        }

        if (roll < 85 && record.ZoneId)
            return "the Terror of " + BountyBoard::ZoneName(record.ZoneId);

        for (uint8 bit = 0; bit < S_COUNT; ++bit)
            if (record.Strengths & (1u << bit))
                return Strengths[bit].Epithet;

        return "the " + who + "-Slayer";
    }

    std::string ScarEpithet(uint8 scars)
    {
        static char const* const pool[] = { "Scarface", "the Twice-Dead", "the Unkillable", "Death-Cheater",
            "the Stitched", "Grave-Spurner", "the Returned" };
        if (scars >= 3)
            return "the Unkillable";
        return pool[urand(0, uint32(std::size(pool) - 1))];
    }

    // ---------------------------------------------------------------- traits

    template <size_t N>
    bool RollTrait(uint32& mask, TraitText const (&/*table*/)[N])
    {
        std::vector<uint8> free;
        for (uint8 bit = 0; bit < N; ++bit)
            if (!(mask & (1u << bit)))
                free.push_back(bit);
        if (free.empty())
            return false;
        mask |= 1u << free[urand(0, uint32(free.size() - 1))];
        return true;
    }

    uint32 CountBits(uint32 mask)
    {
        uint32 n = 0;
        for (; mask; mask &= mask - 1)
            ++n;
        return n;
    }

    // Which crowd control a nemesis shrugs off, as mechanic bits.
    uint32 ImmunityMechanics(uint32 strengths)
    {
        uint32 mechanics = 0;
        if (strengths & (1u << S_FEARLESS))
            mechanics |= (1u << MECHANIC_FEAR) | (1u << MECHANIC_HORROR);
        if (strengths & (1u << S_UNSTOPPABLE))
            mechanics |= 1u << MECHANIC_STUN;
        if (strengths & (1u << S_UNBOUND))
            mechanics |= (1u << MECHANIC_ROOT) | (1u << MECHANIC_SNARE);
        if (strengths & (1u << S_UNBOWED))
            mechanics |= (1u << MECHANIC_SILENCE) | (1u << MECHANIC_POLYMORPH);
        return mechanics;
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

    void SaveRecord(Record const& record)
    {
        if (!s_tablesReady)
            return;

        std::string epithet = record.Epithet;
        std::string name = record.Name;
        std::string slainBy = record.SlainByName;
        CharacterDatabase.EscapeString(epithet);
        CharacterDatabase.EscapeString(name);
        CharacterDatabase.EscapeString(slainBy);
        CharacterDatabase.PExecute(
            "REPLACE INTO character_bot_nemesis (guid, name, level, class, `rank`, epithet, kills, strengths, weaknesses, "
            "scars, zoneId, risenAt, fallen, fallenAt, returnAt, slainBy, slainByName) "
            "VALUES ({}, '{}', {}, {}, {}, '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}')",
            record.Bot.GetCounter(), name, uint32(record.Level), uint32(record.Class), uint32(record.Rank), epithet,
            record.Kills, record.Strengths, record.Weaknesses, uint32(record.Scars), record.ZoneId,
            uint32(record.RisenAt), record.Fallen ? 1 : 0, uint32(record.FallenAt), uint32(record.ReturnAt),
            record.SlainBy.GetCounter(), slainBy);
    }

    void DeleteRecord(ObjectGuid bot)
    {
        if (!s_tablesReady)
            return;
        CharacterDatabase.PExecute("DELETE FROM character_bot_nemesis WHERE guid = {}", bot.GetCounter());
        CharacterDatabase.PExecute("DELETE FROM character_bot_nemesis_victim WHERE bot = {}", bot.GetCounter());
    }

    void SaveVictim(ObjectGuid bot, Victim const& victim)
    {
        if (!s_tablesReady)
            return;
        std::string name = victim.Name;
        CharacterDatabase.EscapeString(name);
        CharacterDatabase.PExecute(
            "REPLACE INTO character_bot_nemesis_victim (bot, victim, victimName, times, lastAt) VALUES ({}, {}, '{}', {}, {})",
            bot.GetCounter(), victim.Player.GetCounter(), name, victim.Times, uint32(victim.LastAt));
    }

    void SaveIntel(ObjectGuid player, ObjectGuid bot, KnownTraits const& known)
    {
        if (!s_tablesReady)
            return;
        CharacterDatabase.PExecute(
            "REPLACE INTO character_nemesis_intel (guid, bot, strengths, weaknesses) VALUES ({}, {}, {}, {})",
            player.GetCounter(), bot.GetCounter(), known.Strengths, known.Weaknesses);
    }

    // With g_lock held exclusively.
    void AddRumourLocked(uint32 zoneId, std::string const& text)
    {
        Rumour rumour;
        rumour.At = GameTime::GetGameTime();
        rumour.ZoneId = zoneId;
        rumour.Text = text;
        g_rumours.push_front(rumour);
        while (g_rumours.size() > kMaxRumours)
            g_rumours.pop_back();

        if (!s_tablesReady)
            return;
        std::string escaped = text;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.PExecute("INSERT INTO character_bot_nemesis_rumour (at, zoneId, text) VALUES ({}, {}, '{}')",
            uint32(rumour.At), zoneId, escaped);
    }

    // ---------------------------------------------------------------- intel

    // Teach a person one trait they do not know yet. Strength only, or either.
    // With g_lock held exclusively. Returns the line to tell them, or empty;
    // `outStrength` and `outBit` say which it was, to teach others the same.
    std::string RevealLocked(ObjectGuid person, Record const& record, bool strengthOnly,
        bool* outStrength = nullptr, uint8* outBit = nullptr)
    {
        KnownTraits& known = g_intel[person.GetRawValue()][record.Bot.GetRawValue()];

        std::vector<std::pair<bool, uint8>> unknown;   // (is strength, bit)
        for (uint8 bit = 0; bit < S_COUNT; ++bit)
            if ((record.Strengths & (1u << bit)) && !(known.Strengths & (1u << bit)))
                unknown.emplace_back(true, bit);
        if (!strengthOnly)
            for (uint8 bit = 0; bit < W_COUNT; ++bit)
                if ((record.Weaknesses & (1u << bit)) && !(known.Weaknesses & (1u << bit)))
                    unknown.emplace_back(false, bit);

        if (unknown.empty())
            return std::string();

        auto const [strength, bit] = unknown[urand(0, uint32(unknown.size() - 1))];
        if (strength)
            known.Strengths |= 1u << bit;
        else
            known.Weaknesses |= 1u << bit;
        SaveIntel(person, record.Bot, known);

        if (outStrength)
            *outStrength = strength;
        if (outBit)
            *outBit = bit;
        return strength ? TraitLine("strength:", Strengths[bit]) : TraitLine("weakness:", Weaknesses[bit]);
    }

    // The same trait, taught to somebody who saw it happen. True when it was
    // news to them. With g_lock held exclusively.
    bool TeachLocked(ObjectGuid person, Record const& record, bool strength, uint8 bit)
    {
        KnownTraits& known = g_intel[person.GetRawValue()][record.Bot.GetRawValue()];
        uint32& mask = strength ? known.Strengths : known.Weaknesses;
        if (mask & (1u << bit))
            return false;
        mask |= 1u << bit;
        SaveIntel(person, record.Bot, known);
        return true;
    }

    KnownTraits KnownLocked(ObjectGuid person, ObjectGuid bot)
    {
        auto const itr = g_intel.find(person.GetRawValue());
        if (itr == g_intel.end())
            return KnownTraits();
        auto const known = itr->second.find(bot.GetRawValue());
        return known != itr->second.end() ? known->second : KnownTraits();
    }

    // ---------------------------------------------------------------- the bot's body

    // Scale and immunities follow the record and the switches. With the bot in
    // hand on its own thread (or the world thread, maps idle), g_lock exclusive.
    void ApplyBodyLocked(Player* bot, Record& record)
    {
        bool const active = On() && Active(record);

        uint32 const want = (active && s_cfg.Traits) ? ImmunityMechanics(record.Strengths) : 0;
        if (want != record.AppliedImmunities)
        {
            for (uint32 mechanic = 0; mechanic < 32; ++mechanic)
            {
                uint32 const bit = 1u << mechanic;
                if ((want & bit) != (record.AppliedImmunities & bit))
                    bot->ApplySpellImmune(0, IMMUNITY_MECHANIC, mechanic, (want & bit) != 0);
            }
            record.AppliedImmunities = want;
        }

        float factor = 1.0f;
        if (active && s_cfg.Scale)
            factor += s_cfg.ScalePerRank * float(record.Rank) + (record.Warchief ? s_cfg.ScaleWarchief : 0.0f);

        if (factor != 1.0f || record.ScaleApplied)
        {
            // On top of whatever the bot's own auras say, so a shrink or a grow
            // effect still works on a nemesis.
            int32 const auras = bot->GetTotalAuraModifier(SPELL_AURA_MOD_SCALE) +
                bot->GetTotalAuraModifier(SPELL_AURA_MOD_SCALE_2);
            float const target = std::max(0.1f, (1.0f + float(auras) / 100.0f) * factor);
            if (std::abs(bot->GetObjectScale() - target) > 0.001f)
                bot->SetObjectScale(target);
            record.ScaleApplied = factor != 1.0f;
        }
    }

    // ---------------------------------------------------------------- guards

    void ReleaseGuardsLocked(Record& record)
    {
        for (ObjectGuid guard : record.Guards)
        {
            playerbot::PveManager::ClearNoticeErrand(guard);
            playerbot::PveManager::SetNoticeHeld(guard, false);
        }
        record.Guards.clear();
    }

    uint32 WantedGuards(Record const& record)
    {
        if (!On() || !Active(record) || !s_cfg.Guards)
            return 0;
        uint32 n = 0;
        if (record.Warchief && s_cfg.Warchiefs)
            n += s_cfg.WarchiefGuards;
        if (s_cfg.Traits && (record.Strengths & (1u << S_BODYGUARD)))
            n += 1;
        return n;
    }

    // World thread.
    void TendGuardsLocked(Player* bot, Record& record)
    {
        // Gone, or no longer guarding: let them go.
        record.Guards.erase(std::remove_if(record.Guards.begin(), record.Guards.end(), [](ObjectGuid guard)
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(guard);
            if (p && p->IsInWorld() && playerbot::PveManager::HasNoticeErrand(guard))
                return false;
            playerbot::PveManager::SetNoticeHeld(guard, false);
            return true;
        }), record.Guards.end());

        uint32 const want = bot ? WantedGuards(record) : 0;
        while (record.Guards.size() > want)
        {
            playerbot::PveManager::ClearNoticeErrand(record.Guards.back());
            playerbot::PveManager::SetNoticeHeld(record.Guards.back(), false);
            record.Guards.pop_back();
        }

        if (!bot || record.Guards.size() >= want)
            return;

        std::vector<std::pair<float, ObjectGuid>> candidates;
        for (auto const& [guid, other] : ObjectAccessor::GetPlayers())
        {
            if (other == bot || !BountyBoard::IsOnTheBoard(other) || other->IsInCombat())
                continue;
            if (other->GetMapId() != bot->GetMapId() || other->GetZoneId() != bot->GetZoneId())
                continue;
            if (std::abs(int32(other->GetLevel()) - int32(bot->GetLevel())) > 6)
                continue;
            if (playerbot::PveManager::IsNoticeHeld(other->GetGUID()))
                continue;
            if (g_records.count(other->GetGUID().GetRawValue()))
                continue;   // a nemesis serves nobody
            float const distance = other->GetDistance(bot);
            if (distance <= 400.0f)
                candidates.emplace_back(distance, other->GetGUID());
        }
        std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

        for (auto const& [distance, guard] : candidates)
        {
            if (record.Guards.size() >= want)
                break;
            playerbot::PveManager::SetNoticeErrand(guard, playerbot::PveManager::NoticeErrandKind::Escort, record.Bot);
            playerbot::PveManager::SetNoticeHeld(guard, true);
            record.Guards.push_back(guard);
        }
    }

    // ---------------------------------------------------------------- groups

    // The people in `who`'s party or raid standing near `where` - on its map,
    // inside `yards` - not counting `who`. On a map thread only people on that
    // map are touched; on the world thread, anybody.
    std::vector<Player*> PartyNear(Player* who, WorldObject const* where, float yards)
    {
        std::vector<Player*> out;
        Group* group = who ? who->GetGroup() : nullptr;
        if (!group || !where)
            return out;

        for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == who || !member->IsInMap(where) || !IsPerson(member) ||
                !member->IsWithinDistInMap(where, yards))
                continue;
            out.push_back(member);
        }
        return out;
    }

    // Up to `by` ranks, with a new strength on reaching the third and the fifth.
    void RaiseLocked(Record& record, uint32 by)
    {
        for (uint32 i = 0; i < by && record.Rank < s_cfg.MaxRank; ++i)
        {
            ++record.Rank;
            if (record.Rank == 3 || record.Rank == 5)
                RollTrait(record.Strengths, Strengths);
        }
    }

    // ---------------------------------------------------------------- the revenant

    bool HuntingLocked(Record const& record)
    {
        return !record.Hunting.IsEmpty();
    }

    // The feud a revenant comes back with: its slayer - and, by the switch,
    // everybody in its slayer's party, who may fight it War Mode or not.
    void PutHuntStance(Record const& record)
    {
        if (s_cfg.HuntGroupStance)
            PairStance::SetShared(record.Bot, record.Hunting, PairStance::Stance::Feud);
        else
            PairStance::Set(record.Bot, record.Hunting, PairStance::Stance::Feud);
    }

    // Everything a hunt put on the realm, taken back. With g_lock exclusive. The
    // bot only when it is in hand - its own map thread, or the world thread
    // with the maps idle - because its look comes off it.
    void EndHuntLocked(Record& record, Player* bot)
    {
        if (!HuntingLocked(record))
            return;

        PairStance::Clear(record.Bot, record.Hunting);
        playerbot::PveManager::ClearNoticeErrand(record.Bot);
        playerbot::PveManager::SetNoticeHeld(record.Bot, false);
        if (bot && record.HuntVisual)
            bot->RemoveAurasDueToSpell(record.HuntVisual);

        record.Hunting.Clear();
        record.HuntingName.clear();
        record.HuntUntil = 0;
        record.HuntStage = 0;
        record.HuntAllies = 0;
        record.HuntParty.clear();
        record.HuntVisual = 0;
    }

    // Where a revenant may come for its slayer: out in the open world and on
    // their feet; not resting at an inn or in a city, not in the air, a
    // sanctuary or a duel; and - unless the switch says otherwise - not in a
    // raid. It does not walk into an army.
    bool Exposed(Player* slayer)
    {
        if (!slayer || !slayer->IsInWorld() || !slayer->IsAlive() || !IsPerson(slayer) || !InOpenWorld(slayer))
            return false;
        if (slayer->IsInFlight() || slayer->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) ||
            slayer->pvpInfo.IsInNoPvPArea || slayer->duel)
            return false;
        if (!s_cfg.StrikeInRaid)
            if (Group const* group = slayer->GetGroup(); group && group->isRaidGroup())
                return false;
        return true;
    }

    // ---------------------------------------------------------------- rising

    // Map thread: a world bot has killed a person, both in hand.
    void OnBotKilledPerson(Player* bot, Player* victim)
    {
        if (!On())
            return;

        if (s_cfg.MaxLevelGap && uint32(victim->GetLevel()) + s_cfg.MaxLevelGap < uint32(bot->GetLevel()))
            return;   // no glory in it

        time_t const now = GameTime::GetGameTime();
        uint32 const zone = bot->GetZoneId();

        // Who watched it happen, to learn from it.
        std::vector<Player*> const party = (s_cfg.Intel && s_cfg.IntelOnDeath && s_cfg.Traits && s_cfg.GroupIntel)
            ? PartyNear(victim, victim, s_cfg.CreditYards) : std::vector<Player*>();

        enum class Outcome : uint8 { Rose, Climbed, Held, Revenge, Ambush };
        Outcome outcome = Outcome::Held;
        std::string yell, toVictim, rumour, learned, bossLine;
        std::vector<Player*> learnedToo;
        uint8 rank = 0;

        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            uint64 const botRaw = bot->GetGUID().GetRawValue();

            // Down, and waiting to come back for its slayer. Nobody else's death
            // stirs it - but its slayer, run into by chance first, is another
            // matter.
            if (auto const waiting = g_records.find(botRaw); waiting != g_records.end() &&
                waiting->second.Fallen && waiting->second.ReturnAt && waiting->second.SlainBy != victim->GetGUID())
                return;

            Record& record = g_records[botRaw];
            std::vector<Victim>& victims = g_victims[botRaw];
            auto vitr = std::find_if(victims.begin(), victims.end(),
                [&](Victim const& v) { return v.Player == victim->GetGUID(); });
            time_t const lastAt = vitr != victims.end() ? vitr->LastAt : 0;

            if (record.Bot.IsEmpty() || (record.Fallen && !record.ReturnAt))
            {
                // Gone for good, or never was: a fresh nemesis.
                record = Record();
                record.Bot = bot->GetGUID();
                record.Rank = 1;
                record.ZoneId = zone;
                record.RisenAt = now;
                record.ClimbedAt = now;
                RollTrait(record.Strengths, Strengths);
                RollTrait(record.Weaknesses, Weaknesses);
                victims.clear();
                vitr = victims.end();
                outcome = Outcome::Rose;
            }
            else if (record.Fallen)
            {
                // Its slayer, before it could come for them.
                record.Fallen = false;
                record.ReturnAt = 0;
                ++record.Scars;
                record.Epithet = ScarEpithet(record.Scars);
                RaiseLocked(record, s_cfg.RevengeRanks);
                record.ClimbedAt = now;
                outcome = Outcome::Ambush;
            }
            else if (HuntingLocked(record) && record.Hunting == victim->GetGUID())
            {
                RaiseLocked(record, s_cfg.RevengeRanks);
                record.ClimbedAt = now;
                EndHuntLocked(record, bot);
                outcome = Outcome::Revenge;
            }
            else
            {
                // One rank a fight, however many of a party fall in it, and
                // never off the same person again this soon: nobody feeds a
                // nemesis its ranks.
                bool const sameFight = s_cfg.ClimbFightSeconds && now - record.ClimbedAt < time_t(s_cfg.ClimbFightSeconds);
                bool const sameVictim = s_cfg.SameVictimMinutes && lastAt &&
                    now - lastAt < time_t(s_cfg.SameVictimMinutes) * MINUTE;
                if (!sameFight && !sameVictim && record.Rank < s_cfg.MaxRank)
                {
                    RaiseLocked(record, 1);
                    record.ClimbedAt = now;
                    outcome = Outcome::Climbed;
                }
            }

            record.Name = bot->GetName();
            record.Level = bot->GetLevel();
            record.Class = bot->GetClass();
            record.ZoneId = zone;
            ++record.Kills;
            if (outcome == Outcome::Rose)
                record.Epithet = RiseEpithet(victim, record);

            if (vitr == victims.end())
            {
                victims.push_back(Victim());
                vitr = victims.end() - 1;
                vitr->Player = victim->GetGUID();
            }
            vitr->Name = victim->GetName();
            ++vitr->Times;
            vitr->LastAt = now;
            uint32 const times = vitr->Times;

            rank = record.Rank;
            std::string const display = Display(record);
            std::string const zoneName = BountyBoard::ZoneName(zone);
            std::string const name = victim->GetName();

            switch (outcome)
            {
                case Outcome::Rose:
                    switch (urand(0, 2))
                    {
                        case 0:  yell = Trinity::StringFormat("{} falls! Remember the name: {}!", name, display); break;
                        case 1:  yell = Trinity::StringFormat("Tell them who did this, {}. Tell them it was {}.", name, record.Name); break;
                        default: yell = Trinity::StringFormat("Another notice for the board, {}. Mine, this time.", name); break;
                    }
                    toVictim = Trinity::StringFormat("{} killed you, and has become your nemesis. It will remember you.", display);
                    rumour = Trinity::StringFormat("{} killed {} in {} and has become a nemesis.", display, name, zoneName);
                    break;
                case Outcome::Climbed:
                    if (times > 1)
                    {
                        yell = Trinity::StringFormat("{} again? I have killed you {} times now. It is getting to be a habit.",
                            name, times);
                        toVictim = Trinity::StringFormat("{} has killed you again ({} times). Rank {}.", display, times, Roman(rank));
                        rumour = Trinity::StringFormat("{} killed {} again in {}, and stands at rank {}.", display, name,
                            zoneName, Roman(rank));
                    }
                    else
                    {
                        yell = Trinity::StringFormat("Rank by rank, corpse by corpse. {} climbs!", record.Name);
                        toVictim = Trinity::StringFormat("{} killed you, and grows stronger: rank {}.", display, Roman(rank));
                        rumour = Trinity::StringFormat("{} killed {} in {} and climbs to rank {}.", display, name,
                            zoneName, Roman(rank));
                    }
                    break;
                case Outcome::Held:
                    // Another of a party it already climbed on, or somebody it
                    // had only just killed. No rumour: the board would be all wipes.
                    if (HuntingLocked(record))
                        yell = Trinity::StringFormat("Out of my way, {}. It is {} I want.", name, record.HuntingName);
                    else if (urand(0, 1))
                        yell = Trinity::StringFormat("{} falls. Who is next?", name);
                    else
                        yell = Trinity::StringFormat("Is that all you had, {}?", name);
                    toVictim = times > 1
                        ? Trinity::StringFormat("{} has killed you again ({} times).", display, times)
                        : Trinity::StringFormat("{} killed you.", display);
                    break;
                case Outcome::Revenge:
                    switch (urand(0, 2))
                    {
                        case 0:  yell = Trinity::StringFormat("Now we are even, {}.", name); break;
                        case 1:  yell = Trinity::StringFormat("Rest now, {}. I know how cold the ground is.", name); break;
                        default: yell = Trinity::StringFormat("I told you I would find you, {}. I always find you.", name); break;
                    }
                    bossLine = Trinity::StringFormat("{} has had its revenge.", record.Name);
                    toVictim = Trinity::StringFormat("{} came back from the grave for you, and had its revenge. Rank {}.",
                        display, Roman(rank));
                    rumour = Trinity::StringFormat("{} came back from the dead for {} in {}, and killed them.", display, name,
                        zoneName);
                    break;
                case Outcome::Ambush:
                    if (urand(0, 1))
                        yell = Trinity::StringFormat("{}! Did you think you had killed me?", name);
                    else
                        yell = Trinity::StringFormat("Surprised, {}? The dead do not stay down for the likes of you.", name);
                    bossLine = Trinity::StringFormat("{} was not as dead as you thought.", record.Name);
                    toVictim = Trinity::StringFormat("{} was not as dead as you thought, and had its revenge. Rank {}.",
                        display, Roman(rank));
                    rumour = Trinity::StringFormat("{}, thought dead, came back and killed {} in {}.", display, name, zoneName);
                    break;
            }

            // Dying to it is one way of finding out what it can do - and the
            // party watching finds out with you.
            if (s_cfg.Intel && s_cfg.IntelOnDeath && s_cfg.Traits)
            {
                bool strength = false;
                uint8 bit = 0;
                learned = RevealLocked(victim->GetGUID(), record, true, &strength, &bit);
                if (!learned.empty())
                    for (Player* member : party)
                        if (TeachLocked(member->GetGUID(), record, strength, bit))
                            learnedToo.push_back(member);
            }

            ApplyBodyLocked(bot, record);
            if (!rumour.empty())
                AddRumourLocked(zone, rumour);
            SaveRecord(record);
            SaveVictim(bot->GetGUID(), *vitr);
            UpdateAnyLocked();
        }

        Shout(bot, yell);
        Tell(victim, toVictim);
        if (!bossLine.empty() && s_cfg.Whisper)
            bot->Unit::Whisper(bossLine, LANG_UNIVERSAL, victim, true);
        if (!learned.empty())
            Tell(victim, Trinity::StringFormat("You learned the hard way - {}", learned));
        for (Player* member : learnedToo)
            Tell(member, Trinity::StringFormat("{} learned it the hard way, and you were watching - {}", victim->GetName(), learned));

        TC_LOG_INFO("scripts", "Nemesis: {} killed {} in zone {} - outcome {} (rank {}).",
            bot->GetName(), victim->GetName(), zone, uint32(outcome), uint32(rank));
    }

    // ---------------------------------------------------------------- falling

    // Map thread: a world bot has died. If a person had a hand in it and it was
    // a nemesis, it falls; either way, a bot dying near a nemesis may talk.
    // `people` are everybody who shares the kill - the fighters and, by the
    // switch, their parties close by.
    void OnBotDied(Player* bot, std::vector<Player*> const& people, Player* killer)
    {
        if (!On())
            return;

        time_t const now = GameTime::GetGameTime();
        uint32 const zone = bot->GetZoneId();
        std::string const zoneName = BountyBoard::ZoneName(zone);

        // --- a nemesis falls
        std::string display, name;
        uint8 rank = 0;
        bool warchief = false;
        bool rested = false;            // a revenant, laid to rest
        ObjectGuid hunted;              // ...and the one it came back for
        std::vector<Victim> avenged;
        bool fell = false;
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            auto const itr = g_records.find(bot->GetGUID().GetRawValue());
            if (itr != g_records.end() && Active(itr->second) && !people.empty())
            {
                Record& record = itr->second;
                display = Display(record);
                name = record.Name;
                rank = record.Rank;
                warchief = record.Warchief;
                fell = true;

                Player* slayer = killer && IsPerson(killer) ? killer : people.front();

                rested = HuntingLocked(record);
                hunted = record.Hunting;
                EndHuntLocked(record, bot);

                record.Fallen = true;
                record.FallenAt = now;
                record.SlainBy = slayer->GetGUID();
                record.SlainByName = slayer->GetName();
                record.ReturnAt = 0;

                // Whether it will come back for them, some hours from now. Never
                // after it already did: put down twice, it stays down.
                if (s_cfg.Returns && !rested)
                {
                    int32 const chance = std::clamp(int32(s_cfg.ReturnChance) +
                        int32(s_cfg.ReturnChancePerRank) * int32(record.Rank) -
                        int32(s_cfg.ReturnChanceLossPerScar) * int32(record.Scars), 0, 95);
                    if (chance && roll_chance_i(chance))
                        record.ReturnAt = now + time_t(urand(s_cfg.ReturnMinHours * HOUR, s_cfg.ReturnMaxHours * HOUR + 1));
                }

                record.Warchief = false;
                ReleaseGuardsLocked(record);
                ApplyBodyLocked(bot, record);

                if (auto const v = g_victims.find(bot->GetGUID().GetRawValue()); v != g_victims.end())
                    avenged = v->second;

                AddRumourLocked(zone, rested
                    ? Trinity::StringFormat("{} laid {} to rest in {}. It will not rise again.", slayer->GetName(), display,
                        zoneName)
                    : Trinity::StringFormat("{} slew {}{} in {}.", slayer->GetName(), display,
                        warchief ? Trinity::StringFormat(", Warchief of {},", zoneName) : std::string(), zoneName));
                SaveRecord(record);
                UpdateAnyLocked();
            }
        }

        if (fell)
        {
            if (rested)
            {
                static char const* const restWords[] = { "No... not again... not... again...", "The ground... is so cold...",
                    "Finally... I can... rest..." };
                Shout(bot, restWords[urand(0, 2)]);

                // The last word is for the one it came back for.
                Player* huntedHere = nullptr;
                for (Player* person : people)
                    if (person->GetGUID() == hunted)
                        huntedHere = person;
                std::string const rest = Trinity::StringFormat("{} is laid to rest. It will not rise again.", name);
                if (huntedHere && s_cfg.Whisper)
                    bot->Unit::Whisper(rest, LANG_UNIVERSAL, huntedHere, true);
                else if (!hunted.IsEmpty())
                    TellLater(hunted, rest);
            }
            else
            {
                // The same last words whether it will be back or not.
                static char const* const lastWords[] = { "This... is not... the end...", "Remember... my name...",
                    "You... got lucky..." };
                Shout(bot, lastWords[urand(0, 2)]);
            }

            uint32 const mult = (warchief ? s_cfg.WarchiefRewardMultiplier : 1) * (rested ? s_cfg.RestRewardMultiplier : 1);
            uint32 const money = s_cfg.Rewards ? s_cfg.MoneyPerRank * rank * mult : 0;
            uint32 const seals = s_cfg.Rewards ? s_cfg.SealsPerRank * rank * mult : 0;
            for (Player* person : people)
            {
                // No purse for putting down something far beneath you.
                if (!s_cfg.Rewards || bot->GetLevel() <= Trinity::XP::GetGrayLevel(person->GetLevel()))
                {
                    Tell(person, Trinity::StringFormat("{} is dead.", display));
                    continue;
                }
                if (money)
                    person->ModifyMoney(int64(money));
                bool const sealed = seals && BountyNotices::AddSeals(person, seals);
                Tell(person, Trinity::StringFormat("{} is dead. {}{}{}", display,
                    money ? Trinity::StringFormat("{}g {}s {}c", money / 10000, (money % 10000) / 100, money % 100) : "",
                    money && sealed ? ", " : "", sealed ? Trinity::StringFormat("{} seal{}", seals, seals == 1 ? "" : "s") : ""));
            }

            if (s_cfg.Vengeance && !avenged.empty())
            {
                std::unordered_set<uint64> present;
                for (Player* person : people)
                    present.insert(person->GetGUID().GetRawValue());

                std::string const slayerName = (killer && IsPerson(killer)) ? killer->GetName() : people.front()->GetName();
                std::string names;
                for (Victim const& victim : avenged)
                {
                    if (present.count(victim.Player.GetRawValue()))
                        continue;
                    TellLater(victim.Player, Trinity::StringFormat("{} has avenged you: {} is dead.", slayerName, display));
                    names += (names.empty() ? "" : ", ") + victim.Name;
                }
                if (!names.empty())
                    for (Player* person : people)
                        Tell(person, Trinity::StringFormat("You avenged {}.", names));
            }

            TC_LOG_INFO("scripts", "Nemesis: {} (rank {}) was slain in zone {}{}.", display, uint32(rank), zone,
                rested ? " - laid to rest" : "");
        }

        // --- a bot killed in a nemesis's zone talks before it dies
        if (!s_cfg.Intel || !s_cfg.Traits || !killer || !IsPerson(killer) || !roll_chance_i(int32(s_cfg.IntelZoneKillChance)))
            return;

        std::vector<Player*> const party = s_cfg.GroupIntel ? PartyNear(killer, bot, s_cfg.CreditYards) : std::vector<Player*>();
        std::string told, about;
        std::vector<Player*> toldToo;
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            std::vector<Record*> local;
            for (auto& [raw, record] : g_records)
                if (Active(record) && record.ZoneId == zone && record.Bot != bot->GetGUID())
                    local.push_back(&record);
            std::shuffle(local.begin(), local.end(), RandomEngine::Instance());
            for (Record* record : local)
            {
                bool strength = false;
                uint8 bit = 0;
                told = RevealLocked(killer->GetGUID(), *record, false, &strength, &bit);
                if (!told.empty())
                {
                    about = Display(*record);
                    for (Player* member : party)
                        if (TeachLocked(member->GetGUID(), *record, strength, bit))
                            toldToo.push_back(member);
                    break;
                }
            }
        }

        if (!told.empty())
        {
            Tell(killer, Trinity::StringFormat("Before {} died, they gasped out a secret about {} - {}",
                bot->GetName(), about, told));
            for (Player* member : toldToo)
                Tell(member, Trinity::StringFormat("You heard it too: before {} died, they gasped out a secret about {} - {}",
                    bot->GetName(), about, told));
        }
    }

    // ---------------------------------------------------------------- power

    // The percentage this hit lands at, and a taunt if one is due. One side a
    // nemesis bot and the other a person, or 100.
    uint32 HitPct(Unit* target, Unit* attacker, SpellSchoolMask school, Player** tauntBot, Player** tauntPerson)
    {
        if (!g_anyActive.load(std::memory_order_relaxed) || !s_cfg.Enabled)
            return 100;
        if (!target || !attacker || target == attacker || !target->IsControlledByPlayer() || !attacker->IsControlledByPlayer())
            return 100;

        Player* attackerPlayer = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player* targetPlayer = target->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!attackerPlayer || !targetPlayer || attackerPlayer == targetPlayer)
            return 100;

        bool const attackerIsBot = BarracksHardcore::IsPlayerbot(attackerPlayer);
        if (attackerIsBot == BarracksHardcore::IsPlayerbot(targetPlayer))
            return 100;

        Player* bot = attackerIsBot ? attackerPlayer : targetPlayer;
        Player* person = attackerIsBot ? targetPlayer : attackerPlayer;

        int32 pct = 100;
        {
            std::shared_lock<std::shared_mutex> guard(g_lock);
            auto const itr = g_records.find(bot->GetGUID().GetRawValue());
            if (itr == g_records.end() || !Active(itr->second))
                return 100;
            Record const& record = itr->second;

            int32 const warchief = (record.Warchief && s_cfg.Warchiefs) ? int32(s_cfg.WarchiefPowerBonus) : 0;
            uint32 const s = s_cfg.Traits ? record.Strengths : 0;
            uint32 const w = s_cfg.Traits ? record.Weaknesses : 0;

            // A revenant on the hunt: harder on its slayer, and harder on all
            // of them for every friend standing at the slayer's side.
            bool const prey = HuntingLocked(record) && record.Hunting == person->GetGUID();
            bool const party = prey || (HuntingLocked(record) &&
                std::find(record.HuntParty.begin(), record.HuntParty.end(), person->GetGUID()) != record.HuntParty.end());
            int32 const allies = party ? int32(s_cfg.PowerPerAlly * std::min(record.HuntAllies, s_cfg.MaxAllies)) : 0;

            if (attackerIsBot)
            {
                if (s_cfg.Power)
                    pct += int32(s_cfg.DamagePerRank) * record.Rank + warchief;
                if ((s & (1u << S_EXECUTIONER)) && target->GetHealthPct() < 35.0f)
                    pct += 25;
                if ((s & (1u << S_BERSERKER)) && bot->GetHealthPct() < 50.0f)
                    pct += 20;
                if (w & (1u << W_CLUMSY))
                    pct -= 10;
                if (prey)
                    pct += int32(s_cfg.VendettaDamage);
                pct += allies;
            }
            else
            {
                bool const physical = (school & SPELL_SCHOOL_MASK_NORMAL) != 0;
                if (s_cfg.Power)
                    pct -= int32(s_cfg.ResistPerRank) * record.Rank + warchief;
                if ((s & (1u << S_IRON_HIDE)) && physical)
                    pct -= 20;
                if ((s & (1u << S_SPELLWARD)) && !physical)
                    pct -= 20;
                if ((w & (1u << W_THIN_SKINNED)) && physical)
                    pct += 20;
                if ((w & (1u << W_SPELL_SICK)) && !physical)
                    pct += 20;
                if ((w & (1u << W_FIRE)) && (school & SPELL_SCHOOL_MASK_FIRE))
                    pct += 30;
                if ((w & (1u << W_FROST)) && (school & SPELL_SCHOOL_MASK_FROST))
                    pct += 30;
                if ((w & (1u << W_SHADOW)) && (school & SPELL_SCHOOL_MASK_SHADOW))
                    pct += 30;
                if ((w & (1u << W_HOLY)) && (school & SPELL_SCHOOL_MASK_HOLY))
                    pct += 30;
                if ((w & (1u << W_NATURE)) && (school & SPELL_SCHOOL_MASK_NATURE))
                    pct += 30;
                if ((w & (1u << W_ARCANE)) && (school & SPELL_SCHOOL_MASK_ARCANE))
                    pct += 30;
                if ((w & (1u << W_FALTERS)) && bot->GetHealthPct() < 30.0f)
                    pct += 30;
                pct -= allies;
            }
        }

        if (tauntBot && s_cfg.Taunts && s_cfg.Yell)
        {
            *tauntBot = bot;
            *tauntPerson = person;
        }

        return uint32(std::clamp(pct, 25, 300));
    }

    // Map thread, the bot in hand. Rate-limited per pair and per bot.
    void MaybeTaunt(Player* bot, Player* person)
    {
        if (!bot || !person || !bot->IsAlive() || !s_cfg.Taunts || !s_cfg.Yell)
            return;

        time_t const now = GameTime::GetGameTime();
        uint64 const pairKey = bot->GetGUID().GetRawValue() * 1000003ull ^ person->GetGUID().GetRawValue();
        {
            std::lock_guard<std::mutex> guard(g_tauntLock);
            time_t& pairAt = g_pairTauntAt[pairKey];
            time_t& botAt = g_botTauntAt[bot->GetGUID().GetRawValue()];
            if (now < pairAt || now < botAt)
                return;
            pairAt = now + time_t(s_cfg.TauntCooldownMinutes) * MINUTE;
            botAt = now + 30;
        }

        std::string line;
        {
            std::shared_lock<std::shared_mutex> guard(g_lock);
            auto const itr = g_records.find(bot->GetGUID().GetRawValue());
            if (itr == g_records.end() || !Active(itr->second))
                return;
            Record const& record = itr->second;

            uint32 times = 0;
            if (auto const v = g_victims.find(bot->GetGUID().GetRawValue()); v != g_victims.end())
                for (Victim const& victim : v->second)
                    if (victim.Player == person->GetGUID())
                        times = victim.Times;

            if (record.Scars && record.SlainBy == person->GetGUID() && s_cfg.Returns)
                line = urand(0, 1)
                    ? Trinity::StringFormat("{}! You left me for dead. Look at me now!", person->GetName())
                    : Trinity::StringFormat("You should have finished the job, {}.", person->GetName());
            else if (times)
                line = urand(0, 1)
                    ? Trinity::StringFormat("{}! Back for more? I still remember how you fell.", person->GetName())
                    : Trinity::StringFormat("I have killed you {} time{}, {}. Shall we make it {}?", times,
                        times == 1 ? "" : "s", person->GetName(), times + 1);
            else if (record.Warchief && s_cfg.Warchiefs)
                line = Trinity::StringFormat("You stand before {}, Warchief of {}!", Display(record),
                    BountyBoard::ZoneName(bot->GetZoneId()));
            else
                line = Trinity::StringFormat("I am {}. Remember the name - it will be the last you hear.", Display(record));
        }

        Shout(bot, line);
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

    // Every few seconds: bodies, and the zone behind every nemesis.
    void TickBodies()
    {
        time_t const now = GameTime::GetGameTime();
        std::unique_lock<std::shared_mutex> guard(g_lock);
        for (auto itr = g_records.begin(); itr != g_records.end(); )
        {
            Record& record = itr->second;

            // Gone for good, and long enough ago to drop from the history.
            if (record.Fallen && !record.ReturnAt && s_cfg.FallenHistoryDays &&
                now - record.FallenAt > time_t(s_cfg.FallenHistoryDays) * DAY)
            {
                DeleteRecord(record.Bot);
                g_victims.erase(itr->first);
                itr = g_records.erase(itr);
                continue;
            }

            Player* bot = ObjectAccessor::FindConnectedPlayer(record.Bot);
            if (bot && bot->IsInWorld() && IsWorldBot(bot) && !bot->InBattleground() && !bot->InArena())
            {
                record.Level = bot->GetLevel();
                if (Active(record) && InOpenWorld(bot))
                    record.ZoneId = bot->GetZoneId();
                ApplyBodyLocked(bot, record);
            }

            ++itr;
        }
        UpdateAnyLocked();
    }

    // The rising, as its slayer lives it: a whisper in their ear, a line across
    // the middle of their screen and a sting only they hear - and a word to
    // their party, who may stand with them. World thread, maps idle.
    void SayRise(Player* bot, Player* slayer, std::string const& name, std::string const& display, std::string const& grave)
    {
        if (s_cfg.Whisper)
        {
            std::string line;
            switch (urand(0, 3))
            {
                case 0:
                    line = Trinity::StringFormat("Did you think the dirt would hold me, {}?", slayer->GetName());
                    break;
                case 1:
                    line = Trinity::StringFormat("You left me in the ground in {}, {}. I clawed my way out. I am coming for you.",
                        grave, slayer->GetName());
                    break;
                case 2:
                    line = Trinity::StringFormat("I remember your face, {}. It was the last thing I saw. It will be the last "
                        "thing you see.", slayer->GetName());
                    break;
                default:
                    line = Trinity::StringFormat("Every night in the grave I thought of you, {}. Every single night.",
                        slayer->GetName());
                    break;
            }
            bot->Whisper(line, LANG_UNIVERSAL, slayer);
            bot->Unit::Whisper(Trinity::StringFormat("{} has risen from the grave. It is coming for you.", name),
                LANG_UNIVERSAL, slayer, true);
        }
        if (s_cfg.RiseSound)
            bot->PlayDirectSound(s_cfg.RiseSound, slayer);

        Tell(slayer, Trinity::StringFormat("{} has come back from the dead for you. For the next {} minutes it will find you "
            "anywhere out in the world. Kill it again, or outlast it.", display, s_cfg.VendettaMinutes));

        if (Group* group = slayer->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
                if (Player* member = ref->GetSource(); member && member != slayer && IsPerson(member))
                    Tell(member, s_cfg.HuntGroupStance
                        ? Trinity::StringFormat("{} has come back from the dead for {}. Stand with them: it is fair game to "
                            "the whole party, War Mode or not.", display, slayer->GetName())
                        : Trinity::StringFormat("{} has come back from the dead for {}.", display, slayer->GetName()));
    }

    // Every few seconds: a slain nemesis that is coming back picks its moment.
    // Its slayer has to be out where it can reach them; then it is a roll, so
    // nobody can say when. World thread, maps idle.
    void TickRevenants()
    {
        if (!On() || !s_cfg.Returns)
            return;

        time_t const now = GameTime::GetGameTime();
        struct Rising
        {
            Player* Bot;
            Player* Slayer;
            std::string Name;
            std::string Display;
            std::string Grave;
        };
        std::vector<Rising> risings;
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);

            // One revenant at a time on anybody.
            std::unordered_set<uint64> hunted;
            for (auto const& [raw, record] : g_records)
                if (HuntingLocked(record))
                    hunted.insert(record.Hunting.GetRawValue());

            for (auto& [raw, record] : g_records)
            {
                if (!record.Fallen || !record.ReturnAt || now < record.ReturnAt)
                    continue;

                // Its slayer never came out where it could reach them in all
                // that time: it stays down.
                if (record.SlainBy.IsEmpty() || now - record.ReturnAt > time_t(s_cfg.ReturnGiveUpDays) * DAY)
                {
                    record.ReturnAt = 0;
                    SaveRecord(record);
                    continue;
                }

                if (hunted.count(record.SlainBy.GetRawValue()))
                    continue;

                Player* bot = ObjectAccessor::FindConnectedPlayer(record.Bot);
                if (!bot || !bot->IsInWorld() || !IsWorldBot(bot) || !bot->IsAlive() || bot->IsInCombat() ||
                    bot->InBattleground() || bot->InArena() || !InOpenWorld(bot) ||
                    playerbot::PveManager::IsNoticeHeld(record.Bot))
                    continue;

                Player* slayer = ObjectAccessor::FindConnectedPlayer(record.SlainBy);
                if (!Exposed(slayer))
                    continue;

                // Outgrown: there is no glory left in it, and it knows. Still out
                // of its reach the other way: it waits for them to grow.
                if (bot->GetLevel() <= Trinity::XP::GetGrayLevel(slayer->GetLevel()))
                {
                    record.ReturnAt = 0;
                    SaveRecord(record);
                    continue;
                }
                if (s_cfg.MaxLevelGap && uint32(bot->GetLevel()) > uint32(slayer->GetLevel()) + s_cfg.MaxLevelGap)
                    continue;

                if (!roll_chance_i(int32(s_cfg.ReturnStrikeChance)))
                    continue;

                std::string const grave = BountyBoard::ZoneName(record.ZoneId);

                record.Fallen = false;
                record.ReturnAt = 0;
                ++record.Scars;
                record.Epithet = ScarEpithet(record.Scars);
                record.Name = bot->GetName();
                record.Level = bot->GetLevel();
                record.ZoneId = slayer->GetZoneId();
                record.Hunting = slayer->GetGUID();
                record.HuntingName = slayer->GetName();
                record.HuntUntil = now + time_t(s_cfg.VendettaMinutes) * MINUTE;
                record.HuntStage = 0;
                record.HuntAllies = 0;
                record.HuntParty.clear();
                hunted.insert(record.Hunting.GetRawValue());

                PutHuntStance(record);
                playerbot::PveManager::SetNoticeHeld(record.Bot, true);
                playerbot::PveManager::SetNoticeErrand(record.Bot, playerbot::PveManager::NoticeErrandKind::Pursue,
                    record.Hunting, s_cfg.HuntDispatch ? playerbot::PveManager::NoticeDispatch::Always
                                                       : playerbot::PveManager::NoticeDispatch::Never);
                if (s_cfg.RevenantVisual && sSpellMgr->GetSpellInfo(s_cfg.RevenantVisual))
                {
                    bot->AddAura(s_cfg.RevenantVisual, bot);
                    record.HuntVisual = s_cfg.RevenantVisual;
                }
                ApplyBodyLocked(bot, record);

                AddRumourLocked(record.ZoneId, Trinity::StringFormat("{} has clawed its way out of the grave, and is looking "
                    "for {} in {}.", Display(record), record.HuntingName, BountyBoard::ZoneName(record.ZoneId)));
                SaveRecord(record);
                risings.push_back({ bot, slayer, record.Name, Display(record), grave });

                TC_LOG_INFO("scripts", "Nemesis: {} came back from the dead for {} (scar {}).", record.Name,
                    record.HuntingName, uint32(record.Scars));
            }
            UpdateAnyLocked();
        }

        for (Rising const& rising : risings)
            SayRise(rising.Bot, rising.Slayer, rising.Name, rising.Display, rising.Grave);
    }

    enum class Beat : uint8 { Closing, Arrived, TimedOut };

    // The hunt as its slayer hears it. World thread, maps idle.
    void SayBeat(Beat beat, Player* bot, Player* slayer, std::string const& name, uint32 allies)
    {
        switch (beat)
        {
            case Beat::Closing:
            {
                if (!s_cfg.Whisper)
                    break;
                std::string line;
                switch (urand(0, 2))
                {
                    case 0:  line = Trinity::StringFormat("I can smell you, {}. Closer now.", slayer->GetName()); break;
                    case 1:  line = Trinity::StringFormat("Do you hear the footsteps, {}? They are mine.", slayer->GetName()); break;
                    default: line = Trinity::StringFormat("Not far now, {}...", slayer->GetName()); break;
                }
                bot->Whisper(line, LANG_UNIVERSAL, slayer);
                bot->Unit::Whisper(Trinity::StringFormat("{} is close.", name), LANG_UNIVERSAL, slayer, true);
                break;
            }
            case Beat::Arrived:
            {
                std::string yell;
                switch (urand(0, 2))
                {
                    case 0:  yell = Trinity::StringFormat("{}! You put me in the ground. Now I put you in it!", slayer->GetName()); break;
                    case 1:  yell = Trinity::StringFormat("Remember me, {}? I remember YOU.", slayer->GetName()); break;
                    default: yell = Trinity::StringFormat("From the grave I come, {}! Face me!", slayer->GetName()); break;
                }
                Shout(bot, yell);
                if (s_cfg.Yell)
                    bot->TextEmote(urand(0, 1) ? "claws its way up out of the earth, eyes burning with hate."
                                               : "rises from the dirt, still wearing the wounds that killed it.");
                if (s_cfg.ArriveSound)
                    bot->PlayDistanceSound(s_cfg.ArriveSound);
                if (allies)
                    Shout(bot, Trinity::StringFormat("You brought friends, {}? Good. Graves for all of you.", slayer->GetName()));
                break;
            }
            case Beat::TimedOut:
                if (s_cfg.Whisper)
                    bot->Whisper(urand(0, 1)
                        ? Trinity::StringFormat("Hide, then, {}. I have nothing but time. I am already dead.", slayer->GetName())
                        : Trinity::StringFormat("This is not over, {}.", slayer->GetName()), LANG_UNIVERSAL, slayer);
                Tell(slayer, Trinity::StringFormat("{} has lost your trail - for now.", name));
                break;
        }
    }

    // Every second: each revenant on the hunt, staged as it closes in, and ended
    // when its time runs out or its slayer is gone. World thread, maps idle.
    void TickHunts()
    {
        if (!g_anyHunt.load(std::memory_order_relaxed))
            return;

        time_t const now = GameTime::GetGameTime();
        struct Line
        {
            Beat What;
            Player* Bot;
            Player* Slayer;
            std::string Name;
            uint32 Allies;
        };
        std::vector<Line> lines;
        {
            std::unique_lock<std::shared_mutex> guard(g_lock);
            for (auto& [raw, record] : g_records)
            {
                if (!HuntingLocked(record))
                    continue;

                Player* bot = ObjectAccessor::FindConnectedPlayer(record.Bot);
                bool const botHere = bot && bot->IsInWorld();
                Player* slayer = ObjectAccessor::FindConnectedPlayer(record.Hunting);

                if (!On() || !s_cfg.Returns || !botHere)
                {
                    EndHuntLocked(record, botHere ? bot : nullptr);
                    continue;
                }
                if (!slayer || !slayer->IsInWorld())
                {
                    AddRumourLocked(record.ZoneId, Trinity::StringFormat("{} lost {}'s trail.", Display(record),
                        record.HuntingName));
                    EndHuntLocked(record, bot);
                    continue;
                }
                if (now >= record.HuntUntil)
                {
                    lines.push_back({ Beat::TimedOut, bot, slayer, record.Name, 0 });
                    AddRumourLocked(record.ZoneId, Trinity::StringFormat("{} hunted {} and lost them. It is still out there.",
                        Display(record), record.HuntingName));
                    EndHuntLocked(record, bot);
                    continue;
                }
                if (!bot->IsAlive() || !slayer->IsAlive() || bot->GetMap() != slayer->GetMap())
                    continue;

                float const distance = bot->GetDistance(slayer);
                if (record.HuntStage == 0 && distance < 120.0f)
                {
                    record.HuntStage = 1;
                    lines.push_back({ Beat::Closing, bot, slayer, record.Name, 0 });
                }

                bool const arrived = record.HuntStage == 1 && distance < 45.0f;
                if (arrived)
                    record.HuntStage = 2;

                // Once it is upon them: who stands at the slayer's side, counted
                // every beat, so a friend who runs stops counting.
                if (record.HuntStage == 2)
                {
                    record.HuntParty.clear();
                    for (Player* member : PartyNear(slayer, slayer, s_cfg.AllyYards))
                        if (member->IsAlive())
                            record.HuntParty.push_back(member->GetGUID());
                    record.HuntAllies = uint32(record.HuntParty.size());
                }

                if (arrived)
                    lines.push_back({ Beat::Arrived, bot, slayer, record.Name, record.HuntAllies });
            }
            UpdateAnyLocked();
        }

        for (Line const& line : lines)
            SayBeat(line.What, line.Bot, line.Slayer, line.Name, line.Allies);
    }

    // Every half minute: who leads each zone, and who guards whom.
    void TickWarchiefs()
    {
        std::vector<std::pair<Player*, std::string>> yells;
        std::unique_lock<std::shared_mutex> guard(g_lock);

        std::map<uint32, Record*> best;
        std::unordered_map<uint64, Player*> online;
        for (auto& [raw, record] : g_records)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(record.Bot);
            bool const here = bot && bot->IsInWorld() && IsWorldBot(bot) && !bot->InBattleground() && InOpenWorld(bot);
            if (here)
                online[raw] = bot;

            if (!On() || !s_cfg.Warchiefs || !here || !Active(record) || record.Rank < s_cfg.WarchiefMinRank)
                continue;

            uint32 const zone = bot->GetZoneId();
            Record*& top = best[zone];
            if (!top || record.Rank > top->Rank || (record.Rank == top->Rank && record.Kills > top->Kills))
                top = &record;
        }

        for (auto& [raw, record] : g_records)
        {
            // A plain pointer for the lambda: clang before 16 refuses to capture
            // a structured binding.
            Record const* const self = &record;
            bool const chief = std::any_of(best.begin(), best.end(), [self](auto const& entry) { return entry.second == self; });
            if (chief && !record.Warchief)
            {
                AddRumourLocked(record.ZoneId, Trinity::StringFormat("{} is now Warchief of {}.", Display(record),
                    BountyBoard::ZoneName(record.ZoneId)));
                if (auto const bot = online.find(raw); bot != online.end())
                    yells.emplace_back(bot->second, Trinity::StringFormat("{} bows to me now! I am {}, Warchief!",
                        BountyBoard::ZoneName(record.ZoneId), record.Name));
            }
            record.Warchief = chief;

            auto const bot = online.find(raw);
            TendGuardsLocked(bot != online.end() ? bot->second : nullptr, record);
            if (bot != online.end())
                ApplyBodyLocked(bot->second, record);
        }

        guard.unlock();
        for (auto const& [bot, text] : yells)
            Shout(bot, text);
    }

    // Now and then: nemeses in one zone go for each other's standing, and the
    // ones that are due are settled.
    void TickStruggles(bool schedule)
    {
        time_t const now = GameTime::GetGameTime();
        std::vector<std::pair<Player*, std::string>> yells;
        std::unique_lock<std::shared_mutex> guard(g_lock);

        if (!On() || !s_cfg.Struggles)
        {
            g_struggles.clear();
            return;
        }

        // Settle what is due.
        for (auto itr = g_struggles.begin(); itr != g_struggles.end(); )
        {
            if (now < itr->ResolveAt)
            {
                ++itr;
                continue;
            }

            auto const a = g_records.find(itr->Challenger.GetRawValue());
            auto const b = g_records.find(itr->Defender.GetRawValue());
            bool const aIn = a != g_records.end() && Active(a->second);
            bool const bIn = b != g_records.end() && Active(b->second);

            // Not while either is out for somebody's blood: it keeps.
            if ((aIn && HuntingLocked(a->second)) || (bIn && HuntingLocked(b->second)))
            {
                itr->ResolveAt = now + 5 * MINUTE;
                ++itr;
                continue;
            }

            uint32 const zone = itr->ZoneId;
            itr = g_struggles.erase(itr);

            if (!aIn && !bIn)
                continue;

            Record* winner = nullptr;
            Record* loser = nullptr;
            bool forfeit = false;
            if (!aIn || !bIn)
            {
                winner = aIn ? &a->second : &b->second;
                loser = aIn ? (b != g_records.end() ? &b->second : nullptr) : (a != g_records.end() ? &a->second : nullptr);
                forfeit = true;
            }
            else
            {
                auto const weight = [](Record const& r)
                {
                    return int32(r.Rank) * 2 + int32(std::min<uint32>(r.Kills, 10)) + (r.Warchief ? 2 : 0) + int32(urand(0, 10));
                };
                bool const aWins = weight(a->second) >= weight(b->second);
                winner = aWins ? &a->second : &b->second;
                loser = aWins ? &b->second : &a->second;
            }

            if (winner->Rank < s_cfg.MaxRank)
                ++winner->Rank;
            std::string const winnerName = Display(*winner);
            std::string text;
            if (forfeit)
                text = Trinity::StringFormat("{} never made it to the fight. {} takes the standing and rises to rank {}.",
                    loser ? Display(*loser) : std::string("The challenger"), winnerName, Roman(winner->Rank));
            else
            {
                std::string const loserName = Display(*loser);
                if (loser->Rank > 1)
                {
                    --loser->Rank;
                    text = Trinity::StringFormat("{} beat {} in {}: {} rises to rank {}, {} falls to rank {}.", winnerName,
                        loserName, BountyBoard::ZoneName(zone), winner->Name, Roman(winner->Rank), loser->Name, Roman(loser->Rank));
                    SaveRecord(*loser);
                }
                else
                {
                    loser->Rank = 0;
                    loser->Warchief = false;
                    ReleaseGuardsLocked(*loser);
                    if (Player* lb = ObjectAccessor::FindConnectedPlayer(loser->Bot))
                        ApplyBodyLocked(lb, *loser);
                    text = Trinity::StringFormat("{} beat {} in {}. {} rises to rank {}; {} is a nobody again.", winnerName,
                        loserName, BountyBoard::ZoneName(zone), winner->Name, Roman(winner->Rank), loser->Name);
                    DeleteRecord(loser->Bot);
                    ObjectGuid const gone = loser->Bot;
                    g_victims.erase(gone.GetRawValue());
                    g_records.erase(gone.GetRawValue());
                    winner = &g_records[winner->Bot.GetRawValue()];
                }
            }

            SaveRecord(*winner);
            AddRumourLocked(zone, text);
            if (Player* wb = ObjectAccessor::FindConnectedPlayer(winner->Bot))
            {
                ApplyBodyLocked(wb, *winner);
                yells.emplace_back(wb, Trinity::StringFormat("{} knows its master now!", BountyBoard::ZoneName(zone)));
            }
        }

        // Post new challenges.
        if (schedule)
        {
            std::map<uint32, std::vector<Record*>> byZone;
            for (auto& [raw, record] : g_records)
                if (Active(record) && record.ZoneId)
                    byZone[record.ZoneId].push_back(&record);

            for (auto& [zone, records] : byZone)
            {
                if (records.size() < 2 || !roll_chance_i(int32(s_cfg.StruggleChance)))
                    continue;

                auto const busy = [](Record const* r)
                {
                    return HuntingLocked(*r) || std::any_of(g_struggles.begin(), g_struggles.end(), [r](Struggle const& s)
                        { return s.Challenger == r->Bot || s.Defender == r->Bot; });
                };
                records.erase(std::remove_if(records.begin(), records.end(), busy), records.end());
                if (records.size() < 2)
                    continue;

                std::shuffle(records.begin(), records.end(), RandomEngine::Instance());
                Record* challenger = records[0];
                Record* defender = records[1];
                if (challenger->Rank > defender->Rank)
                    std::swap(challenger, defender);   // the lesser one does the challenging

                Struggle struggle;
                struggle.Challenger = challenger->Bot;
                struggle.Defender = defender->Bot;
                struggle.ZoneId = zone;
                struggle.ResolveAt = now + time_t(s_cfg.StruggleLeadMinutes) * MINUTE;
                g_struggles.push_back(struggle);

                AddRumourLocked(zone, Trinity::StringFormat(
                    "{} has challenged {} for their standing in {}. It will be settled within {} minutes - unless somebody "
                    "settles it for them.", Display(*challenger), Display(*defender), BountyBoard::ZoneName(zone),
                    s_cfg.StruggleLeadMinutes));
            }
        }

        UpdateAnyLocked();
        guard.unlock();
        for (auto const& [bot, text] : yells)
            Shout(bot, text);
    }

    // After a .reload config: anything a switch took away comes off now.
    void ReconcileAll()
    {
        std::unique_lock<std::shared_mutex> guard(g_lock);
        for (auto& [raw, record] : g_records)
        {
            if (!On() || !s_cfg.Warchiefs)
                record.Warchief = false;
            Player* bot = ObjectAccessor::FindConnectedPlayer(record.Bot);
            bool const here = bot && bot->IsInWorld();

            // A hunt ends with its switch, or is shared with the party (or not)
            // as the switch now says.
            if (HuntingLocked(record))
            {
                if (!On() || !s_cfg.Returns)
                    EndHuntLocked(record, here ? bot : nullptr);
                else
                    PutHuntStance(record);
            }

            TendGuardsLocked(here ? bot : nullptr, record);
            if (here)
                ApplyBodyLocked(bot, record);
        }
        if (!On() || !s_cfg.Struggles)
            g_struggles.clear();
        UpdateAnyLocked();
    }

    // ---------------------------------------------------------------- the board

    std::string VictimList(ObjectGuid bot)
    {
        std::string out;
        auto const itr = g_victims.find(bot.GetRawValue());
        if (itr == g_victims.end())
            return out;
        std::vector<Victim> victims = itr->second;
        std::sort(victims.begin(), victims.end(), [](Victim const& a, Victim const& b) { return a.LastAt > b.LastAt; });
        for (size_t i = 0; i < victims.size() && i < 6; ++i)
        {
            out += (i ? "/" : "") + Scrubbed(victims[i].Name);
            if (victims[i].Times > 1)
                out += Trinity::StringFormat(" x{}", victims[i].Times);
        }
        return out;
    }

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

    void SendHierarchy(Player* viewer, uint32 zoneFilter)
    {
        std::string const head = std::to_string(zoneFilter) + "|";
        uint32 count = 0;
        {
            std::shared_lock<std::shared_mutex> guard(g_lock);

            std::vector<Record const*> rows;
            for (auto const& [raw, record] : g_records)
                if (record.Rank > 0 && (!zoneFilter || record.ZoneId == zoneFilter))
                    rows.push_back(&record);
            std::sort(rows.begin(), rows.end(), [](Record const* a, Record const* b)
            {
                if (a->Fallen != b->Fallen)
                    return !a->Fallen;
                if (a->Warchief != b->Warchief)
                    return a->Warchief;
                if (a->Rank != b->Rank)
                    return a->Rank > b->Rank;
                return a->Kills > b->Kills;
            });

            Chunker out(viewer, "BBHR", head);
            for (Record const* record : rows)
            {
                if (count >= 30)
                    break;
                KnownTraits const known = KnownLocked(viewer->GetGUID(), record->Bot);
                Player const* bot = ObjectAccessor::FindConnectedPlayer(record->Bot);
                out.Add(Trinity::StringFormat("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
                    record->Bot.GetCounter(), uint32(record->Rank), record->Warchief ? 1 : 0, uint32(record->Scars),
                    record->Kills, record->ZoneId, record->Fallen ? 1 : 0,
                    s_cfg.Traits ? (known.Strengths & record->Strengths) : 0u,
                    s_cfg.Traits ? (known.Weaknesses & record->Weaknesses) : 0u,
                    s_cfg.Traits ? CountBits(record->Strengths) : 0u, s_cfg.Traits ? CountBits(record->Weaknesses) : 0u,
                    uint32(record->Level), uint32(record->Class), bot && bot->IsInWorld() ? 1 : 0,
                    Scrubbed(record->Fallen ? record->Epithet + " - slain by " + record->SlainByName
                        : HuntingLocked(*record) ? record->Epithet + " - hunting " + record->HuntingName
                        : record->Epithet),
                    Scrubbed(record->Name)));
                ++count;
            }
            out.Flush();
        }
        BountyBoard::SendTagged(viewer, "BBHE", head + std::to_string(count));

        // Rumours, newest first: this zone's, or everybody's.
        time_t const now = GameTime::GetGameTime();
        uint32 sent = 0;
        {
            std::shared_lock<std::shared_mutex> guard(g_lock);
            Chunker out(viewer, "BBRU", "");
            for (Rumour const& rumour : g_rumours)
            {
                if (sent >= 8)
                    break;
                if (zoneFilter && rumour.ZoneId != zoneFilter)
                    continue;
                out.Add(Trinity::StringFormat("{},{},{}", uint32(std::max<time_t>(0, now - rumour.At)), rumour.ZoneId,
                    Scrubbed(rumour.Text)));
                ++sent;
            }
            out.Flush();
        }
        BountyBoard::SendTagged(viewer, "BBRZ", std::to_string(sent));
    }

    constexpr uint32 kSenderNemeses = 900320;
}

namespace Nemesis
{
    bool IsHunting(ObjectGuid bot)
    {
        if (!g_anyHunt.load(std::memory_order_relaxed))
            return false;
        std::shared_lock<std::shared_mutex> guard(g_lock);
        auto const itr = g_records.find(bot.GetRawValue());
        return itr != g_records.end() && HuntingLocked(itr->second);
    }

    bool IsWarchief(ObjectGuid bot)
    {
        if (!On() || !s_cfg.Warchiefs || !g_anyActive.load(std::memory_order_relaxed))
            return false;
        std::shared_lock<std::shared_mutex> guard(g_lock);
        auto const itr = g_records.find(bot.GetRawValue());
        return itr != g_records.end() && Active(itr->second) && itr->second.Warchief;
    }

    bool Enabled()
    {
        return On();
    }

    void SendBoardState(Player* viewer)
    {
        BountyBoard::SendTagged(viewer, "BBNO", On() && s_cfg.Board ? "1" : "0");
    }

    void SendBoardRows(Player* viewer, uint32 zoneId, std::vector<Player*> const& bots)
    {
        if (!On() || !s_cfg.Board || !viewer)
            return;

        std::shared_lock<std::shared_mutex> guard(g_lock);
        Chunker out(viewer, "BBNX", std::to_string(zoneId) + "|");
        for (Player* bot : bots)
        {
            auto const itr = g_records.find(bot->GetGUID().GetRawValue());
            if (itr == g_records.end() || !Active(itr->second))
                continue;
            Record const& record = itr->second;
            out.Add(Trinity::StringFormat("{},{},{},{},{}", bot->GetGUID().GetCounter(), uint32(record.Rank),
                record.Warchief ? 1 : 0, uint32(record.Scars), Scrubbed(record.Epithet)));
        }
        out.Flush();
    }

    void SendPosterLine(Player* viewer, Player* bot)
    {
        if (!On() || !s_cfg.Board || !viewer || !bot)
            return;

        std::shared_lock<std::shared_mutex> guard(g_lock);
        auto const itr = g_records.find(bot->GetGUID().GetRawValue());
        if (itr == g_records.end() || !Active(itr->second))
            return;
        Record const& record = itr->second;
        KnownTraits const known = KnownLocked(viewer->GetGUID(), record.Bot);

        BountyBoard::SendTagged(viewer, "BBNP", Trinity::StringFormat("{}|{},{},{},{},{},{},{},{}|{}|{}",
            bot->GetGUID().GetCounter(), uint32(record.Rank), record.Warchief ? 1 : 0, uint32(record.Scars), record.Kills,
            s_cfg.Traits ? (known.Strengths & record.Strengths) : 0u, s_cfg.Traits ? (known.Weaknesses & record.Weaknesses) : 0u,
            s_cfg.Traits ? CountBits(record.Strengths) : 0u, s_cfg.Traits ? CountBits(record.Weaknesses) : 0u,
            Scrubbed(record.Epithet), VictimList(record.Bot)));
    }

    bool HandleRequest(Player* player, std::string_view verb, std::string_view argument)
    {
        if (verb != "NEMESES")
            return false;

        if (On() && s_cfg.Board)
            if (Optional<uint32> const zone = Trinity::StringTo<uint32>(argument))
                SendHierarchy(player, *zone);
        return true;
    }

    void AddGossipOptions(Player* player)
    {
        if (On() && s_cfg.Gossip && player)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Who are the nemeses around here?", kSenderNemeses, 0);
    }

    bool OnGossipSelect(Player* player, GameObject* board, uint32 sender, uint32 /*action*/)
    {
        if (sender != kSenderNemeses)
            return false;

        CloseGossipMenuFor(player);
        if (!On() || !s_cfg.Gossip)
            return true;

        uint32 const zone = board->GetZoneId();
        std::vector<std::string> lines;
        {
            std::shared_lock<std::shared_mutex> guard(g_lock);
            for (auto const& [raw, record] : g_records)
            {
                if (record.ZoneId != zone || record.Rank == 0)
                    continue;
                if (record.Fallen)
                {
                    lines.push_back(Trinity::StringFormat("{} - slain by {}.", Display(record), record.SlainByName));
                    continue;
                }
                KnownTraits const known = KnownLocked(player->GetGUID(), record.Bot);
                std::string traits;
                for (uint8 bit = 0; bit < S_COUNT && s_cfg.Traits; ++bit)
                    if (record.Strengths & (1u << bit))
                        traits += std::string(traits.empty() ? "" : ", ") +
                            ((known.Strengths & (1u << bit)) ? Strengths[bit].Name : "?");
                for (uint8 bit = 0; bit < W_COUNT && s_cfg.Traits; ++bit)
                    if (record.Weaknesses & (1u << bit))
                        traits += std::string(traits.empty() ? "" : ", ") + "weak: " +
                            ((known.Weaknesses & (1u << bit)) ? Weaknesses[bit].Name : "?");
                lines.push_back(Trinity::StringFormat("{}{} - rank {}, {} kill{}{}{}", record.Warchief ? "Warchief " : "",
                    Display(record), Roman(record.Rank), record.Kills, record.Kills == 1 ? "" : "s",
                    traits.empty() ? "" : ". ", traits));
            }
        }

        if (lines.empty())
            Tell(player, Trinity::StringFormat("No nemesis walks {}. Yet.", BountyBoard::ZoneName(zone)));
        for (std::string const& line : lines)
            Tell(player, line);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
class custom_nemesis_world : public WorldScript
{
public:
    custom_nemesis_world() : WorldScript("custom_nemesis_world") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
        if (s_tablesReady)
            ReconcileAll();
    }

    void OnStartup() override
    {
        LoadConfig();

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bot_nemesis ("
            "guid INT UNSIGNED NOT NULL PRIMARY KEY,"
            "name VARCHAR(32) NOT NULL DEFAULT '',"
            "level TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "class TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`rank` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "epithet VARCHAR(64) NOT NULL DEFAULT '',"
            "kills INT UNSIGNED NOT NULL DEFAULT 0,"
            "strengths INT UNSIGNED NOT NULL DEFAULT 0,"
            "weaknesses INT UNSIGNED NOT NULL DEFAULT 0,"
            "scars TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "zoneId INT UNSIGNED NOT NULL DEFAULT 0,"
            "risenAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "fallen TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "fallenAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "returnAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "slainBy INT UNSIGNED NOT NULL DEFAULT 0,"
            "slainByName VARCHAR(32) NOT NULL DEFAULT ''"
            ") ENGINE=InnoDB");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bot_nemesis_victim ("
            "bot INT UNSIGNED NOT NULL,"
            "victim INT UNSIGNED NOT NULL,"
            "victimName VARCHAR(32) NOT NULL DEFAULT '',"
            "times INT UNSIGNED NOT NULL DEFAULT 0,"
            "lastAt INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (bot, victim)"
            ") ENGINE=InnoDB");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_nemesis_intel ("
            "guid INT UNSIGNED NOT NULL,"
            "bot INT UNSIGNED NOT NULL,"
            "strengths INT UNSIGNED NOT NULL DEFAULT 0,"
            "weaknesses INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (guid, bot)"
            ") ENGINE=InnoDB");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS character_bot_nemesis_rumour ("
            "id INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
            "at INT UNSIGNED NOT NULL DEFAULT 0,"
            "zoneId INT UNSIGNED NOT NULL DEFAULT 0,"
            "text VARCHAR(255) NOT NULL DEFAULT ''"
            ") ENGINE=InnoDB");

        s_tablesReady = bool(CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
            "('character_bot_nemesis', 'character_bot_nemesis_victim', 'character_nemesis_intel', "
            "'character_bot_nemesis_rumour') HAVING COUNT(*) = 4"));
        if (!s_tablesReady)
        {
            TC_LOG_ERROR("server.loading", "Nemesis: the nemesis tables are missing - the system is off.");
            return;
        }

        std::unique_lock<std::shared_mutex> guard(g_lock);

        if (QueryResult result = CharacterDatabase.Query(
                "SELECT guid, name, level, class, `rank`, epithet, kills, strengths, weaknesses, scars, zoneId, risenAt, "
                "fallen, fallenAt, returnAt, slainBy, slainByName FROM character_bot_nemesis"))
        {
            do
            {
                Field* f = result->Fetch();
                Record record;
                record.Bot = ObjectGuid::Create<HighGuid::Player>(f[0].GetUInt32());
                record.Name = f[1].GetString();
                record.Level = f[2].GetUInt8();
                record.Class = f[3].GetUInt8();
                record.Rank = f[4].GetUInt8();
                record.Epithet = f[5].GetString();
                record.Kills = f[6].GetUInt32();
                record.Strengths = f[7].GetUInt32();
                record.Weaknesses = f[8].GetUInt32();
                record.Scars = f[9].GetUInt8();
                record.ZoneId = f[10].GetUInt32();
                record.RisenAt = time_t(f[11].GetUInt32());
                record.Fallen = f[12].GetUInt8() != 0;
                record.FallenAt = time_t(f[13].GetUInt32());
                record.ReturnAt = time_t(f[14].GetUInt32());
                record.SlainBy = ObjectGuid::Create<HighGuid::Player>(f[15].GetUInt32());
                if (!f[15].GetUInt32())
                    record.SlainBy = ObjectGuid::Empty;
                record.SlainByName = f[16].GetString();
                g_records[record.Bot.GetRawValue()] = record;
            } while (result->NextRow());
        }

        if (QueryResult result = CharacterDatabase.Query(
                "SELECT bot, victim, victimName, times, lastAt FROM character_bot_nemesis_victim"))
        {
            do
            {
                Field* f = result->Fetch();
                Victim victim;
                victim.Player = ObjectGuid::Create<HighGuid::Player>(f[1].GetUInt32());
                victim.Name = f[2].GetString();
                victim.Times = f[3].GetUInt32();
                victim.LastAt = time_t(f[4].GetUInt32());
                g_victims[ObjectGuid::Create<HighGuid::Player>(f[0].GetUInt32()).GetRawValue()].push_back(victim);
            } while (result->NextRow());
        }

        CharacterDatabase.DirectExecute(Trinity::StringFormat(
            "DELETE FROM character_bot_nemesis_rumour WHERE at < {}", uint32(GameTime::GetGameTime() - 14 * DAY)).c_str());
        if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT at, zoneId, text FROM character_bot_nemesis_rumour ORDER BY id DESC LIMIT {}", uint32(kMaxRumours)))
        {
            do
            {
                Field* f = result->Fetch();
                Rumour rumour;
                rumour.At = time_t(f[0].GetUInt32());
                rumour.ZoneId = f[1].GetUInt32();
                rumour.Text = f[2].GetString();
                g_rumours.push_back(rumour);
            } while (result->NextRow());
        }

        UpdateAnyLocked();
        TC_LOG_INFO("server.loading", "Nemesis: {} - {} nemeses on record, {} rumours.", On() ? "on" : "OFF",
            g_records.size(), g_rumours.size());
    }

    void OnUpdate(uint32 diff) override
    {
        if (!s_tablesReady)
            return;

        _sinceOutbox += diff;
        _sinceBodies += diff;
        _sinceChiefs += diff;
        _sinceStruggleCheck += diff;

        if (_sinceOutbox >= 1000)
        {
            _sinceOutbox = 0;
            FlushOutbox();
            TickHunts();
            // The core follows shared stances onto parties and re-sends colours;
            // the notices call it too, and either may be switched off.
            PairStance::Update();
        }

        if (_sinceBodies >= 5000)
        {
            _sinceBodies = 0;
            TickBodies();
            TickRevenants();
            bool const schedule = _sinceStruggleCheck >= s_cfg.StruggleCheckMinutes * MINUTE * IN_MILLISECONDS;
            if (schedule)
                _sinceStruggleCheck = 0;
            TickStruggles(schedule);
        }

        if (_sinceChiefs >= 30000)
        {
            _sinceChiefs = 0;
            TickWarchiefs();
        }
    }

private:
    uint32 _sinceOutbox = 0;
    uint32 _sinceBodies = 0;
    uint32 _sinceChiefs = 0;
    uint32 _sinceStruggleCheck = 0;
};

class custom_nemesis_player : public PlayerScript
{
public:
    custom_nemesis_player() : PlayerScript("custom_nemesis_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        // A revenant's look is an aura, and auras outlive a crash: nobody logs
        // in hunting.
        if (s_cfg.RevenantVisual && IsWorldBot(player) && player->HasAura(s_cfg.RevenantVisual))
            player->RemoveAurasDueToSpell(s_cfg.RevenantVisual);

        if (!s_tablesReady || !IsPerson(player))
            return;

        std::unordered_map<uint64, KnownTraits> known;
        if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT bot, strengths, weaknesses FROM character_nemesis_intel WHERE guid = {}", player->GetGUID().GetCounter()))
        {
            do
            {
                Field* f = result->Fetch();
                KnownTraits traits;
                traits.Strengths = f[1].GetUInt32();
                traits.Weaknesses = f[2].GetUInt32();
                known[ObjectGuid::Create<HighGuid::Player>(f[0].GetUInt32()).GetRawValue()] = traits;
            } while (result->NextRow());
        }

        std::unique_lock<std::shared_mutex> guard(g_lock);
        g_intel[player->GetGUID().GetRawValue()] = std::move(known);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        std::unique_lock<std::shared_mutex> guard(g_lock);
        g_intel.erase(player->GetGUID().GetRawValue());

        // A bot's body goes with its session: nothing is applied any more, its
        // guards are free to go, and a hunt ends here.
        if (auto const itr = g_records.find(player->GetGUID().GetRawValue()); itr != g_records.end())
        {
            EndHuntLocked(itr->second, player);
            itr->second.AppliedImmunities = 0;
            itr->second.ScaleApplied = false;
            itr->second.Warchief = false;
            ReleaseGuardsLocked(itr->second);
            UpdateAnyLocked();
        }
    }

    // Map thread, before the combat references are torn down.
    void OnPlayerJustDied(Player* victim, Unit* killer) override
    {
        if (!On() || !victim)
            return;

        Player* killerPlayer = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;

        // A person, killed in the open world by a world bot's own hand.
        if (IsPerson(victim))
        {
            if (killerPlayer && IsWorldBot(killerPlayer) && InOpenWorld(victim) && killerPlayer->IsInWorld())
                OnBotKilledPerson(killerPlayer, victim);
            return;
        }

        if (!IsWorldBot(victim) || !InOpenWorld(victim))
            return;

        // A bot: who had a hand in it.
        std::vector<Player*> people;
        std::unordered_set<uint64> seen;
        auto const consider = [&](Unit* unit)
        {
            Player* person = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (person && person != victim && IsPerson(person) && person->IsInWorld() &&
                person->GetMap() == victim->GetMap() && seen.insert(person->GetGUID().GetRawValue()).second)
                people.push_back(person);
        };
        CombatManager const& combat = victim->GetCombatManager();
        for (auto const& reference : combat.GetPvPCombatRefs())
            consider(reference.second->GetOther(victim));
        consider(killer);

        // Their parties close by share it, as they would a quest kill - the
        // healer who never touched it included.
        if (s_cfg.GroupCredit)
        {
            size_t const fought = people.size();
            for (size_t i = 0; i < fought; ++i)
                for (Player* member : PartyNear(people[i], victim, s_cfg.CreditYards))
                    if (seen.insert(member->GetGUID().GetRawValue()).second)
                        people.push_back(member);
        }

        OnBotDied(victim, people, killerPlayer && IsPerson(killerPlayer) ? killerPlayer : nullptr);
    }
};

class custom_nemesis_damage : public UnitScript
{
public:
    custom_nemesis_damage() : UnitScript("custom_nemesis_damage") { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        Apply(target, attacker, damage, SPELL_SCHOOL_MASK_NORMAL);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage <= 0)
            return;
        uint32 value = uint32(damage);
        Apply(target, attacker, value, spellInfo ? spellInfo->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL);
        damage = int32(value);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        Apply(target, attacker, damage, spellInfo ? spellInfo->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL);
    }

private:
    static void Apply(Unit* target, Unit* attacker, uint32& damage, SpellSchoolMask school)
    {
        if (!damage)
            return;

        Player* tauntBot = nullptr;
        Player* tauntPerson = nullptr;
        uint32 const pct = HitPct(target, attacker, school, &tauntBot, &tauntPerson);
        if (pct != 100)
            damage = CalculatePct(damage, pct);

        if (tauntBot)
            MaybeTaunt(tauntBot, tauntPerson);
    }
};

void AddSC_custom_nemesis()
{
    LoadConfig();
    new custom_nemesis_world();
    new custom_nemesis_player();
    new custom_nemesis_damage();
}
