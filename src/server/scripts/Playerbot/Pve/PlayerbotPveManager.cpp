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

#include "Playerbot/Pve/PlayerbotPveManager.h"

#include "Playerbot/Pvp/PlayerbotPvpClassActions.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "Playerbot/Pvp/PlayerbotSharedStateGuard.h"

#include "Bag.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Item.h"
#include "Log.h"
#include "Loot.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using PveClock = std::chrono::steady_clock;
using PveTimePoint = PveClock::time_point;

// The same free eat/drink pair the battleground preparation logic uses.
constexpr uint32 SPELL_PVE_OUT_OF_COMBAT_EAT = 29073;
constexpr uint32 SPELL_PVE_OUT_OF_COMBAT_DRINK = 22734;

constexpr std::chrono::milliseconds PveFastTickInterval(250);
constexpr std::chrono::milliseconds PveSlowTickInterval(750);
constexpr std::chrono::seconds PveGrindScanInterval(2);
constexpr uint32 PvePendingSummonTimeoutMs = 90 * 1000;
constexpr float PveCompanionTeleportCatchupDistance = 150.0f;
constexpr float PveRestBreakFollowDistance = 40.0f;

playerbot::PveConfig g_PveConfig;

enum class PveErrandKind : uint8
{
    None = 0,
    Vendor,
    QuestGiver
};

struct PveBotState
{
    ObjectGuid masterGuid;
    ObjectGuid orderedTargetGuid;
    bool passive = false;
    bool stay = false;
    bool engaged = false;
    PveTimePoint nextFastTick{};
    PveTimePoint nextSlowTick{};
    PveTimePoint nextGrindScanAt{};
    PveTimePoint nextWanderAt{};
    PveTimePoint deathObservedAt{};
    bool deathObserved = false;
    ObjectGuid pendingLootGuid;
    PveTimePoint pendingLootUntil{};
    ObjectGuid errandGuid;
    PveErrandKind errandKind = PveErrandKind::None;
    PveTimePoint errandUntil{};
    PveTimePoint nextErrandScanAt{};
    PveTimePoint nextEquipCheckAt{};
};

std::unordered_map<uint64, PveBotState> g_PveBotStateByGuid;

struct PendingSummon
{
    ObjectGuid summonerGuid;
    uint32 deadlineMs = 0;
    bool joinGroup = false;
};

// Written from map-update threads and command handlers, drained on the world
// thread so every group mutation and cross-map teleport happens there.
std::mutex g_PvePendingLock;
std::unordered_map<uint64, PendingSummon> g_PendingSummonsByBotGuid;
std::unordered_set<uint64> g_PendingGroupInviteAccepts;

bool IsHumanPlayer(Player const* player)
{
    if (!player)
        return false;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsVirtualSession() || session->IsTransientPlayerSession())
        return false;

    return !playerbot::IsManagedRandomBot(player);
}

bool HasRestAura(Player const* player)
{
    return player->HasAura(SPELL_PVE_OUT_OF_COMBAT_EAT) || player->HasAura(SPELL_PVE_OUT_OF_COMBAT_DRINK);
}

void RemoveRestAuras(Player* player)
{
    player->RemoveAurasDueToSpell(SPELL_PVE_OUT_OF_COMBAT_EAT);
    player->RemoveAurasDueToSpell(SPELL_PVE_OUT_OF_COMBAT_DRINK);
}

Unit* ResolveAttackableByGuid(Player* bot, ObjectGuid const& guid)
{
    if (guid.IsEmpty() || guid == bot->GetGUID())
        return nullptr;

    Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
    if (!unit || !unit->IsAlive() || !bot->IsValidAttackTarget(unit))
        return nullptr;

    // An evading creature resets to full health while immune; treating it as
    // a live target would chase-loop it forever.
    if (Creature const* creature = unit->ToCreature())
        if (creature->IsInEvadeMode())
            return nullptr;

    return unit;
}

void DisengagePveCombat(Player* bot, PveBotState& state)
{
    playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
    state.engaged = false;
    if (bot->GetVictim())
        bot->AttackStop();
    bot->SetTarget(ObjectGuid::Empty);
}

void QueuePendingSummon(ObjectGuid const& botGuid, ObjectGuid const& summonerGuid, bool joinGroup)
{
    std::lock_guard<std::mutex> guard(g_PvePendingLock);
    PendingSummon& pending = g_PendingSummonsByBotGuid[botGuid.GetRawValue()];
    pending.summonerGuid = summonerGuid;
    pending.deadlineMs = GameTime::GetGameTimeMS() + PvePendingSummonTimeoutMs;
    pending.joinGroup = joinGroup;
}

bool HasPendingSummon(ObjectGuid const& botGuid)
{
    std::lock_guard<std::mutex> guard(g_PvePendingLock);
    return g_PendingSummonsByBotGuid.find(botGuid.GetRawValue()) != g_PendingSummonsByBotGuid.end();
}

struct GrindTargetCheck
{
    Player* bot;
    playerbot::PveConfig const& cfg;

    bool operator()(Creature* creature) const
    {
        if (!creature->IsAlive() || creature->IsCivilian() || creature->IsTrigger() || creature->IsInEvadeMode())
            return false;

        // Rabbits and other critters are technically attackable but grinding
        // them is neither XP nor a convincing simulation.
        if (creature->GetCreatureType() == CREATURE_TYPE_CRITTER)
            return false;

        if (creature->IsPet() || creature->IsTotem() || creature->IsControlledByPlayer())
            return false;

        if (!cfg.grindAllowElites && (creature->isElite() || creature->isWorldBoss()))
            return false;

        int32 const levelDelta = int32(creature->GetLevel()) - int32(bot->GetLevel());
        if (levelDelta > int32(cfg.grindMaxLevelAbove) || -levelDelta > int32(cfg.grindMaxLevelBelow))
            return false;

        // Never steal a mob someone else is already fighting.
        if (creature->IsInCombat() && creature->GetVictim() != bot)
            return false;

        return bot->IsValidAttackTarget(creature);
    }
};

