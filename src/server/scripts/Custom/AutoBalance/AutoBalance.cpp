#include "AutoBalance.h"
#include "Map.h"
#include "Player.h"
#include <algorithm>

ABScriptMgr& ABScriptMgr::Instance()
{
    static ABScriptMgr instance;
    return instance;
}

void ABScriptMgr::RegisterModuleScript(ABModuleScript* script)
{
    if (!script)
        return;

    auto itr = std::find(_moduleScripts.begin(), _moduleScripts.end(), script);
    if (itr == _moduleScripts.end())
        _moduleScripts.push_back(script);
}

void ABScriptMgr::Dispatch(AutoBalance::Message const& message)
{
    for (ABModuleScript* script : _moduleScripts)
        script->OnMessage(message);
}

void ABScriptMgr::OnMapCreate(Map* map)
{
    if (!map)
        return;

    _combatStateByMap[map] = false;
    Dispatch(AutoBalance::Message::MapCreated(map));
}

void ABScriptMgr::OnMapDestroy(Map* map)
{
    if (!map)
        return;

    _combatStateByMap.erase(map);
    Dispatch(AutoBalance::Message::MapDestroyed(map));
}

void ABScriptMgr::OnPlayerEnterMap(Map* map, Player* player)
{
    if (!map)
        return;

    Dispatch(AutoBalance::Message::PlayerEntered(map, player));
}

void ABScriptMgr::OnPlayerLeaveMap(Map* map, Player* player)
{
    if (!map)
        return;

    Dispatch(AutoBalance::Message::PlayerLeft(map, player));
}

void ABScriptMgr::OnCombatStateChanged(Map* map, bool locked, Player* player)
{
    if (!map)
        return;

    auto result = _combatStateByMap.emplace(map, locked);
    if (!result.second && result.first->second == locked)
        return;

    result.first->second = locked;
    Dispatch(AutoBalance::Message::CombatState(map, locked, player));
}

ABModuleScript::ABModuleScript(char const* name) : _name(name)
{
    ABScriptMgr::Instance().RegisterModuleScript(this);
}

void ABModuleScript::OnMessage(AutoBalance::Message const& /*message*/)
{
}
