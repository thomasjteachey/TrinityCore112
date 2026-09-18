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

// The tournament battleground loadout: what a character fights in once it steps
// into a tournament-pool battleground or arena, and how it gets its own gear
// back afterwards - whichever way it leaves, including the ways it did not choose.
//
// Going in, every equippable item the character has - worn or carried - is
// answered in this order:
//
//   1. it has a tournament twin (`item_tournament_link`)  -> the twin,
//   2. it is worn and has no twin                         -> the same slot from
//      its class's starter template (`tournament_loadout_template`: the gear
//      Startrogue, Startwarrior and the rest are wearing),
//   3. what that offered cannot be equipped - unique, no proficiency, the wrong
//      armour - -> a field kit piece for the slot, and nothing at all when the
//      kit has none for it (neck, rings and trinkets have none),
//   4. it is carried and has no twin                      -> it is put away for
//      the match, so nothing untwinned can be swapped in mid-fight.
//
// The originals are neither destroyed nor rebuilt from a description: the item
// rows are moved aside exactly as an account bank deposit moves them
// (Accounts/AccountBankMgr.cpp) and handed back with their enchants, charges,
// durability and guids intact. Every move is written to
// `character_tournament_loadout` in the same transaction that makes it, so a
// logout, a disconnect, a crash or a restart mid-match all end the same way: the
// next login gives the character its own gear back.
//
// Two more match rules live here because they start and end at the same moments:
// a world-mode character is taught the eat/drink/bandage spells a tournament
// character knows innately (and untaught on the way out, unless it knew them
// already), and no consumable works inside except the tournament PvP ones.
//
// None of it runs unless Centurion.Tournament.Enable is on AND the match came
// from the tournament queue pool (Battleground::IsTournamentPool).

