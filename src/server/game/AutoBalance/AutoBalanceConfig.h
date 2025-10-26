#pragma once

#include "Define.h"
#include "Optional.h"
#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace AutoBalance
{
    inline constexpr char const* LogFilter = "module.AutoBalance";

    enum class InstanceDifficultyToggle : uint8
    {
        Normal5,
        Normal10,
        Normal15,
        Normal20,
        Normal25,
        Normal40,
        NormalOther,
        Heroic5,
        Heroic10,
        Heroic25,
        HeroicOther,
        Count
    };

    struct InflectionPointSettings
    {
        float Value = 0.5f;
        float CurveFloor = 0.0f;
        float CurveCeiling = 1.0f;
        float BossModifier = 1.0f;
    };

    struct InflectionOverride
    {
        Optional<float> Value;
        Optional<float> CurveFloor;
        Optional<float> CurveCeiling;
        Optional<float> BossModifier;

        bool HasAny() const
        {
            return Value.has_value() || CurveFloor.has_value() || CurveCeiling.has_value() || BossModifier.has_value();
        }
    };

    struct StatModifierValues
    {
        float Global = 1.0f;
        float Health = 1.0f;
        float Mana = 1.0f;
        float Armor = 1.0f;
        float Damage = 1.0f;
        float CrowdControlDuration = 1.0f;
    };

    struct StatModifierOverride
    {
        Optional<float> Global;
        Optional<float> Health;
        Optional<float> Mana;
        Optional<float> Armor;
        Optional<float> Damage;
        Optional<float> CrowdControlDuration;

        bool HasAny() const
        {
            return Global.has_value() || Health.has_value() || Mana.has_value() || Armor.has_value() || Damage.has_value() || CrowdControlDuration.has_value();
        }
    };

    struct ModuleConfig
    {
        bool Enabled = true;
        bool EnableDungeons = true;
        bool EnableRaids = false;
        bool EnableWorldMaps = false;
        bool DebugLogging = false;
        uint32 MinimumPlayers = 1;
        uint32 MinimumPlayersHeroic = 1;
        std::array<bool, static_cast<size_t>(InstanceDifficultyToggle::Count)> InstanceToggles{};
        std::vector<uint32> DisabledInstances;
        std::unordered_map<uint32, uint32> MinPlayersOverridesNormal;
        std::unordered_map<uint32, uint32> MinPlayersOverridesHeroic;
        int32 PlayerCountDifficultyOffset = 0;
        float MinCCDurationModifier = 0.25f;
        float MaxCCDurationModifier = 1.0f;
        float MinHPModifier = 0.01f;
        float MinManaModifier = 0.01f;
        float MinDamageModifier = 0.01f;

        InflectionPointSettings DungeonInflection;
        InflectionPointSettings DungeonHeroicInflection;
        InflectionPointSettings RaidInflection;
        InflectionPointSettings RaidHeroicInflection;
        std::unordered_map<uint32, InflectionOverride> RaidInflectionOverrides;
        std::unordered_map<uint32, InflectionOverride> RaidHeroicInflectionOverrides;
        std::unordered_map<uint32, InflectionOverride> InflectionOverridesByInstance;
        std::unordered_map<uint32, float> InflectionBossOverridesByInstance;

        StatModifierValues DungeonStatModifiers;
        StatModifierValues DungeonBossStatModifiers;
        StatModifierValues DungeonHeroicStatModifiers;
        StatModifierValues DungeonHeroicBossStatModifiers;
        StatModifierValues RaidStatModifiers;
        StatModifierValues RaidBossStatModifiers;
        StatModifierValues RaidHeroicStatModifiers;
        StatModifierValues RaidHeroicBossStatModifiers;
        std::unordered_map<uint32, StatModifierOverride> RaidStatOverridesBySize;
        std::unordered_map<uint32, StatModifierOverride> RaidHeroicStatOverridesBySize;
        std::unordered_map<uint32, StatModifierOverride> RaidBossStatOverridesBySize;
        std::unordered_map<uint32, StatModifierOverride> RaidHeroicBossStatOverridesBySize;
        std::unordered_map<uint32, StatModifierOverride> StatModifierOverridesByInstance;
        std::unordered_map<uint32, StatModifierOverride> StatModifierBossOverridesByInstance;
        std::unordered_map<uint32, StatModifierOverride> StatModifierOverridesByCreature;
        std::unordered_map<uint32, StatModifierOverride> StatModifierBossOverridesByCreature;
    };

    ModuleConfig const& GetConfig();
    bool IsEnabled();
    void LoadConfig(bool reload);
    int32 GetPlayerCountDifficultyOffset();
    void SetPlayerCountDifficultyOffset(int32 offset);
}
