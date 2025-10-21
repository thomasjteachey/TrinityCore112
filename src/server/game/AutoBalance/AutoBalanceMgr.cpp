#include "AutoBalance/AutoBalanceMgr.h"

#include "Chat.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <vector>
#include <sstream>

namespace
{
float ClampMultiplier(float base, float multiplier, float minimum)
{
    if (base <= 0.0f)
        return 0.0f;

    float value = base * multiplier;
    float minValue = base * minimum;
    if (value < minValue)
        value = minValue;
    return value;
}

bool TryGetFloatValue(std::string const& key, float& value)
{
    std::string raw = sConfigMgr->GetStringDefault(key, "", true);
    if (raw.empty())
        return false;

    try
    {
        value = std::stof(raw);
        return true;
    }
    catch (std::exception const&)
    {
        TC_LOG_ERROR("server.loading", "Bad value defined for name {} in config file {}, going to use {} instead", key,
            sConfigMgr->GetFilename(), value);
        return false;
    }
}
}

AutoBalanceMgr* AutoBalanceMgr::instance()
{
    static AutoBalanceMgr instance;
    return &instance;
}

void AutoBalanceMgr::Initialize()
{
    if (_initialized)
        return;

    LoadConfigValues();
    _initialized = true;
}

void AutoBalanceMgr::Reload()
{
    LoadConfigValues();
    _mapStates.clear();
    _creatureBaseData.clear();
    _mapCreatures.clear();
}

