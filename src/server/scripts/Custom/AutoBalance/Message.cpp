#include "Message.h"
#include "Map.h"
#include "Player.h"

namespace AutoBalance
{
    Message Message::MapCreated(Map* map)
    {
        return { MessageType::MapCreated, map, nullptr, false };
    }

    Message Message::MapDestroyed(Map* map)
    {
        return { MessageType::MapDestroyed, map, nullptr, false };
    }

    Message Message::PlayerEntered(Map* map, Player* player)
    {
        return { MessageType::PlayerEntered, map, player, false };
    }

    Message Message::PlayerLeft(Map* map, Player* player)
    {
        return { MessageType::PlayerLeft, map, player, false };
    }

    Message Message::CombatState(Map* map, bool locked, Player* player)
    {
        return { MessageType::CombatStateChanged, map, player, locked };
    }
}
