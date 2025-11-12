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
#include "Configuration/Config.h"
#include "Corpse.h"
#include "Loot.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include <algorithm>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace
{
static constexpr uint32 DefaultOgreBeadItemId = 21982;
static constexpr uint32 DefaultOgreBeadAuraId = 90002;

uint32 s_OgreBeadItemId = DefaultOgreBeadItemId;
uint32 s_OgreBeadAuraId = DefaultOgreBeadAuraId;

void LoadDireMaulBeadConfig()
{
    int32 const configuredItemId = sConfigMgr->GetIntDefault("DireMaulBeads.ItemId", static_cast<int32>(DefaultOgreBeadItemId));
    int32 const configuredAuraId = sConfigMgr->GetIntDefault("DireMaulBeads.AuraId", static_cast<int32>(DefaultOgreBeadAuraId));

    s_OgreBeadItemId = configuredItemId > 0 ? static_cast<uint32>(configuredItemId) : 0u;
    s_OgreBeadAuraId = configuredAuraId > 0 ? static_cast<uint32>(configuredAuraId) : 0u;
}

uint32 GetOgreBeadItemId()
{
    return s_OgreBeadItemId;
}

uint32 GetOgreBeadAuraId()
{
    return s_OgreBeadAuraId;
}

bool IsInOutdoorDireMaul(Player* player)
{
    if (!player)
        return false;

    if (player->GetMapId() != 1)
        return false;

    switch (player->GetAreaId())
    {
        case 495: // Dire Maul
        case 496: // Broken Commons
        case 498: // The Maul
            return true;
        default:
            break;
    }

    return false;
}

void UpdateBeadAura(Player* player)
{
    if (!player)
        return;

    uint32 const auraId = GetOgreBeadAuraId();
    if (!auraId)
        return;

    if (!IsInOutdoorDireMaul(player))
    {
        player->RemoveAura(auraId);
        return;
    }

    uint32 const beadItemId = GetOgreBeadItemId();
    if (!beadItemId)
    {
        player->RemoveAura(auraId);
        return;
    }

    uint32 const beadCount = player->GetItemCount(beadItemId, false);
    if (!beadCount)
    {
        player->RemoveAura(auraId);
        return;
    }

    uint8 const stacks = beadCount > 255 ? 255 : static_cast<uint8>(beadCount);

    if (Aura* aura = player->GetAura(auraId))
        aura->SetStackAmount(stacks);
    else if (Aura* aura = player->AddAura(auraId, player))
        aura->SetStackAmount(stacks);
}

struct PendingBeadLoot
{
    uint32 BeadCount = 0;
};

std::unordered_map<ObjectGuid, PendingBeadLoot> s_PendingBeadLoot;

bool ApplyPendingBeadsToCorpse(Player* player, uint32 beadCount)
{
    if (!player || !beadCount)
        return false;

    uint32 const beadItemId = GetOgreBeadItemId();
    if (!beadItemId)
        return false;

    Corpse* corpse = player->GetCorpse();
    if (!corpse)
        return false;

    corpse->SetUInt32Value(CORPSE_FIELD_FLAGS, corpse->GetUInt32Value(CORPSE_FIELD_FLAGS) | CORPSE_FLAG_LOOTABLE);
    corpse->SetUInt32Value(CORPSE_FIELD_DYNAMIC_FLAGS, corpse->GetUInt32Value(CORPSE_FIELD_DYNAMIC_FLAGS) | CORPSE_DYNFLAG_LOOTABLE);
    corpse->lootRecipient = nullptr;

    Loot& loot = corpse->loot;
    loot.clear();
    loot.loot_type = LOOT_CORPSE;
    loot.lootOwnerGUID.Clear();
    loot.roundRobinPlayer.Clear();

    uint32 remaining = beadCount;
    while (remaining && loot.items.size() < MAX_NR_LOOT_ITEMS)
    {
        uint8 const stack = static_cast<uint8>(std::min<uint32>(remaining, 255));

        LootItem lootItem;
        lootItem.itemid = beadItemId;
        lootItem.count = stack;
        lootItem.itemIndex = static_cast<uint32>(loot.items.size());
        lootItem.freeforall = true;
        lootItem.follow_loot_rules = false;

        loot.items.push_back(lootItem);
        ++loot.unlootedCount;
        remaining -= stack;
    }

    return true;
}

bool TryApplyPendingBeads(Player* player)
{
    if (!player)
        return false;

    auto const pending = s_PendingBeadLoot.find(player->GetGUID());
    if (pending == s_PendingBeadLoot.end())
        return false;

    if (!ApplyPendingBeadsToCorpse(player, pending->second.BeadCount))
        return false;

    s_PendingBeadLoot.erase(pending);
    return true;
}
}

