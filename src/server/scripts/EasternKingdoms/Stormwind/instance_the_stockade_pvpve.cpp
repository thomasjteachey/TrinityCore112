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

#include "Bag.h"
#include "Configuration/Config.h"
#include "Containers.h"
#include "Creature.h"
#include "Duration.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Loot.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Position.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include "Pet.h"

#include "../../Custom/custom_loot_chest_helper.h"
#include "../../Custom/mod_pvpve_dungeon.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DireMaulBeads
{
    uint32 GetOgreBeadItemId();
    void UpdateBeadAura(Player* player);
}

namespace StockadesPvPvE
{
    constexpr uint32 StockadesMapId = 34;
    constexpr uint32 StockadesExteriorMapId = 0;
    constexpr uint32 BossKeyItemId = 43650;
    constexpr uint32 HonorTokenItemId = 100529;
    constexpr uint32 HonorTokenKillBonus = 10;

    namespace
    {
        Position const kStockadesExteriorPosition = { -8779.9f, 834.349f, 94.6801f, 0.653013f };
        Position const kStockadesBossPosition = { 170.737518f, 0.004752f, -25.606199f, 3.168689f };
    }

    namespace
    {
        constexpr uint32 DefaultChestDespawnSeconds = 300;
        Position const   DefaultChestPosition = { 71.879f, -15.478f, -20.215f, 0.0f };
        constexpr uint32 DefaultDeathChestDespawnSeconds = 300;

        uint32   s_ChestGameObjectId = 0;
        Seconds  s_ChestDespawn = Seconds(DefaultChestDespawnSeconds);
        Position s_ChestPosition = DefaultChestPosition;
        uint32   s_BossCreatureEntry = 0;
        std::vector<uint32> s_BossEntries;
        uint32   s_DeathChestGameObjectId = 0;
        Seconds  s_DeathChestDespawn = Seconds(DefaultDeathChestDespawnSeconds);

        std::vector<uint32> s_ScarletDefenderEntries;
        std::vector<uint32> s_BigBadWolfEntries;
        std::vector<uint32> s_WrathboneSkeletonEntries;

        void LoadConfig()
        {
            int32 const configuredEntry = sConfigMgr->GetIntDefault("StockadesPvPvE.ChestGameObjectId", 0);
            int32 const configuredDespawn = sConfigMgr->GetIntDefault("StockadesPvPvE.ChestDespawnSeconds", int32(DefaultChestDespawnSeconds));
            float const configuredX = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnX", DefaultChestPosition.GetPositionX());
            float const configuredY = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnY", DefaultChestPosition.GetPositionY());
            float const configuredZ = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnZ", DefaultChestPosition.GetPositionZ());
            float const configuredO = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnO", DefaultChestPosition.GetOrientation());
            int32 const configuredBoss = sConfigMgr->GetIntDefault("StockadesPvPvE.BossCreatureEntry", 0);
            std::string const bossEntries = sConfigMgr->GetStringDefault("StockadesPvPvE.BossEntries", "");
            int32 const configuredDeathChest = sConfigMgr->GetIntDefault("StockadesPvPvE.DeathChestGameObjectId", 0);
            int32 const configuredDeathDespawn = sConfigMgr->GetIntDefault("StockadesPvPvE.DeathChestDespawnSeconds", int32(DefaultDeathChestDespawnSeconds));
            std::string const scarletEntries = sConfigMgr->GetStringDefault("StockadesPvPvE.ScarletDefenderEntries", "");
            std::string const wolfEntries = sConfigMgr->GetStringDefault("StockadesPvPvE.BigBadWolfEntries", "");
            std::string const skeletonEntries = sConfigMgr->GetStringDefault("StockadesPvPvE.WrathboneSkeletonEntries", "");

            s_ChestGameObjectId = configuredEntry > 0 ? uint32(configuredEntry) : 0u;
            auto const loadEntries = [](std::string const& rawList, std::vector<uint32>& destination, char const* label)
            {
                destination.clear();
                for (std::string_view token : Trinity::Tokenize(rawList, ',', false))
                {
                    uint32 value = 0;
                    if (!token.empty() && std::from_chars(token.data(), token.data() + token.size(), value).ec == std::errc())
                        destination.push_back(value);
                }

                if (destination.empty())
                    TC_LOG_WARN("server.custom", "Stockades PvPvE: no entries configured for {} key group; boss keys will not drop for that group.", label);
            };

            s_ChestDespawn = Seconds(configuredDespawn >= 0 ? uint32(configuredDespawn) : DefaultChestDespawnSeconds);
            s_ChestPosition.Relocate(configuredX, configuredY, configuredZ, configuredO);
            s_BossCreatureEntry = configuredBoss > 0 ? uint32(configuredBoss) : 0u;
            s_DeathChestGameObjectId = configuredDeathChest > 0 ? uint32(configuredDeathChest) : 0u;
            s_DeathChestDespawn = Seconds(configuredDeathDespawn >= 0 ? uint32(configuredDeathDespawn) : DefaultDeathChestDespawnSeconds);

            loadEntries(scarletEntries, s_ScarletDefenderEntries, "Scarlet Defender");
            loadEntries(wolfEntries, s_BigBadWolfEntries, "Big Bad Wolf");
            loadEntries(skeletonEntries, s_WrathboneSkeletonEntries, "Wrathbone Skeleton");
            loadEntries(bossEntries, s_BossEntries, "Boss");

            if (s_BossEntries.empty() && s_BossCreatureEntry)
                s_BossEntries.push_back(s_BossCreatureEntry);

            if (!s_ChestGameObjectId)
                TC_LOG_WARN("server.custom", "Stockades PvPvE: reward chest entry is 0; chest spawning is disabled.");

            if (!s_BossCreatureEntry && s_BossEntries.empty())
                TC_LOG_WARN("server.custom", "Stockades PvPvE: boss creature entries are not configured; boss defeat tracking is disabled.");

            if (!s_DeathChestGameObjectId)
                TC_LOG_WARN("server.custom", "Stockades PvPvE: death chest entry is 0; death loot chests are disabled.");
        }