// Creature entries the bot still needs kill credit for. Grinding prefers
// these so an accepted kill quest completes as a side effect of leveling.
std::unordered_set<uint32> CollectWantedKillEntries(Player* bot)
{
    std::unordered_set<uint32> wanted;
    if (!playerbot::PveManager::GetConfig().questsEnabled)
        return wanted;

    for (auto const& [questId, status] : bot->getQuestStatusMap())
    {
        if (status.Status != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (uint8 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
            if (quest->RequiredNpcOrGo[index] > 0 && status.CreatureOrGOCount[index] < quest->RequiredNpcOrGoCount[index])
                wanted.insert(uint32(quest->RequiredNpcOrGo[index]));
    }

    return wanted;
}

Unit* PickGrindTarget(Player* bot, playerbot::PveConfig const& cfg)
{
    std::vector<Creature*> matches;
    GrindTargetCheck check{ bot, cfg };
    Trinity::CreatureListSearcher<GrindTargetCheck> searcher(bot, matches, check);
    Cell::VisitGridObjects(bot, searcher, cfg.grindSearchRadius);
    if (matches.empty())
        return nullptr;

    std::unordered_set<uint32> const wanted = CollectWantedKillEntries(bot);
    std::sort(matches.begin(), matches.end(), [bot, &wanted](Creature* left, Creature* right)
    {
        bool const leftWanted = wanted.count(left->GetEntry()) != 0;
        bool const rightWanted = wanted.count(right->GetEntry()) != 0;
        if (leftWanted != rightWanted)
            return leftWanted;
        return bot->GetDistance(left) < bot->GetDistance(right);
    });

    // Raycasts are the expensive part; only vet the closest handful.
    size_t const losProbeLimit = std::min<size_t>(matches.size(), 8);
    for (size_t index = 0; index < losProbeLimit; ++index)
        if (bot->IsWithinLOSInMap(matches[index]))
            return matches[index];

    return nullptr;
}

// When the engage-radius scan is empty, look further out and walk toward the
// nearest prospect instead of wandering blind. Racial start points sit in
// mob-free pockets (vendors, guards, triggers only), and an undirected random
// walk takes minutes to drift out of one.
Creature* FindGrindProspect(Player* bot, playerbot::PveConfig const& cfg)
{
    std::vector<Creature*> matches;
    GrindTargetCheck check{ bot, cfg };
    Trinity::CreatureListSearcher<GrindTargetCheck> searcher(bot, matches, check);
    Cell::VisitGridObjects(bot, searcher, cfg.grindSearchRadius * 3.0f);
    if (matches.empty())
        return nullptr;

    Creature* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (Creature* candidate : matches)
    {
        float const distance = bot->GetDistance(candidate);
        if (!nearest || distance < nearestDistance)
        {
            nearest = candidate;
            nearestDistance = distance;
        }
    }

    return nearest;
}

void MoveTowardThrottled(Player* bot, Position const& destination)
{
    if (bot->isMoving())
        return;

    playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
    bot->GetMotionMaster()->MovePoint(0, destination, true);
}

template<typename Fn>
void ForEachBagItem(Player* bot, Fn&& fn) // fn(Item*, bag, slot)
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            fn(item, INVENTORY_SLOT_BAG_0, slot);

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bot->GetItemByPos(bagSlot, uint8(slot)))
                    fn(item, bagSlot, uint8(slot));
}

uint32 CountFreeBagSlots(Player* bot)
{
    uint32 freeSlots = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++freeSlots;

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            freeSlots += bag->GetFreeSlots();

    return freeSlots;
}

bool AnyEquippedItemBelowDurabilityPct(Player* bot, uint32 thresholdPct)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (maxDurability && item->GetUInt32Value(ITEM_FIELD_DURABILITY) * 100 < maxDurability * thresholdPct)
            return true;
    }

    return false;
}

uint32 SellVendorJunk(Player* bot)
{
    uint32 soldCount = 0;
    ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Quality != ITEM_QUALITY_POOR || !proto->SellPrice)
            return;

        bot->ModifyMoney(int32(proto->SellPrice * item->GetCount()));
        bot->DestroyItem(bag, slot, true);
        ++soldCount;
    });
    return soldCount;
}

// The kill loop leaves the bot targeting its dead victim for one tick; grab
// the corpse for looting before the disengage path clears the target.
void DetectFreshKillForLoot(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    if (!cfg.lootEnabled || !state.pendingLootGuid.IsEmpty())
        return;

    ObjectGuid const targetGuid = bot->GetTarget();
    if (targetGuid.IsEmpty())
        return;

    Creature* corpse = ObjectAccessor::GetCreature(*bot, targetGuid);
    if (!corpse || corpse->IsAlive() || corpse->loot.isLooted())
        return;

    Player* recipient = corpse->GetLootRecipient();
    bool const lootIsOurs = recipient && (recipient == bot ||
        (bot->GetGroup() && recipient->GetGroup() == bot->GetGroup()));
    if (!lootIsOurs)
        return;

    state.pendingLootGuid = targetGuid;
    state.pendingLootUntil = PveClock::now() + std::chrono::seconds(20);
}

