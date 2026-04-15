#include "BattlegroundTV.h"

#include "Log.h"
#include "Player.h"

BattlegroundTV::BattlegroundTV()
{
    BgObjects.resize(BG_TV_OBJECT_MAX);
}

BattlegroundTV::~BattlegroundTV() = default;

void BattlegroundTV::StartingEventCloseDoors()
{
    for (uint32 i = BG_TV_OBJECT_DOOR_1; i <= BG_TV_OBJECT_DOOR_2; ++i)
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);

    for (uint32 i = BG_TV_OBJECT_BUFF_1; i <= BG_TV_OBJECT_BUFF_2; ++i)
        SpawnBGObject(i, RESPAWN_ONE_DAY);
}

void BattlegroundTV::StartingEventOpenDoors()
{
    for (uint32 i = BG_TV_OBJECT_DOOR_1; i <= BG_TV_OBJECT_DOOR_2; ++i)
        DoorOpen(i);

    for (uint32 i = BG_TV_OBJECT_BUFF_1; i <= BG_TV_OBJECT_BUFF_2; ++i)
        SpawnBGObject(i, 60);
}

void BattlegroundTV::HandleAreaTrigger(Player* /*player*/, uint32 trigger)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    switch (trigger)
    {
        case 4536:
        case 4537:
            break;
        default:
            break;
    }
}

void BattlegroundTV::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(0xE1A, 1); // 3610, show arena frame
    Arena::FillInitialWorldStates(packet);
}

bool BattlegroundTV::SetupBattleground()
{
    if (!AddObject(BG_TV_OBJECT_DOOR_1, BG_TV_OBJECT_TYPE_DOOR_1, -10774.6f, 430.992f, 24.41076f, 0.0156f, 0.0f, 0.0f, 0.00779992f, 0.99996958f, RESPAWN_IMMEDIATELY) ||
        !AddObject(BG_TV_OBJECT_DOOR_2, BG_TV_OBJECT_TYPE_DOOR_2, -10655.0f, 428.117f, 24.416f, 3.148f, 0.0f, 0.0f, 0.99999487f, -0.00320367f, RESPAWN_IMMEDIATELY) ||
        !AddObject(BG_TV_OBJECT_BUFF_1, BG_TV_OBJECT_TYPE_BUFF_1, -10717.63f, 383.8223f, 24.412825f, 1.555f, 0.0f, 0.0f, 0.70149994f, 0.71266951f, 120) ||
        !AddObject(BG_TV_OBJECT_BUFF_2, BG_TV_OBJECT_TYPE_BUFF_2, -10716.6f, 475.364f, 24.4131f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 120))
    {
        TC_LOG_ERROR("sql.sql", "BattlegroundTV: Failed to spawn some object!");
        return false;
    }

    return true;
}
