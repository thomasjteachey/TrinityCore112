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

#include "Transmogrification.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <unordered_set>

namespace
{
    // Inventory types that never take part in transmogrification, on either side.
    bool IsForbiddenInvType(uint32 invType)
    {
        switch (invType)
        {
            case INVTYPE_NON_EQUIP:
            case INVTYPE_NECK:
            case INVTYPE_FINGER:
            case INVTYPE_TRINKET:
            case INVTYPE_BAG:
            case INVTYPE_AMMO:
            case INVTYPE_QUIVER:
            case INVTYPE_RELIC:
                return true;
            default:
                return false;
        }
    }

    // Collapse inventory types that are visually interchangeable, so that a robe
    // can be worn as a chest and a one-hander can pose as an off-hander.
    uint32 NormalizeInvType(uint32 invType)
    {
        switch (invType)
        {
            case INVTYPE_ROBE:
                return INVTYPE_CHEST;
            case INVTYPE_RANGEDRIGHT:
                return INVTYPE_RANGED;
            case INVTYPE_WEAPONMAINHAND:
            case INVTYPE_WEAPONOFFHAND:
                return INVTYPE_WEAPON;
            default:
                return invType;
        }
    }

    // Bucklers are just small shields; everything else compares by raw subclass.
    uint32 NormalizeArmorSubClass(uint32 subClass)
    {
        return subClass == ITEM_SUBCLASS_ARMOR_BUCKLER ? uint32(ITEM_SUBCLASS_ARMOR_SHIELD) : subClass;
    }

    bool IsRangedWeapon(ItemTemplate const* proto)
    {
        return proto->Class == ITEM_CLASS_WEAPON &&
            ((1 << proto->SubClass) & ITEM_SUBCLASS_MASK_WEAPON_RANGED) != 0;
    }

    // Whether the player could ever wear the item, with each requirement
    // individually waivable by config.
    bool IsUsableBy(Player const* player, ItemTemplate const* proto)
    {
        if (((proto->Flags2 & ITEM_FLAG2_FACTION_HORDE) && player->GetTeam() != HORDE) ||
            ((proto->Flags2 & ITEM_FLAG2_FACTION_ALLIANCE) && player->GetTeam() != ALLIANCE))
            return false;

        if (!sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_IGNORE_REQ_CLASS))
            if ((proto->AllowableClass & player->GetClassMask()) == 0)
                return false;

        if (!sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_IGNORE_REQ_RACE))
            if ((proto->AllowableRace & player->GetRaceMask()) == 0)
                return false;

        if (!sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_IGNORE_REQ_LEVEL))
            if (player->GetLevel() < proto->RequiredLevel)
                return false;

        if (!sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_IGNORE_REQ_SKILL) && proto->RequiredSkill)
            if (player->GetSkillValue(proto->RequiredSkill) < proto->RequiredSkillRank)
                return false;

        if (!sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_IGNORE_REQ_SPELL) && proto->RequiredSpell)
            if (!player->HasSpell(proto->RequiredSpell))
                return false;

        return true;
    }

    // Runs `action` over every item the player is carrying: equipped, backpack,
    // equipped bags, keyring, bank slots and bank bags.
    template<typename Action>
    void ForEachOwnedItem(Player const* player, Action action)
    {
        auto visitRange = [&](uint8 first, uint8 last)
        {
            for (uint8 slot = first; slot < last; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    action(item);
        };

        auto visitBags = [&](uint8 first, uint8 last)
        {
            for (uint8 bagSlot = first; bagSlot < last; ++bagSlot)
                if (Bag* bag = player->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        if (Item* item = bag->GetItemByPos(uint8(slot)))
                            action(item);
        };

        visitRange(EQUIPMENT_SLOT_START, EQUIPMENT_SLOT_END);
        visitRange(INVENTORY_SLOT_ITEM_START, INVENTORY_SLOT_ITEM_END);
        visitRange(BANK_SLOT_ITEM_START, BANK_SLOT_ITEM_END);
        visitBags(INVENTORY_SLOT_BAG_START, INVENTORY_SLOT_BAG_END);
        visitBags(BANK_SLOT_BAG_START, BANK_SLOT_BAG_END);
    }
}

namespace Trinity::Transmog
{

bool IsEnabled()
{
    return sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_ENABLE);
}

bool IsTransmogSlot(uint8 slot)
{
    switch (slot)
    {
        case EQUIPMENT_SLOT_HEAD:
        case EQUIPMENT_SLOT_SHOULDERS:
        case EQUIPMENT_SLOT_BODY:
        case EQUIPMENT_SLOT_CHEST:
        case EQUIPMENT_SLOT_WAIST:
        case EQUIPMENT_SLOT_LEGS:
        case EQUIPMENT_SLOT_FEET:
        case EQUIPMENT_SLOT_WRISTS:
        case EQUIPMENT_SLOT_HANDS:
        case EQUIPMENT_SLOT_BACK:
        case EQUIPMENT_SLOT_MAINHAND:
        case EQUIPMENT_SLOT_OFFHAND:
        case EQUIPMENT_SLOT_RANGED:
        case EQUIPMENT_SLOT_TABARD:
            return true;
        default:
            return false;
    }
}

Result CanTransmogrify(Player const* player, Item const* target, ItemTemplate const* source)
{
    if (!IsEnabled())
        return Result::Disabled;

    if (!player)
        return Result::NoTargetItem;

    if (!target)
        return Result::NoTargetItem;

    if (!source)
        return Result::NoSourceItem;

    ItemTemplate const* destination = target->GetTemplate();
    if (!destination)
        return Result::NoTargetItem;

    if (!destination->DisplayInfoID || !source->DisplayInfoID)
        return Result::NoDisplay;

    if (destination->DisplayInfoID == source->DisplayInfoID)
        return Result::SameAppearance;

    if (destination->Class != source->Class)
        return Result::ClassMismatch;

    if (destination->Class != ITEM_CLASS_ARMOR && destination->Class != ITEM_CLASS_WEAPON)
        return Result::ClassMismatch;

    if (IsForbiddenInvType(destination->InventoryType) || IsForbiddenInvType(source->InventoryType))
        return Result::SlotMismatch;

    if (NormalizeInvType(destination->InventoryType) != NormalizeInvType(source->InventoryType))
        return Result::SlotMismatch;

    if (source->Class == ITEM_CLASS_WEAPON)
    {
        // A bow can never stand in for a sword, whatever the mixing config says.
        if (IsRangedWeapon(destination) != IsRangedWeapon(source))
            return Result::WeaponTypeMismatch;

        if (source->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE &&
            !sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_ALLOW_FISHING_POLES))
            return Result::WeaponTypeMismatch;

        if (destination->SubClass != source->SubClass &&
            !sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_ALLOW_MIXED_WEAPON_TYPES))
            return Result::WeaponTypeMismatch;
    }
    else if (NormalizeArmorSubClass(destination->SubClass) != NormalizeArmorSubClass(source->SubClass) &&
        !sWorld->getBoolConfig(CONFIG_CENTURION_TRANSMOG_ALLOW_MIXED_ARMOR_TYPES))
        return Result::ArmorTypeMismatch;

    // Quality is gated on the source only: what the player is actually wearing is
    // their business, the appearance is what we are handing out.
    if (source->Quality >= MAX_ITEM_QUALITY ||
        (sWorld->getIntConfig(CONFIG_CENTURION_TRANSMOG_QUALITY_MASK) & (1 << source->Quality)) == 0)
        return Result::QualityNotAllowed;

    if (!IsUsableBy(player, source))
        return Result::NotUsable;

    return Result::Ok;
}

