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
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
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

Unit* PickGrindTarget(Player* bot, playerbot::PveConfig const& cfg)
{
    std::vector<Creature*> matches;
    GrindTargetCheck check{ bot, cfg };
    Trinity::CreatureListSearcher<GrindTargetCheck> searcher(bot, matches, check);
    Cell::VisitGridObjects(bot, searcher, cfg.grindSearchRadius);
    if (matches.empty())
        return nullptr;

    std::sort(matches.begin(), matches.end(), [bot](Creature* left, Creature* right)
    {
        return bot->GetDistance(left) < bot->GetDistance(right);
    });

    // Raycasts are the expensive part; only vet the closest handful.
    size_t const losProbeLimit = std::min<size_t>(matches.size(), 8);
    for (size_t index = 0; index < losProbeLimit; ++index)
        if (bot->IsWithinLOSInMap(matches[index]))
            return matches[index];

    return nullptr;
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
    for (auto const& [botRawGuid, pending] : snapshot)
    {
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
}

void RunFastTick(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
{
    Player* master = state.masterGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, state.masterGuid);

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
        }

        ExecuteEngagedCombatTick(bot);
        return;
    }

    if (state.engaged)
        DisengagePveCombat(bot, state);

    if (HasRestAura(bot))
    {
        bool const stillRecovering = bot->GetHealthPct() < 99.0f ||
            (bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 99.0f);
        bool const masterLeftRestRange = master && bot->GetDistance(master) > PveRestBreakFollowDistance;
        if (stillRecovering && !masterLeftRestRange)
            return;

        RemoveRestAuras(bot);
    }

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
            MotionMaster* motionMaster = bot->GetMotionMaster();
            bool const movementIdle = !motionMaster || motionMaster->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE;
            if (movementIdle && !bot->isMoving())
            {
                Position const destination = bot->GetRandomNearPosition(cfg.grindWanderRadius);
                playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                bot->GetMotionMaster()->MovePoint(0, destination, true);
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
