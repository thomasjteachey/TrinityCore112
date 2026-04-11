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
SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player);

constexpr float kReferenceHunterSwitchDistance = 8.0f;
std::unordered_map<ObjectGuid, bool> g_HunterRangedModeByBot;
std::mutex g_HunterRangedModeByBotLock;
thread_local ObjectGuid g_CurrentDecisionBotGuid = ObjectGuid::Empty;
thread_local uint32 g_SuppressedDecisionSpellId = 0;

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
        if (target->GetVictim() == player && distance <= kReferenceHunterSwitchDistance)
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

SpellDecision MaybeSelectUtilitySpell(Player const* player, Unit const* hostileTarget);

playerbot::PvpCoreConfig g_PvpCoreConfig;
bool CanAttemptMount(Player const* player, SpellInfo const* mountSpellInfo);
bool IsHardControlled(Player const* player);

float GetConfiguredSpellRange() { return g_PvpCoreConfig.spellRange; }
float GetConfiguredHealRange() { return g_PvpCoreConfig.healRange; }
float GetConfiguredMeleeRange() { return g_PvpCoreConfig.meleeRange; }
float GetConfiguredCloseRange() { return g_PvpCoreConfig.closeRange; }
float GetConfiguredLongRange() { return g_PvpCoreConfig.longRange; }

bool IsLifecycleGateEnabled(playerbot::PvpCoreConfig const& config)
{
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
}

