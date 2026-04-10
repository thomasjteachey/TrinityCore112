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
#include "Player.h"
#include "Pet.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Unit.h"
#include "Util.h"

#include <array>
#include <cmath>
#include <vector>

namespace
{
playerbot::PvpCoreConfig g_PvpCoreConfig;
bool HasHostileTarget(Player const* player, Unit const* target);

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

constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT = 22734;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK = 29073;
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

        return spellId;
    }

    return 0;
}

SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player)
{
    SpellDecision decision;
    if (!player || !player->IsAlive() || player->IsInCombat() || player->IsMounted())
        return decision;

    bool const needsFood = player->GetHealthPct() < 100.0f;
    bool const usesMana = player->GetMaxPower(POWER_MANA) > 0;
    bool const needsDrink = usesMana && player->GetPowerPct(POWER_MANA) < 100.0f;
    bool const hasEatAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
    bool const hasDrinkAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

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
    if (inBattlegroundPreparation)
        return decision;

    if (!player->IsOutdoors())
        return decision;

    // Avoid mounting if a hostile enemy player is already close enough to engage.
    // This keeps bots on-foot so their combat spell selection can start immediately.
    if (player->GetMap())
    {
        Map::PlayerList const& mapPlayers = player->GetMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, 35.0f))
                continue;

            return decision;
        }
    }

    if (IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT))
        return { "mount", "mount while outside and out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT, playerbot::PvpClassSpellContext::TargetMode::Self };

    if (uint32 const knownMountSpellId = SelectReadyKnownMountSpell(player))
        return { "mount", "mount while outside and out of combat", knownMountSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };

    return decision;
}

bool HasHostileTarget(Player const* player, Unit const* target)
{
    return player && target && target->IsAlive() && target->GetGUID() != player->GetGUID() && player->IsValidAttackTarget(target);
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

bool HasTemporaryWeaponImbue(Player const* player)
{
    if (!player)
        return false;

    Item* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
    Item* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
    return (mainHand && mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)) ||
        (offHand && offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT));
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

    Unit const* target = player->GetVictim();
    if (isTargetUsable(target))
        return target;

    target = player->GetSelectedUnit();
    if (isTargetUsable(target))
        return target;

    return SelectClosestEnemyTarget(player, true);
}

