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

#include "PlayerbotPvpCore.h"
#include "PlayerbotPvpClassActions.h"
#include "PlayerbotRandomBotParticipation.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundEY.h"
#include "BattlegroundTP.h"
#include "BattlegroundWS.h"
#include "Configuration/Config.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Unit.h"
#include "Util.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
struct SpellDecision;
bool HasHostileTarget(Player const* player, Unit const* target);
bool IsPetSpellReady(Player const* player, uint32 spellId);
bool IsFriendlySupportTarget(Player const* player, Unit const* target);
SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player);

constexpr float kReferenceHunterSwitchDistance = 8.0f;
constexpr float kRangedSpacingEnterOutOfRangeBuffer = 2.0f;
constexpr float kRangedSpacingEnterTooCloseBuffer = 1.0f;
constexpr uint32 kHunterAutoShotSpellId = 75;
constexpr uint32 kPlayerbotDispelCooldownToken = 900004;
constexpr uint32 kPlayerbotHandOfSacrificeCooldownToken = 900005;
std::unordered_map<ObjectGuid, bool> g_HunterRangedModeByBot;
std::mutex g_HunterRangedModeByBotLock;
std::unordered_map<ObjectGuid, uint8> g_CombatNoTargetTicksByBot;
std::mutex g_CombatNoTargetTicksByBotLock;
thread_local ObjectGuid g_CurrentDecisionBotGuid = ObjectGuid::Empty;
thread_local uint32 g_SuppressedDecisionSpellId = 0;

bool IsPlayerbotDispelSpell(uint32 spellId)
{
    if (!spellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;

    switch (firstRankSpellId)
    {
        case 370:  // Purge
        case 475:  // Remove Lesser Curse
        case 527:  // Dispel Magic
        case 2782: // Remove Curse
        case 2893: // Abolish Poison
        case 4987: // Cleanse
            return true;
        default:
            return false;
    }
}

bool IsHunterInRangedMode(Player const* player)
{
    if (!player)
        return true;

    std::lock_guard<std::mutex> lock(g_HunterRangedModeByBotLock);
    auto itr = g_HunterRangedModeByBot.find(player->GetGUID());
    if (itr == g_HunterRangedModeByBot.end())
        return true;

    return itr->second;
}

void UpdateHunterCombatMode(Player const* player, Unit const* target)
{
    if (!player || player->GetClass() != CLASS_HUNTER)
        return;

    bool rangedMode = IsHunterInRangedMode(player);
    if (!target || !target->IsAlive())
    {
        {
            std::lock_guard<std::mutex> lock(g_HunterRangedModeByBotLock);
            g_HunterRangedModeByBot[player->GetGUID()] = true;
        }
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "Hunter mode reset to ranged: botGuid={} reason=no-valid-target.",
            player->GetGUID().ToString());
        return;
    }

    float const distance = player->GetDistance(target);
    bool const previousRangedMode = rangedMode;
    if (rangedMode)
    {
        // Deadzone guard: once a hostile is inside hunter minimum-range
        // pressure distance, force melee mode even if threat ownership has not
        // switched yet. Otherwise bots can stay in ranged mode with no valid
        // cast options and appear "locked out" at point-blank range.
        if (distance <= kReferenceHunterSwitchDistance)
            rangedMode = false;
    }
    else
    {
        if (target->GetVictim() != player && distance > kReferenceHunterSwitchDistance)
            rangedMode = true;
    }

    {
        std::lock_guard<std::mutex> lock(g_HunterRangedModeByBotLock);
        g_HunterRangedModeByBot[player->GetGUID()] = rangedMode;
    }

    if (previousRangedMode != rangedMode)
    {
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "Hunter mode switch: botGuid={} targetGuid={} previousMode={} newMode={} distance={} targetVictimIsBot={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), previousRangedMode ? "ranged" : "melee",
            rangedMode ? "ranged" : "melee", distance, target->GetVictim() == player ? 1 : 0);
    }
}

float GetHunterDeadZoneMaxRange()
{
    SpellInfo const* autoShotInfo = sSpellMgr->GetSpellInfo(kHunterAutoShotSpellId);
    if (!autoShotInfo)
        return kReferenceHunterSwitchDistance;

    float const minRange = autoShotInfo->GetMinRange(false);
    if (minRange <= 0.0f)
        return kReferenceHunterSwitchDistance;

    return minRange;
}

uint8 IncrementCombatNoTargetTicks(Player const* player)
{
    if (!player)
        return 0;

    std::lock_guard<std::mutex> lock(g_CombatNoTargetTicksByBotLock);
    uint8& ticks = g_CombatNoTargetTicksByBot[player->GetGUID()];
    ticks = std::min<uint8>(static_cast<uint8>(ticks + 1), static_cast<uint8>(20));
    return ticks;
}

void ResetCombatNoTargetTicks(Player const* player)
{
    if (!player)
        return;

    std::lock_guard<std::mutex> lock(g_CombatNoTargetTicksByBotLock);
    g_CombatNoTargetTicksByBot.erase(player->GetGUID());
}

SpellDecision MaybeSelectUtilitySpell(Player const* player, Unit const* hostileTarget);

playerbot::PvpCoreConfig g_PvpCoreConfig;
bool CanAttemptMount(Player const* player, SpellInfo const* mountSpellInfo);
bool IsHardControlled(Player const* player);
bool IsEffectivelyOutdoors(Player const* player);
bool IsStrictlyOutdoorsForMount(Player const* player);
bool HasNearbyAttackableEnemyPlayer(Player const* player, float maxDistance);

float GetConfiguredSpellRange() { return g_PvpCoreConfig.spellRange; }
float GetConfiguredHealRange() { return g_PvpCoreConfig.healRange; }
float GetConfiguredMeleeRange() { return g_PvpCoreConfig.meleeRange; }
float GetConfiguredCloseRange() { return g_PvpCoreConfig.closeRange; }
float GetConfiguredLongRange() { return g_PvpCoreConfig.longRange; }

float GetConfiguredCombatRange()
{
    float range = std::max(GetConfiguredLongRange(), GetConfiguredSpellRange());
    range = std::max(range, GetConfiguredCloseRange());
    range = std::max(range, GetConfiguredMeleeRange());
    return std::max(range, 0.0f);
}

