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

#include "PlayerbotRandomBotParticipation.h"

#include "PlayerbotObcClone.h"
#include "PlayerbotPvpCore.h"
#include "PlayerbotPvpClassActions.h"
#include "PlayerbotPvpLifecycleActions.h"

#include "AccountMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Configuration/Config.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "Realm.h"
#include "SpellDefines.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using ManagedBotAccountIds = std::vector<uint32>;

bool IsManagedRandomBotImpl(Player const* player, ManagedBotAccountIds const& botAccounts);

bool IsCrowdControlledForLifecyclePause(Player const* player)
{
    if (!player)
        return false;

    constexpr uint32 ccMechanicMask =
        (1u << MECHANIC_CHARM) |
        (1u << MECHANIC_DISORIENTED) |
        (1u << MECHANIC_FEAR) |
        (1u << MECHANIC_SLEEP) |
        (1u << MECHANIC_STUN) |
        (1u << MECHANIC_FREEZE) |
        (1u << MECHANIC_POLYMORPH) |
        (1u << MECHANIC_BANISH) |
        (1u << MECHANIC_HORROR) |
        (1u << MECHANIC_SAPPED);

    // UNIT_STATE_LOST_CONTROL also includes CHARGING and JUMPING. Those are
    // legitimate effect-driven movement states, not crowd control; treating
    // them as CC makes the 50 ms lifecycle tick clear charge/leap generators.
    return player->HasUnitState(UNIT_STATE_CONTROLLED | UNIT_STATE_POSSESSED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->HasAuraWithMechanic(ccMechanicMask) ||
        player->IsPolymorphed();
}

void ClearActiveMovementForControlLoss(Player* player)
{
    if (!player)
        return;

    player->AttackStop();
    player->SetSelection(ObjectGuid::Empty);
    // Preserve server-owned confused movement (e.g. polymorph drift). Clearing
    // active movement while confused pins the unit in place.
    if (player->HasUnitState(UNIT_STATE_CONFUSED) || player->HasAuraType(SPELL_AURA_MOD_CONFUSE) || player->IsPolymorphed())
        return;

    if (MotionMaster* motionMaster = player->GetMotionMaster())
        motionMaster->Clear(MOTION_SLOT_ACTIVE);
}

bool TryMageBlinkOutOfControl(Player* player)
{
    if (!playerbot::PvpCore::CanMageBlinkOutOfControl(player))
        return false;

    playerbot::PvpValues const values = playerbot::PvpCore::CollectValues(player);
    playerbot::PvpClassSpellContext const classContext = playerbot::PvpCore::BuildClassSpellContext(player, values);
    if (!classContext.classSpellsEnabled || !classContext.shouldExecute || classContext.spellId != 1953)
        return false;

    return playerbot::PvpClassActions::Execute(player, classContext);
}

bool TryHunterBestialWrathOutOfControl(Player* player)
{
    if (!playerbot::PvpCore::CanHunterBestialWrathOutOfControl(player))
        return false;

    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    if (!config.moduleEnabled || !config.pvpCoreEnabled || !config.pvpClassSpellsEnabled)
        return false;

    playerbot::PvpClassSpellContext context;
    context.classSpellsEnabled = true;
    context.shouldExecute = true;
    context.actionName = "hunter bestial wrath";
    context.reason = "fast-path break any removable crowd-control effect";
    context.spellId = 81300;
    context.selfCast = true;
    context.targetMode = playerbot::PvpClassSpellContext::TargetMode::Self;
    return playerbot::PvpClassActions::Execute(player, context);
}

bool IsLifecycleGateEnabled()
{
    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

using LifecycleCadenceClock = std::chrono::steady_clock;
using LifecycleCadenceTimePoint = LifecycleCadenceClock::time_point;

constexpr std::chrono::milliseconds RandomBotLifecycleCadenceInterval(500);
constexpr std::chrono::milliseconds PlayerbotInsigniaCheckInterval(50);
constexpr uint32 kPriestSpiritOfRedemptionSpellId = 81321;
constexpr uint32 kHolyPriestProfileTalentSpellId = 724;

std::unordered_map<uint64, LifecycleCadenceTimePoint> g_NextRandomBotLifecycleProcessTimeByGuid;
std::mutex g_RandomBotLifecycleCadenceLock;
// Player::Update can run concurrently for different maps. The PvP decision
// modules intentionally retain per-bot movement, targeting, and cooldown state
// in shared containers, so a complete decision tick must not overlap another
// map's tick and mutate those containers concurrently.
std::mutex g_PlayerbotPvpDecisionLock;
std::unordered_map<uint64, LifecycleCadenceTimePoint> g_NextPlayerbotInsigniaCheckTimeByGuid;
std::unordered_map<uint64, LifecycleCadenceTimePoint> g_PlayerbotInsigniaBreakableAuraFirstSeenTimeByGuid;
std::mutex g_PlayerbotInsigniaCheckLock;
std::unordered_set<uint64> g_StartupRevivedManagedBotGuids;
std::mutex g_StartupReviveLock;
bool g_StartupRevivePending = false;
uint32 g_LastForcedScmQueueSweepMs = 0;
constexpr uint32 PLAYERBOT_FORCED_SCM_QUEUE_SWEEP_THROTTLE_MS = 3000;

void EmitLifecycleGmDebug(Player const* player, std::string const& detail, uint32 throttleMs = 5000)
{
    (void)player;
    (void)detail;
    (void)throttleMs;
}

uint32 GetHumanInsigniaRacialSpell(uint8 classId)
{
    // Custom human racials mirror the five class-specific PvP insignia variants.
    switch (classId)
    {
        case CLASS_WARRIOR:
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return 89148;
        case CLASS_ROGUE:
        case CLASS_WARLOCK:
            return 89149;
        case CLASS_DRUID:
            return 89150;
        case CLASS_PRIEST:
        case CLASS_PALADIN:
            return 89151;
        case CLASS_MAGE:
            return 89152;
        default:
            return 0;
    }
}

bool HasClassInsigniaBreakableAura(Player const* player)
{
    if (!player)
        return false;

    bool const rooted = player->HasUnitState(UNIT_STATE_ROOT) ||
        player->HasAuraType(SPELL_AURA_MOD_ROOT) ||
        player->HasAuraWithMechanic(1u << MECHANIC_ROOT);
    bool const snared = player->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
        player->HasAuraWithMechanic(1u << MECHANIC_SNARE);
    bool const stunned = player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasAuraType(SPELL_AURA_MOD_STUN) ||
        player->HasAuraWithMechanic(1u << MECHANIC_STUN);
    bool const charmed = player->HasAuraType(SPELL_AURA_MOD_CHARM) ||
        player->HasAuraWithMechanic(1u << MECHANIC_CHARM);
    bool const feared = player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->HasAuraType(SPELL_AURA_MOD_FEAR) ||
        player->HasAuraWithMechanic(1u << MECHANIC_FEAR);
    bool const polymorphed = player->IsPolymorphed() ||
        player->HasAuraWithMechanic(1u << MECHANIC_POLYMORPH);

    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return rooted || snared || stunned;
        case CLASS_ROGUE:
        case CLASS_WARLOCK:
            return charmed || feared || polymorphed;
        case CLASS_DRUID:
            return charmed || feared || stunned;
        case CLASS_PRIEST:
        case CLASS_PALADIN:
            return feared || polymorphed || stunned;
        case CLASS_MAGE:
            return feared || polymorphed || snared;
        default:
            return false;
    }
}

bool IsPlayerbotInsigniaCheckReady(Player const* player)
{
    if (!player)
        return false;

    uint64 const playerGuid = player->GetGUID().GetRawValue();
    LifecycleCadenceTimePoint const now = LifecycleCadenceClock::now();

    std::lock_guard<std::mutex> lock(g_PlayerbotInsigniaCheckLock);
    LifecycleCadenceTimePoint& nextCheckTime = g_NextPlayerbotInsigniaCheckTimeByGuid[playerGuid];
    if (nextCheckTime > now)
        return false;

    nextCheckTime = now + PlayerbotInsigniaCheckInterval;
    return true;
}

bool HasPlayerbotInsigniaCcDelayElapsed(Player const* player, bool hasBreakableAura)
{
    if (!player)
        return false;

    uint64 const playerGuid = player->GetGUID().GetRawValue();
    LifecycleCadenceTimePoint const now = LifecycleCadenceClock::now();
    constexpr std::chrono::milliseconds insigniaUseDelay(500);

    std::lock_guard<std::mutex> lock(g_PlayerbotInsigniaCheckLock);
    if (!hasBreakableAura)
    {
        g_PlayerbotInsigniaBreakableAuraFirstSeenTimeByGuid.erase(playerGuid);
        return false;
    }

    auto insertResult = g_PlayerbotInsigniaBreakableAuraFirstSeenTimeByGuid.emplace(playerGuid, now);
    return now - insertResult.first->second >= insigniaUseDelay;
}

bool TryCastHumanInsigniaRacial(Player* player)
{
    uint32 const spellId = GetHumanInsigniaRacialSpell(player ? player->GetClass() : 0);
    if (!spellId || !player->HasSpell(spellId))
        return false;

    if (player->HasStealthAura())
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo || player->GetSpellHistory()->HasCooldown(spellId))
        return false;

    SpellCastResult const castResult = player->CastSpell(player, spellId, CastSpellExtraArgs(TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS)));
    TC_LOG_DEBUG("playerbots.pvp.class",
        "Playerbot PvP human insignia racial attempt: guid={} spell={} result={}.",
        player->GetGUID().ToString(), spellId, uint32(castResult));
    return castResult == SPELL_CAST_OK;
}

uint32 GetFirstOnUseItemSpell(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return 0;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& spellData = itemTemplate->Spells[i];
        if (spellData.SpellId > 0 && spellData.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            return spellData.SpellId;
    }

    return 0;
}

uint32 ResolveKnownPlayerSpellInChain(Player const* player, uint32 spellId)
{
    if (!player || !spellId)
        return 0;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!baseSpellInfo || !baseSpellInfo->GetFirstRankSpell())
        return 0;

    uint32 resolvedSpellId = 0;
    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
    {
        if (player->HasSpell(chainSpellId))
            resolvedSpellId = chainSpellId;
    }

    return resolvedSpellId;
}

