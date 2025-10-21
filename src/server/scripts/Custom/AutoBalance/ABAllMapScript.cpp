#include "ABAllMapScript.h"

#include "AutoBalanceMgr.h"
#include "Map.h"

namespace
{
    void NotifyMap(Map* map)
    {
        if (!map)
            return;

        AutoBalance::NotifyPlayerEvent(map);
    }
}

AutoBalance_AllMapScript::AutoBalance_AllMapScript() : PlayerScript("AutoBalance_AllMapScript") { }

void AutoBalance_AllMapScript::OnLogin(Player* player, bool /*firstLogin*/)
{
    NotifyMap(player ? player->GetMap() : nullptr);
}

void AutoBalance_AllMapScript::OnLogout(Player* player)
{
    NotifyMap(player ? player->GetMap() : nullptr);
}

void AutoBalance_AllMapScript::OnMapChanged(Player* player)
{
    NotifyMap(player ? player->GetMap() : nullptr);
}
