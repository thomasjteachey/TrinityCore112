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

#include "Miscellaneous/BattlegroundSpoils.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Loot.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "World.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    struct SpoilsPool
    {
        std::vector<ItemTemplate const*> items;
        uint32 minLevel = 0;
        uint32 maxLevel = 0;
    };

    std::unordered_map<uint32 /*chest*/, SpoilsPool> Pools;

    // Which primary stats a class has any use for. Stamina is good for all.
    uint32 UsefulPrimaryMask(uint8 playerClass)
    {
        constexpr uint32 AGI = 1 << ITEM_MOD_AGILITY;
        constexpr uint32 STR = 1 << ITEM_MOD_STRENGTH;
        constexpr uint32 INT = 1 << ITEM_MOD_INTELLECT;
        constexpr uint32 SPI = 1 << ITEM_MOD_SPIRIT;

        switch (playerClass)
        {
            case CLASS_WARRIOR:      return STR | AGI;
            case CLASS_PALADIN:      return STR | INT;
            case CLASS_HUNTER:       return AGI | INT;
            case CLASS_ROGUE:        return AGI | STR;
            case CLASS_PRIEST:       return INT | SPI;
            case CLASS_DEATH_KNIGHT: return STR;
            case CLASS_SHAMAN:       return STR | AGI | INT;
            case CLASS_MAGE:         return INT | SPI;
            case CLASS_WARLOCK:      return INT | SPI;
            case CLASS_DRUID:        return STR | AGI | INT | SPI;
            default:                 return STR | AGI | INT | SPI;
        }
    }

    bool IsPrimary(uint32 stat)
    {
        return stat == ITEM_MOD_AGILITY || stat == ITEM_MOD_STRENGTH || stat == ITEM_MOD_INTELLECT || stat == ITEM_MOD_SPIRIT;
    }

    // Running tally of an item's stats against a class: any point of a primary
    // stat the class cannot use makes the item unfit; otherwise the useful
    // points (stamina included) are its score.
    struct StatFit
    {
        uint32 useful = 0;
        bool unfit = false;

        void Add(uint32 statType, int32 value, uint32 usefulMask)
        {
            if (value <= 0)
                return;
            if (IsPrimary(statType))
            {
                if (usefulMask & (1 << statType))
                    useful += value;
                else
                    unfit = true;
            }
            else if (statType == ITEM_MOD_STAMINA)
                useful += value;
        }
    };

    // Adds one enchantment's stats. Anything that is not a plain stat or a
    // resistance - chiefly the spell-damage and healing suffixes, which are
    // equip spells - cannot be judged here, so it counts as unfit and the roll
    // is taken again.
    void AddEnchantment(StatFit& fit, uint32 enchantId, uint32 usefulMask)
    {
        if (!enchantId)
            return;

        SpellItemEnchantmentEntry const* ench = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!ench)
            return;

        for (uint8 i = 0; i < MAX_ITEM_ENCHANTMENT_EFFECTS; ++i)
        {
            switch (ench->Effect[i])
            {
                case ITEM_ENCHANTMENT_TYPE_NONE:
                case ITEM_ENCHANTMENT_TYPE_RESISTANCE:
                    break;
                case ITEM_ENCHANTMENT_TYPE_STAT:
                    fit.Add(ench->EffectArg[i], 1, usefulMask);
                    break;
                default:
                    fit.unfit = true;
                    break;
            }
        }
    }

    // A random property/suffix roll the class can use, or 0 when a dozen
    // tries found none.
    int32 RollFittingProperty(ItemTemplate const* proto, uint32 usefulMask)
    {
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            int32 const prop = GenerateItemRandomPropertyId(proto->ItemId);
            if (!prop)
                return 0;

            StatFit fit;
            if (prop > 0)
            {
                if (ItemRandomPropertiesEntry const* entry = sItemRandomPropertiesStore.LookupEntry(prop))
                    for (uint32 enchant : entry->Enchantment)
                        AddEnchantment(fit, enchant, usefulMask);
            }
            else if (ItemRandomSuffixEntry const* entry = sItemRandomSuffixStore.LookupEntry(-prop))
                for (uint32 enchant : entry->Enchantment)
                    AddEnchantment(fit, enchant, usefulMask);

            if (!fit.unfit && fit.useful > 0)
                return prop;
        }
        return 0;
    }

    bool HasRandomStats(ItemTemplate const* proto)
    {
        return proto->RandomProperty != 0 || proto->RandomSuffix != 0;
    }

    // The heaviest armour the character has learned: plate for a 40+ warrior,
    // mail below that, and so on.
    uint32 BestArmorSubclass(Player const* player)
    {
        uint32 const mask = player->GetArmorProficiency();
        for (uint32 sub : { ITEM_SUBCLASS_ARMOR_PLATE, ITEM_SUBCLASS_ARMOR_MAIL, ITEM_SUBCLASS_ARMOR_LEATHER })
            if (mask & (1 << sub))
                return sub;
        return ITEM_SUBCLASS_ARMOR_CLOTH;
    }

    // Worn or wielded by this character at all.
    bool IsForThisCharacter(ItemTemplate const* proto, Player const* player, uint32 bestArmor)
    {
        if (player->CanUseItem(proto) != EQUIP_ERR_OK)
            return false;

        if (proto->Class == ITEM_CLASS_WEAPON)
            return (player->GetWeaponProficiency() & (1 << proto->SubClass)) != 0;

        if (proto->Class != ITEM_CLASS_ARMOR)
            return false;

        if (proto->InventoryType == INVTYPE_CLOAK)
            return true;

        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_ARMOR_MISC:          // rings, necks, trinkets, off-hands
                return true;
            case ITEM_SUBCLASS_ARMOR_CLOTH:
            case ITEM_SUBCLASS_ARMOR_LEATHER:
            case ITEM_SUBCLASS_ARMOR_MAIL:
            case ITEM_SUBCLASS_ARMOR_PLATE:
                return proto->SubClass == bestArmor;
            default:                                // shields and relics
                return (player->GetArmorProficiency() & (1 << proto->SubClass)) != 0;
        }
    }

    struct Pick
    {
        ItemTemplate const* proto = nullptr;
        int32 property = 0;
    };

    Pick PickFor(SpoilsPool const& pool, Player const* player, uint32 lowLevel, uint32 highLevel, bool allowStatless,
        std::unordered_set<uint32> const& taken)
    {
        uint32 const usefulMask = UsefulPrimaryMask(player->GetClass());
        uint32 const bestArmor = BestArmorSubclass(player);

        std::vector<ItemTemplate const*> candidates;
        for (ItemTemplate const* proto : pool.items)
            if (proto->RequiredLevel >= lowLevel && proto->RequiredLevel <= highLevel && !taken.count(proto->ItemId)
                && IsForThisCharacter(proto, player, bestArmor))
                candidates.push_back(proto);

        Trinity::Containers::RandomShuffle(candidates);

        for (ItemTemplate const* proto : candidates)
        {
            if (HasRandomStats(proto))
            {
                if (int32 prop = RollFittingProperty(proto, usefulMask))
                    return { proto, prop };
                continue;
            }

            StatFit fit;
            for (uint32 i = 0; i < proto->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
                fit.Add(proto->ItemStat[i].ItemStatType, proto->ItemStat[i].ItemStatValue, usefulMask);

            if (fit.unfit)
                continue;
            if (fit.useful > 0 || allowStatless)
                return { proto, 0 };
        }

        return {};
    }
}

