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

#include "AllStar.h"
#include "AddonRequestRoutes.h"
#include "AsyncCallbackProcessor.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "CharacterScreen.h"
#include "Chat.h"
#include "Containers.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "QueryHolder.h"
#include "QuestDef.h"
#include "Random.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Surnames.h"
#include "TemporarySummon.h"
#include "TournamentMode.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace AllStar
{
// ======================================================================= kits

struct KitSpell
{
    uint32 Id = 0;
    bool Active = true;
    bool Disabled = false;
};

struct KitSkill
{
    uint16 Id = 0;
    uint16 Value = 0;
    uint16 Max = 0;
};

struct KitButton
{
    uint8 Button = 0;
    uint32 Action = 0;
    uint8 Type = 0;
};

struct KitItem
{
    uint8 Slot = 0;
    uint32 Entry = 0;
    int32 RandomPropertyId = 0;
    std::array<uint32, MAX_ENCHANTMENT_SLOT * MAX_ENCHANTMENT_OFFSET> Enchants{};
};

struct Kit
{
    ObjectGuid::LowType Source = 0;
    bool Template = false;                  // worn under the player's own name
    bool StandIn = false;                   // a playerbot character (a Template too)
    std::string Name;
    uint8 Race = 0;
    uint8 Class = 0;
    uint8 Gender = 0;
    uint8 Skin = 0;
    uint8 Face = 0;
    uint8 HairStyle = 0;
    uint8 HairColor = 0;
    uint8 FacialStyle = 0;
    uint32 Ammo = 0;
    std::vector<KitSpell> Spells;
    std::vector<uint32> Talents;            // the source's active talent group
    std::vector<KitSkill> Skills;
    std::vector<KitButton> Buttons;         // the source's active talent group
    std::array<uint32, MAX_GLYPH_SLOT_INDEX> Glyphs{};
    std::vector<KitItem> Items;             // equipment slots only
    // Food, drink and bandages the source carries (backpack and worn bags,
    // not the bank), by entry: a life gets copies of them (GiveConsumables).
    std::vector<std::pair<uint32, uint32>> Consumables;
    bool TournamentMode = false;            // extra_flags PLAYER_EXTRA_TOURNAMENT_MODE
    // The source's own keybindings (character_account_data type 3, the
    // client's bindings-cache: "bind KEY COMMAND" over the defaults), when it
    // keeps a set of its own; without one it plays on its account's.
    bool OwnBindings = false;
    std::vector<std::pair<std::string, std::string>> Bindings;
    // The source's character macros by button number (type 5, "MACRO
    // 0x1000000|n ..."). A button can only point at the player's own: on the
    // server each becomes the spell its macro casts, and CENTURION_AllStar is
    // sent the macro itself to make and put back on its buttons (SendMacros).
    struct Macro
    {
        std::string Name;
        std::string Icon;
        std::string Body;
    };
    std::unordered_map<uint32, Macro> CharacterMacros;
    std::vector<uint32> RewardedQuests;     // their spells are taught at every login
    bool HasPet = false;
    uint32 SourcePetNumber = 0;
    PetStable::PetInfo Pet;
    std::vector<std::pair<uint32, uint8>> PetSpells;
};

namespace
{
    // --------------------------------------------------------------- config
    struct Settings
    {
        bool Enabled = true;
        uint32 RespawnDelayMs = 2 * IN_MILLISECONDS;
        uint32 TimeLimitMs = 30 * MINUTE * IN_MILLISECONDS;
        uint8 SourceMinLevel = 60;
        bool ShuffleLives = true;           // every life after the first in a random order
        std::array<ObjectGuid::LowType, 12> Templates{};
    };
    Settings Config;

    // ------------------------------------------------------------- the store
    enum class KitState : uint8 { Loading, Ready, Failed };
    struct KitEntry
    {
        KitState State = KitState::Loading;
        KitPtr Value;
    };

    std::mutex KitMutex;
    std::unordered_map<ObjectGuid::LowType, KitEntry> Kits;

    struct LineupKey
    {
        uint32 Account;
        ObjectGuid::LowType Playing;
        bool operator==(LineupKey const& other) const { return Account == other.Account && Playing == other.Playing; }
    };
    struct LineupKeyHash
    {
        size_t operator()(LineupKey const& key) const { return std::hash<uint64>()((uint64(key.Account) << 32) ^ key.Playing); }
    };
    struct LineupEntryState
    {
        bool Ready = false;
        Lineup Value;
    };
    std::mutex LineupMutex;
    std::unordered_map<LineupKey, LineupEntryState, LineupKeyHash> Lineups;

    // The processors are touched on the world thread only (Update): any
    // thread may ASK for a kit or a lineup, and the ask waits in these lists
    // until Update queues the queries and, later, runs their answers.
    AsyncCallbackProcessor<SQLQueryHolderCallback> HolderCallbacks;
    QueryCallbackProcessor QueryCallbacks;
    struct PendingLineup
    {
        LineupKey Key;
        uint8 PlayingClass;
    };
    std::mutex PendingMutex;
    std::vector<ObjectGuid::LowType> PendingKits;
    std::vector<PendingLineup> PendingLineups;
    bool TemplatesAsked = false;

    // ----------------------------------------------------------- name answers
    struct PersonaName
    {
        std::string Name;
        uint8 Race = 0;
        uint8 Gender = 0;
        uint8 Class = 0;
    };
    std::mutex NameMutex;
    std::unordered_map<ObjectGuid, PersonaName> Names;

    // -------------------------------------------------------- client catch-up
    std::mutex SwapTimeMutex;
    std::unordered_map<ObjectGuid, uint32> SwapTimes;
    uint32 constexpr KEEP_BUTTONS_MS = 3 * IN_MILLISECONDS;

    thread_local Player const* Swapping = nullptr;

    struct SwapScope
    {
        explicit SwapScope(Player const* player) : _previous(Swapping) { Swapping = player; }
        ~SwapScope() { Swapping = _previous; }
        Player const* _previous;
    };

    enum KitQuery : uint8
    {
        KIT_QUERY_CHARACTER,
        KIT_QUERY_SPELLS,
        KIT_QUERY_TALENTS,
        KIT_QUERY_SKILLS,
        KIT_QUERY_ACTIONS,
        KIT_QUERY_GLYPHS,
        KIT_QUERY_INVENTORY,
        KIT_QUERY_PETS,
        KIT_QUERY_QUESTS,
        KIT_QUERY_ACCOUNT_DATA,
        MAX_KIT_QUERY
    };

    // The same statements a login reads, so the kit is what the character
    // would be if it logged in (the columns are LoadFromDB's).
    class KitQueryHolder : public CharacterDatabaseQueryHolder
    {
    public:
        explicit KitQueryHolder(ObjectGuid::LowType guid) : _guid(guid) { }

        bool Initialize()
        {
            SetSize(MAX_KIT_QUERY);
            bool res = true;
            auto add = [&](KitQuery index, CharacterDatabaseStatements statement)
            {
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(statement);
                stmt->setUInt32(0, _guid);
                res &= SetPreparedQuery(index, stmt);
            };
            add(KIT_QUERY_CHARACTER, CHAR_SEL_CHARACTER);
            add(KIT_QUERY_SPELLS, CHAR_SEL_CHARACTER_SPELL);
            add(KIT_QUERY_TALENTS, CHAR_SEL_CHARACTER_TALENTS);
            add(KIT_QUERY_SKILLS, CHAR_SEL_CHARACTER_SKILLS);
            add(KIT_QUERY_ACTIONS, CHAR_SEL_CHARACTER_ACTIONS);
            add(KIT_QUERY_GLYPHS, CHAR_SEL_CHARACTER_GLYPHS);
            add(KIT_QUERY_INVENTORY, CHAR_SEL_CHARACTER_INVENTORY);
            add(KIT_QUERY_PETS, CHAR_SEL_CHAR_PETS);
            add(KIT_QUERY_QUESTS, CHAR_SEL_CHARACTER_QUESTSTATUSREW);
            add(KIT_QUERY_ACCOUNT_DATA, CHAR_SEL_PLAYER_ACCOUNT_DATA);
            return res;
        }

        ObjectGuid::LowType GetGuid() const { return _guid; }

    private:
        ObjectGuid::LowType _guid;
    };

    uint32 ClassBit(uint8 playerClass) { return playerClass ? 1u << (playerClass - 1) : 0; }

    // Level-60 characters of the realm's playerbot accounts, by class. A bot's
    // lineup plays a class it has nobody for as one of them at random rather
    // than as its one starter template every time (owner: "the lives should be
    // a little more random ... take from other specs"). A player's lineup
    // never does: a player gets the class's starter template (BuildLineup).
    // Read once at startup (Update); like a template, a stand-in is worn under
    // the player's name.
    std::mutex StandInMutex;
    std::array<std::vector<ObjectGuid::LowType>, MAX_CLASSES> StandIns;
    std::unordered_set<ObjectGuid::LowType> StandInGuids;
    bool StandInsAsked = false;

    bool IsStandInGuid(ObjectGuid::LowType guid)
    {
        std::lock_guard<std::mutex> lock(StandInMutex);
        return StandInGuids.count(guid) != 0;
    }

    ObjectGuid::LowType PickStandIn(uint8 playerClass)
    {
        if (playerClass >= MAX_CLASSES)
            return 0;
        std::lock_guard<std::mutex> lock(StandInMutex);
        std::vector<ObjectGuid::LowType> const& candidates = StandIns[playerClass];
        return candidates.empty() ? 0 : Trinity::Containers::SelectRandomContainerElement(candidates);
    }

    bool IsTemplateGuid(ObjectGuid::LowType guid)
    {
        if (!guid)
            return false;
        for (ObjectGuid::LowType templateGuid : Config.Templates)
            if (templateGuid == guid)
                return true;
        return IsStandInGuid(guid);
    }

    void Publish(ObjectGuid::LowType source, std::shared_ptr<Kit> kit)
    {
        std::lock_guard<std::mutex> lock(KitMutex);
        KitEntry& entry = Kits[source];
        if (kit)
        {
            entry.State = KitState::Ready;
            entry.Value = std::move(kit);
        }
        else
        {
            entry.State = KitState::Failed;
            entry.Value.reset();
        }
    }

    // ---------------------------------------------------------- kit parsing
    std::string Trim(std::string_view text)
    {
        size_t first = 0;
        while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])))
            ++first;
        size_t last = text.size();
        while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
            --last;
        return std::string(text.substr(first, last - first));
    }

    std::string Lower(std::string text)
    {
        for (char& c : text)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // bindings-cache: "BINDINGMODE n", then "bind KEY COMMAND" per changed key.
    void ParseBindings(std::string const& data, Kit& kit)
    {
        kit.OwnBindings = !Trim(data).empty();
        for (std::string_view raw : Trinity::Tokenize(data, '\n', false))
        {
            std::string const line = Trim(raw);
            if (line.rfind("bind ", 0) != 0)
                continue;
            std::string const rest = Trim(std::string_view(line).substr(5));
            size_t const space = rest.find(' ');
            if (space == std::string::npos || !space)
                continue;
            std::string command = Trim(std::string_view(rest).substr(space + 1));
            if (!command.empty())
                kit.Bindings.emplace_back(rest.substr(0, space), std::move(command));
        }
    }

    // macros-cache: "MACRO <id> \"<name>\" <icon>", the body, "END". A
    // character macro's id is 0x1000000 | its number on the action buttons.
    void ParseMacros(std::string const& data, Kit& kit)
    {
        uint32 current = 0;
        Kit::Macro macro;
        for (std::string_view raw : Trinity::Tokenize(data, '\n', true))
        {
            std::string const line = Trim(raw);
            if (line.rfind("MACRO ", 0) == 0)
            {
                std::string_view const rest = std::string_view(line).substr(6);
                uint32 const id = Trinity::StringTo<uint32>(rest.substr(0, rest.find(' '))).value_or(0);
                current = (id & 0x1000000) ? (id & 0xFFFFFF) : 0;
                macro = Kit::Macro();
                size_t const open = line.find('"');
                size_t const close = open == std::string::npos ? std::string::npos : line.find('"', open + 1);
                if (close != std::string::npos)
                {
                    macro.Name = line.substr(open + 1, close - open - 1);
                    macro.Icon = Trim(std::string_view(line).substr(close + 1));
                }
            }
            else if (line == "END")
            {
                if (current)
                    kit.CharacterMacros[current] = macro;
                current = 0;
            }
            else if (current)
                macro.Body += line + '\n';
        }
    }

    // The spells a macro casts, in the order it would try them: every
    // "#showtooltip X", "/cast", "/use", "/castsequence" and "/castrandom" option, with its
    // [conditions], ! and (Rank N) taken off - the rank kept apart.
    std::vector<std::pair<std::string, uint32>> MacroSpellCandidates(std::string const& body)
    {
        std::vector<std::pair<std::string, uint32>> candidates;
        for (std::string_view raw : Trinity::Tokenize(body, '\n', false))
        {
            std::string const line = Trim(raw);
            std::string const lower = Lower(line);
            std::string rest;
            for (char const* command : { "#showtooltip ", "/castsequence ", "/castrandom ", "/cast ", "/use " })
            {
                if (lower.rfind(command, 0) == 0)
                {
                    rest = line.substr(std::strlen(command));
                    break;
                }
            }
            if (rest.empty())
                continue;

            for (std::string_view option : Trinity::Tokenize(rest, ';', false))
            {
                std::string name = Trim(option);
                while (!name.empty() && name[0] == '[')
                {
                    size_t const close = name.find(']');
                    name = close == std::string::npos ? std::string() : Trim(std::string_view(name).substr(close + 1));
                }
                if (Lower(name).rfind("reset=", 0) == 0)
                {
                    size_t const space = name.find(' ');
                    name = space == std::string::npos ? std::string() : Trim(std::string_view(name).substr(space + 1));
                }
                size_t const comma = name.find(',');
                if (comma != std::string::npos)
                    name = Trim(std::string_view(name).substr(0, comma));
                while (!name.empty() && (name[0] == '!' || name[0] == '?'))
                    name.erase(0, 1);

                uint32 rank = 0;
                size_t const paren = name.find('(');
                if (paren != std::string::npos)
                {
                    std::string const inside = Lower(name.substr(paren + 1));
                    if (inside.rfind("rank ", 0) == 0)
                        rank = Trinity::StringTo<uint32>(std::string_view(inside).substr(5, inside.find(')') - 5)).value_or(0);
                    name = Trim(std::string_view(name).substr(0, paren));
                }
                if (!name.empty())
                    candidates.emplace_back(name, rank);
            }
        }
        return candidates;
    }

    // The known spell a macro line names: that rank when it names one and it
    // is in the spellbook, else the highest rank the spellbook lists.
    uint32 FindKnownSpell(Player const* player, std::string const& name, uint32 rank)
    {
        std::string const wanted = Lower(name);
        std::string const wantedRank = rank ? Trinity::StringFormat("rank {}", rank) : std::string();
        uint32 best = 0;
        uint8 bestRank = 0;
        for (auto const& [spell, known] : player->GetSpellMap())
        {
            if (known.state == PLAYERSPELL_REMOVED || known.disabled || !known.active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            if (!info || !info->SpellName[LOCALE_enUS] || Lower(info->SpellName[LOCALE_enUS]) != wanted)
                continue;
            if (rank && info->Rank[LOCALE_enUS] && Lower(info->Rank[LOCALE_enUS]) == wantedRank)
                return spell;
            uint8 const thisRank = sSpellMgr->GetSpellRank(spell);
            if (!best || thisRank > bestRank)
            {
                best = spell;
                bestRank = thisRank;
            }
        }
        return best;
    }

    // Food, drink or a bandage, by what it does - the test the tournament's
    // free match consumables use (TournamentLoadout.cpp): a bandage by its
    // mechanic, food and drink as a health or power regen you sit for.
    bool IsRefreshment(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE)
            return false;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId);
            if (!info)
                continue;
            if (info->Mechanic == MECHANIC_BANDAGE)
                return true;
            if ((info->HasAura(SPELL_AURA_MOD_REGEN) || info->HasAura(SPELL_AURA_MOD_POWER_REGEN)) &&
                (info->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED))
                return true;
        }
        return false;
    }

    std::shared_ptr<Kit> ParseKit(KitQueryHolder const& holder)
    {
        PreparedQueryResult character = holder.GetPreparedResult(KIT_QUERY_CHARACTER);
        if (!character)
            return nullptr;

        auto kit = std::make_shared<Kit>();
        kit->Source = holder.GetGuid();
        kit->StandIn = IsStandInGuid(kit->Source);
        kit->Template = kit->StandIn || IsTemplateGuid(kit->Source);

        // The columns of CHAR_SEL_CHARACTER (Player::LoadFromDB lists them).
        Field* fields = character->Fetch();
        kit->Name = fields[2].GetString();
        kit->Race = fields[3].GetUInt8();
        kit->Class = fields[4].GetUInt8();
        kit->Gender = fields[5].GetUInt8();
        kit->Skin = fields[9].GetUInt8();
        kit->Face = fields[10].GetUInt8();
        kit->HairStyle = fields[11].GetUInt8();
        kit->HairColor = fields[12].GetUInt8();
        kit->FacialStyle = fields[13].GetUInt8();
        kit->TournamentMode = (fields[36].GetUInt16() & PLAYER_EXTRA_TOURNAMENT_MODE) != 0;
        uint8 const activeGroup = fields[65].GetUInt8();
        kit->Ammo = fields[68].GetUInt32();

        if (!IsPlayableClass(kit->Class) || !sObjectMgr->GetPlayerInfo(kit->Race, kit->Class) || kit->Gender >= GENDER_NONE)
        {
            TC_LOG_ERROR("bg.battleground", "All-Star: character {} ({}) cannot be worn: race {} class {} gender {}.",
                kit->Name, kit->Source, uint32(kit->Race), uint32(kit->Class), uint32(kit->Gender));
            return nullptr;
        }

        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_SPELLS))
        {
            do
            {
                Field* row = result->Fetch();
                kit->Spells.push_back({ row[0].GetUInt32(), row[1].GetBool(), row[2].GetBool() });
            } while (result->NextRow());
        }

        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_TALENTS))
        {
            do
            {
                Field* row = result->Fetch();
                if (row[1].GetUInt8() == activeGroup)
                    kit->Talents.push_back(row[0].GetUInt32());
            } while (result->NextRow());
        }

        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_SKILLS))
        {
            do
            {
                Field* row = result->Fetch();
                kit->Skills.push_back({ row[0].GetUInt16(), row[1].GetUInt16(), row[2].GetUInt16() });
            } while (result->NextRow());
        }

        // Already the active group's (the statement joins activeTalentGroup).
        // A macro or an equipment set on a button is only an index: the client
        // looks it up among ITS character's macros and sets. Sets are left
        // off; character macros are kept here and turned into the spell the
        // macro casts in Build (CharacterMacros); account macros are the
        // player's too, except for a template, which lives on another account.
        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_ACTIONS))
        {
            do
            {
                Field* row = result->Fetch();
                uint8 const type = row[2].GetUInt8();
                if (type == ACTION_BUTTON_EQSET || (kit->Template && type == ACTION_BUTTON_MACRO))
                    continue;
                kit->Buttons.push_back({ row[0].GetUInt8(), row[1].GetUInt32(), type });
            } while (result->NextRow());
        }

        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_QUESTS))
        {
            do
                kit->RewardedQuests.push_back(result->Fetch()[0].GetUInt32());
            while (result->NextRow());
        }

        //   0     1     2
        // type, time, data
        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_ACCOUNT_DATA))
        {
            do
            {
                Field* row = result->Fetch();
                uint8 const type = row[0].GetUInt8();
                std::string const data = row[2].GetString();
                if (type == PER_CHARACTER_BINDINGS_CACHE)
                    ParseBindings(data, *kit);
                else if (type == PER_CHARACTER_MACROS_CACHE)
                    ParseMacros(data, *kit);
            } while (result->NextRow());
        }

        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_GLYPHS))
        {
            do
            {
                Field* row = result->Fetch();
                if (row[0].GetUInt8() != activeGroup)
                    continue;
                for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
                    kit->Glyphs[i] = row[1 + i].GetUInt16();
            } while (result->NextRow());
        }

        //   0            1                2      3         4        5      6             7                 8           9           10    11   12
        // creatorGuid, giftCreatorGuid, count, duration, charges, flags, enchantments, randomPropertyId, durability, playedTime, text, bag, slot,
        //   13    14
        // item, itemEntry
        if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_INVENTORY))
        {
            // ORDER BY bag, slot: everything on the character itself (bag 0),
            // the bags among it, comes before what is inside the bags.
            std::unordered_set<uint32> carriedBags;
            std::map<uint32, uint32> consumables;
            do
            {
                Field* row = result->Fetch();
                uint32 const bag = row[11].GetUInt32();
                uint8 const slot = row[12].GetUInt8();
                uint32 const entry = row[14].GetUInt32();
                bool const carried = bag ? carriedBags.count(bag) != 0
                    : (slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END);
                if (!bag && slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_BAG_END)
                    carriedBags.insert(row[13].GetUInt32());
                if (carried && IsRefreshment(entry))
                    consumables[entry] += std::max<uint32>(1, row[2].GetUInt32());

                if (bag != 0)                           // in a bag, not worn
                    continue;
                if (slot >= EQUIPMENT_SLOT_END)
                    continue;

                KitItem item;
                item.Slot = slot;
                item.Entry = row[14].GetUInt32();
                item.RandomPropertyId = row[7].GetInt16();
                std::vector<std::string_view> const tokens = Trinity::Tokenize(row[6].GetStringView(), ' ', false);
                for (size_t i = 0; i < tokens.size() && i < item.Enchants.size(); ++i)
                    item.Enchants[i] = Trinity::StringTo<uint32>(tokens[i]).value_or(0);
                kit->Items.push_back(item);
            } while (result->NextRow());
            kit->Consumables.assign(consumables.begin(), consumables.end());
        }

        //   0   1      2        3      4    5           6     7     8        9          10       11            12      13        14              15
        // id, entry, modelid, level, exp, Reactstate, slot, name, renamed, curhealth, curmana, curhappiness, abdata, savetime, CreatedBySpell, PetType
        // A hunter's pet, and a warlock's demon (owner: "for warlocks,
        // automatically summon their last summoned pet").
        if (kit->Class == CLASS_HUNTER || kit->Class == CLASS_WARLOCK)
        {
            PetType const wanted = kit->Class == CLASS_HUNTER ? HUNTER_PET : SUMMON_PET;
            if (PreparedQueryResult result = holder.GetPreparedResult(KIT_QUERY_PETS))
            {
                bool haveCurrent = false;
                uint32 newestPutAway = 0;
                do
                {
                    Field* row = result->Fetch();
                    if (PetType(row[15].GetUInt8()) != wanted)
                        continue;
                    bool const current = row[6].GetUInt8() == PET_SAVE_AS_CURRENT;
                    // The pet out, or else the one that is neither out nor
                    // stabled (dismissed, the hunter dead or mounted at logout)
                    // - a warlock's last one put away, when it has several.
                    if (haveCurrent || (!current && row[6].GetUInt8() != PET_SAVE_NOT_IN_SLOT))
                        continue;
                    if (!current && kit->HasPet && row[13].GetUInt32() < newestPutAway)
                        continue;
                    if (!current)
                        newestPutAway = row[13].GetUInt32();

                    PetStable::PetInfo& pet = kit->Pet;
                    kit->SourcePetNumber = row[0].GetUInt32();
                    pet.CreatureId = row[1].GetUInt32();
                    pet.DisplayId = row[2].GetUInt32();
                    pet.Level = row[3].GetUInt16();
                    pet.Experience = row[4].GetUInt32();
                    pet.ReactState = ReactStates(row[5].GetUInt8());
                    pet.Name = row[7].GetString();
                    pet.WasRenamed = row[8].GetBool();
                    pet.Health = row[9].GetUInt32();
                    pet.Mana = row[10].GetUInt32();
                    pet.Happiness = row[11].GetUInt32();
                    pet.ActionBar = row[12].GetString();
                    pet.LastSaveTime = row[13].GetUInt32();
                    pet.CreatedBySpellId = row[14].GetUInt32();
                    pet.Type = wanted;
                    kit->HasPet = true;
                    haveCurrent = current;
                } while (result->NextRow());
            }
        }

        return kit;
    }

    void LoadKit(ObjectGuid::LowType source)
    {
        auto holder = std::make_shared<KitQueryHolder>(source);
        if (!holder->Initialize())
        {
            Publish(source, nullptr);
            return;
        }

        HolderCallbacks.AddCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
            [source](SQLQueryHolderBase const& result)
        {
            std::shared_ptr<Kit> kit = ParseKit(static_cast<KitQueryHolder const&>(result));
            if (!kit || !kit->HasPet)
            {
                Publish(source, std::move(kit));
                return;
            }

            // A hunter's pet - a warlock's demon - comes with what it was taught.
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_PET_SPELL);
            stmt->setUInt32(0, kit->SourcePetNumber);
            QueryCallbacks.AddCallback(CharacterDatabase.AsyncQuery(stmt).WithPreparedCallback(
                [source, kit](PreparedQueryResult spells) mutable
            {
                if (spells)
                {
                    do
                    {
                        Field* row = spells->Fetch();
                        kit->PetSpells.emplace_back(row[0].GetUInt32(), row[1].GetUInt8());
                    } while (spells->NextRow());
                }
                Publish(source, std::move(kit));
            }));
        });
    }

    // ---------------------------------------------------------- the lineup
    struct ListedCharacter
    {
        ObjectGuid::LowType Guid;
        uint8 Class;
        uint8 Level;
        uint32 Position;
    };

    // standIns: a bot's lineup, whose missing classes are random playerbot
    // characters. A player's are the class's starter template - the standard
    // spec and gear (owner 2026-10-29: "for PLAYERS, they need to transform
    // into the standard class kit"; "how bots work rn is perfect").
    Lineup BuildLineup(uint8 playingClass, std::vector<ListedCharacter> characters, bool standIns)
    {
        std::sort(characters.begin(), characters.end(), [](ListedCharacter const& a, ListedCharacter const& b)
        {
            return a.Position != b.Position ? a.Position < b.Position : a.Guid < b.Guid;
        });

        Lineup lineup;
        uint32 used = 0;
        lineup.push_back({ playingClass, 0, false });
        used |= ClassBit(playingClass);

        for (ListedCharacter const& character : characters)
        {
            if (character.Level < Config.SourceMinLevel || !IsPlayableClass(character.Class) || (used & ClassBit(character.Class)))
                continue;
            lineup.push_back({ character.Class, character.Guid, false });
            used |= ClassBit(character.Class);
        }

        for (uint8 playerClass : CLASS_ORDER)
        {
            if (used & ClassBit(playerClass))
                continue;
            ObjectGuid::LowType source = standIns ? PickStandIn(playerClass) : 0;
            if (!source)
                source = GetTemplateCharacter(playerClass);
            if (source)
                lineup.push_back({ playerClass, source, true });
            used |= ClassBit(playerClass);
        }

        // Life one is always the character that queued; the rest in a random
        // order unless Centurion.AllStar.ShuffleLives is off (owner: "make it
        // random order for everyone. make this a config").
        if (Config.ShuffleLives && lineup.size() > 2)
            std::shuffle(lineup.begin() + 1, lineup.end(), RandomEngine::Instance());

        return lineup;
    }

    // ------------------------------------------------------------- clients
    void SendAddonLine(Player* player, std::string const& line)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        if (!session || session->IsVirtualSession())
            return;

        std::string const message = "CCGAME\tALLSTAR:" + line;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
            message, 0, player->GetName(), player->GetName());
        player->SendDirectMessage(&data);
    }

    void SendSpellbookChange(Player* player, uint32 spell, bool add)
    {
        if (add)
        {
            WorldPacket data(SMSG_LEARNED_SPELL, 6);
            data << uint32(spell);
            data << uint16(0);
            player->SendDirectMessage(&data);
        }
        else
        {
            WorldPacket data(SMSG_REMOVED_SPELL, 4);
            data << uint32(spell);
            player->SendDirectMessage(&data);
        }
    }

    // What the client's spellbook lists: SendInitialSpells' rule.
    std::unordered_set<uint32> VisibleSpells(Player const* player)
    {
        std::unordered_set<uint32> visible;
        for (auto const& [spell, known] : player->GetSpellMap())
            if (known.state != PLAYERSPELL_REMOVED && !known.disabled && known.active)
                visible.insert(spell);
        return visible;
    }

    // Everybody in the match has the old name cached against this guid; the
    // answer to a name query is what the client keeps, so it is sent again.
    void BroadcastName(Player* player)
    {
        Map* map = player->FindMap();
        if (!map)
            return;

        std::string name;
        uint8 race = 0;
        uint8 gender = 0;
        uint8 playerClass = 0;
        if (!GetPersonaNameData(player->GetGUID(), name, race, gender, playerClass))
        {
            CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(player->GetGUID());
            name = Surnames::Decorated(player->GetGUID(), cache ? cache->Name : player->GetName());
            race = player->GetRace();
            gender = player->GetNativeGender();
            playerClass = player->GetClass();
        }

        WorldPacket data(SMSG_NAME_QUERY_RESPONSE, 8 + 1 + name.size() + 1 + 1 + 1 + 1 + 1 + 1);
        data << player->GetGUID().WriteAsPacked();
        data << uint8(0);
        data << name;
        data << uint8(0);
        data << uint8(race);
        data << uint8(gender);
        data << uint8(playerClass);
        data << uint8(0);

        // Everybody but the player: its own client keeps its real name, which
        // the addons compare the server's whispers to (sender == UnitName).
        for (MapReference const& ref : map->GetPlayers())
            if (Player* viewer = ref.GetSource())
                if (viewer != player && viewer->GetSession() && !viewer->GetSession()->IsVirtualSession())
                    viewer->SendDirectMessage(&data);
    }

    // LEARNED first, then ONE bars packet, then REMOVED: the client clears a
    // button (and tells the server) for any spell it does not know when the
    // bars arrive, and for every spell taken away while a button holds it.
    void SyncClient(Player* player, std::unordered_set<uint32> const& before, std::unordered_set<uint32>& shown)
    {
        BroadcastName(player);

        // Party and raid frames read class, power and health from the party
        // stats and rebuild on the group list - a swap changes all of them,
        // for a bot's teammates too.
        player->SetGroupUpdateFlag(GROUP_UPDATE_FULL);
        if (Group* group = player->GetGroup())
            group->SendUpdate();

        WorldSession* session = player->GetSession();
        if (!session || session->IsVirtualSession())
            return;

        std::unordered_set<uint32> const after = VisibleSpells(player);
        uint32 learned = 0;
        for (uint32 spell : after)
            if (!before.count(spell))
            {
                SendSpellbookChange(player, spell, true);
                ++learned;
            }

        player->SendTalentsInfoData(false);
        player->SendActionButtons(1);

        uint32 removed = 0;
        shown.insert(before.begin(), before.end());
        for (uint32 spell : shown)
            if (!after.count(spell))
            {
                SendSpellbookChange(player, spell, false);
                ++removed;
            }
        shown.insert(after.begin(), after.end());

        // What the client was told, for the reports of bars and spellbooks
        // it did not show (2026-10-28: a shaman's bars bare, a druid still
        // offered Execute): every spell button whose spell it never heard of.
        uint32 buttons = 0;
        std::string unknown;
        for (uint8 slot = 0; slot < MAX_ACTION_BUTTONS; ++slot)
        {
            ActionButton const* button = player->GetActionButton(slot);
            if (!button)
                continue;
            ++buttons;
            if (button->GetType() == ACTION_BUTTON_SPELL && !after.count(button->GetAction()))
                unknown += Trinity::StringFormat(" {}:{}", uint32(slot), button->GetAction());
        }
        TC_LOG_INFO("bg.battleground", "All-Star: {} ({}) client sync: {} spells learned, {} removed, {} known, {} buttons{}{}.",
            player->GetName(), player->GetGUID().GetCounter(), learned, removed, uint32(after.size()), buttons,
            unknown.empty() ? "" : ", spell buttons it was not told about:", unknown);

        player->SendProficiency(ITEM_CLASS_WEAPON, player->GetWeaponProficiency());
        player->SendProficiency(ITEM_CLASS_ARMOR, player->GetArmorProficiency());
        if (player->GetPet())
            player->PetSpellInitialize();
    }

    // A warrior comes into a life in Battle Stance, as from a login (owner:
    // "warriors should start in battle stance").
    void EnterDefaultStance(Player* player)
    {
        uint32 constexpr BATTLE_STANCE = 2457;
        if (player->GetClass() == CLASS_WARRIOR && player->IsAlive() && player->HasSpell(BATTLE_STANCE) &&
            player->GetShapeshiftForm() == FORM_NONE)
            player->CastSpell(player, BATTLE_STANCE, true);
    }

    // Characters whose client holds a borrowed character's macros in the
    // character's own macro slots (CENTURION_AllStar), from the first macro
    // sent until the addon says they are back (ALLSTAR:MACROS:RESTORED).
    std::mutex BorrowedMutex;
    std::unordered_set<ObjectGuid::LowType> MacrosBorrowed;

    // The stored macros are the character's own: they are kept, and stored a
    // second newer than anything the client can hold, so a client that goes
    // away still holding the borrowed ones takes these back at its next login
    // (SMSG_ACCOUNT_DATA_TIMES: a newer server copy is downloaded).
    void KeepRealMacros(WorldSession* session, time_t clientTime)
    {
        AccountData const* data = session->GetAccountData(PER_CHARACTER_MACROS_CACHE);
        if (data && !data->Data.empty())
            session->SetAccountData(PER_CHARACTER_MACROS_CACHE,
                std::max<time_t>({ data->Time, clientTime, time_t(GameTime::GetGameTime()) }) + 1, data->Data);
    }

    void NoteMacrosBorrowed(Player* player)
    {
        {
            std::lock_guard<std::mutex> lock(BorrowedMutex);
            if (!MacrosBorrowed.insert(player->GetGUID().GetCounter()).second)
                return;
        }
        KeepRealMacros(player->GetSession(), 0);
    }

    void HandleAddonRequest(Player* player, std::string_view argument)
    {
        if (argument == "MACROS:RESTORED")
        {
            std::lock_guard<std::mutex> lock(BorrowedMutex);
            MacrosBorrowed.erase(player->GetGUID().GetCounter());
        }
    }

    // CENTURION_AllStar writes the persona's character macros over the
    // player's own macro slots for the life (new ones past them, up to the
    // game's 18), puts each on the buttons it sat on, and puts every slot back
    // at the next MAC:RESET or MAC:SWAP (every swap and the restore start with
    // one):
    //   MAC:B:<buttons>:<icon>:<name>, MAC:L:<line> and MAC:C:<more of that
    //   line>, MAC:E per macro, MAC:DONE after the last.
    void SendMacros(Player* player, Kit const* kit)
    {
        WorldSession* session = player->GetSession();
        if (!session || session->IsVirtualSession())
            return;

        std::map<uint32, std::string> buttons;
        if (kit)
            for (KitButton const& button : kit->Buttons)
                if (button.Type == ACTION_BUTTON_CMACRO && button.Button < MAX_ACTION_BUTTONS && kit->CharacterMacros.count(button.Action))
                {
                    std::string& list = buttons[button.Action];
                    list += (list.empty() ? "" : ",") + std::to_string(uint32(button.Button));
                }

        // A bare RESET puts the player's own macros back with nothing borrowed
        // after it, and the addon confirms it (ALLSTAR:MACROS:RESTORED). SWAP
        // puts them back too, before the next character's: no confirmation,
        // which would arrive after the borrow below and undo it.
        if (buttons.empty())
        {
            SendAddonLine(player, "MAC:RESET");
            return;
        }

        NoteMacrosBorrowed(player);
        SendAddonLine(player, "MAC:SWAP");

        // An addon line is a whisper: 255 bytes with its prefix.
        size_t constexpr CHUNK = 200;
        for (auto const& [id, list] : buttons)
        {
            Kit::Macro const& macro = kit->CharacterMacros.at(id);
            SendAddonLine(player, "MAC:B:" + list + ":" + macro.Icon + ":" + macro.Name.substr(0, 64));
            for (std::string_view line : Trinity::Tokenize(macro.Body, '\n', false))
                for (size_t at = 0; at < line.size(); at += CHUNK)
                    SendAddonLine(player, (at ? "MAC:C:" : "MAC:L:") + std::string(line.substr(at, CHUNK)));
            SendAddonLine(player, "MAC:E");
        }
        SendAddonLine(player, "MAC:DONE");
    }

    // CENTURION_AllStar puts the persona's keybindings on the client (never
    // saved there) and the player's own back on RESET.
    void SendBindings(Player* player, Kit const* kit)
    {
        WorldSession* session = player->GetSession();
        if (!session || session->IsVirtualSession())
            return;

        if (!kit || kit->Template)
        {
            SendAddonLine(player, "BIND:RESET");
            return;
        }
        if (!kit->OwnBindings)
        {
            SendAddonLine(player, "BIND:ACCOUNT");
            return;
        }
        SendAddonLine(player, "BIND:BEGIN");
        for (auto const& [key, command] : kit->Bindings)
            SendAddonLine(player, "BIND:" + key + " " + command);
        SendAddonLine(player, "BIND:END");
    }

    void NoteSwap(Player const* player)
    {
        std::lock_guard<std::mutex> lock(SwapTimeMutex);
        SwapTimes[player->GetGUID()] = GameTime::GetGameTimeMS();
    }
}

