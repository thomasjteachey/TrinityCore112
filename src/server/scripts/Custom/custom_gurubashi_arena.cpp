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
#include "Bag.h"
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
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "TaskScheduler.h"
#include "TemporarySummon.h"
#include "Util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{
constexpr uint32 GURUBASHI_ARENA_MAP_ID = 0;
constexpr uint32 STRANGLETHORN_VALE_ZONE_ID = 33;
constexpr uint32 GURUBASHI_CATACOMBS_AREA_ID = 2177;
constexpr uint32 GURUBASHI_BATTLE_RING_AREA_ID = 30232;
constexpr uint32 GURUBASHI_CHEST_ENTRY = 179697;
constexpr uint32 SHADOW_SIGHT_ENTRY = 184663;
constexpr uint32 LEGIONNAIRE_MARK_OF_HONOR = 20558;
constexpr uint32 CHROMIE_ENTRY = 10667;
constexpr uint32 PVP_CONSUMABLE_ITEM_LIMIT_CATEGORY = 5;
constexpr uint32 TELEPORT_VISUAL_SPELL = 64446;
constexpr uint32 FORCED_DEATH_STARFIRE_SPELL_ID = 48465;
constexpr uint32 REQUIRED_PLAYER_COUNT = 5;
constexpr Seconds CHEST_DESPAWN_TIME = 15min;

namespace GurubashiShadowSight
{
constexpr uint32 Entry = 184663;
constexpr float PlayerClearance = 5.0f;
constexpr float TriggerRadius = 5.0f;
constexpr float MaxSpawnRadius = 30.0f;
constexpr uint8 SpawnAttempts = 128;
constexpr Seconds DespawnTime = 30s;
constexpr Seconds RespawnInterval = 30s;
}

constexpr std::chrono::milliseconds CHECK_INTERVAL = 1h;
char const* const GURUBASHI_EXIT_KILL_WHISPERS[] =
{
    "The only way out of the arena is death.",
    "One does not simply walk out of the Battle Ring.",
    "Coward.",
    "Enemy players impede the exit from the Battle Ring."
};

Position const ChestSpawnPosition = { -13205.281250f, 273.045685f, 20.550077f, 4.423725f };
char const* const GURUBASHI_REENTRY_RULE_WHISPER = "You died while the chest is active. No re-entry to the Battle Ring until the chest is looted or despawns.";
char const* const GURUBASHI_LATE_ENTRY_RULE_WHISPER = "You were not part of this chest battle. Entering the Battle Ring now is forbidden.";

void ClearChestDeathLockouts();
void ClearChestParticipants();
void StopGurubashiShadowSightSpawns();
void MarkChestParticipants(std::vector<ObjectGuid> const& participantGuids);
bool IsChestParticipant(ObjectGuid guid);

uint32 GetChestMarkRewardCount()
{
    time_t const now = GameTime::GetGameTime();
    tm localTime {};
#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    return localTime.tm_hour == 21 ? 3u : 1u;
}

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

enum class GurubashiAreaState
{
    Outside,
    BattleRing,
    NonRing
};

GurubashiAreaState GetGurubashiAreaState(Player const* player, uint32 zoneId, uint32 areaId)
{
    if (!player)
        return GurubashiAreaState::Outside;

    if (player->GetMapId() != GURUBASHI_ARENA_MAP_ID)
        return GurubashiAreaState::Outside;

    if (zoneId != STRANGLETHORN_VALE_ZONE_ID)
        return GurubashiAreaState::Outside;

    if (areaId == GURUBASHI_BATTLE_RING_AREA_ID)
        return GurubashiAreaState::BattleRing;

    if (areaId == GURUBASHI_CATACOMBS_AREA_ID)
        return GurubashiAreaState::NonRing;

    return GurubashiAreaState::NonRing;
}


void WhisperFromChromi(Player* player, std::string_view message)
{
    if (!player)
        return;

    ObjectGuid chromieGuid = ObjectGuid::Create<HighGuid::Player>(1);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_FOREIGN, LANG_UNIVERSAL, chromieGuid, player->GetGUID(), message, 0, "Chromie");
    player->SendDirectMessage(&data);
}

