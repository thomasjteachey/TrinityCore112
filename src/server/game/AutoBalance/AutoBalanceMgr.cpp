#include "AutoBalanceMgr.h"

#include "Chat.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "InstanceMap.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AutoBalance
{
namespace
{
    constexpr char const* AutoBalanceConfigFileName = "autobalance.conf";

    struct CurveConfig
    {
        float value = 0.35f;
        float curveFloor = 0.0f;
        float curveCeiling = 1.0f;
        float bossModifier = 1.0f;
    };

    struct CurveOverride
    {
        std::optional<float> value;
        std::optional<float> curveFloor;
        std::optional<float> curveCeiling;
    };

    struct StatConfig
    {
        float global = 1.0f;
        float health = 1.0f;
        float mana = 1.0f;
        float armor = 1.0f;
        float damage = 1.0f;
        float ccDuration = 1.0f;
    };

    struct StatOverride
    {
        std::optional<float> global;
        std::optional<float> health;
        std::optional<float> mana;
        std::optional<float> armor;
        std::optional<float> damage;
        std::optional<float> ccDuration;
    };

    struct Settings
    {
        bool enableGlobal = true;
        bool enable5M = true;
        bool enable10M = true;
        bool enable15M = true;
        bool enable20M = true;
        bool enable25M = true;
        bool enable40M = true;
        bool enableOtherNormal = true;
        bool enable5MHeroic = true;
        bool enable10MHeroic = true;
        bool enable25MHeroic = true;
        bool enableOtherHeroic = true;

        uint32 minPlayersNormal = 1;
        uint32 minPlayersHeroic = 1;
        int32 playerDifficultyOffset = 0;

        CurveConfig dungeonNormal;
        CurveConfig dungeonHeroic;
        CurveConfig raidDefault;
        CurveConfig raidHeroic;
        CurveConfig raid10;
        CurveConfig raid15;
        CurveConfig raid20;
        CurveConfig raid25;
        CurveConfig raid40;
        CurveConfig raid10Heroic;
        CurveConfig raid25Heroic;

        StatConfig dungeonStats;
        StatConfig dungeonBossStats;
        StatConfig dungeonHeroicStats;
        StatConfig dungeonHeroicBossStats;
        StatConfig raidStats;
        StatConfig raidBossStats;
        StatConfig raidHeroicStats;
        StatConfig raidHeroicBossStats;
        StatConfig raid10Stats;
        StatConfig raid10BossStats;
        StatConfig raid10HeroicStats;
        StatConfig raid10HeroicBossStats;
        StatConfig raid15Stats;
        StatConfig raid15BossStats;
        StatConfig raid20Stats;
        StatConfig raid20BossStats;
        StatConfig raid25Stats;
        StatConfig raid25BossStats;
        StatConfig raid25HeroicStats;
        StatConfig raid25HeroicBossStats;
        StatConfig raid40Stats;
        StatConfig raid40BossStats;

        std::unordered_set<uint32> disabledInstances;
        std::unordered_map<uint32, uint8> minPlayersPerInstance;
        std::unordered_map<uint32, uint8> minPlayersPerInstanceHeroic;
        std::unordered_map<uint32, CurveOverride> curveOverrides;
        std::unordered_map<uint32, float> bossCurveMultipliers;
        std::unordered_map<uint32, StatOverride> statOverrides;
        std::unordered_map<uint32, StatOverride> statBossOverrides;
        std::unordered_map<uint32, StatOverride> statCreatureOverrides;
        std::unordered_map<uint32, uint32> forcedCreatureSizes;
        std::unordered_set<uint32> disabledCreatureEntries;

        float minHPModifier = 0.01f;
        float minManaModifier = 0.01f;
        float minDamageModifier = 0.01f;
        float minCCDurationModifier = 0.25f;
        float maxCCDurationModifier = 1.0f;

        bool playerChangeNotify = true;
    } sSettings;

    struct MapState
    {
        uint32 lastEffectivePlayers = 0;
        float lastScale = 0.0f;
    };

    std::unordered_map<uint64, MapState> sMapStates;

    struct InstanceContext
    {
        bool isHeroic = false;
        bool isRaid = false;
        uint32 maxPlayers = 5u;
    };

    std::string Trim(std::string const& str)
    {
        size_t begin = 0;
        while (begin < str.size() && std::isspace(static_cast<unsigned char>(str[begin])))
            ++begin;

        size_t end = str.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(str[end - 1])))
            --end;

        return str.substr(begin, end - begin);
    }

    std::vector<std::string> Split(std::string const& str, char delimiter)
    {
        std::vector<std::string> tokens;
        std::string current;
        std::istringstream stream(str);

        while (std::getline(stream, current, delimiter))
        {
            current = Trim(current);
            if (!current.empty())
                tokens.push_back(current);
        }

        return tokens;
    }

    bool ParseUint(std::string const& value, uint32& result)
    {
        std::istringstream stream(value);
        uint64 temp;
        stream >> temp;
        if (stream.fail())
            return false;
        if (temp > std::numeric_limits<uint32>::max())
            return false;
        result = static_cast<uint32>(temp);
        return true;
    }

    bool ParseFloat(std::string const& value, float& result)
    {
        std::istringstream stream(value);
        stream >> result;
        return !stream.fail();
    }

    std::optional<float> ReadOptionalFloat(std::string const& key)
    {
        std::string raw = Trim(sConfigMgr->GetStringDefault(key, ""));
        if (raw.empty())
            return std::nullopt;

        float parsed;
        if (!ParseFloat(raw, parsed))
        {
            TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse float configuration value for %s (%s)", key.c_str(), raw.c_str());
            return std::nullopt;
        }

        return parsed;
    }

    void ApplyStatOverride(StatConfig& config, StatOverride const& override)
    {
        if (override.global)
            config.global = *override.global;
        if (override.health)
            config.health = *override.health;
        if (override.mana)
            config.mana = *override.mana;
        if (override.armor)
            config.armor = *override.armor;
        if (override.damage)
            config.damage = *override.damage;
        if (override.ccDuration)
            config.ccDuration = *override.ccDuration;
    }

    uint64 MakeMapKey(Map const* map)
    {
        uint32 instanceId = map->GetInstanceId();
        return (uint64(map->GetId()) << 32) | instanceId;
    }

    bool IsInstanceDisabled(uint32 mapId)
    {
        return sSettings.disabledInstances.find(mapId) != sSettings.disabledInstances.end();
    }

    std::optional<uint32> GetForcedSizeForCreature(uint32 entry)
    {
        auto itr = sSettings.forcedCreatureSizes.find(entry);
        if (itr != sSettings.forcedCreatureSizes.end())
            return itr->second;
        return std::nullopt;
    }

    bool IsCreatureDisabled(uint32 entry)
    {
        return sSettings.disabledCreatureEntries.find(entry) != sSettings.disabledCreatureEntries.end();
    }

    InstanceContext MakeContext(InstanceMap* map, std::optional<uint32> forcedSize = std::nullopt)
    {
        InstanceContext context;
        context.isHeroic = map->IsHeroic();
        context.isRaid = map->IsRaid();
        context.maxPlayers = forcedSize.value_or(map->GetMaxPlayers());

        if (!context.maxPlayers)
            context.maxPlayers = context.isRaid ? 10u : 5u;

        return context;
    }

    bool IsEnabledForContext(InstanceMap* map, InstanceContext const& context)
    {
        if (!sSettings.enableGlobal)
            return false;

        if (IsInstanceDisabled(map->GetId()))
            return false;

        uint32 size = context.maxPlayers;

        if (context.isHeroic)
        {
            if (context.isRaid)
            {
                if (size == 10)
                    return sSettings.enable10MHeroic;
                if (size == 25)
                    return sSettings.enable25MHeroic;
                return sSettings.enableOtherHeroic;
            }

            if (size == 5)
                return sSettings.enable5MHeroic;
            if (size == 10)
                return sSettings.enable10MHeroic;
            if (size == 25)
                return sSettings.enable25MHeroic;
            return sSettings.enableOtherHeroic;
        }

        if (context.isRaid)
        {
            switch (size)
            {
                case 10: return sSettings.enable10M;
                case 15: return sSettings.enable15M;
                case 20: return sSettings.enable20M;
                case 25: return sSettings.enable25M;
                case 40: return sSettings.enable40M;
                default: return sSettings.enableOtherNormal;
            }
        }

        switch (size)
        {
            case 5: return sSettings.enable5M;
            case 10: return sSettings.enable10M;
            case 15: return sSettings.enable15M;
            case 20: return sSettings.enable20M;
            case 25: return sSettings.enable25M;
            default: return sSettings.enableOtherNormal;
        }
    }

    uint32 GetConfiguredMinPlayers(InstanceMap* map)
    {
        uint32 mapId = map->GetId();
        bool heroic = map->IsHeroic();

        if (heroic)
        {
            if (auto itr = sSettings.minPlayersPerInstanceHeroic.find(mapId); itr != sSettings.minPlayersPerInstanceHeroic.end())
                return itr->second;
            return sSettings.minPlayersHeroic;
        }

        if (auto itr = sSettings.minPlayersPerInstance.find(mapId); itr != sSettings.minPlayersPerInstance.end())
            return itr->second;

        return sSettings.minPlayersNormal;
    }

    uint32 GetEffectivePlayerCount(InstanceMap* map, InstanceContext const& context)
    {
        uint32 count = map->GetPlayersCountExceptGMs();
        uint32 minPlayers = GetConfiguredMinPlayers(map);

        if (count < minPlayers)
            count = minPlayers;

        if (sSettings.playerDifficultyOffset != 0)
        {
            if (sSettings.playerDifficultyOffset > 0)
                count += static_cast<uint32>(sSettings.playerDifficultyOffset);
            else
            {
                uint32 offset = static_cast<uint32>(-sSettings.playerDifficultyOffset);
                if (count > offset)
                    count -= offset;
                else
                    count = 0;
            }
        }

        if (context.maxPlayers)
            count = std::min(count, context.maxPlayers);

        return count;
    }

    CurveConfig const& GetBaseCurve(InstanceContext const& context)
    {
        if (context.isHeroic)
        {
            if (context.isRaid)
            {
                if (context.maxPlayers == 10)
                    return sSettings.raid10Heroic;
                if (context.maxPlayers == 25)
                    return sSettings.raid25Heroic;
                return sSettings.raidHeroic;
            }

            return sSettings.dungeonHeroic;
        }

        if (context.isRaid)
        {
            switch (context.maxPlayers)
            {
                case 10: return sSettings.raid10;
                case 15: return sSettings.raid15;
                case 20: return sSettings.raid20;
                case 25: return sSettings.raid25;
                case 40: return sSettings.raid40;
                default: return sSettings.raidDefault;
            }
        }

        return sSettings.dungeonNormal;
    }

    StatConfig const& GetBaseStatConfig(InstanceContext const& context, bool isBoss)
    {
        if (context.isHeroic)
        {
            if (context.isRaid)
            {
                if (context.maxPlayers == 10)
                    return isBoss ? sSettings.raid10HeroicBossStats : sSettings.raid10HeroicStats;
                if (context.maxPlayers == 25)
                    return isBoss ? sSettings.raid25HeroicBossStats : sSettings.raid25HeroicStats;
                return isBoss ? sSettings.raidHeroicBossStats : sSettings.raidHeroicStats;
            }

            return isBoss ? sSettings.dungeonHeroicBossStats : sSettings.dungeonHeroicStats;
        }

        if (context.isRaid)
        {
            switch (context.maxPlayers)
            {
                case 10: return isBoss ? sSettings.raid10BossStats : sSettings.raid10Stats;
                case 15: return isBoss ? sSettings.raid15BossStats : sSettings.raid15Stats;
                case 20: return isBoss ? sSettings.raid20BossStats : sSettings.raid20Stats;
                case 25: return isBoss ? sSettings.raid25BossStats : sSettings.raid25Stats;
                case 40: return isBoss ? sSettings.raid40BossStats : sSettings.raid40Stats;
                default: return isBoss ? sSettings.raidBossStats : sSettings.raidStats;
            }
        }

        return isBoss ? sSettings.dungeonBossStats : sSettings.dungeonStats;
    }

    CurveConfig ResolveCurveConfig(InstanceMap* map, InstanceContext const& context, bool isBoss)
    {
        CurveConfig config = GetBaseCurve(context);

        if (auto itr = sSettings.curveOverrides.find(map->GetId()); itr != sSettings.curveOverrides.end())
        {
            if (itr->second.value)
                config.value = *itr->second.value;
            if (itr->second.curveFloor)
                config.curveFloor = *itr->second.curveFloor;
            if (itr->second.curveCeiling)
                config.curveCeiling = *itr->second.curveCeiling;
        }

        float bossMultiplier = config.bossModifier;
        if (auto itr = sSettings.bossCurveMultipliers.find(map->GetId()); itr != sSettings.bossCurveMultipliers.end())
            bossMultiplier = itr->second;

        if (isBoss)
            config.value *= bossMultiplier;

        return config;
    }

    StatConfig ResolveStatConfig(InstanceMap* map, InstanceContext const& context, bool isBoss, Creature const* creature)
    {
        StatConfig config = GetBaseStatConfig(context, isBoss);

        if (isBoss)
        {
            if (auto itr = sSettings.statBossOverrides.find(map->GetId()); itr != sSettings.statBossOverrides.end())
                ApplyStatOverride(config, itr->second);
        }
        else
        {
            if (auto itr = sSettings.statOverrides.find(map->GetId()); itr != sSettings.statOverrides.end())
                ApplyStatOverride(config, itr->second);
        }

        if (creature)
        {
            if (auto itr = sSettings.statCreatureOverrides.find(creature->GetEntry()); itr != sSettings.statCreatureOverrides.end())
                ApplyStatOverride(config, itr->second);
        }

        return config;
    }

    float ComputeDefaultMultiplier(InstanceMap* map, InstanceContext const& context, CurveConfig const& curve, uint32 effectivePlayers)
    {
        float maxPlayers = static_cast<float>(std::max<uint32>(1u, context.maxPlayers));
        float diff = (maxPlayers / 5.0f) * 1.5f;
        if (diff <= 0.0f)
            diff = 1.0f;

        float tanhMax = (std::tanh((maxPlayers - curve.value) / diff) + 1.0f) * 0.5f;
        float denominator = tanhMax * (curve.curveCeiling - curve.curveFloor) + curve.curveFloor;
        float curveCeilingAdjustment = 1.0f;

        if (denominator > 0.0f)
            curveCeilingAdjustment = curve.curveCeiling / denominator;

        float adjustedPlayers = static_cast<float>(effectivePlayers);
        float tanhPlayers = (std::tanh((adjustedPlayers - curve.value) / diff) + 1.0f) * 0.5f;
        float multiplier = tanhPlayers * (curve.curveCeiling * curveCeilingAdjustment - curve.curveFloor) + curve.curveFloor;

        return std::max(0.01f, multiplier);
    }

    struct CreatureContext
    {
        Creature const* creature = nullptr;
        InstanceMap* instance = nullptr;
        InstanceContext context;
        bool isBoss = false;
    };

    bool BuildCreatureContext(Creature const* creature, CreatureContext& outContext)
    {
        if (!creature)
            return false;

        if (IsCreatureDisabled(creature->GetEntry()))
            return false;

        if (creature->IsPet() || creature->IsTotem() || creature->IsSummon() || creature->IsControlledByPlayer())
            return false;

        Map* map = creature->GetMap();
        if (!map || !map->IsDungeon())
            return false;

        InstanceMap* instance = map->ToInstanceMap();
        if (!instance)
            return false;

        CreatureContext context;
        context.creature = creature;
        context.instance = instance;
        context.context = MakeContext(instance, GetForcedSizeForCreature(creature->GetEntry()));
        context.isBoss = creature->IsDungeonBoss() || creature->isWorldBoss();

        if (!IsEnabledForContext(instance, context.context))
            return false;

        outContext = context;
        return true;
    }

    float GetHealthMultiplier(Creature const* creature)
    {
        CreatureContext context;
        if (!BuildCreatureContext(creature, context))
            return 1.0f;

        uint32 effectivePlayers = GetEffectivePlayerCount(context.instance, context.context);
        CurveConfig curve = ResolveCurveConfig(context.instance, context.context, context.isBoss);
        StatConfig stats = ResolveStatConfig(context.instance, context.context, context.isBoss, creature);

        float multiplier = ComputeDefaultMultiplier(context.instance, context.context, curve, effectivePlayers);
        multiplier *= stats.global;
        multiplier *= stats.health;

        if (multiplier < sSettings.minHPModifier)
            multiplier = sSettings.minHPModifier;

        return multiplier;
    }

    float GetDamageMultiplier(Creature const* creature)
    {
        CreatureContext context;
        if (!BuildCreatureContext(creature, context))
            return 1.0f;

        uint32 effectivePlayers = GetEffectivePlayerCount(context.instance, context.context);
        CurveConfig curve = ResolveCurveConfig(context.instance, context.context, context.isBoss);
        StatConfig stats = ResolveStatConfig(context.instance, context.context, context.isBoss, creature);

        float multiplier = ComputeDefaultMultiplier(context.instance, context.context, curve, effectivePlayers);
        multiplier *= stats.global;
        multiplier *= stats.damage;

        if (multiplier < sSettings.minDamageModifier)
            multiplier = sSettings.minDamageModifier;

        return multiplier;
    }

    template<typename T>
    void ApplyModifier(T& value, float modifier)
    {
        if (value <= 0)
            return;

        double scaled = std::floor(static_cast<double>(value) * modifier + 0.5);
        if (scaled < 1.0)
            scaled = 1.0;

        value = static_cast<T>(scaled);
    }

    std::optional<float> GetPlayerToCreatureModifier(Unit* attacker, Unit* victim)
    {
        if (!attacker || !victim)
            return std::nullopt;

        if (!attacker->GetCharmerOrOwnerPlayerOrPlayerItself())
            return std::nullopt;

        Creature* creature = victim->ToCreature();
        if (!creature)
            return std::nullopt;

        float healthMultiplier = GetHealthMultiplier(creature);
        if (healthMultiplier <= 0.0f)
            return std::nullopt;

        return 1.0f / healthMultiplier;
    }

    std::optional<float> GetCreatureToPlayerModifier(Unit* attacker, Unit* victim)
    {
        if (!attacker || !victim)
            return std::nullopt;

        if (!victim->GetCharmerOrOwnerPlayerOrPlayerItself())
            return std::nullopt;

        Creature* creature = attacker->ToCreature();
        if (!creature)
            return std::nullopt;

        float damageMultiplier = GetDamageMultiplier(creature);
        if (damageMultiplier <= 0.0f)
            return std::nullopt;

        return damageMultiplier;
    }

    void ParseDisabledInstances()
    {
        sSettings.disabledInstances.clear();
        std::string list = sConfigMgr->GetStringDefault("AutoBalance.Disable.PerInstance", "");
        for (std::string const& token : Split(list, ','))
        {
            uint32 mapId;
            if (ParseUint(token, mapId))
                sSettings.disabledInstances.insert(mapId);
            else if (!token.empty())
                TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse map id '%s' in AutoBalance.Disable.PerInstance", token.c_str());
        }
    }

    void ParseMinPlayerOverrides(char const* key, std::unordered_map<uint32, uint8>& storage)
    {
        storage.clear();
        std::string list = sConfigMgr->GetStringDefault(key, "");
        for (std::string const& token : Split(list, ','))
        {
            std::istringstream stream(token);
            uint32 mapId;
            uint32 value;
            stream >> mapId >> value;
            if (stream.fail())
            {
                TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse min player override '%s' in %s", token.c_str(), key);
                continue;
            }

            storage[mapId] = static_cast<uint8>(std::max<uint32>(1u, value));
        }
    }

    CurveConfig LoadCurveConfig(char const* key, CurveConfig const& defaults)
    {
        CurveConfig config = defaults;

        if (auto value = ReadOptionalFloat(key))
            config.value = *value;
        if (auto value = ReadOptionalFloat(std::string(key) + ".CurveFloor"))
            config.curveFloor = *value;
        if (auto value = ReadOptionalFloat(std::string(key) + ".CurveCeiling"))
            config.curveCeiling = *value;
        if (auto value = ReadOptionalFloat(std::string(key) + ".BossModifier"))
            config.bossModifier = *value;

        return config;
    }

    StatConfig LoadStatConfig(std::string const& prefix, StatConfig const& defaults)
    {
        StatConfig config = defaults;

        if (auto value = ReadOptionalFloat(prefix + ".Global"))
            config.global = *value;
        if (auto value = ReadOptionalFloat(prefix + ".Health"))
            config.health = *value;
        if (auto value = ReadOptionalFloat(prefix + ".Mana"))
            config.mana = *value;
        if (auto value = ReadOptionalFloat(prefix + ".Armor"))
            config.armor = *value;
        if (auto value = ReadOptionalFloat(prefix + ".Damage"))
            config.damage = *value;
        if (auto value = ReadOptionalFloat(prefix + ".CCDuration"))
            config.ccDuration = *value;

        return config;
    }

    StatOverride ParseStatOverride(std::string const& token)
    {
        StatOverride override;
        std::istringstream stream(token);
        float global;
        float health;
        float mana;
        float armor;
        float damage;
        float ccDuration;

        if (stream >> global)
            if (global != -1.0f)
                override.global = global;
        if (stream >> health)
            if (health != -1.0f)
                override.health = health;
        if (stream >> mana)
            if (mana != -1.0f)
                override.mana = mana;
        if (stream >> armor)
            if (armor != -1.0f)
                override.armor = armor;
        if (stream >> damage)
            if (damage != -1.0f)
                override.damage = damage;
        if (stream >> ccDuration)
            if (ccDuration != -1.0f)
                override.ccDuration = ccDuration;

        return override;
    }

    void ParseStatOverrideList(char const* key, std::unordered_map<uint32, StatOverride>& storage)
    {
        storage.clear();
        std::string list = sConfigMgr->GetStringDefault(key, "");
        for (std::string const& token : Split(list, ','))
        {
            std::istringstream stream(token);
            uint32 id;
            stream >> id;
            if (stream.fail())
            {
                TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse stat override entry '%s' in %s", token.c_str(), key);
                continue;
            }

            std::string rest;
            std::getline(stream, rest);
            rest = Trim(rest);
            storage[id] = ParseStatOverride(rest);
        }
    }

    void ParseCurveOverrideList()
    {
        sSettings.curveOverrides.clear();
        std::string list = sConfigMgr->GetStringDefault("AutoBalance.InflectionPoint.PerInstance", "");
        for (std::string const& token : Split(list, ','))
        {
            std::istringstream stream(token);
            uint32 id;
            stream >> id;
            if (stream.fail())
            {
                TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse inflection override '%s'", token.c_str());
                continue;
            }

            CurveOverride override;
            float value;
            if (stream >> value)
            {
                if (value != -1.0f)
                    override.value = value;
            }
            float floor;
            if (stream >> floor)
            {
                if (floor != -1.0f)
                    override.curveFloor = floor;
            }
            float ceiling;
            if (stream >> ceiling)
            {
                if (ceiling != -1.0f)
                    override.curveCeiling = ceiling;
            }

            sSettings.curveOverrides[id] = override;
        }
    }

    void ParseBossCurveOverrideList()
    {
        sSettings.bossCurveMultipliers.clear();
        std::string list = sConfigMgr->GetStringDefault("AutoBalance.InflectionPoint.Boss.PerInstance", "");
        for (std::string const& token : Split(list, ','))
        {
            std::istringstream stream(token);
            uint32 id;
            float multiplier;
            stream >> id >> multiplier;
            if (stream.fail())
            {
                TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse boss inflection override '%s'", token.c_str());
                continue;
            }

            sSettings.bossCurveMultipliers[id] = multiplier;
        }
    }

    void ParseForcedCreatureIds(uint32 playerCount, char const* key)
    {
        std::string list = sConfigMgr->GetStringDefault(key, "");
        for (std::string const& token : Split(list, ','))
        {
            uint32 entry;
            if (ParseUint(token, entry))
                sSettings.forcedCreatureSizes[entry] = playerCount;
            else if (!token.empty())
                TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse forced creature entry '%s' in %s", token.c_str(), key);
        }
    }

    void ParseDisabledCreatureIds()
    {
        sSettings.disabledCreatureEntries.clear();
        std::string list = sConfigMgr->GetStringDefault("AutoBalance.DisabledID", "");
        for (std::string const& token : Split(list, ','))
        {
            uint32 entry;
            if (ParseUint(token, entry))
                sSettings.disabledCreatureEntries.insert(entry);
            else if (!token.empty())
                TC_LOG_ERROR("module.AutoBalance", "AutoBalance: could not parse disabled creature entry '%s'", token.c_str());
        }
    }

    void ResetMapState()
    {
        sMapStates.clear();
    }

    void LoadAutoBalanceConfigFile()
    {
        static bool loaded = false;
        static bool warned = false;

        if (loaded)
            return;

        std::string configPath = sConfigMgr->GetFilename();
        size_t separatorPos = configPath.find_last_of("/\\");
        if (separatorPos != std::string::npos)
            configPath.resize(separatorPos + 1);
        else
            configPath.clear();

        configPath += AutoBalanceConfigFileName;

        std::string error;
        if (!sConfigMgr->LoadAdditionalFile(configPath, true, error))
        {
            if (!warned)
            {
                warned = true;
                TC_LOG_WARN("module.AutoBalance", "AutoBalance: failed to load configuration file '%s': %s (using built-in defaults)", configPath.c_str()
, error.c_str());
            }
            return;
        }

        loaded = true;
        TC_LOG_INFO("module.AutoBalance", "AutoBalance: loaded configuration from '%s'", configPath.c_str());
    }
}

