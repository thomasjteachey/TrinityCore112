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
#include "Common.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Item.h"
#include "Log.h"
#include "Loot.h"
#include "MapManager.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "AuctionHouseBot/AuctionHouseBot.h"
#include "AuctionHouseMgr.h"
#include "Mail.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "Spell.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <tuple>
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
    QuestGiver,
    QuestObject
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
    PveTimePoint nextTalentCheckAt{};
    PveTimePoint nextCombatDiagAt{};
    // Errand targets (NPCs and quest objects) visited recently, whether or
    // not the visit achieved anything - stops ping-ponging between blocked
    // targets (locked chest, turn-in with full bags, vendor that can't fix
    // the trigger).
    std::unordered_map<uint64, PveTimePoint> recentErrandTargets;
    // Targets that dropped out of engagement while still alive (evading,
    // turned invalid): re-acquiring them immediately produces the visible
    // engage/AttackStop flap that drags the mob back and forth.
    std::unordered_map<uint64, PveTimePoint> recentBadTargets;
    bool initialKitDone = false;
    uint32 dryWanderCount = 0;
    PveTimePoint restingUntil{};
    PveTimePoint nextSupplyRunAt{};
    PveTimePoint nextMailCheckAt{};
    PveTimePoint nextAuctionShopAt{};
    PveTimePoint nextProfessionCheckAt{};
    uint32 engagedStallTicks = 0;
    float lastEngagedX = 0.0f;
    float lastEngagedY = 0.0f;
    // Walked journey (relocation/town run on foot). Fallback kind decides
    // what happens on timeout or stuck: the old teleport.
    // walkFallbackUntil: after a failed walk, executors skip the walk branch
    // so the retry actually reaches taxi/teleport instead of re-walking into
    // the same wall forever.
    PveTimePoint walkFallbackUntil{};
    bool journeyActive = false;
    uint8 journeyFallbackKind = 0; // 1 = grind relocation, 2 = supply run
    uint16 journeyMapId = 0;
    float journeyX = 0.0f;
    float journeyY = 0.0f;
    float journeyZ = 0.0f;
    PveTimePoint journeyUntil{};
    PveTimePoint nextJourneyStepAt{};
    PveTimePoint journeyProgressAt{};
    float journeyProgressX = 0.0f;
    float journeyProgressY = 0.0f;
};

bool IsRecentErrandTarget(PveBotState& state, ObjectGuid const& guid)
{
    auto itr = state.recentErrandTargets.find(guid.GetRawValue());
    if (itr == state.recentErrandTargets.end())
        return false;

    if (PveClock::now() >= itr->second)
    {
        state.recentErrandTargets.erase(itr);
        return false;
    }

    return true;
}

void MarkRecentErrandTarget(PveBotState& state, ObjectGuid const& guid)
{
    PveTimePoint const now = PveClock::now();
    if (state.recentErrandTargets.size() > 24)
    {
        for (auto itr = state.recentErrandTargets.begin(); itr != state.recentErrandTargets.end();)
            itr = now >= itr->second ? state.recentErrandTargets.erase(itr) : std::next(itr);
    }

    state.recentErrandTargets[guid.GetRawValue()] = now + std::chrono::minutes(2);
}

bool IsRecentBadTarget(PveBotState& state, ObjectGuid const& guid)
{
    auto itr = state.recentBadTargets.find(guid.GetRawValue());
    if (itr == state.recentBadTargets.end())
        return false;

    if (PveClock::now() >= itr->second)
    {
        state.recentBadTargets.erase(itr);
        return false;
    }

    return true;
}

void MarkRecentBadTarget(PveBotState& state, ObjectGuid const& guid)
{
    PveTimePoint const now = PveClock::now();
    if (state.recentBadTargets.size() > 24)
    {
        for (auto itr = state.recentBadTargets.begin(); itr != state.recentBadTargets.end();)
            itr = now >= itr->second ? state.recentBadTargets.erase(itr) : std::next(itr);
    }

    state.recentBadTargets[guid.GetRawValue()] = now + std::chrono::seconds(60);
}

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
std::unordered_set<uint64> g_PendingGrindRelocations;
// Bots that need a vendor with none in walking range: the world thread
// teleports them to the nearest vendor cluster (a town run), shopping and
// repairs happen through the normal errand, and the dry-scan relocation
// afterwards sends them back to a grind spot.
std::unordered_set<uint64> g_PendingSupplyRuns;
// Mail collection and auction shopping mutate world-thread-only structures
// (Player::m_mail, AuctionHouseObject) - executed from OnWorldUpdate.
std::unordered_set<uint64> g_PendingMailCollections;
std::unordered_set<uint64> g_PendingAuctionShopping;
// Loot EXECUTION must happen on the world thread: Player::SendLoot for a
// group-tagged kill (or a group-rules chest) mutates shared Group state
// (roll lists, looter guid) that the core only ever touches from
// PROCESS_THREADUNSAFE loot opcodes. The map-thread fast tick only walks the
// bot to the corpse and enqueues here.
std::unordered_map<uint64, ObjectGuid> g_PendingLootExecutions;

GameObject* FindNearestQuestGameObject(Player* bot, PveBotState& state, float radius);
void UseQuestGameObject(Player* bot, PveBotState& state, GameObject* go);
bool BotHasIncompleteQuest(Player* bot);
void ProcessPendingLootExecutions();
void GrantGatherSkillCredit(Player* bot, GameObject* go);
void TrySkinCorpse(Player* bot, Creature* corpse);
bool IsGatherableNodeFor(Player* bot, GameObject const* go, int32* outRequiredSkill);

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

// Below the rest thresholds a bot must not START anything new: without this
// gate the 250ms acquisition tick re-engages before the 750ms rest check can
// ever run, and a wounded bot beelines from corpse to corpse until it dies.
bool NeedsRecovery(Player const* player, playerbot::PveConfig const& cfg)
{
    return player->GetHealthPct() < cfg.restHealthPct ||
        (player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < cfg.restManaPct);
}

bool IsRestingNow(Player const* player, PveBotState const& state)
{
    if (HasRestAura(player))
        return true;

    // Consumable-based rest has no fixed aura ids to sniff; the state timer
    // set when the item was used stands in for it.
    return PveClock::now() < state.restingUntil;
}

// The spell category is the discriminating field, NOT the subclass: B+'s
// classic-imported item rows keep the old subclass 0 on basic food/water
// (Tough Jerky, Refreshing Spring Water), and a subclass==FOOD requirement
// makes every vendor staple invisible to both the buyer and the eater.
bool IsFoodTemplate(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
        proto->Spells[0].SpellCategory == SPELL_CATEGORY_FOOD;
}

bool IsDrinkTemplate(ItemTemplate const* proto)
{
    return proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
        proto->Spells[0].SpellCategory == SPELL_CATEGORY_DRINK;
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
    // Player::SetTarget is an EMPTY override ("does not apply to players") -
    // player selection only changes through SetSelection.
    bot->SetSelection(ObjectGuid::Empty);
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

// Training and target dummies are attackable and level-appropriate but
// effectively immortal: a bot that picks one attacks it until the end of
// time. The many variants (classic engineering dummies, test dummies, the
// wotlk trainer dummies) share no single template flag, but they all carry
// the name, and the scripted ones additionally sit perma-stunned.
bool IsTargetDummyCreature(Creature const* creature)
{
    if (creature->HasUnitFlag(UNIT_FLAG_STUNNED))
        return true;

    CreatureTemplate const* proto = creature->GetCreatureTemplate();
    return proto && proto->Name.find("Dummy") != std::string::npos;
}

// Zones no bot should ever grind or shop in even when their spawns form
// convincing clusters on an allowed map: this DB spawns the death knight
// intro copy on map 0 (Plaguelands: The Scarlet Enclave), and GM Island
// sits on map 1.
bool IsForbiddenGrindZone(uint32 zoneId)
{
    return zoneId == 4298 || zoneId == 876;
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

        if (IsTargetDummyCreature(creature))
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

Unit* PickGrindTarget(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    std::vector<Creature*> matches;
    GrindTargetCheck check{ bot, cfg };
    Trinity::CreatureListSearcher<GrindTargetCheck> searcher(bot, matches, check);
    Cell::VisitGridObjects(bot, searcher, cfg.grindSearchRadius);
    if (matches.empty())
        return nullptr;

    // Quest targets are preferred, but only on a distance leash: walking
    // past a closer mob's aggro radius to reach a farther quest mob pulls
    // both. A wanted mob wins only when it is at most 20y farther than the
    // nearest candidate.
    float nearestDistance = 0.0f;
    for (Creature* candidate : matches)
    {
        float const distance = bot->GetDistance(candidate);
        if (!nearestDistance || distance < nearestDistance)
            nearestDistance = distance;
    }

    std::unordered_set<uint32> const wanted = CollectWantedKillEntries(bot);
    float const questLeash = nearestDistance + 20.0f;
    std::sort(matches.begin(), matches.end(), [bot, &wanted, questLeash](Creature* left, Creature* right)
    {
        bool const leftWanted = wanted.count(left->GetEntry()) != 0 && bot->GetDistance(left) <= questLeash;
        bool const rightWanted = wanted.count(right->GetEntry()) != 0 && bot->GetDistance(right) <= questLeash;
        if (leftWanted != rightWanted)
            return leftWanted;
        return bot->GetDistance(left) < bot->GetDistance(right);
    });

    // Raycasts are the expensive part; only vet the closest handful.
    size_t losProbesLeft = 8;
    for (Creature* candidate : matches)
    {
        if (IsRecentBadTarget(state, candidate->GetGUID()))
            continue;

        if (!losProbesLeft--)
            break;

        if (bot->IsWithinLOSInMap(candidate))
            return candidate;
    }

    return nullptr;
}

// When the engage-radius scan is empty, look further out and walk toward the
// nearest prospect instead of wandering blind. Racial start points sit in
// mob-free pockets (vendors, guards, triggers only), and an undirected random
// walk takes minutes to drift out of one.
Creature* FindGrindProspect(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
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
        if (IsRecentBadTarget(state, candidate->GetGUID()))
            continue;

        float const distance = bot->GetDistance(candidate);
        if (!nearest || distance < nearestDistance)
        {
            nearest = candidate;
            nearestDistance = distance;
        }
    }

    return nearest;
}

// Begin a walked journey toward a destination on the bot's current map.
// Called from world-thread executors; the map-thread fast tick advances it.
void StartWalkedJourney(PveBotState& state, uint16 mapId, float x, float y, float z, uint8 fallbackKind, float distance)
{
    state.journeyActive = true;
    state.journeyFallbackKind = fallbackKind;
    state.journeyMapId = mapId;
    state.journeyX = x;
    state.journeyY = y;
    state.journeyZ = z;
    // Generous deadline: walking speed with detours plus fights on the way.
    state.journeyUntil = PveClock::now() + std::chrono::seconds(uint32(distance / 4.0f) + 120);
    state.nextJourneyStepAt = {};
    state.journeyProgressAt = PveClock::now();
    state.journeyProgressX = 0.0f;
    state.journeyProgressY = 0.0f;
}

void CancelJourneyWithFallback(Player* bot, PveBotState& state)
{
    uint8 const fallbackKind = state.journeyFallbackKind;
    state.journeyActive = false;
    state.journeyFallbackKind = 0;
    // The executors' walk branches honor this: without it the deterministic
    // nearest-destination pick re-walks into the same unreachable wall
    // forever and the teleport fallback is never reached.
    state.walkFallbackUntil = PveClock::now() + std::chrono::minutes(10);

    // The walk failed (timeout or stuck); the old teleport still delivers.
    std::lock_guard<std::mutex> guard(g_PvePendingLock);
    if (fallbackKind == 1)
        g_PendingGrindRelocations.insert(bot->GetGUID().GetRawValue());
    else if (fallbackKind == 2)
        g_PendingSupplyRuns.insert(bot->GetGUID().GetRawValue());
}

// Returns true while the journey owns this tick.
bool AdvanceWalkedJourney(Player* bot, PveBotState& state)
{
    if (!state.journeyActive)
        return false;

    PveTimePoint const now = PveClock::now();

    // A wounded traveler may sit and eat; the trek resumes after. Keep the
    // stuck detector quiet meanwhile.
    if (IsRestingNow(bot, state))
    {
        state.journeyProgressAt = now;
        return true;
    }
    if (bot->GetMapId() != state.journeyMapId || now > state.journeyUntil)
    {
        CancelJourneyWithFallback(bot, state);
        return false;
    }

    float const dx = bot->GetPositionX() - state.journeyX;
    float const dy = bot->GetPositionY() - state.journeyY;
    if (dx * dx + dy * dy < 20.0f * 20.0f)
    {
        state.journeyActive = false;
        state.journeyFallbackKind = 0;
        TC_LOG_INFO("playerbots.pve", "Bot {} finished its walked journey.", bot->GetName());
        return false;
    }

    // Stuck detection: no ground covered in 12s despite trying.
    float const progressDx = bot->GetPositionX() - state.journeyProgressX;
    float const progressDy = bot->GetPositionY() - state.journeyProgressY;
    if (progressDx * progressDx + progressDy * progressDy > 4.0f)
    {
        state.journeyProgressX = bot->GetPositionX();
        state.journeyProgressY = bot->GetPositionY();
        state.journeyProgressAt = now;
    }
    else if (now - state.journeyProgressAt > std::chrono::seconds(12))
    {
        CancelJourneyWithFallback(bot, state);
        return false;
    }

    if (now >= state.nextJourneyStepAt && !bot->isMoving())
    {
        state.nextJourneyStepAt = now + std::chrono::seconds(2);
        playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
        // MovePoint clamps to a partial mmap path toward a far destination;
        // re-issuing on arrival at each partial end walks the full distance.
        bot->GetMotionMaster()->MovePoint(0, Position(state.journeyX, state.journeyY, state.journeyZ), true);
    }

    return true;
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

Item* FindBestConsumable(Player* bot, bool drink)
{
    Item* best = nullptr;
    uint32 bestLevel = 0;
    ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (drink ? !IsDrinkTemplate(proto) : !IsFoodTemplate(proto))
            return;

        if (proto->RequiredLevel > bot->GetLevel())
            return;

        if (!best || proto->RequiredLevel >= bestLevel)
        {
            best = item;
            bestLevel = proto->RequiredLevel;
        }
    });
    return best;
}