void WhisperRandomExitKillLineFromChromie(Player* player)
{
    if (!player)
        return;

    WhisperFromChromi(player, GURUBASHI_EXIT_KILL_WHISPERS[urand(0, 3)]);
}

void PlayForcedDeathStarfireVisual(Player* player)
{
    if (!player || !player->IsInWorld())
        return;

    SpellInfo const* starfireInfo = sSpellMgr->GetSpellInfo(FORCED_DEATH_STARFIRE_SPELL_ID);
    if (!starfireInfo || !starfireInfo->SpellVisual[0])
        return;

    SpellVisualEntry const* spellVisual = sSpellVisualStore.LookupEntry(starfireInfo->SpellVisual[0]);
    if (!spellVisual)
        return;

    uint32 const visualKit = spellVisual->TargetImpactKit ? spellVisual->TargetImpactKit :
        (spellVisual->ImpactKit ? spellVisual->ImpactKit : spellVisual->CastKit);
    if (!visualKit)
        return;

    player->SendPlaySpellVisual(visualKit);
}

bool HasLivingHostileInGurubashiBattleRing(Player const* player)
{
    if (!player)
        return false;

    std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
    for (auto const& playerPair : ObjectAccessor::GetPlayers())
    {
        Player* other = playerPair.second;
        if (!other || other == player || !other->IsAlive())
            continue;

        if (other->GetMapId() != player->GetMapId() || other->GetZoneId() != STRANGLETHORN_VALE_ZONE_ID)
            continue;

        if (GetGurubashiAreaState(other, other->GetZoneId(), other->GetAreaId()) != GurubashiAreaState::BattleRing)
            continue;

        // Stealthed/invisible players should not block non-lethal exits from the ring.
        if (other->HasStealthAura() || other->HasInvisibilityAura())
            continue;

        // Evaluate hostility without relying on transient FFA flags.
        // When a player steps out of the ring, FFA can drop before this check runs,
        // but the exit rule should still treat non-group/raid players in the ring as hostile.
        if (!player->IsInSameRaidWith(other))
            return true;
    }

    return false;
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

Position BuildRandomBattleRingPosition(Player* player)
{
    Position destination = ChestSpawnPosition;

    if (!player)
        return destination;

    constexpr float twoPi = 6.28318530718f;

    for (uint8 attempt = 0; attempt < 8; ++attempt)
    {
        float const angle = frand(0.0f, twoPi);
        float const radius = frand(3.0f, GurubashiShadowSight::MaxSpawnRadius);

        float const x = ChestSpawnPosition.GetPositionX() + std::cos(angle) * radius;
        float const y = ChestSpawnPosition.GetPositionY() + std::sin(angle) * radius;
        float const z = player->GetMap()->GetHeight(player->GetPhaseMask(), x, y, ChestSpawnPosition.GetPositionZ() + 6.0f);

        if (!std::isfinite(z))
            continue;

        destination.Relocate(x, y, z + 0.25f, frand(0.0f, twoPi));
        return destination;
    }

    return destination;
}

bool IsBattleRingPointClearOfPlayers(Map const* map, Position const& position)
{
    if (!map)
        return false;

    std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
    for (auto const& playerPair : ObjectAccessor::GetPlayers())
    {
        Player* player = playerPair.second;
        if (!player || !player->IsInWorld() || !player->IsAlive())
            continue;

        if (player->GetMap() != map || GetGurubashiAreaState(player, player->GetZoneId(), player->GetAreaId()) != GurubashiAreaState::BattleRing)
            continue;

        if (player->GetExactDist2d(position.GetPositionX(), position.GetPositionY()) < GurubashiShadowSight::PlayerClearance)
            return false;
    }

    return true;
}

bool BuildRandomShadowSightPosition(Player* summoner, Position& destination)
{
    if (!summoner || !summoner->GetMap())
        return false;

    constexpr float twoPi = 6.28318530718f;
    Map* map = summoner->GetMap();

    for (uint8 attempt = 0; attempt < GurubashiShadowSight::SpawnAttempts; ++attempt)
    {
        float const angle = frand(0.0f, twoPi);
        // sqrt keeps selection uniform across the circular battle-ring surface.
        float const radius = std::sqrt(frand(0.0f, 1.0f)) * GurubashiShadowSight::MaxSpawnRadius;
        float const x = ChestSpawnPosition.GetPositionX() + std::cos(angle) * radius;
        float const y = ChestSpawnPosition.GetPositionY() + std::sin(angle) * radius;
        float const z = map->GetHeight(summoner->GetPhaseMask(), x, y, ChestSpawnPosition.GetPositionZ() + 6.0f);

        if (!std::isfinite(z))
            continue;

        Position candidate(x, y, z + 0.25f, frand(0.0f, twoPi));
        if (!IsBattleRingPointClearOfPlayers(map, candidate))
            continue;

        destination = candidate;
        return true;
    }

    return false;
}

void TeleportStranglethornPlayersToBattleRing()
{
    std::vector<ObjectGuid> playersToTeleport;
    std::vector<ObjectGuid> teleportedPlayers;

    {
        std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
        for (auto const& playerPair : ObjectAccessor::GetPlayers())
        {
            Player* player = playerPair.second;
            if (!player || !player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive())
                continue;

            if (player->GetMapId() != GURUBASHI_ARENA_MAP_ID || player->GetZoneId() != STRANGLETHORN_VALE_ZONE_ID)
                continue;

            playersToTeleport.push_back(player->GetGUID());
        }
    }

    for (ObjectGuid const& guid : playersToTeleport)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive())
            continue;

        if (player->GetGroup())
            player->RemoveFromGroup();

        player->CastSpell(player, TELEPORT_VISUAL_SPELL, TRIGGERED_FULL_MASK);
        Position const destination = BuildRandomBattleRingPosition(player);
        player->NearTeleportTo(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), destination.GetOrientation(), true);
        player->SetFullHealth();
        for (Powers powerType = POWER_MANA; powerType < MAX_POWERS; powerType = Powers(powerType + 1))
            if (int32 maxPower = player->GetMaxPower(powerType); maxPower > 0)
                player->SetPower(powerType, maxPower);
        player->RemoveArenaSpellCooldowns(true);
        player->CastSpell(player, TELEPORT_VISUAL_SPELL, TRIGGERED_FULL_MASK);
        teleportedPlayers.push_back(guid);
    }

    MarkChestParticipants(teleportedPlayers);
}
bool IsChestGuidActiveInWorld(ObjectGuid chestGuid)
{
    if (!chestGuid)
        return false;

    bool found = false;
    sMapMgr->DoForAllMaps([&](Map* map)
    {
        if (found)
            return;

        if (GameObject* chest = map->GetGameObject(chestGuid))
            found = chest->IsInWorld();
    });

    return found;
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

            uint32 const rewardCount = GetChestMarkRewardCount();
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, LEGIONNAIRE_MARK_OF_HONOR, rewardCount) != EQUIP_ERR_OK)
            {
                player->SendEquipError(EQUIP_ERR_INVENTORY_FULL, nullptr, nullptr);
                me->SetLootState(GO_READY);
                return;
            }

            if (Item* item = player->StoreNewItem(dest, LEGIONNAIRE_MARK_OF_HONOR, true))
                player->SendNewItem(item, rewardCount, true, false);

            _rewardGranted = true;
            ClearChestDeathLockouts();
            ClearChestParticipants();
            StopGurubashiShadowSightSpawns();
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
        StopShadowSightSpawns();
        _currentChestGuid.Clear();
        _nextCheckTimeMs = 0;
        _lastEligibleCount = 0;
        _chestActive = false;
        ClearChestDeathLockouts();
        ClearChestParticipants();
    }

    void OnUpdate(uint32 diff) override
    {
        _scheduler.Update(diff);
        _shadowSightScheduler.Update(diff);
        DespawnTriggeredShadowSights();

        if (_chestActive && !IsChestGuidActiveInWorld(_currentChestGuid))
        {
            _currentChestGuid.Clear();
            _chestActive = false;
            StopShadowSightSpawns();
            ClearChestDeathLockouts();
            ClearChestParticipants();
        }
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

    bool IsChestActive() const
    {
        return _currentChestGuid && _chestActive;
    }

    void OnChestLooted()
    {
        _currentChestGuid.Clear();
        _chestActive = false;
        StopShadowSightSpawns();
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

    static bool IsEightPmServerTime()
    {
        time_t const now = GameTime::GetGameTime();
        tm localTime {};
#ifdef _WIN32
        localtime_s(&localTime, &now);
#else
        localtime_r(&now, &localTime);
#endif
        return localTime.tm_hour == 21;
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

        bool const bypassPlayerRequirement = !force && IsEightPmServerTime();
        if (!force && !bypassPlayerRequirement && playerCount < REQUIRED_PLAYER_COUNT)
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
            _chestActive = false;
            StopShadowSightSpawns();
            ClearChestDeathLockouts();
            ClearChestParticipants();
        }

        if (GameObject* chest = summoner->SummonGameObject(GURUBASHI_CHEST_ENTRY, ChestSpawnPosition, QuaternionData::fromEulerAnglesZYX(ChestSpawnPosition.GetOrientation(), 0.f, 0.f), CHEST_DESPAWN_TIME))
        {
            // Clear temporary ownership so all players in the arena can interact with the chest.
            summoner->RemoveGameObject(chest, false);
            chest->SetRespawnTime(0);
            _currentChestGuid = chest->GetGUID();
            _chestActive = true;
            ClearChestDeathLockouts();
            TeleportStranglethornPlayersToBattleRing();
            ScheduleShadowSightSpawns();
            YellFromChromie();
            return SpawnResult::Success;
        }

        return SpawnResult::MapNotAvailable;
    }

    void ScheduleShadowSightSpawns()
    {
        _shadowSightScheduler.CancelAll();
        _shadowSightScheduler.Schedule(GurubashiShadowSight::RespawnInterval, [this](TaskContext context)
        {
            if (!IsChestActive())
                return;

            AttemptShadowSightSpawn();
            context.Repeat(GurubashiShadowSight::RespawnInterval);
        });
    }

    void StopShadowSightSpawns()
    {
        _shadowSightScheduler.CancelAll();
        DespawnShadowSights();
    }

    void DespawnShadowSights()
    {
        if (_shadowSightGuids.empty())
            return;

        std::vector<ObjectGuid> shadowSightGuids = std::move(_shadowSightGuids);
        _shadowSightGuids.clear();

        sMapMgr->DoForAllMaps([&shadowSightGuids](Map* map)
        {
            for (ObjectGuid const& guid : shadowSightGuids)
                if (GameObject* shadowSight = map->GetGameObject(guid))
                    shadowSight->DespawnOrUnsummon();
        });
    }


    void DespawnPickedUpShadowSight(GameObject* shadowSight, Player* triggerPlayer)
    {
        if (!shadowSight || !shadowSight->IsInWorld())
            return;

        if (triggerPlayer)
            if (GameObjectTemplate const* goInfo = shadowSight->GetGOInfo())
                if (goInfo->type == GAMEOBJECT_TYPE_TRAP && goInfo->trap.spellId)
                    shadowSight->CastSpell(triggerPlayer, goInfo->trap.spellId);

        ObjectGuid const shadowSightGuid = shadowSight->GetGUID();
        shadowSight->DespawnOrUnsummon();
        _shadowSightGuids.erase(std::remove(_shadowSightGuids.begin(), _shadowSightGuids.end(), shadowSightGuid), _shadowSightGuids.end());
    }

    void DespawnTriggeredShadowSights()
    {
        if (!_chestActive)
            return;

        std::vector<ObjectGuid> playerGuids;
        {
            std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
            for (auto const& playerPair : ObjectAccessor::GetPlayers())
            {
                Player* player = playerPair.second;
                if (!IsPlayerEligible(player))
                    continue;

                playerGuids.push_back(player->GetGUID());
            }
        }

        for (ObjectGuid const& playerGuid : playerGuids)
        {
            Player* player = ObjectAccessor::FindPlayer(playerGuid);
            if (!IsPlayerEligible(player))
                continue;

            if (GameObject* shadowSight = player->FindNearestGameObject(GurubashiShadowSight::Entry, GurubashiShadowSight::TriggerRadius))
                DespawnPickedUpShadowSight(shadowSight, player);
        }

        PruneShadowSightGuids();
    }

    void PruneShadowSightGuids()
    {
        _shadowSightGuids.erase(std::remove_if(_shadowSightGuids.begin(), _shadowSightGuids.end(), [](ObjectGuid const& guid)
        {
            bool active = false;
            sMapMgr->DoForAllMaps([&](Map* map)
            {
                if (active)
                    return;

                if (GameObject* shadowSight = map->GetGameObject(guid))
                    active = shadowSight->IsInWorld();
            });
            return !active;
        }), _shadowSightGuids.end());
    }

    void AttemptShadowSightSpawn()
    {
        PruneShadowSightGuids();

        ObjectGuid summonerGuid;
        CountEligiblePlayers(&summonerGuid);
        Player* summoner = FindEligibleSummoner(summonerGuid);
        if (!summoner || !summoner->IsInWorld())
            return;

        Position shadowSightPosition;
        if (!BuildRandomShadowSightPosition(summoner, shadowSightPosition))
            return;

        DespawnShadowSights();

        if (GameObject* shadowSight = summoner->SummonGameObject(GurubashiShadowSight::Entry, shadowSightPosition, QuaternionData::fromEulerAnglesZYX(shadowSightPosition.GetOrientation(), 0.f, 0.f), GurubashiShadowSight::DespawnTime))
        {
            summoner->RemoveGameObject(shadowSight, false);
            shadowSight->SetRespawnTime(0);
            _shadowSightGuids.push_back(shadowSight->GetGUID());
        }
    }

    TaskScheduler _scheduler;
    TaskScheduler _shadowSightScheduler;
    ObjectGuid _currentChestGuid;
    uint32 _nextCheckTimeMs = 0;
    bool _chestActive = false;
    uint32 _lastEligibleCount = 0;
    std::vector<ObjectGuid> _shadowSightGuids;

    static gurubashi_arena_hourly_event* s_Instance;
};