bool TryCastPriestSpiritOfRedemption(Player* player)
{
    if (!player)
        return false;

    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    if (!config.moduleEnabled || !config.pvpCoreEnabled || !config.pvpClassSpellsEnabled)
        return false;

    if (!playerbot::IsManagedRandomBot(player) || !player->IsInWorld() || !player->IsAlive())
        return false;

    bool const inActiveBattleground = player->InBattleground() &&
        player->GetBattleground() &&
        player->GetBattleground()->GetStatus() == STATUS_IN_PROGRESS;
    bool const inActiveDuel = player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;
    if (!inActiveBattleground && !inActiveDuel)
        return false;

    if (player->GetClass() != CLASS_PRIEST || !player->HealthBelowPct(50))
        return false;

    if (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
        return false;

    uint8 const activeSpec = player->GetActiveSpec();
    if (!player->HasTalent(kHolyPriestProfileTalentSpellId, activeSpec))
        return false;

    if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
        return false;

    uint32 const spellId = ResolveKnownPlayerSpellInChain(player, kPriestSpiritOfRedemptionSpellId);
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo || player->GetSpellHistory()->HasCooldown(spellId))
        return false;

    SpellCastResult const castResult = player->CastSpell(player, spellId, CastSpellExtraArgs(TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS)));
    TC_LOG_DEBUG("playerbots.pvp.class",
        "Playerbot PvP spirit of redemption fast-path attempt: guid={} spell={} result={}.",
        player->GetGUID().ToString(), spellId, uint32(castResult));
    return castResult == SPELL_CAST_OK;
}

void WhisperInsigniaDiagnostic(Player const* player, std::string const& detail)
{
    if (!player || !player->duel || !player->duel->Opponent)
        return;

    WorldSession const* observerSession = player->duel->Opponent->GetSession();
    if (!observerSession || !observerSession->IsGmDiagnosticEnabled(GmDiagnosticCategory::Playerbot))
        return;

    const_cast<Player*>(player)->Whisper("INSIGNIA DIAG: " + detail, LANG_UNIVERSAL, player->duel->Opponent);
}

bool IsLikelyInsigniaTrinketItem(Item const* item)
{
    if (!item)
        return false;

    // Known Classic PvP insignia IDs. Keep this intentionally broad so custom
    // class variants in either trinket slot can use the same fast path.
    switch (item->GetEntry())
    {
        case 18834: // Insignia of the Horde
        case 18846: // Insignia of the Horde
        case 18849: // Insignia of the Horde
        case 18850: // Insignia of the Horde
        case 18851: // Insignia of the Horde
        case 18852: // Insignia of the Horde
        case 18853: // Insignia of the Horde
        case 18854: // Insignia of the Alliance
        case 18856: // Insignia of the Alliance
        case 18857: // Insignia of the Alliance
        case 18858: // Insignia of the Alliance
        case 18859: // Insignia of the Alliance
        case 18862: // Insignia of the Alliance
        case 18863: // Insignia of the Alliance
            return true;
        default:
            break;
    }

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate)
        return false;

    std::string const name = itemTemplate->Name1;
    return name.find("Insignia of the Horde") != std::string::npos ||
        name.find("Insignia of the Alliance") != std::string::npos;
}

bool TryUseEquippedInsigniaTrinketSlot(Player* player, EquipmentSlots slot)
{
    if (!player)
        return false;

    Item* trinket = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!trinket)
        return false;

    if (!IsLikelyInsigniaTrinketItem(trinket))
        return false;

    if (player->CanUseItem(trinket) != EQUIP_ERR_OK)
    {
        WhisperInsigniaDiagnostic(player, "slot=" + std::to_string(uint32(slot)) +
            " item=" + std::to_string(trinket->GetEntry()) + " result=item_unusable");
        return false;
    }

    uint32 const spellId = GetFirstOnUseItemSpell(trinket->GetTemplate());
    SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!spellInfo)
    {
        WhisperInsigniaDiagnostic(player, "slot=" + std::to_string(uint32(slot)) +
            " item=" + std::to_string(trinket->GetEntry()) + " result=no_on_use_spell");
        return false;
    }

    if (player->GetSpellHistory()->HasCooldown(spellId) ||
        player->GetSpellHistory()->HasCooldown(spellInfo, trinket->GetEntry()) ||
        player->GetSpellHistory()->HasGlobalCooldown(spellInfo))
        return false;

    SpellCastResult const castResult = player->CastSpell(player, spellId,
        CastSpellExtraArgs(TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CAST_IN_PROGRESS)).SetCastItem(trinket));

    TC_LOG_DEBUG("playerbots.pvp.class",
        "Playerbot PvP insignia trinket attempt: guid={} item={} spell={} slot={} result={}.",
        player->GetGUID().ToString(), trinket->GetEntry(), spellId, uint32(slot), uint32(castResult));

    WhisperInsigniaDiagnostic(player, "slot=" + std::to_string(uint32(slot)) +
        " item=" + std::to_string(trinket->GetEntry()) + " spell=" + std::to_string(spellId) +
        " result=" + std::to_string(uint32(castResult)));

    return castResult == SPELL_CAST_OK;
}

bool TryUseEquippedInsigniaTrinket(Player* player)
{
    if (!player)
        return false;

    // Check both trinket slots. The previous implementation only checked
    // EQUIPMENT_SLOT_TRINKET2 (slot 13), so a bot wearing item 18834 in the
    // top trinket slot (EQUIPMENT_SLOT_TRINKET1 / slot 12) never attempted it.
    if (TryUseEquippedInsigniaTrinketSlot(player, EQUIPMENT_SLOT_TRINKET1))
        return true;

    return TryUseEquippedInsigniaTrinketSlot(player, EQUIPMENT_SLOT_TRINKET2);
}

bool TryUsePlayerbotInsigniaBreaker(Player* player)
{
    if (!player)
        return false;

    playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
    if (!config.moduleEnabled || !config.pvpCoreEnabled || !config.pvpClassSpellsEnabled)
        return false;

    if (!playerbot::IsManagedRandomBot(player) || !player->IsInWorld() || !player->IsAlive())
        return false;

    bool const inActiveBattleground = player->InBattleground() &&
        player->GetBattleground() &&
        player->GetBattleground()->GetStatus() == STATUS_IN_PROGRESS;
    bool const inActiveDuel = player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;
    if (!inActiveBattleground && !inActiveDuel)
        return false;

    if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
        return false;

    if (!IsPlayerbotInsigniaCheckReady(player))
        return false;

    bool const hasBreakableAura = HasClassInsigniaBreakableAura(player);
    if (!HasPlayerbotInsigniaCcDelayElapsed(player, hasBreakableAura))
        return false;

    // Humans use the custom class-specific insignia racial from
    // GetHumanInsigniaRacialSpell() instead of looking for an equipped PvP
    // trinket. This keeps the same 500ms CC delay/cooldown-safe fast path,
    // but does not require a physical Insignia item in either trinket slot.
    if (player->GetRace() == RACE_HUMAN)
        return TryCastHumanInsigniaRacial(player);

    return TryUseEquippedInsigniaTrinket(player);
}

enum class LifecycleObservationReason : uint8
{
    GateDisabled = 0,
    CadenceThrottled,
    InvalidPlayerState,
    NoLifecycleHooksActive,
    BattlegroundLifecycleExecuted,
    ArenaLifecycleExecuted
};

struct LifecycleObservationCounters
{
    std::atomic<uint64> gateDisabled{ 0 };
    std::atomic<uint64> cadenceThrottled{ 0 };
    std::atomic<uint64> invalidPlayerState{ 0 };
    std::atomic<uint64> noLifecycleHooksActive{ 0 };
    std::atomic<uint64> battlegroundLifecycleExecuted{ 0 };
    std::atomic<uint64> arenaLifecycleExecuted{ 0 };
};

LifecycleObservationCounters g_LifecycleObservationCounters;

struct RandomBotPopulationConfig
{
    bool enabled = false;
    uint32 targetMin = 0;
    uint32 targetMax = 0;
    uint32 rebalanceIntervalMs = 15000;
    uint8 minLevel = 10;
    uint8 maxLevel = 80;
    uint8 allianceRatioPercent = 50;
    uint32 maxOnlineBotsPerAccount = 0;
    uint32 selectionHistorySize = 48;
    ManagedBotAccountIds botAccountIds;
};

struct RandomBotPoolCandidate
{
    uint32 lowGuid = 0;
    uint32 account = 0;
    uint8 level = 1;
    uint8 race = 0;
};

struct RandomBotPopulationState
{
    RandomBotPopulationConfig config;
    bool runtimeEnabled = false;
    bool startupBootstrapDone = false;
    bool rebalanceRequested = false;
    uint32 rebalanceTimerMs = 0;
    uint64 rebalanceEpoch = 0;
    uint64 rebalanceTicks = 0;
    uint64 loginAttempts = 0;
    uint64 loginSuccess = 0;
    uint64 logoutAttempts = 0;
    uint64 logoutSuccess = 0;
    uint64 skippedSafetyRealPlayers = 0;
    uint64 skippedNoCandidatePool = 0;
    uint64 skippedIntegrationGap = 0;
    uint64 lastRebalanceUnixTime = 0;
    std::deque<uint32> recentSelectedLowGuids;
    std::unordered_set<uint32> recentSelectedLowGuidSet;
};

RandomBotPopulationState g_RandomPopulation;
std::mutex g_RandomPopulationLock;

bool IsNoOp(playerbot::BattlegroundLifecycleContext const& context)
{
    return context.queueOperation == playerbot::QueueOperationType::None &&
        context.invitationResponse == playerbot::InvitationResponseType::None &&
        !context.shouldHandleInProgressStatus;
}

bool IsNoOp(playerbot::ArenaLifecycleContext const& context)
{
    return context.queueOperation == playerbot::QueueOperationType::None &&
        context.teamInteraction == playerbot::ArenaTeamInteractionType::None;
}