        QuaternionData GetChestRotation()
        {
            return QuaternionData::fromEulerAnglesZYX(s_ChestPosition.GetOrientation(), 0.0f, 0.0f);
        }
    }

    void EnsureConfigLoaded()
    {
        static bool s_ConfigLoaded = false;
        if (s_ConfigLoaded)
            return;

        LoadConfig();
        s_ConfigLoaded = true;
    }

    uint32 GetChestGameObjectId()
    {
        return s_ChestGameObjectId;
    }

    uint32 GetBossCreatureEntry()
    {
        return s_BossCreatureEntry;
    }

    void SetBossCreatureEntry(uint32 entry)
    {
        s_BossCreatureEntry = entry;
    }

    std::vector<uint32> const& GetBossEntries()
    {
        return s_BossEntries;
    }

    Seconds GetChestDespawnTime()
    {
        return s_ChestDespawn;
    }

    Position const& GetChestPosition()
    {
        return s_ChestPosition;
    }

    QuaternionData GetChestQuaternion()
    {
        return GetChestRotation();
    }

    uint32 GetDeathChestGameObjectId()
    {
        return s_DeathChestGameObjectId;
    }

    Seconds GetDeathChestDespawnTime()
    {
        return s_DeathChestDespawn;
    }

    std::vector<uint32> const& GetScarletDefenderEntries()
    {
        return s_ScarletDefenderEntries;
    }

    std::vector<uint32> const& GetBigBadWolfEntries()
    {
        return s_BigBadWolfEntries;
    }

    std::vector<uint32> const& GetWrathboneSkeletonEntries()
    {
        return s_WrathboneSkeletonEntries;
    }
} // namespace StockadesPvPvE

namespace
{
    struct KeyDropGroup
    {
        KeyDropGroup(char const* name, std::vector<uint32> const& entries)
            : Label(name), EntrySet(entries.begin(), entries.end()) {
        }

        bool Matches(uint32 entry) const
        {
            return EntrySet.contains(entry);
        }

        void AddMember(Creature* creature)
        {
            if (!creature)
                return;

            Members.push_back(creature->GetGUID());
        }

        void RemoveMember(ObjectGuid const& guid)
        {
            Members.erase(std::remove(Members.begin(), Members.end(), guid), Members.end());
            if (KeyCarrier == guid)
                KeyCarrier.Clear();
        }