Unit const* SelectAllyTarget(Player const* player)
{
    if (!player)
        return nullptr;

    Unit const* selected = player->GetSelectedUnit();
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
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    Unit const* activeTarget = SelectCombatTarget(player);
    if (!HasHostileTarget(player, activeTarget))
        return decision;

    Pet const* pet = player->GetPet();
    bool const hasLivingPet = pet && pet->IsAlive();
    bool const hasDeadPet = pet && !pet->IsAlive();

    Unit const* enemyOnTopTarget = SelectNearbyEnemyTarget(player, activeTarget, 5.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 20.0f, activeTarget);
    Unit const* closeMeleeThreat = SelectNearbyMeleeTarget(player, enemyOnTopTarget, 5.0f);
    Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, 35.0f);
    Unit const* manaTarget = SelectNearbyEnemyTarget(player, activeTarget, 35.0f);

    target = activeTarget;
    bool const targetClose = player->IsWithinDistInMap(target, 8.0f);
    bool const enemyOnTop = HasHostileTarget(player, enemyOnTopTarget);
    bool const enemyNear = player->IsWithinDistInMap(target, 15.0f);

    if (rogueTarget && !HasAuraFromSpellChain(rogueTarget, 14325) && IsSpellReady(player, 14325))
        return { "hunter mark", "mark rogue targets for anti-stealth pressure", 14325, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget->GetGUID() };
    if (rogueTarget && !HasAuraFromSpellChain(rogueTarget, 25295) && IsSpellReady(player, 25295))
        return { "hunter serpent sting", "apply ranged dot pressure", 25295, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget->GetGUID() };

    bool const targetSnaredOrStunned = target &&
        (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
         target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN)));

    if (!targetClose && !targetSnaredOrStunned && IsSpellReady(player, 5116))
        return { "hunter concussive shot", "kite or chase control", 5116, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    if (enemyOnTop && IsSpellReady(player, 14268) && !HasAuraFromSpellChain(enemyOnTopTarget, 14268))
        return { "hunter wing clip", "close-range fallback snare", 14268, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget->GetGUID() };
    if (enemyOnTop && HasAuraFromSpellChain(enemyOnTopTarget, 14268) && IsSpellReady(player, 5384) && IsSpellReady(player, 14311))
        return { "hunter feign death", "set up freezing trap while pressured in melee", 5384, playerbot::PvpClassSpellContext::TargetMode::Self, enemyOnTopTarget->GetGUID() };
    if (enemyOnTop && enemyOnTopTarget->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 19503))
        return { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget->GetGUID() };
    if (nearbyCastingTarget && IsSpellReady(player, 19503))
        return { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget->GetGUID() };
    if (enemyOnTop && (!IsSpellReady(player, 5384) || !IsSpellReady(player, 14311)) && IsSpellReady(player, 19503) && !HasBreakableCrowdControl(enemyOnTopTarget))
        return { "hunter scatter shot", "fallback peel when trap setup unavailable", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget->GetGUID() };
    if (enemyOnTop && closeMeleeThreat && !IsSpellReady(player, 19503) && (!IsSpellReady(player, 5384) || !IsSpellReady(player, 14311)) && IsSpellReady(player, 19263))
        return { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self };

    if (!enemyNear && IsSpellReady(player, 20904))
        return { "hunter aimed shot", "long cast pressure from range", 20904, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!inMelee && IsSpellReady(player, 3045))
        return { "hunter rapid fire", "burst cooldown while freecasting at range", 3045, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!inMelee && IsSpellReady(player, 25294))
        return { "hunter multi-shot", "ranged burst pressure", 25294, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (manaTarget && manaTarget->GetPowerType() == POWER_MANA && !HasAuraFromSpellChain(manaTarget, 14280) && IsSpellReady(player, 14280))
        return { "hunter viper sting", "drain mana on mana users", 14280, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaTarget->GetGUID() };
    if (!HasAuraFromSpellChain(player, 20906) && IsSpellReady(player, 20906))
        return { "hunter trueshot aura", "maintain personal buff aura", 20906, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!hasLivingPet && !hasDeadPet && IsSpellReady(player, 883))
        return { "hunter call pet", "summon active stable pet when no pet is present", 883, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (hasDeadPet && !player->IsInCombat() && IsSpellReady(player, 982))
        return { "hunter revive pet", "recover pet out of combat", 982, playerbot::PvpClassSpellContext::TargetMode::Self };

    return decision;
}

SpellDecision SelectMageSpell(Player const* player, Unit const* target, bool inMelee)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const closePressure = hasHostileTarget && player->IsWithinDistInMap(target, 8.0f);
    float const manaPct = player->GetPowerPct(POWER_MANA);

    if (player->HealthBelowPct(25) && IsSpellReady(player, 11958))
        return { "mage ice block", "self-preservation emergency", 11958, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (manaPct < 25.0f && IsSpellReady(player, 12051))
        return { "mage evocation", "recover mana below 25 percent", 12051, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (manaPct < 50.0f && player->HasItemCount(8008))
        return { "use mana ruby", "consume mana ruby below 50 percent mana", 22044, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), 8008 };
    if (IsSpellReady(player, 475))
        if (Unit const* cursedTarget = SelectFriendlyCurseTarget(player, 40.0f))
            return { "remove lesser curse", "dispel curse from friendly target", 475, cursedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget->GetGUID() };
    if (player->HasUnitState(UNIT_STATE_STUNNED) && IsSpellReady(player, 1953))
        return { "mage blink", "break stun pressure when possible", 1953, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!IsSpellReady(player, 11958) && IsSpellReady(player, 12472))
        return { "mage cold snap", "reset frost defenses when ice block unavailable", 12472, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (closePressure && IsSpellReady(player, 10230))
        return { "mage frost nova", "close defensive peel", 10230, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (closePressure && IsMeleeClass(target) && IsSpellReady(player, 10161))
        return { "mage cone of cold", "defensive snare versus nearby melee", 10161, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (closePressure && IsSpellReady(player, 1953))
        return { "mage blink", "escape melee pressure", 1953, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 2139))
        if (Unit const* castingTarget = SelectEnemyCastingTarget(player, 30.0f, target))
            return { "mage counterspell", "interrupt any enemy cast in range", 2139, playerbot::PvpClassSpellContext::TargetMode::Enemy, castingTarget->GetGUID() };
    if (!HasAuraFromSpellChain(player, 13033) && IsSpellReady(player, 13033))
        return { "mage ice barrier", "maintain defensive absorb shield", 13033, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (hasHostileTarget && target->HealthBelowPct(20) && IsSpellReady(player, 10199))
        return { "mage fire blast", "instant execute pressure on low health target", 10199, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 12826) && !AnyEnemyPolymorphed(player, 40.0f))
        if (Unit const* polymorphTarget = SelectPolymorphTarget(player, target, 30.0f))
            return { "mage polymorph", "priority crowd control on non-dotted paladin/priest targets", 12826, playerbot::PvpClassSpellContext::TargetMode::Enemy, polymorphTarget->GetGUID() };
    if (hasHostileTarget && IsSpellReady(player, 25304))
        return { "mage frostbolt", "default ranged pressure", 25304, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    if (!player->IsInCombat() && IsSpellReady(player, 10157) && !player->HasAura(10157))
        return { "arcane intellect", "arcane intellect", 10157, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!player->IsInCombat() && IsSpellReady(player, 10220) && !player->HasAura(10220))
        return { "frost armor", "frost armor", 10220, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 10054) && !player->HasItemCount(8008))
        return { "create mana ruby", "create mana ruby", 10054, playerbot::PvpClassSpellContext::TargetMode::Self };

    return decision;
}

SpellDecision SelectPriestSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    if (profileSelection.profile == ClassicClassProfile::PrimaryClassic)
    {
        if (IsSpellReady(player, 988))
            if (Unit const* debuffedAlly = SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f))
                return { "priest dispel magic ally", "prioritize dispelling magic debuffs from allies", 988, debuffedAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, debuffedAlly->GetGUID() };
        if (IsSpellReady(player, 988))
            if (Unit const* enemyBuffedTarget = SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f))
                return { "priest dispel magic enemy", "prioritize dispelling magic buffs from enemies", 988, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyBuffedTarget->GetGUID() };

        Unit const* shieldTarget = SelectFriendlyHealthTarget(player, 40.0f, 50.0f);
        if (shieldTarget && !HasAuraFromSpellChain(shieldTarget, 10901) && IsSpellReady(player, 10901))
            return { "priest power word shield ally", "protect ally below 50 percent health", 10901, shieldTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, shieldTarget->GetGUID() };
        if (player->IsInCombat() && IsSpellReady(player, 10060))
            if (Unit const* casterAlly = SelectFriendlyHealthTarget(player, 40.0f, 100.0f))
                if (casterAlly->GetPowerType() == POWER_MANA)
                    return { "priest power infusion", "boost nearby caster throughput in combat", 10060, casterAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, casterAlly->GetGUID() };
        if (!player->IsInCombat() && !player->HasAura(10938) && IsSpellReady(player, 10938))
            return { "priest power word fortitude", "maintain fortitude out of combat", 10938, playerbot::PvpClassSpellContext::TargetMode::Self };
        if (!player->IsInCombat() && !HasAuraFromSpellChain(player, 10958) && IsSpellReady(player, 10958))
            return { "priest shadow protection", "maintain shadow protection out of combat", 10958, playerbot::PvpClassSpellContext::TargetMode::Self };
        if (!player->IsInCombat() && !HasAuraFromSpellChain(player, 1006) && IsSpellReady(player, 1006))
            return { "priest inner fire", "maintain inner fire out of combat", 1006, playerbot::PvpClassSpellContext::TargetMode::Self };
        if (IsSpellReady(player, 10929))
            if (Unit const* renewTarget = SelectFriendlyHealthTarget(player, 40.0f, 80.0f))
                if (!HasAuraFromSpellChain(renewTarget, 10929))
                    return { "priest renew", "maintain renew on moderately injured allies", 10929, renewTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, renewTarget->GetGUID() };
        if (IsSpellReady(player, 10917))
            if (Unit const* healTarget = SelectFriendlyHealthTarget(player, 40.0f, 85.0f))
                return { "priest flash heal", "heal party with flash heal", 10917, healTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, healTarget->GetGUID() };
    }

    if (!HasHostileTarget(player, target))
        return decision;

    if (target->GetClass() == CLASS_ROGUE && !HasAuraFromSpellChain(target, 27605) && IsSpellReady(player, 27605))
        return { "priest shadow word pain", "maintain dot pressure on rogues", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (target->GetPowerType() == POWER_MANA && IsSpellReady(player, 14033))
        return { "priest mana burn", "burn mana from enemy casters", 14033, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (CountNearbyEnemies(player, 10.0f) >= 2 && CountNearbyFriendlyPlayers(player, 10.0f) >= 2 && IsSpellReady(player, 27801))
        return { "priest holy nova", "aoe pressure and splash healing in melee cluster", 27801, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 27605))
    {
        if (!HasBreakableCrowdControl(target) && !HasAuraFromSpellChain(target, 27605))
            return { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled or open targets", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy };
        if (Unit const* controlledTarget = SelectEnemyNonBreakableCrowdControlTarget(player, 30.0f))
            if (!HasAuraFromSpellChain(controlledTarget, 27605))
                return { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled targets", 27605, playerbot::PvpClassSpellContext::TargetMode::Enemy, controlledTarget->GetGUID() };
    }
    if (IsSpellReady(player, 10917) && player->HealthBelowPct(85))
        return { "priest flash heal", "fallback self-healing while under pressure", 10917, playerbot::PvpClassSpellContext::TargetMode::Self };

    return decision;
}

SpellDecision SelectDruidSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    if (IsSpellReady(player, 29166))
        if (Unit const* lowManaAlly = SelectFriendlyLowManaTarget(player, 40.0f, 10.0f))
            if (!lowManaAlly->HasAura(29166))
                return { "druid innervate", "stabilize low-mana ally with innervate", 29166, lowManaAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowManaAlly->GetGUID() };
    if (IsSpellReady(player, 2782))
        if (Unit const* cursedTarget = SelectFriendlyDispelTarget(player, DISPEL_CURSE, 40.0f))
            return { "druid remove curse", "remove curses from allies", 2782, cursedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget->GetGUID() };
    if (IsSpellReady(player, 2893))
        if (Unit const* poisonedTarget = SelectFriendlyDispelTarget(player, DISPEL_POISON, 40.0f))
            return { "druid abolish poison", "remove poison pressure from allies", 2893, poisonedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, poisonedTarget->GetGUID() };
    if (IsSpellReady(player, 18562))
        if (Unit const* swiftmendTarget = SelectFriendlyHealthTarget(player, 40.0f, 50.0f))
            if (HasAuraFromSpellChain(swiftmendTarget, 9858) || HasAuraFromSpellChain(swiftmendTarget, 25299))
                return { "druid swiftmend", "consume hot for emergency heal under 50 percent", 18562, swiftmendTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, swiftmendTarget->GetGUID() };
    if (IsSpellReady(player, 17116) && IsSpellReady(player, 25297))
        if (Unit const* emergencyTarget = SelectFriendlyHealthTarget(player, 40.0f, 25.0f))
            return { "druid natures swiftness", "prepare instant healing touch for critical ally", 17116, playerbot::PvpClassSpellContext::TargetMode::Self, emergencyTarget->GetGUID() };
    if (player->HasAura(17116) && IsSpellReady(player, 25297))
        if (Unit const* emergencyTarget = SelectFriendlyHealthTarget(player, 40.0f, 50.0f))
            return { "druid healing touch", "consume natures swiftness with healing touch", 25297, emergencyTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyTarget->GetGUID() };
    if (IsSpellReady(player, 9858))
        if (Unit const* regrowthTarget = SelectFriendlyHealthTarget(player, 40.0f, 85.0f))
            if (!HasAuraFromSpellChain(regrowthTarget, 9858))
                return { "druid regrowth", "maintain regrowth on injured allies", 9858, regrowthTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, regrowthTarget->GetGUID() };
    if (IsSpellReady(player, 25299))
        if (Unit const* rejuvTarget = SelectFriendlyHealthTarget(player, 40.0f, 90.0f))
            if (!HasAuraFromSpellChain(rejuvTarget, 25299))
                return { "druid rejuvenation", "maintain rejuvenation on injured allies", 25299, rejuvTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, rejuvTarget->GetGUID() };

    if (Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, 30.0f))
        if (!HasAuraFromSpellChain(rogueTarget, 9907) && IsSpellReady(player, 9907))
            return { "druid faerie fire", "apply faerie fire to nearby rogues", 9907, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget->GetGUID() };
    if (Unit const* meleeThreat = SelectNearbyMeleeTarget(player, target, 8.0f))
        if (IsSpellReady(player, 5487))
            return { "druid bear form", "swap to bear under physical melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self, meleeThreat->GetGUID() };
    if (player->HasAura(5487) && IsSpellReady(player, 16979))
        if (Unit const* meleeThreat = SelectNearbyMeleeTarget(player, target, 8.0f))
            return { "druid feral charge", "charge away from melee pressure in bear form", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy, meleeThreat->GetGUID() };

    return decision;
}

SpellDecision SelectPaladinSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    if (player->HealthBelowPct(20) && IsSpellReady(player, 1020))
        return { "paladin divine shield", "emergency immunity under lethal pressure", 1020, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!player->HasAura(19746) && IsSpellReady(player, 19746))
        return { "paladin concentration aura", "maintain concentration aura", 19746, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!player->IsInCombat() && !player->HasAura(25898) && IsSpellReady(player, 25898))
        return { "paladin greater blessing of kings", "maintain kings out of combat", 25898, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 4987))
        if (Unit const* cleanseTarget = SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f))
            return { "paladin cleanse", "prioritize cleansing allies", 4987, cleanseTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cleanseTarget->GetGUID() };
    if (IsSpellReady(player, 1044))
        if (Unit const* freedomTarget = SelectFriendlySnaredTarget(player, 40.0f))
            return { "paladin hand of freedom", "free snared or rooted ally", 1044, freedomTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, freedomTarget->GetGUID() };
    if (IsSpellReady(player, 6940))
        if (Unit const* sacrificeTarget = SelectFriendlyHealthTarget(player, 40.0f, 95.0f))
            if (sacrificeTarget != player && !sacrificeTarget->HasAura(6940))
                return { "paladin hand of sacrifice", "keep hand of sacrifice cycling on allies", 6940, playerbot::PvpClassSpellContext::TargetMode::Ally, sacrificeTarget->GetGUID() };
    if (CountNearbyEnemies(player, 8.0f) >= 2 && IsSpellReady(player, 26573))
        return { "paladin consecration", "aoe pressure under close melee collapse", 26573, playerbot::PvpClassSpellContext::TargetMode::Self };

    Unit const* executeTarget = SelectNearbyEnemyTarget(player, target, 30.0f);
    if (executeTarget && executeTarget->HealthBelowPct(20) && IsSpellReady(player, 24239))
        return { "paladin hammer of wrath", "execute low-health enemy", 24239, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget->GetGUID() };
    if (IsSpellReady(player, 10308))
        if (Unit const* stunTarget = SelectEnemyCastingTarget(player, 10.0f, executeTarget))
            return { "paladin hammer of justice", "stun nearby cast target", 10308, playerbot::PvpClassSpellContext::TargetMode::Enemy, stunTarget->GetGUID() };
    if (IsSpellReady(player, 20216) && player->IsInCombat())
        return { "paladin divine favor", "increase emergency heal throughput", 20216, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 19943))
        if (Unit const* healTarget = SelectFriendlyHealthTarget(player, 40.0f, 85.0f))
            return { "paladin flash of light", "heal injured allies efficiently", 19943, healTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, healTarget->GetGUID() };
    if (IsSpellReady(player, 635))
        if (Unit const* healTarget = SelectFriendlyHealthTarget(player, 40.0f, 60.0f))
            return { "paladin holy light", "large heal for heavily injured ally", 635, healTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, healTarget->GetGUID() };

    return decision;
}