void ObserveLifecycleReason(LifecycleObservationReason reason, ObjectGuid const& guid)
{
    char const* reasonLabel = "unknown";
    uint64 total = 0;

    switch (reason)
    {
        case LifecycleObservationReason::GateDisabled:
            total = ++g_LifecycleObservationCounters.gateDisabled;
            reasonLabel = "gate-disabled";
            break;
        case LifecycleObservationReason::CadenceThrottled:
            total = ++g_LifecycleObservationCounters.cadenceThrottled;
            reasonLabel = "cadence-throttled";
            break;
        case LifecycleObservationReason::InvalidPlayerState:
            total = ++g_LifecycleObservationCounters.invalidPlayerState;
            reasonLabel = "invalid-player-state";
            break;
        case LifecycleObservationReason::NoLifecycleHooksActive:
            total = ++g_LifecycleObservationCounters.noLifecycleHooksActive;
            reasonLabel = "no-lifecycle-hooks-active";
            break;
        case LifecycleObservationReason::BattlegroundLifecycleExecuted:
            total = ++g_LifecycleObservationCounters.battlegroundLifecycleExecuted;
            reasonLabel = "battleground-lifecycle-executed";
            break;
        case LifecycleObservationReason::ArenaLifecycleExecuted:
            total = ++g_LifecycleObservationCounters.arenaLifecycleExecuted;
            reasonLabel = "arena-lifecycle-executed";
            break;
        default:
            break;
    }

    (void)reasonLabel;
    (void)guid;
    (void)total;
}

void LogLifecycleBranchSummary(ObjectGuid const& guid, char const* branchLabel)
{
    (void)guid;
    (void)branchLabel;
}

bool CanProcessPlayerLifecycle(Player const* player)
{
    if (!player)
    {
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, ObjectGuid::Empty);
        return false;
    }

    ObjectGuid const guid = player->GetGUID();

    if (!IsLifecycleGateEnabled())
    {
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle blocked: guid={} reason=gate-disabled",
            guid.ToString());
        EmitLifecycleGmDebug(player, "can-process=no gate-disabled");
        return false;
    }

    if (!playerbot::IsManagedRandomBot(player))
    {
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle blocked: guid={} reason=unmanaged-bot",
            guid.ToString());
        EmitLifecycleGmDebug(player, "can-process=no unmanaged-bot");
        return false;
    }

    if (!player->IsInWorld())
    {
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, guid);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle blocked: guid={} reason=not-in-world",
            guid.ToString());
        EmitLifecycleGmDebug(player, "can-process=no not-in-world");
        return false;
    }

    if (player->IsBeingTeleported())
    {
        bool const hasPendingTeleportAck = player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear();
        if (hasPendingTeleportAck)
        {
            ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, guid);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Lifecycle blocked: guid={} reason=pending-teleport far={} near={} bgId={} inBg={}",
                guid.ToString(),
                player->IsBeingTeleportedFar() ? 1 : 0,
                player->IsBeingTeleportedNear() ? 1 : 0,
                player->GetBattlegroundId(),
                player->InBattleground() ? 1 : 0);
            return false;
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle tolerated stale teleport flag: guid={} battlegroundId={} inBattleground={}",
            guid.ToString(), player->GetBattlegroundId(), player->InBattleground() ? 1 : 0);
    }

    uint64 const playerGuid = guid.GetRawValue();
    LifecycleCadenceTimePoint const now = LifecycleCadenceClock::now();
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    LifecycleCadenceTimePoint& nextProcessTime = g_NextRandomBotLifecycleProcessTimeByGuid[playerGuid];

    bool const inActiveBattleground = player->InBattleground() &&
        player->GetBattleground() &&
        player->GetBattleground()->GetStatus() == STATUS_IN_PROGRESS;
    std::chrono::milliseconds const cadenceInterval = RandomBotLifecycleCadenceInterval;

    if (nextProcessTime > now)
    {
        ObserveLifecycleReason(LifecycleObservationReason::CadenceThrottled, guid);
        auto const waitRemainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextProcessTime - now).count();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle blocked: guid={} reason=cadence-throttled waitMs={} cadenceMs={} bgActive={}",
            guid.ToString(), waitRemainingMs, cadenceInterval.count(), inActiveBattleground ? 1 : 0);
        std::ostringstream cadenceDetail;
        cadenceDetail << "can-process=no cadence-throttled wait_ms=" << waitRemainingMs
                      << " cadence_ms=" << cadenceInterval.count()
                      << " bg_active=" << (inActiveBattleground ? 1 : 0)
                      << " model=bg-fasttick-v3";
        EmitLifecycleGmDebug(player, cadenceDetail.str());
        return false;
    }

    nextProcessTime = now + cadenceInterval;

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Lifecycle allowed: guid={} inBg={} bgId={} inQueue={} teleport={} far={} near={}",
        guid.ToString(),
        player->InBattleground() ? 1 : 0,
        player->GetBattlegroundId(),
        player->InBattlegroundQueue() ? 1 : 0,
        player->IsBeingTeleported() ? 1 : 0,
        player->IsBeingTeleportedFar() ? 1 : 0,
        player->IsBeingTeleportedNear() ? 1 : 0);

    return true;
}

bool ConsumeCloneClassDecisionCadence(Player const* player)
{
    if (!player)
        return false;

    LifecycleCadenceTimePoint const now = LifecycleCadenceClock::now();
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    LifecycleCadenceTimePoint& nextProcessTime = g_NextRandomBotLifecycleProcessTimeByGuid[player->GetGUID().GetRawValue()];
    if (nextProcessTime > now)
        return false;

    nextProcessTime = now + RandomBotLifecycleCadenceInterval;
    return true;
}

void ProcessBattlegroundPlayerbotTick(Player* player)
{
    if (!player || !IsLifecycleGateEnabled())
        return;

    bool const activeClone = playerbot::PlayerbotObcCloneManager::IsActiveClone(player);
    if (!playerbot::IsManagedRandomBot(player) && !activeClone)
        return;

    if (!player->IsInWorld() || !player->InBattleground())
        return;

    Battleground* battleground = player->GetBattleground();
    if (!battleground)
        return;

    if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
        return;

    if (battleground->GetStatus() == STATUS_WAIT_JOIN)
    {
        // The preparation aura prevents player damage, but an aggressive pet
        // can still acquire an enemy or retain a command issued by a stale
        // combat context. Enforce the closed-gates invariant on every bot tick.
        if (Pet* pet = player->GetPet(); pet && pet->IsAlive() &&
            (pet->GetVictim() || pet->IsInCombat() || pet->HasUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE)))
        {
            if (CharmInfo* charmInfo = pet->GetCharmInfo())
            {
                charmInfo->SetIsCommandAttack(false);
                charmInfo->SetIsAtStay(false);
                charmInfo->SetIsCommandFollow(true);
                charmInfo->SetCommandState(COMMAND_FOLLOW);
            }
            pet->AttackStop();
            pet->CombatStop(true);
            if (MotionMaster* motionMaster = pet->GetMotionMaster())
                motionMaster->Clear(MOTION_SLOT_ACTIVE);
        }
    }

    // Persistent bots reach preparation through the normal lifecycle below.
    // Transient custom-game copies return before that lifecycle ownership, so
    // explicitly run their ordinary preparation decision graph while arena or
    // battleground gates are closed. Do not run tactical/movement actions here.
    if (battleground->GetStatus() == STATUS_WAIT_JOIN)
    {
        if (!activeClone || !ConsumeCloneClassDecisionCadence(player))
            return;

        playerbot::PvpValues const prepValues = playerbot::PvpCore::CollectValues(player);
        playerbot::PvpClassSpellContext const prepContext = playerbot::PvpCore::BuildClassSpellContext(player, prepValues);
        bool const didExecutePrep = playerbot::PvpClassActions::Execute(player, prepContext);

        std::ostringstream prepDetail;
        prepDetail << "bg-preparation clone_class_tick=1"
                   << " class_exec=" << (didExecutePrep ? 1 : 0)
                   << " class_spell=" << prepContext.spellId;
        EmitLifecycleGmDebug(player, prepDetail.str(), 1500);
        return;
    }

    if (battleground->GetStatus() != STATUS_IN_PROGRESS)
        return;

    // The normal class decision graph is deliberately skipped while crowd
    // controlled. Run the BM hunter's universal control break before that gate,
    // including roots/snares that do not pause the lifecycle tick.
    if (TryHunterBestialWrathOutOfControl(player))
    {
        EmitLifecycleGmDebug(player, "bg-fasttick escaped crowd-control with Bestial Wrath", 1000);
        return;
    }

    if (IsCrowdControlledForLifecyclePause(player))
    {
        ClearActiveMovementForControlLoss(player);
        bool const didBlink = TryMageBlinkOutOfControl(player);
        EmitLifecycleGmDebug(player, didBlink ? "bg-fasttick escaped crowd-control with Blink" : "bg-fasttick paused crowd-controlled", 1000);
        return;
    }

    playerbot::PvpValues const values = playerbot::PvpCore::CollectValues(player);
    playerbot::BattlegroundTacticalContext const tacticalContext = playerbot::PvpCore::BuildBattlegroundTacticalContext(player, values);
    bool const didExecuteTactical = playerbot::BattlegroundTacticalActions::Execute(player, tacticalContext);
    playerbot::PvpClassSpellContext const classContext = playerbot::PvpCore::BuildClassSpellContext(player, values);
    MotionMaster* motionMaster = player->GetMotionMaster();
    bool const movementIdle = !motionMaster || motionMaster->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE;
    bool const shouldForceClassMovementTick = classContext.classSpellsEnabled &&
        classContext.shouldExecute &&
        classContext.movementDirective != playerbot::PvpClassSpellContext::MovementDirective::None &&
        movementIdle;
    bool const shouldForcePetSpellTick = classContext.classSpellsEnabled &&
        classContext.shouldExecute &&
        playerbot::PvpClassActions::IsPetSpellAction(player, classContext);

    // Persistent bots reach the full lifecycle below on its normal cadence.
    // Transient custom-game copies deliberately return before queue/lifecycle
    // ownership, so give only those copies an equivalent class-decision cadence
    // here instead of limiting them to autoattacks and movement/pet actions.
    bool const runCloneClassDecision = activeClone && ConsumeCloneClassDecisionCadence(player);

    bool const didExecuteClassSpell = (runCloneClassDecision || shouldForceClassMovementTick || shouldForcePetSpellTick) &&
        playerbot::PvpClassActions::Execute(player, classContext);

    playerbot::BattlegroundLifecycleContext inProgressContext;
    inProgressContext.lifecycleEnabled = true;
    inProgressContext.queueOperation = playerbot::QueueOperationType::None;
    inProgressContext.invitationResponse = playerbot::InvitationResponseType::None;
    inProgressContext.shouldHandleInProgressStatus = true;
    bool const didExecuteLifecycle = playerbot::BattlegroundLifecycleActions::Execute(player, inProgressContext);

    std::ostringstream tickDetail;
    uint32 const bgTeam = player->GetBGTeam();
    uint32 const assignedTeam = battleground->GetPlayerTeam(player->GetGUID());
    tickDetail << "bg-fasttick tactical=" << (didExecuteTactical ? 1 : 0)
               << " class_force=" << (shouldForceClassMovementTick ? 1 : 0)
               << " clone_class_tick=" << (runCloneClassDecision ? 1 : 0)
               << " pet_spell_force=" << (shouldForcePetSpellTick ? 1 : 0)
               << " class_exec=" << (didExecuteClassSpell ? 1 : 0)
               << " class_directive=" << static_cast<uint32>(classContext.movementDirective)
               << " class_spell=" << classContext.spellId
               << " lifecycle=" << (didExecuteLifecycle ? 1 : 0)
               << " alive=" << (player->IsAlive() ? 1 : 0)
               << " rooted=" << (player->HasUnitState(UNIT_STATE_ROOT) ? 1 : 0)
               << " stunned=" << (player->HasUnitState(UNIT_STATE_STUNNED) ? 1 : 0)
               << " bgTeam=" << bgTeam
               << " assignedTeam=" << assignedTeam
               << " x=" << int32(player->GetPositionX())
               << " y=" << int32(player->GetPositionY());
    EmitLifecycleGmDebug(player, tickDetail.str(), 1500);
}