uint32 CountConsumableUnits(Player* bot, bool drink)
{
    uint32 units = 0;
    ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (drink ? IsDrinkTemplate(proto) : IsFoodTemplate(proto))
            units += item->GetCount();
    });
    return units;
}

// Ammo the bot's ranged weapon feeds on, or 0 when none is needed.
uint32 RequiredAmmoSubclass(Player* bot)
{
    Item* ranged = bot->GetWeaponForAttack(RANGED_ATTACK, true);
    if (!ranged || !ranged->GetTemplate())
        return 0;

    switch (ranged->GetTemplate()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ITEM_SUBCLASS_ARROW;
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ITEM_SUBCLASS_BULLET;
        default:
            return 0;
    }
}

// Buy the best level-appropriate food (and water for mana users, and ammo
// for ranged users) this vendor sells. Money is checked by the purchase
// itself; a broke bot just fails quietly and grinds more coin.
void TryBuySupplies(Player* bot, Creature* vendor)
{
    VendorItemData const* vendorItems = vendor->GetVendorItems();
    if (!vendorItems)
        return;

    bool const wantsDrink = bot->GetMaxPower(POWER_MANA) > 0;
    uint32 const ammoSubclass = RequiredAmmoSubclass(bot);

    int32 bestFoodSlot = -1, bestDrinkSlot = -1, bestAmmoSlot = -1;
    ItemTemplate const* bestFood = nullptr;
    ItemTemplate const* bestDrink = nullptr;
    ItemTemplate const* bestAmmo = nullptr;
    for (uint8 slot = 0; slot < vendorItems->GetItemCount(); ++slot)
    {
        VendorItem const* vendorItem = vendorItems->GetItem(slot);
        if (!vendorItem || vendorItem->ExtendedCost)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
        if (!proto || proto->RequiredLevel > bot->GetLevel())
            continue;

        if (IsFoodTemplate(proto) && (!bestFood || proto->RequiredLevel >= bestFood->RequiredLevel))
        {
            bestFood = proto;
            bestFoodSlot = slot;
        }
        else if (wantsDrink && IsDrinkTemplate(proto) && (!bestDrink || proto->RequiredLevel >= bestDrink->RequiredLevel))
        {
            bestDrink = proto;
            bestDrinkSlot = slot;
        }
        else if (ammoSubclass && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == ammoSubclass &&
            (!bestAmmo || proto->RequiredLevel >= bestAmmo->RequiredLevel))
        {
            bestAmmo = proto;
            bestAmmoSlot = slot;
        }
    }

    auto buyUnits = [&](int32 slot, ItemTemplate const* proto, uint32 desiredUnits)
    {
        if (slot < 0 || !proto)
            return;

        uint32 const buyCount = std::max<uint32>(1, proto->BuyCount);
        uint32 units = std::max(buyCount, desiredUnits - (desiredUnits % buyCount));
        units = std::min<uint32>(units, 250);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), uint32(slot), proto->ItemId, uint8(units), NULL_BAG, NULL_SLOT);
    };

    if (bestFood && CountConsumableUnits(bot, false) < 10)
        buyUnits(bestFoodSlot, bestFood, 20);
    if (bestDrink && CountConsumableUnits(bot, true) < 10)
        buyUnits(bestDrinkSlot, bestDrink, 20);
    if (bestAmmo)
    {
        uint32 const currentAmmoId = bot->GetUInt32Value(PLAYER_AMMO_ID);
        if (!currentAmmoId || bot->GetItemCount(currentAmmoId) < 100)
        {
            buyUnits(bestAmmoSlot, bestAmmo, 200);
            if (bot->GetItemCount(bestAmmo->ItemId))
                bot->SetAmmo(bestAmmo->ItemId);
        }
    }
}

bool IsQuestRequiredItem(Player* bot, uint32 itemId)
{
    for (auto const& [questId, status] : bot->getQuestStatusMap())
    {
        if (status.Status != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (uint8 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
            if (quest->RequiredItemId[index] == itemId)
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
        if (!proto || !proto->SellPrice)
            return;

        // Greys always; gathered trade goods too (bots don't craft), unless
        // an active quest needs the item.
        bool sellable = proto->Quality == ITEM_QUALITY_POOR;
        if (!sellable && playerbot::PveManager::GetConfig().professionsEnabled &&
            proto->Class == ITEM_CLASS_TRADE_GOODS)
            sellable = !IsQuestRequiredItem(bot, proto->ItemId);

        if (!sellable)
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
        if (state.journeyActive)
            state.journeyProgressAt = PveClock::now();
        MoveTowardThrottled(bot, corpse->GetPosition());
        return true;
    }

    // In reach: hand the actual loot session to the world thread.
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        g_PendingLootExecutions[bot->GetGUID().GetRawValue()] = state.pendingLootGuid;
    }
    clearPending();
    state.nextEquipCheckAt = PveClock::now() + std::chrono::seconds(3);
    return false;
}

// World thread. Opens the loot session, takes gold (group-split when the
// kill was group-tagged), stores every slot and releases.
void ProcessPendingLootExecutions()
{
    std::unordered_map<uint64, ObjectGuid> drained;
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        drained.swap(g_PendingLootExecutions);
    }

    for (auto const& entry : drained)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(entry.first));
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            continue;

        ObjectGuid const lootGuid = entry.second;
        WorldObject* lootObject = nullptr;
        Loot* loot = nullptr;
        LootType lootType = LOOT_CORPSE;
        GameObject* lootGameObject = nullptr;
        Creature* lootCorpse = nullptr;
        if (lootGuid.IsGameObject())
        {
            lootGameObject = ObjectAccessor::GetGameObject(*bot, lootGuid);
            if (!lootGameObject || !lootGameObject->isSpawned())
                continue;
            lootObject = lootGameObject;
            loot = &lootGameObject->loot;
            lootType = LOOT_SKINNING;
        }
        else
        {
            lootCorpse = ObjectAccessor::GetCreature(*bot, lootGuid);
            if (!lootCorpse || lootCorpse->IsAlive() || lootCorpse->loot.isLooted())
                continue;
            lootObject = lootCorpse;
            loot = &lootCorpse->loot;
        }

        if (!bot->IsWithinDistInMap(lootObject, INTERACTION_DISTANCE + 2.0f))
            continue;

        bot->SendLoot(lootGuid, lootType);
        // SendLoot can refuse (permission, despawn race); it releases on its
        // own failure paths, so only proceed while the session is ours.
        if (bot->GetLootGUID() != lootGuid)
            continue;

        if (loot->gold)
        {
            // Mirror the loot-money handler's group split for shared kills.
            Group* group = bot->GetGroup();
            if (group)
            {
                std::vector<Player*> nearMembers;
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                    if (Player* member = ObjectAccessor::GetPlayer(*bot, slot.guid))
                        if (member->IsAtGroupRewardDistance(lootObject))
                            nearMembers.push_back(member);

                uint32 const share = std::max<uint32>(1, loot->gold / std::max<size_t>(1, nearMembers.size()));
                for (Player* member : nearMembers)
                    member->ModifyMoney(int32(share));
                if (nearMembers.empty())
                    bot->ModifyMoney(int32(loot->gold));
            }
            else
                bot->ModifyMoney(int32(loot->gold));

            loot->gold = 0;
            loot->NotifyMoneyRemoved();
        }

        uint32 const maxSlot = std::min<uint32>(loot->GetMaxSlotInLootFor(bot), 255);
        for (uint32 slot = 0; slot < maxSlot; ++slot)
            bot->StoreLootItem(uint8(slot), loot);

        if (bot->GetLootGUID() == lootGuid)
            bot->GetSession()->DoLootRelease(lootGuid);

        if (playerbot::PveManager::GetConfig().professionsEnabled)
        {
            // Gather nodes grant their skill-up; freshly emptied corpses
            // become skinnable and get skinned in the same visit.
            if (lootGameObject)
                GrantGatherSkillCredit(bot, lootGameObject);
            else if (lootCorpse)
                TrySkinCorpse(bot, lootCorpse);
        }
    }
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
        if (bot->CanRewardQuest(quest, false) && bot->CanRewardQuest(quest, rewardIndex, false))
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

void RequestSupplyRunIfDue(Player* bot, PveBotState& state)
{
    PveTimePoint const now = PveClock::now();
    if (now < state.nextSupplyRunAt)
        return;

    state.nextSupplyRunAt = now + std::chrono::minutes(5);
    std::lock_guard<std::mutex> guard(g_PvePendingLock);
    g_PendingSupplyRuns.insert(bot->GetGUID().GetRawValue());
}

