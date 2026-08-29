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

// Barracks+ hardcore ruleset (config-gated; entirely inert on Legionnaire+):
//  - Loot drop on death: WORN EQUIPMENT is at stake - a configurable share
//    drops into a chest at the corpse (Dire Maul beads style), the rest is
//    destroyed as a deflationary sink. Bags, inventory and money are safe.
//    World only - battlegrounds and arenas are exempt.
//  - Opt-in free-for-all PvP: a flagger NPC in the capitals toggles it. The
//    flag only ARMS in zones of a configurable minimum level - never in
//    starter zones, capitals or sanctuaries - and while armed the player
//    earns double experience and loot gold.
//  - Playerbots are ALWAYS armed in eligible zones and render red to
//    everyone.
// Item bindings are removed by the core-side Centurion.Hardcore.NoBinding
// switch (Item::IsSoulBound), which also opens the auction house to
// everything.

#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Duration.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "Util.h"
#include "WorldSession.h"
#include "custom_loot_chest_helper.h"
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace BarracksHardcore
{
    bool s_enabled = false;
    uint32 s_chestEntry = 0;
    uint32 s_chestDespawnSeconds = 600;
    uint32 s_dropChancePercent = 50;
    uint32 s_minZoneLevel = 20;
    uint32 s_rewardMultiplier = 2;
    std::unordered_set<uint32> s_botAccountIds;

    std::shared_mutex s_optInLock;
    std::unordered_set<uint32> s_optInGuids; // low guids of opted-in players

    void LoadHardcoreConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.Hardcore.Enable", false);
        s_chestEntry = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestGameObjectId", 0)));
        s_chestDespawnSeconds = uint32(std::max(30, sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestDespawnSeconds", 600)));
        s_dropChancePercent = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.DropChancePercent", 50), 0, 100));
        s_minZoneLevel = uint32(std::max(1, sConfigMgr->GetIntDefault("Centurion.Hardcore.FfaPvp.MinZoneLevel", 20)));
        s_rewardMultiplier = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Hardcore.FfaPvp.RewardMultiplier", 2), 1, 10));

        s_botAccountIds.clear();
        std::stringstream stream(sConfigMgr->GetStringDefault("Playerbot.RandomPopulation.BotAccountIds", ""));
        std::string token;
        while (std::getline(stream, token, ','))
            if (!token.empty())
                s_botAccountIds.insert(uint32(std::strtoul(token.c_str(), nullptr, 10)));
    }

    bool IsBotAccount(uint32 accountId)
    {
        return s_botAccountIds.contains(accountId);
    }

    bool IsOptedIn(uint32 guidLow)
    {
        std::shared_lock<std::shared_mutex> guard(s_optInLock);
        return s_optInGuids.contains(guidLow);
    }

    void SetOptedIn(uint32 guidLow, bool optedIn)
    {
        {
            std::unique_lock<std::shared_mutex> guard(s_optInLock);
            if (optedIn)
                s_optInGuids.insert(guidLow);
            else
                s_optInGuids.erase(guidLow);
        }
        if (optedIn)
            CharacterDatabase.PExecute("REPLACE INTO character_ffa_optin (guid) VALUES ({})", guidLow);
        else
            CharacterDatabase.PExecute("DELETE FROM character_ffa_optin WHERE guid = {}", guidLow);
    }

    // "The world": everything hardcore is inert inside battlegrounds/arenas.
    bool IsWorldContext(Player* player)
    {
        return s_enabled && player && !player->InBattleground() && !player->InArena();
    }

    // FFA arms only in real PvP-level zones - never in starter zones like
    // Durotar, never in capitals or sanctuaries.
    bool IsFfaEligibleZone(Player* player)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId());
        if (!zone)
            return false;

        if (zone->Flags & (AREA_FLAG_CAPITAL | AREA_FLAG_SANCTUARY))
            return false;

        return zone->ExplorationLevel >= int32(s_minZoneLevel);
    }

    bool IsFfaArmed(Player* player)
    {
        if (!IsWorldContext(player) || !IsFfaEligibleZone(player))
            return false;

        WorldSession const* session = player->GetSession();
        if (session && IsBotAccount(session->GetAccountId()))
            return true;

        return IsOptedIn(player->GetGUID().GetCounter());
    }

    void ApplyFfaState(Player* player)
    {
        if (!s_enabled || !player || !player->IsInWorld())
            return;

        bool const shouldFfa = IsFfaArmed(player);
        // Never clear a core-driven FFA state (arena zones own their flag).
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
        bool const coreFfaArea = area && (area->Flags & AREA_FLAG_ARENA);

        if (shouldFfa && !player->pvpInfo.IsInFFAPvPArea)
        {
            player->pvpInfo.IsInFFAPvPArea = true;
            player->UpdatePvPState(true);
            // The bot pseudo-faction render (Unit::BuildValuesUpdate) keys on
            // the FFA byte, but the faction FIELD never changes on its own -
            // push it back through the update pipe so watchers recolor.
            player->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
        }
        else if (!shouldFfa && player->pvpInfo.IsInFFAPvPArea && !coreFfaArea)
        {
            player->pvpInfo.IsInFFAPvPArea = false;
            player->UpdatePvPState(true);
            player->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
        }
    }

    // Full loot: everything worn and carried (contents before the bags that
    // hold them, so destruction order stays legal), plus the money pouch.
    void DropFullLootChest(Player* victim)
    {
        if (!s_enabled || !s_chestEntry || !victim || victim->IsGameMaster())
            return;

        if (!IsWorldContext(victim))
            return;

        CustomLootChests::PlayerChestBuilder chest(victim, s_chestEntry, Seconds(s_chestDespawnSeconds));
        std::vector<CustomLootChests::ItemLocation> droppedItems;

        // ONLY worn equipment is at stake: bags, their contents and money
        // are safe. Deflation: each worn piece leaves the corpse, but only
        // a configurable share reaches the chest - the rest simply burns.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            if (urand(0, 99) < s_dropChancePercent)
                chest.AddItem(item);
            droppedItems.push_back({ INVENTORY_SLOT_BAG_0, slot });
        }

        // Money is SAFE: gold never drops and never burns.
        if (droppedItems.empty())
            return;

        GameObject* spawnedChest = chest.Summon();
        if (!spawnedChest)
            return;

        for (CustomLootChests::ItemLocation const& dropped : droppedItems)
            victim->DestroyItem(dropped.Bag, dropped.Slot, true);

        TC_LOG_DEBUG("scripts", "BarracksHardcore: {} lost {} items at death ({}% reached the chest).",
            victim->GetName(), uint32(droppedItems.size()), s_dropChancePercent);
    }
}

