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
#include "Creature.h"
#include "Duration.h"
#include "GameObject.h"
#include "Log.h"
#include "Loot.h"
#include "ObjectAccessor.h"
#include "Map.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "UpdateFields.h"
#include "Util.h"
#include "custom_loot_chest_helper.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

namespace DireMaulBeads
{
    static constexpr uint32 KalimdorMapId = 1;
    static constexpr uint32 DefaultOgreBeadItemId = 21982;
    static constexpr uint32 DefaultOgreBeadAuraId = 90002;
    static constexpr uint32 HonorTokenItemId = 100529;
    static constexpr char const* DefaultAreaIdList = "495,496,498,2557,3217";
    static constexpr uint32 DefaultChestGameObjectId = 0;
    static constexpr uint32 DefaultChestDespawnSeconds = 300;

    uint32 s_OgreBeadItemId = DefaultOgreBeadItemId;
    uint32 s_OgreBeadAuraId = DefaultOgreBeadAuraId;
    std::unordered_set<uint32> s_DireMaulAreaIds;
    uint32 s_BeadChestGameObjectId = DefaultChestGameObjectId;
    uint32 s_BeadChestDespawnSeconds = DefaultChestDespawnSeconds;

    namespace
    {
        uint32 GetMapAreaId(std::string_view token)
        {
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);

            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);

            if (token.empty())
                return 0;

            uint32 value = 0;
            std::from_chars_result const result = std::from_chars(token.data(), token.data() + token.size(), value);
            if (result.ec != std::errc())
                return 0;

            return value;
        }
    }

    void LoadDireMaulBeadConfig()
    {
        int32 const configuredItemId = sConfigMgr->GetIntDefault("DireMaulBeads.ItemId", static_cast<int32>(DefaultOgreBeadItemId));
        int32 const configuredAuraId = sConfigMgr->GetIntDefault("DireMaulBeads.AuraId", static_cast<int32>(DefaultOgreBeadAuraId));
        std::string const configuredAreaList = sConfigMgr->GetStringDefault("DireMaulBeads.AreaIds", DefaultAreaIdList);
        int32 const configuredChestId = sConfigMgr->GetIntDefault("DireMaulBeads.ChestGameObjectId", static_cast<int32>(DefaultChestGameObjectId));
        int32 const configuredChestDespawn = sConfigMgr->GetIntDefault("DireMaulBeads.ChestDespawnSeconds", static_cast<int32>(DefaultChestDespawnSeconds));

        s_OgreBeadItemId = configuredItemId > 0 ? static_cast<uint32>(configuredItemId) : 0u;
        s_OgreBeadAuraId = configuredAuraId > 0 ? static_cast<uint32>(configuredAuraId) : 0u;
        s_BeadChestGameObjectId = configuredChestId > 0 ? static_cast<uint32>(configuredChestId) : 0u;
        s_BeadChestDespawnSeconds = configuredChestDespawn >= 0 ? static_cast<uint32>(configuredChestDespawn) : DefaultChestDespawnSeconds;

        s_DireMaulAreaIds.clear();
        for (std::string_view token : Trinity::Tokenize(configuredAreaList, ',', false))
        {
            if (uint32 const value = GetMapAreaId(token))
                s_DireMaulAreaIds.insert(value);
        }

        if (!s_OgreBeadItemId || !s_OgreBeadAuraId)
            TC_LOG_DEBUG("scripts", "DireMaulBeads: feature disabled (item {}, aura {})", s_OgreBeadItemId, s_OgreBeadAuraId);
        else if (s_DireMaulAreaIds.empty())
            TC_LOG_DEBUG("scripts", "DireMaulBeads: no Dire Maul area IDs configured; beads will never trigger");

        if (s_OgreBeadItemId && s_OgreBeadAuraId && !s_BeadChestGameObjectId)
            TC_LOG_DEBUG("scripts", "DireMaulBeads: bead chest entry is 0; carriers will keep beads on death");
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

        if (player->GetMapId() != KalimdorMapId)
            return false;

        if (s_DireMaulAreaIds.empty())
            return false;

        uint32 const areaId = player->GetAreaId();
        uint32 const zoneId = player->GetZoneId();

        if (s_DireMaulAreaIds.contains(areaId) || s_DireMaulAreaIds.contains(zoneId))
            return true;

        return false;
    }

    void UpdateBeadAura(Player* player)
    {
        if (!player)
            return;

        uint32 const auraId = GetOgreBeadAuraId();
        if (!auraId)
            return;

        uint32 const mapId = player->GetMapId();
        uint32 const areaId = player->GetAreaId();
        uint32 const zoneId = player->GetZoneId();

        if (!IsInOutdoorDireMaul(player))
        {
            player->RemoveAura(auraId);
            TC_LOG_DEBUG("scripts", "DireMaulBeads: skipping aura {} for {} - outside configured area (map {}, zone {}, area {})",
                auraId, player->GetName(), mapId, zoneId, areaId);
            return;
        }

        uint32 const beadItemId = GetOgreBeadItemId();
        if (!beadItemId)
        {
            player->RemoveAura(auraId);
            TC_LOG_DEBUG("scripts", "DireMaulBeads: skipping aura {} for {} - bead item id is 0 (map {}, zone {}, area {})",
                auraId, player->GetName(), mapId, zoneId, areaId);
            return;
        }

        uint32 const beadCount = player->GetItemCount(beadItemId, false);
        if (!beadCount)
        {
            player->RemoveAura(auraId);
            TC_LOG_DEBUG("scripts", "DireMaulBeads: skipping aura {} for {} - no beads (map {}, zone {}, area {}, item {})",
                auraId, player->GetName(), mapId, zoneId, areaId, beadItemId);
            return;
        }

        uint8 const stacks = beadCount > 255 ? 255 : static_cast<uint8>(beadCount);

        Aura* aura = player->GetAura(auraId);
        if (aura)
            aura->SetStackAmount(stacks);
        else
            aura = player->AddAura(auraId, player);

        if (!aura)
        {
            TC_LOG_DEBUG("scripts", "DireMaulBeads: failed to apply aura {} for {} despite {} beads (map {}, zone {}, area {})",
                auraId, player->GetName(), beadCount, mapId, zoneId, areaId);
            return;
        }

        TC_LOG_DEBUG("scripts", "DireMaulBeads: applied aura {} ({} stacks) for {} (map {}, zone {}, area {})",
            auraId, stacks, player->GetName(), mapId, zoneId, areaId);
    }

    uint32 GetBeadChestGameObjectId()
    {
        return s_BeadChestGameObjectId;
    }

    Seconds GetChestDespawnDuration()
    {
        return Seconds(s_BeadChestDespawnSeconds);
    }

    void DropBeadChest(Player* victim)
    {
        if (!victim)
            return;

        if (!IsInOutdoorDireMaul(victim))
            return;

        uint32 const beadItemId = GetOgreBeadItemId();
        uint32 const chestEntry = GetBeadChestGameObjectId();
        if (!beadItemId || !chestEntry)
            return;

        uint32 const beadCount = victim->GetItemCount(beadItemId, false);
        if (!beadCount)
            return;

        CustomLootChests::PlayerChestBuilder chest(victim, chestEntry, GetChestDespawnDuration());
        uint32 const honorTokenCount = victim->GetItemCount(HonorTokenItemId, false);
        std::vector<CustomLootChests::ItemLocation> artifactItems;

        chest.AddStackableItem(beadItemId, beadCount);
        if (honorTokenCount)
            chest.AddStackableItem(HonorTokenItemId, honorTokenCount);

        std::unordered_set<uint32> excludedArtifacts = { HonorTokenItemId };
        if (beadItemId)
            excludedArtifacts.insert(beadItemId);

        CustomLootChests::CollectItemsWithQuality(victim, ITEM_QUALITY_ARTIFACT, chest, artifactItems, excludedArtifacts);

        if (GameObject* spawnedChest = chest.Summon())
        {
            victim->DestroyItemCount(beadItemId, beadCount, true);
            if (honorTokenCount)
                victim->DestroyItemCount(HonorTokenItemId, honorTokenCount, true);
            for (CustomLootChests::ItemLocation const& removed : artifactItems)
                victim->RemoveItem(removed.Bag, removed.Slot, true);
            UpdateBeadAura(victim);
        }
    }

    void OnItemStored(Player* player, uint32 itemId, uint32 count)
    {
        if (!player || !count)
            return;

        if (!GetOgreBeadItemId() || itemId != GetOgreBeadItemId())
            return;

        UpdateBeadAura(player);
    }

    void OnItemLooted(Player* player, uint32 itemId)
    {
        if (!player)
            return;

        if (!GetOgreBeadItemId() || itemId != GetOgreBeadItemId())
            return;

        UpdateBeadAura(player);
    }
}

