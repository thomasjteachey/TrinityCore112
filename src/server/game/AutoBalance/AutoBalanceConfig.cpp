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
        void LogMessage(MessageLevel level, bool logEnabled, char const* format, Args&&... args)
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

                std::istringstream stream(std::string(entry));
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