// =================================================================== personas

struct PersonaStash
{
    // The character's own, put aside the first time it changed class.
    uint8 Race = 0;
    uint8 Class = 0;
    uint8 Gender = 0;
    uint8 NativeGender = 0;
    uint8 Skin = 0;
    uint8 Face = 0;
    uint8 HairStyle = 0;
    uint8 HairColor = 0;
    uint8 FacialStyle = 0;
    PlayerSpellMap Spells;
    std::array<PlayerTalentMap*, MAX_TALENT_SPECS> Talents{};
    uint8 ActiveSpec = 0;
    uint8 SpecsCount = 1;
    uint32 Glyphs[MAX_TALENT_SPECS][MAX_GLYPH_SLOT_INDEX] = {};
    std::array<uint32, MAX_GLYPH_SLOT_INDEX> GlyphFields{};
    ActionButtonList Buttons;
    SkillStatusMap Skills;
    std::array<uint32, PLAYER_MAX_SKILLS * 3> SkillFields{};
    uint32 WeaponProficiency = 0;
    uint32 ArmorProficiency = 0;
    bool CanParry = false;
    bool CanBlock = false;
    bool CanDualWield = false;
    bool CanTitanGrip = false;
    uint32 UsedTalentCount = 0;
    uint32 FreeTalentPoints = 0;
    uint32 Ammo = 0;
    std::array<Item*, EQUIPMENT_SLOT_END> Equipment{};
    std::unique_ptr<PetStable> Stable;
    uint32 TemporaryUnsummonedPetNumber = 0;
    uint32 LastPetNumber = 0;
    uint32 OldPetSpell = 0;