// Returns true while the bot is still busy walking to the corpse.
bool ProcessPendingLoot(Player* bot, PveBotState& state, playerbot::PveConfig const& /*cfg*/)
{
    if (state.pendingLootGuid.IsEmpty())
        return false;

    auto clearPending = [&]() { state.pendingLootGuid = ObjectGuid::Empty; };
    if (PveClock::now() > state.pendingLootUntil)
    {
        clearPending();
        return false;
    }

    Creature* corpse = ObjectAccessor::GetCreature(*bot, state.pendingLootGuid);
    if (!corpse || corpse->IsAlive() || corpse->loot.isLooted())
    {
        clearPending();
        return false;
    }

    if (!bot->IsWithinDistInMap(corpse, INTERACTION_DISTANCE))
    {
        MoveTowardThrottled(bot, corpse->GetPosition());
        return true;
    }

    bot->SendLoot(corpse->GetGUID(), LOOT_CORPSE);
    Loot* loot = &corpse->loot;
    if (loot->gold)
    {
        bot->ModifyMoney(int32(loot->gold));
        loot->gold = 0;
        loot->NotifyMoneyRemoved();
    }

    uint32 const maxSlot = loot->GetMaxSlotInLootFor(bot);
    for (uint8 slot = 0; slot < maxSlot; ++slot)
        bot->StoreLootItem(slot, loot);

    if (bot->GetLootGUID() == corpse->GetGUID())
        bot->GetSession()->DoLootRelease(corpse->GetGUID());

    clearPending();
    state.nextEquipCheckAt = PveClock::now();
    return false;
}

uint32 PickQuestRewardIndex(Player* bot, Quest const* quest)
{
    for (uint32 index = 0; index < quest->GetRewChoiceItemsCount(); ++index)
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[index]))
            if (bot->CanUseItem(proto) == EQUIP_ERR_OK)
                return index;

    return 0;
}

