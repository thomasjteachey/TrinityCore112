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
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"

#include <map>
#include <set>
#include <shared_mutex>

namespace
{
struct HiddenSetInfo
{
    uint32 setGroup = 0;
    uint32 itemEntry = 0;
    uint8 requiredCount = 0;
    uint32 spellId = 0;
};

using HiddenSetStore = std::multimap<uint32, HiddenSetInfo>;

HiddenSetStore s_HiddenSets;
std::set<uint32> s_AllHiddenSetSpells;

bool IsItemEquipped(Player* player, uint32 itemEntry)
{
    if (!player)
        return false;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetEntry() == itemEntry)
                return true;

    return false;
}

void RecalcHiddenSets(Player* player)
{
    if (!player)
        return;

    std::map<uint32, uint8> counts;
    std::map<uint32, uint8> required;
    std::map<uint32, uint32> setSpells;
    std::set<uint32> validSpells;

    for (HiddenSetStore::value_type const& pair : s_HiddenSets)
    {
        HiddenSetInfo const& info = pair.second;

        required[info.setGroup] = info.requiredCount;
        setSpells[info.setGroup] = info.spellId;
        validSpells.insert(info.spellId);

        if (IsItemEquipped(player, info.itemEntry))
            ++counts[info.setGroup];
    }

    for (auto const& requirement : required)
    {
        uint32 const setGroup = requirement.first;
        uint8 const need = requirement.second;
        uint8 const have = counts[setGroup];
        uint32 const spellId = setSpells[setGroup];

        if (have >= need)
        {
            if (!player->HasAura(spellId))
            {
                player->AddAura(spellId, player);
                TC_LOG_INFO("server.custom", "hidden_set: player {} gained hidden set {} (spell {}).",
                    player->GetGUID().ToString(), setGroup, spellId);
            }
        }
        else
        {
            if (player->HasAura(spellId))
            {
                player->RemoveAura(spellId);
                TC_LOG_INFO("server.custom", "hidden_set: player {} lost hidden set {} (spell {}).",
                    player->GetGUID().ToString(), setGroup, spellId);
            }
        }
    }

    for (uint32 spellId : s_AllHiddenSetSpells)
    {
        if (validSpells.find(spellId) == validSpells.end() && player->HasAura(spellId))
            player->RemoveAura(spellId);
    }
}
} // namespace

namespace HiddenSets
{
void OnEquipmentChanged(Player* player)
{
    RecalcHiddenSets(player);
}
} // namespace HiddenSets

void LoadHiddenSets()
{
    s_HiddenSets.clear();

    TC_LOG_INFO("server.custom", "Loading hidden set definitions...");

    QueryResult result = WorldDatabase.Query(
        "SELECT set_group, item_entry, required_count, spell_to_apply FROM custom_hidden_sets");

    if (!result)
    {
        TC_LOG_INFO("server.custom", ">> Loaded 0 hidden set entries (table is empty).");
        return;
    }

    uint32 count = 0;
    std::set<uint32> currentSpells;

    do
    {
        Field* fields = result->Fetch();

        HiddenSetInfo info;
        info.setGroup = fields[0].GetUInt32();
        info.itemEntry = fields[1].GetUInt32();
        info.requiredCount = fields[2].GetUInt8();
        info.spellId = fields[3].GetUInt32();

        s_HiddenSets.emplace(info.setGroup, info);
        currentSpells.insert(info.spellId);
        ++count;
    }
    while (result->NextRow());

    s_AllHiddenSetSpells.insert(currentSpells.begin(), currentSpells.end());

    TC_LOG_INFO("server.custom", ">> Loaded {} hidden set entries.", count);
}

namespace
{
using namespace Trinity::ChatCommands;

class hidden_set_player_script : public PlayerScript
{
public:
    hidden_set_player_script() : PlayerScript("hidden_set_player_script") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        HiddenSets::OnEquipmentChanged(player);
    }
};

class hidden_set_command : public CommandScript
{
public:
    hidden_set_command() : CommandScript("hidden_set_command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable reloadCommandTable =
        {
            { "reload", HandleReload, rbac::RBAC_PERM_COMMAND_GM, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "hiddenset", reloadCommandTable }
        };

        return commandTable;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadHiddenSets();

        {
            std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            for (auto const& playerEntry : players)
                if (Player* player = playerEntry.second)
                    HiddenSets::OnEquipmentChanged(player);
        }

        handler->SendSysMessage("Hidden sets reloaded.");
        return true;
    }
};
} // namespace

void AddSC_custom_hidden_sets()
{
    new hidden_set_player_script();
    new hidden_set_command();
}