void LoadConfig(bool reload)
{
    LoadAutoBalanceConfigFile();

    sSettings.enableGlobal = sConfigMgr->GetBoolDefault("AutoBalance.Enable.Global", true);
    sSettings.enable5M = sConfigMgr->GetBoolDefault("AutoBalance.Enable.5M", true);
    sSettings.enable10M = sConfigMgr->GetBoolDefault("AutoBalance.Enable.10M", true);
    sSettings.enable15M = sConfigMgr->GetBoolDefault("AutoBalance.Enable.15M", true);
    sSettings.enable20M = sConfigMgr->GetBoolDefault("AutoBalance.Enable.20M", true);
    sSettings.enable25M = sConfigMgr->GetBoolDefault("AutoBalance.Enable.25M", true);
    sSettings.enable40M = sConfigMgr->GetBoolDefault("AutoBalance.Enable.40M", true);
    sSettings.enableOtherNormal = sConfigMgr->GetBoolDefault("AutoBalance.Enable.OtherNormal", true);
    sSettings.enable5MHeroic = sConfigMgr->GetBoolDefault("AutoBalance.Enable.5MHeroic", true);
    sSettings.enable10MHeroic = sConfigMgr->GetBoolDefault("AutoBalance.Enable.10MHeroic", true);
    sSettings.enable25MHeroic = sConfigMgr->GetBoolDefault("AutoBalance.Enable.25MHeroic", true);
    sSettings.enableOtherHeroic = sConfigMgr->GetBoolDefault("AutoBalance.Enable.OtherHeroic", true);

    sSettings.minPlayersNormal = std::max<uint32>(1u, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers", 1));
    sSettings.minPlayersHeroic = std::max<uint32>(1u, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.Heroic", sSettings.minPlayersNormal));
    sSettings.playerDifficultyOffset = sConfigMgr->GetIntDefault("AutoBalance.playerCountDifficultyOffset", 0);

    ParseDisabledInstances();
    ParseMinPlayerOverrides("AutoBalance.MinPlayers.PerInstance", sSettings.minPlayersPerInstance);
    ParseMinPlayerOverrides("AutoBalance.MinPlayers.Heroic.PerInstance", sSettings.minPlayersPerInstanceHeroic);

    CurveConfig baseCurve = {0.35f, 0.0f, 1.0f, 1.0f};
    sSettings.dungeonNormal = LoadCurveConfig("AutoBalance.InflectionPoint", baseCurve);
    sSettings.dungeonHeroic = LoadCurveConfig("AutoBalance.InflectionPointHeroic", baseCurve);
    sSettings.raidDefault = LoadCurveConfig("AutoBalance.InflectionPointRaid", baseCurve);
    sSettings.raidHeroic = LoadCurveConfig("AutoBalance.InflectionPointRaidHeroic", baseCurve);
    sSettings.raid10 = LoadCurveConfig("AutoBalance.InflectionPointRaid10M", sSettings.raidDefault);
    sSettings.raid15 = LoadCurveConfig("AutoBalance.InflectionPointRaid15M", sSettings.raidDefault);
    sSettings.raid20 = LoadCurveConfig("AutoBalance.InflectionPointRaid20M", sSettings.raidDefault);
    sSettings.raid25 = LoadCurveConfig("AutoBalance.InflectionPointRaid25M", sSettings.raidDefault);
    sSettings.raid40 = LoadCurveConfig("AutoBalance.InflectionPointRaid40M", sSettings.raidDefault);
    sSettings.raid10Heroic = LoadCurveConfig("AutoBalance.InflectionPointRaid10MHeroic", sSettings.raidHeroic);
    sSettings.raid25Heroic = LoadCurveConfig("AutoBalance.InflectionPointRaid25MHeroic", sSettings.raidHeroic);

    sSettings.dungeonStats = LoadStatConfig("AutoBalance.StatModifier", StatConfig{});
    sSettings.dungeonBossStats = LoadStatConfig("AutoBalance.StatModifier.Boss", sSettings.dungeonStats);
    sSettings.dungeonHeroicStats = LoadStatConfig("AutoBalance.StatModifierHeroic", sSettings.dungeonStats);
    sSettings.dungeonHeroicBossStats = LoadStatConfig("AutoBalance.StatModifierHeroic.Boss", sSettings.dungeonHeroicStats);
    sSettings.raidStats = LoadStatConfig("AutoBalance.StatModifierRaid", StatConfig{});
    sSettings.raidBossStats = LoadStatConfig("AutoBalance.StatModifierRaid.Boss", sSettings.raidStats);
    sSettings.raidHeroicStats = LoadStatConfig("AutoBalance.StatModifierRaidHeroic", sSettings.raidStats);
    sSettings.raidHeroicBossStats = LoadStatConfig("AutoBalance.StatModifierRaidHeroic.Boss", sSettings.raidHeroicStats);
    sSettings.raid10Stats = LoadStatConfig("AutoBalance.StatModifierRaid10M", sSettings.raidStats);
    sSettings.raid10BossStats = LoadStatConfig("AutoBalance.StatModifierRaid10M.Boss", sSettings.raid10Stats);
    sSettings.raid10HeroicStats = LoadStatConfig("AutoBalance.StatModifierRaid10MHeroic", sSettings.raidHeroicStats);
    sSettings.raid10HeroicBossStats = LoadStatConfig("AutoBalance.StatModifierRaid10MHeroic.Boss", sSettings.raid10HeroicStats);
    sSettings.raid15Stats = LoadStatConfig("AutoBalance.StatModifierRaid15M", sSettings.raidStats);
    sSettings.raid15BossStats = LoadStatConfig("AutoBalance.StatModifierRaid15M.Boss", sSettings.raid15Stats);
    sSettings.raid20Stats = LoadStatConfig("AutoBalance.StatModifierRaid20M", sSettings.raidStats);
    sSettings.raid20BossStats = LoadStatConfig("AutoBalance.StatModifierRaid20M.Boss", sSettings.raid20Stats);
    sSettings.raid25Stats = LoadStatConfig("AutoBalance.StatModifierRaid25M", sSettings.raidStats);
    sSettings.raid25BossStats = LoadStatConfig("AutoBalance.StatModifierRaid25M.Boss", sSettings.raid25Stats);
    sSettings.raid25HeroicStats = LoadStatConfig("AutoBalance.StatModifierRaid25MHeroic", sSettings.raidHeroicStats);
    sSettings.raid25HeroicBossStats = LoadStatConfig("AutoBalance.StatModifierRaid25MHeroic.Boss", sSettings.raid25HeroicStats);
    sSettings.raid40Stats = LoadStatConfig("AutoBalance.StatModifierRaid40M", sSettings.raidStats);
    sSettings.raid40BossStats = LoadStatConfig("AutoBalance.StatModifierRaid40M.Boss", sSettings.raid40Stats);

    ParseStatOverrideList("AutoBalance.StatModifier.PerInstance", sSettings.statOverrides);
    ParseStatOverrideList("AutoBalance.StatModifier.Boss.PerInstance", sSettings.statBossOverrides);
    ParseStatOverrideList("AutoBalance.StatModifier.PerCreature", sSettings.statCreatureOverrides);

    ParseCurveOverrideList();
    ParseBossCurveOverrideList();

    sSettings.forcedCreatureSizes.clear();
    ParseForcedCreatureIds(40, "AutoBalance.ForcedID40");
    ParseForcedCreatureIds(25, "AutoBalance.ForcedID25");
    ParseForcedCreatureIds(20, "AutoBalance.ForcedID20");
    ParseForcedCreatureIds(10, "AutoBalance.ForcedID10");
    ParseForcedCreatureIds(5, "AutoBalance.ForcedID5");
    ParseForcedCreatureIds(2, "AutoBalance.ForcedID2");

    ParseDisabledCreatureIds();

    sSettings.minHPModifier = std::max(0.0f, sConfigMgr->GetFloatDefault("AutoBalance.MinHPModifier", 0.01f));
    sSettings.minManaModifier = std::max(0.0f, sConfigMgr->GetFloatDefault("AutoBalance.MinManaModifier", 0.01f));
    sSettings.minDamageModifier = std::max(0.0f, sConfigMgr->GetFloatDefault("AutoBalance.MinDamageModifier", 0.01f));
    sSettings.minCCDurationModifier = std::max(0.0f, sConfigMgr->GetFloatDefault("AutoBalance.MinCCDurationModifier", 0.25f));
    sSettings.maxCCDurationModifier = std::max(0.0f, sConfigMgr->GetFloatDefault("AutoBalance.MaxCCDurationModifier", 1.0f));

    sSettings.playerChangeNotify = sConfigMgr->GetBoolDefault("AutoBalance.PlayerChangeNotify", true);

    ResetMapState();

    if (!reload)
        TC_LOG_INFO("module.AutoBalance", "AutoBalance: configuration loaded (enabled: %s)", sSettings.enableGlobal ? "true" : "false");
    else
        TC_LOG_INFO("module.AutoBalance", "AutoBalance: configuration reloaded (enabled: %s)", sSettings.enableGlobal ? "true" : "false");
}

bool IsEnabled()
{
    return sSettings.enableGlobal;
}

void NotifyPlayerEvent(Map* map)
{
    if (!map || !map->IsDungeon())
        return;

    InstanceMap* instance = map->ToInstanceMap();
    if (!instance)
        return;

    InstanceContext context = MakeContext(instance);
    if (!IsEnabledForContext(instance, context))
        return;

    uint32 effectivePlayers = GetEffectivePlayerCount(instance, context);
    float scale = GetMapScale(instance);

    MapState& state = sMapStates[MakeMapKey(map)];
    bool changed = state.lastEffectivePlayers != effectivePlayers || std::fabs(state.lastScale - scale) > 0.0001f;
    state.lastEffectivePlayers = effectivePlayers;
    state.lastScale = scale;

    if (!changed)
        return;

    if (sSettings.playerChangeNotify)
    {
        std::ostringstream message;
        message << "AutoBalance: instance difficulty adjusted for " << effectivePlayers << " player" << (effectivePlayers == 1 ? "" : "s")
                << " (multiplier " << std::fixed << std::setprecision(2) << scale << ").";

        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            if (Player* player = itr->GetSource())
                ChatHandler(player->GetSession()).PSendSysMessage("%s", message.str().c_str());
        }
    }

    TC_LOG_INFO("module.AutoBalance", "AutoBalance: map %u instance %u scaled for %u players (multiplier %.3f)", map->GetId(), instance->GetInstanceId(), effectivePlayers, scale);
}

void ModifyDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    if (!IsEnabled() || !damage)
        return;

    if (std::optional<float> modifier = GetPlayerToCreatureModifier(attacker, victim))
    {
        ApplyModifier(damage, *modifier);
        return;
    }

    if (std::optional<float> modifier = GetCreatureToPlayerModifier(attacker, victim))
    {
        ApplyModifier(damage, *modifier);
    }
}

