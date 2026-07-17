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
#include "BattlegroundTP.h"
#include "BattlegroundWS.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Totem.h"
#include "Unit.h"
#include "Util.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
struct SpellDecision;
bool HasHostileTarget(Player const* player, Unit const* target);
uint32 ResolveKnownPlayerSpellInChain(Player const* player, uint32 spellId);
bool IsPetSpellReady(Player const* player, uint32 spellId);
bool IsSpellReady(Player const* player, uint32 spellId);
bool MeetsCasterAuraStateRequirements(Player const* player, uint32 spellId);
bool IsFriendlySupportTarget(Player const* player, Unit const* target);
bool HasAuraFromSpellChain(Unit const* unit, uint32 baseSpellId);
bool HasBreakableCrowdControl(Unit const* unit);
uint32 CountNearbyEnemies(Player const* player, float maxDistance);
SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player);
SpellDecision SelectRacialSpell(Player const* player, Unit const* target, Unit const* allyTarget);
bool HasActiveMovementEffectSpline(Player const* player);

constexpr float kReferenceHunterMeleeDistance = 5.0f;
constexpr float kReferenceHunterSwitchDistance = 8.0f;
constexpr float kRangedSpacingEnterOutOfRangeBuffer = 2.0f;
constexpr float kRangedSpacingEnterTooCloseBuffer = 1.0f;
constexpr uint32 kHunterAutoShotSpellId = 75;
constexpr uint32 kHunterCallPetSpellId = 883;
constexpr uint32 kHunterRevivePetSpellId = 982;
constexpr uint32 kPlayerbotHunterStationaryCastLockToken = 900006;
constexpr uint32 kPlayerbotShadowmeldGraceToken = 900007;
constexpr uint32 kWandShootSpellId = 5019;
constexpr uint32 kPlayerbotDispelCooldownToken = 900004;
constexpr uint32 kPlayerbotHandOfSacrificeCooldownToken = 900005;
constexpr uint32 kPaladinSacrificialAuraSpellId = 83256;
constexpr uint32 kDruidCasterFaerieFireSpellId = 9907;
constexpr uint32 kDruidThornsSpellId = 9910;
constexpr uint32 kDruidMassThornsSpellId = 89762;
constexpr uint32 kPriestWeakenedSoulSpellId = 6788;
constexpr uint32 kPriestShadowWordPainSpellId = 589;
constexpr uint32 kPriestDevouringCurseSpellId = 19280;
constexpr uint32 kPriestElunesGraceSpellId = 81351;
constexpr uint32 kPriestWispFormSpellId = 81352;
constexpr uint32 kPriestWyrmsShadowSpellId = 81357;
constexpr uint32 kPriestLightwellSpellId = 27871;
constexpr uint32 kPriestLightwellRenewSpellId = 27874;
constexpr uint32 kPriestLightwellGameObjectEntry = 181106;
constexpr uint32 kWarlockCreateSoulwellSpellId = 29886;
constexpr uint32 kWarlockRitualOfSoulsSpellId = 29893;
constexpr std::array<uint32, 6> kWarlockSoulwellGameObjectEntries = { 181621, 183510, 183511, 193169, 193170, 193171 };
constexpr std::array<uint32, 24> kHealthstoneItemEntries =
{
    36894, 36893, 36892, 36891, 36890, 36889,
    22105, 22104, 22103,
    19013, 19012, 9421,
    19011, 19010, 5510,
    19009, 19008, 5509,
    19007, 19006, 5511,
    19005, 19004, 5512
};
constexpr uint32 kWarlockFirestoneItemEntry = 13701;
constexpr uint32 kWarlockFirestoneUseSpellId = 81334;
constexpr uint32 kMageManaRubyUseSpellId = 22044;
constexpr uint32 kMageManaRubyItemId = 8008;
constexpr uint32 kRacialOrcBloodFurySpellId = 20572;
constexpr uint32 kRacialTrollBloodlustSpellId = 20554;
constexpr uint32 kRacialNightElfShadowmeldSpellId = 20580;
constexpr uint32 kRacialTaurenWarStompSpellId = 20549;
constexpr uint32 kRacialUndeadWillOfTheForsakenSpellId = 7744;
constexpr uint32 kRacialDwarfStoneformSpellId = 20594;
constexpr uint32 kRacialGnomeSurpriseSpellId = 89160;
constexpr uint32 kRacialHumanPerceptionSpellId = 20600;
constexpr float kPlayerbotTotemRefreshDistance = 30.0f;
// Carrier auras are a second source of truth for the short interval between a
// flag click and the next collected battleground-state snapshot. Keep this
// centralized rather than teaching class selectors about individual BGs.
constexpr std::array<uint32, 4> kBattlegroundFlagCarrierAuraIds = { 23333, 23335, 34976, 89798 };
std::unordered_map<ObjectGuid, bool> g_HunterRangedModeByBot;
std::mutex g_HunterRangedModeByBotLock;
std::unordered_map<ObjectGuid, uint8> g_CombatNoTargetTicksByBot;
std::mutex g_CombatNoTargetTicksByBotLock;
std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, bool>> g_HumanPerceptionStealthStateByBot;
std::mutex g_HumanPerceptionStealthStateLock;
thread_local ObjectGuid g_CurrentDecisionBotGuid = ObjectGuid::Empty;
thread_local uint32 g_SuppressedDecisionSpellId = 0;

bool IsPriestFlashHealSpell(uint32 spellId)
{
    if (!spellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;
    return firstRankSpellId == 2061; // Flash Heal (rank 1)
}

bool IsShamanFrostShockSpell(uint32 spellId)
{
    if (!spellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;
    return firstRankSpellId == 8056;
}

bool IsPriestInSpiritOfRedemption(Player const* player)
{
    return player && player->GetClass() == CLASS_PRIEST &&
        (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION) || HasAuraFromSpellChain(player, 27827));
}

bool IsSpiritOfRedemptionFreeHeal(Player const* player, uint32 spellId)
{
    return IsPriestInSpiritOfRedemption(player) && IsPriestFlashHealSpell(spellId);
}

bool IsHunterTrapSpell(uint32 spellId)
{
    if (!spellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;

    switch (firstRankSpellId)
    {
        case 1499:  // Freezing Trap
        case 13795: // Immolation Trap
        case 13809: // Frost Trap
        case 13813: // Explosive Trap
            return true;
        default:
            return false;
    }
}

bool IsHunterAimedShotSpellId(uint32 spellId)
{
    switch (spellId)
    {
        case 19434: // Aimed Shot rank 1
        case 20900:
        case 20901:
        case 20902:
        case 20903:
        case 20904:
        case 27065:
        case 49049:
        case 49050:
            return true;
        default:
            return false;
    }
}

bool IsHunterAimedShotSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    if (IsHunterAimedShotSpellId(spellInfo->Id))
        return true;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && IsHunterAimedShotSpellId(firstRank->Id);
}

bool IsHunterMultiShotSpellId(uint32 spellId)
{
    switch (spellId)
    {
        case 2643:  // Multi-Shot rank 1
        case 14288:
        case 14289:
        case 14290:
        case 25294:
        case 27021:
        case 49047:
        case 49048:
            return true;
        default:
            return false;
    }
}

bool IsHunterMultiShotSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    if (IsHunterMultiShotSpellId(spellInfo->Id))
        return true;

    SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
    return firstRank && IsHunterMultiShotSpellId(firstRank->Id);
}

uint32 GetHunterStationaryCastTimeMs(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return 0;

    uint32 castTimeMs = uint32(std::max<int32>(0, spellInfo->CalcCastTime()));

    // Aimed Shot is the hunter hard-cast this lock primarily exists for. Some
    // custom DBC/rank data reports it inconsistently, so recognize known ranks
    // explicitly and give them a real stationary guard.
    if (IsHunterAimedShotSpell(spellInfo))
        return std::max<uint32>(castTimeMs, 3000);

    // Revive Pet is also a real cast-time action and should not be clipped by
    // the stutter/flee loop.
    if (spellInfo->Id == 982)
        return std::max<uint32>(castTimeMs, 1000);

    // Multi-Shot is short, but it still has a stationary firing delay/cast
    // window in this branch. It must be protected from stutter movement too;
    // otherwise the hunter starts Multi-Shot, immediately resumes fleeing, and
    // clips the shot before it launches. Use a short explicit guard even when
    // custom DBC data reports zero cast time.
    if (IsHunterMultiShotSpell(spellInfo))
        return std::max<uint32>(castTimeMs, 500);

    return castTimeMs > 0 ? castTimeMs : 0;
}

bool IsActiveHunterCastTimeSpell(Spell const* spell)
{
    if (!spell || spell->getState() == SPELL_STATE_FINISHED)
        return false;

    SpellInfo const* spellInfo = spell->GetSpellInfo();
    if (!spellInfo)
        return false;

    return GetHunterStationaryCastTimeMs(spellInfo) > 0 ||
        (spellInfo->IsChanneled() && !spellInfo->IsMoveAllowedChannel());
}

bool IsHunterCastTimeActionLocked(Player const* player)
{
    if (!player || player->GetClass() != CLASS_HUNTER)
        return false;

    if (IsActiveHunterCastTimeSpell(player->GetCurrentSpell(CURRENT_GENERIC_SPELL)))
        return true;

    if (IsActiveHunterCastTimeSpell(player->GetCurrentSpell(CURRENT_CHANNELED_SPELL)))
        return true;

    // Do not use broad IsNonMeleeSpellCast() here: the explicit stationarity
    // helper above covers the hunter shots we actually need to protect
    // (Aimed Shot, Multi-Shot, Revive Pet) without catching unrelated delayed
    // spell states.
    return playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHunterStationaryCastLockToken);
}

SpellInfo const* GetFirstOnUseItemSpellInfo(Item const* item)
{
    if (!item)
        return nullptr;

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate)
        return nullptr;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& spellData = itemTemplate->Spells[i];
        if (spellData.SpellId <= 0 || spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellData.SpellId))
            return spellInfo;
    }

    return nullptr;
}

bool IsOnUseItemReady(Player const* player, uint32 itemEntry)
{
    if (!player || !itemEntry)
        return false;

    Item* item = player->GetItemByEntry(itemEntry);
    if (!item)
        return false;

    if (player->CanUseItem(item) != EQUIP_ERR_OK)
        return false;

    SpellInfo const* itemSpellInfo = GetFirstOnUseItemSpellInfo(item);
    if (!itemSpellInfo)
        return false;

    if (player->GetSpellHistory()->HasCooldown(itemSpellInfo->Id) ||
        player->GetSpellHistory()->HasCooldown(itemSpellInfo, item->GetEntry()) ||
        player->GetSpellHistory()->HasGlobalCooldown(itemSpellInfo))
        return false;

    return true;
}

uint32 SelectReadyHealthstoneItemEntry(Player const* player)
{
    if (!player)
        return 0;

    for (uint32 itemEntry : kHealthstoneItemEntries)
        if (IsOnUseItemReady(player, itemEntry))
            return itemEntry;

    return 0;
}

bool IsWarlockSpellstoneItemEntry(uint32 itemEntry)
{
    switch (itemEntry)
    {
        case 5522:  // Spellstone
        case 13602: // Greater Spellstone
        case 13603: // Major Spellstone
        case 22646: // Master Spellstone
        case 41196: // Demonic Spellstone / later-client spellstone variant
            return true;
        default:
            return false;
    }
}

uint32 SelectReadyWarlockSpellstoneItemEntry(Player const* player)
{
    if (!player || player->GetClass() != CLASS_WARLOCK)
        return 0;

    // Prefer the actually equipped wand/relic slot item first. Spellstones are
    // commonly equipped there on this branch, and GetItemByEntry can also find
    // bag copies, so slot-first avoids selecting an older loose stone.
    if (Item const* rangedItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
    {
        uint32 const rangedEntry = rangedItem->GetEntry();
        if (IsWarlockSpellstoneItemEntry(rangedEntry) && IsOnUseItemReady(player, rangedEntry))
            return rangedEntry;
    }

    // Highest-rank-first fallback for spellstones in bags/equipment.
    static constexpr std::array<uint32, 5> kSpellstoneEntries = { 41196, 22646, 13603, 13602, 5522 };
    for (uint32 itemEntry : kSpellstoneEntries)
        if (IsOnUseItemReady(player, itemEntry))
            return itemEntry;

    return 0;
}


bool HasPoisonEffect(Player const* player)
{
    if (!player)
        return false;

    for (Unit::AuraApplicationMap::value_type const& appliedAura : player->GetAppliedAuras())
    {
        AuraApplication const* aurApp = appliedAura.second;
        SpellInfo const* spellInfo = aurApp ? aurApp->GetBase()->GetSpellInfo() : nullptr;
        if (spellInfo && spellInfo->Dispel == DISPEL_POISON)
            return true;
    }

    return false;
}

bool HasWillOfTheForsakenBreakableControl(Player const* player)
{
    if (!player)
        return false;

    constexpr uint32 wotfMechanicMask =
        (1u << MECHANIC_FEAR) |
        (1u << MECHANIC_CHARM) |
        (1u << MECHANIC_SLEEP);

    return player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->HasAuraWithMechanic(wotfMechanicMask);
}

bool HasCastTimeSpellTargetingPlayer(Unit const* caster, Player const* target)
{
    if (!caster || !target)
        return false;

    auto isCastTimeSpellTargetingPlayer = [target](Spell const* spell) -> bool
    {
        if (!spell)
            return false;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo || spellInfo->CalcCastTime() <= 0)
            return false;

        return spell->m_targets.GetUnitTargetGUID() == target->GetGUID();
    };

    return isCastTimeSpellTargetingPlayer(caster->GetCurrentSpell(CURRENT_GENERIC_SPELL)) ||
        isCastTimeSpellTargetingPlayer(caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL));
}

Unit const* SelectEnemyCastingAtPlayer(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
            continue;
        if (HasCastTimeSpellTargetingPlayer(candidate, player))
            return candidate;
    }

    return nullptr;
}

Unit const* SelectRandomEnemyWithoutBreakableCrowdControl(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    std::vector<Unit const*> candidates;
    candidates.reserve(8);

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate))
            continue;
        if (!candidate->IsAlive() || !player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
            continue;
        if (HasBreakableCrowdControl(candidate))
            continue;
        candidates.push_back(candidate);
    }

    if (candidates.empty())
        return nullptr;

    return candidates[urand(0u, static_cast<uint32>(candidates.size() - 1))];
}

bool HumanRecentlySawStealthTransition(Player const* player, float maxDistance)
{
    if (!player || player->GetRace() != RACE_HUMAN || !player->FindMap())
        return false;

    bool shouldPerceive = false;
    std::lock_guard<std::mutex> lock(g_HumanPerceptionStealthStateLock);
    std::unordered_map<ObjectGuid, bool>& seenStealthByEnemy = g_HumanPerceptionStealthStateByBot[player->GetGUID()];

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate) || !candidate->IsAlive() || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;

        bool const currentlyStealthed = candidate->HasStealthAura();
        auto knownItr = seenStealthByEnemy.find(candidate->GetGUID());
        bool const wasKnown = knownItr != seenStealthByEnemy.end();
        bool const wasStealthed = wasKnown ? knownItr->second : currentlyStealthed;

        if (wasKnown && !wasStealthed && currentlyStealthed)
            shouldPerceive = true;

        seenStealthByEnemy[candidate->GetGUID()] = currentlyStealthed;
    }

    return shouldPerceive;
}

bool WantsNightElfShadowmeldUtility(Player const* player)
{
    if (!player || !player->IsInCombat())
        return false;

    bool const wantsDrink = player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 35.0f;
    bool const wantsEat = player->HealthBelowPct(50);
    bool const hunterWantsTrap = player->GetClass() == CLASS_HUNTER &&
        (IsSpellReady(player, 1499) || IsSpellReady(player, 14311) || IsSpellReady(player, 13809));
    bool const rogueWantsStealth = player->GetClass() == CLASS_ROGUE && !player->HasStealthAura() && IsSpellReady(player, 1784);

    return wantsDrink || wantsEat || hunterWantsTrap || rogueWantsStealth;
}


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
            break;
    }

    if (spellInfo)
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (effect.Effect == SPELL_EFFECT_DISPEL)
                return true;

    return false;
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

    float const distance = player->GetExactDist(target);
    bool const previousRangedMode = rangedMode;
    if (rangedMode)
    {
        // Do not collapse the 5-8y hunter dead-zone into melee mode. Melee
        // pressure is only valid at true melee distance; the dead-zone must be
        // handled by explicit retreat movement before ranged spell selection.
        if (distance <= kReferenceHunterMeleeDistance)
            rangedMode = false;
    }
    else
    {
        // Once the hunter has created real ranged-weapon separation, immediately
        // return to ranged mode even if the enemy is still targeting/chasing the
        // hunter. The old victim check kept hunters stuck in melee mode while a
        // warrior/rogue followed them, so they would Wing Clip once and never
        // resume shooting.
        if (distance >= kReferenceHunterSwitchDistance)
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

float GetHunterDeadZoneMaxRange(Player const* player, Unit const* target)
{
    playerbot::HunterAutoShotRangeInfo rangeInfo;
    if (!playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo))
        return kReferenceHunterSwitchDistance;

    return rangeInfo.minRange;
}

bool IsHunterWithinAutoShotBand(Player const* player, Unit const* target)
{
    playerbot::HunterAutoShotRangeInfo rangeInfo;
    if (!playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo))
        return false;

    return player->IsWithinLOSInMap(target) &&
        rangeInfo.exactDistance > rangeInfo.minRange + playerbot::PLAYERBOT_HUNTER_AUTOSHOT_MIN_SAFETY_MARGIN &&
        rangeInfo.exactDistance <= rangeInfo.maxRange;
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

bool IsHunterExactDeadZone(Player const* player, Unit const* target)
{
    playerbot::HunterAutoShotRangeInfo rangeInfo;
    if (!playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo))
        return false;

    // SPELL_RANGE_RANGED adds the caster/target melee range to the raw DBC
    // minimum. For normal players this is the familiar 3 + 5 = 8 yard Auto
    // Shot floor, but using the core-equivalent values also handles combat
    // reach and custom melee-range modifiers correctly.
    return rangeInfo.exactDistance > rangeInfo.meleeRange &&
        rangeInfo.exactDistance < rangeInfo.minRange;
}

Unit const* SelectHunterDeadZoneEnemy(Player const* player, Unit const* preferredTarget)
{
    if (!player || player->GetClass() != CLASS_HUNTER)
        return nullptr;

    if (HasHostileTarget(player, preferredTarget) &&
        player->IsWithinLOSInMap(preferredTarget) &&
        IsHunterExactDeadZone(player, preferredTarget))
        return preferredTarget;

    if (!player->FindMap())
        return nullptr;

    Unit const* bestTarget = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate) || !player->IsWithinLOSInMap(candidate) || !IsHunterExactDeadZone(player, candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestTarget = candidate;
        }
    }

    return bestTarget;
}

float ComputeHunterDeadZoneRetreatStep(Player const* player, Unit const* target)
{
    if (!player || !target)
        return 6.0f;

    float const currentDistance = player->GetExactDist(target);
    float const desiredExactDistance = std::max(GetHunterDeadZoneMaxRange(player, target) + 4.0f, 12.0f);
    return std::clamp(desiredExactDistance - currentDistance, 4.0f, 10.0f);
}

float ComputeHunterMeleeKiteRetreatStep(Player const* player, Unit const* target)
{
    if (!player || !target)
        return 10.0f;

    float const currentDistance = player->GetExactDist(target);
    float const desiredExactDistance = std::max(GetHunterDeadZoneMaxRange(player, target) + 5.0f, 13.0f);
    return std::clamp(desiredExactDistance - currentDistance, 8.0f, 14.0f);
}

bool IsHunterMeleeKiteTargetControlled(Unit const* target)
{
    if (!target)
        return false;

    return HasAuraFromSpellChain(target, 14268) || // Wing Clip
        target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
        target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN));
}

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

bool IsAutoRepeatRangedSpellActive(Player const* player, uint32 spellId)
{
    if (!player || !spellId)
        return false;

    Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
    if (!autoRepeat)
        return false;

    SpellInfo const* activeAutoRepeatInfo = autoRepeat->GetSpellInfo();
    return activeAutoRepeatInfo && activeAutoRepeatInfo->Id == spellId;
}

bool IsWandShootReadyForDecision(Player const* player)
{
    return HasWandEquipped(player) &&
        IsSpellReady(player, kWandShootSpellId) &&
        !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kWandShootSpellId) &&
        !IsAutoRepeatRangedSpellActive(player, kWandShootSpellId);
}


struct HunterPetDecisionState
{
    bool hasActivePet = false;
    bool hasLivingPet = false;
    bool hasDeadPet = false;
    bool hasLoadableHunterPet = false;
    bool loadablePetDead = false;
    bool loadablePetTameable = false;
    bool canCallPet = false;
    bool shouldRevivePet = false;
};