void StartErrandIfNeeded(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    if (!cfg.questsEnabled && !cfg.vendorEnabled)
        return;

    // Gameobject objectives and gather nodes first: they sit inside the
    // grind area, so finishing them costs almost nothing.
    if ((cfg.questsEnabled && BotHasIncompleteQuest(bot)) || cfg.professionsEnabled)
    {
        if (GameObject* questGo = FindNearestQuestGameObject(bot, state, 120.0f))
        {
            state.errandGuid = questGo->GetGUID();
            state.errandKind = PveErrandKind::QuestObject;
            state.errandUntil = PveClock::now() + std::chrono::seconds(90);
            TC_LOG_DEBUG("playerbots.pve", "Bot {} starting quest-object errand to {} at {:.0f}y.",
                bot->GetName(), questGo->GetEntry(), bot->GetDistance(questGo));
            return;
        }
    }

    bool const needRepair = AnyEquippedItemBelowDurabilityPct(bot, 35);
    bool const needSupplies = cfg.restUseConsumables &&
        (CountConsumableUnits(bot, false) == 0 ||
            (bot->GetMaxPower(POWER_MANA) > 0 && CountConsumableUnits(bot, true) == 0) ||
            (RequiredAmmoSubclass(bot) &&
                (!bot->GetUInt32Value(PLAYER_AMMO_ID) || bot->GetItemCount(bot->GetUInt32Value(PLAYER_AMMO_ID)) == 0)));
    bool const needVendor = cfg.vendorEnabled &&
        (CountFreeBagSlots(bot) < 4 || needRepair || needSupplies);

    std::vector<Creature*> serviceNpcs;
    ErrandNpcCheck check{ bot };
    Trinity::CreatureListSearcher<ErrandNpcCheck> searcher(bot, serviceNpcs, check);
    Cell::VisitGridObjects(bot, searcher, 200.0f);
    if (serviceNpcs.empty())
    {
        // Nothing in walking range at all: a bot that needs a vendor gets a
        // town run instead of starving in the wilderness.
        if (needVendor)
            RequestSupplyRunIfDue(bot, state);
        return;
    }

    std::sort(serviceNpcs.begin(), serviceNpcs.end(), [bot](Creature* left, Creature* right)
    {
        return bot->GetDistance(left) < bot->GetDistance(right);
    });

    std::vector<uint32> completedQuests;
    if (cfg.questsEnabled)
        for (auto const& [questId, status] : bot->getQuestStatusMap())
            if (status.Status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                completedQuests.push_back(questId);

    // A worn-out bot needs a vendor that can actually repair, not just any
    // merchant; try those first.
    if (needVendor && needRepair)
        for (Creature* npc : serviceNpcs)
            if (!IsRecentErrandTarget(state, npc->GetGUID()) && npc->IsVendor() && npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
            {
                state.errandGuid = npc->GetGUID();
                state.errandKind = PveErrandKind::Vendor;
                state.errandUntil = PveClock::now() + std::chrono::seconds(90);
                return;
            }

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
        if (IsRecentErrandTarget(state, npc->GetGUID()))
            continue;

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

    // NPCs around, but no usable vendor among them (quest camp, cooldowns):
    // the vendor need still stands, so town-run it.
    if (needVendor)
        RequestSupplyRunIfDue(bot, state);
}

// Wilderness grind spots can be hundreds of yards from the nearest vendor;
// gear that hits zero durability counts as unequipped and silently disarms
// whole rotations (Hamstring, Shield Slam, white swings). When repair is
// CRITICAL and no vendor is in reach, pay for a field repair from the bot's
// own gold rather than let it fist-fight plague bears.
void MaybeFieldRepair(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    if (!cfg.vendorEnabled || state.errandKind != PveErrandKind::None)
        return;

    if (!AnyEquippedItemBelowDurabilityPct(bot, 10))
        return;

    bot->DurabilityRepairAll(true, 1.0f, false);
    TC_LOG_INFO("playerbots.pve", "Bot {} field-repaired its gear (no vendor within reach).", bot->GetName());
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

    if (state.errandKind == PveErrandKind::QuestObject)
    {
        GameObject* questGo = ObjectAccessor::GetGameObject(*bot, state.errandGuid);
        if (!questGo || !questGo->isSpawned())
        {
            clearErrand();
            return false;
        }

        if (!bot->IsWithinDistInMap(questGo, INTERACTION_DISTANCE))
        {
            MoveTowardThrottled(bot, questGo->GetPosition());
            return true;
        }

        UseQuestGameObject(bot, state, questGo);
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

    // Whatever the visit achieves, don't re-pick this NPC for a while - a
    // turn-in blocked by full bags or an unaffordable repair would otherwise
    // re-trigger the same errand every scan.
    MarkRecentErrandTarget(state, npc->GetGUID());

    if (state.errandKind == PveErrandKind::Vendor)
    {
        uint32 const soldCount = SellVendorJunk(bot);
        if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
            bot->DurabilityRepairAll(true, 1.0f, false);
        if (playerbot::PveManager::GetConfig().restUseConsumables)
            TryBuySupplies(bot, npc);
        TC_LOG_INFO("playerbots.pve", "Bot {} visited vendor {} (sold {} junk items).",
            bot->GetName(), npc->GetName(), soldCount);
    }
    else
        AcceptAndTurnInQuestsAt(bot, npc);

    clearErrand();
    return false;
}

// A two-hander upgrade once benched a prot warrior's shield through
// AutoUnequipOffhandIfNeed, permanently breaking Shield Slam. Undo that
// state where possible: two-hander in main hand, empty off hand, and both a
// one-hander and a shield sitting in the bags.
void TryRestoreShieldProfile(Player* bot)
{
    switch (bot->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_SHAMAN:
            break;
        default:
            return;
    }

    Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!mainHand || !mainHand->GetTemplate() || mainHand->GetTemplate()->InventoryType != INVTYPE_2HWEAPON)
        return;

    if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
        return;

    Item* baggedShield = nullptr;
    Item* baggedOneHander = nullptr;
    ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return;

        if (!baggedShield && proto->InventoryType == INVTYPE_SHIELD)
            baggedShield = item;
        if (!baggedOneHander && proto->Class == ITEM_CLASS_WEAPON &&
            (proto->InventoryType == INVTYPE_WEAPON || proto->InventoryType == INVTYPE_WEAPONMAINHAND))
            baggedOneHander = item;
    });

    if (!baggedShield || !baggedOneHander)
        return;

    uint16 const mainHandPos = uint16(INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND;
    bot->SwapItem(baggedOneHander->GetPos(), mainHandPos);

    uint16 dest = 0;
    if (bot->CanEquipItem(NULL_SLOT, dest, baggedShield, false) == EQUIP_ERR_OK)
    {
        uint8 const bag = baggedShield->GetBagSlot();
        uint8 const slot = baggedShield->GetSlot();
        bot->RemoveItem(bag, slot, true);
        bot->EquipItem(dest, baggedShield, true);
        TC_LOG_INFO("playerbots.pve", "Bot {} restored its one-hander + shield profile.", bot->GetName());
    }
}

void TryEquipUpgrades(Player* bot)
{
    TryRestoreShieldProfile(bot);

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

        // Never bench an equipped off hand (shield!) for a two-hander: the
        // "upgrade" silently disarms shield-dependent rotations.
        if (proto->InventoryType == INVTYPE_2HWEAPON &&
            bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
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

// ---------------------------------------------------------------------------
// Talent auto-spend
//
// The talent trees on this fork are CUSTOM (rebuilt from talent_lplus, ids
// renumbered on every regeneration), so no premade id lists can be trusted.
// Instead: pick the tab that contains the signature talent of the bot's
// assigned profile (same signature spells DetectClassicClassProfile keys on -
// spell ids are stable across talent rebuilds), then greedily fill that tab
// tier by tier. Player::LearnTalent validates class, prerequisites and row
// gates itself and silently no-ops, so the greedy loop is safe by
// construction.
// ---------------------------------------------------------------------------

// Signature talent SPELLS per class, Primary/Secondary/Tertiary - keep in
// sync with DetectClassicClassProfile in PlayerbotPvpCore.cpp.
std::array<uint32, 3> GetProfileSignatureSpells(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR: return { 12294, 81273, 23922 };
        case CLASS_PALADIN: return { 20473, 20925, 20375 };
        case CLASS_HUNTER:  return { 81300, 19506, 19386 };
        case CLASS_ROGUE:   return { 81302, 13750, 14185 };
        case CLASS_PRIEST:  return { 10060, 724, 15473 };
        case CLASS_SHAMAN:  return { 16166, 17364, 16188 };
        case CLASS_MAGE:    return { 12042, 33041, 11426 };
        case CLASS_WARLOCK: return { 48181, 19028, 17962 };
        case CLASS_DRUID:   return { 24858, 18562, 17007 };
        default:            return { 0, 0, 0 };
    }
}

std::vector<TalentEntry const*> const& GetSortedTabTalents(uint32 tabId)
{
    static std::mutex cacheLock;
    static std::unordered_map<uint32, std::vector<TalentEntry const*>> cacheByTab;

    std::lock_guard<std::mutex> guard(cacheLock);
    auto const [itr, inserted] = cacheByTab.try_emplace(tabId);
    if (inserted)
    {
        for (TalentEntry const* talent : sTalentStore)
            if (talent && talent->TabID == tabId)
                itr->second.push_back(talent);

        std::sort(itr->second.begin(), itr->second.end(), [](TalentEntry const* left, TalentEntry const* right)
        {
            if (left->TierID != right->TierID)
                return left->TierID < right->TierID;
            return left->ColumnIndex < right->ColumnIndex;
        });
    }

    return itr->second;
}

uint32 FindTalentTabContainingSpell(uint32 spellId)
{
    if (!spellId)
        return 0;

    for (TalentEntry const* talent : sTalentStore)
        if (talent)
            for (uint32 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                if (talent->SpellRank[rank] == spellId)
                    return talent->TabID;

    return 0;
}

std::vector<uint32> GetClassTalentTabs(Player const* bot)
{
    std::vector<uint32> tabs;
    for (TalentTabEntry const* tab : sTalentTabStore)
        if (tab && (tab->ClassMask & bot->GetClassMask()) && !tab->PetTalentMask)
            tabs.push_back(tab->ID);

    std::sort(tabs.begin(), tabs.end(), [](uint32 left, uint32 right)
    {
        TalentTabEntry const* leftTab = sTalentTabStore.LookupEntry(left);
        TalentTabEntry const* rightTab = sTalentTabStore.LookupEntry(right);
        return (leftTab ? leftTab->OrderIndex : 0) < (rightTab ? rightTab->OrderIndex : 0);
    });
    return tabs;
}

uint8 CurrentTalentRank(Player const* bot, TalentEntry const* talent)
{
    for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        if (talent->SpellRank[rank] && bot->HasTalent(talent->SpellRank[rank], bot->GetActiveSpec()))
            return uint8(rank + 1);

    return 0;
}

// One greedy pass over a tab; returns points spent.
uint32 GreedySpendInTab(Player* bot, uint32 tabId)
{
    uint32 spent = 0;
    bool progress = true;
    while (progress && bot->GetFreeTalentPoints())
    {
        progress = false;
        for (TalentEntry const* talent : GetSortedTabTalents(tabId))
        {
            uint8 const currentRank = CurrentTalentRank(bot, talent);
            if (currentRank >= MAX_TALENT_RANK || !talent->SpellRank[currentRank])
                continue;

            uint32 const before = bot->GetFreeTalentPoints();
            bot->LearnTalent(talent->ID, currentRank);
            if (bot->GetFreeTalentPoints() < before)
            {
                spent += before - bot->GetFreeTalentPoints();
                progress = true;
                if (!bot->GetFreeTalentPoints())
                    return spent;
            }
        }
    }

    return spent;
}

void SpendPendingTalentPoints(Player* bot)
{
    if (bot->GetLevel() < 10 || !bot->GetFreeTalentPoints())
        return;

    std::array<uint32, 3> const signatures = GetProfileSignatureSpells(bot->GetClass());
    // Deterministic per-character profile so a class's bots spread across
    // specs but each keeps the same build for life.
    uint32 const profileIndex = uint32(bot->GetGUID().GetCounter() % 3);
    uint32 preferredTab = FindTalentTabContainingSpell(signatures[profileIndex]);
    for (uint32 probe = 1; probe < 3 && !preferredTab; ++probe)
        preferredTab = FindTalentTabContainingSpell(signatures[(profileIndex + probe) % 3]);

    std::vector<uint32> const classTabs = GetClassTalentTabs(bot);
    if (!preferredTab && !classTabs.empty())
        preferredTab = classTabs.front();
    if (!preferredTab)
        return;

    uint32 spent = GreedySpendInTab(bot, preferredTab);
    // Overflow into the other trees once the main tab can't absorb points.
    for (uint32 tabId : classTabs)
        if (tabId != preferredTab && bot->GetFreeTalentPoints())
            spent += GreedySpendInTab(bot, tabId);

    if (spent)
        TC_LOG_INFO("playerbots.pve", "Bot {} spent {} talent points (main tab {}).",
            bot->GetName(), spent, preferredTab);
}

// Same loop as ".learn my trainer": keep taking every class-trainer spell
// the bot qualifies for until a full pass adds nothing, so rank chains
// resolve in one go. Returns the number of spells taught.
uint32 RunTrainerSpellCatchup(Player* player)
{
    // GetClassTrainers is an unguarded unordered_map::at - a class with zero
    // class-trainer rows (death knights here) would throw.
    static std::vector<Trainer::Trainer const*> const emptyTrainers;
    std::vector<Trainer::Trainer const*> const* trainersPtr = &emptyTrainers;
    try
    {
        trainersPtr = &sObjectMgr->GetClassTrainers(player->GetClass());
    }
    catch (std::out_of_range const&)
    {
    }

    uint32 learned = 0;
    uint32 passes = 0;
    bool hadNew;
    do
    {
        // Pass cap = insurance against a trainer row whose teach never
        // changes learnable state.
        if (++passes > 10)
            break;

        hadNew = false;
        for (Trainer::Trainer const* trainer : *trainersPtr)
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

    return learned;
}

// One-time per login: SQL-provisioned bot characters start with an empty
// spellbook and a bare weapon (the auto-learn hook only fires on level-up,
// which a level-1 bot has never had). Teach everything trainable, spend any
// banked talents, and dress a naked bot in its class's real starter outfit.
void EnsureFirstLoginKit(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    if (state.initialKitDone || !bot->IsAlive())
        return;
    state.initialKitDone = true;

    uint32 learned = 0;
    if (cfg.autoLearnSpellsOnLevelUp)
        learned = RunTrainerSpellCatchup(bot);

    if (cfg.talentsEnabled)
        SpendPendingTalentPoints(bot);

    bool outfitGranted = false;
    bool const nakedTorso = !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST) &&
        !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_LEGS);
    if (nakedTorso)
    {
        if (CharStartOutfitEntry const* outfit = GetCharStartOutfitEntry(bot->GetRace(), bot->GetClass(), bot->GetGender()))
        {
            // Mirrors Player::Create's outfit grant, food stacks included.
            for (int32 outfitItemId : outfit->ItemID)
            {
                if (outfitItemId <= 0)
                    continue;

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(uint32(outfitItemId));
                if (!proto)
                    continue;

                uint32 count = proto->BuyCount;
                if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD)
                {
                    switch (proto->Spells[0].SpellCategory)
                    {
                        case SPELL_CATEGORY_FOOD:
                            count = 4;
                            break;
                        case SPELL_CATEGORY_DRINK:
                            count = 2;
                            break;
                    }
                    if (proto->GetMaxStackSize() < count)
                        count = proto->GetMaxStackSize();
                }

                outfitGranted = bot->StoreNewItemInBestSlots(uint32(outfitItemId), count) || outfitGranted;
            }
        }
    }

    if (learned || outfitGranted)
        TC_LOG_INFO("playerbots.pve", "Bot {} first-login kit: {} trainer spells{}.",
            bot->GetName(), learned, outfitGranted ? ", starter outfit granted" : "");
}

// ---------------------------------------------------------------------------
// Level-appropriate relocation (the reference module's teleport-for-level,
// rebuilt on this core's in-memory spawn store)
// ---------------------------------------------------------------------------

struct GrindSpot
{
    uint16 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

std::mutex g_GrindSpotLock;
std::unordered_map<uint8, std::vector<GrindSpot>> g_GrindSpotsByLevel;
bool g_GrindSpotsBuilt = false;

// World thread only. Mirrors the reference filters: normal-rank lootable
// mobs with tight level bands and short respawns, excluding service NPCs,
// friendly/guard factions, critters and unattackable flags; clusters of 3+
// spawns in a 50yd cell become one candidate spot for bot levels
// [meanLevel-1 .. meanLevel+3].
void BuildGrindSpotCacheOnce()
{
    std::lock_guard<std::mutex> guard(g_GrindSpotLock);
    if (g_GrindSpotsBuilt)
        return;
    g_GrindSpotsBuilt = true;

    struct SpotBucket
    {
        uint32 count = 0;
        GrindSpot spot;
        uint8 meanLevel = 0;
    };
    std::map<std::tuple<uint16, int32, int32>, SpotBucket> buckets;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        // Configurable per realm: B+ stays on the vanilla continents,
        // L+ can add 530/571. (The cache builds once per process, so a map
        // change needs a restart.)
        if (!std::binary_search(g_PveConfig.relocateMaps.begin(), g_PveConfig.relocateMaps.end(), data.mapId))
            continue;

        if (data.spawntimesecs >= 1000)
            continue;

        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
        if (!proto || proto->npcflag || !proto->lootid || proto->rank != CREATURE_ELITE_NORMAL)
            continue;

        if (proto->maxlevel < proto->minlevel || proto->maxlevel - proto->minlevel >= 3)
            continue;

        if (proto->type == CREATURE_TYPE_CRITTER || proto->type == CREATURE_TYPE_TOTEM)
            continue;

        if (proto->flags_extra & (CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER))
            continue;

        // Friendly/guard factions the reference excludes explicitly.
        switch (proto->faction)
        {
            case 11: case 71: case 79: case 85: case 188: case 1575:
                continue;
            default:
                break;
        }

        uint32 const unitFlags = proto->unit_flags | data.unit_flags;
        if (unitFlags & (UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE))
            continue;

        uint8 const meanLevel = uint8((proto->minlevel + proto->maxlevel + 1) / 2);
        if (!meanLevel || meanLevel > 83)
            continue;

        auto const key = std::make_tuple(uint16(data.mapId),
            int32(data.spawnPoint.GetPositionX()) / 50, int32(data.spawnPoint.GetPositionY()) / 50);
        SpotBucket& bucket = buckets[key];
        if (!bucket.count)
        {
            bucket.spot = { uint16(data.mapId), data.spawnPoint.GetPositionX(),
                data.spawnPoint.GetPositionY(), data.spawnPoint.GetPositionZ() };
            bucket.meanLevel = meanLevel;
        }
        ++bucket.count;
    }

    uint32 spotCount = 0;
    for (auto const& [key, bucket] : buckets)
    {
        if (bucket.count < 3)
            continue;

        // Zone screen at the source, so forbidden clusters never exist for
        // any travel arm (walk, taxi or teleport) to deliver bots into.
        if (Map* map = sMapMgr->FindMap(bucket.spot.mapId, 0))
            if (IsForbiddenGrindZone(map->GetZoneId(PHASEMASK_NORMAL, bucket.spot.x, bucket.spot.y, bucket.spot.z)))
                continue;

        ++spotCount;
        for (int32 level = int32(bucket.meanLevel) - 1; level <= int32(bucket.meanLevel) + 3; ++level)
            if (level >= 1 && level <= 80)
                g_GrindSpotsByLevel[uint8(level)].push_back(bucket.spot);
    }

    TC_LOG_INFO("playerbots.pve", "Grind spot cache built: {} clusters across {} level buckets.",
        spotCount, g_GrindSpotsByLevel.size());
}

bool HasHumanPlayerNearby(Player* bot, float radius)
{
    Map* map = bot->FindMap();
    if (!map)
        return false;

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (candidate && candidate != bot && IsHumanPlayer(candidate) && bot->IsWithinDistInMap(candidate, radius))
            return true;
    }

    return false;
}