#include "Miscellaneous/TournamentMode.h"
#include "Bag.h"
#include "Battleground.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Tournament
{
namespace
{
    // Row kinds in `character_tournament_loadout`.
    enum LoadoutRowKind : uint8
    {
        LOADOUT_ROW_STASHED    = 0,   // an original that was taken off or out
        LOADOUT_ROW_SUBSTITUTE = 1,   // an item this system issued in its place
        LOADOUT_ROW_SPELL      = 2    // a spell this system taught for the match
    };

    struct LoadoutSettings
    {
        bool Enabled = true;
        bool BanConsumables = true;
        std::unordered_set<uint32> AllowedConsumables;
    };

    LoadoutSettings LoadoutConfig;

    // class << 8 | equipment slot -> item entry. Read-only once loaded.
    std::unordered_map<uint16, uint32> TemplateGear;
    // The field kit's pieces by inventory type, highest required level first.
    // Built from the item templates rather than from a table of its own: the
    // kit's entry block is a constant (ItemTemplate.h), so the pool follows the
    // item data instead of a list somebody has to maintain in two places.
    std::unordered_map<uint32 /*InventoryType*/, std::vector<uint32>> KitPieces;
    // Whether this realm has the stash table at all. Probed at load: a realm
    // without it never queries it, and every path here does nothing.
    bool StashTablePresent = false;

    // Characters wearing a loadout right now. The database is the record that
    // survives a restart; this is what the hot paths ask.
    std::mutex ActiveLoadoutMutex;
    std::unordered_set<ObjectGuid> ActiveLoadouts;

    // Set while this thread is dressing or undressing a character, so the
    // battleground armour lock (Player.cpp IsBattlegroundEquipChangeAllowed)
    // stands aside for it - inside a battleground only weapons and trinkets may
    // change hands, and this is the one pass that has to change everything. A
    // swap runs start to finish on the map thread that owns the player, so one
    // pointer per thread says all of it.
    thread_local Player const* SwapInProgress = nullptr;

    struct SwapGuard
    {
        explicit SwapGuard(Player const* player) : Previous(SwapInProgress) { SwapInProgress = player; }
        ~SwapGuard() { SwapInProgress = Previous; }

        SwapGuard(SwapGuard const&) = delete;
        SwapGuard& operator=(SwapGuard const&) = delete;

        Player const* Previous;
    };

    uint16 TemplateKey(uint8 playerClass, uint8 slot)
    {
        return uint16((uint16(playerClass) << 8) | slot);
    }

    uint16 PackPosition(uint8 bag, uint8 slot)
    {
        return uint16((uint16(bag) << 8) | slot);
    }

    void MarkActive(ObjectGuid guid, bool active)
    {
        std::lock_guard<std::mutex> lock(ActiveLoadoutMutex);
        if (active)
            ActiveLoadouts.insert(guid);
        else
            ActiveLoadouts.erase(guid);
    }

    // Shirts and tabards carry no fight in them - a guild's colours and a red
    // shirt - so the loadout leaves them on. Bags, quivers and ammo are not gear
    // either, and taking a bag would take everything inside it along.
    bool IsLoadoutRelevant(ItemTemplate const* proto)
    {
        if (!proto)
            return false;

        switch (proto->InventoryType)
        {
            case INVTYPE_NON_EQUIP:
            case INVTYPE_BODY:
            case INVTYPE_TABARD:
            case INVTYPE_BAG:
            case INVTYPE_AMMO:
            case INVTYPE_QUIVER:
                return false;
            default:
                return true;
        }
    }

    // What the field kit may offer for an equipment slot, in the order it is
    // tried. CanEquipNewItem has the last word on every candidate, so nothing
    // here needs to know about armour proficiency, class or race.
    std::vector<uint32> InventoryTypesForSlot(uint8 slot)
    {
        switch (slot)
        {
            case EQUIPMENT_SLOT_HEAD:      return { INVTYPE_HEAD };
            case EQUIPMENT_SLOT_SHOULDERS: return { INVTYPE_SHOULDERS };
            case EQUIPMENT_SLOT_CHEST:     return { INVTYPE_CHEST, INVTYPE_ROBE };
            case EQUIPMENT_SLOT_WAIST:     return { INVTYPE_WAIST };
            case EQUIPMENT_SLOT_LEGS:      return { INVTYPE_LEGS };
            case EQUIPMENT_SLOT_FEET:      return { INVTYPE_FEET };
            case EQUIPMENT_SLOT_WRISTS:    return { INVTYPE_WRISTS };
            case EQUIPMENT_SLOT_HANDS:     return { INVTYPE_HANDS };
            case EQUIPMENT_SLOT_BACK:      return { INVTYPE_CLOAK };
            case EQUIPMENT_SLOT_MAINHAND:  return { INVTYPE_WEAPON, INVTYPE_WEAPONMAINHAND, INVTYPE_2HWEAPON };
            case EQUIPMENT_SLOT_OFFHAND:   return { INVTYPE_WEAPON, INVTYPE_WEAPONOFFHAND, INVTYPE_SHIELD, INVTYPE_HOLDABLE };
            case EQUIPMENT_SLOT_RANGED:    return { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT, INVTYPE_THROWN, INVTYPE_RELIC };
            default:                       return { };   // neck, rings, trinkets: the kit has none
        }
    }

    // The first kit piece for this slot the character can actually put on, or 0.
    uint32 PickKitPiece(Player* player, uint8 slot)
    {
        for (uint32 invType : InventoryTypesForSlot(slot))
        {
            auto itr = KitPieces.find(invType);
            if (itr == KitPieces.end())
                continue;

            for (uint32 entry : itr->second)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                if (!proto || proto->RequiredLevel > player->GetLevel())
                    continue;

                uint16 dest = 0;
                if (player->CanEquipNewItem(slot, dest, entry, false) == EQUIP_ERR_OK)
                    return entry;
            }
        }

        return 0;
    }

    // An enchant, gem or bonus read off an original before it is put away, to
    // be put on whatever takes its place. TEMP_ENCHANTMENT_SLOT is left out:
    // poisons, oils and sharpening stones are applied from a consumable, run on
    // a clock, and a battleground strips them on the way in anyway.
    constexpr std::array<EnchantmentSlot, 6> CarriedEnchantSlots = { {
        PERM_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT_2,
        SOCK_ENCHANTMENT_SLOT_3, BONUS_ENCHANTMENT_SLOT, PRISMATIC_ENCHANTMENT_SLOT
    } };

    struct CarriedEnchant
    {
        uint32 Id = 0;
        uint32 Duration = 0;
        uint32 Charges = 0;
    };

    // One item the loadout will touch, and what it will put in its place.
    // Everything read off the original is read HERE, while it is still in the
    // character's hands: by the time a substitute is issued the original has
    // been put away and its Item object is gone.
    struct LoadoutEntry
    {
        Item* Original = nullptr;   // valid only until the item is stashed
        uint8 Bag = 0;
        uint8 Slot = 0;
        uint32 Count = 1;
        uint32 Substitute = 0;      // 0 = the slot or the bag space is left empty
        bool Worn = false;
        std::array<CarriedEnchant, CarriedEnchantSlots.size()> Enchants = { };
    };

    // The enchants, gems and bonuses the original was carrying go onto the item
    // that stands in for it - the character keeps what it paid for. A gem only
    // goes on when the substitute has that socket to put it in; a kit piece with
    // no sockets does not quietly gain three gems' worth of stats.
    void CarryEnchants(Player* player, LoadoutEntry const& entry, Item* issued)
    {
        ItemTemplate const* proto = issued->GetTemplate();
        if (!proto)
            return;

        for (size_t index = 0; index < CarriedEnchantSlots.size(); ++index)
        {
            CarriedEnchant const& carried = entry.Enchants[index];
            if (!carried.Id)
                continue;

            EnchantmentSlot const slot = CarriedEnchantSlots[index];
            if (slot >= SOCK_ENCHANTMENT_SLOT && slot <= SOCK_ENCHANTMENT_SLOT_3 &&
                !proto->Socket[slot - SOCK_ENCHANTMENT_SLOT].Color)
                continue;

            issued->SetEnchantment(slot, carried.Id, carried.Duration, carried.Charges);
            if (issued->IsEquipped())
                player->ApplyEnchantment(issued, slot, true);
        }
    }

    void CollectFrom(Player* player, uint8 bag, uint8 slot, bool worn, std::vector<LoadoutEntry>& out)
    {
        Item* item = player->GetItemByPos(bag, slot);
        if (!item)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!IsLoadoutRelevant(proto))
            return;

        uint32 const twin = GetItemForMode(proto->ItemId, true);
        if (twin == proto->ItemId)
            return;                      // already a tournament item: left alone

        LoadoutEntry entry;
        entry.Original = item;
        entry.Bag = bag;
        entry.Slot = slot;
        entry.Count = item->GetCount();
        entry.Worn = worn;

        for (size_t index = 0; index < CarriedEnchantSlots.size(); ++index)
        {
            EnchantmentSlot const enchantSlot = CarriedEnchantSlots[index];
            entry.Enchants[index].Id = item->GetEnchantmentId(enchantSlot);
            entry.Enchants[index].Duration = item->GetEnchantmentDuration(enchantSlot);
            entry.Enchants[index].Charges = item->GetEnchantmentCharges(enchantSlot);
        }

        if (twin)
            entry.Substitute = twin;                                              // rule 1
        else if (worn)
            entry.Substitute = GetLoadoutTemplateItem(player->GetClass(), slot);  // rule 2
        // rule 4: carried and untwinned - put away, nothing in its place

        out.push_back(entry);
    }

    std::vector<LoadoutEntry> BuildLoadout(Player* player)
    {
        std::vector<LoadoutEntry> entries;

        // Worn first and in slot order, which puts the main hand before the off
        // hand: a two-hander has to settle before anything is offered the slot
        // it swallows.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            CollectFrom(player, INVENTORY_SLOT_BAG_0, slot, true, entries);

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            CollectFrom(player, INVENTORY_SLOT_BAG_0, slot, false, entries);

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag const* container = player->GetBagByPos(bag);
            if (!container)
                continue;

            for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                CollectFrom(player, bag, uint8(slot), false, entries);
        }

        return entries;
    }

    // Dynamic statements rather than prepared ones, deliberately: a realm
    // without these tables must still start, and a prepared statement for a
    // table that is not there is fatal at startup.
    void InsertRow(CharacterDatabaseTransaction& trans, ObjectGuid::LowType guid, LoadoutRowKind kind, uint32 id, uint16 position)
    {
        trans->Append(Trinity::StringFormat(
            "INSERT INTO character_tournament_loadout (guid, kind, id, position) VALUES ({}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE position = VALUES(position)",
            guid, uint32(kind), id, uint32(position)).c_str());
    }

    // The character keeps the item; the item simply stops being in its bags for
    // the length of the match. The same moves as an account bank deposit: out of
    // the inventory, out of `character_inventory`, and its own row written back,
    // so it stands on its own until it is asked for again.
    void StashOriginal(Player* player, LoadoutEntry const& entry, CharacterDatabaseTransaction& trans)
    {
        Item* item = entry.Original;
        ObjectGuid::LowType const itemGuid = item->GetGUID().GetCounter();

        player->MoveItemFromInventory(entry.Bag, entry.Slot, true);
        item->DeleteFromInventoryDB(trans);
        item->SetGuidValue(ITEM_FIELD_CONTAINED, ObjectGuid::Empty);
        item->FSetState(ITEM_NEW);
        item->SaveToDB(trans);

        InsertRow(trans, player->GetGUID().GetCounter(), LOADOUT_ROW_STASHED, itemGuid, PackPosition(entry.Bag, entry.Slot));

        delete item;
    }

    void IssueSubstitute(Player* player, LoadoutEntry const& entry, CharacterDatabaseTransaction& trans, bool record)
    {
        uint32 wanted = entry.Substitute;
        Item* issued = nullptr;

        if (entry.Worn)
        {
            uint16 dest = 0;
            if (wanted && player->CanEquipNewItem(entry.Slot, dest, wanted, false) != EQUIP_ERR_OK)
                wanted = 0;

            // Rule 3: unique, no proficiency, the wrong armour - whatever the
            // reason, the slot falls back to the kit, and to nothing after that.
            if (!wanted)
            {
                wanted = PickKitPiece(player, entry.Slot);
                if (wanted && player->CanEquipNewItem(entry.Slot, dest, wanted, false) != EQUIP_ERR_OK)
                    wanted = 0;
            }

            if (wanted)
                issued = player->EquipNewItem(dest, wanted, true);
        }
        else if (wanted)
        {
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(entry.Bag, entry.Slot, dest, wanted, entry.Count) != EQUIP_ERR_OK &&
                player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, wanted, entry.Count) != EQUIP_ERR_OK)
                return;

            issued = player->StoreNewItem(dest, wanted, true);
        }

        if (!issued)
            return;

        CarryEnchants(player, entry, issued);

        if (!record)
            return;

        InsertRow(trans, player->GetGUID().GetCounter(), LOADOUT_ROW_SUBSTITUTE, issued->GetGUID().GetCounter(),
            PackPosition(issued->GetBagSlot(), issued->GetSlot()));
    }

    // A world-mode character borrows the eat/drink/bandage spells a tournament
    // character has always known, for the length of the match. One it already
    // knows is left alone and not written down, so leaving can never take away
    // something it learned for itself.
    void GrantMatchSpells(Player* player, CharacterDatabaseTransaction& trans, bool record)
    {
        if (IsTournamentCharacter(player))
            return;

        uint32 const classMask = player->GetClassMask();
        for (auto const& innate : GetInnateSpells())
        {
            uint32 const spellId = innate.first;
            uint32 const spellClassMask = innate.second;

            if (spellClassMask && !(spellClassMask & classMask))
                continue;

            if (player->HasSpell(spellId) || !sSpellMgr->GetSpellInfo(spellId))
                continue;

            player->LearnSpell(spellId, false);
            if (record)
                InsertRow(trans, player->GetGUID().GetCounter(), LOADOUT_ROW_SPELL, spellId, 0);
        }
    }

    // Puts one stashed item back: where it came from when that is free, anywhere
    // in the bags when it is not, and in the post when the bags are full - the
    // same last resort Player::_LoadInventory uses for gear it cannot place.
    //
    // `fields` is one row of the item_instance columns Item::LoadFromDB reads,
    // read for the whole loadout in one query: a match ends with everyone
    // leaving at once, and a query per piece of gear per player would be paid
    // on the map thread.
    void RestoreStashedItem(Player* player, Field* fields, ObjectGuid::LowType itemGuid, uint16 position, CharacterDatabaseTransaction& trans)
    {
        uint32 const itemEntry = fields[11].GetUInt32();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto)
        {
            TC_LOG_ERROR("entities.player", "Tournament loadout: stashed item {} of {} has unknown template {}, deleted.",
                itemGuid, player->GetName(), itemEntry);
            Item::DeleteFromDB(trans, itemGuid);
            return;
        }

        Item* item = NewItemOrBag(proto);
        if (!item->LoadFromDB(itemGuid, player->GetGUID(), fields, itemEntry))
        {
            TC_LOG_ERROR("entities.player", "Tournament loadout: stashed item {} of {} could not be loaded.", itemGuid, player->GetName());
            delete item;
            return;
        }

        uint8 const bag = uint8(position >> 8);
        uint8 const slot = uint8(position & 0xFF);

        // Back where it was. not_loading is false on purpose: this is the
        // server handing a character its own gear back, not the character
        // changing clothes, so combat, the arena and the battleground armour
        // lock have no say in it.
        if (bag == INVENTORY_SLOT_BAG_0 && slot < EQUIPMENT_SLOT_END)
        {
            uint16 dest = 0;
            if (player->CanEquipItem(slot, dest, item, false, false) == EQUIP_ERR_OK)
            {
                player->EquipItem(dest, item, true);
                return;
            }
        }
        else
        {
            ItemPosCountVec dest;
            if (player->CanStoreItem(bag, slot, dest, item, false) == EQUIP_ERR_OK)
            {
                player->MoveItemToInventory(dest, item, true);
                return;
            }
        }

        ItemPosCountVec anywhere;
        if (player->CanStoreItem(NULL_BAG, NULL_SLOT, anywhere, item, false) == EQUIP_ERR_OK)
        {
            player->MoveItemToInventory(anywhere, item, true);
            return;
        }

        MailDraft draft("Tournament loadout", "Your bags were full when the match ended, so this came back by post.");
        draft.AddItem(item);
        draft.SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
    }
}

