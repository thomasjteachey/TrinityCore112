#include "AutoBalance/AutoBalanceCreature.h"
#include "AutoBalance/AutoBalanceConfig.h"
#include "AutoBalance/AutoBalanceMapData.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "SharedDefines.h"
#include <algorithm>
#include <cmath>

namespace AutoBalance
{
namespace
{
    bool IsMapEnabled(ModuleConfig const& config, Map const* map)
    {
        if (!map)
            return false;

        if (std::find(config.DisabledInstances.begin(), config.DisabledInstances.end(), map->GetId()) != config.DisabledInstances.end())
            return false;

        if (!map->IsDungeon())
            return config.EnableWorldMaps;

        if (map->IsRaid())
            return config.EnableRaids;

        return config.EnableDungeons;
    }

    uint32 GetTargetPlayerCount(Map const* map)
    {
        if (!map)
            return 1;

        if (InstanceMap const* instance = map->ToInstanceMap())
        {
            if (uint32 maxPlayers = instance->GetMaxPlayers())
                return maxPlayers;
        }

        return 1;
    }

    float CalculateMultiplier(uint32 effectivePlayers, uint32 targetPlayers)
    {
        if (!effectivePlayers)
            effectivePlayers = 1;

        if (!targetPlayers)
            targetPlayers = 1;

        float multiplier = static_cast<float>(effectivePlayers) / static_cast<float>(targetPlayers);
        if (!std::isfinite(multiplier) || multiplier <= 0.0f)
            multiplier = 0.01f;

        return multiplier;
    }

}

AutoBalanceCreatureInfo& GetCreatureInfo(Creature& creature)
{
    return creature.GetCustomData().AutoBalance.CreatureInfo;
}

AutoBalanceCreatureInfo const& GetCreatureInfo(Creature const& creature)
{
    return creature.GetCustomData().AutoBalance.CreatureInfo;
}

void ScaleCreature(Creature* creature)
{
    if (!creature)
        return;

    if (!IsEnabled())
        return;

    Map* map = creature->GetMap();
    if (!map)
        return;

    ModuleConfig const& config = GetConfig();
    if (!IsMapEnabled(config, map))
        return;

    uint32 const effectivePlayers = GetEffectivePlayerCount(map);
    uint32 const targetPlayers = GetTargetPlayerCount(map);
    float const multiplier = CalculateMultiplier(effectivePlayers, targetPlayers);

    CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
    if (!creatureTemplate)
        return;

    uint8 const level = creature->GetLevel();
    CreatureBaseStats const* baseStats = sObjectMgr->GetCreatureBaseStats(level, creatureTemplate->unit_class);
    if (!baseStats)
        return;

    AutoBalanceCreatureInfo& info = GetCreatureInfo(*creature);

    bool const requiresUpdate = !info.Initialized ||
        info.BaseLevel != level ||
        info.TargetPlayerCount != targetPlayers ||
        info.EffectivePlayerCount != effectivePlayers ||
        std::fabs(info.Multipliers.Health - multiplier) > 0.0005f ||
        std::fabs(info.Multipliers.Mana - multiplier) > 0.0005f ||
        std::fabs(info.Multipliers.Damage - multiplier) > 0.0005f ||
        std::fabs(info.Multipliers.Armor - multiplier) > 0.0005f;

    if (!requiresUpdate)
        return;

    info.BaseValues.Health = baseStats->GenerateHealth(creatureTemplate);
    info.BaseValues.Mana = baseStats->GenerateMana(creatureTemplate);
    info.BaseValues.Armor = baseStats->GenerateArmor(creatureTemplate);

    float const baseDamage = baseStats->GenerateBaseDamage(creatureTemplate);
    info.BaseValues.MinDamage = baseDamage;
    info.BaseValues.MaxDamage = baseDamage * 1.5f;
    info.BaseValues.AttackPower = static_cast<float>(baseStats->AttackPower);
    info.BaseValues.RangedAttackPower = static_cast<float>(baseStats->RangedAttackPower);

    info.BaseLevel = level;
    info.TargetPlayerCount = targetPlayers;
    info.EffectivePlayerCount = effectivePlayers;
    info.Initialized = true;

    info.Multipliers.Health = multiplier;
    info.Multipliers.Mana = multiplier;
    info.Multipliers.Damage = multiplier;
    info.Multipliers.Armor = multiplier;

    uint32 const oldMaxHealth = creature->GetMaxHealth();
    float const healthPct = oldMaxHealth ? std::clamp(static_cast<float>(creature->GetHealth()) / static_cast<float>(oldMaxHealth), 0.0f, 1.0f) : 1.0f;
    uint32 const newHealth = std::max<uint32>(1u, static_cast<uint32>(std::round(info.BaseValues.Health * info.Multipliers.Health)));
    creature->SetCreateHealth(newHealth);
    creature->SetMaxHealth(newHealth);
    creature->SetModifierValue(UNIT_MOD_HEALTH, BASE_VALUE, static_cast<float>(newHealth));
    uint32 const scaledHealth = std::clamp<uint32>(static_cast<uint32>(std::round(newHealth * healthPct)), 1u, newHealth);
    creature->SetHealth(scaledHealth);

    uint32 const oldMaxMana = creature->GetMaxPower(POWER_MANA);
    uint32 const baseMana = info.BaseValues.Mana;
    if (baseMana || oldMaxMana)
    {
        float const manaPct = oldMaxMana ? std::clamp(static_cast<float>(creature->GetPower(POWER_MANA)) / static_cast<float>(oldMaxMana), 0.0f, 1.0f) : 1.0f;
        uint32 const newMana = static_cast<uint32>(std::round(baseMana * info.Multipliers.Mana));
        creature->SetCreateMana(newMana);
        creature->SetMaxPower(POWER_MANA, newMana);
        creature->SetModifierValue(UNIT_MOD_MANA, BASE_VALUE, static_cast<float>(newMana));
        uint32 const scaledMana = std::min<uint32>(static_cast<uint32>(std::round(newMana * manaPct)), newMana);
        creature->SetPower(POWER_MANA, scaledMana);
    }

    uint32 const newArmor = static_cast<uint32>(std::round(info.BaseValues.Armor * info.Multipliers.Armor));
    creature->SetArmor(newArmor);
    creature->SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, static_cast<float>(newArmor));

    float const scaledMinDamage = info.BaseValues.MinDamage * info.Multipliers.Damage;
    float const scaledMaxDamage = info.BaseValues.MaxDamage * info.Multipliers.Damage;

    for (WeaponAttackType attackType : { BASE_ATTACK, OFF_ATTACK, RANGED_ATTACK })
    {
        creature->SetBaseWeaponDamage(attackType, MINDAMAGE, scaledMinDamage);
        creature->SetBaseWeaponDamage(attackType, MAXDAMAGE, scaledMaxDamage);
    }

    creature->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER, BASE_VALUE, info.BaseValues.AttackPower * info.Multipliers.Damage);
    creature->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, BASE_VALUE, info.BaseValues.RangedAttackPower * info.Multipliers.Damage);

    creature->UpdateDamagePhysical(BASE_ATTACK);
    creature->UpdateDamagePhysical(OFF_ATTACK);
    creature->UpdateDamagePhysical(RANGED_ATTACK);

    if (config.DebugLogging)
        TC_LOG_DEBUG(LogFilter, "AutoBalance::ScaleCreature - entry={} level={} players={}/{} multiplier={:.3f}",
            creature->GetEntry(), level, effectivePlayers, targetPlayers, multiplier);
}
}