bool IsLifecycleGateEnabled(playerbot::PvpCoreConfig const& config)
{
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

bool IsClassSpellGateEnabled(playerbot::PvpCoreConfig const& config)
{
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpClassSpellsEnabled;
}

bool IsLowOrOutOfManaForFallback(Player const* player)
{
    if (!player || player->GetPowerType() != POWER_MANA)
        return false;

    return player->GetPowerPct(POWER_MANA) <= 10.0f || player->GetPower(POWER_MANA) < 250;
}

bool HasWandEquipped(Player const* player)
{
    if (!player)
        return false;

    Item const* ranged = player->GetWeaponForAttack(RANGED_ATTACK, true);
    if (!ranged || !ranged->GetTemplate())
        return false;

    return ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_WAND;
}

bool IsFlagCarrierNear(Player const* player, ObjectGuid const& carrierGuid, float maxDistance)
{
    if (!player || carrierGuid.IsEmpty())
        return false;

    Player const* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
    if (!carrier || !carrier->IsAlive() || carrier->GetMapId() != player->GetMapId())
        return false;

    return player->IsWithinDistInMap(carrier, maxDistance);
}

void PopulateObjectiveStateTriggers(Player const* player, playerbot::PvpValues& values)
{
    if (!player || !values.inBattleground)
        return;

    Battleground* battleground = player->GetBattleground();
    if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint32 const botTeamValue = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    TeamId const botTeam = (botTeamValue == ALLIANCE || botTeamValue == TEAM_ALLIANCE) ? TEAM_ALLIANCE :
        (botTeamValue == HORDE || botTeamValue == TEAM_HORDE) ? TEAM_HORDE : player->GetTeamId();
    TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
    ObjectGuid const playerGuid = player->GetGUID();

    if (BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(battleground))
    {
        ObjectGuid const enemyCarrierGuid = bgWs->GetFlagPickerGUID(botTeam);
        ObjectGuid const teamCarrierGuid = bgWs->GetFlagPickerGUID(enemyTeam);

        values.playerHasFlag = (teamCarrierGuid == playerGuid);
        values.enemyFlagCarrierActive = !enemyCarrierGuid.IsEmpty();
        values.enemyFlagCarrierNear = IsFlagCarrierNear(player, enemyCarrierGuid, 100.0f);

        bool const bothFlagsNotAtBase =
            bgWs->GetFlagState(ALLIANCE) != BG_WS_FLAG_STATE_ON_BASE &&
            bgWs->GetFlagState(HORDE) != BG_WS_FLAG_STATE_ON_BASE;
        if (!bothFlagsNotAtBase)
            values.teamFlagCarrierNear = IsFlagCarrierNear(player, teamCarrierGuid, 200.0f);

        return;
    }

    if (BattlegroundTP* bgTp = dynamic_cast<BattlegroundTP*>(battleground))
    {
        ObjectGuid const enemyCarrierGuid = bgTp->GetFlagPickerGUID(botTeam);
        ObjectGuid const teamCarrierGuid = bgTp->GetFlagPickerGUID(enemyTeam);

        values.playerHasFlag = (teamCarrierGuid == playerGuid);
        values.enemyFlagCarrierActive = !enemyCarrierGuid.IsEmpty();
        values.enemyFlagCarrierNear = IsFlagCarrierNear(player, enemyCarrierGuid, 100.0f);
        values.teamFlagCarrierNear = IsFlagCarrierNear(player, teamCarrierGuid, 200.0f);
        return;
    }

    if (BattlegroundEY* bgEy = dynamic_cast<BattlegroundEY*>(battleground))
    {
        ObjectGuid const carrierGuid = bgEy->GetFlagPickerGUID();
        if (carrierGuid.IsEmpty())
            return;

        values.playerHasFlag = (carrierGuid == playerGuid);
        Player const* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
        if (!carrier || !carrier->IsAlive() || carrier->GetMapId() != player->GetMapId())
            return;

        if (carrier->GetTeamId() == botTeam)
            values.teamFlagCarrierNear = player->IsWithinDistInMap(carrier, 200.0f);
        else
        {
            values.enemyFlagCarrierActive = true;
            values.enemyFlagCarrierNear = player->IsWithinDistInMap(carrier, 100.0f);
        }
    }
}

struct SpellDecision
{
    char const* actionName = nullptr;
    char const* reason = nullptr;
    uint32 spellId = 0;
    playerbot::PvpClassSpellContext::TargetMode targetMode = playerbot::PvpClassSpellContext::TargetMode::None;
    ObjectGuid targetGuid = ObjectGuid::Empty;
    uint32 itemEntry = 0;
    char const* triggerName = nullptr;
};

struct PrioritizedSpellDecision
{
    float priority = 0.0f;
    SpellDecision decision;
};

bool IsDecisionImmediatelyCastable(Player const* player, SpellDecision const& decision, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget);
SpellDecision SelectHighestPriorityCastableDecision(std::vector<PrioritizedSpellDecision>& candidates, Player const* player,
    Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget);

SpellDecision MaybeSelectUtilitySpell(Player const* player, Unit const* hostileTarget)
{
    if (!player)
        return {};

    constexpr uint32 kPlayerbotDrinkSpell = 22734;
    bool const maintainExistingDrink = !player->IsInCombat() &&
        player->GetMaxPower(POWER_MANA) > 0 &&
        player->HasAura(kPlayerbotDrinkSpell) &&
        player->GetPowerPct(POWER_MANA) < 50.0f;

    // Match reference behavior more closely: do not let out-of-combat utility
    // preempt combat spell trees while a valid hostile target exists.
    //
    // Exception: if the bot is already drinking and still below the 50% mana
    // floor, keep utility selection available so drink remains sticky instead
    // of immediately breaking back into combat posture.
    if (HasHostileTarget(player, hostileTarget) && !maintainExistingDrink)
        return {};

    return SelectOutOfCombatEatDrinkOrMountSpell(player);
}

class DecisionEvaluationScope
{
public:
    DecisionEvaluationScope(Player const* player, uint32 suppressedSpellId)
        : _previousBotGuid(g_CurrentDecisionBotGuid), _previousSuppressedSpellId(g_SuppressedDecisionSpellId)
    {
        g_CurrentDecisionBotGuid = player ? player->GetGUID() : ObjectGuid::Empty;
        g_SuppressedDecisionSpellId = suppressedSpellId;
    }

    ~DecisionEvaluationScope()
    {
        g_CurrentDecisionBotGuid = _previousBotGuid;
        g_SuppressedDecisionSpellId = _previousSuppressedSpellId;
    }

private:
    ObjectGuid _previousBotGuid;
    uint32 _previousSuppressedSpellId = 0;
};

void AddDecisionCandidate(std::vector<PrioritizedSpellDecision>& candidates, bool condition, float priority, SpellDecision const& decision)
{
    if (!condition || !decision.spellId)
        return;

    if (g_SuppressedDecisionSpellId != 0 && decision.spellId == g_SuppressedDecisionSpellId)
        return;

    candidates.push_back({ priority, decision });
}

struct SpellTriggerRule
{
    char const* triggerName = nullptr;
    bool condition = false;
    float priority = 0.0f;
    SpellDecision decision;
};

SpellDecision SelectFromTriggerGraph(Player const* player, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget,
    std::initializer_list<SpellTriggerRule> rules)
{
    std::vector<PrioritizedSpellDecision> candidates;
    candidates.reserve(rules.size());

    for (SpellTriggerRule const& rule : rules)
    {
        SpellDecision decision = rule.decision;
        if (!decision.triggerName)
            decision.triggerName = rule.triggerName;
        AddDecisionCandidate(candidates, rule.condition, rule.priority, decision);
    }

    return SelectHighestPriorityCastableDecision(candidates, player, defaultEnemyTarget, defaultAllyTarget);
}

SpellDecision SelectHighestPriorityCastableDecision(std::vector<PrioritizedSpellDecision>& candidates, Player const* player,
    Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget)
{
    if (candidates.empty())
        return {};

    std::stable_sort(candidates.begin(), candidates.end(), [](PrioritizedSpellDecision const& left, PrioritizedSpellDecision const& right)
    {
        return left.priority > right.priority;
    });

    if (!player)
        return candidates.front().decision;

    for (PrioritizedSpellDecision const& candidate : candidates)
        if (IsDecisionImmediatelyCastable(player, candidate.decision, defaultEnemyTarget, defaultAllyTarget))
            return candidate.decision;

    // Preserve highest-priority fallback so execution can still drive movement/range correction.
    return candidates.front().decision;
}

bool IsDecisionImmediatelyCastable(Player const* player, SpellDecision const& decision, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget)
{
    if (!player)
        return false;

    if (decision.itemEntry)
        return player->HasItemCount(decision.itemEntry);

    if (!decision.spellId)
        return false;

    bool const knownByPlayer = player->HasSpell(decision.spellId);
    bool const knownByPet = IsPetSpellReady(player, decision.spellId);
    if (!knownByPlayer && !knownByPet)
        return false;

    // Treat the shared playerbot dispel cooldown as an immediate castability
    // failure so the same decision pass suppresses this dispel and selects the
    // next available action instead of returning an idle cooldown attempt.
    if (IsPlayerbotDispelSpell(decision.spellId) &&
        playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(decision.spellId);
    if (!spellInfo)
        return false;

    Unit const* resolvedTarget = nullptr;
    switch (decision.targetMode)
    {
        case playerbot::PvpClassSpellContext::TargetMode::Self:
            resolvedTarget = player;
            break;
        case playerbot::PvpClassSpellContext::TargetMode::Enemy:
            resolvedTarget = defaultEnemyTarget;
            break;
        case playerbot::PvpClassSpellContext::TargetMode::Ally:
            resolvedTarget = defaultAllyTarget;
            break;
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default:
            return false;
    }

    if (!decision.targetGuid.IsEmpty())
        if (Unit const* explicitTarget = ObjectAccessor::GetUnit(*player, decision.targetGuid))
            resolvedTarget = explicitTarget;

    if (!resolvedTarget || !resolvedTarget->IsAlive())
        return false;

    if (decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && !player->IsValidAttackTarget(resolvedTarget, spellInfo))
        return false;
    if (decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally && !IsFriendlySupportTarget(player, resolvedTarget))
        return false;

    if (!player->IsWithinLOSInMap(resolvedTarget))
        return false;

    float const maxRange = spellInfo->GetMaxRange(false);
    if (maxRange > 0.0f && !player->IsWithinDistInMap(resolvedTarget, maxRange))
        return false;

    float const minRange = spellInfo->GetMinRange(false);
    if (minRange > 0.0f && player->IsWithinDistInMap(resolvedTarget, minRange))
        return false;

    // Skip decisions we cannot currently pay for so the fallback chain can
    // choose a castable alternative (wand, movement, etc.) instead of
    // repeatedly selecting an OOM support spell.
    Unit const* powerCaster = knownByPet ? static_cast<Unit const*>(player->GetPet()) : static_cast<Unit const*>(player);
    if (powerCaster && spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
    {
        Powers const powerType = Powers(spellInfo->PowerType);
        int32 const powerCost = spellInfo->CalcPowerCost(powerCaster, spellInfo->GetSchoolMask());
        if (powerCost > 0 && powerCaster->GetPower(powerType) < powerCost)
            return false;
    }

    return true;
}

constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT = 29073;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK = 22734;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT = 22328;

struct TacticalDecision
{
    char const* triggerName = nullptr;
    char const* actionName = nullptr;
    float priority = 0.0f;
};

enum class ClassicClassProfile : uint8
{
    UnknownClassic = 0,
    PrimaryClassic,
    SecondaryClassic,
    TertiaryClassic
};

struct ClassicProfileSelection
{
    ClassicClassProfile profile = ClassicClassProfile::UnknownClassic;
    char const* profileLabel = "UnknownClassic";
    bool usedFallback = true;
    bool unsupportedClass = false;
};

enum class HunterPvpSpec : uint8
{
    BeastMastery = 0,
    Marksmanship,
    Survival
};

HunterPvpSpec GetHunterPvpSpec(ClassicProfileSelection const& profileSelection)
{
    switch (profileSelection.profile)
    {
        case ClassicClassProfile::PrimaryClassic:
            return HunterPvpSpec::BeastMastery;
        case ClassicClassProfile::SecondaryClassic:
            return HunterPvpSpec::Marksmanship;
        case ClassicClassProfile::TertiaryClassic:
            return HunterPvpSpec::Survival;
        case ClassicClassProfile::UnknownClassic:
        default:
            return HunterPvpSpec::Marksmanship;
    }
}

ClassicProfileSelection DetectClassicClassProfile(Player const* player)
{
    ClassicProfileSelection selection;
    if (!player)
        return selection;

    uint8 const activeSpec = player->GetActiveSpec();
    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
            if (player->HasTalent(12294, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Arms-like", false, false };
            if (player->HasTalent(23881, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Fury-like", false, false };
            if (player->HasTalent(23922, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Prot-like", false, false };
            break;
        case CLASS_PALADIN:
            if (player->HasTalent(20473, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Holy-like", false, false };
            if (player->HasTalent(20925, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Prot-like", false, false };
            if (player->HasTalent(20066, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Ret-like", false, false };
            break;
        case CLASS_HUNTER:
            if (player->HasTalent(19574, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "BM-like", false, false };
            if (player->HasTalent(19506, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "MM-like", false, false };
            if (player->HasTalent(19386, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "SV-like", false, false };
            break;
        case CLASS_ROGUE:
            if (player->HasTalent(14177, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Assassination-like", false, false };
            if (player->HasTalent(13750, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Combat-like", false, false };
            if (player->HasTalent(14185, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Subtlety-like", false, false };
            break;
        case CLASS_PRIEST:
            if (player->HasTalent(10060, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Discipline-like", false, false };
            if (player->HasTalent(724, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Holy-like", false, false };
            if (player->HasTalent(15473, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Shadow-like", false, false };
            break;
        case CLASS_SHAMAN:
            if (player->HasTalent(16166, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Elemental-like", false, false };
            if (player->HasTalent(17364, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Enhancement-like", false, false };
            if (player->HasTalent(16188, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Restoration-like", false, false };
            break;
        case CLASS_MAGE:
            if (player->HasTalent(12042, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Arcane-like", false, false };
            if (player->HasTalent(33041, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Fire-like", false, false };
            if (player->HasTalent(11426, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Frost-like", false, false };
            break;
        case CLASS_WARLOCK:
            if (player->HasTalent(48181, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Affliction-like", false, false };
            if (player->HasTalent(19028, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Demonology-like", false, false };
            if (player->HasTalent(17962, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Destruction-like", false, false };
            break;
        case CLASS_DRUID:
            if (player->HasTalent(24858, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Balance-like", false, false };
            if (player->HasTalent(18562, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Restoration-like", false, false };
            if (player->HasTalent(17007, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Feral-like", false, false };
            break;
        case CLASS_DEATH_KNIGHT:
            return { ClassicClassProfile::UnknownClassic, "UnsupportedClassicClass", true, true };
        default:
            break;
    }

    return selection;
}

bool IsSpellReady(Player const* player, uint32 spellId)
{
    if (!player || !spellId)
        return false;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!baseSpellInfo)
        return false;

    uint32 resolvedSpellId = 0;
    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
    {
        if (player->HasSpell(chainSpellId))
            resolvedSpellId = chainSpellId;
    }

    if (!resolvedSpellId)
        return false;

    return !player->GetSpellHistory()->HasCooldown(resolvedSpellId);
}

bool IsPetSpellReady(Player const* player, uint32 spellId)
{
    if (!player || !spellId)
        return false;

    Pet const* pet = player->GetPet();
    if (!pet || !pet->IsAlive())
        return false;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!baseSpellInfo)
        return false;

    uint32 resolvedSpellId = 0;
    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
        if (pet->HasSpell(chainSpellId))
            resolvedSpellId = chainSpellId;

    if (!resolvedSpellId)
        return false;

    return !pet->GetSpellHistory()->HasCooldown(resolvedSpellId);
}

bool IsEffectivelyOutdoors(Player const* player)
{
    if (!player)
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return player->IsOutdoors();

    PositionFullTerrainStatus terrainStatus;
    map->GetFullTerrainStatusForPosition(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        terrainStatus, MAP_ALL_LIQUIDS, player->GetCollisionHeight());
    // Travel-state checks should tolerate brief map flag flickers around
    // battleground ramps/fences, so treat either signal as outdoors.
    return player->IsOutdoors() || terrainStatus.outdoors;
}

bool IsStrictlyOutdoorsForMount(Player const* player)
{
    if (!player)
        return false;

    // Keep mount checks aligned with reference behavior: require IsOutdoors and
    // reject cases where the unit is clipping slightly below floor level (which
    // can misreport outdoor state in battleground tunnels/bases).
    if (!player->IsOutdoors())
        return false;

    float const posZ = player->GetPositionZ();
    float const groundLevel = player->GetMapWaterOrGroundLevel(player->GetPositionX(), player->GetPositionY(), posZ);
    if (!player->HasAuraType(SPELL_AURA_WATER_WALK) && posZ < groundLevel)
        return false;

    return true;
}

bool ShouldForceIndoorDismount(Player const* player, bool outdoors, uint32 lingerMs = 1500)
{
    if (!player)
        return false;

    static std::unordered_map<uint64, uint32> indoorSinceMsByGuid;
    uint64 const guid = player->GetGUID().GetRawValue();

    if (outdoors)
    {
        indoorSinceMsByGuid.erase(guid);
        return false;
    }

    uint32 const nowMs = GameTime::GetGameTimeMS();
    auto itr = indoorSinceMsByGuid.find(guid);
    if (itr == indoorSinceMsByGuid.end())
    {
        indoorSinceMsByGuid.emplace(guid, nowMs);
        return false;
    }

    return nowMs >= itr->second + lingerMs;
}

bool HasNearbyAttackableEnemyPlayer(Player const* player, float maxDistance)
{
    if (!player || !player->IsInWorld())
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return false;

    float const checkDistance = std::max(maxDistance, 0.0f);
    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (!player->IsWithinDistInMap(candidate, checkDistance))
            continue;
        if (!player->IsValidAttackTarget(candidate))
            continue;
        return true;
    }

    return false;
}

bool CanAttemptMount(Player const* player, SpellInfo const* mountSpellInfo)
{
    if (!player || !mountSpellInfo)
        return false;

    uint32 zoneId = 0;
    uint32 areaId = 0;
    player->GetZoneAndAreaId(zoneId, areaId);
    if (mountSpellInfo->CheckLocation(player->GetMapId(), zoneId, areaId, player, false) != SPELL_CAST_OK)
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return false;

    bool allowMount = !map->IsDungeon() || map->IsBattlegroundOrArena();
    if (InstanceTemplate const* instanceTemplate = sObjectMgr->GetInstanceTemplate(player->GetMapId()))
        allowMount = instanceTemplate->AllowMount;

    return allowMount || mountSpellInfo->AreaGroupId;
}

bool IsHardControlled(Player const* player)
{
    if (!player)
        return false;

    return player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->HasUnitState(UNIT_STATE_ROOT);
}

uint32 SelectReadyKnownMountSpell(Player const* player)
{
    if (!player)
        return 0;

    for (PlayerSpellMap::value_type const& knownSpellPair : player->GetSpellMap())
    {
        uint32 const spellId = knownSpellPair.first;
        PlayerSpell const& knownSpell = knownSpellPair.second;
        if (!knownSpell.active || knownSpell.disabled)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->IsPassive())
            continue;
        if (!spellInfo->HasAura(SPELL_AURA_MOUNTED) && spellInfo->Mechanic != MECHANIC_MOUNT)
            continue;
        if (!IsSpellReady(player, spellId))
            continue;
        if (!CanAttemptMount(player, spellInfo))
            continue;

        return spellId;
    }

    return 0;
}

SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player)
{
    SpellDecision decision;
    if (!player || !player->IsAlive() || player->IsInCombat() || player->IsMounted())
        return decision;

    // Do not attempt recovery/mount actions while hard controlled. This avoids
    // mount selections during fear/polymorph/stun/root states.
    if (IsHardControlled(player))
        return decision;

    bool const usesMana = player->GetMaxPower(POWER_MANA) > 0;
    float const manaPct = usesMana ? player->GetPowerPct(POWER_MANA) : 100.0f;
    bool const needsDrink = usesMana && manaPct < 100.0f;
    bool const keepDrinkingFloor = usesMana && manaPct < 50.0f;
    bool const urgentlyNeedsDrink = needsDrink && (player->GetPowerPct(POWER_MANA) < 35.0f || IsLowOrOutOfManaForFallback(player));
    bool const needsFood = player->GetHealthPct() < 100.0f;
    bool const hasEatAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
    bool const hasDrinkAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

    // Keep a single nearby-enemy boundary for "switch to combat posture".
    // Inside this range we should avoid out-of-combat utility behaviors
    // (eat/drink/mount), so movement + combat targeting can take over cleanly.
    //
    // Exception: when mana is critically low and the bot is already out of
    // combat, allow drink selection so they can recover instead of idling in a
    // perpetual "combat posture" loop.
    float const nearbyHostileCombatBoundary = std::max(GetConfiguredLongRange(), 35.0f);
    if (player->FindMap())
    {
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, nearbyHostileCombatBoundary))
                continue;

            // If the bot is already drinking, keep that decision sticky until
            // we reach the configured recovery floor instead of breaking to
            // combat posture after only a small mana tick.
            bool const shouldMaintainDrink = hasDrinkAura && keepDrinkingFloor;
            if (!urgentlyNeedsDrink && !shouldMaintainDrink)
                return decision;

            break;
        }
    }

    // If already drinking, never decide away from drinking until at least 50%
    // mana has been recovered.
    if (hasDrinkAura && keepDrinkingFloor)
    {
        if (!hasEatAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            return { "eat", "pair food with active drink", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };
        return { "drink", "maintain drink until 50% mana", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, playerbot::PvpClassSpellContext::TargetMode::Self };
    }

    // Recovery auras should naturally break on movement and should not linger
    // once the corresponding resource has fully recovered.
    if (Player* mutablePlayer = const_cast<Player*>(player))
    {
        if (hasEatAura && (mutablePlayer->isMoving() || (!needsFood && !needsDrink)))
            mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        if (hasDrinkAura && (mutablePlayer->isMoving() || !needsDrink))
            mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
    }

    // When both health and mana are missing, mirror real player behavior:
    // apply both food and drink so recovery happens in parallel.
    if (needsFood && needsDrink)
    {
        if (!hasEatAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            return { "eat", "recover health out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };

        if (!hasDrinkAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            return { "drink", "recover mana out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, playerbot::PvpClassSpellContext::TargetMode::Self };
    }

    if (needsFood && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
        return { "eat", "recover health out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };

    if (needsDrink)
    {
        if (!hasEatAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            return { "eat", "pair food with mana recovery", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };

        if (IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            return { "drink", "recover mana out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, playerbot::PvpClassSpellContext::TargetMode::Self };
    }

    bool const inBattlegroundPreparation = player->InBattleground() &&
        (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION));
    bool const inActiveBattleground = player->InBattleground() && !inBattlegroundPreparation;
    bool const inArenaMap = player->InArena();
    bool const inActiveDuel = player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;

    // Keep arena and duel behavior conservative; battleground out-of-combat
    // mounting is intentionally allowed for parity with reference bots.
    if (inArenaMap || inActiveDuel)
        return decision;

    if (inBattlegroundPreparation)
        return decision;

    if (!IsStrictlyOutdoorsForMount(player))
        return decision;

    // Keep pressure logic responsive: don't choose an out-of-combat mount
    // action while hostile players are already within practical engage range.
    if (!player->InBattleground() && HasNearbyAttackableEnemyPlayer(player, 45.0f))
        return decision;

    if (IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT))
        if (SpellInfo const* defaultMountInfo = sSpellMgr->GetSpellInfo(SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT))
            if (CanAttemptMount(player, defaultMountInfo))
                return { "mount", "mount while outside and out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT, playerbot::PvpClassSpellContext::TargetMode::Self };

    if (uint32 const knownMountSpellId = SelectReadyKnownMountSpell(player))
        return { "mount", "mount while outside and out of combat", knownMountSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };

    return decision;
}

bool HasHostileTarget(Player const* player, Unit const* target)
{
    return player && target && target != player && target->IsAlive() && player->IsValidAttackTarget(target);
}

bool IsFriendlySupportTarget(Player const* player, Unit const* target)
{
    if (!player || !target || !target->IsAlive())
        return false;

    if (target == player)
        return true;

    if (player->IsValidAssistTarget(target))
        return true;

    Player const* targetPlayer = target->ToPlayer();
    if (!targetPlayer || !player->InBattleground() || !targetPlayer->InBattleground())
        return false;

    if (player->GetBattlegroundId() != targetPlayer->GetBattlegroundId())
        return false;

    uint32 const playerTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    uint32 const targetTeam = targetPlayer->GetBGTeam() ? targetPlayer->GetBGTeam() : targetPlayer->GetTeam();
    return playerTeam == targetTeam;
}

bool HasAnyAura(Unit const* unit, std::initializer_list<uint32> spellIds)
{
    if (!unit)
        return false;

    for (uint32 spellId : spellIds)
        if (unit->HasAura(spellId))
            return true;

    return false;
}

bool IsPhysicalDamageClass(uint8 classId)
{
    return classId == CLASS_WARRIOR || classId == CLASS_ROGUE || classId == CLASS_HUNTER;
}

bool IsUsingTwoHander(Player const* player)
{
    if (!player)
        return false;

    Item* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
    return mainHand && mainHand->GetTemplate() && mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
}

bool IsMeleeClass(Unit const* unit)
{
    Player const* player = unit ? unit->ToPlayer() : nullptr;
    if (!player)
        return false;

    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
            return true;
        case CLASS_SHAMAN:
        case CLASS_PALADIN:
            return IsUsingTwoHander(player);
        default:
            return false;
    }
}

bool HasAuraFromSpellChain(Unit const* unit, uint32 baseSpellId)
{
    if (!unit || !baseSpellId)
        return false;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
    if (!baseSpellInfo)
        return false;

    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
        if (unit->HasAura(chainSpellId))
            return true;

    return false;
}

bool HasAuraFromSpellChain(Unit const* unit, uint32 baseSpellId, ObjectGuid casterGuid)
{
    if (!unit || !baseSpellId || casterGuid.IsEmpty())
        return false;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
    if (!baseSpellInfo)
        return false;

    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
        if (unit->HasAura(chainSpellId, casterGuid))
            return true;

    return false;
}

bool HasHunterStingFromCaster(Unit const* unit, ObjectGuid casterGuid)
{
    return HasAuraFromSpellChain(unit, 25295, casterGuid) || // Serpent Sting
        HasAuraFromSpellChain(unit, 14280, casterGuid) ||   // Viper Sting
        HasAuraFromSpellChain(unit, 19386, casterGuid);     // Wyvern Sting
}

bool HasActivePaladinSeal(Player const* player)
{
    if (!player)
        return false;

    return HasAuraFromSpellChain(player, 21084) || // Seal of Righteousness
        HasAuraFromSpellChain(player, 20164) ||    // Seal of Justice
        HasAuraFromSpellChain(player, 20165) ||    // Seal of Light
        HasAuraFromSpellChain(player, 20166) ||    // Seal of Wisdom
        HasAuraFromSpellChain(player, 20375) ||    // Seal of Command
        HasAuraFromSpellChain(player, 31801) ||    // Seal of Vengeance
        HasAuraFromSpellChain(player, 53736) ||    // Seal of Corruption
        HasAuraFromSpellChain(player, 31892);      // Seal of Blood / Seal of the Martyr
}

ObjectGuid SelectFriendlyWithoutAuraFromSpellChain(Player const* player, uint32 baseSpellId, float maxDistance, bool includeSelf)
{
    if (!player || !player->FindMap() || !baseSpellId)
        return ObjectGuid::Empty;

    auto isEligible = [&](Player* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return false;
        if (!IsFriendlySupportTarget(player, candidate))
            return false;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return false;
        if (HasAuraFromSpellChain(candidate, baseSpellId))
            return false;

        return true;
    };

    if (includeSelf &&
        player->IsAlive() &&
        player->IsWithinLOSInMap(player) &&
        player->IsWithinDistInMap(player, maxDistance) &&
        !HasAuraFromSpellChain(player, baseSpellId))
    {
        return player->GetGUID();
    }

    Player* bestTarget = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isEligible(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestTarget = candidate;
        }
    }

    return bestTarget ? bestTarget->GetGUID() : ObjectGuid::Empty;
}

SpellDecision SelectPreparationBuffSpell(Player const* player)
{
    SpellDecision decision;
    if (!player || player->IsInCombat())
        return decision;

    auto hasUnimbuedWeapon = [player]()
    {
        Item const* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
        if (mainHand && !mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return true;

        Item const* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
        if (offHand && !offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return true;

        return false;
    };

    switch (player->GetClass())
    {
        case CLASS_ROGUE:
        {
            if (IsSpellReady(player, 11202) && hasUnimbuedWeapon())
                return { "rogue crippling poison prep", "coat both weapons before gates open", 11202, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_WARRIOR:
        {
            if (IsSpellReady(player, 2687) && !HasAuraFromSpellChain(player, 2687))
                return { "warrior bloodrage prep", "generate opening rage before gates open", 2687, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (IsSpellReady(player, 25289) && !HasAuraFromSpellChain(player, 25289))
                return { "warrior battle shout prep", "maintain battle shout before gates open", 25289, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_DRUID:
        {
            if (IsSpellReady(player, 21850) && !HasAuraFromSpellChain(player, 21850))
                return { "druid gift of the wild prep", "apply raid-wide stat buff before gates open", 21850, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (IsSpellReady(player, 9910))
            {
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAuraFromSpellChain(player, 9910, 45.0f, true); !targetGuid.IsEmpty())
                    return { "druid thorns prep", "apply thorns to nearby allies before gates open", 9910, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            break;
        }
        case CLASS_PRIEST:
        {
            if (IsSpellReady(player, 21564) && !HasAuraFromSpellChain(player, 21564))
                return { "priest prayer of fortitude prep", "buff nearby party before gates open", 21564, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (IsSpellReady(player, 27683) && !HasAuraFromSpellChain(player, 27683))
                return { "priest prayer of shadow protection prep", "buff nearby party before gates open", 27683, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (IsSpellReady(player, 27681) && !HasAuraFromSpellChain(player, 27681))
                return { "priest prayer of spirit prep", "buff nearby party before gates open", 27681, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_MAGE:
        {
            if (IsSpellReady(player, 10054) && !player->HasItemCount(8008))
                return { "create mana ruby prep", "create mana ruby before gates open", 10054, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (IsSpellReady(player, 23028) && !HasAuraFromSpellChain(player, 23028))
                return { "arcane brilliance prep", "buff nearby party before gates open", 23028, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (!HasAuraFromSpellChain(player, 10220) && IsSpellReady(player, 10220))
                return { "frost armor prep", "maintain armor before gates open", 10220, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_PALADIN:
        {
            if (IsSpellReady(player, 25898))
            {
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAuraFromSpellChain(player, 25898, 45.0f, true); !targetGuid.IsEmpty())
                    return { "paladin greater blessing of kings prep", "buff nearby team before gates open", 25898, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            break;
        }
        default:
            break;
    }

    return decision;
}

bool IsCasterClass(Unit const* unit)
{
    Player const* player = unit ? unit->ToPlayer() : nullptr;
    if (!player)
        return false;

    switch (player->GetClass())
    {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
        case CLASS_DRUID:
        case CLASS_SHAMAN:
        case CLASS_PALADIN:
            return true;
        default:
            return false;
    }
}

bool ShouldUseCurseOfTongues(Unit const* unit)
{
    Player const* player = unit ? unit->ToPlayer() : nullptr;
    if (!player)
        return false;

    // Curse of Tongues should focus on true caster targets and avoid hunters or melee hybrids.
    if (player->GetClass() == CLASS_HUNTER)
        return false;

    return IsCasterClass(player) && !IsMeleeClass(player);
}

bool IsTargetInvalidByImmunity(Player const* player, Unit const* target);

uint8 GetArmorPriority(Unit const* unit)
{
    if (!unit)
        return 4;

    switch (unit->GetClass())
    {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
            return 0; // Cloth
        case CLASS_ROGUE:
        case CLASS_DRUID:
            return 1; // Leather
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return 2; // Mail
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_DEATH_KNIGHT:
            return 3; // Plate
        default:
            return 4;
    }
}

Unit const* SelectWarriorPriorityTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        return HasHostileTarget(player, candidate) &&
            !IsTargetInvalidByImmunity(player, candidate) &&
            player->IsWithinLOSInMap(candidate) &&
            player->IsWithinDistInMap(candidate, maxDistance);
    };

    Unit const* best = nullptr;
    uint8 bestArmorPriority = std::numeric_limits<uint8>::max();
    float bestDistance = std::numeric_limits<float>::max();

    if (isCandidateUsable(preferredTarget))
    {
        best = preferredTarget;
        bestArmorPriority = GetArmorPriority(preferredTarget);
        bestDistance = player->GetDistance(preferredTarget);
    }

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isCandidateUsable(candidate))
            continue;

        uint8 const armorPriority = GetArmorPriority(candidate);
        float const distance = player->GetDistance(candidate);
        if (armorPriority < bestArmorPriority || (armorPriority == bestArmorPriority && distance < bestDistance))
        {
            best = candidate;
            bestArmorPriority = armorPriority;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectNearbyMeleeTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        return HasHostileTarget(player, candidate) &&
            !IsTargetInvalidByImmunity(player, candidate) &&
            IsMeleeClass(candidate) &&
            player->IsWithinLOSInMap(candidate) &&
            player->IsWithinDistInMap(candidate, maxDistance);
    };

    if (isCandidateUsable(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isCandidateUsable(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectNearbyEnemyTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        return HasHostileTarget(player, candidate) &&
            !IsTargetInvalidByImmunity(player, candidate) &&
            player->IsWithinLOSInMap(candidate) &&
            player->IsWithinDistInMap(candidate, maxDistance);
    };

    if (isCandidateUsable(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isCandidateUsable(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectNearbyEnemyManaTarget(Player const* player, Unit const* preferredTarget, float maxDistance, float minManaPct)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        if (!HasHostileTarget(player, candidate) || IsTargetInvalidByImmunity(player, candidate))
            return false;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return false;
        if (candidate->GetPowerType() != POWER_MANA)
            return false;

        return candidate->GetPowerPct(POWER_MANA) > minManaPct;
    };

    if (isCandidateUsable(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isCandidateUsable(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

uint32 CountNearbyUnsNaredEnemies(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (candidate->HasAura(1715))
            continue;

        ++count;
    }

    return count;
}

bool HasBreakableCrowdControl(Unit const* unit)
{
    // Approximation list for common "break on damage" PvP CCs.
    return unit->HasBreakableByDamageCrowdControlAura();
}

bool IsPolymorphed(Unit const* unit)
{
    return HasAnyAura(unit, { 118, 12824, 12825, 12826, 28272, 28271, 61305, 61721 });
}

bool IsWyvernStung(Unit const* unit)
{
    return HasAuraFromSpellChain(unit, 19386);
}

bool HasDotAura(Unit const* unit)
{
    if (!unit)
        return false;

    return unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) ||
        unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
}

bool IsInterruptibleCast(Unit const* unit)
{
    if (!unit)
        return false;

    auto isInterruptibleCurrentSpell = [&](CurrentSpellTypes spellType)
    {
        Spell const* currentSpell = unit->GetCurrentSpell(spellType);
        if (!currentSpell)
            return false;

        SpellInfo const* spellInfo = currentSpell->GetSpellInfo();
        if (!spellInfo || spellInfo->PreventionType != SPELL_PREVENTION_TYPE_SILENCE)
            return false;

        uint32 const state = currentSpell->getState();
        bool const isInInterruptiblePhase = state == SPELL_STATE_CASTING ||
            (state == SPELL_STATE_PREPARING && currentSpell->GetCastTime() > 0.0f);
        if (!isInInterruptiblePhase)
            return false;

        if (spellType == CURRENT_GENERIC_SPELL)
            return (spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;

        if (spellType == CURRENT_CHANNELED_SPELL)
            return (spellInfo->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT) != 0;

        return false;
    };

    return isInterruptibleCurrentSpell(CURRENT_GENERIC_SPELL) ||
        isInterruptibleCurrentSpell(CURRENT_CHANNELED_SPELL);
}

bool IsTargetInvalidByImmunity(Player const* player, Unit const* target)
{
    if (!player || !target)
        return true;

    if (Player const* targetPlayer = target->ToPlayer())
    {
        if (targetPlayer->isTotalImmune())
            return true;
    }

    if (target->HasAura(642)) // Divine Shield
        return true;

    if (target->HasAura(11958)) // Ice Block
        return true;

    if (IsPhysicalDamageClass(player->GetClass()) && HasAnyAura(target, { 1022, 5599, 10278 })) // Blessing of Protection ranks
        return true;

    if (HasBreakableCrowdControl(target))
        return true;

    return false;
}

Unit const* SelectClosestEnemyTarget(Player const* player, bool requireReachable)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive() || candidate == player)
            continue;
        if (!player->IsValidAttackTarget(candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (requireReachable && distance > 35.0f)
            continue;

        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectEnemyCastingTarget(Player const* player, float maxDistance, Unit const* preferredTarget = nullptr)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        return HasHostileTarget(player, candidate) &&
            !IsTargetInvalidByImmunity(player, candidate) &&
            candidate->HasUnitState(UNIT_STATE_CASTING) &&
            IsInterruptibleCast(candidate) &&
            player->IsWithinLOSInMap(candidate) &&
            player->IsWithinDistInMap(candidate, maxDistance);
    };

    if (isCandidateUsable(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isCandidateUsable(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

bool AnyEnemyPolymorphed(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return false;

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (IsPolymorphed(candidate))
            return true;
    }

    return false;
}

Unit const* SelectPolymorphTarget(Player const* player, Unit const* primaryTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    SpellInfo const* polymorphInfo = sSpellMgr->GetSpellInfo(12826);
    DiminishingGroup const polymorphDrGroup = polymorphInfo ? polymorphInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;

    std::vector<Unit const*> preferredTargets;
    std::vector<Unit const*> fallbackTargets;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (primaryTarget && candidate == primaryTarget)
            continue;
        if (candidate->GetClass() == CLASS_DRUID)
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (HasDotAura(candidate) || IsPolymorphed(candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (polymorphDrGroup != DIMINISHING_NONE && candidate->GetDiminishing(polymorphDrGroup) > DIMINISHING_LEVEL_0)
            continue;

        if (candidate->GetClass() == CLASS_PALADIN || candidate->GetClass() == CLASS_PRIEST)
            preferredTargets.push_back(candidate);
        else
            fallbackTargets.push_back(candidate);
    }

    if (!preferredTargets.empty())
        return preferredTargets[urand(0, preferredTargets.size() - 1)];

    if (!fallbackTargets.empty())
        return fallbackTargets[urand(0, fallbackTargets.size() - 1)];

    return nullptr;
}

bool AnyEnemyWyvernStung(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return false;

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (IsWyvernStung(candidate))
            return true;
    }

    return false;
}

Unit const* SelectWyvernStingTarget(Player const* player, Unit const* primaryTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    SpellInfo const* wyvernInfo = sSpellMgr->GetSpellInfo(24133);
    DiminishingGroup const wyvernDrGroup = wyvernInfo ? wyvernInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;

    std::vector<Unit const*> preferredTargets;
    std::vector<Unit const*> fallbackTargets;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (primaryTarget && candidate == primaryTarget)
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (HasDotAura(candidate) || IsWyvernStung(candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (wyvernDrGroup != DIMINISHING_NONE && candidate->GetDiminishing(wyvernDrGroup) > DIMINISHING_LEVEL_0)
            continue;

        if (candidate->GetClass() == CLASS_SHAMAN || candidate->GetClass() == CLASS_DRUID || candidate->GetClass() == CLASS_PALADIN)
            preferredTargets.push_back(candidate);
        else
            fallbackTargets.push_back(candidate);
    }

    if (!preferredTargets.empty())
        return preferredTargets[urand(0, preferredTargets.size() - 1)];

    if (!fallbackTargets.empty())
        return fallbackTargets[urand(0, fallbackTargets.size() - 1)];

    return nullptr;
}

Unit const* SelectFriendlyCurseTarget(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto hasDispellableCurse = [&](Unit const* target)
    {
        if (!target || !target->IsAlive())
            return false;

        DispelChargesList dispelList;
        target->GetDispellableAuraList(player, (1 << DISPEL_CURSE), dispelList);
        return !dispelList.empty();
    };

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (!hasDispellableCurse(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    if (best)
        return best;

    return hasDispellableCurse(player) ? player : nullptr;
}

Unit const* SelectRogueBlindTarget(Player const* player, Unit const* primaryTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isPriorityBlindTarget = [&](Unit const* candidate)
    {
        if (!HasHostileTarget(player, candidate))
            return false;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return false;
        if (IsTargetInvalidByImmunity(player, candidate))
            return false;
        if (!(candidate->GetClass() == CLASS_DRUID || candidate->GetClass() == CLASS_SHAMAN || candidate->GetClass() == CLASS_PALADIN))
            return false;
        if (HasAnyAura(candidate, { 2893 })) // Abolish Poison
            return false;
        if (HasBreakableCrowdControl(candidate) || HasDotAura(candidate))
            return false;
        return true;
    };

    Unit const* bestSecondary = nullptr;
    float bestSecondaryDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isPriorityBlindTarget(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (primaryTarget && candidate->GetGUID() == primaryTarget->GetGUID())
            continue;

        if (distance < bestSecondaryDistance)
        {
            bestSecondary = candidate;
            bestSecondaryDistance = distance;
        }
    }

    return bestSecondary;
}

Unit const* SelectWarlockFearTarget(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto hasFearFromPlayer = [&](Player const* candidate)
    {
        if (!candidate)
            return false;

        Unit::AuraEffectList const& fearAuras = candidate->GetAuraEffectsByType(SPELL_AURA_MOD_FEAR);
        for (AuraEffect const* auraEffect : fearAuras)
        {
            if (!auraEffect)
                continue;
            if (auraEffect->GetCasterGUID() == player->GetGUID())
                return true;
        }

        return false;
    };

    SpellInfo const* fearInfo = sSpellMgr->GetSpellInfo(6215);
    DiminishingGroup const fearDrGroup = fearInfo ? fearInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;

    auto isFearInvalidTarget = [&](Player const* candidate)
    {
        if (!candidate)
            return true;

        if (fearInfo && candidate->IsImmunedToSpell(fearInfo, player))
            return true;

        if (fearDrGroup != DIMINISHING_NONE && candidate->GetDiminishing(fearDrGroup) > DIMINISHING_LEVEL_0)
            return true;

        return false;
    };

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (hasFearFromPlayer(candidate))
            return nullptr;
    }

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    std::vector<Unit const*> fallbackCandidates;
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (isFearInvalidTarget(candidate))
            continue;

        if (!(candidate->GetClass() == CLASS_PALADIN || candidate->GetClass() == CLASS_PRIEST))
        {
            fallbackCandidates.push_back(candidate);
            continue;
        }

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    if (best)
        return best;

    if (fallbackCandidates.empty())
        return nullptr;

    return fallbackCandidates[urand(0u, static_cast<uint32>(fallbackCandidates.size() - 1))];
}

Unit const* SelectEnemyClassTarget(Player const* player, uint8 classId, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (candidate->GetClass() != classId)
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectFriendlyHealthTarget(Player const* player, float maxDistance, float maxHealthPct)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    Unit const* selfCandidate = nullptr;
    float bestHealth = 101.0f;
    float bestDistance = std::numeric_limits<float>::max();

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return;
        if (!IsFriendlySupportTarget(player, candidate))
            return;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return;

        float const healthPct = candidate->GetHealthPct();
        if (healthPct > maxHealthPct)
            return;

        float const distance = player->GetDistance(candidate);
        if (candidate == player)
        {
            selfCandidate = player;
            return;
        }

        if (healthPct < bestHealth || (std::abs(healthPct - bestHealth) < 0.1f && distance < bestDistance))
        {
            best = candidate;
            bestHealth = healthPct;
            bestDistance = distance;
        }
    };

    evaluateCandidate(player);

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best ? best : selfCandidate;
}

Unit const* SelectFriendlyCasterTarget(Player const* player, float maxDistance, float maxHealthPct)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    Unit const* selfCandidate = nullptr;
    float bestHealth = 101.0f;
    float bestDistance = std::numeric_limits<float>::max();

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return;
        if (!IsCasterClass(candidate))
            return;
        if (!IsFriendlySupportTarget(player, candidate))
            return;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return;

        float const healthPct = candidate->GetHealthPct();
        if (healthPct > maxHealthPct)
            return;

        float const distance = player->GetDistance(candidate);
        if (candidate == player)
        {
            selfCandidate = player;
            return;
        }

        if (healthPct < bestHealth || (std::abs(healthPct - bestHealth) < 0.1f && distance < bestDistance))
        {
            best = candidate;
            bestHealth = healthPct;
            bestDistance = distance;
        }
    };

    evaluateCandidate(player);

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best ? best : selfCandidate;
}

Unit const* SelectFriendlyDispelTarget(Player const* player, DispelType dispelType, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto hasDispellableAura = [&](Unit const* target)
    {
        if (!target || !target->IsAlive())
            return false;

        DispelChargesList dispelList;
        target->GetDispellableAuraList(player, (1 << dispelType), dispelList);
        return !dispelList.empty();
    };

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (!hasDispellableAura(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    if (best)
        return best;

    return hasDispellableAura(player) ? player : nullptr;
}

Unit const* SelectEnemyNonBreakableCrowdControlTarget(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    uint32 constexpr mechanicMask =
        (1 << MECHANIC_ROOT) |
        (1 << MECHANIC_STUN) |
        (1 << MECHANIC_FREEZE) |
        (1 << MECHANIC_SNARE);

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!candidate->HasAuraWithMechanic(mechanicMask))
            continue;
        if (HasBreakableCrowdControl(candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectEnemyDispelTarget(Player const* player, DispelType dispelType, Unit const* preferredTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto hasDispellableAura = [&](Unit const* target)
    {
        if (!target || !target->IsAlive())
            return false;
        if (!HasHostileTarget(player, target))
            return false;
        if (!player->IsWithinLOSInMap(target) || !player->IsWithinDistInMap(target, maxDistance))
            return false;

        DispelChargesList dispelList;
        target->GetDispellableAuraList(player, (1 << dispelType), dispelList);
        for (DispelableAura const& dispelable : dispelList)
        {
            Aura const* aura = dispelable.GetAura();
            if (!aura)
                continue;

            // Sweeping Strikes should never be a valid offensive-dispel target
            // for playerbots, even if external spell data marks it dispellable.
            if (HasAuraFromSpellChain(target, 12328) && HasAuraFromSpellChain(target, aura->GetId()))
                continue;

            return true;
        }

        return false;
    };

    if (hasDispellableAura(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!hasDispellableAura(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectFriendlyLowManaTarget(Player const* player, float maxDistance, float maxManaPct)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestMana = 101.0f;
    float bestDistance = std::numeric_limits<float>::max();

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return;
        if (!IsFriendlySupportTarget(player, candidate))
            return;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return;
        if (candidate->GetMaxPower(POWER_MANA) <= 0)
            return;

        float const manaPct = candidate->GetPowerPct(POWER_MANA);
        if (manaPct > maxManaPct)
            return;

        float const distance = player->GetDistance(candidate);
        if (manaPct < bestMana || (std::abs(manaPct - bestMana) < 0.1f && distance < bestDistance))
        {
            best = candidate;
            bestMana = manaPct;
            bestDistance = distance;
        }
    };

    evaluateCandidate(player);
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best;
}

Unit const* SelectFriendlySnaredTarget(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isSnared = [](Unit const* target)
    {
        if (!target || target->HasStealthAura())
            return false;

        return target && (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) || target->HasAuraWithMechanic(1 << MECHANIC_ROOT));
    };

    if (isSnared(player))
        return player;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive())
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (!isSnared(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}


bool IsRootedOrSnared(Unit const* unit)
{
    if (!unit)
        return false;

    return unit->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
        unit->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_SNARE));
}

bool HasShieldEquipped(Player const* player)
{
    if (!player)
        return false;

    Item const* offHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    return offHand && offHand->GetTemplate() && offHand->GetTemplate()->InventoryType == INVTYPE_SHIELD;
}

Unit const* SelectFriendlyMeleePressureTarget(Player const* player, float maxDistance, float maxHealthPct)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestHealth = 101.0f;
    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive() || candidate->GetHealthPct() > maxHealthPct)
            return;
        if (!IsFriendlySupportTarget(player, candidate))
            return;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return;

        bool underMeleePressure = false;
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* enemy = itr->GetSource();
            if (!HasHostileTarget(player, enemy) || !IsMeleeClass(enemy))
                continue;
            if (enemy->IsWithinMeleeRange(candidate))
            {
                underMeleePressure = true;
                break;
            }
        }

        if (!underMeleePressure)
            return;

        if (candidate->GetHealthPct() < bestHealth)
        {
            best = candidate;
            bestHealth = candidate->GetHealthPct();
        }
    };

    evaluateCandidate(player);
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best;
}

Unit const* SelectStunnedEnemyTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto usable = [&](Unit const* candidate)
    {
        return HasHostileTarget(player, candidate) &&
            !IsTargetInvalidByImmunity(player, candidate) &&
            candidate->HasAuraWithMechanic(1 << MECHANIC_STUN) &&
            player->IsWithinLOSInMap(candidate) &&
            player->IsWithinDistInMap(candidate, maxDistance);
    };

    if (usable(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!usable(candidate))
            continue;
        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

Unit const* SelectUnstunDREnemyTarget(Player const* player, Unit const* preferredTarget, float maxDistance, uint32 stunSpellId)
{
    if (!player || !player->FindMap())
        return nullptr;

    SpellInfo const* stunInfo = sSpellMgr->GetSpellInfo(stunSpellId);
    DiminishingGroup const stunDrGroup = stunInfo ? stunInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;
    auto usable = [&](Unit const* candidate)
    {
        if (!HasHostileTarget(player, candidate) || !player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return false;
        if (IsTargetInvalidByImmunity(player, candidate))
            return false;
        return stunDrGroup == DIMINISHING_NONE || candidate->GetDiminishing(stunDrGroup) == DIMINISHING_LEVEL_0;
    };

    if (usable(preferredTarget))
        return preferredTarget;

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        if (usable(itr->GetSource()))
            return itr->GetSource();

    return nullptr;
}

Unit const* SelectPredatorsSwiftnessRootTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* bestMelee = nullptr;
    Unit const* bestFallback = nullptr;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate) || !player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate) || HasAuraFromSpellChain(candidate, 1044))
            continue;
        if (IsMeleeClass(candidate))
            bestMelee = candidate;
        else if (!bestFallback)
            bestFallback = candidate;
    }

    if (bestMelee)
        return bestMelee;
    return bestFallback ? bestFallback : preferredTarget;
}

bool AllFriendlyPlayersHealthy(Player const* player, float maxDistance, float minHealthPct)
{
    if (!player || !player->FindMap())
        return true;

    if (player->GetHealthPct() < minHealthPct)
        return false;

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive() || !IsFriendlySupportTarget(player, candidate))
            continue;
        if (player->IsWithinLOSInMap(candidate) && player->IsWithinDistInMap(candidate, maxDistance) && candidate->GetHealthPct() < minHealthPct)
            return false;
    }

    return true;
}

Unit const* SelectFriendlyMissingBuffTarget(Player const* player, uint32 baseSpellId, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    std::vector<Unit const*> candidates;
    if (!HasAuraFromSpellChain(player, baseSpellId))
        candidates.push_back(player);

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive() || !IsFriendlySupportTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (HasAuraFromSpellChain(candidate, baseSpellId))
            continue;
        candidates.push_back(candidate);
    }

    if (candidates.empty())
        return nullptr;

    return candidates[urand(0, candidates.size() - 1)];
}

uint32 CountNearbyEnemies(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        ++count;
    }

    return count;
}

uint32 CountNearbyFriendlyPlayers(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive())
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        ++count;
    }

    return count;
}

ObjectGuid SelectCombatTargetGuid(Player const* player)
{
    if (!player)
        return ObjectGuid::Empty;

    if (ObjectGuid const selectedGuid = player->GetTarget(); !selectedGuid.IsEmpty())
        if (Unit const* selectedTarget = ObjectAccessor::GetUnit(*player, selectedGuid); HasHostileTarget(player, selectedTarget) && !IsTargetInvalidByImmunity(player, selectedTarget))
            return selectedGuid;

    return ObjectGuid::Empty;
}

ObjectGuid SelectAllyTargetGuid(Player const* player)
{
    if (!player)
        return ObjectGuid::Empty;

    ObjectGuid const selectedGuid = player->GetTarget();
    if (selectedGuid.IsEmpty() || selectedGuid == player->GetGUID())
        return ObjectGuid::Empty;

    Unit const* selected = ObjectAccessor::GetUnit(*player, selectedGuid);
    if (!selected || !selected->IsAlive())
        return ObjectGuid::Empty;

    if (!IsFriendlySupportTarget(player, selected))
        return ObjectGuid::Empty;

    if (!player->IsWithinLOSInMap(selected))
        return ObjectGuid::Empty;

    return selectedGuid;
}

SpellDecision SelectHunterSpell(Player const* player, Unit const* target, bool inMelee, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    Unit const* activeTarget = target;

    if (!HasHostileTarget(player, activeTarget))
        return decision;

    UpdateHunterCombatMode(player, activeTarget);

    Pet const* pet = player->GetPet();
    bool const hasLivingPet = pet && pet->IsAlive();
    bool const hasDeadPet = pet && !pet->IsAlive();

    Unit const* enemyOnTopTarget = SelectNearbyEnemyTarget(player, activeTarget, 5.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 20.0f, activeTarget);
    Unit const* closeMeleeThreat = SelectNearbyMeleeTarget(player, enemyOnTopTarget, 5.0f);
    Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, GetConfiguredLongRange());
    HunterPvpSpec const hunterSpec = GetHunterPvpSpec(profileSelection);
    bool const isSurvivalHunter = hunterSpec == HunterPvpSpec::Survival;
    bool const isMarksmanshipHunter = hunterSpec == HunterPvpSpec::Marksmanship;
    Unit const* manaTarget = isSurvivalHunter
        ? SelectNearbyEnemyManaTarget(player, activeTarget, GetConfiguredLongRange(), 0.0f)
        : SelectNearbyEnemyTarget(player, activeTarget, GetConfiguredLongRange());
    Unit const* wyvernTarget = (isSurvivalHunter && IsSpellReady(player, 24133) && !AnyEnemyWyvernStung(player, 40.0f))
        ? SelectWyvernStingTarget(player, activeTarget, 30.0f)
        : nullptr;

    target = activeTarget;
    bool const targetClose = player->IsWithinDistInMap(target, kReferenceHunterSwitchDistance);
    bool const enemyOnTop = HasHostileTarget(player, enemyOnTopTarget);
    bool const enemyNear = player->IsWithinDistInMap(target, GetConfiguredCloseRange());
    bool const rangedMode = IsHunterInRangedMode(player);
    bool const preferredTrapReady = isSurvivalHunter && enemyOnTopTarget && HasDotAura(enemyOnTopTarget) ? IsSpellReady(player, 13809) : IsSpellReady(player, 14311);

    bool const targetSnaredOrStunned = target &&
        (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
         target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN)));

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HealthBelowPct(35) && IsSpellReady(player, 19263), 35.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, enemyOnTop && HasAuraFromSpellChain(enemyOnTopTarget, 14268) && IsSpellReady(player, 5384) && preferredTrapReady, 35.0f,
        { "hunter feign death", "set up freezing trap while pressured in melee", 5384, playerbot::PvpClassSpellContext::TargetMode::Self, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isSurvivalHunter && IsSpellReady(player, 23989) && !HasAuraFromSpellChain(player, 19263), 34.0f,
        { "hunter readiness", "reset cooldowns after deterrence has fallen", 23989, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isSurvivalHunter && enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) && IsSpellReady(player, 20910), 34.5f,
        { "hunter counterattack", "strike any enemy in melee range when available", 20910, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isSurvivalHunter && wyvernTarget && IsSpellReady(player, 24133), 30.0f,
        { "hunter wyvern sting", "crowd-control a non-dotted enemy support target", 24133, playerbot::PvpClassSpellContext::TargetMode::Enemy, wyvernTarget ? wyvernTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 14325) && IsSpellReady(player, 14325), 29.5f,
        { "hunter mark", "mark rogue targets for anti-stealth pressure", 14325, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && !HasAuraFromSpellChain(player, 20906) && IsSpellReady(player, 20906), 27.5f,
        { "hunter trueshot aura", "maintain personal buff aura", 20906, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !hasLivingPet && !hasDeadPet && IsSpellReady(player, 883), 26.0f,
        { "hunter call pet", "summon active stable pet when no pet is present", 883, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasDeadPet && !player->IsInCombat() && IsSpellReady(player, 982), 25.0f,
        { "hunter revive pet", "recover pet out of combat", 982, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && enemyOnTop && enemyOnTopTarget->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 19503), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && nearbyCastingTarget && IsSpellReady(player, 19503), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates,
        enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) &&
            IsSpellReady(player, 14268) && !HasAuraFromSpellChain(enemyOnTopTarget, 14268),
        21.0f,
        { "hunter wing clip", "close-range fallback snare", 14268, playerbot::PvpClassSpellContext::TargetMode::Enemy,
            enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !targetClose && !targetSnaredOrStunned && IsSpellReady(player, 5116), 20.0f,
        { "hunter concussive shot", "kite or chase control", 5116, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isSurvivalHunter && activeTarget && activeTarget->GetPowerType() != POWER_MANA &&
        !HasHunterStingFromCaster(activeTarget, player->GetGUID()) && IsSpellReady(player, 25295), 19.75f,
        { "hunter serpent sting", "apply ranged dot pressure to non-mana kill target", 25295, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 25295) && IsSpellReady(player, 25295), 19.5f,
        { "hunter serpent sting", "apply ranged dot pressure", 25295, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && rangedMode && !enemyNear && IsSpellReady(player, 20904), 18.0f,
        { "hunter aimed shot", "long cast pressure from range", 20904, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, rangedMode && !inMelee && IsSpellReady(player, 25294), 17.0f,
        { "hunter multi-shot", "ranged burst pressure", 25294, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, rangedMode && !inMelee && IsSpellReady(player, 3045), 16.0f,
        { "hunter rapid fire", "burst cooldown while freecasting at range", 3045, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, manaTarget && manaTarget->GetPowerType() == POWER_MANA && !HasAuraFromSpellChain(manaTarget, 14280) && IsSpellReady(player, 14280), 15.0f,
        { "hunter viper sting", "drain mana on mana users", 14280, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaTarget ? manaTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && enemyOnTop && (!IsSpellReady(player, 5384) || !IsSpellReady(player, 14311)) && IsSpellReady(player, 19503) && !HasBreakableCrowdControl(enemyOnTopTarget), 14.0f,
        { "hunter scatter shot", "fallback peel when trap setup unavailable", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, enemyOnTop && closeMeleeThreat && (isSurvivalHunter || !IsSpellReady(player, 19503)) && (!IsSpellReady(player, 5384) || !preferredTrapReady) && IsSpellReady(player, 19263), 13.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectMageSpell(Player const* player, Unit const* target, bool inMelee, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const closePressure = hasHostileTarget && player->IsWithinDistInMap(target, GetConfiguredMeleeRange());
    float const manaPct = player->GetPowerPct(POWER_MANA);
    bool const isFireMage = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    Unit const* cursedTarget = IsSpellReady(player, 475) ? SelectFriendlyCurseTarget(player, 40.0f) : nullptr;
    Unit const* castingTarget = IsSpellReady(player, 2139) ? SelectEnemyCastingTarget(player, 30.0f, target) : nullptr;
    Unit const* polymorphTarget =
        (IsSpellReady(player, 12826) && !AnyEnemyPolymorphed(player, 40.0f)) ? SelectPolymorphTarget(player, target, 30.0f) : nullptr;
    return SelectFromTriggerGraph(player, target, nullptr,
    {
        { "critical health", !isFireMage && player->HealthBelowPct(25) && IsSpellReady(player, 11958), 60.0f,
            { "mage ice block", "self-preservation emergency", 11958, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "enemy too close for spell", closePressure && IsSpellReady(player, 1953), 45.0f,
            { "mage blink", "escape melee pressure", 1953, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "enemy is casting", castingTarget && IsSpellReady(player, 2139), 44.0f,
            { "mage counterspell", "interrupt any enemy cast in range", 2139, playerbot::PvpClassSpellContext::TargetMode::Enemy, castingTarget ? castingTarget->GetGUID() : ObjectGuid::Empty } },
        { "enemy too close for spell", closePressure && IsSpellReady(player, 10230), 43.0f,
            { "mage frost nova", "close defensive peel", 10230, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "enemy too close for spell", closePressure && target && IsMeleeClass(target) && IsSpellReady(player, isFireMage ? uint32(31661) : uint32(10161)), 42.0f,
            { isFireMage ? "mage dragon's breath" : "mage cone of cold", isFireMage ? "disorient nearby melee pressure" : "defensive snare versus nearby melee", isFireMage ? uint32(31661) : uint32(10161), playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "low mana", manaPct < 25.0f && IsSpellReady(player, 12051), 41.0f,
            { "mage evocation", "recover mana below 25 percent", 12051, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "high mana", manaPct < 50.0f && player->HasItemCount(8008), 40.0f,
            { "use mana ruby", "consume mana ruby below 50 percent mana", 22044, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), 8008 } },
        { "remove curse", cursedTarget != nullptr, 39.0f,
            { "remove lesser curse", "dispel curse from friendly target", 475, (cursedTarget == player) ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty } },
        { "ice barrier", !isFireMage && !HasAuraFromSpellChain(player, 13033) && IsSpellReady(player, 13033), 35.0f,
            { "mage ice barrier", "maintain defensive absorb shield", 13033, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "enemy low health", hasHostileTarget && target && IsSpellReady(player, 10199), 30.0f,
            { "mage fire blast", isFireMage ? "instant fire blast pressure on cooldown" : "instant execute pressure on low health target", 10199, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "clustered enemies", isFireMage && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 13021), 29.5f,
            { "mage blast wave", "area fire pressure against nearby enemies", 13021, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "polymorph", polymorphTarget && !polymorphTarget->HealthBelowPct(75), 29.0f,
            { "mage polymorph", "priority crowd control on non-dotted paladin/priest targets", 12826, playerbot::PvpClassSpellContext::TargetMode::Enemy, polymorphTarget ? polymorphTarget->GetGUID() : ObjectGuid::Empty } },
        { "default ranged", hasHostileTarget && IsSpellReady(player, isFireMage ? uint32(10207) : uint32(25304)), 18.0f,
            { isFireMage ? "mage scorch" : "mage frostbolt", isFireMage ? "default fire pressure" : "default ranged pressure", isFireMage ? uint32(10207) : uint32(25304), playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "maintain buff", !player->IsInCombat() && IsSpellReady(player, 10157) && !player->HasAura(10157), 10.0f,
            { "arcane intellect", "arcane intellect", 10157, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "maintain buff", !player->IsInCombat() && IsSpellReady(player, 10220) && !player->HasAura(10220), 9.0f,
            { "frost armor", "frost armor", 10220, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "mana gem missing", IsSpellReady(player, 10054) && !player->HasItemCount(8008), 8.0f,
            { "create mana ruby", "create mana ruby", 10054, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "defensive reset", !isFireMage && !IsSpellReady(player, 11958) && IsSpellReady(player, 12472), 7.0f,
            { "mage cold snap", "reset frost defenses when ice block unavailable", 12472, playerbot::PvpClassSpellContext::TargetMode::Self } }
    });
}

SpellDecision SelectPriestSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    Unit const* debuffedAlly = (!dispelThrottleActive && IsSpellReady(player, 988)) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, GetConfiguredHealRange()) : nullptr;
    Unit const* enemyBuffedTarget = (!dispelThrottleActive && IsSpellReady(player, 988) && hasHostileTarget) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, GetConfiguredSpellRange()) : nullptr;
    Unit const* shieldTarget = IsSpellReady(player, 10901) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 50.0f) : nullptr;
    Unit const* renewTarget = IsSpellReady(player, 10929) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 80.0f) : nullptr;
    Unit const* healTarget = IsSpellReady(player, 10917) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 85.0f) : nullptr;
    Unit const* emergencyLowAlly = IsSpellReady(player, 10917) ? SelectFriendlyHealthTarget(player, 15.0f, 25.0f) : nullptr;
    Unit const* casterAlly = (player->IsInCombat() && IsSpellReady(player, 10060)) ? SelectFriendlyCasterTarget(player, GetConfiguredHealRange(), 100.0f) : nullptr;
    Unit const* controlledTarget = IsSpellReady(player, 27605) ? SelectEnemyNonBreakableCrowdControlTarget(player, 30.0f) : nullptr;
    Unit const* manaBurnTarget = IsSpellReady(player, 10876) ? SelectNearbyEnemyManaTarget(player, target, GetConfiguredLongRange(), 25.0f) : nullptr;
    Unit const* rogueTarget = IsSpellReady(player, 27605) ? SelectEnemyClassTarget(player, CLASS_ROGUE, GetConfiguredLongRange()) : nullptr;
    bool const isHolyPriest = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    Unit const* spiritHealTarget = (isHolyPriest && HasAuraFromSpellChain(player, 27827) && IsSpellReady(player, 10917)) ? SelectFriendlyHealthTarget(player, 40.0f, 100.0f) : nullptr;
    Unit const* fearWardTarget = (player->GetRace() == RACE_DWARF && IsSpellReady(player, 6346)) ? SelectFriendlyMissingBuffTarget(player, 6346, 40.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, isHolyPriest && player->HealthBelowPct(50) && IsSpellReady(player, 81321), 61.0f,
        { "priest spirit of redemption", "enter spirit of redemption below half health", 81321, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, spiritHealTarget, 60.5f,
        { "priest flash heal", "spam flash heal during spirit of redemption", 10917, spiritHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, spiritHealTarget ? spiritHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, fearWardTarget, 60.2f,
        { "priest fear ward", "place fear ward on a random unwarded ally", 6346, fearWardTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, fearWardTarget ? fearWardTarget->GetGUID() : ObjectGuid::Empty });

    if (profileSelection.profile == ClassicClassProfile::PrimaryClassic)
    {
        AddDecisionCandidate(candidates, emergencyLowAlly, 47.0f,
            { "priest flash heal", "prioritize emergency healing for nearby ally below 25 percent health", 10917, emergencyLowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyLowAlly ? emergencyLowAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !emergencyLowAlly && debuffedAlly, 46.0f,
            { "priest dispel magic ally", "prioritize dispelling magic debuffs from allies", 988, debuffedAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, debuffedAlly ? debuffedAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !emergencyLowAlly && enemyBuffedTarget, 45.0f,
            { "priest dispel magic enemy", "prioritize dispelling magic buffs from enemies", 988, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyBuffedTarget ? enemyBuffedTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, shieldTarget && !HasAuraFromSpellChain(shieldTarget, 10901), 44.0f,
            { "priest power word shield ally", "protect ally below 50 percent health", 10901, shieldTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, shieldTarget ? shieldTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !isHolyPriest && casterAlly, 30.0f,
            { "priest power infusion", "boost nearby caster throughput in combat", 10060, casterAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, casterAlly ? casterAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !player->IsInCombat() && !player->HasAura(10938) && IsSpellReady(player, 10938), 14.0f,
            { "priest power word fortitude", "maintain fortitude out of combat", 10938, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 10958) && IsSpellReady(player, 10958), 13.0f,
            { "priest shadow protection", "maintain shadow protection out of combat", 10958, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 1006) && IsSpellReady(player, 1006), 12.0f,
            { "priest inner fire", "maintain inner fire out of combat", 1006, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, renewTarget && !HasAuraFromSpellChain(renewTarget, 10929), 28.0f,
            { "priest renew", "maintain renew on moderately injured allies", 10929, renewTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, renewTarget ? renewTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, healTarget, 27.0f,
            { "priest flash heal", "heal party with flash heal", 10917, healTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, healTarget ? healTarget->GetGUID() : ObjectGuid::Empty });
    }

    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 27605), 22.0f,
        { "priest shadow word pain", "maintain dot pressure on rogues", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isHolyPriest && manaBurnTarget, 21.0f,
        { "priest mana burn", "burn mana from enemy casters", 10876, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaBurnTarget ? manaBurnTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isHolyPriest && hasHostileTarget && IsSpellReady(player, 10934), 21.0f,
        { "priest smite", "holy fallback damage instead of mana burn", 10934, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 10.0f) >= 2 && CountNearbyFriendlyPlayers(player, 10.0f) >= 2 && IsSpellReady(player, 27801), 20.0f,
        { "priest holy nova", "aoe pressure and splash healing in melee cluster", 27801, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 8.0f) >= 2 && IsSpellReady(player, 10890), 19.5f,
        { "priest psychic scream", "fear nearby enemies when surrounded", 10890, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && IsSpellReady(player, 27605) && !HasBreakableCrowdControl(target) && !HasAuraFromSpellChain(target, 27605), 19.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled or open targets", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, controlledTarget && !HasAuraFromSpellChain(controlledTarget, 27605), 18.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled targets", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy, controlledTarget ? controlledTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, hasHostileTarget && target && IsLowOrOutOfManaForFallback(player) && HasWandEquipped(player) && IsSpellReady(player, 5019), 18.5f,
        { "priest shoot wand", "fallback to wand pressure while low on mana", 5019, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, IsSpellReady(player, 10917) && player->HealthBelowPct(85), 17.0f,
        { "priest flash heal", "fallback self-healing while under pressure", 10917, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && !HasBreakableCrowdControl(target) && HasWandEquipped(player) && IsSpellReady(player, 5019), 8.0f,
        { "priest shoot wand", "default offensive fallback when no better priest action is available", 5019, playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityCastableDecision(candidates, player, target, allyTarget);
}

SpellDecision SelectDruidSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const recoveredFromPolymorph =
        playerbot::PvpClassActions::GetLastExecutionStatus(player) == "cast_failed_crowd_controlled_polymorph" &&
        !player->HasUnitState(UNIT_STATE_CONFUSED) &&
        !player->HasAuraType(SPELL_AURA_MOD_CONFUSE) &&
        !player->IsPolymorphed();

    Unit const* lowManaAlly = IsSpellReady(player, 29166) ? SelectFriendlyLowManaTarget(player, 40.0f, 10.0f) : nullptr;
    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    Unit const* cursedTarget = (!dispelThrottleActive && IsSpellReady(player, 2782)) ? SelectFriendlyDispelTarget(player, DISPEL_CURSE, 40.0f) : nullptr;
    Unit const* poisonedTarget = (!dispelThrottleActive && IsSpellReady(player, 2893)) ? SelectFriendlyDispelTarget(player, DISPEL_POISON, 40.0f) : nullptr;
    Unit const* swiftmendTarget = IsSpellReady(player, 18562) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* emergencyLowTarget = (IsSpellReady(player, 17116) && IsSpellReady(player, 25297)) ? SelectFriendlyHealthTarget(player, 40.0f, 25.0f) : nullptr;
    Unit const* emergencyTarget = IsSpellReady(player, 25297) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* regrowthTarget = IsSpellReady(player, 9858) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
    Unit const* rejuvTarget = IsSpellReady(player, 25299) ? SelectFriendlyHealthTarget(player, 40.0f, 90.0f) : nullptr;
    Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, 30.0f);
    Unit const* meleeThreat = SelectNearbyMeleeTarget(player, target, 8.0f);
    Unit const* moonfireExecuteTarget = nullptr;
    if (IsSpellReady(player, 8921))
    {
        Unit const* nearbyTarget = SelectNearbyEnemyTarget(player, target, 25.0f);
        if (nearbyTarget && nearbyTarget->HealthBelowPct(20))
            moonfireExecuteTarget = nearbyTarget;
    }

    bool const isFeralDruid = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    if (isFeralDruid && HasHostileTarget(player, target))
    {
        bool const inCat = HasAuraFromSpellChain(player, 768);
        bool const inBear = HasAuraFromSpellChain(player, 5487);
        Unit const* lowAlly = SelectFriendlyHealthTarget(player, 40.0f, 80.0f);
        Unit const* rootTarget = SelectPredatorsSwiftnessRootTarget(player, target, 30.0f);
        bool const safeAgain = !player->HealthBelowPct(60) || !SelectNearbyMeleeTarget(player, target, 8.0f);

        std::vector<PrioritizedSpellDecision> feralCandidates;
        AddDecisionCandidate(feralCandidates, player->HasAura(69369) && lowAlly && IsSpellReady(player, 9858), 70.0f,
            { "druid regrowth", "predator's swiftness regrowth on lowest ally", 9858, lowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowAlly ? lowAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, player->HasAura(69369) && AllFriendlyPlayersHealthy(player, 40.0f, 80.0f) && rootTarget && IsSpellReady(player, 9853), 69.0f,
            { "druid entangling roots", "predator's swiftness root when allies are healthy", 9853, playerbot::PvpClassSpellContext::TargetMode::Enemy, rootTarget ? rootTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, player->HealthBelowPct(60) && meleeThreat && !inBear && IsSpellReady(player, 5487), 68.0f,
            { "druid bear form", "swap bear under melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && player->GetComboPoints() >= 5 && player->GetPowerPct(POWER_MANA) < 50.0f && !player->HasAura(89758) && IsSpellReady(player, 89758), 67.0f,
            { "druid thinnervate", "bear combo point thinnervate", 89758, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 16979), 66.0f,
            { "druid feral charge bear", "escape by charging a distant target", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 22842), 65.0f,
            { "druid frenzied regeneration", "bear survival recovery", 22842, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 5229), 64.0f,
            { "druid enrage", "generate bear rage", 5229, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 9898), 63.0f,
            { "druid demoralizing roar", "debuff melee attackers", 9898, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && player->GetPower(POWER_RAGE) >= 150 && IsSpellReady(player, 9881), 62.0f,
            { "druid maul", "dump extra bear rage", 9881, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inBear && safeAgain && IsSpellReady(player, 768), 61.0f,
            { "druid cat form", "return to cat form once safe", 768, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, !inCat && !inBear && IsSpellReady(player, 768), 60.0f,
            { "druid cat form", "prefer cat form for feral pressure", 768, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inCat && IsRootedOrSnared(player) && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 768), 59.0f,
            { "druid cat form", "powershift root or snare", 768, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inCat && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 9821), 58.0f,
            { "druid dash", "catch target in cat form", 9821, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inCat && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 49376), 57.0f,
            { "druid feral charge cat", "close gap in cat form", 49376, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && player->GetComboPoints() >= 5 && player->GetPowerPct(POWER_MANA) < 50.0f && !player->HasAura(89758) && IsSpellReady(player, 89758), 56.0f,
            { "druid thinnervate", "restore mana with combo points", 89758, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inCat && player->GetComboPoints() >= 5 && IsSpellReady(player, 9896), 55.0f,
            { "druid rip", "feral combo point bleed finisher", 9896, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && !HasAuraFromSpellChain(target, 33876) && IsSpellReady(player, 9850), 54.0f,
            { "druid claw", "build combo points when mangle is missing", 9850, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && !HasAuraFromSpellChain(target, 9904) && IsSpellReady(player, 9904), 53.0f,
            { "druid rake", "maintain rake bleed", 9904, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && IsSpellReady(player, 17392), 52.0f,
            { "druid faerie fire feral", "feral armor debuff filler", 17392, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && IsSpellReady(player, 9830), 51.0f,
            { "druid shred", "behind-target combo point builder", 9830, playerbot::PvpClassSpellContext::TargetMode::Enemy });

        return SelectHighestPriorityCastableDecision(feralCandidates, player, target, nullptr);
    }

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, recoveredFromPolymorph && IsSpellReady(player, 783), 55.0f,
        { "druid travel form recovery", "recovering from polymorph by travel-form reposition", 783, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isFeralDruid && lowManaAlly && !lowManaAlly->HasAura(29166), 50.0f,
        { "druid innervate", "stabilize low-mana ally with innervate", 29166, lowManaAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowManaAlly ? lowManaAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, cursedTarget, 49.0f,
        { "druid remove curse", "remove curses from allies", 2782, cursedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, poisonedTarget && !HasAuraFromSpellChain(poisonedTarget, 2893), 48.0f,
        { "druid abolish poison", "remove poison pressure from allies", 2893, poisonedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, poisonedTarget ? poisonedTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, swiftmendTarget && (HasAuraFromSpellChain(swiftmendTarget, 9858) || HasAuraFromSpellChain(swiftmendTarget, 25299)), 47.0f,
        { "druid swiftmend", "consume hot for emergency heal under 50 percent", 18562, swiftmendTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, swiftmendTarget ? swiftmendTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, emergencyLowTarget, 46.0f,
        { "druid natures swiftness", "prepare instant healing touch for critical ally", 17116, playerbot::PvpClassSpellContext::TargetMode::Self, emergencyLowTarget ? emergencyLowTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->HasAura(17116) && emergencyTarget, 45.0f,
        { "druid healing touch", "consume natures swiftness with healing touch", 25297, emergencyTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyTarget ? emergencyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, regrowthTarget && !HasAuraFromSpellChain(regrowthTarget, 9858), 44.0f,
        { "druid regrowth", "maintain regrowth on injured allies", 9858, regrowthTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, regrowthTarget ? regrowthTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, rejuvTarget && !HasAuraFromSpellChain(rejuvTarget, 25299), 43.0f,
        { "druid rejuvenation", "maintain rejuvenation on injured allies", 25299, rejuvTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, rejuvTarget ? rejuvTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, moonfireExecuteTarget, 42.0f,
        { "druid moonfire execute", "spam moonfire pressure on nearby low-health enemies", 8921, playerbot::PvpClassSpellContext::TargetMode::Enemy, moonfireExecuteTarget ? moonfireExecuteTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 9907) && IsSpellReady(player, 9907), 30.0f,
        { "druid faerie fire", "apply faerie fire to nearby rogues", 9907, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, meleeThreat && IsSpellReady(player, 5487), 29.0f,
        { "druid bear form", "swap to bear under physical melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->HasAura(5487) && meleeThreat && IsSpellReady(player, 16979), 28.0f,
        { "druid feral charge", "charge away from melee pressure in bear form", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectPaladinSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const isRetPaladin = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    Unit const* emergencyLowAlly = isRetPaladin ? nullptr : SelectFriendlyHealthTarget(player, 15.0f, 25.0f);
    Unit const* cleanseTarget = nullptr;
    if (IsSpellReady(player, 4987))
    {
        cleanseTarget = SelectFriendlyDispelTarget(player, DISPEL_POISON, 40.0f);
        if (!cleanseTarget)
            cleanseTarget = SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f);
    }
    Unit const* freedomTarget = IsSpellReady(player, 1044) ? SelectFriendlySnaredTarget(player, 40.0f) : nullptr;
    Unit const* sacrificeTarget = IsSpellReady(player, 6940) ? SelectFriendlyHealthTarget(player, 40.0f, 95.0f) : nullptr;
    Unit const* executeTarget = SelectNearbyEnemyTarget(player, target, 30.0f);
    Unit const* stunTarget = IsSpellReady(player, 10308) ? SelectEnemyCastingTarget(player, 10.0f, executeTarget) : nullptr;
    Unit const* repentanceTarget = (isRetPaladin && IsSpellReady(player, 20066)) ? SelectEnemyCastingTarget(player, 20.0f, executeTarget) : nullptr;
    Unit const* stunnedJudgementTarget = (isRetPaladin && HasAuraFromSpellChain(player, 20375)) ? SelectStunnedEnemyTarget(player, executeTarget, 30.0f) : nullptr;
    Unit const* protectionTarget = (isRetPaladin && IsSpellReady(player, 10278)) ? SelectFriendlyMeleePressureTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* flashHealTarget = (!isRetPaladin && IsSpellReady(player, 19943)) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
    Unit const* holyLightTarget = (!isRetPaladin && IsSpellReady(player, 635)) ? SelectFriendlyHealthTarget(player, 40.0f, 60.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, isRetPaladin && !HasAuraFromSpellChain(player, 20218) && IsSpellReady(player, 20218), 61.0f,
        { "paladin sanctity aura", "maintain sanctity aura for ret pressure", 20218, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRetPaladin && !HasAuraFromSpellChain(player, 20375) && IsSpellReady(player, 20375), 60.8f,
        { "paladin seal of command", "maintain seal of command", 20375, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HealthBelowPct(20) && IsSpellReady(player, 1020), 60.0f,
        { "paladin divine shield", "emergency immunity under lethal pressure", 1020, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, emergencyLowAlly && IsSpellReady(player, 635), 56.0f,
        { "paladin holy light", "prioritize emergency heal for nearby ally below 25 percent health", 635, emergencyLowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyLowAlly ? emergencyLowAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, emergencyLowAlly && IsSpellReady(player, 19943), 55.5f,
        { "paladin flash of light", "fallback emergency heal for nearby ally below 25 percent health", 19943, emergencyLowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyLowAlly ? emergencyLowAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !emergencyLowAlly && cleanseTarget, 55.0f,
        { "paladin cleanse", "prioritize cleansing allies", 4987, cleanseTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cleanseTarget ? cleanseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, freedomTarget, 54.0f,
        { "paladin hand of freedom", "free snared or rooted ally", 1044, freedomTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, freedomTarget ? freedomTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates,
        sacrificeTarget && sacrificeTarget != player &&
        !player->HasAura(6940) &&
        !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHandOfSacrificeCooldownToken) &&
        !HasAuraFromSpellChain(sacrificeTarget, 6940) &&
        !HasAuraFromSpellChain(sacrificeTarget, 1022) &&
        !HasAuraFromSpellChain(sacrificeTarget, 1044), 53.0f,
        { "paladin hand of sacrifice", "keep hand of sacrifice cycling on allies", 6940, playerbot::PvpClassSpellContext::TargetMode::Ally, sacrificeTarget ? sacrificeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 8.0f) >= 2 && IsSpellReady(player, 26573), 52.0f,
        { "paladin consecration", "aoe pressure under close melee collapse", 26573, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, protectionTarget, 53.5f,
        { "paladin hand of protection", "protect low-health ally under melee pressure", 10278, protectionTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, protectionTarget ? protectionTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, repentanceTarget, 52.5f,
        { "paladin repentance", "interrupt enemy spellcast", 20066, playerbot::PvpClassSpellContext::TargetMode::Enemy, repentanceTarget ? repentanceTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, executeTarget && executeTarget->HealthBelowPct(20) && IsSpellReady(player, 24239), 51.0f,
        { "paladin hammer of wrath", "execute low-health enemy", 24239, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, stunTarget, 50.0f,
        { "paladin hammer of justice", "stun nearby cast target", 10308, playerbot::PvpClassSpellContext::TargetMode::Enemy, stunTarget ? stunTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, IsSpellReady(player, 20216) && player->IsInCombat(), 49.0f,
        { "paladin divine favor", "increase emergency heal throughput", 20216, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, flashHealTarget, 48.0f,
        { "paladin flash of light", "heal injured allies efficiently", 19943, flashHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, flashHealTarget ? flashHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, holyLightTarget, 47.0f,
        { "paladin holy light", "large heal for heavily injured ally", 635, holyLightTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, holyLightTarget ? holyLightTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRetPaladin && stunnedJudgementTarget && IsSpellReady(player, 20271), 46.5f,
        { "paladin judgement", "judge nearby stunned enemy while seal of command is active", 20271, playerbot::PvpClassSpellContext::TargetMode::Enemy, stunnedJudgementTarget ? stunnedJudgementTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, executeTarget && HasActivePaladinSeal(player) && IsSpellReady(player, 20271), 46.0f,
        { "paladin judgement", "default offensive pressure when a seal is active", 20271, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isRetPaladin && !player->HasAura(19746) && IsSpellReady(player, 19746), 20.0f,
        { "paladin concentration aura", "maintain concentration aura", 19746, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsInCombat() && !player->HasAura(25898) && IsSpellReady(player, 25898), 19.0f,
        { "paladin greater blessing of kings", "maintain kings out of combat", 25898, playerbot::PvpClassSpellContext::TargetMode::Self });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectWarlockSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const isAfflictionWarlock = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    Pet const* pet = player->GetPet();
    bool const hasLivingPet = pet && pet->IsAlive();
    bool const needsPetSummon = !hasLivingPet;
    uint32 const summonPetSpell = isAfflictionWarlock ? 691 : 697;
    bool const hasHostileTarget = HasHostileTarget(player, target);

    if (!hasHostileTarget)
    {
        if (needsPetSummon && !player->IsInCombat() && IsSpellReady(player, summonPetSpell))
            return { isAfflictionWarlock ? "warlock summon felhunter" : "warlock summon voidwalker", isAfflictionWarlock ? "maintain felhunter pet while out of combat" : "maintain voidwalker pet while out of combat", summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self };

        return decision;
    }

    bool const closePressure = player->IsWithinDistInMap(target, 8.0f);
    Unit const* fearTarget = IsSpellReady(player, 6215) ? SelectWarlockFearTarget(player, 20.0f) : nullptr;
    Unit const* spellLockTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19647)) ? SelectEnemyCastingTarget(player, 30.0f, target) : nullptr;
    Unit const* devourEnemyTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19736)) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* devourFriendlyTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19736)) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 30.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, !isAfflictionWarlock && player->HealthBelowPct(25) && hasLivingPet && IsPetSpellReady(player, 19443), 70.0f,
        { "warlock sacrifice", "emergency voidwalker sacrifice at or below 25 percent health", 19443, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && target->HasUnitState(UNIT_STATE_CASTING) && IsPetSpellReady(player, 19244), 54.0f,
        { "warlock spell lock", "pet interrupt when available", 19244, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, spellLockTarget, 57.0f,
        { "warlock spell lock", "felhunter interrupt on any nearby caster", 19647, playerbot::PvpClassSpellContext::TargetMode::Enemy, spellLockTarget ? spellLockTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devourFriendlyTarget, 56.5f,
        { "warlock devour magic ally", "felhunter dispels friendly magic debuffs", 19736, playerbot::PvpClassSpellContext::TargetMode::Ally, devourFriendlyTarget ? devourFriendlyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devourEnemyTarget, 56.0f,
        { "warlock devour magic enemy", "felhunter dispels enemy magic buffs", 19736, playerbot::PvpClassSpellContext::TargetMode::Enemy, devourEnemyTarget ? devourEnemyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, fearTarget, 53.0f,
        { "warlock fear", "prioritize fear control on paladin/priest targets in range", 6215, playerbot::PvpClassSpellContext::TargetMode::Enemy, fearTarget ? fearTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && player->IsInCombat() && needsPetSummon && !player->HasAura(18708) && IsSpellReady(player, 18708), 52.0f,
        { "warlock fel domination", "prepare instant pet recovery before voidwalker summon", 18708, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, needsPetSummon && IsSpellReady(player, summonPetSpell), 51.0f,
        { isAfflictionWarlock ? "warlock summon felhunter" : "warlock summon voidwalker", isAfflictionWarlock ? "recover felhunter in combat when absent" : "recover voidwalker in combat when absent", summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && !player->HasAura(25228) && IsSpellReady(player, 19028), 45.0f,
        { "warlock soul link", "maintain soul link when pet is available", 19028, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, target->GetPowerType() == POWER_MANA && ShouldUseCurseOfTongues(target) && !HasAuraFromSpellChain(target, 11719) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11719) && IsSpellReady(player, 11719), 36.0f,
        { "warlock curse of tongues", "slow enemy casting throughput", 11719, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !IsCasterClass(target) && !HasAuraFromSpellChain(target, 11713) && !HasAuraFromSpellChain(target, 11719) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11713) && IsSpellReady(player, 11713), 35.0f,
        { "warlock curse of agony", "apply curse of agony pressure to non-caster players", 11713, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isAfflictionWarlock && player->HasItemCount(13603) && SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 0.0f) == player, 34.7f,
        { "warlock soulstone", "use soulstone to remove magic debuffs", 0, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), 13603 });
    AddDecisionCandidate(candidates, isAfflictionWarlock && !HasAuraFromSpellChain(target, 48181) && IsSpellReady(player, 48181), 34.5f,
        { "warlock haunt", "maintain haunt on kill target", 48181, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !HasAuraFromSpellChain(target, 11672) && IsSpellReady(player, 11672), 34.0f,
        { "warlock corruption", "maintain corruption dot", 11672, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, (target->HealthBelowPct(20) || (closePressure && IsMeleeClass(target))) && IsSpellReady(player, 17926), 33.0f,
        { "warlock death coil", "peel melee or finish low enemy target", 17926, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isAfflictionWarlock && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 17928), 32.5f,
        { "warlock howl of terror", "fear clustered nearby enemies", 17928, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->GetPower(POWER_MANA) < 400 && IsSpellReady(player, 11689), 32.0f,
        { "warlock life tap", "convert health to mana for sustained casting", 11689, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsLowOrOutOfManaForFallback(player) && HasWandEquipped(player) && IsSpellReady(player, 5019), 31.5f,
        { "warlock shoot wand", "fallback to wand pressure while low on mana", 5019, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->HasAura(17941) && IsSpellReady(player, 25307), 20.0f,
        { "warlock shadow bolt", "consume nightfall proc for instant pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && IsSpellReady(player, 25307), 19.0f,
        { "warlock shadow bolt", "default ranged pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isAfflictionWarlock && IsSpellReady(player, 11700), 18.0f,
        { "warlock drain life", "fallback affliction channel", 11700, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, HasHostileTarget(player, target) && target && !HasBreakableCrowdControl(target) && HasWandEquipped(player) && IsSpellReady(player, 5019), 8.0f,
        { "warlock shoot wand", "default offensive fallback when no better warlock action is available", 5019, playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectWarriorSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (!HasHostileTarget(player, target))
        return decision;

    Unit const* activeTarget = SelectWarriorPriorityTarget(player, target, 25.0f);
    if (!HasHostileTarget(player, activeTarget))
        activeTarget = target;

    bool const isProtWarrior = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const inBattleStance = player->HasAura(2457);
    bool const inDefensiveStance = player->HasAura(71);
    bool const inBerserkerStance = player->HasAura(2458);
    Unit const* gapCloseTarget = HasHostileTarget(player, target) ? target : activeTarget;
    SpellInfo const* chargeInfo = sSpellMgr->GetSpellInfo(11578);
    SpellInfo const* interceptInfo = sSpellMgr->GetSpellInfo(20617);
    float const chargeMinRange = chargeInfo ? chargeInfo->GetMinRange(false) : 8.0f;
    float const interceptMinRange = interceptInfo ? interceptInfo->GetMinRange(false) : 8.0f;
    bool const canChargeByRange = !player->IsWithinDistInMap(gapCloseTarget, chargeMinRange);
    bool const canInterceptByRange = !player->IsWithinDistInMap(gapCloseTarget, interceptMinRange);
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, activeTarget, 8.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 8.0f, activeTarget);
    bool const hasNearbyMeleeThreat = HasHostileTarget(player, nearbyMeleeTarget);
    bool const nearbyMeleeThreatSnared = hasNearbyMeleeThreat && nearbyMeleeTarget->HasAuraWithMechanic(1 << MECHANIC_SNARE);
    bool const canDisarmNearbyMeleeThreat = hasNearbyMeleeThreat &&
        nearbyMeleeThreatSnared &&
        player->IsWithinMeleeRange(nearbyMeleeTarget) &&
        nearbyMeleeTarget->CanUseAttackType(BASE_ATTACK) &&
        !HasAuraFromSpellChain(nearbyMeleeTarget, 676);

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HasAuraWithMechanic(1 << MECHANIC_FEAR) && !inBerserkerStance && IsSpellReady(player, 2458), 60.5f,
        { "warrior berserker stance", "swap to berserker stance to break fear", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HasAuraWithMechanic(1 << MECHANIC_FEAR) && inBerserkerStance && IsSpellReady(player, 18499), 60.0f,
        { "warrior berserker rage", "break fear-like control while in berserker stance", 18499, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isProtWarrior && player->HealthBelowPct(25) && IsSpellReady(player, 12975), 61.0f,
        { "warrior last stand", "emergency health cooldown below 25 percent", 12975, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isProtWarrior && HasHostileTarget(player, nearbyCastingTarget) && (inDefensiveStance || inBattleStance) && HasShieldEquipped(player) && IsSpellReady(player, 1672), 60.0f,
        { "warrior shield bash", "shield bash nearby spellcasts", 1672, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isProtWarrior && HasHostileTarget(player, nearbyCastingTarget) && IsSpellReady(player, 6552), 59.0f,
        { "warrior pummel", "interrupt nearby spellcasts", 6552, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, canDisarmNearbyMeleeThreat && inDefensiveStance && IsSpellReady(player, 81492), 58.0f,
        { "warrior disarm", "disarm threatening melee weapon users", 81492, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyMeleeTarget ? nearbyMeleeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, canDisarmNearbyMeleeThreat && !inDefensiveStance && IsSpellReady(player, 81492) && IsSpellReady(player, 71) && player->GetPower(POWER_RAGE) >= 200, 57.0f,
        { "warrior defensive stance", "swap defensive before disarm against melee", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isProtWarrior && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 355), 56.8f,
        { "warrior taunt", "taunt when surrounded by enemies", 355, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, CountNearbyUnsNaredEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 12323), 56.0f,
        { "warrior piercing howl", "apply area snare when multiple enemies are unsnared in melee range", 12323, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, HasHostileTarget(player, activeTarget) && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 5246), 55.5f,
        { "warrior intimidating shout", "aoe fear around the current target when outnumbered", 5246, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, (IsSpellReady(player, 6552) || IsSpellReady(player, 81492) || IsSpellReady(player, 20617) || IsSpellReady(player, 1680) || IsSpellReady(player, isProtWarrior ? uint32(23925) : uint32(21553))) &&
            player->GetPower(POWER_RAGE) < 150 && IsSpellReady(player, 2687), 54.0f,
        { "warrior bloodrage", "generate rage to unlock rotational abilities", 2687, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inDefensiveStance && (!IsSpellReady(player, 81492) || !hasNearbyMeleeThreat) && IsSpellReady(player, 2458), 53.0f,
        { "warrior berserker stance", "leave defensive stance when disarm is unavailable or no melee threat is nearby", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(gapCloseTarget) && !player->IsInCombat() && !inBattleStance && IsSpellReady(player, 2457), 52.5f,
        { "warrior battle stance", "switch to battle stance before out-of-combat charge", 2457, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(gapCloseTarget) && !player->IsInCombat() && canChargeByRange && IsSpellReady(player, 11578), 52.0f,
        { "warrior charge", "close gap to target from out of combat", 11578, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(gapCloseTarget) && player->IsInCombat() && !inBerserkerStance && IsSpellReady(player, 2458), 51.5f,
        { "warrior berserker stance", "switch to berserker stance before intercept gap close", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(gapCloseTarget) && player->IsInCombat() && canInterceptByRange && IsSpellReady(player, 20617), 51.0f,
        { "warrior intercept", "close gap to target while in combat", 20617, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && !inBerserkerStance &&
            IsSpellReady(player, 1680) && IsSpellReady(player, 2458), 50.4f,
        { "warrior berserker stance", "switch to berserker stance to enable whirlwind in melee", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, activeTarget->HealthBelowPct(20) && IsSpellReady(player, 20662), 50.0f,
        { "warrior execute", "finisher at low enemy health", 20662, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 25289) && IsSpellReady(player, 25289), 40.0f,
        { "warrior battle shout", "maintain attack power buff", 25289, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isProtWarrior && player->IsWithinMeleeRange(activeTarget) && IsSpellReady(player, 25228), 39.8f,
        { "warrior revenge", "use reactive revenge whenever available", 25228, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    Unit const* concussionTarget = isProtWarrior && IsSpellReady(player, 12809) ? SelectUnstunDREnemyTarget(player, activeTarget, 5.0f, 12809) : nullptr;
    AddDecisionCandidate(candidates, concussionTarget, 39.6f,
        { "warrior concussion blow", "stun a target without stun diminishing returns", 12809, playerbot::PvpClassSpellContext::TargetMode::Enemy, concussionTarget ? concussionTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) &&
            (isProtWarrior ? !HasAuraFromSpellChain(activeTarget, 11597) : (!HasAuraFromSpellChain(activeTarget, 7373) || (activeTarget->GetAura(7373) && activeTarget->GetAura(7373)->GetDuration() < 2000))) &&
            IsSpellReady(player, isProtWarrior ? uint32(11597) : uint32(7373)), 39.0f,
        { isProtWarrior ? "warrior sunder armor" : "warrior hamstring", isProtWarrior ? "apply sunder armor as protection filler" : "maintain stickiness snare", isProtWarrior ? uint32(11597) : uint32(7373), playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && (isProtWarrior || !HasAuraFromSpellChain(activeTarget, 21553)) &&
            IsSpellReady(player, isProtWarrior ? uint32(23925) : uint32(21553)), 38.0f,
        { isProtWarrior ? "warrior shield slam" : "warrior mortal strike", isProtWarrior ? "protection kill target pressure" : "arms-like burst pressure", isProtWarrior ? uint32(23925) : uint32(21553), playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && activeTarget->GetClass() == CLASS_ROGUE &&
            !HasAuraFromSpellChain(activeTarget, 11574) && IsSpellReady(player, 11574), 37.0f,
        { "warrior rend", "apply anti-stealth bleed pressure on rogues", 11574, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && player->GetPower(POWER_RAGE) >= 500 && IsSpellReady(player, 1680), 36.0f,
        { "warrior whirlwind", "fallback aoe melee pressure", 1680, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });

    return SelectHighestPriorityCastableDecision(candidates, player, activeTarget, nullptr);
}

SpellDecision SelectRogueSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (!HasHostileTarget(player, target))
        return decision;

    bool const isCombatRogue = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    Unit const* blindTarget = IsSpellReady(player, 2094) ? SelectRogueBlindTarget(player, target, 15.0f) : nullptr;
    Unit const* nearbyCastingTarget = IsSpellReady(player, 1766) ? SelectEnemyCastingTarget(player, 5.0f, target) : nullptr;
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, target, 5.0f);
    bool const rootedOrSnared = IsRootedOrSnared(player);

    std::vector<PrioritizedSpellDecision> candidates;
    // Disabled: weapon-poison automation from PvP decision loop.
    // This avoids touching weapon-enchant mutation paths while investigating combat-time crashes.

    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 1784) && IsSpellReady(player, 1784), 50.0f,
        { "rogue stealth", "enter stealth before engagement", 1784, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HasStealthAura() && player->IsWithinMeleeRange(target) && IsSpellReady(player, 1833), 49.0f,
        { "rogue cheap shot", "default opener", 1833, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, ((isCombatRogue && nearbyCastingTarget) || (!isCombatRogue && target->HasUnitState(UNIT_STATE_CASTING))) && IsSpellReady(player, 1766), 48.0f,
        { "rogue kick", isCombatRogue ? "interrupt nearby enemy cast" : "interrupt enemy cast", 1766, playerbot::PvpClassSpellContext::TargetMode::Enemy, isCombatRogue && nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->HealthBelowPct(40) && IsSpellReady(player, 5277), 47.0f,
        { "rogue evasion", "defensive survival in melee", 5277, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, (isCombatRogue ? rootedOrSnared : (!player->HealthBelowPct(50) && !player->IsWithinMeleeRange(target) && player->IsWithinDistInMap(target, 30.0f))) && IsSpellReady(player, 11305), 46.0f,
        { "rogue sprint", "close gap for melee pressure", 11305, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && rootedOrSnared && !IsSpellReady(player, 11305) && IsSpellReady(player, 26889), 45.8f,
        { "rogue vanish", "escape root or snare when sprint is unavailable", 26889, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && player->IsWithinMeleeRange(target) && IsSpellReady(player, 13750), 45.7f,
        { "rogue adrenaline rush", "combat burst when in melee", 13750, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && player->IsWithinMeleeRange(target) && IsSpellReady(player, 13877), 45.6f,
        { "rogue blade flurry", "cleave burst when in melee", 13877, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && nearbyMeleeTarget && IsSpellReady(player, 51722), 45.5f,
        { "rogue dismantle", "disarm nearby melee threat", 51722, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyMeleeTarget ? nearbyMeleeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, blindTarget, 45.0f,
        { "rogue blind", "prioritize druid/shaman/paladin secondary targets without abolish poison", 2094, playerbot::PvpClassSpellContext::TargetMode::Enemy, blindTarget ? blindTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, (isCombatRogue ? player->GetComboPoints() >= 5 : player->GetComboPoints() >= 5) && IsSpellReady(player, 8643), 44.0f,
        { "rogue kidney shot", "primary stun finisher at full combo points", 8643, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isCombatRogue && player->GetComboPoints() >= 2 && !HasAuraFromSpellChain(player, 6774) && IsSpellReady(player, 6774), 43.5f,
        { "rogue slice and dice", "maintain slice and dice at two combo points", 6774, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 81308), 43.2f,
        { "rogue deadly shot", "ranged fallback when kill target is out of melee", 81308, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->GetComboPoints() >= 5 && IsSpellReady(player, 11300), 43.0f,
        { "rogue eviscerate", "combo finisher pressure", 11300, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(target) && player->IsWithinDistInMap(target, 25.0f) && IsSpellReady(player, 36554), 42.0f,
        { "rogue shadowstep", "bridge short gap before melee globals", 36554, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, IsSpellReady(player, isCombatRogue ? uint32(11294) : uint32(16511)), 20.0f,
        { isCombatRogue ? "rogue sinister strike" : "rogue hemorrhage", isCombatRogue ? "default combat combo point builder" : "default subtlety combo point builder", isCombatRogue ? uint32(11294) : uint32(16511), playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectShamanSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const isRestoShaman = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    if (!hasHostileTarget && !allyTarget && !isRestoShaman)
        return decision;

    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    Unit const* chainHealTarget = isRestoShaman && IsSpellReady(player, 10623) ? SelectFriendlyHealthTarget(player, 40.0f, 95.0f) : nullptr;
    Unit const* lesserHealTarget = isRestoShaman && IsSpellReady(player, 10468) ? SelectFriendlyHealthTarget(player, 40.0f, 90.0f) : nullptr;
    Unit const* nsHealTarget = isRestoShaman && IsSpellReady(player, 16188) && IsSpellReady(player, 25357) ? SelectFriendlyHealthTarget(player, 40.0f, 35.0f) : nullptr;
    Unit const* earthShieldTarget = isRestoShaman && IsSpellReady(player, 32593) && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, 32593) ? SelectFriendlyHealthTarget(player, 40.0f, 100.0f) : nullptr;
    Unit const* purgeTarget = isRestoShaman && hasHostileTarget && IsSpellReady(player, 81325) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* allyMagicTarget = isRestoShaman && IsSpellReady(player, 81325) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f) : nullptr;
    Unit const* distantEscapeTarget = isRestoShaman && hasHostileTarget && SelectNearbyMeleeTarget(player, target, 8.0f) ? SelectNearbyEnemyTarget(player, target, 25.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    // Disabled: auto-casting Windfury Weapon from PvP loop while investigating weapon-dependent aura crashes.
    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 10432) && IsSpellReady(player, 10432), 34.0f,
        { "shaman lightning shield", "maintain shield buff out of combat", 10432, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 10414), 60.0f,
        { "shaman earth shock", "interrupt enemy cast with shock", 10414, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isRestoShaman && hasHostileTarget && target->GetPowerType() == POWER_MANA && IsSpellReady(player, 8177), 59.0f,
        { "shaman grounding totem", "counter incoming caster pressure", 8177, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isRestoShaman && IsSpellReady(player, 16166), 58.0f,
        { "shaman elemental mastery", "trigger burst throughput cooldown", 16166, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && nsHealTarget && !player->HasAura(16188), 59.5f,
        { "shaman nature's swiftness", "prepare instant emergency healing wave", 16188, playerbot::PvpClassSpellContext::TargetMode::Self, nsHealTarget ? nsHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && player->HasAura(16188) && nsHealTarget, 59.4f,
        { "shaman healing wave", "consume nature's swiftness on emergency target", 25357, nsHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, nsHealTarget ? nsHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && chainHealTarget, 58.5f,
        { "shaman chain heal", "primary restoration heal", 10623, chainHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, chainHealTarget ? chainHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && earthShieldTarget && !HasAuraFromSpellChain(earthShieldTarget, 32593), 58.0f,
        { "shaman earth shield", "protect lowest health ally", 32593, earthShieldTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, earthShieldTarget ? earthShieldTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isRestoShaman && IsSpellReady(player, 10605), 57.0f,
        { "shaman chain lightning", "primary burst cast on kill target", 10605, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, hasHostileTarget && IsMeleeClass(target) && player->IsWithinDistInMap(target, 10.0f) && IsSpellReady(player, 2484), 56.0f,
        { "shaman earthbind totem", "kite nearby melee pressure", 2484, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && IsMeleeClass(target) && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 10473), 55.0f,
        { "shaman frost shock", "snare medium-range melee threats", 10473, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    Unit const* poisonedAllyInTotemRange = IsSpellReady(player, 8170) ? SelectFriendlyDispelTarget(player, DISPEL_POISON, 20.0f) : nullptr;
    AddDecisionCandidate(candidates, poisonedAllyInTotemRange && !HasAuraFromSpellChain(player, 8170), 54.0f,
        { "shaman poison cleansing totem", "answer rogue poison pressure", 8170, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && (target->GetClass() == CLASS_PRIEST || target->GetClass() == CLASS_WARLOCK) && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 8143), 53.0f,
        { "shaman tremor totem", "mitigate fear pressure from priest/warlock", 8143, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && player->GetPowerPct(POWER_MANA) < 50.0f && IsSpellReady(player, 16190), 52.8f,
        { "shaman mana tide totem", "restore mana below half", 16190, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && !HasAuraFromSpellChain(player, 81476) && IsSpellReady(player, 81476), 52.7f,
        { "shaman tremor totem", "maintain tremor totem", 81476, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && !HasAuraFromSpellChain(player, 81477) && IsSpellReady(player, 81477), 52.6f,
        { "shaman poison cleansing totem", "maintain poison cleansing totem", 81477, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && !HasAuraFromSpellChain(player, 81478) && IsSpellReady(player, 81478), 52.5f,
        { "shaman grounding totem", "maintain grounding totem", 81478, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && SelectNearbyMeleeTarget(player, target, 8.0f) && player->HealthBelowPct(50) && IsSpellReady(player, 2645), 52.4f,
        { "shaman ghost wolf", "escape melee pressure while endangered", 2645, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && distantEscapeTarget && player->HasAura(2645) && IsSpellReady(player, 82419), 52.3f,
        { "shaman rehgar's fury", "leap to distant target while escaping", 82419, playerbot::PvpClassSpellContext::TargetMode::Enemy, distantEscapeTarget ? distantEscapeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->HealthBelowPct(50) && IsSpellReady(player, 10468), 52.0f,
        { "shaman lesser healing wave", "self-sustain while focused", 10468, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && purgeTarget, 53.5f,
        { "shaman purge", "purge enemy magic buffs", 81325, playerbot::PvpClassSpellContext::TargetMode::Enemy, purgeTarget ? purgeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && allyMagicTarget, 53.2f,
        { "shaman purge ally", "purge sheep or fear magic effects from allies", 81325, allyMagicTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, allyMagicTarget ? allyMagicTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !dispelThrottleActive && hasHostileTarget && IsSpellReady(player, 370), 40.0f,
        { "shaman purge", "strip enemy magical effects by default", 370, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isRestoShaman && lesserHealTarget, 51.0f,
        { "shaman lesser healing wave", "restoration fallback heal", 10468, lesserHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lesserHealTarget ? lesserHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, hasHostileTarget && IsSpellReady(player, 15208), isRestoShaman ? 5.0f : 39.0f,
        { "shaman lightning bolt", "fallback ranged damage cast", 15208, playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityCastableDecision(candidates, player, target, allyTarget);
}

SpellDecision SelectClassicClassSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
    {
        decision.reason = "missing-player";
        return decision;
    }

    if (profileSelection.unsupportedClass)
    {
        decision.reason = "unsupported-class";
        return decision;
    }

    bool const inMelee = target && player->IsWithinMeleeRange(target);
    switch (player->GetClass())
    {
    case CLASS_HUNTER:
        return SelectHunterSpell(player, target, inMelee, profileSelection);
    case CLASS_MAGE:
    {
        SpellDecision mageDecision = SelectMageSpell(player, target, inMelee, profileSelection);
        if (!mageDecision.spellId && HasHostileTarget(player, target) && IsLowOrOutOfManaForFallback(player) && HasWandEquipped(player) && IsSpellReady(player, 5019))
            return { "mage shoot wand", "fallback to wand pressure while low on mana", 5019, playerbot::PvpClassSpellContext::TargetMode::Enemy };
        return mageDecision;
    }
    case CLASS_PRIEST:
        return SelectPriestSpell(player, target, allyTarget, profileSelection);
    case CLASS_PALADIN:
        return SelectPaladinSpell(player, target, profileSelection);
    case CLASS_WARLOCK:
        return SelectWarlockSpell(player, target, profileSelection);
    case CLASS_DRUID:
        return SelectDruidSpell(player, target, profileSelection);
    case CLASS_WARRIOR:
        return SelectWarriorSpell(player, target, profileSelection);
    case CLASS_ROGUE:
        return SelectRogueSpell(player, target, profileSelection);
    case CLASS_SHAMAN:
        return SelectShamanSpell(player, target, allyTarget, profileSelection);
    default:
        decision.reason = "class-not-in-this-pass";
        return decision;
    }
}

SpellDecision SelectClassOrUtilitySpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    if (SpellDecision const utilityDecision = MaybeSelectUtilitySpell(player, target); utilityDecision.spellId)
        return utilityDecision;

    if (!HasHostileTarget(player, target) && !allyTarget)
        return {};

    return SelectClassicClassSpell(player, target, allyTarget, profileSelection);
}

char const* GetClassLabel(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR: return "Warrior";
        case CLASS_PALADIN: return "Paladin";
        case CLASS_HUNTER: return "Hunter";
        case CLASS_ROGUE: return "Rogue";
        case CLASS_PRIEST: return "Priest";
        case CLASS_SHAMAN: return "Shaman";
        case CLASS_MAGE: return "Mage";
        case CLASS_WARLOCK: return "Warlock";
        case CLASS_DRUID: return "Druid";
        case CLASS_DEATH_KNIGHT: return "DeathKnight";
        default: return "UnknownClass";
    }
}

bool IsPrimaryRangedClassForSpacing(uint8 classId)
{
    switch (classId)
    {
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
        case CLASS_SHAMAN:
            return true;
        default:
            return false;
    }
}

bool IsDruidMeleeForm(Player const* player)
{
    if (!player || player->GetClass() != CLASS_DRUID)
        return false;

    switch (player->GetShapeshiftForm())
    {
        case FORM_CAT:
        case FORM_BEAR:
        case FORM_DIREBEAR:
            return true;
        default:
            return false;
    }
}

bool UsesRangedSpacingProfile(Player const* player, ClassicProfileSelection const& profileSelection)
{
    if (!player)
        return false;

    switch (player->GetClass())
    {
        case CLASS_DRUID:
            if (IsDruidMeleeForm(player))
                return false;
            return profileSelection.profile != ClassicClassProfile::TertiaryClassic;
        case CLASS_PALADIN:
            return profileSelection.profile == ClassicClassProfile::PrimaryClassic;
        case CLASS_SHAMAN:
            return profileSelection.profile == ClassicClassProfile::PrimaryClassic ||
                profileSelection.profile == ClassicClassProfile::TertiaryClassic;
        default:
            break;
    }

    return IsPrimaryRangedClassForSpacing(player->GetClass());
}

bool CanUseHealRangeSpacing(uint8 classId)
{
    switch (classId)
    {
        case CLASS_PRIEST:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_SHAMAN:
            return true;
        default:
            return false;
    }
}

float ComputeApproachFollowRange(float nominalRange)
{
    // Keep an extra movement buffer so ranged bots do not settle in a
    // dead-zone where spell-selection still reports out-of-range but chase
    // motion does not re-engage because the desired distance is too close to
    // edge tolerances.
    return std::max(1.0f, nominalRange - 3.0f);
}

bool IsPrimaryMeleeClassForSpacing(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_DEATH_KNIGHT:
            return true;
        default:
            return false;
    }
}

bool UsesMeleeSpacingProfile(Player const* player, ClassicProfileSelection const& profileSelection)
{
    if (!player)
        return false;

    switch (player->GetClass())
    {
        case CLASS_DRUID:
            if (IsDruidMeleeForm(player))
                return true;
            return profileSelection.profile == ClassicClassProfile::TertiaryClassic;
        case CLASS_PALADIN:
            return profileSelection.profile != ClassicClassProfile::PrimaryClassic;
        case CLASS_SHAMAN:
            return profileSelection.profile == ClassicClassProfile::SecondaryClassic;
        default:
            break;
    }

    return IsPrimaryMeleeClassForSpacing(player->GetClass());
}

void ConsiderMovementDirective(playerbot::PvpClassSpellContext& context, playerbot::PvpClassSpellContext::MovementDirective directive,
    ObjectGuid targetGuid, float followRange, char const* actionName, char const* reason, float priority)
{
    if (priority < context.movementPriority)
        return;

    context.movementDirective = directive;
    context.movementTargetGuid = targetGuid;
    context.movementFollowRange = followRange;
    context.actionName = actionName;
    context.reason = reason;
    context.shouldExecute = true;
    context.movementPriority = priority;
}

TacticalDecision SelectBattlegroundTacticalDecision(Player const* player, playerbot::PvpValues const& values)
{
    TacticalDecision decision;
    if (!player)
        return decision;

    bool const lowHealth = player->HealthBelowPct(35);
    bool const lowMana = player->GetPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 25.0f;
    bool const bgActive = values.battlegroundState == playerbot::BattlegroundState::Active;
    bool const bgWaiting = values.battlegroundState == playerbot::BattlegroundState::WaitingToStart;

    struct TacticalRule
    {
        char const* triggerName;
        bool condition;
        char const* actionName;
        float priority;
    };

    // Keep pressure behavior generic, but elevate flag-carrier directives when
    // objective triggers report an active carrier. This ensures CTF matches
    // chase carriers instead of defaulting to midfield skirmishes.
    bool const enemyFlagCarrierActive = values.enemyFlagCarrierActive;
    bool const teamFlagCarrierNear = values.teamFlagCarrierNear;

    std::array<TacticalRule, 6> const rules =
    {{
        { "bg waiting", bgWaiting, "bg move to start", 50.0f },
        { "enemy flag carrier active", bgActive && enemyFlagCarrierActive, "attack enemy flag carrier", 95.0f },
        { "team flag carrier near", bgActive && teamFlagCarrierNear, "bg protect fc", 80.0f },
        { "bg active", bgActive, "bg pursue enemy", 60.0f },
        { "low health", lowHealth, "bg use buff", 45.0f },
        { "low mana", lowMana, "bg use buff", 45.0f }
    }};

    for (TacticalRule const& rule : rules)
    {
        if (rule.condition)
        {
            decision.triggerName = rule.triggerName;
            decision.actionName = rule.actionName;
            decision.priority = rule.priority;
            return decision;
        }
    }

    return decision;
}
}

namespace playerbot
{
uint32 PvpCore::CountHumanPlayersOnBattlegroundTeam(Player const* player)
{
    if (!player || !player->InBattleground() || !player->FindMap())
        return 0;

    Battleground const* battleground = player->GetBattleground();
    if (!battleground)
        return 0;

    uint32 const botBgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    uint32 humanCount = 0;

    Map::PlayerList const& players = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player const* teammate = itr->GetSource();
        if (!teammate || teammate->GetBattlegroundId() != player->GetBattlegroundId())
            continue;

        uint32 const teammateBgTeam = teammate->GetBGTeam() ? teammate->GetBGTeam() : teammate->GetTeam();
        if (teammateBgTeam != botBgTeam)
            continue;

        if (!IsManagedRandomBot(teammate))
            ++humanCount;
    }

    return humanCount;
}

bool PvpCore::TeamHasHumanPlayers(Player const* player)
{
    return CountHumanPlayersOnBattlegroundTeam(player) > 0;
}

void PvpCore::LoadConfig()
{
    g_PvpCoreConfig.moduleEnabled = sConfigMgr->GetBoolDefault("Playerbot.Enable", false);
    g_PvpCoreConfig.pvpCoreEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpCore.Enable", false);
    g_PvpCoreConfig.pvpTacticsEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpTactics.Enable", false);
    g_PvpCoreConfig.pvpLifecycleEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpLifecycle.Enable", false);
    g_PvpCoreConfig.pvpClassSpellsEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpClassSpells.Enable", false);
    g_PvpCoreConfig.spellRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Spell", 30.0f);
    g_PvpCoreConfig.healRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Heal", 40.0f);
    g_PvpCoreConfig.meleeRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Melee", 8.0f);
    g_PvpCoreConfig.closeRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Close", 15.0f);
    g_PvpCoreConfig.longRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Long", 35.0f);
}

PvpCoreConfig const& PvpCore::GetConfig()
{
    return g_PvpCoreConfig;
}

PvpValues PvpCore::CollectValues(Player const* player)
{
    PvpValues values;
    if (!player)
        return values;

    values.inBattleground = player->InBattleground();
    values.inBattlegroundQueue = IsInBattlegroundQueue(player);
    values.battlegroundState = DetectBattlegroundState(player, values.inBattlegroundQueue);

    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
        bool const isInvited = player->IsInvitedForBattlegroundQueueType(bgQueueTypeId);

        values.hasArenaQueue = values.hasArenaQueue || isArenaQueue;
        values.hasBattlegroundQueue = values.hasBattlegroundQueue || !isArenaQueue;
        values.hasArenaInvite = values.hasArenaInvite || (isArenaQueue && isInvited);
        values.hasBattlegroundInvite = values.hasBattlegroundInvite || (!isArenaQueue && isInvited);
    }

    values.hasArenaTeamInvite = player->GetArenaTeamIdInvited() != 0;

    if (values.inBattleground)
        values.battlegroundTypeId = player->GetBattlegroundTypeId();

    PopulateObjectiveStateTriggers(player, values);
    values.battlegroundTeamHumanCount = CountHumanPlayersOnBattlegroundTeam(player);
    values.battlegroundTeamHasHumans = values.battlegroundTeamHumanCount > 0;

    return values;
}

bool PvpCore::IsTriggerActive(PvpTrigger trigger, PvpValues const& values)
{
    switch (trigger)
    {
        case PvpTrigger::InBattleground:
            return values.inBattleground;
        case PvpTrigger::BgQueueing:
            return values.battlegroundState == BattlegroundState::Queueing;
        case PvpTrigger::BgWaiting:
            return values.battlegroundState == BattlegroundState::WaitingToStart;
        case PvpTrigger::BgActive:
            return values.battlegroundState == BattlegroundState::Active;
        case PvpTrigger::BgInviteActive:
            return values.hasBattlegroundInvite;
        case PvpTrigger::InBattlegroundWithoutFlag:
            return values.inBattleground;
        case PvpTrigger::PlayerHasFlag:
            return values.playerHasFlag;
        case PvpTrigger::EnemyFlagCarrierActive:
            return values.enemyFlagCarrierActive;
        case PvpTrigger::EnemyFlagCarrierNear:
            return values.enemyFlagCarrierNear;
        case PvpTrigger::TeamFlagCarrierNear:
            return values.teamFlagCarrierNear;
        default:
            break;
    }

    return false;
}

BattlegroundTacticalContext PvpCore::BuildBattlegroundTacticalContext(Player const* player, PvpValues const& values)
{
    BattlegroundTacticalContext context;
    context.tacticsEnabled = g_PvpCoreConfig.moduleEnabled && g_PvpCoreConfig.pvpCoreEnabled && g_PvpCoreConfig.pvpTacticsEnabled;
    if (!context.tacticsEnabled || !player)
        return context;

    if (!IsTriggerActive(PvpTrigger::BgWaiting, values) && !IsTriggerActive(PvpTrigger::BgActive, values))
        return context;

    context.shouldEvaluate = true;
    TacticalDecision const decision = SelectBattlegroundTacticalDecision(player, values);
    context.triggerName = decision.triggerName;
    context.actionName = decision.actionName;
    context.actionPriority = decision.priority;
    context.objective = SelectObjectiveSkeleton(values);
    context.movement = SelectMovementPrimitiveSkeleton(values, context.objective);
    context.flagCarrierDirective = SelectFlagCarrierDirectiveSkeleton(values);
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP human-first context: guid={} human_count={} has_humans={} player_has_flag={} blocked_player_fc={} directive={} action={}.",
        player->GetGUID().ToString(), values.battlegroundTeamHumanCount, values.battlegroundTeamHasHumans, values.playerHasFlag,
        values.battlegroundTeamHasHumans && values.playerHasFlag, static_cast<uint8>(context.flagCarrierDirective),
        context.actionName ? context.actionName : "none");
    return context;
}

BattlegroundLifecycleContext PvpCore::BuildBattlegroundLifecycleContext(Player const* player, PvpValues const& values)
{
    BattlegroundLifecycleContext context;
    context.lifecycleEnabled = IsLifecycleEnabled();
    if (!context.lifecycleEnabled || !player)
        return context;

    context.queueOperation = SelectBattlegroundQueueOperationSkeleton(values);
    context.invitationResponse = SelectBattlegroundInvitationResponseSkeleton(values);
    context.shouldHandleInProgressStatus = ShouldHandleBattlegroundInProgressStatusSkeleton(values);
    return context;
}

ArenaLifecycleContext PvpCore::BuildArenaLifecycleContext(Player const* player, PvpValues const& values)
{
    ArenaLifecycleContext context;
    context.lifecycleEnabled = IsLifecycleEnabled();
    if (!context.lifecycleEnabled || !player)
        return context;

    context.queueOperation = SelectArenaQueueOperationSkeleton(values);
    context.teamInteraction = SelectArenaTeamInteractionSkeleton(values);
    return context;
}

PvpClassSpellContext PvpCore::BuildClassSpellContext(Player const* player, PvpValues const& values)
{
    PvpClassSpellContext context;
    context.classSpellsEnabled = IsClassSpellGateEnabled(g_PvpCoreConfig);
    if (!context.classSpellsEnabled || !player)
        return context;

    bool const inActiveBattleground = values.inBattleground && IsTriggerActive(PvpTrigger::BgActive, values);
    bool const inBattlegroundPreparation = player->InBattleground() &&
        (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION));
    bool const inActiveDuel = player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;
    if (!inActiveBattleground && !inBattlegroundPreparation && !inActiveDuel)
        return context;

    if (inBattlegroundPreparation)
    {
        SpellDecision const prepDecision = SelectPreparationBuffSpell(player);
        if (prepDecision.spellId)
        {
            context.actionName = prepDecision.actionName;
            context.reason = prepDecision.reason;
            context.spellId = prepDecision.spellId;
            context.targetMode = prepDecision.targetMode;
            context.targetGuid = prepDecision.targetGuid;
            context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
            context.shouldExecute = true;
            return context;
        }
    }

    ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
    ObjectGuid const selectedTargetGuid = SelectCombatTargetGuid(player);
    ObjectGuid activeTargetGuid = selectedTargetGuid;
    if (activeTargetGuid.IsEmpty())
        if (Unit const* combatVictim = player->GetVictim(); HasHostileTarget(player, combatVictim))
            activeTargetGuid = combatVictim->GetGUID();
    if (activeTargetGuid.IsEmpty())
    {
        bool const allowLongAcquire =
            UsesRangedSpacingProfile(player, profileSelection) ||
            CanUseHealRangeSpacing(player->GetClass());
        if (Unit const* fallbackTarget = SelectClosestEnemyTarget(player, !allowLongAcquire))
            activeTargetGuid = fallbackTarget->GetGUID();
    }

    auto resolveTargetByGuid = [&](ObjectGuid const& guid) -> Unit const*
    {
        if (guid.IsEmpty() || guid == player->GetGUID())
            return nullptr;

        Unit const* resolved = ObjectAccessor::GetUnit(*player, guid);
        if (!resolved || !resolved->IsAlive())
            return nullptr;

        return resolved;
    };
    bool const hasValidTarget = resolveTargetByGuid(activeTargetGuid) != nullptr;
    Unit const* selectedTargetByGuid = resolveTargetByGuid(activeTargetGuid);
    bool const hasInvalidSelectedTarget = !selectedTargetGuid.IsEmpty() &&
        (!selectedTargetByGuid || !HasHostileTarget(player, selectedTargetByGuid));
    ObjectGuid const selectedAllyGuid = SelectAllyTargetGuid(player);
    bool const hasValidAllyTarget = resolveTargetByGuid(selectedAllyGuid) != nullptr;

    // Reference parity guard: never allow mounted state indoors. In addition,
    // while in combat always force mount-state correction immediately. When a
    // mounted bot is simply traveling, do not let class spell selection break
    // the mount unless an enemy has actually entered the combat envelope.
    bool const outdoors = IsEffectivelyOutdoors(player);
    bool const sustainedIndoorMounted = player->IsMounted() && ShouldForceIndoorDismount(player, outdoors);
    if (player->IsMounted())
    {
        if (sustainedIndoorMounted || player->IsInCombat())
        {
            context.movementDirective = PvpClassSpellContext::MovementDirective::CheckMountState;
            context.actionName = "check mount state";
            context.reason = player->IsInCombat() ? "mounted in combat" : "mounted indoors";
            context.shouldExecute = true;
            return context;
        }

        if (hasInvalidSelectedTarget)
        {
            context.movementDirective = PvpClassSpellContext::MovementDirective::DropInvalidTarget;
            context.actionName = "drop target";
            context.reason = "invalid target";
            context.shouldExecute = true;
            context.movementTargetGuid = selectedTargetGuid;
            return context;
        }

        if (!HasNearbyAttackableEnemyPlayer(player, GetConfiguredCombatRange()))
            return context;
    }

    bool const criticalLowMana = player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 10.0f;

    // Hard-priority mana preservation policy: below 10% mana, disengage from
    // combat above all other class behavior, then drink as soon as combat drops.
    if (criticalLowMana)
    {
        if (player->IsInCombat())
        {
            Unit const* disengageTarget = resolveTargetByGuid(activeTargetGuid);
            if (!disengageTarget)
                disengageTarget = resolveTargetByGuid(selectedTargetGuid);
            if (!disengageTarget && player->GetVictim() && player->GetVictim()->IsAlive())
                disengageTarget = player->GetVictim();

            if (disengageTarget)
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, disengageTarget->GetGUID(),
                    std::max(GetConfiguredLongRange(), GetConfiguredCloseRange() + 8.0f),
                    "flee", "critical low mana disengage", 99.0f);
                return context;
            }

            context.movementDirective = PvpClassSpellContext::MovementDirective::ResetCombatState;
            context.actionName = "reset";
            context.reason = "critical low mana force combat reset";
            context.shouldExecute = true;
            return context;
        }

        if (IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
        {
            context.actionName = "drink";
            context.reason = "critical low mana recover";
            context.spellId = SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK;
            context.targetMode = PvpClassSpellContext::TargetMode::Self;
            context.targetGuid = player->GetGUID();
            context.selfCast = true;
            context.shouldExecute = true;
            return context;
        }
    }

    if (player->IsInCombat() && !hasValidTarget && !hasValidAllyTarget)
    {
        if (IncrementCombatNoTargetTicks(player) >= 3)
        {
            context.movementDirective = PvpClassSpellContext::MovementDirective::ResetCombatState;
            context.actionName = "reset";
            context.reason = "combat stuck";
            context.shouldExecute = true;
            ResetCombatNoTargetTicks(player);
            return context;
        }
    }
    else
    {
        ResetCombatNoTargetTicks(player);
    }

    if (hasInvalidSelectedTarget)
    {
        context.movementDirective = PvpClassSpellContext::MovementDirective::DropInvalidTarget;
        context.actionName = "drop target";
        context.reason = "invalid target";
        context.shouldExecute = true;
        context.movementTargetGuid = selectedTargetGuid;
        return context;
    }

    if (player->GetClass() == CLASS_HUNTER)
    {
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "BuildClassSpellContext snapshot: botGuid={} inBg={} bgActive={} inPrep={} inDuel={} hasValidTarget={} targetGuid={} allyGuid={}.",
            player->GetGUID().ToString(), values.inBattleground ? 1 : 0, IsTriggerActive(PvpTrigger::BgActive, values) ? 1 : 0,
            inBattlegroundPreparation ? 1 : 0, inActiveDuel ? 1 : 0, hasValidTarget ? 1 : 0,
            hasValidTarget ? selectedTargetGuid.ToString() : "none", hasValidAllyTarget ? selectedAllyGuid.ToString() : "none");
    }

    SpellDecision decision;
    SpellDecision firstDecision;
    uint32 suppressedSpellId = 0;
    uint32 attempts = 0;
    constexpr uint32 kMaxDecisionAttempts = 8;
    while (attempts++ < kMaxDecisionAttempts)
    {
        DecisionEvaluationScope decisionScope(player, suppressedSpellId);
        Unit const* decisionTarget = resolveTargetByGuid(activeTargetGuid);
        Unit const* decisionAllyTarget = resolveTargetByGuid(selectedAllyGuid);
        SpellDecision const candidate = SelectClassOrUtilitySpell(player, decisionTarget, decisionAllyTarget, profileSelection);
        if (!candidate.spellId)
            break;

        if (!firstDecision.spellId)
            firstDecision = candidate;

        Unit const* immediateCastTarget = resolveTargetByGuid(activeTargetGuid);
        Unit const* immediateCastAllyTarget = resolveTargetByGuid(selectedAllyGuid);
        if (IsDecisionImmediatelyCastable(player, candidate, immediateCastTarget, immediateCastAllyTarget))
        {
            decision = candidate;
            if (suppressedSpellId != 0)
            {
                TC_LOG_DEBUG("playerbots.pvp.classspell",
                    "Class spell fallback chain selected castable spell: botGuid={} fallbackSpell={} suppressedSeed={} attempts={} targetGuid={} allyGuid={}.",
                    player->GetGUID().ToString(), candidate.spellId, suppressedSpellId, attempts,
                    hasValidTarget ? selectedTargetGuid.ToString() : "none", hasValidAllyTarget ? selectedAllyGuid.ToString() : "none");
            }
            break;
        }

        suppressedSpellId = candidate.spellId;
    }

    // If no immediately castable spell was found, keep the first decision so execution
    // can still drive movement/position correction (for example out-of-range follow).
    if (!decision.spellId)
        decision = firstDecision;

    context.actionName = decision.actionName;
    context.reason = decision.reason;
    context.spellId = decision.spellId;
    if (context.spellId)
    {
        if (SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(context.spellId))
        {
            uint32 resolvedSpellId = 0;
            for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
            {
                if (player->HasSpell(chainSpellId))
                    resolvedSpellId = chainSpellId;
            }

            if (resolvedSpellId)
                context.spellId = resolvedSpellId;
        }
    }
    context.targetMode = decision.targetMode;
    context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
    context.itemEntry = decision.itemEntry;
    context.targetGuid = hasValidTarget ? activeTargetGuid : ObjectGuid::Empty;
    context.allyTargetGuid = hasValidAllyTarget ? selectedAllyGuid : ObjectGuid::Empty;
    if (!decision.targetGuid.IsEmpty())
        context.targetGuid = decision.targetGuid;
    if (context.targetMode == PvpClassSpellContext::TargetMode::Ally && context.targetGuid.IsEmpty())
        context.targetGuid = context.allyTargetGuid;
    else if (context.targetMode == PvpClassSpellContext::TargetMode::Self)
        context.targetGuid = player->GetGUID();

    if (context.spellId &&
        (context.targetMode == PvpClassSpellContext::TargetMode::Enemy || context.targetMode == PvpClassSpellContext::TargetMode::Ally))
    {
        Unit const* losRecoveryTarget = resolveTargetByGuid(context.targetGuid);
        if (losRecoveryTarget && !player->IsWithinLOSInMap(losRecoveryTarget))
        {
            SpellInfo const* recoverySpellInfo = sSpellMgr->GetSpellInfo(context.spellId);
            float const spellMaxRange = recoverySpellInfo ? player->GetSpellMaxRangeForTarget(losRecoveryTarget, recoverySpellInfo) : 0.0f;
            float const currentDistance = player->GetDistance(losRecoveryTarget);
            float const maxFollowRange = spellMaxRange > 0.0f
                ? std::max(1.5f, spellMaxRange - 1.0f)
                : std::max(1.5f, GetConfiguredSpellRange() - 1.0f);
            float const desiredRange = std::clamp(currentDistance - 2.0f, 1.5f, maxFollowRange);

            ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, losRecoveryTarget->GetGUID(),
                desiredRange, "recover los", "selected spell target out of line of sight", 86.0f);
            context.spellId = 0;
            context.itemEntry = 0;
            context.targetMode = PvpClassSpellContext::TargetMode::None;
            context.targetGuid = ObjectGuid::Empty;
            context.selfCast = false;
        }
    }

    if (context.spellId &&
        (context.targetMode == PvpClassSpellContext::TargetMode::Enemy || context.targetMode == PvpClassSpellContext::TargetMode::Ally))
    {
        Unit const* facingTarget = resolveTargetByGuid(context.targetGuid);
        if (facingTarget && !player->HasInArc(static_cast<float>(M_PI), facingTarget))
        {
            ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FaceSpellTarget, facingTarget->GetGUID(), 0.0f,
                "set facing", "not facing target", 92.0f);
            context.spellId = 0;
            context.itemEntry = 0;
            context.targetMode = PvpClassSpellContext::TargetMode::None;
            context.targetGuid = ObjectGuid::Empty;
            context.selfCast = false;
        }
    }

    if (context.spellId && context.targetMode == PvpClassSpellContext::TargetMode::Enemy && UsesMeleeSpacingProfile(player, profileSelection))
    {
        Unit const* meleeTarget = resolveTargetByGuid(context.targetGuid);
        bool const isGapCloser = context.spellId == 11578 || context.spellId == 20617;
        if (meleeTarget && !player->IsWithinMeleeRange(meleeTarget) && !isGapCloser)
        {
            ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, meleeTarget->GetGUID(),
                std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "enemy out of melee", 85.0f);
            context.spellId = 0;
            context.itemEntry = 0;
            context.targetMode = PvpClassSpellContext::TargetMode::None;
            context.targetGuid = ObjectGuid::Empty;
            context.selfCast = false;
        }
    }

    if (!context.spellId && hasValidTarget)
    {
        Unit const* facingFallbackTarget = resolveTargetByGuid(activeTargetGuid);
        if (facingFallbackTarget && !player->HasInArc(static_cast<float>(M_PI), facingFallbackTarget))
        {
            ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FaceSpellTarget, facingFallbackTarget->GetGUID(), 0.0f,
                "set facing", "not facing target", 72.0f);
        }
    }

    // If the selected spell is not immediately castable due spacing, switch this
    // tick into movement-directive execution to mirror reference trigger flow.
    if (context.spellId && UsesRangedSpacingProfile(player, profileSelection))
    {
        Unit const* spacingTarget = nullptr;
        if (context.targetMode == PvpClassSpellContext::TargetMode::Enemy ||
            context.targetMode == PvpClassSpellContext::TargetMode::Ally)
        {
            spacingTarget = resolveTargetByGuid(context.targetGuid);
        }

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(context.spellId);
        if (spellInfo && spacingTarget)
        {
            float const distance = player->GetDistance(spacingTarget);
            float const maxRange = spellInfo->GetMaxRange(false);
            float const minRange = spellInfo->GetMinRange(false);
            if (maxRange > 0.0f && distance > maxRange)
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, spacingTarget->GetGUID(),
                    ComputeApproachFollowRange(maxRange), "reach spell", "selected spell out of range", 84.0f);
                context.spellId = 0;
                context.itemEntry = 0;
                context.targetMode = PvpClassSpellContext::TargetMode::None;
                context.targetGuid = ObjectGuid::Empty;
                context.selfCast = false;
            }
            else if (maxRange > 0.0f &&
                spellInfo->CalcCastTime() > 0 &&
                distance > std::max(1.0f, maxRange - 2.0f))
            {
                // Hard-cast edge guard: repeated casts at the absolute spell
                // ceiling can stutter on movement jitter and LOS drift. Step
                // in slightly so caster bots do not idle at max-range fringe.
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, spacingTarget->GetGUID(),
                    ComputeApproachFollowRange(maxRange), "reach spell", "selected spell near max range edge", 83.0f);
                context.spellId = 0;
                context.itemEntry = 0;
                context.targetMode = PvpClassSpellContext::TargetMode::None;
                context.targetGuid = ObjectGuid::Empty;
                context.selfCast = false;
            }
            else if (minRange > 0.0f && distance < std::max(0.0f, minRange + kRangedSpacingEnterTooCloseBuffer))
            {
                bool collapseToMelee = false;
                if (player->GetClass() == CLASS_HUNTER)
                {
                    float const meleeEnterRange = std::max(0.0f, GetConfiguredMeleeRange() + kRangedSpacingEnterTooCloseBuffer);
                    if (distance > meleeEnterRange)
                    {
                        // Hunters in the 5-8y dead-zone cannot use either
                        // ranged weapon shots or reliable melee pressure.
                        // Collapse into melee first, then resume normal
                        // hunter mode logic on subsequent ticks.
                        ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, spacingTarget->GetGUID(),
                            std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "selected spell dead-zone collapse", 84.0f);
                        collapseToMelee = true;
                    }
                }

                if (!collapseToMelee)
                {
                    // Enter too-close movement before strict dead-zone boundaries so
                    // ranged users do not idle in 5-8y style min-range gaps.
                    // Keep an extra cushion over strict spell minimum range so ranged
                    // weapon casts (e.g. Hunter Auto Shot at 8y min range) do not
                    // immediately re-enter the dead-zone from minor pathing drift.
                    float const fleeFollowRange = std::max(std::max(1.0f, GetConfiguredCloseRange()), minRange + 2.0f);
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, spacingTarget->GetGUID(),
                        fleeFollowRange, "flee", "selected spell minimum range violation", 84.0f);
                }
                context.spellId = 0;
                context.itemEntry = 0;
                context.targetMode = PvpClassSpellContext::TargetMode::None;
                context.targetGuid = ObjectGuid::Empty;
                context.selfCast = false;
            }
            else if (context.targetMode == PvpClassSpellContext::TargetMode::Enemy &&
                distance > (GetConfiguredSpellRange() + kRangedSpacingEnterOutOfRangeBuffer))
            {
                // Some classic spell entries report atypical range metadata,
                // which can leave ranged bots idling at ~35-40y while still
                // selecting enemy casts. Keep a config-based engage floor so
                // they always step in to practical casting distance.
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, spacingTarget->GetGUID(),
                    ComputeApproachFollowRange(GetConfiguredSpellRange()), "reach spell", "enemy outside configured cast distance", 82.0f);
                context.spellId = 0;
                context.itemEntry = 0;
                context.targetMode = PvpClassSpellContext::TargetMode::None;
                context.targetGuid = ObjectGuid::Empty;
                context.selfCast = false;
            }
        }
    }

    bool const healerHasLivingAllyTarget = hasValidAllyTarget && CanUseHealRangeSpacing(player->GetClass());

    if (!context.spellId && context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
        healerHasLivingAllyTarget)
    {
        Unit const* allyMovementTarget = resolveTargetByGuid(selectedAllyGuid);
        if (allyMovementTarget)
        {
            float const distance = player->GetDistance(allyMovementTarget);
            if (distance > GetConfiguredHealRange())
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, allyMovementTarget->GetGUID(),
                    ComputeApproachFollowRange(GetConfiguredHealRange()), "reach party member to heal", "party member to heal out of spell range", 72.0f);
            }
        }
    }

    // Reference parity bridge: provide trigger-like movement directives even
    // when we do not have a castable spell yet ("enemy out of spell" / "enemy
    // too close for spell"). Keep classic spell IDs untouched.
    if (!context.spellId && hasValidTarget && UsesRangedSpacingProfile(player, profileSelection) && !healerHasLivingAllyTarget)
    {
        Unit const* movementTarget = resolveTargetByGuid(activeTargetGuid);
        if (movementTarget)
        {
            float const distance = player->GetDistance(movementTarget);
            if (distance > (GetConfiguredSpellRange() + kRangedSpacingEnterOutOfRangeBuffer))
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, movementTarget->GetGUID(),
                    ComputeApproachFollowRange(GetConfiguredSpellRange()), "reach spell", "enemy out of spell range", 70.0f);
            }
            else if (player->GetClass() == CLASS_HUNTER &&
                distance < std::max(0.0f, GetHunterDeadZoneMaxRange() + kRangedSpacingEnterTooCloseBuffer) &&
                distance > std::max(0.0f, GetConfiguredMeleeRange() + kRangedSpacingEnterTooCloseBuffer))
            {
                // No castable spell + hunter dead-zone often left the bot in a
                // bow-raise idle loop. For this 5-8y band, force a short melee
                // close to re-enter a valid action envelope.
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, movementTarget->GetGUID(),
                    std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "enemy in hunter dead-zone", 71.0f);
            }
            else if (distance < std::max(0.0f, GetConfiguredMeleeRange() + kRangedSpacingEnterTooCloseBuffer))
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, movementTarget->GetGUID(),
                    std::max(1.0f, GetConfiguredCloseRange()), "flee", "enemy too close for spell", 71.0f);
            }
        }
    }

    if (!context.spellId && context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
        hasValidTarget && UsesMeleeSpacingProfile(player, profileSelection))
    {
        Unit const* meleeMovementTarget = resolveTargetByGuid(selectedTargetGuid);
        if (meleeMovementTarget && !player->IsWithinMeleeRange(meleeMovementTarget))
        {
            ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, meleeMovementTarget->GetGUID(),
                std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "enemy out of melee", 69.0f);
        }
    }

    if (!context.spellId && context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
        hasValidTarget && IsLowOrOutOfManaForFallback(player))
    {
        Unit const* fallbackTarget = resolveTargetByGuid(selectedTargetGuid);
        if (fallbackTarget)
        {
            if (HasWandEquipped(player) && IsSpellReady(player, 5019) && !HasBreakableCrowdControl(fallbackTarget))
            {
                context.actionName = "fallback wand";
                context.reason = "low mana fallback to wand pressure";
                context.spellId = 5019;
                context.targetMode = PvpClassSpellContext::TargetMode::Enemy;
                context.targetGuid = fallbackTarget->GetGUID();
                context.selfCast = false;
            }
            else
            {
                if (UsesRangedSpacingProfile(player, profileSelection))
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, fallbackTarget->GetGUID(),
                        std::max(1.0f, GetConfiguredCloseRange()), "flee", "low mana fallback disengage to recover", 67.0f);
                }
                else
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, fallbackTarget->GetGUID(),
                        std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "low mana fallback to auto-attack", 67.0f);
                }
            }
        }
    }

    // PvP insignia/class-trinket crowd-control breaks are handled by the
    // per-player fast path in RandomBotParticipationManager::ProcessPlayerLifecycle.
    // Keep them out of the normal class-spell decision graph so they are not
    // throttled by decision cadence and do not consume the class action tick/GCD.

    context.shouldExecute = context.shouldExecute || context.spellId != 0;

    char const* targetModeLabel = "none";
    switch (context.targetMode)
    {
        case PvpClassSpellContext::TargetMode::Enemy:
            targetModeLabel = "enemy";
            break;
        case PvpClassSpellContext::TargetMode::Self:
            targetModeLabel = "self";
            break;
        case PvpClassSpellContext::TargetMode::Ally:
            targetModeLabel = "ally";
            break;
        case PvpClassSpellContext::TargetMode::None:
        default:
            break;
    }

    TC_LOG_DEBUG("playerbots.pvp.class",
        "Playerbot PvP class context: class={} profile={} fallback={} unsupported={} has_enemy_target={} enemy_target_guid={} ally_target_guid={} target_mode={} spell={} action={} reason={}.",
        GetClassLabel(player->GetClass()), profileSelection.profileLabel, profileSelection.usedFallback,
        profileSelection.unsupportedClass, hasValidTarget, hasValidTarget ? selectedTargetGuid.ToString() : ObjectGuid::Empty.ToString(),
        context.allyTargetGuid.ToString(), targetModeLabel, context.spellId, context.actionName ? context.actionName : "none",
        context.reason ? context.reason : "none");
    return context;
}

RandomBotParticipationHooks PvpCore::BuildRandomBotParticipationHooks(Player const* player, PvpValues const& values)
{
    RandomBotParticipationHooks hooks;
    hooks.lifecycleEnabled = IsLifecycleEnabled();
    if (!hooks.lifecycleEnabled || !player)
        return hooks;

    BattlegroundLifecycleContext bgContext = BuildBattlegroundLifecycleContext(player, values);
    ArenaLifecycleContext arenaContext = BuildArenaLifecycleContext(player, values);

    hooks.battlegroundParticipationHook = (bgContext.queueOperation != QueueOperationType::None) ||
        (bgContext.invitationResponse != InvitationResponseType::None) || bgContext.shouldHandleInProgressStatus;
    hooks.arenaParticipationHook = (arenaContext.queueOperation != QueueOperationType::None) ||
        (arenaContext.teamInteraction != ArenaTeamInteractionType::None);

    return hooks;
}

bool PvpCore::IsLifecycleEnabled()
{
    return IsLifecycleGateEnabled(g_PvpCoreConfig);
}

bool PvpCore::IsInBattlegroundQueue(Player const* player)
{
    if (!player)
        return false;

    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (player->GetBattlegroundQueueTypeId(i) != BATTLEGROUND_QUEUE_NONE)
            return true;

    return false;
}

BattlegroundState PvpCore::DetectBattlegroundState(Player const* player, bool inQueue)
{
    if (!player)
        return BattlegroundState::None;

    if (!player->InBattleground())
        return inQueue ? BattlegroundState::Queueing : BattlegroundState::None;

    if (Battleground const* battleground = player->GetBattleground())
    {
        if (battleground->GetStatus() == STATUS_WAIT_JOIN)
            return BattlegroundState::WaitingToStart;

        if (battleground->GetStatus() == STATUS_IN_PROGRESS)
            return BattlegroundState::Active;
    }

    return BattlegroundState::None;
}

BattlegroundObjectiveSelection PvpCore::SelectObjectiveSkeleton(PvpValues const& values)
{
    BattlegroundObjectiveSelection objective;

    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierActive, values))
        objective.type = BattlegroundObjectiveType::AttackFlagCarrier;
    else if ((!values.battlegroundTeamHasHumans && IsTriggerActive(PvpTrigger::PlayerHasFlag, values)) ||
        IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
        objective.type = BattlegroundObjectiveType::ProtectFlagCarrier;

    return objective;
}

BattlegroundMovementPrimitive PvpCore::SelectMovementPrimitiveSkeleton(PvpValues const& values,
    BattlegroundObjectiveSelection const& objective)
{
    if (!IsTriggerActive(PvpTrigger::BgActive, values))
        return BattlegroundMovementPrimitive::None;

    switch (objective.type)
    {
        case BattlegroundObjectiveType::AttackFlagCarrier:
        case BattlegroundObjectiveType::ProtectFlagCarrier:
            return BattlegroundMovementPrimitive::MoveToObjectiveUnit;
        case BattlegroundObjectiveType::AssaultNode:
        case BattlegroundObjectiveType::DefendNode:
        case BattlegroundObjectiveType::CaptureFlag:
            return BattlegroundMovementPrimitive::MoveToObjectivePosition;
        case BattlegroundObjectiveType::None:
        default:
            return BattlegroundMovementPrimitive::MoveToObjectivePosition;
    }
}

FlagCarrierDirective PvpCore::SelectFlagCarrierDirectiveSkeleton(PvpValues const& values)
{
    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierActive, values))
        return FlagCarrierDirective::AttackEnemyCarrier;

    if ((!values.battlegroundTeamHasHumans && IsTriggerActive(PvpTrigger::PlayerHasFlag, values)) ||
        IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
        return FlagCarrierDirective::ProtectTeamCarrier;

    return FlagCarrierDirective::None;
}

QueueOperationType PvpCore::SelectBattlegroundQueueOperationSkeleton(PvpValues const& values)
{
    if (values.inBattleground)
        return QueueOperationType::None;

    if (values.hasBattlegroundQueue && values.hasArenaInvite)
        return QueueOperationType::Leave;

    if (!values.inBattlegroundQueue && !values.hasBattlegroundQueue && !values.hasBattlegroundInvite &&
        !values.hasArenaQueue && !values.hasArenaInvite)
        return QueueOperationType::Join;

    return QueueOperationType::None;
}

InvitationResponseType PvpCore::SelectBattlegroundInvitationResponseSkeleton(PvpValues const& values)
{
    if (!values.hasBattlegroundInvite)
        return InvitationResponseType::None;

    if (values.inBattleground)
        return InvitationResponseType::Decline;

    return InvitationResponseType::Accept;
}

bool PvpCore::ShouldHandleBattlegroundInProgressStatusSkeleton(PvpValues const& values)
{
    // Keep lifecycle handling active for bots that are still flagged in a battleground,
    // including STATUS_WAIT_LEAVE so they can execute LeaveBattleground() and return
    // to their recorded queue entry point.
    return values.inBattleground;
}

QueueOperationType PvpCore::SelectArenaQueueOperationSkeleton(PvpValues const& values)
{
    if (values.inBattleground)
        return QueueOperationType::None;

    // Managed PvP lifecycle policy: never auto-fill arena. Any lingering arena
    // queue or invite state should be actively removed so battleground
    // participation can converge back to Warsong-only behavior.
    if (values.hasArenaQueue || values.hasArenaInvite)
        return QueueOperationType::Leave;

    return QueueOperationType::None;
}

ArenaTeamInteractionType PvpCore::SelectArenaTeamInteractionSkeleton(PvpValues const& values)
{
    // Managed PvP lifecycle policy: never join arena teams automatically.
    // Decline any arena team invite regardless of current queue state.
    if (values.hasArenaTeamInvite)
        return ArenaTeamInteractionType::DeclineInvite;

    return ArenaTeamInteractionType::None;
}
}