void LoadLoadoutConfig()
{
    LoadoutSettings loaded;
    loaded.Enabled = sConfigMgr->GetBoolDefault("Centurion.Tournament.BgLoadout", true);
    loaded.BanConsumables = sConfigMgr->GetBoolDefault("Centurion.Tournament.BgBanConsumables", true);

    // The tournament PvP consumables: what Jazzik (creature 920027) sells. Both
    // sides of each pair are allowed, so the rule does not depend on which one
    // a character happens to be holding - and the loadout has already turned
    // every twinned item it owns into the tournament one anyway.
    std::string const raw = sConfigMgr->GetStringDefault("Centurion.Tournament.BgConsumables", "200026,200030,200041,200991,200992");
    for (std::string_view token : Trinity::Tokenize(raw, ',', false))
    {
        Optional<uint32> const entry = Trinity::StringTo<uint32>(token);
        if (!entry)
        {
            TC_LOG_ERROR("server.loading", "Centurion.Tournament.BgConsumables: ignoring '{}', not a number.", token);
            continue;
        }

        loaded.AllowedConsumables.insert(*entry);
        if (uint32 const counterpart = GetCounterpartItem(*entry))
            loaded.AllowedConsumables.insert(counterpart);
    }

    LoadoutConfig = std::move(loaded);
}