HunterPetDecisionState GetHunterPetDecisionState(Player const* player)
{
    HunterPetDecisionState state;
    if (!player || player->GetClass() != CLASS_HUNTER)
        return state;

    if (Pet const* pet = player->GetPet())
    {
        state.hasActivePet = true;
        state.hasLivingPet = pet->IsAlive();
        state.hasDeadPet = !pet->IsAlive();
        state.shouldRevivePet = state.hasDeadPet;
        return state;
    }

    PetStable const* petStable = player->GetPetStable();
    if (!petStable)
        return state;

    std::pair<PetStable::PetInfo const*, PetSaveMode> const loadInfo = Pet::GetLoadPetInfo(*petStable, 0, 0, false);
    PetStable::PetInfo const* petInfo = loadInfo.first;
    if (!petInfo || petInfo->Type != HUNTER_PET)
        return state;

    state.hasLoadableHunterPet = true;
    state.loadablePetDead = petInfo->Health == 0;

    CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(petInfo->CreatureId);
    state.loadablePetTameable = creatureInfo && creatureInfo->IsTameable(player->CanTameExoticPets());
    state.canCallPet = !state.loadablePetDead && state.loadablePetTameable;
    state.shouldRevivePet = state.loadablePetDead;
    return state;
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
    ObjectGuid const flagPickupGuid = battleground->GetFlagPickupGUID(playerGuid);
    values.flagPickupAvailable = !flagPickupGuid.IsEmpty();
    if (values.flagPickupAvailable)
        if (Map* map = player->FindMap())
            if (GameObject* flag = map->GetGameObject(flagPickupGuid))
                values.flagPickupNearby = flag->IsInWorld() && player->IsWithinDistInMap(flag, 10.0f);

    // Use one local-pressure boundary for tactical objective selection and
    // class movement ownership. A midfield bot should break off its flag route
    // to fight nearby enemies, while distant enemies must not pull it into a
    // map-wide chase. LOS is intentionally not required: an enemy immediately
    // around a corner is still local pressure that combat pathing should handle.
    float const nearbyEnemyBoundary = std::max(GetConfiguredLongRange(), 35.0f);
    if (Map const* map = player->FindMap())
    {
        Map::PlayerList const& mapPlayers = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player const* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate) ||
                !player->IsWithinDistInMap(candidate, nearbyEnemyBoundary))
                continue;

            values.nearbyEnemyActive = true;
            break;
        }
    }

    BattlegroundNodeObjective nodeObjective;
    if (battleground->GetNodeObjective(playerGuid, nodeObjective))
    {
        values.nodeObjectiveId = nodeObjective.NodeId;
        bool const isDefense = nodeObjective.Status == BattlegroundNodeStatus::FriendlyControlled ||
            nodeObjective.Status == BattlegroundNodeStatus::FriendlyContested ||
            nodeObjective.Status == BattlegroundNodeStatus::FriendlyUnderAttack;
        values.nodeDefenseAvailable = isDefense;
        values.nodeAssaultAvailable = !isDefense;
    }

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

    auto populateNeutralFlagCarrierValues = [&](ObjectGuid const& carrierGuid)
    {
        if (carrierGuid.IsEmpty())
            return;

        values.playerHasFlag = (carrierGuid == playerGuid);
        Player const* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
        if (!carrier || !carrier->IsAlive() || carrier->GetMapId() != player->GetMapId())
            return;

        uint32 const assignedCarrierTeam = battleground->GetPlayerTeam(carrierGuid);
        TeamId const carrierTeam = assignedCarrierTeam == ALLIANCE ? TEAM_ALLIANCE :
            assignedCarrierTeam == HORDE ? TEAM_HORDE : carrier->GetTeamId();
        if (carrierTeam == botTeam)
            values.teamFlagCarrierNear = player->IsWithinDistInMap(carrier, 200.0f);
        else
        {
            values.enemyFlagCarrierActive = true;
            values.enemyFlagCarrierNear = player->IsWithinDistInMap(carrier, 100.0f);
        }
    };

    populateNeutralFlagCarrierValues(battleground->GetFlagPickerGUID());
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

