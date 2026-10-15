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

#include "Miscellaneous/GoodieBags.h"
#include "Miscellaneous/BattlegroundSpoils.h"
#include "Miscellaneous/TournamentMode.h"
#include "Config.h"
#include "Containers.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Loot.h"
#include "LootItemStorage.h"
#include "LootMgr.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringConvert.h"
#include "Util.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr std::size_t MAX_FINAL_BOSSES = 32;

    // How many boss loot rolls are tried for a piece the character can use
    // before settling for any piece the rolls turned up.
    constexpr uint32 PIECE_ROLL_ATTEMPTS = 30;

    // A fixed array rather than a vector, like the mark config: map threads
    // read the list while a `.reload config` may be rewriting it.
    struct GoodieBagConfig
    {
        bool Enable = false;
        uint32 MinLevel = 60;
        uint32 DungeonBag = 3746;
        uint32 RaidBag = 22152;
        bool SkipGreyBosses = true;
        std::array<uint32, MAX_FINAL_BOSSES> FinalBosses = { };
        std::size_t FinalBossCount = 0;
    };

    GoodieBagConfig Config;

    // creature_loot_template ids of each instance's bosses, keyed by
    // MAKE_PAIR32(map, difficulty). Built once at startup and only read after.
    std::unordered_map<uint32, std::vector<uint32>> Pools;

    // On a harder difficulty the creature that spawns is the template's
    // difficulty entry, and its loot is that entry's.
    CreatureTemplate const* ForDifficulty(CreatureTemplate const* cInfo, uint8 difficulty)
    {
        if (cInfo && difficulty > 0 && cInfo->DifficultyEntry[difficulty - 1])
            if (CreatureTemplate const* diffInfo = sObjectMgr->GetCreatureTemplate(cInfo->DifficultyEntry[difficulty - 1]))
                return diffInfo;
        return cInfo;
    }

    // Worn or wielded gear, green to epic. Legendaries are left to the boss
    // that drops them, and free-for-all items are not one player's to hold.
    bool IsPiece(ItemTemplate const* proto)
    {
        return proto && (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
            && proto->Quality >= ITEM_QUALITY_UNCOMMON && proto->Quality <= ITEM_QUALITY_EPIC
            && !proto->HasFlag(ITEM_FLAG_MULTI_DROP);
    }

    // One more drop from a random boss of the instance, kept to the gear in
    // it - so a 1% weapon stays as rare in the bag as off the boss. Rolled
    // until a drop holds something the character wears or wields (the
    // battleground spoils' fit rules); failing that, any piece seen.
    ItemTemplate const* RollPiece(std::vector<uint32> const& lootIds, Player const* player)
    {
        std::vector<ItemTemplate const*> anyPiece;
        for (uint32 attempt = 0; attempt < PIECE_ROLL_ATTEMPTS; ++attempt)
        {
            LootTemplate const* tab = LootTemplates_Creature.GetLootFor(Trinity::Containers::SelectRandomContainerElement(lootIds));
            if (!tab)
                continue;

            // Nobody owns this loot, so Loot::AddItem only lists the items.
            Loot drop;
            tab->Process(drop, LootTemplates_Creature.IsRatesAllowed(), LOOT_MODE_DEFAULT);

            std::vector<ItemTemplate const*> fits;
            for (LootItem const& item : drop.items)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
                if (!IsPiece(proto) || !item.AllowedForPlayer(player, ObjectGuid::Empty))
                    continue;

                // A unique piece the character already holds could never be
                // looted, and the bag would never empty.
                if (proto->MaxCount > 0 && player->GetItemCount(proto->ItemId, true) >= uint32(proto->MaxCount))
                    continue;

                anyPiece.push_back(proto);
                if (BattlegroundSpoils::SuitsCharacter(proto, player))
                    fits.push_back(proto);
            }

            if (!fits.empty())
                return Trinity::Containers::SelectRandomContainerElement(fits);
        }

        return anyPiece.empty() ? nullptr : Trinity::Containers::SelectRandomContainerElement(anyPiece);
    }

    bool IsEligible(Player const* player, uint8 bossLevel)
    {
        WorldSession* session = player->GetSession();
        if (!session || session->IsVirtualSession() || session->IsTransientPlayerSession())
            return false;

        if (Tournament::IsTournamentCharacter(player))
            return false;

        if (player->GetLevel() < Config.MinLevel)
            return false;

        // A level 60 clearing the Deadmines alone is not finishing a dungeon:
        // a boss grey to the character pays no bag, as it pays no experience.
        if (Config.SkipGreyBosses && bossLevel && bossLevel <= Trinity::XP::GetGrayLevel(player->GetLevel()))
            return false;

        return true;
    }

    void AwardBag(Player* player, InstanceMap const* instance, std::vector<uint32> const* lootIds)
    {
        uint32 const bagEntry = instance->IsRaid() ? Config.RaidBag : Config.DungeonBag;
        ItemTemplate const* bagProto = sObjectMgr->GetItemTemplate(bagEntry);
        if (!bagProto)
        {
            TC_LOG_ERROR("misc", "GoodieBags: bag item {} does not exist, {} gets nothing for finishing {} (map {})",
                bagEntry, player->GetName(), instance->GetMapName(), instance->GetId());
            return;
        }

        Item* bag = nullptr;
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, bagEntry, 1) == EQUIP_ERR_OK)
            bag = player->StoreNewItem(dest, bagEntry, true);

        bool const mailed = !bag;
        if (mailed)
            bag = Item::CreateItem(bagEntry, 1, player);
        if (!bag)
            return;

        // Opening the bag reads this back (Player::SendLoot -> LoadStoredLoot)
        // instead of rolling item_loot_template, which only knows the marks.
        Loot loot;
        loot.containerID = bag->GetGUID().GetCounter();
        loot.generateMoneyLoot(bagProto->MinMoneyLoot, bagProto->MaxMoneyLoot);
        loot.FillLoot(bagEntry, LootTemplates_Item, player, true, loot.gold != 0);
        if (lootIds && !lootIds->empty())
        {
            if (ItemTemplate const* piece = RollPiece(*lootIds, player))
                loot.AddItem(LootStoreItem(piece->ItemId, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));
            else
                TC_LOG_DEBUG("misc", "GoodieBags: no boss of {} (map {}, difficulty {}) dropped a piece for {}",
                    instance->GetMapName(), instance->GetId(), uint32(instance->GetDifficulty()), player->GetName());
        }
        sLootItemStorage->AddNewStoredLoot(&loot, player);

        if (!mailed)
        {
            player->SendNewItem(bag, 1, true, false);
            return;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        bag->SaveToDB(trans);
        MailDraft(bagProto->Name1, "Your bags were full when the last boss fell, so your goodie bag was sent here.")
            .AddItem(bag)
            .SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
        CharacterDatabase.CommitTransaction(trans);
    }
}

