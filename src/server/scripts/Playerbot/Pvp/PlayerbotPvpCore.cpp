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

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellHistory.h"

#include <array>

namespace
{
playerbot::PvpCoreConfig g_PvpCoreConfig;

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
};

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
    return player && spellId && player->HasSpell(spellId) && !player->GetSpellHistory()->HasCooldown(spellId);
}

bool HasHostileTarget(Player const* player, Unit const* target)
{
    return player && target && target->IsAlive() && target->GetGUID() != player->GetGUID() && player->IsValidAttackTarget(target);
}

Unit const* SelectAllyTarget(Player const* player)
{
    if (!player)
        return nullptr;

    ObjectGuid const selectedGuid = player->GetSelection();
    if (selectedGuid.IsEmpty())
        return nullptr;

    Unit const* selected = ObjectAccessor::GetUnit(*player, selectedGuid);
    if (!selected || !selected->IsAlive() || selected->GetGUID() == player->GetGUID())
        return nullptr;

    if (!player->IsValidAssistTarget(selected))
        return nullptr;

    if (!player->IsWithinLOSInMap(selected) || !player->IsWithinDistInMap(selected, 40.0f))
        return nullptr;

    return selected;
}

SpellDecision SelectHunterSpell(Player const* player, Unit const* target, bool inMelee)
{
    SpellDecision decision;
    if (!HasHostileTarget(player, target))
        return decision;

    bool const targetClose = player->IsWithinDistInMap(target, 8.0f);

    if (!targetClose && !target->HasAura(1978) && IsSpellReady(player, 1978))
        return { "hunter serpent sting", "apply ranged dot pressure", 1978, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    if (!targetClose && IsSpellReady(player, 5116))
        return { "hunter concussive shot", "kite or chase control", 5116, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    if (targetClose && IsSpellReady(player, 2974))
        return { "hunter wing clip", "close-range fallback snare", 2974, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    if (!inMelee && IsSpellReady(player, 19434))
        return { "hunter aimed shot", "long cast pressure from range", 19434, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!inMelee && IsSpellReady(player, 2643))
        return { "hunter multi-shot", "ranged burst pressure", 2643, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!inMelee && IsSpellReady(player, 3044))
        return { "hunter arcane shot", "ranged instant pressure", 3044, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    if (player->HealthBelowPct(25) && inMelee && IsSpellReady(player, 5384))
        return { "hunter feign death", "defensive reset under melee pressure", 5384, playerbot::PvpClassSpellContext::TargetMode::Self };

    return decision;
}

SpellDecision SelectMageSpell(Player const* player, Unit const* target, bool inMelee)
{
    SpellDecision decision;
    if (!HasHostileTarget(player, target))
        return decision;

    bool const closePressure = player->IsWithinDistInMap(target, 8.0f);

    if (player->HealthBelowPct(25) && IsSpellReady(player, 11958))
        return { "mage ice block", "self-preservation emergency", 11958, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (closePressure && IsSpellReady(player, 122))
        return { "mage frost nova", "close defensive peel", 122, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (closePressure && IsSpellReady(player, 1953))
        return { "mage blink", "escape melee pressure", 1953, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!closePressure && target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 2139))
        return { "mage counterspell", "interrupt enemy cast at range", 2139, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!target->HasAura(118) && !closePressure && IsSpellReady(player, 118))
        return { "mage polymorph", "safe ranged crowd control", 118, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (closePressure && IsSpellReady(player, 120))
        return { "mage cone of cold", "short-range defensive pressure", 120, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (closePressure && IsSpellReady(player, 2136))
        return { "mage fire blast", "instant close burst", 2136, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 116))
        return { "mage frostbolt", "default ranged pressure", 116, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    return decision;
}

SpellDecision SelectPriestSpell(Player const* player, Unit const* target, Unit const* allyTarget)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (player->HealthBelowPct(45) && !player->HasAura(17) && IsSpellReady(player, 17))
        return { "priest power word shield self", "self-preservation under pressure", 17, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->HealthBelowPct(65) && !player->HasAura(139) && IsSpellReady(player, 139))
        return { "priest renew self", "self-preservation heal over time", 139, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->HealthBelowPct(60) && player->IsInCombat() && IsSpellReady(player, 8122))
        return { "priest psychic scream", "defensive peel while pressured", 8122, playerbot::PvpClassSpellContext::TargetMode::Self };

    if (allyTarget && allyTarget->HealthBelowPct(50) && !allyTarget->HasAura(17) && IsSpellReady(player, 17))
        return { "priest power word shield ally", "lightweight ally preservation", 17, playerbot::PvpClassSpellContext::TargetMode::Ally };
    if (allyTarget && allyTarget->HealthBelowPct(70) && !allyTarget->HasAura(139) && IsSpellReady(player, 139))
        return { "priest renew ally", "lightweight ally heal support", 139, playerbot::PvpClassSpellContext::TargetMode::Ally };

    if (!HasHostileTarget(player, target))
        return decision;

    if (!target->HasAura(589) && IsSpellReady(player, 589))
        return { "priest shadow word pain", "maintain dot pressure", 589, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 8092))
        return { "priest mind blast", "direct shadow burst", 8092, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 585))
        return { "priest smite fallback", "fallback ranged cast", 585, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    return decision;
}

SpellDecision SelectWarlockSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!HasHostileTarget(player, target))
        return decision;

    bool const closePressure = player->IsWithinDistInMap(target, 8.0f);
    if (closePressure && IsSpellReady(player, 5782))
        return { "warlock fear", "close defensive control", 5782, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!target->HasAura(172) && IsSpellReady(player, 172))
        return { "warlock corruption", "maintain corruption dot", 172, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!target->HasAura(980) && IsSpellReady(player, 980))
        return { "warlock curse of agony", "apply curse pressure", 980, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 19647))
        return { "warlock spell lock", "pet interrupt when available", 19647, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 686))
        return { "warlock shadow bolt", "default ranged pressure", 686, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    return decision;
}

SpellDecision SelectWarriorSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!HasHostileTarget(player, target))
        return decision;

    if (!player->IsWithinMeleeRange(target) && IsSpellReady(player, 100))
        return { "warrior charge", "close gap to target", 100, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!target->HasAura(1715) && IsSpellReady(player, 1715))
        return { "warrior hamstring", "maintain stickiness snare", 1715, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!target->HasAura(772) && IsSpellReady(player, 772))
        return { "warrior rend", "bleed pressure baseline", 772, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (target->HealthBelowPct(20) && IsSpellReady(player, 5308))
        return { "warrior execute", "finisher at low enemy health", 5308, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (profileSelection.profile == ClassicClassProfile::PrimaryClassic && IsSpellReady(player, 12294))
        return { "warrior mortal strike", "arms-like burst pressure", 12294, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 7384))
        return { "warrior overpower", "classic melee punish", 7384, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    return decision;
}

SpellDecision SelectRogueSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!HasHostileTarget(player, target))
        return decision;

    if (target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 1766))
        return { "rogue kick", "interrupt enemy cast", 1766, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (player->HealthBelowPct(40) && IsSpellReady(player, 5277))
        return { "rogue evasion", "defensive survival in melee", 5277, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!player->HealthBelowPct(50) && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 2983))
        return { "rogue sprint", "close gap for melee pressure", 2983, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 1776))
        return { "rogue gouge", "short control in melee", 1776, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (player->GetComboPoints() >= 3 && IsSpellReady(player, 2098))
        return { "rogue eviscerate", "combo finisher pressure", 2098, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (player->HasSpell(53) && IsSpellReady(player, 53))
        return { "rogue backstab", "positional melee pressure", 53, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 1752))
        return { "rogue sinister strike", "default combo builder", 1752, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    return decision;
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
            return SelectPriestSpell(player, target, allyTarget);
        case CLASS_WARLOCK:
            return SelectWarlockSpell(player, target);
        case CLASS_WARRIOR:
            return SelectWarriorSpell(player, target, profileSelection);
        case CLASS_ROGUE:
            return SelectRogueSpell(player, target);
        default:
            decision.reason = "class-not-in-this-pass";
            return decision;
    }
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
        { "player has flag", playerbot::PvpCore::IsTriggerActive(playerbot::PvpTrigger::PlayerHasFlag, values), "bg move to objective", 90.0f },
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
void PvpCore::LoadConfig()
{
    g_PvpCoreConfig.moduleEnabled = sConfigMgr->GetBoolDefault("Playerbot.Enable", false);
    g_PvpCoreConfig.pvpCoreEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpCore.Enable", false);
    g_PvpCoreConfig.pvpTacticsEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpTactics.Enable", false);
    g_PvpCoreConfig.pvpLifecycleEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpLifecycle.Enable", false);
    g_PvpCoreConfig.pvpClassSpellsEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpClassSpells.Enable", false);
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
    if (!context.classSpellsEnabled || !player || !values.inBattleground || !IsTriggerActive(PvpTrigger::BgActive, values))
        return context;

    Unit const* target = player->GetVictim();
    bool hasValidTarget = target && target->IsAlive() && target->GetGUID() != player->GetGUID();
    if (!hasValidTarget)
    {
        ObjectGuid const selectedGuid = player->GetSelection();
        if (!selectedGuid.IsEmpty())
        {
            if (Unit const* selectedTarget = ObjectAccessor::GetUnit(*player, selectedGuid))
            {
                target = selectedTarget;
                hasValidTarget = target->IsAlive() && target->GetGUID() != player->GetGUID();
            }
        }
    }

    ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
    Unit const* allyTarget = SelectAllyTarget(player);
    SpellDecision const decision = SelectClassicClassSpell(player, hasValidTarget ? target : nullptr, allyTarget, profileSelection);

    context.actionName = decision.actionName;
    context.reason = decision.reason;
    context.spellId = decision.spellId;
    context.targetMode = decision.targetMode;
    context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
    context.targetGuid = hasValidTarget ? target->GetGUID() : ObjectGuid::Empty;
    context.allyTargetGuid = allyTarget ? allyTarget->GetGUID() : ObjectGuid::Empty;
    if (context.targetMode == PvpClassSpellContext::TargetMode::Ally)
        context.targetGuid = context.allyTargetGuid;
    else if (context.targetMode == PvpClassSpellContext::TargetMode::Self)
        context.targetGuid = player->GetGUID();
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
        profileSelection.unsupportedClass, hasValidTarget, hasValidTarget ? target->GetGUID().ToString() : ObjectGuid::Empty.ToString(),
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

    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierNear, values))
        objective.type = BattlegroundObjectiveType::AttackFlagCarrier;
    else if (IsTriggerActive(PvpTrigger::PlayerHasFlag, values) || IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
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
    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierNear, values))
        return FlagCarrierDirective::AttackEnemyCarrier;

    if (IsTriggerActive(PvpTrigger::PlayerHasFlag, values) || IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
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