void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage)
{
    ModifyDamage(attacker, target, damage);
}

void ModifyPeriodicDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    ModifyDamage(attacker, victim, damage);
}

void ModifySpellDamage(Unit* target, Unit* attacker, int32& damage)
{
    if (!IsEnabled() || damage <= 0)
        return;

    uint32 asPositive = static_cast<uint32>(damage);
    ModifyDamage(attacker, target, asPositive);
    damage = static_cast<int32>(asPositive);
}

float GetMapScale(InstanceMap* map, std::optional<uint32> forcedSize)
{
    if (!map)
        return 1.0f;

    InstanceContext context = MakeContext(map, forcedSize);
    if (!IsEnabledForContext(map, context))
        return 1.0f;

    uint32 effectivePlayers = GetEffectivePlayerCount(map, context);
    CurveConfig curve = ResolveCurveConfig(map, context, false);
    return ComputeDefaultMultiplier(map, context, curve, effectivePlayers);
}

uint32 GetEffectivePlayerCountForInstance(InstanceMap* map)
{
    if (!map)
        return 0;

    InstanceContext context = MakeContext(map);

    if (!IsEnabledForContext(map, context))
        return map->GetPlayersCountExceptGMs();

    return GetEffectivePlayerCount(map, context);
}

void SetPlayerDifficultyOffset(int32 offset)
{
    if (sSettings.playerDifficultyOffset == offset)
        return;

    sSettings.playerDifficultyOffset = offset;
    sMapStates.clear();

    TC_LOG_INFO("module.AutoBalance", "AutoBalance: player difficulty offset set to %d", offset);
}

int32 GetPlayerDifficultyOffset()
{
    return sSettings.playerDifficultyOffset;
}
}
