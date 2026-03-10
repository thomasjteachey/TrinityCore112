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

#include "Chat.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GameTime.h"
#include "Item.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "TaskScheduler.h"
#include "Util.h"

#include <chrono>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

using namespace std::chrono_literals;

namespace
{
constexpr uint32 GURUBASHI_ARENA_MAP_ID = 0;
constexpr uint32 STRANGLETHORN_VALE_ZONE_ID = 33;
constexpr uint32 GURUBASHI_CHEST_ENTRY = 179697;
constexpr uint32 LEGIONNAIRE_MARK_OF_HONOR = 20558;
constexpr uint32 CHROMIE_ENTRY = 10667;
constexpr uint32 MOONFIRE_SPELL_ID = 8921;
constexpr uint32 GURUBASHI_EXIT_PUNISH_DAMAGE = 1000000;
constexpr uint32 REQUIRED_PLAYER_COUNT = 5;
constexpr Seconds CHEST_DESPAWN_TIME = 15min;
constexpr std::chrono::milliseconds CHECK_INTERVAL = 1h;
char const* const CHROMI_NAME = "Chromi";
char const* const GURUBASHI_EXIT_WHISPER = "The only way out of the arena is death.";

Position const ChestSpawnPosition = { -13204.609f, 272.2056f, 21.858f, 1.022f };

bool IsPlayerEligible(Player* player)
{
    if (!player || !player->IsInWorld())
        return false;

    if (player->GetMapId() != GURUBASHI_ARENA_MAP_ID)
        return false;

    if (player->IsBeingTeleported())
        return false;

    if (!player->IsAlive())
        return false;

    return player->GetZoneId() == STRANGLETHORN_VALE_ZONE_ID;
}

bool IsInGurubashiBattleRing(Player const* player, uint32 zoneId, uint32 areaId)
{
    if (!player || player->GetMapId() != GURUBASHI_ARENA_MAP_ID)
        return false;

    if (zoneId != STRANGLETHORN_VALE_ZONE_ID)
        return false;

    if (AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(areaId))
        return (areaEntry->Flags & AREA_FLAG_ARENA) != 0;

    return false;
}

void WhisperFromChromi(Player* player, std::string_view message)
{
    WorldPacket data;
    ObjectGuid chromiGuid = ObjectGuid::Create<HighGuid::Unit>(CHROMIE_ENTRY, 1);
    ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_WHISPER, LANG_UNIVERSAL, chromiGuid, player->GetGUID(), message,
        0, CHROMI_NAME, player->GetName());
    player->SendDirectMessage(&data);
}

uint32 CountEligiblePlayers(ObjectGuid* firstEligibleGuid = nullptr)
{
    uint32 playerCount = 0;

    std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
    for (auto const& playerPair : ObjectAccessor::GetPlayers())
    {
        Player* player = playerPair.second;
        if (!IsPlayerEligible(player))
            continue;

        if (firstEligibleGuid && !*firstEligibleGuid)
            *firstEligibleGuid = player->GetGUID();

        ++playerCount;
    }

    return playerCount;
}