void CollectSourcesForSlot(Player const* player, uint8 slot, std::vector<ItemTemplate const*>& out)
{
    out.clear();

    if (!player || !IsTransmogSlot(slot))
        return;

    Item const* target = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!target)
        return;

    std::unordered_set<uint32> seen;
    ForEachOwnedItem(player, [&](Item const* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !seen.insert(proto->ItemId).second)
            return;

        if (CanTransmogrify(player, target, proto) == Result::Ok)
            out.push_back(proto);
    });

    std::sort(out.begin(), out.end(), [](ItemTemplate const* left, ItemTemplate const* right)
    {
        if (left->Quality != right->Quality)
            return left->Quality > right->Quality;
        return left->Name1 < right->Name1;
    });
}

uint32 GetTokenEntry()
{
    return sWorld->getIntConfig(CONFIG_CENTURION_TRANSMOG_TOKEN_ENTRY);
}

uint32 GetTokenCost()
{
    return sWorld->getIntConfig(CONFIG_CENTURION_TRANSMOG_TOKEN_COST);
}

std::string GetTokenName()
{
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(GetTokenEntry()))
        return proto->Name1;

    return "tokens";
}

bool HasEnoughTokens(Player const* player)
{
    uint32 const cost = GetTokenCost();
    if (!cost || !GetTokenEntry())
        return true;

    return player && player->GetItemCount(GetTokenEntry(), true) >= cost;
}

bool TakeTokens(Player* player)
{
    uint32 const cost = GetTokenCost();
    if (!cost || !GetTokenEntry())
        return true;

    if (!HasEnoughTokens(player))
        return false;

    player->DestroyItemCount(GetTokenEntry(), cost, true, false);
    return true;
}

char const* GetResultText(Result result)
{
    switch (result)
    {
        case Result::Ok:                 return "";
        case Result::Disabled:           return "Transmogrification is disabled.";
        case Result::BadSlot:            return "That slot cannot be transmogrified.";
        case Result::NoTargetItem:       return "You have nothing equipped in that slot.";
        case Result::NoSourceItem:       return "You no longer have that item.";
        case Result::SameAppearance:     return "That item already looks like this.";
        case Result::ClassMismatch:      return "Those two items are nothing alike.";
        case Result::SlotMismatch:       return "That item is worn in a different slot.";
        case Result::ArmorTypeMismatch:  return "That is a different type of armor.";
        case Result::WeaponTypeMismatch: return "That is a different type of weapon.";
        case Result::NoDisplay:          return "That item has no appearance to copy.";
        case Result::QualityNotAllowed:  return "That item's quality is not allowed.";
        case Result::NotUsable:          return "You could never wear that item.";
        case Result::NotEnoughTokens:    return "You do not have enough tokens.";
        default:                         return "You cannot do that.";
    }
}

std::string GetColoredName(ItemTemplate const* proto)
{
    if (!proto)
        return "";

    uint32 const quality = proto->Quality < MAX_ITEM_QUALITY ? proto->Quality : uint32(ITEM_QUALITY_POOR);
    return Trinity::StringFormat("|c{:08x}{}|r", ItemQualityColors[quality], proto->Name1);
}

}
