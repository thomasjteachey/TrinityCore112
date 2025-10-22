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

#ifndef CONFIG_H
#define CONFIG_H

#include "Define.h"
#include <string>
#include <type_traits>
#include <vector>

class TC_COMMON_API ConfigMgr
{
    ConfigMgr() = default;
    ConfigMgr(ConfigMgr const&) = delete;
    ConfigMgr& operator=(ConfigMgr const&) = delete;
    ~ConfigMgr() = default;

public:
    /// Method used only for loading main configuration files (authserver.conf and worldserver.conf)
    bool LoadInitial(std::string file, std::vector<std::string> args, std::string& error);
    bool LoadAdditionalFile(std::string file, bool keepOnReload, std::string& error);

    /// Overrides configuration with environment variables and returns overridden keys
    std::vector<std::string> OverrideWithEnvVariablesIfAny();

    static ConfigMgr* instance();

    bool Reload(std::vector<std::string>& errors);

    std::string GetStringDefault(std::string const& name, const std::string& def, bool quiet = false) const;
    bool GetBoolDefault(std::string const& name, bool def, bool quiet = false) const;
    int GetIntDefault(std::string const& name, int def, bool quiet = false) const;
    float GetFloatDefault(std::string const& name, float def, bool quiet = false) const;

    std::string const& GetFilename();
    std::vector<std::string> const& GetArguments() const;
    std::vector<std::string> GetKeysByString(std::string const& name);

    template<class T>
    T GetOption(std::string const& name, T const& def, bool quiet = false) const;

private:
    template<class T>
    T GetValueDefault(std::string const& name, T def, bool quiet) const;
};

template<class T>
inline T ConfigMgr::GetOption(std::string const& /*name*/, T const& def, bool /*quiet*/) const
{
    static_assert(!std::is_same<T, T>::value, "Unsupported config option type requested via ConfigMgr::GetOption");
    return def;
}

template<>
inline std::string ConfigMgr::GetOption<std::string>(std::string const& name, std::string const& def, bool quiet) const
{
    return GetStringDefault(name, def, quiet);
}

template<>
inline bool ConfigMgr::GetOption<bool>(std::string const& name, bool const& def, bool quiet) const
{
    return GetBoolDefault(name, def, quiet);
}

template<>
inline int ConfigMgr::GetOption<int>(std::string const& name, int const& def, bool quiet) const
{
    return GetIntDefault(name, def, quiet);
}

template<>
inline uint32 ConfigMgr::GetOption<uint32>(std::string const& name, uint32 const& def, bool quiet) const
{
    return static_cast<uint32>(GetIntDefault(name, static_cast<int>(def), quiet));
}

template<>
inline uint8 ConfigMgr::GetOption<uint8>(std::string const& name, uint8 const& def, bool quiet) const
{
    return static_cast<uint8>(GetIntDefault(name, static_cast<int>(def), quiet));
}

template<>
inline float ConfigMgr::GetOption<float>(std::string const& name, float const& def, bool quiet) const
{
    return GetFloatDefault(name, def, quiet);
}

#define sConfigMgr ConfigMgr::instance()

#endif