        void EnsureCarrier(Map* map)
        {
            if (KeyDropped || KeyCarrier || EntrySet.empty() || !map)
                return;

            std::vector<Creature*> available;
            available.reserve(Members.size());
            for (ObjectGuid const& guid : Members)
            {
                if (Creature* candidate = map->GetCreature(guid))
                {
                    if (candidate->IsAlive())
                        available.push_back(candidate);
                }
            }

            if (available.empty())
                return;

            KeyCarrier = Trinity::Containers::SelectRandomContainerElement(available)->GetGUID();
        }

        void AddKeyToLoot(Creature* creature) const
        {
            if (!creature)
                return;

            Loot& loot = creature->loot;
            LootItem lootItem;
            lootItem.itemid = StockadesPvPvE::BossKeyItemId;
            lootItem.itemIndex = static_cast<uint32>(loot.items.size());
            lootItem.count = 1;
            lootItem.freeforall = false;
            lootItem.follow_loot_rules = false;

            loot.items.push_back(lootItem);
            ++loot.unlootedCount;
        }

        void HandleDeath(Creature* creature)
        {
            if (!creature || KeyDropped)
                return;

            RemoveMember(creature->GetGUID());

            if (!KeyCarrier)
                KeyCarrier = creature->GetGUID();

            if (KeyCarrier == creature->GetGUID())
            {
                AddKeyToLoot(creature);
                KeyDropped = true;
                KeyCarrier.Clear();
            }
        }

        std::string const              Label;
        std::unordered_set<uint32> const EntrySet;
        std::vector<ObjectGuid>        Members;
        ObjectGuid                     KeyCarrier;
        bool                           KeyDropped = false;
    };

    void RemoveBossKeys(Player* player)
    {
        if (!player)
            return;

        uint32 const keyCount = player->GetItemCount(StockadesPvPvE::BossKeyItemId, false);
        if (keyCount)
            player->DestroyItemCount(StockadesPvPvE::BossKeyItemId, keyCount, true);
    }

    GuidSet s_RecentDeathChests;
    std::unordered_map<uint32, GuidSet> s_DeathBonusRecipientsByInstance;

    bool ShouldGrantDeathHonorBonus(Player* victim)
    {
        if (!victim)
            return false;

        uint32 const instanceId = victim->GetInstanceId();
        if (!instanceId)
            return false;

        GuidSet& recipients = s_DeathBonusRecipientsByInstance[instanceId];
        return recipients.insert(victim->GetGUID()).second;
    }

    void DropDeathChest(Player* victim)
    {
        if (!victim || victim->GetMapId() != StockadesPvPvE::StockadesMapId)
            return;

        ObjectGuid const victimGuid = victim->GetGUID();
        if (!victimGuid)
            return;

        if (!s_RecentDeathChests.insert(victimGuid).second)
            return;

        uint32 const chestEntry = StockadesPvPvE::GetDeathChestGameObjectId();
        if (!chestEntry)
            return;

        CustomLootChests::PlayerChestBuilder chest(victim, chestEntry, StockadesPvPvE::GetDeathChestDespawnTime());

        uint32 const beadItemId = DireMaulBeads::GetOgreBeadItemId();
        uint32 const beadCount = beadItemId ? victim->GetItemCount(beadItemId, false) : 0;
        uint32 const honorTokenCount = victim->GetItemCount(StockadesPvPvE::HonorTokenItemId, false);
        uint32 const bossKeyCount = victim->GetItemCount(StockadesPvPvE::BossKeyItemId, false);

        if (beadCount)
            chest.AddStackableItem(beadItemId, beadCount);

        uint32 const deathBonus = ShouldGrantDeathHonorBonus(victim) ? StockadesPvPvE::HonorTokenKillBonus : 0;
        chest.AddStackableItem(StockadesPvPvE::HonorTokenItemId, honorTokenCount + deathBonus);

        if (bossKeyCount)
            chest.AddStackableItem(StockadesPvPvE::BossKeyItemId, bossKeyCount);

        std::vector<CustomLootChests::ItemLocation> artifactItems;
        std::unordered_set<uint32> excludedArtifacts = { StockadesPvPvE::HonorTokenItemId };
        if (beadItemId)
            excludedArtifacts.insert(beadItemId);

        CustomLootChests::CollectItemsWithQuality(victim, ITEM_QUALITY_ARTIFACT, chest, artifactItems, excludedArtifacts);

        if (GameObject* chestGO = chest.Summon())
        {
            if (beadCount)
                victim->DestroyItemCount(beadItemId, beadCount, true);

            if (honorTokenCount)
                victim->DestroyItemCount(StockadesPvPvE::HonorTokenItemId, honorTokenCount, true);

            if (bossKeyCount)
                victim->DestroyItemCount(StockadesPvPvE::BossKeyItemId, bossKeyCount, true);

            for (CustomLootChests::ItemLocation const& removed : artifactItems)
                victim->RemoveItem(removed.Bag, removed.Slot, true);

            if (beadCount)
                DireMaulBeads::UpdateBeadAura(victim);

            chestGO->SetLootRecipient(nullptr);
        }

        victim->m_Events.AddEventAtOffset([victimGuid]()
        {
            s_RecentDeathChests.erase(victimGuid);
        }, 1s);
    }

