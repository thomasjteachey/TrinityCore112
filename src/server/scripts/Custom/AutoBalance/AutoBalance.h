#pragma once

#include <unordered_map>
#include <vector>

#include "AutoBalance/AutoBalanceConfig.h"

#include "Message.h"

class Map;
class Player;

class ABModuleScript;

class ABScriptMgr
{
public:
    static ABScriptMgr& Instance();

    void RegisterModuleScript(ABModuleScript* script);

    void OnMapCreate(Map* map);
    void OnMapDestroy(Map* map);
    void OnPlayerEnterMap(Map* map, Player* player);
    void OnPlayerLeaveMap(Map* map, Player* player);
    void OnCombatStateChanged(Map* map, bool locked, Player* player = nullptr);

private:
    ABScriptMgr() = default;

    void Dispatch(AutoBalance::Message const& message);

    std::vector<ABModuleScript*> _moduleScripts;
    std::unordered_map<Map const*, bool> _combatStateByMap;
};

class ABModuleScript
{
public:
    explicit ABModuleScript(char const* name);
    virtual ~ABModuleScript() = default;

    virtual void OnMessage(AutoBalance::Message const& message);

    char const* GetName() const { return _name; }

private:
    char const* _name;
};