bool TryFinalizePendingVirtualPlayerTeleport(Player* player)
{
    if (!player || (!playerbot::IsManagedRandomBot(player) &&
        !playerbot::PlayerbotObcCloneManager::IsActiveClone(player)))
        return false;

    WorldSession* session = player->GetSession();
    if (!session || (!session->IsVirtualSession() && !session->IsTransientPlayerSession()))
        return false;

    bool finalizedTeleport = false;

    if (player->IsBeingTeleportedFar())
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot lifecycle pre-check teleport finalization: guid={} type=far.",
            player->GetGUID().ToString());
        session->HandleMoveWorldportAck();
        finalizedTeleport = true;
    }

    if (player->IsBeingTeleportedNear())
    {
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot lifecycle pre-check teleport finalization: guid={} type=near.",
            player->GetGUID().ToString());

        player->StopMoving();
        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear();

        WorldPacket teleportAck(MSG_MOVE_TELEPORT_ACK, 20);
        teleportAck << player->GetPackGUID();
        teleportAck << uint32(0);
        teleportAck << uint32(0);
        session->HandleMoveTeleportAck(teleportAck);

        if (player->IsBeingTeleportedNear())
        {
            uint32 const oldZone = player->GetZoneId();
            WorldLocation dest = player->GetTeleportDest();
            float safeDestZ = dest.GetPositionZ();
            player->UpdateAllowedPositionZ(dest.GetPositionX(), dest.GetPositionY(), safeDestZ);
            dest.Relocate(dest.GetPositionX(), dest.GetPositionY(), safeDestZ, dest.GetOrientation());
            player->SetSemaphoreTeleportNear(false);
            player->UpdatePosition(dest, true);

            uint32 newZone = 0;
            uint32 newArea = 0;
            player->GetZoneAndAreaId(newZone, newArea);
            player->UpdateZone(newZone, newArea);

            if (oldZone != newZone)
            {
                if (player->pvpInfo.IsHostile)
                    player->CastSpell(player, 2479, true);
                else if (player->IsPvP() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
                    player->UpdatePvP(false, false);
            }

            player->ResummonPetTemporaryUnSummonedIfAny();
            player->ProcessDelayedOperations();
        }

        finalizedTeleport = true;
    }

    return finalizedTeleport;
}

void RecoverManagedVirtualBotTeleports(ManagedBotAccountIds const& botAccounts)
{
    std::vector<ObjectGuid> managedTeleportGuids;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !IsManagedRandomBotImpl(player, botAccounts))
                continue;

            WorldSession const* session = player->GetSession();
            if (!session || !session->IsVirtualSession())
                continue;

            // Near teleports are completed by ProcessPlayerLifecycle on the
            // player's map-update thread so no world-thread recovery can race
            // the following combat/movement tick. Far transfers may remove the
            // player from normal map updates and still require world recovery.
            if (!player->IsBeingTeleportedFar())
                continue;

            managedTeleportGuids.push_back(guid);
        }
    }

    for (ObjectGuid const& guid : managedTeleportGuids)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            TryFinalizePendingVirtualPlayerTeleport(player);
}

void TryReviveManagedBotAfterStartup(Player* player)
{
    if (!player || !playerbot::IsManagedRandomBot(player))
        return;

    bool shouldAttemptRevive = false;
    {
        std::lock_guard<std::mutex> startupReviveLock(g_StartupReviveLock);
        if (!g_StartupRevivePending)
            return;

        auto const [_, inserted] = g_StartupRevivedManagedBotGuids.emplace(player->GetGUID().GetRawValue());
        shouldAttemptRevive = inserted;
    }

    if (!shouldAttemptRevive || player->IsAlive())
        return;

    player->ResurrectPlayer(1.0f);
    TC_LOG_INFO("playerbots.population", "Startup managed bot revive applied: guid={}", player->GetGUID().ToString());
}

bool IsChromieWhisperFacade(Player const* player)
{
    if (!player)
        return false;

    std::string const& name = player->GetName();
    return name == "Chromie" || name == "Chromi";
}

uint32 ResolvePlayerAccountId(Player const* player)
{
    if (!player)
        return 0;

    if (WorldSession const* session = player->GetSession())
        return session->GetAccountId();

    return sCharacterCache->GetCharacterAccountIdByGuid(player->GetGUID());
}

bool IsManagedRandomBotImpl(Player const* player, ManagedBotAccountIds const& botAccounts)
{
    if (!player)
        return false;

    if (IsChromieWhisperFacade(player))
        return false;

    if (WorldSession const* session = player->GetSession())
    {
        // In-memory clone players and temporary database-only clone sources
        // deliberately use socketless sessions, but they are not managed
        // playerbots.  Sending them through login/teleport recovery assumes
        // account, social, and other runtime state that transient players do
        // not own.
        if (session->IsTransientPlayerSession())
            return false;

        if (session->IsVirtualSession())
            return true;
    }

    if (botAccounts.empty())
        return false;

    uint32 const accountId = ResolvePlayerAccountId(player);
    if (!accountId || !std::binary_search(botAccounts.begin(), botAccounts.end(), accountId))
        return false;

    // BotAccountIds remains an explicit allow-list for non-virtual sessions.
    return true;
}

ManagedBotAccountIds GetManagedBotAccountIdsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    return g_RandomPopulation.config.botAccountIds;
}

bool IsRandomBotCandidate(Player const* player, ManagedBotAccountIds const& botAccounts)
{
    if (playerbot::PlayerbotObcCloneManager::IsActiveClone(player))
        return false;

    return IsManagedRandomBotImpl(player, botAccounts);
}

bool ShouldEmitRebalanceErrorDiagnostics(RandomBotPopulationState const& state)
{
    return state.rebalanceTicks <= 10 || (state.rebalanceTicks % 50) == 0;
}

struct OnlineRandomBotMetrics
{
    uint32 total = 0;
    uint32 alliance = 0;
    uint32 horde = 0;
    std::vector<ObjectGuid> guids;
};

bool HasAnyRealHumanInterestInBattleground(BattlegroundTypeId targetBgType, ManagedBotAccountIds const& botAccounts)
{
    if (targetBgType == BATTLEGROUND_TYPE_NONE)
        return false;

    BattlegroundQueueTypeId const targetQueueType = BattlegroundMgr::BGQueueTypeId(targetBgType, 0);
    TC_LOG_ERROR("playerbots.population",
        "DIAG startup-hang: Begin HasAnyRealHumanInterestInBattleground bgTypeId={} botAccounts={}.",
        uint32(targetBgType), botAccounts.size());

    std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
    uint32 scannedParticipants = 0;
    for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
    {
        ++scannedParticipants;
        if (!participant)
            continue;

        WorldSession const* session = participant->GetSession();
        bool const isVirtualSession = session && session->IsVirtualSession();
        if (isVirtualSession || IsManagedRandomBotImpl(participant, botAccounts))
            continue;

        if (participant->InBattleground() && participant->GetBattlegroundTypeId() == targetBgType &&
            participant->GetBattleground() && !participant->GetBattleground()->IsCustomGame())
        {
            TC_LOG_ERROR("playerbots.population",
                "DIAG startup-hang: Human battleground interest found from active participant guid={} bgTypeId={} scanned={}.",
                participant->GetGUID().ToString(), uint32(targetBgType), scannedParticipants);
            return true;
        }

        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            if (participant->GetBattlegroundQueueTypeId(i) == targetQueueType)
            {
                TC_LOG_ERROR("playerbots.population",
                    "DIAG startup-hang: Human battleground interest found from queue participant guid={} bgTypeId={} scanned={}.",
                    participant->GetGUID().ToString(), uint32(targetBgType), scannedParticipants);
                return true;
            }
        }
    }

    TC_LOG_ERROR("playerbots.population",
        "DIAG startup-hang: End HasAnyRealHumanInterestInBattleground bgTypeId={} scanned={} no-interest.",
        uint32(targetBgType), scannedParticipants);
    return false;
}