    // ---------------------------------------------------------------------------
    // Honor token helpers (bosses / minibosses -> team-wide honor on kill)
    // ---------------------------------------------------------------------------

    // Return how many honor tokens this creature entry should award per kill.
    uint32 GetHonorTokensForCreatureEntry(uint32 entry)
    {
        // Bosses: 12 tokens each kill
        switch (entry)
        {
        case 1716:
        case 25447:
        case 23970:
            return 12;
        default:
            break;
        }

        // Minibosses: 2 tokens each kill
        switch (entry)
        {
        case 4298:
        case 8:
        case 17521:
            return 2;
        default:
            break;
        }

        return 0;
    }

    // Find the killer's team inside its current run
    static PvpveTeam* FindTeamForPlayerInRun(PvpveDungeonRun* run, ObjectGuid const& playerGuid)
    {
        if (!run)
            return nullptr;

        for (uint64 teamId : run->Teams)
        {
            PvpveTeam* team = sPvpveDungeonMgr->GetTeam(teamId);
            if (!team)
                continue;

            for (ObjectGuid const& memberGuid : team->Members)
            {
                if (memberGuid == playerGuid)
                    return team;
            }
        }

        return nullptr;
    }

    // Give `amount` honor tokens to every active member of killer?s PvPvE team.
    void AwardHonorTokensToKillerTeam(Player* killer, uint32 amount)
    {
        if (!killer || !amount)
            return;

        // If somehow not in a PvPvE run, just reward the killer.
        if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(killer))
        {
            killer->AddItem(StockadesPvPvE::HonorTokenItemId, amount);
            return;
        }

        PvpveDungeonRun* run = sPvpveDungeonMgr->GetRunForPlayer(killer->GetGUID());
        if (!run)
        {
            killer->AddItem(StockadesPvPvE::HonorTokenItemId, amount);
            return;
        }

        PvpveTeam* team = FindTeamForPlayerInRun(run, killer->GetGUID());
        if (!team)
        {
            killer->AddItem(StockadesPvPvE::HonorTokenItemId, amount);
            return;
        }

        for (ObjectGuid const& guid : team->Members)
        {
            Player* member = ObjectAccessor::FindPlayer(guid);
            if (!member)
                continue;

            // Only reward players still participating in the run and in Stockades.
            if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(member))
                continue;

            if (member->GetMapId() != StockadesPvPvE::StockadesMapId)
                continue;

            member->AddItem(StockadesPvPvE::HonorTokenItemId, amount);
        }
    }

    // Central handler for PvE honor on kill
    void HandleStockadesPveHonorKill(Player* killer, Creature* killed)
    {
        if (!killer || !killed)
            return;

        if (killed->GetMapId() != StockadesPvPvE::StockadesMapId)
            return;

        uint32 const tokens = GetHonorTokensForCreatureEntry(killed->GetEntry());
        if (!tokens)
            return; // this mob doesn't award tokens

        AwardHonorTokensToKillerTeam(killer, tokens);

        // Also treat boss kills as the Stockades completion trigger so teleport
        // spells unlock even if the instance death hook misses the GUID match.
        auto const& bossEntries = StockadesPvPvE::GetBossEntries();
        bool const killedBoss = killed->GetEntry() == StockadesPvPvE::GetBossCreatureEntry() ||
            std::find(bossEntries.begin(), bossEntries.end(), killed->GetEntry()) != bossEntries.end();

        if (!killedBoss)
            return;

        PvpveDungeonRun* run = sPvpveDungeonMgr->GetRunForPlayer(killer->GetGUID());
        if (!run || run->Finished || run->BossDefeated)
            return;

        DungeonTemplate const* dungeonTemplate = sPvpveDungeonMgr->GetDungeonTemplate(run->TemplateId);
        if (!dungeonTemplate || dungeonTemplate->MapId != StockadesPvPvE::StockadesMapId)
            return;

        sPvpveDungeonMgr->OnBossDefeated(run->Id, killer->GetGUID());
    }
} // anonymous namespace