SpellDecision SelectWarlockSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    if (!HasHostileTarget(player, target))
        return decision;

    Pet const* pet = player->GetPet();
    bool const needsPetSummon = !pet || !pet->IsAlive();

    bool const closePressure = player->IsWithinDistInMap(target, 8.0f);
    if (player->IsInCombat() && needsPetSummon && !player->HasAura(18708) && IsSpellReady(player, 18708))
        return { "warlock fel domination", "prepare instant pet recovery before voidwalker summon", 18708, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!player->IsInCombat() && needsPetSummon && IsSpellReady(player, 697))
        return { "warlock summon voidwalker", "maintain voidwalker pet while out of combat", 697, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->IsInCombat() && needsPetSummon && IsSpellReady(player, 697))
        return { "warlock summon voidwalker", "recover voidwalker in combat when absent", 697, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->HealthBelowPct(45) && IsSpellReady(player, 7812))
        return { "warlock sacrifice", "consume voidwalker shield under low health pressure", 7812, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!player->HasAura(25228) && IsSpellReady(player, 19028))
        return { "warlock soul link", "maintain soul link when pet is available", 19028, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 6215))
        if (Unit const* fearTarget = SelectWarlockFearTarget(player, 20.0f))
            return { "warlock fear", "prioritize fear control on paladin/priest targets in range", 6215, playerbot::PvpClassSpellContext::TargetMode::Enemy, fearTarget->GetGUID() };
    if (target->GetPowerType() == POWER_MANA && ShouldUseCurseOfTongues(target) && !HasAuraFromSpellChain(target, 11719) &&
        !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11719) && IsSpellReady(player, 11719))
        return { "warlock curse of tongues", "slow enemy casting throughput", 11719, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!IsCasterClass(target) && !HasAuraFromSpellChain(target, 11713) && !HasAuraFromSpellChain(target, 11719) &&
        !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11713) && IsSpellReady(player, 11713))
        return { "warlock curse of agony", "apply curse of agony pressure to non-caster players", 11713, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!HasAuraFromSpellChain(target, 11672) && IsSpellReady(player, 11672))
        return { "warlock corruption", "maintain corruption dot", 11672, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if ((target->HealthBelowPct(20) || (closePressure && IsMeleeClass(target))) && IsSpellReady(player, 17926))
        return { "warlock death coil", "peel melee or finish low enemy target", 17926, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 19647))
        return { "warlock spell lock", "pet interrupt when available", 19647, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (player->GetPower(POWER_MANA) < 400 && IsSpellReady(player, 11689))
        return { "warlock life tap", "convert health to mana for sustained casting", 11689, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->HasAura(17941) && IsSpellReady(player, 25307))
        return { "warlock shadow bolt", "consume nightfall proc for instant pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 25307))
        return { "warlock shadow bolt", "default ranged pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    return decision;
}

SpellDecision SelectWarriorSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    if (!HasHostileTarget(player, target))
        return decision;

    Unit const* activeTarget = SelectWarriorPriorityTarget(player, target, 25.0f);
    if (!HasHostileTarget(player, activeTarget))
        activeTarget = target;

    bool const inDefensiveStance = player->HasAura(71);
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, activeTarget, 8.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 8.0f, activeTarget);
    bool const hasNearbyMeleeThreat = HasHostileTarget(player, nearbyMeleeTarget);

    if ((player->HasAuraWithMechanic(1 << MECHANIC_FEAR) || player->HasAuraWithMechanic(1 << MECHANIC_SAPPED)) && player->HasAura(2458) && IsSpellReady(player, 18499))
        return { "warrior berserker rage", "break fear-like control while in berserker stance", 18499, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (inDefensiveStance && (!IsSpellReady(player, 676) || !hasNearbyMeleeThreat) && IsSpellReady(player, 2458))
        return { "warrior berserker stance", "leave defensive stance when disarm is unavailable or no melee threat is nearby", 2458, playerbot::PvpClassSpellContext::TargetMode::Self };
    if ((IsSpellReady(player, 6552) || IsSpellReady(player, 676) || IsSpellReady(player, 20252) || IsSpellReady(player, 1680) || IsSpellReady(player, 21553)) &&
        player->GetPower(POWER_RAGE) < 150 && IsSpellReady(player, 2687))
        return { "warrior bloodrage", "generate rage to unlock rotational abilities", 2687, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (HasHostileTarget(player, nearbyCastingTarget) && IsSpellReady(player, 6552))
        return { "warrior pummel", "interrupt nearby spellcasts", 6552, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget->GetGUID() };
    if (CountNearbyUnsNaredEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 12323))
        return { "warrior piercing howl", "apply area snare when multiple enemies are unsnared in melee range", 12323, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (hasNearbyMeleeThreat && !inDefensiveStance && IsSpellReady(player, 676) && IsSpellReady(player, 71) && player->GetPower(POWER_RAGE) >= 200)
        return { "warrior defensive stance", "swap defensive before disarm against melee", 71, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (hasNearbyMeleeThreat && inDefensiveStance && IsSpellReady(player, 676))
        return { "warrior disarm", "disarm threatening melee weapon users", 676, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyMeleeTarget->GetGUID() };
    if (!player->IsWithinMeleeRange(activeTarget) && !player->IsInCombat() && IsSpellReady(player, 11578))
        return { "warrior charge", "close gap to target from out of combat", 11578, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() };
    if (!player->IsWithinMeleeRange(activeTarget) && player->IsInCombat() && IsSpellReady(player, 20617))
        return { "warrior intercept", "close gap to target while in combat", 20617, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() };
    if (activeTarget->HealthBelowPct(20) && IsSpellReady(player, 20662))
        return { "warrior execute", "finisher at low enemy health", 20662, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() };
    if (!HasAuraFromSpellChain(player, 25289) && IsSpellReady(player, 25289))
        return { "warrior battle shout", "maintain attack power buff", 25289, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->IsWithinMeleeRange(activeTarget))
    {
        if ((!HasAuraFromSpellChain(activeTarget, 7373) || (activeTarget->GetAura(7373) && activeTarget->GetAura(7373)->GetDuration() < 2000)) && IsSpellReady(player, 7373))
            return { "warrior hamstring", "maintain stickiness snare", 7373, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() };
        if (profileSelection.profile == ClassicClassProfile::PrimaryClassic && !HasAuraFromSpellChain(activeTarget, 21553) && IsSpellReady(player, 21553))
            return { "warrior mortal strike", "arms-like burst pressure", 21553, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() };
        if (activeTarget->GetClass() == CLASS_ROGUE && !HasAuraFromSpellChain(activeTarget, 11574) && IsSpellReady(player, 11574))
            return { "warrior rend", "apply anti-stealth bleed pressure on rogues", 11574, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() };
        if (IsSpellReady(player, 1680))
            return { "warrior whirlwind", "fallback aoe melee pressure", 1680, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget->GetGUID() };
    }

    return decision;
}

SpellDecision SelectRogueSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    if (!HasHostileTarget(player, target))
        return decision;

    if (!player->IsInCombat() && IsSpellReady(player, 11202))
    {
        Item* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
        Item* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
        bool const mainHandMissingPoison = mainHand && !mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        bool const offHandMissingPoison = offHand && !offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        if (mainHandMissingPoison || offHandMissingPoison)
            return { "rogue crippling poison", "apply crippling poison to unpoisoned weapons out of combat", 11202, playerbot::PvpClassSpellContext::TargetMode::Self };
    }

    if (!player->IsInCombat() && !HasAuraFromSpellChain(player, 1784) && IsSpellReady(player, 1784))
        return { "rogue stealth", "enter stealth before engagement", 1784, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->HasStealthAura() && IsSpellReady(player, 1833))
        return { "rogue cheap shot", "default opener", 1833, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 1766))
        return { "rogue kick", "interrupt enemy cast", 1766, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (player->HealthBelowPct(40) && IsSpellReady(player, 5277))
        return { "rogue evasion", "defensive survival in melee", 5277, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (!player->HealthBelowPct(50) && !player->IsWithinMeleeRange(target) && player->IsWithinDistInMap(target, 30.0f) && IsSpellReady(player, 11305))
        return { "rogue sprint", "close gap for melee pressure", 11305, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 2094))
        if (Unit const* blindTarget = SelectRogueBlindTarget(player, target, 15.0f))
            return { "rogue blind", "prioritize druid/shaman/paladin secondary targets without abolish poison", 2094, playerbot::PvpClassSpellContext::TargetMode::Enemy, blindTarget->GetGUID() };
    if (player->GetComboPoints() >= 5 && IsSpellReady(player, 8643))
        return { "rogue kidney shot", "primary stun finisher at full combo points", 8643, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (player->GetComboPoints() >= 5 && IsSpellReady(player, 11300))
        return { "rogue eviscerate", "combo finisher pressure", 11300, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (!player->IsWithinMeleeRange(target) && player->IsWithinDistInMap(target, 25.0f) && IsSpellReady(player, 36554))
        return { "rogue shadowstep", "bridge short gap before melee globals", 36554, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 16511))
        return { "rogue hemorrhage", "default subtlety combo point builder", 16511, playerbot::PvpClassSpellContext::TargetMode::Enemy };

    return decision;
}

SpellDecision SelectShamanSpell(Player const* player, Unit const* target)
{
    SpellDecision decision;
    if (!player)
        return decision;

    if (SpellDecision const utilityDecision = SelectOutOfCombatEatDrinkOrMountSpell(player); utilityDecision.spellId)
        return utilityDecision;

    if (!HasHostileTarget(player, target))
        return decision;

    if (!player->IsInCombat())
    {
        if (!HasTemporaryWeaponImbue(player) && IsSpellReady(player, 8232))
            return { "shaman windfury weapon", "maintain weapon imbue out of combat", 8232, playerbot::PvpClassSpellContext::TargetMode::Self };
        if (!HasAuraFromSpellChain(player, 10432) && IsSpellReady(player, 10432))
            return { "shaman lightning shield", "maintain shield buff out of combat", 10432, playerbot::PvpClassSpellContext::TargetMode::Self };
    }

    if (target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 10414))
        return { "shaman earth shock", "interrupt enemy cast with shock", 10414, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (target->GetPowerType() == POWER_MANA && IsSpellReady(player, 8177))
        return { "shaman grounding totem", "counter incoming caster pressure", 8177, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 16166))
        return { "shaman elemental mastery", "trigger burst throughput cooldown", 16166, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 10605))
        return { "shaman chain lightning", "primary burst cast on kill target", 10605, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsMeleeClass(target) && player->IsWithinDistInMap(target, 10.0f) && IsSpellReady(player, 2484))
        return { "shaman earthbind totem", "kite nearby melee pressure", 2484, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsMeleeClass(target) && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 10473))
        return { "shaman frost shock", "snare medium-range melee threats", 10473, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (target->GetClass() == CLASS_ROGUE && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 8170))
        return { "shaman poison cleansing totem", "answer rogue poison pressure", 8170, playerbot::PvpClassSpellContext::TargetMode::Self };
    if ((target->GetClass() == CLASS_PRIEST || target->GetClass() == CLASS_WARLOCK) && player->IsWithinDistInMap(target, 20.0f) && IsSpellReady(player, 8143))
        return { "shaman tremor totem", "mitigate fear pressure from priest/warlock", 8143, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (player->HealthBelowPct(50) && IsSpellReady(player, 10468))
        return { "shaman lesser healing wave", "self-sustain while focused", 10468, playerbot::PvpClassSpellContext::TargetMode::Self };
    if (IsSpellReady(player, 370))
        return { "shaman purge", "strip enemy magical effects by default", 370, playerbot::PvpClassSpellContext::TargetMode::Enemy };
    if (IsSpellReady(player, 15208))
        return { "shaman lightning bolt", "fallback ranged damage cast", 15208, playerbot::PvpClassSpellContext::TargetMode::Enemy };

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

    Unit const* target = SelectCombatTarget(player);
    bool const hasValidTarget = target && target->IsAlive() && target->GetGUID() != player->GetGUID();

    ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
    Unit const* allyTarget = SelectAllyTarget(player);
    SpellDecision const decision = SelectClassicClassSpell(player, hasValidTarget ? target : nullptr, allyTarget, profileSelection);

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
    context.targetGuid = hasValidTarget ? target->GetGUID() : ObjectGuid::Empty;
    context.allyTargetGuid = allyTarget ? allyTarget->GetGUID() : ObjectGuid::Empty;
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