void GoodieBags::LoadConfig()
{
    GoodieBagConfig loaded;
    loaded.Enable = sConfigMgr->GetBoolDefault("Centurion.GoodieBag.Enable", false);
    loaded.MinLevel = uint32(std::max(1, sConfigMgr->GetIntDefault("Centurion.GoodieBag.MinLevel", 60)));
    loaded.DungeonBag = uint32(sConfigMgr->GetIntDefault("Centurion.GoodieBag.DungeonItem", 3746));
    loaded.RaidBag = uint32(sConfigMgr->GetIntDefault("Centurion.GoodieBag.RaidItem", 22152));
    loaded.SkipGreyBosses = sConfigMgr->GetBoolDefault("Centurion.GoodieBag.SkipGreyBosses", true);

    // "301000, 351019" or "301000 351019"
    std::string raw = sConfigMgr->GetStringDefault("Centurion.GoodieBag.FinalBosses", "");
    std::replace(raw.begin(), raw.end(), ',', ' ');
    for (std::string_view token : Trinity::Tokenize(raw, ' ', false))
    {
        Optional<uint32> const entry = Trinity::StringTo<uint32>(token);
        if (!entry || !*entry)
        {
            TC_LOG_ERROR("server.loading", "Centurion.GoodieBag.FinalBosses: ignoring '{}', expected a creature entry.", token);
            continue;
        }

        if (loaded.FinalBossCount == loaded.FinalBosses.size())
        {
            TC_LOG_ERROR("server.loading", "Centurion.GoodieBag.FinalBosses: more than {} entries, the rest are ignored.", loaded.FinalBosses.size());
            break;
        }
        loaded.FinalBosses[loaded.FinalBossCount++] = *entry;
    }

    Config = loaded;
}

