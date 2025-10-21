#ifndef TRINITY_ABALLMAPSCRIPT_H
#define TRINITY_ABALLMAPSCRIPT_H

#include "Player.h"
#include "ScriptMgr.h"

class AutoBalance_AllMapScript : public PlayerScript
{
public:
    AutoBalance_AllMapScript();

    void OnLogin(Player* player, bool firstLogin) override;
    void OnLogout(Player* player) override;
    void OnMapChanged(Player* player) override;
};

#endif // TRINITY_ABALLMAPSCRIPT_H