    // Every spell this client has been told it knows since the first swap: a
    // swap unlearns all of them the next character lacks, not only the last
    // life's (a druid was still offered Execute and Overpower).
    std::unordered_set<uint32> ClientSpells;

    // The persona being worn.
    KitPtr Persona;
    std::vector<ObjectGuid> Issued;         // its gear and ammo, never saved
    uint32 PersonaPetNumber = 0;
    bool PetSpellsTaught = true;

    ~PersonaStash()
    {
        // Only reached with something in it when the character went away
        // without a restore (a bot torn down mid-match): the rows were never
        // touched, so what is left here is memory and nothing more.
        for (PlayerTalentMap*& talents : Talents)
        {
            if (!talents)
                continue;
            for (auto& [spell, talent] : *talents)
                delete talent;
            delete talents;
            talents = nullptr;
        }
        for (Item*& item : Equipment)
        {
            delete item;
            item = nullptr;
        }
    }
};

void PersonaStashDeleter::operator()(PersonaStash* stash) const
{
    delete stash;
}

// Everything that reaches into the character's private state. A friend of
// Player (Player.h).
class PersonaAccess
{
public:
    static bool Apply(Player* player, KitPtr const& kit);
    static void Restore(Player* player);
    static void Maintain(Player* player);

    static PersonaStash* Stash(Player const* player) { return player->m_allStarPersona.get(); }

private:
    static void Strip(Player* player);
    static void RemovePersonaPet(Player* player);
    static void TakeAside(Player* player, PersonaStash& stash);
    static void DropPersona(Player* player, PersonaStash& stash);
    static void Build(Player* player, PersonaStash& stash, Kit const& kit);
    static void LoadSkills(Player* player, Kit const& kit);
    static void EquipKit(Player* player, PersonaStash& stash, Kit const& kit);
    static void GiveAmmo(Player* player, PersonaStash& stash, uint32 ammo);
    static void GiveConsumables(Player* player, PersonaStash& stash, Kit const& kit);
    static void GiveConjured(Player* player, PersonaStash& stash);
    static void SummonPet(Player* player, PersonaStash& stash, Kit const& kit);
    static void PutBack(Player* player, uint8 slot, Item* item);
    static void RecastPassives(Player* player);
    static void ClearSkillFields(Player* player);
};