void AcceptAndTurnInQuestsAt(Player* bot, Creature* giver)
{
    std::vector<uint32> completedQuests;
    for (auto const& [questId, status] : bot->getQuestStatusMap())
        if (status.Status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
            completedQuests.push_back(questId);

    QuestRelationResult const involved = sObjectMgr->GetCreatureQuestInvolvedRelations(giver->GetEntry());
    for (uint32 questId : completedQuests)
    {
        if (!involved.HasQuest(questId))
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        uint32 const rewardIndex = PickQuestRewardIndex(bot, quest);
        if (bot->CanRewardQuest(quest, rewardIndex, false))
        {
            bot->RewardQuest(quest, rewardIndex, giver, false);
            TC_LOG_INFO("playerbots.pve", "Bot {} turned in quest {} ({}).",
                bot->GetName(), questId, quest->GetTitle());
        }
    }

    for (uint32 questId : sObjectMgr->GetCreatureQuestRelations(giver->GetEntry()))
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        if (bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
        {
            bot->AddQuestAndCheckCompletion(quest, giver);
            TC_LOG_INFO("playerbots.pve", "Bot {} accepted quest {} ({}).",
                bot->GetName(), questId, quest->GetTitle());
        }
    }
}

struct ErrandNpcCheck
{
    Player* bot;

    bool operator()(Creature* creature) const
    {
        return creature->IsAlive() && !creature->IsInEvadeMode() &&
            (creature->IsQuestGiver() || creature->IsVendor()) && creature->IsFriendlyTo(bot);
    }
};

void StartErrandIfNeeded(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    if (!cfg.questsEnabled && !cfg.vendorEnabled)
        return;

    std::vector<Creature*> serviceNpcs;
    ErrandNpcCheck check{ bot };
    Trinity::CreatureListSearcher<ErrandNpcCheck> searcher(bot, serviceNpcs, check);
    Cell::VisitGridObjects(bot, searcher, 200.0f);
    if (serviceNpcs.empty())
        return;

    std::sort(serviceNpcs.begin(), serviceNpcs.end(), [bot](Creature* left, Creature* right)
    {
        return bot->GetDistance(left) < bot->GetDistance(right);
    });

    std::vector<uint32> completedQuests;
    if (cfg.questsEnabled)
        for (auto const& [questId, status] : bot->getQuestStatusMap())
            if (status.Status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                completedQuests.push_back(questId);

    bool const needVendor = cfg.vendorEnabled &&
        (CountFreeBagSlots(bot) < 4 || AnyEquippedItemBelowDurabilityPct(bot, 35));

    auto beginErrand = [&](Creature* npc, PveErrandKind kind)
    {
        state.errandGuid = npc->GetGUID();
        state.errandKind = kind;
        state.errandUntil = PveClock::now() + std::chrono::seconds(90);
        TC_LOG_DEBUG("playerbots.pve", "Bot {} starting {} errand to {} at {:.0f}y.",
            bot->GetName(), kind == PveErrandKind::Vendor ? "vendor" : "quest", npc->GetName(), bot->GetDistance(npc));
    };

    for (Creature* npc : serviceNpcs)
    {
        if (cfg.questsEnabled && npc->IsQuestGiver())
        {
            QuestRelationResult const involved = sObjectMgr->GetCreatureQuestInvolvedRelations(npc->GetEntry());
            for (uint32 questId : completedQuests)
                if (involved.HasQuest(questId))
                    return beginErrand(npc, PveErrandKind::QuestGiver);

            for (uint32 questId : sObjectMgr->GetCreatureQuestRelations(npc->GetEntry()))
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                    if (bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                        return beginErrand(npc, PveErrandKind::QuestGiver);
        }

        if (needVendor && npc->IsVendor())
            return beginErrand(npc, PveErrandKind::Vendor);
    }
}

// Returns true while the bot is still busy walking to the errand target.
bool ProcessErrand(Player* bot, PveBotState& state, playerbot::PveConfig const& /*cfg*/)
{
    if (state.errandKind == PveErrandKind::None)
        return false;

    auto clearErrand = [&]()
    {
        state.errandGuid = ObjectGuid::Empty;
        state.errandKind = PveErrandKind::None;
    };

    // Errands belong to autonomous bots; a bot that just gained a master
    // drops its errand and follows.
    if (!state.masterGuid.IsEmpty() || PveClock::now() > state.errandUntil)
    {
        clearErrand();
        return false;
    }

    Creature* npc = ObjectAccessor::GetCreature(*bot, state.errandGuid);
    if (!npc || !npc->IsAlive())
    {
        clearErrand();
        return false;
    }

    if (!bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
    {
        MoveTowardThrottled(bot, npc->GetPosition());
        return true;
    }

    if (state.errandKind == PveErrandKind::Vendor)
    {
        uint32 const soldCount = SellVendorJunk(bot);
        if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
            bot->DurabilityRepairAll(true, 1.0f, false);
        TC_LOG_INFO("playerbots.pve", "Bot {} visited vendor {} (sold {} junk items).",
            bot->GetName(), npc->GetName(), soldCount);
    }
    else
        AcceptAndTurnInQuestsAt(bot, npc);

    clearErrand();
    return false;
}

void TryEquipUpgrades(Player* bot)
{
    std::vector<std::pair<uint8, uint8>> positions;
    ForEachBagItem(bot, [&](Item* /*item*/, uint8 bag, uint8 slot)
    {
        positions.emplace_back(bag, slot);
    });

    for (auto const& position : positions)
    {
        Item* item = bot->GetItemByPos(position.first, position.second);
        if (!item)
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
            continue;

        uint16 dest = 0;
        if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
            continue;

        if (Item* equipped = bot->GetItemByPos(dest))
        {
            ItemTemplate const* equippedProto = equipped->GetTemplate();
            if (equippedProto && equippedProto->ItemLevel >= proto->ItemLevel)
                continue;

            bot->SwapItem(item->GetPos(), dest);
        }
        else
        {
            bot->RemoveItem(position.first, position.second, true);
            bot->EquipItem(dest, item, true);
            bot->AutoUnequipOffhandIfNeed();
        }

        TC_LOG_INFO("playerbots.pve", "Bot {} equipped {} (item level {}).",
            bot->GetName(), proto->Name1, proto->ItemLevel);
    }
}

Unit* PickCompanionTarget(Player* bot, Player* master, playerbot::PveConfig const& cfg)
{
    Unit* best = nullptr;
    float bestDistance = 0.0f;
    auto consider = [&](Unit* candidate)
    {
        if (!candidate || !candidate->IsAlive() || !bot->IsValidAttackTarget(candidate))
            return;

        float const distance = bot->GetDistance(candidate);
        if (distance > cfg.companionAssistRadius)
            return;

        if (!best || distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    };

    for (Unit* attacker : bot->getAttackers())
        consider(attacker);

    if (master)
    {
        for (Unit* attacker : master->getAttackers())
            consider(attacker);

        // Assist the master's active fight, but never open on something the
        // master merely has selected.
        if (master->IsInCombat())
            consider(master->GetVictim());
    }

    if (Group* group = bot->GetGroup())
    {
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            if (slot.guid == bot->GetGUID() || (master && slot.guid == master->GetGUID()))
                continue;

            if (Player* member = ObjectAccessor::GetPlayer(*bot, slot.guid))
                for (Unit* attacker : member->getAttackers())
                    consider(attacker);
        }
    }

    return best;
}

void ExecuteEngagedCombatTick(Player* bot)
{
    // Outside battlegrounds the values snapshot is all-default; the class
    // context builder only consults it for battleground triggers.
    playerbot::PvpValues const values{};
    playerbot::PvpClassSpellContext const context = playerbot::PvpCore::BuildClassSpellContext(bot, values);
    bool const executed = playerbot::PvpClassActions::Execute(bot, context);

    Unit* victim = ResolveAttackableByGuid(bot, bot->GetTarget());
    if (!victim)
        return;

    // White-swing floor: a fresh low-level bot can know nothing castable, and
    // the decision graph then yields neither a spell nor a movement directive.
    // Plain chase plus auto-attack is what a player of that level does too.
    bool const engineActedThisTick = executed && context.shouldExecute;
    if (!engineActedThisTick)
    {
        if (bot->GetVictim() != victim)
            bot->Attack(victim, true);
        if (!bot->IsWithinMeleeRange(victim))
            playerbot::PvpClassActions::IssueFollowMovement(bot, victim, 1.0f);
    }
}

void TryJoinSummonerGroup(Player* summoner, Player* bot)
{
    if (bot->GetGroup())
    {
        if (bot->GetGroup() == summoner->GetGroup())
            return;

        bot->RemoveFromGroup();
    }

    Group* group = summoner->GetGroup();
    if (!group)
    {
        group = new Group;
        if (!group->Create(summoner))
        {
            delete group;
            return;
        }

        sGroupMgr->AddGroup(group);
    }

    if (group->IsFull())
    {
        bot->Whisper("Your group is full; following without a group slot.", LANG_UNIVERSAL, summoner);
        return;
    }

    if (group->AddMember(bot))
        group->BroadcastGroupUpdate();
}

void ProcessPendingSummons()
{
    std::unordered_map<uint64, PendingSummon> snapshot;
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        snapshot = g_PendingSummonsByBotGuid;
    }

    uint32 const nowMs = GameTime::GetGameTimeMS();
    for (auto const& pendingEntry : snapshot)
    {
        // Not a structured binding: the erasePending lambda below must capture
        // these, which C++17 forbids for bindings (clang enforces it).
        uint64 const botRawGuid = pendingEntry.first;
        PendingSummon const& pending = pendingEntry.second;
        ObjectGuid const botGuid(botRawGuid);
        auto erasePending = [&]()
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingSummonsByBotGuid.erase(botRawGuid);
        };

        Player* summoner = ObjectAccessor::FindConnectedPlayer(pending.summonerGuid);
        if (!summoner || !summoner->IsInWorld())
        {
            erasePending();
            continue;
        }

        Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
        if (!bot)
        {
            // Still materializing through the asynchronous login holder.
            if (nowMs > pending.deadlineMs)
                erasePending();
            continue;
        }

        if (!bot->IsInWorld() || bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            continue;

        if (bot->InBattleground())
        {
            erasePending();
            continue;
        }

        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }

        bool const sameMap = bot->GetMapId() == summoner->GetMapId();
        if (!sameMap || bot->GetDistance(summoner) > 40.0f)
        {
            if (nowMs > pending.deadlineMs)
            {
                erasePending();
                continue;
            }

            bot->TeleportTo(summoner->GetMapId(),
                summoner->GetPositionX() + frand(-2.5f, 2.5f),
                summoner->GetPositionY() + frand(-2.5f, 2.5f),
                summoner->GetPositionZ(), summoner->GetOrientation());
            continue;
        }

        if (pending.joinGroup)
            TryJoinSummonerGroup(summoner, bot);

        PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
        state.masterGuid = summoner->GetGUID();
        state.passive = false;
        state.stay = false;
        bot->Whisper("At your side.", LANG_UNIVERSAL, summoner);
        erasePending();
    }
}

void ProcessPendingGroupInviteAccepts()
{
    std::unordered_set<uint64> drained;
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        drained.swap(g_PendingGroupInviteAccepts);
    }

    for (uint64 botRawGuid : drained)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
        if (!bot || !bot->GetSession() || !bot->GetGroupInvite())
            continue;

        WorldPacket acceptPacket(CMSG_GROUP_ACCEPT, 4);
        acceptPacket << uint32(0);
        bot->GetSession()->HandleGroupAcceptOpcode(acceptPacket);
    }
}