bool HasHumanPlayerNearPosition(Map* map, float x, float y, float radius)
{
    if (!map)
        return false;

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !IsHumanPlayer(candidate))
            continue;

        float const dx = candidate->GetPositionX() - x;
        float const dy = candidate->GetPositionY() - y;
        if (dx * dx + dy * dy <= radius * radius)
            return true;
    }

    return false;
}

struct VendorSpot
{
    uint16 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

std::mutex g_VendorSpotLock;
std::vector<VendorSpot> g_VendorSpots;
bool g_VendorSpotsBuilt = false;

// World thread only.
void BuildVendorSpotCacheOnce()
{
    std::lock_guard<std::mutex> guard(g_VendorSpotLock);
    if (g_VendorSpotsBuilt)
        return;
    g_VendorSpotsBuilt = true;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!std::binary_search(g_PveConfig.relocateMaps.begin(), g_PveConfig.relocateMaps.end(), data.mapId))
            continue;

        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
        if (!proto || !(proto->npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;

        // Same zone screen as the grind clusters: no supply runs into the
        // DK intro area or GM Island.
        if (Map* map = sMapMgr->FindMap(data.mapId, 0))
            if (IsForbiddenGrindZone(map->GetZoneId(PHASEMASK_NORMAL, data.spawnPoint.GetPositionX(),
                data.spawnPoint.GetPositionY(), data.spawnPoint.GetPositionZ())))
                continue;

        g_VendorSpots.push_back({ uint16(data.mapId), data.spawnPoint.GetPositionX(),
            data.spawnPoint.GetPositionY(), data.spawnPoint.GetPositionZ() });
    }

    TC_LOG_INFO("playerbots.pve", "Vendor spot cache built: {} vendors.", g_VendorSpots.size());
}

// ---------------------------------------------------------------------------
// Gathering professions: each bot takes two of herbalism/mining/skinning
// (deterministic by guid), learns and ranks them automatically, gathers
// nodes through the errand system and skins finished corpses.
// ---------------------------------------------------------------------------

struct ProfessionTier
{
    uint32 spellId;
    uint16 requiredSkill;
    uint8 requiredLevel;
};

// Classic ladders through Artisan (300) - the fork's realms are 60-capped.
constexpr std::array<ProfessionTier, 4> kHerbalismTiers = { { { 2366, 0, 1 }, { 2368, 50, 10 }, { 3570, 125, 20 }, { 11993, 200, 35 } } };
constexpr std::array<ProfessionTier, 4> kMiningTiers = { { { 2575, 0, 1 }, { 2576, 50, 10 }, { 3564, 125, 20 }, { 10248, 200, 35 } } };
constexpr std::array<ProfessionTier, 4> kSkinningTiers = { { { 8613, 0, 1 }, { 8617, 50, 10 }, { 8618, 125, 20 }, { 10768, 200, 35 } } };

bool BotHasProfession(Player const* bot, LockType lockType)
{
    // guid % 3 picks the pair: 0 = herb+skin, 1 = mining+skin, 2 = herb+mining.
    uint32 const pick = bot->GetGUID().GetCounter() % 3;
    switch (lockType)
    {
        case LOCKTYPE_HERBALISM: return pick != 1;
        case LOCKTYPE_MINING:    return pick != 0;
        default:                 return false;
    }
}

bool BotSkins(Player const* bot)
{
    return bot->GetGUID().GetCounter() % 3 != 2;
}

void EnsureProfessionTier(Player* bot, std::array<ProfessionTier, 4> const& tiers, uint32 skillId)
{
    uint16 const skillValue = bot->GetSkillValue(skillId);
    for (ProfessionTier const& tier : tiers)
    {
        if (bot->HasSpell(tier.spellId))
            continue;

        if (bot->GetLevel() < tier.requiredLevel || skillValue < tier.requiredSkill)
            return;

        bot->LearnSpell(tier.spellId, false);
        TC_LOG_INFO("playerbots.pve", "Bot {} learned profession tier spell {}.", bot->GetName(), tier.spellId);
        return; // one tier per pass; the next check picks up the rest
    }
}

void EnsureProfessions(Player* bot)
{
    if (BotHasProfession(bot, LOCKTYPE_HERBALISM))
        EnsureProfessionTier(bot, kHerbalismTiers, SKILL_HERBALISM);
    if (BotHasProfession(bot, LOCKTYPE_MINING))
        EnsureProfessionTier(bot, kMiningTiers, SKILL_MINING);
    if (BotSkins(bot))
        EnsureProfessionTier(bot, kSkinningTiers, SKILL_SKINNING);
}

// A herb/ore node this bot can gather right now: chest-type GO whose lock
// carries a skill case of the matching type within the bot's skill.
bool IsGatherableNodeFor(Player* bot, GameObject const* go, int32* outRequiredSkill = nullptr)
{
    GameObjectTemplate const* goInfo = go->GetGOInfo();
    if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST)
        return false;

