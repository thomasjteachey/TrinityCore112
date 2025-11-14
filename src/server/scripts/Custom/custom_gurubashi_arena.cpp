/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "GameObject.h"
#include "GameObjectAI.h"
#include "Item.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "TaskScheduler.h"

#include <chrono>
#include <cmath>
#include <shared_mutex>
#include <unordered_set>

namespace
{
using namespace std::chrono_literals;

constexpr uint32 GURUBASHI_ARENA_AREA_ID = 1599;
constexpr uint32 GURUBASHI_ARENA_MAP_ID = 0;
constexpr uint32 GURUBASHI_CHEST_ENTRY = 179697;
constexpr uint32 LEGIONNAIRE_MARK_OF_HONOR = 20558;
constexpr uint32 CHROMIE_ENTRY = 10667;
constexpr Seconds CHEST_DESPAWN_TIME = 15min;
constexpr auto CHECK_INTERVAL = 1h;
constexpr float ARENA_RADIUS = 45.0f;
constexpr float MAX_HEIGHT_DELTA = 20.0f;

Position const ChestSpawnPosition = { -13232.5f, 199.5f, 31.760f, 3.124f };

bool IsPlayerInArena(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsGameMaster())
        return false;

    if (player->GetMapId() != GURUBASHI_ARENA_MAP_ID)
        return false;

    if (player->IsBeingTeleported())
        return false;

    if (!player->IsAlive())
        return false;

    if (player->GetAreaId() == GURUBASHI_ARENA_AREA_ID)
        return true;

    if (player->GetDistance2d(ChestSpawnPosition) > ARENA_RADIUS)
        return false;

    return std::fabs(player->GetPositionZ() - ChestSpawnPosition.GetPositionZ()) <= MAX_HEIGHT_DELTA;
}

void YellFromChromie()
{
    static char const* const yellText = "The Gurubashi Arena chest has appeared!";

    std::unordered_set<Creature*> signaledChromies;

    sMapMgr->DoForAllMaps([&signaledChromies](Map* map)
    {
        for (auto const& spawnPair : map->GetCreatureBySpawnIdStore())
        {
            if (Creature* creature = spawnPair.second)
            {
                if (creature->GetEntry() == CHROMIE_ENTRY && creature->IsAlive() && signaledChromies.insert(creature).second)
                    creature->Yell(yellText, LANG_UNIVERSAL);
            }
        }
    });
}
}

class go_custom_gurubashi_hourly_chest : public GameObjectScript
{
public:
    go_custom_gurubashi_hourly_chest() : GameObjectScript("go_custom_gurubashi_hourly_chest") { }

    struct go_custom_gurubashi_hourly_chestAI : public GameObjectAI
    {
        go_custom_gurubashi_hourly_chestAI(GameObject* go) : GameObjectAI(go) { }

        void OnLootStateChanged(uint32 state, Unit* unit) override
        {
            if (state != GO_ACTIVATED)
                return;

            if (_rewardGranted)
                return;

            Player* player = unit ? unit->ToPlayer() : nullptr;
            if (!player)
                return;

            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, LEGIONNAIRE_MARK_OF_HONOR, 1) != EQUIP_ERR_OK)
            {
                player->SendEquipError(EQUIP_ERR_INVENTORY_FULL, nullptr, nullptr);
                me->SetLootState(GO_READY);
                return;
            }

            if (Item* item = player->StoreNewItem(dest, LEGIONNAIRE_MARK_OF_HONOR, true))
                player->SendNewItem(item, 1, true, false);

            _rewardGranted = true;
            me->DespawnOrUnsummon();
        }

    private:
        bool _rewardGranted = false;
    };

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new go_custom_gurubashi_hourly_chestAI(go);
    }
};

class gurubashi_arena_hourly_event : public WorldScript
{
public:
    gurubashi_arena_hourly_event() : WorldScript("gurubashi_arena_hourly_event") { }

    void OnStartup() override
    {
        ScheduleNextCheck(0s);
    }

    void OnShutdown() override
    {
        _scheduler.CancelAll();
        _currentChestGuid.Clear();
    }

    void OnUpdate(uint32 diff) override
    {
        _scheduler.Update(diff);
    }

private:
    void ScheduleNextCheck(std::chrono::milliseconds delay)
    {
        _scheduler.Schedule(delay, [this](TaskContext context)
        {
            AttemptSpawn();
            context.Repeat(CHECK_INTERVAL);
        });
    }

    void AttemptSpawn()
    {
        ObjectGuid summonerGuid;
        uint32 playerCount = 0;

        {
            std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
            for (auto const& playerPair : ObjectAccessor::GetPlayers())
            {
                Player* player = playerPair.second;
                if (!IsPlayerInArena(player))
                    continue;

                if (!summonerGuid)
                    summonerGuid = player->GetGUID();

                ++playerCount;
            }
        }

        if (playerCount < 5)
            return;

        if (!summonerGuid)
            return;

        Player* summoner = ObjectAccessor::FindPlayer(summonerGuid);
        if (!summoner || !summoner->IsInWorld())
            return;

        Map* map = summoner->GetMap();
        if (!map)
            return;

        if (GameObject* existing = _currentChestGuid ? map->GetGameObject(_currentChestGuid) : nullptr)
        {
            if (existing->IsInWorld())
                return;

            _currentChestGuid.Clear();
        }

        if (GameObject* chest = summoner->SummonGameObject(GURUBASHI_CHEST_ENTRY, ChestSpawnPosition, QuaternionData::fromEulerAnglesZYX(ChestSpawnPosition.GetOrientation(), 0.f, 0.f), CHEST_DESPAWN_TIME))
        {
            chest->SetRespawnTime(0);
            _currentChestGuid = chest->GetGUID();
            YellFromChromie();
        }
    }

    TaskScheduler _scheduler;
    ObjectGuid _currentChestGuid;
};

void AddSC_custom_gurubashi_arena()
{
    new go_custom_gurubashi_hourly_chest();
    new gurubashi_arena_hourly_event();
}