void LoadLoadoutData()
{
    uint32 const oldMSTime = getMSTime();

    TemplateGear.clear();
    KitPieces.clear();

    StashTablePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_tournament_loadout'") != nullptr;

    // The field kit pool, straight out of the item templates.
    size_t kitPieces = 0;
    for (auto const& itemPair : sObjectMgr->GetItemTemplateStore())
    {
        if (!IsFieldKitDuplicateEntry(itemPair.first) || itemPair.second.InventoryType == INVTYPE_NON_EQUIP)
            continue;

        KitPieces[itemPair.second.InventoryType].push_back(itemPair.first);
        ++kitPieces;
    }

    for (auto& kitPair : KitPieces)
    {
        std::sort(kitPair.second.begin(), kitPair.second.end(), [](uint32 left, uint32 right)
        {
            ItemTemplate const* leftProto = sObjectMgr->GetItemTemplate(left);
            ItemTemplate const* rightProto = sObjectMgr->GetItemTemplate(right);
            uint32 const leftLevel = leftProto ? leftProto->RequiredLevel : 0;
            uint32 const rightLevel = rightProto ? rightProto->RequiredLevel : 0;
            if (leftLevel != rightLevel)
                return leftLevel > rightLevel;
            return left < right;
        });
    }

    // Probe before querying: a missing table aborts the server, and only the
    // Centurion realms have this one.
    if (!WorldDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'tournament_loadout_template'"))
    {
        TC_LOG_INFO("server.loading", ">> No tournament loadout templates on this realm (`tournament_loadout_template` absent); {} field kit piece(s) indexed.", kitPieces);
        return;
    }

    uint32 skipped = 0;
    //                                                   0      1     2
    if (QueryResult result = WorldDatabase.Query("SELECT class, slot, item_entry FROM tournament_loadout_template"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 const playerClass = fields[0].GetUInt8();
            uint8 const slot = fields[1].GetUInt8();
            uint32 const entry = fields[2].GetUInt32();

            if (playerClass >= MAX_CLASSES || slot >= EQUIPMENT_SLOT_END)
            {
                TC_LOG_ERROR("sql.sql", "`tournament_loadout_template`: class {} slot {} is out of range, ignoring.", uint32(playerClass), uint32(slot));
                ++skipped;
                continue;
            }

            if (!sObjectMgr->GetItemTemplate(entry))
            {
                TC_LOG_ERROR("sql.sql", "`tournament_loadout_template`: item {} (class {} slot {}) does not exist, ignoring.", entry, uint32(playerClass), uint32(slot));
                ++skipped;
                continue;
            }

            TemplateGear[TemplateKey(playerClass, slot)] = entry;
        }
        while (result->NextRow());
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} tournament loadout template piece(s), {} skipped, {} field kit piece(s) indexed, in {} ms",
        TemplateGear.size(), skipped, kitPieces, GetMSTimeDiffToNow(oldMSTime));
}