void PersonaAccess::Strip(Player* player)
{
    player->CombatStopWithPets(true);
    player->InterruptNonMeleeSpells(true);
    player->UnsummonAllTotems();
    player->RemoveAllDynObjects();
    player->RemoveAllGameObjects();

    Pet* pet = player->GetPet();
    std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
    for (Unit* unit : controlled)
        if (unit && unit != pet && unit->GetTypeId() == TYPEID_UNIT && unit->IsSummon() && !unit->IsTotem())
            unit->ToTempSummon()->UnSummon();

    // Everything but what the gear put on, which comes off with the gear, and
    // what lasts through a death that is not the class's (the World/Tournament/
    // Bot markers, the lobby flags): the class passives, the fight's buffs and
    // debuffs, and every stance, form and lasting aura of the player's own
    // class spells - Battle Stance and the other stances are death-persistent,
    // and one left on rode into the next life (a shaman whose main bar was the
    // warrior's stance page) and was saved on the real character at the end.
    ObjectGuid const self = player->GetGUID();
    auto const strip = [self](Aura const* aura)
    {
        if (aura->GetCastItemGUID())
            return false;
        if (!aura->IsDeathPersistent())
            return true;
        SpellInfo const* info = aura->GetSpellInfo();
        return info->HasAura(SPELL_AURA_MOD_SHAPESHIFT) ||
            (aura->GetCasterGUID() == self && info->SpellFamilyName != SPELLFAMILY_GENERIC);
    };
    player->RemoveOwnedAuras(strip);
    player->RemoveAppliedAuras([&strip](AuraApplication const* application) { return strip(application->GetBase()); });

    player->RemoveArenaSpellCooldowns(true);
    player->ClearComboPoints();
}

void PersonaAccess::RemovePersonaPet(Player* player)
{
    // A persona's pet has no rows: the save it is removed with does nothing
    // (Pet::SavePetToDB asks HasAllStarPersona).
    if (Pet* pet = player->GetPet())
        player->RemovePet(pet, PET_SAVE_NOT_IN_SLOT);
}

void PersonaAccess::ClearSkillFields(Player* player)
{
    for (uint32 i = 0; i < PLAYER_MAX_SKILLS * 3; ++i)
        player->SetUInt32Value(PLAYER_SKILL_INFO_1_1 + i, 0);
}

