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
    constexpr float DefaultInflectionRatio = 0.35f;

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

    float GetBaseExpansionValueForLevel(float const (&baseValues)[MAX_EXPANSIONS], uint8 targetLevel)
    {
        float const vanilla = baseValues[EXPANSION_CLASSIC];
        float const burningCrusade = baseValues[EXPANSION_THE_BURNING_CRUSADE];
        float const wrath = baseValues[EXPANSION_WRATH_OF_THE_LICH_KING];

        if (targetLevel <= 60)
            return vanilla;

        if (targetLevel < 63)
        {
            float const vanillaMultiplier = (63.0f - static_cast<float>(targetLevel)) / 3.0f;
            float const bcMultiplier = 1.0f - vanillaMultiplier;
            return vanilla * vanillaMultiplier + burningCrusade * bcMultiplier;
        }

        if (targetLevel <= 70)
            return burningCrusade;

        if (targetLevel < 73)
        {
            float const bcMultiplier = (73.0f - static_cast<float>(targetLevel)) / 3.0f;
            float const wrathMultiplier = 1.0f - bcMultiplier;
            return burningCrusade * bcMultiplier + wrath * wrathMultiplier;
        }

        return wrath;
    }

    float GetBaseExpansionValueForLevel(uint32 const (&baseValues)[MAX_EXPANSIONS], uint8 targetLevel)
    {
        float converted[MAX_EXPANSIONS];
        for (uint8 i = 0; i < MAX_EXPANSIONS; ++i)
            converted[i] = static_cast<float>(baseValues[i]);

        return GetBaseExpansionValueForLevel(converted, targetLevel);
    }

    InflectionPointSettings SelectInflectionSettings(ModuleConfig const& config, Map const* map, uint32 targetPlayers, bool isBoss)
    {
        uint32 const maxPlayers = std::max<uint32>(targetPlayers, 1u);
        float const playerCount = static_cast<float>(maxPlayers);

        auto sanitizeRatio = [](float value, float fallback)
        {
            if (!std::isfinite(value) || value <= 0.0f)
                return fallback;
            return value;
        };

        float curveFloor = 0.0f;
        float curveCeiling = 1.0f;
        float baseBossMultiplier = 1.0f;
        float inflectionValue = playerCount;

        auto assignFrom = [&](InflectionPointSettings const& base)
        {
            float const ratio = sanitizeRatio(base.Value, DefaultInflectionRatio);
            inflectionValue = playerCount * ratio;
            curveFloor = base.CurveFloor;
            curveCeiling = base.CurveCeiling;
            baseBossMultiplier = base.BossModifier;
        };

        bool const isRaid = map->IsRaid();
        bool const isHeroic = map->IsHeroic();

        if (isHeroic)
        {
            if (maxPlayers <= 5)
                assignFrom(config.DungeonHeroicInflection);
            else if (isRaid)
            {
                if (maxPlayers <= 10)
                    assignFrom(config.RaidHeroicInflection10);
                else if (maxPlayers <= 25)
                    assignFrom(config.RaidHeroicInflection25);
                else
                    assignFrom(config.RaidHeroicInflection);
            }
            else
            {
                assignFrom(config.DungeonHeroicInflection);
            }
        }
        else if (!isRaid || maxPlayers <= 5)
        {
            assignFrom(config.DungeonInflection);
        }
        else if (maxPlayers <= 10)
        {
            assignFrom(config.RaidInflection10);
        }
        else if (maxPlayers <= 15)
        {
            assignFrom(config.RaidInflection15);
        }
        else if (maxPlayers <= 20)
        {
            assignFrom(config.RaidInflection20);
        }
        else if (maxPlayers <= 25)
        {
            assignFrom(config.RaidInflection25);
        }
        else if (maxPlayers <= 40)
        {
            assignFrom(config.RaidInflection40);
        }
        else
        {
            assignFrom(config.RaidInflection);
        }

        auto applyOverride = [&](InflectionOverride const& override)
        {
            if (override.Value)
            {
                float const currentRatio = playerCount > 0.0f ? inflectionValue / playerCount : 1.0f;
                float const ratio = sanitizeRatio(*override.Value, currentRatio);
                inflectionValue = playerCount * ratio;
            }

            if (override.CurveFloor)
                curveFloor = *override.CurveFloor;

            if (override.CurveCeiling)
                curveCeiling = *override.CurveCeiling;

            if (override.BossModifier)
                baseBossMultiplier = *override.BossModifier;
        };

        if (isRaid)
        {
            auto const& overrides = isHeroic ? config.RaidHeroicInflectionOverrides : config.RaidInflectionOverrides;
            if (auto const it = overrides.find(maxPlayers); it != overrides.end())
                applyOverride(it->second);
        }

        if (auto const it = config.InflectionOverridesByInstance.find(map->GetId()); it != config.InflectionOverridesByInstance.end())
            applyOverride(it->second);

        float bossMultiplier = 1.0f;
        if (isBoss)
        {
            bossMultiplier = baseBossMultiplier;
            if (auto const it = config.InflectionBossOverridesByInstance.find(map->GetId()); it != config.InflectionBossOverridesByInstance.end())
                bossMultiplier = it->second;

            if (!std::isfinite(bossMultiplier) || bossMultiplier <= 0.0f)
                bossMultiplier = 1.0f;

            inflectionValue *= bossMultiplier;
        }

        if (!std::isfinite(inflectionValue) || inflectionValue <= 0.0f)
            inflectionValue = playerCount * DefaultInflectionRatio;

        if (!std::isfinite(curveFloor))
            curveFloor = 0.0f;

        if (!std::isfinite(curveCeiling) || curveCeiling <= 0.0f)
            curveCeiling = 1.0f;

        InflectionPointSettings settings;
        settings.Value = inflectionValue;
        settings.CurveFloor = curveFloor;
        settings.CurveCeiling = curveCeiling;
        settings.BossModifier = bossMultiplier;
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

ActiveInflectionInfo GetInflectionInfoForMap(Map const* map, bool forBoss)
{
    ActiveInflectionInfo info;
    info.TargetPlayers = GetTargetPlayerCount(map);
    info.Settings = SelectInflectionSettings(GetConfig(), map, info.TargetPlayers, forBoss);
    return info;
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
    uint8 highestPlayerLevel = GetHighestPlayerLevel(map);
    if (!highestPlayerLevel)
        highestPlayerLevel = creature->GetLevel();

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

    AutoBalanceCreatureInfo& info = GetCreatureInfo(*creature);
    info.IsBoss = isBoss;
    info.InstancePlayerCount = effectivePlayers;

    uint8 const level = creature->GetLevel();
    info.SelectedLevel = level;
    uint8 const unmodifiedLevel = info.Initialized ? info.UnmodifiedLevel : level;

    CreatureBaseStats const* originalBaseStats = sObjectMgr->GetCreatureBaseStats(unmodifiedLevel, creatureTemplate->unit_class);
    if (!originalBaseStats)
        return;

    CreatureBaseValues originalBaseValues;
    originalBaseValues.Health = originalBaseStats->GenerateHealth(creatureTemplate);
    originalBaseValues.Mana = originalBaseStats->GenerateMana(creatureTemplate);
    originalBaseValues.Armor = originalBaseStats->GenerateArmor(creatureTemplate);
    float const originalBaseDamage = originalBaseStats->GenerateBaseDamage(creatureTemplate);
    originalBaseValues.MinDamage = originalBaseDamage;
    originalBaseValues.MaxDamage = originalBaseDamage * 1.5f;
    originalBaseValues.AttackPower = static_cast<float>(originalBaseStats->AttackPower);
    originalBaseValues.RangedAttackPower = static_cast<float>(originalBaseStats->RangedAttackPower);

    CreatureBaseValues levelBaseValues = originalBaseValues;
    float levelHealthMultiplier = 1.0f;
    float levelManaMultiplier = 1.0f;
    float levelArmorMultiplier = 1.0f;
    float levelDamageMultiplier = 1.0f;
    float levelAttackPowerMultiplier = 1.0f;
    float levelRangedAttackPowerMultiplier = 1.0f;

    bool const levelScalingActive = config.LevelScalingEnabled && unmodifiedLevel != level;
    if (levelScalingActive)
    {
        CreatureBaseStats const* levelBaseStats = sObjectMgr->GetCreatureBaseStats(level, creatureTemplate->unit_class);
        if (!levelBaseStats)
            return;

        float const smoothedBaseHealth = GetBaseExpansionValueForLevel(levelBaseStats->BaseHealth, highestPlayerLevel);
        levelBaseValues.Health = static_cast<uint32>(std::round(smoothedBaseHealth * creatureTemplate->ModHealth));
        levelBaseValues.Mana = levelBaseStats->GenerateMana(creatureTemplate);
        levelBaseValues.Armor = levelBaseStats->GenerateArmor(creatureTemplate);
        float const levelBaseDamage = GetBaseExpansionValueForLevel(levelBaseStats->BaseDamage, highestPlayerLevel);
        levelBaseValues.MinDamage = levelBaseDamage;
        levelBaseValues.MaxDamage = levelBaseDamage * 1.5f;
        levelBaseValues.AttackPower = static_cast<float>(levelBaseStats->AttackPower);
        levelBaseValues.RangedAttackPower = static_cast<float>(levelBaseStats->RangedAttackPower);

        auto computeLevelMultiplier = [](float originalValue, float newValue)
        {
            if (originalValue <= 0.0f)
                return 1.0f;

            float multiplier = newValue / originalValue;
            if (!std::isfinite(multiplier) || multiplier <= 0.0f)
                return 1.0f;

            return multiplier;
        };

        levelHealthMultiplier = computeLevelMultiplier(static_cast<float>(originalBaseValues.Health), static_cast<float>(levelBaseValues.Health));
        levelManaMultiplier = computeLevelMultiplier(static_cast<float>(originalBaseValues.Mana), static_cast<float>(levelBaseValues.Mana));
        levelArmorMultiplier = computeLevelMultiplier(static_cast<float>(originalBaseValues.Armor), static_cast<float>(levelBaseValues.Armor));
        levelDamageMultiplier = computeLevelMultiplier(originalBaseValues.MinDamage, levelBaseValues.MinDamage);
        levelAttackPowerMultiplier = computeLevelMultiplier(originalBaseValues.AttackPower, levelBaseValues.AttackPower);
        levelRangedAttackPowerMultiplier = computeLevelMultiplier(originalBaseValues.RangedAttackPower, levelBaseValues.RangedAttackPower);
    }

    CreatureMultipliers const baseMultipliers = { healthMultiplier, manaMultiplier, damageMultiplier, armorMultiplier, crowdControlMultiplier };
    CreatureMultipliers finalMultipliers = baseMultipliers;
    if (levelScalingActive)
    {
        finalMultipliers.Health *= levelHealthMultiplier;
        finalMultipliers.Mana *= levelManaMultiplier;
        finalMultipliers.Damage *= levelDamageMultiplier;
        finalMultipliers.Armor *= levelArmorMultiplier;
    }

    auto multipliersDiffer = [](float lhs, float rhs)
    {
        return std::fabs(lhs - rhs) > 0.0005f;
    };

    auto baseValuesDiffer = [](CreatureBaseValues const& lhs, CreatureBaseValues const& rhs)
    {
        auto floatsDiffer = [](float l, float r)
        {
            return std::fabs(l - r) > 0.0005f;
        };

        return lhs.Health != rhs.Health || lhs.Mana != rhs.Mana || lhs.Armor != rhs.Armor ||
            floatsDiffer(lhs.MinDamage, rhs.MinDamage) || floatsDiffer(lhs.MaxDamage, rhs.MaxDamage) ||
            floatsDiffer(lhs.AttackPower, rhs.AttackPower) || floatsDiffer(lhs.RangedAttackPower, rhs.RangedAttackPower);
    };

    bool const requiresUpdate = !info.Initialized ||
        info.BaseLevel != level ||
        info.TargetPlayerCount != targetPlayers ||
        info.EffectivePlayerCount != effectivePlayers ||
        multipliersDiffer(info.BaseMultipliers.Health, baseMultipliers.Health) ||
        multipliersDiffer(info.BaseMultipliers.Mana, baseMultipliers.Mana) ||
        multipliersDiffer(info.BaseMultipliers.Damage, baseMultipliers.Damage) ||
        multipliersDiffer(info.BaseMultipliers.Armor, baseMultipliers.Armor) ||
        multipliersDiffer(info.BaseMultipliers.CrowdControlDuration, baseMultipliers.CrowdControlDuration) ||
        multipliersDiffer(info.Multipliers.Health, finalMultipliers.Health) ||
        multipliersDiffer(info.Multipliers.Mana, finalMultipliers.Mana) ||
        multipliersDiffer(info.Multipliers.Damage, finalMultipliers.Damage) ||
        multipliersDiffer(info.Multipliers.Armor, finalMultipliers.Armor) ||
        multipliersDiffer(info.Multipliers.CrowdControlDuration, finalMultipliers.CrowdControlDuration) ||
        baseValuesDiffer(info.LevelScaledBaseValues, levelBaseValues);

    if (!requiresUpdate)
        return;

    if (!info.Initialized)
        info.UnmodifiedLevel = unmodifiedLevel;

    info.BaseValues = originalBaseValues;
    info.LevelScaledBaseValues = levelBaseValues;
    info.BaseLevel = level;
    info.TargetPlayerCount = targetPlayers;
    info.EffectivePlayerCount = effectivePlayers;
    info.BaseMultipliers = baseMultipliers;
    info.Multipliers = finalMultipliers;
    info.XPModifier = 1.0f;
    info.MoneyModifier = 1.0f;
    info.Initialized = true;

    uint32 const oldMaxHealth = creature->GetMaxHealth();
    float const healthPct = oldMaxHealth ? std::clamp(static_cast<float>(creature->GetHealth()) / static_cast<float>(oldMaxHealth), 0.0f, 1.0f) : 1.0f;
    uint32 const newHealth = std::max<uint32>(1u, static_cast<uint32>(std::round(info.BaseValues.Health * info.Multipliers.Health)));
    creature->SetCreateHealth(newHealth);
    creature->SetMaxHealth(newHealth);
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, static_cast<float>(newHealth));
    uint32 const scaledHealth = std::clamp<uint32>(static_cast<uint32>(std::round(newHealth * healthPct)), 1u, newHealth);
    creature->SetHealth(scaledHealth);

    // Keep the player-damage requirement aligned with the scaled health pool.
    if (creature->m_PlayerDamageReq)
    {
        uint32 newDamageReq = newHealth / 2;

        if (oldMaxHealth)
            newDamageReq = static_cast<uint32>(std::round(static_cast<float>(creature->m_PlayerDamageReq) * newHealth / oldMaxHealth));

        creature->m_PlayerDamageReq = std::min(newDamageReq, newHealth / 2);
    }

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

    float const attackPowerMultiplier = info.BaseMultipliers.Damage * levelAttackPowerMultiplier;
    float const rangedAttackPowerMultiplier = info.BaseMultipliers.Damage * levelRangedAttackPowerMultiplier;
    creature->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER, BASE_VALUE, info.BaseValues.AttackPower * attackPowerMultiplier);
    creature->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, BASE_VALUE, info.BaseValues.RangedAttackPower * rangedAttackPowerMultiplier);

    if (TempSummon* summon = creature->ToTempSummon())
    {
        info.IsSummon = true;
        if (Unit* summoner = summon->GetSummonerUnit())
        {
            if (Creature* summonerCreature = summoner->ToCreature())
            {
                info.SummonerName = summonerCreature->GetName();
                info.SummonerLevel = summonerCreature->GetLevel();
            }
            else if (Player* summonerPlayer = summoner->ToPlayer())
            {
                info.SummonerName = summonerPlayer->GetName();
                info.SummonerLevel = summonerPlayer->GetLevel();
            }
        }
    }
    else
    {
        info.IsSummon = false;
        info.SummonerName.clear();
        info.SummonerLevel = 0;
        info.IsSummonClone = false;
    }

    auto computeRewardModifier = [&](bool enabled, float modifier)
    {
        if (!enabled)
            return 1.0f;

        if (config.RewardScalingMethod == ScalingMethod::Fixed)
            return modifier;

        float avg = (finalMultipliers.Health + finalMultipliers.Damage) / 2.0f;
        return std::max(0.0f, avg * modifier);
    };

    info.XPModifier = computeRewardModifier(config.RewardScalingXP, config.RewardScalingXPModifier);
    info.MoneyModifier = computeRewardModifier(config.RewardScalingMoney, config.RewardScalingMoneyModifier);

    creature->UpdateDamagePhysical(BASE_ATTACK);
    creature->UpdateDamagePhysical(OFF_ATTACK);
    creature->UpdateDamagePhysical(RANGED_ATTACK);

    if (config.DebugLogging)
        TC_LOG_DEBUG(LogFilter,
            "AutoBalance::ScaleCreature - entry={} level={} players={}/{} base={:.3f} baseMult(h/m/d/a)={:.3f}/{:.3f}/{:.3f}/{:.3f} "
            "levelAdj(h/m/d/a)={:.3f}/{:.3f}/{:.3f}/{:.3f} final(h/m/d/a)={:.3f}/{:.3f}/{:.3f}/{:.3f} cc={:.3f}",
            creature->GetEntry(), level, effectivePlayers, targetPlayers, baseMultiplier,
            info.BaseMultipliers.Health, info.BaseMultipliers.Mana, info.BaseMultipliers.Damage, info.BaseMultipliers.Armor,
            levelHealthMultiplier, levelManaMultiplier, levelDamageMultiplier, levelArmorMultiplier,
            info.Multipliers.Health, info.Multipliers.Mana, info.Multipliers.Damage, info.Multipliers.Armor,
            info.Multipliers.CrowdControlDuration);
}
}