void UpdateMasterFromGroup(Player* bot, PveBotState& state)
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        state.masterGuid = ObjectGuid::Empty;
        return;
    }

    ObjectGuid const leaderGuid = group->GetLeaderGUID();
    ObjectGuid firstHumanGuid;
    bool leaderIsHuman = false;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        Player const* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
        if (!IsHumanPlayer(member))
            continue;

        if (slot.guid == leaderGuid)
        {
            leaderIsHuman = true;
            break;
        }

        if (firstHumanGuid.IsEmpty())
            firstHumanGuid = slot.guid;
    }

    state.masterGuid = leaderIsHuman ? leaderGuid : firstHumanGuid;
}

void RunDeathRecovery(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    if (bot->IsAlive())
    {
        state.deathObserved = false;
        return;
    }

    if (state.engaged)
        DisengagePveCombat(bot, state);

    if (bot->IsResurrectRequested())
    {
        bot->ResurrectUsingRequestData();
        state.deathObserved = false;
        return;
    }

    PveTimePoint const now = PveClock::now();
    if (!state.deathObserved)
    {
        state.deathObserved = true;
        state.deathObservedAt = now;
        return;
    }

    if (now - state.deathObservedAt < std::chrono::seconds(cfg.autoReviveSeconds))
        return;

    if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        bot->BuildPlayerRepop();
        bot->RepopAtGraveyard();
        return;
    }

    bot->ResurrectPlayer(0.66f);
    bot->SpawnCorpseBones();
    state.deathObserved = false;

    // A revived companion whose master moved on catches up by teleport; the
    // world-update pass owns the actual move.
    if (!state.masterGuid.IsEmpty() && !HasPendingSummon(bot->GetGUID()))
    {
        Player const* masterSameMap = ObjectAccessor::GetPlayer(*bot, state.masterGuid);
        if (!masterSameMap || bot->GetDistance(masterSameMap) > PveCompanionTeleportCatchupDistance)
            QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
    }
}

void RunSlowTick(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    RunDeathRecovery(bot, state, cfg);
    if (!bot->IsAlive())
        return;

    if (bot->GetGroupInvite())
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        g_PendingGroupInviteAccepts.insert(bot->GetGUID().GetRawValue());
    }

    UpdateMasterFromGroup(bot, state);

    // Cross-map or extreme-distance master catch-up.
    if (!state.masterGuid.IsEmpty() && !state.engaged && !HasPendingSummon(bot->GetGUID()))
    {
        Player const* masterSameMap = ObjectAccessor::GetPlayer(*bot, state.masterGuid);
        if (!masterSameMap)
        {
            if (ObjectAccessor::FindConnectedPlayer(state.masterGuid))
                QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
        }
        else if (bot->GetDistance(masterSameMap) > PveCompanionTeleportCatchupDistance)
            QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
    }

    // Rest: sit down and use the free food/water once the fight is over.
    if (!state.engaged && !bot->IsInCombat() && !HasRestAura(bot))
    {
        bool const needFood = bot->GetHealthPct() < cfg.restHealthPct;
        bool const needDrink = bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < cfg.restManaPct;
        if (needFood || needDrink)
        {
            playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
            if (MotionMaster* motionMaster = bot->GetMotionMaster())
                motionMaster->Clear();
            bot->StopMoving();
            bot->CastSpell(bot, needFood ? SPELL_PVE_OUT_OF_COMBAT_EAT : SPELL_PVE_OUT_OF_COMBAT_DRINK, true);
        }
    }

    PveTimePoint const now = PveClock::now();

    // Quest/vendor errands are for autonomous bots; companions stay on their
    // master's heel.
    if (state.masterGuid.IsEmpty() && !state.engaged && !bot->IsInCombat() &&
        state.errandKind == PveErrandKind::None && state.pendingLootGuid.IsEmpty() &&
        now >= state.nextErrandScanAt)
    {
        state.nextErrandScanAt = now + std::chrono::seconds(15);
        StartErrandIfNeeded(bot, state, cfg);
    }

    if (cfg.equipUpgradesEnabled && !bot->IsInCombat() && now >= state.nextEquipCheckAt)
    {
        state.nextEquipCheckAt = now + std::chrono::seconds(15);
        TryEquipUpgrades(bot);
    }
}