using namespace DireMaulBeads;

class diremaul_beads_player : public PlayerScript
{
public:
    diremaul_beads_player() : PlayerScript("diremaul_beads_player") {}

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
        if (!victim)
            return;

        DropBeadChest(victim);

        if (killer && killer != victim)
            UpdateBeadAura(killer);
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* victim) override
    {
        DropBeadChest(victim);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        if (uint32 const auraId = GetOgreBeadAuraId())
            player->RemoveAura(auraId);
    }

    void OnPlayerRepop(Player* player) override
    {
        UpdateBeadAura(player);
    }
};

class diremaul_beads_world : public WorldScript
{
public:
    diremaul_beads_world() : WorldScript("diremaul_beads_world") {}

    void OnConfigLoad(bool /*reload*/) override
    {
        uint32 const previousAuraId = GetOgreBeadAuraId();
        LoadDireMaulBeadConfig();

        uint32 const auraId = GetOgreBeadAuraId();
        uint32 const itemId = GetOgreBeadItemId();
        uint32 const chestId = GetBeadChestGameObjectId();

        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
        for (auto const& playerEntry : players)
        {
            if (Player* player = playerEntry.second)
            {
                if (previousAuraId && previousAuraId != auraId)
                    player->RemoveAura(previousAuraId);

                if (auraId && itemId && chestId)
                    UpdateBeadAura(player);
                else if (auraId && (!itemId || !chestId))
                    player->RemoveAura(auraId);
            }
        }
    }
};

void AddSC_custom_diremaul_beads()
{
    DireMaulBeads::LoadDireMaulBeadConfig();
    new diremaul_beads_player();
    new diremaul_beads_world();
}
