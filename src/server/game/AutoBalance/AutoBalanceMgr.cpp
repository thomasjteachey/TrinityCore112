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
}

void AutoBalanceMgr::LoadConfigValues()
{
    _globalEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.Global", true);

    _dungeonNormalEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.5M", true);
    _dungeonHeroicEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.5MHeroic", true);
    _raidNormalEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.10M", true);
    _raidHeroicEnabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable.10MHeroic", true);

    _minPlayersNormal = uint32(std::max(1, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers", 1)));
    _minPlayersHeroic = uint32(std::max(1, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers.Heroic", 1)));
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
}

bool AutoBalanceMgr::IsEnabledFor(InstanceMap* map) const
{
    if (!_globalEnabled || !map)
        return false;

    if (map->IsRaid())
        return map->IsHeroic() ? _raidHeroicEnabled : _raidNormalEnabled;

    return map->IsHeroic() ? _dungeonHeroicEnabled : _dungeonNormalEnabled;
}

uint32 AutoBalanceMgr::GetMinPlayers(InstanceMap* map) const
{
    return map && map->IsHeroic() ? _minPlayersHeroic : _minPlayersNormal;
}

AutoBalanceMgr::InflectionSettings const& AutoBalanceMgr::GetInflection(InstanceMap* map, bool /*isBoss*/) const
{
    if (map->IsRaid())
        return map->IsHeroic() ? _raidHeroicInflection : _raidNormalInflection;

    return map->IsHeroic() ? _dungeonHeroicInflection : _dungeonNormalInflection;
}

AutoBalanceMgr::StatSettings const& AutoBalanceMgr::GetStats(InstanceMap* map, bool /*isBoss*/) const
{
    if (map->IsRaid())
        return map->IsHeroic() ? _raidHeroicStats : _raidNormalStats;

    return map->IsHeroic() ? _dungeonHeroicStats : _dungeonNormalStats;
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

    if (_notifyPlayerChanges && state.effectivePlayers != previous && state.effectivePlayers > 0)
    {
        state.lastAnnouncedPlayers = state.effectivePlayers;
        std::ostringstream ss;
        ss << "AutoBalance: Instance scaled for " << state.effectivePlayers << " player(s).";
        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
            if (Player* player = itr->GetSource())
                ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
    }
}

void AutoBalanceMgr::OnPlayerLeave(Map* map, Player* /*player*/)
{
    if (!map || !map->IsDungeon())
        return;

    MapState& state = _mapStates[map];
    uint32 previous = state.effectivePlayers;
    UpdateMapState(map, state);

    if (_notifyPlayerChanges && state.effectivePlayers != previous && state.effectivePlayers > 0)
    {
        state.lastAnnouncedPlayers = state.effectivePlayers;
        std::ostringstream ss;
        ss << "AutoBalance: Instance scaled for " << state.effectivePlayers << " player(s).";
        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
            if (Player* player = itr->GetSource())
                ChatHandler(player->GetSession()).SendSysMessage(ss.str().c_str());
    }
}

void AutoBalanceMgr::OnCreatureRemoved(Creature* creature)
{
    if (!creature)
        return;

    _creatureBaseData.erase(creature);
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

    MapState& state = _mapStates[map];
    if (state.effectivePlayers == 0)
        UpdateMapState(map, state);

    if (state.effectivePlayers == 0)
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
    }

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

    float scaledHealth = ClampMultiplier(float(base.baseHealth), healthMultiplier, _minHealthModifier);
    uint32 newHealth = uint32(std::max(0.0f, scaledHealth));
    if (newHealth == 0 && base.baseHealth > 0)
        newHealth = 1;

    creature->SetCreateHealth(newHealth);
    creature->SetMaxHealth(newHealth);
    creature->SetHealth(newHealth);

    if (base.baseMana > 0)
    {
        float scaledMana = ClampMultiplier(float(base.baseMana), manaMultiplier, _minManaModifier);
        uint32 newMana = uint32(std::max(0.0f, scaledMana));
        creature->SetCreateMana(newMana);
        creature->SetMaxPower(POWER_MANA, newMana);
        creature->SetPower(POWER_MANA, newMana);
    }

    float scaledArmor = base.baseArmor * armorMultiplier;
    if (scaledArmor < 0.0f)
        scaledArmor = 0.0f;
    creature->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, scaledArmor);

    auto applyWeapon = [&](WeaponAttackType attackType, float baseMin, float baseMax)
    {
        if (baseMin <= 0.0f && baseMax <= 0.0f)
            return;

        float newMin = ClampMultiplier(baseMin, damageMultiplier, _minDamageModifier);
        float newMax = ClampMultiplier(baseMax, damageMultiplier, _minDamageModifier);
        creature->SetBaseWeaponDamage(attackType, MINDAMAGE, newMin);
        creature->SetBaseWeaponDamage(attackType, MAXDAMAGE, newMax);
    };

    applyWeapon(BASE_ATTACK, base.baseMainMin, base.baseMainMax);
    applyWeapon(OFF_ATTACK, base.baseOffMin, base.baseOffMax);
    applyWeapon(RANGED_ATTACK, base.baseRangedMin, base.baseRangedMax);
}