void GoodieBags::LoadPools()
{
    Pools.clear();

    std::unordered_map<uint32, std::unordered_set<uint32>> lootIds;
    auto addBoss = [&lootIds](uint32 mapId, uint8 difficulty, CreatureTemplate const* cInfo)
    {
        cInfo = ForDifficulty(cInfo, difficulty);
        if (cInfo && cInfo->lootid && LootTemplates_Creature.HaveLootFor(cInfo->lootid))
            lootIds[MAKE_PAIR32(mapId, difficulty)].insert(cInfo->lootid);
    };

    // The bosses DungeonEncounter.dbc lists for the instance, which is every
    // boss of every stock dungeon and raid...
    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        MapEntry const* map = sMapStore.LookupEntry(i);
        if (!map || !map->IsDungeon())
            continue;

        for (uint8 difficulty = 0; difficulty < MAX_DIFFICULTY; ++difficulty)
            if (DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(map->ID, Difficulty(difficulty)))
                for (auto const& encounter : *encounters)
                    if (encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
                        addBoss(map->ID, difficulty, sObjectMgr->GetCreatureTemplate(encounter->creditEntry));
    }

    // ...plus every boss-ranked creature spawned there, which is how content
    // the DBC does not know (the 40-player raids on difficulty 2) gets a pool.
    for (auto const& spawn : sObjectMgr->GetAllCreatureData())
    {
        CreatureData const& data = spawn.second;
        MapEntry const* map = sMapStore.LookupEntry(data.mapId);
        if (!map || !map->IsDungeon())
            continue;

        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id);
        if (!cInfo || cInfo->rank != CREATURE_ELITE_WORLDBOSS)
            continue;

        for (uint8 difficulty = 0; difficulty < MAX_DIFFICULTY; ++difficulty)
            if (data.spawnMask & (1 << difficulty))
                addBoss(data.mapId, difficulty, cInfo);
    }

    uint32 tables = 0;
    for (auto const& pool : lootIds)
    {
        Pools[pool.first].assign(pool.second.begin(), pool.second.end());
        tables += uint32(pool.second.size());
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} goodie bag pools ({} boss loot tables).", uint32(Pools.size()), tables);
}

void GoodieBags::OnInstanceFinished(InstanceMap* instance, Unit const* source)
{
    if (!Config.Enable || !instance)
        return;

    auto const pool = Pools.find(MAKE_PAIR32(instance->GetId(), instance->GetDifficulty()));
    std::vector<uint32> const* lootIds = pool != Pools.end() ? &pool->second : nullptr;

    // A credit with no unit (a scripted event) has no level to judge.
    uint8 const bossLevel = source ? source->GetLevel() : 0;

    for (auto const& ref : instance->GetPlayers())
        if (Player* player = ref.GetSource())
            if (IsEligible(player, bossLevel))
                AwardBag(player, instance, lootIds);
}

void GoodieBags::OnCreatureKilled(Creature* victim)
{
    if (!Config.Enable || !victim || !Config.FinalBossCount)
        return;

    auto const end = Config.FinalBosses.begin() + Config.FinalBossCount;
    if (std::find(Config.FinalBosses.begin(), end, victim->GetEntry()) == end)
        return;

    if (InstanceMap* instance = victim->GetMap()->ToInstanceMap())
        OnInstanceFinished(instance, victim);
}