SpellDecision SelectRacialSpell(Player const* player, Unit const* target, Unit const* allyTarget)
{
    if (!player || !player->IsAlive())
        return {};

    // Avoid breaking rogue/druid stealth openers or Shadowmeld recovery windows
    // with racial utility. Stealthed bots should commit to their opener instead
    // of spending a racial before they engage.
    if (player->HasStealthAura())
        return {};

    switch (player->GetRace())
    {
        case RACE_ORC:
        {
            bool const wantsThroughput = player->IsInCombat() &&
                ((target && HasHostileTarget(player, target) && !HasBreakableCrowdControl(target)) ||
                 (allyTarget && IsFriendlySupportTarget(player, allyTarget) && allyTarget->GetHealthPct() < 85.0f));
            if (wantsThroughput && IsSpellReady(player, kRacialOrcBloodFurySpellId))
                return { "racial blood fury", "orc racial throughput for healing or damage", kRacialOrcBloodFurySpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        }
        case RACE_TROLL:
        {
            bool const activelyDoingAnything = player->IsInCombat() ||
                (target && HasHostileTarget(player, target)) ||
                (allyTarget && IsFriendlySupportTarget(player, allyTarget) && allyTarget->GetHealthPct() < 95.0f);
            if (activelyDoingAnything && IsSpellReady(player, kRacialTrollBloodlustSpellId))
                return { "racial bloodlust", "troll racial haste while actively fighting or casting", kRacialTrollBloodlustSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        }
        case RACE_NIGHTELF:
        {
            Unit const* casterTargetingMe = SelectEnemyCastingAtPlayer(player, 45.0f);
            if ((casterTargetingMe || WantsNightElfShadowmeldUtility(player)) && IsSpellReady(player, kRacialNightElfShadowmeldSpellId))
                return { "racial shadowmeld", casterTargetingMe ? "break incoming cast target with shadowmeld" : "drop combat for recovery or setup", kRacialNightElfShadowmeldSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        }
        case RACE_TAUREN:
            if (CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, kRacialTaurenWarStompSpellId))
                return { "racial war stomp", "stomp clustered nearby enemies", kRacialTaurenWarStompSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        case RACE_UNDEAD_PLAYER:
            if (HasWillOfTheForsakenBreakableControl(player) && IsSpellReady(player, kRacialUndeadWillOfTheForsakenSpellId))
                return { "racial will of the forsaken", "break fear charm or sleep", kRacialUndeadWillOfTheForsakenSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        case RACE_DWARF:
            if (HasPoisonEffect(player) && IsSpellReady(player, kRacialDwarfStoneformSpellId))
                return { "racial stoneform", "remove poison effects", kRacialDwarfStoneformSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        case RACE_GNOME:
        {
            Unit const* surpriseTarget = IsSpellReady(player, kRacialGnomeSurpriseSpellId) ? SelectRandomEnemyWithoutBreakableCrowdControl(player, 30.0f) : nullptr;
            if (surpriseTarget)
                return { "racial surprise", "throw surprise grenade at random non-cc enemy", kRacialGnomeSurpriseSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, surpriseTarget->GetGUID() };
            break;
        }
        case RACE_HUMAN:
            if (HumanRecentlySawStealthTransition(player, 30.0f) && IsSpellReady(player, kRacialHumanPerceptionSpellId))
                return { "racial perception", "enemy just entered stealth nearby", kRacialHumanPerceptionSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        default:
            break;
    }

    return {};
}


bool IsDecisionImmediatelyCastable(Player const* player, SpellDecision const& decision, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget);
SpellDecision SelectHighestPriorityCastableDecision(std::vector<PrioritizedSpellDecision>& candidates, Player const* player,
    Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget);

SpellDecision MaybeSelectUtilitySpell(Player const* player, Unit const* hostileTarget)
{
    if (!player)
        return {};

    // Spirit of Redemption is a short free-cast healing window. Never let
    // food/drink/mount utility preempt the priest healing selector here, even
    // if the priest is technically out of combat or at 0 mana.
    if (IsPriestInSpiritOfRedemption(player))
        return {};

    constexpr uint32 kPlayerbotDrinkSpell = 22734;
    bool const maintainExistingDrink = !player->IsInCombat() &&
        player->GetMaxPower(POWER_MANA) > 0 &&
        player->HasAura(kPlayerbotDrinkSpell) &&
        player->GetPowerPct(POWER_MANA) < 50.0f;

    // A selected enemy can remain valid across most of a battleground. Only
    // let it suppress out-of-combat utility when it is actually close enough
    // to matter; the mount selector separately scans for every nearby enemy.
    //
    // Exception: if the bot is already drinking and still below the 50% mana
    // floor, keep utility selection available so drink remains sticky instead
    // of immediately breaking back into combat posture.
    if (HasHostileTarget(player, hostileTarget) &&
        player->IsWithinDistInMap(hostileTarget, playerbot::PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE) && !maintainExistingDrink)
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
    if (!condition || (!decision.spellId && !decision.itemEntry))
        return;

    if (g_SuppressedDecisionSpellId != 0 && decision.spellId != 0 && decision.spellId == g_SuppressedDecisionSpellId)
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

    // Cast-time hunter actions are exclusive. Do not use the normal fallback
    // behavior here: if every candidate is rejected because Aimed Shot/Revive
    // Pet is already preparing, returning candidates.front() would re-cast the
    // same action and cancel the original cast.
    if (IsHunterCastTimeActionLocked(player))
        return {};

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
        return !playerbot::PvpCore::ShouldSeekLightwell(player) && IsOnUseItemReady(player, decision.itemEntry);

    if (!decision.spellId)
        return false;

    if (IsHunterCastTimeActionLocked(player))
        return false;

    bool const canUseRitualSoulwellEffect = player->GetClass() == CLASS_WARLOCK &&
        decision.spellId == kWarlockCreateSoulwellSpellId && player->HasSpell(kWarlockRitualOfSoulsSpellId);
    uint32 const knownPlayerSpellId = canUseRitualSoulwellEffect ? kWarlockCreateSoulwellSpellId :
        ResolveKnownPlayerSpellInChain(player, decision.spellId);
    bool const knownByPlayer = knownPlayerSpellId != 0;
    bool const knownByPet = IsPetSpellReady(player, decision.spellId);
    if (!knownByPlayer && !knownByPet)
        return false;

    if (knownByPlayer && player->GetSpellHistory()->HasCooldown(knownPlayerSpellId))
        return false;

    // Treat the shared playerbot dispel cooldown as an immediate castability
    // failure so the same decision pass suppresses this dispel and selects the
    // next available action instead of returning an idle cooldown attempt.
    if (IsPlayerbotDispelSpell(decision.spellId) &&
        playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken))
        return false;

    if (decision.spellId == kDruidCasterFaerieFireSpellId &&
        playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kDruidCasterFaerieFireSpellId))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(knownByPlayer ? knownPlayerSpellId : decision.spellId);
    if (!spellInfo)
        return false;

    if (playerbot::PvpCore::ShouldSeekLightwell(player))
    {
        if (spellInfo->CalcCastTime() > 0 || spellInfo->IsChanneled() || spellInfo->IsAutoRepeatRangedSpell())
            return false;

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_TELEPORT_UNITS:
                case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
                case SPELL_EFFECT_LEAP:
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                case SPELL_EFFECT_LEAP_BACK:
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                    return false;
                default:
                    break;
            }
        }
    }

    bool const breaksFlagCarry = playerbot::PvpCore::SpellWouldBreakFlagCarry(spellInfo->Id);
    if (breaksFlagCarry && playerbot::PvpCore::IsBattlegroundFlagCarrier(player))
        return false;

    // A shapeshift form (Moonkin, Ghost Wolf, Bear/Cat, ...) makes most other
    // spells uncastable. Without this check the selector marks something like
    // Lightning Shield as "castable" while the caster is a Ghost Wolf, attempts
    // it, and it fails server-side with SPELL_FAILED_NOT_SHAPESHIFT instead of
    // the loop moving on to a candidate that actually works.
    // Rehgar's Fury (82419) is exempted: its whole purpose (spell_sha_ghost_wolf_charge
    // / Unit::CompleteGhostWolfCharge) is to be cast while Ghost Wolf is active, but
    // if its spell_template Stances field was never authored to explicitly allow the
    // Ghost Wolf stance, this generic DBC-driven check would wrongly reject it too -
    // leaving nothing valid to do while shifted except immediately unshift again,
    // which reads as the shaman flickering in and out of Ghost Wolf and never leaping.
    if (knownByPlayer && decision.spellId != 82419 && player->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) &&
        spellInfo->CheckShapeshift(player->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;

    // Classic hunter traps cannot be placed while the hunter is still in combat.
    // Feign Death may clear combat and make the next trap valid, but the selector
    // must not return an in-combat trap attempt that would spam
    // SPELL_FAILED_AFFECTING_COMBAT.
    if (player->GetClass() == CLASS_HUNTER && IsHunterTrapSpell(knownByPlayer ? knownPlayerSpellId : decision.spellId) && player->IsInCombat())
        return false;

    // Reactive spells like Revenge can be known and off cooldown while still
    // being unusable because the required caster aura state is not active yet.
    // Treat that as not immediately castable so the selector picks another
    // candidate instead of logging SPELL_FAILED_CASTER_AURASTATE every tick.
    if (knownByPlayer && !MeetsCasterAuraStateRequirements(player, knownPlayerSpellId))
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

    if (breaksFlagCarry)
        if (Player const* targetPlayer = resolvedTarget->ToPlayer())
            if (playerbot::PvpCore::IsBattlegroundFlagCarrier(targetPlayer))
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
    if (powerCaster && !IsSpiritOfRedemptionFreeHeal(player, decision.spellId) && spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
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
constexpr uint32 kEnvironmentalMagmaDamageAuraId = 57634;

// Bots must never sit down to eat/drink while standing in lava/slime, and
// should not select eat/drink while actually submerged in water either --
// mirrors the hazard check used for movement escape in
// PlayerbotPvpLifecycleActions::IsInHazardousLiquid.
bool IsInHazardousLiquidForRecovery(Player const* player)
{
    if (!player)
        return false;

    Map const* map = player->FindMap();
    if (!map)
        return false;

    if (player->HasAura(kEnvironmentalMagmaDamageAuraId))
        return true;

    LiquidData liquidData{};
    ZLiquidStatus const status = map->GetLiquidStatus(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight());
    if ((status & MAP_LIQUID_STATUS_IN_CONTACT) == 0)
        return false;

    return (liquidData.type_flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
}

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
            if (player->HasTalent(81273, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Fury-like", false, false };
            if (player->HasTalent(23922, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Prot-like", false, false };
            break;
        case CLASS_PALADIN:
            if (player->HasTalent(20473, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Holy-like", false, false };
            if (player->HasTalent(20925, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Prot-like", false, false };
            if (player->HasTalent(20375, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Ret-like", false, false };
            break;
        case CLASS_HUNTER:
            if (player->HasTalent(81300, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "BM-like", false, false };
            if (player->HasTalent(19506, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "MM-like", false, false };
            if (player->HasTalent(19386, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "SV-like", false, false };
            break;
        case CLASS_ROGUE:
            if (player->HasTalent(81302, activeSpec))
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

bool PartyBenefitsFromWindfuryTotem(Player const* player)
{
    Group const* group = player ? player->GetGroup() : nullptr;
    if (!group)
        return false;

    for (GroupReference const* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player const* member = itr->GetSource();
        if (!member || member == player)
            continue;

        ClassicProfileSelection const memberProfile = DetectClassicClassProfile(member);
        if (member->GetClass() == CLASS_WARRIOR &&
            (memberProfile.profile == ClassicClassProfile::PrimaryClassic ||
             memberProfile.profile == ClassicClassProfile::SecondaryClassic))
            return true;

        if (member->GetClass() == CLASS_PALADIN && memberProfile.profile == ClassicClassProfile::TertiaryClassic)
            return true;
    }

    return false;
}

uint32 ResolveKnownPlayerSpellInChain(Player const* player, uint32 spellId)
{
    if (!player || !spellId)
        return 0;

    SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!baseSpellInfo)
        return 0;

    uint32 resolvedSpellId = 0;
    for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
    {
        if (player->HasSpell(chainSpellId))
            resolvedSpellId = chainSpellId;
    }

    return resolvedSpellId;
}

bool IsSpellReady(Player const* player, uint32 spellId)
{
    uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
    if (!resolvedSpellId)
        return false;

    return !player->GetSpellHistory()->HasCooldown(resolvedSpellId);
}

bool IsZeroManaCostSpellReady(Player const* player, uint32 spellId)
{
    uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
    if (!player || !resolvedSpellId || player->GetSpellHistory()->HasCooldown(resolvedSpellId))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(resolvedSpellId);
    if (!spellInfo || spellInfo->PowerType != POWER_MANA)
        return false;

    return spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask()) == 0;
}

bool MeetsCasterAuraStateRequirements(Player const* player, uint32 spellId)
{
    uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
    if (!player || !resolvedSpellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(resolvedSpellId);
    if (!spellInfo)
        return false;

    if (spellInfo->CasterAuraState && !player->HasAuraState(AuraStateType(spellInfo->CasterAuraState), spellInfo, player))
        return false;

    if (spellInfo->CasterAuraStateNot && player->HasAuraState(AuraStateType(spellInfo->CasterAuraStateNot), spellInfo, player))
        return false;

    if (spellInfo->CasterAuraSpell && !player->HasAura(sSpellMgr->GetSpellIdForDifficulty(spellInfo->CasterAuraSpell, player)))
        return false;

    if (spellInfo->ExcludeCasterAuraSpell && player->HasAura(sSpellMgr->GetSpellIdForDifficulty(spellInfo->ExcludeCasterAuraSpell, player)))
        return false;

    return true;
}

bool IsSpellReadyAndCasterAuraAllowed(Player const* player, uint32 spellId)
{
    return IsSpellReady(player, spellId) && MeetsCasterAuraStateRequirements(player, spellId);
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

bool IsDruidCasterForm(Player const* player)
{
    return player && player->GetClass() == CLASS_DRUID && player->GetShapeshiftForm() == FORM_NONE;
}

float GetPlayerbotTotemRefreshDistance(Creature const* totem)
{
    if (!totem || !totem->IsTotem())
        return kPlayerbotTotemRefreshDistance;

    float maxRadius = 0.0f;
    Totem const* totemUnit = totem->ToTotem();
    for (uint8 spellSlot = 0; spellSlot < MAX_CREATURE_SPELLS; ++spellSlot)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(totemUnit->GetSpell(spellSlot));
        if (!spellInfo)
            continue;

        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            if (spellEffectInfo.IsEffect() && spellEffectInfo.HasRadius())
                maxRadius = std::max(maxRadius, spellEffectInfo.CalcRadius(const_cast<Creature*>(totem)));
    }

    return maxRadius > 0.0f ? maxRadius : kPlayerbotTotemRefreshDistance;
}

bool HasActiveTotemInSlot(Player const* player, uint8 slot)
{
    if (!player || slot >= MAX_SUMMON_SLOT)
        return false;

    ObjectGuid const& summonGuid = player->m_SummonSlot[slot];
    if (!summonGuid)
        return false;

    Creature* totem = ObjectAccessor::GetCreature(*player, summonGuid);
    if (!totem || !totem->IsAlive())
        return false;

    return player->IsWithinDistInMap(totem, GetPlayerbotTotemRefreshDistance(totem));
}

bool HasActiveEarthTotem(Player const* player)
{
    return HasActiveTotemInSlot(player, SUMMON_SLOT_TOTEM_EARTH);
}

bool HasActiveWaterTotem(Player const* player)
{
    return HasActiveTotemInSlot(player, SUMMON_SLOT_TOTEM_WATER);
}

bool HasActiveAirTotem(Player const* player)
{
    return HasActiveTotemInSlot(player, SUMMON_SLOT_TOTEM_AIR);
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
    // Mounting must be conservative around WMO boundaries. A stale cached
    // outdoor bit must not override fresh terrain/WMO indoor classification (or
    // vice versa), otherwise bots mount inside bases and tunnels.
    return player->IsOutdoors() && terrainStatus.outdoors;
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

bool CanCastMountSpellAtCurrentLocation(Player const* player, SpellInfo const* mountSpellInfo)
{
    if (!CanAttemptMount(player, mountSpellInfo))
        return false;

    // A real client refuses to summon a mount while swimming. Bots have no
    // such client-side gate, so without this check they could cast a ground
    // mount while already standing in water and then never get dismounted
    // until their next relocation tick re-evaluates terrain status.
    if (player->IsInWater())
        return false;

    // Playerbot mount selection must honor current terrain/WMO classification
    // even for custom mount spells that omitted the normal outdoors-only spell
    // attribute. Otherwise the AI repeatedly chooses those spells in indoor
    // battleground bases although a normal player cannot mount there.
    return IsEffectivelyOutdoors(player);
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
        if (!CanCastMountSpellAtCurrentLocation(player, spellInfo))
            continue;

        return spellId;
    }

    return 0;
}

SpellDecision SelectMountSpell(Player const* player, char const* reason)
{
    SpellDecision decision;
    if (!player || !player->IsAlive() || player->IsInCombat() || player->IsMounted())
        return decision;

    if (IsHardControlled(player))
        return decision;

    if (IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT))
        if (SpellInfo const* defaultMountInfo = sSpellMgr->GetSpellInfo(SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT))
            if (CanCastMountSpellAtCurrentLocation(player, defaultMountInfo))
                return { "mount", reason, SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

    if (uint32 const knownMountSpellId = SelectReadyKnownMountSpell(player))
        return { "mount", reason, knownMountSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

    return decision;
}

SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player)
{
    SpellDecision decision;
    if (!player || !player->IsAlive() || player->IsInCombat() || player->IsMounted())
        return decision;

    // Spirit of Redemption makes priest healing free and time-limited. Do not
    // waste that window by selecting drink/eat/mount recovery.
    if (IsPriestInSpiritOfRedemption(player))
        return decision;

    // Do not attempt recovery/mount actions while hard controlled. This avoids
    // mount selections during fear/polymorph/stun/root states.
    if (IsHardControlled(player))
        return decision;

    // Never sit down to eat/drink while standing in lava/slime, or while
    // in water at all -- a real player cannot use food/drink items while
    // swimming (not just while fully submerged), and bots have no
    // client-side check to enforce this on their own.
    if (IsInHazardousLiquidForRecovery(player) || player->IsInWater())
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

    // During battleground preparation this selector only runs after
    // SelectPreparationBuffSpell() has no pet/buff actions left. At that point,
    // mount if the current spot is legal for the selected mount spell so bots
    // are ready to move as soon as the gates open.
    char const* mountReason = inBattlegroundPreparation ? "mount during preparation after prep actions" : "mount while out of combat";

    // Keep pressure logic responsive outside the prep phase: don't choose an
    // out-of-combat mount action while hostile players are already within
    // practical engage range.
    if (!inBattlegroundPreparation && HasNearbyAttackableEnemyPlayer(player, playerbot::PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE))
        return decision;

    return SelectMountSpell(player, mountReason);
}

bool HasHostileTarget(Player const* player, Unit const* target)
{
    return player && target && target != player && target->IsAlive() && player->IsValidAttackTarget(target);
}

bool IsFriendlySupportTarget(Player const* player, Unit const* target)
{
    if (!player || !target || !target->IsAlive())
        return false;

    Player const* targetPlayer = target->ToPlayer();
    if (targetPlayer && targetPlayer->IsSpectator())
        return false;

    if (target == player)
        return true;

    if (player->IsValidAssistTarget(target))
        return true;

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

bool HasAnyAuraFromSpellChain(Unit const* unit, std::initializer_list<uint32> baseSpellIds)
{
    for (uint32 baseSpellId : baseSpellIds)
        if (HasAuraFromSpellChain(unit, baseSpellId))
            return true;

    return false;
}

bool HasHunterDamagingStingFromCaster(Unit const* unit, ObjectGuid casterGuid)
{
    return HasAuraFromSpellChain(unit, 1978, casterGuid) || // Serpent Sting
        HasAuraFromSpellChain(unit, 3034, casterGuid) ||     // Viper Sting
        HasAuraFromSpellChain(unit, 3043, casterGuid);       // Scorpid Sting
}

bool HasHunterStingFromCaster(Unit const* unit, ObjectGuid casterGuid)
{
    return HasHunterDamagingStingFromCaster(unit, casterGuid) ||
        HasAuraFromSpellChain(unit, 19386, casterGuid);      // Wyvern Sting
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

ObjectGuid SelectFriendlyWithoutManaAndAuraFromSpellChain(Player const* player, uint32 baseSpellId, float maxDistance)
{
    if (!player || !player->FindMap() || !baseSpellId)
        return ObjectGuid::Empty;

    Player* bestTarget = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive())
            continue;
        if (candidate == player || candidate->GetMaxPower(POWER_MANA) > 0)
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        if (HasAuraFromSpellChain(candidate, baseSpellId))
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

ObjectGuid SelectFriendlyWithManaAndWithoutAuraFromSpellChain(Player const* player, uint32 baseSpellId, float maxDistance, bool includeSelf)
{
    if (!player || !player->FindMap() || !baseSpellId)
        return ObjectGuid::Empty;

    auto isEligible = [&](Player* candidate)
    {
        return candidate && candidate->IsAlive() && candidate->GetMaxPower(POWER_MANA) > 0 &&
            IsFriendlySupportTarget(player, candidate) &&
            player->IsWithinLOSInMap(candidate) && player->IsWithinDistInMap(candidate, maxDistance) &&
            !HasAuraFromSpellChain(candidate, baseSpellId);
    };

    if (includeSelf && isEligible(const_cast<Player*>(player)))
        return player->GetGUID();

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

ObjectGuid SelectFriendlyWithoutAnyAuraFromSpellChain(Player const* player, std::initializer_list<uint32> baseSpellIds, float maxDistance, bool includeSelf)
{
    if (!player || !player->FindMap() || baseSpellIds.size() == 0)
        return ObjectGuid::Empty;

    auto isEligible = [&](Player* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return false;
        if (!IsFriendlySupportTarget(player, candidate))
            return false;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            return false;
        if (HasAnyAuraFromSpellChain(candidate, baseSpellIds))
            return false;

        return true;
    };

    if (includeSelf && isEligible(const_cast<Player*>(player)))
        return player->GetGUID();

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

SpellDecision SelectMissingBattlegroundRaidBuff(Player const* player)
{
    if (!player || !player->InBattleground() || player->IsInCombat())
        return {};

    auto makeDecision = [player](char const* actionName, char const* reason, uint32 spellId,
        playerbot::PvpClassSpellContext::TargetMode targetMode, ObjectGuid targetGuid)
    {
        SpellDecision decision;
        if (targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally && targetGuid.IsEmpty())
            return decision;

        decision.actionName = actionName;
        decision.reason = reason;
        decision.spellId = spellId;
        decision.targetMode = targetMode;
        decision.targetGuid = targetGuid;
        return decision;
    };

    // Raid/group buffs are intentionally selected only while their actual
    // post-aura mana cost is zero. Battleground Preparation and the brief
    // post-resurrection mana-cost aura make these casts free; outside those
    // windows bots conserve mana instead of topping off buffs.
    //
    // Do not fall back to the cheaper single-target versions. If a raid buff
    // is free, always use the broader, normally more expensive version.
    switch (player->GetClass())
    {
        case CLASS_DRUID:
        {
            if (IsZeroManaCostSpellReady(player, 21850))
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAnyAuraFromSpellChain(player, { 9885, 21850 }, 45.0f, true); !targetGuid.IsEmpty())
                    return makeDecision("druid gift of the wild raid", "apply free raid-wide stat buff", 21850,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            if (IsZeroManaCostSpellReady(player, kDruidMassThornsSpellId))
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAnyAuraFromSpellChain(player,
                    { kDruidThornsSpellId, kDruidMassThornsSpellId }, 45.0f, true); !targetGuid.IsEmpty())
                    return makeDecision("druid mass thorns raid", "apply free raid-wide thorns buff", kDruidMassThornsSpellId,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            break;
        }
        case CLASS_PRIEST:
        {
            if (IsZeroManaCostSpellReady(player, 21564))
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAnyAuraFromSpellChain(player, { 10938, 21564 }, 45.0f, true); !targetGuid.IsEmpty())
                    return makeDecision("priest prayer of fortitude raid", "apply free raid-wide fortitude buff", 21564,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            if (IsZeroManaCostSpellReady(player, 27683))
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAnyAuraFromSpellChain(player, { 10958, 27683 }, 45.0f, true); !targetGuid.IsEmpty())
                    return makeDecision("priest prayer of shadow protection raid", "apply free raid-wide shadow protection buff", 27683,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            if (IsZeroManaCostSpellReady(player, 27681))
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAnyAuraFromSpellChain(player, { 27841, 27681 }, 45.0f, true); !targetGuid.IsEmpty())
                    return makeDecision("priest prayer of spirit raid", "apply free raid-wide spirit buff", 27681,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            break;
        }
        case CLASS_MAGE:
        {
            if (IsZeroManaCostSpellReady(player, 23028))
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAnyAuraFromSpellChain(player, { 10157, 23028 }, 45.0f, true); !targetGuid.IsEmpty())
                    return makeDecision("mage arcane brilliance raid", "apply free raid-wide intellect buff", 23028,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            break;
        }
        case CLASS_PALADIN:
        {
            bool const isRetPaladin = DetectClassicClassProfile(player).profile == ClassicClassProfile::TertiaryClassic;
            if (!isRetPaladin && IsZeroManaCostSpellReady(player, 25898))
                if (ObjectGuid targetGuid = SelectFriendlyWithoutAuraFromSpellChain(player, 25898, 45.0f, true); !targetGuid.IsEmpty())
                    return makeDecision("paladin greater blessing of kings raid", "apply free greater blessing to an unbuffed class", 25898,
                        playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid);

            // Ret keeps its deliberate single-target blessing behavior in the
            // normal paladin selector below. It never substitutes a Greater
            // Blessing here merely because the raid version is free.
            break;
        }
        default:
            break;
    }

    return {};
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
            if (DetectClassicClassProfile(player).profile == ClassicClassProfile::PrimaryClassic)
            {
                // Assassination: Wound Poison on the mainhand, Mind-Numbing
                // Poison on the offhand, instead of double Crippling Poison.
                // CastDirectSpell's temporary-weapon-imbue handling always
                // fills the mainhand slot first, then the offhand, so gating
                // each poison on the other slot's enchant state sequences
                // them onto the correct weapon.
                Item const* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
                bool const mainHandUnenchanted = mainHand && !mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
                Item const* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
                bool const offHandUnenchanted = offHand && !offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);

                if (mainHandUnenchanted && IsSpellReady(player, 13227))
                    return { "rogue wound poison prep", "coat mainhand with wound poison before gates open", 13227, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

                if (!mainHandUnenchanted && offHandUnenchanted && IsSpellReady(player, 11399))
                    return { "rogue mind-numbing poison prep", "coat offhand with mind-numbing poison before gates open", 11399, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

                break;
            }

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
        case CLASS_HUNTER:
        {
            HunterPetDecisionState const petState = GetHunterPetDecisionState(player);
            bool const hasDeadPet = petState.hasDeadPet || petState.shouldRevivePet;
            if (hasDeadPet &&
                !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterRevivePetSpellId) &&
                IsSpellReady(player, kHunterRevivePetSpellId))
                return { "hunter revive pet prep", "revive dead hunter pet before gates open", kHunterRevivePetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (!petState.hasLivingPet && !hasDeadPet && petState.canCallPet &&
                !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterCallPetSpellId) &&
                IsSpellReady(player, kHunterCallPetSpellId))
                return { "hunter call pet prep", "call active stable pet before gates open", kHunterCallPetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_DRUID:
        case CLASS_PRIEST:
            // Raid buffs are handled by SelectMissingBattlegroundRaidBuff()
            // after class-specific preparation actions.
            break;
        case CLASS_MAGE:
        {
            if (IsSpellReady(player, 10054) && !player->HasItemCount(8008))
                return { "create mana ruby prep", "create mana ruby before gates open", 10054, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (!HasAuraFromSpellChain(player, 10220) && IsSpellReady(player, 10220))
                return { "frost armor prep", "maintain armor before gates open", 10220, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_PALADIN:
        {
            ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
            bool const isRetPaladin = profileSelection.profile == ClassicClassProfile::TertiaryClassic;

            if (!isRetPaladin)
                break;

            if (IsSpellReady(player, 25290))
            {
                if (ObjectGuid targetGuid = SelectFriendlyWithManaAndWithoutAuraFromSpellChain(player, 25290, 45.0f, false); !targetGuid.IsEmpty())
                    return { "paladin blessing of wisdom prep", "ret paladin manually buffs nearby mana allies", 25290, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            if (IsSpellReady(player, 25291))
            {
                if (!HasAuraFromSpellChain(player, 25291))
                    return { "paladin blessing of might self prep", "ret paladin prefers might before gates open", 25291, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

                if (ObjectGuid targetGuid = SelectFriendlyWithoutManaAndAuraFromSpellChain(player, 25291, 45.0f); !targetGuid.IsEmpty())
                    return { "paladin blessing of might prep", "buff nearby non-mana allies before gates open", 25291, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            break;
        }
        case CLASS_WARLOCK:
        {
            Pet const* pet = player->GetPet();
            if (!pet || !pet->IsAlive())
            {
                ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
                bool const isAfflictionWarlock = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
                bool const isDestructionWarlock = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
                uint32 const summonPetSpell = isAfflictionWarlock ? 691 : (isDestructionWarlock ? 712 : 697);
                char const* summonPetName = isAfflictionWarlock ? "warlock summon felhunter prep" : (isDestructionWarlock ? "warlock summon succubus prep" : "warlock summon voidwalker prep");
                char const* summonPetReason = isAfflictionWarlock ? "summon felhunter before gates open" : (isDestructionWarlock ? "summon succubus before gates open" : "summon voidwalker before gates open");
                if (IsSpellReady(player, summonPetSpell))
                    return { summonPetName, summonPetReason, summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };
            }

            // The player knows Ritual of Souls; its triggered Create Soulwell
            // effect is selected directly so virtual teammates do not have to
            // emulate a three-client summoning ritual during the short prep
            // window. The normal summoned Soulwell object, ownership, lifetime,
            // and charge counter are still used.
            if (player->HasSpell(kWarlockRitualOfSoulsSpellId) &&
                !playerbot::PvpCore::FindUsableSoulwell(player, 60.0f))
                return { "warlock create soulwell prep", "create a team healthstone well before gates open",
                    kWarlockCreateSoulwellSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        default:
            break;
    }

    if (SpellDecision const raidBuffDecision = SelectMissingBattlegroundRaidBuff(player); raidBuffDecision.spellId)
        return raidBuffDecision;

    if (SpellDecision const mountDecision = SelectMountSpell(player, "mount during preparation after prep actions"); mountDecision.spellId)
        return mountDecision;

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

bool IsTargetEffectivelyImmune(Player const* player, Unit const* target);
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

Unit const* SelectWarriorTauntTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
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

    auto isPressuringFriendly = [&](Unit const* candidate)
    {
        if (!isCandidateUsable(candidate))
            return false;

        Unit const* victim = candidate->GetVictim();
        return victim && victim != player && IsFriendlySupportTarget(player, victim);
    };

    if (isPressuringFriendly(preferredTarget))
        return preferredTarget;

    Unit const* bestPressureTarget = nullptr;
    float bestPressureDistance = std::numeric_limits<float>::max();
    Unit const* bestFallbackTarget = nullptr;
    float bestFallbackDistance = std::numeric_limits<float>::max();

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isCandidateUsable(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (isPressuringFriendly(candidate) && distance < bestPressureDistance)
        {
            bestPressureTarget = candidate;
            bestPressureDistance = distance;
        }

        if (candidate->GetVictim() != player && distance < bestFallbackDistance)
        {
            bestFallbackTarget = candidate;
            bestFallbackDistance = distance;
        }
    }

    if (bestPressureTarget)
        return bestPressureTarget;

    if (isCandidateUsable(preferredTarget) && preferredTarget->GetVictim() != player)
        return preferredTarget;

    return bestFallbackTarget;
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

Unit const* SelectEnemyTargetInSpellRange(Player const* player, Unit const* preferredTarget, uint32 spellId)
{
    if (!player || !player->FindMap())
        return nullptr;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return nullptr;

    float const minRange = spellInfo->GetMinRange(false);
    float const maxRange = spellInfo->GetMaxRange(false);

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        if (!HasHostileTarget(player, candidate) || IsTargetInvalidByImmunity(player, candidate))
            return false;
        if (!player->IsWithinLOSInMap(candidate))
            return false;
        if (minRange > 0.0f && player->IsWithinDistInMap(candidate, minRange))
            return false;
        if (maxRange > 0.0f && !player->IsWithinDistInMap(candidate, maxRange))
            return false;

        return player->IsValidAttackTarget(candidate, spellInfo);
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

Unit const* SelectEnemyGapCloserTarget(Player const* player, Unit const* preferredTarget, float minDistance, float maxDistance, bool preferFarthest)
{
    if (!player || !player->FindMap())
        return nullptr;

    auto isCandidateUsable = [&](Unit const* candidate)
    {
        if (!HasHostileTarget(player, candidate) || IsTargetInvalidByImmunity(player, candidate))
            return false;
        if (HasBreakableCrowdControl(candidate))
            return false;
        if (!player->IsWithinLOSInMap(candidate))
            return false;

        float const distance = player->GetExactDist(candidate);
        return distance >= minDistance && distance <= maxDistance;
    };

    if (isCandidateUsable(preferredTarget))
        return preferredTarget;

    Unit const* best = nullptr;
    float bestDistance = preferFarthest ? -1.0f : std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!isCandidateUsable(candidate))
            continue;

        float const distance = player->GetExactDist(candidate);
        if ((preferFarthest && distance > bestDistance) || (!preferFarthest && distance < bestDistance))
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

bool IsTargetEffectivelyImmune(Player const* player, Unit const* target)
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

    return false;
}

bool IsTargetInvalidByImmunity(Player const* player, Unit const* target)
{
    return IsTargetEffectivelyImmune(player, target) || HasBreakableCrowdControl(target);
}

Unit const* SelectClosestEnemyTarget(Player const* player, bool requireReachable)
{
    if (!player || !player->FindMap())
        return nullptr;

    // Gather attackable candidates with the cheap checks first, then spend
    // immunity scans and line-of-sight raycasts nearest-first. The selected
    // target is identical to evaluating every candidate, but usually only the
    // closest one or two ever pay for a raycast.
    std::vector<std::pair<float, Player const*>> candidates;
    candidates.reserve(16);
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive() || candidate == player)
            continue;
        if (!player->IsValidAttackTarget(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (requireReachable && distance > 35.0f)
            continue;

        candidates.emplace_back(distance, candidate);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](std::pair<float, Player const*> const& left, std::pair<float, Player const*> const& right)
    {
        return left.first < right.first;
    });

    for (auto const& [distance, candidate] : candidates)
    {
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate))
            continue;

        return candidate;
    }

    return nullptr;
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

Unit const* SelectFriendlyHealthTarget(Player const* player, float maxDistance, float maxHealthPct, uint32 excludedAuraId = 0)
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
        if (excludedAuraId && candidate->HasAura(excludedAuraId))
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

Unit const* SelectFriendlyLowestHealthTarget(Player const* player, float maxDistance, float maxHealthPct, uint32 excludedAuraId = 0, bool includeSelf = true)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestHealth = 101.0f;
    float bestDistance = std::numeric_limits<float>::max();

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive())
            return;
        if (!includeSelf && candidate == player)
            return;
        if (!IsFriendlySupportTarget(player, candidate))
            return;
        if (excludedAuraId && candidate->HasAura(excludedAuraId))
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

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best;
}

// Finds the ally with the greatest distance strictly inside the supplied range,
// i.e. the ally closest to the range cap without violating the spell's minimum.
// Used by escape/leap abilities that need a valid friendly destination.
Unit const* SelectFriendlyNearRangeCapTarget(Player const* player, float minDistance, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = -1.0f;

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive() || candidate == player)
            return;
        if (!IsFriendlySupportTarget(player, candidate))
            return;

        float const distance = player->GetDistance(candidate);
        if (distance <= minDistance || distance >= maxDistance || !player->IsWithinLOSInMap(candidate))
            return;

        if (distance > bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    };

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best;
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


bool IsStealthMovementPenalty(SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->HasAura(SPELL_AURA_MOD_STEALTH);
}

bool HasNonStealthDecreaseSpeedAura(Unit const* unit)
{
    Unit::AuraEffectList const& slowAuras = unit->GetAuraEffectsByType(SPELL_AURA_MOD_DECREASE_SPEED);
    for (AuraEffect const* slowAura : slowAuras)
        if (slowAura && !IsStealthMovementPenalty(slowAura->GetSpellInfo()))
            return true;

    return false;
}

bool HasNonStealthRootOrSnareMechanic(Unit const* unit)
{
    uint32 const rootOrSnareMask = (1 << MECHANIC_ROOT) | (1 << MECHANIC_SNARE);

    for (Unit::AuraApplicationMap::value_type const& appliedAura : unit->GetAppliedAuras())
    {
        AuraApplication const* aurApp = appliedAura.second;
        SpellInfo const* spellInfo = aurApp ? aurApp->GetBase()->GetSpellInfo() : nullptr;
        if (!spellInfo || IsStealthMovementPenalty(spellInfo))
            continue;

        if (spellInfo->Mechanic && (rootOrSnareMask & (1 << spellInfo->Mechanic)))
            return true;

        for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
            if (aurApp->HasEffect(spellEffectInfo.EffectIndex) && spellEffectInfo.IsEffect() && spellEffectInfo.Mechanic &&
                (rootOrSnareMask & (1 << spellEffectInfo.Mechanic)))
                return true;
    }

    return false;
}

bool IsRootedOrSnared(Unit const* unit)
{
    if (!unit)
        return false;

    return unit->HasUnitState(UNIT_STATE_ROOT) ||
        unit->HasAuraType(SPELL_AURA_MOD_ROOT) ||
        HasNonStealthDecreaseSpeedAura(unit) ||
        HasNonStealthRootOrSnareMechanic(unit);
}


bool IsMageBlinkableControl(Player const* player)
{
    if (!player || player->GetClass() != CLASS_MAGE)
        return false;

    constexpr uint32 blinkableMechanicMask =
        (1u << MECHANIC_STUN) |
        (1u << MECHANIC_ROOT);

    constexpr uint32 nonBlinkableControlMask =
        (1u << MECHANIC_CHARM) |
        (1u << MECHANIC_DISORIENTED) |
        (1u << MECHANIC_FEAR) |
        (1u << MECHANIC_SLEEP) |
        (1u << MECHANIC_FREEZE) |
        (1u << MECHANIC_POLYMORPH) |
        (1u << MECHANIC_BANISH) |
        (1u << MECHANIC_HORROR) |
        (1u << MECHANIC_SAPPED);

    bool const blinkableControl =
        player->HasUnitState(UNIT_STATE_STUNNED) ||
        player->HasUnitState(UNIT_STATE_ROOT) ||
        player->HasAuraType(SPELL_AURA_MOD_ROOT) ||
        player->HasAuraWithMechanic(blinkableMechanicMask);

    if (!blinkableControl)
        return false;

    bool const hardNonBlinkableControl =
        player->HasUnitState(UNIT_STATE_CONFUSED) ||
        player->HasUnitState(UNIT_STATE_FLEEING) ||
        player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->HasAuraWithMechanic(nonBlinkableControlMask) ||
        player->IsPolymorphed();

    return !hardNonBlinkableControl;
}

bool IsHunterBestialWrathBreakableControl(Player const* player)
{
    if (!player || player->GetClass() != CLASS_HUNTER ||
        !player->HasTalent(81300, player->GetActiveSpec()))
        return false;

    // Only genuine incapacitating crowd control -- stun/fear/confuse/polymorph
    // and similar states that stop the bot from acting at all -- should burn
    // this. IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK is the real
    // PvP-trinket mechanic mask, so it (correctly, for a trinket) also
    // includes MECHANIC_ROOT and MECHANIC_SNARE; combined with the separate
    // IsRootedOrSnared() check below, a mere root or snare counted as
    // "breakable control" here. Arena Preparation itself applies a root to
    // hold players in place before the gates open, so this fired Bestial
    // Wrath the instant the match started rather than only during real CC.
    return player->HasUnitState(UNIT_STATE_CONTROLLED | UNIT_STATE_POSSESSED | UNIT_STATE_STUNNED |
            UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
        player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->IsPolymorphed();
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


bool HasActiveMovementEffectSpline(Player const* player)
{
    if (!player)
        return false;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    if (!motionMaster || motionMaster->GetCurrentMovementGeneratorType() != EFFECT_MOTION_TYPE)
        return false;

    bool const hasActiveSpline = player->movespline && player->movespline->Initialized() && !player->movespline->Finalized();
    return hasActiveSpline || player->HasUnitState(UNIT_STATE_CHARGING) || player->isMoving();
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

uint32 CountNearbyMeleeThreats(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!HasHostileTarget(player, candidate) || !IsMeleeClass(candidate))
            continue;
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        ++count;
    }

    return count;
}

uint32 CountNearbyFriendlyPlayers(Player const* player, float maxDistance, bool includeSelf = true, bool requireLineOfSight = true)
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
        if (!includeSelf && candidate == player)
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if ((requireLineOfSight && !player->IsWithinLOSInMap(candidate)) || !player->IsWithinDistInMap(candidate, maxDistance))
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

    // Exact 5-8y is the classic hunter dead-zone: melee cannot reach, but
    // ranged weapon shots also fail. Still allow Feign Death -> trap setup here;
    // otherwise the movement layer owns the high-priority retreat directive.
    Unit const* activeDeadZoneTarget = SelectHunterDeadZoneEnemy(player, activeTarget);
    bool const activeTargetDeadZone = activeDeadZoneTarget != nullptr;

    HunterPetDecisionState const petState = GetHunterPetDecisionState(player);
    bool const hasLivingPet = petState.hasLivingPet;
    bool const hasDeadPet = petState.hasDeadPet || petState.shouldRevivePet;
    bool const canCallPet = !hasLivingPet && !hasDeadPet && petState.canCallPet &&
        !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterCallPetSpellId);
    bool const shouldRevivePet = hasDeadPet &&
        !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterRevivePetSpellId);

    Unit const* enemyOnTopTarget = SelectNearbyEnemyTarget(player, activeTarget, 5.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 20.0f, activeTarget);
    Unit const* closeMeleeThreat = SelectNearbyMeleeTarget(player, enemyOnTopTarget, 5.0f);
    Unit const* trapSetupTarget = SelectNearbyEnemyTarget(player, enemyOnTopTarget ? enemyOnTopTarget : activeTarget, 8.0f);
    Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, GetConfiguredLongRange());
    HunterPvpSpec const hunterSpec = GetHunterPvpSpec(profileSelection);
    bool const isSurvivalHunter = hunterSpec == HunterPvpSpec::Survival;
    bool const isMarksmanshipHunter = hunterSpec == HunterPvpSpec::Marksmanship;
    bool const isBeastMasteryHunter = hunterSpec == HunterPvpSpec::BeastMastery;
    Pet* bmPet = isBeastMasteryHunter ? player->GetPet() : nullptr;
    bool const bmPetAttacking = bmPet && bmPet->IsAlive() && bmPet->GetVictim();
    float const bmPetDistance = bmPet && bmPet->IsAlive() ? player->GetDistance(bmPet) : -1.0f;
    bool const bmPetAtUsefulSwapPosition = bmPetDistance >= 8.0f && bmPetDistance <= 30.0f &&
        player->IsWithinLOSInMap(bmPet) &&
        std::abs(player->GetPositionZ() - bmPet->GetPositionZ()) <= 20.0f;
    bool const bmCrowdControlled = isBeastMasteryHunter && IsHunterBestialWrathBreakableControl(player);
    bool const hasMongooseBite = ResolveKnownPlayerSpellInChain(player, 81285) != 0;
    bool const hasBitePrimerOnKillTarget = hasMongooseBite && activeTarget &&
        HasHunterDamagingStingFromCaster(activeTarget, player->GetGUID());
    // Must also require melee range: this flag suppresses every ranged
    // filler below (Concussive Shot, Arcane Shot, Multi-Shot, Viper Sting)
    // whenever it is true. Without the range check it would go true purely
    // from having a Sting up, making the hunter hold all ranged pressure for
    // a bite it had not reached yet. The movement layer closes whenever the
    // learned bite is ready; fillers remain available until melee is reached.
    bool const readyToBiteKillTarget = hasBitePrimerOnKillTarget && !IsRootedOrSnared(player) &&
        activeTarget && player->IsWithinMeleeRange(activeTarget);
    Unit const* manaTarget = isSurvivalHunter
        ? SelectNearbyEnemyManaTarget(player, activeTarget, GetConfiguredLongRange(), 0.0f)
        : SelectNearbyEnemyTarget(player, activeTarget, GetConfiguredLongRange());
    Unit const* wyvernTarget = (isSurvivalHunter && IsSpellReady(player, 24133) && !AnyEnemyWyvernStung(player, 40.0f))
        ? SelectWyvernStingTarget(player, activeTarget, 30.0f)
        : nullptr;

    target = activeTarget;
    bool const targetBreakableCrowdControl = target && HasBreakableCrowdControl(target);
    bool const targetClose = player->IsWithinDistInMap(target, kReferenceHunterSwitchDistance);
    bool const enemyOnTop = HasHostileTarget(player, enemyOnTopTarget);
    bool const trapSetupThreat = HasHostileTarget(player, trapSetupTarget);
    bool const enemyNear = player->IsWithinDistInMap(target, GetConfiguredCloseRange());
    bool const rangedMode = IsHunterInRangedMode(player);
    uint32 const preferredTrapSpellId = trapSetupTarget && HasDotAura(trapSetupTarget) ? uint32(13809) : uint32(14311);
    bool const preferredTrapReady = IsSpellReady(player, preferredTrapSpellId);
    // Traps are only legal out of combat. Feign Death is allowed as a quick
    // defensive/trap-setup attempt, but it must never put hunter movement into a
    // "wait for trap" state. If combat does not actually drop, the next tick
    // should immediately fall through to Wing Clip / Counterattack / flee / shots.
    bool const canFeignUnderPressure = trapSetupThreat && player->IsInCombat() &&
        IsSpellReady(player, 5384) && !HasAuraFromSpellChain(player, 5384);
    bool const canDropTrapNow = trapSetupThreat && !player->IsInCombat() && preferredTrapReady;

    bool const targetSnaredOrStunned = target &&
        (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
         target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN)));

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HealthBelowPct(35) && IsSpellReady(player, 19263), 35.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, canFeignUnderPressure, 36.0f,
        { "hunter feign death", "drop combat under melee/dead-zone pressure", 5384, playerbot::PvpClassSpellContext::TargetMode::Self, trapSetupTarget ? trapSetupTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, canDropTrapNow, 35.75f,
        { preferredTrapSpellId == 13809 ? "hunter frost trap" : "hunter freezing trap", "drop trap after feign death setup", preferredTrapSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, trapSetupTarget ? trapSetupTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isSurvivalHunter && IsSpellReady(player, 23989) && !HasAuraFromSpellChain(player, 19263) && !IsSpellReady(player, 19263), 34.0f,
        { "hunter readiness", "reset cooldowns after deterrence has fallen", 23989, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isSurvivalHunter && enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) && IsSpellReadyAndCasterAuraAllowed(player, 20910), 34.5f,
        { "hunter counterattack", "strike any enemy in melee range after a parry", 20910, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isSurvivalHunter && wyvernTarget && IsSpellReady(player, 24133), 30.0f,
        { "hunter wyvern sting", "crowd-control a non-dotted enemy support target", 24133, playerbot::PvpClassSpellContext::TargetMode::Enemy, wyvernTarget ? wyvernTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 14325) && IsSpellReady(player, 14325), 29.5f,
        { "hunter mark", "mark rogue targets for anti-stealth pressure", 14325, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && !HasAuraFromSpellChain(player, 20906) && IsSpellReady(player, 20906), 27.5f,
        { "hunter trueshot aura", "maintain personal buff aura", 20906, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, canCallPet && IsSpellReady(player, kHunterCallPetSpellId), 26.0f,
        { "hunter call pet", "summon active stable pet when no living pet is present", kHunterCallPetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldRevivePet && IsSpellReady(player, kHunterRevivePetSpellId), 25.0f,
        { "hunter revive pet", "revive dead hunter pet instead of repeatedly calling it", kHunterRevivePetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && enemyOnTop && enemyOnTopTarget->HasUnitState(UNIT_STATE_CASTING) && !HasBreakableCrowdControl(enemyOnTopTarget) && IsSpellReady(player, 19503), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && nearbyCastingTarget && !HasBreakableCrowdControl(nearbyCastingTarget) && IsSpellReady(player, 19503), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates,
        enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) &&
            IsSpellReady(player, 14268) && !HasAuraFromSpellChain(enemyOnTopTarget, 14268),
        21.0f,
        { "hunter wing clip", "close-range fallback snare", 14268, playerbot::PvpClassSpellContext::TargetMode::Enemy,
            enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && !targetBreakableCrowdControl && !targetClose && !targetSnaredOrStunned && IsSpellReady(player, 5116), 20.0f,
        { "hunter concussive shot", "kite or chase control", 5116, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !activeTargetDeadZone && !targetBreakableCrowdControl && (isSurvivalHunter || isBeastMasteryHunter) && activeTarget && activeTarget->GetPowerType() != POWER_MANA &&
        !HasHunterStingFromCaster(activeTarget, player->GetGUID()) && IsSpellReady(player, 25295), 19.75f,
        { "hunter serpent sting", "apply ranged dot pressure to non-mana kill target", 25295, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !activeTargetDeadZone && rogueTarget && !HasBreakableCrowdControl(rogueTarget) && !HasAuraFromSpellChain(rogueTarget, 25295) && IsSpellReady(player, 25295), 19.5f,
        { "hunter serpent sting", "apply ranged dot pressure", 25295, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !activeTargetDeadZone && !targetBreakableCrowdControl && isMarksmanshipHunter && rangedMode && !enemyNear && IsSpellReady(player, 20904), 18.0f,
        { "hunter aimed shot", "long cast pressure from range", 20904, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, hasBitePrimerOnKillTarget && activeTarget && player->IsWithinMeleeRange(activeTarget) && IsSpellReady(player, 81285), 27.0f,
        { "hunter mongoose bite", "consume hunter sting for melee burst", 81285, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && !targetBreakableCrowdControl && (isSurvivalHunter || isBeastMasteryHunter) && rangedMode && !inMelee && activeTarget && IsSpellReady(player, 14287), 17.5f,
        { "hunter arcane shot", "instant pressure on kill target", 14287, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && !targetBreakableCrowdControl && rangedMode && !inMelee && IsSpellReady(player, 25294), 17.0f,
        { "hunter multi-shot", "ranged burst pressure", 25294, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && rangedMode && !inMelee && IsSpellReady(player, 3045), 16.0f,
        { "hunter rapid fire", "burst cooldown while freecasting at range", 3045, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && manaTarget && !HasBreakableCrowdControl(manaTarget) && manaTarget->GetPowerType() == POWER_MANA && !HasAuraFromSpellChain(manaTarget, 14280) && IsSpellReady(player, 14280), 15.0f,
        { "hunter viper sting", "drain mana on mana users", 14280, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaTarget ? manaTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && enemyOnTop && (!IsSpellReady(player, 5384) || !IsSpellReady(player, 14311)) && IsSpellReady(player, 19503) && !HasBreakableCrowdControl(enemyOnTopTarget), 14.0f,
        { "hunter scatter shot", "fallback peel when trap setup unavailable", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, enemyOnTop && closeMeleeThreat && (isSurvivalHunter || !IsSpellReady(player, 19503)) && (!IsSpellReady(player, 5384) || !preferredTrapReady) && IsSpellReady(player, 19263), 13.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, bmCrowdControlled && IsSpellReady(player, 81300), 100.0f,
        { "hunter bestial wrath", "break any removable crowd-control effect", 81300, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isBeastMasteryHunter && bmPetAttacking && activeTarget && IsSpellReady(player, 19577), 28.5f,
        { "hunter intimidate", "stun the kill target whenever the pet is attacking", 19577, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isBeastMasteryHunter && bmPetAtUsefulSwapPosition &&
        (enemyOnTop || activeTargetDeadZone || IsRootedOrSnared(player)) && IsSpellReady(player, 81297), 36.5f,
        { "hunter outmaneuver", "swap to the pet's safe position under movement or melee pressure", 81297, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasMongooseBite && enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) && IsSpellReady(player, 81285), 24.0f,
        { "hunter mongoose bite", "bite the nearest attacker under melee pressure", 81285, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    // The old unconditional fallback candidate here (no melee-range check,
    // relying on SelectHighestPriorityCastableDecision's uncastable-decision
    // fallback to force approach movement) is gone: readyToBiteKillTarget
    // now requires melee range itself, which makes it identical to the
    // melee-range-gated candidate above minus the root/snare check, so it
    // never added anything and only forced the bot to path in from any
    // range instead of holding the BM weave profile (see
    // GetCombatPositioningProfile/DriveHunterKiteLoop in
    // PlayerbotPvpLifecycleActions.cpp) and closing to melee naturally.

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectMageSpell(Player const* player, Unit const* target, bool inMelee, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const closePressure = hasHostileTarget && player->IsWithinDistInMap(target, GetConfiguredMeleeRange());
    bool const blinkableControl = IsMageBlinkableControl(player);
    float const manaPct = player->GetPowerPct(POWER_MANA);
    bool const isFireMage = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const isArcaneMage = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    Unit const* cursedTarget = IsSpellReady(player, 475) ? SelectFriendlyCurseTarget(player, 40.0f) : nullptr;
    Unit const* castingTarget = IsSpellReady(player, 2139) ? SelectEnemyCastingTarget(player, 30.0f, target) : nullptr;
    Unit const* polymorphTarget =
        (IsSpellReady(player, 12826) && !AnyEnemyPolymorphed(player, 40.0f)) ? SelectPolymorphTarget(player, target, 30.0f) : nullptr;
    // Arcane Power and Presence of Mind are off the global cooldown, so a
    // human player fires all three of these back to back. This engine can
    // only return one decision per Execute() call, so the three steps are
    // sequenced by priority/aura-state instead: each becomes castable only
    // once the previous one has landed. ProcessLifecycleEntryPoint's cadence
    // bypass (triggered on spellId 12042/12043) collapses those separate
    // calls back-to-back within the same overall update instead of waiting
    // out the normal decision cadence between them.
    bool const arcaneBurstWindow = isArcaneMage && target && target->HealthBelowPct(50);
    bool const arcanePowerReady = arcaneBurstWindow && !HasAuraFromSpellChain(player, 12042) && IsSpellReady(player, 12042);
    bool const presenceOfMindReady = arcaneBurstWindow && player->HasAura(12042) && !player->HasAura(12043) && IsSpellReady(player, 12043);
    bool const burstPyroblastReady = arcaneBurstWindow && player->HasAura(12043) && IsSpellReady(player, 18809);
    return SelectFromTriggerGraph(player, target, nullptr,
    {
        { "critical health", !isFireMage && player->HealthBelowPct(25) && IsSpellReady(player, 11958), 60.0f,
            { "mage ice block", "self-preservation emergency", 11958, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "mage is stunned or rooted", blinkableControl && IsSpellReady(player, 1953), 61.0f,
            { "mage blink", "escape stun/root control", 1953, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "arcane burst window", arcanePowerReady, 57.0f,
            { "mage arcane power", "open the sub-50-percent burst window", 12042, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "arcane burst window", presenceOfMindReady, 56.8f,
            { "mage presence of mind", "queue an instant pyroblast during the burst window", 12043, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "arcane burst window", burstPyroblastReady, 56.6f,
            { "mage pyroblast", "instant burst finisher under presence of mind", 18809, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "enemy too close for spell", closePressure && IsSpellReady(player, 1953), 45.0f,
            { "mage blink", "escape melee pressure", 1953, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "enemy is casting", castingTarget && IsSpellReady(player, 2139), 44.0f,
            { "mage counterspell", "interrupt any enemy cast in range", 2139, playerbot::PvpClassSpellContext::TargetMode::Enemy, castingTarget ? castingTarget->GetGUID() : ObjectGuid::Empty } },
        { "enemy too close for spell", closePressure && IsSpellReady(player, 10230), 43.0f,
            { "mage frost nova", "close defensive peel", 10230, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "enemy too close for spell", closePressure && target && IsMeleeClass(target) && IsSpellReady(player, isFireMage ? uint32(33041) : uint32(10161)), 42.0f,
            { isFireMage ? "mage dragon's breath" : "mage cone of cold", isFireMage ? "disorient nearby melee pressure" : "defensive snare versus nearby melee", isFireMage ? uint32(33041) : uint32(10161), playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "low mana", manaPct < 25.0f && IsSpellReady(player, 12051), 41.0f,
            { "mage evocation", "recover mana below 25 percent", 12051, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "high mana", manaPct < 50.0f && IsOnUseItemReady(player, kMageManaRubyItemId) &&
            !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kMageManaRubyUseSpellId), 40.0f,
            { "use mana ruby", "consume mana ruby below 50 percent mana", kMageManaRubyUseSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), kMageManaRubyItemId } },
        { "remove curse", cursedTarget != nullptr, 39.0f,
            { "remove lesser curse", "dispel curse from friendly target", 475, (cursedTarget == player) ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty } },
        { "ice barrier", !isFireMage && !HasAuraFromSpellChain(player, 13033) && IsSpellReady(player, 13033), 35.0f,
            { "mage ice barrier", "maintain defensive absorb shield", 13033, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "mana shield", isArcaneMage && closePressure && !HasAuraFromSpellChain(player, 10193) && IsSpellReady(player, 10193), 34.5f,
            { "mage mana shield", "absorb physical damage while being hit in melee", 10193, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "enemy low health", hasHostileTarget && target && IsSpellReady(player, 10199), 30.0f,
            { "mage fire blast", isFireMage ? "instant fire blast pressure on cooldown" : "instant execute pressure on low health target", 10199, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "clustered enemies", isFireMage && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 13021), 29.5f,
            { "mage blast wave", "area fire pressure against nearby enemies", 13021, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "polymorph", polymorphTarget && !polymorphTarget->HealthBelowPct(75), 29.0f,
            { "mage polymorph", "priority crowd control on non-dotted paladin/priest targets", 12826, playerbot::PvpClassSpellContext::TargetMode::Enemy, polymorphTarget ? polymorphTarget->GetGUID() : ObjectGuid::Empty } },
        { "default ranged", hasHostileTarget && IsSpellReady(player, (isFireMage || isArcaneMage) ? uint32(10207) : uint32(25304)), 18.0f,
            { (isFireMage || isArcaneMage) ? "mage scorch" : "mage frostbolt", isFireMage ? "default fire pressure" : (isArcaneMage ? "scorch instead of frostbolt" : "default ranged pressure"), (isFireMage || isArcaneMage) ? uint32(10207) : uint32(25304), playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "maintain buff", !player->IsInCombat() && IsSpellReady(player, 10220) && !player->HasAura(10220), 9.0f,
            { "frost armor", "frost armor", 10220, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "mana gem missing", !player->IsInCombat() && IsSpellReady(player, 10054) && !player->HasItemCount(8008), 8.0f,
            { "create mana ruby", "create mana ruby outside combat", 10054, playerbot::PvpClassSpellContext::TargetMode::Self } },
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

    // Spirit of Redemption is a short pure-healing window. Do not let the normal
    // priest fallback graph pick buffs, offensive casts, or arbitrary explicit
    // ally targets here. Always pick the lowest-health injured ally in heal range,
    // and do nothing if nobody is actually injured.
    if (IsPriestInSpiritOfRedemption(player))
    {
        Unit const* spiritHealTarget = IsSpellReady(player, 10917) ? SelectFriendlyLowestHealthTarget(player, GetConfiguredHealRange(), 99.0f, 0, false) : nullptr;
        if (spiritHealTarget)
            return { "priest flash heal", "heal lowest-health ally during spirit of redemption", 10917, playerbot::PvpClassSpellContext::TargetMode::Ally, spiritHealTarget->GetGUID() };

        return {};
    }

    // While Fade/Shadow Wraith (89784) is active, the priest's own body is
    // rooted and control has transferred to a possessed wraith creature (see
    // spell_pri_shadow_wraith_aura in spell_priest.cpp). The whole point of
    // using it is to escape melee pressure, but nothing in the normal
    // decision graph below ever issues the wraith a movement order - only
    // the (now rooted) player - so steer it explicitly here instead. The
    // drain life visual/channel during this window is already handled by
    // the aura script itself, so there is nothing else useful to decide
    // while shifted.
    if (player->HasAura(89784))
    {
        Unit const* wraithThreat = SelectNearbyMeleeTarget(player, target, 20.0f);
        playerbot::PvpClassActions::TryIssueShadowWraithFleeMovement(const_cast<Player*>(player),
            const_cast<Unit*>(wraithThreat ? wraithThreat : target));
        return decision;
    }

    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    Unit const* debuffedAlly = (!dispelThrottleActive && IsSpellReady(player, 988)) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, GetConfiguredHealRange()) : nullptr;
    Unit const* enemyBuffedTarget = (!dispelThrottleActive && IsSpellReady(player, 988) && hasHostileTarget) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, GetConfiguredSpellRange()) : nullptr;
    Unit const* shieldTarget = IsSpellReady(player, 10901) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 50.0f, kPriestWeakenedSoulSpellId) : nullptr;
    Unit const* renewTarget = IsSpellReady(player, 10929) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 80.0f) : nullptr;
    Unit const* healTarget = IsSpellReady(player, 10917) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 75.0f) : nullptr;
    Unit const* emergencyLowAlly = IsSpellReady(player, 10917) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 75.0f) : nullptr;
    Unit const* casterAlly = (player->IsInCombat() && IsSpellReady(player, 10060)) ? SelectFriendlyCasterTarget(player, GetConfiguredHealRange(), 100.0f) : nullptr;
    bool const shadowWordPainReady = IsSpellReady(player, kPriestShadowWordPainSpellId);
    Unit const* controlledTarget = shadowWordPainReady ? SelectEnemyNonBreakableCrowdControlTarget(player, 30.0f) : nullptr;
    Unit const* manaBurnTarget = IsSpellReady(player, 10876) ? SelectNearbyEnemyManaTarget(player, target, GetConfiguredLongRange(), 25.0f) : nullptr;
    Unit const* selectedRogueTarget = (shadowWordPainReady && HasHostileTarget(player, target) && target->GetClass() == CLASS_ROGUE &&
        !IsTargetInvalidByImmunity(player, target) && player->IsWithinLOSInMap(target) && player->IsWithinDistInMap(target, GetConfiguredLongRange())) ? target : nullptr;
    Unit const* rogueTarget = selectedRogueTarget ? selectedRogueTarget : (shadowWordPainReady ? SelectEnemyClassTarget(player, CLASS_ROGUE, GetConfiguredLongRange()) : nullptr);
    bool const isHolyPriest = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const isShadowPriest = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const holySchoolLocked = player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_HOLY);
    bool const shadowSchoolLocked = player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_SHADOW);
    bool const shadowHealingFallback = isShadowPriest && shadowSchoolLocked;
    bool const isHealingPriest = profileSelection.profile == ClassicClassProfile::PrimaryClassic || isHolyPriest || shadowHealingFallback;

    // A shadow-locked shadow priest cannot heal while Shadowform remains active.
    // Drop the form immediately, reuse the normal healing-priest decision tree
    // for the lockout window, then the high-priority Shadowform candidate below
    // restores normal behavior as soon as Shadow becomes available again.
    if (shadowHealingFallback && player->HasAura(15473))
        const_cast<Player*>(player)->RemoveAurasDueToSpell(15473);

    if (isHolyPriest && holySchoolLocked)
    {
        Unit const* mindBlastTarget = IsSpellReady(player, 10947) ? SelectEnemyTargetInSpellRange(player, target, 10947) : nullptr;
        if (mindBlastTarget)
            return { "priest mind blast", "use shadow offense during a holy-school lockout", 10947,
                playerbot::PvpClassSpellContext::TargetMode::Enemy, mindBlastTarget->GetGUID() };
    }
    bool const isHumanPriest = player->GetRace() == RACE_HUMAN;
    bool const isUndeadPriest = player->GetRace() == RACE_UNDEAD_PLAYER;
    bool const isNightElfPriest = player->GetRace() == RACE_NIGHTELF;
    Unit const* chastiseTarget = (isHumanPriest && IsSpellReady(player, 81350)) ?
        SelectEnemyTargetInSpellRange(player, target, 81350) : nullptr;
    Unit const* devouringCurseTarget = (isUndeadPriest && IsSpellReady(player, kPriestDevouringCurseSpellId)) ?
        SelectEnemyTargetInSpellRange(player, target, kPriestDevouringCurseSpellId) : nullptr;
    if (devouringCurseTarget && (HasBreakableCrowdControl(devouringCurseTarget) ||
        HasAuraFromSpellChain(devouringCurseTarget, kPriestDevouringCurseSpellId)))
        devouringCurseTarget = nullptr;
    Unit const* wyrmsShadowTarget = (isUndeadPriest && IsSpellReady(player, kPriestWyrmsShadowSpellId)) ?
        SelectEnemyCastingTarget(player, 13.0f, target) : nullptr;
    Unit const* wispFormThreat = isNightElfPriest ? SelectNearbyMeleeTarget(player, target, 8.0f) : nullptr;
    Unit const* elunesGraceTarget = (isNightElfPriest && IsSpellReady(player, kPriestElunesGraceSpellId)) ?
        SelectFriendlyMeleePressureTarget(player, 30.0f, 60.0f) : nullptr;
    if (elunesGraceTarget && (elunesGraceTarget->HasAura(kPriestElunesGraceSpellId) ||
        elunesGraceTarget->HasInvisibilityAura() ||
        playerbot::PvpCore::IsBattlegroundFlagCarrier(elunesGraceTarget->ToPlayer())))
        elunesGraceTarget = nullptr;
    bool const shouldCastLightwell = isHolyPriest && player->IsInCombat() &&
        CountNearbyFriendlyPlayers(player, 10.0f, false, false) >= 2 && IsSpellReady(player, kPriestLightwellSpellId);
    Unit const* spiritHealTarget = (isHolyPriest && IsPriestInSpiritOfRedemption(player) && IsSpellReady(player, 10917)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 99.5f, 0, false) : nullptr;
    Unit const* fearWardTarget = (player->GetRace() == RACE_DWARF && IsSpellReady(player, 6346)) ? SelectFriendlyMissingBuffTarget(player, 6346, 40.0f) : nullptr;
    Unit const* shadowSilenceTarget = isShadowPriest && IsSpellReady(player, 15487) ? SelectEnemyCastingTarget(player, GetConfiguredSpellRange(), target) : nullptr;
    bool const shadowUnderMeleePressure = isShadowPriest && CountNearbyMeleeThreats(player, 8.0f) >= 1;
    // Troll racial addition: keep Hex of Weakness on the kill target and
    // Shadowguard on self, regardless of priest spec.
    bool const isTrollPriest = player->GetRace() == RACE_TROLL;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, isHumanPriest && player->HealthBelowPct(35) && IsSpellReady(player, 19243), 61.0f,
        { "priest desperate prayer", "emergency human-priest self heal below 35 percent health", 19243, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isNightElfPriest && wispFormThreat && player->HealthBelowPct(45) &&
        IsSpellReady(player, kPriestWispFormSpellId), 60.9f,
        { "priest wisp form", "escape melee pressure while below 45 percent health", kPriestWispFormSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, spiritHealTarget, 60.5f,
        { "priest flash heal", "spam flash heal during spirit of redemption", 10917, spiritHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, spiritHealTarget ? spiritHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, fearWardTarget, 60.2f,
        { "priest fear ward", "place fear ward on a random unwarded ally", 6346, fearWardTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, fearWardTarget ? fearWardTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, wyrmsShadowTarget, 59.9f,
        { "priest wyrms shadow", "interrupt an enemy caster with the shadow cone", kPriestWyrmsShadowSpellId,
            playerbot::PvpClassSpellContext::TargetMode::Enemy, wyrmsShadowTarget ? wyrmsShadowTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, elunesGraceTarget, 47.5f,
        { "priest elunes grace", "fade a low-health ally under melee pressure into invisibility", kPriestElunesGraceSpellId,
            elunesGraceTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally,
            elunesGraceTarget ? elunesGraceTarget->GetGUID() : ObjectGuid::Empty });

    if (isHealingPriest)
    {
        AddDecisionCandidate(candidates, shouldCastLightwell, 48.0f,
            { "priest lightwell", "place a lightwell in combat when at least two allies are within ten yards", kPriestLightwellSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, emergencyLowAlly, 47.0f,
            { "priest flash heal", "prioritize healing for any nearby ally below 75 percent health", 10917, emergencyLowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyLowAlly ? emergencyLowAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !emergencyLowAlly && debuffedAlly, 46.0f,
            { "priest dispel magic ally", "prioritize dispelling magic debuffs from allies", 988, debuffedAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, debuffedAlly ? debuffedAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !emergencyLowAlly && enemyBuffedTarget, 45.0f,
            { "priest dispel magic enemy", "prioritize dispelling magic buffs from enemies", 988, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyBuffedTarget ? enemyBuffedTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, shieldTarget && !HasAuraFromSpellChain(shieldTarget, 10901), 48.0f,
            { "priest power word shield ally", "protect ally below 50 percent health", 10901, shieldTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, shieldTarget ? shieldTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !isHolyPriest && casterAlly, 30.0f,
            { "priest power infusion", "boost nearby caster throughput in combat", 10060, casterAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, casterAlly ? casterAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 1006) && IsSpellReady(player, 1006), 12.0f,
            { "priest inner fire", "maintain inner fire out of combat", 1006, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, renewTarget && !HasAuraFromSpellChain(renewTarget, 10929), 28.0f,
            { "priest renew", "maintain renew on moderately injured allies", 10929, renewTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, renewTarget ? renewTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, healTarget, 27.0f,
            { "priest flash heal", "heal party members below 75 percent health with flash heal", 10917, healTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, healTarget ? healTarget->GetGUID() : ObjectGuid::Empty });
    }

    if (isShadowPriest && !shadowHealingFallback)
    {
        AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 15473) && IsSpellReady(player, 15473), 58.0f,
            { "priest shadowform", "stay in shadowform", 15473, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, shadowSilenceTarget, 38.5f,
            { "priest silence", "silence a nearby enemy caster", 15487, playerbot::PvpClassSpellContext::TargetMode::Enemy, shadowSilenceTarget ? shadowSilenceTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, shadowUnderMeleePressure && IsSpellReady(player, 89784), 37.0f,
            { "priest fade", "drop threat and disengage under melee pressure", 89784, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 15286) && IsSpellReady(player, 15286), 24.0f,
            { "priest vampiric embrace", "maintain vampiric embrace on the kill target", 15286, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(candidates, hasHostileTarget && target && IsSpellReady(player, 10947), 22.5f,
            { "priest mind blast", "burst damage on cooldown", 10947, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(candidates, hasHostileTarget && target && IsSpellReady(player, 18807), 16.0f,
            { "priest mind flay", "default shadow damage filler", 18807, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    }

    AddDecisionCandidate(candidates, isTrollPriest && hasHostileTarget && target && !HasAuraFromSpellChain(target, 9035), 23.5f,
        { "priest hex of weakness", "troll racial debuff on the kill target", 9035, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isTrollPriest && !HasAuraFromSpellChain(player, 19312) && IsSpellReady(player, 19312), 23.0f,
        { "priest shadowguard", "troll racial self-buff upkeep", 19312, playerbot::PvpClassSpellContext::TargetMode::Self });

    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, kPriestShadowWordPainSpellId), 35.0f,
        { "priest shadow word pain", "maintain dot pressure on rogues", kPriestShadowWordPainSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, chastiseTarget, 39.0f,
        { "priest chastise", "human priest damage and root on an enemy target", 81350, playerbot::PvpClassSpellContext::TargetMode::Enemy, chastiseTarget ? chastiseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devouringCurseTarget, 36.0f,
        { "priest devouring curse", "undead priest racial dot used ahead of shadow word pain whenever its cooldown is ready",
            kPriestDevouringCurseSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy,
            devouringCurseTarget ? devouringCurseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isHolyPriest && manaBurnTarget, 21.0f,
        { "priest mana burn", "burn mana from enemy casters", 10876, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaBurnTarget ? manaBurnTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isHolyPriest && hasHostileTarget && IsSpellReady(player, 10934), 21.0f,
        { "priest smite", "holy fallback damage instead of mana burn", 10934, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 10.0f) >= 2 && CountNearbyFriendlyPlayers(player, 10.0f) >= 2 && IsSpellReady(player, 27801), 20.0f,
        { "priest holy nova", "aoe pressure and splash healing in melee cluster", 27801, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 8.0f) >= 2 && IsSpellReady(player, 10890), 19.5f,
        { "priest psychic scream", "fear nearby enemies when surrounded", 10890, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && shadowWordPainReady && !HasBreakableCrowdControl(target) && !HasAuraFromSpellChain(target, kPriestShadowWordPainSpellId), 19.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled or open targets", kPriestShadowWordPainSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, controlledTarget && !HasAuraFromSpellChain(controlledTarget, kPriestShadowWordPainSpellId), 18.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled targets", kPriestShadowWordPainSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, controlledTarget ? controlledTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, hasHostileTarget && target && IsLowOrOutOfManaForFallback(player) && IsWandShootReadyForDecision(player), 18.5f,
        { "priest shoot wand", "fallback to wand pressure while low on mana", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isShadowPriest && IsSpellReady(player, 10917) && player->HealthBelowPct(85), 17.0f,
        { "priest flash heal", "fallback self-healing while under pressure", 10917, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && !HasBreakableCrowdControl(target) && IsWandShootReadyForDecision(player), 8.0f,
        { "priest shoot wand", "default offensive fallback when no better priest action is available", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });

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

    bool const isFeralDruid = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isBalanceDruid = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    bool const isCasterForm = IsDruidCasterForm(player);
    bool const feralMayUseCasterUtility = !isFeralDruid || isCasterForm;

    Unit const* lowManaAlly = (feralMayUseCasterUtility && IsSpellReady(player, 29166)) ? SelectFriendlyLowManaTarget(player, 40.0f, 10.0f) : nullptr;
    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    Unit const* cursedTarget = (feralMayUseCasterUtility && !dispelThrottleActive && IsSpellReady(player, 2782)) ? SelectFriendlyDispelTarget(player, DISPEL_CURSE, 40.0f) : nullptr;
    Unit const* poisonedTarget = (feralMayUseCasterUtility && !dispelThrottleActive && IsSpellReady(player, 2893)) ? SelectFriendlyDispelTarget(player, DISPEL_POISON, 40.0f) : nullptr;
    Unit const* swiftmendTarget = IsSpellReady(player, 18562) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* emergencyLowTarget = (IsSpellReady(player, 17116) && IsSpellReady(player, 25297)) ? SelectFriendlyHealthTarget(player, 40.0f, 25.0f) : nullptr;
    Unit const* emergencyTarget = IsSpellReady(player, 25297) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* regrowthTarget = IsSpellReady(player, 9858) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
    Unit const* rejuvTarget = IsSpellReady(player, 25299) ? SelectFriendlyHealthTarget(player, 40.0f, 90.0f) : nullptr;
    Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, 30.0f);
    Unit const* meleeThreat = SelectNearbyMeleeTarget(player, target, 8.0f);
    uint32 const nearbyMeleeThreats = CountNearbyMeleeThreats(player, 8.0f);
    bool const heavyMeleePressure = nearbyMeleeThreats >= 2;
    Unit const* moonfireExecuteTarget = nullptr;
    if (IsSpellReady(player, 8921))
    {
        Unit const* nearbyTarget = SelectNearbyEnemyTarget(player, target, 25.0f);
        if (nearbyTarget && nearbyTarget->HealthBelowPct(20))
            moonfireExecuteTarget = nearbyTarget;
    }

    if (isFeralDruid && HasHostileTarget(player, target))
    {
        bool const inCat = HasAuraFromSpellChain(player, 768);
        bool const inBear = HasAuraFromSpellChain(player, 5487);
        bool const inFeralForm = inCat || inBear;
        bool const isProwling = HasAuraFromSpellChain(player, 9913);
        Unit const* lowAlly = SelectFriendlyHealthTarget(player, 40.0f, 80.0f);
        Unit const* rootTarget = SelectPredatorsSwiftnessRootTarget(player, target, 30.0f);
        Unit const* feralChargeBearTarget = IsSpellReady(player, 16979) ? SelectEnemyGapCloserTarget(player, target, 8.0f, 25.0f, false) : nullptr;
        Unit const* feralChargeCatTarget = IsSpellReady(player, 49376) ? SelectEnemyGapCloserTarget(player, target, 8.0f, 25.0f, false) : nullptr;
        bool const safeAgain = !player->HealthBelowPct(60) || !heavyMeleePressure;

        std::vector<PrioritizedSpellDecision> feralCandidates;
        AddDecisionCandidate(feralCandidates, isCasterForm && lowManaAlly && !lowManaAlly->HasAura(29166), 72.0f,
            { "druid innervate", "feral caster-form utility on low-mana ally", 29166, lowManaAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowManaAlly ? lowManaAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, isCasterForm && cursedTarget, 71.5f,
            { "druid remove curse", "feral caster-form curse removal", 2782, cursedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, isCasterForm && poisonedTarget && !HasAuraFromSpellChain(poisonedTarget, 2893), 71.0f,
            { "druid abolish poison", "feral caster-form poison removal", 2893, poisonedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, poisonedTarget ? poisonedTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, !player->IsInCombat() && !isProwling && !HasAuraFromSpellChain(player, 16864) && IsSpellReady(player, 16864), 70.9f,
            { "druid omen of clarity", "maintain omen of clarity before prowling or engaging", 16864, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inCat && !player->IsInCombat() && !isProwling && IsSpellReady(player, 9913), 70.8f,
            { "druid prowl", "stealth before opening as feral cat", 9913, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inFeralForm && !isProwling && target && !HasAuraFromSpellChain(target, 17392) && !HasAuraFromSpellChain(target, 9907) && IsSpellReady(player, 17392), 70.5f,
            { "druid faerie fire feral", "feral-form armor debuff", 17392, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, player->HasAura(69369) && lowAlly && IsSpellReady(player, 9858), 70.0f,
            { "druid regrowth", "predator's swiftness regrowth on lowest ally", 9858, lowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowAlly ? lowAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, player->HasAura(69369) && AllFriendlyPlayersHealthy(player, 40.0f, 80.0f) && rootTarget && IsSpellReady(player, 9853), 69.0f,
            { "druid entangling roots", "predator's swiftness root when allies are healthy", 9853, playerbot::PvpClassSpellContext::TargetMode::Enemy, rootTarget ? rootTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, player->HealthBelowPct(60) && heavyMeleePressure && !inBear && IsSpellReady(player, 5487), 68.0f,
            { "druid bear form", "swap bear under heavy melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, inBear && player->GetComboPoints() >= 5 && player->GetPowerPct(POWER_MANA) < 50.0f && !player->HasAura(89758) && IsSpellReady(player, 89758), 67.0f,
            { "druid thinnervate", "bear combo point thinnervate", 89758, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inBear && feralChargeBearTarget, 70.7f,
            { "druid feral charge bear", "bear gap close / interrupt from charge range", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy, feralChargeBearTarget ? feralChargeBearTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 22842), 65.0f,
            { "druid frenzied regeneration", "bear survival recovery", 22842, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 5229), 64.0f,
            { "druid enrage", "generate bear rage", 5229, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 9898) && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, 9898), 63.0f,
            { "druid demoralizing roar", "debuff melee attackers", 9898, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inBear && player->GetPower(POWER_RAGE) >= 150 && IsSpellReady(player, 9881), 62.0f,
            { "druid maul", "dump extra bear rage", 9881, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inBear && safeAgain && IsSpellReady(player, 768), 61.0f,
            { "druid cat form", "return to cat form once safe", 768, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, !inCat && !inBear && IsSpellReady(player, 768), 60.0f,
            { "druid cat form", "prefer cat form for feral pressure", 768, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inCat && IsRootedOrSnared(player) && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 768), 59.0f,
            { "druid cat form", "powershift root or snare", 768, playerbot::PvpClassSpellContext::TargetMode::Self, target ? target->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, inCat && feralChargeCatTarget, 70.6f,
            { "druid feral charge cat", "cat gap close / interrupt from charge range", 49376, playerbot::PvpClassSpellContext::TargetMode::Enemy, feralChargeCatTarget ? feralChargeCatTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(feralCandidates, inCat && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 9821), 58.0f,
            { "druid dash", "catch target in cat form", 9821, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(feralCandidates, inCat && player->GetComboPoints() >= 5 && player->GetPowerPct(POWER_MANA) < 50.0f && !player->HasAura(89758) && IsSpellReady(player, 89758), 56.0f,
            { "druid thinnervate", "restore mana with combo points", 89758, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && player->GetComboPoints() >= 5 && IsSpellReady(player, 9896), 55.0f,
            { "druid rip", "feral combo point bleed finisher", 9896, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && !HasAuraFromSpellChain(target, 33876) && IsSpellReady(player, 9850), 54.0f,
            { "druid claw", "build combo points when mangle is missing", 9850, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && !HasAuraFromSpellChain(target, 9904) && IsSpellReady(player, 9904), 53.0f,
            { "druid rake", "maintain rake bleed", 9904, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inFeralForm && !isProwling && target && !HasAuraFromSpellChain(target, 17392) && !HasAuraFromSpellChain(target, 9907) && IsSpellReady(player, 17392), 52.0f,
            { "druid faerie fire feral", "feral armor debuff filler", 17392, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(feralCandidates, inCat && IsSpellReady(player, 9830), 51.0f,
            { "druid shred", "behind-target combo point builder", 9830, playerbot::PvpClassSpellContext::TargetMode::Enemy });

        return SelectHighestPriorityCastableDecision(feralCandidates, player, target, nullptr);
    }

    // Balance stays in Moonkin Form at all times, so melee-pressure responses
    // are Moon Bash / Nature's Grasp / Leap instead of shifting to Bear Form.
    bool const balanceUnderMeleePressure = isBalanceDruid && CountNearbyEnemies(player, 10.0f) >= 1;
    Unit const* balanceMeleeRangeTarget = isBalanceDruid ? SelectNearbyMeleeTarget(player, target, 5.0f) : nullptr;
    Unit const* balanceEscapeAlly = (isBalanceDruid && balanceMeleeRangeTarget) ? SelectFriendlyNearRangeCapTarget(player, 8.0f, 25.0f) : nullptr;
    // 81342/81343 are the moonkin talents that permit casting Regrowth/
    // Rejuvenation while shapeshifted; without them Moonkin Form blocks both.
    Unit const* balanceRegrowthTarget = (isBalanceDruid && player->HasAura(81342)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 80.0f) : nullptr;
    Unit const* balanceRejuvTarget = (isBalanceDruid && player->HasAura(81343)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 80.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, recoveredFromPolymorph && IsSpellReady(player, 783), 55.0f,
        { "druid travel form recovery", "recovering from polymorph by travel-form reposition", 783, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isBalanceDruid && !HasAuraFromSpellChain(player, 24858) && IsSpellReady(player, 24858), 54.8f,
        { "druid moonkin form", "always stay in moonkin form", 24858, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isFeralDruid && !player->IsInCombat() && !player->HasStealthAura() && !HasAuraFromSpellChain(player, 16864) && IsSpellReady(player, 16864), 54.5f,
        { "druid omen of clarity", "maintain omen of clarity out of combat", 16864, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, feralMayUseCasterUtility && lowManaAlly && !lowManaAlly->HasAura(29166), 50.0f,
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
    AddDecisionCandidate(candidates, !isBalanceDruid && regrowthTarget && !HasAuraFromSpellChain(regrowthTarget, 9858), 44.0f,
        { "druid regrowth", "maintain regrowth on injured allies", 9858, regrowthTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, regrowthTarget ? regrowthTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isBalanceDruid && rejuvTarget && !HasAuraFromSpellChain(rejuvTarget, 25299), 43.0f,
        { "druid rejuvenation", "maintain rejuvenation on injured allies", 25299, rejuvTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, rejuvTarget ? rejuvTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isBalanceDruid && moonfireExecuteTarget, 42.0f,
        { "druid moonfire execute", "spam moonfire pressure on nearby low-health enemies", 8921, playerbot::PvpClassSpellContext::TargetMode::Enemy, moonfireExecuteTarget ? moonfireExecuteTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, balanceRegrowthTarget, 45.5f,
        { "druid regrowth", "moonkin talent allows regrowth on lowest-health ally below 80 percent", 9858, balanceRegrowthTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, balanceRegrowthTarget ? balanceRegrowthTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, balanceRejuvTarget, 45.4f,
        { "druid rejuvenation", "moonkin talent allows rejuvenation on lowest-health ally below 80 percent", 25299, balanceRejuvTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, balanceRejuvTarget ? balanceRejuvTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isBalanceDruid && target && !HasAuraFromSpellChain(target, 8921), 44.5f,
        { "druid moonfire", "maintain moonfire on the kill target", 8921, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Escalation while a melee attacker is on top of the moonkin: bash first,
    // root if it is still pressured, then disengage toward a nearby ally.
    AddDecisionCandidate(candidates, isBalanceDruid && balanceMeleeRangeTarget && IsSpellReady(player, 82423), 41.5f,
        { "druid moon bash", "strike an enemy that has closed to melee range", 82423, playerbot::PvpClassSpellContext::TargetMode::Enemy, balanceMeleeRangeTarget ? balanceMeleeRangeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isBalanceDruid && balanceMeleeRangeTarget && !HasAuraFromSpellChain(player, 17329) && IsSpellReady(player, 17329), 41.0f,
        { "druid natures grasp", "root the next melee attacker while still pressured", 17329, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isBalanceDruid && balanceMeleeRangeTarget && balanceEscapeAlly && IsSpellReady(player, 83111), 40.5f,
        { "druid leap", "disengage toward a nearby ally while still under melee pressure", 83111, playerbot::PvpClassSpellContext::TargetMode::Ally, balanceEscapeAlly ? balanceEscapeAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isBalanceDruid && target && balanceUnderMeleePressure && IsSpellReady(player, 5176), 40.0f,
        { "druid wrath", "ranged pressure while enemies are within 10 yards", 5176, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isBalanceDruid && target && !balanceUnderMeleePressure && IsSpellReady(player, 2912), 39.5f,
        { "druid starfire", "primary nuke while clear of melee pressure; castable while moving", 2912, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isFeralDruid && (HasAuraFromSpellChain(player, 768) || HasAuraFromSpellChain(player, 5487)) && rogueTarget && !HasAuraFromSpellChain(rogueTarget, 17392) && !HasAuraFromSpellChain(rogueTarget, 9907) && IsSpellReady(player, 17392), 30.5f,
        { "druid faerie fire feral", "apply feral faerie fire to nearby rogues", 17392, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isFeralDruid && feralMayUseCasterUtility && rogueTarget && !HasAuraFromSpellChain(rogueTarget, kDruidCasterFaerieFireSpellId) &&
        IsSpellReady(player, kDruidCasterFaerieFireSpellId) && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kDruidCasterFaerieFireSpellId), 30.0f,
        { "druid faerie fire", "apply faerie fire to nearby rogues", kDruidCasterFaerieFireSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, ((!isFeralDruid && !isBalanceDruid && meleeThreat) || (isFeralDruid && player->HealthBelowPct(60) && heavyMeleePressure)) && IsSpellReady(player, 5487), 29.0f,
        { "druid bear form", isFeralDruid ? "swap to bear only under heavy melee pressure below 60 percent health" : "swap to bear under physical melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });
    Unit const* bearChargeTarget = player->HasAura(5487) && IsSpellReady(player, 16979) ? SelectEnemyGapCloserTarget(player, target, 8.0f, 25.0f, false) : nullptr;
    AddDecisionCandidate(candidates, bearChargeTarget, 28.0f,
        { "druid feral charge", "bear gap close / interrupt from charge range", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy, bearChargeTarget ? bearChargeTarget->GetGUID() : ObjectGuid::Empty });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectPaladinSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const isRetPaladin = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isProtPaladin = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const knowsSacrificialAura = player->HasSpell(kPaladinSacrificialAuraSpellId);
    Unit const* emergencyLowAlly = isRetPaladin ? nullptr : SelectFriendlyHealthTarget(player, 15.0f, 25.0f);
    Unit const* rebukeTarget = (isProtPaladin && IsSpellReady(player, 81276)) ? SelectEnemyCastingTarget(player, 10.0f, target) : nullptr;
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
    Unit const* compelTarget = (isProtPaladin && IsSpellReady(player, 62124)) ?
        SelectEnemyTargetInSpellRange(player, target, 62124) : nullptr;
    Unit const* stunTarget = IsSpellReady(player, 10308) ? SelectEnemyCastingTarget(player, 10.0f, executeTarget) : nullptr;
    Unit const* repentanceTarget = (isRetPaladin && IsSpellReady(player, 20066)) ? SelectEnemyCastingTarget(player, 20.0f, executeTarget) : nullptr;
    Unit const* stunnedJudgementTarget = (isRetPaladin && HasAuraFromSpellChain(player, 20375)) ? SelectStunnedEnemyTarget(player, executeTarget, 30.0f) : nullptr;
    Unit const* protectionTarget = IsSpellReady(player, 10278) ? SelectFriendlyMeleePressureTarget(player, 40.0f, 50.0f) : nullptr;
    if (protectionTarget && playerbot::PvpCore::IsBattlegroundFlagCarrier(protectionTarget->ToPlayer()))
        protectionTarget = nullptr;
    Unit const* holyStrikeFlashHealTarget = (isRetPaladin && player->HasAura(89796) && IsSpellReady(player, 19943)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 100.0f) : nullptr;
    ObjectGuid const wisdomTargetGuid = (isRetPaladin && IsSpellReady(player, 25290)) ?
        SelectFriendlyWithManaAndWithoutAuraFromSpellChain(player, 25290, 45.0f, false) : ObjectGuid::Empty;
    ObjectGuid const mightTargetGuid = (isRetPaladin && IsSpellReady(player, 25291)) ?
        SelectFriendlyWithoutManaAndAuraFromSpellChain(player, 25291, 45.0f) : ObjectGuid::Empty;
    Unit const* flashHealTarget = (!isRetPaladin && IsSpellReady(player, 19943)) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
    // Holy Light is intentionally never used; Flash of Light covers every heal tier instead.
    Unit const* bigFlashHealTarget = (!isRetPaladin && IsSpellReady(player, 19943)) ? SelectFriendlyHealthTarget(player, 40.0f, 60.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, isRetPaladin && holyStrikeFlashHealTarget, 63.0f,
        { "paladin flash of light holy strike", "consume holy strike buff on the lowest-health friendly target", 19943, holyStrikeFlashHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, holyStrikeFlashHealTarget ? holyStrikeFlashHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRetPaladin && executeTarget && IsSpellReady(player, 89796), 62.5f,
        { "paladin holy strike", "use holy strike with very high priority", 89796, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, knowsSacrificialAura && !player->HasAura(kPaladinSacrificialAuraSpellId) &&
        IsSpellReady(player, kPaladinSacrificialAuraSpellId), 61.2f,
        { "paladin sacrificial aura", "prefer sacrificial aura when learned", kPaladinSacrificialAuraSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !knowsSacrificialAura && isRetPaladin && !HasAuraFromSpellChain(player, 20218) && IsSpellReady(player, 20218), 61.0f,
        { "paladin sanctity aura", "maintain sanctity aura for ret pressure", 20218, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRetPaladin && !HasAuraFromSpellChain(player, 20375) && IsSpellReady(player, 20375), 60.8f,
        { "paladin seal of command", "maintain seal of command", 20375, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HealthBelowPct(20) && IsSpellReady(player, 1020), 60.0f,
        { "paladin divine shield", "emergency immunity under lethal pressure", 1020, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, rebukeTarget, 59.7f,
        { "paladin rebuke", "interrupt a nearby enemy cast", 81276, playerbot::PvpClassSpellContext::TargetMode::Enemy, rebukeTarget ? rebukeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, compelTarget, 58.5f,
        { "paladin compel", "pull a target from Compel range onto the tank", 62124, playerbot::PvpClassSpellContext::TargetMode::Enemy, compelTarget ? compelTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isProtPaladin && executeTarget && IsSpellReady(player, 32699), 45.5f,
        { "paladin avengers shield", "ranged pressure and silence on the kill target", 32699, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, emergencyLowAlly && IsSpellReady(player, 19943), 56.0f,
        { "paladin flash of light", "emergency heal for nearby ally below 25 percent health", 19943, emergencyLowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyLowAlly ? emergencyLowAlly->GetGUID() : ObjectGuid::Empty });
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
    AddDecisionCandidate(candidates, bigFlashHealTarget, 47.0f,
        { "paladin flash of light", "large heal for heavily injured ally", 19943, bigFlashHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, bigFlashHealTarget ? bigFlashHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRetPaladin && stunnedJudgementTarget && IsSpellReady(player, 20271), 46.5f,
        { "paladin judgement", "judge nearby stunned enemy while seal of command is active", 20271, playerbot::PvpClassSpellContext::TargetMode::Enemy, stunnedJudgementTarget ? stunnedJudgementTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, executeTarget && HasActivePaladinSeal(player) && IsSpellReady(player, 20271), 46.0f,
        { "paladin judgement", "default offensive pressure when a seal is active", 20271, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !knowsSacrificialAura && !isRetPaladin && !player->HasAura(19746) && IsSpellReady(player, 19746), 20.0f,
        { "paladin concentration aura", "maintain concentration aura", 19746, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsInCombat() && isRetPaladin && IsSpellReady(player, 25291) && !HasAuraFromSpellChain(player, 25291), 19.0f,
        { "paladin blessing of might self", "ret paladin manually maintains lesser might on self", 25291, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() });
    AddDecisionCandidate(candidates, !player->IsInCombat() && isRetPaladin && !wisdomTargetGuid.IsEmpty(), 18.5f,
        { "paladin blessing of wisdom", "ret paladin manually maintains lesser wisdom on nearby mana allies", 25290, playerbot::PvpClassSpellContext::TargetMode::Ally, wisdomTargetGuid });
    AddDecisionCandidate(candidates, !player->IsInCombat() && isRetPaladin && !mightTargetGuid.IsEmpty(), 18.0f,
        { "paladin blessing of might", "ret paladin manually maintains lesser might on nearby non-mana allies", 25291, playerbot::PvpClassSpellContext::TargetMode::Ally, mightTargetGuid });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectWarlockSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const isAfflictionWarlock = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    bool const isDestructionWarlock = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    Pet const* pet = player->GetPet();
    bool const hasLivingPet = pet && pet->IsAlive();
    bool const needsPetSummon = !hasLivingPet;
    uint32 const summonPetSpell = isAfflictionWarlock ? 691 : (isDestructionWarlock ? 712 : 697);
    char const* summonPetName = isAfflictionWarlock ? "warlock summon felhunter" : (isDestructionWarlock ? "warlock summon succubus" : "warlock summon voidwalker");
    char const* summonPetReasonIdle = isAfflictionWarlock ? "maintain felhunter pet while out of combat" : (isDestructionWarlock ? "maintain succubus pet while out of combat" : "maintain voidwalker pet while out of combat");
    char const* summonPetReasonRecover = isAfflictionWarlock ? "recover felhunter out of combat when absent" : (isDestructionWarlock ? "recover succubus out of combat when absent" : "recover voidwalker out of combat when absent");
    bool const hasHostileTarget = HasHostileTarget(player, target);

    if (!hasHostileTarget)
    {
        if (needsPetSummon && !player->IsInCombat() && IsSpellReady(player, summonPetSpell))
            return { summonPetName, summonPetReasonIdle, summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self };

        return decision;
    }

    bool const closePressure = player->IsWithinDistInMap(target, 8.0f);
    Unit const* fearTarget = IsSpellReady(player, 6215) ? SelectWarlockFearTarget(player, 20.0f) : nullptr;
    Unit const* spellLockTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19647)) ? SelectEnemyCastingTarget(player, 30.0f, target) : nullptr;
    Unit const* devourEnemyTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19736)) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* devourFriendlyTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19736)) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 30.0f) : nullptr;
    Item const* equippedOffhand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    bool const firestoneEquipped = equippedOffhand && equippedOffhand->GetEntry() == kWarlockFirestoneItemEntry;
    SpellInfo const* firestoneSpellInfo = sSpellMgr->GetSpellInfo(kWarlockFirestoneUseSpellId);
    float const firestoneRange = firestoneSpellInfo ? firestoneSpellInfo->GetMaxRange(false) : 0.0f;
    Unit const* firestoneTarget = (isDestructionWarlock && firestoneEquipped && IsOnUseItemReady(player, kWarlockFirestoneItemEntry) &&
        firestoneRange > 0.0f && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken)) ?
        SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, firestoneRange) : nullptr;
    uint32 const spellstoneItemEntry = isAfflictionWarlock ? SelectReadyWarlockSpellstoneItemEntry(player) : 0;
    bool const hasSelfMagicDebuff = SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 0.0f) == player;
    bool const shouldUseSpellstone = spellstoneItemEntry != 0 && hasSelfMagicDebuff;

    bool const canUseVoidwalkerSacrifice = !isAfflictionWarlock && !isDestructionWarlock && player->HealthBelowPct(25) && !player->HasAura(19443) &&
        hasLivingPet && IsPetSpellReady(player, 19443);
    Unit const* seduceCandidate = (isDestructionWarlock && IsPetSpellReady(player, 6358)) ? SelectRandomEnemyWithoutBreakableCrowdControl(player, 30.0f) : nullptr;
    Unit const* seduceTarget = (seduceCandidate && !HasDotAura(seduceCandidate)) ? seduceCandidate : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, canUseVoidwalkerSacrifice, 70.0f,
        { "warlock sacrifice", "emergency voidwalker sacrifice at or below 25 percent health", 19443, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && target->HasUnitState(UNIT_STATE_CASTING) && IsPetSpellReady(player, 19244), 54.0f,
        { "warlock spell lock", "pet interrupt when available", 19244, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, spellLockTarget, 57.0f,
        { "warlock spell lock", "felhunter interrupt on any nearby caster", 19647, playerbot::PvpClassSpellContext::TargetMode::Enemy, spellLockTarget ? spellLockTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devourFriendlyTarget, 56.5f,
        { "warlock devour magic ally", "felhunter dispels friendly magic debuffs", 19736, playerbot::PvpClassSpellContext::TargetMode::Ally, devourFriendlyTarget ? devourFriendlyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devourEnemyTarget, 56.0f,
        { "warlock devour magic enemy", "felhunter dispels enemy magic buffs", 19736, playerbot::PvpClassSpellContext::TargetMode::Enemy, devourEnemyTarget ? devourEnemyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, firestoneTarget, 56.2f,
        { "warlock firestone", "use the equipped offhand firestone to purge enemy magic buffs", kWarlockFirestoneUseSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, firestoneTarget ? firestoneTarget->GetGUID() : ObjectGuid::Empty, kWarlockFirestoneItemEntry });
    AddDecisionCandidate(candidates, fearTarget, 53.0f,
        { "warlock fear", "prioritize fear control on paladin/priest targets in range", 6215, playerbot::PvpClassSpellContext::TargetMode::Enemy, fearTarget ? fearTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && !player->IsInCombat() && needsPetSummon && !player->HasAura(18708) && IsSpellReady(player, 18708), 52.0f,
        { "warlock fel domination", "prepare instant out-of-combat pet recovery before demon summon", 18708, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, needsPetSummon && !player->IsInCombat() && IsSpellReady(player, summonPetSpell), 51.0f,
        { summonPetName, summonPetReasonRecover, summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && !player->HasAura(25228) && IsSpellReady(player, 19028), 45.0f,
        { "warlock soul link", "maintain soul link when pet is available", 19028, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, seduceTarget, 52.5f,
        { "warlock seduce", "succubus crowd control on a non-cced, non-dotted target", 6358, playerbot::PvpClassSpellContext::TargetMode::Enemy, seduceTarget ? seduceTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, target->GetPowerType() == POWER_MANA && ShouldUseCurseOfTongues(target) && !HasAuraFromSpellChain(target, 11719) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11719) && IsSpellReady(player, 11719), 36.0f,
        { "warlock curse of tongues", "slow enemy casting throughput", 11719, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && !HasAuraFromSpellChain(target, 11722) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11722) && IsSpellReady(player, 11722), 35.5f,
        { "warlock curse of the elements", "keep the kill target cursed", 11722, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    bool const shouldApplyCurseOfAgony = !isDestructionWarlock && !IsCasterClass(target) && !HasAuraFromSpellChain(target, 11713) && !HasAuraFromSpellChain(target, 11719) &&
        !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11713) && IsSpellReady(player, 11713);
    // Amplify Curse boosts whatever curse lands next and is off the global
    // cooldown, so it should land immediately before Curse of Agony rather
    // than being sequenced across ticks like a normal candidate - see the
    // cadence-bypass whitelist in ProcessLifecycleEntryPoint (18288 there).
    AddDecisionCandidate(candidates, shouldApplyCurseOfAgony && !HasAuraFromSpellChain(player, 18288) && IsSpellReady(player, 18288), 35.2f,
        { "warlock amplify curse", "boost the curse about to land on the kill target", 18288, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldApplyCurseOfAgony, 35.0f,
        { "warlock curse of agony", "apply curse of agony pressure to non-caster players", 11713, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, shouldUseSpellstone, 56.7f,
        { "warlock spellstone", "use spellstone to remove magic debuffs", 0, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), spellstoneItemEntry });
    AddDecisionCandidate(candidates, isAfflictionWarlock && !HasAuraFromSpellChain(target, 48181) && IsSpellReady(player, 48181), 34.5f,
        { "warlock haunt", "maintain haunt on kill target", 48181, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && HasAuraFromSpellChain(target, 25309) && IsSpellReady(player, 18932), 34.4f,
        { "warlock conflagrate", "consume immolate for burst damage", 18932, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && !HasAuraFromSpellChain(target, 25309) && IsSpellReady(player, 25309), 34.2f,
        { "warlock immolate", "apply immolate to enable conflagrate", 25309, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && target && IsSpellReady(player, 18871), 33.8f,
        { "warlock shadowburn", "instant execute pressure on the kill target", 18871, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isDestructionWarlock && !HasAuraFromSpellChain(target, 11672) && IsSpellReady(player, 11672), 34.0f,
        { "warlock corruption", "maintain corruption dot", 11672, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, (target->HealthBelowPct(20) || (closePressure && IsMeleeClass(target))) && IsSpellReady(player, 17926), 33.0f,
        { "warlock death coil", "peel melee or finish low enemy target", 17926, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isAfflictionWarlock && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 17928), 32.5f,
        { "warlock howl of terror", "fear clustered nearby enemies", 17928, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->GetPower(POWER_MANA) < 400 && IsSpellReady(player, 11689), 32.0f,
        { "warlock life tap", "convert health to mana for sustained casting", 11689, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsLowOrOutOfManaForFallback(player) && IsWandShootReadyForDecision(player), 31.5f,
        { "warlock shoot wand", "fallback to wand pressure while low on mana", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->HasAura(17941) && IsSpellReady(player, 25307), 20.0f,
        { "warlock shadow bolt", "consume nightfall proc for instant pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Destruction should never default-cast Shadow Bolt - only ever on a
    // Nightfall proc (above). Its own filler is Searing Pain once Immolate
    // is already ticking and both Conflagrate and Shadowburn are on
    // cooldown, not a Shadow Bolt spam loop.
    AddDecisionCandidate(candidates, isDestructionWarlock && HasAuraFromSpellChain(target, 25309) &&
            !IsSpellReady(player, 18932) && !IsSpellReady(player, 18871) && IsSpellReady(player, 17923), 19.5f,
        { "warlock searing pain", "filler pressure while conflagrate and shadowburn are both on cooldown", 17923, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && !isDestructionWarlock && IsSpellReady(player, 25307), 19.0f,
        { "warlock shadow bolt", "default ranged pressure", 25307, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isAfflictionWarlock && IsSpellReady(player, 11700), 18.0f,
        { "warlock drain life", "fallback affliction channel", 11700, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, HasHostileTarget(player, target) && target && !HasBreakableCrowdControl(target) && IsWandShootReadyForDecision(player), 8.0f,
        { "warlock shoot wand", "default offensive fallback when no better warlock action is available", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });

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

    // Charge, Intercept, and Heroic Leap drive their own effect-motion spline.
    // While that movement is resolving the warrior is locked in, so defer all
    // spell decisions until the native gap-closer motion finishes.
    if (HasActiveMovementEffectSpline(player))
        return decision;

    bool const isProtWarrior = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isFuryWarrior = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const inBattleStance = player->HasAura(2457);
    bool const inDefensiveStance = player->HasAura(71);
    bool const inBerserkerStance = player->HasAura(2458);
    // Charge, Intercept, and Heroic Leap drive their own effect-motion spline.
    // Do not pick stance-swap actions while that spline is still resolving.
    bool const warriorGapCloserInFlight = HasActiveMovementEffectSpline(player);
    Unit const* gapCloseTarget = HasHostileTarget(player, target) ? target : activeTarget;
    SpellInfo const* chargeInfo = sSpellMgr->GetSpellInfo(11578);
    SpellInfo const* interceptInfo = sSpellMgr->GetSpellInfo(20617);
    float const chargeMinRange = chargeInfo ? chargeInfo->GetMinRange(false) : 8.0f;
    float const interceptMinRange = interceptInfo ? interceptInfo->GetMinRange(false) : 8.0f;
    bool const canChargeByRange = !player->IsWithinDistInMap(gapCloseTarget, chargeMinRange);
    bool const canInterceptByRange = !player->IsWithinDistInMap(gapCloseTarget, interceptMinRange);
    bool const shouldUseChargeGapCloser = !player->IsWithinMeleeRange(gapCloseTarget) && !player->IsInCombat() && canChargeByRange && IsSpellReady(player, 11578);
    bool const shouldUseInterceptGapCloser = !isFuryWarrior && !player->IsWithinMeleeRange(gapCloseTarget) && player->IsInCombat() && canInterceptByRange && IsSpellReady(player, 20617);
    // Fury uses Heroic Leap (ground-targeted, same range band as Intercept)
    // instead of Intercept. This was missing the same min-range gate
    // Intercept has above - close-but-not-melee distances inside Intercept's
    // min range would try to fire Heroic Leap anyway and fail the spell's own
    // range validation, silently doing nothing.
    bool const shouldUseHeroicLeapGapCloser = isFuryWarrior && !player->IsWithinMeleeRange(gapCloseTarget) && player->IsInCombat() && canInterceptByRange && IsSpellReady(player, 81271);
    bool const furyInDanger = isFuryWarrior && player->HealthBelowPct(35) && SelectNearbyMeleeTarget(player, activeTarget, 8.0f) && IsSpellReady(player, 81271);
    bool const furyRecentGapCloser = isFuryWarrior && (player->GetSpellHistory()->HasCooldown(11578) || player->GetSpellHistory()->HasCooldown(81271));
    uint32 const meleeFinisherSpellId = isProtWarrior ? uint32(23925) : (isFuryWarrior ? uint32(23881) : uint32(21553));
    // Bloodrage/Battle Shout are self-targeted, so the immediate-castability
    // check never fails them for range - they are always "ready" whenever
    // their own condition holds. Sitting above the gap closers in priority,
    // they permanently starve Charge/Intercept/Heroic Leap from ever being
    // selected while out of melee range, since the selector always returns
    // the first immediately-castable candidate scanning top-down rather than
    // preferring an out-of-range action that would trigger approach movement.
    bool const gapCloseUrgent = shouldUseChargeGapCloser || shouldUseInterceptGapCloser || shouldUseHeroicLeapGapCloser;
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, activeTarget, 8.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 8.0f, activeTarget);
    Unit const* tauntTarget = isProtWarrior && IsSpellReady(player, 355) ? SelectWarriorTauntTarget(player, activeTarget, 30.0f) : nullptr;
    bool const revengeReady = isProtWarrior && player->IsWithinMeleeRange(activeTarget) && IsSpellReadyAndCasterAuraAllowed(player, 25288);
    bool const executeReady = activeTarget && activeTarget->HealthBelowPct(20) && IsSpellReady(player, 20662) && player->GetPower(POWER_RAGE) >= 150;
    bool const hasNearbyMeleeThreat = HasHostileTarget(player, nearbyMeleeTarget);
    bool const nearbyMeleeThreatSnared = hasNearbyMeleeThreat && nearbyMeleeTarget->HasAuraWithMechanic(1 << MECHANIC_SNARE);
    bool const canDisarmNearbyMeleeThreat = hasNearbyMeleeThreat &&
        nearbyMeleeThreatSnared &&
        player->IsWithinMeleeRange(nearbyMeleeTarget) &&
        nearbyMeleeTarget->CanUseAttackType(BASE_ATTACK) &&
        !HasAuraFromSpellChain(nearbyMeleeTarget, 676);

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && player->HasAuraWithMechanic(1 << MECHANIC_FEAR) && !inBerserkerStance && IsSpellReady(player, 2458), 60.5f,
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
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && canDisarmNearbyMeleeThreat && !inDefensiveStance && IsSpellReady(player, 81492) && IsSpellReady(player, 71) && player->GetPower(POWER_RAGE) >= 200, 57.0f,
        { "warrior defensive stance", "swap defensive before disarm against melee", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && tauntTarget && !inDefensiveStance && !shouldUseInterceptGapCloser && IsSpellReady(player, 71), 57.2f,
        { "warrior defensive stance", "swap defensive before taunt", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && revengeReady && !inDefensiveStance && IsSpellReady(player, 71), 57.1f,
        { "warrior defensive stance", "swap defensive before revenge", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, tauntTarget && inDefensiveStance, 56.8f,
        { "warrior taunt", "taunt enemy pressuring an ally or current kill target", 355, playerbot::PvpClassSpellContext::TargetMode::Enemy, tauntTarget ? tauntTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, CountNearbyUnsNaredEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 12323), 56.0f,
        { "warrior piercing howl", "apply area snare when multiple enemies are unsnared in melee range", 12323, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, HasHostileTarget(player, activeTarget) && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 5246), 55.5f,
        { "warrior intimidating shout", "aoe fear around the current target when outnumbered", 5246, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !gapCloseUrgent && (IsSpellReady(player, 6552) || IsSpellReady(player, 81492) || IsSpellReady(player, 20617) || IsSpellReady(player, 1680) || IsSpellReady(player, meleeFinisherSpellId)) &&
            player->GetPower(POWER_RAGE) < 150 && IsSpellReady(player, 2687), 54.0f,
        { "warrior bloodrage", "generate rage to unlock rotational abilities", 2687, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && !isProtWarrior && inDefensiveStance && (!IsSpellReady(player, 81492) || !hasNearbyMeleeThreat) && IsSpellReady(player, 2458), 53.0f,
        { "warrior berserker stance", "leave defensive stance when disarm is unavailable or no melee threat is nearby", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && shouldUseChargeGapCloser && !inBattleStance && IsSpellReady(player, 2457), 52.5f,
        { "warrior battle stance", "switch to battle stance before out-of-combat charge", 2457, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldUseChargeGapCloser, 52.0f,
        { "warrior charge", "close gap to target from out of combat", 11578, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && shouldUseInterceptGapCloser && !inBerserkerStance && IsSpellReady(player, 2458), 51.5f,
        { "warrior berserker stance", "switch to berserker stance before intercept gap close", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldUseInterceptGapCloser, 51.0f,
        { "warrior intercept", "close gap to target while in combat", 20617, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    // Heroic Leap's ShapeshiftMask requires Berserker Stance (same shapeshift
    // mechanism as Druid forms/Ghost Wolf), and there was no stance-swap
    // candidate for it - unlike Intercept, which already has one just above.
    // Without this, the generic shapeshift-compatibility check silently
    // rejects Heroic Leap whenever the warrior isn't already in Berserker
    // Stance, and the bot falls back to Charge (which doesn't need it).
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && (shouldUseHeroicLeapGapCloser || furyInDanger) && !inBerserkerStance && IsSpellReady(player, 2458), 1000.0f,
        { "warrior berserker stance", "switch to berserker stance before heroic leap", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    // TEMPORARY: forced to top priority (above everything, including
    // emergency defensives) at the user's explicit request to isolate and
    // test whether Heroic Leap fires at all once the stance prerequisite is
    // met. Revert to a normal tier once confirmed working.
    AddDecisionCandidate(candidates, shouldUseHeroicLeapGapCloser, 999.0f,
        { "warrior heroic leap", "close gap to target with heroic leap instead of intercept", 81271, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, furyInDanger, 999.5f,
        { "warrior heroic leap", "leap away from danger instead of intercept", 81271, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, furyRecentGapCloser && !HasAuraFromSpellChain(player, 12328) && IsSpellReady(player, 12328), 49.5f,
        { "warrior death wish", "pop death wish right after closing with charge or heroic leap", 12328, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && !isProtWarrior && player->IsWithinMeleeRange(activeTarget) && !inBerserkerStance &&
            IsSpellReady(player, 1680) && IsSpellReady(player, 2458), 50.4f,
        { "warrior berserker stance", "switch to berserker stance to enable whirlwind in melee", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && executeReady && !inBerserkerStance && IsSpellReady(player, 2458), 50.2f,
        { "warrior berserker stance", "switch to berserker stance before execute", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, executeReady && inBerserkerStance, 50.0f,
        { "warrior execute", "finisher at low enemy health while in berserker stance", 20662, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !gapCloseUrgent && !HasAuraFromSpellChain(player, 25289) && IsSpellReady(player, 25289), 40.0f,
        { "warrior battle shout", "maintain attack power buff", 25289, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, revengeReady && inDefensiveStance, 40.5f,
        { "warrior revenge", "use reactive revenge whenever available", 25288, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    Unit const* concussionTarget = isProtWarrior && IsSpellReady(player, 12809) ? SelectUnstunDREnemyTarget(player, activeTarget, 5.0f, 12809) : nullptr;
    AddDecisionCandidate(candidates, concussionTarget, 39.6f,
        { "warrior concussion blow", "stun a target without stun diminishing returns", 12809, playerbot::PvpClassSpellContext::TargetMode::Enemy, concussionTarget ? concussionTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) &&
            (isProtWarrior ? !HasAuraFromSpellChain(activeTarget, 11597) : (!HasAuraFromSpellChain(activeTarget, 7373) || (activeTarget->GetAura(7373) && activeTarget->GetAura(7373)->GetDuration() < 2000))) &&
            IsSpellReady(player, isProtWarrior ? uint32(11597) : uint32(7373)), 39.0f,
        { isProtWarrior ? "warrior sunder armor" : "warrior hamstring", isProtWarrior ? "apply sunder armor as protection filler" : "maintain stickiness snare", isProtWarrior ? uint32(11597) : uint32(7373), playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    char const* meleeFinisherName = isProtWarrior ? "warrior shield slam" : (isFuryWarrior ? "warrior bloodthirst" : "warrior mortal strike");
    char const* meleeFinisherReason = isProtWarrior ? "protection kill target pressure" : (isFuryWarrior ? "fury primary strike instead of mortal strike" : "arms-like burst pressure");
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && (isProtWarrior || isFuryWarrior || !HasAuraFromSpellChain(activeTarget, 21553)) &&
            IsSpellReady(player, meleeFinisherSpellId), 38.0f,
        { meleeFinisherName, meleeFinisherReason, meleeFinisherSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
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
    bool const isAssassinationRogue = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    bool const hasImprovedBackstab = isAssassinationRogue && player->HasTalent(13866, player->GetActiveSpec());
    Unit const* blindTarget = IsSpellReady(player, 2094) ? SelectRogueBlindTarget(player, target, 15.0f) : nullptr;
    Unit const* nearbyCastingTarget = IsSpellReady(player, 1766) ? SelectEnemyCastingTarget(player, 5.0f, target) : nullptr;
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, target, 5.0f);
    bool const rootedOrSnared = IsRootedOrSnared(player);

    std::vector<PrioritizedSpellDecision> candidates;
    // Disabled: weapon-poison automation from PvP decision loop.
    // This avoids touching weapon-enchant mutation paths while investigating combat-time crashes.

    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 1784) && IsSpellReady(player, 1784), 50.0f,
        { "rogue stealth", "enter stealth before engagement", 1784, playerbot::PvpClassSpellContext::TargetMode::Self });
    // Assassination opens with Garrote instead of Cheap Shot. Garrote carries
    // SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET, so the shared cast-failure
    // handling in CastDirectSpell already repositions the bot behind the
    // target (IssueBehindTargetMeleeMovement) before retrying the cast.
    AddDecisionCandidate(candidates, isAssassinationRogue && player->HasStealthAura() && player->IsWithinMeleeRange(target) && IsSpellReady(player, 703), 49.2f,
        { "rogue garrote", "assassination opener from stealth", 703, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isAssassinationRogue && player->HasStealthAura() && player->IsWithinMeleeRange(target) && IsSpellReady(player, 1833), 49.0f,
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
        { "rogue slice and dice", "maintain slice and dice at two combo points", 6774, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isCombatRogue && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 81308), 43.2f,
        { "rogue deadly shot", "ranged fallback when kill target is out of melee", 81308, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Cold Blood guarantees the next finisher crits; use it right before
    // dumping a full combo point bar into Eviscerate.
    AddDecisionCandidate(candidates, isAssassinationRogue && player->GetComboPoints() >= 5 && !HasAuraFromSpellChain(player, 14177) && IsSpellReady(player, 14177), 43.1f,
        { "rogue cold blood", "guarantee a critical finisher at full combo points", 14177, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->GetComboPoints() >= 5 && IsSpellReady(player, 11300), 43.0f,
        { "rogue eviscerate", "combo finisher pressure", 11300, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Improved Backstab lets Assassination use Backstab as its combo builder
    // instead of Hemorrhage. Backstab also carries the behind-target attribute,
    // so the shared reposition-on-cast-failure handling keeps the bot flanking.
    AddDecisionCandidate(candidates, hasImprovedBackstab && player->IsWithinMeleeRange(target) && IsSpellReady(player, 53), 20.5f,
        { "rogue backstab", "improved backstab combo point builder", 53, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !hasImprovedBackstab && IsSpellReady(player, isCombatRogue ? uint32(11294) : uint32(16511)), 20.0f,
        { isCombatRogue ? "rogue sinister strike" : "rogue hemorrhage", isCombatRogue ? "default combat combo point builder" : "default subtlety combo point builder", isCombatRogue ? uint32(11294) : uint32(16511), playerbot::PvpClassSpellContext::TargetMode::Enemy });

    return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
}

SpellDecision SelectShamanSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const inCombat = player->IsInCombat();
    bool const isRestoShaman = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isEnhancementShaman = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    if (!hasHostileTarget && !allyTarget && !isRestoShaman && !isEnhancementShaman)
        return decision;

    bool const partyBenefitsFromWindfury = PartyBenefitsFromWindfuryTotem(player);

    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    // 89745 lets an enhancement shaman weave in a low-priority heal.
    Unit const* enhLowHealTarget = (isEnhancementShaman && player->HasAura(89745) && IsSpellReady(player, 10468)) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    Unit const* enhPurgeTarget = (isEnhancementShaman && !dispelThrottleActive && hasHostileTarget && IsSpellReady(player, 370)) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* chainHealTarget = isRestoShaman && IsSpellReady(player, 10623) ? SelectFriendlyHealthTarget(player, 40.0f, 95.0f) : nullptr;
    Unit const* lesserHealTarget = isRestoShaman && IsSpellReady(player, 10468) ? SelectFriendlyHealthTarget(player, 40.0f, 90.0f) : nullptr;
    Unit const* nsHealTarget = isRestoShaman && IsSpellReady(player, 16188) && IsSpellReady(player, 25357) ? SelectFriendlyHealthTarget(player, 40.0f, 35.0f) : nullptr;
    Unit const* earthShieldTarget = isRestoShaman && IsSpellReady(player, 32593) && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, 32593) ? SelectFriendlyHealthTarget(player, 40.0f, 100.0f) : nullptr;
    Unit const* purgeTarget = isRestoShaman && hasHostileTarget && IsSpellReady(player, 81325) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* allyMagicTarget = isRestoShaman && IsSpellReady(player, 81325) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f) : nullptr;
    // Rehgar's Fury (82419) is a charge that requires Ghost Wolf form active
    // (see spell_sha_ghost_wolf_charge / Unit::CompleteGhostWolfCharge) - it
    // consumes the form on impact. Enhancement shifts into Ghost Wolf first,
    // then charges the kill target with Rehgar's Fury to close the gap.
    bool const enhNeedsGapClose = isEnhancementShaman && hasHostileTarget && target && !player->IsWithinMeleeRange(target);
    bool const shamanInGhostWolf = HasAuraFromSpellChain(player, 2645);
    bool const enhInGhostWolf = isEnhancementShaman && shamanInGhostWolf;
    SpellInfo const* rehgarsFuryInfo = sSpellMgr->GetSpellInfo(82419);
    bool const canRehgarsFuryTarget = isEnhancementShaman && hasHostileTarget && target && rehgarsFuryInfo &&
        player->IsValidAttackTarget(target, rehgarsFuryInfo) && player->IsWithinLOSInMap(target) &&
        (rehgarsFuryInfo->GetMaxRange(false) <= 0.0f || player->IsWithinDistInMap(target, rehgarsFuryInfo->GetMaxRange(false))) &&
        (rehgarsFuryInfo->GetMinRange(false) <= 0.0f || !player->IsWithinDistInMap(target, rehgarsFuryInfo->GetMinRange(false)));
    bool const canUseRehgarsFury = canRehgarsFuryTarget && IsSpellReady(player, 82419);

    if (shamanInGhostWolf)
    {
        if (canUseRehgarsFury)
            return { "shaman rehgar's fury", "only allowed action while in ghost wolf form", 82419, playerbot::PvpClassSpellContext::TargetMode::Enemy };

        // Do not cancel Ghost Wolf just because Rehgar's Fury isn't castable
        // on this exact tick (its own cooldown, a brief LOS blip, target
        // sitting inside the charge's min-range band, etc.) while the shaman
        // still has a genuine reason to be closing distance - that reads as
        // flickering in and out of the form instead of committing to the
        // chase. Only unshift once the gap-close reason itself is gone
        // (target dead/invalid or already in melee range). Rehgar's Fury
        // landing still removes the form on its own via
        // Unit::CompleteGhostWolfCharge.
        if (enhNeedsGapClose)
            return decision;

        const_cast<Player*>(player)->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        return decision;
    }

    std::vector<PrioritizedSpellDecision> candidates;
    // Disabled: auto-casting Windfury Weapon from PvP loop while investigating weapon-dependent aura crashes.
    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 10432) && IsSpellReady(player, 10432), 34.0f,
        { "shaman lightning shield", "maintain shield buff out of combat", 10432, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target->HasUnitState(UNIT_STATE_CASTING) && IsSpellReady(player, 10414), 60.0f,
        { "shaman earth shock", "interrupt enemy cast with shock", 10414, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, inCombat && !isRestoShaman && !isEnhancementShaman && !partyBenefitsFromWindfury && hasHostileTarget && target->GetPowerType() == POWER_MANA && !HasActiveAirTotem(player) && IsSpellReady(player, 8177), 59.0f,
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
    AddDecisionCandidate(candidates, !isRestoShaman && !isEnhancementShaman && IsSpellReady(player, 10605), 57.0f,
        { "shaman chain lightning", "primary burst cast on kill target", 10605, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isEnhancementShaman && hasHostileTarget && target && IsSpellReady(player, 17364), 56.5f,
        { "shaman stormstrike", "primary melee burst on the kill target", 17364, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Enhancement leans on purge harder than the default cross-spec candidate below.
    AddDecisionCandidate(candidates, enhPurgeTarget, 54.5f,
        { "shaman purge", "heavier purge priority for enhancement", 370, playerbot::PvpClassSpellContext::TargetMode::Enemy, enhPurgeTarget ? enhPurgeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, enhLowHealTarget, 30.5f,
        { "shaman lesser healing wave", "weave a heal on a low-health ally", 10468, enhLowHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, enhLowHealTarget ? enhLowHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, inCombat && hasHostileTarget && IsMeleeClass(target) && player->IsWithinDistInMap(target, 10.0f) && !HasActiveEarthTotem(player) && IsSpellReady(player, 2484), 56.0f,
        { "shaman earthbind totem", "kite nearby melee pressure", 2484, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && IsSpellReady(player, 10473) &&
        ((isEnhancementShaman && enhNeedsGapClose) || (IsMeleeClass(target) && player->IsWithinDistInMap(target, 20.0f))), 55.0f,
        { "shaman frost shock", isEnhancementShaman && enhNeedsGapClose ? "snare the kill target while chasing" : "snare medium-range melee threats",
            10473, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    Unit const* poisonedAllyInTotemRange = inCombat && IsSpellReady(player, 8170) ? SelectFriendlyDispelTarget(player, DISPEL_POISON, 20.0f) : nullptr;
    AddDecisionCandidate(candidates, inCombat && poisonedAllyInTotemRange && !HasActiveWaterTotem(player), 54.0f,
        { "shaman poison cleansing totem", "answer rogue poison pressure with a nearby water totem", 8170, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && hasHostileTarget && (target->GetClass() == CLASS_PRIEST || target->GetClass() == CLASS_WARLOCK) && player->IsWithinDistInMap(target, 20.0f) && !HasActiveEarthTotem(player) && IsSpellReady(player, 8143), 53.0f,
        { "shaman tremor totem", "mitigate fear pressure from priest/warlock", 8143, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && isRestoShaman && player->GetPowerPct(POWER_MANA) < 50.0f && !HasActiveWaterTotem(player) && IsSpellReady(player, 16190), 52.8f,
        { "shaman mana tide totem", "restore mana below half", 16190, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && !HasActiveEarthTotem(player) && IsSpellReady(player, 81476), 52.7f,
        { "shaman tremor totem", "maintain a nearby tremor totem", 81476, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && !HasActiveWaterTotem(player) && IsSpellReady(player, 81477), 52.6f,
        { "shaman poison cleansing totem", "maintain a nearby poison cleansing totem", 81477, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && !partyBenefitsFromWindfury && !HasActiveAirTotem(player) && IsSpellReady(player, 81478), 52.5f,
        { "shaman grounding totem", "maintain a nearby grounding totem", 81478, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && partyBenefitsFromWindfury && !HasActiveAirTotem(player) && IsSpellReady(player, 10614), 52.5f,
        { "shaman windfury totem", "support an arms/fury warrior or retribution paladin", 10614, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && SelectNearbyMeleeTarget(player, target, 8.0f) && player->HealthBelowPct(50) && IsSpellReady(player, 2645), 52.4f,
        { "shaman ghost wolf", "escape melee pressure while endangered", 2645, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, enhNeedsGapClose && !enhInGhostWolf && canUseRehgarsFury && IsSpellReady(player, 2645), 59.6f,
        { "shaman ghost wolf", "shift to close the gap with rehgar's fury", 2645, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, enhNeedsGapClose && enhInGhostWolf && canUseRehgarsFury, 59.5f,
        { "shaman rehgar's fury", "charge the kill target while in ghost wolf form", 82419, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Enhancement only gets Lesser Healing Wave weaving from the Maelstrom
    // Weapon-style talent (89745, "blurry") that makes it a fast/free-ish
    // proc-consumer - without that aura up, this was letting an Enhancement
    // shaman self-heal on the normal, full-cost cast whenever health dropped
    // below 50 percent regardless of whether the proc was actually active.
    AddDecisionCandidate(candidates, (!isEnhancementShaman || player->HasAura(89745)) && player->HealthBelowPct(50) && IsSpellReady(player, 10468), 52.0f,
        { "shaman lesser healing wave", "self-sustain while focused", 10468, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && purgeTarget, 53.5f,
        { "shaman purge", "purge enemy magic buffs", 81325, playerbot::PvpClassSpellContext::TargetMode::Enemy, purgeTarget ? purgeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && allyMagicTarget, 53.2f,
        { "shaman purge ally", "purge sheep or fear magic effects from allies", 81325, allyMagicTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, allyMagicTarget ? allyMagicTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !dispelThrottleActive && hasHostileTarget && IsSpellReady(player, 370), 40.0f,
        { "shaman purge", "strip enemy magical effects by default", 370, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isRestoShaman && lesserHealTarget, 51.0f,
        { "shaman lesser healing wave", "restoration fallback heal", 10468, lesserHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lesserHealTarget ? lesserHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isEnhancementShaman && hasHostileTarget && IsSpellReady(player, 15208), isRestoShaman ? 5.0f : 39.0f,
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
        if (!mageDecision.spellId && HasHostileTarget(player, target) && IsLowOrOutOfManaForFallback(player) && IsWandShootReadyForDecision(player))
            return { "mage shoot wand", "fallback to wand pressure while low on mana", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy };
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
    if (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotShadowmeldGraceToken))
    {
        SpellDecision holdDecision;
        holdDecision.reason = "shadowmeld grace window";
        return holdDecision;
    }

    if (player && player->HealthBelowPct(50))
    {
        if (uint32 const healthstoneItemEntry = SelectReadyHealthstoneItemEntry(player))
            return { "use healthstone", "restore health below fifty percent", 0,
                playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), healthstoneItemEntry };
    }

    // Spirit of Redemption must run the priest healing selector even with no
    // selected hostile/ally target; the selector finds the lowest-health ally
    // itself and Flash Heal is free during this aura.
    if (IsPriestInSpiritOfRedemption(player))
        return SelectClassicClassSpell(player, target, allyTarget, profileSelection);

    // School-lockout fallbacks are combat recovery, not ordinary utility.
    // Run them before racial/mount/eat-drink selection so a shadow-locked
    // shadow priest drops Shadowform and begins healing immediately, and a
    // holy-locked holy priest can answer with Mind Blast without losing a
    // decision tick to unrelated utility.
    if (player && player->GetClass() == CLASS_PRIEST)
    {
        bool const holyPriestHolyLocked = profileSelection.profile == ClassicClassProfile::SecondaryClassic &&
            player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_HOLY);
        bool const isShadowPriest = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
        bool const shadowPriestShadowLocked = isShadowPriest &&
            player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_SHADOW);
        bool const shadowPriestNeedsForm = isShadowPriest && !shadowPriestShadowLocked &&
            !HasAuraFromSpellChain(player, 15473) && IsSpellReady(player, 15473);
        if (holyPriestHolyLocked || shadowPriestShadowLocked || shadowPriestNeedsForm)
            return SelectClassicClassSpell(player, target, allyTarget, profileSelection);
    }

    // Hunter cast-time actions (especially Aimed Shot and Revive Pet) must be
    // exclusive. Lifecycle movement already holds the hunter still, but the
    // selector also has to stop returning other instant actions like Rapid Fire
    // while the accepted cast is preparing/channeling. Otherwise the next AI
    // tick can attempt another action during Aimed Shot and clip/interrupt it on
    // some branches.
    if (IsHunterCastTimeActionLocked(player))
    {
        SpellDecision holdDecision;
        holdDecision.reason = "hunter cast-time spell in progress";
        return holdDecision;
    }

    if (SpellDecision const racialDecision = SelectRacialSpell(player, target, allyTarget); racialDecision.spellId)
        return racialDecision;

    if (SpellDecision const utilityDecision = MaybeSelectUtilitySpell(player, target); utilityDecision.spellId)
        return utilityDecision;

    if (!HasHostileTarget(player, target) && !allyTarget)
    {
        if (SpellDecision const racialDecision = SelectRacialSpell(player, target, allyTarget); racialDecision.spellId)
            return racialDecision;

        return {};
    }

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

    std::array<TacticalRule, 11> const rules =
    {{
        { "bg waiting", bgWaiting, "bg move to start", 50.0f },
        { "player has flag", bgActive && values.playerHasFlag, "bg move to objective", 100.0f },
        { "flag pickup nearby", bgActive && values.flagPickupNearby, "bg move to objective", 99.0f },
        { "enemy flag carrier active", bgActive && enemyFlagCarrierActive, "attack enemy flag carrier", 95.0f },
        { "flag pickup available", bgActive && values.flagPickupAvailable, "bg move to objective", 90.0f },
        { "team flag carrier near", bgActive && teamFlagCarrierNear, "bg protect fc", 80.0f },
        { "node assault available", bgActive && values.nodeAssaultAvailable, "bg move to objective", 75.0f },
        { "node defense available", bgActive && values.nodeDefenseAvailable, "bg move to objective", 74.0f },
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

GameObject* PvpCore::FindUsableLightwell(Player const* player, float maxDistance)
{
    if (!player || !player->IsAlive() || !player->FindMap() || maxDistance <= 0.0f)
        return nullptr;

    std::list<GameObject*> lightwells;
    player->GetGameObjectListWithEntryInGrid(lightwells, kPriestLightwellGameObjectEntry, maxDistance);

    GameObject* nearest = nullptr;
    float nearestDistance = maxDistance;
    for (GameObject* lightwell : lightwells)
    {
        if (!lightwell || !lightwell->IsInWorld() || !lightwell->isSpawned() ||
            lightwell->GetGoType() != GAMEOBJECT_TYPE_SPELLCASTER)
            continue;

        GameObjectTemplate const* lightwellTemplate = lightwell->GetGOInfo();
        if (!lightwellTemplate || (lightwellTemplate->spellcaster.charges &&
            lightwell->GetUseCount() >= lightwellTemplate->spellcaster.charges))
            continue;

        Unit* owner = lightwell->GetOwner();
        Player* ownerPlayer = owner ? owner->ToPlayer() : nullptr;
        if (!ownerPlayer || !player->IsInSameRaidWith(ownerPlayer))
            continue;

        float const distance = player->GetExactDist(lightwell);
        if (distance <= nearestDistance)
        {
            nearest = lightwell;
            nearestDistance = distance;
        }
    }

    return nearest;
}

bool PvpCore::ShouldSeekLightwell(Player const* player)
{
    return player && player->InBattleground() && player->IsAlive() &&
        !IsBattlegroundFlagCarrier(player) && player->GetHealthPct() < 65.0f &&
        !(player->GetHealthPct() < 50.0f && SelectReadyHealthstoneItemEntry(player)) &&
        !player->HasAura(kPriestLightwellRenewSpellId) && FindUsableLightwell(player, 20.0f) != nullptr;
}

bool PvpCore::HasHealthstone(Player const* player)
{
    if (!player)
        return false;

    for (uint32 itemEntry : kHealthstoneItemEntries)
        if (player->GetItemByEntry(itemEntry))
            return true;

    return false;
}

GameObject* PvpCore::FindUsableSoulwell(Player const* player, float maxDistance)
{
    if (!player || !player->IsAlive() || !player->FindMap() || maxDistance <= 0.0f)
        return nullptr;

    GameObject* nearest = nullptr;
    float nearestDistance = maxDistance;
    for (uint32 soulwellEntry : kWarlockSoulwellGameObjectEntries)
    {
        std::list<GameObject*> soulwells;
        player->GetGameObjectListWithEntryInGrid(soulwells, soulwellEntry, maxDistance);
        for (GameObject* soulwell : soulwells)
        {
            if (!soulwell || !soulwell->IsInWorld() || !soulwell->isSpawned() ||
                soulwell->GetGoType() != GAMEOBJECT_TYPE_SPELLCASTER)
                continue;

            GameObjectTemplate const* soulwellTemplate = soulwell->GetGOInfo();
            if (!soulwellTemplate || (soulwellTemplate->spellcaster.charges &&
                soulwell->GetUseCount() >= soulwellTemplate->spellcaster.charges))
                continue;

            Unit* owner = soulwell->GetOwner();
            Player* ownerPlayer = owner ? owner->ToPlayer() : nullptr;
            if (!ownerPlayer || !player->IsInSameRaidWith(ownerPlayer))
                continue;

            float const distance = player->GetExactDist(soulwell);
            if (distance <= nearestDistance)
            {
                nearest = soulwell;
                nearestDistance = distance;
            }
        }
    }

    return nearest;
}

bool PvpCore::IsBattlegroundFlagCarrier(Player const* player)
{
    if (!player || !player->InBattleground())
        return false;

    Battleground const* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    ObjectGuid const playerGuid = player->GetGUID();
    if (battleground->GetFlagPickerGUID(TEAM_ALLIANCE) == playerGuid ||
        battleground->GetFlagPickerGUID(TEAM_HORDE) == playerGuid ||
        battleground->GetFlagPickerGUID() == playerGuid)
        return true;

    return std::any_of(kBattlegroundFlagCarrierAuraIds.begin(), kBattlegroundFlagCarrierAuraIds.end(),
        [player](uint32 spellId) { return player->HasAura(spellId); });
}

bool PvpCore::SpellWouldBreakFlagCarry(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    return spellInfo && spellInfo->WouldDropBattlegroundFlag();
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

bool PvpCore::GetHunterAutoShotRange(Player const* player, Unit const* target, HunterAutoShotRangeInfo& rangeInfo)
{
    rangeInfo = {};
    if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive() || !player->HasSpell(kHunterAutoShotSpellId))
        return false;

    SpellInfo const* autoShotInfo = sSpellMgr->GetSpellInfo(kHunterAutoShotSpellId);
    if (!autoShotInfo || !autoShotInfo->RangeEntry)
        return false;

    rangeInfo.exactDistance = player->GetExactDist(target);
    rangeInfo.dbcMinRange = player->GetSpellMinRangeForTarget(target, autoShotInfo);
    if (autoShotInfo->RangeEntry->Flags & SPELL_RANGE_RANGED)
        rangeInfo.meleeRange = player->GetMeleeRange(target);
    rangeInfo.minRange = rangeInfo.dbcMinRange + rangeInfo.meleeRange;

    // Keep this in the same order as Spell::GetMinMaxRange(): target-sensitive
    // base range, ranged-weapon range modifier, caster spell-range modifiers,
    // then combat reach (plus the moving-target tolerance used by the core).
    rangeInfo.maxRange = player->GetSpellMaxRangeForTarget(target, autoShotInfo);
    if (autoShotInfo->HasAttribute(SPELL_ATTR0_REQ_AMMO))
        if (Item* rangedWeapon = player->GetWeaponForAttack(RANGED_ATTACK, true))
            rangeInfo.maxRange *= rangedWeapon->GetTemplate()->RangedModRange * 0.01f;

    if (Player* modOwner = player->GetSpellModOwner())
        modOwner->ApplySpellMod(autoShotInfo->Id, SPELLMOD_RANGE, rangeInfo.maxRange, nullptr);

    float maxRangeMod = player->GetCombatReach() + target->GetCombatReach();
    if (player->isMoving() && target->isMoving() && !player->IsWalking() && !target->IsWalking() && target->GetTypeId() == TYPEID_PLAYER)
        maxRangeMod += 8.0f / 3.0f;
    rangeInfo.maxRange += maxRangeMod;

    return rangeInfo.minRange > 0.0f && rangeInfo.maxRange > rangeInfo.minRange;
}

bool PvpCore::CanMageBlinkOutOfControl(Player const* player)
{
    return IsMageBlinkableControl(player) && IsSpellReady(player, 1953);
}

bool PvpCore::CanHunterBestialWrathOutOfControl(Player const* player)
{
    return IsHunterBestialWrathBreakableControl(player) && IsSpellReady(player, 81300);
}

bool PvpCore::IsEffectivelyImmuneTarget(Player const* player, Unit const* target)
{
    return IsTargetEffectivelyImmune(player, target);
}

bool PvpCore::IsMovementPreventedByRoot(Player const* player)
{
    return player &&
        (player->HasUnitState(UNIT_STATE_ROOT) ||
            player->HasAuraType(SPELL_AURA_MOD_ROOT) ||
            player->HasAuraWithMechanic(1u << MECHANIC_ROOT));
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
    // Re-evaluate from live keeper/aura state as well. The values object can be
    // collected immediately before a flag click and then reused later in the
    // same update, so relying only on its original objective snapshot lets a
    // pre-pickup stealth/Feign decision survive after the bot becomes carrier.
    values.playerHasFlag = values.playerHasFlag || IsBattlegroundFlagCarrier(player);
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
        case PvpTrigger::FlagPickupAvailable:
            return values.flagPickupAvailable;
        case PvpTrigger::EnemyFlagCarrierActive:
            return values.enemyFlagCarrierActive;
        case PvpTrigger::EnemyFlagCarrierNear:
            return values.enemyFlagCarrierNear;
        case PvpTrigger::TeamFlagCarrierNear:
            return values.teamFlagCarrierNear;
        case PvpTrigger::NodeAssaultAvailable:
            return values.nodeAssaultAvailable;
        case PvpTrigger::NodeDefenseAvailable:
            return values.nodeDefenseAvailable;
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
    context.nearbyEnemyActive = values.nearbyEnemyActive;
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP human-first context: guid={} human_count={} has_humans={} player_has_flag={} flag_pickup_available={} nearby_enemy={} directive={} action={}.",
        player->GetGUID().ToString(), values.battlegroundTeamHumanCount, values.battlegroundTeamHasHumans, values.playerHasFlag,
        values.flagPickupAvailable, values.nearbyEnemyActive, static_cast<uint8>(context.flagCarrierDirective),
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
    // A carrier always owns its capture route. A midfield bot owns its live
    // pickup route only while local combat pressure is clear; nearby enemies
    // deliberately hand movement back to combat. Without this shared decision,
    // class range/facing and tactical navmesh movement replace each other on
    // alternating cadences and produce the visible run/turn/stop loop.
    context.preserveFlagObjectiveMovement = inActiveBattleground &&
        (values.playerHasFlag || values.flagPickupNearby ||
            (values.flagPickupAvailable && !values.nearbyEnemyActive));
    context.preserveFlagCarrierMovement = inActiveBattleground && values.playerHasFlag;
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

        // Preparation is a closed-gates support phase. Once consumables,
        // buffs, pets, and mounting are complete, do not fall through into
        // the ordinary enemy/pet combat graph before the match starts.
        return context;
    }

    if (!player->IsInCombat())
    {
        SpellDecision const raidBuffDecision = SelectMissingBattlegroundRaidBuff(player);
        if (raidBuffDecision.spellId)
        {
            context.actionName = raidBuffDecision.actionName;
            context.reason = raidBuffDecision.reason;
            context.spellId = raidBuffDecision.spellId;
            context.targetMode = raidBuffDecision.targetMode;
            context.targetGuid = raidBuffDecision.targetGuid;
            context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
            context.shouldExecute = true;
            return context;
        }
    }

    ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
    Unit const* combatVictim = player->GetVictim();
    bool const hasEffectivelyImmuneCombatVictim = combatVictim && HasHostileTarget(player, combatVictim) &&
        IsTargetEffectivelyImmune(player, combatVictim);
    ObjectGuid const selectedTargetGuid = SelectCombatTargetGuid(player);
    ObjectGuid activeTargetGuid = selectedTargetGuid;
    if (activeTargetGuid.IsEmpty())
        if (HasHostileTarget(player, combatVictim) && !hasEffectivelyImmuneCombatVictim)
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

    // Wisp Form and the Elune's Grace fade/invisibility sequence are escape
    // windows, not opportunities to continue the normal spell rotation. The
    // tactical movement layer owns the retreat while these auras are active;
    // yielding here prevents class facing/range directives or a fresh cast
    // from replacing that movement and immediately breaking invisibility.
    if (player->HasAura(kPriestElunesGraceSpellId) || player->HasInvisibilityAura() ||
        (player->GetClass() == CLASS_PRIEST && player->HasAura(kPriestWispFormSpellId)))
        return context;

    bool const inSpiritOfRedemption = IsPriestInSpiritOfRedemption(player);
    bool const movementPreventedByRoot = PvpCore::IsMovementPreventedByRoot(player);
    bool const criticalLowMana = !inSpiritOfRedemption &&
        !UsesMeleeSpacingProfile(player, profileSelection) &&
        player->GetClass() != CLASS_HUNTER &&
        player->GetClass() != CLASS_WARLOCK &&
        player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 10.0f;

    // Hard-priority mana preservation policy for ranged/healing profiles: below
    // 10% mana, disengage above all other class behavior, then drink as soon as
    // combat drops. Melee mana users keep pressure with attacks instead of
    // oscillating between chase and retreat as their mana crosses the threshold.
    // Spirit of Redemption is exempt because its healing casts are free and the
    // aura duration is too short to spend on drinking or disengaging.
    if (criticalLowMana && !movementPreventedByRoot)
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
        // Do not emit hunter flee directives from the spell selector. Hunter
        // movement is timer-driven in PlayerbotPvpLifecycleActions: run while
        // Auto Shot is charging, plant briefly to fire, then resume running.
        // Returning FleeTooCloseForSpell here created a second movement owner
        // that fought the lifecycle loop and produced triangle/orbit movement.
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
        if (!candidate.spellId && !candidate.itemEntry)
            break;

        if (!firstDecision.spellId && !firstDecision.itemEntry)
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

        if (!candidate.spellId)
            break;

        suppressedSpellId = candidate.spellId;
    }

    // If no immediately castable spell or item was found, keep the first decision so execution
    // can still drive movement/position correction (for example out-of-range follow).
    if (!decision.spellId && !decision.itemEntry)
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
        if (losRecoveryTarget && !player->IsWithinLOSInMap(losRecoveryTarget) && !movementPreventedByRoot)
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

    // A selected spell owns its facing at execution time. Cast-time spells
    // stop and face before casting; instant/move-allowed spells update facing
    // without replacing their active retreat spline. Converting the spell to
    // a separate FaceSpellTarget directive here made casting wait for movement
    // to finish and caused move/face spline oscillation.

    if (context.spellId && !movementPreventedByRoot &&
        context.targetMode == PvpClassSpellContext::TargetMode::Enemy && UsesMeleeSpacingProfile(player, profileSelection))
    {
        Unit const* meleeTarget = resolveTargetByGuid(context.targetGuid);
        // Gap closers and ranged chase tools belong on this allowlist: they are
        // the mechanism used to reach or hold melee range in the first place.
        // Without them here, this generic
        // "not in melee yet" check zeroes context.spellId and silently substitutes
        // a plain ReachMeleeRange walk before CastDirectSpell/NotifyDuelDecision
        // are ever reached, which reads as the bot doing nothing but walking and
        // never whispering a decision at all.
        bool const canCastOutsideMelee = context.spellId == 11578 || context.spellId == 20617 || context.spellId == 62124 ||
            context.spellId == 81271 || context.spellId == 82419 || context.spellId == 49376 || context.spellId == 16979 ||
            IsShamanFrostShockSpell(context.spellId);
        if (meleeTarget && !player->IsWithinMeleeRange(meleeTarget) && !canCastOutsideMelee)
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
    if (context.spellId && !movementPreventedByRoot && UsesRangedSpacingProfile(player, profileSelection))
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
                if (!(player->GetClass() == CLASS_HUNTER && context.targetMode == PvpClassSpellContext::TargetMode::Enemy &&
                    IsHunterWithinAutoShotBand(player, spacingTarget)))
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, spacingTarget->GetGUID(),
                        ComputeApproachFollowRange(maxRange), "reach spell", "selected spell out of range", 84.0f);
                }
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
                if (player->GetClass() != CLASS_HUNTER)
                {
                    // Enter too-close movement before strict dead-zone boundaries so
                    // ranged users do not idle in 5-8y style min-range gaps.
                    // Hunters are excluded because their movement is owned by the
                    // Auto Shot stutter loop in lifecycle; emitting spell-selector
                    // flee directives here creates competing movement vectors.
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
                distance > (GetConfiguredSpellRange() + kRangedSpacingEnterOutOfRangeBuffer) &&
                !IsHunterWithinAutoShotBand(player, spacingTarget))
            {
                // Some classic spell entries report atypical range metadata,
                // which can leave ranged bots idling at ~35-40y while still
                // selecting enemy casts. Keep a config-based engage floor so
                // they always step in to practical casting distance. Hunters are
                // the exception: if they are already in Auto Shot range, do not
                // pull them back inward while kiting just because an instant shot
                // would prefer a slightly shorter configured spell distance.
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
    if (!context.spellId && hasValidTarget && player->GetClass() != CLASS_HUNTER &&
        UsesRangedSpacingProfile(player, profileSelection) && !healerHasLivingAllyTarget)
    {
        Unit const* movementTarget = resolveTargetByGuid(activeTargetGuid);
        if (movementTarget)
        {
            float const distance = player->GetDistance(movementTarget);
            if (distance > (GetConfiguredSpellRange() + kRangedSpacingEnterOutOfRangeBuffer) &&
                !IsHunterWithinAutoShotBand(player, movementTarget))
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, movementTarget->GetGUID(),
                    ComputeApproachFollowRange(GetConfiguredSpellRange()), "reach spell", "enemy out of spell range", 70.0f);
            }
            else if (IsHunterExactDeadZone(player, movementTarget))
            {
                // No castable spell + hunter dead-zone often left the bot in a
                // bow-raise idle loop. Force retreat out past ranged minimum
                // instead of collapsing into melee.
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, movementTarget->GetGUID(),
                    ComputeHunterDeadZoneRetreatStep(player, movementTarget),
                    "hunter deadzone retreat", "enemy in hunter dead-zone", 100.0f);
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
            if (IsWandShootReadyForDecision(player) && !HasBreakableCrowdControl(fallbackTarget))
            {
                context.actionName = "fallback wand";
                context.reason = "low mana fallback to wand pressure";
                context.spellId = kWandShootSpellId;
                context.targetMode = PvpClassSpellContext::TargetMode::Enemy;
                context.targetGuid = fallbackTarget->GetGUID();
                context.selfCast = false;
            }
            else
            {
                if (UsesRangedSpacingProfile(player, profileSelection) && player->GetClass() != CLASS_HUNTER)
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, fallbackTarget->GetGUID(),
                        std::max(1.0f, GetConfiguredCloseRange()), "flee", "low mana fallback disengage to recover", 67.0f);
                }
                else if (player->GetClass() != CLASS_HUNTER)
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, fallbackTarget->GetGUID(),
                        std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "low mana fallback to auto-attack", 67.0f);
                }
            }
        }
    }

    // PvP insignia/class-trinket crowd-control breaks and holy-priest Spirit
    // of Redemption are handled by per-player fast paths in
    // RandomBotParticipationManager::ProcessPlayerLifecycle. Keep them out of
    // the normal class-spell decision graph so they are not throttled by
    // decision cadence and do not consume the class action tick/GCD.

    context.shouldExecute = context.shouldExecute || context.spellId != 0 || context.itemEntry != 0;

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

    if (IsTriggerActive(PvpTrigger::PlayerHasFlag, values) || IsTriggerActive(PvpTrigger::FlagPickupAvailable, values))
        objective.type = BattlegroundObjectiveType::CaptureFlag;
    else if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierActive, values))
        objective.type = BattlegroundObjectiveType::AttackFlagCarrier;
    else if (IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
        objective.type = BattlegroundObjectiveType::ProtectFlagCarrier;
    else if (IsTriggerActive(PvpTrigger::NodeAssaultAvailable, values))
    {
        objective.type = BattlegroundObjectiveType::AssaultNode;
        objective.objectiveId = values.nodeObjectiveId;
    }
    else if (IsTriggerActive(PvpTrigger::NodeDefenseAvailable, values))
    {
        objective.type = BattlegroundObjectiveType::DefendNode;
        objective.objectiveId = values.nodeObjectiveId;
    }

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
    if (IsTriggerActive(PvpTrigger::PlayerHasFlag, values))
        return FlagCarrierDirective::None;

    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierActive, values))
        return FlagCarrierDirective::AttackEnemyCarrier;

    if (IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
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