void RunFastTick(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    Player* master = state.masterGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, state.masterGuid);

    // Must run before the disengage path clears the dead victim's guid.
    DetectFreshKillForLoot(bot, state, cfg);

    Unit* target = ResolveAttackableByGuid(bot, bot->GetTarget());
    if (!state.orderedTargetGuid.IsEmpty())
    {
        if (Unit* ordered = ResolveAttackableByGuid(bot, state.orderedTargetGuid))
            target = ordered;
        state.orderedTargetGuid = ObjectGuid::Empty;
    }

    if (state.passive)
    {
        if (state.engaged)
            DisengagePveCombat(bot, state);
        target = nullptr;
    }
    else if (!target)
    {
        if (master)
            target = PickCompanionTarget(bot, master, cfg);
        else if (cfg.grindEnabled && state.masterGuid.IsEmpty() && !HasRestAura(bot))
        {
            PveTimePoint const now = PveClock::now();
            if (now >= state.nextGrindScanAt)
            {
                state.nextGrindScanAt = now + PveGrindScanInterval;
                target = PickGrindTarget(bot, cfg);
            }
        }
    }

    if (target)
    {
        if (bot->GetTarget() != target->GetGUID())
            bot->SetTarget(target->GetGUID());
        if (!state.engaged)
        {
            playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), true);
            state.engaged = true;
            TC_LOG_INFO("playerbots.pve", "Bot {} engaging {} (level {}) at {:.0f}y.",
                bot->GetName(), target->GetName(), target->GetLevel(), bot->GetDistance(target));
        }

        ExecuteEngagedCombatTick(bot);
        return;
    }

    if (state.engaged)
        DisengagePveCombat(bot, state);

    if (ProcessPendingLoot(bot, state, cfg))
        return;

    if (HasRestAura(bot))
    {
        bool const stillRecovering = bot->GetHealthPct() < 99.0f ||
            (bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 99.0f);
        bool const masterLeftRestRange = master && bot->GetDistance(master) > PveRestBreakFollowDistance;
        if (stillRecovering && !masterLeftRestRange)
            return;

        RemoveRestAuras(bot);
    }

    if (ProcessErrand(bot, state, cfg))
        return;

    if (master && master->IsAlive() && !state.stay)
    {
        if (bot->GetDistance(master) > cfg.companionFollowDistance + 1.5f)
            playerbot::PvpClassActions::IssueFollowMovement(bot, master, cfg.companionFollowDistance);
        return;
    }

    if (cfg.grindEnabled && state.masterGuid.IsEmpty() && !state.stay)
    {
        PveTimePoint const now = PveClock::now();
        if (now >= state.nextWanderAt)
        {
            state.nextWanderAt = now + std::chrono::seconds(urand(12, 25));
            // A freshly logged-in player's MotionMaster is EMPTY and reports
            // MAX_MOTION_TYPE, not IDLE - treating only IDLE as "free to
            // wander" left spawned bots standing forever.
            MotionMaster* motionMaster = bot->GetMotionMaster();
            MovementGeneratorType const currentMovement = motionMaster
                ? motionMaster->GetCurrentMovementGeneratorType()
                : MAX_MOTION_TYPE;
            bool const movementIdle = currentMovement == IDLE_MOTION_TYPE || currentMovement == MAX_MOTION_TYPE;
            if (movementIdle && !bot->isMoving())
            {
                // Walk toward the nearest attackable creature when one exists
                // beyond engage range; random-wander only in a truly empty
                // area. The engage-radius scan takes over on arrival.
                if (Creature* prospect = FindGrindProspect(bot, cfg))
                {
                    playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                    bot->GetMotionMaster()->MovePoint(0, prospect->GetPosition(), true);
                    TC_LOG_DEBUG("playerbots.pve", "Grind bot {} walking toward prospect {} at {:.0f}y.",
                        bot->GetName(), prospect->GetName(), bot->GetDistance(prospect));
                }
                else
                {
                    Position const destination = bot->GetRandomNearPosition(cfg.grindWanderRadius);
                    playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                    bot->GetMotionMaster()->MovePoint(0, destination, true);
                }
            }
        }
    }
}
}