using namespace BarracksHardcore;

class barracks_hardcore_player : public PlayerScript
{
public:
    barracks_hardcore_player() : PlayerScript("barracks_hardcore_player") {}

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (!s_enabled)
            return;

        uint32 const guidLow = player->GetGUID().GetCounter();
        if (QueryResult result = CharacterDatabase.PQuery("SELECT 1 FROM character_ffa_optin WHERE guid = {}", guidLow))
        {
            std::unique_lock<std::shared_mutex> guard(s_optInLock);
            s_optInGuids.insert(guidLow);
        }
        ApplyFfaState(player);
    }

    void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        ApplyFfaState(player);
    }

    void OnMapChanged(Player* player) override
    {
        ApplyFfaState(player);
    }

    void OnGiveXP(Player* player, uint32& amount, Unit* /*victim*/) override
    {
        if (!s_enabled || s_rewardMultiplier < 2)
            return;

        // The reward rides the RISK: only while the flag is actually armed,
        // and only for real players - bots level at the normal pace.
        WorldSession const* session = player->GetSession();
        if (session && IsBotAccount(session->GetAccountId()))
            return;

        if (IsFfaArmed(player))
            amount *= s_rewardMultiplier;
    }

    void OnMoneyChanged(Player* player, int32& amount) override
    {
        if (!s_enabled || s_rewardMultiplier < 2 || amount <= 0)
            return;

        // Loot pickups only: the loot window is open while money is looted,
        // which keeps vendor sales, mail and the auction house at face value.
        if (!player->GetLootGUID())
            return;

        WorldSession const* session = player->GetSession();
        if (session && IsBotAccount(session->GetAccountId()))
            return;

        if (IsFfaArmed(player))
            amount += amount * int32(s_rewardMultiplier - 1);
    }

    void OnPVPKill(Player* /*killer*/, Player* victim) override
    {
        DropFullLootChest(victim);
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* victim) override
    {
        DropFullLootChest(victim);
    }
};

