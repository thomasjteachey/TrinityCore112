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

uint32 SelectReferenceClassSpell(Player const* player, Unit const* target, bool inMelee)
{
    if (!player || !target)
        return 0;

    bool const targetCriticalHealth = target->HealthBelowPct(20);
    bool const rangedWindow = player->IsWithinDistInMap(target, 35.0f) && player->IsWithinLOSInMap(target);
    bool const hasSuddenDeath = player->HasAura(52437);
    bool const tasteForBlood = player->HasAura(60503);
    bool const hasBattleShout = player->HasAura(6673);
    bool const inBattleStance = player->HasAura(2457);
    bool const hasRendAura = HasRendFromPlayer(player, target);
    bool const targetAlreadySlowed = target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED);
    bool const highRageAvailable = player->GetPower(POWER_RAGE) >= 600;
    bool const lowRageAvailable = player->GetPower(POWER_RAGE) < 200;
    bool const enemyOutOfMelee = !inMelee;
    bool const enemyInChargeReach = player->IsWithinDistInMap(target, 25.0f) && player->IsWithinLOSInMap(target);

    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
            if (enemyOutOfMelee && enemyInChargeReach)
                if (uint32 spellId = SelectFirstReadySpell(player, { 11578, 11577, 100 }))
                    return spellId; // charge
            if (!inBattleStance)
                if (uint32 spellId = SelectFirstReadySpell(player, { 2457 }))
                    return spellId; // battle stance
            if (!hasBattleShout)
                if (uint32 spellId = SelectFirstReadySpell(player, { 47436, 47435, 6673 }))
                    return spellId; // battle shout
            if (!hasRendAura && inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 47465, 47466, 25208, 772 }))
                    return spellId; // rend
            if ((targetCriticalHealth || hasSuddenDeath) && inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 47471, 25236, 5308 }))
                    return spellId; // execute
            if (tasteForBlood && inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 7384 }))
                    return spellId; // overpower
            if (inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 47486, 47485, 12294, 23881, 47498 }))
                    return spellId; // mortal strike / bloodthirst / devestate fallback
            if (lowRageAvailable)
                if (uint32 spellId = SelectFirstReadySpell(player, { 2687 }))
                    return spellId; // bloodrage
            if (uint32 spellId = SelectFirstReadySpell(player, { 12292 }))
                return spellId; // death wish
            if (!targetAlreadySlowed && inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 1715 }))
                    return spellId; // hamstring
            if (highRageAvailable && inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 47450, 47449, 78 }))
                    return spellId; // heroic strike
            break;
        case CLASS_PALADIN:
            if (targetCriticalHealth)
                if (uint32 spellId = SelectFirstReadySpell(player, { 48806, 24275 }))
                    return spellId; // hammer of wrath
            if (inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 35395, 53385, 53408, 20271, 48819 }))
                    return spellId; // crusader strike/divine storm/judgement/consecration
            if (rangedWindow)
                if (uint32 spellId = SelectFirstReadySpell(player, { 48801, 48806 }))
                    return spellId; // exorcism/hammer of wrath
            if (uint32 spellId = SelectFirstReadySpell(player, { 31884 }))
                return spellId; // avenging wrath
            break;
        case CLASS_HUNTER:
            if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 49001, 13555, 13554, 1978 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 49001, 13555, 1978 }))
                        return spellId; // serpent sting
                if (uint32 spellId = SelectFirstReadySpell(player, { 53351, 53209, 60053, 19434, 49045, 49052, 49001 }))
                    return spellId; // kill shot/chimera/explosive/aimed/arcane/steady/serpent sting
            }
            if (uint32 spellId = SelectFirstReadySpell(player, { 19574 }))
                return spellId; // bestial wrath
            break;
        case CLASS_ROGUE:
            if (inMelee)
            {
                if (player->GetComboPoints() >= 4)
                    if (uint32 spellId = SelectFirstReadySpell(player, { 57993, 48668 }))
                        return spellId; // envenom/eviscerate
                if (uint32 spellId = SelectFirstReadySpell(player, { 48666, 48638, 57993, 48668, 48657 }))
                    return spellId; // mutilate/sinister strike/envenom/eviscerate/backstab
            }
            if (uint32 spellId = SelectFirstReadySpell(player, { 51690 }))
                return spellId; // killing spree
            break;
        case CLASS_PRIEST:
            if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 48125, 25368, 10894, 589 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 48125, 25368, 589 }))
                        return spellId; // shadow word: pain
                if (!TargetHasAuraFromPlayer(target, player, { 48300, 2944 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 48300, 2944 }))
                        return spellId; // devouring plague
                if (uint32 spellId = SelectFirstReadySpell(player, { 48127, 48156, 48158, 48123 }))
                    return spellId; // mind blast/mind flay/shadow word: death/smite
            }
            break;
        case CLASS_DEATH_KNIGHT:
            if (inMelee)
                if (uint32 spellId = SelectFirstReadySpell(player, { 55268, 51425, 55090, 49924, 55050, 49921, 45477 }))
                    return spellId; // obliterate/frost strike/scourge strike/heart strike/death+plague+icy touch
            if (rangedWindow)
                if (uint32 spellId = SelectFirstReadySpell(player, { 49895, 47632 }))
                    return spellId; // death coil/rune strike fallback
            break;
        case CLASS_SHAMAN:
            if (inMelee)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 49233, 8050 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 49233, 8050 }))
                        return spellId; // flame shock
                if (uint32 spellId = SelectFirstReadySpell(player, { 17364, 60103, 49231 }))
                    return spellId; // stormstrike/lava lash/earth shock
            }
            else if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 49233, 8050 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 49233, 8050 }))
                        return spellId; // flame shock
                if (uint32 spellId = SelectFirstReadySpell(player, { 60043, 49238, 49271 }))
                    return spellId; // lava burst/lightning bolt/chain lightning
            }
            if (uint32 spellId = SelectFirstReadySpell(player, { 51533 }))
                return spellId; // feral spirit
            break;
        case CLASS_MAGE:
            if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 42891, 12654 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 42891, 12654 }))
                        return spellId; // living bomb / ignite-style maintenance
                if (uint32 spellId = SelectFirstReadySpell(player, { 42897, 42842, 42833, 42846, 42914, 42873 }))
                    return spellId; // arcane blast/frostbolt/fireball/arcane missiles/ice lance/fire blast
            }
            if (uint32 spellId = SelectFirstReadySpell(player, { 12042, 12472 }))
                return spellId; // arcane power/icy veins
            break;
        case CLASS_WARLOCK:
            if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 47813, 172 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 47813, 172 }))
                        return spellId; // corruption
                if (!TargetHasAuraFromPlayer(target, player, { 47811, 348 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 47811, 348 }))
                        return spellId; // immolate
                if (uint32 spellId = SelectFirstReadySpell(player, { 48181, 47843, 59172, 17962, 47838, 47809 }))
                    return spellId; // immolate/conflagrate/chaos bolt/incinerate/corruption/UA/haunt/shadow bolt
            }
            break;
        case CLASS_DRUID:
            if (inMelee)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 48574, 1822 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 48574, 1822 }))
                        return spellId; // rake
                if (uint32 spellId = SelectFirstReadySpell(player, { 48566, 48572, 48574 }))
                    return spellId; // mangle(cat)/shred/rake
            }
            else if (rangedWindow)
            {
                if (!TargetHasAuraFromPlayer(target, player, { 48463, 8921 }))
                    if (uint32 spellId = SelectFirstReadySpell(player, { 48463, 8921 }))
                        return spellId; // moonfire
                if (uint32 spellId = SelectFirstReadySpell(player, { 48461, 48465, 48463 }))
                    return spellId; // wrath/starfire/moonfire
            }
            if (uint32 spellId = SelectFirstReadySpell(player, { 53201, 17116 }))
                return spellId; // starfall/nature's swiftness style pressure
            break;
        default:
            break;
    }

    return 0;
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

    context.spellId = SelectReferenceClassSpell(player, target, inMelee);
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