    LockEntry const* lock = sLockStore.LookupEntry(goInfo->GetLockId());
    if (!lock)
        return false;

    for (uint8 caseIndex = 0; caseIndex < MAX_LOCK_CASE; ++caseIndex)
    {
        if (lock->Type[caseIndex] != LOCK_KEY_SKILL)
            continue;

        LockType const lockType = LockType(lock->Index[caseIndex]);
        if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
            continue;

        if (!BotHasProfession(bot, lockType))
            return false;

        uint32 const skillId = SkillByLockType(lockType);
        if (!skillId || bot->GetSkillValue(skillId) < lock->Skill[caseIndex])
            return false;

        if (outRequiredSkill)
            *outRequiredSkill = int32(lock->Skill[caseIndex]);
        return true;
    }

    return false;
}

// Skinning replica of Spell::EffectSkinning for the world-thread loot
// executor: runs after a corpse is fully looted (which is what sets
// UNIT_FLAG_SKINNABLE).
void TrySkinCorpse(Player* bot, Creature* corpse)
{
    if (!BotSkins(bot) || !bot->HasSkill(SKILL_SKINNING))
        return;

    if (!corpse->HasUnitFlag(UNIT_FLAG_SKINNABLE))
        return;

    CreatureTemplate const* proto = corpse->GetCreatureTemplate();
    if (!proto || proto->GetRequiredLootSkill() != SKILL_SKINNING)
        return;

    uint32 const skillValue = bot->GetSkillValue(SKILL_SKINNING);
    int32 const targetLevel = int32(corpse->GetLevel());
    int32 const requiredValue = targetLevel < 10 ? 0 : (targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5);
    if (int32(skillValue) < requiredValue)
        return;

    corpse->RemoveUnitFlag(UNIT_FLAG_SKINNABLE);
    corpse->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
    bot->SendLoot(corpse->GetGUID(), LOOT_SKINNING);
    if (bot->GetLootGUID() != corpse->GetGUID())
        return;

    Loot* loot = &corpse->loot;
    uint32 const maxSlot = std::min<uint32>(loot->GetMaxSlotInLootFor(bot), 255);
    for (uint32 slot = 0; slot < maxSlot; ++slot)
        bot->StoreLootItem(uint8(slot), loot);

    if (bot->GetLootGUID() == corpse->GetGUID())
        bot->GetSession()->DoLootRelease(corpse->GetGUID());

    // UpdateGatherSkill compares against max-skill caps, so feed it the pure
    // (unbuffed) value; racial/enchant bonuses would stall skill-ups early.
    bot->UpdateGatherSkill(SKILL_SKINNING, bot->GetPureSkillValue(SKILL_SKINNING),
        uint32(std::max(0, requiredValue)), corpse->isElite() ? 2 : 1);
}

// After the world-thread executor loots a gather node: the skill-up the real
// gather spell would have granted (EffectOpenLock's tail), with the same
// one-per-respawn guard.
void GrantGatherSkillCredit(Player* bot, GameObject* go)
{
    GameObjectTemplate const* goInfo = go->GetGOInfo();
    if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST)
        return;

    LockEntry const* lock = sLockStore.LookupEntry(goInfo->GetLockId());
    if (!lock)
        return;

    for (uint8 caseIndex = 0; caseIndex < MAX_LOCK_CASE; ++caseIndex)
    {
        if (lock->Type[caseIndex] != LOCK_KEY_SKILL)
            continue;

        LockType const lockType = LockType(lock->Index[caseIndex]);
        if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
            continue;

        uint32 const skillId = SkillByLockType(lockType);
        if (!skillId)
            return;

        if (uint32 const pureSkill = bot->GetPureSkillValue(skillId))
        {
            if (!go->IsInSkillupList(bot->GetGUID()))
            {
                bot->UpdateGatherSkill(skillId, pureSkill, lock->Skill[caseIndex]);
                go->AddToSkillupList(bot->GetGUID());
            }
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Mail collection (world thread): auction wins and system mail arrive by
// post; a bot empties attachments and gold into its bags, mirroring the
// take-item/take-money handlers. Emptied mails are left to expire (flagging
// them deleted would hard-delete any attachment that failed to fit).
// ---------------------------------------------------------------------------

void ProcessPendingMailCollections()
{
    std::unordered_set<uint64> drained;
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        drained.swap(g_PendingMailCollections);
    }

    for (uint64 botRawGuid : drained)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
        if (!bot || !bot->IsInWorld() || bot->GetMails().empty())
            continue;

        bool tookAnything = false;
        uint32 tookItems = 0;
        time_t const nowTime = GameTime::GetGameTime();
        for (Mail* mail : bot->GetMails())
        {
            if (!mail || mail->state == MAIL_STATE_DELETED || mail->deliver_time > nowTime)
                continue;

            // Bots don't do COD trades: taking a CODed attachment without
            // paying would silently rob the sender. Leave such mail to
            // expire back to them.
            if (mail->COD)
                continue;

            if (mail->money)
            {
                if (bot->ModifyMoney(mail->money, false))
                {
                    mail->money = 0;
                    mail->state = MAIL_STATE_CHANGED;
                    tookAnything = true;
                }
            }

            if (mail->HasItems())
            {
                // Copy: taking an attachment mutates mail->items.
                std::vector<uint32> attachmentIds;
                for (MailItemInfo const& info : mail->items)
                    attachmentIds.push_back(info.item_guid);

                for (uint32 attachmentId : attachmentIds)
                {
                    Item* item = bot->GetMItem(attachmentId);
                    if (!item)
                        continue;

                    ItemPosCountVec dest;
                    if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                        continue;

                    mail->RemoveItem(attachmentId);
                    mail->removedItems.push_back(attachmentId);
                    mail->state = MAIL_STATE_CHANGED;
                    bot->RemoveMItem(attachmentId);
                    item->SetState(ITEM_UNCHANGED);
                    bot->MoveItemToInventory(dest, item, true);
                    tookAnything = true;
                    ++tookItems;
                }
            }
        }

        if (tookAnything)
        {
            bot->m_mailsUpdated = true;
            // _SaveMail is protected; the full save runs it and keeps the
            // mail rows and the moved items consistent in one transaction.
            bot->SaveToDB();
            TC_LOG_INFO("playerbots.pve", "Bot {} collected its mail ({} items).", bot->GetName(), tookItems);
        }
    }
}

// ---------------------------------------------------------------------------
// Auction shopping (world thread): browse the bot's faction house for one
// affordable gear upgrade and buy it out, replicating the handler's buyout
// branch exactly. The win arrives by mail and the collector above brings it
// home; the equip pass puts it on.
// ---------------------------------------------------------------------------