gurubashi_arena_hourly_event* gurubashi_arena_hourly_event::s_Instance = nullptr;

namespace
{
void StopGurubashiShadowSightSpawns()
{
    if (gurubashi_arena_hourly_event* event = gurubashi_arena_hourly_event::GetInstance())
        event->OnChestLooted();
}
}

namespace
{
std::mutex g_GurubashiTrackedPlayersMutex;
std::unordered_set<ObjectGuid> g_GurubashiTrackedPlayers;
std::unordered_set<ObjectGuid> g_GurubashiChestDeathLockouts;
std::unordered_set<ObjectGuid> g_GurubashiChestParticipants;

bool IsChestDeathLockoutActive(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    return g_GurubashiChestDeathLockouts.find(guid) != g_GurubashiChestDeathLockouts.end();
}

void MarkChestDeathLockout(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    g_GurubashiChestDeathLockouts.insert(guid);
}

void ClearChestDeathLockouts()
{
    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    g_GurubashiChestDeathLockouts.clear();
}

void ClearChestParticipants()
{
    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    g_GurubashiChestParticipants.clear();
}

void MarkChestParticipants(std::vector<ObjectGuid> const& participantGuids)
{
    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    g_GurubashiChestParticipants.clear();

    for (ObjectGuid const& guid : participantGuids)
        g_GurubashiChestParticipants.insert(guid);
}

bool IsChestParticipant(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    return g_GurubashiChestParticipants.find(guid) != g_GurubashiChestParticipants.end();
}


bool ShouldTrackGurubashiPlayer(Player const* player)
{
    return player && player->IsInWorld() && player->GetMapId() == GURUBASHI_ARENA_MAP_ID && player->GetZoneId() == STRANGLETHORN_VALE_ZONE_ID;
}

bool HasBelowMaxCharges(Item const* item)
{
    if (!item)
        return false;

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate || itemTemplate->ItemLimitCategory != PVP_CONSUMABLE_ITEM_LIMIT_CATEGORY)
        return false;