void ForceManagedScmQueueSweep(ManagedBotAccountIds const& botAccounts)
{
    constexpr BattlegroundTypeId kManagedBattleground = BATTLEGROUND_SCM;
    if (!HasAnyRealHumanInterestInBattleground(kManagedBattleground, botAccounts))
        return;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    if (nowMs < g_LastForcedScmQueueSweepMs + PLAYERBOT_FORCED_SCM_QUEUE_SWEEP_THROTTLE_MS)
        return;

    g_LastForcedScmQueueSweepMs = nowMs;

    playerbot::BattlegroundLifecycleContext joinContext;
    joinContext.lifecycleEnabled = true;
    joinContext.queueOperation = playerbot::QueueOperationType::Join;
    joinContext.invitationResponse = playerbot::InvitationResponseType::None;
    joinContext.shouldHandleInProgressStatus = false;

    playerbot::BattlegroundLifecycleContext acceptContext;
    acceptContext.lifecycleEnabled = true;
    acceptContext.queueOperation = playerbot::QueueOperationType::None;
    acceptContext.invitationResponse = playerbot::InvitationResponseType::Accept;
    acceptContext.shouldHandleInProgressStatus = false;

    std::vector<ObjectGuid> managedGuids;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->IsInWorld())
                continue;

            if (!IsManagedRandomBotImpl(player, botAccounts))
                continue;

            managedGuids.push_back(guid);
        }
    }

    for (ObjectGuid const& guid : managedGuids)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
        {
            playerbot::BattlegroundLifecycleActions::Execute(player, joinContext);
            playerbot::BattlegroundLifecycleActions::Execute(player, acceptContext);
        }
}

OnlineRandomBotMetrics CollectOnlineRandomBotMetrics(ManagedBotAccountIds const& botAccounts)
{
    OnlineRandomBotMetrics metrics;

    std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (!IsRandomBotCandidate(player, botAccounts))
            continue;

        ++metrics.total;
        if (player->GetTeamId() == TEAM_ALLIANCE)
            ++metrics.alliance;
        else if (player->GetTeamId() == TEAM_HORDE)
            ++metrics.horde;

        metrics.guids.push_back(guid);
    }

    return metrics;
}

std::vector<RandomBotPoolCandidate> QueryOfflinePool(RandomBotPopulationConfig const& config)
{
    std::vector<RandomBotPoolCandidate> candidates;

    if (config.botAccountIds.empty())
        return candidates;

    std::string accountList;
    accountList.reserve(config.botAccountIds.size() * 6);
    for (uint32 accountId : config.botAccountIds)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    QueryResult result = CharacterDatabase.PQuery(
        "SELECT guid, account, level, race FROM characters "
        "WHERE online = 0 AND account IN ({}) AND name NOT LIKE 'Obcc%' AND level >= {} AND level <= {}",
        accountList, config.minLevel, config.maxLevel);

    if (!result)
        return candidates;

    do
    {
        Field* fields = result->Fetch();
        RandomBotPoolCandidate candidate;
        candidate.lowGuid = fields[0].GetUInt32();
        candidate.account = fields[1].GetUInt32();
        candidate.level = fields[2].GetUInt8();
        candidate.race = fields[3].GetUInt8();
        candidates.push_back(candidate);
    }
    while (result->NextRow());

    return candidates;
}

std::unordered_map<uint32, uint32> QueryOnlineBotCountsByAccount(RandomBotPopulationConfig const& config)
{
    std::unordered_map<uint32, uint32> onlineByAccount;
    if (config.botAccountIds.empty())
        return onlineByAccount;

    std::string accountList;
    accountList.reserve(config.botAccountIds.size() * 6);
    for (uint32 accountId : config.botAccountIds)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    QueryResult result = CharacterDatabase.PQuery(
        "SELECT account, COUNT(*) FROM characters WHERE online = 1 AND account IN ({}) GROUP BY account",
        accountList);
    if (!result)
        return onlineByAccount;

    do
    {
        Field* fields = result->Fetch();
        onlineByAccount[fields[0].GetUInt32()] = fields[1].GetUInt32();
    } while (result->NextRow());

    return onlineByAccount;
}

void TrackRecentSelection(RandomBotPopulationState& state, uint32 lowGuid)
{
    if (!lowGuid)
        return;

    uint32 const historyLimit = std::max<uint32>(state.config.selectionHistorySize, 1);

    if (state.recentSelectedLowGuidSet.find(lowGuid) == state.recentSelectedLowGuidSet.end())
    {
        state.recentSelectedLowGuids.push_back(lowGuid);
        state.recentSelectedLowGuidSet.insert(lowGuid);
    }

    while (state.recentSelectedLowGuids.size() > historyLimit)
    {
        uint32 const dropped = state.recentSelectedLowGuids.front();
        state.recentSelectedLowGuids.pop_front();
        state.recentSelectedLowGuidSet.erase(dropped);
    }
}

uint64 ComputeDeterministicCandidateScore(RandomBotPoolCandidate const& candidate, uint64 epoch, bool wasRecent)
{
    uint64 score = (static_cast<uint64>(candidate.lowGuid) * 2654435761ULL) ^ (epoch * 11400714819323198485ULL);
    score ^= static_cast<uint64>(candidate.account) << 17;
    score ^= static_cast<uint64>(candidate.level) << 9;
    if (wasRecent)
        score = std::numeric_limits<uint64>::max() - score;

    return score;
}

std::vector<RandomBotPoolCandidate> PickLoginCandidates(RandomBotPopulationState const& state,
    std::vector<RandomBotPoolCandidate> pool, uint32 requiredCount, uint32 currentAllianceOnline, uint32 currentHordeOnline,
    std::unordered_map<uint32, uint32> onlineByAccount)
{
    if (!requiredCount || pool.empty())
        return {};

    std::stable_sort(pool.begin(), pool.end(), [&](RandomBotPoolCandidate const& left, RandomBotPoolCandidate const& right)
    {
        bool const leftRecent = state.recentSelectedLowGuidSet.find(left.lowGuid) != state.recentSelectedLowGuidSet.end();
        bool const rightRecent = state.recentSelectedLowGuidSet.find(right.lowGuid) != state.recentSelectedLowGuidSet.end();
        uint64 const leftScore = ComputeDeterministicCandidateScore(left, state.rebalanceEpoch + 1, leftRecent);
        uint64 const rightScore = ComputeDeterministicCandidateScore(right, state.rebalanceEpoch + 1, rightRecent);
        return leftScore < rightScore;
    });

    std::vector<RandomBotPoolCandidate> selected;
    selected.reserve(std::min<uint32>(requiredCount, static_cast<uint32>(pool.size())));

    uint32 projectedAlliance = currentAllianceOnline;
    uint32 projectedHorde = currentHordeOnline;

    for (RandomBotPoolCandidate const& candidate : pool)
    {
        if (selected.size() >= requiredCount)
            break;

        TeamId const candidateTeam = Player::TeamForRace(candidate.race) == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE;
        uint32 const totalProjected = projectedAlliance + projectedHorde;
        uint32 const alliancePercent = totalProjected ? static_cast<uint32>((projectedAlliance * 100) / totalProjected) : 50;

        bool shouldTake = true;
        if (candidateTeam == TEAM_ALLIANCE && alliancePercent > state.config.allianceRatioPercent + 10)
            shouldTake = false;
        if (candidateTeam == TEAM_HORDE && alliancePercent + 10 < state.config.allianceRatioPercent)
            shouldTake = false;

        if (!shouldTake)
            continue;

        if (state.config.maxOnlineBotsPerAccount > 0)
        {
            uint32 const onlineOnAccount = onlineByAccount[candidate.account];
            if (onlineOnAccount >= state.config.maxOnlineBotsPerAccount)
                continue;
            onlineByAccount[candidate.account] = onlineOnAccount + 1;
        }

        selected.push_back(candidate);
        if (candidateTeam == TEAM_ALLIANCE)
            ++projectedAlliance;
        else
            ++projectedHorde;
    }

    for (RandomBotPoolCandidate const& candidate : pool)
    {
        if (selected.size() >= requiredCount)
            break;

        auto const exists = std::find_if(selected.begin(), selected.end(), [&](RandomBotPoolCandidate const& value)
        {
            return value.lowGuid == candidate.lowGuid;
        });

        if (exists == selected.end())
            selected.push_back(candidate);
    }

    return selected;
}

bool SupportsLoginOrchestration()
{
    return true;
}

uint32 AcquireVirtualSessionKeyForCandidate(uint32 lowGuid)
{
    // Use a reserved high-bit namespace and probe to avoid rare collisions when
    // multiple character GUIDs map to the same synthesized session key.
    constexpr uint32 kVirtualSessionNamespaceBit = 0x80000000u;
    constexpr uint32 kVirtualSessionProbeLimit = 64u;

    if (!lowGuid)
        return 0;

    uint32 candidateKey = kVirtualSessionNamespaceBit | lowGuid;
    for (uint32 probe = 0; probe < kVirtualSessionProbeLimit; ++probe)
    {
        if (!sWorld->FindSession(candidateKey))
            return candidateKey;

        ++candidateKey;
        if (!(candidateKey & kVirtualSessionNamespaceBit))
            candidateKey = kVirtualSessionNamespaceBit;
    }

    return 0;
}

