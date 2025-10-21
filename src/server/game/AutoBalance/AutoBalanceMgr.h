#pragma once

#include "Define.h"

#include <unordered_map>

class Creature;
class InstanceMap;
class Map;
class Player;

class AutoBalanceMgr
{
public:
    static AutoBalanceMgr* instance();

    void Initialize();
    void Reload();

    void OnPlayerEnter(Map* map, Player* player);
    void OnPlayerLeave(Map* map, Player* player);
    void OnCreatureRemoved(Creature* creature);

    void ApplyScaling(Creature* creature);

private:
    struct InflectionSettings
    {
        float value = 0.5f;
        float floor = 0.0f;
        float ceiling = 1.0f;
        float bossModifier = 1.0f;
    };

    struct StatSettings
    {
        float global = 1.0f;
        float health = 1.0f;
        float mana = 1.0f;
        float armor = 1.0f;
        float damage = 1.0f;
        float bossGlobal = 1.0f;
        float bossHealth = 1.0f;
        float bossMana = 1.0f;
        float bossArmor = 1.0f;
        float bossDamage = 1.0f;
    };

    struct MapState
    {
        uint32 playerCount = 0;
        uint32 effectivePlayers = 0;
        uint32 lastAnnouncedPlayers = 0;
    };

    struct CreatureBaseData
    {
        bool initialized = false;
        uint32 baseHealth = 0;
        uint32 baseMana = 0;
        float baseArmor = 0.0f;
        float baseMainMin = 0.0f;
        float baseMainMax = 0.0f;
        float baseOffMin = 0.0f;
        float baseOffMax = 0.0f;
        float baseRangedMin = 0.0f;
        float baseRangedMax = 0.0f;
    };

    void LoadConfigValues();
    void UpdateMapState(Map* map, MapState& state);
    bool IsEnabledFor(InstanceMap* map) const;
    uint32 GetMinPlayers(InstanceMap* map) const;
    InflectionSettings const& GetInflection(InstanceMap* map, bool isBoss) const;
    StatSettings const& GetStats(InstanceMap* map, bool isBoss) const;

    bool _initialized = false;
    bool _globalEnabled = true;
    bool _dungeonNormalEnabled = true;
    bool _dungeonHeroicEnabled = true;
    bool _raidNormalEnabled = true;
    bool _raidHeroicEnabled = true;
    uint32 _minPlayersNormal = 1;
    uint32 _minPlayersHeroic = 1;
    int32 _difficultyOffset = 0;
    bool _notifyPlayerChanges = true;

    InflectionSettings _dungeonNormalInflection;
    InflectionSettings _dungeonHeroicInflection;
    InflectionSettings _raidNormalInflection;
    InflectionSettings _raidHeroicInflection;

    StatSettings _dungeonNormalStats;
    StatSettings _dungeonHeroicStats;
    StatSettings _raidNormalStats;
    StatSettings _raidHeroicStats;

    float _minHealthModifier = 0.01f;
    float _minManaModifier = 0.01f;
    float _minDamageModifier = 0.01f;

    std::unordered_map<Map const*, MapState> _mapStates;
    std::unordered_map<Creature const*, CreatureBaseData> _creatureBaseData;
};

#define sAutoBalanceMgr AutoBalanceMgr::instance()
