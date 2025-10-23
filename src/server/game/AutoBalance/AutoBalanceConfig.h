#pragma once

#include "Define.h"
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
    };

    ModuleConfig const& GetConfig();
    bool IsEnabled();
    void LoadConfig(bool reload);
}
