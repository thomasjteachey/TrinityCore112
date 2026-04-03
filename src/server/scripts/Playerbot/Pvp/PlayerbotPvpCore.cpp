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
#include "Configuration/Config.h"
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

bool HasRendFromPlayer(Player const* player, Unit const* target)
{
    if (!player || !target)
        return false;

    constexpr std::array<uint32, 10> rendRanks{ 47465, 47466, 25208, 11574, 11573, 11572, 6547, 6546, 772, 0 };
    for (uint32 spellId : rendRanks)
    {
        if (!spellId)
            continue;

        if (target->HasAura(spellId, player->GetGUID()))
            return true;
    }

    return false;
}

uint32 SelectFirstReadySpell(Player const* player, std::initializer_list<uint32> spellIds)
{
    for (uint32 spellId : spellIds)
        if (spellId && player->HasSpell(spellId) && !player->GetSpellHistory()->HasCooldown(spellId))
            return spellId;

    return 0;
}

bool TargetHasAuraFromPlayer(Unit const* target, Player const* player, std::initializer_list<uint32> spellIds)
{
    if (!target || !player)
        return false;

    for (uint32 spellId : spellIds)
        if (spellId && target->HasAura(spellId, player->GetGUID()))
            return true;

    return false;
}

bool TargetHasShieldImmunityAura(Unit const* target)
{
    return target && (target->HasAura(642) || target->HasAura(45438) || target->HasAura(41450) ||
        target->HasAura(1022) || target->HasAura(5599) || target->HasAura(10278));
}

uint8 CountMeleeAttackers(Player const* player)
{
    if (!player)
        return 0;

    uint8 attackers = 0;
    for (Unit const* attacker : player->getAttackers())
    {
        if (!attacker || !attacker->IsAlive() || attacker->GetVictim() != player)
            continue;

        if (attacker->IsWithinMeleeRange(player))
            ++attackers;
    }

    return attackers;
}

struct SpellDecision
{
    char const* actionName = nullptr;
    uint32 spellId = 0;
    bool selfCast = false;
};

enum class ClassSpecProfile : uint8
{
    Unknown = 0,
    Primary,
    Secondary,
    Tertiary
};

ClassSpecProfile DetectClassSpecProfile(Player const* player)
{
    if (!player)
        return ClassSpecProfile::Unknown;

    uint8 const activeSpec = player->GetActiveSpec();
    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
            if (player->HasTalent(46924, activeSpec)) // bladestorm
                return ClassSpecProfile::Primary;      // Arms
            if (player->HasTalent(23881, activeSpec)) // bloodthirst
                return ClassSpecProfile::Secondary;    // Fury
            if (player->HasTalent(46968, activeSpec)) // shockwave
                return ClassSpecProfile::Tertiary;     // Protection
            break;
        case CLASS_PALADIN:
            if (player->HasTalent(20473, activeSpec)) // holy shock
                return ClassSpecProfile::Primary;      // Holy
            if (player->HasTalent(31935, activeSpec)) // avenger's shield
                return ClassSpecProfile::Secondary;    // Protection
            if (player->HasTalent(53385, activeSpec)) // divine storm
                return ClassSpecProfile::Tertiary;     // Retribution
            break;
        case CLASS_HUNTER:
            if (player->HasTalent(19574, activeSpec)) // bestial wrath
                return ClassSpecProfile::Primary;      // Beast Mastery
            if (player->HasTalent(53209, activeSpec)) // chimera shot
                return ClassSpecProfile::Secondary;    // Marksmanship
            if (player->HasTalent(60053, activeSpec)) // explosive shot
                return ClassSpecProfile::Tertiary;     // Survival
            break;
        case CLASS_ROGUE:
            if (player->HasTalent(48666, activeSpec)) // mutilate
                return ClassSpecProfile::Primary;      // Assassination
            if (player->HasTalent(51690, activeSpec)) // killing spree
                return ClassSpecProfile::Secondary;    // Combat
            if (player->HasTalent(51713, activeSpec)) // shadow dance
                return ClassSpecProfile::Tertiary;     // Subtlety
            break;
        case CLASS_PRIEST:
            if (player->HasTalent(47540, activeSpec)) // penance
                return ClassSpecProfile::Primary;      // Discipline
            if (player->HasTalent(34861, activeSpec)) // circle of healing (rank 1)
                return ClassSpecProfile::Secondary;    // Holy
            if (player->HasTalent(34914, activeSpec)) // vampiric touch (rank 1)
                return ClassSpecProfile::Tertiary;     // Shadow
            break;
        case CLASS_DEATH_KNIGHT:
            if (player->HasTalent(55050, activeSpec)) // heart strike
                return ClassSpecProfile::Primary;      // Blood
            if (player->HasTalent(49184, activeSpec)) // howling blast
                return ClassSpecProfile::Secondary;    // Frost
            if (player->HasTalent(55090, activeSpec)) // scourge strike
                return ClassSpecProfile::Tertiary;     // Unholy
            break;
        case CLASS_SHAMAN:
            if (player->HasTalent(59159, activeSpec)) // thunderstorm
                return ClassSpecProfile::Primary;      // Elemental
            if (player->HasTalent(51533, activeSpec)) // feral spirit
                return ClassSpecProfile::Secondary;    // Enhancement
            if (player->HasTalent(61295, activeSpec)) // riptide (rank 1)
                return ClassSpecProfile::Tertiary;     // Restoration
            break;
        case CLASS_MAGE:
            if (player->HasTalent(44425, activeSpec)) // arcane barrage
                return ClassSpecProfile::Primary;      // Arcane
            if (player->HasTalent(11113, activeSpec)) // blast wave (fire tree marker)
                return ClassSpecProfile::Secondary;    // Fire
            if (player->HasTalent(12472, activeSpec)) // icy veins
                return ClassSpecProfile::Tertiary;     // Frost
            break;
        case CLASS_WARLOCK:
            if (player->HasTalent(48181, activeSpec)) // haunt
                return ClassSpecProfile::Primary;      // Affliction
            if (player->HasTalent(59672, activeSpec)) // metamorphosis
                return ClassSpecProfile::Secondary;    // Demonology
            if (player->HasTalent(59172, activeSpec)) // chaos bolt
                return ClassSpecProfile::Tertiary;     // Destruction
            break;
        case CLASS_DRUID:
            if (player->HasTalent(53201, activeSpec)) // starfall
                return ClassSpecProfile::Primary;      // Balance
            if (player->HasTalent(33891, activeSpec)) // tree of life
                return ClassSpecProfile::Secondary;    // Restoration
            if (player->HasTalent(33876, activeSpec)) // mangle
                return ClassSpecProfile::Tertiary;     // Feral
            break;
        default:
            break;
    }

    return ClassSpecProfile::Unknown;
}