void ProcessPendingAuctionShopping()
{
    std::unordered_set<uint64> drained;
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        drained.swap(g_PendingAuctionShopping);
    }

    // A full-house scan is the expensive part; two shoppers per world pass,
    // the rest keep their place in line.
    uint32 scansLeft = 2;
    for (auto itr = drained.begin(); itr != drained.end(); ++itr)
    {
        if (!scansLeft)
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingAuctionShopping.insert(itr, drained.end());
            break;
        }

        uint64 const botRawGuid = *itr;
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            continue;

        // The fork destroys auction wins whose buyer is an AHBot character
        // (SendAuctionWonMail's IsBotChar gate) - those bots must not shop.
        if (sAuctionBotConfig->IsBotChar(bot->GetGUID().GetCounter()))
            continue;

        // A win that cannot be pocketed burns gold on mail that rots.
        if (CountFreeBagSlots(bot) < 2)
            continue;

        AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(bot->GetFaction());
        if (!auctionHouse)
            continue;

        --scansLeft;
        uint32 const budget = CalculatePct(bot->GetMoney(), g_PveConfig.auctionBuyBudgetPct);
        uint32 const botAccountId = bot->GetSession() ? bot->GetSession()->GetAccountId() : 0;

        AuctionEntry* bestAuction = nullptr;
        int32 bestGain = 0;
        for (auto itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
        {
            AuctionEntry* auction = itr->second;
            if (!auction || !auction->buyout || auction->buyout > budget)
                continue;

            if (auction->owner == bot->GetGUID().GetCounter())
                continue;

            Item* item = sAuctionMgr->GetAItem(auction->itemGUIDLow);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
                continue;

            if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;

            uint16 dest = 0;
            if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
                continue;

            // Never bench an equipped off hand for a two-hander (same rule
            // as the local equip pass).
            if (proto->InventoryType == INVTYPE_2HWEAPON &&
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                continue;

            uint32 equippedItemLevel = 0;
            if (Item const* equipped = bot->GetItemByPos(dest))
                equippedItemLevel = equipped->GetTemplate() ? equipped->GetTemplate()->ItemLevel : 0;

            int32 const gain = int32(proto->ItemLevel) - int32(equippedItemLevel);
            if (gain <= 0 || gain <= bestGain)
                continue;

            // No trading with the bot's own account.
            if (botAccountId && sCharacterCache->GetCharacterAccountIdByGuid(
                    ObjectGuid::Create<HighGuid::Player>(auction->owner)) == botAccountId)
                continue;

            bestAuction = auction;
            bestGain = gain;
        }

        if (!bestAuction)
            continue;

        // Buyout replica: bidder/bid assigned BEFORE the mails (they read
        // them), won-mail before RemoveAItem, RemoveAuction last.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        bot->ModifyMoney(-int32(bestAuction->buyout));
        if (bestAuction->bidder)
            sAuctionMgr->SendAuctionOutbiddedMail(bestAuction, bestAuction->buyout, bot, trans);
        bestAuction->bidder = bot->GetGUID().GetCounter();
        bestAuction->bid = bestAuction->buyout;
        // Bots hold no GM permission; same clear as the handler's else arm.
        bestAuction->Flags = AuctionEntryFlag(bestAuction->Flags & ~AUCTION_ENTRY_FLAG_GM_LOG_BUYER);

        sAuctionMgr->SendAuctionSalePendingMail(bestAuction, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(bestAuction, trans);
        sAuctionMgr->SendAuctionWonMail(bestAuction, trans);

        TC_LOG_INFO("playerbots.pve", "Bot {} bought auction {} (item {} for {} copper, +{} item levels).",
            bot->GetName(), bestAuction->Id, bestAuction->itemEntry, bestAuction->buyout, bestGain);

        bestAuction->DeleteFromDB(trans);
        sAuctionMgr->RemoveAItem(bestAuction->itemGUIDLow);
        auctionHouse->RemoveAuction(bestAuction);

        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        // Fetch the win right away.
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        g_PendingMailCollections.insert(botRawGuid);
    }
}

// ---------------------------------------------------------------------------
// Taxi travel: BFS over direct taxi edges; the bot pays its own fare.
// ---------------------------------------------------------------------------

std::vector<uint32> FindTaxiNodeChain(uint32 sourceNode, uint32 destNode)
{
    std::vector<uint32> chain;
    if (!sourceNode || !destNode || sourceNode == destNode)
        return chain;

    std::unordered_map<uint32, uint32> cameFrom;
    std::deque<uint32> frontier;
    frontier.push_back(sourceNode);
    cameFrom[sourceNode] = 0;

    while (!frontier.empty() && cameFrom.size() < 500)
    {
        uint32 const node = frontier.front();
        frontier.pop_front();
        auto const edges = sTaxiPathSetBySource.find(node);
        if (edges == sTaxiPathSetBySource.end())
            continue;

        for (auto const& edge : edges->second)
        {
            uint32 const next = edge.first;
            if (cameFrom.count(next))
                continue;

            cameFrom[next] = node;
            if (next == destNode)
            {
                for (uint32 walk = destNode; walk; walk = cameFrom[walk])
                    chain.push_back(walk);
                std::reverse(chain.begin(), chain.end());
                return chain;
            }
            frontier.push_back(next);
        }
    }

    return chain;
}

// World thread. Fly the bot toward a destination when a route exists and a
// taxi node is close by; the post-landing leg becomes a walked journey.
bool TryTaxiTravel(Player* bot, uint64 botRawGuid, uint16 destMapId, float destX, float destY, float destZ, uint8 fallbackKind)
{
    if (!g_PveConfig.travelUseFlightPaths || bot->IsInFlight() || bot->IsInCombat())
        return false;

    uint32 const sourceNode = sObjectMgr->GetNearestTaxiNodeAnyTeam(
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    uint32 const destNode = sObjectMgr->GetNearestTaxiNodeAnyTeam(destX, destY, destZ, destMapId);
    if (!sourceNode || !destNode || sourceNode == destNode)
        return false;

    TaxiNodesEntry const* sourceEntry = sTaxiNodesStore.LookupEntry(sourceNode);
    TaxiNodesEntry const* destEntry = sTaxiNodesStore.LookupEntry(destNode);
    if (!sourceEntry || !destEntry)
        return false;

    // Only fly when the departure point is genuinely nearby; a flight that
    // visibly starts hundreds of yards from any flight master looks wrong.
    if (uint32(sourceEntry->ContinentID) != bot->GetMapId() ||
        bot->GetDistance(sourceEntry->Pos.X, sourceEntry->Pos.Y, sourceEntry->Pos.Z) > 300.0f)
        return false;

    std::vector<uint32> const chain = FindTaxiNodeChain(sourceNode, destNode);
    if (chain.size() < 2 || chain.size() > 8)
        return false;

    // Post-landing leg: walk from the arrival node to the actual target.
    {
        PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
        state.engaged = false;
        if (uint32(destEntry->ContinentID) == destMapId)
        {
            StartWalkedJourney(state, destMapId, destX, destY, destZ, fallbackKind,
                std::max(200.0f, bot->GetDistance(destX, destY, destZ)));
            // Flights are long; extend the deadline generously.
            state.journeyUntil = PveClock::now() + std::chrono::seconds(900);
        }
    }
    playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);

    if (!bot->ActivateTaxiPathTo(chain, nullptr))
    {
        // InstantTaxi pays the fare, teleports to the last node and returns
        // false; the journey leg still applies, so treat it as flown - a
        // false return here would send the caller on to teleport elsewhere.
        if (sWorld->getBoolConfig(CONFIG_INSTANT_TAXI))
            return true;

        PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
        state.journeyActive = false;
        state.journeyFallbackKind = 0;
        return false;
    }

    TC_LOG_INFO("playerbots.pve", "Bot {} took a flight: {} hops toward map {} ({:.0f} {:.0f}).",
        bot->GetName(), uint32(chain.size() - 1), destMapId, destX, destY);
    return true;
}

// World thread. Town run: teleport a supply-starved bot next to the nearest
// vendor spawn, avoiding real players at both ends.
void ProcessPendingSupplyRuns()
{
    std::unordered_set<uint64> drained;
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        drained.swap(g_PendingSupplyRuns);
    }

    if (drained.empty())
        return;

    BuildVendorSpotCacheOnce();

    for (uint64 botRawGuid : drained)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->InBattleground() ||
            bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            continue;

        VendorSpot const* nearest = nullptr;
        float nearestDist2 = 0.0f;
        {
            std::lock_guard<std::mutex> guard(g_VendorSpotLock);
            for (VendorSpot const& spot : g_VendorSpots)
            {
                if (spot.mapId != bot->GetMapId())
                    continue;

                float const dx = spot.x - bot->GetPositionX();
                float const dy = spot.y - bot->GetPositionY();
                float const dist2 = dx * dx + dy * dy;
                if (!nearest || dist2 < nearestDist2)
                {
                    nearest = &spot;
                    nearestDist2 = dist2;
                }
            }
        }

        if (!nearest)
            continue;

        // Walking distance? Make it a real trip to town.
        bool walkAllowed;
        {
            PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
            walkAllowed = PveClock::now() >= state.walkFallbackUntil;
        }
        if (walkAllowed && g_PveConfig.travelWalkMaxDistance > 0.0f && nearest->mapId == bot->GetMapId())
        {
            float const walkDistance = bot->GetDistance(nearest->x, nearest->y, nearest->z);
            if (walkDistance <= g_PveConfig.travelWalkMaxDistance)
            {
                playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                state.engaged = false;
                StartWalkedJourney(state, nearest->mapId, nearest->x, nearest->y, nearest->z, 2, walkDistance);
                TC_LOG_INFO("playerbots.pve", "Bot {} walking {:.0f}y to town for supplies.",
                    bot->GetName(), walkDistance);
                continue;
            }
        }

        if (TryTaxiTravel(bot, botRawGuid, nearest->mapId, nearest->x, nearest->y, nearest->z, 2))
            continue;

        // Walking and flying are visible, legitimate travel; only the
        // teleport needs the vanish-guard at the source end.
        if (HasHumanPlayerNearby(bot, 150.0f))
            continue;

        Map* map = sMapMgr->FindMap(nearest->mapId, 0);
        if (!map || HasHumanPlayerNearPosition(map, nearest->x, nearest->y, 150.0f))
            continue;

        playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
        {
            PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
            state.engaged = false;
        }
        if (MotionMaster* motionMaster = bot->GetMotionMaster())
            motionMaster->Clear();
        bot->TeleportTo(nearest->mapId, nearest->x + frand(-3.0f, 3.0f), nearest->y + frand(-3.0f, 3.0f),
            nearest->z + 0.5f, frand(0.0f, 6.28f));
        TC_LOG_INFO("playerbots.pve", "Supply run: teleported {} to a vendor cluster on map {}.",
            bot->GetName(), nearest->mapId);
    }
}

// World thread. Teleports one bot to a random spot for its level, with the
// reference safety checks: loaded map, no enemy-faction zone, not into
// water, grounded Z, and never in sight of a real player.
void ProcessPendingGrindRelocations()
{
    std::unordered_set<uint64> drained;
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        drained.swap(g_PendingGrindRelocations);
    }

    if (drained.empty())
        return;

    BuildGrindSpotCacheOnce();

    for (uint64 botRawGuid : drained)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->InBattleground() ||
            bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            continue;

        // Only the teleport arm below must hide from real players; walking
        // and flying are visible, legitimate travel.
        bool const humanNearby = HasHumanPlayerNearby(bot, 150.0f);

        std::vector<GrindSpot> candidates;
        {
            std::lock_guard<std::mutex> guard(g_GrindSpotLock);
            uint8 level = uint8(std::min<uint32>(bot->GetLevel(), 80));
            // Walk down a few brackets if the exact level has no clusters.
            for (uint8 probe = 0; probe < 5 && candidates.empty() && level > probe; ++probe)
            {
                auto itr = g_GrindSpotsByLevel.find(level - probe);
                if (itr != g_GrindSpotsByLevel.end())
                    candidates = itr->second;
            }
        }

        if (candidates.empty())
            continue;

        for (uint8 attempt = 0; attempt < 10; ++attempt)
        {
            GrindSpot const& spot = candidates[urand(0, uint32(candidates.size() - 1))];

            // Walk when the spot is on this map within range: visible travel
            // beats teleporting, and walking needs no vanish-guards.
            bool walkAllowed;
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                walkAllowed = PveClock::now() >= state.walkFallbackUntil;
            }
            if (walkAllowed && g_PveConfig.travelWalkMaxDistance > 0.0f && spot.mapId == bot->GetMapId())
            {
                float const walkDistance = bot->GetDistance(spot.x, spot.y, spot.z);
                if (walkDistance <= g_PveConfig.travelWalkMaxDistance)
                {
                    playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
                    PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                    state.engaged = false;
                    StartWalkedJourney(state, spot.mapId, spot.x, spot.y, spot.z, 1, walkDistance);
                    TC_LOG_INFO("playerbots.pve", "Bot {} walking {:.0f}y to a new grind spot.",
                        bot->GetName(), walkDistance);
                    break;
                }
            }

            // A real flight beats teleporting when a route exists.
            if (TryTaxiTravel(bot, botRawGuid, spot.mapId, spot.x, spot.y, spot.z, 1))
                break;

            if (humanNearby)
                break;

            Map* map = sMapMgr->FindMap(spot.mapId, 0);
            if (!map)
                continue;

            uint32 const zoneId = map->GetZoneId(PHASEMASK_NORMAL, spot.x, spot.y, spot.z);
            if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
            {
                if (bot->GetTeamId() == TEAM_ALLIANCE && zone->FactionGroupMask == 4)
                    continue;
                if (bot->GetTeamId() == TEAM_HORDE && zone->FactionGroupMask == 2)
                    continue;
            }

            if (map->IsInWater(PHASEMASK_NORMAL, spot.x, spot.y, spot.z))
                continue;

            float const ground = map->GetHeight(PHASEMASK_NORMAL, spot.x, spot.y, spot.z + 5.0f);
            if (ground <= INVALID_HEIGHT)
                continue;

            playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
            {
                // Keep the per-bot flag in sync with the registry, or the
                // bot's next fights run with the class-spell gate closed.
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                state.engaged = false;
            }
            if (MotionMaster* motionMaster = bot->GetMotionMaster())
                motionMaster->Clear();
            bot->TeleportTo(spot.mapId, spot.x, spot.y, ground + 0.05f, frand(0.0f, 6.28f));
            TC_LOG_INFO("playerbots.pve", "Relocated grind bot {} (level {}) to map {} {:.0f} {:.0f}.",
                bot->GetName(), bot->GetLevel(), spot.mapId, spot.x, spot.y);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Gameobject quest objectives (goobers grant RequiredNpcOrGo<0 credit via
// Use(); quest chests are looted for their quest items)
// ---------------------------------------------------------------------------

struct QuestGameObjectCheck
{
    Player* bot;
    bool wantQuestObjects;
    bool wantGatherNodes;

    bool operator()(GameObject* go) const
    {
        if (!go->isSpawned())
            return false;

        if (wantQuestObjects && go->ActivateToQuest(bot))
        {
            GameobjectTypes const type = go->GetGoType();
            if (type == GAMEOBJECT_TYPE_GOOBER || type == GAMEOBJECT_TYPE_CHEST)
                return true;
        }

        return wantGatherNodes && IsGatherableNodeFor(bot, go, nullptr);
    }
};

GameObject* FindNearestQuestGameObject(Player* bot, PveBotState& state, float radius)
{
    std::vector<GameObject*> matches;
    playerbot::PveConfig const& cfg = playerbot::PveManager::GetConfig();
    QuestGameObjectCheck check{ bot, cfg.questsEnabled && BotHasIncompleteQuest(bot), cfg.professionsEnabled };
    Trinity::GameObjectListSearcher<QuestGameObjectCheck> searcher(bot, matches, check);
    Cell::VisitGridObjects(bot, searcher, radius);

    GameObject* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (GameObject* candidate : matches)
    {
        if (IsRecentErrandTarget(state, candidate->GetGUID()))
            continue;

        float const distance = bot->GetDistance(candidate);
        if (!nearest || distance < nearestDistance)
        {
            nearest = candidate;
            nearestDistance = distance;
        }
    }

    return nearest;
}

void UseQuestGameObject(Player* bot, PveBotState& state, GameObject* go)
{
    // Whatever happens next, do not immediately re-pick this object: a
    // locked or script-gated one would otherwise loop the errand forever.
    MarkRecentErrandTarget(state, go->GetGUID());

    if (go->GetGoType() == GAMEOBJECT_TYPE_GOOBER)
    {
        go->Use(bot);
        TC_LOG_INFO("playerbots.pve", "Bot {} used quest object {} ({}).",
            bot->GetName(), go->GetEntry(), go->GetGOInfo() ? go->GetGOInfo()->name : "");
        return;
    }

    if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST || go->getLootState() != GO_READY)
        return;

    // The chest loot session mutates group loot state; execute it on the
    // world thread with the corpse loot.
    std::lock_guard<std::mutex> guard(g_PvePendingLock);
    g_PendingLootExecutions[bot->GetGUID().GetRawValue()] = go->GetGUID();
}

