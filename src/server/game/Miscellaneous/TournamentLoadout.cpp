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
//   2. it is a weapon, a shield or a holdable with no twin -> a tournament one
//      of the SAME shape (a one-handed axe for a one-handed axe, a bow for a
//      bow), no better than the class template's piece for that slot,
//   3. anything else worn with no twin                    -> the same slot from
//      its class's starter template (`tournament_loadout_template`: the gear
//      Startrogue, Startwarrior and the rest are wearing),
//   4. what that offered cannot be equipped - unique, no proficiency, the wrong
//      armour, an off hand behind a two-hander - -> a field kit piece for the
//      slot, and nothing at all when the kit has none for it (neck, rings and
//      trinkets have none),
//   5. it is carried and has no twin                      -> it is put away for
//      the match, so nothing untwinned can be swapped in mid-fight.
//
// An EMPTY equipment slot is dressed from the class template too: somebody who
// walked in without a neck, rings or trinkets fights in the tournament's and
// loses them again on the way out. Shirts and tabards are nobody's business.
//
// The originals are neither destroyed nor rebuilt from a description: the item
// rows are moved aside exactly as an account bank deposit moves them
// (Accounts/AccountBankMgr.cpp) and handed back with their enchants, charges,
// durability and guids intact. Every move is written to
// `character_tournament_loadout` in the same transaction that makes it, so a
// logout, a disconnect, a crash or a restart mid-match all end the same way: the
// next login gives the character its own gear back.
//
// And none of it leaves with a world-mode character: SweepTournamentItems takes
// every tournament item off one that is outside a tournament match, on the way
// out and again at every login. The rows are the bookkeeping; the sweep is the
// guarantee that survives a realm being killed outright, a row nobody wrote, or
// an item handed over inside the match.
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
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <functional>
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

    // Every weapon, shield and holdable the tournament sells, by class and
    // subclass, highest item level first. A hand that held a one-handed axe gets
    // a one-handed axe back: the class template can only offer one shape per
    // slot, and its sword is no answer for somebody who fights with axes.
    std::unordered_map<uint32 /*class << 8 | subclass*/, std::vector<uint32>> TournamentWeapons;
    // Whether this realm has the stash table at all. Probed at load: a realm
    // without it never queries it, and every path here does nothing.
    bool StashTablePresent = false;

    // Characters wearing a loadout right now. The database is the record that
    // survives a restart; this is what the hot paths ask.
    std::mutex ActiveLoadoutMutex;
    std::unordered_set<ObjectGuid> ActiveLoadouts;

    // Characters whose gear was handed back while they were on their way out of
    // the battleground map. The field values are right the moment the swap ends,
    // but a client in the middle of a world port can be shown the old weapon
    // until something makes the server say it again - which is why logging out
    // and back in used to be the cure. Refreshed on the first update after the
    // character is back in a world, by the tournament player script.
    std::atomic<size_t> PendingVisualCount{ 0 };
    std::mutex PendingVisualMutex;
    std::unordered_set<ObjectGuid> PendingVisuals;

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

    void MarkVisualsStale(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(PendingVisualMutex);
        PendingVisuals.insert(guid);
        PendingVisualCount.store(PendingVisuals.size(), std::memory_order_relaxed);
    }

    // Every position a character can keep an item in: what it is wearing, its
    // bags and their contents, the bank and its bags. The keyring holds keys.
    void ForEachHeldItem(Player* player, std::function<void(uint8 /*bag*/, uint8 /*slot*/, Item*)> const& visit)
    {
        auto const visitRange = [&](uint8 bag, uint8 first, uint8 last)
        {
            for (uint8 slot = first; slot < last; ++slot)
                if (Item* item = player->GetItemByPos(bag, slot))
                    visit(bag, slot, item);
        };

        visitRange(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_START, EQUIPMENT_SLOT_END);
        visitRange(INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_ITEM_START, INVENTORY_SLOT_ITEM_END);
        visitRange(INVENTORY_SLOT_BAG_0, BANK_SLOT_ITEM_START, BANK_SLOT_ITEM_END);

        auto const visitBags = [&](uint8 first, uint8 last)
        {
            for (uint8 bag = first; bag < last; ++bag)
                if (Bag const* container = player->GetBagByPos(bag))
                    for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                        if (Item* item = player->GetItemByPos(bag, uint8(slot)))
                            visit(bag, uint8(slot), item);
        };

        visitBags(INVENTORY_SLOT_BAG_START, INVENTORY_SLOT_BAG_END);
        visitBags(BANK_SLOT_BAG_START, BANK_SLOT_BAG_END);
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

    uint32 WeaponKey(uint32 itemClass, uint32 subClass)
    {
        return (itemClass << 8) | subClass;
    }

    bool IsHandSlot(uint8 slot)
    {
        return slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND || slot == EQUIPMENT_SLOT_RANGED;
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
        Item* Original = nullptr;   // valid only until the item is stashed; null for an empty slot being dressed
        uint8 Bag = 0;
        uint8 Slot = 0;
        uint32 Count = 1;
        uint32 Substitute = 0;      // the twin when there is one, else the class's template piece
        bool Worn = false;
        bool Twinned = false;       // the substitute above is this item's own tournament twin
        uint32 OriginalClass = 0;   // what the hand was holding, for the shape match below
        uint32 OriginalSubClass = 0;
        uint32 OriginalInventoryType = 0;
        std::array<CarriedEnchant, CarriedEnchantSlots.size()> Enchants = { };
    };

    // A hand keeps its shape: a one-handed axe comes back a one-handed axe, a
    // bow a bow, a shield a shield - the class template only has one weapon per
    // slot and its sword is no answer for somebody who fights with axes. Held to
    // the template piece's item level, so matching the shape cannot hand out
    // something better than the loadout is meant to give.
    uint32 PickTournamentWeapon(Player* player, LoadoutEntry const& entry)
    {
        if (!entry.OriginalInventoryType || !IsHandSlot(entry.Slot))
            return 0;

        auto itr = TournamentWeapons.find(WeaponKey(entry.OriginalClass, entry.OriginalSubClass));
        if (itr == TournamentWeapons.end())
            return 0;

        uint32 cap = 0;
        if (ItemTemplate const* templateProto = sObjectMgr->GetItemTemplate(GetLoadoutTemplateItem(player->GetClass(), entry.Slot)))
            cap = templateProto->ItemLevel;

        bool const twoHanded = entry.OriginalInventoryType == INVTYPE_2HWEAPON;
        for (uint32 candidate : itr->second)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(candidate);
            if (!proto)
                continue;

            // Never turn one hand into two, or two into one: the other hand is
            // being answered by its own rule and the two have to agree.
            if ((proto->InventoryType == INVTYPE_2HWEAPON) != twoHanded)
                continue;

            if (cap && proto->ItemLevel > cap)
                continue;

            uint16 dest = 0;
            if (player->CanEquipNewItem(entry.Slot, dest, candidate, false) == EQUIP_ERR_OK)
                return candidate;
        }

        return 0;
    }

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
        {
            // An empty equipment slot is dressed from the class's template: a
            // character that walked in without a neck, rings or trinkets fights
            // in the tournament's, and loses them again on the way out. Shirt and
            // tabard are not the loadout's business, and neither is bag space.
            if (!worn || slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                return;

            uint32 const piece = GetLoadoutTemplateItem(player->GetClass(), slot);
            if (!piece)
                return;

            LoadoutEntry empty;
            empty.Bag = bag;
            empty.Slot = slot;
            empty.Worn = true;
            empty.Substitute = piece;
            out.push_back(empty);
            return;
        }

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
        entry.OriginalClass = proto->Class;
        entry.OriginalSubClass = proto->SubClass;
        entry.OriginalInventoryType = proto->InventoryType;

        for (size_t index = 0; index < CarriedEnchantSlots.size(); ++index)
        {
            EnchantmentSlot const enchantSlot = CarriedEnchantSlots[index];
            entry.Enchants[index].Id = item->GetEnchantmentId(enchantSlot);
            entry.Enchants[index].Duration = item->GetEnchantmentDuration(enchantSlot);
            entry.Enchants[index].Charges = item->GetEnchantmentCharges(enchantSlot);
        }

        if (twin)
        {
            entry.Substitute = twin;                                              // rule 1
            entry.Twinned = true;
        }
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
        // it swallows. Empty slots are collected in the same pass, so they keep
        // their place in that order.
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

    // Returns true when something was actually issued.
    bool IssueSubstitute(Player* player, LoadoutEntry const& entry, CharacterDatabaseTransaction& trans, bool record)
    {
        uint32 wanted = entry.Substitute;
        Item* issued = nullptr;

        if (entry.Worn)
        {
            // In order: this item's own twin, then a tournament weapon of the
            // same shape for a hand, then the class's template piece, then a kit
            // piece - and an empty slot when even that cannot be worn (unique,
            // no proficiency, the wrong armour, an off hand behind a two-hander).
            std::vector<uint32> candidates;
            if (entry.Twinned && entry.Substitute)
                candidates.push_back(entry.Substitute);

            if (!entry.Twinned)
                if (uint32 const shaped = PickTournamentWeapon(player, entry))
                    candidates.push_back(shaped);

            if (!entry.Twinned && entry.Substitute)
                candidates.push_back(entry.Substitute);
            else if (entry.Twinned)
                if (uint32 const fromTemplate = GetLoadoutTemplateItem(player->GetClass(), entry.Slot))
                    candidates.push_back(fromTemplate);

            if (uint32 const kit = PickKitPiece(player, entry.Slot))
                candidates.push_back(kit);

            uint16 dest = 0;
            wanted = 0;
            for (uint32 candidate : candidates)
            {
                if (player->CanEquipNewItem(entry.Slot, dest, candidate, false) != EQUIP_ERR_OK)
                    continue;

                wanted = candidate;
                break;
            }

            if (wanted)
                issued = player->EquipNewItem(dest, wanted, true);
        }
        else if (wanted)
        {
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(entry.Bag, entry.Slot, dest, wanted, entry.Count) != EQUIP_ERR_OK &&
                player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, wanted, entry.Count) != EQUIP_ERR_OK)
                return false;

            issued = player->StoreNewItem(dest, wanted, true);
        }

        if (!issued)
            return false;

        CarryEnchants(player, entry, issued);

        if (record)
            InsertRow(trans, player->GetGUID().GetCounter(), LOADOUT_ROW_SUBSTITUTE, issued->GetGUID().GetCounter(),
                PackPosition(issued->GetBagSlot(), issued->GetSlot()));

        return true;
    }

    // A world-mode character borrows the eat/drink/bandage spells a tournament
    // character has always known, for the length of the match. One it already
    // knows is left alone and not written down, so leaving can never take away
    // something it learned for itself.
    // Returns how many spells were actually taught.
    uint32 GrantMatchSpells(Player* player, CharacterDatabaseTransaction& trans, bool record)
    {
        if (IsTournamentCharacter(player))
            return 0;

        uint32 taught = 0;

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
            ++taught;
            if (record)
                InsertRow(trans, player->GetGUID().GetCounter(), LOADOUT_ROW_SPELL, spellId, 0);
        }

        return taught;
    }

    // Eat, drink and bandage out of a character's own bags, for free. The
    // tournament hands those out as spells, so somebody who reaches for the food
    // in their pack instead of the innate one is not spending anything either -
    // the item is used and stays where it is (Spell::TakeCastItem asks).
    //
    // Only what this realm already lets anyone use in an arena - the arena flag,
    // a conjured consumable, or a real First Aid bandage, exactly as
    // Handlers/SpellHandler.cpp judges it - and only the eat/drink/bandage
    // family: a healthstone is not a meal, and an endless one would be a hole.
    // Classic bandage ranks sit on the food subclass, which is why both are here.
    bool IsFreeMatchConsumable(ItemTemplate const* proto)
    {
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE)
            return false;

        if (proto->SubClass != ITEM_SUBCLASS_FOOD && proto->SubClass != ITEM_SUBCLASS_BANDAGE)
            return false;

        if (proto->HasFlag(ITEM_FLAG_IGNORE_DEFAULT_ARENA_RESTRICTIONS) || proto->IsConjuredConsumable())
            return true;

        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId))
                if (spellInfo->Mechanic == MECHANIC_BANDAGE)
                    return true;

        return false;
    }

    bool InTournamentMatch(Player const* player)
    {
        if (!player || !IsEnabled())
            return false;

        Battleground const* battleground = player->GetBattleground();
        return battleground && battleground->IsTournamentPool();
    }

    // Chromie keeps the tournament's clock, and she is already the voice that
    // turns people away from a battleground door (Handlers/BattleGroundHandler.cpp),
    // so she is the one who explains what just happened to a world-mode
    // character's gear. Tournament characters have always lived by these rules
    // and are told nothing.
    uint32 constexpr ChromieEntry = 27915;
    char const* const ChromieName = "Chromie";

    void WhisperAsChromie(Player* player, std::string const& message)
    {
        WorldPacket data;
        ObjectGuid const chromieGuid = ObjectGuid::Create<HighGuid::Unit>(ChromieEntry, 1);
        ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_WHISPER, LANG_UNIVERSAL, chromieGuid, player->GetGUID(), message,
            0, ChromieName, player->GetName());
        player->SendDirectMessage(&data);
    }

    void BriefWorldCharacter(Player* player, uint32 swapped, uint32 putAway, uint32 spells)
    {
        WorldSession const* session = player->GetSession();
        if (!session || session->IsVirtualSession() || IsTournamentCharacter(player))
            return;

        WhisperAsChromie(player, "Welcome to the tournament. Everyone fights on the same footing in here, so I have taken care of your equipment.");

        if (swapped || putAway)
        {
            std::string what = "You are wearing tournament gear for this match";
            if (swapped)
                what += Trinity::StringFormat(" - {} piece(s) of it", swapped);
            if (putAway)
                what += Trinity::StringFormat(", and {} thing(s) you were carrying are being kept aside", putAway);
            what += ". Any enchants and gems you paid for came across with it.";
            WhisperAsChromie(player, what);
        }

        WhisperAsChromie(player, "Everything of yours comes straight back the moment you leave - a win, a loss, a disconnect, a crash, it makes no difference. None of it is lost.");

        if (spells)
            WhisperAsChromie(player, "You also know how to eat, drink and bandage while you are here, whether or not you ever learned them. That knowledge leaves with the match.");

        WhisperAsChromie(player, "Your own food, drink and bandages still work in here, and using one costs you nothing - you will walk out with everything you walked in with.");

        if (LoadoutConfig.BanConsumables)
            WhisperAsChromie(player, "Nothing else out of your bags works in here: no potions, elixirs, food or grenades but the tournament's own, which Jazzik sells.");
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

// Tournament gear does not leave the arena on a world-mode character. The rows
// in `character_tournament_loadout` are the bookkeeping, and this is the
// guarantee that does not depend on it: whatever the tournament owns is taken
// off a world character the moment it is outside a tournament match - on the way
// out, and again at EVERY login, which is where an ungraceful shutdown, a lost
// row, a trade inside the match and anything else nobody thought of is caught.
//
// A tournament character keeps its gear, of course, and a Game Master is left
// alone: they can conjure anything anyway, and eating a GM's test items would be
// its own bug.
uint32 SweepTournamentItems(Player* player)
{
    if (!player || !IsEnabled() || IsTournamentCharacter(player) || player->IsGameMaster())
        return 0;

    std::vector<std::pair<uint8, uint8>> confiscate;
    ForEachHeldItem(player, [&confiscate](uint8 bag, uint8 slot, Item* item)
    {
        if (IsTournamentItem(item->GetEntry()))
            confiscate.emplace_back(bag, slot);
    });

    if (confiscate.empty())
        return 0;

    // The swap itself reaches every slot, so it takes the same waiver: a
    // character swept while still standing on the battleground map would
    // otherwise keep whatever its armour slots were holding.
    SwapGuard guard(player);

    for (auto const& position : confiscate)
        player->DestroyItem(position.first, position.second, true);

    player->SaveToDB(false);

    TC_LOG_INFO("entities.player.items", "Tournament loadout: took {} tournament item(s) off {} outside a tournament match.",
        confiscate.size(), player->GetName());
    return uint32(confiscate.size());
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
    TournamentWeapons.clear();

    StashTablePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_tournament_loadout'") != nullptr;

    // The field kit pool and the tournament's own weapon rack, straight out of
    // the item templates. The rack is every weapon, shield and holdable the
    // tournament sells - the item links say which items those are, so it is
    // built after LoadItemLinks.
    size_t kitPieces = 0;
    size_t weapons = 0;
    for (auto const& itemPair : sObjectMgr->GetItemTemplateStore())
    {
        ItemTemplate const& proto = itemPair.second;
        if (proto.InventoryType == INVTYPE_NON_EQUIP)
            continue;

        if (IsFieldKitDuplicateEntry(itemPair.first))
        {
            KitPieces[proto.InventoryType].push_back(itemPair.first);
            ++kitPieces;
            continue;
        }

        if (!IsTournamentItem(itemPair.first))
            continue;

        bool const armsAndShields = proto.Class == ITEM_CLASS_WEAPON ||
            (proto.Class == ITEM_CLASS_ARMOR && (proto.InventoryType == INVTYPE_SHIELD || proto.InventoryType == INVTYPE_HOLDABLE));
        if (!armsAndShields)
            continue;

        TournamentWeapons[WeaponKey(proto.Class, proto.SubClass)].push_back(itemPair.first);
        ++weapons;
    }

    for (auto& weaponPair : TournamentWeapons)
    {
        std::sort(weaponPair.second.begin(), weaponPair.second.end(), [](uint32 left, uint32 right)
        {
            ItemTemplate const* leftProto = sObjectMgr->GetItemTemplate(left);
            ItemTemplate const* rightProto = sObjectMgr->GetItemTemplate(right);
            uint32 const leftLevel = leftProto ? leftProto->ItemLevel : 0;
            uint32 const rightLevel = rightProto ? rightProto->ItemLevel : 0;
            if (leftLevel != rightLevel)
                return leftLevel > rightLevel;
            return left < right;
        });
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

    TC_LOG_INFO("server.loading", ">> Loaded {} tournament loadout template piece(s), {} skipped, {} field kit piece(s) and {} tournament weapon(s) indexed, in {} ms",
        TemplateGear.size(), skipped, kitPieces, weapons, GetMSTimeDiffToNow(oldMSTime));
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

    uint32 issued = 0;
    for (LoadoutEntry const& entry : entries)
        if (IssueSubstitute(player, entry, trans, !transient))
            ++issued;

    uint32 const taught = GrantMatchSpells(player, trans, !transient);

    if (!transient)
    {
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        MarkActive(player->GetGUID(), true);

        uint32 putAway = 0;
        for (LoadoutEntry const& entry : entries)
            if (entry.Original && !entry.Worn)
                ++putAway;

        BriefWorldCharacter(player, issued, putAway, taught);
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

    // Anything of the tournament's that the rows did not account for - traded
    // inside the match, left over from an interrupted swap - goes here.
    SweepTournamentItems(player);

    // The character is usually on its way to the entry point as this runs, and a
    // client in the middle of a world port can keep showing the weapon it just
    // put down. Say it again once it has arrived.
    MarkVisualsStale(player->GetGUID());

    TC_LOG_DEBUG("bg.battleground", "Tournament loadout: gave {} back {} item(s).", player->GetName(), stashed.size());
}

void RestoreLoadoutAfterLogin(Player* player)
{
    if (!player)
        return;

    // Logged back into the match it left: it is wearing the loadout for a
    // reason, and leaving will still hand everything back.
    Battleground const* battleground = player->GetBattleground();
    if (battleground && battleground->IsTournamentPool())
    {
        if (StashTablePresent && CharacterDatabase.PQuery("SELECT 1 FROM character_tournament_loadout WHERE guid = {} LIMIT 1", player->GetGUID().GetCounter()))
            MarkActive(player->GetGUID(), true);
        return;
    }

    if (StashTablePresent && CharacterDatabase.PQuery("SELECT 1 FROM character_tournament_loadout WHERE guid = {} LIMIT 1", player->GetGUID().GetCounter()))
        RestoreBattlegroundLoadout(player);

    // Swept whether or not there were rows to restore. A realm that was killed
    // outright - no shutdown, no save, no chance to hand anything back - is the
    // case this catches: the character logs in wearing the tournament's gear and
    // takes it off here, at the door.
    SweepTournamentItems(player);
}

void RefreshLoadoutVisuals(Player* player)
{
    if (!player || PendingVisualCount.load(std::memory_order_relaxed) == 0)
        return;

    {
        std::lock_guard<std::mutex> lock(PendingVisualMutex);
        if (!PendingVisuals.count(player->GetGUID()))
            return;
    }

    // Wait for the world port to finish: told now, the client would drop it
    // again, and this is what the stale weapon was in the first place.
    if (!player->IsInWorld() || player->IsBeingTeleported())
        return;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        player->SetVisibleItemSlot(slot, player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        player->ForceValuesUpdateAtIndex(PLAYER_VISIBLE_ITEM_1_ENTRYID + (slot * 2));
        player->ForceValuesUpdateAtIndex(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + (slot * 2));
    }

    std::lock_guard<std::mutex> lock(PendingVisualMutex);
    PendingVisuals.erase(player->GetGUID());
    PendingVisualCount.store(PendingVisuals.size(), std::memory_order_relaxed);
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

    // Eating, drinking and bandaging are what the tournament gives everyone for
    // nothing; doing it out of your own bags is the same thing by another route,
    // and costs the item nothing either (KeepsCastItem).
    if (IsFreeMatchConsumable(proto))
        return true;

    return LoadoutConfig.AllowedConsumables.count(proto->ItemId) != 0;
}

bool KeepsCastItem(Player const* player, ItemTemplate const* proto)
{
    return InTournamentMatch(player) && IsFreeMatchConsumable(proto);
}
}
