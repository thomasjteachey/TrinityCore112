#ifndef __BATTLEGROUNDTV_H
#define __BATTLEGROUNDTV_H

#include "Arena.h"
#include "WorldStatePackets.h"

enum BattlegroundTVObjectTypes
{
    BG_TV_OBJECT_DOOR_1 = 0,
    BG_TV_OBJECT_DOOR_2 = 1,
    BG_TV_OBJECT_BUFF_1 = 2,
    BG_TV_OBJECT_BUFF_2 = 3,
    BG_TV_OBJECT_MAX    = 4
};

enum BattlegroundTVObjects
{
    BG_TV_OBJECT_TYPE_DOOR_1 = 213196,
    BG_TV_OBJECT_TYPE_DOOR_2 = 213197,
    BG_TV_OBJECT_TYPE_BUFF_1 = 184663,
    BG_TV_OBJECT_TYPE_BUFF_2 = 184664
};

class BattlegroundTV : public Arena
{
public:
    BattlegroundTV();
    ~BattlegroundTV() override;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    void HandleAreaTrigger(Player* player, uint32 trigger) override;
    bool SetupBattleground() override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
};

#endif
