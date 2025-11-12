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

#include "ScriptMgr.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"

#include <map>

struct HiddenItemsetBonus
{
    uint32 itemsetId;
    uint8 requiredCount;
    uint32 spellId;
};

static std::map<uint32, HiddenItemsetBonus> s_HiddenItemsetBonuses;

void LoadHiddenItemsetBonuses()
{
    s_HiddenItemsetBonuses.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT itemset_id, required_count, spell_to_apply FROM custom_hidden_itemset_bonus"
    );
    if (!result)
    {
        TC_LOG_INFO("server.custom", "Loaded 0 hidden itemset bonuses.");
        return;
    }

    do
    {
        Field* f = result->Fetch();

        HiddenItemsetBonus b;
        b.itemsetId     = f[0].GetUInt32();
        b.requiredCount = f[1].GetUInt8();
        b.spellId       = f[2].GetUInt32();

        s_HiddenItemsetBonuses[b.itemsetId] = b;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.custom", "Loaded {} hidden itemset bonuses.", s_HiddenItemsetBonuses.size());
}

class hidden_itemset_bonus_player : public PlayerScript
{
public:
    hidden_itemset_bonus_player() : PlayerScript("hidden_itemset_bonus_player") { }

    void OnLogin(Player* player, bool) override
    {
        Recalc(player);
    }

    void OnEquip(Player* player, Item*, uint8, uint8, bool) override
    {
        Recalc(player);
    }

    void OnUnequip(Player* player, Item*, uint8, uint8, bool) override
    {
        Recalc(player);
    }

private:
    void Recalc(Player* player);
};

void hidden_itemset_bonus_player::Recalc(Player* player)
{
    // 1) Count equipped items per itemset
    std::map<uint32, uint8> equippedCounts;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!it)
            continue;

        ItemTemplate const* proto = it->GetTemplate();
        if (!proto)
            continue;

        uint32 itemsetId = proto->ItemSet;
        if (!itemsetId)
            continue;

        equippedCounts[itemsetId] += 1;
    }

    // 2) For each configured hidden bonus, decide what to do
    for (auto const& kv : s_HiddenItemsetBonuses)
    {
        uint32 itemsetId = kv.first;
        HiddenItemsetBonus const& bonus = kv.second;

        uint8 have = 0;
        auto it = equippedCounts.find(itemsetId);
        if (it != equippedCounts.end())
            have = it->second;

        bool hasAura = player->HasAura(bonus.spellId);

        if (have >= bonus.requiredCount)
        {
            if (!hasAura)
            {
                player->AddAura(bonus.spellId, player);
                TC_LOG_INFO("server.custom",
                    "hidden_itemset: player {} gained hidden bonus for itemset {} (spell {}).",
                    player->GetGUID().ToString(), itemsetId, bonus.spellId);
            }
        }
        else
        {
            if (hasAura)
            {
                player->RemoveAura(bonus.spellId);
                TC_LOG_INFO("server.custom",
                    "hidden_itemset: player {} lost hidden bonus for itemset {} (spell {}).",
                    player->GetGUID().ToString(), itemsetId, bonus.spellId);
            }
        }
    }
}

void AddSC_custom_hidden_itemset_bonus()
{
    new hidden_itemset_bonus_player();
}
