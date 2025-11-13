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

void LoadHiddenItemsetBonuses();

namespace
{
struct HiddenItemsetBonus
{
    uint32 itemsetId;
    uint8 requiredCount;
    uint32 spellId;
};

using HiddenItemsetBonusMap = std::map<uint32, HiddenItemsetBonus>;

HiddenItemsetBonusMap s_HiddenItemsetBonuses;
std::set<uint32> s_AllHiddenItemsetBonusSpells;

void RecalcHiddenItemsetBonuses(Player* player)
{
    if (!player)
        return;

    std::set<uint32> validSpells;

    for (auto const& kv : s_HiddenItemsetBonuses)
    {
        uint32 itemsetId = kv.first;
        HiddenItemsetBonus const& bonus = kv.second;

        validSpells.insert(bonus.spellId);

        uint8 have = player->GetEquippedItemSetCount(itemsetId);

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

    for (uint32 spellId : s_AllHiddenItemsetBonusSpells)
    {
        if (validSpells.find(spellId) == validSpells.end() && player->HasAura(spellId))
            player->RemoveAura(spellId);
    }
}

class hidden_itemset_bonus_player : public PlayerScript
{
public:
    hidden_itemset_bonus_player() : PlayerScript("hidden_itemset_bonus_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        RecalcHiddenItemsetBonuses(player);
    }

    void OnPlayerResurrect(Player* player) override
    {
        RecalcHiddenItemsetBonuses(player);
    }
};

using namespace Trinity::ChatCommands;

class hidden_itemset_bonus_command : public CommandScript
{
public:
    hidden_itemset_bonus_command() : CommandScript("hidden_itemset_bonus_command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable reloadCommandTable =
        {
            { "reload", HandleReload, rbac::RBAC_PERM_COMMAND_GM, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "hiddenitemset", reloadCommandTable }
        };

        return commandTable;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadHiddenItemsetBonuses();

        {
            std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            for (auto const& playerEntry : players)
                if (Player* player = playerEntry.second)
                    RecalcHiddenItemsetBonuses(player);
        }

        handler->SendSysMessage("Hidden itemset bonuses reloaded.");
        return true;
    }
};
} // namespace

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

    std::set<uint32> currentSpells;

    do
    {
        Field* f = result->Fetch();

        HiddenItemsetBonus b;
        b.itemsetId     = f[0].GetUInt32();
        b.requiredCount = f[1].GetUInt8();
        b.spellId       = f[2].GetUInt32();

        s_HiddenItemsetBonuses[b.itemsetId] = b;
        currentSpells.insert(b.spellId);
    }
    while (result->NextRow());

    s_AllHiddenItemsetBonusSpells.insert(currentSpells.begin(), currentSpells.end());

    TC_LOG_INFO("server.custom", "Loaded {} hidden itemset bonuses.", s_HiddenItemsetBonuses.size());
}

namespace HiddenSets
{
void OnEquipmentChanged(Player* player)
{
    RecalcHiddenItemsetBonuses(player);
}
}

void AddSC_custom_hidden_itemset_bonus()
{
    new hidden_itemset_bonus_player();
    new hidden_itemset_bonus_command();
}
