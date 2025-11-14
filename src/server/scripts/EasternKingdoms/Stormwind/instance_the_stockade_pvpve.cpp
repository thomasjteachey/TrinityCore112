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

#include "ScriptMgr.h"

#include "Configuration/Config.h"
#include "Duration.h"
#include "GameObject.h"
#include "Map.h"
#include "InstanceScript.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Position.h"
#include "SharedDefines.h"
#include "StringFormat.h"

#include "../../Custom/mod_pvpve_dungeon.h"

#include <string>

namespace StockadesPvPvE
{
constexpr uint32 StockadesMapId = 34;

namespace
{
constexpr uint32 DefaultChestDespawnSeconds = 300;
Position const DefaultChestPosition = { 71.879f, -15.478f, -20.215f, 0.0f };

uint32 s_ChestGameObjectId = 0;
Seconds s_ChestDespawn = Seconds(DefaultChestDespawnSeconds);
Position s_ChestPosition = DefaultChestPosition;

void LoadConfig()
{
    int32 const configuredEntry = sConfigMgr->GetIntDefault("StockadesPvPvE.ChestGameObjectId", 0);
    int32 const configuredDespawn = sConfigMgr->GetIntDefault("StockadesPvPvE.ChestDespawnSeconds", int32(DefaultChestDespawnSeconds));
    float const configuredX = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnX", DefaultChestPosition.GetPositionX());
    float const configuredY = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnY", DefaultChestPosition.GetPositionY());
    float const configuredZ = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnZ", DefaultChestPosition.GetPositionZ());
    float const configuredO = sConfigMgr->GetFloatDefault("StockadesPvPvE.ChestSpawnO", DefaultChestPosition.GetOrientation());

    s_ChestGameObjectId = configuredEntry > 0 ? uint32(configuredEntry) : 0u;
    s_ChestDespawn = Seconds(configuredDespawn >= 0 ? uint32(configuredDespawn) : DefaultChestDespawnSeconds);
    s_ChestPosition.Relocate(configuredX, configuredY, configuredZ, configuredO);

    if (!s_ChestGameObjectId)
        TC_LOG_WARN("server.custom", "Stockades PvPvE: reward chest entry is 0; chest spawning is disabled.");
}

QuaternionData GetChestRotation()
{
    return QuaternionData::fromEulerAnglesZYX(s_ChestPosition.GetOrientation(), 0.0f, 0.0f);
}
}

void EnsureConfigLoaded()
{
    static bool s_ConfigLoaded = false;
    if (s_ConfigLoaded)
        return;

    LoadConfig();
    s_ConfigLoaded = true;
}

uint32 GetChestGameObjectId()
{
    return s_ChestGameObjectId;
}

Seconds GetChestDespawnTime()
{
    return s_ChestDespawn;
}

Position const& GetChestPosition()
{
    return s_ChestPosition;
}

QuaternionData GetChestQuaternion()
{
    return GetChestRotation();
}
}

class instance_the_stockade_pvpve : public InstanceMapScript
{
public:
    instance_the_stockade_pvpve() : InstanceMapScript("instance_the_stockade_pvpve", StockadesPvPvE::StockadesMapId) { }

    InstanceScript* GetInstanceScript(InstanceMap* map) const override
    {
        return new instance_the_stockade_pvpve_InstanceMapScript(map);
    }

    struct instance_the_stockade_pvpve_InstanceMapScript : public InstanceScript, public PvpveDungeonInstance
    {
        instance_the_stockade_pvpve_InstanceMapScript(InstanceMap* map) : InstanceScript(map) { }

        void OnPlayerEnter(Player* player) override
        {
            InstanceScript::OnPlayerEnter(player);

            if (!player)
                return;

            if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
                return;

            if (PvpveDungeonRun* run = PvpveDungeonMgr::instance()->GetRunForPlayer(player->GetGUID()))
                PvpveDungeonMgr::instance()->OnInstanceCreated(run->TemplateId, run->Id, player->GetInstanceId());

            ApplyPvpveFfaState(player);
            sPvpveDungeonMgr->OnPlayerEnteredInstance(player, this);
        }

        void OnPlayerLeave(Player* player) override
        {
            InstanceScript::OnPlayerLeave(player);

            if (!player)
                return;

            ClearPvpveFfaState(player);

            if (!sPvpveDungeonMgr->IsPlayerInPvpveRun(player))
                return;
        }

        void OnPvpveRunFinished(uint32 runId, PvpveTeam const& winningTeam) override
        {
            AnnounceVictory(runId, winningTeam);
            SummonRewardChest(runId, winningTeam);
            ClearFfaState(winningTeam);
        }

    private:
        std::string CollectMemberNames(PvpveTeam const& team) const
        {
            std::string result;
            for (ObjectGuid const& guid : team.Members)
            {
                if (Player* player = ObjectAccessor::FindPlayer(guid))
                {
                    if (!result.empty())
                        result += ", ";
                    result += player->GetName();
                }
            }

            return result;
        }

        void AnnounceVictory(uint32 runId, PvpveTeam const& team)
        {
            std::string const memberNames = CollectMemberNames(team);
            if (!memberNames.empty())
                DoSendNotifyToInstance(Trinity::StringFormat("Stockades PvPvE run %u complete! Team %llu (%s) is victorious!", runId, team.Id, memberNames.c_str()).c_str());
            else
                DoSendNotifyToInstance(Trinity::StringFormat("Stockades PvPvE run %u complete! Team %llu is victorious!", runId, team.Id).c_str());
        }

        Player* SelectSummoner(PvpveTeam const& team) const
        {
            for (ObjectGuid const& guid : team.Members)
            {
                if (Player* player = ObjectAccessor::FindPlayer(guid))
                {
                    if (player->GetMap() == instance)
                        return player;
                }
            }

            return nullptr;
        }

        void SummonRewardChest(uint32 runId, PvpveTeam const& team)
        {
            uint32 const chestEntry = StockadesPvPvE::GetChestGameObjectId();
            if (!chestEntry)
                return;

            if (_rewardChestRunId == runId)
                return;

            Player* summoner = SelectSummoner(team);
            if (!summoner)
            {
                TC_LOG_WARN("server.custom", "Stockades PvPvE: unable to find a summoner for the reward chest (run {}, team {}).", runId, team.Id);
                return;
            }

            if (GameObject* chest = summoner->SummonGameObject(chestEntry, StockadesPvPvE::GetChestPosition(), StockadesPvPvE::GetChestQuaternion(), StockadesPvPvE::GetChestDespawnTime(), GO_SUMMON_TIMED_DESPAWN))
            {
                _rewardChestRunId = runId;
                _rewardChestGuid = chest->GetGUID();
                TC_LOG_INFO("server.custom", "Stockades PvPvE: spawned reward chest {} for run {} (team {}).", chestEntry, runId, team.Id);
            }
            else
            {
                TC_LOG_WARN("server.custom", "Stockades PvPvE: failed to summon reward chest {} for run {} (team {}).", chestEntry, runId, team.Id);
            }
        }

        void ClearFfaState(PvpveTeam const& team)
        {
            for (ObjectGuid const& guid : team.Members)
            {
                if (Player* player = ObjectAccessor::FindPlayer(guid))
                    ClearPvpveFfaState(player);
            }
        }

        ObjectGuid _rewardChestGuid;
        uint32 _rewardChestRunId = 0;
    };
};

void AddSC_instance_the_stockade_pvpve()
{
    StockadesPvPvE::EnsureConfigLoaded();
    new instance_the_stockade_pvpve();
}