class instance_the_stockade_pvpve : public InstanceMapScript
{
public:
    instance_the_stockade_pvpve()
        : InstanceMapScript("instance_the_stockade_pvpve", StockadesPvPvE::StockadesMapId) {
    }

    InstanceScript* GetInstanceScript(InstanceMap* map) const override
    {
        return new instance_the_stockade_pvpve_InstanceMapScript(map);
    }

    struct instance_the_stockade_pvpve_InstanceMapScript : public InstanceScript, public PvpveDungeonInstance
    {
        instance_the_stockade_pvpve_InstanceMapScript(InstanceMap* map)
            : InstanceScript(map),
            _scarletDefenders("Scarlet Defenders", StockadesPvPvE::GetScarletDefenderEntries()),
            _bigBadWolves("Big Bad Wolves", StockadesPvPvE::GetBigBadWolfEntries()),
            _wrathboneSkeletons("Wrathbone Skeletons", StockadesPvPvE::GetWrathboneSkeletonEntries())
        {
        }

        void OnPlayerEnter(Player* player) override
        {
            InstanceScript::OnPlayerEnter(player);

            if (!player)
                return;

            if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
            {
                if (!player->IsGameMaster())
                {
                    if (WorldSession* session = player->GetSession())
                        session->SendNotification("The Stockades are only accessible through the PvPvE queue.");

                    player->TeleportTo(StockadesPvPvE::StockadesExteriorMapId,
                        StockadesPvPvE::kStockadesExteriorPosition.GetPositionX(),
                        StockadesPvPvE::kStockadesExteriorPosition.GetPositionY(),
                        StockadesPvPvE::kStockadesExteriorPosition.GetPositionZ(),
                        StockadesPvPvE::kStockadesExteriorPosition.GetOrientation());
                }

                return;
            }

            if (PvpveDungeonRun* run = PvpveDungeonMgr::instance()->GetRunForPlayer(player->GetGUID()))
            {
                _pvpveRunId = run->Id;
                PvpveDungeonMgr::instance()->OnInstanceCreated(run->TemplateId, run->Id, player->GetInstanceId());
            }

            player->RemoveArenaSpellCooldowns(true);

            SpawnRandomBoss();

            NotifyOpposingPlayersOfInvasion(player);

            ApplyPvpveFfaState(player);
            sPvpveDungeonMgr->OnPlayerEnteredInstance(player, this);
        }

        void OnPlayerLeave(Player* player) override
        {
            InstanceScript::OnPlayerLeave(player);

            if (!player)
                return;

            ClearPvpveFfaState(player);

            if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
                return;

            RemoveBossKeys(player);
        }

        void OnPvpveRunFinished(uint32 runId, PvpveTeam const& winningTeam) override
        {
            AnnounceVictory(runId, winningTeam);
            SummonRewardChest(runId, winningTeam);
            ClearFfaState(winningTeam);
            _pvpveRunId = 0;
        }

        void OnUnitDeath(Unit* unit) override
        {
            InstanceScript::OnUnitDeath(unit);

            if (!_pvpveRunId)
                return;

            if (!unit)
                return;

            Creature* creature = unit->ToCreature();
            if (!creature)
                return;

            if (KeyDropGroup* group = GetKeyDropGroup(creature->GetEntry()))
                group->HandleDeath(creature);

            uint32 const honorTokens = GetHonorTokensForCreatureEntry(creature->GetEntry());
            auto const& bossEntries = StockadesPvPvE::GetBossEntries();
            bool const isStockadesBoss = creature->GetGUID() == _bossGuid ||
                std::find(bossEntries.begin(), bossEntries.end(), creature->GetEntry()) != bossEntries.end();

            if (isStockadesBoss)
            {
                DoSendNotifyToInstance("The Stockades boss has been defeated!");

                ObjectGuid creditGuid = ObjectGuid::Empty;
                if (Player* killer = creature->GetLootRecipient())
                    creditGuid = killer->GetGUID();

                sPvpveDungeonMgr->OnBossDefeated(_pvpveRunId, creditGuid);
            }
        }