    for (uint8 spellIndex = 0; spellIndex < MAX_ITEM_PROTO_SPELLS; ++spellIndex)
    {
        int32 const maxCharges = itemTemplate->Spells[spellIndex].SpellCharges;
        if (maxCharges == 0)
            continue;

        int32 const currentCharges = item->GetSpellCharges(spellIndex);
        bool const isBelowMaxCharges = (maxCharges > 0) ? (currentCharges < maxCharges) : (currentCharges > maxCharges);
        if (isBelowMaxCharges)
            return true;
    }

    return false;
}

bool RestoreItemCharges(Player* owner, uint8 bagSlot, uint8 slot)
{
    if (!owner)
        return false;

    Item* item = owner->GetItemByPos(bagSlot, slot);
    if (!HasBelowMaxCharges(item))
        return false;

    uint32 const itemEntry = item->GetEntry();
    int32 const randomPropertyId = item->GetItemRandomPropertyId();

    owner->DestroyItem(bagSlot, slot, true);

    ItemPosCountVec dest;
    if (owner->CanStoreNewItem(bagSlot, slot, dest, itemEntry, 1) != EQUIP_ERR_OK)
        return false;

    owner->StoreNewItem(dest, itemEntry, true, randomPropertyId);
    return true;
}

bool RestorePvpConsumableCharges(Player* player)
{
    if (!player)
        return false;

    std::vector<std::pair<uint8, uint8>> itemsToRestore;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (HasBelowMaxCharges(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            itemsToRestore.emplace_back(INVENTORY_SLOT_BAG_0, slot);

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (HasBelowMaxCharges(bag->GetItemByPos(slot)))
                    itemsToRestore.emplace_back(bagSlot, slot);

    bool restoredAny = false;
    for (auto const& [bagSlot, slot] : itemsToRestore)
        restoredAny = RestoreItemCharges(player, bagSlot, slot) || restoredAny;

    return restoredAny;
}

void UpdateGurubashiPlayerTracking(Player* player)
{
    if (!player)
        return;

    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    if (ShouldTrackGurubashiPlayer(player))
        g_GurubashiTrackedPlayers.insert(player->GetGUID());
    else
        g_GurubashiTrackedPlayers.erase(player->GetGUID());
}

void RemoveGurubashiPlayerTracking(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
    g_GurubashiTrackedPlayers.erase(guid);
}
}

class gurubashi_arena_exit_tracker : public PlayerScript
{
public:
    gurubashi_arena_exit_tracker() : PlayerScript("gurubashi_arena_exit_tracker") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        RefreshPvpConsumablesIfInStranglethorn(player);
        UpdateGurubashiPlayerTracking(player);
    }

    void OnMapChanged(Player* player) override
    {
        RefreshPvpConsumablesIfInStranglethorn(player);
        UpdateGurubashiPlayerTracking(player);
    }

    void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        RefreshPvpConsumablesIfInStranglethorn(player);
        UpdateGurubashiPlayerTracking(player);
    }

    void OnLogout(Player* player) override
    {
        if (player)
            RemoveGurubashiPlayerTracking(player->GetGUID());
    }

    void OnPVPKill(Player* /*killer*/, Player* killed) override
    {
        gurubashi_arena_hourly_event* event = gurubashi_arena_hourly_event::GetInstance();
        if (!killed || !event || !event->IsChestActive())
            return;

        GurubashiAreaState const areaState = GetGurubashiAreaState(killed, killed->GetZoneId(), killed->GetAreaId());
        if (areaState == GurubashiAreaState::BattleRing)
            MarkChestDeathLockout(killed->GetGUID());
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* killed) override
    {
        gurubashi_arena_hourly_event* event = gurubashi_arena_hourly_event::GetInstance();
        if (!killed || !event || !event->IsChestActive())
            return;

        GurubashiAreaState const areaState = GetGurubashiAreaState(killed, killed->GetZoneId(), killed->GetAreaId());
        if (areaState == GurubashiAreaState::BattleRing)
            MarkChestDeathLockout(killed->GetGUID());
    }

    void OnPlayerRepop(Player* player) override
    {
        gurubashi_arena_hourly_event* event = gurubashi_arena_hourly_event::GetInstance();
        if (!player || !event || !event->IsChestActive() || !IsChestDeathLockoutActive(player->GetGUID()))
            return;

        WhisperFromChromi(player, GURUBASHI_REENTRY_RULE_WHISPER);
    }

