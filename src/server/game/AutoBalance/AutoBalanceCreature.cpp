#include "AutoBalance/AutoBalanceCreature.h"
#include "AutoBalance/AutoBalanceConfig.h"
#include "AutoBalance/AutoBalanceMapData.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "SharedDefines.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <cmath>
#include <limits>

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

    void ApplyInflectionOverride(InflectionPointSettings& settings, InflectionOverride const& override)
    {
        if (override.Value)
            settings.Value = *override.Value;
        if (override.CurveFloor)
            settings.CurveFloor = *override.CurveFloor;
        if (override.CurveCeiling)
            settings.CurveCeiling = *override.CurveCeiling;
        if (override.BossModifier)
            settings.BossModifier = *override.BossModifier;
    }

    void ApplyStatOverride(StatModifierValues& values, StatModifierOverride const& override)
    {
        if (override.Global)
            values.Global = *override.Global;
        if (override.Health)
            values.Health = *override.Health;
        if (override.Mana)
            values.Mana = *override.Mana;
        if (override.Armor)
            values.Armor = *override.Armor;
        if (override.Damage)
            values.Damage = *override.Damage;
        if (override.CrowdControlDuration)
            values.CrowdControlDuration = *override.CrowdControlDuration;
    }

    bool IsBossCreature(Creature const& creature)
    {
        if (creature.IsDungeonBoss() || creature.isWorldBoss())
            return true;

        if (TempSummon const* summon = creature.ToTempSummon())
        {
            if (Unit const* summoner = summon->GetSummonerUnit())
            {
                Creature const* summonerCreature = summoner->ToCreature();
                if (summonerCreature && (summonerCreature->IsDungeonBoss() || summonerCreature->isWorldBoss()))
                    return true;
            }
        }

        return false;
    }

    float EvaluateInflectionMultiplier(uint32 effectivePlayers, uint32 targetPlayers, InflectionPointSettings const& settings)
    {
        if (!effectivePlayers)
            effectivePlayers = 1;

        if (!targetPlayers)
            targetPlayers = 1;

        float const maxPlayers = static_cast<float>(targetPlayers);
        float const adjustedPlayers = static_cast<float>(effectivePlayers);

        float diff = (maxPlayers / 5.0f) * 1.5f;
        if (!std::isfinite(diff) || diff <= 0.0f)
            diff = 1.0f;

        auto evaluate = [&](float players)
        {
            return (std::tanh((players - settings.Value) / diff) + 1.0f) / 2.0f;
        };

        float curveCeilingAdjustment = 1.0f;
        {
            float const numerator = evaluate(maxPlayers);
            float const denominator = numerator * (settings.CurveCeiling - settings.CurveFloor) + settings.CurveFloor;
            if (std::fabs(denominator) > std::numeric_limits<float>::epsilon())
                curveCeilingAdjustment = settings.CurveCeiling / denominator;
        }

        float multiplier = evaluate(adjustedPlayers) * (settings.CurveCeiling * curveCeilingAdjustment - settings.CurveFloor) + settings.CurveFloor;
        if (!std::isfinite(multiplier) || multiplier <= 0.0f)
            multiplier = 0.01f;

        return multiplier;
    }

    InflectionPointSettings SelectInflectionSettings(ModuleConfig const& config, Map const* map, uint32 targetPlayers, bool isBoss)
    {
        InflectionPointSettings settings = map->IsRaid()
            ? (map->IsHeroic() ? config.RaidHeroicInflection : config.RaidInflection)
            : (map->IsHeroic() ? config.DungeonHeroicInflection : config.DungeonInflection);

        if (map->IsRaid())
        {
            auto const& overrides = map->IsHeroic() ? config.RaidHeroicInflectionOverrides : config.RaidInflectionOverrides;
            if (auto const it = overrides.find(targetPlayers); it != overrides.end())
                ApplyInflectionOverride(settings, it->second);
        }

        if (auto const it = config.InflectionOverridesByInstance.find(map->GetId()); it != config.InflectionOverridesByInstance.end())
            ApplyInflectionOverride(settings, it->second);

        if (isBoss)
        {
            float bossMultiplier = settings.BossModifier;
            if (auto const it = config.InflectionBossOverridesByInstance.find(map->GetId()); it != config.InflectionBossOverridesByInstance.end())
                bossMultiplier = it->second;

            if (!std::isfinite(bossMultiplier) || bossMultiplier <= 0.0f)
                bossMultiplier = 1.0f;

            settings.Value *= bossMultiplier;
            settings.BossModifier = bossMultiplier;
        }

        return settings;
    }

    StatModifierValues SelectStatModifiers(ModuleConfig const& config, Map const* map, Creature const& creature, bool isBoss, uint32 targetPlayers)
    {
        StatModifierValues values;

        if (map->IsRaid())
            values = map->IsHeroic() ? (isBoss ? config.RaidHeroicBossStatModifiers : config.RaidHeroicStatModifiers)
                                     : (isBoss ? config.RaidBossStatModifiers : config.RaidStatModifiers);
        else
            values = map->IsHeroic() ? (isBoss ? config.DungeonHeroicBossStatModifiers : config.DungeonHeroicStatModifiers)
                                     : (isBoss ? config.DungeonBossStatModifiers : config.DungeonStatModifiers);

        if (map->IsRaid())
        {
            auto const& overrides = map->IsHeroic()
                ? (isBoss ? config.RaidHeroicBossStatOverridesBySize : config.RaidHeroicStatOverridesBySize)
                : (isBoss ? config.RaidBossStatOverridesBySize : config.RaidStatOverridesBySize);

            if (auto const it = overrides.find(targetPlayers); it != overrides.end())
                ApplyStatOverride(values, it->second);
        }

        auto const& instanceOverrides = isBoss ? config.StatModifierBossOverridesByInstance : config.StatModifierOverridesByInstance;
        if (auto const it = instanceOverrides.find(map->GetId()); it != instanceOverrides.end())
            ApplyStatOverride(values, it->second);

        auto const& creatureOverrides = isBoss ? config.StatModifierBossOverridesByCreature : config.StatModifierOverridesByCreature;
        if (auto const it = creatureOverrides.find(creature.GetEntry()); it != creatureOverrides.end())
            ApplyStatOverride(values, it->second);

        return values;
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

AutoBalanceCreatureInfo* TryGetCreatureInfo(Unit* unit)
{
    if (!unit)
        return nullptr;

    Creature* creature = unit->ToCreature();
    if (!creature)
        return nullptr;

    AutoBalanceCreatureInfo& info = GetCreatureInfo(*creature);
    if (!info.Initialized)
        return nullptr;

    return &info;
}

AutoBalanceCreatureInfo const* TryGetCreatureInfo(Unit const* unit)
{
    if (!unit)
        return nullptr;

    Creature const* creature = unit->ToCreature();
    if (!creature)
        return nullptr;

    AutoBalanceCreatureInfo const& info = GetCreatureInfo(*creature);
    if (!info.Initialized)
        return nullptr;

    return &info;
}

Optional<float> GetDamageHealingMultiplier(Unit const* unit)
{
    if (!IsEnabled())
        return { };

    AutoBalanceCreatureInfo const* info = TryGetCreatureInfo(unit);
    if (!info)
        return { };

    float const multiplier = info->Multipliers.Damage;
    if (!std::isfinite(multiplier) || multiplier <= 0.0f)
        return { };

    if (std::fabs(multiplier - 1.0f) <= 0.0005f)
        return { };

    return multiplier;
}

Optional<float> GetCrowdControlDurationMultiplier(Unit const* unit)
{
    if (!IsEnabled())
        return { };

    AutoBalanceCreatureInfo const* info = TryGetCreatureInfo(unit);
    if (!info)
        return { };

    float multiplier = info->Multipliers.CrowdControlDuration;
    if (!std::isfinite(multiplier) || multiplier <= 0.0f)
        return { };

    ModuleConfig const& config = GetConfig();
    multiplier = std::clamp(multiplier, config.MinCCDurationModifier, config.MaxCCDurationModifier);

    if (std::fabs(multiplier - 1.0f) <= 0.0005f)
        return { };

    return multiplier;
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
    bool const isBoss = IsBossCreature(*creature);

    InflectionPointSettings const inflectionSettings = SelectInflectionSettings(config, map, targetPlayers, isBoss);
    float const baseMultiplier = EvaluateInflectionMultiplier(effectivePlayers, targetPlayers, inflectionSettings);
    StatModifierValues const statModifiers = SelectStatModifiers(config, map, *creature, isBoss, targetPlayers);

    auto sanitize = [](float value, float fallback)
    {
        if (!std::isfinite(value) || value <= 0.0f)
            return fallback;
        return value;
    };

    float healthMultiplier = sanitize(baseMultiplier * statModifiers.Global * statModifiers.Health, config.MinHPModifier);
    float manaMultiplier = sanitize(baseMultiplier * statModifiers.Global * statModifiers.Mana, config.MinManaModifier);
    float armorMultiplier = sanitize(baseMultiplier * statModifiers.Global * statModifiers.Armor, 1.0f);
    float damageMultiplier = sanitize(baseMultiplier * statModifiers.Global * statModifiers.Damage, config.MinDamageModifier);
    float crowdControlMultiplier = sanitize(baseMultiplier * statModifiers.CrowdControlDuration, 1.0f);

    healthMultiplier = std::max(healthMultiplier, config.MinHPModifier);
    manaMultiplier = std::max(manaMultiplier, config.MinManaModifier);
    armorMultiplier = std::max(armorMultiplier, 0.01f);
    damageMultiplier = std::max(damageMultiplier, config.MinDamageModifier);
    crowdControlMultiplier = std::clamp(crowdControlMultiplier, config.MinCCDurationModifier, config.MaxCCDurationModifier);

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
        std::fabs(info.Multipliers.Health - healthMultiplier) > 0.0005f ||
        std::fabs(info.Multipliers.Mana - manaMultiplier) > 0.0005f ||
        std::fabs(info.Multipliers.Damage - damageMultiplier) > 0.0005f ||
        std::fabs(info.Multipliers.Armor - armorMultiplier) > 0.0005f ||
        std::fabs(info.Multipliers.CrowdControlDuration - crowdControlMultiplier) > 0.0005f;

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

    info.Multipliers.Health = healthMultiplier;
    info.Multipliers.Mana = manaMultiplier;
    info.Multipliers.Damage = damageMultiplier;
    info.Multipliers.Armor = armorMultiplier;
    info.Multipliers.CrowdControlDuration = crowdControlMultiplier;

    uint32 const oldMaxHealth = creature->GetMaxHealth();
    float const healthPct = oldMaxHealth ? std::clamp(static_cast<float>(creature->GetHealth()) / static_cast<float>(oldMaxHealth), 0.0f, 1.0f) : 1.0f;
    uint32 const newHealth = std::max<uint32>(1u, static_cast<uint32>(std::round(info.BaseValues.Health * info.Multipliers.Health)));
    creature->SetCreateHealth(newHealth);
    creature->SetMaxHealth(newHealth);
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, static_cast<float>(newHealth));
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
        creature->SetStatFlatModifier(UNIT_MOD_MANA, BASE_VALUE, static_cast<float>(newMana));
        uint32 const scaledMana = std::min<uint32>(static_cast<uint32>(std::round(newMana * manaPct)), newMana);
        creature->SetPower(POWER_MANA, scaledMana);
    }

    uint32 const newArmor = static_cast<uint32>(std::round(info.BaseValues.Armor * info.Multipliers.Armor));
    creature->SetArmor(newArmor);
    creature->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, static_cast<float>(newArmor));

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
        TC_LOG_DEBUG(LogFilter, "AutoBalance::ScaleCreature - entry={} level={} players={}/{} base={:.3f} health={:.3f} mana={:.3f} damage={:.3f} armor={:.3f} cc={:.3f}",
            creature->GetEntry(), level, effectivePlayers, targetPlayers, baseMultiplier, info.Multipliers.Health, info.Multipliers.Mana, info.Multipliers.Damage, info.Multipliers.Armor, info.Multipliers.CrowdControlDuration);
}
}