void AutoBalanceMgr::LoadConfigValues()
{
    _raidNormalEnabledBySize.clear();
    _raidHeroicEnabledBySize.clear();
    _raidNormalInflectionBySize.clear();
    _raidHeroicInflectionBySize.clear();
    _raidNormalStatsBySize.clear();
    _raidHeroicStatsBySize.clear();

    _globalEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.Global", true);

    _dungeonNormalEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.5M", true);
    _dungeonHeroicEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.5MHeroic", true);
    _dungeonOtherNormalEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.OtherNormal", true);
    _dungeonOtherHeroicEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.OtherHeroic", true);

    auto loadRaidEnabled = [&](char const* key, uint32 size, bool heroic)
    {
        bool value = sConfigMgr->GetBoolDefault(key, true);
        if (heroic)
            _raidHeroicEnabledBySize[size] = value;
        else
            _raidNormalEnabledBySize[size] = value;
        return value;
    };

    _raidNormalEnabled = loadRaidEnabled("AutoBalance.Enable.10M", 10, false);
    loadRaidEnabled("AutoBalance.Enable.15M", 15, false);
    loadRaidEnabled("AutoBalance.Enable.20M", 20, false);
    loadRaidEnabled("AutoBalance.Enable.25M", 25, false);
    loadRaidEnabled("AutoBalance.Enable.40M", 40, false);

    _raidHeroicEnabled = loadRaidEnabled("AutoBalance.Enable.10MHeroic", 10, true);
    loadRaidEnabled("AutoBalance.Enable.25MHeroic", 25, true);

    _minPlayersNormal = uint32(std::max(1, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers", 1)));
    _minPlayersHeroic = uint32(std::max(1, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.Heroic", 1)));
    _minPlayersRaidNormal = uint32(std::max(1, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.Raid", _minPlayersNormal)));
    _minPlayersRaidHeroic = uint32(std::max(1, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.RaidHeroic", _minPlayersHeroic)));
    _difficultyOffset = sConfigMgr->GetIntDefault("AutoBalance.playerCountDifficultyOffset", 0);
    _notifyPlayerChanges = sConfigMgr->GetBoolDefault("AutoBalance.PlayerChangeNotify", true);

    _minHealthModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinHPModifier", 0.01f);
    _minManaModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinManaModifier", 0.01f);
    _minDamageModifier = sConfigMgr->GetFloatDefault("AutoBalance.MinDamageModifier", 0.01f);

    auto loadInflection = [&](char const* prefix, InflectionSettings& target)
    {
        target.value = sConfigMgr->GetFloatDefault(prefix, 0.5f);

        std::string floorKey = std::string(prefix) + ".CurveFloor";
        std::string ceilingKey = std::string(prefix) + ".CurveCeiling";
        std::string bossKey = std::string(prefix) + ".BossModifier";

        target.floor = sConfigMgr->GetFloatDefault(floorKey, 0.0f);
        target.ceiling = sConfigMgr->GetFloatDefault(ceilingKey, 1.0f);
        target.bossModifier = sConfigMgr->GetFloatDefault(bossKey, 1.0f);
    };

    loadInflection("AutoBalance.InflectionPoint", _dungeonNormalInflection);
    loadInflection("AutoBalance.InflectionPointHeroic", _dungeonHeroicInflection);
    loadInflection("AutoBalance.InflectionPointRaid", _raidNormalInflection);
    loadInflection("AutoBalance.InflectionPointRaidHeroic", _raidHeroicInflection);

    auto loadRaidInflectionOverride = [&](char const* prefix, uint32 size, bool heroic)
    {
        InflectionSettings settings = heroic ? _raidHeroicInflection : _raidNormalInflection;
        bool changed = false;

        float value = settings.value;
        if (TryGetFloatValue(prefix, value))
        {
            settings.value = value;
            changed = true;
        }

        std::string floorKey = std::string(prefix) + ".CurveFloor";
        float floorValue = settings.floor;
        if (TryGetFloatValue(floorKey, floorValue))
        {
            settings.floor = floorValue;
            changed = true;
        }

        std::string ceilingKey = std::string(prefix) + ".CurveCeiling";
        float ceilingValue = settings.ceiling;
        if (TryGetFloatValue(ceilingKey, ceilingValue))
        {
            settings.ceiling = ceilingValue;
            changed = true;
        }

        std::string bossKey = std::string(prefix) + ".BossModifier";
        float bossValue = settings.bossModifier;
        if (TryGetFloatValue(bossKey, bossValue))
        {
            settings.bossModifier = bossValue;
            changed = true;
        }

        if (!changed)
            return;

        if (heroic)
            _raidHeroicInflectionBySize[size] = settings;
        else
            _raidNormalInflectionBySize[size] = settings;
    };

    loadRaidInflectionOverride("AutoBalance.InflectionPointRaid10M", 10, false);
    loadRaidInflectionOverride("AutoBalance.InflectionPointRaid15M", 15, false);
    loadRaidInflectionOverride("AutoBalance.InflectionPointRaid20M", 20, false);
    loadRaidInflectionOverride("AutoBalance.InflectionPointRaid25M", 25, false);
    loadRaidInflectionOverride("AutoBalance.InflectionPointRaid40M", 40, false);
    loadRaidInflectionOverride("AutoBalance.InflectionPointRaid10MHeroic", 10, true);
    loadRaidInflectionOverride("AutoBalance.InflectionPointRaid25MHeroic", 25, true);

    auto loadStats = [&](char const* prefix, StatSettings& target)
    {
        target.global = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Global", 1.0f);
        target.health = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Health", 1.0f);
        target.mana = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Mana", 1.0f);
        target.armor = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Armor", 1.0f);
        target.damage = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Damage", 1.0f);

        target.bossGlobal = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Boss.Global", target.global);
        target.bossHealth = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Boss.Health", target.health);
        target.bossMana = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Boss.Mana", target.mana);
        target.bossArmor = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Boss.Armor", target.armor);
        target.bossDamage = sConfigMgr->GetFloatDefault(std::string(prefix) + ".Boss.Damage", target.damage);
    };

    loadStats("AutoBalance.StatModifier", _dungeonNormalStats);
    loadStats("AutoBalance.StatModifierHeroic", _dungeonHeroicStats);
    loadStats("AutoBalance.StatModifierRaid", _raidNormalStats);
    loadStats("AutoBalance.StatModifierRaidHeroic", _raidHeroicStats);

    auto loadRaidStatsOverride = [&](char const* prefix, uint32 size, bool heroic)
    {
        StatSettings settings = heroic ? _raidHeroicStats : _raidNormalStats;
        bool changed = false;

        auto apply = [&](std::string const& key, float& field)
        {
            float value = field;
            if (TryGetFloatValue(key, value))
            {
                field = value;
                changed = true;
            }
        };

        std::string base(prefix);
        apply(base + ".Global", settings.global);
        apply(base + ".Health", settings.health);
        apply(base + ".Mana", settings.mana);
        apply(base + ".Armor", settings.armor);
        apply(base + ".Damage", settings.damage);
        apply(base + ".Boss.Global", settings.bossGlobal);
        apply(base + ".Boss.Health", settings.bossHealth);
        apply(base + ".Boss.Mana", settings.bossMana);
        apply(base + ".Boss.Armor", settings.bossArmor);
        apply(base + ".Boss.Damage", settings.bossDamage);

        if (!changed)
            return;

        if (heroic)
            _raidHeroicStatsBySize[size] = settings;
        else
            _raidNormalStatsBySize[size] = settings;
    };

    loadRaidStatsOverride("AutoBalance.StatModifierRaid10M", 10, false);
    loadRaidStatsOverride("AutoBalance.StatModifierRaid15M", 15, false);
    loadRaidStatsOverride("AutoBalance.StatModifierRaid20M", 20, false);
    loadRaidStatsOverride("AutoBalance.StatModifierRaid25M", 25, false);
    loadRaidStatsOverride("AutoBalance.StatModifierRaid40M", 40, false);
    loadRaidStatsOverride("AutoBalance.StatModifierRaid10MHeroic", 10, true);
    loadRaidStatsOverride("AutoBalance.StatModifierRaid25MHeroic", 25, true);
}

bool AutoBalanceMgr::IsEnabledFor(InstanceMap* map) const
{
    if (!_globalEnabled || !map)
        return false;

    if (map->IsRaid())
    {
        uint32 size = GetRaidSizeKey(map);
        if (map->IsHeroic())
        {
            auto itr = _raidHeroicEnabledBySize.find(size);
            if (itr != _raidHeroicEnabledBySize.end())
                return itr->second;
            return _raidHeroicEnabled;
        }

        auto itr = _raidNormalEnabledBySize.find(size);
        if (itr != _raidNormalEnabledBySize.end())
            return itr->second;
        return _raidNormalEnabled;
    }

    uint32 maxPlayers = map->GetMaxPlayers();
    if (map->IsHeroic())
        return maxPlayers > 5 ? _dungeonOtherHeroicEnabled : _dungeonHeroicEnabled;

    return maxPlayers > 5 ? _dungeonOtherNormalEnabled : _dungeonNormalEnabled;
}

uint32 AutoBalanceMgr::GetMinPlayers(InstanceMap* map) const
{
    if (!map)
        return _minPlayersNormal;

    if (map->IsRaid())
        return map->IsHeroic() ? _minPlayersRaidHeroic : _minPlayersRaidNormal;

    return map->IsHeroic() ? _minPlayersHeroic : _minPlayersNormal;
}

AutoBalanceMgr::InflectionSettings const& AutoBalanceMgr::GetInflection(InstanceMap* map, bool /*isBoss*/) const
{
    if (map->IsRaid())
    {
        uint32 size = GetRaidSizeKey(map);
        if (map->IsHeroic())
        {
            auto itr = _raidHeroicInflectionBySize.find(size);
            if (itr != _raidHeroicInflectionBySize.end())
                return itr->second;
            return _raidHeroicInflection;
        }

        auto itr = _raidNormalInflectionBySize.find(size);
        if (itr != _raidNormalInflectionBySize.end())
            return itr->second;
        return _raidNormalInflection;
    }

    return map->IsHeroic() ? _dungeonHeroicInflection : _dungeonNormalInflection;
}

AutoBalanceMgr::StatSettings const& AutoBalanceMgr::GetStats(InstanceMap* map, bool /*isBoss*/) const
{
    if (map->IsRaid())
    {
        uint32 size = GetRaidSizeKey(map);
        if (map->IsHeroic())
        {
            auto itr = _raidHeroicStatsBySize.find(size);
            if (itr != _raidHeroicStatsBySize.end())
                return itr->second;
            return _raidHeroicStats;
        }

        auto itr = _raidNormalStatsBySize.find(size);
        if (itr != _raidNormalStatsBySize.end())
            return itr->second;
        return _raidNormalStats;
    }

    return map->IsHeroic() ? _dungeonHeroicStats : _dungeonNormalStats;
}

uint32 AutoBalanceMgr::GetRaidSizeKey(InstanceMap* map) const
{
    if (!map)
        return 0;

    uint32 maxPlayers = map->GetMaxPlayers();
    return maxPlayers > 0 ? maxPlayers : 0;
}

void AutoBalanceMgr::UpdateMapState(Map* map, MapState& state)
{
    state.playerCount = map->GetPlayersCountExceptGMs();

    InstanceMap* instanceMap = map->ToInstanceMap();
    if (!instanceMap || !IsEnabledFor(instanceMap))
    {
        state.effectivePlayers = 0;
        return;
    }

    int32 effectivePlayers = int32(state.playerCount) + _difficultyOffset;
    if (effectivePlayers < 1)
        effectivePlayers = 1;

    uint32 minPlayers = GetMinPlayers(instanceMap);
    if (uint32(effectivePlayers) < minPlayers)
        effectivePlayers = int32(minPlayers);

    uint32 maxPlayers = instanceMap->GetMaxPlayers();
    if (uint32(effectivePlayers) > maxPlayers)
        effectivePlayers = int32(maxPlayers);

    state.effectivePlayers = uint32(effectivePlayers);
}

void AutoBalanceMgr::OnPlayerEnter(Map* map, Player* /*player*/)
{
    if (!map || !map->IsDungeon())
        return;

    MapState& state = _mapStates[map];
    uint32 previous = state.effectivePlayers;
    UpdateMapState(map, state);

    if (state.effectivePlayers != previous)
    {
        if (state.effectivePlayers > 0)
            RescaleMapCreatures(map);

        if (_notifyPlayerChanges && state.effectivePlayers > 0)
        {
            state.lastAnnouncedPlayers = state.effectivePlayers;
            std::ostringstream ss;
            ss << "AutoBalance: Instance scaled for " << state.effectivePlayers << " player(s).";
            for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
                if (Player* player = itr->GetSource())
                    ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
        }
    }
}

void AutoBalanceMgr::RescaleMapCreatures(Map* map)
{
    auto itr = _mapCreatures.find(map);
    if (itr == _mapCreatures.end())
        return;

    std::vector<Creature*> creatures;
    creatures.reserve(itr->second.size());
    for (Creature* creature : itr->second)
        if (creature && creature->IsInWorld() && creature->GetMap() == map)
            creatures.push_back(creature);

    for (Creature* creature : creatures)
        ApplyScaling(creature);
}

void AutoBalanceMgr::OnPlayerLeave(Map* map, Player* /*player*/)
{
    if (!map || !map->IsDungeon())
        return;

    MapState& state = _mapStates[map];
    uint32 previous = state.effectivePlayers;
    UpdateMapState(map, state);

    if (state.effectivePlayers != previous)
    {
        if (state.effectivePlayers > 0)
            RescaleMapCreatures(map);

        if (_notifyPlayerChanges && state.effectivePlayers > 0)
        {
            state.lastAnnouncedPlayers = state.effectivePlayers;
            std::ostringstream ss;
            ss << "AutoBalance: Instance scaled for " << state.effectivePlayers << " player(s).";
            for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
                if (Player* player = itr->GetSource())
                    ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
        }
    }
}

void AutoBalanceMgr::OnCreatureRemoved(Creature* creature)
{
    if (!creature)
        return;

    _creatureBaseData.erase(creature);

    Map* map = creature->GetMap();
    if (!map)
        return;

    auto itr = _mapCreatures.find(map);
    if (itr != _mapCreatures.end())
    {
        itr->second.erase(creature);
        if (itr->second.empty())
            _mapCreatures.erase(itr);
    }
}

void AutoBalanceMgr::ApplyScaling(Creature* creature)
{
    if (!creature)
        return;

    Map* map = creature->GetMap();
    if (!map || !map->IsDungeon())
        return;

    InstanceMap* instanceMap = map->ToInstanceMap();
    if (!instanceMap || !IsEnabledFor(instanceMap))
        return;

    CreatureBaseData& base = _creatureBaseData[creature];
    if (!base.initialized)
    {
        base.baseHealth = creature->GetMaxHealth();
        base.baseMana = creature->GetMaxPower(POWER_MANA);
        base.baseArmor = float(creature->GetArmor());
        base.baseMainMin = creature->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE);
        base.baseMainMax = creature->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE);
        base.baseOffMin = creature->GetWeaponDamageRange(OFF_ATTACK, MINDAMAGE);
        base.baseOffMax = creature->GetWeaponDamageRange(OFF_ATTACK, MAXDAMAGE);
        base.baseRangedMin = creature->GetWeaponDamageRange(RANGED_ATTACK, MINDAMAGE);
        base.baseRangedMax = creature->GetWeaponDamageRange(RANGED_ATTACK, MAXDAMAGE);
        base.initialized = true;

        _mapCreatures[map].insert(creature);
    }

    MapState& state = _mapStates[map];
    if (state.effectivePlayers == 0)
        UpdateMapState(map, state);

    if (state.effectivePlayers == 0)
        return;

    uint32 maxPlayers = instanceMap->GetMaxPlayers();
    float adjustedPlayers = float(state.effectivePlayers);

    InflectionSettings inflection = GetInflection(instanceMap, creature->IsDungeonBoss() || creature->isWorldBoss());
    bool isBoss = creature->IsDungeonBoss() || creature->isWorldBoss();
    if (isBoss)
        inflection.value *= inflection.bossModifier;

    float diff = (float(maxPlayers) / 5.0f) * 1.5f;

    float rawCeiling = inflection.ceiling;
    float curveCeilingAdjustment = rawCeiling;
    float tanhMax = (tanh((float(maxPlayers) - inflection.value) / diff) + 1.0f) / 2.0f;
    if (tanhMax > 0.0f)
        curveCeilingAdjustment = rawCeiling / (tanhMax * (rawCeiling - inflection.floor) + inflection.floor);

    float tanhValue = (tanh((adjustedPlayers - inflection.value) / diff) + 1.0f) / 2.0f;
    float defaultMultiplier = tanhValue * (rawCeiling * curveCeilingAdjustment - inflection.floor) + inflection.floor;

    StatSettings const& stats = GetStats(instanceMap, isBoss);

    auto computeStatMultiplier = [&](float global, float statValue)
    {
        return defaultMultiplier * global * statValue;
    };

    float healthMultiplier = computeStatMultiplier(isBoss ? stats.bossGlobal : stats.global,
        isBoss ? stats.bossHealth : stats.health);
    float manaMultiplier = computeStatMultiplier(isBoss ? stats.bossGlobal : stats.global,
        isBoss ? stats.bossMana : stats.mana);
    float armorMultiplier = computeStatMultiplier(isBoss ? stats.bossGlobal : stats.global,
        isBoss ? stats.bossArmor : stats.armor);
    float damageMultiplier = computeStatMultiplier(isBoss ? stats.bossGlobal : stats.global,
        isBoss ? stats.bossDamage : stats.damage);

    uint32 previousPlayerDamageReq = creature->m_PlayerDamageReq;

    float scaledHealth = ClampMultiplier(float(base.baseHealth), healthMultiplier, _minHealthModifier);
    uint32 newHealth = uint32(std::max(0.0f, scaledHealth));
    if (newHealth == 0 && base.baseHealth > 0)
        newHealth = 1;

    uint32 previousMaxHealth = creature->GetMaxHealth();
    uint32 previousHealth = creature->GetHealth();
    float previousHealthPct = previousMaxHealth > 0 ? float(previousHealth) / float(previousMaxHealth) : 1.0f;

    creature->SetCreateHealth(newHealth);
    creature->SetMaxHealth(newHealth);
    creature->ResetPlayerDamageReq();

    uint32 scaledCurrentHealth = newHealth;
    if (previousMaxHealth > 0)
    {
        scaledCurrentHealth = uint32(std::round(previousHealthPct * float(newHealth)));
        if (scaledCurrentHealth == 0 && newHealth > 0)
            scaledCurrentHealth = 1;
    }

    creature->SetHealth(scaledCurrentHealth);

    if (base.baseMana > 0)
    {
        float scaledMana = ClampMultiplier(float(base.baseMana), manaMultiplier, _minManaModifier);
        uint32 newMana = uint32(std::max(0.0f, scaledMana));

        uint32 previousMaxMana = creature->GetMaxPower(POWER_MANA);
        uint32 previousMana = creature->GetPower(POWER_MANA);
        float previousManaPct = previousMaxMana > 0 ? float(previousMana) / float(previousMaxMana) : 1.0f;

        creature->SetCreateMana(newMana);
        creature->SetMaxPower(POWER_MANA, newMana);

        uint32 scaledCurrentMana = newMana;
        if (previousMaxMana > 0)
        {
            scaledCurrentMana = uint32(std::round(previousManaPct * float(newMana)));
            if (scaledCurrentMana == 0 && newMana > 0)
                scaledCurrentMana = 1;
        }

        creature->SetPower(POWER_MANA, scaledCurrentMana);
    }

    float scaledArmor = base.baseArmor * armorMultiplier;
    if (scaledArmor < 0.0f)
        scaledArmor = 0.0f;
    creature->SetArmor(int32(std::round(scaledArmor)));

    auto applyWeapon = [&](WeaponAttackType attackType, float baseMin, float baseMax)
    {
        if (baseMin <= 0.0f && baseMax <= 0.0f)
            return;

        float newMin = ClampMultiplier(baseMin, damageMultiplier, _minDamageModifier);
        float newMax = ClampMultiplier(baseMax, damageMultiplier, _minDamageModifier);
        creature->SetBaseWeaponDamage(attackType, MINDAMAGE, newMin);
        creature->SetBaseWeaponDamage(attackType, MAXDAMAGE, newMax);
        creature->UpdateDamagePhysical(attackType);
    };

    applyWeapon(BASE_ATTACK, base.baseMainMin, base.baseMainMax);
    applyWeapon(OFF_ATTACK, base.baseOffMin, base.baseOffMax);
    applyWeapon(RANGED_ATTACK, base.baseRangedMin, base.baseRangedMax);

    uint32 playerDamageRequired = creature->m_PlayerDamageReq;
    if (previousPlayerDamageReq == 0)
    {
        creature->LowerPlayerDamageReq(playerDamageRequired);
    }
    else if (previousMaxHealth > 0)
    {
        uint32 scaledPlayerDamageReq = uint32(std::round(float(previousPlayerDamageReq) * float(newHealth) / float(previousMaxHealth)));
        if (scaledPlayerDamageReq > playerDamageRequired)
            scaledPlayerDamageReq = playerDamageRequired;

        creature->LowerPlayerDamageReq(playerDamageRequired - scaledPlayerDamageReq);
    }
}