private:
    void RefreshPvpConsumablesIfInStranglethorn(Player* player)
    {
        if (!ShouldTrackGurubashiPlayer(player))
            return;

        if (RestorePvpConsumableCharges(player))
            WhisperFromChromi(player, "Your PvP consumable charges have been restored.");
    }
};

class gurubashi_arena_exit_enforcer : public WorldScript
{
public:
    gurubashi_arena_exit_enforcer() : WorldScript("gurubashi_arena_exit_enforcer") { }

    void OnUpdate(uint32 diff) override
    {
        _scanAccumulator += diff;
        if (_scanAccumulator < _scanIntervalMs)
            return;

        _scanAccumulator = 0;

        std::vector<ObjectGuid> trackedPlayers;
        {
            std::lock_guard<std::mutex> lock(g_GurubashiTrackedPlayersMutex);
            trackedPlayers.reserve(g_GurubashiTrackedPlayers.size());
            for (ObjectGuid const& guid : g_GurubashiTrackedPlayers)
                trackedPlayers.push_back(guid);
        }

        std::unordered_set<ObjectGuid> processedPlayers;
        processedPlayers.reserve(trackedPlayers.size());

        for (ObjectGuid const& guid : trackedPlayers)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player || !player->IsInWorld())
            {
                RemoveGurubashiPlayerTracking(guid);
                _trackedPlayers.erase(guid);
                continue;
            }