bool BotHasIncompleteQuest(Player* bot)
{
    for (auto const& [questId, status] : bot->getQuestStatusMap())
        if (status.Status == QUEST_STATUS_INCOMPLETE)
            return true;

    return false;
}

Unit* PickCompanionTarget(Player* bot, PveBotState& state, Player* master, playerbot::PveConfig const& cfg)
{
    Unit* best = nullptr;
    float bestDistance = 0.0f;
    auto consider = [&](Unit* candidate)
    {
        if (!candidate || !candidate->IsAlive() || !bot->IsValidAttackTarget(candidate))
            return;

        if (IsRecentBadTarget(state, candidate->GetGUID()))
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

void ExecuteEngagedCombatTick(Player* bot, PveBotState& state)
{
    // Outside battlegrounds the values snapshot is all-default; the class
    // context builder only consults it for battleground triggers.
    playerbot::PvpValues const values{};
    playerbot::PvpClassSpellContext const context = playerbot::PvpCore::BuildClassSpellContext(bot, values);
    bool const executed = playerbot::PvpClassActions::Execute(bot, context);

    Unit* victim = ResolveAttackableByGuid(bot, bot->GetTarget());

    if (playerbot::PveManager::GetConfig().combatDiagnostics)
    {
        PveTimePoint const now = PveClock::now();
        if (now >= state.nextCombatDiagAt)
        {
            state.nextCombatDiagAt = now + std::chrono::seconds(2);
            TC_LOG_INFO("playerbots.pve",
                "combatdiag bot={} target={} dist={:.1f} exec={} should={} action={} reason={} spell={} moveDir={} lastExec=[{}] victim={} inCombat={} o={:.2f} moving={} motion={}",
                bot->GetName(), victim ? victim->GetName() : "none",
                victim ? bot->GetDistance(victim) : -1.0f,
                executed ? 1 : 0, context.shouldExecute ? 1 : 0,
                context.actionName ? context.actionName : "-",
                context.reason ? context.reason : "-",
                context.spellId, uint32(context.movementDirective),
                playerbot::PvpClassActions::GetLastExecutionStatus(bot),
                bot->GetVictim() ? "yes" : "no", bot->IsInCombat() ? 1 : 0,
                bot->GetOrientation(), bot->isMoving() ? 1 : 0,
                bot->GetMotionMaster() ? uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()) : 999);
        }
    }

    if (!victim)
        return;

    // White-swing floor: a fresh low-level bot can know nothing castable, and
    // the decision graph then yields neither a spell nor a movement directive.
    // Plain chase plus auto-attack is what a player of that level does too.
    bool const engineActedThisTick = executed && context.shouldExecute;

    // Auto-attack parity with a real client: pressing any offensive ability
    // turns melee on and casting never turns it off. Bots have no client,
    // and the class-action cast paths establish spell victims through
    // Attack(target, false) - which Unit::Attack treats as a DOWNGRADE,
    // clearing UNIT_STATE_MELEE_ATTACKING and broadcasting attack-stop
    // (Player::Update gates every white swing on that state). Re-assert
    // melee after the engine has acted each tick, or casting classes fight
    // entire battles with auto-attack visibly off and no rage/dodge flow.
    // Stealth is the one state where starting swings would wreck the opener.
    if ((bot->GetVictim() != victim || !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING)) &&
        !bot->HasAuraType(SPELL_AURA_MOD_STEALTH))
        bot->Attack(victim, true);

    if (!engineActedThisTick && !bot->IsWithinMeleeRange(victim))
        playerbot::PvpClassActions::IssueFollowMovement(bot, victim, 1.0f);

    // Melee swings demand the 120-degree arc (Player::Update just re-arms
    // the attack timer outside it) and nothing maintains a stopped bot's
    // facing: real players re-face client-side, bots have no client, and
    // the cast-time fixups use the wider 180-degree spell arc and only run
    // when a cast actually executes. A victim that sidesteps into the
    // 60-120 degree band silently blocks every auto-attack - no rage, no
    // dodges, and the whole rotation starves behind the dead swing timer.
    if (bot->IsWithinMeleeRange(victim) && !bot->HasUnitState(UNIT_STATE_CASTING) &&
        !bot->HasInArc(2.0f * float(M_PI) / 3.0f, victim))
    {
        if (!bot->IsStopped() || (bot->movespline && !bot->movespline->Finalized()))
            bot->SetInFront(victim);
        else
        {
            bot->SetFacingToObject(victim);
            bot->SetInFront(victim);
        }
    }

    // Stalled-chase recovery: a chase generator issued while the
    // MotionMaster sat in its pending state never actually moves, and the
    // movement throttle then "preserves" the dead chase forever
    // (motion=chase, chase_move=no, moving=no). If an engaged bot with a far
    // target holds the same position for ~1.5s, rebuild its movement.
    float const victimDistance = bot->GetDistance(victim);
    if (victimDistance > 35.0f && !bot->HasUnitState(UNIT_STATE_CASTING))
    {
        float const deltaX = bot->GetPositionX() - state.lastEngagedX;
        float const deltaY = bot->GetPositionY() - state.lastEngagedY;
        if (deltaX * deltaX + deltaY * deltaY < 0.25f)
        {
            if (++state.engagedStallTicks >= 6 && !bot->isMoving())
            {
                state.engagedStallTicks = 0;
                if (MotionMaster* motionMaster = bot->GetMotionMaster())
                    motionMaster->Clear();
                playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                playerbot::PvpClassActions::IssueFollowMovement(bot, victim, 25.0f);
                TC_LOG_INFO("playerbots.pve", "Bot {} recovered a stalled chase toward {} at {:.0f}y.",
                    bot->GetName(), victim->GetName(), victimDistance);
            }
        }
        else
            state.engagedStallTicks = 0;

        state.lastEngagedX = bot->GetPositionX();
        state.lastEngagedY = bot->GetPositionY();
    }
    else
        state.engagedStallTicks = 0;
}

bool TryJoinSummonerGroup(Player* summoner, Player* bot)
{
    // A pending invite from some other group would leave a dangling invitee
    // entry behind a direct AddMember.
    if (bot->GetGroupInvite())
        bot->UninviteFromGroup();

    if (bot->GetGroup())
    {
        if (bot->GetGroup() == summoner->GetGroup())
            return true;

        bot->RemoveFromGroup();
    }

    Group* group = summoner->GetGroup();
    if (!group)
    {
        group = new Group;
        if (!group->Create(summoner))
        {
            delete group;
            return false;
        }

        sGroupMgr->AddGroup(group);
    }

    if (group->IsFull())
    {
        bot->Whisper("Your group is full.", LANG_UNIVERSAL, summoner);
        return false;
    }

    if (!group->AddMember(bot))
        return false;

    group->BroadcastGroupUpdate();
    return true;
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
        if (!summoner || !summoner->IsInWorld() || summoner->InBattleground())
        {
            // A master inside a battleground can't be joined; drop the
            // request rather than retrying against it forever.
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

        // Master status is derived from group membership everywhere else
        // (UpdateMasterFromGroup); never set it for a bot that could not
        // join the group, or the two disagree forever.
        if (pending.joinGroup && !TryJoinSummonerGroup(summoner, bot))
        {
            erasePending();
            continue;
        }

        PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
        state.masterGuid = summoner->GetGUID();
        state.passive = false;
        state.stay = false;
        // Serving a master supersedes any solo trek in progress.
        state.journeyActive = false;
        state.journeyFallbackKind = 0;
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
    // Gaining a master supersedes any solo trek in progress.
    if (!state.masterGuid.IsEmpty() && state.journeyActive)
    {
        state.journeyActive = false;
        state.journeyFallbackKind = 0;
    }
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

    EnsureFirstLoginKit(bot, state, cfg);

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
            // No catch-up into battlegrounds: the summon executor refuses
            // those anyway, so queueing would just retry forever.
            Player const* master = ObjectAccessor::FindConnectedPlayer(state.masterGuid);
            if (master && !master->InBattleground())
                QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
        }
        else if (bot->GetDistance(masterSameMap) > PveCompanionTeleportCatchupDistance)
            QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
    }

    // Buff upkeep before the rest check: buffs only fire above 60% mana,
    // rest starts under 50%, so the two never chase each other.
    if (cfg.buffsEnabled && !state.engaged && !IsRestingNow(bot, state))
        playerbot::PvpCore::TryCastOpenWorldBuff(bot);

    // Rest: sit down and use the free food/water once the fight is over.
    // Companions only sit while their master is close (well inside the 40y
    // rest-break distance), so start/break can't oscillate behind a moving
    // master.
    bool masterAllowsRest = true;
    if (!state.masterGuid.IsEmpty())
    {
        Player const* masterSameMap = ObjectAccessor::GetPlayer(*bot, state.masterGuid);
        masterAllowsRest = masterSameMap && bot->GetDistance(masterSameMap) < PveRestBreakFollowDistance * 0.75f;
    }

    if (!state.engaged && !bot->IsInCombat() && !IsRestingNow(bot, state) && masterAllowsRest &&
        (!state.journeyActive || NeedsRecovery(bot, cfg)))
    {
        bool const needFood = bot->GetHealthPct() < cfg.restHealthPct;
        bool const needDrink = bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < cfg.restManaPct;
        if (needFood || needDrink)
        {
            if (cfg.restUseConsumables)
            {
                // Economy realm: eat/drink the real thing from the bags; a
                // bot with nothing edible restocks through the vendor errand.
                Item* consumable = FindBestConsumable(bot, !needFood);
                if (!consumable && needDrink)
                    consumable = FindBestConsumable(bot, true);
                if (consumable)
                {
                    // The cast may consume the last unit and delete the item;
                    // capture everything needed before it runs.
                    uint32 const consumableEntry = consumable->GetEntry();
                    uint32 usedSpellId = 0;
                    if (ItemTemplate const* proto = consumable->GetTemplate())
                        for (uint8 spellIdx = 0; spellIdx < MAX_ITEM_PROTO_SPELLS; ++spellIdx)
                            if (proto->Spells[spellIdx].SpellId > 0 &&
                                proto->Spells[spellIdx].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                            {
                                usedSpellId = uint32(proto->Spells[spellIdx].SpellId);
                                break;
                            }

                    playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                    if (MotionMaster* motionMaster = bot->GetMotionMaster())
                        motionMaster->Clear();
                    bot->StopMoving();
                    SpellCastTargets targets;
                    targets.SetUnitTarget(bot);
                    bot->CastItemUseSpell(consumable, targets, 0, 0);

                    // CastItemUseSpell reports nothing: only the landed
                    // food/drink aura counts as eating. Without this check a
                    // rejected cast faked a 22s "rest" that healed nothing,
                    // forever. Sitting is the client's job for real players,
                    // so do it here too.
                    if (usedSpellId && bot->HasAura(usedSpellId))
                    {
                        bot->SetStandState(UNIT_STAND_STATE_SIT);
                        state.restingUntil = PveClock::now() + std::chrono::seconds(22);
                    }
                    else if (cfg.combatDiagnostics)
                        TC_LOG_INFO("playerbots.pve", "Bot {} could not eat/drink item {} (use spell {} did not land).",
                            bot->GetName(), consumableEntry, usedSpellId);
                }
            }
            else
            {
                playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                if (MotionMaster* motionMaster = bot->GetMotionMaster())
                    motionMaster->Clear();
                bot->StopMoving();
                bot->CastSpell(bot, needFood ? SPELL_PVE_OUT_OF_COMBAT_EAT : SPELL_PVE_OUT_OF_COMBAT_DRINK, true);
            }
        }
    }

    PveTimePoint const now = PveClock::now();

    // Quest/vendor errands are for autonomous bots; companions stay on their
    // master's heel.
    if (state.masterGuid.IsEmpty() && !state.engaged && !bot->IsInCombat() &&
        state.errandKind == PveErrandKind::None && state.pendingLootGuid.IsEmpty() &&
        !state.journeyActive && now >= state.nextErrandScanAt)
    {
        state.nextErrandScanAt = now + std::chrono::seconds(15);
        StartErrandIfNeeded(bot, state, cfg);
        MaybeFieldRepair(bot, state, cfg);
    }

    if (cfg.equipUpgradesEnabled && !bot->IsInCombat() && now >= state.nextEquipCheckAt)
    {
        state.nextEquipCheckAt = now + std::chrono::seconds(15);
        TryEquipUpgrades(bot);
        // Companions never run vendor errands (they stay on their master's
        // heel), so critical durability gets the field repair here.
        if (!state.masterGuid.IsEmpty())
            MaybeFieldRepair(bot, state, cfg);
    }

    if (cfg.talentsEnabled && !bot->IsInCombat() && now >= state.nextTalentCheckAt)
    {
        state.nextTalentCheckAt = now + std::chrono::seconds(60);
        SpendPendingTalentPoints(bot);
    }

    if (cfg.professionsEnabled && !bot->IsInCombat() && now >= state.nextProfessionCheckAt)
    {
        state.nextProfessionCheckAt = now + std::chrono::seconds(60);
        EnsureProfessions(bot);
    }

    // Mail and auction browsing touch world-thread structures; the map
    // thread only enqueues.
    if (now >= state.nextMailCheckAt)
    {
        state.nextMailCheckAt = now + std::chrono::minutes(3);
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        g_PendingMailCollections.insert(bot->GetGUID().GetRawValue());
    }

    if (cfg.auctionBuyEnabled && !bot->IsInCombat() && now >= state.nextAuctionShopAt)
    {
        // First pass after a restart: spread the fleet's shopping trips out
        // instead of lining every bot up for the same world tick.
        if (state.nextAuctionShopAt == PveTimePoint())
            state.nextAuctionShopAt = now + std::chrono::seconds(60 + bot->GetGUID().GetCounter() % 540);
        else
        {
            state.nextAuctionShopAt = now + std::chrono::minutes(10);
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingAuctionShopping.insert(bot->GetGUID().GetRawValue());
        }
    }
}

