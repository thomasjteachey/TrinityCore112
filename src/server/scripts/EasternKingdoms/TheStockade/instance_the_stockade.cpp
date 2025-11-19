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

#include "Config.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Optional.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Util.h"
#include "the_stockade.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

DungeonEncounterData const encounters[] =
{
    { DATA_RANDOLPH_MOLOCH, {{ 1146 }} },
    { DATA_LORD_OVERHEAT, {{ 1145 }} },
    { DATA_HOGGER, {{ 1144 }} }
};

namespace
{
constexpr uint32 StockadesMapId = 34;
constexpr uint32 StockadesFailMapId = 0;
Position const StockadesFailTeleport = { -8762.38f, 848.01f, 86.3139f, 0.0f };

bool IsPvPvEEnabled()
{
    return sConfigMgr->GetOption<bool>("PvPvEDungeon.Stockades.Enable", false);
}

std::string const& GetSpawnsFullMessage()
{
    static std::string const message = sConfigMgr->GetOption<std::string>(
        "PvPvEDungeon.Stockades.SpawnsFullMessage",
        "The Stockades PvPvE dungeon is currently full. Please wait for the next run.");
    return message;
}

bool TryParseSpawnEntry(std::string_view entry, Position& out)
{
    std::string normalized(entry);
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::replace(normalized.begin(), normalized.end(), '|', ' ');

    std::vector<std::string_view> tokens = Trinity::Tokenize(normalized, ' ', false);
    if (tokens.size() < 3)
    {
        TC_LOG_WARN("server.loading", "PvPvEDungeon.Stockades.SpawnPoints entry '%s' is invalid (needs at least X Y Z).", normalized.c_str());
        return false;
    }

    auto const parseFloat = [](std::string_view token) -> Optional<float>
    {
        return Trinity::StringTo<float>(token);
    };

    Optional<float> x = parseFloat(tokens[0]);
    Optional<float> y = parseFloat(tokens[1]);
    Optional<float> z = parseFloat(tokens[2]);
    Optional<float> o = tokens.size() >= 4 ? parseFloat(tokens[3]) : Optional<float>(0.0f);

    if (!x || !y || !z || !o)
    {
        TC_LOG_WARN("server.loading", "PvPvEDungeon.Stockades.SpawnPoints entry '%s' could not be parsed into coordinates.", normalized.c_str());
        return false;
    }

    out.Relocate(*x, *y, *z, *o);
    return true;
}

std::vector<Position> const& GetConfiguredSpawnPoints()
{
    static std::vector<Position> const spawnPoints = []()
    {
        std::vector<Position> result;
        std::string const config = sConfigMgr->GetOption<std::string>("PvPvEDungeon.Stockades.SpawnPoints", "");
        if (config.empty())
            return result;

        for (std::string_view entry : Trinity::Tokenize(config, ';', false))
        {
            Position position;
            if (TryParseSpawnEntry(entry, position))
                result.push_back(position);
        }

        if (result.empty())
            TC_LOG_WARN("server.loading", "PvPvEDungeon.Stockades.SpawnPoints configured but no valid coordinates were parsed.");

        return result;
    }();

    return spawnPoints;
}

bool ShouldHandle(Player* player)
{
    return IsPvPvEEnabled() && player && !player->IsGameMaster() && player->GetMapId() == StockadesMapId;
}

ObjectGuid GetTeamIdentifier(Player* player)
{
    if (!player)
        return ObjectGuid::Empty;

    if (Group* group = player->GetGroup())
        return group->GetGUID();

    return player->GetGUID();
}

void NotifySpawnsFull(Player* player)
{
    if (!player)
        return;

    if (WorldSession* session = player->GetSession())
    {
        std::string const& message = GetSpawnsFullMessage();
        if (!message.empty())
            session->SendNotification("%s", message.c_str());
    }
}

void TeleportPlayerOut(Player* player)
{
    if (!player)
        return;

    if (!player->IsBeingTeleported())
        player->TeleportTo(StockadesFailMapId, StockadesFailTeleport.GetPositionX(), StockadesFailTeleport.GetPositionY(),
            StockadesFailTeleport.GetPositionZ(), StockadesFailTeleport.GetOrientation());
}
}

class instance_the_stockade : public InstanceMapScript
{ 
public:
    instance_the_stockade() : InstanceMapScript("instance_the_stockade", StockadesMapId) { }

    struct instance_the_stockade_InstanceMapScript : public InstanceScript
    {
        instance_the_stockade_InstanceMapScript(InstanceMap* map) : InstanceScript(map)
        {
            SetHeaders(DataHeader);
            SetBossNumber(EncounterCount);
            LoadDungeonEncounterData(encounters);
        }

        void Initialize() override
        {
            InstanceScript::Initialize();
            _teamSpawnAssignments.clear();
            _availableSpawnIndices.clear();

            if (!IsPvPvEEnabled())
                return;

            std::vector<Position> const& spawnPoints = GetConfiguredSpawnPoints();
            _availableSpawnIndices.reserve(spawnPoints.size());
            for (uint32 i = 0; i < spawnPoints.size(); ++i)
                _availableSpawnIndices.push_back(i);
        }

        void OnPlayerEnter(Player* player) override
        {
            InstanceScript::OnPlayerEnter(player);

            if (!ShouldHandle(player))
                return;

            if (std::vector<Position> const& spawnPoints = GetConfiguredSpawnPoints(); spawnPoints.empty())
                return;

            ObjectGuid const teamGuid = GetTeamIdentifier(player);
            if (!teamGuid)
                return;

            auto const teamAssignment = _teamSpawnAssignments.find(teamGuid);
            uint32 spawnIndex = 0;
            if (teamAssignment != _teamSpawnAssignments.end())
            {
                spawnIndex = teamAssignment->second;
            }
            else
            {
                if (_availableSpawnIndices.empty())
                {
                    NotifySpawnsFull(player);
                    TeleportPlayerOut(player);
                    return;
                }

                uint32 const randomSlotIndex = urand(0, _availableSpawnIndices.size() - 1);
                spawnIndex = _availableSpawnIndices[randomSlotIndex];
                _availableSpawnIndices.erase(_availableSpawnIndices.begin() + randomSlotIndex);
                _teamSpawnAssignments.emplace(teamGuid, spawnIndex);
            }

            if (spawnIndex < spawnPoints.size())
                player->NearTeleportTo(spawnPoints[spawnIndex]);
        }

    private:
        std::unordered_map<ObjectGuid, uint32, ObjectGuid::Hash> _teamSpawnAssignments;
        std::vector<uint32> _availableSpawnIndices;
    };

    InstanceScript* GetInstanceScript(InstanceMap* map) const override
    {
        return new instance_the_stockade_InstanceMapScript(map);
    }
};

void AddSC_instance_the_stockade()
{
    new instance_the_stockade();
}