uint32 GetLoadoutTemplateItem(uint8 playerClass, uint8 slot)
{
    auto itr = TemplateGear.find(TemplateKey(playerClass, slot));
    return itr != TemplateGear.end() ? itr->second : 0;
}

bool IsLoadoutSwapInProgress(Player const* player)
{
    return player && SwapInProgress == player;
}

bool HasBattlegroundLoadout(Player const* player)
{
    if (!player)
        return false;

    std::lock_guard<std::mutex> lock(ActiveLoadoutMutex);
    return ActiveLoadouts.count(player->GetGUID()) != 0;
}

void ApplyBattlegroundLoadout(Player* player)
{
    if (!IsEnabled() || !LoadoutConfig.Enabled || !player)
        return;

    Battleground const* battleground = player->GetBattleground();
    if (!battleground || !battleground->IsTournamentPool())
        return;

    if (player->IsGameMaster() || HasBattlegroundLoadout(player))
        return;

    // A transient copy - a battleground fill clone, a colosseum mirror - owns
    // nothing: every item it holds was minted for it minutes ago and dies with
    // it. It is dressed by the same rules, and nothing is written down or kept.
    WorldSession const* session = player->GetSession();
    bool const transient = !session || session->IsTransientPlayerSession();

    if (!transient && !StashTablePresent)
        return;

    SwapGuard guard(player);

    std::vector<LoadoutEntry> entries = BuildLoadout(player);

    // A copy writes nothing down, so it needs no transaction either.
    CharacterDatabaseTransaction trans = transient ? CharacterDatabaseTransaction(nullptr) : CharacterDatabase.BeginTransaction();

    for (LoadoutEntry const& entry : entries)
    {
        if (transient)
            player->DestroyItem(entry.Bag, entry.Slot, true);
        else
            StashOriginal(player, entry, trans);
    }

    for (LoadoutEntry const& entry : entries)
        IssueSubstitute(player, entry, trans, !transient);

    GrantMatchSpells(player, trans, !transient);

    if (!transient)
    {
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        MarkActive(player->GetGUID(), true);
    }

    TC_LOG_DEBUG("bg.battleground", "Tournament loadout: dressed {} on map {} ({} item(s) swapped).",
        player->GetName(), battleground->GetMapId(), entries.size());
}