            if (!ShouldTrackGurubashiPlayer(player))
            {
                UpdateGurubashiPlayerTracking(player);
                _trackedPlayers.erase(guid);
                continue;
            }

            processedPlayers.insert(guid);

            TrackedState& tracked = _trackedPlayers[guid];
            Position const currentPosition = player->GetPosition();
            GurubashiAreaState const currentState = GetGurubashiAreaState(player, player->GetZoneId(), player->GetAreaId());

            gurubashi_arena_hourly_event* event = gurubashi_arena_hourly_event::GetInstance();
            bool const chestActive = event && event->IsChestActive();
            if (chestActive && IsChestDeathLockoutActive(guid) && currentState == GurubashiAreaState::BattleRing && player->IsAlive() && !player->IsGameMaster())
            {
                PlayForcedDeathStarfireVisual(player);
                Unit::Kill(player, player);
                WhisperFromChromi(player, GURUBASHI_REENTRY_RULE_WHISPER);
            }
            else if (chestActive && currentState == GurubashiAreaState::BattleRing && player->IsAlive() && !player->IsGameMaster() && !IsChestParticipant(guid))
            {
                PlayForcedDeathStarfireVisual(player);
                Unit::Kill(player, player);
                WhisperFromChromi(player, GURUBASHI_LATE_ENTRY_RULE_WHISPER);
                MarkChestDeathLockout(guid);
            }