void BattlegroundSpoils::LoadPools()
{
    Pools.clear();

    uint32 const first = sWorld->getIntConfig(CONFIG_CENTURION_BG_SPOILS_FIRST_ITEM);
    if (!first)
        return;

    // Ten entries is room for every bracket below any cap this core runs.
    QueryResult result = WorldDatabase.PQuery(
        "SELECT il.Entry, r.Item FROM item_loot_template il "
        "JOIN reference_loot_template r ON r.Entry = il.Reference "
        "WHERE il.Entry BETWEEN {} AND {} AND il.Reference > 0", first, first + 9);
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 battleground spoils pools.");
        return;
    }

    uint32 count = 0;
    std::unordered_map<uint32, std::unordered_set<uint32>> seen;
    do
    {
        Field* fields = result->Fetch();
        uint32 const chest = fields[0].GetUInt32();
        uint32 const itemId = fields[1].GetUInt32();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto || !seen[chest].insert(itemId).second)
            continue;

        SpoilsPool& pool = Pools[chest];
        pool.items.push_back(proto);
        pool.minLevel = pool.items.size() == 1 ? proto->RequiredLevel : std::min(pool.minLevel, proto->RequiredLevel);
        pool.maxLevel = std::max(pool.maxLevel, proto->RequiredLevel);
        ++count;
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} battleground spoils pools ({} items).", uint32(Pools.size()), count);
}

void BattlegroundSpoils::TailorChestLoot(Loot& loot, uint32 chestEntry, Player const* opener)
{
    auto const itr = Pools.find(chestEntry);
    if (itr == Pools.end() || !opener)
        return;

    SpoilsPool const& pool = itr->second;

    // Near their level: the top of the window is their level (or the bracket's
    // top, for a chest opened after levelling out of it), the bottom four under.
    uint32 const top = std::min<uint32>(opener->GetLevel(), pool.maxLevel);
    uint32 const bottom = std::max<uint32>(pool.minLevel, top > 4 ? top - 4 : 0);

    std::unordered_set<uint32> taken;
    for (LootItem& item : loot.items)
    {
        Pick pick = PickFor(pool, opener, bottom, top, false, taken);
        if (!pick.proto)
            pick = PickFor(pool, opener, pool.minLevel, top, false, taken);
        if (!pick.proto)
            pick = PickFor(pool, opener, pool.minLevel, top, true, taken);
        if (!pick.proto)
        {
            TC_LOG_DEBUG("bg.battleground", "BattlegroundSpoils: nothing in chest {} fits {} (class {}, level {}), keeping item {}",
                chestEntry, opener->GetName(), uint32(opener->GetClass()), uint32(opener->GetLevel()), item.itemid);
            continue;
        }

        taken.insert(pick.proto->ItemId);
        item.itemid = pick.proto->ItemId;
        item.randomPropertyId = pick.property;
        item.randomSuffix = pick.property < 0 ? GenerateEnchSuffixFactor(pick.proto->ItemId) : 0;
        item.count = 1;
    }
}