namespace playerbot
{
void PveManager::LoadConfig()
{
    g_PveConfig.enabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Enable", false);
    g_PveConfig.companionFollowDistance = sConfigMgr->GetFloatDefault("Playerbot.Pve.Companion.FollowDistance", 2.5f);
    g_PveConfig.companionAssistRadius = sConfigMgr->GetFloatDefault("Playerbot.Pve.Companion.AssistRadius", 45.0f);
    g_PveConfig.autoReviveSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.AutoReviveSeconds", 30), 5, 600));
    g_PveConfig.restHealthPct = sConfigMgr->GetFloatDefault("Playerbot.Pve.RestHealthPct", 60.0f);
    g_PveConfig.restManaPct = sConfigMgr->GetFloatDefault("Playerbot.Pve.RestManaPct", 50.0f);
    g_PveConfig.autoLearnSpellsOnLevelUp = sConfigMgr->GetBoolDefault("Playerbot.Pve.AutoLearnSpellsOnLevelUp", true);
    g_PveConfig.grindEnabled = sConfigMgr->GetBoolDefault("Playerbot.PveGrind.Enable", false);
    g_PveConfig.grindSearchRadius = sConfigMgr->GetFloatDefault("Playerbot.PveGrind.SearchRadius", 60.0f);
    g_PveConfig.grindWanderRadius = sConfigMgr->GetFloatDefault("Playerbot.PveGrind.WanderRadius", 40.0f);
    g_PveConfig.grindMaxLevelAbove = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.PveGrind.MaxLevelAbove", 3), 0, 10));
    g_PveConfig.grindMaxLevelBelow = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.PveGrind.MaxLevelBelow", 5), 0, 80));
    g_PveConfig.grindAllowElites = sConfigMgr->GetBoolDefault("Playerbot.PveGrind.AllowElites", false);
    g_PveConfig.lootEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Loot.Enable", true);
    g_PveConfig.vendorEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Vendor.Enable", true);
    g_PveConfig.questsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Quests.Enable", true);
    g_PveConfig.equipUpgradesEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.EquipUpgrades.Enable", true);
}

PveConfig const& PveManager::GetConfig()
{
    return g_PveConfig;
}

void PveManager::OnWorldUpdate(uint32 /*diffMs*/)
{
    if (!g_PveConfig.enabled)
        return;

    static uint32 lastPassMs = 0;
    uint32 const nowMs = GameTime::GetGameTimeMS();
    if (lastPassMs && nowMs < lastPassMs + 1000)
        return;
    lastPassMs = nowMs;

    ProcessPendingGroupInviteAccepts();
    ProcessPendingSummons();
}

void PveManager::OnPlayerLifecycleTick(Player* player)
{
    PveConfig const& cfg = g_PveConfig;
    if (!cfg.enabled || !player || !player->IsInWorld())
        return;

    if (!playerbot::IsManagedRandomBot(player))
        return;

    ObjectGuid const guid = player->GetGUID();
    uint64 const rawGuid = guid.GetRawValue();

    if (player->InBattleground() || player->duel)
    {
        PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);
        if (state.engaged)
            DisengagePveCombat(player, state);
        return;
    }

    if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
        return;

    PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);
    PveTimePoint const now = PveClock::now();
    if (state.nextFastTick == PveTimePoint{})
    {
        // Stagger first evaluation by GUID so a freshly loaded population does
        // not decide on the same world tick.
        state.nextFastTick = now + std::chrono::milliseconds(rawGuid % uint64(PveFastTickInterval.count()));
        state.nextSlowTick = state.nextFastTick;
        return;
    }

    if (now >= state.nextSlowTick)
    {
        state.nextSlowTick = now + PveSlowTickInterval;
        RunSlowTick(player, state, cfg);
    }

    if (!player->IsAlive())
        return;

    if (player->HasUnitState(UNIT_STATE_CONTROLLED))
        return;

    if (now < state.nextFastTick)
        return;
    state.nextFastTick = now + PveFastTickInterval;

    RunFastTick(player, state, cfg);
}

void PveManager::OnBotLogout(Player const* player)
{
    if (!player)
        return;

    uint64 const rawGuid = player->GetGUID().GetRawValue();
    PvpCore::SetPveCombatEngagement(player->GetGUID(), false);
    LockedErase(g_PveBotStateByGuid, rawGuid);

    std::lock_guard<std::mutex> guard(g_PvePendingLock);
    g_PendingSummonsByBotGuid.erase(rawGuid);
    g_PendingGroupInviteAccepts.erase(rawGuid);
}

bool PveManager::IsExemptFromBattlegroundOrchestration(Player const* player)
{
    if (!g_PveConfig.enabled || !player)
        return false;

    if (!playerbot::IsManagedRandomBot(player))
        return false;

    uint64 const rawGuid = player->GetGUID().GetRawValue();
    PveBotState stateCopy;
    if (LockedGetCopy(g_PveBotStateByGuid, rawGuid, stateCopy) && !stateCopy.masterGuid.IsEmpty())
        return true;

    return HasPendingSummon(player->GetGUID());
}

bool PveManager::HandleWhisperCommand(Player* sender, Player* botReceiver, std::string const& command)
{
    if (!g_PveConfig.enabled || !sender || !botReceiver)
        return false;

    if (!playerbot::IsManagedRandomBot(botReceiver))
        return false;

    uint64 const rawGuid = botReceiver->GetGUID().GetRawValue();
    PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);

    bool const senderIsMaster = state.masterGuid == sender->GetGUID();
    bool const senderIsGm = sender->GetSession() && sender->GetSession()->HasPermission(rbac::RBAC_PERM_COMMAND_GM);
    if (!senderIsMaster && !senderIsGm)
        return false;

    if (command == "follow")
    {
        state.stay = false;
        state.passive = false;
        botReceiver->Whisper("Following.", LANG_UNIVERSAL, sender);
        return true;
    }

    if (command == "stay")
    {
        state.stay = true;
        PvpClassActions::PrepareForExplicitMovement(botReceiver);
        if (MotionMaster* motionMaster = botReceiver->GetMotionMaster())
            motionMaster->Clear();
        botReceiver->StopMoving();
        botReceiver->Whisper("Holding position.", LANG_UNIVERSAL, sender);
        return true;
    }

    if (command == "passive")
    {
        state.passive = true;
        botReceiver->Whisper("Standing down.", LANG_UNIVERSAL, sender);
        return true;
    }

    if (command == "attack" || command == "assist")
    {
        state.passive = false;
        ObjectGuid const orderedGuid = sender->GetTarget();
        if (orderedGuid.IsEmpty())
        {
            botReceiver->Whisper("You have no target for me to attack.", LANG_UNIVERSAL, sender);
            return true;
        }

        state.orderedTargetGuid = orderedGuid;
        botReceiver->Whisper("Attacking your target.", LANG_UNIVERSAL, sender);
        return true;
    }

    if (command == "come")
    {
        QueuePendingSummon(botReceiver->GetGUID(), sender->GetGUID(), false);
        botReceiver->Whisper("On my way.", LANG_UNIVERSAL, sender);
        return true;
    }

    if (command == "dismiss")
    {
        std::string statusMessage;
        RequestCompanionDismiss(sender, botReceiver, statusMessage);
        return true;
    }

    if (command == "pve status")
    {
        botReceiver->Whisper(BuildStatusLine(botReceiver), LANG_UNIVERSAL, sender);
        return true;
    }

    return false;
}