    private:
        void DespawnExistingBosses(std::vector<uint32> const& entries)
        {
            if (!instance)
                return;

            for (Map::CreatureBySpawnIdContainer::value_type const& pair : instance->GetCreatureBySpawnIdStore())
            {
                Creature* creature = pair.second;
                if (!creature)
                    continue;

                if (std::find(entries.begin(), entries.end(), creature->GetEntry()) != entries.end())
                    creature->DespawnOrUnsummon();
            }
        }

        void SpawnRandomBoss()
        {
            if (_bossSpawned)
                return;

            auto const& bossEntries = StockadesPvPvE::GetBossEntries();
            if (bossEntries.empty())
                return;

            _bossSpawned = true;

            DespawnExistingBosses(bossEntries);

            uint32 const entry = Trinity::Containers::SelectRandomContainerElement(bossEntries);
            if (Creature* boss = instance->SummonCreature(entry, StockadesPvPvE::kStockadesBossPosition))
            {
                StockadesPvPvE::SetBossCreatureEntry(entry);
                _bossGuid = boss->GetGUID();
            }
            else
            {
                TC_LOG_ERROR("server.custom", "Stockades PvPvE: failed to summon boss entry {} at Stockades PvPvE location.", entry);
            }
        }

        std::string CollectMemberNames(PvpveTeam const& team) const
        {
            std::string result;
            for (ObjectGuid const& guid : team.Members)
            {
                if (Player* player = ObjectAccessor::FindPlayer(guid))
                {
                    if (!result.empty())
                        result += ", ";
                    result += player->GetName();
                }
            }

            return result;
        }

        void SendServerMessageToPlayer(Player* player, std::string const& message) const
        {
            if (player)
                sWorld->SendServerMessage(SERVER_MSG_STRING, message, player);
        }

        void SendServerMessageToRelevantPlayers(std::string const& message) const
        {
            if (!instance)
                return;

            Map::PlayerList const& players = instance->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                SendServerMessageToPlayer(itr->GetSource(), message);
        }

        void AnnounceVictory(uint32 runId, PvpveTeam const& team)
        {
            std::string const memberNames = CollectMemberNames(team);
            std::string const victoryMessage = memberNames.empty()
                ? Trinity::StringFormat("Stockades PvPvE run {} complete! A team is victorious!", runId)
                : Trinity::StringFormat("Stockades PvPvE run {} complete! Victorious players: {}.", runId, memberNames);
            DoSendNotifyToInstance(victoryMessage.c_str());
            SendServerMessageToRelevantPlayers(victoryMessage);
        }

        Player* SelectSummoner(PvpveTeam const& team) const
        {
            for (ObjectGuid const& guid : team.Members)
            {
                if (Player* player = ObjectAccessor::FindPlayer(guid))
                {
                    if (player->GetMap() == static_cast<Map*>(instance))
                        return player;
                }
            }

            return nullptr;
        }

        void SummonRewardChest(uint32 runId, PvpveTeam const& team)
        {
            uint32 const chestEntry = StockadesPvPvE::GetChestGameObjectId();
            if (!chestEntry)
                return;

            if (_rewardChestRunId == runId)
                return;

            Player* summoner = SelectSummoner(team);
            if (!summoner)
            {
                TC_LOG_WARN("server.custom", "Stockades PvPvE: unable to find a summoner for the reward chest (run {}, team {}).", runId, team.Id);
                return;
            }

            if (GameObject* chest = summoner->SummonGameObject(chestEntry,
                StockadesPvPvE::GetChestPosition(),
                StockadesPvPvE::GetChestQuaternion(),
                StockadesPvPvE::GetChestDespawnTime(),
                GO_SUMMON_TIMED_DESPAWN))
            {
                _rewardChestRunId = runId;
                _rewardChestGuid = chest->GetGUID();
                TC_LOG_INFO("server.custom", "Stockades PvPvE: spawned reward chest {} for run {} (team {}).", chestEntry, runId, team.Id);
            }
            else
            {
                TC_LOG_WARN("server.custom", "Stockades PvPvE: failed to summon reward chest {} for run {} (team {}).", chestEntry, runId, team.Id);
            }
        }

