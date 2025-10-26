#include "AutoBalance/AutoBalanceMapData.h"
#include "AutoBalance/AutoBalanceConfig.h"
#include "GameTime.h"
#include "Map.h"
#include "Player.h"
#include <algorithm>

namespace AutoBalance
{
namespace
{
    Map::CustomData::AutoBalanceData& GetMapData(Map* map)
    {
        return map->GetCustomData().AutoBalance;
    }

    Map::CustomData::AutoBalanceData const& GetMapData(Map const* map)
    {
        return map->GetCustomData().AutoBalance;
    }

    uint32 GetMinimumPlayersForMap(Map const* map)
    {
        ModuleConfig const& config = GetConfig();

        auto const& overrides = map->IsHeroic() ? config.MinPlayersOverridesHeroic : config.MinPlayersOverridesNormal;
        uint32 const mapId = map->GetId();

        if (auto const itr = overrides.find(mapId); itr != overrides.end())
            return itr->second;

        return map->IsHeroic() ? config.MinimumPlayersHeroic : config.MinimumPlayers;
    }

    void UpdateEffectivePlayerCountInternal(Map* map, uint32 now)
    {
        auto& data = GetMapData(map);
        ModuleConfig const& config = GetConfig();

        data.QueueOffset = config.PlayerCountDifficultyOffset;

        uint32 playerCount = data.PlayerCount;
        if (map->IsDungeon())
            playerCount = std::max<uint32>(playerCount, 1u);

        uint32 effectivePlayerCount = playerCount;

        if (map->IsDungeon())
        {
            if (data.CombatLocked)
            {
                if (playerCount > data.CombatLockMinPlayers)
                {
                    data.CombatLockMinPlayers = playerCount;
                    data.CombatLockTripped = false;
                }
                else if (playerCount < data.CombatLockMinPlayers)
                {
                    data.CombatLockTripped = true;
                }

                effectivePlayerCount = std::max(playerCount, data.CombatLockMinPlayers);
            }
            else
            {
                data.CombatLockMinPlayers = 0;
                data.CombatLockTripped = false;
            }
        }
        else
        {
            data.CombatLockMinPlayers = 0;
            data.CombatLockTripped = false;
        }

        uint32 const minimumPlayers = GetMinimumPlayersForMap(map);
        if (effectivePlayerCount < minimumPlayers)
            effectivePlayerCount = minimumPlayers;

        int32 adjustedPlayerCount = static_cast<int32>(effectivePlayerCount) + data.QueueOffset;
        if (adjustedPlayerCount < 1)
            adjustedPlayerCount = 1;

        data.EffectivePlayerCount = static_cast<uint32>(adjustedPlayerCount);
        data.LastPlayerCountUpdateTimeMS = now;
    }
}

void HandleMapCreate(Map* map)
{
    if (!map)
        return;

    auto& data = GetMapData(map);
    data = Map::CustomData::AutoBalanceData{};

    uint32 const now = GameTime::GetGameTimeMS();
    UpdateEffectivePlayerCountInternal(map, now);
    data.LastCombatStateChangeTimeMS = now;
}

void HandleMapDestroy(Map* map)
{
    if (!map)
        return;

    GetMapData(map) = Map::CustomData::AutoBalanceData{};
}

void HandlePlayerEnter(Map* map, Player* /*player*/)
{
    if (!map)
        return;

    auto& data = GetMapData(map);
    uint32 const now = GameTime::GetGameTimeMS();

    ++data.PlayerCount;
    data.LastPlayerJoinTimeMS = now;
    UpdateEffectivePlayerCountInternal(map, now);
}

void HandlePlayerLeave(Map* map, Player* /*player*/)
{
    if (!map)
        return;

    auto& data = GetMapData(map);
    uint32 const now = GameTime::GetGameTimeMS();

    if (data.PlayerCount)
        --data.PlayerCount;

    data.LastPlayerLeaveTimeMS = now;
    UpdateEffectivePlayerCountInternal(map, now);
}

void HandleCombatStateChange(Map* map, bool locked, Player* /*player*/)
{
    if (!map)
        return;

    auto& data = GetMapData(map);
    if (data.CombatLocked == locked)
        return;

    uint32 const now = GameTime::GetGameTimeMS();

    data.CombatLocked = locked;
    data.CombatStateDirty = true;
    data.LastCombatStateChangeTimeMS = now;

    if (locked)
    {
        data.LastCombatStartTimeMS = now;

        if (map->IsDungeon())
        {
            uint32 const playerCount = std::max<uint32>(data.PlayerCount, 1u);
            if (playerCount > data.CombatLockMinPlayers)
                data.CombatLockMinPlayers = playerCount;

            data.CombatLockTripped = false;
        }
    }
    else
    {
        data.LastCombatEndTimeMS = now;
        data.CombatLockMinPlayers = 0;
        data.CombatLockTripped = false;
    }

    UpdateEffectivePlayerCountInternal(map, now);
}

void RefreshEffectivePlayerCount(Map* map)
{
    if (!map)
        return;

    UpdateEffectivePlayerCountInternal(map, GameTime::GetGameTimeMS());
}

uint32 GetActivePlayerCount(Map const* map)
{
    if (!map)
        return 0;

    return GetMapData(map).PlayerCount;
}

uint32 GetEffectivePlayerCount(Map const* map)
{
    if (!map)
        return 0;

    return GetMapData(map).EffectivePlayerCount;
}
}