Player* FindEligibleSummoner(ObjectGuid preferredGuid)
{
    if (preferredGuid)
        if (Player* player = ObjectAccessor::FindPlayer(preferredGuid))
            if (IsPlayerEligible(player))
                return player;

    std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
    for (auto const& playerPair : ObjectAccessor::GetPlayers())
    {
        Player* player = playerPair.second;
        if (IsPlayerEligible(player))
            return player;
    }

    return nullptr;
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

enum class SpawnResult
{
    Success,
    NotEnoughPlayers,
    NoEligiblePlayers,
    MapNotAvailable,
    ChestAlreadyActive
};

class gurubashi_arena_hourly_event : public WorldScript
{
public:
    gurubashi_arena_hourly_event() : WorldScript("gurubashi_arena_hourly_event")
    {
        s_Instance = this;
    }

    ~gurubashi_arena_hourly_event() override
    {
        if (s_Instance == this)
            s_Instance = nullptr;
    }

    void OnStartup() override
    {
        ScheduleNextCheck(CalculateDelayUntilNextHour(true));
    }

    void OnShutdown() override
    {
        _scheduler.CancelAll();
        _currentChestGuid.Clear();
        _nextCheckTimeMs = 0;
        _lastEligibleCount = 0;
    }

    void OnUpdate(uint32 diff) override
    {
        _scheduler.Update(diff);
    }

    static gurubashi_arena_hourly_event* GetInstance()
    {
        return s_Instance;
    }

    uint32 GetLastEligibleCount() const
    {
        return _lastEligibleCount;
    }

    std::chrono::milliseconds GetTimeUntilNextScan() const
    {
        if (!_nextCheckTimeMs)
            return std::chrono::milliseconds::zero();

        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (_nextCheckTimeMs <= nowMs)
            return std::chrono::milliseconds::zero();

        return std::chrono::milliseconds(_nextCheckTimeMs - nowMs);
    }

    SpawnResult ForceSpawn()
    {
        return AttemptSpawn(true);
    }

private:
    static std::chrono::milliseconds CalculateDelayUntilNextHour(bool allowImmediate)
    {
        time_t const now = GameTime::GetGameTime();
        uint32 const secondsIntoHour = uint32(now % HOUR);

        if (secondsIntoHour == 0)
            return allowImmediate ? std::chrono::milliseconds::zero() : CHECK_INTERVAL;

        uint32 const secondsUntilNextHour = HOUR - secondsIntoHour;
        return std::chrono::milliseconds(secondsUntilNextHour * IN_MILLISECONDS);
    }

    void ScheduleNextCheck(std::chrono::milliseconds delay)
    {
        _nextCheckTimeMs = GameTime::GetGameTimeMS() + static_cast<uint32>(delay.count());
        _scheduler.Schedule(delay, [this](TaskContext /*context*/)
        {
            AttemptSpawn(false);
            ScheduleNextCheck(CalculateDelayUntilNextHour(false));
        });
    }

    SpawnResult AttemptSpawn(bool force)
    {
        ObjectGuid summonerGuid;
        uint32 const playerCount = CountEligiblePlayers(&summonerGuid);
        _lastEligibleCount = playerCount;

        if (!force && playerCount < REQUIRED_PLAYER_COUNT)
            return SpawnResult::NotEnoughPlayers;

        Player* summoner = FindEligibleSummoner(summonerGuid);
        if (!summoner || !summoner->IsInWorld())
            return SpawnResult::NoEligiblePlayers;

        Map* map = summoner->GetMap();
        if (!map)
            return SpawnResult::MapNotAvailable;

        if (GameObject* existing = _currentChestGuid ? map->GetGameObject(_currentChestGuid) : nullptr)
        {
            if (existing->IsInWorld())
                return SpawnResult::ChestAlreadyActive;

            _currentChestGuid.Clear();
        }

        if (GameObject* chest = summoner->SummonGameObject(GURUBASHI_CHEST_ENTRY, ChestSpawnPosition, QuaternionData::fromEulerAnglesZYX(ChestSpawnPosition.GetOrientation(), 0.f, 0.f), CHEST_DESPAWN_TIME))
        {
            // Clear temporary ownership so all players in the arena can interact with the chest.
            summoner->RemoveGameObject(chest, false);
            chest->SetRespawnTime(0);
            _currentChestGuid = chest->GetGUID();
            YellFromChromie();
            return SpawnResult::Success;
        }

        return SpawnResult::MapNotAvailable;
    }

    TaskScheduler _scheduler;
    ObjectGuid _currentChestGuid;
    uint32 _nextCheckTimeMs = 0;
    uint32 _lastEligibleCount = 0;

    static gurubashi_arena_hourly_event* s_Instance;
};

gurubashi_arena_hourly_event* gurubashi_arena_hourly_event::s_Instance = nullptr;


class gurubashi_arena_exit_enforcer : public PlayerScript
{
public:
    gurubashi_arena_exit_enforcer() : PlayerScript("gurubashi_arena_exit_enforcer") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        UpdateArenaState(player);
    }

    void OnLogout(Player* player) override
    {
        _playerInBattleRing.erase(player->GetGUID());
    }

    void OnMapChanged(Player* player) override
    {
        UpdateArenaState(player);
    }

    void OnUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
    {
        if (!player || !player->IsAlive() || player->IsBeingTeleported() || player->IsGameMaster())
        {
            UpdateArenaState(player);
            return;
        }

        ObjectGuid const guid = player->GetGUID();
        bool const wasInBattleRing = _playerInBattleRing[guid];
        bool const isInBattleRing = IsInGurubashiBattleRing(player, newZone, newArea);

        if (wasInBattleRing && !isInBattleRing)
        {
            player->CastSpell(player, MOONFIRE_SPELL_ID, TRIGGERED_FULL_MASK);
            Unit::DealDamage(player, player, GURUBASHI_EXIT_PUNISH_DAMAGE, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NATURE, nullptr, false);
            WhisperFromChromi(player, GURUBASHI_EXIT_WHISPER);
        }

        _playerInBattleRing[guid] = isInBattleRing;
    }

private:
    void UpdateArenaState(Player* player)
    {
        if (!player)
            return;

        _playerInBattleRing[player->GetGUID()] = IsInGurubashiBattleRing(player, player->GetZoneId(), player->GetAreaId());
    }

    std::unordered_map<ObjectGuid, bool> _playerInBattleRing;
};