void RestoreBattlegroundLoadout(Player* player)
{
    if (!player || !StashTablePresent)
        return;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

    QueryResult rows = CharacterDatabase.PQuery("SELECT kind, id, position FROM character_tournament_loadout WHERE guid = {} ORDER BY kind, position", guid);
    if (!rows)
    {
        MarkActive(player->GetGUID(), false);
        return;
    }

    SwapGuard guard(player);

    std::vector<std::pair<uint32 /*item guid*/, uint16 /*position*/>> stashed;
    std::vector<uint32> substitutes;
    std::vector<uint32> spells;

    do
    {
        Field* fields = rows->Fetch();
        switch (fields[0].GetUInt8())
        {
            case LOADOUT_ROW_STASHED:    stashed.emplace_back(fields[1].GetUInt32(), fields[2].GetUInt16()); break;
            case LOADOUT_ROW_SUBSTITUTE: substitutes.push_back(fields[1].GetUInt32()); break;
            case LOADOUT_ROW_SPELL:      spells.push_back(fields[1].GetUInt32()); break;
            default: break;
        }
    }
    while (rows->NextRow());

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // The issued gear goes first: it is standing in the slots the character's
    // own gear is about to want back.
    for (uint32 itemGuid : substitutes)
    {
        if (Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuid)))
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        else
            Item::DeleteFromDB(trans, itemGuid);   // no longer in the bags: destroyed, or left in a match it did not come back from
    }

    if (!stashed.empty())
    {
        std::unordered_map<uint32, uint16> positions;
        for (auto const& stashedItem : stashed)
            positions[stashedItem.first] = stashedItem.second;

        //                                                             0            1                2      3         4        5      6             7                 8           9           10    11         12
        if (QueryResult items = CharacterDatabase.PQuery("SELECT ii.creatorGuid, ii.giftCreatorGuid, ii.count, ii.duration, ii.charges, ii.flags, ii.enchantments, ii.randomPropertyId, "
            "ii.durability, ii.playedTime, ii.text, ii.itemEntry, ii.guid "
            "FROM character_tournament_loadout l JOIN item_instance ii ON ii.guid = l.id WHERE l.guid = {} AND l.kind = {} ORDER BY l.position", guid, uint32(LOADOUT_ROW_STASHED)))
        {
            do
            {
                Field* fields = items->Fetch();
                ObjectGuid::LowType const itemGuid = fields[12].GetUInt32();

                auto positionItr = positions.find(itemGuid);
                RestoreStashedItem(player, fields, itemGuid, positionItr != positions.end() ? positionItr->second : 0, trans);
                positions.erase(itemGuid);
            }
            while (items->NextRow());
        }

        for (auto const& missing : positions)
            TC_LOG_ERROR("entities.player", "Tournament loadout: stashed item {} of {} is no longer in item_instance; it cannot be given back.",
                missing.first, player->GetName());
    }

    for (uint32 spellId : spells)
        if (player->HasSpell(spellId))
            player->RemoveSpell(spellId, false, false);

    trans->Append(Trinity::StringFormat("DELETE FROM character_tournament_loadout WHERE guid = {}", guid).c_str());

    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    MarkActive(player->GetGUID(), false);

    TC_LOG_DEBUG("bg.battleground", "Tournament loadout: gave {} back {} item(s).", player->GetName(), stashed.size());
}