void RunFastTick(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    Player* master = state.masterGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, state.masterGuid);

    // Must run before the disengage path clears the dead victim's guid.
    DetectFreshKillForLoot(bot, state, cfg);

    ObjectGuid const previousTargetGuid = bot->GetTarget();
    Unit* target = ResolveAttackableByGuid(bot, previousTargetGuid);

    // A held target that stopped resolving while still alive (evade flicker,
    // validity flicker) must not be immediately re-acquired: the resulting
    // engage/AttackStop cycle stutters both the bot and the mob chasing it.
    if (!target && state.engaged && !previousTargetGuid.IsEmpty())
    {
        Unit const* lost = ObjectAccessor::GetUnit(*bot, previousTargetGuid);
        if (lost && lost->IsAlive())
            MarkRecentBadTarget(state, previousTargetGuid);

        if (playerbot::PveManager::GetConfig().combatDiagnostics)
        {
            Creature const* lostCreature = lost ? lost->ToCreature() : nullptr;
            TC_LOG_INFO("playerbots.pve",
                "combatdiag bot={} LOST target={} resolved={} alive={} validAttack={} evade={}",
                bot->GetName(), previousTargetGuid.ToString(),
                lost ? 1 : 0, lost && lost->IsAlive() ? 1 : 0,
                lost && bot->IsValidAttackTarget(lost) ? 1 : 0,
                lostCreature && lostCreature->IsInEvadeMode() ? 1 : 0);
        }
    }
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
        // Self-defense first, with no level window: something outside the
        // grind filter (a higher-level aggro) beating on the bot must still
        // be fought back regardless of mode.
        Unit* nearestAttacker = nullptr;
        float nearestAttackerDistance = 0.0f;
        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || !bot->IsValidAttackTarget(attacker))
                continue;

            // No bad-target screen here: that list stops re-PICKING
            // evade-flickery mobs, but a unit actively hitting the bot is
            // present and reachable by definition - and a genuinely evading
            // mob is not in getAttackers() at all. Honoring the list here
            // left bots standing in place eating hits for its full 60s.
            float const distance = bot->GetDistance(attacker);
            if (!nearestAttacker || distance < nearestAttackerDistance)
            {
                nearestAttacker = attacker;
                nearestAttackerDistance = distance;
            }
        }

        if (nearestAttacker)
        {
            target = nearestAttacker;
            // Fighting it voids any stale bad-listing, or the engaged
            // tick's own resolve would flap on the same stale entry.
            state.recentBadTargets.erase(nearestAttacker->GetGUID().GetRawValue());
        }
        else if (master)
            target = PickCompanionTarget(bot, state, master, cfg);
        else if (cfg.grindEnabled && state.masterGuid.IsEmpty() && !IsRestingNow(bot, state) &&
            !NeedsRecovery(bot, cfg))
        {
            PveTimePoint const now = PveClock::now();
            if (now >= state.nextGrindScanAt)
            {
                state.nextGrindScanAt = now + PveGrindScanInterval;
                target = PickGrindTarget(bot, state, cfg);
            }
        }
    }

    if (target)
    {
        state.dryWanderCount = 0;
        if (bot->GetTarget() != target->GetGUID())
            bot->SetSelection(target->GetGUID());
        if (!state.engaged)
        {
            state.engaged = true;
            // A bot jumped mid-meal must not fight sitting down.
            bot->SetStandState(UNIT_STAND_STATE_STAND);
            TC_LOG_INFO("playerbots.pve", "Bot {} engaging {} (level {}) at {:.0f}y.",
                bot->GetName(), target->GetName(), target->GetLevel(), bot->GetDistance(target));
        }

        // Re-arm the registry every combat tick, not just on the first: the
        // relocation executor (and any future cross-thread cleanup) clears
        // the registry without seeing this map's engaged flag, and a bot
        // whose registry is down fights with white swings only.
        playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), true);

        // A fight en route must not count as journey stall.
        if (state.journeyActive)
            state.journeyProgressAt = PveClock::now();

        ExecuteEngagedCombatTick(bot, state);
        return;
    }

    if (state.engaged)
        DisengagePveCombat(bot, state);

    if (ProcessPendingLoot(bot, state, cfg))
        return;

    if (IsRestingNow(bot, state))
    {
        bool const stillRecovering = bot->GetHealthPct() < 99.0f ||
            (bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 99.0f);
        bool const masterLeftRestRange = master && bot->GetDistance(master) > PveRestBreakFollowDistance;
        if (stillRecovering && !masterLeftRestRange)
        {
            // A rest stop en route must not count as journey stall.
            if (state.journeyActive)
                state.journeyProgressAt = PveClock::now();
            return;
        }

        RemoveRestAuras(bot);
        // Standing up also removes the consumable food/drink auras via the
        // core's NOT_SEATED interrupt; a seated bot must never start moving.
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        state.restingUntil = PveClock::now();
    }

    if (ProcessErrand(bot, state, cfg))
        return;

    if (AdvanceWalkedJourney(bot, state))
        return;

    if (master && master->IsAlive() && !state.stay)
    {
        if (bot->GetDistance(master) > cfg.companionFollowDistance + 1.5f)
            playerbot::PvpClassActions::IssueFollowMovement(bot, master, cfg.companionFollowDistance);
        return;
    }

    // A recovering bot holds still: rest (or the vendor/supply pipeline for
    // an empty pantry) is about to run, and walking cancels eating.
    if (cfg.grindEnabled && state.masterGuid.IsEmpty() && !state.stay && !NeedsRecovery(bot, cfg))
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
                // Every wander cycle that did not END IN AN ENGAGEMENT counts
                // toward relocation - including cycles that walk toward a
                // prospect. Only actually reaching combat resets the counter,
                // so an unreachable prospect (across a chasm) can't pin the
                // bot in place forever.
                if (cfg.relocateEnabled && ++state.dryWanderCount >= cfg.relocateDryWanders)
                {
                    state.dryWanderCount = 0;
                    std::lock_guard<std::mutex> guard(g_PvePendingLock);
                    g_PendingGrindRelocations.insert(bot->GetGUID().GetRawValue());
                }
                // Walk toward the nearest attackable creature when one exists
                // beyond engage range; random-wander only in a truly empty
                // area. The engage-radius scan takes over on arrival.
                else if (Creature* prospect = FindGrindProspect(bot, state, cfg))
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
    g_PveConfig.buffsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Buffs.Enable", true);
    g_PveConfig.talentsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Talents.Enable", true);
    g_PveConfig.combatDiagnostics = sConfigMgr->GetBoolDefault("Playerbot.Pve.CombatDiagnostics", false);
    g_PveConfig.restUseConsumables = sConfigMgr->GetBoolDefault("Playerbot.Pve.Rest.UseConsumables", false);
    g_PveConfig.travelWalkMaxDistance = sConfigMgr->GetFloatDefault("Playerbot.Pve.Travel.WalkMaxDistance", 900.0f);
    g_PveConfig.travelUseFlightPaths = sConfigMgr->GetBoolDefault("Playerbot.Pve.Travel.UseFlightPaths", true);
    g_PveConfig.auctionBuyEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.AuctionBuy.Enable", false);
    g_PveConfig.auctionBuyBudgetPct = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionBuy.BudgetPct", 30), 1, 100));
    g_PveConfig.professionsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Professions.Enable", false);
    g_PveConfig.relocateEnabled = sConfigMgr->GetBoolDefault("Playerbot.PveGrind.Relocate.Enable", true);
    g_PveConfig.relocateDryWanders = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Playerbot.PveGrind.Relocate.DryWandersBeforeMove", 5), 2, 100));

    g_PveConfig.relocateMaps.clear();
    std::string const mapsCsv = sConfigMgr->GetStringDefault("Playerbot.PveGrind.Relocate.Maps", "0,1");
    std::stringstream mapsStream(mapsCsv);
    std::string token;
    while (std::getline(mapsStream, token, ','))
        if (!token.empty())
            g_PveConfig.relocateMaps.push_back(uint32(std::strtoul(token.c_str(), nullptr, 10)));
    if (g_PveConfig.relocateMaps.empty())
        g_PveConfig.relocateMaps = { 0, 1 };
    std::sort(g_PveConfig.relocateMaps.begin(), g_PveConfig.relocateMaps.end());
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
    ProcessPendingLootExecutions();
    ProcessPendingMailCollections();
    if (g_PveConfig.auctionBuyEnabled)
        ProcessPendingAuctionShopping();
    ProcessPendingSupplyRuns();
    if (g_PveConfig.relocateEnabled)
        ProcessPendingGrindRelocations();
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
        // Clear only the PvE bookkeeping: the PvP engine owns the bot's
        // target and combat state from here, and an AttackStop/SetTarget
        // would clobber a target it may already have acquired.
        PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);
        if (state.engaged)
        {
            playerbot::PvpCore::SetPveCombatEngagement(player->GetGUID(), false);
            state.engaged = false;
        }
        return;
    }

    if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
        return;

    PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);
    if (player->IsInFlight())
    {
        // Keep the post-landing journey's stuck detector quiet while the
        // taxi does the traveling.
        if (state.journeyActive)
            state.journeyProgressAt = PveClock::now();
        return;
    }
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
    g_PendingGrindRelocations.erase(rawGuid);
    g_PendingLootExecutions.erase(rawGuid);
    g_PendingSupplyRuns.erase(rawGuid);
    g_PendingMailCollections.erase(rawGuid);
    g_PendingAuctionShopping.erase(rawGuid);
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
    if (!g_PveConfig.enabled)
        return;

    if (!player || !playerbot::IsManagedRandomBot(player))
        return;

    if (g_PveConfig.autoLearnSpellsOnLevelUp)
    {
        uint32 const learned = RunTrainerSpellCatchup(player);
        if (learned)
            TC_LOG_DEBUG("playerbots.pve", "Managed bot {} learned {} trainer spells on reaching level {}.",
                player->GetGUID().ToString(), learned, player->GetLevel());
    }

    if (g_PveConfig.talentsEnabled)
        SpendPendingTalentPoints(player);
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
