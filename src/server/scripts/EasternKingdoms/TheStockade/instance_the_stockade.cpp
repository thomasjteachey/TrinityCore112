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
#include "MapManager.h"
#include "Optional.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Util.h"
#include "WorldLocation.h"
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

bool IsPvPvEEnabled()
{
    return sConfigMgr->GetOption<bool>("PvPvEDungeon.Stockades.Enable", false);
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
                    RedirectTeamToFreshInstance(player, teamGuid);
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
        void RedirectTeamToFreshInstance(Player* player, ObjectGuid const& teamGuid)
        {
            if (!player || !teamGuid)
                return;

            uint32 redirectInstanceId = 0;
            if (auto const redirect = _teamRedirectInstanceIds.find(teamGuid); redirect != _teamRedirectInstanceIds.end())
            {
                redirectInstanceId = redirect->second;
            }
            else
            {
                redirectInstanceId = sMapMgr->GenerateInstanceId();
                _teamRedirectInstanceIds.emplace(teamGuid, redirectInstanceId);
            }

            if (!player->IsBeingTeleported())
            {
                WorldLocation const currentLocation = player->GetWorldLocation();
                player->TeleportTo(currentLocation, TELE_TO_NONE, redirectInstanceId);
            }
        }

        std::unordered_map<ObjectGuid, uint32, ObjectGuid::Hash> _teamSpawnAssignments;
        std::vector<uint32> _availableSpawnIndices;
        std::unordered_map<ObjectGuid, uint32, ObjectGuid::Hash> _teamRedirectInstanceIds;
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