            if (tracked.HasPosition)
            {
                float const distance2d = tracked.LastPosition.GetExactDist2d(currentPosition);
                bool const isTeleportTransition = player->IsBeingTeleported() || tracked.MapId != player->GetMapId() || distance2d > 45.0f;
                bool const crossedBattleRingBoundary =
                    tracked.AreaState == GurubashiAreaState::BattleRing && currentState == GurubashiAreaState::NonRing;

                if (crossedBattleRingBoundary && !isTeleportTransition && !player->IsGameMaster() && player->IsAlive() &&
                    HasLivingHostileInGurubashiBattleRing(player))
                {
                    PlayForcedDeathStarfireVisual(player);
                    Unit::Kill(player, player);
                    MarkChestDeathLockout(guid);

                    WhisperRandomExitKillLineFromChromie(player);
                }
            }

            tracked.AreaState = currentState;
            tracked.LastPosition = currentPosition;
            tracked.MapId = player->GetMapId();
            tracked.HasPosition = true;
        }

        for (auto itr = _trackedPlayers.begin(); itr != _trackedPlayers.end();)
            if (processedPlayers.find(itr->first) == processedPlayers.end())
                itr = _trackedPlayers.erase(itr);
            else
                ++itr;
    }

private:
    struct TrackedState
    {
        GurubashiAreaState AreaState = GurubashiAreaState::Outside;
        Position LastPosition;
        uint32 MapId = 0;
        bool HasPosition = false;
    };

    std::unordered_map<ObjectGuid, TrackedState> _trackedPlayers;
    uint32 _scanAccumulator = 0;
    static constexpr uint32 _scanIntervalMs = 250;
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
    new gurubashi_arena_exit_tracker();
    new gurubashi_arena_exit_enforcer();
    new gurubashi_arena_commands();
}