bool IsClassSpellGateEnabled(playerbot::PvpCoreConfig const& config)
{
    return config.moduleEnabled && config.pvpCoreEnabled && config.pvpClassSpellsEnabled;
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

    TeamId const botTeam = player->GetTeamId();
    TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
    ObjectGuid const playerGuid = player->GetGUID();

    if (BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(battleground))
    {
        ObjectGuid const enemyCarrierGuid = bgWs->GetFlagPickerGUID(botTeam);
        ObjectGuid const teamCarrierGuid = bgWs->GetFlagPickerGUID(enemyTeam);

        values.playerHasFlag = (teamCarrierGuid == playerGuid);
        values.enemyFlagCarrierNear = IsFlagCarrierNear(player, enemyCarrierGuid, 100.0f);

        bool const bothFlagsNotAtBase =
            bgWs->GetFlagState(ALLIANCE) != BG_WS_FLAG_STATE_ON_BASE &&
            bgWs->GetFlagState(HORDE) != BG_WS_FLAG_STATE_ON_BASE;
        if (!bothFlagsNotAtBase)
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
            values.enemyFlagCarrierNear = player->IsWithinDistInMap(carrier, 100.0f);
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
};

struct PrioritizedSpellDecision
{
    float priority = 0.0f;
    SpellDecision decision;
};

SpellDecision MaybeSelectUtilitySpell(Player const* player, Unit const* hostileTarget)
{
    if (!player)
        return {};

    // Match reference behavior more closely: do not let out-of-combat utility
    // preempt combat spell trees while a valid hostile target exists.
    if (HasHostileTarget(player, hostileTarget))
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

SpellDecision SelectHighestPriorityDecision(std::vector<PrioritizedSpellDecision>& candidates)
{
    if (candidates.empty())
        return {};

    std::stable_sort(candidates.begin(), candidates.end(), [](PrioritizedSpellDecision const& left, PrioritizedSpellDecision const& right)
    {
        return left.priority > right.priority;
    });

    return candidates.front().decision;
}

bool IsDecisionImmediatelyCastable(Player const* player, SpellDecision const& decision, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget)
{
    if (!player || !decision.spellId || !player->HasSpell(decision.spellId))
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
    if (decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally && !player->IsValidAssistTarget(resolvedTarget, spellInfo))
        return false;

    if (!player->IsWithinLOSInMap(resolvedTarget))
        return false;

    float const maxRange = spellInfo->GetMaxRange(false);
    if (maxRange > 0.0f && !player->IsWithinDistInMap(resolvedTarget, maxRange))
        return false;

    float const minRange = spellInfo->GetMinRange(false);
    if (minRange > 0.0f && player->IsWithinDistInMap(resolvedTarget, minRange))
        return false;

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
            if (player->HasTalent(11129, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Fire-like", false, false };
            if (player->HasTalent(11426, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Frost-like", false, false };
            break;
        case CLASS_WARLOCK:
            if (player->HasTalent(18220, activeSpec))
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

bool CanAttemptMount(Player const* player, SpellInfo const* mountSpellInfo)
{
    if (!player || !mountSpellInfo)
        return false;

    uint32 zoneId = 0;
    uint32 areaId = 0;
    player->GetZoneAndAreaId(zoneId, areaId);
    if (mountSpellInfo->CheckLocation(player->GetMapId(), zoneId, areaId, player, false) != SPELL_CAST_OK)
        return false;

    Map const* map = player->GetMap();
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

    // Keep a single nearby-enemy boundary for "switch to combat posture".
    // Inside this range we should avoid out-of-combat utility behaviors
    // (eat/drink/mount), so movement + combat targeting can take over cleanly.
    float const nearbyHostileCombatBoundary = std::max(GetConfiguredLongRange(), 35.0f);
    if (player->GetMap())
    {
        Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, nearbyHostileCombatBoundary))
                continue;

            return decision;
        }
    }

    bool const needsFood = player->GetHealthPct() < 100.0f;
    bool const usesMana = player->GetMaxPower(POWER_MANA) > 0;
    bool const needsDrink = usesMana && player->GetPowerPct(POWER_MANA) < 100.0f;
    bool const hasEatAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
    bool const hasDrinkAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

    // Recovery auras should naturally break on movement and should not linger
    // once the corresponding resource has fully recovered.
    if (Player* mutablePlayer = const_cast<Player*>(player))
    {
        if (hasEatAura && (mutablePlayer->isMoving() || !needsFood))
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

    if (needsDrink && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
        return { "drink", "recover mana out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, playerbot::PvpClassSpellContext::TargetMode::Self };

    bool const inBattlegroundPreparation = player->InBattleground() &&
        (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION));
    bool const inActiveBattleground = player->InBattleground() && !inBattlegroundPreparation;
    bool const inArenaMap = player->InArena();
    bool const inActiveDuel = player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;

    // Mounting while an active PvP round is running creates behavior that
    // diverges from reference check-mount-state logic. Keep recovery spells,
    // but suppress new mount decisions in these contexts.
    if (inActiveBattleground || inArenaMap || inActiveDuel)
        return decision;

    if (inBattlegroundPreparation)
        return decision;

    if (!player->IsOutdoors())
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

ObjectGuid SelectFriendlyWithoutAuraFromSpellChain(Player const* player, uint32 baseSpellId, float maxDistance, bool includeSelf)
{
    if (!player || !player->GetMap() || !baseSpellId)
        return ObjectGuid::Empty;

    auto isEligible = [&](Player* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return false;
        if (candidate != player && !player->IsValidAssistTarget(candidate))
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
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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

    switch (player->GetClass())
    {
        case CLASS_PRIEST:
        {
            if (IsSpellReady(player, 10938))
            {
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAuraFromSpellChain(player, 10938, 45.0f, true); !targetGuid.IsEmpty())
                    return { "priest power word fortitude prep", "buff nearby team before gates open", 10938, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            if (IsSpellReady(player, 10958))
            {
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAuraFromSpellChain(player, 10958, 45.0f, true); !targetGuid.IsEmpty())
                    return { "priest shadow protection prep", "buff nearby team before gates open", 10958, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            break;
        }
        case CLASS_MAGE:
        {
            if (IsSpellReady(player, 10157))
            {
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAuraFromSpellChain(player, 10157, 45.0f, true); !targetGuid.IsEmpty())
                    return { "arcane intellect prep", "buff nearby team before gates open", 10157, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

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
    if (!player || !player->GetMap())
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

    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
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
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
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
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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

bool HasDotAura(Unit const* unit)
{
    if (!unit)
        return false;

    return unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) ||
        unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
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
    if (!player || !player->GetMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
        return nullptr;

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        return HasHostileTarget(player, candidate) &&
            !IsTargetInvalidByImmunity(player, candidate) &&
            candidate->HasUnitState(UNIT_STATE_CASTING) &&
            player->IsWithinLOSInMap(candidate) &&
            player->IsWithinDistInMap(candidate, maxDistance);
    };

    if (isCandidateUsable(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
        return false;

    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
        return nullptr;

    SpellInfo const* polymorphInfo = sSpellMgr->GetSpellInfo(12826);
    DiminishingGroup const polymorphDrGroup = polymorphInfo ? polymorphInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;

    std::vector<Unit const*> preferredTargets;
    std::vector<Unit const*> fallbackTargets;
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
        if (polymorphDrGroup != DIMINISHING_NONE && candidate->GetDiminishing(polymorphDrGroup) >= DIMINISHING_LEVEL_IMMUNE)
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

Unit const* SelectFriendlyCurseTarget(Player const* player, float maxDistance)
{
    if (!player || !player->GetMap())
        return nullptr;

    auto hasDispellableCurse = [&](Unit const* target)
    {
        if (!target || !target->IsAlive())
            return false;

        DispelChargesList dispelList;
        target->GetDispellableAuraList(player, (1 << DISPEL_CURSE), dispelList);
        return !dispelList.empty();
    };

    if (hasDispellableCurse(player))
        return player;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (!player->IsValidAssistTarget(candidate))
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

    return best;
}

Unit const* SelectRogueBlindTarget(Player const* player, Unit const* primaryTarget, float maxDistance)
{
    if (!player || !player->GetMap())
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
        return true;
    };

    Unit const* bestSecondary = nullptr;
    float bestSecondaryDistance = std::numeric_limits<float>::max();
    Unit const* fallbackPrimary = nullptr;
    float fallbackPrimaryDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isPriorityBlindTarget(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (primaryTarget && candidate->GetGUID() == primaryTarget->GetGUID())
        {
            if (distance < fallbackPrimaryDistance)
            {
                fallbackPrimary = candidate;
                fallbackPrimaryDistance = distance;
            }
            continue;
        }

        if (distance < bestSecondaryDistance)
        {
            bestSecondary = candidate;
            bestSecondaryDistance = distance;
        }
    }

    return bestSecondary ? bestSecondary : fallbackPrimary;
}

Unit const* SelectWarlockFearTarget(Player const* player, float maxDistance)
{
    if (!player || !player->GetMap())
        return nullptr;

    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (candidate->HasAuraType(SPELL_AURA_MOD_FEAR))
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
    if (!player || !player->GetMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestHealth = 101.0f;
    float bestDistance = std::numeric_limits<float>::max();

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return;
        if (candidate != player && !player->IsValidAssistTarget(candidate))
            return;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return;

        float const healthPct = candidate->GetHealthPct();
        if (healthPct > maxHealthPct)
            return;

        float const distance = player->GetDistance(candidate);
        if (healthPct < bestHealth || (std::abs(healthPct - bestHealth) < 0.1f && distance < bestDistance))
        {
            best = candidate;
            bestHealth = healthPct;
            bestDistance = distance;
        }
    };

    evaluateCandidate(player);

    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best;
}

Unit const* SelectFriendlyDispelTarget(Player const* player, DispelType dispelType, float maxDistance)
{
    if (!player || !player->GetMap())
        return nullptr;

    auto hasDispellableAura = [&](Unit const* target)
    {
        if (!target || !target->IsAlive())
            return false;

        DispelChargesList dispelList;
        target->GetDispellableAuraList(player, (1 << dispelType), dispelList);
        return !dispelList.empty();
    };

    if (hasDispellableAura(player))
        return player;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive())
            continue;
        if (!player->IsValidAssistTarget(candidate))
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

    return best;
}

Unit const* SelectEnemyNonBreakableCrowdControlTarget(Player const* player, float maxDistance)
{
    if (!player || !player->GetMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    uint32 constexpr mechanicMask =
        (1 << MECHANIC_ROOT) |
        (1 << MECHANIC_STUN) |
        (1 << MECHANIC_FREEZE) |
        (1 << MECHANIC_SNARE);

    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
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
        return !dispelList.empty();
    };

    if (hasDispellableAura(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestMana = 101.0f;
    float bestDistance = std::numeric_limits<float>::max();

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return;
        if (candidate != player && !player->IsValidAssistTarget(candidate))
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
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best;
}

Unit const* SelectFriendlySnaredTarget(Player const* player, float maxDistance)
{
    if (!player || !player->GetMap())
        return nullptr;

    auto isSnared = [](Unit const* target)
    {
        return target && (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) || target->HasAuraWithMechanic(1 << MECHANIC_ROOT));
    };

    if (isSnared(player))
        return player;

    Unit const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive())
            continue;
        if (!player->IsValidAssistTarget(candidate))
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

uint32 CountNearbyEnemies(Player const* player, float maxDistance)
{
    if (!player || !player->GetMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
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
    if (!player || !player->GetMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive())
            continue;
        if (candidate != player && !player->IsValidAssistTarget(candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        ++count;
    }

    return count;
}

Unit const* SelectCombatTarget(Player const* player)
{
    if (!player)
        return nullptr;

    auto isTargetUsable = [&](Unit const* candidate)
    {
        if (!HasHostileTarget(player, candidate))
            return false;
        if (IsTargetInvalidByImmunity(player, candidate))
            return false;
        return true;
    };

    if (ObjectGuid const selectedGuid = player->GetTarget(); !selectedGuid.IsEmpty())
        if (Unit const* selectedTarget = ObjectAccessor::GetUnit(*player, selectedGuid); isTargetUsable(selectedTarget))
            return selectedTarget;

    Unit const* victimTarget = player->GetVictim();
    if (isTargetUsable(victimTarget))
        return victimTarget;

    return SelectClosestEnemyTarget(player, true);
}

Unit const* SelectAllyTarget(Player const* player)
{
    if (!player)
        return nullptr;

    ObjectGuid const selectedGuid = player->GetTarget();
    if (selectedGuid.IsEmpty() || selectedGuid == player->GetGUID())
        return nullptr;

    Unit const* selected = ObjectAccessor::GetUnit(*player, selectedGuid);
    if (!selected || !selected->IsAlive())
        return nullptr;

    if (!player->IsValidAssistTarget(selected))
        return nullptr;

    if (!player->IsWithinLOSInMap(selected) || !player->IsWithinDistInMap(selected, GetConfiguredHealRange()))
        return nullptr;

    return selected;
}

SpellDecision SelectHunterSpell(Player const* player, Unit const* target, bool inMelee)
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
    Unit const* manaTarget = SelectNearbyEnemyTarget(player, activeTarget, GetConfiguredLongRange());

    target = activeTarget;
    bool const targetClose = player->IsWithinDistInMap(target, kReferenceHunterSwitchDistance);
    bool const enemyOnTop = HasHostileTarget(player, enemyOnTopTarget);
    bool const enemyNear = player->IsWithinDistInMap(target, GetConfiguredCloseRange());
    bool const rangedMode = IsHunterInRangedMode(player);

    bool const targetSnaredOrStunned = target &&
        (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
         target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN)));

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HealthBelowPct(35) && IsSpellReady(player, 19263), 35.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, enemyOnTop && HasAuraFromSpellChain(enemyOnTopTarget, 14268) && IsSpellReady(player, 5384) && IsSpellReady(player, 14311), 35.0f,
        { "hunter feign death", "set up freezing trap while pressured in melee", 5384, playerbot::PvpClassSpellContext::TargetMode::Self, enemyOnTopTarget->GetGUID() });
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 14325) && IsSpellReady(player, 14325), 29.5f,
        { "hunter mark", "mark rogue targets for anti-stealth pressure", 14325, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget->GetGUID() });
    AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 20906) && IsSpellReady(player, 20906), 27.5f,
        { "hunter trueshot aura", "maintain personal buff aura", 20906, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !hasLivingPet && !hasDeadPet && IsSpellReady(player, 883), 26.0f,
        { "hunter call pet", "summon active stable pet when no pet is present", 883, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasDeadPet && !player->IsInCombat() && IsSpellReady(player, 982), 25.0f,
        { "hunter revive pet", "recover pet out of combat", 982, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, enemyOnTop && enemyOnTopTarget->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 19503), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget->GetGUID() });
    AddDecisionCandidate(candidates, nearbyCastingTarget && IsSpellReady(player, 19503), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget->GetGUID() });
    AddDecisionCandidate(candidates, enemyOnTop && IsSpellReady(player, 14268) && !HasAuraFromSpellChain(enemyOnTopTarget, 14268), 21.0f,
        { "hunter wing clip", "close-range fallback snare", 14268, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget->GetGUID() });
    AddDecisionCandidate(candidates, !targetClose && !targetSnaredOrStunned && IsSpellReady(player, 5116), 20.0f,
        { "hunter concussive shot", "kite or chase control", 5116, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 25295) && IsSpellReady(player, 25295), 19.5f,
        { "hunter serpent sting", "apply ranged dot pressure", 25295, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget->GetGUID() });
    AddDecisionCandidate(candidates, rangedMode && !enemyNear && IsSpellReady(player, 20904), 18.0f,
        { "hunter aimed shot", "long cast pressure from range", 20904, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, rangedMode && !inMelee && IsSpellReady(player, 25294), 17.0f,
        { "hunter multi-shot", "ranged burst pressure", 25294, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, rangedMode && !inMelee && IsSpellReady(player, 3045), 16.0f,
        { "hunter rapid fire", "burst cooldown while freecasting at range", 3045, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, manaTarget && manaTarget->GetPowerType() == POWER_MANA && !HasAuraFromSpellChain(manaTarget, 14280) && IsSpellReady(player, 14280), 15.0f,
        { "hunter viper sting", "drain mana on mana users", 14280, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaTarget->GetGUID() });
    AddDecisionCandidate(candidates, enemyOnTop && (!IsSpellReady(player, 5384) || !IsSpellReady(player, 14311)) && IsSpellReady(player, 19503) && !HasBreakableCrowdControl(enemyOnTopTarget), 14.0f,
        { "hunter scatter shot", "fallback peel when trap setup unavailable", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget->GetGUID() });
    AddDecisionCandidate(candidates, enemyOnTop && closeMeleeThreat && !IsSpellReady(player, 19503) && (!IsSpellReady(player, 5384) || !IsSpellReady(player, 14311)) && IsSpellReady(player, 19263), 13.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });

    return SelectHighestPriorityDecision(candidates);
}

SpellDecision SelectMageSpell(Player const* player, Unit const* target, bool inMelee)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const closePressure = hasHostileTarget && player->IsWithinDistInMap(target, GetConfiguredMeleeRange());
    float const manaPct = player->GetPowerPct(POWER_MANA);
    Unit const* cursedTarget = IsSpellReady(player, 475) ? SelectFriendlyCurseTarget(player, 40.0f) : nullptr;
    Unit const* castingTarget = IsSpellReady(player, 2139) ? SelectEnemyCastingTarget(player, 30.0f, target) : nullptr;
    Unit const* polymorphTarget =
        (IsSpellReady(player, 12826) && !AnyEnemyPolymorphed(player, 40.0f)) ? SelectPolymorphTarget(player, target, 30.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HealthBelowPct(25) && IsSpellReady(player, 11958), 60.0f,
        { "mage ice block", "self-preservation emergency", 11958, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, closePressure && IsSpellReady(player, 1953), 45.0f,
        { "mage blink", "escape melee pressure", 1953, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, castingTarget && IsSpellReady(player, 2139), 44.0f,
        { "mage counterspell", "interrupt any enemy cast in range", 2139, playerbot::PvpClassSpellContext::TargetMode::Enemy, castingTarget ? castingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, closePressure && IsSpellReady(player, 10230), 43.0f,
        { "mage frost nova", "close defensive peel", 10230, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, closePressure && target && IsMeleeClass(target) && IsSpellReady(player, 10161), 42.0f,
        { "mage cone of cold", "defensive snare versus nearby melee", 10161, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, manaPct < 25.0f && IsSpellReady(player, 12051), 41.0f,
        { "mage evocation", "recover mana below 25 percent", 12051, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, manaPct < 50.0f && player->HasItemCount(8008), 40.0f,
        { "use mana ruby", "consume mana ruby below 50 percent mana", 22044, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), 8008 });
    AddDecisionCandidate(candidates, cursedTarget, 39.0f,
        { "remove lesser curse", "dispel curse from friendly target", 475, (cursedTarget == player) ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 13033) && IsSpellReady(player, 13033), 35.0f,
        { "mage ice barrier", "maintain defensive absorb shield", 13033, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && target->HealthBelowPct(20) && IsSpellReady(player, 10199), 30.0f,
        { "mage fire blast", "instant execute pressure on low health target", 10199, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, polymorphTarget, 29.0f,
        { "mage polymorph", "priority crowd control on non-dotted paladin/priest targets", 12826, playerbot::PvpClassSpellContext::TargetMode::Enemy, polymorphTarget ? polymorphTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, hasHostileTarget && IsSpellReady(player, 25304), 18.0f,
        { "mage frostbolt", "default ranged pressure", 25304, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !player->IsInCombat() && IsSpellReady(player, 10157) && !player->HasAura(10157), 10.0f,
        { "arcane intellect", "arcane intellect", 10157, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsInCombat() && IsSpellReady(player, 10220) && !player->HasAura(10220), 9.0f,
        { "frost armor", "frost armor", 10220, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsSpellReady(player, 10054) && !player->HasItemCount(8008), 8.0f,
        { "create mana ruby", "create mana ruby", 10054, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !IsSpellReady(player, 11958) && IsSpellReady(player, 12472), 7.0f,
        { "mage cold snap", "reset frost defenses when ice block unavailable", 12472, playerbot::PvpClassSpellContext::TargetMode::Self });

    return SelectHighestPriorityDecision(candidates);
}

SpellDecision SelectPriestSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    Unit const* debuffedAlly = IsSpellReady(player, 988) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, GetConfiguredHealRange()) : nullptr;
    Unit const* enemyBuffedTarget = (IsSpellReady(player, 988) && hasHostileTarget) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, GetConfiguredSpellRange()) : nullptr;
    Unit const* shieldTarget = IsSpellReady(player, 10901) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 50.0f) : nullptr;
    Unit const* renewTarget = IsSpellReady(player, 10929) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 80.0f) : nullptr;
    Unit const* healTarget = IsSpellReady(player, 10917) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 85.0f) : nullptr;
    Unit const* casterAlly = (player->IsInCombat() && IsSpellReady(player, 10060)) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 100.0f) : nullptr;
    Unit const* controlledTarget = IsSpellReady(player, 27605) ? SelectEnemyNonBreakableCrowdControlTarget(player, 30.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;

    if (profileSelection.profile == ClassicClassProfile::PrimaryClassic)
    {
        AddDecisionCandidate(candidates, debuffedAlly, 46.0f,
            { "priest dispel magic ally", "prioritize dispelling magic debuffs from allies", 988, debuffedAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, debuffedAlly ? debuffedAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, enemyBuffedTarget, 45.0f,
            { "priest dispel magic enemy", "prioritize dispelling magic buffs from enemies", 988, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyBuffedTarget ? enemyBuffedTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, shieldTarget && !HasAuraFromSpellChain(shieldTarget, 10901), 44.0f,
            { "priest power word shield ally", "protect ally below 50 percent health", 10901, shieldTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, shieldTarget ? shieldTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, casterAlly && casterAlly->GetPowerType() == POWER_MANA, 30.0f,
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

    AddDecisionCandidate(candidates, hasHostileTarget && target && target->GetClass() == CLASS_ROGUE && !HasAuraFromSpellChain(target, 27605) && IsSpellReady(player, 27605), 22.0f,
        { "priest shadow word pain", "maintain dot pressure on rogues", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, hasHostileTarget && target && target->GetPowerType() == POWER_MANA && IsSpellReady(player, 14033), 21.0f,
        { "priest mana burn", "burn mana from enemy casters", 14033, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 10.0f) >= 2 && CountNearbyFriendlyPlayers(player, 10.0f) >= 2 && IsSpellReady(player, 27801), 20.0f,
        { "priest holy nova", "aoe pressure and splash healing in melee cluster", 27801, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && IsSpellReady(player, 27605) && !HasBreakableCrowdControl(target) && !HasAuraFromSpellChain(target, 27605), 19.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled or open targets", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, controlledTarget && !HasAuraFromSpellChain(controlledTarget, 27605), 18.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled targets", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy, controlledTarget ? controlledTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, IsSpellReady(player, 10917) && player->HealthBelowPct(85), 17.0f,
        { "priest flash heal", "fallback self-healing while under pressure", 10917, playerbot::PvpClassSpellContext::TargetMode::Self });

    return SelectHighestPriorityDecision(candidates);
}

SpellDecision SelectDruidSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    Unit const* lowManaAlly = IsSpellReady(player, 29166) ? SelectFriendlyLowManaTarget(player, 40.0f, 10.0f) : nullptr;
    Unit const* cursedTarget = IsSpellReady(player, 2782) ? SelectFriendlyDispelTarget(player, DISPEL_CURSE, 40.0f) : nullptr;
    Unit const* poisonedTarget = IsSpellReady(player, 2893) ? SelectFriendlyDispelTarget(player, DISPEL_POISON, 40.0f) : nullptr;
    Unit const* swiftmendTarget = IsSpellReady(player, 18562) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* emergencyLowTarget = (IsSpellReady(player, 17116) && IsSpellReady(player, 25297)) ? SelectFriendlyHealthTarget(player, 40.0f, 25.0f) : nullptr;
    Unit const* emergencyTarget = IsSpellReady(player, 25297) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* regrowthTarget = IsSpellReady(player, 9858) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
    Unit const* rejuvTarget = IsSpellReady(player, 25299) ? SelectFriendlyHealthTarget(player, 40.0f, 90.0f) : nullptr;
    Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, 30.0f);
    Unit const* meleeThreat = SelectNearbyMeleeTarget(player, target, 8.0f);

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, lowManaAlly && !lowManaAlly->HasAura(29166), 50.0f,
        { "druid innervate", "stabilize low-mana ally with innervate", 29166, lowManaAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowManaAlly ? lowManaAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, cursedTarget, 49.0f,
        { "druid remove curse", "remove curses from allies", 2782, cursedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, poisonedTarget, 48.0f,
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
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 9907) && IsSpellReady(player, 9907), 30.0f,
        { "druid faerie fire", "apply faerie fire to nearby rogues", 9907, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, meleeThreat && IsSpellReady(player, 5487), 29.0f,
        { "druid bear form", "swap to bear under physical melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->HasAura(5487) && meleeThreat && IsSpellReady(player, 16979), 28.0f,
        { "druid feral charge", "charge away from melee pressure in bear form", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });

    return SelectHighestPriorityDecision(candidates);
}

SpellDecision SelectPaladinSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    Unit const* cleanseTarget = IsSpellReady(player, 4987) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f) : nullptr;
    Unit const* freedomTarget = IsSpellReady(player, 1044) ? SelectFriendlySnaredTarget(player, 40.0f) : nullptr;
    Unit const* sacrificeTarget = IsSpellReady(player, 6940) ? SelectFriendlyHealthTarget(player, 40.0f, 95.0f) : nullptr;
    Unit const* executeTarget = SelectNearbyEnemyTarget(player, target, 30.0f);
    Unit const* stunTarget = IsSpellReady(player, 10308) ? SelectEnemyCastingTarget(player, 10.0f, executeTarget) : nullptr;
    Unit const* flashHealTarget = IsSpellReady(player, 19943) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
    Unit const* holyLightTarget = IsSpellReady(player, 635) ? SelectFriendlyHealthTarget(player, 40.0f, 60.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HealthBelowPct(20) && IsSpellReady(player, 1020), 60.0f,
        { "paladin divine shield", "emergency immunity under lethal pressure", 1020, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, cleanseTarget, 55.0f,
        { "paladin cleanse", "prioritize cleansing allies", 4987, cleanseTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cleanseTarget ? cleanseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, freedomTarget, 54.0f,
        { "paladin hand of freedom", "free snared or rooted ally", 1044, freedomTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, freedomTarget ? freedomTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, sacrificeTarget && sacrificeTarget != player && !sacrificeTarget->HasAura(6940), 53.0f,
        { "paladin hand of sacrifice", "keep hand of sacrifice cycling on allies", 6940, playerbot::PvpClassSpellContext::TargetMode::Ally, sacrificeTarget ? sacrificeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 8.0f) >= 2 && IsSpellReady(player, 26573), 52.0f,
        { "paladin consecration", "aoe pressure under close melee collapse", 26573, playerbot::PvpClassSpellContext::TargetMode::Self });
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
    AddDecisionCandidate(candidates, !player->HasAura(19746) && IsSpellReady(player, 19746), 20.0f,
        { "paladin concentration aura", "maintain concentration aura", 19746, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsInCombat() && !player->HasAura(25898) && IsSpellReady(player, 25898), 19.0f,
        { "paladin greater blessing of kings", "maintain kings out of combat", 25898, playerbot::PvpClassSpellContext::TargetMode::Self });

    return SelectHighestPriorityDecision(candidates);
}

SpellDecision SelectWarlockSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    Pet const* pet = player->GetPet();
    bool const needsPetSummon = !pet || !pet->IsAlive();
    bool const hasHostileTarget = HasHostileTarget(player, target);

    if (!hasHostileTarget)
    {
        if (needsPetSummon && !player->IsInCombat() && IsSpellReady(player, 697))
            return { "warlock summon voidwalker", "maintain voidwalker pet while out of combat", 697, playerbot::PvpClassSpellContext::TargetMode::Self };

        return decision;
    }

    bool const closePressure = player->IsWithinDistInMap(target, 8.0f);
    Unit const* fearTarget = IsSpellReady(player, 6215) ? SelectWarlockFearTarget(player, 20.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HealthBelowPct(45) && IsSpellReady(player, 7812), 55.0f,
        { "warlock sacrifice", "consume voidwalker shield under low health pressure", 7812, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 19647), 54.0f,
        { "warlock spell lock", "pet interrupt when available", 19647, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, fearTarget, 53.0f,
        { "warlock fear", "prioritize fear control on paladin/priest targets in range", 6215, playerbot::PvpClassSpellContext::TargetMode::Enemy, fearTarget ? fearTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsInCombat() && needsPetSummon && !player->HasAura(18708) && IsSpellReady(player, 18708), 52.0f,
        { "warlock fel domination", "prepare instant pet recovery before voidwalker summon", 18708, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, needsPetSummon && IsSpellReady(player, 697), 51.0f,
        { "warlock summon voidwalker", "recover voidwalker in combat when absent", 697, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->HasAura(25228) && IsSpellReady(player, 19028), 45.0f,
        { "warlock soul link", "maintain soul link when pet is available", 19028, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, target->GetPowerType() == POWER_MANA && ShouldUseCurseOfTongues(target) && !HasAuraFromSpellChain(target, 11719) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11719) && IsSpellReady(player, 11719), 36.0f,
        { "warlock curse of tongues", "slow enemy casting throughput", 11719, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !IsCasterClass(target) && !HasAuraFromSpellChain(target, 11713) && !HasAuraFromSpellChain(target, 11719) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11713) && IsSpellReady(player, 11713), 35.0f,
        { "warlock curse of agony", "apply curse of agony pressure to non-caster players", 11713, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !HasAuraFromSpellChain(target, 11672) && IsSpellReady(player, 11672), 34.0f,
        { "warlock corruption", "maintain corruption dot", 11672, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, (target->HealthBelowPct(20) || (closePressure && IsMeleeClass(target))) && IsSpellReady(player, 17926), 33.0f,
        { "warlock death coil", "peel melee or finish low enemy target", 17926, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->GetPower(POWER_MANA) < 400 && IsSpellReady(player, 11689), 32.0f,
        { "warlock life tap", "convert health to mana for sustained casting", 11689, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HasAura(17941) && IsSpellReady(player, 25307), 20.0f,
        { "warlock shadow bolt", "consume nightfall proc for instant pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, IsSpellReady(player, 25307), 19.0f,
        { "warlock shadow bolt", "default ranged pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityDecision(candidates);
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

    bool const inDefensiveStance = player->HasAura(71);
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, activeTarget, 8.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 8.0f, activeTarget);
    bool const hasNearbyMeleeThreat = HasHostileTarget(player, nearbyMeleeTarget);

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, (player->HasAuraWithMechanic(1 << MECHANIC_FEAR) || player->HasAuraWithMechanic(1 << MECHANIC_SAPPED)) &&
            player->HasAura(2458) && IsSpellReady(player, 18499), 60.0f,
        { "warrior berserker rage", "break fear-like control while in berserker stance", 18499, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, HasHostileTarget(player, nearbyCastingTarget) && IsSpellReady(player, 6552), 59.0f,
        { "warrior pummel", "interrupt nearby spellcasts", 6552, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, hasNearbyMeleeThreat && inDefensiveStance && IsSpellReady(player, 676), 58.0f,
        { "warrior disarm", "disarm threatening melee weapon users", 676, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyMeleeTarget ? nearbyMeleeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, hasNearbyMeleeThreat && !inDefensiveStance && IsSpellReady(player, 676) && IsSpellReady(player, 71) && player->GetPower(POWER_RAGE) >= 200, 57.0f,
        { "warrior defensive stance", "swap defensive before disarm against melee", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, CountNearbyUnsNaredEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 12323), 56.0f,
        { "warrior piercing howl", "apply area snare when multiple enemies are unsnared in melee range", 12323, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, (IsSpellReady(player, 6552) || IsSpellReady(player, 676) || IsSpellReady(player, 20252) || IsSpellReady(player, 1680) || IsSpellReady(player, 21553)) &&
            player->GetPower(POWER_RAGE) < 150 && IsSpellReady(player, 2687), 54.0f,
        { "warrior bloodrage", "generate rage to unlock rotational abilities", 2687, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inDefensiveStance && (!IsSpellReady(player, 676) || !hasNearbyMeleeThreat) && IsSpellReady(player, 2458), 53.0f,
        { "warrior berserker stance", "leave defensive stance when disarm is unavailable or no melee threat is nearby", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(activeTarget) && !player->IsInCombat() && IsSpellReady(player, 11578), 52.0f,
        { "warrior charge", "close gap to target from out of combat", 11578, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(activeTarget) && player->IsInCombat() && IsSpellReady(player, 20617), 51.0f,
        { "warrior intercept", "close gap to target while in combat", 20617, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() });
    AddDecisionCandidate(candidates, activeTarget->HealthBelowPct(20) && IsSpellReady(player, 20662), 50.0f,
        { "warrior execute", "finisher at low enemy health", 20662, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() });
    AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 25289) && IsSpellReady(player, 25289), 40.0f,
        { "warrior battle shout", "maintain attack power buff", 25289, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) &&
            (!HasAuraFromSpellChain(activeTarget, 7373) || (activeTarget->GetAura(7373) && activeTarget->GetAura(7373)->GetDuration() < 2000)) &&
            IsSpellReady(player, 7373), 39.0f,
        { "warrior hamstring", "maintain stickiness snare", 7373, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && profileSelection.profile == ClassicClassProfile::PrimaryClassic &&
            !HasAuraFromSpellChain(activeTarget, 21553) && IsSpellReady(player, 21553), 38.0f,
        { "warrior mortal strike", "arms-like burst pressure", 21553, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && activeTarget->GetClass() == CLASS_ROGUE &&
            !HasAuraFromSpellChain(activeTarget, 11574) && IsSpellReady(player, 11574), 37.0f,
        { "warrior rend", "apply anti-stealth bleed pressure on rogues", 11574, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && IsSpellReady(player, 1680), 36.0f,
        { "warrior whirlwind", "fallback aoe melee pressure", 1680, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() });

    return SelectHighestPriorityDecision(candidates);
}

SpellDecision SelectRogueSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (!HasHostileTarget(player, target))
        return decision;

    Unit const* blindTarget = IsSpellReady(player, 2094) ? SelectRogueBlindTarget(player, target, 15.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    // Disabled: weapon-poison automation from PvP decision loop.
    // This avoids touching weapon-enchant mutation paths while investigating combat-time crashes.

    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 1784) && IsSpellReady(player, 1784), 50.0f,
        { "rogue stealth", "enter stealth before engagement", 1784, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HasStealthAura() && IsSpellReady(player, 1833), 49.0f,
        { "rogue cheap shot", "default opener", 1833, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 1766), 48.0f,
        { "rogue kick", "interrupt enemy cast", 1766, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->HealthBelowPct(40) && IsSpellReady(player, 5277), 47.0f,
        { "rogue evasion", "defensive survival in melee", 5277, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->HealthBelowPct(50) && !player->IsWithinMeleeRange(target) && player->IsWithinDistInMap(target, 30.0f) && IsSpellReady(player, 11305), 46.0f,
        { "rogue sprint", "close gap for melee pressure", 11305, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, blindTarget, 45.0f,
        { "rogue blind", "prioritize druid/shaman/paladin secondary targets without abolish poison", 2094, playerbot::PvpClassSpellContext::TargetMode::Enemy, blindTarget ? blindTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->GetComboPoints() >= 5 && IsSpellReady(player, 8643), 44.0f,
        { "rogue kidney shot", "primary stun finisher at full combo points", 8643, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->GetComboPoints() >= 5 && IsSpellReady(player, 11300), 43.0f,
        { "rogue eviscerate", "combo finisher pressure", 11300, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !player->IsWithinMeleeRange(target) && player->IsWithinDistInMap(target, 25.0f) && IsSpellReady(player, 36554), 42.0f,
        { "rogue shadowstep", "bridge short gap before melee globals", 36554, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, IsSpellReady(player, 16511), 20.0f,
        { "rogue hemorrhage", "default subtlety combo point builder", 16511, playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityDecision(candidates);
}

SpellDecision SelectShamanSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (!HasHostileTarget(player, target))
        return decision;

    std::vector<PrioritizedSpellDecision> candidates;
    // Disabled: auto-casting Windfury Weapon from PvP loop while investigating weapon-dependent aura crashes.
    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 10432) && IsSpellReady(player, 10432), 34.0f,
        { "shaman lightning shield", "maintain shield buff out of combat", 10432, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 10414), 60.0f,
        { "shaman earth shock", "interrupt enemy cast with shock", 10414, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, target->GetPowerType() == POWER_MANA && IsSpellReady(player, 8177), 59.0f,
        { "shaman grounding totem", "counter incoming caster pressure", 8177, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsSpellReady(player, 16166), 58.0f,
        { "shaman elemental mastery", "trigger burst throughput cooldown", 16166, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsSpellReady(player, 10605), 57.0f,
        { "shaman chain lightning", "primary burst cast on kill target", 10605, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, IsMeleeClass(target) && player->IsWithinDistInMap(target, 10.0f) && IsSpellReady(player, 2484), 56.0f,
        { "shaman earthbind totem", "kite nearby melee pressure", 2484, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsMeleeClass(target) && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 10473), 55.0f,
        { "shaman frost shock", "snare medium-range melee threats", 10473, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, target->GetClass() == CLASS_ROGUE && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 8170), 54.0f,
        { "shaman poison cleansing totem", "answer rogue poison pressure", 8170, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, (target->GetClass() == CLASS_PRIEST || target->GetClass() == CLASS_WARLOCK) && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 8143), 53.0f,
        { "shaman tremor totem", "mitigate fear pressure from priest/warlock", 8143, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HealthBelowPct(50) && IsSpellReady(player, 10468), 52.0f,
        { "shaman lesser healing wave", "self-sustain while focused", 10468, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsSpellReady(player, 370), 40.0f,
        { "shaman purge", "strip enemy magical effects by default", 370, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, IsSpellReady(player, 15208), 39.0f,
        { "shaman lightning bolt", "fallback ranged damage cast", 15208, playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityDecision(candidates);
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
        return SelectHunterSpell(player, target, inMelee);
    case CLASS_MAGE:
        return SelectMageSpell(player, target, inMelee);
    case CLASS_PRIEST:
        return SelectPriestSpell(player, target, allyTarget, profileSelection);
    case CLASS_PALADIN:
        return SelectPaladinSpell(player, target);
    case CLASS_WARLOCK:
        return SelectWarlockSpell(player, target);
    case CLASS_DRUID:
        return SelectDruidSpell(player, target);
    case CLASS_WARRIOR:
        return SelectWarriorSpell(player, target, profileSelection);
    case CLASS_ROGUE:
        return SelectRogueSpell(player, target);
    case CLASS_SHAMAN:
        return SelectShamanSpell(player, target);
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

TacticalDecision SelectBattlegroundTacticalDecision(Player const* player, playerbot::PvpValues const& values)
{
    TacticalDecision decision;
    if (!player)
        return decision;

    bool const lowHealth = player->HealthBelowPct(35);
    bool const lowMana = player->GetPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 25.0f;
    bool const bgActive = values.battlegroundState == playerbot::BattlegroundState::Active;
    bool const bgWaiting = values.battlegroundState == playerbot::BattlegroundState::WaitingToStart;
    bool const periodicRefresh = bgActive;
    bool const often = bgActive;

    struct TacticalRule
    {
        char const* triggerName;
        bool condition;
        char const* actionName;
        float priority;
    };

    // Preserve Warsong/Battleground trigger intent as an explicit ordered chain:
    // highest-priority emergency handling first, then raid/bg pressure, then sustain.
    std::array<TacticalRule, 9> const rules =
    {{
        { "player has flag", playerbot::PvpCore::IsTriggerActive(playerbot::PvpTrigger::PlayerHasFlag, values) && !values.battlegroundTeamHasHumans, "bg move to objective", 90.0f },
        { "enemy flagcarrier near", playerbot::PvpCore::IsTriggerActive(playerbot::PvpTrigger::EnemyFlagCarrierNear, values), "attack enemy flag carrier", 70.0f },
        { "team flagcarrier near", playerbot::PvpCore::IsTriggerActive(playerbot::PvpTrigger::TeamFlagCarrierNear, values), "bg protect fc", 65.0f },
        { "bg waiting", bgWaiting, "bg move to start", 50.0f },
        { "bg active", bgActive, "bg move to objective", 50.0f },
        { "often", often, "bg check objective", 51.0f },
        { "timer bg", periodicRefresh, "bg reset objective force", 80.0f },
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
    if (!player || !player->InBattleground() || !player->GetMap())
        return 0;

    Battleground const* battleground = player->GetBattleground();
    if (!battleground)
        return 0;

    uint32 const botBgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    uint32 humanCount = 0;

    Map::PlayerList const& players = player->GetMap()->GetPlayers();
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

    Unit const* selectedTarget = SelectCombatTarget(player);
    ObjectGuid const selectedTargetGuid = selectedTarget ? selectedTarget->GetGUID() : ObjectGuid::Empty;
    auto resolveTargetByGuid = [&](ObjectGuid const& guid) -> Unit const*
    {
        if (guid.IsEmpty() || guid == player->GetGUID())
            return nullptr;

        Unit const* resolved = ObjectAccessor::GetUnit(*player, guid);
        if (!resolved || !resolved->IsAlive())
            return nullptr;

        return resolved;
    };
    bool const hasValidTarget = resolveTargetByGuid(selectedTargetGuid) != nullptr;

    ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
    Unit const* selectedAllyTarget = SelectAllyTarget(player);
    ObjectGuid const selectedAllyGuid = selectedAllyTarget ? selectedAllyTarget->GetGUID() : ObjectGuid::Empty;
    bool const hasValidAllyTarget = resolveTargetByGuid(selectedAllyGuid) != nullptr;
    if (player->GetClass() == CLASS_HUNTER)
    {
        TC_LOG_DEBUG("playerbots.pvp.classspell",
            "BuildClassSpellContext snapshot: botGuid={} inBg={} bgActive={} inPrep={} inDuel={} hasValidTarget={} targetGuid={} allyGuid={}.",
            player->GetGUID().ToString(), values.inBattleground ? 1 : 0, IsTriggerActive(PvpTrigger::BgActive, values) ? 1 : 0,
            inBattlegroundPreparation ? 1 : 0, inActiveDuel ? 1 : 0, hasValidTarget ? 1 : 0,
            hasValidTarget ? selectedTargetGuid.ToString() : "none", hasValidAllyTarget ? selectedAllyGuid.ToString() : "none");
    }

    SpellDecision decision;
    {
        DecisionEvaluationScope decisionScope(player, 0);
        Unit const* decisionTarget = resolveTargetByGuid(selectedTargetGuid);
        Unit const* decisionAllyTarget = resolveTargetByGuid(selectedAllyGuid);
        decision = SelectClassOrUtilitySpell(player, decisionTarget, decisionAllyTarget, profileSelection);
    }

    Unit const* immediateCastTarget = resolveTargetByGuid(selectedTargetGuid);
    Unit const* immediateCastAllyTarget = resolveTargetByGuid(selectedAllyGuid);
    if (decision.spellId && !IsDecisionImmediatelyCastable(player, decision, immediateCastTarget, immediateCastAllyTarget))
    {
        uint32 const initialDecisionSpellId = decision.spellId;
        DecisionEvaluationScope fallbackScope(player, decision.spellId);
        Unit const* fallbackTarget = resolveTargetByGuid(selectedTargetGuid);
        Unit const* fallbackAllyTarget = resolveTargetByGuid(selectedAllyGuid);
        SpellDecision const fallbackDecision = SelectClassOrUtilitySpell(player, fallbackTarget, fallbackAllyTarget, profileSelection);
        if (fallbackDecision.spellId)
        {
            decision = fallbackDecision;
            TC_LOG_DEBUG("playerbots.pvp.classspell",
                "Class spell fallback used: botGuid={} initialSpell={} fallbackSpell={} targetGuid={} allyGuid={}.",
                player->GetGUID().ToString(), initialDecisionSpellId, fallbackDecision.spellId,
                hasValidTarget ? selectedTargetGuid.ToString() : "none", hasValidAllyTarget ? selectedAllyGuid.ToString() : "none");
        }
    }

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
    context.targetGuid = hasValidTarget ? selectedTargetGuid : ObjectGuid::Empty;
    context.allyTargetGuid = hasValidAllyTarget ? selectedAllyGuid : ObjectGuid::Empty;
    if (!decision.targetGuid.IsEmpty())
        context.targetGuid = decision.targetGuid;
    if (context.targetMode == PvpClassSpellContext::TargetMode::Ally)
        context.targetGuid = context.allyTargetGuid;
    else if (context.targetMode == PvpClassSpellContext::TargetMode::Self)
        context.targetGuid = player->GetGUID();
    if (player->HasAuraWithMechanic((1 << MECHANIC_STUN) | (1 << MECHANIC_FEAR) | (1 << MECHANIC_CHARM) | (1 << MECHANIC_ROOT)))
    {
        if (IsSpellReady(player, 42292))
        {
            context.actionName = "pvp trinket";
            context.reason = "break major crowd control with medallion";
            context.spellId = 42292;
            context.targetMode = PvpClassSpellContext::TargetMode::Self;
            context.targetGuid = player->GetGUID();
        }
        else if (IsSpellReady(player, 7744))
        {
            context.actionName = "will of the forsaken";
            context.reason = "break fear/charm/sleep with racial";
            context.spellId = 7744;
            context.targetMode = PvpClassSpellContext::TargetMode::Self;
            context.targetGuid = player->GetGUID();
        }
        else if (IsSpellReady(player, 20589))
        {
            context.actionName = "escape artist";
            context.reason = "break movement-impairing control with racial";
            context.spellId = 20589;
            context.targetMode = PvpClassSpellContext::TargetMode::Self;
            context.targetGuid = player->GetGUID();
        }
    }

    context.shouldExecute = context.spellId != 0;

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

    // WSG brawl mode: ignore flag/objective play and let tactical movement
    // converge bots to the shared midfield fight anchor.
    if (values.battlegroundTypeId == BATTLEGROUND_WS)
        return objective;

    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierNear, values))
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
    if (values.battlegroundTypeId == BATTLEGROUND_WS)
        return FlagCarrierDirective::None;

    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierNear, values))
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
    return values.battlegroundState == BattlegroundState::Active;
}

QueueOperationType PvpCore::SelectArenaQueueOperationSkeleton(PvpValues const& values)
{
    if (values.inBattleground)
        return QueueOperationType::None;

    if (values.hasArenaQueue && values.hasBattlegroundInvite)
        return QueueOperationType::Leave;

    if (!values.inBattlegroundQueue && !values.hasArenaQueue && !values.hasArenaInvite &&
        !values.hasBattlegroundQueue && !values.hasBattlegroundInvite)
        return QueueOperationType::Join;

    return QueueOperationType::None;
}

ArenaTeamInteractionType PvpCore::SelectArenaTeamInteractionSkeleton(PvpValues const& values)
{
    if (!values.hasArenaTeamInvite)
        return ArenaTeamInteractionType::None;

    if (values.inBattleground || values.inBattlegroundQueue)
        return ArenaTeamInteractionType::DeclineInvite;

    return ArenaTeamInteractionType::AcceptInvite;

}
}