bool TryLoginBotCharacter(RandomBotPoolCandidate const& candidate)
{
    if (!candidate.lowGuid || !candidate.account)
    {
        TC_LOG_DEBUG("playerbots.population", "Random bot login skipped due to invalid candidate payload (guidLow={}, account={}).",
            candidate.lowGuid, candidate.account);
        return false;
    }

    uint32 const virtualSessionKey = AcquireVirtualSessionKeyForCandidate(candidate.lowGuid);
    if (!virtualSessionKey)
    {
        TC_LOG_WARN("playerbots.population",
            "Random bot login skipped: failed to allocate a virtual session key (candidate guidLow={} account={}).",
            candidate.lowGuid, candidate.account);
        return false;
    }

    std::string accountName;
    if (!sAccountMgr->GetName(candidate.account, accountName))
    {
        TC_LOG_DEBUG("playerbots.population", "Random bot login failed: unable to resolve account name for account {} (candidate guidLow={}).",
            candidate.account, candidate.lowGuid);
        return false;
    }

    ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(candidate.lowGuid);
    int32 const realmId = static_cast<int32>(realm.Id.Realm);
    AccountTypes const security = static_cast<AccountTypes>(sAccountMgr->GetSecurity(candidate.account, realmId));
    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    WorldSession* session = new WorldSession(candidate.account, std::move(accountName), nullptr, security, expansion, 0, Minutes(0),
        LOCALE_enUS, 0, false);
    session->SetSessionMapKey(virtualSessionKey);
    sWorld->AddSession(session);
    session->AllowCharacterLogin(playerGuid);

    WorldPacket loginPacket(CMSG_PLAYER_LOGIN, 8);
    loginPacket << playerGuid;
    session->HandlePlayerLoginOpcode(loginPacket);

    // HandlePlayerLoginOpcode loads the character through an asynchronous query
    // holder. A null session player here means the login is pending, not that it
    // failed. The session update will materialize the player on a later world
    // tick; do not terminate the virtual session while that work is in flight.
    TC_LOG_INFO("playerbots.population", "Random bot login dispatched: guid={} guidLow={} account={} level={}.",
        playerGuid.ToString(), candidate.lowGuid, candidate.account, candidate.level);
    return true;
}

bool TryLogoutRandomBot(ObjectGuid const& guid)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    if (!player)
    {
        TC_LOG_WARN("playerbots.population", "Random bot logout skipped: guid {} is no longer online.", guid.ToString());
        return false;
    }

    if (playerbot::PlayerbotObcCloneManager::IsActiveClone(player))
    {
        TC_LOG_WARN("playerbots.population", "Random bot logout skipped: guid {} is an in-memory OBC clone.", guid.ToString());
        return false;
    }

    if (!playerbot::IsManagedRandomBot(player))
    {
        TC_LOG_WARN("playerbots.population", "Random bot logout safety skip: guid {} account {} is not a managed random bot.",
            guid.ToString(), ResolvePlayerAccountId(player));
        return false;
    }

    WorldSession* session = player->GetSession();
    if (!session)
    {
        TC_LOG_WARN("playerbots.population", "Random bot logout skipped: managed bot guid {} account {} has no world session. TODO: add sessionless bot eviction adapter.",
            guid.ToString(), ResolvePlayerAccountId(player));
        return false;
    }

    TC_LOG_INFO("playerbots.population", "Random bot logout dispatched: guid={} account={}.", guid.ToString(), session->GetAccountId());
    session->LogoutPlayer(true);
    session->KickPlayer("Random bot population manager logout");
    return true;
}

bool RebalanceRandomPopulation(RandomBotPopulationState& state)
{
    constexpr BattlegroundTypeId kManagedBattleground = BATTLEGROUND_SCM;

    state.rebalanceEpoch++;
    state.rebalanceTicks++;
    state.lastRebalanceUnixTime = static_cast<uint64>(GameTime::GetGameTime());
    bool const emitDiag = ShouldEmitRebalanceErrorDiagnostics(state);
    if (emitDiag)
    {
        TC_LOG_ERROR("playerbots.population",
            "DIAG startup-hang: Rebalance start epoch={} tick={} runtimeEnabled={} configEnabled={} accountPool={}.",
            state.rebalanceEpoch, state.rebalanceTicks, state.runtimeEnabled ? 1 : 0, state.config.enabled ? 1 : 0,
            state.config.botAccountIds.size());
    }

    OnlineRandomBotMetrics const online = CollectOnlineRandomBotMetrics(state.config.botAccountIds);
    if (emitDiag)
    {
        TC_LOG_ERROR("playerbots.population",
            "DIAG startup-hang: Rebalance after CollectOnlineRandomBotMetrics total={} alliance={} horde={}.",
            online.total, online.alliance, online.horde);
    }

    uint32 target = state.config.targetMin;
    if (state.config.targetMax > state.config.targetMin)
    {
        uint64 const span = static_cast<uint64>(state.config.targetMax - state.config.targetMin + 1);
        target = state.config.targetMin + static_cast<uint32>(state.rebalanceEpoch % span);
    }

    if (emitDiag)
        TC_LOG_ERROR("playerbots.population", "DIAG startup-hang: Rebalance before human-interest check bgTypeId={}.", uint32(kManagedBattleground));
    bool const hasHumanInterest = HasAnyRealHumanInterestInBattleground(kManagedBattleground, state.config.botAccountIds);
    if (hasHumanInterest)
    {
        if (Battleground* battlegroundTemplate = sBattlegroundMgr->GetBattlegroundTemplate(kManagedBattleground))
        {
            uint32 const fillTarget = battlegroundTemplate->GetMaxPlayersPerTeam() * 2u;
            if (fillTarget > target)
                target = fillTarget;
        }
    }
    if (emitDiag)
        TC_LOG_ERROR("playerbots.population", "DIAG startup-hang: Rebalance after human-interest check target={}.", target);

    uint32 effectiveOnlineCount = online.total;

    TC_LOG_INFO("playerbots.population", "Random bot population rebalance tick={} online={} target={} range=[{}, {}] enabled={} runtime={}",
        state.rebalanceTicks, online.total, target, state.config.targetMin, state.config.targetMax,
        state.config.enabled ? 1 : 0, state.runtimeEnabled ? 1 : 0);

    if (effectiveOnlineCount < target)
    {
        uint32 const needed = target - effectiveOnlineCount;
        if (!SupportsLoginOrchestration())
        {
            state.skippedIntegrationGap += needed;
            TC_LOG_WARN("playerbots.population",
                "Random bot login orchestration is not supported in this build/runtime path; skipping {} planned login attempts.",
                needed);
            return false;
        }

        std::vector<RandomBotPoolCandidate> pool = QueryOfflinePool(state.config);
        std::unordered_map<uint32, uint32> const onlineByAccount = QueryOnlineBotCountsByAccount(state.config);
        if (pool.empty())
        {
            state.skippedNoCandidatePool++;
            return false;
        }

        std::vector<RandomBotPoolCandidate> const selection = PickLoginCandidates(state, pool, needed, online.alliance, online.horde, onlineByAccount);
        for (RandomBotPoolCandidate const& candidate : selection)
        {
            state.loginAttempts++;
            TrackRecentSelection(state, candidate.lowGuid);
            if (TryLoginBotCharacter(candidate))
                state.loginSuccess++;
            else
                state.skippedIntegrationGap++;
        }

        return !selection.empty();
    }

    if (!hasHumanInterest && online.total > target)
    {
        uint32 const excess = online.total - target;
        uint32 processed = 0;
        for (ObjectGuid const& guid : online.guids)
        {
            if (processed >= excess)
                break;

            if (Player const* player = ObjectAccessor::FindConnectedPlayer(guid))
            {
                if (!IsManagedRandomBotImpl(player, state.config.botAccountIds))
                {
                    state.skippedSafetyRealPlayers++;
                    TC_LOG_WARN("playerbots.population", "Random bot logout safety skip: guid={} account={} is not a managed random bot.",
                        guid.ToString(), ResolvePlayerAccountId(player));
                    continue;
                }
            }

            state.logoutAttempts++;
            if (TryLogoutRandomBot(guid))
                state.logoutSuccess++;
            else
                state.skippedIntegrationGap++;

            ++processed;
        }

        return processed > 0;
    }

    return false;
}

