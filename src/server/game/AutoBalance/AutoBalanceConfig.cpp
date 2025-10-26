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
#include <vector>
#include <utility>

namespace AutoBalance
{
    namespace
    {
        ModuleConfig s_Config;
        bool s_ConfigInitialized = false;
        bool s_LoggedStartup = false;
        ConfigLoadInfo s_ConfigLoadInfo;

        struct ConfigFileLoadResult
        {
            bool Loaded = false;
            bool UsedFallback = false;
            std::string ResolvedPath;
            std::vector<std::string> Attempts;
            std::string Error;
        };

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

        bool IsAbsolutePath(std::string const& path)
        {
            if (path.empty())
                return false;

            if (path[0] == '/' || path[0] == '\\')
                return true;

            if (path.size() > 1 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0])))
                return true;

            return false;
        }

        std::string ExtractDirectory(std::string const& path)
        {
            size_t slash = path.find_last_of("/\\");
            if (slash == std::string::npos)
                return { };

            return path.substr(0, slash + 1);
        }

        ConfigFileLoadResult MergeConfigFile(std::string const& file, bool logEnabled)
        {
            ConfigFileLoadResult result;

            if (file.empty())
                return result;

            auto registerAttempt = [&](std::string const& candidate)
            {
                if (candidate.empty())
                    return;

                if (std::find(result.Attempts.begin(), result.Attempts.end(), candidate) == result.Attempts.end())
                    result.Attempts.push_back(candidate);
            };

            std::string lastError;

            auto tryLoad = [&](std::string const& candidate, bool fallback) -> bool
            {
                if (candidate.empty())
                    return false;

                registerAttempt(candidate);

                std::string error;
                if (sConfigMgr->LoadAdditionalFile(candidate, false, error))
                {
                    result.Loaded = true;
                    result.UsedFallback = fallback;
                    result.ResolvedPath = candidate;
                    return true;
                }

                if (!error.empty())
                    lastError = error;

                return false;
            };

            std::vector<std::string> fallbacks;
            fallbacks.reserve(4);

            auto pushUnique = [&](std::string candidate)
            {
                if (candidate.empty() || candidate == file)
                    return;

                if (std::find(fallbacks.begin(), fallbacks.end(), candidate) == fallbacks.end())
                    fallbacks.emplace_back(std::move(candidate));
            };

            auto addJoinedCandidate = [&](std::string const& baseDir, std::string const& relative)
            {
                if (relative.empty())
                    return;

                if (baseDir.empty())
                {
                    pushUnique(relative);
                    return;
                }

                std::string candidate = baseDir;
                if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
                    candidate.push_back('/');

                candidate += relative;
                pushUnique(std::move(candidate));
            };

            std::string const primaryDirectory = ExtractDirectory(file);
            std::string const filename = primaryDirectory.empty() ? file : file.substr(primaryDirectory.size());
            std::string const lowerFilename = [&]() -> std::string
            {
                std::string lowered = filename;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });
                return lowered;
            }();

            std::string const configDirectory = ExtractDirectory(sConfigMgr->GetFilename());

            if (tryLoad(file, false))
                return result;

            if (!filename.empty())
            {
                pushUnique(primaryDirectory + lowerFilename);

                if (!IsAbsolutePath(file))
                {
                    addJoinedCandidate(configDirectory, file);
                    addJoinedCandidate(configDirectory, primaryDirectory + filename);
                    addJoinedCandidate(configDirectory, filename);

                    if (lowerFilename != filename)
                    {
                        addJoinedCandidate(configDirectory, lowerFilename);
                        addJoinedCandidate(configDirectory, primaryDirectory + lowerFilename);
                    }
                }

                std::string modsDirectory = primaryDirectory;
                if (!modsDirectory.empty() && modsDirectory.back() != '/' && modsDirectory.back() != '\\')
                    modsDirectory.push_back('/');

                modsDirectory += "mods/";
                pushUnique(modsDirectory + filename);
                if (lowerFilename != filename)
                    pushUnique(modsDirectory + lowerFilename);

                if (!configDirectory.empty() && !IsAbsolutePath(file))
                {
                    addJoinedCandidate(configDirectory, modsDirectory + filename);
                    if (lowerFilename != filename)
                        addJoinedCandidate(configDirectory, modsDirectory + lowerFilename);
                }
            }

            for (std::string const& candidate : fallbacks)
            {
                if (tryLoad(candidate, true))
                {
                    LogMessage(MessageLevel::Info, logEnabled, "Loaded configuration file '{}' via fallback '{}'.", file, candidate);
                    return result;
                }
            }

            std::string const errorMessage = lastError.empty() ? std::string("unknown error") : lastError;
            result.Error = errorMessage;
            LogMessage(MessageLevel::Error, logEnabled, "Failed to load configuration file '{}' ({}).", file, errorMessage);
            return result;
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

        float ParseInflectionPointField(char const* option, float fallback, bool logEnabled)
        {
            if (!option)
                return fallback;

            std::string const raw = sConfigMgr->GetStringDefault(option, "");
            std::string_view const trimmed = Trim(raw);
            if (trimmed.empty())
                return fallback;

            if (Optional<float> parsed = ParseOptionalFloat(std::string(trimmed), option, logEnabled))
                return *parsed;

            return fallback;
        }

        InflectionPointSettings ParseInflectionPointSettings(char const* valueOption, char const* floorOption, char const* ceilingOption, char const* bossOption, InflectionPointSettings defaults, bool logEnabled)
        {
            InflectionPointSettings settings = defaults;
            settings.Value = ParseInflectionPointField(valueOption, settings.Value, logEnabled);
            settings.CurveFloor = ParseInflectionPointField(floorOption, settings.CurveFloor, logEnabled);
            settings.CurveCeiling = ParseInflectionPointField(ceilingOption, settings.CurveCeiling, logEnabled);
            settings.BossModifier = ParseInflectionPointField(bossOption, settings.BossModifier, logEnabled);
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

        ScalingMethod ParseScalingMethod(char const* option, ScalingMethod defaultValue, bool logEnabled)
        {
            std::string raw = sConfigMgr->GetStringDefault(option, defaultValue == ScalingMethod::Dynamic ? "dynamic" : "fixed");
            std::string lowered = raw;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (lowered == "dynamic")
                return ScalingMethod::Dynamic;

            if (lowered == "fixed")
                return ScalingMethod::Fixed;

            LogMessage(MessageLevel::Warn, logEnabled, "Invalid scaling method '{}' for {}. Expected 'dynamic' or 'fixed'. Using default ({}).",
                raw, option, defaultValue == ScalingMethod::Dynamic ? "dynamic" : "fixed");
            return defaultValue;
        }

        LevelScalingSettings ParseLevelScalingSettings(char const* ceilingOption, char const* floorOption, LevelScalingSettings defaults, bool logEnabled)
        {
            LevelScalingSettings settings = defaults;

            int32 ceiling = sConfigMgr->GetIntDefault(ceilingOption, defaults.Ceiling);
            int32 floor = sConfigMgr->GetIntDefault(floorOption, defaults.Floor);

            if (ceiling < 0)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "{} ({}) must be >= 0. Using {} instead.", ceilingOption, ceiling, defaults.Ceiling);
                ceiling = defaults.Ceiling;
            }

            if (floor < 0)
            {
                LogMessage(MessageLevel::Warn, logEnabled, "{} ({}) must be >= 0. Using {} instead.", floorOption, floor, defaults.Floor);
                floor = defaults.Floor;
            }

            settings.Ceiling = static_cast<int8>(ceiling);
            settings.Floor = static_cast<int8>(floor);
            return settings;
        }

        std::unordered_map<uint32, LevelScalingSettings> ParseLevelScalingOverrides(std::string const& value, bool logEnabled)
        {
            std::unordered_map<uint32, LevelScalingSettings> overrides;

            for (std::string_view entry : Trinity::Tokenize(value, ',', false))
            {
                entry = Trim(entry);
                if (entry.empty())
                    continue;

                std::istringstream stream{std::string(entry)};
                uint32 mapId = 0;
                if (!(stream >> mapId))
                {
                    LogMessage(MessageLevel::Warn, logEnabled, "Ignoring entry '{}' in AutoBalance.LevelScaling.DynamicLevel.PerInstance: missing map id.", entry);
                    continue;
                }

                LevelScalingSettings settings{};
                settings.Floor = -1;
                settings.Ceiling = -1;
                settings.SkipHigher = -1;
                settings.SkipLower = -1;

                auto readNext = [&](int8& field)
                {
                    int32 temp = 0;
                    if (!(stream >> temp))
                        return false;

                    field = static_cast<int8>(temp);
                    return true;
                };

                if (!readNext(settings.SkipHigher))
                    settings.SkipHigher = -1;
                if (!readNext(settings.SkipLower))
                    settings.SkipLower = -1;
                if (!readNext(settings.Ceiling))
                    settings.Ceiling = -1;
                if (!readNext(settings.Floor))
                    settings.Floor = -1;

                overrides[mapId] = settings;
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

    ConfigLoadInfo const& GetConfigLoadInfo()
    {
        return s_ConfigLoadInfo;
    }

    void LoadConfig(bool reload)
    {
        bool wasInitialized = s_ConfigInitialized;

        std::string configFile = sConfigMgr->GetStringDefault("AutoBalance.Conf", "conf/AutoBalance.conf");
        bool logReady = reload || wasInitialized;

        ConfigFileLoadResult loadResult;
        if (!configFile.empty())
            loadResult = MergeConfigFile(configFile, logReady);

        s_ConfigLoadInfo.RequestedPath = configFile;
        s_ConfigLoadInfo.Attempts = std::move(loadResult.Attempts);
        s_ConfigLoadInfo.ResolvedPath = std::move(loadResult.ResolvedPath);
        s_ConfigLoadInfo.Loaded = loadResult.Loaded;
        s_ConfigLoadInfo.UsedFallback = loadResult.UsedFallback;
        s_ConfigLoadInfo.Error = std::move(loadResult.Error);
        if (configFile.empty())
            s_ConfigLoadInfo.Attempts.clear();

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

        int32 minPlayersRaid = sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.Raid", minPlayers);
        if (minPlayersRaid < 0)
        {
            LogMessage(MessageLevel::Warn, logReady, "AutoBalance.MinPlayers.Raid ({}) must be >= 0. Using 0 instead.", minPlayersRaid);
            minPlayersRaid = 0;
        }

        int32 minPlayersRaidHeroic = sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.RaidHeroic", minPlayersHeroic);
        if (minPlayersRaidHeroic < 0)
        {
            LogMessage(MessageLevel::Warn, logReady, "AutoBalance.MinPlayers.RaidHeroic ({}) must be >= 0. Using 0 instead.", minPlayersRaidHeroic);
            minPlayersRaidHeroic = 0;
        }

        newConfig.MinimumPlayers = static_cast<uint32>(minPlayers);
        newConfig.MinimumPlayersHeroic = static_cast<uint32>(minPlayersHeroic);
        newConfig.MinimumPlayersRaid = static_cast<uint32>(minPlayersRaid);
        newConfig.MinimumPlayersRaidHeroic = static_cast<uint32>(minPlayersRaidHeroic);

        newConfig.DisabledInstances = ParseDisabledInstances(sConfigMgr->GetStringDefault("AutoBalance.Disable.PerInstance", ""), logReady);
        newConfig.MinPlayersOverridesNormal = ParseMinPlayersOverrides(sConfigMgr->GetStringDefault("AutoBalance.MinPlayers.PerInstance", ""), logReady, "AutoBalance.MinPlayers.PerInstance");
        newConfig.MinPlayersOverridesHeroic = ParseMinPlayersOverrides(sConfigMgr->GetStringDefault("AutoBalance.MinPlayers.Heroic.PerInstance", ""), logReady, "AutoBalance.MinPlayers.Heroic.PerInstance");
        newConfig.PlayerCountDifficultyOffset = sConfigMgr->GetIntDefault("AutoBalance.playerCountDifficultyOffset", 0);
        newConfig.MinCCDurationModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinCCDurationModifier", 0.25f));
        newConfig.MaxCCDurationModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MaxCCDurationModifier", 1.0f));
        newConfig.MinHPModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinHPModifier", 0.01f));
        newConfig.MinManaModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinManaModifier", 0.01f));
        newConfig.MinDamageModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.MinDamageModifier", 0.01f));

        newConfig.LevelScalingEnabled = sConfigMgr->GetBoolDefault("AutoBalance.LevelScaling", false);
        newConfig.LevelScalingMethod = ParseScalingMethod("AutoBalance.LevelScaling.Method", ScalingMethod::Dynamic, logReady);
        newConfig.LevelScalingSkipHigherLevels = static_cast<int8>(sConfigMgr->GetIntDefault("AutoBalance.LevelScaling.SkipHigherLevels", 0));
        newConfig.LevelScalingSkipLowerLevels = static_cast<int8>(sConfigMgr->GetIntDefault("AutoBalance.LevelScaling.SkipLowerLevels", 0));
        newConfig.LevelScalingDungeonSettings = ParseLevelScalingSettings("AutoBalance.LevelScaling.DynamicLevel.Ceiling.Dungeons", "AutoBalance.LevelScaling.DynamicLevel.Floor.Dungeons", newConfig.LevelScalingDungeonSettings, logReady);
        newConfig.LevelScalingHeroicDungeonSettings = ParseLevelScalingSettings("AutoBalance.LevelScaling.DynamicLevel.Ceiling.HeroicDungeons", "AutoBalance.LevelScaling.DynamicLevel.Floor.HeroicDungeons", newConfig.LevelScalingHeroicDungeonSettings, logReady);
        newConfig.LevelScalingRaidSettings = ParseLevelScalingSettings("AutoBalance.LevelScaling.DynamicLevel.Ceiling.Raids", "AutoBalance.LevelScaling.DynamicLevel.Floor.Raids", newConfig.LevelScalingRaidSettings, logReady);
        newConfig.LevelScalingHeroicRaidSettings = ParseLevelScalingSettings("AutoBalance.LevelScaling.DynamicLevel.Ceiling.HeroicRaids", "AutoBalance.LevelScaling.DynamicLevel.Floor.HeroicRaids", newConfig.LevelScalingHeroicRaidSettings, logReady);
        newConfig.LevelScalingOverridesByInstance = ParseLevelScalingOverrides(sConfigMgr->GetStringDefault("AutoBalance.LevelScaling.DynamicLevel.PerInstance", ""), logReady);

        newConfig.RewardScalingMethod = ParseScalingMethod("AutoBalance.RewardScaling.Method", ScalingMethod::Dynamic, logReady);
        newConfig.RewardScalingXP = sConfigMgr->GetBoolDefault("AutoBalance.RewardScaling.XP", false);
        newConfig.RewardScalingXPModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.RewardScaling.XP.Modifier", 1.0f));
        newConfig.RewardScalingMoney = sConfigMgr->GetBoolDefault("AutoBalance.RewardScaling.Money", false);
        newConfig.RewardScalingMoneyModifier = static_cast<float>(sConfigMgr->GetFloatDefault("AutoBalance.RewardScaling.Money.Modifier", 1.0f));

        auto const defaultInflection = InflectionPointSettings{};
        newConfig.DungeonInflection = ParseInflectionPointSettings("AutoBalance.InflectionPoint", "AutoBalance.InflectionPoint.CurveFloor", "AutoBalance.InflectionPoint.CurveCeiling", "AutoBalance.InflectionPoint.BossModifier", defaultInflection, logReady);
        newConfig.DungeonHeroicInflection = ParseInflectionPointSettings("AutoBalance.InflectionPointHeroic", "AutoBalance.InflectionPointHeroic.CurveFloor", "AutoBalance.InflectionPointHeroic.CurveCeiling", "AutoBalance.InflectionPointHeroic.BossModifier", defaultInflection, logReady);
        newConfig.RaidInflection = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid", "AutoBalance.InflectionPointRaid.CurveFloor", "AutoBalance.InflectionPointRaid.CurveCeiling", "AutoBalance.InflectionPointRaid.BossModifier", defaultInflection, logReady);
        newConfig.RaidHeroicInflection = ParseInflectionPointSettings("AutoBalance.InflectionPointRaidHeroic", "AutoBalance.InflectionPointRaidHeroic.CurveFloor", "AutoBalance.InflectionPointRaidHeroic.CurveCeiling", "AutoBalance.InflectionPointRaidHeroic.BossModifier", defaultInflection, logReady);
        newConfig.RaidInflection10 = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid10M", "AutoBalance.InflectionPointRaid10M.CurveFloor", "AutoBalance.InflectionPointRaid10M.CurveCeiling", "AutoBalance.InflectionPointRaid10M.BossModifier", newConfig.RaidInflection, logReady);
        newConfig.RaidInflection15 = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid15M", "AutoBalance.InflectionPointRaid15M.CurveFloor", "AutoBalance.InflectionPointRaid15M.CurveCeiling", "AutoBalance.InflectionPointRaid15M.BossModifier", newConfig.RaidInflection, logReady);
        newConfig.RaidInflection20 = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid20M", "AutoBalance.InflectionPointRaid20M.CurveFloor", "AutoBalance.InflectionPointRaid20M.CurveCeiling", "AutoBalance.InflectionPointRaid20M.BossModifier", newConfig.RaidInflection, logReady);
        newConfig.RaidInflection25 = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid25M", "AutoBalance.InflectionPointRaid25M.CurveFloor", "AutoBalance.InflectionPointRaid25M.CurveCeiling", "AutoBalance.InflectionPointRaid25M.BossModifier", newConfig.RaidInflection, logReady);
        newConfig.RaidInflection40 = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid40M", "AutoBalance.InflectionPointRaid40M.CurveFloor", "AutoBalance.InflectionPointRaid40M.CurveCeiling", "AutoBalance.InflectionPointRaid40M.BossModifier", newConfig.RaidInflection, logReady);
        newConfig.RaidHeroicInflection10 = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid10MHeroic", "AutoBalance.InflectionPointRaid10MHeroic.CurveFloor", "AutoBalance.InflectionPointRaid10MHeroic.CurveCeiling", "AutoBalance.InflectionPointRaid10MHeroic.BossModifier", newConfig.RaidHeroicInflection, logReady);
        newConfig.RaidHeroicInflection25 = ParseInflectionPointSettings("AutoBalance.InflectionPointRaid25MHeroic", "AutoBalance.InflectionPointRaid25MHeroic.CurveFloor", "AutoBalance.InflectionPointRaid25MHeroic.CurveCeiling", "AutoBalance.InflectionPointRaid25MHeroic.BossModifier", newConfig.RaidHeroicInflection, logReady);

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

        std::string logPath = s_ConfigLoadInfo.ResolvedPath.empty() ? configFile : s_ConfigLoadInfo.ResolvedPath;
        if (logPath.empty())
            logPath = "<defaults>";

        if (s_ConfigLoadInfo.Loaded)
        {
            if (reload)
                LogMessage(MessageLevel::Info, true, "AutoBalance configuration reloaded from '{}'.", logPath);
            else if (logReady && !s_LoggedStartup)
            {
                LogMessage(MessageLevel::Info, true, "AutoBalance configuration loaded from '{}'.", logPath);
                s_LoggedStartup = true;
            }
        }
        else if (logReady && !s_LoggedStartup)
            s_LoggedStartup = true;
    }
}
