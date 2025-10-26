#include "AutoBalance/AutoBalanceConfig.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "StringFormat.h"
#include "Utilities/StringConvert.h"
#include "Utilities/Util.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace AutoBalance
{
    namespace
    {
        ModuleConfig s_Config;
        bool s_ConfigInitialized = false;
        bool s_LoggedStartup = false;

        constexpr size_t ToggleIndex(InstanceDifficultyToggle toggle)
        {
            return static_cast<size_t>(toggle);
        }

        enum class MessageLevel
        {
            Info,
            Warn,
            Error
        };

        template<typename... Args>
        void LogMessage(MessageLevel level, bool logEnabled, Trinity::FormatString<Args...> format, Args&&... args)
        {
            std::string const message = Trinity::StringFormat(format, std::forward<Args>(args)...);

            if (logEnabled)
            {
                switch (level)
                {
                    case MessageLevel::Info:
                        TC_LOG_INFO(LogFilter, "{}", message);
                        break;
                    case MessageLevel::Warn:
                        TC_LOG_WARN(LogFilter, "{}", message);
                        break;
                    case MessageLevel::Error:
                        TC_LOG_ERROR(LogFilter, "{}", message);
                        break;
                }
            }
            else
                std::printf("AutoBalance: %s\n", message.c_str());
        }

        void MergeConfigFile(std::string const& file, bool logEnabled)
        {
            if (file.empty())
                return;

            std::string error;
            if (!sConfigMgr->LoadAdditionalFile(file, false, error))
                LogMessage(MessageLevel::Error, logEnabled, "Failed to load configuration file '{}' ({})", file, error);
        }

        std::string_view Trim(std::string_view view)
        {
            while (!view.empty() && std::isspace(static_cast<unsigned char>(view.front())))
                view.remove_prefix(1);

            while (!view.empty() && std::isspace(static_cast<unsigned char>(view.back())))
                view.remove_suffix(1);

            return view;
        }

        std::vector<uint32> ParseDisabledInstances(std::string const& value, bool logEnabled)
        {
            std::vector<uint32> instances;
            for (std::string_view token : Trinity::Tokenize(value, ',', false))
            {
                token = Trim(token);
                if (token.empty())
                    continue;

                if (Optional<uint32> instanceId = Trinity::StringTo<uint32>(token))
                    instances.push_back(*instanceId);
                else
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring invalid instance id '{}' in AutoBalance.Disable.PerInstance.", token);
            }

            return instances;
        }

        std::unordered_map<uint32, uint32> ParseMinPlayersOverrides(std::string const& value, bool logEnabled, char const* settingName)
        {
            std::unordered_map<uint32, uint32> overrides;
            for (std::string_view entry : Trinity::Tokenize(value, ',', false))
            {
                entry = Trim(entry);
                if (entry.empty())
                    continue;

                std::istringstream stream{std::string(entry)};
                uint32 instanceId = 0;
                int32 minPlayers = 0;

                if (!(stream >> instanceId))
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: missing instance id.", entry, settingName);
                    continue;
                }

                if (!(stream >> minPlayers))
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: missing minimum player value.", entry, settingName);
                    continue;
                }

                if (minPlayers <= 0)
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: minimum player value must be > 0.", entry, settingName);
                    continue;
                }

                overrides[instanceId] = static_cast<uint32>(minPlayers);
            }

            return overrides;
        }

        Optional<float> ParseOptionalFloat(std::string const& value, char const* option, bool logEnabled)
        {
            if (value.empty())
                return { };

            if (Optional<float> parsed = Trinity::StringTo<float>(value))
                return parsed;

            LogMessage(MessageLevel::Warn, logEnabled, "Ignoring invalid value '{}' for {}.", value, option);
            return { };
        }

        InflectionPointSettings ParseInflectionPointSettings(char const* valueOption, char const* floorOption, char const* ceilingOption, char const* bossOption, InflectionPointSettings defaults)
        {
            InflectionPointSettings settings = defaults;
            settings.Value = sConfigMgr->GetFloatDefault(valueOption, defaults.Value);
            settings.CurveFloor = sConfigMgr->GetFloatDefault(floorOption, defaults.CurveFloor);
            settings.CurveCeiling = sConfigMgr->GetFloatDefault(ceilingOption, defaults.CurveCeiling);
            settings.BossModifier = sConfigMgr->GetFloatDefault(bossOption, defaults.BossModifier);
            return settings;
        }

        Optional<InflectionOverride> ParseInflectionOverride(char const* valueOption, char const* floorOption, char const* ceilingOption, char const* bossOption, bool logEnabled)
        {
            InflectionOverride override;
            bool hasAny = false;

            auto parse = [&](char const* option, Optional<float>& field)
            {
                if (!option)
                    return;

                std::string const raw = sConfigMgr->GetStringDefault(option, "");
                if (Optional<float> parsed = ParseOptionalFloat(raw, option, logEnabled))
                {
                    field = *parsed;
                    hasAny = true;
                }
            };

            parse(valueOption, override.Value);
            parse(floorOption, override.CurveFloor);
            parse(ceilingOption, override.CurveCeiling);
            parse(bossOption, override.BossModifier);

            if (!hasAny)
                return { };

            return override;
        }

        std::unordered_map<uint32, InflectionOverride> ParseInflectionOverrides(std::string const& value, bool logEnabled, char const* settingName)
        {
            std::unordered_map<uint32, InflectionOverride> overrides;
            for (std::string_view entry : Trinity::Tokenize(value, ',', false))
            {
                entry = Trim(entry);
                if (entry.empty())
                    continue;

                std::istringstream stream{std::string(entry)};
                uint32 instanceId = 0;
                if (!(stream >> instanceId))
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: missing instance id.", entry, settingName);
                    continue;
                }

                float valueField = 0.0f;
                bool hasAny = false;
                InflectionOverride override;

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.Value = valueField;
                        hasAny = true;
                    }
                }

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.CurveFloor = valueField;
                        hasAny = true;
                    }
                }

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.CurveCeiling = valueField;
                        hasAny = true;
                    }
                }

                if (!hasAny)
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: no override values provided.", entry, settingName);
                    continue;
                }

                overrides[instanceId] = override;
            }

            return overrides;
        }

        std::unordered_map<uint32, float> ParseInflectionBossOverrides(std::string const& value, bool logEnabled, char const* settingName)
        {
            std::unordered_map<uint32, float> overrides;
            for (std::string_view entry : Trinity::Tokenize(value, ',', false))
            {
                entry = Trim(entry);
                if (entry.empty())
                    continue;

                std::istringstream stream{std::string(entry)};
                uint32 instanceId = 0;
                if (!(stream >> instanceId))
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: missing instance id.", entry, settingName);
                    continue;
                }

                float multiplier = 0.0f;
                if (!(stream >> multiplier))
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: missing boss multiplier value.", entry, settingName);
                    continue;
                }

                if (multiplier <= 0.0f)
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: boss multiplier must be > 0.", entry, settingName);
                    continue;
                }

                overrides[instanceId] = multiplier;
            }

            return overrides;
        }

        StatModifierValues ParseStatModifierValues(char const* prefix, StatModifierValues defaults)
        {
            StatModifierValues values = defaults;

            auto read = [&](char const* suffix, float fallback) -> float
            {
                std::string option = std::string(prefix) + suffix;
                return sConfigMgr->GetFloatDefault(option.c_str(), fallback);
            };

            values.Global = read("Global", defaults.Global);
            values.Health = read("Health", defaults.Health);
            values.Mana = read("Mana", defaults.Mana);
            values.Armor = read("Armor", defaults.Armor);
            values.Damage = read("Damage", defaults.Damage);
            values.CrowdControlDuration = read("CCDuration", defaults.CrowdControlDuration);

            return values;
        }

        Optional<StatModifierOverride> ParseStatModifierOverride(char const* prefix, bool logEnabled)
        {
            StatModifierOverride override;
            bool hasAny = false;

            auto parse = [&](char const* suffix, Optional<float>& field)
            {
                std::string option = std::string(prefix) + suffix;
                std::string const raw = sConfigMgr->GetStringDefault(option.c_str(), "");
                if (Optional<float> parsed = ParseOptionalFloat(raw, option.c_str(), logEnabled))
                {
                    field = *parsed;
                    hasAny = true;
                }
            };

            parse("Global", override.Global);
            parse("Health", override.Health);
            parse("Mana", override.Mana);
            parse("Armor", override.Armor);
            parse("Damage", override.Damage);
            parse("CCDuration", override.CrowdControlDuration);

            if (!hasAny)
                return { };

            return override;
        }

        std::unordered_map<uint32, StatModifierOverride> ParseStatModifierOverrides(std::string const& value, bool logEnabled, char const* settingName)
        {
            std::unordered_map<uint32, StatModifierOverride> overrides;
            for (std::string_view entry : Trinity::Tokenize(value, ',', false))
            {
                entry = Trim(entry);
                if (entry.empty())
                    continue;

                std::istringstream stream{std::string(entry)};
                uint32 id = 0;
                if (!(stream >> id))
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: missing identifier.", entry, settingName);
                    continue;
                }

                float valueField = 0.0f;
                bool hasAny = false;
                StatModifierOverride override;

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.Global = valueField;
                        hasAny = true;
                    }
                }

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.Health = valueField;
                        hasAny = true;
                    }
                }

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.Mana = valueField;
                        hasAny = true;
                    }
                }

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.Armor = valueField;
                        hasAny = true;
                    }
                }

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.Damage = valueField;
                        hasAny = true;
                    }
                }

                if (stream >> valueField)
                {
                    if (valueField != -1.0f)
                    {
                        override.CrowdControlDuration = valueField;
                        hasAny = true;
                    }
                }

                if (!hasAny)
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in {}: no override values provided.", entry, settingName);
                    continue;
                }

                overrides[id] = override;
            }

            return overrides;
        }

        void Validate(ModuleConfig& config, bool logEnabled)
        {
            if (!config.MinimumPlayers)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "AutoBalance.MinPlayers is set to 0. Using 1 instead.");
                config.MinimumPlayers = 1;
            }

            if (!config.MinimumPlayersHeroic)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "AutoBalance.MinPlayers.Heroic is set to 0. Using 1 instead.");
                config.MinimumPlayersHeroic = 1;
            }

            if (config.MinCCDurationModifier < 0.0f)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "AutoBalance.MinCCDurationModifier ({:.3f}) must be >= 0. Using 0 instead.", config.MinCCDurationModifier);
                config.MinCCDurationModifier = 0.0f;
            }

            if (config.MaxCCDurationModifier < config.MinCCDurationModifier)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "AutoBalance.MaxCCDurationModifier ({:.3f}) must be >= AutoBalance.MinCCDurationModifier ({:.3f}). Using the minimum value instead.", config.MaxCCDurationModifier, config.MinCCDurationModifier);
                config.MaxCCDurationModifier = config.MinCCDurationModifier;
            }

            if (config.MinHPModifier < 0.0f)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "AutoBalance.MinHPModifier ({:.3f}) must be >= 0. Using 0 instead.", config.MinHPModifier);
                config.MinHPModifier = 0.0f;
            }

            if (config.MinManaModifier < 0.0f)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "AutoBalance.MinManaModifier ({:.3f}) must be >= 0. Using 0 instead.", config.MinManaModifier);
                config.MinManaModifier = 0.0f;
            }

            if (config.MinDamageModifier < 0.0f)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "AutoBalance.MinDamageModifier ({:.3f}) must be >= 0. Using 0 instead.", config.MinDamageModifier);
                config.MinDamageModifier = 0.0f;
            }
        }
    }

    ModuleConfig const& GetConfig()
    {
        return s_Config;
    }

    bool IsEnabled()
    {
        return s_Config.Enabled;
    }

    int32 GetPlayerCountDifficultyOffset()
    {
        return s_Config.PlayerCountDifficultyOffset;
    }

    void SetPlayerCountDifficultyOffset(int32 offset)
    {
        s_Config.PlayerCountDifficultyOffset = offset;
    }

    void LoadConfig(bool reload)
    {
        bool wasInitialized = s_ConfigInitialized;

        std::string configFile = sConfigMgr->GetStringDefault("AutoBalance.Conf", "conf/AutoBalance.conf");
        bool logReady = reload || wasInitialized;

        if (!configFile.empty())
            MergeConfigFile(configFile, logReady);

        ModuleConfig newConfig;

        bool enabledFallback = sConfigMgr->GetBoolDefault("AutoBalance.Enabled", true);
        newConfig.Enabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.Global", enabledFallback);

        auto setToggle = [&](InstanceDifficultyToggle toggle, char const* option, bool defaultValue)
        {
            newConfig.InstanceToggles[ToggleIndex(toggle)] = sConfigMgr->GetBoolDefault(option, defaultValue);
        };

        setToggle(InstanceDifficultyToggle::Normal5, "AutoBalance.Enable.5M", true);
        setToggle(InstanceDifficultyToggle::Normal10, "AutoBalance.Enable.10M", true);
        setToggle(InstanceDifficultyToggle::Normal15, "AutoBalance.Enable.15M", true);
        setToggle(InstanceDifficultyToggle::Normal20, "AutoBalance.Enable.20M", true);
        setToggle(InstanceDifficultyToggle::Normal25, "AutoBalance.Enable.25M", true);
        setToggle(InstanceDifficultyToggle::Normal40, "AutoBalance.Enable.40M", true);
        setToggle(InstanceDifficultyToggle::NormalOther, "AutoBalance.Enable.OtherNormal", true);
        setToggle(InstanceDifficultyToggle::Heroic5, "AutoBalance.Enable.5MHeroic", true);
        setToggle(InstanceDifficultyToggle::Heroic10, "AutoBalance.Enable.10MHeroic", true);
        setToggle(InstanceDifficultyToggle::Heroic25, "AutoBalance.Enable.25MHeroic", true);
        setToggle(InstanceDifficultyToggle::HeroicOther, "AutoBalance.Enable.OtherHeroic", true);

        auto anyEnabled = [&](std::initializer_list<InstanceDifficultyToggle> toggles)
        {
            return std::any_of(toggles.begin(), toggles.end(), [&](InstanceDifficultyToggle toggle)
            {
                return newConfig.InstanceToggles[ToggleIndex(toggle)];
            });
        };

        bool defaultDungeonToggle = anyEnabled({
            InstanceDifficultyToggle::Normal5,
            InstanceDifficultyToggle::NormalOther,
            InstanceDifficultyToggle::Heroic5,
            InstanceDifficultyToggle::HeroicOther
        });

        bool defaultRaidToggle = anyEnabled({
            InstanceDifficultyToggle::Normal10,
            InstanceDifficultyToggle::Normal15,
            InstanceDifficultyToggle::Normal20,
            InstanceDifficultyToggle::Normal25,
            InstanceDifficultyToggle::Normal40,
            InstanceDifficultyToggle::Heroic10,
            InstanceDifficultyToggle::Heroic25,
            InstanceDifficultyToggle::NormalOther,
            InstanceDifficultyToggle::HeroicOther
        });

        newConfig.EnableDungeons = sConfigMgr->GetBoolDefault("AutoBalance.EnableDungeons", defaultDungeonToggle);
        newConfig.EnableRaids = sConfigMgr->GetBoolDefault("AutoBalance.EnableRaids", defaultRaidToggle);
        newConfig.EnableWorldMaps = sConfigMgr->GetBoolDefault("AutoBalance.EnableWorldMaps", false);
        newConfig.DebugLogging = sConfigMgr->GetBoolDefault("AutoBalance.DebugLogging", false);

        int32 minPlayers = sConfigMgr->GetIntDefault("AutoBalance.MinPlayers", 1);
        if (minPlayers < 0)
        {
            LogMessage(MessageLevel::Warn, logReady, "AutoBalance.MinPlayers ({}) must be >= 0. Using 0 instead.", minPlayers);
            minPlayers = 0;
        }

        int32 minPlayersHeroic = sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.Heroic", minPlayers);
        if (minPlayersHeroic < 0)
        {
            LogMessage(MessageLevel::Warn, logReady, "AutoBalance.MinPlayers.Heroic ({}) must be >= 0. Using 0 instead.", minPlayersHeroic);
            minPlayersHeroic = 0;
        }

        newConfig.MinimumPlayers = static_cast<uint32>(minPlayers);
        newConfig.MinimumPlayersHeroic = static_cast<uint32>(minPlayersHeroic);

        newConfig.DisabledInstances = ParseDisabledInstances(sConfigMgr->GetStringDefault("AutoBalance.Disable.PerInstance", ""), logReady);
        newConfig.MinPlayersOverridesNormal = ParseMinPlayersOverrides(sConfigMgr->GetStringDefault("AutoBalance.MinPlayers.PerInstance", ""), logReady, "AutoBalance.MinPlayers.PerInstance");
        newConfig.MinPlayersOverridesHeroic = ParseMinPlayersOverrides(sConfigMgr->GetStringDefault("AutoBalance.MinPlayers.Heroic.PerInstance", ""), logReady, "AutoBalance.MinPlayers.Heroic.PerInstance");
        newConfig.PlayerCountDifficultyOffset = sConfigMgr->GetIntDefault("AutoBalance.playerCountDifficultyOffset", 0);
        newConfig.MinCCDurationModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinCCDurationModifier", 0.25f));
        newConfig.MaxCCDurationModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MaxCCDurationModifier", 1.0f));
        newConfig.MinHPModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinHPModifier", 0.01f));
        newConfig.MinManaModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinManaModifier", 0.01f));
        newConfig.MinDamageModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinDamageModifier", 0.01f));

        auto const defaultInflection = InflectionPointSettings{};
        newConfig.DungeonInflection = ParseInflectionPointSettings("AutoBalance.InflectionPoint", "AutoBalance.InflectionPoint.CurveFloor", "AutoBalance.InflectionPoint.CurveCeiling", "AutoBalance.InflectionPoint.BossModifier", defaultInflection);
        newConfig.DungeonHeroicInflection = ParseInflectionPointSettings("AutoBalance.InflectionPointHeroic", "AutoBalance.InflectionPointHeroic.CurveFloor", "AutoBalance.InflectionPointHeroic.CurveCeiling", "AutoBalance.InflectionPointHeroic.BossModifier", defaultInflection);
        newConfig.RaidInflection = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid", "AutoBalance.InflectionPointRaid.CurveFloor", "AutoBalance.InflectionPointRaid.CurveCeiling", "AutoBalance.InflectionPointRaid.BossModifier", defaultInflection);
        newConfig.RaidHeroicInflection = ParseInflectionPointSettings("AutoBalance.InflectionPointRaidHeroic", "AutoBalance.InflectionPointRaidHeroic.CurveFloor", "AutoBalance.InflectionPointRaidHeroic.CurveCeiling", "AutoBalance.InflectionPointRaidHeroic.BossModifier", defaultInflection);

        auto applyRaidInflectionOverride = [&](uint32 size, char const* normalPrefix, char const* heroicPrefix)
        {
            std::string normalFloor = std::string(normalPrefix) + ".CurveFloor";
            std::string normalCeiling = std::string(normalPrefix) + ".CurveCeiling";
            std::string normalBoss = std::string(normalPrefix) + ".BossModifier";

            if (Optional<InflectionOverride> normal = ParseInflectionOverride(normalPrefix, normalFloor.c_str(), normalCeiling.c_str(), normalBoss.c_str(), logReady))
                newConfig.RaidInflectionOverrides[size] = *normal;

            if (heroicPrefix)
            {
                std::string heroicFloor = std::string(heroicPrefix) + ".CurveFloor";
                std::string heroicCeiling = std::string(heroicPrefix) + ".CurveCeiling";
                std::string heroicBoss = std::string(heroicPrefix) + ".BossModifier";

                if (Optional<InflectionOverride> heroic = ParseInflectionOverride(heroicPrefix, heroicFloor.c_str(), heroicCeiling.c_str(), heroicBoss.c_str(), logReady))
                    newConfig.RaidHeroicInflectionOverrides[size] = *heroic;
            }
        };

        applyRaidInflectionOverride(10, "AutoBalance.InflectionPointRaid10M", "AutoBalance.InflectionPointRaid10MHeroic");
        applyRaidInflectionOverride(15, "AutoBalance.InflectionPointRaid15M", "AutoBalance.InflectionPointRaid15MHeroic");
        applyRaidInflectionOverride(20, "AutoBalance.InflectionPointRaid20M", "AutoBalance.InflectionPointRaid20MHeroic");
        applyRaidInflectionOverride(25, "AutoBalance.InflectionPointRaid25M", "AutoBalance.InflectionPointRaid25MHeroic");
        applyRaidInflectionOverride(40, "AutoBalance.InflectionPointRaid40M", nullptr);

        newConfig.InflectionOverridesByInstance = ParseInflectionOverrides(sConfigMgr->GetStringDefault("AutoBalance.InflectionPoint.PerInstance", ""), logReady, "AutoBalance.InflectionPoint.PerInstance");
        newConfig.InflectionBossOverridesByInstance = ParseInflectionBossOverrides(sConfigMgr->GetStringDefault("AutoBalance.InflectionPoint.Boss.PerInstance", ""), logReady, "AutoBalance.InflectionPoint.Boss.PerInstance");

        auto const defaultStatModifiers = StatModifierValues{};
        newConfig.DungeonStatModifiers = ParseStatModifierValues("AutoBalance.StatModifier.", defaultStatModifiers);
        newConfig.DungeonBossStatModifiers = ParseStatModifierValues("AutoBalance.StatModifier.Boss.", defaultStatModifiers);
        newConfig.DungeonHeroicStatModifiers = ParseStatModifierValues("AutoBalance.StatModifierHeroic.", defaultStatModifiers);
        newConfig.DungeonHeroicBossStatModifiers = ParseStatModifierValues("AutoBalance.StatModifierHeroic.Boss.", defaultStatModifiers);
        newConfig.RaidStatModifiers = ParseStatModifierValues("AutoBalance.StatModifierRaid.", defaultStatModifiers);
        newConfig.RaidBossStatModifiers = ParseStatModifierValues("AutoBalance.StatModifierRaid.Boss.", defaultStatModifiers);
        newConfig.RaidHeroicStatModifiers = ParseStatModifierValues("AutoBalance.StatModifierRaidHeroic.", defaultStatModifiers);
        newConfig.RaidHeroicBossStatModifiers = ParseStatModifierValues("AutoBalance.StatModifierRaidHeroic.Boss.", defaultStatModifiers);

        auto applyRaidStatOverride = [&](uint32 size, char const* normalPrefix, char const* heroicPrefix)
        {
            if (Optional<StatModifierOverride> normal = ParseStatModifierOverride(normalPrefix, logReady))
                newConfig.RaidStatOverridesBySize[size] = *normal;

            std::string normalBossPrefix = std::string(normalPrefix) + "Boss.";
            if (Optional<StatModifierOverride> bossNormal = ParseStatModifierOverride(normalBossPrefix.c_str(), logReady))
                newConfig.RaidBossStatOverridesBySize[size] = *bossNormal;

            if (heroicPrefix)
            {
                if (Optional<StatModifierOverride> heroic = ParseStatModifierOverride(heroicPrefix, logReady))
                    newConfig.RaidHeroicStatOverridesBySize[size] = *heroic;

                std::string heroicBossPrefix = std::string(heroicPrefix) + "Boss.";
                if (Optional<StatModifierOverride> heroicBoss = ParseStatModifierOverride(heroicBossPrefix.c_str(), logReady))
                    newConfig.RaidHeroicBossStatOverridesBySize[size] = *heroicBoss;
            }
        };

        applyRaidStatOverride(10, "AutoBalance.StatModifierRaid10M.", "AutoBalance.StatModifierRaid10MHeroic.");
        applyRaidStatOverride(15, "AutoBalance.StatModifierRaid15M.", "AutoBalance.StatModifierRaid15MHeroic.");
        applyRaidStatOverride(20, "AutoBalance.StatModifierRaid20M.", "AutoBalance.StatModifierRaid20MHeroic.");
        applyRaidStatOverride(25, "AutoBalance.StatModifierRaid25M.", "AutoBalance.StatModifierRaid25MHeroic.");
        applyRaidStatOverride(40, "AutoBalance.StatModifierRaid40M.", nullptr);

        newConfig.StatModifierOverridesByInstance = ParseStatModifierOverrides(sConfigMgr->GetStringDefault("AutoBalance.StatModifier.PerInstance", ""), logReady, "AutoBalance.StatModifier.PerInstance");
        newConfig.StatModifierBossOverridesByInstance = ParseStatModifierOverrides(sConfigMgr->GetStringDefault("AutoBalance.StatModifier.Boss.PerInstance", ""), logReady, "AutoBalance.StatModifier.Boss.PerInstance");
        newConfig.StatModifierOverridesByCreature = ParseStatModifierOverrides(sConfigMgr->GetStringDefault("AutoBalance.StatModifier.PerCreature", ""), logReady, "AutoBalance.StatModifier.PerCreature");
        newConfig.StatModifierBossOverridesByCreature = ParseStatModifierOverrides(sConfigMgr->GetStringDefault("AutoBalance.StatModifier.Boss.PerCreature", ""), logReady, "AutoBalance.StatModifier.Boss.PerCreature");

        Validate(newConfig, logReady);

        s_Config = std::move(newConfig);
        s_ConfigInitialized = true;

        if (reload)
            LogMessage(MessageLevel::Info, true, "AutoBalance configuration reloaded from '{}'.", configFile);
        else if (logReady && !s_LoggedStartup)
        {
            LogMessage(MessageLevel::Info, true, "AutoBalance configuration loaded from '{}'.", configFile);
            s_LoggedStartup = true;
        }
    }
}
