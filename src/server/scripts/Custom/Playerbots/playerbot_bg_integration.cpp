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
#include "BattlegroundQueue.h"
#include "BattlegroundMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

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
        bool AutoQueueBots = false;
        BattlegroundTypeId AutoQueueBgType = BATTLEGROUND_RB;
        std::string BotNamePrefix = "bot";
        bool PreferRealPlayers = true;
        bool AutoGenerateBots = false;
        uint32 AutoGenerateTargetAlliance = 10;
        uint32 AutoGenerateTargetHorde = 10;
        uint32 AutoGenerateMaxCreatePerTick = 2;
        std::string AutoGenerateNamePrefix = "pbbot";
        std::string RuntimeBootstrapProvider = "none";
    };

    PlayerbotBGConfig LoadPlayerbotBGConfig()
    {
        PlayerbotBGConfig config;
        config.Enabled = sConfigMgr->GetBoolDefault("Playerbot.BG.Enable", true);
        config.QueueUpdateIntervalMs = std::max<uint32>(1000, uint32(sConfigMgr->GetIntDefault("Playerbot.BG.QueueUpdateMs", 5000)));
        config.MaxTeamImbalance = std::min<uint32>(5, uint32(sConfigMgr->GetIntDefault("Playerbot.BG.MaxTeamImbalance", 1)));
        config.MaxBackfillPerUpdate = std::max<uint32>(1, uint32(sConfigMgr->GetIntDefault("Playerbot.BG.MaxBackfillPerUpdate", 3)));
        config.AutoQueueBots = sConfigMgr->GetBoolDefault("Playerbot.BG.AutoQueueBots", false);
        config.AutoQueueBgType = BattlegroundTypeId(uint32(sConfigMgr->GetIntDefault("Playerbot.BG.AutoQueueBgType", int32(BATTLEGROUND_RB))));
        config.BotNamePrefix = sConfigMgr->GetStringDefault("Playerbot.BG.BotNamePrefix", "bot");
        config.PreferRealPlayers = sConfigMgr->GetBoolDefault("Playerbot.BG.PreferRealPlayers", true);
        config.AutoGenerateBots = sConfigMgr->GetBoolDefault("Playerbot.BG.AutoGenerateBots", false);
        config.AutoGenerateTargetAlliance = uint32(sConfigMgr->GetIntDefault("Playerbot.BG.AutoGenerateTargetAlliance", 10));
        config.AutoGenerateTargetHorde = uint32(sConfigMgr->GetIntDefault("Playerbot.BG.AutoGenerateTargetHorde", 10));
        config.AutoGenerateMaxCreatePerTick = std::max<uint32>(1, uint32(sConfigMgr->GetIntDefault("Playerbot.BG.AutoGenerateMaxCreatePerTick", 2)));
        config.AutoGenerateNamePrefix = sConfigMgr->GetStringDefault("Playerbot.BG.AutoGenerateNamePrefix", "pbbot");
        config.RuntimeBootstrapProvider = sConfigMgr->GetStringDefault("Playerbot.BG.RuntimeBootstrapProvider", "none");
        return config;
    }

    class PlayerbotBGRuntimeBootstrap
    {
        public:
            using BootstrapFn = std::function<uint32(TeamId team, uint32 requestedCount, std::string const& botNamePrefix, BattlegroundTypeId bgTypeId)>;

            static PlayerbotBGRuntimeBootstrap& Instance()
            {
                static PlayerbotBGRuntimeBootstrap instance;
                return instance;
            }

            void RegisterProvider(std::string name, BootstrapFn callback)
            {
                if (name.empty() || !callback)
                    return;

                _providers.insert_or_assign(std::move(name), std::move(callback));
            }

            uint32 BootstrapOnlineCandidates(std::string const& providerName, TeamId team, uint32 requestedCount, std::string const& botNamePrefix, BattlegroundTypeId bgTypeId) const
            {
                if (!requestedCount || providerName.empty() || providerName == "none")
                    return 0;

                auto itr = _providers.find(providerName);
                if (itr == _providers.end())
                    return 0;

                return itr->second(team, requestedCount, botNamePrefix, bgTypeId);
            }

            bool HasProvider(std::string const& providerName) const
            {
                return _providers.find(providerName) != _providers.end();
            }

        private:
            std::unordered_map<std::string, BootstrapFn> _providers;
    };

    uint32 EnqueueSqlBootstrapRequests(TeamId team, uint32 requestedCount, std::string const& botNamePrefix, BattlegroundTypeId bgTypeId)
    {
        if (!requestedCount)
            return 0;

        uint32 enqueued = 0;
        for (uint32 i = 0; i < requestedCount; ++i)
        {
            CharacterDatabase.Execute("INSERT INTO playerbot_bg_bootstrap_queue "
                "(requested_at, team_id, battleground_type_id, bot_name_prefix, state) "
                "VALUES (NOW(), {}, {}, '{}', 'queued')", uint32(team), uint32(bgTypeId), botNamePrefix);
            ++enqueued;
        }

        return enqueued;
    }

    struct GeneratedBotSeed
    {
        TeamId Team = TEAM_ALLIANCE;
        std::string Name;
    };

    class PlayerbotBotGenerationScaffold
    {
        public:
            static PlayerbotBotGenerationScaffold& Instance()
            {
                static PlayerbotBotGenerationScaffold instance;
                return instance;
            }

            void Update(PlayerbotBGConfig const& config, BackfillRequest demand)
            {
                if (!config.AutoGenerateBots)
                    return;

                uint32 onlineAlliance = CountOnlineCandidates(TEAM_ALLIANCE, config.BotNamePrefix);
                uint32 onlineHorde = CountOnlineCandidates(TEAM_HORDE, config.BotNamePrefix);
                uint32 pendingAlliance = CountPending(TEAM_ALLIANCE);
                uint32 pendingHorde = CountPending(TEAM_HORDE);

                uint32 desiredAlliance = std::max<uint32>(config.AutoGenerateTargetAlliance, demand.Alliance);
                uint32 desiredHorde = std::max<uint32>(config.AutoGenerateTargetHorde, demand.Horde);
                uint32 missingAlliance = desiredAlliance > onlineAlliance + pendingAlliance ? desiredAlliance - (onlineAlliance + pendingAlliance) : 0;
                uint32 missingHorde = desiredHorde > onlineHorde + pendingHorde ? desiredHorde - (onlineHorde + pendingHorde) : 0;

                uint32 created = 0;
                created += GenerateSeeds(TEAM_ALLIANCE, missingAlliance, config);
                created += GenerateSeeds(TEAM_HORDE, missingHorde, config);

                if (created)
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG generator scaffold queued {} seed(s): online A:{} H:{}, pending A:{} H:{}, desired A:{} H:{}",
                        created, onlineAlliance, onlineHorde, pendingAlliance, pendingHorde, desiredAlliance, desiredHorde);
            }

        private:
            static uint32 CountOnlineCandidates(TeamId team, std::string const& prefix)
            {
                std::shared_lock lock(*HashMapHolder<Player>::GetLock());
                uint32 count = 0;
                for (auto const& [_, player] : ObjectAccessor::GetPlayers())
                    if (player && player->GetTeamId() == team && (prefix.empty() || player->GetName().starts_with(prefix)))
                        ++count;

                return count;
            }

            uint32 CountPending(TeamId team) const
            {
                return uint32(std::count_if(_pendingSeeds.begin(), _pendingSeeds.end(), [team](GeneratedBotSeed const& seed) { return seed.Team == team; }));
            }

            uint32 GenerateSeeds(TeamId team, uint32 missing, PlayerbotBGConfig const& config)
            {
                if (!missing)
                    return 0;

                uint32 toCreate = std::min<uint32>(missing, config.AutoGenerateMaxCreatePerTick);
                for (uint32 i = 0; i < toCreate; ++i)
                {
                    GeneratedBotSeed seed;
                    seed.Team = team;
                    seed.Name = BuildSeedName(team, config.AutoGenerateNamePrefix);
                    _pendingSeeds.push_back(seed);
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG generator scaffold created seed '{}' for team {} (placeholder only; hook real character/session creation here)",
                        seed.Name, uint32(team));
                }

                return toCreate;
            }

            std::string BuildSeedName(TeamId team, std::string const& prefix)
            {
                char side = team == TEAM_ALLIANCE ? 'a' : 'h';
                return std::string(prefix).append("_").append(1, side).append("_").append(std::to_string(++_nameSerial));
            }

            std::deque<GeneratedBotSeed> _pendingSeeds;
            uint64 _nameSerial = 0;
    };

    bool TryAutoQueueBotPlayer(Player* player, BattlegroundTypeId bgTypeId)
    {
        if (!player || player->InBattleground() || player->InBattlegroundQueue() || !player->HasFreeBattlegroundQueueId() || player->IsDeserter())
            return false;

        BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(bgTypeId);
        if (!bgTemplate || bgTemplate->IsArena())
            return false;

        PVPDifficultyEntry const* bracketEntry = DB2Manager::GetBattlegroundBracketByLevel(bgTemplate->MapIDs.front(), player->GetLevel());
        if (!bracketEntry)
            return false;

        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, BattlegroundQueueIdType::Battleground, false, 0);
        if (!BattlegroundMgr::IsValidQueueId(bgQueueTypeId))
            return false;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, Team(player->GetTeam()), bracketEntry, false, false, 0);
        player->AddBattlegroundQueueId(bgQueueTypeId);

        TC_LOG_INFO("bg.playerbot",
            "Playerbot BG auto-queued '{}' ({}) to queue {{ listId {}, type {} }} at level {} (joinTime {})",
            player->GetName(), player->GetGUID().ToString(), uint32(bgQueueTypeId.BattlemasterListId), uint32(bgQueueTypeId.Type), uint32(player->GetLevel()), ginfo->JoinTime);
        return true;
    }

    BackfillRequest BuildQueueStartBackfillRequest(BattlegroundTypeId bgTypeId, PlayerbotBGConfig const& config)
    {
        if (!config.AutoQueueBots)
            return { };

        BattlegroundTemplate const* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplateByTypeId(bgTypeId);
        if (!bgTemplate || bgTemplate->IsArena())
            return { };

        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, BattlegroundQueueIdType::Battleground, false, 0);
        if (!BattlegroundMgr::IsValidQueueId(bgQueueTypeId))
            return { };

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        uint32 allianceQueued = bgQueue.GetPlayersInQueue(TEAM_ALLIANCE);
        uint32 hordeQueued = bgQueue.GetPlayersInQueue(TEAM_HORDE);
        uint32 minPlayersPerTeam = bgTemplate->GetMinPlayersPerTeam();
        if (allianceQueued == 0 && hordeQueued == 0)
            return { };

        uint32 allianceNeed = allianceQueued < minPlayersPerTeam ? minPlayersPerTeam - allianceQueued : 0;
        uint32 hordeNeed = hordeQueued < minPlayersPerTeam ? minPlayersPerTeam - hordeQueued : 0;

        if (allianceQueued > hordeQueued + config.MaxTeamImbalance)
            hordeNeed += allianceQueued - (hordeQueued + config.MaxTeamImbalance);
        else if (hordeQueued > allianceQueued + config.MaxTeamImbalance)
            allianceNeed += hordeQueued - (allianceQueued + config.MaxTeamImbalance);

        allianceNeed = std::min(allianceNeed, config.MaxBackfillPerUpdate);
        hordeNeed = std::min(hordeNeed, config.MaxBackfillPerUpdate);
        return { uint8(allianceNeed), uint8(hordeNeed) };
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

            BackfillRequest GetAggregateRequest() const
            {
                BackfillRequest aggregate;
                for (auto const& [_, state] : _activeQueueFill)
                {
                    aggregate.Alliance = uint8(std::min<uint32>(uint32(aggregate.Alliance) + state.LastRequest.Alliance, 255));
                    aggregate.Horde = uint8(std::min<uint32>(uint32(aggregate.Horde) + state.LastRequest.Horde, 255));
                }

                return aggregate;
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
                    AutoQueueBots(config);
                    _updateTimer = config.QueueUpdateIntervalMs;
                }
                else
                    _updateTimer -= diff;
            }

        private:
            static bool IsBotCandidate(Player* player, std::string const& prefix, TeamId team)
            {
                if (!player || player->GetTeamId() != team)
                    return false;

                if (prefix.empty())
                    return true;

                return player->GetName().starts_with(prefix);
            }

            static uint8 QueueBotCandidates(TeamId team, uint8 needed, PlayerbotBGConfig const& config)
            {
                if (!needed)
                    return 0;

                std::shared_lock lock(*HashMapHolder<Player>::GetLock());
                HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
                uint8 queued = 0;
                for (auto const& [_, player] : players)
                {
                    if (!IsBotCandidate(player, config.BotNamePrefix, team))
                        continue;

                    if (!TryAutoQueueBotPlayer(player, config.AutoQueueBgType))
                        continue;

                    if (++queued >= needed)
                        break;
                }

                return queued;
            }

            static uint32 CountOnlineBotCandidates(TeamId team, PlayerbotBGConfig const& config)
            {
                std::shared_lock lock(*HashMapHolder<Player>::GetLock());
                HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
                uint32 count = 0;
                for (auto const& [_, player] : players)
                    if (IsBotCandidate(player, config.BotNamePrefix, team))
                        ++count;

                return count;
            }

            static void BootstrapOfflineCandidatesIfNeeded(PlayerbotBGConfig const& config, BackfillRequest const& mergedNeed)
            {
                if (config.RuntimeBootstrapProvider.empty() || config.RuntimeBootstrapProvider == "none")
                    return;

                uint32 onlineAlliance = CountOnlineBotCandidates(TEAM_ALLIANCE, config);
                uint32 onlineHorde = CountOnlineBotCandidates(TEAM_HORDE, config);
                uint32 allianceMissing = mergedNeed.Alliance > onlineAlliance ? mergedNeed.Alliance - onlineAlliance : 0;
                uint32 hordeMissing = mergedNeed.Horde > onlineHorde ? mergedNeed.Horde - onlineHorde : 0;

                uint32 bootstrappedAlliance = PlayerbotBGRuntimeBootstrap::Instance().BootstrapOnlineCandidates(
                    config.RuntimeBootstrapProvider, TEAM_ALLIANCE, allianceMissing, config.BotNamePrefix, config.AutoQueueBgType);
                uint32 bootstrappedHorde = PlayerbotBGRuntimeBootstrap::Instance().BootstrapOnlineCandidates(
                    config.RuntimeBootstrapProvider, TEAM_HORDE, hordeMissing, config.BotNamePrefix, config.AutoQueueBgType);

                if (bootstrappedAlliance || bootstrappedHorde)
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG runtime bootstrap provider '{}' requested candidates A:{} H:{} and reported bootstrapped A:{} H:{}",
                        config.RuntimeBootstrapProvider, allianceMissing, hordeMissing, bootstrappedAlliance, bootstrappedHorde);
            }

            static uint8 CountQueuedBotCandidates(TeamId team, PlayerbotBGConfig const& config, BattlegroundQueueTypeId queueTypeId)
            {
                std::shared_lock lock(*HashMapHolder<Player>::GetLock());
                HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
                uint8 queued = 0;
                for (auto const& [_, player] : players)
                    if (IsBotCandidate(player, config.BotNamePrefix, team) && player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
                        ++queued;

                return queued;
            }

            static uint8 DequeueBotCandidates(TeamId team, uint8 toRemove, PlayerbotBGConfig const& config, BattlegroundQueueTypeId queueTypeId)
            {
                if (!toRemove)
                    return 0;

                BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
                std::shared_lock lock(*HashMapHolder<Player>::GetLock());
                HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
                uint8 removed = 0;
                for (auto const& [_, player] : players)
                {
                    if (!IsBotCandidate(player, config.BotNamePrefix, team))
                        continue;

                    if (!player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId) || player->InBattleground())
                        continue;

                    player->RemoveBattlegroundQueueId(queueTypeId);
                    queue.RemovePlayer(player->GetGUID(), true);
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG auto-dequeued '{}' ({}) from queue {{ listId {}, type {} }} to prioritize real players",
                        player->GetName(), player->GetGUID().ToString(), uint32(queueTypeId.BattlemasterListId), uint32(queueTypeId.Type));

                    if (++removed >= toRemove)
                        break;
                }

                return removed;
            }

            static void AutoQueueBots(PlayerbotBGConfig const& config)
            {
                static uint8 noCandidateWarningCooldown = 0;

                if (!config.AutoQueueBots)
                    return;

                BattlegroundQueueTypeId queueTypeId = BattlegroundMgr::BGQueueTypeId(config.AutoQueueBgType, BattlegroundQueueIdType::Battleground, false, 0);
                if (!BattlegroundMgr::IsValidQueueId(queueTypeId))
                    return;

                BackfillRequest activeBgNeed = PlayerbotBGQueueCoordinator::Instance().GetAggregateRequest();
                BackfillRequest queueStartNeed = BuildQueueStartBackfillRequest(config.AutoQueueBgType, config);
                BackfillRequest mergedNeed;
                mergedNeed.Alliance = uint8(std::min<uint32>(config.MaxBackfillPerUpdate, uint32(activeBgNeed.Alliance) + queueStartNeed.Alliance));
                mergedNeed.Horde = uint8(std::min<uint32>(config.MaxBackfillPerUpdate, uint32(activeBgNeed.Horde) + queueStartNeed.Horde));
                PlayerbotBotGenerationScaffold::Instance().Update(config, mergedNeed);
                BootstrapOfflineCandidatesIfNeeded(config, mergedNeed);

                uint8 queuedAllianceBots = CountQueuedBotCandidates(TEAM_ALLIANCE, config, queueTypeId);
                uint8 queuedHordeBots = CountQueuedBotCandidates(TEAM_HORDE, config, queueTypeId);

                if (config.PreferRealPlayers)
                {
                    if (queuedAllianceBots > mergedNeed.Alliance)
                        queuedAllianceBots = uint8(queuedAllianceBots - DequeueBotCandidates(TEAM_ALLIANCE, uint8(queuedAllianceBots - mergedNeed.Alliance), config, queueTypeId));
                    if (queuedHordeBots > mergedNeed.Horde)
                        queuedHordeBots = uint8(queuedHordeBots - DequeueBotCandidates(TEAM_HORDE, uint8(queuedHordeBots - mergedNeed.Horde), config, queueTypeId));
                }

                uint8 allianceQueued = mergedNeed.Alliance > queuedAllianceBots ? QueueBotCandidates(TEAM_ALLIANCE, uint8(mergedNeed.Alliance - queuedAllianceBots), config) : 0;
                uint8 hordeQueued = mergedNeed.Horde > queuedHordeBots ? QueueBotCandidates(TEAM_HORDE, uint8(mergedNeed.Horde - queuedHordeBots), config) : 0;

                if (allianceQueued || hordeQueued)
                    TC_LOG_INFO("bg.playerbot",
                        "Playerbot BG auto-queue tick: requested A:{} H:{}, queued A:{} H:{}, queueType {}",
                        uint32(mergedNeed.Alliance), uint32(mergedNeed.Horde), uint32(allianceQueued), uint32(hordeQueued), uint32(config.AutoQueueBgType));
                else if (mergedNeed.Alliance || mergedNeed.Horde)
                {
                    uint32 onlineAlliance = CountOnlineBotCandidates(TEAM_ALLIANCE, config);
                    uint32 onlineHorde = CountOnlineBotCandidates(TEAM_HORDE, config);
                    bool shouldLog = (onlineAlliance == 0 || onlineHorde == 0 || config.AutoGenerateBots) && noCandidateWarningCooldown == 0;
                    if (shouldLog)
                    {
                        TC_LOG_INFO("bg.playerbot",
                            "Playerbot BG auto-queue could not satisfy demand (requested A:{} H:{}, online candidates A:{} H:{}, prefix '{}', autoGenerate {}). "
                            "AutoGenerate currently creates placeholder seeds only; provide online bot sessions or integrate a real bot runtime/session bootstrap to run solo BGs. "
                            "Configured runtime provider '{}', provider registered {}.",
                            uint32(mergedNeed.Alliance), uint32(mergedNeed.Horde), onlineAlliance, onlineHorde, config.BotNamePrefix, config.AutoGenerateBots ? 1 : 0,
                            config.RuntimeBootstrapProvider, PlayerbotBGRuntimeBootstrap::Instance().HasProvider(config.RuntimeBootstrapProvider) ? 1 : 0);
                        noCandidateWarningCooldown = 6;
                    }
                }

                if (noCandidateWarningCooldown > 0)
                    --noCandidateWarningCooldown;
            }

            uint32 _updateTimer = LoadPlayerbotBGConfig().QueueUpdateIntervalMs;
    };
}

void RegisterPlayerbotBGRuntimeBootstrapProvider(std::string const& name, std::function<uint32(TeamId, uint32, std::string const&, BattlegroundTypeId)> callback);

void AddSC_playerbot_bg_integration()
{
    RegisterPlayerbotBGRuntimeBootstrapProvider("sql_queue", [](TeamId team, uint32 requestedCount, std::string const& botNamePrefix, BattlegroundTypeId bgTypeId) -> uint32
    {
        return EnqueueSqlBootstrapRequests(team, requestedCount, botNamePrefix, bgTypeId);
    });
    new PlayerbotBGStateTracker();
    new PlayerbotBGQueueWorldUpdater();
}

void RegisterPlayerbotBGRuntimeBootstrapProvider(std::string const& name, std::function<uint32(TeamId, uint32, std::string const&, BattlegroundTypeId)> callback)
{
    PlayerbotBGRuntimeBootstrap::Instance().RegisterProvider(name, std::move(callback));
}