        void ClearFfaState(PvpveTeam const& team)
        {
            for (ObjectGuid const& guid : team.Members)
            {
                if (Player* player = ObjectAccessor::FindPlayer(guid))
                    ClearPvpveFfaState(player);
            }
        }

        void NotifyOpposingPlayersOfInvasion(Player* invadingPlayer)
        {
            if (!invadingPlayer || !instance)
                return;

            Map::PlayerList const& players = instance->GetPlayers();
            if (players.isEmpty())
                return;

            Group const* invadingGroup = invadingPlayer->GetGroup();
            bool hasOpponents = false;

            // First pass: notify all *other* players not in invader's group,
            // and track if we actually found any opponents.
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            {
                Player* target = itr->GetSource();
                if (!target || target == invadingPlayer)
                    continue;

                if (invadingGroup && target->GetGroup() == invadingGroup)
                    continue;

                hasOpponents = true;

                if (WorldSession* session = target->GetSession())
                {
                    session->SendNotification("You sense an evil presence");
                    SendServerMessageToPlayer(target, "You sense an evil presence");
                }
            }

            // Only if we actually found opponents do we tell the invading player.
            if (hasOpponents)
            {
                if (WorldSession* invSession = invadingPlayer->GetSession())
                {
                    invSession->SendNotification("You sense an evil presence");
                    SendServerMessageToPlayer(invadingPlayer, "You sense an evil presence");
                }
            }
        }


        void OnCreatureCreate(Creature* creature) override
        {
            InstanceScript::OnCreatureCreate(creature);

            if (KeyDropGroup* group = GetKeyDropGroup(creature->GetEntry()))
            {
                group->AddMember(creature);
                group->EnsureCarrier(instance);
            }
        }

        void OnCreatureRemove(Creature* creature) override
        {
            if (KeyDropGroup* group = GetKeyDropGroup(creature->GetEntry()))
                group->RemoveMember(creature->GetGUID());

            InstanceScript::OnCreatureRemove(creature);
        }

        KeyDropGroup* GetKeyDropGroup(uint32 entry)
        {
            if (_scarletDefenders.Matches(entry))
                return &_scarletDefenders;

            if (_bigBadWolves.Matches(entry))
                return &_bigBadWolves;

            if (_wrathboneSkeletons.Matches(entry))
                return &_wrathboneSkeletons;

            return nullptr;
        }

        ObjectGuid   _rewardChestGuid;
        uint32       _rewardChestRunId = 0;
        uint64       _pvpveRunId = 0;
        KeyDropGroup _scarletDefenders;
        KeyDropGroup _bigBadWolves;
        KeyDropGroup _wrathboneSkeletons;
        bool         _bossSpawned = false;
        ObjectGuid   _bossGuid = ObjectGuid::Empty;
    };
};

class stockades_pvpve_player : public PlayerScript
{
public:
    stockades_pvpve_player() : PlayerScript("stockades_pvpve_player") {}

    // PvP deaths -> drop death chest
    void OnPVPKill(Player* /*killer*/, Player* victim) override
    {
        DropDeathChest(victim);
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* victim) override
    {
        DropDeathChest(victim);
    }

    // PvE kills from player
    void OnCreatureKill(Player* killer, Creature* killed) override
    {
        HandleStockadesPveHonorKill(killer, killed);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        if (player->GetMapId() != StockadesPvPvE::StockadesMapId)
            return;

        DropDeathChest(player);
    }
};

struct go_stockades_boss_doorAI : public GameObjectAI
{
    using GameObjectAI::GameObjectAI;

    bool OnReportUse(Player* player) override
    {
        if (player && player->GetMapId() == StockadesPvPvE::StockadesMapId)
            RemoveBossKeys(player);

        return false;
    }
};

class go_stockades_boss_door : public GameObjectScript
{
public:
    go_stockades_boss_door() : GameObjectScript("go_stockades_boss_door") {}

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new go_stockades_boss_doorAI(go);
    }
};

void AddSC_instance_the_stockade_pvpve()
{
    StockadesPvPvE::EnsureConfigLoaded();
    new instance_the_stockade_pvpve();
    new stockades_pvpve_player();
    new go_stockades_boss_door();
}