void PersonaAccess::TakeAside(Player* player, PersonaStash& stash)
{
    stash.Race = player->GetRace();
    stash.Class = player->GetClass();
    stash.Gender = player->GetGender();
    stash.NativeGender = player->GetNativeGender();
    stash.Skin = player->GetSkinId();
    stash.Face = player->GetFaceId();
    stash.HairStyle = player->GetHairStyleId();
    stash.HairColor = player->GetHairColorId();
    stash.FacialStyle = player->GetFacialStyle();

    // The gear comes off whole - the same Item objects, enchants and all - and
    // waits here; nothing about it is written anywhere.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            player->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
            stash.Equipment[slot] = item;
        }
    }
    stash.Ammo = player->GetUInt32Value(PLAYER_AMMO_ID);
    if (stash.Ammo)
        player->RemoveAmmo();

    stash.Spells.swap(player->m_spells);
    for (uint8 spec = 0; spec < MAX_TALENT_SPECS; ++spec)
    {
        stash.Talents[spec] = player->m_talents[spec];
        player->m_talents[spec] = new PlayerTalentMap();
    }
    stash.ActiveSpec = player->m_activeSpec;
    stash.SpecsCount = player->m_specsCount;
    std::copy(&player->m_Glyphs[0][0], &player->m_Glyphs[0][0] + MAX_TALENT_SPECS * MAX_GLYPH_SLOT_INDEX, &stash.Glyphs[0][0]);
    for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
        stash.GlyphFields[i] = player->GetUInt32Value(PLAYER_FIELD_GLYPHS_1 + i);
    stash.Buttons.swap(player->m_actionButtons);
    stash.Skills.swap(player->mSkillStatus);
    for (uint32 i = 0; i < PLAYER_MAX_SKILLS * 3; ++i)
        stash.SkillFields[i] = player->GetUInt32Value(PLAYER_SKILL_INFO_1_1 + i);
    stash.WeaponProficiency = player->m_WeaponProficiency;
    stash.ArmorProficiency = player->m_ArmorProficiency;
    stash.CanParry = player->CanParry();
    stash.CanBlock = player->CanBlock();
    stash.CanDualWield = player->CanDualWield();
    stash.CanTitanGrip = player->CanTitanGrip();
    stash.UsedTalentCount = player->m_usedTalentCount;
    stash.FreeTalentPoints = player->GetFreeTalentPoints();

    stash.Stable = std::move(player->m_petStable);
    stash.TemporaryUnsummonedPetNumber = player->m_temporaryUnsummonedPetNumber;
    stash.LastPetNumber = player->m_lastpetnumber;
    stash.OldPetSpell = player->m_oldpetspell;
    player->m_temporaryUnsummonedPetNumber = 0;
}

void PersonaAccess::DropPersona(Player* player, PersonaStash& stash)
{
    for (ObjectGuid const& guid : stash.Issued)
        if (Item* item = player->GetItemByGuid(guid))
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    stash.Issued.clear();
    if (player->GetUInt32Value(PLAYER_AMMO_ID))
        player->RemoveAmmo();

    player->m_spells.clear();
    for (uint8 spec = 0; spec < MAX_TALENT_SPECS; ++spec)
    {
        for (auto& [spell, talent] : *player->m_talents[spec])
            delete talent;
        player->m_talents[spec]->clear();
    }
    player->m_actionButtons.clear();
    player->mSkillStatus.clear();
    player->m_petStable.reset();
    player->m_temporaryUnsummonedPetNumber = 0;
    stash.PersonaPetNumber = 0;
    stash.PetSpellsTaught = true;
}

void PersonaAccess::LoadSkills(Player* player, Kit const& kit)
{
    // Player::_LoadSkills, without its two writes: a value-0 skill is simply
    // not taken (the login deletes the row). Its skill-reward spells are taught
    // after, as the login does: they are "dependent" and never saved, so the
    // kit's spell rows do not have them - a troll's Berserking, among others,
    // was missing from every borrowed character.
    std::vector<std::pair<uint16, uint16>> loaded;
    uint32 count = 0;
    for (KitSkill const& skill : kit.Skills)
    {
        SkillRaceClassInfoEntry const* rcEntry = GetSkillRaceClassInfo(skill.Id, player->GetRace(), player->GetClass());
        if (!rcEntry)
            continue;

        uint16 value = skill.Value;
        uint16 max = skill.Max;
        switch (GetSkillRangeType(rcEntry))
        {
            case SKILL_RANGE_LANGUAGE:
                value = max = 300;
                break;
            case SKILL_RANGE_MONO:
                value = max = 1;
                break;
            case SKILL_RANGE_LEVEL:
                max = player->GetMaxSkillValueForLevel();
                break;
            default:
                break;
        }
        if (!value)
            continue;

        uint16 step = 0;
        if (SkillTiersEntry const* skillTier = sSkillTiersStore.LookupEntry(rcEntry->SkillTierID))
        {
            for (uint32 i = 0; i < MAX_SKILL_STEP; ++i)
            {
                if (skillTier->Value[step] == max)
                {
                    step = i + 1;
                    break;
                }
            }
        }

        player->SetUInt32Value(PLAYER_SKILL_INFO_1_1 + count * 3, MAKE_PAIR32(skill.Id, step));
        player->SetUInt32Value(PLAYER_SKILL_INFO_1_1 + count * 3 + 1, MAKE_PAIR32(value, max));
        player->SetUInt32Value(PLAYER_SKILL_INFO_1_1 + count * 3 + 2, 0);
        player->mSkillStatus.insert(SkillStatusMap::value_type(skill.Id, SkillStatusData(count, SKILL_UNCHANGED)));
        loaded.emplace_back(skill.Id, value);

        if (++count >= PLAYER_MAX_SKILLS)
            break;
    }

    for (auto const& [skill, value] : loaded)
        player->LearnSkillRewardedSpells(skill, value);

    if (player->HasSkill(SKILL_FIST_WEAPONS))
        player->SetSkill(SKILL_FIST_WEAPONS, 0, player->GetSkillValue(SKILL_UNARMED), player->GetMaxSkillValueForLevel());
}

void PersonaAccess::PutBack(Player* player, uint8 slot, Item* item)
{
    uint16 const pos = (uint16(INVENTORY_SLOT_BAG_0) << 8) | slot;
    if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
    {
        player->EquipItem(pos, item, true);
        return;
    }

    // Unreachable while the gear lock holds (IsBattlegroundEquipChangeAllowed),
    // kept so a character's own item can never be lost: into the bags.
    ItemPosCountVec dest;
    if (player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
    {
        player->StoreItem(dest, item, true);
        return;
    }

    TC_LOG_ERROR("bg.battleground", "All-Star: {} ({}) had no room for its own item {} (guid {}) from slot {}; it stays where the database has it.",
        player->GetName(), player->GetGUID().GetCounter(), item->GetEntry(), item->GetGUID().GetCounter(), uint32(slot));
    // Its row still says it is in this slot; at the next login it is loaded
    // and, if the slot is taken, mailed by the stock inventory load.
    RemoveItemFromUpdateQueueOf(item, player);
    delete item;
}

void PersonaAccess::GiveAmmo(Player* player, PersonaStash& stash, uint32 ammo)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(ammo);
    if (!proto)
        return;

    // A stack of its own in an empty slot - never merged into one the character
    // owns, or taking it back would take theirs too. Ammunition is not spent
    // in a match, so one stack lasts.
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            continue;
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(INVENTORY_SLOT_BAG_0, slot, dest, ammo, std::max<uint32>(1, proto->GetMaxStackSize())) != EQUIP_ERR_OK)
            continue;
        if (Item* item = player->StoreNewItem(dest, ammo, true))
        {
            stash.Issued.push_back(item->GetGUID());
            player->SetAmmo(ammo);
        }
        return;
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Item* bagItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        Bag* bag = bagItem ? bagItem->ToBag() : nullptr;
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            if (bag->GetItemByPos(uint8(slot)))
                continue;
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(bagSlot, uint8(slot), dest, ammo, std::max<uint32>(1, proto->GetMaxStackSize())) != EQUIP_ERR_OK)
                continue;
            if (Item* item = player->StoreNewItem(dest, ammo, true))
            {
                stash.Issued.push_back(item->GetGUID());
                player->SetAmmo(ammo);
            }
            return;
        }
    }
}

void PersonaAccess::GiveConsumables(Player* player, PersonaStash& stash, Kit const& kit)
{
    // The empty slots the bags have, backpack first. Each copy goes into one
    // of them as a stack of its own - never merged into a stack the character
    // owns, or taking the copy back would take theirs too.
    std::vector<std::pair<uint8, uint8>> free;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            free.emplace_back(uint8(INVENTORY_SLOT_BAG_0), slot);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Item* bagItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        Bag* bag = bagItem ? bagItem->ToBag() : nullptr;
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (!bag->GetItemByPos(uint8(slot)))
                free.emplace_back(bagSlot, uint8(slot));
    }

    size_t next = 0;
    uint32 given = 0, left = 0;
    for (auto const& [entry, total] : kit.Consumables)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            continue;
        uint32 const stackSize = std::max<uint32>(1, proto->GetMaxStackSize());
        uint32 remaining = total;
        while (remaining && next < free.size())
        {
            auto const [bag, slot] = free[next++];
            uint32 const count = std::min(remaining, stackSize);
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(bag, slot, dest, entry, count) != EQUIP_ERR_OK)
                continue;
            if (Item* item = player->StoreNewItem(dest, entry, true))
            {
                stash.Issued.push_back(item->GetGUID());
                remaining -= count;
                given += count;
            }
        }
        left += remaining;
    }
    if (left)
        TC_LOG_INFO("bg.battleground", "All-Star: {} ({}) had no room for {} of {}'s food, drink and bandages ({} given).",
            player->GetName(), player->GetGUID().GetCounter(), left, kit.Name, given);
}

void PersonaAccess::GiveConjured(Player* player, PersonaStash& stash)
{
    // A warlock life comes in with its Healthstone and a mage's with its mana
    // gems (owner: "when you switch to a warlock or mage, grant them their
    // mana gem and health stone"): the life's own top Create Healthstone and
    // every Conjure Mana gem it knows, cast for free the way the character
    // would cast them - a talent such as Improved Healthstone still picks the
    // stronger stone, its script does. What they make is issued like the gear.
    if (player->GetClass() != CLASS_WARLOCK && player->GetClass() != CLASS_MAGE)
        return;

    uint32 healthstone = 0;
    uint32 healthstoneLevel = 0;
    std::vector<uint32> manaGems;
    for (auto const& [spell, known] : player->m_spells)
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled || !known.active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info || info->IsPassive() || !info->SpellName[LOCALE_enUS])
            continue;
        if (uint32 const next = sSpellMgr->GetNextSpellInChain(spell))
            if (player->HasSpell(next))
                continue;
        std::string const name = info->SpellName[LOCALE_enUS];
        if (info->SpellFamilyName == SPELLFAMILY_WARLOCK && name.rfind("Create Healthstone", 0) == 0)
        {
            if (!healthstone || info->SpellLevel > healthstoneLevel)
            {
                healthstone = spell;
                healthstoneLevel = info->SpellLevel;
            }
        }
        else if (info->SpellFamilyName == SPELLFAMILY_MAGE && name.rfind("Conjure Mana", 0) == 0)
            manaGems.push_back(spell);
    }
    if (healthstone)
        manaGems.push_back(healthstone);
    if (manaGems.empty())
        return;

    auto const carried = [player]()
    {
        std::unordered_set<ObjectGuid> items;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items.insert(item->GetGUID());
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Item* bagItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
                if (Bag* bag = bagItem->ToBag())
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        if (Item* item = bag->GetItemByPos(uint8(slot)))
                            items.insert(item->GetGUID());
        return items;
    };

    std::unordered_set<ObjectGuid> const before = carried();
    for (uint32 spell : manaGems)
        player->CastSpell(player, spell, true);
    for (ObjectGuid const& guid : carried())
        if (!before.count(guid))
            stash.Issued.push_back(guid);
}

