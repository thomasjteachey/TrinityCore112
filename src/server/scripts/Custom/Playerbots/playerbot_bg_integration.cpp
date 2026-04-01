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

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace
{
    struct PlayerbotRoleTargets
    {
        uint8 Tanks = 1;
        uint8 Healers = 2;
        uint8 RangedDps = 4;
        uint8 MeleeDps = 3;
    };

    struct PlayerbotSquadPlan
    {
        char const* Name = "main";
        uint8 DesiredSize = 0;
        uint8 Tanks = 0;
        uint8 Healers = 0;
        uint8 RangedDps = 0;
        uint8 MeleeDps = 0;
        char const* Objective = "Hold center";
    };

    struct PlayerbotMapPolicy
    {
        char const* ProfileName = "skirmish";
        char const* PrimaryObjective = "Team fight around map center";
        uint8 MinHomeDefenders = 1;
        uint8 MaxRoamers = 2;
        std::array<PlayerbotSquadPlan, 3> Squads{};
        uint8 SquadCount = 0;
    };

    struct ActiveBattlegroundStrategy
    {
        BattlegroundTypeId TypeId = BATTLEGROUND_TYPE_NONE;
        PlayerbotRoleTargets AllianceTargets;
        PlayerbotRoleTargets HordeTargets;
        PlayerbotMapPolicy Policy;
    };

    struct BackfillRequest
    {
        uint8 Alliance = 0;
        uint8 Horde = 0;
    };

    struct ActiveQueueFillState
    {
        BattlegroundTypeId TypeId = BATTLEGROUND_TYPE_NONE;
        uint32 InstanceId = 0;
        BackfillRequest LastRequest;
    };

    struct PlayerbotBGConfig
    {
        bool Enabled = true;
        uint32 QueueUpdateIntervalMs = 5000;
        uint32 MaxTeamImbalance = 1;
        uint32 MaxBackfillPerUpdate = 3;
    };

    PlayerbotBGConfig LoadPlayerbotBGConfig()
    {
        PlayerbotBGConfig config;
        config.Enabled = sConfigMgr->GetOption<bool>("Playerbot.BG.Enable", true);
        config.QueueUpdateIntervalMs = std::max<uint32>(1000, sConfigMgr->GetOption<uint32>("Playerbot.BG.QueueUpdateMs", 5000));
        config.MaxTeamImbalance = std::min<uint32>(5, sConfigMgr->GetOption<uint32>("Playerbot.BG.MaxTeamImbalance", 1));
        config.MaxBackfillPerUpdate = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Playerbot.BG.MaxBackfillPerUpdate", 3));
        return config;
    }

    enum class PlayerbotBGAction : uint8
    {
        HoldObjectives,
        CaptureObjectives,
        EscortCarrier,
        DefendCarrierRoute,
        PressureFrontline
    };

    struct PlayerbotBGSquadDirective
    {
        char const* SquadName = "main";
        PlayerbotBGAction Action = PlayerbotBGAction::HoldObjectives;
        uint8 Priority = 1;
    };

    char const* ToString(PlayerbotBGAction action)
    {
        switch (action)
        {
            case PlayerbotBGAction::HoldObjectives: return "HoldObjectives";
            case PlayerbotBGAction::CaptureObjectives: return "CaptureObjectives";
            case PlayerbotBGAction::EscortCarrier: return "EscortCarrier";
            case PlayerbotBGAction::DefendCarrierRoute: return "DefendCarrierRoute";
            case PlayerbotBGAction::PressureFrontline: return "PressureFrontline";
            default: return "Unknown";
        }
    }

    std::array<PlayerbotBGSquadDirective, 3> BuildSquadDirectives(PlayerbotMapPolicy const& policy)
    {
        std::array<PlayerbotBGSquadDirective, 3> directives{ };
        for (uint8 i = 0; i < policy.SquadCount; ++i)
        {
            directives[i].SquadName = policy.Squads[i].Name;
            directives[i].Priority = uint8(i + 1);

            if (policy.ProfileName == std::string_view("ctf_split"))
                directives[i].Action = i == 0 ? PlayerbotBGAction::CaptureObjectives : (i == 1 ? PlayerbotBGAction::EscortCarrier : PlayerbotBGAction::DefendCarrierRoute);
            else if (policy.ProfileName == std::string_view("lane_pressure"))
                directives[i].Action = i == 0 ? PlayerbotBGAction::PressureFrontline : PlayerbotBGAction::HoldObjectives;
            else
                directives[i].Action = i == 2 ? PlayerbotBGAction::CaptureObjectives : PlayerbotBGAction::HoldObjectives;
        }

        return directives;
    }

    PlayerbotRoleTargets BuildRoleTargets(uint32 maxPlayersPerTeam)
    {
        if (maxPlayersPerTeam >= 40)
            return { 4, 6, 16, 14 };

        if (maxPlayersPerTeam >= 15)
            return { 2, 3, 6, 4 };

        if (maxPlayersPerTeam >= 10)
            return { 1, 2, 4, 3 };

        return { 1, 1, 2, 1 };
    }

    uint8 AtLeastOne(uint32 value)
    {
        return uint8(value > 0 ? value : 1);
    }

    PlayerbotSquadPlan MakeSquad(char const* name, uint8 size, char const* objective)
    {
        PlayerbotSquadPlan squad;
        squad.Name = name;
        squad.DesiredSize = size;
        squad.Tanks = uint8(size >= 8 ? 1 : 0);
        squad.Healers = uint8(size >= 4 ? 1 : 0);

        uint8 combatants = uint8(size - squad.Tanks - squad.Healers);
        squad.RangedDps = uint8(combatants / 2);
        squad.MeleeDps = uint8(combatants - squad.RangedDps);
        squad.Objective = objective;
        return squad;
    }

    PlayerbotMapPolicy BuildMapPolicy(BattlegroundTypeId typeId, uint32 maxPlayersPerTeam)
    {
        PlayerbotMapPolicy policy;

        switch (typeId)
        {
            case BATTLEGROUND_WS:
            case BATTLEGROUND_TP:
            {
                policy.ProfileName = "ctf_split";
                policy.PrimaryObjective = "Pressure enemy flag room while protecting your carrier route";
                policy.MinHomeDefenders = AtLeastOne(maxPlayersPerTeam / 5);
                policy.MaxRoamers = AtLeastOne(maxPlayersPerTeam / 4);
                policy.SquadCount = 3;
                policy.Squads[0] = MakeSquad("flag_offense", AtLeastOne(maxPlayersPerTeam / 3), "Take enemy flag with sustained pressure");
                policy.Squads[1] = MakeSquad("carrier_escort", AtLeastOne(maxPlayersPerTeam / 3), "Escort carrier and lock midfield chokepoints");
                policy.Squads[2] = MakeSquad("base_defense", policy.MinHomeDefenders, "Protect own flag room and stall enemy picks");
                return policy;
            }
            case BATTLEGROUND_AB:
            case BATTLEGROUND_EY:
            case BATTLEGROUND_BFG:
            {
                policy.ProfileName = "resource_control";
                policy.PrimaryObjective = "Control majority nodes, rotate quickly, and avoid overcommitting";
                policy.MinHomeDefenders = AtLeastOne(maxPlayersPerTeam / 4);
                policy.MaxRoamers = AtLeastOne(maxPlayersPerTeam / 3);
                policy.SquadCount = 3;
                policy.Squads[0] = MakeSquad("node_defense", AtLeastOne(maxPlayersPerTeam / 3), "Anchor owned nodes with heal coverage");
                policy.Squads[1] = MakeSquad("rotation", AtLeastOne(maxPlayersPerTeam / 3), "Rotate to contested nodes and reinforce weak points");
                policy.Squads[2] = MakeSquad("assault", AtLeastOne(maxPlayersPerTeam / 4), "Pressure enemy weak node and force splits");
                return policy;
            }
            case BATTLEGROUND_AV:
            case BATTLEGROUND_SA:
            case BATTLEGROUND_IC:
            {
                policy.ProfileName = "lane_pressure";
                policy.PrimaryObjective = "Maintain lane momentum while preserving enough defenders for fallback";
                policy.MinHomeDefenders = AtLeastOne(maxPlayersPerTeam / 6);
                policy.MaxRoamers = AtLeastOne(maxPlayersPerTeam / 5);
                policy.SquadCount = 3;
                policy.Squads[0] = MakeSquad("frontline", AtLeastOne(maxPlayersPerTeam / 2), "Win the primary lane push and secure siege objectives");
                policy.Squads[1] = MakeSquad("flank", AtLeastOne(maxPlayersPerTeam / 4), "Disrupt enemy reinforcements and pick healers");
                policy.Squads[2] = MakeSquad("fallback_defense", policy.MinHomeDefenders, "Preserve graveyard/base integrity and buy time");
                return policy;
            }
            default:
            {
                policy.ProfileName = "skirmish";
                policy.PrimaryObjective = "Take favorable fights near active objectives";
                policy.MinHomeDefenders = 1;
                policy.MaxRoamers = AtLeastOne(maxPlayersPerTeam / 3);
                policy.SquadCount = 2;
                policy.Squads[0] = MakeSquad("main_group", AtLeastOne((maxPlayersPerTeam * 2) / 3), "Hold objective pressure with healer support");
                policy.Squads[1] = MakeSquad("flex_group", AtLeastOne(maxPlayersPerTeam / 3), "Float to assist allies and clean up overextensions");
                return policy;
            }
        }
    }

    BackfillRequest BuildSymmetricBackfillRequest(Battleground* bg)
    {
        if (!bg || bg->isArena())
            return { };

        PlayerbotBGConfig const config = LoadPlayerbotBGConfig();
        if (!config.Enabled)
            return { };

        uint32 maxPlayersPerTeam = bg->GetMaxPlayersPerTeam();
        uint32 minPlayersPerTeam = bg->GetMinPlayersPerTeam();
        uint32 allianceOccupied = bg->GetPlayersCountByTeam(ALLIANCE) + bg->GetInvitedCount(ALLIANCE);
        uint32 hordeOccupied = bg->GetPlayersCountByTeam(HORDE) + bg->GetInvitedCount(HORDE);
        uint32 allianceFreeSlots = bg->GetFreeSlotsForTeam(ALLIANCE);
        uint32 hordeFreeSlots = bg->GetFreeSlotsForTeam(HORDE);

        uint32 allianceNeed = allianceOccupied < minPlayersPerTeam ? minPlayersPerTeam - allianceOccupied : 0;
        uint32 hordeNeed = hordeOccupied < minPlayersPerTeam ? minPlayersPerTeam - hordeOccupied : 0;

        if (allianceOccupied > hordeOccupied + config.MaxTeamImbalance)
            hordeNeed += allianceOccupied - (hordeOccupied + config.MaxTeamImbalance);
        else if (hordeOccupied > allianceOccupied + config.MaxTeamImbalance)
            allianceNeed += hordeOccupied - (allianceOccupied + config.MaxTeamImbalance);

        allianceNeed = std::min(allianceNeed, allianceFreeSlots);
        hordeNeed = std::min(hordeNeed, hordeFreeSlots);
        allianceNeed = std::min(allianceNeed, maxPlayersPerTeam);
        hordeNeed = std::min(hordeNeed, maxPlayersPerTeam);
        allianceNeed = std::min(allianceNeed, config.MaxBackfillPerUpdate);
        hordeNeed = std::min(hordeNeed, config.MaxBackfillPerUpdate);

        return { uint8(allianceNeed), uint8(hordeNeed) };
    }

    class PlayerbotBGQueueCoordinator
    {
        public:
            static PlayerbotBGQueueCoordinator& Instance()
            {
                static PlayerbotBGQueueCoordinator instance;
                return instance;
            }

            void StartTracking(Battleground* bg)
            {
                if (!bg || bg->isArena())
                    return;
                if (!LoadPlayerbotBGConfig().Enabled)
                    return;

                ActiveQueueFillState& fillState = _activeQueueFill[bg->GetInstanceID()];
                fillState.TypeId = bg->GetTypeID();
                fillState.InstanceId = bg->GetInstanceID();
                fillState.LastRequest = BuildSymmetricBackfillRequest(bg);
                LogBackfillState(bg, fillState.LastRequest, "initialized");
            }

            std::optional<BackfillRequest> StopTracking(Battleground* bg)
            {
                if (!bg)
                    return std::nullopt;

                auto itr = _activeQueueFill.find(bg->GetInstanceID());
                if (itr == _activeQueueFill.end())
                    return std::nullopt;

                BackfillRequest request = itr->second.LastRequest;
                _activeQueueFill.erase(itr);
                return request;
            }

            void Update()
            {
                for (auto itr = _activeQueueFill.begin(); itr != _activeQueueFill.end();)
                {
                    ActiveQueueFillState& state = itr->second;
                    Battleground* bg = sBattlegroundMgr->GetBattleground(state.InstanceId, state.TypeId);
                    if (!bg || bg->GetStatus() == STATUS_NONE)
                    {
                        itr = _activeQueueFill.erase(itr);
                        continue;
                    }

                    BackfillRequest current = BuildSymmetricBackfillRequest(bg);
                    if (current.Alliance != state.LastRequest.Alliance || current.Horde != state.LastRequest.Horde)
                    {
                        state.LastRequest = current;
                        LogBackfillState(bg, current, "updated");
                    }

                    ++itr;
                }
            }

        private:
            static void LogBackfillState(Battleground const* bg, BackfillRequest request, char const* stage)
            {
                TC_LOG_INFO("bg.playerbot",
                    "Playerbot BG queue fill {} (instance {}, type {}): request A:{} H:{} (players A:{} H:{}, invited A:{} H:{})",
                    stage, bg->GetInstanceID(), uint32(bg->GetTypeID()),
                    uint32(request.Alliance), uint32(request.Horde),
                    bg->GetPlayersCountByTeam(ALLIANCE), bg->GetPlayersCountByTeam(HORDE),
                    bg->GetInvitedCount(ALLIANCE), bg->GetInvitedCount(HORDE));
            }

            std::unordered_map<uint32, ActiveQueueFillState> _activeQueueFill;
    };

    class PlayerbotBGStateTracker : public BGScript
    {
        public:
            PlayerbotBGStateTracker() : BGScript("playerbot_bg_state_tracker") { }

            void OnBattlegroundStart(Battleground* bg) override
            {
                if (!bg)
                    return;
                if (!LoadPlayerbotBGConfig().Enabled)
                    return;

                uint32 instanceId = bg->GetInstanceID();
                BattlegroundTypeId typeId = bg->GetTypeID();
                uint32 maxPlayersPerTeam = bg->GetMaxPlayersPerTeam();

                ActiveBattlegroundStrategy plan;
                plan.TypeId = typeId;
                plan.AllianceTargets = BuildRoleTargets(maxPlayersPerTeam);
                plan.HordeTargets = BuildRoleTargets(maxPlayersPerTeam);
                plan.Policy = BuildMapPolicy(typeId, maxPlayersPerTeam);
                auto const directives = BuildSquadDirectives(plan.Policy);
                PlayerbotBGQueueCoordinator::Instance().StartTracking(bg);

                _activeStrategies[instanceId] = plan;

                TC_LOG_INFO("bg.playerbot",
                    "Playerbot BG planner: started '{}' (type {}, instance {}) with profile '{}'",
                    bg->GetName(), uint32(typeId), instanceId, plan.Policy.ProfileName);
                TC_LOG_INFO("bg.playerbot",
                    "Playerbot BG planner objective (instance {}): {} (minDefenders {}, maxRoamers {})",
                    instanceId, plan.Policy.PrimaryObjective, uint32(plan.Policy.MinHomeDefenders), uint32(plan.Policy.MaxRoamers));
                TC_LOG_INFO("bg.playerbot",
                    "Playerbot BG role targets (instance {}): T/H/R/M {}/{}/{}/{}",
                    instanceId,
                    uint32(plan.AllianceTargets.Tanks), uint32(plan.AllianceTargets.Healers),
                    uint32(plan.AllianceTargets.RangedDps), uint32(plan.AllianceTargets.MeleeDps));
                for (uint8 i = 0; i < plan.Policy.SquadCount; ++i)
                {
                    PlayerbotSquadPlan const& squad = plan.Policy.Squads[i];
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG squad plan (instance {}, squad {}): '{}' size {} with T/H/R/M {}/{}/{}/{} -> {}",
                        instanceId, uint32(i), squad.Name, uint32(squad.DesiredSize),
                        uint32(squad.Tanks), uint32(squad.Healers), uint32(squad.RangedDps), uint32(squad.MeleeDps), squad.Objective);
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG directive (instance {}, squad '{}'): action {} priority {}",
                        instanceId, directives[i].SquadName, ToString(directives[i].Action), uint32(directives[i].Priority));
                }
            }

            void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeam) override
            {
                if (!bg)
                    return;

                uint32 instanceId = bg->GetInstanceID();
                auto itr = _activeStrategies.find(instanceId);
                if (itr == _activeStrategies.end())
                {
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG planner: ended '{}' (type {}, instance {}, winnerTeam {}) with no active plan",
                        bg->GetName(), uint32(bg->GetTypeID()), instanceId, uint32(winnerTeam));
                    return;
                }

                PlayerbotMapPolicy const policy = itr->second.Policy;
                _activeStrategies.erase(itr);
                std::optional<BackfillRequest> finalBackfill = PlayerbotBGQueueCoordinator::Instance().StopTracking(bg);

                TC_LOG_INFO("bg.playerbot",
                    "Playerbot BG planner: ended '{}' (type {}, instance {}, winnerTeam {}) and retired profile '{}'",
                    bg->GetName(), uint32(bg->GetTypeID()), instanceId, uint32(winnerTeam), policy.ProfileName);

                if (finalBackfill)
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG queue fill final snapshot (instance {}): request A:{} H:{}",
                        instanceId, uint32(finalBackfill->Alliance), uint32(finalBackfill->Horde));
            }

        private:
            std::unordered_map<uint32, ActiveBattlegroundStrategy> _activeStrategies;
    };

    class PlayerbotBGQueueWorldUpdater : public WorldScript
    {
        public:
            PlayerbotBGQueueWorldUpdater() : WorldScript("playerbot_bg_queue_world_updater") { }

            void OnUpdate(uint32 diff) override
            {
                PlayerbotBGConfig const config = LoadPlayerbotBGConfig();
                if (!config.Enabled)
                    return;

                if (_updateTimer <= diff)
                {
                    PlayerbotBGQueueCoordinator::Instance().Update();
                    _updateTimer = config.QueueUpdateIntervalMs;
                }
                else
                    _updateTimer -= diff;
            }

        private:
            uint32 _updateTimer = LoadPlayerbotBGConfig().QueueUpdateIntervalMs;
    };
}

void AddSC_playerbot_bg_integration()
{
    new PlayerbotBGStateTracker();
    new PlayerbotBGQueueWorldUpdater();
}