SpellDecision SelectReferenceClassSpell(Player const* player, Unit const* target, bool inMelee)
{
    SpellDecision decision;
    if (!player || !target)
        return decision;

    bool const targetCriticalHealth = target->HealthBelowPct(20);
    bool const rangedWindow = player->IsWithinDistInMap(target, 35.0f) && player->IsWithinLOSInMap(target);
    bool const hasSuddenDeath = player->HasAura(52437);
    bool const hasTasteForBlood = player->HasAura(60503);
    bool const hasVictoryRush = player->HasAura(32216);
    bool const hasBattleShout = player->HasAura(6673);
    bool const inBattleStance = player->HasAura(2457);
    bool const hasRendAura = HasRendFromPlayer(player, target);
    bool const targetAlreadySlowed = target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
    bool const highRageAvailable = player->GetPower(POWER_RAGE) >= 600;
    bool const lowRageAvailable = player->GetPower(POWER_RAGE) < 200;
    bool const enemyOutOfMelee = !inMelee;
    bool const enemyInChargeReach = player->IsWithinDistInMap(target, 25.0f) && player->IsWithinLOSInMap(target);
    bool const mediumHealth = player->HealthBelowPct(60);
    bool const almostFullHealth = player->GetHealthPct() >= 85.0f;
    ClassSpecProfile const specProfile = DetectClassSpecProfile(player);
    auto tryAction = [&](char const* actionName, std::initializer_list<uint32> spellIds, bool selfCast = false) -> bool
    {
        if (uint32 spellId = SelectFirstReadySpell(player, spellIds))
        {
            decision.actionName = actionName;
            decision.spellId = spellId;
            decision.selfCast = selfCast;
            return true;
        }
        return false;
    };

    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
            if (enemyOutOfMelee && enemyInChargeReach && tryAction("charge", { 11578, 11577, 100 }))
                return decision;
            if (!inBattleStance && tryAction("battle stance", { 2457 }, true))
                return decision;
            if (!hasBattleShout && tryAction("battle shout", { 47436, 47435, 6673 }, true))
                return decision;
            if (!hasRendAura && inMelee && tryAction("rend", { 47465, 47466, 25208, 772 }))
                return decision;
            if ((targetCriticalHealth || hasSuddenDeath) && inMelee && tryAction("execute", { 47471, 25236, 5308 }))
                return decision;
            if (hasTasteForBlood && inMelee && tryAction("overpower", { 7384 }))
                return decision;
            if (hasVictoryRush && inMelee && tryAction("victory rush", { 34428 }))
                return decision;
            if (inMelee && specProfile == ClassSpecProfile::Primary && tryAction("mortal strike", { 47486, 47498 }))
                return decision;
            if (inMelee && specProfile == ClassSpecProfile::Secondary && tryAction("bloodthirst", { 47450, 23881 }))
                return decision;
            if (inMelee && specProfile == ClassSpecProfile::Tertiary && tryAction("shield slam", { 47488, 23922 }))
                return decision;
            if (inMelee && tryAction("melee pressure", { 47486, 47485, 12294, 23881, 47498 }))
                return decision;
            if (!targetAlreadySlowed && inMelee && tryAction("hamstring", { 1715 }))
                return decision; // reference fallback chain: piercing howl -> mocking blow -> hamstring
            if (highRageAvailable && inMelee && tryAction("slam", { 47475, 47474, 25242, 1464 }))
                return decision;
            if (highRageAvailable && inMelee && tryAction("heroic strike", { 47450, 47449, 78 }))
                return decision;
            if (lowRageAvailable && tryAction("bloodrage", { 2687 }, true))
                return decision;
            if (tryAction("death wish", { 12292 }, true))
                return decision;
            if (mediumHealth && tryAction("enraged regeneration", { 55694 }, true))
                return decision;
            if (almostFullHealth && CountMeleeAttackers(player) >= 2 && tryAction("retaliation", { 20230 }, true))
                return decision;
            if (TargetHasShieldImmunityAura(target) && player->IsWithinDistInMap(target, 30.0f) && player->IsWithinLOSInMap(target) &&
                tryAction("shattering throw", { 64382 }))
                return decision;
            break;
        case CLASS_PALADIN:
            if (targetCriticalHealth && tryAction("hammer of wrath", { 48806, 24275 }))
                return decision;
            if (inMelee && specProfile == ClassSpecProfile::Tertiary && tryAction("retribution burst", { 35395, 53385, 53408, 20271 }))
                return decision;
            if (inMelee && specProfile == ClassSpecProfile::Secondary && tryAction("protection burst", { 48827, 53595, 48819 }))
                return decision;
            if (inMelee && tryAction("melee burst", { 35395, 53385, 53408, 20271, 48819 }))
                return decision;
            if (rangedWindow && tryAction("ranged pressure", { 48801, 48806 }))
                return decision;
            if (tryAction("avenging wrath", { 31884 }, true))
                return decision;
            break;
        case CLASS_HUNTER:
            if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 49001, 13555, 13554, 1978 }))
                    if (tryAction("serpent sting", { 49001, 13555, 1978 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Primary &&
                    tryAction("beast mastery shots", { 49045, 49052, 53351, 49001 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Secondary && tryAction("marksmanship shots", { 53209, 19434, 53351, 49045 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Tertiary && tryAction("survival shots", { 60053, 53351, 49052, 49045 }))
                    return decision;
                if (tryAction("ranged shot priority", { 53351, 53209, 60053, 19434, 49045, 49052, 49001 }))
                    return decision;
            }
            if (tryAction("bestial wrath", { 19574 }, true))
                return decision;
            break;
        case CLASS_ROGUE:
            if (inMelee)
            {
                if (player->GetComboPoints() >= 4)
                    if (tryAction("finisher", { 57993, 48668 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Primary && tryAction("assassination builders", { 48666, 57993, 48668 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Secondary && tryAction("combat builders", { 48638, 48668, 48657 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Tertiary && tryAction("subtlety builders", { 48657, 48638, 48668 }))
                    return decision;
                if (tryAction("builder chain", { 48666, 48638, 57993, 48668, 48657 }))
                    return decision;
            }
            if (tryAction("killing spree", { 51690 }, true))
                return decision;
            break;
        case CLASS_PRIEST:
            if (rangedWindow)
            {
                if (specProfile != ClassSpecProfile::Tertiary && tryAction("discipline-holy pressure", { 48123, 48127, 48066 }))
                    return decision;
                if (!TargetHasAuraFromPlayer(target, player, { 48125, 25368, 10894, 589 }))
                    if (tryAction("shadow word: pain", { 48125, 25368, 589 }))
                        return decision;
                if (!TargetHasAuraFromPlayer(target, player, { 48300, 2944 }))
                    if (tryAction("devouring plague", { 48300, 2944 }))
                        return decision;
                if (tryAction("shadow nuke chain", { 48127, 48156, 48158, 48123 }))
                    return decision;
            }
            break;
        case CLASS_DEATH_KNIGHT:
            if (inMelee && specProfile == ClassSpecProfile::Primary && tryAction("blood strike chain", { 55050, 49924, 55262 }))
                return decision;
            if (inMelee && specProfile == ClassSpecProfile::Secondary && tryAction("frost strike chain", { 55268, 51425, 49184 }))
                return decision;
            if (inMelee && specProfile == ClassSpecProfile::Tertiary && tryAction("unholy strike chain", { 55090, 49921, 45477 }))
                return decision;
            if (inMelee && tryAction("melee rune strike chain", { 55268, 51425, 55090, 49924, 55050, 49921, 45477 }))
                return decision;
            if (rangedWindow && tryAction("death coil pressure", { 49895, 47632 }))
                return decision;
            break;
        case CLASS_SHAMAN:
            if (inMelee)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 49233, 8050 }))
                    if (tryAction("flame shock", { 49233, 8050 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Secondary && tryAction("enhancement chain", { 17364, 60103, 49231 }))
                    return decision;
                if (tryAction("melee chain", { 17364, 60103, 49231 }))
                    return decision;
            }
            else if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 49233, 8050 }))
                    if (tryAction("flame shock", { 49233, 8050 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Primary && tryAction("elemental chain", { 60043, 49238, 49271 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Tertiary && tryAction("resto pressure", { 49238, 49271, 49233 }))
                    return decision;
                if (tryAction("caster chain", { 60043, 49238, 49271 }))
                    return decision;
            }
            if (tryAction("feral spirit", { 51533 }, true))
                return decision;
            break;
        case CLASS_MAGE:
            if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 42891, 12654 }))
                    if (tryAction("living bomb", { 42891, 12654 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Primary && tryAction("arcane chain", { 42897, 44781, 42846, 42873 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Secondary && tryAction("fire chain", { 42833, 42891, 42873 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Tertiary && tryAction("frost chain", { 42842, 42914, 42833 }))
                    return decision;
                if (tryAction("caster chain", { 42897, 42842, 42833, 42846, 42914, 42873 }))
                    return decision;
            }
            if (tryAction("burst cooldown", { 12042, 12472 }, true))
                return decision;
            break;
        case CLASS_WARLOCK:
            if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 47813, 172 }))
                    if (tryAction("corruption", { 47813, 172 }))
                        return decision;
                if (!TargetHasAuraFromPlayer(target, player, { 47811, 348 }))
                    if (tryAction("immolate", { 47811, 348 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Primary && tryAction("affliction chain", { 48181, 47843, 47813, 47809 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Secondary && tryAction("demonology chain", { 59672, 47809, 47813, 59164 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Tertiary && tryAction("destruction chain", { 59172, 17962, 47838, 47809 }))
                    return decision;
                if (tryAction("warlock chain", { 48181, 47843, 59172, 17962, 47838, 47809 }))
                    return decision;
            }
            break;
        case CLASS_DRUID:
            if (inMelee)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 48574, 1822 }))
                    if (tryAction("rake", { 48574, 1822 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Tertiary && tryAction("feral chain", { 48566, 48572, 48574 }))
                    return decision;
                if (tryAction("melee chain", { 48566, 48572, 48574 }))
                    return decision;
            }
            else if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 48463, 8921 }))
                    if (tryAction("moonfire", { 48463, 8921 }))
                        return decision;
                if (specProfile == ClassSpecProfile::Primary && tryAction("balance chain", { 48461, 48465, 48463 }))
                    return decision;
                if (specProfile == ClassSpecProfile::Secondary && tryAction("restoration pressure", { 48461, 48463, 8921 }))
                    return decision;
                if (tryAction("caster chain", { 48461, 48465, 48463 }))
                    return decision;
            }
            if (tryAction("burst cooldown", { 53201, 17116 }, true))
                return decision;
            break;
        default:
            break;
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
        case PvpTrigger::PlayerHasFlag:
        case PvpTrigger::EnemyFlagCarrierNear:
        case PvpTrigger::TeamFlagCarrierNear:
            return false;
        default:
            break;
    }

    return false;
}

BattlegroundTacticalContext PvpCore::BuildBattlegroundTacticalContext(Player const* player, PvpValues const& values)
{
    BattlegroundTacticalContext context;
    context.tacticsEnabled = g_PvpCoreConfig.moduleEnabled && g_PvpCoreConfig.pvpCoreEnabled && g_PvpCoreConfig.pvpTacticsEnabled;
    if (!context.tacticsEnabled || !player || !IsTriggerActive(PvpTrigger::BgActive, values))
        return context;

    context.shouldEvaluate = true;
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
    if (!target || !target->IsAlive() || target->GetGUID() == player->GetGUID())
        return context;

    bool const inMelee = player->IsWithinMeleeRange(target);

    SpellDecision const decision = SelectReferenceClassSpell(player, target, inMelee);
    context.actionName = decision.actionName;
    context.spellId = decision.spellId;
    context.selfCast = decision.selfCast;
    context.shouldExecute = context.spellId != 0;
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
            return BattlegroundMovementPrimitive::None;
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