void PersonaAccess::EquipKit(Player* player, PersonaStash& stash, Kit const& kit)
{
    // A match fought under the tournament's rules dresses everyone the same
    // way (Miscellaneous/TournamentLoadout.cpp): the tournament twin of a piece,
    // else the class's starter template piece for the slot - and an empty slot
    // gets the template piece too. Any other match wears the kit as it is.
    bool const tournament = Tournament::IsInTournamentMatch(player);

    std::array<KitItem const*, EQUIPMENT_SLOT_END> worn{};
    for (KitItem const& item : kit.Items)
        if (item.Slot < EQUIPMENT_SLOT_END)
            worn[item.Slot] = &item;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        KitItem const* source = worn[slot];
        uint32 entry = source ? source->Entry : 0;
        bool const cosmetic = slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD;
        if (tournament && !cosmetic)
        {
            uint32 const twin = entry ? Tournament::GetItemForMode(entry, true) : 0;
            entry = twin ? twin : Tournament::GetLoadoutTemplateItem(kit.Class, slot);
        }
        if (!entry)
            continue;

        Item* item = Item::CreateItem(entry, 1, player);
        if (!item)
            continue;

        // The kit's own piece keeps its suffix and every enchant and gem; a
        // substitute takes the permanent enchant and the poison or oil on it.
        bool const same = source && source->Entry == entry;
        if (source)
        {
            if (same && source->RandomPropertyId)
                item->SetItemRandomProperties(source->RandomPropertyId);
            for (uint8 e = 0; e < MAX_ENCHANTMENT_SLOT; ++e)
            {
                if (!same && e != PERM_ENCHANTMENT_SLOT && e != TEMP_ENCHANTMENT_SLOT)
                    continue;
                uint32 const id = source->Enchants[e * MAX_ENCHANTMENT_OFFSET + ENCHANTMENT_ID_OFFSET];
                if (!id || !sSpellItemEnchantmentStore.LookupEntry(id))
                    continue;
                item->SetEnchantment(EnchantmentSlot(e), id,
                    source->Enchants[e * MAX_ENCHANTMENT_OFFSET + ENCHANTMENT_DURATION_OFFSET],
                    source->Enchants[e * MAX_ENCHANTMENT_OFFSET + ENCHANTMENT_CHARGES_OFFSET]);
            }
        }

        player->EquipItem((uint16(INVENTORY_SLOT_BAG_0) << 8) | slot, item, true);
        stash.Issued.push_back(item->GetGUID());
    }

    if (kit.Class == CLASS_HUNTER && kit.Ammo)
        GiveAmmo(player, stash, kit.Ammo);
    GiveConsumables(player, stash, kit);
    GiveConjured(player, stash);
}

void PersonaAccess::SummonPet(Player* player, PersonaStash& stash, Kit const& kit)
{
    if ((kit.Class != CLASS_HUNTER && kit.Class != CLASS_WARLOCK) || !kit.HasPet)
        return;

    // The source's pet, under a number of its own that has no rows anywhere:
    // the source's pet is never loaded, moved or saved. LoadPetFromDB reads
    // what it needs from the stable and answers a moment later.
    PetStable& stable = player->GetOrInitPetStable();
    PetStable::PetInfo info = kit.Pet;
    info.PetNumber = sObjectMgr->GeneratePetNumber();
    info.Health = std::numeric_limits<uint32>::max();
    info.Mana = std::numeric_limits<uint32>::max();
    info.Level = player->GetLevel();
    stable.CurrentPet = info;
    stash.PersonaPetNumber = info.PetNumber;
    stash.PetSpellsTaught = kit.PetSpells.empty();

    Pet* pet = new Pet(player, info.Type);
    if (!pet->LoadPetFromDB(player, 0, info.PetNumber, true))
    {
        delete pet;
        stash.PersonaPetNumber = 0;
        stash.PetSpellsTaught = true;
    }
}

void PersonaAccess::RecastPassives(Player* player)
{
    // What _LoadSpells' AddSpell casts at a login: the passives, by the same
    // test. Collected first - a passive may teach a spell.
    std::vector<uint32> passives;
    for (auto const& [spell, known] : player->m_spells)
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled || !known.active)
            continue;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
            if (info->IsPassive())
                passives.push_back(spell);
    }
    for (uint32 spell : passives)
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
            if (player->HandlePassiveSpellLearn(info))
                player->CastSpell(player, spell, true);
}

void PersonaAccess::Build(Player* player, PersonaStash& stash, Kit const& kit)
{
    // Clean slates for what the kit fills.
    ClearSkillFields(player);
    for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
        player->SetUInt32Value(PLAYER_FIELD_GLYPHS_1 + i, 0);
    std::fill(&player->m_Glyphs[0][0], &player->m_Glyphs[0][0] + MAX_TALENT_SPECS * MAX_GLYPH_SLOT_INDEX, 0u);
    player->m_WeaponProficiency = 0;
    player->m_ArmorProficiency = 0;
    player->SetCanParry(false);
    player->SetCanBlock(false);
    player->SetCanDualWield(false);
    player->SetCanTitanGrip(false);
    player->m_usedTalentCount = 0;
    player->m_activeSpec = 0;
    player->m_specsCount = 1;

    // Who it is (Player::LoadFromDB's order from here on).
    player->SetRace(kit.Race);
    player->SetClass(kit.Class);
    player->SetGender(Gender(kit.Gender));
    player->SetNativeGender(Gender(kit.Gender));
    player->SetSkinId(kit.Skin);
    player->SetFaceId(kit.Face);
    player->SetHairStyleId(kit.HairStyle);
    player->SetHairColorId(kit.HairColor);
    player->SetFacialStyle(kit.FacialStyle);
    player->InitDisplayIds();
    player->InitStatsForLevel(true);

    LoadSkills(player, kit);
    player->UpdateSkillsForLevel();

    // Talents before spells: the low-rank walk in AddSpell asks HasTalent.
    for (uint32 talent : kit.Talents)
        player->AddTalent(talent, 0, false);
    for (KitSpell const& spell : kit.Spells)
        player->AddSpell(spell.Id, spell.Active, false, false, spell.Disabled, true);

    // What a login teaches on top of the saved rows (Player::LoadFromDB): the
    // spells the source's rewarded quests teach - stances, forms, demons,
    // totems (LearnQuestRewardedSpells), ...
    for (uint32 questId : kit.RewardedQuests)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        int32 const rewardSpell = quest ? quest->GetRewSpellCast() : 0;
        SpellInfo const* rewardInfo = rewardSpell > 0 ? sSpellMgr->GetSpellInfo(rewardSpell) : nullptr;
        if (!rewardInfo)
            continue;
        for (SpellEffectInfo const& effect : rewardInfo->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell && !player->HasSpell(effect.TriggerSpell))
                player->AddSpell(effect.TriggerSpell, true, false, false, false, true);
    }
    player->LearnDefaultSkills();

    // ... and Player::LearnCustomSpells' list for its race, class and mode:
    // most of a tournament character's kit - every starter template's - is
    // that list, never saved.
    if (sWorld->getBoolConfig(CONFIG_START_ALL_SPELLS))
    {
        std::vector<uint32> const* custom = nullptr;
        if (kit.TournamentMode && Tournament::IsEnabled())
            if (Tournament::CreateInfo const* info = Tournament::GetCreateInfo(kit.Race, kit.Class))
                if (info->HasCustomSpells)
                    custom = &info->CustomSpells;
        if (!custom)
            if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(kit.Race, kit.Class))
                custom = &info->customSpells;
        if (custom)
            for (uint32 spell : *custom)
                player->AddSpell(spell, true, false, true, false, true);
    }

    for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
        if (kit.Glyphs[i])
            player->SetGlyph(i, kit.Glyphs[i]);
    player->_LoadGlyphAuras();

    player->RecountUsedTalentsFromTalentMap();
    uint32 const points = player->CalculateTalentsPoints();
    player->SetFreeTalentPoints(points > player->m_usedTalentCount ? points - player->m_usedTalentCount : 0);

    // A stand-in's own bars are a bot's - empty, or nothing a player would
    // want. It plays on its class's starter template's: each spell button
    // moved to the stand-in's rank of that spell, or left off when it has
    // none, and then the abilities its talents teach that are on none of them
    // go on what is left of the main bars.
    KitPtr const layout = kit.StandIn ? FindKit(GetTemplateCharacter(kit.Class)) : nullptr;
    Kit const& bars = layout ? *layout : kit;
    for (KitButton const& button : bars.Buttons)
    {
        if (button.Button >= MAX_ACTION_BUTTONS)
            continue;

        uint32 action = button.Action;
        uint8 type = button.Type;
        if (type == ACTION_BUTTON_CMACRO)
        {
            // The spell the macro casts, if the persona knows one of them.
            auto const macro = bars.CharacterMacros.find(button.Action);
            if (macro == bars.CharacterMacros.end())
                continue;
            action = 0;
            for (auto const& [name, rank] : MacroSpellCandidates(macro->second.Body))
                if ((action = FindKnownSpell(player, name, rank)))
                    break;
            if (!action)
                continue;
            type = ACTION_BUTTON_SPELL;
        }
        else if (layout && type == ACTION_BUTTON_SPELL)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(action);
            action = info && info->SpellName[LOCALE_enUS]
                ? FindKnownSpell(player, info->SpellName[LOCALE_enUS], sSpellMgr->GetSpellRank(button.Action)) : 0;
            if (!action)
                continue;
        }

        ActionButton& ab = player->m_actionButtons[button.Button];
        ab.SetActionAndType(action, ActionButtonType(type));
        ab.uState = ACTIONBUTTON_UNCHANGED;
    }

    if (layout)
    {
        std::unordered_set<uint32> onBars;
        for (auto const& [slot, ab] : player->m_actionButtons)
            if (ab.uState != ACTIONBUTTON_DELETED && ab.GetType() == ACTION_BUTTON_SPELL)
                onBars.insert(ab.GetAction());

        // The main bar first, then the bottom left and right bars.
        std::vector<uint8> free;
        for (uint8 first : { 0, 60, 48 })
            for (uint8 slot = first; slot < first + 12; ++slot)
                if (!player->m_actionButtons.count(slot))
                    free.push_back(slot);

        size_t next = 0;
        for (uint32 talentSpell : kit.Talents)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(talentSpell);
            if (!info)
                continue;
            uint32 ability = talentSpell;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
                    ability = effect.TriggerSpell;
            SpellInfo const* abilityInfo = sSpellMgr->GetSpellInfo(ability);
            if (!abilityInfo || abilityInfo->IsPassive() || !player->HasSpell(ability) || onBars.count(ability) || next >= free.size())
                continue;
            ActionButton& ab = player->m_actionButtons[free[next++]];
            ab.SetActionAndType(ability, ACTION_BUTTON_SPELL);
            ab.uState = ACTIONBUTTON_UNCHANGED;
            onBars.insert(ability);
        }
    }

    EquipKit(player, stash, kit);

    player->UpdateAllStats();
    player->SetFullHealth();
    player->ResetAllPowers();

    SummonPet(player, stash, kit);
}