// The town flagger: one gossip NPC entry, spawned in the capitals. Gossip
// lives on the CreatureAI in this core, not on CreatureScript.
class npc_ffa_flagger : public CreatureScript
{
public:
    npc_ffa_flagger() : CreatureScript("npc_ffa_flagger") {}

    struct npc_ffa_flaggerAI : public ScriptedAI
    {
        explicit npc_ffa_flaggerAI(Creature* creature) : ScriptedAI(creature) {}

        bool OnGossipHello(Player* player) override
        {
            if (!s_enabled)
            {
                SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, me->GetGUID());
                return true;
            }

            // Greenhorns see the offer but cannot take it: the select
            // handler turns them away until level 10.
            if (player->GetLevel() < 10)
            {
                me->Whisper("Come back when you have seen your tenth season. The mark is not for greenhorns.", LANG_UNIVERSAL, player);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Flag me for free-for-all PvP. (Requires level 10.)", GOSSIP_SENDER_MAIN, 1);
            }
            else if (IsOptedIn(player->GetGUID().GetCounter()))
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Remove my free-for-all flag.", GOSSIP_SENDER_MAIN, 2);
            else
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Flag me for free-for-all PvP. (Double experience and gold while armed!)", GOSSIP_SENDER_MAIN, 1);
            // A second option is load-bearing: the 3.3.5 client auto-selects
            // a one-option gossip menu, so right-clicking Grix toggled the
            // flag instantly with no window ever shown.
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Farewell.", GOSSIP_SENDER_MAIN, 3);
            // Custom flavor text row (bplusworld.npc_text 900001).
            SendGossipMenuFor(player, 900001, me->GetGUID());
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            CloseGossipMenuFor(player);
            if (!s_enabled)
                return true;
            if (player->GetLevel() < 10)
            {
                me->Whisper("Not yet, greenhorn. Come back at level 10.", LANG_UNIVERSAL, player);
                return true;
            }

            uint32 const guidLow = player->GetGUID().GetCounter();
            if (action == 1)
            {
                SetOptedIn(guidLow, true);
                me->Whisper("You are marked for free-for-all combat. The flag arms outside the safe zones - fight well, and profit doubly.", LANG_UNIVERSAL, player);
            }
            else
            {
                SetOptedIn(guidLow, false);
                me->Whisper("Your mark is lifted. The wilds are merely dangerous again.", LANG_UNIVERSAL, player);
            }
            ApplyFfaState(player);
            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_ffa_flaggerAI(creature);
    }
};

class barracks_hardcore_world : public WorldScript
{
public:
    barracks_hardcore_world() : WorldScript("barracks_hardcore_world") {}

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadHardcoreConfig();
        if (s_enabled)
            CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS character_ffa_optin (guid INT UNSIGNED NOT NULL, PRIMARY KEY (guid)) ENGINE=InnoDB");
    }
};

void AddSC_custom_barracks_hardcore()
{
    LoadHardcoreConfig();
    new barracks_hardcore_player();
    new npc_ffa_flagger();
    new barracks_hardcore_world();
}