void RestoreLoadoutAfterLogin(Player* player)
{
    if (!player || !StashTablePresent)
        return;

    if (!CharacterDatabase.PQuery("SELECT 1 FROM character_tournament_loadout WHERE guid = {} LIMIT 1", player->GetGUID().GetCounter()))
        return;

    // Logged back into the match it left: it is wearing the loadout for a
    // reason, and leaving will still hand everything back.
    Battleground const* battleground = player->GetBattleground();
    if (battleground && battleground->IsTournamentPool())
    {
        MarkActive(player->GetGUID(), true);
        return;
    }

    RestoreBattlegroundLoadout(player);
}

bool IsConsumableAllowedInMatch(Player const* player, ItemTemplate const* proto)
{
    if (!proto || !player || !IsEnabled() || !LoadoutConfig.BanConsumables)
        return true;

    Battleground const* battleground = player->GetBattleground();
    if (!battleground || !battleground->IsTournamentPool())
        return true;

    if (player->IsGameMaster())
        return true;

    // Potions, elixirs, food, bandages, scrolls - and the thrown explosives that
    // are trade goods rather than consumables. Equipment is the loadout's
    // business, not this rule's.
    bool const consumable = proto->Class == ITEM_CLASS_CONSUMABLE ||
        (proto->Class == ITEM_CLASS_TRADE_GOODS && proto->SubClass == ITEM_SUBCLASS_EXPLOSIVES);
    if (!consumable)
        return true;

    return LoadoutConfig.AllowedConsumables.count(proto->ItemId) != 0;
}
}
