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
#include "Log.h"
#include "ScriptMgr.h"

#include <array>
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

    class PlayerbotBGStateTracker : public BGScript
    {
        public:
            PlayerbotBGStateTracker() : BGScript("playerbot_bg_state_tracker") { }

            void OnBattlegroundStart(Battleground* bg) override
            {
                if (!bg)
                    return;

                uint32 instanceId = bg->GetInstanceID();
                BattlegroundTypeId typeId = bg->GetTypeID();
                uint32 maxPlayersPerTeam = bg->GetMaxPlayersPerTeam();

                ActiveBattlegroundStrategy plan;
                plan.TypeId = typeId;
                plan.AllianceTargets = BuildRoleTargets(maxPlayersPerTeam);
                plan.HordeTargets = BuildRoleTargets(maxPlayersPerTeam);
                plan.Policy = BuildMapPolicy(typeId, maxPlayersPerTeam);

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

                TC_LOG_INFO("bg.playerbot",
                    "Playerbot BG planner: ended '{}' (type {}, instance {}, winnerTeam {}) and retired profile '{}'",
                    bg->GetName(), uint32(bg->GetTypeID()), instanceId, uint32(winnerTeam), policy.ProfileName);
            }

        private:
            std::unordered_map<uint32, ActiveBattlegroundStrategy> _activeStrategies;
    };
}

void AddSC_playerbot_bg_integration()
{
    new PlayerbotBGStateTracker();
}