bool PersonaAccess::Apply(Player* player, KitPtr const& kit)
{
    if (!player || !kit || !player->IsInWorld() || !player->IsAlive())
        return false;
    if (!IsPlayableClass(kit->Class) || !sObjectMgr->GetPlayerInfo(kit->Race, kit->Class))
        return false;

    std::unordered_set<uint32> const before = VisibleSpells(player);
    bool const first = !player->m_allStarPersona;
    // CENTURION_AllStar hides the "You have (un)learned" and skill lines the
    // swap prints - told first, because building the kit already sends some.
    SendAddonLine(player, "QUIET");
    if (first)
    {
        // The last look the database gets at the real character until it is
        // restored, and its real pet sent home and saved as it always is -
        // both before the persona exists, while saving still means saving.
        player->SaveToDB();
        player->UnsummonPetTemporaryIfAny();
        player->m_allStarPersona.reset(new PersonaStash());
    }
    else
        RemovePersonaPet(player);

    PersonaStash& stash = *player->m_allStarPersona;
    SwapScope const swap(player);
    Strip(player);
    if (first)
        TakeAside(player, stash);
    else
        DropPersona(player, stash);

    stash.Persona = kit;
    if (!kit->Template)
    {
        PersonaName name;
        name.Name = Surnames::Decorated(ObjectGuid::Create<HighGuid::Player>(kit->Source), kit->Name);
        name.Race = kit->Race;
        name.Gender = kit->Gender;
        name.Class = kit->Class;
        std::lock_guard<std::mutex> lock(NameMutex);
        Names[player->GetGUID()] = std::move(name);
    }
    else
    {
        // A default loadout is the player in a starter's clothes: their own name.
        PersonaName name;
        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(player->GetGUID());
        name.Name = Surnames::Decorated(player->GetGUID(), cache ? cache->Name : player->GetName());
        name.Race = kit->Race;
        name.Gender = kit->Gender;
        name.Class = kit->Class;
        std::lock_guard<std::mutex> lock(NameMutex);
        Names[player->GetGUID()] = std::move(name);
    }

    Build(player, stash, *kit);
    NoteSwap(player);
    SyncClient(player, before, stash.ClientSpells);
    SendBindings(player, kit.get());
    SendMacros(player, kit.get());
    EnterDefaultStance(player);

    TC_LOG_DEBUG("bg.battleground", "All-Star: {} ({}) now wears {} ({}, class {}{}): {} spells, {} talents, {} pieces.",
        player->GetName(), player->GetGUID().GetCounter(), kit->Name, kit->Source, uint32(kit->Class),
        kit->Template ? ", default loadout" : "", kit->Spells.size(), kit->Talents.size(), stash.Issued.size());
    return true;
}

void PersonaAccess::Restore(Player* player)
{
    if (!player || !player->m_allStarPersona)
        return;

    std::unordered_set<uint32> const before = VisibleSpells(player);
    PersonaStash& stash = *player->m_allStarPersona;
    std::unordered_set<uint32> shown = std::move(stash.ClientSpells);
    if (player->IsInWorld())
        SendAddonLine(player, "QUIET");
    {
        SwapScope const swap(player);
        RemovePersonaPet(player);
        Strip(player);
        DropPersona(player, stash);

        player->SetRace(stash.Race);
        player->SetClass(stash.Class);
        player->SetGender(Gender(stash.Gender));
        player->SetNativeGender(Gender(stash.NativeGender));
        player->SetSkinId(stash.Skin);
        player->SetFaceId(stash.Face);
        player->SetHairStyleId(stash.HairStyle);
        player->SetHairColorId(stash.HairColor);
        player->SetFacialStyle(stash.FacialStyle);
        player->InitDisplayIds();

        player->m_spells.swap(stash.Spells);
        for (uint8 spec = 0; spec < MAX_TALENT_SPECS; ++spec)
        {
            delete player->m_talents[spec];     // emptied by DropPersona
            player->m_talents[spec] = stash.Talents[spec];
            stash.Talents[spec] = nullptr;
        }
        player->m_activeSpec = stash.ActiveSpec;
        player->m_specsCount = stash.SpecsCount;
        std::copy(&stash.Glyphs[0][0], &stash.Glyphs[0][0] + MAX_TALENT_SPECS * MAX_GLYPH_SLOT_INDEX, &player->m_Glyphs[0][0]);
        for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
            player->SetUInt32Value(PLAYER_FIELD_GLYPHS_1 + i, stash.GlyphFields[i]);
        player->m_actionButtons.swap(stash.Buttons);
        player->mSkillStatus.swap(stash.Skills);
        for (uint32 i = 0; i < PLAYER_MAX_SKILLS * 3; ++i)
            player->SetUInt32Value(PLAYER_SKILL_INFO_1_1 + i, stash.SkillFields[i]);
        player->m_WeaponProficiency = stash.WeaponProficiency;
        player->m_ArmorProficiency = stash.ArmorProficiency;
        player->SetCanParry(stash.CanParry);
        player->SetCanBlock(stash.CanBlock);
        player->SetCanDualWield(stash.CanDualWield);
        player->SetCanTitanGrip(stash.CanTitanGrip);
        player->m_usedTalentCount = stash.UsedTalentCount;
        player->SetFreeTalentPoints(stash.FreeTalentPoints);

        player->m_petStable = std::move(stash.Stable);
        player->m_temporaryUnsummonedPetNumber = stash.TemporaryUnsummonedPetNumber;
        player->m_lastpetnumber = stash.LastPetNumber;
        player->m_oldpetspell = stash.OldPetSpell;

        player->InitStatsForLevel(true);
        // InitStatsForLevel fills health whatever the death state: a character
        // restored dead (a logout mid-match, before the leave revives it) stays
        // a corpse with none.
        if (!player->IsAlive())
            player->SetHealth(0);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item* item = stash.Equipment[slot])
            {
                stash.Equipment[slot] = nullptr;
                PutBack(player, slot, item);
            }
        }
        if (stash.Ammo)
            player->SetAmmo(stash.Ammo);

        RecastPassives(player);
        player->_LoadGlyphAuras();
    }

    // From here the character saves as itself again.
    player->m_allStarPersona.reset();
    {
        std::lock_guard<std::mutex> lock(NameMutex);
        Names.erase(player->GetGUID());
    }

    player->UpdateAllStats();
    if (player->IsAlive())
    {
        player->SetFullHealth();
        player->ResetAllPowers();
    }
    NoteSwap(player);
    if (player->IsInWorld())
    {
        SyncClient(player, before, shown);
        SendBindings(player, nullptr);
        SendMacros(player, nullptr);
        EnterDefaultStance(player);
    }
}

void PersonaAccess::Maintain(Player* player)
{
    PersonaStash* stash = player ? player->m_allStarPersona.get() : nullptr;
    if (!stash || stash->PetSpellsTaught || !stash->Persona)
        return;

    Pet* pet = player->GetPet();
    if (!pet || !pet->IsInWorld() || !pet->GetCharmInfo() || pet->GetCharmInfo()->GetPetNumber() != stash->PersonaPetNumber)
        return;

    for (auto const& [spell, active] : stash->Persona->PetSpells)
        if (sSpellMgr->GetSpellInfo(spell) && !pet->HasSpell(spell))
            pet->addSpell(spell, ActiveStates(active), PETSPELL_UNCHANGED);
    stash->PetSpellsTaught = true;
    player->PetSpellInitialize();
}

// ==================================================================== public

void LoadConfig()
{
    // CENTURION_AllStar's requests (ALLSTAR:MACROS:RESTORED). Once: a
    // .reload config runs this again.
    static bool routed = false;
    if (!routed)
    {
        AddonRequestRoutes::Register("ALLSTAR", &HandleAddonRequest);
        routed = true;
    }

    Settings loaded;
    loaded.Enabled = sConfigMgr->GetBoolDefault("Centurion.AllStar.Enable", true);
    loaded.RespawnDelayMs = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.AllStar.RespawnDelay", 2 * IN_MILLISECONDS)));
    loaded.TimeLimitMs = uint32(std::max(1, sConfigMgr->GetIntDefault("Centurion.AllStar.TimeLimit", 30))) * MINUTE * IN_MILLISECONDS;
    loaded.SourceMinLevel = uint8(std::clamp(sConfigMgr->GetIntDefault("Centurion.AllStar.SourceMinLevel", 60), 1, 255));
    loaded.ShuffleLives = sConfigMgr->GetBoolDefault("Centurion.AllStar.ShuffleLives", true);

    std::string const templates = sConfigMgr->GetStringDefault("Centurion.AllStar.TemplateCharacters",
        "1:13307 2:13308 3:13314 4:13309 5:13310 7:13311 8:13315 9:13312 11:13313");
    for (std::string_view token : Trinity::Tokenize(templates, ' ', false))
    {
        std::vector<std::string_view> const pair = Trinity::Tokenize(token, ':', false);
        if (pair.size() != 2)
            continue;
        Optional<uint32> const playerClass = Trinity::StringTo<uint32>(pair[0]);
        Optional<uint32> const guid = Trinity::StringTo<uint32>(pair[1]);
        if (playerClass && guid && *playerClass < loaded.Templates.size())
            loaded.Templates[*playerClass] = *guid;
    }

    Config = loaded;
    TemplatesAsked = false;     // a changed list is fetched again at the next Update
}

bool IsEnabled() { return Config.Enabled; }
uint32 GetRespawnDelayMs() { return Config.RespawnDelayMs; }
uint32 GetTimeLimitMs() { return Config.TimeLimitMs; }

ObjectGuid::LowType GetTemplateCharacter(uint8 playerClass)
{
    return playerClass < Config.Templates.size() ? Config.Templates[playerClass] : 0;
}

bool IsPlayableClass(uint8 playerClass)
{
    return std::find(CLASS_ORDER.begin(), CLASS_ORDER.end(), playerClass) != CLASS_ORDER.end();
}

