#pragma once

#include <cstdint>

class Map;
class Player;

namespace AutoBalance
{
    void HandleMapCreate(Map* map);
    void HandleMapDestroy(Map* map);
    void HandlePlayerEnter(Map* map, Player* player);
    void HandlePlayerLeave(Map* map, Player* player);
    void HandleCombatStateChange(Map* map, bool locked, Player* player);

    void RefreshEffectivePlayerCount(Map* map);

    uint32 GetActivePlayerCount(Map const* map);
    uint32 GetEffectivePlayerCount(Map const* map);
    uint8 GetHighestPlayerLevel(Map const* map);
}