bool PveManager::RequestCompanionSummon(Player* summoner, std::string const& characterName, std::string& statusMessage)
{
    if (!g_PveConfig.enabled)
    {
        statusMessage = "Playerbot PvE support is disabled (Playerbot.Pve.Enable).";
        return false;
    }

    if (!summoner)
        return false;

    ObjectGuid const characterGuid = sCharacterCache->GetCharacterGuidByName(characterName);
    if (characterGuid.IsEmpty())
    {
        statusMessage = "No character named '" + characterName + "' exists.";
        return false;
    }

    uint32 const accountId = sCharacterCache->GetCharacterAccountIdByGuid(characterGuid);
    std::vector<uint32> const botAccounts = RandomBotParticipationManager::GetConfiguredBotAccountIds();
    if (!std::binary_search(botAccounts.begin(), botAccounts.end(), accountId))
    {
        statusMessage = "'" + characterName + "' is not on a configured bot account.";
        return false;
    }

    if (!ObjectAccessor::FindConnectedPlayer(characterGuid) &&
        !RandomBotParticipationManager::RequestBotLoginByGuidLow(characterGuid.GetCounter()))
    {
        statusMessage = "Could not bring '" + characterName + "' online.";
        return false;
    }

    QueuePendingSummon(characterGuid, summoner->GetGUID(), true);
    statusMessage = "Summoning " + characterName + "; they will join your group shortly.";
    return true;
}

bool PveManager::RequestCompanionDismiss(Player* requester, Player* bot, std::string& statusMessage)
{
    if (!bot || !playerbot::IsManagedRandomBot(bot))
    {
        statusMessage = "That is not a managed playerbot.";
        return false;
    }

    uint64 const rawGuid = bot->GetGUID().GetRawValue();
    PveBotState stateCopy;
    LockedGetCopy(g_PveBotStateByGuid, rawGuid, stateCopy);

    bool const requesterIsMaster = requester && stateCopy.masterGuid == requester->GetGUID();
    bool const requesterIsGm = requester && requester->GetSession() &&
        requester->GetSession()->HasPermission(rbac::RBAC_PERM_COMMAND_GM);
    if (!requesterIsMaster && !requesterIsGm)
    {
        statusMessage = "Only the bot's master (or a GM) can dismiss it.";
        return false;
    }

    if (requester)
        bot->Whisper("Farewell.", LANG_UNIVERSAL, requester);

    if (bot->GetGroup())
        bot->RemoveFromGroup();

    OnBotLogout(bot);
    RandomBotParticipationManager::RequestManagedBotLogout(bot->GetGUID());
    statusMessage = "Dismissed " + bot->GetName() + ".";
    return true;
}

void PveManager::OnManagedBotLevelChanged(Player* player, uint8 /*oldLevel*/)
{
    if (!g_PveConfig.enabled || !g_PveConfig.autoLearnSpellsOnLevelUp)
        return;

    if (!player || !playerbot::IsManagedRandomBot(player))
        return;

    // Same loop as ".learn my trainer": keep taking every class-trainer spell
    // the bot now qualifies for until a full pass adds nothing, so rank chains
    // resolve within one level-up.
    std::vector<Trainer::Trainer const*> const& trainers = sObjectMgr->GetClassTrainers(player->GetClass());
    uint32 learned = 0;
    bool hadNew;
    do
    {
        hadNew = false;
        for (Trainer::Trainer const* trainer : trainers)
        {
            if (!trainer->IsTrainerValidForPlayer(player))
                continue;

            for (Trainer::Spell const& trainerSpell : trainer->GetSpells())
            {
                if (!trainer->CanTeachSpell(player, &trainerSpell))
                    continue;

                if (trainerSpell.IsCastable())
                    player->CastSpell(player, trainerSpell.SpellId, true);
                else
                    player->LearnSpell(trainerSpell.SpellId, false);

                ++learned;
                hadNew = true;
            }
        }
    } while (hadNew);

    if (learned)
        TC_LOG_DEBUG("playerbots.pve", "Managed bot {} learned {} trainer spells on reaching level {}.",
            player->GetGUID().ToString(), learned, player->GetLevel());
}

std::string PveManager::BuildStatusLine(Player const* bot)
{
    std::ostringstream stream;
    stream << "PvE: enabled=" << (g_PveConfig.enabled ? "yes" : "no");
    if (!bot)
        return stream.str();

    PveBotState stateCopy;
    LockedGetCopy(g_PveBotStateByGuid, bot->GetGUID().GetRawValue(), stateCopy);

    std::string masterName = "none";
    if (!stateCopy.masterGuid.IsEmpty())
        if (!sCharacterCache->GetCharacterNameByGuid(stateCopy.masterGuid, masterName))
            masterName = stateCopy.masterGuid.ToString();

    stream << " master=" << masterName
        << " mode=" << (!stateCopy.masterGuid.IsEmpty() ? "companion" : (g_PveConfig.grindEnabled ? "grind" : "idle"))
        << " passive=" << (stateCopy.passive ? "yes" : "no")
        << " stay=" << (stateCopy.stay ? "yes" : "no")
        << " engaged=" << (stateCopy.engaged ? "yes" : "no");
    return stream.str();
}
}