namespace
{
using namespace Trinity::ChatCommands;

std::string FormatDuration(std::chrono::milliseconds duration)
{
    if (duration <= std::chrono::milliseconds::zero())
        return "0s";

    uint64 seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    if (duration.count() % 1000)
        ++seconds;

    return secsToTimeString(seconds, TimeFormat::ShortText, false);
}

class gurubashi_arena_commands : public CommandScript
{
public:
    gurubashi_arena_commands() : CommandScript("gurubashi_arena_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable gurubashiCommandTable =
        {
            { "status", HandleStatus, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "scan",   HandleScan,   rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "force",  HandleForce,  rbac::RBAC_PERM_COMMAND_GM, Console::Yes }
        };

        static ChatCommandTable rootTable =
        {
            { "gurubashi", gurubashiCommandTable }
        };

        return rootTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        if (gurubashi_arena_hourly_event* event = gurubashi_arena_hourly_event::GetInstance())
        {
            handler->PSendSysMessage("Last eligible players counted: %u", event->GetLastEligibleCount());
            handler->PSendSysMessage("Next automatic scan in: %s", FormatDuration(event->GetTimeUntilNextScan()).c_str());
            return true;
        }

        handler->SendSysMessage("Gurubashi arena event script is not initialized.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    static bool HandleScan(ChatHandler* handler)
    {
        uint32 const count = CountEligiblePlayers();
        handler->PSendSysMessage("Currently %u eligible players are in Stranglethorn Vale.", count);
        return true;
    }

    static bool HandleForce(ChatHandler* handler)
    {
        gurubashi_arena_hourly_event* event = gurubashi_arena_hourly_event::GetInstance();
        if (!event)
        {
            handler->SendSysMessage("Gurubashi arena event script is not initialized.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        switch (event->ForceSpawn())
        {
            case SpawnResult::Success:
                handler->SendSysMessage("Gurubashi chest spawned.");
                return true;
            case SpawnResult::ChestAlreadyActive:
                handler->SendSysMessage("A Gurubashi chest is already active.");
                return false;
            case SpawnResult::NoEligiblePlayers:
                handler->SendSysMessage("No eligible players are currently in Stranglethorn Vale to anchor the spawn.");
                handler->SetSentErrorMessage(true);
                return false;
            case SpawnResult::MapNotAvailable:
                handler->SendSysMessage("Unable to access the map to spawn the chest.");
                handler->SetSentErrorMessage(true);
                return false;
            case SpawnResult::NotEnoughPlayers:
            default:
                handler->SendSysMessage("Force spawn failed for an unknown reason.");
                handler->SetSentErrorMessage(true);
                return false;
        }
    }
};
}

void AddSC_custom_gurubashi_arena()
{
    new go_custom_gurubashi_hourly_chest();
    new gurubashi_arena_hourly_event();
    new gurubashi_arena_exit_enforcer();
    new gurubashi_arena_commands();
}
