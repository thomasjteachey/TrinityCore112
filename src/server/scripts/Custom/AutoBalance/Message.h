#pragma once

class Map;
class Player;

namespace AutoBalance
{
    enum class MessageType
    {
        MapCreated,
        MapDestroyed,
        PlayerEntered,
        PlayerLeft,
        CombatStateChanged
    };

    struct Message
    {
        MessageType Type;
        Map* TargetMap;
        Player* TargetPlayer;
        bool CombatLocked;

        static Message MapCreated(Map* map);
        static Message MapDestroyed(Map* map);
        static Message PlayerEntered(Map* map, Player* player);
        static Message PlayerLeft(Map* map, Player* player);
        static Message CombatState(Map* map, bool locked, Player* player);
    };
}