char const* ClassName(uint8 playerClass)
{
    switch (playerClass)
    {
        case CLASS_WARRIOR: return "Warrior";
        case CLASS_PALADIN: return "Paladin";
        case CLASS_HUNTER:  return "Hunter";
        case CLASS_ROGUE:   return "Rogue";
        case CLASS_PRIEST:  return "Priest";
        case CLASS_SHAMAN:  return "Shaman";
        case CLASS_MAGE:    return "Mage";
        case CLASS_WARLOCK: return "Warlock";
        case CLASS_DRUID:   return "Druid";
        default:            return "Hero";
    }
}

void RequestKit(ObjectGuid::LowType source)
{
    if (!source)
        return;
    {
        // Asked once: a kit that could not be built stays failed (the match
        // falls back to the template) until ForgetKit.
        std::lock_guard<std::mutex> lock(KitMutex);
        if (Kits.count(source))
            return;
        Kits[source] = KitEntry();
    }
    std::lock_guard<std::mutex> lock(PendingMutex);
    PendingKits.push_back(source);
}

KitPtr FindKit(ObjectGuid::LowType source)
{
    std::lock_guard<std::mutex> lock(KitMutex);
    auto const itr = Kits.find(source);
    return itr != Kits.end() && itr->second.State == KitState::Ready ? itr->second.Value : nullptr;
}

bool KitFailed(ObjectGuid::LowType source)
{
    std::lock_guard<std::mutex> lock(KitMutex);
    auto const itr = Kits.find(source);
    return itr != Kits.end() && itr->second.State == KitState::Failed;
}

void ForgetKit(ObjectGuid::LowType source)
{
    if (IsTemplateGuid(source))
        return;
    std::lock_guard<std::mutex> lock(KitMutex);
    Kits.erase(source);
}

uint8 GetKitClass(Kit const& kit) { return kit.Class; }
std::string const& GetKitName(Kit const& kit) { return kit.Name; }
bool IsTemplateKit(Kit const& kit) { return kit.Template; }

void RequestLineup(uint32 accountId, ObjectGuid::LowType playing, uint8 playingClass)
{
    LineupKey const key{ accountId, playing };
    {
        std::lock_guard<std::mutex> lock(LineupMutex);
        if (Lineups.count(key))
            return;
        Lineups[key] = LineupEntryState();
    }

    std::lock_guard<std::mutex> lock(PendingMutex);
    PendingLineups.push_back({ key, playingClass });
}

namespace
{
    // World thread (Update).
    void QueryLineup(LineupKey const key, uint8 playingClass)
    {
    std::string const sql = Trinity::StringFormat(
        "SELECT guid, class, level FROM characters WHERE account = {} AND guid <> {} AND deleteDate IS NULL",
        key.Account, key.Playing);
    QueryCallbacks.AddCallback(CharacterDatabase.AsyncQuery(sql.c_str()).WithCallback(
        [key, playingClass](QueryResult result)
    {
        std::vector<ListedCharacter> characters;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                ObjectGuid::LowType const guid = fields[0].GetUInt32();
                characters.push_back({ guid, fields[1].GetUInt8(), fields[2].GetUInt8(), CharacterScreen::GetListPosition(guid) });
            } while (result->NextRow());
        }

        Lineup lineup = BuildLineup(playingClass, std::move(characters), false);
        for (LineupEntry const& entry : lineup)
            if (entry.Source)
                RequestKit(entry.Source);

        std::lock_guard<std::mutex> lock(LineupMutex);
        LineupEntryState& state = Lineups[key];
        state.Value = std::move(lineup);
        state.Ready = true;
    }));
    }
}

bool FindLineup(uint32 accountId, ObjectGuid::LowType playing, Lineup& out)
{
    std::lock_guard<std::mutex> lock(LineupMutex);
    auto const itr = Lineups.find(LineupKey{ accountId, playing });
    if (itr == Lineups.end() || !itr->second.Ready)
        return false;
    out = itr->second.Value;
    return true;
}

void ForgetLineup(uint32 accountId, ObjectGuid::LowType playing)
{
    Lineup lineup;
    {
        std::lock_guard<std::mutex> lock(LineupMutex);
        auto const itr = Lineups.find(LineupKey{ accountId, playing });
        if (itr == Lineups.end())
            return;
        lineup = std::move(itr->second.Value);
        Lineups.erase(itr);
    }
    for (LineupEntry const& entry : lineup)
        if (entry.Source && !entry.Template)
            ForgetKit(entry.Source);
}

Lineup BuildBotLineup(uint8 playingClass)
{
    Lineup lineup = BuildLineup(playingClass, {}, true);
    for (LineupEntry const& entry : lineup)
        if (entry.Source)
            RequestKit(entry.Source);
    return lineup;
}

void Update()
{
    if (Config.Enabled && !TemplatesAsked)
    {
        TemplatesAsked = true;
        for (uint8 playerClass : CLASS_ORDER)
            RequestKit(GetTemplateCharacter(playerClass));
    }

    // The playerbot accounts' level-60 characters, by class (StandIns).
    if (Config.Enabled && !StandInsAsked)
    {
        StandInsAsked = true;
        QueryCallbacks.AddCallback(LoginDatabase.AsyncQuery("SELECT id FROM account WHERE username LIKE 'PLAYERBOT%'").WithCallback(
            [](QueryResult accounts)
        {
            if (!accounts)
                return;
            std::string ids;
            do
            {
                if (!ids.empty())
                    ids += ',';
                ids += std::to_string(accounts->Fetch()[0].GetUInt32());
            } while (accounts->NextRow());

            std::string const sql = Trinity::StringFormat(
                "SELECT guid, class FROM characters WHERE account IN ({}) AND level >= {} AND deleteDate IS NULL",
                ids, uint32(Config.SourceMinLevel));
            QueryCallbacks.AddCallback(CharacterDatabase.AsyncQuery(sql.c_str()).WithCallback([](QueryResult characters)
            {
                std::array<std::vector<ObjectGuid::LowType>, MAX_CLASSES> found;
                std::unordered_set<ObjectGuid::LowType> guids;
                if (characters)
                {
                    do
                    {
                        Field* row = characters->Fetch();
                        uint8 const playerClass = row[1].GetUInt8();
                        if (!IsPlayableClass(playerClass) || playerClass >= MAX_CLASSES)
                            continue;
                        ObjectGuid::LowType const guid = row[0].GetUInt32();
                        found[playerClass].push_back(guid);
                        guids.insert(guid);
                    } while (characters->NextRow());
                }
                TC_LOG_INFO("bg.battleground", "All-Star: {} playerbot characters can stand in for a class a lineup has nobody for.",
                    uint32(guids.size()));
                std::lock_guard<std::mutex> lock(StandInMutex);
                StandIns = std::move(found);
                StandInGuids = std::move(guids);
            }));
        }));
    }

    // What other threads asked for since the last tick. The processors are
    // only ever touched here, so a callback may queue the next stage itself
    // (a hunter's pet spells, a lineup's kits) without any lock.
    std::vector<ObjectGuid::LowType> kits;
    std::vector<PendingLineup> lineups;
    {
        std::lock_guard<std::mutex> lock(PendingMutex);
        kits.swap(PendingKits);
        lineups.swap(PendingLineups);
    }
    for (ObjectGuid::LowType source : kits)
        LoadKit(source);
    for (PendingLineup const& lineup : lineups)
        QueryLineup(lineup.Key, lineup.PlayingClass);

    HolderCallbacks.ProcessReadyCallbacks();
    QueryCallbacks.ProcessReadyCallbacks();
}

bool ApplyPersona(Player* player, KitPtr const& kit)
{
    return PersonaAccess::Apply(player, kit);
}

void RestorePersona(Player* player)
{
    // The match is over for them, persona or not (a leave, a logout).
    if (player)
        player->RemoveAurasDueToSpell(LIVES_AURA);
    PersonaAccess::Restore(player);
}

uint8 GetRealClass(Player const* player)
{
    if (!player)
        return 0;
    PersonaStash const* stash = PersonaAccess::Stash(player);
    return stash ? stash->Class : player->GetClass();
}

std::string GetPersonaName(Player const* player)
{
    PersonaStash const* stash = player ? PersonaAccess::Stash(player) : nullptr;
    if (!stash || !stash->Persona || stash->Persona->Template)
        return std::string();
    return stash->Persona->Name;
}

bool IsPersonaFromTemplate(Player const* player)
{
    PersonaStash const* stash = player ? PersonaAccess::Stash(player) : nullptr;
    return stash && stash->Persona && stash->Persona->Template;
}

ObjectGuid NamePastLife(std::string const& name, uint8 race, uint8 gender, uint8 playerClass)
{
    // Far above any character guid this realm will reach.
    static std::atomic<uint32> next{ 0 };
    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(PAST_LIFE_GUID_BASE + (next++ & 0x0FFFFFFF));
    PersonaName entry;
    entry.Name = name;
    entry.Race = race;
    entry.Gender = gender;
    entry.Class = playerClass;
    std::lock_guard<std::mutex> lock(NameMutex);
    Names[guid] = std::move(entry);
    return guid;
}

void ForgetPastLife(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(NameMutex);
    Names.erase(guid);
}

bool GetPersonaNameData(ObjectGuid guid, std::string& name, uint8& race, uint8& gender, uint8& playerClass)
{
    std::lock_guard<std::mutex> lock(NameMutex);
    auto const itr = Names.find(guid);
    if (itr == Names.end())
        return false;
    name = itr->second.Name;
    race = itr->second.Race;
    gender = itr->second.Gender;
    playerClass = itr->second.Class;
    return true;
}

bool IsSwapInProgress(Player const* player)
{
    return player && Swapping == player;
}

bool KeepsAccountData(WorldSession* session, uint32 type, uint32 clientTime)
{
    if (type != PER_CHARACTER_MACROS_CACHE || !session || !session->GetPlayer())
        return false;
    {
        std::lock_guard<std::mutex> lock(BorrowedMutex);
        if (!MacrosBorrowed.count(session->GetPlayer()->GetGUID().GetCounter()))
            return false;
    }
    KeepRealMacros(session, time_t(clientTime));
    TC_LOG_INFO("bg.battleground", "All-Star: {} ({}) uploaded its macros while holding borrowed ones - its own are kept.",
        session->GetPlayer()->GetName(), session->GetPlayer()->GetGUID().GetCounter());
    return true;
}

void SendLives(Player* player, uint32 left, uint32 total)
{
    if (!player)
        return;
    // The life being played: the borrowed character's name, or the player's
    // own (life one, a starter template), and its class for the colour.
    std::string name = GetPersonaName(player);
    if (name.empty())
        name = player->GetName();
    SendAddonLine(player, Trinity::StringFormat("LIVES:{}:{}:{}:{}", left, total, uint32(player->GetClass()), name));
}

bool IgnoresClientButtons(Player const* player)
{
    if (!player)
        return false;
    std::lock_guard<std::mutex> lock(SwapTimeMutex);
    auto const itr = SwapTimes.find(player->GetGUID());
    if (itr == SwapTimes.end())
        return false;
    if (GameTime::GetGameTimeMS() - itr->second > KEEP_BUTTONS_MS)
    {
        SwapTimes.erase(itr);
        return false;
    }
    return true;
}

void MaintainPersona(Player* player)
{
    PersonaAccess::Maintain(player);
}
}