void LoadPopulationConfigLocked(RandomBotPopulationState& state)
{
    RandomBotPopulationConfig config;
    config.enabled = sConfigMgr->GetBoolDefault("Playerbot.RandomPopulation.Enable", false);
    config.targetMin = std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.TargetMin", 0));
    config.targetMax = std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.TargetMax", 0));
    config.rebalanceIntervalMs = std::max<int32>(1000, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.RebalanceIntervalMs", 15000));
    config.minLevel = static_cast<uint8>(std::clamp<int32>(sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.MinLevel", 10), 1, 80));
    config.maxLevel = static_cast<uint8>(std::clamp<int32>(sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.MaxLevel", 80), 1, 80));
    config.allianceRatioPercent = static_cast<uint8>(std::clamp<int32>(sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.AllianceRatioPercent", 50), 0, 100));
    config.maxOnlineBotsPerAccount = std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.MaxOnlineBotsPerAccount", 0));
    config.selectionHistorySize = std::max<int32>(1, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.SelectionHistorySize", 48));

    if (config.targetMax < config.targetMin)
        std::swap(config.targetMin, config.targetMax);
    if (config.maxLevel < config.minLevel)
        std::swap(config.maxLevel, config.minLevel);

    std::string const rawBotAccounts = sConfigMgr->GetStringDefault("Playerbot.RandomPopulation.BotAccountIds", "");
    for (std::string_view token : Trinity::Tokenize(rawBotAccounts, ',', false))
        if (Optional<uint32> accountId = Trinity::StringTo<uint32>(token))
            if (*accountId > 0)
                config.botAccountIds.push_back(*accountId);

    std::sort(config.botAccountIds.begin(), config.botAccountIds.end());
    config.botAccountIds.erase(std::unique(config.botAccountIds.begin(), config.botAccountIds.end()), config.botAccountIds.end());

    state.config = std::move(config);
    state.runtimeEnabled = state.config.enabled;
    state.rebalanceTimerMs = 0;
    state.rebalanceRequested = false;

    while (state.recentSelectedLowGuids.size() > state.config.selectionHistorySize)
    {
        uint32 const guid = state.recentSelectedLowGuids.front();
        state.recentSelectedLowGuids.pop_front();
        state.recentSelectedLowGuidSet.erase(guid);
    }

    TC_LOG_INFO("playerbots.population", "Random bot population config loaded: enabled={}, targetMin={}, targetMax={}, intervalMs={}, levelRange=[{}, {}], allianceRatio={}, maxPerAccount={}, accountPoolSize={}.",
        state.config.enabled ? 1 : 0, state.config.targetMin, state.config.targetMax, state.config.rebalanceIntervalMs,
        state.config.minLevel, state.config.maxLevel, state.config.allianceRatioPercent, state.config.maxOnlineBotsPerAccount, state.config.botAccountIds.size());
}
}

namespace playerbot
{
bool IsManagedRandomBot(Player const* player)
{
    return IsManagedRandomBotImpl(player, GetManagedBotAccountIdsSnapshot());
}

void RandomBotParticipationManager::ResetCadence()
{
    std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
    g_NextRandomBotLifecycleProcessTimeByGuid.clear();

    std::lock_guard<std::mutex> startupReviveLock(g_StartupReviveLock);
    g_StartupRevivePending = false;
    g_StartupRevivedManagedBotGuids.clear();
}

void RandomBotParticipationManager::LoadPopulationConfig()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    LoadPopulationConfigLocked(g_RandomPopulation);
}

void RandomBotParticipationManager::OnStartupBootstrap()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    if (!g_RandomPopulation.startupBootstrapDone)
    {
        g_RandomPopulation.startupBootstrapDone = true;
        g_RandomPopulation.rebalanceRequested = true;

        std::lock_guard<std::mutex> startupReviveLock(g_StartupReviveLock);
        g_StartupRevivePending = true;
        g_StartupRevivedManagedBotGuids.clear();
    }
}

void RandomBotParticipationManager::OnWorldUpdate(uint32 diffMs)
{
    ManagedBotAccountIds botAccountsForSweep;
    bool shouldRunScmSweep = false;

    {
        std::lock_guard<std::mutex> lock(g_RandomPopulationLock);

        if (!g_RandomPopulation.config.enabled || !g_RandomPopulation.runtimeEnabled)
            return;

        if (g_RandomPopulation.config.botAccountIds.empty())
        {
            g_RandomPopulation.skippedNoCandidatePool++;
            return;
        }

        botAccountsForSweep = g_RandomPopulation.config.botAccountIds;

        if (g_RandomPopulation.rebalanceTimerMs < g_RandomPopulation.config.rebalanceIntervalMs)
            g_RandomPopulation.rebalanceTimerMs += diffMs;

        if (g_RandomPopulation.rebalanceTimerMs < g_RandomPopulation.config.rebalanceIntervalMs && !g_RandomPopulation.rebalanceRequested)
        {
            // Still run teleport recovery below even when no rebalance/sweep is due.
        }
        else
        {
            g_RandomPopulation.rebalanceTimerMs = 0;
            g_RandomPopulation.rebalanceRequested = false;
            RebalanceRandomPopulation(g_RandomPopulation);
            shouldRunScmSweep = true;
        }
    }

    RecoverManagedVirtualBotTeleports(botAccountsForSweep);

    // Avoid running the SCM sweep while holding g_RandomPopulationLock:
    // lifecycle queue operations can call TriggerImmediateRebalance(), which
    // re-enters this mutex and can deadlock the world update thread.
    if (!shouldRunScmSweep)
        return;

    ForceManagedScmQueueSweep(botAccountsForSweep);
}

void RandomBotParticipationManager::OnPlayerLogout(Player const* player)
{
    if (!player)
        return;

    uint64 const rawGuid = player->GetGUID().GetRawValue();

    {
        std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
        g_NextRandomBotLifecycleProcessTimeByGuid.erase(rawGuid);
    }

    {
        std::lock_guard<std::mutex> insigniaLock(g_PlayerbotInsigniaCheckLock);
        g_NextPlayerbotInsigniaCheckTimeByGuid.erase(rawGuid);
        g_PlayerbotInsigniaBreakableAuraFirstSeenTimeByGuid.erase(rawGuid);
    }
}

// Real players get an under-map recovery for free: MovementHandler.cpp's
// CMSG_MOVE_* handler checks GetPositionZ() against GetMinHeight() on every
// incoming movement packet and calls Battleground::HandlePlayerUnderMap()
// (see e.g. BattlegroundBRT::HandlePlayerUnderMap, which teleports the
// player back to their team's starting graveyard). Playerbots move via
// server-side motion generators, never send real CMSG_MOVE_* packets, and so
// never reach that check - nothing in this module calls HandlePlayerUnderMap
// at all. A bot that clips through geometry (most likely on custom maps
// whose navmesh has gaps a client's own collision would have caught) just
// keeps falling forever with no recovery. Mirror the same check here on the
// bot lifecycle tick so bots get the same safety net real players get.
bool TryRecoverPlayerbotFromUnderMap(Player* player)
{
    if (!player || !player->IsInWorld() || !player->FindMap())
        return false;

    if (player->GetPositionZ() >= player->GetMap()->GetMinHeight(player->GetPositionX(), player->GetPositionY()))
        return false;

    if (Battleground* bg = player->GetBattleground())
        if (bg->HandlePlayerUnderMap(player))
            return true;

    if (player->IsAlive())
    {
        player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_IS_OUT_OF_BOUNDS);
        player->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, player->GetMaxHealth());
        if (player->IsAlive())
            player->KillPlayer();
    }

    return true;
}

void RandomBotParticipationManager::ProcessPlayerLifecycle(Player* player)
{
    if (!player)
        return;

    bool const isTransientClone = PlayerbotObcCloneManager::IsActiveClone(player);
    if (!isTransientClone && !playerbot::IsManagedRandomBot(player))
        return;

    // PlayerScript::OnUpdate is invoked from Map::Update. With more than one
    // map-update thread, bots on different maps reach the PvP modules at the
    // same time. Serialize the whole tick so all of the modules' per-GUID
    // state remains coherent for both persistent bots and transient clones.
    std::lock_guard<std::mutex> decisionLock(g_PlayerbotPvpDecisionLock);

    TryRecoverPlayerbotFromUnderMap(player);
    TryReviveManagedBotAfterStartup(player);
    // A near teleport such as Blink must finish in its own player-update turn.
    // Issuing combat movement immediately after the synthetic ACK can launch
    // observer-visible movement from both the old and new positions.
    if (TryFinalizePendingVirtualPlayerTeleport(player))
        return;
    TryUsePlayerbotInsigniaBreaker(player);
    TryCastPriestSpiritOfRedemption(player);

    if (isTransientClone && player && player->InBattleground() &&
        BattlegroundLifecycleActions::HandleDeathPrimitive(player))
        return;

    ProcessBattlegroundPlayerbotTick(player);

    // Dark clones deliberately share the playerbot tactical implementation,
    // but the clone manager alone owns their battleground membership and team.
    // Their death/release/resurrection state is handled above, but do not let
    // the normal persistent-bot lifecycle queue, leave, or rebalance these
    // transient players after their tactical tick has run.
    if (isTransientClone)
        return;

    if (!CanProcessPlayerLifecycle(player))
        return;

    RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(player);
}

void RandomBotParticipationManager::FinalizePendingVirtualPlayerTeleport(Player* player)
{
    TryFinalizePendingVirtualPlayerTeleport(player);
}

void RandomBotParticipationManager::SetPopulationRuntimeEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    g_RandomPopulation.runtimeEnabled = enabled;
    if (enabled)
        g_RandomPopulation.rebalanceRequested = true;
}

bool RandomBotParticipationManager::IsPopulationRuntimeEnabled()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    return g_RandomPopulation.runtimeEnabled;
}

bool RandomBotParticipationManager::TriggerImmediateRebalance()
{
    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    g_RandomPopulation.rebalanceRequested = true;
    g_RandomPopulation.rebalanceTimerMs = g_RandomPopulation.config.rebalanceIntervalMs;
    return RebalanceRandomPopulation(g_RandomPopulation);
}

std::vector<uint32> RandomBotParticipationManager::GetConfiguredBotAccountIds()
{
    return GetManagedBotAccountIdsSnapshot();
}

LifecycleObservationSnapshot RandomBotParticipationManager::GetLifecycleObservationSnapshot()
{
    LifecycleObservationSnapshot snapshot;
    snapshot.gateDisabled = g_LifecycleObservationCounters.gateDisabled.load(std::memory_order_relaxed);
    snapshot.cadenceThrottled = g_LifecycleObservationCounters.cadenceThrottled.load(std::memory_order_relaxed);
    snapshot.invalidPlayerState = g_LifecycleObservationCounters.invalidPlayerState.load(std::memory_order_relaxed);
    snapshot.noLifecycleHooksActive = g_LifecycleObservationCounters.noLifecycleHooksActive.load(std::memory_order_relaxed);
    snapshot.battlegroundLifecycleExecuted = g_LifecycleObservationCounters.battlegroundLifecycleExecuted.load(std::memory_order_relaxed);
    snapshot.arenaLifecycleExecuted = g_LifecycleObservationCounters.arenaLifecycleExecuted.load(std::memory_order_relaxed);
    return snapshot;
}

RandomBotPopulationSnapshot RandomBotParticipationManager::GetPopulationSnapshot()
{
    RandomBotPopulationSnapshot snapshot;

    std::lock_guard<std::mutex> lock(g_RandomPopulationLock);
    OnlineRandomBotMetrics const online = CollectOnlineRandomBotMetrics(g_RandomPopulation.config.botAccountIds);

    snapshot.configEnabled = g_RandomPopulation.config.enabled;
    snapshot.runtimeEnabled = g_RandomPopulation.runtimeEnabled;
    snapshot.supportsLoginOrchestration = SupportsLoginOrchestration();
    snapshot.targetMin = g_RandomPopulation.config.targetMin;
    snapshot.targetMax = g_RandomPopulation.config.targetMax;
    snapshot.onlineRandomBots = online.total;
    snapshot.onlineAllianceRandomBots = online.alliance;
    snapshot.onlineHordeRandomBots = online.horde;
    snapshot.offlinePoolSize = static_cast<uint32>(QueryOfflinePool(g_RandomPopulation.config).size());
    snapshot.maxOnlineBotsPerAccount = g_RandomPopulation.config.maxOnlineBotsPerAccount;
    snapshot.rebalanceTicks = g_RandomPopulation.rebalanceTicks;
    snapshot.loginAttempts = g_RandomPopulation.loginAttempts;
    snapshot.loginSuccess = g_RandomPopulation.loginSuccess;
    snapshot.logoutAttempts = g_RandomPopulation.logoutAttempts;
    snapshot.logoutSuccess = g_RandomPopulation.logoutSuccess;
    snapshot.skippedSafetyRealPlayers = g_RandomPopulation.skippedSafetyRealPlayers;
    snapshot.skippedNoCandidatePool = g_RandomPopulation.skippedNoCandidatePool;
    snapshot.skippedIntegrationGap = g_RandomPopulation.skippedIntegrationGap;
    snapshot.lastRebalanceUnixTime = g_RandomPopulation.lastRebalanceUnixTime;

    return snapshot;
}

void RandomBotParticipationLifecycle::ProcessLifecycleEntryPoint(Player* player)
{
    if (!player)
    {
        LogLifecycleBranchSummary(ObjectGuid::Empty, "invalid-player-state");
        ObserveLifecycleReason(LifecycleObservationReason::InvalidPlayerState, ObjectGuid::Empty);
        return;
    }

    ObjectGuid const guid = player->GetGUID();
    uint64 const guidRaw = guid.GetRawValue();

    if (!IsLifecycleGateEnabled())
    {
        LogLifecycleBranchSummary(guid, "gate-disabled");
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        return;
    }

    if (IsCrowdControlledForLifecyclePause(player))
    {
        ClearActiveMovementForControlLoss(player);
        bool const didBlink = TryMageBlinkOutOfControl(player);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle paused: guid={} reason=crowd-control blinkExecuted={}",
            guidRaw,
            didBlink ? 1 : 0);
        return;
    }

    PvpValues const values = PvpCore::CollectValues(player);

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Lifecycle values: guid={} inBg={} bgState={} inBgQueue={} hasBgQueue={} hasBgInvite={} hasArenaQueue={} hasArenaInvite={} hasArenaTeamInvite={} bgTypeId={} bgId={} queueSlot0={} queueSlot1={} queueSlot2={}",
        guidRaw,
        values.inBattleground ? 1 : 0,
        static_cast<uint32>(values.battlegroundState),
        values.inBattlegroundQueue ? 1 : 0,
        values.hasBattlegroundQueue ? 1 : 0,
        values.hasBattlegroundInvite ? 1 : 0,
        values.hasArenaQueue ? 1 : 0,
        values.hasArenaInvite ? 1 : 0,
        values.hasArenaTeamInvite ? 1 : 0,
        uint32(values.battlegroundTypeId),
        player->GetBattlegroundId(),
        uint32(player->GetBattlegroundQueueTypeId(0)),
        uint32(player->GetBattlegroundQueueTypeId(1)),
        uint32(player->GetBattlegroundQueueTypeId(2)));

    BattlegroundTacticalContext const tacticalContext = PvpCore::BuildBattlegroundTacticalContext(player, values);
    bool const didExecuteTactical = BattlegroundTacticalActions::Execute(player, tacticalContext);
    bool const didExecuteDuelTactical = DuelTacticalActions::Execute(player);
    PvpClassSpellContext const classSpellContext = PvpCore::BuildClassSpellContext(player, values);
    bool const didExecuteClassSpell = PvpClassActions::Execute(player, classSpellContext);
    if (didExecuteClassSpell &&
        (classSpellContext.spellId == 16166 ||
         classSpellContext.spellId == 17116 ||
         // Amplify Curse (18288) precedes Curse of Agony, and Arcane Power
         // (12042) / Presence of Mind (12043) precede the rest of the
         // arcane burst chain (see SelectWarlockSpell / SelectMageSpell) -
         // all four are off the global cooldown and are meant to land back
         // to back with their follow-up cast rather than waiting a full
         // cadence tick.
         classSpellContext.spellId == 18288 ||
         classSpellContext.spellId == 12042 ||
         classSpellContext.spellId == 12043 ||
         PvpClassActions::IsPetSpellAction(player, classSpellContext)))
    {
        std::lock_guard<std::mutex> cadenceLock(g_RandomBotLifecycleCadenceLock);
        g_NextRandomBotLifecycleProcessTimeByGuid[guid.GetRawValue()] = LifecycleCadenceClock::now();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP cadence bypass applied: guid={} spell={} reason=off-gcd-or-pet-action.",
            guidRaw, classSpellContext.spellId);
    }

    BattlegroundLifecycleContext const bgContext = PvpCore::BuildBattlegroundLifecycleContext(player, values);
    ArenaLifecycleContext const arenaContext = PvpCore::BuildArenaLifecycleContext(player, values);
    RandomBotParticipationHooks const hooks = PvpCore::BuildRandomBotParticipationHooks(player, values);

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Lifecycle contexts: guid={} bgQueueOp={} bgInviteResp={} bgInProgress={} arenaQueueOp={} arenaTeamInteraction={} bgHook={} arenaHook={}",
        guidRaw,
        static_cast<uint32>(bgContext.queueOperation),
        static_cast<uint32>(bgContext.invitationResponse),
        bgContext.shouldHandleInProgressStatus ? 1 : 0,
        static_cast<uint32>(arenaContext.queueOperation),
        static_cast<uint32>(arenaContext.teamInteraction),
        hooks.battlegroundParticipationHook ? 1 : 0,
        hooks.arenaParticipationHook ? 1 : 0);

    if (!hooks.lifecycleEnabled)
    {
        LogLifecycleBranchSummary(guid, "hooks-lifecycle-disabled");
        ObserveLifecycleReason(LifecycleObservationReason::GateDisabled, guid);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle exited: guid={} reason=hooks-lifecycle-disabled",
            guidRaw);
        return;
    }

    if (!hooks.battlegroundParticipationHook && !hooks.arenaParticipationHook)
    {
        LogLifecycleBranchSummary(guid, "no-lifecycle-hooks-active");
        ObserveLifecycleReason(LifecycleObservationReason::NoLifecycleHooksActive, guid);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle exited: guid={} reason=no-hooks inBg={} inBgQueue={} hasBgQueue={} hasBgInvite={} hasArenaQueue={} hasArenaInvite={} hasArenaTeamInvite={}",
            guidRaw,
            values.inBattleground ? 1 : 0,
            values.inBattlegroundQueue ? 1 : 0,
            values.hasBattlegroundQueue ? 1 : 0,
            values.hasBattlegroundInvite ? 1 : 0,
            values.hasArenaQueue ? 1 : 0,
            values.hasArenaInvite ? 1 : 0,
            values.hasArenaTeamInvite ? 1 : 0);
        return;
    }

    bool didExecuteBattleground = false;
    bool didExecuteArena = false;

    if (hooks.battlegroundParticipationHook)
    {
        if (!IsNoOp(bgContext))
        {
            didExecuteBattleground = BattlegroundLifecycleActions::Execute(player, bgContext);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Lifecycle battleground execute: guid={} executed={} queueOp={} inviteResp={} inProgress={}",
                guidRaw,
                didExecuteBattleground ? 1 : 0,
                static_cast<uint32>(bgContext.queueOperation),
                static_cast<uint32>(bgContext.invitationResponse),
                bgContext.shouldHandleInProgressStatus ? 1 : 0);
        }
    }

    PvpValues const postBattlegroundValues = PvpCore::CollectValues(player);
    ArenaLifecycleContext const postArenaContext = PvpCore::BuildArenaLifecycleContext(player, postBattlegroundValues);
    RandomBotParticipationHooks const postBattlegroundHooks = PvpCore::BuildRandomBotParticipationHooks(player, postBattlegroundValues);

    if (postBattlegroundHooks.arenaParticipationHook)
    {
        if (!IsNoOp(postArenaContext))
        {
            didExecuteArena = ArenaLifecycleActions::Execute(player, postArenaContext);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Lifecycle arena execute: guid={} executed={} queueOp={} teamInteraction={} hasArenaInvite={} hasArenaTeamInvite={}",
                guidRaw,
                didExecuteArena ? 1 : 0,
                static_cast<uint32>(postArenaContext.queueOperation),
                static_cast<uint32>(postArenaContext.teamInteraction),
                postBattlegroundValues.hasArenaInvite ? 1 : 0,
                postBattlegroundValues.hasArenaTeamInvite ? 1 : 0);
        }
    }

    if (!didExecuteBattleground && !didExecuteArena)
    {
        LogLifecycleBranchSummary(guid, "active-hooks-no-op-context");
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Lifecycle no-op with active hooks: guid={} bgHook={} arenaHook={}",
            guidRaw, hooks.battlegroundParticipationHook ? 1 : 0, hooks.arenaParticipationHook ? 1 : 0);
    }

    if (didExecuteBattleground)
    {
        LogLifecycleBranchSummary(guid, "battleground-lifecycle-executed");
        ObserveLifecycleReason(LifecycleObservationReason::BattlegroundLifecycleExecuted, guid);
    }

    if (didExecuteArena)
    {
        LogLifecycleBranchSummary(guid, "arena-lifecycle-executed");
        ObserveLifecycleReason(LifecycleObservationReason::ArenaLifecycleExecuted, guid);
    }

    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Lifecycle complete: guid={} didBg={} didArena={} didTactical={} didClassSpell={}",
        guidRaw,
        didExecuteBattleground ? 1 : 0,
        didExecuteArena ? 1 : 0,
        (didExecuteTactical || didExecuteDuelTactical) ? 1 : 0,
        didExecuteClassSpell ? 1 : 0);
}

bool RandomBotParticipationLifecycle::ProcessBattlegroundLifecycleEntryPoint(Player* player, PvpValues const& values,
    RandomBotParticipationHooks const& hooks)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!hooks.lifecycleEnabled || !hooks.battlegroundParticipationHook)
        return false;

    BattlegroundLifecycleContext const battlegroundContext = PvpCore::BuildBattlegroundLifecycleContext(player, values);
    return BattlegroundLifecycleActions::Execute(player, battlegroundContext);
}

bool RandomBotParticipationLifecycle::ProcessArenaLifecycleEntryPoint(Player* player, PvpValues const& values,
    RandomBotParticipationHooks const& hooks)
{
    if (!player || !IsLifecycleGateEnabled())
        return false;

    if (!hooks.lifecycleEnabled || !hooks.arenaParticipationHook)
        return false;

    ArenaLifecycleContext const arenaContext = PvpCore::BuildArenaLifecycleContext(player, values);
    return ArenaLifecycleActions::Execute(player, arenaContext);
}
}