class diremaul_beads_player : public PlayerScript
{
public:
    diremaul_beads_player() : PlayerScript("diremaul_beads_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        UpdateBeadAura(player);
    }

    void OnMapChanged(Player* player) override
    {
        UpdateBeadAura(player);
    }

    void OnQuestObjectiveProgress(Player* player, Quest const* /*quest*/, uint32 /*objectiveIndex*/, uint16 /*progress*/) override
    {
        UpdateBeadAura(player);
    }

    void OnQuestStatusChange(Player* player, uint32 /*questId*/) override
    {
        UpdateBeadAura(player);
    }

    void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        UpdateBeadAura(player);
    }

    void OnPVPKill(Player* killer, Player* victim) override
    {
        if (!killer || !victim)
            return;

        if (killer == victim)
            return;

        uint32 const beadItemId = GetOgreBeadItemId();
        if (!beadItemId)
            return;

        if (!IsInOutdoorDireMaul(victim))
            return;

        uint32 const beadCount = victim->GetItemCount(beadItemId, false);
        if (!beadCount)
            return;

        victim->DestroyItemCount(beadItemId, beadCount, true);
        s_PendingBeadLoot[victim->GetGUID()] = { beadCount };

        TryApplyPendingBeads(victim);

        UpdateBeadAura(victim);
        UpdateBeadAura(killer);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        if (!TryApplyPendingBeads(player))
            s_PendingBeadLoot.erase(player->GetGUID());

        if (uint32 const auraId = GetOgreBeadAuraId())
            player->RemoveAura(auraId);
    }
};

class diremaul_beads_corpse : public PlayerScript
{
public:
    diremaul_beads_corpse() : PlayerScript("diremaul_beads_corpse") { }

    void OnPlayerRepop(Player* player) override
    {
        if (!player)
            return;

        TryApplyPendingBeads(player);
        UpdateBeadAura(player);
    }
};

class diremaul_beads_world : public WorldScript
{
public:
    diremaul_beads_world() : WorldScript("diremaul_beads_world") { }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (s_PendingBeadLoot.empty())
            return;

        std::vector<ObjectGuid> toRemove;
        toRemove.reserve(s_PendingBeadLoot.size());

        for (auto const& entry : s_PendingBeadLoot)
        {
            Player* player = ObjectAccessor::FindPlayer(entry.first);
            if (!player)
                continue;

            if (ApplyPendingBeadsToCorpse(player, entry.second.BeadCount))
                toRemove.push_back(entry.first);
        }

        for (ObjectGuid const& guid : toRemove)
            s_PendingBeadLoot.erase(guid);
    }

    void OnConfigLoad(bool /*reload*/) override
    {
        uint32 const previousAuraId = GetOgreBeadAuraId();
        uint32 const previousItemId = GetOgreBeadItemId();

        LoadDireMaulBeadConfig();

        uint32 const auraId = GetOgreBeadAuraId();
        uint32 const itemId = GetOgreBeadItemId();

        if (!itemId || itemId != previousItemId)
            s_PendingBeadLoot.clear();

        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
        for (auto const& playerEntry : players)
        {
            if (Player* player = playerEntry.second)
            {
                if (previousAuraId && previousAuraId != auraId)
                    player->RemoveAura(previousAuraId);

                if (!auraId || !itemId)
                    continue;

                UpdateBeadAura(player);
            }
        }
    }
};

void AddSC_custom_diremaul_beads()
{
    LoadDireMaulBeadConfig();
    new diremaul_beads_player();
    new diremaul_beads_corpse();
    new diremaul_beads_world();
}
