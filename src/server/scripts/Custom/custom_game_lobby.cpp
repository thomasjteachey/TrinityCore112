/*
 * Private custom battleground/arena lobbies staged in server-only Map 1
 * subinstances. Nothing in this file is persisted across a world restart.
 */

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbot/Pvp/PlayerbotObcClone.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "TemporarySummon.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32 CUSTOM_GAME_HOST_ENTRY = 900001;
constexpr uint32 CUSTOM_GAME_BLUE_ENTRY = 900002;
constexpr uint32 CUSTOM_GAME_RED_ENTRY = 900003;
constexpr uint32 CUSTOM_GAME_CHROMIE_ENTRY = 900004;
constexpr uint32 CUSTOM_GAME_MAP_ID = 1;
constexpr uint32 CUSTOM_GAME_MAX_PLAYERS_PER_TEAM = 40;
constexpr uint32 BLUE_FLAG_VISUAL = 32609;
constexpr uint32 RED_FLAG_VISUAL = 32610;

Position const LobbyArrival = { 16229.862305f, 16415.587891f, -64.378716f, 3.132112f };
Position const BlueNpcPosition = { 16218.0f, 16424.0f, -64.378716f, 4.70f };
Position const RedNpcPosition = { 16218.0f, 16407.0f, -64.378716f, 1.58f };
Position const ChromieNpcPosition = { 16218.0f, 16415.5f, -64.378716f, 3.14f };
constexpr float LOBBY_MAX_DISTANCE = 32.0f;

enum GossipAction : uint32
{
    ACTION_CREATE = 100,
    ACTION_STATUS = 199,
    ACTION_JOIN_TEAM = 200,
    ACTION_LEAVE_TEAM,
    ACTION_ADD_PARTY,
    ACTION_REMOVE_PARTY,
    ACTION_ADD_BOT,
    ACTION_REMOVE_BOT,
    ACTION_ADD_DARK,
    ACTION_REMOVE_DARK,
    ACTION_SELECT_WS = 300,
    ACTION_SELECT_TP,
    ACTION_SELECT_AB,
    ACTION_SELECT_EY,
    ACTION_SELECT_SCM,
    ACTION_SELECT_BRT,
    ACTION_SELECT_OBC,
    ACTION_SELECT_BFG,
    ACTION_SELECT_AV,
    ACTION_SELECT_SA,
    ACTION_SELECT_IC,
    ACTION_SELECT_NA,
    ACTION_SELECT_BE,
    ACTION_SELECT_RL,
    ACTION_SELECT_NL,
    ACTION_SELECT_TV,
    ACTION_SELECT_TTP,
    ACTION_RULE_FLAG_CAPS = 400,
    ACTION_RULE_RESOURCES,
    ACTION_RULE_RESOURCE_RATE,
    ACTION_RULE_KILLS,
    ACTION_RULE_REZ_TIME,
    ACTION_RULE_NODE_FLAG_TIME,
    ACTION_RULE_NODE_BASE_TIME,
    ACTION_TOGGLE_ENEMY_FLAG = 450,
    ACTION_TOGGLE_ALLY_FLAG,
    ACTION_CYCLE_WEATHER,
    ACTION_START = 500,
    ACTION_CLOSE
};

struct CloneRequest
{
    ObjectGuid SourceGuid;
    std::string SourceName;
    uint32 Team = 0;
    bool IsPlayerbot = false;
};

struct LobbyMember
{
    WorldLocation ReturnLocation;
    uint8 OriginalSubgroup = 0;
};

struct CustomGameLobby
{
    uint32 InstanceId = 0;
    ObjectGuid OwnerGuid;
    std::unordered_map<ObjectGuid, LobbyMember> Members;
    std::unordered_map<ObjectGuid, uint32> Teams;
    std::vector<CloneRequest> CloneRequests;
    BattlegroundTypeId SelectedType = BATTLEGROUND_WS;
    uint8 ArenaType = 0;
    BattlegroundCustomRules Rules;
    uint32 ActiveBattlegroundId = 0;
    bool ClonesSpawned = false;
    bool Closing = false;
};

char const* TeamName(uint32 team)
{
    return team == ALLIANCE ? "Blue" : team == HORDE ? "Red" : "Spectator";
}

std::string BattlegroundName(CustomGameLobby const& lobby)
{
    switch (lobby.SelectedType)
    {
        case BATTLEGROUND_WS: return "Warsong Gulch";
        case BATTLEGROUND_TP: return "Twin Peaks";
        case BATTLEGROUND_AB: return "Arathi Basin";
        case BATTLEGROUND_EY: return "Eye of the Storm";
        case BATTLEGROUND_SCM: return "Scarlet Chapel Deathmatch";
        case BATTLEGROUND_BRT: return "Blackrock Throne Deathmatch";
        case BATTLEGROUND_OBC: return "Obsidian Colosseum";
        case BATTLEGROUND_BFG: return "Battle for Gilneas";
        case BATTLEGROUND_AV: return "Alterac Valley";
        case BATTLEGROUND_SA: return "Strand of the Ancients";
        case BATTLEGROUND_IC: return "Isle of Conquest";
        case BATTLEGROUND_NA: return "Nagrand Arena";
        case BATTLEGROUND_BE: return "Blade's Edge Arena";
        case BATTLEGROUND_RL: return "Ruins of Lordaeron";
        case BATTLEGROUND_NL: return "Nefarian's Arena";
        case BATTLEGROUND_TV: return "Tol'Viron Arena";
        case BATTLEGROUND_TTP: return "Tiger's Peak";
        default: return "Unknown";
    }
}

char const* WeatherName(BattlegroundCustomWeather weather)
{
    switch (weather)
    {
        case BattlegroundCustomWeather::Normal: return "Normal";
        case BattlegroundCustomWeather::Clear: return "Always clear";
        case BattlegroundCustomWeather::Rain: return "Always raining";
        case BattlegroundCustomWeather::Snow: return "Always snowing";
        case BattlegroundCustomWeather::Sandstorm: return "Always sandstorm";
        case BattlegroundCustomWeather::Thunderstorm: return "Always thunderstorm";
        case BattlegroundCustomWeather::Fog: return "Always foggy";
        default: return "Normal";
    }
}

void Notify(Player* player, std::string const& message)
{
    if (player && player->GetSession())
        player->GetSession()->SendNotification("%s", message.c_str());
}

class CustomGameLobbyManager
{
public:
    static CustomGameLobbyManager& Instance()
    {
        static CustomGameLobbyManager manager;
        return manager;
    }

    CustomGameLobby* GetLobby(Player const* player)
    {
        if (!player)
            return nullptr;

        uint32 instanceId = player->GetWorldSubMapInstanceId(CUSTOM_GAME_MAP_ID);
        auto itr = _lobbies.find(instanceId);
        return itr == _lobbies.end() ? nullptr : itr->second.get();
    }

    bool CreateLobby(Player* owner)
    {
        if (!owner || GetLobby(owner) || owner->InBattleground() || owner->InBattlegroundQueue())
            return false;

        Group* group = owner->GetGroup();
        if (group && !group->IsLeader(owner->GetGUID()))
        {
            Notify(owner, "You must be the party or raid leader to bring an existing group into a custom game.");
            return false;
        }

        bool const importGroup = group != nullptr;

        Map* lobbyMap = sMapMgr->CreateWorldSubMap(CUSTOM_GAME_MAP_ID);
        if (!lobbyMap)
        {
            Notify(owner, "The custom lobby instance could not be created.");
            return false;
        }

        auto lobby = std::make_unique<CustomGameLobby>();
        lobby->InstanceId = lobbyMap->GetInstanceId();
        lobby->OwnerGuid = owner->GetGUID();

        auto addMember = [&](Player* member, uint8 subgroup)
        {
            if (!member || member->InBattleground() || member->InBattlegroundQueue() || playerbot::IsManagedRandomBot(member))
                return;

            LobbyMember snapshot;
            snapshot.ReturnLocation = WorldLocation(member->GetMapId(), member->GetPositionX(), member->GetPositionY(), member->GetPositionZ(), member->GetOrientation());
            snapshot.OriginalSubgroup = subgroup;
            lobby->Members.emplace(member->GetGUID(), snapshot);
        };

        if (importGroup)
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                addMember(ObjectAccessor::FindConnectedPlayer(slot.guid), slot.group);
        else
            addMember(owner, 0);

        if (lobby->Members.find(owner->GetGUID()) == lobby->Members.end())
        {
            sMapMgr->DestroyWorldSubMap(CUSTOM_GAME_MAP_ID, lobby->InstanceId);
            Notify(owner, "No eligible players were found for the custom lobby.");
            return false;
        }

        uint32 const instanceId = lobby->InstanceId;
        _lobbies.emplace(instanceId, std::move(lobby));

        lobbyMap->SummonCreature(CUSTOM_GAME_BLUE_ENTRY, BlueNpcPosition);
        lobbyMap->SummonCreature(CUSTOM_GAME_RED_ENTRY, RedNpcPosition);
        lobbyMap->SummonCreature(CUSTOM_GAME_CHROMIE_ENTRY, ChromieNpcPosition);

        std::vector<ObjectGuid> memberGuids;
        for (auto const& [guid, member] : _lobbies[instanceId]->Members)
            memberGuids.push_back(guid);

        if (importGroup)
            group->Disband();

        for (ObjectGuid guid : memberGuids)
            if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
            {
                member->SetWorldSubMap(CUSTOM_GAME_MAP_ID, instanceId);
                member->TeleportTo(CUSTOM_GAME_MAP_ID, LobbyArrival.GetPositionX(), LobbyArrival.GetPositionY(),
                    LobbyArrival.GetPositionZ(), LobbyArrival.GetOrientation());
            }

        return true;
    }

    void SetTeam(Player* player, uint32 team)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId || (team != ALLIANCE && team != HORDE))
            return;

        lobby->Teams[player->GetGUID()] = team;
        ApplyTeamVisual(player, team);
        Notify(player, std::string("You joined team ") + TeamName(team) + ".");
    }

    void LeaveTeam(Player* player)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId)
            return;

        lobby->Teams.erase(player->GetGUID());
        ApplyTeamVisual(player, 0);
        Notify(player, "You are now unteamed and will spectate when the match starts.");
    }

    void SetOriginalPartyTeam(Player* player, uint32 team, bool add)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId)
            return;

        auto memberItr = lobby->Members.find(player->GetGUID());
        if (memberItr == lobby->Members.end())
            return;

        for (auto const& [guid, member] : lobby->Members)
        {
            if (member.OriginalSubgroup != memberItr->second.OriginalSubgroup)
                continue;

            if (add)
                lobby->Teams[guid] = team;
            else if (auto teamItr = lobby->Teams.find(guid); teamItr != lobby->Teams.end() && teamItr->second == team)
                lobby->Teams.erase(teamItr);

            if (Player* cohortMember = ObjectAccessor::FindConnectedPlayer(guid))
                ApplyTeamVisual(cohortMember, add ? team : 0);
        }
    }

    bool ChangeCloneRequest(Player* player, uint32 team, std::string name, bool playerbotClone, bool add)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId || !normalizePlayerName(name))
            return false;

        if (!add)
        {
            auto itr = std::find_if(lobby->CloneRequests.begin(), lobby->CloneRequests.end(), [&](CloneRequest const& request)
            {
                return request.SourceName == name && request.Team == team && request.IsPlayerbot == playerbotClone;
            });
            if (itr != lobby->CloneRequests.end())
                lobby->CloneRequests.erase(itr);
            return true;
        }

        Player* source = ObjectAccessor::FindConnectedPlayerByName(name);
        if (!source)
        {
            Notify(player, "That source character must be online.");
            return false;
        }

        bool const managedBot = playerbot::IsManagedRandomBot(source);
        if (playerbotClone != managedBot)
        {
            Notify(player, playerbotClone ? "That character is not a managed playerbot." : "Use the playerbot-copy option for that character.");
            return false;
        }

        auto matches = [&](CloneRequest const& request)
        {
            return request.SourceGuid == source->GetGUID() && request.Team == team && request.IsPlayerbot == playerbotClone;
        };

        auto itr = std::find_if(lobby->CloneRequests.begin(), lobby->CloneRequests.end(), matches);
        if (itr == lobby->CloneRequests.end())
            lobby->CloneRequests.push_back({ source->GetGUID(), source->GetName(), team, playerbotClone });

        return true;
    }

    bool IsOwner(Player const* player, CustomGameLobby const* lobby) const
    {
        return player && lobby && player->GetGUID() == lobby->OwnerGuid;
    }

    void SelectBattleground(Player* player, BattlegroundTypeId type, uint8 arenaType)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!IsOwner(player, lobby) || lobby->ActiveBattlegroundId)
            return;

        lobby->SelectedType = type;
        lobby->ArenaType = arenaType;
    }

    bool SetRule(Player* player, uint32 action, std::string const& text)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!IsOwner(player, lobby) || lobby->ActiveBattlegroundId)
            return false;

        uint32 value = 0;
        try { value = uint32(std::stoul(text)); }
        catch (...) { return false; }

        switch (action)
        {
            case ACTION_RULE_FLAG_CAPS: lobby->Rules.FlagCaptureLimit = value ? std::clamp(value, 1u, 100u) : 0; break;
            case ACTION_RULE_RESOURCES: lobby->Rules.ResourceLimit = value ? std::clamp(value, 100u, 100000u) : 0; break;
            case ACTION_RULE_RESOURCE_RATE: lobby->Rules.ResourceGainPercent = std::clamp(value, 25u, 1000u); break;
            case ACTION_RULE_KILLS: lobby->Rules.DeathmatchKillLimit = value ? std::clamp(value, 1u, 1000u) : 0; break;
            case ACTION_RULE_REZ_TIME: lobby->Rules.ResurrectionIntervalMs = value ? std::clamp(value, 5u, 120u) * IN_MILLISECONDS : 0; break;
            case ACTION_RULE_NODE_FLAG_TIME: lobby->Rules.NodeFlagCaptureTimeMs = value ? std::clamp(value, 1u, 120u) * IN_MILLISECONDS : 0; break;
            case ACTION_RULE_NODE_BASE_TIME: lobby->Rules.NodeBaseCaptureTimeMs = value ? std::clamp(value, 1u, 600u) * IN_MILLISECONDS : 0; break;
            default: return false;
        }
        return true;
    }

    bool ToggleRule(Player* player, uint32 action)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!IsOwner(player, lobby) || lobby->ActiveBattlegroundId)
            return false;

        switch (action)
        {
            case ACTION_TOGGLE_ENEMY_FLAG:
                lobby->Rules.ShowEnemyFlagOnMap = !lobby->Rules.ShowEnemyFlagOnMap;
                break;
            case ACTION_TOGGLE_ALLY_FLAG:
                lobby->Rules.ShowAllyFlagOnMap = !lobby->Rules.ShowAllyFlagOnMap;
                break;
            case ACTION_CYCLE_WEATHER:
            {
                uint8 next = uint8(lobby->Rules.Weather) + 1;
                if (next >= uint8(BattlegroundCustomWeather::Max))
                    next = uint8(BattlegroundCustomWeather::Normal);
                lobby->Rules.Weather = BattlegroundCustomWeather(next);
                break;
            }
            default:
                return false;
        }

        return true;
    }

    bool StartGame(Player* owner)
    {
        CustomGameLobby* lobby = GetLobby(owner);
        if (!IsOwner(owner, lobby) || lobby->ActiveBattlegroundId)
            return false;

        uint32 blueCount = 0;
        uint32 redCount = 0;
        for (auto const& [guid, team] : lobby->Teams)
            if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
                if (member->GetMapId() == CUSTOM_GAME_MAP_ID && member->GetInstanceId() == lobby->InstanceId)
                    team == ALLIANCE ? ++blueCount : ++redCount;
        for (CloneRequest const& request : lobby->CloneRequests)
            request.Team == ALLIANCE ? ++blueCount : ++redCount;

        if (!blueCount || !redCount)
        {
            Notify(owner, "Both teams need at least one player or requested copy.");
            return false;
        }

        if (blueCount > CUSTOM_GAME_MAX_PLAYERS_PER_TEAM || redCount > CUSTOM_GAME_MAX_PLAYERS_PER_TEAM)
        {
            Notify(owner, "Custom games support up to 40 participants per team.");
            return false;
        }

        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(lobby->SelectedType);
        PvPDifficultyEntry const* bracket = bgTemplate ? GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), owner->GetLevel()) : nullptr;
        if (!bracket && bgTemplate)
            bracket = GetBattlegroundBracketById(bgTemplate->GetMapId(), BG_BRACKET_ID_FIRST);
        if (!bracket)
        {
            Notify(owner, "That battleground has no bracket for your level.");
            return false;
        }

        Battleground* bg = sBattlegroundMgr->CreateNewBattleground(lobby->SelectedType, bracket, lobby->ArenaType, false, true);
        if (!bg)
        {
            Notify(owner, "The private battleground could not be created.");
            return false;
        }

        bg->ConfigureCustomGame(lobby->Rules);
        // Custom matches enter directly and never use the public queue's arena
        // or battleground population requirements. Forty is the underlying
        // battleground-raid limit, so asymmetric matches such as 40v4 work.
        bg->SetMaxPlayersPerTeam(CUSTOM_GAME_MAX_PLAYERS_PER_TEAM);
        bg->SetMaxPlayers(CUSTOM_GAME_MAX_PLAYERS_PER_TEAM * 2);
        bg->SetMinPlayersPerTeam(0);
        bg->SetMinPlayers(0);
        bg->StartBattleground();

        lobby->ActiveBattlegroundId = bg->GetInstanceID();
        lobby->ClonesSpawned = false;

        for (auto const& [guid, member] : lobby->Members)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player || player->GetMapId() != CUSTOM_GAME_MAP_ID || player->GetInstanceId() != lobby->InstanceId)
                continue;

            ApplyTeamVisual(player, 0);
            player->SetBattlegroundEntryPoint();

            auto teamItr = lobby->Teams.find(guid);
            if (teamItr == lobby->Teams.end())
            {
                player->SetIsSpectator(true);
                player->SetUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
                player->SetVisible(false);
                player->SetPendingSpectatorForBG(bg->GetInstanceID());
                player->SetBattlegroundId(bg->GetInstanceID(), bg->GetTypeID(), 0, true, false, TEAM_NEUTRAL);
                player->SetBGTeam(0);
                sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bg->GetTypeID());
                continue;
            }

            uint32 const team = teamItr->second;
            BattlegroundQueueTypeId const queueType = BattlegroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
            uint32 const queueSlot = player->AddBattlegroundQueueId(queueType);
            if (queueSlot >= PLAYER_MAX_BATTLEGROUND_QUEUES)
                continue;

            player->SetInviteForBattlegroundQueueType(queueType, bg->GetInstanceID());
            player->SetBGTeam(team);
            player->SetBattlegroundId(bg->GetInstanceID(), bg->GetTypeID(), queueSlot, true, false, Battleground::GetTeamIndexByTeamId(team));
            bg->IncreaseInvitedCount(team);

            WorldPacket status;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&status, bg, queueSlot, STATUS_WAIT_JOIN, 0, 0, bg->GetArenaType(), team);
            player->SendDirectMessage(&status);
            sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bg->GetTypeID());
        }

        return true;
    }

    void CloseLobby(Player* owner)
    {
        CustomGameLobby* lobby = GetLobby(owner);
        if (!IsOwner(owner, lobby) || lobby->ActiveBattlegroundId)
            return;

        lobby->Closing = true;
        for (auto const& [guid, member] : lobby->Members)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            {
                ApplyTeamVisual(player, 0);
                player->ClearWorldSubMap();
                player->TeleportTo(member.ReturnLocation);
            }
    }

    void OnMapChanged(Player* player)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby)
            return;

        if (player->GetMapId() == CUSTOM_GAME_MAP_ID && player->GetInstanceId() == lobby->InstanceId)
        {
            if (lobby->ActiveBattlegroundId)
            {
                Battleground* bg = sBattlegroundMgr->GetBattleground(lobby->ActiveBattlegroundId, lobby->SelectedType);
                if (bg && (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS) &&
                    player->GetBattlegroundId() != lobby->ActiveBattlegroundId)
                {
                    RemoveLobbyMember(player, *lobby, true);
                    return;
                }
            }

            player->SetVisible(true);
            player->RemoveUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
            if (player->IsSpectator())
                player->SetIsSpectator(false);

            auto teamItr = lobby->Teams.find(player->GetGUID());
            ApplyTeamVisual(player, teamItr == lobby->Teams.end() ? 0 : teamItr->second);
            return;
        }

        if (lobby->ActiveBattlegroundId && player->GetBattlegroundId() == lobby->ActiveBattlegroundId)
        {
            Battleground* bg = sBattlegroundMgr->GetBattleground(lobby->ActiveBattlegroundId, lobby->SelectedType);
            if (bg && player->GetMapId() == bg->GetMapId() && player->GetInstanceId() == bg->GetInstanceID())
                return;

            // A direct teleport can move a player out without first sending
            // the normal leave opcode. Remove that character from the combat
            // roster as well as the retained lobby roster.
            if (bg && (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS))
                bg->RemovePlayerAtLeave(player->GetGUID(), false, true);
        }

        // A teleport out of the staging box is an individual lobby leave. Do
        // not pull that character back, and tear the lobby down when the last
        // retained member has left.
        RemoveLobbyMember(player, *lobby, false);
    }

    void OnPlayerUpdate(Player* player)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || player->GetMapId() != CUSTOM_GAME_MAP_ID || player->GetInstanceId() != lobby->InstanceId)
            return;

        if (player->GetDistance(LobbyArrival) > LOBBY_MAX_DISTANCE || player->GetPositionZ() < -80.0f || player->GetPositionZ() > -45.0f)
            player->NearTeleportTo(LobbyArrival.GetPositionX(), LobbyArrival.GetPositionY(), LobbyArrival.GetPositionZ(), LobbyArrival.GetOrientation());
    }

    void OnLogout(Player* player)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby)
            return;

        RemoveLobbyMember(player, *lobby, false);
    }

    void OnLogin(Player* player)
    {
        if (!player)
            return;

        for (auto const& [instanceId, lobby] : _lobbies)
            if (!lobby->Closing && lobby->Members.find(player->GetGUID()) != lobby->Members.end())
            {
                player->SetWorldSubMap(CUSTOM_GAME_MAP_ID, instanceId);
                player->TeleportTo(CUSTOM_GAME_MAP_ID, LobbyArrival.GetPositionX(), LobbyArrival.GetPositionY(),
                    LobbyArrival.GetPositionZ(), LobbyArrival.GetOrientation());
                return;
            }
    }

    void Update()
    {
        std::vector<uint32> eraseIds;
        std::vector<uint32> completedMatchIds;
        for (auto& [instanceId, lobbyPtr] : _lobbies)
        {
            CustomGameLobby& lobby = *lobbyPtr;
            if (lobby.Closing && lobby.Members.empty() && lobby.ActiveBattlegroundId)
            {
                playerbot::PlayerbotObcCloneManager::DestroyCustomGameClones(lobby.ActiveBattlegroundId);
                if (Battleground* bg = sBattlegroundMgr->GetBattleground(lobby.ActiveBattlegroundId, lobby.SelectedType))
                    if (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS)
                        bg->EndBattleground(PVP_TEAM_NEUTRAL);
                lobby.ActiveBattlegroundId = 0;
                lobby.ClonesSpawned = false;
            }

            if (lobby.ActiveBattlegroundId)
            {
                Battleground* bg = sBattlegroundMgr->GetBattleground(lobby.ActiveBattlegroundId, lobby.SelectedType);
                if (bg && bg->FindBgMap() && !lobby.ClonesSpawned)
                {
                    for (CloneRequest const& request : lobby.CloneRequests)
                        if (Player* source = ObjectAccessor::FindConnectedPlayer(request.SourceGuid))
                            playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, request.Team,
                                request.IsPlayerbot ? "Echo " : "Dark ");
                    lobby.ClonesSpawned = true;
                }

                if (!bg || bg->GetStatus() == STATUS_NONE || bg->GetStatus() == STATUS_WAIT_LEAVE)
                {
                    completedMatchIds.push_back(instanceId);
                    continue;
                }
            }

            if (lobby.Closing)
            {
                for (auto const& [guid, member] : lobby.Members)
                    if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                        if (player->HasWorldSubMap())
                        {
                            player->ClearWorldSubMap();
                            player->TeleportTo(member.ReturnLocation);
                        }

                Map* map = sMapMgr->FindMap(CUSTOM_GAME_MAP_ID, instanceId);
                if (!map || !map->HavePlayers())
                {
                    if (map)
                        sMapMgr->DestroyWorldSubMap(CUSTOM_GAME_MAP_ID, instanceId);
                    eraseIds.push_back(instanceId);
                }
            }
        }

        for (uint32 instanceId : completedMatchIds)
            RecreateLobbyAfterMatch(instanceId);

        for (uint32 instanceId : eraseIds)
            _lobbies.erase(instanceId);
    }

private:
    void RemoveLobbyMember(Player* player, CustomGameLobby& lobby, bool returnToOriginal)
    {
        if (!player)
            return;

        auto memberItr = lobby.Members.find(player->GetGUID());
        if (memberItr == lobby.Members.end())
            return;

        WorldLocation const returnLocation = memberItr->second.ReturnLocation;
        bool const wasOwner = lobby.OwnerGuid == player->GetGUID();
        lobby.Members.erase(memberItr);
        lobby.Teams.erase(player->GetGUID());

        ApplyTeamVisual(player, 0);
        player->SetVisible(true);
        player->RemoveUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
        if (player->IsSpectator())
            player->SetIsSpectator(false);
        player->ClearWorldSubMap();

        if (returnToOriginal)
            player->TeleportTo(returnLocation);

        if (wasOwner && !lobby.Members.empty())
            lobby.OwnerGuid = lobby.Members.begin()->first;
        if (lobby.Members.empty())
            lobby.Closing = true;
    }

    void RecreateLobbyAfterMatch(uint32 oldInstanceId)
    {
        auto lobbyItr = _lobbies.find(oldInstanceId);
        if (lobbyItr == _lobbies.end())
            return;

        CustomGameLobby& lobby = *lobbyItr->second;
        uint32 const battlegroundId = lobby.ActiveBattlegroundId;
        Battleground* bg = sBattlegroundMgr->GetBattleground(battlegroundId, lobby.SelectedType);
        playerbot::PlayerbotObcCloneManager::DestroyCustomGameClones(battlegroundId);

        std::vector<ObjectGuid> returningGuids;
        for (auto const& [guid, member] : lobby.Members)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                if (player->GetBattlegroundId() == battlegroundId)
                    returningGuids.push_back(guid);

        if (returningGuids.empty())
        {
            lobby.Members.clear();
            lobby.Teams.clear();
            lobby.ActiveBattlegroundId = 0;
            lobby.Closing = true;
            return;
        }

        Map* newLobbyMap = sMapMgr->CreateWorldSubMap(CUSTOM_GAME_MAP_ID);
        if (!newLobbyMap)
        {
            for (ObjectGuid guid : returningGuids)
                if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                {
                    if (bg)
                        bg->RemovePlayerAtLeave(guid, false, true);
                    player->ClearWorldSubMap();
                    player->TeleportTo(lobby.Members.at(guid).ReturnLocation);
                }
            lobby.ActiveBattlegroundId = 0;
            lobby.Closing = true;
            return;
        }

        uint32 const newInstanceId = newLobbyMap->GetInstanceId();
        std::unordered_map<ObjectGuid, LobbyMember> returningMembers;
        std::unordered_map<ObjectGuid, uint32> returningTeams;
        for (ObjectGuid guid : returningGuids)
        {
            returningMembers.emplace(guid, lobby.Members.at(guid));
            if (auto teamItr = lobby.Teams.find(guid); teamItr != lobby.Teams.end())
                returningTeams.emplace(guid, teamItr->second);
        }

        lobby.Members = std::move(returningMembers);
        lobby.Teams = std::move(returningTeams);
        lobby.InstanceId = newInstanceId;
        lobby.ActiveBattlegroundId = 0;
        lobby.ClonesSpawned = false;
        lobby.Closing = false;
        if (lobby.Members.find(lobby.OwnerGuid) == lobby.Members.end())
            lobby.OwnerGuid = lobby.Members.begin()->first;

        auto lobbyNode = _lobbies.extract(lobbyItr);
        lobbyNode.key() = newInstanceId;
        _lobbies.insert(std::move(lobbyNode));

        newLobbyMap->SummonCreature(CUSTOM_GAME_BLUE_ENTRY, BlueNpcPosition);
        newLobbyMap->SummonCreature(CUSTOM_GAME_RED_ENTRY, RedNpcPosition);
        newLobbyMap->SummonCreature(CUSTOM_GAME_CHROMIE_ENTRY, ChromieNpcPosition);

        for (ObjectGuid guid : returningGuids)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            {
                if (bg)
                    bg->RemovePlayerAtLeave(guid, false, true);
                else
                {
                    player->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE);
                    player->SetBGTeam(0);
                    player->SetVisible(true);
                    player->RemoveUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
                    if (player->IsSpectator())
                        player->SetIsSpectator(false);
                }

                player->SetWorldSubMap(CUSTOM_GAME_MAP_ID, newInstanceId);
                player->TeleportTo(CUSTOM_GAME_MAP_ID, LobbyArrival.GetPositionX(), LobbyArrival.GetPositionY(),
                    LobbyArrival.GetPositionZ(), LobbyArrival.GetOrientation());
            }

        if (Map* oldLobbyMap = sMapMgr->FindMap(CUSTOM_GAME_MAP_ID, oldInstanceId))
            if (!oldLobbyMap->HavePlayers())
                sMapMgr->DestroyWorldSubMap(CUSTOM_GAME_MAP_ID, oldInstanceId);
    }

    static void ApplyTeamVisual(Player* player, uint32 team)
    {
        if (!player)
            return;
        player->RemoveAurasDueToSpell(BLUE_FLAG_VISUAL);
        player->RemoveAurasDueToSpell(RED_FLAG_VISUAL);
        if (team == ALLIANCE)
            player->CastSpell(player, BLUE_FLAG_VISUAL, true);
        else if (team == HORDE)
            player->CastSpell(player, RED_FLAG_VISUAL, true);
    }

    std::unordered_map<uint32, std::unique_ptr<CustomGameLobby>> _lobbies;
};

class custom_game_lobby_npc : public CreatureScript
{
public:
    custom_game_lobby_npc() : CreatureScript("custom_game_lobby_npc") { }

    struct custom_game_lobby_npcAI : public ScriptedAI
    {
        custom_game_lobby_npcAI(Creature* creature) : ScriptedAI(creature) { }

        void ShowTeamMenu(Player* player, uint32 team)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby)
                return;

            uint32 realCount = 0;
            uint32 copyCount = 0;
            for (auto const& [guid, assignedTeam] : lobby->Teams)
                if (assignedTeam == team)
                    ++realCount;
            for (CloneRequest const& request : lobby->CloneRequests)
                if (request.Team == team)
                    ++copyCount;
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                std::string(TeamName(team)) + " roster: " + std::to_string(realCount) + " players, " + std::to_string(copyCount) + " copies",
                team, ACTION_STATUS);

            auto itr = lobby->Teams.find(player->GetGUID());
            if (itr != lobby->Teams.end() && itr->second == team)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Leave team", team, ACTION_LEAVE_TEAM);
            else
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, std::string("Join team ") + TeamName(team), team, ACTION_JOIN_TEAM);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Add my original party", team, ACTION_ADD_PARTY);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Remove my original party", team, ACTION_REMOVE_PARTY);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Add playerbot copy...", team, ACTION_ADD_BOT, "Enter the online playerbot name", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Remove playerbot copy...", team, ACTION_REMOVE_BOT, "Enter the playerbot name", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Add Dark player copy...", team, ACTION_ADD_DARK, "Enter the online player name", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Remove Dark player copy...", team, ACTION_REMOVE_DARK, "Enter the player name", 0, true);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowChromieMenu(Player* player)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby)
                return;

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Selected: " + BattlegroundName(*lobby), GOSSIP_SENDER_MAIN, ACTION_STATUS);
            if (!CustomGameLobbyManager::Instance().IsOwner(player, lobby))
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Only the lobby owner can configure or start the match.", GOSSIP_SENDER_MAIN, ACTION_STATUS);
                SendGossipMenuFor(player, 1, me->GetGUID());
                return;
            }

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Warsong Gulch", GOSSIP_SENDER_MAIN, ACTION_SELECT_WS);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Twin Peaks", GOSSIP_SENDER_MAIN, ACTION_SELECT_TP);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Arathi Basin", GOSSIP_SENDER_MAIN, ACTION_SELECT_AB);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Eye of the Storm", GOSSIP_SENDER_MAIN, ACTION_SELECT_EY);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Scarlet Chapel Deathmatch", GOSSIP_SENDER_MAIN, ACTION_SELECT_SCM);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Blackrock Throne Deathmatch", GOSSIP_SENDER_MAIN, ACTION_SELECT_BRT);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Obsidian Colosseum", GOSSIP_SENDER_MAIN, ACTION_SELECT_OBC);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Battle for Gilneas", GOSSIP_SENDER_MAIN, ACTION_SELECT_BFG);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Alterac Valley", GOSSIP_SENDER_MAIN, ACTION_SELECT_AV);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Strand of the Ancients", GOSSIP_SENDER_MAIN, ACTION_SELECT_SA);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Isle of Conquest", GOSSIP_SENDER_MAIN, ACTION_SELECT_IC);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Nagrand Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_NA);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Blade's Edge Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_BE);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Ruins of Lordaeron", GOSSIP_SENDER_MAIN, ACTION_SELECT_RL);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Nefarian's Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_NL);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Tol'Viron Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_TV);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Tiger's Peak", GOSSIP_SENDER_MAIN, ACTION_SELECT_TTP);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Flag captures: " + std::to_string(lobby->Rules.FlagCaptureLimit) + " (0 = default)", GOSSIP_SENDER_MAIN, ACTION_RULE_FLAG_CAPS, "Winning flag captures", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Resource limit: " + std::to_string(lobby->Rules.ResourceLimit) + " (0 = default)", GOSSIP_SENDER_MAIN, ACTION_RULE_RESOURCES, "Winning resource total", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Resource gain: " + std::to_string(lobby->Rules.ResourceGainPercent) + "%", GOSSIP_SENDER_MAIN, ACTION_RULE_RESOURCE_RATE, "Percent (100 is normal)", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Deathmatch kills: " + std::to_string(lobby->Rules.DeathmatchKillLimit) + " (0 = default)", GOSSIP_SENDER_MAIN, ACTION_RULE_KILLS, "Winning kill total", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Resurrection: " + std::to_string(lobby->Rules.ResurrectionIntervalMs / IN_MILLISECONDS) + " sec (0 = default)", GOSSIP_SENDER_MAIN, ACTION_RULE_REZ_TIME, "Seconds (5-120)", 0, true);
            if (lobby->SelectedType == BATTLEGROUND_AB || lobby->SelectedType == BATTLEGROUND_BFG)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Flag interaction: " + std::to_string(lobby->Rules.NodeFlagCaptureTimeMs / IN_MILLISECONDS) + " sec (0 = default)",
                    GOSSIP_SENDER_MAIN, ACTION_RULE_NODE_FLAG_TIME, "Seconds (1-120)", 0, true);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Base capture after assault: " + std::to_string(lobby->Rules.NodeBaseCaptureTimeMs / IN_MILLISECONDS) + " sec (0 = default)",
                    GOSSIP_SENDER_MAIN, ACTION_RULE_NODE_BASE_TIME, "Seconds (1-600)", 0, true);
            }
            if (lobby->SelectedType == BATTLEGROUND_WS || lobby->SelectedType == BATTLEGROUND_TP)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    std::string("Enemy team's flag on map: ") + (lobby->Rules.ShowEnemyFlagOnMap ? "Visible" : "Hidden"),
                    GOSSIP_SENDER_MAIN, ACTION_TOGGLE_ENEMY_FLAG);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    std::string("Your team's flag on map: ") + (lobby->Rules.ShowAllyFlagOnMap ? "Visible" : "Hidden"),
                    GOSSIP_SENDER_MAIN, ACTION_TOGGLE_ALLY_FLAG);
            }
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, std::string("Weather: ") + WeatherName(lobby->Rules.Weather) + " (click to change)",
                GOSSIP_SENDER_MAIN, ACTION_CYCLE_WEATHER);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "START PRIVATE GAME", GOSSIP_SENDER_MAIN, ACTION_START, "Start with the current teams and rules?", 0, false);
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Close lobby and return everyone", GOSSIP_SENDER_MAIN, ACTION_CLOSE, "Close this custom lobby?", 0, false);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        bool OnGossipHello(Player* player) override
        {
            player->PlayerTalkClass->ClearMenus();
            if (me->GetEntry() == CUSTOM_GAME_HOST_ENTRY)
            {
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Create a private custom game", GOSSIP_SENDER_MAIN, ACTION_CREATE);
                SendGossipMenuFor(player, 1, me->GetGUID());
            }
            else if (me->GetEntry() == CUSTOM_GAME_BLUE_ENTRY)
                ShowTeamMenu(player, ALLIANCE);
            else if (me->GetEntry() == CUSTOM_GAME_RED_ENTRY)
                ShowTeamMenu(player, HORDE);
            else
                ShowChromieMenu(player);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32, uint32 gossipListId) override
        {
            uint32 action = GetGossipActionFor(player, gossipListId);
            uint32 sender = GetGossipSenderFor(player, gossipListId);
            player->PlayerTalkClass->ClearMenus();
            auto& manager = CustomGameLobbyManager::Instance();

            switch (action)
            {
                case ACTION_CREATE: manager.CreateLobby(player); break;
                case ACTION_JOIN_TEAM: manager.SetTeam(player, sender); ShowTeamMenu(player, sender); return true;
                case ACTION_LEAVE_TEAM: manager.LeaveTeam(player); ShowTeamMenu(player, sender); return true;
                case ACTION_ADD_PARTY: manager.SetOriginalPartyTeam(player, sender, true); ShowTeamMenu(player, sender); return true;
                case ACTION_REMOVE_PARTY: manager.SetOriginalPartyTeam(player, sender, false); ShowTeamMenu(player, sender); return true;
                case ACTION_SELECT_WS: manager.SelectBattleground(player, BATTLEGROUND_WS, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_TP: manager.SelectBattleground(player, BATTLEGROUND_TP, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_AB: manager.SelectBattleground(player, BATTLEGROUND_AB, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_EY: manager.SelectBattleground(player, BATTLEGROUND_EY, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_SCM: manager.SelectBattleground(player, BATTLEGROUND_SCM, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_BRT: manager.SelectBattleground(player, BATTLEGROUND_BRT, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_OBC: manager.SelectBattleground(player, BATTLEGROUND_OBC, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_BFG: manager.SelectBattleground(player, BATTLEGROUND_BFG, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_AV: manager.SelectBattleground(player, BATTLEGROUND_AV, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_SA: manager.SelectBattleground(player, BATTLEGROUND_SA, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_IC: manager.SelectBattleground(player, BATTLEGROUND_IC, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_NA: manager.SelectBattleground(player, BATTLEGROUND_NA, ARENA_TYPE_5v5); ShowChromieMenu(player); return true;
                case ACTION_SELECT_BE: manager.SelectBattleground(player, BATTLEGROUND_BE, ARENA_TYPE_5v5); ShowChromieMenu(player); return true;
                case ACTION_SELECT_RL: manager.SelectBattleground(player, BATTLEGROUND_RL, ARENA_TYPE_5v5); ShowChromieMenu(player); return true;
                case ACTION_SELECT_NL: manager.SelectBattleground(player, BATTLEGROUND_NL, ARENA_TYPE_5v5); ShowChromieMenu(player); return true;
                case ACTION_SELECT_TV: manager.SelectBattleground(player, BATTLEGROUND_TV, ARENA_TYPE_5v5); ShowChromieMenu(player); return true;
                case ACTION_SELECT_TTP: manager.SelectBattleground(player, BATTLEGROUND_TTP, ARENA_TYPE_5v5); ShowChromieMenu(player); return true;
                case ACTION_TOGGLE_ENEMY_FLAG:
                case ACTION_TOGGLE_ALLY_FLAG:
                case ACTION_CYCLE_WEATHER:
                    manager.ToggleRule(player, action);
                    ShowChromieMenu(player);
                    return true;
                case ACTION_START: manager.StartGame(player); break;
                case ACTION_CLOSE: manager.CloseLobby(player); break;
                case ACTION_STATUS:
                    if (me->GetEntry() == CUSTOM_GAME_CHROMIE_ENTRY)
                        ShowChromieMenu(player);
                    else
                        ShowTeamMenu(player, sender);
                    return true;
                default: break;
            }

            CloseGossipMenuFor(player);
            return true;
        }

        bool OnGossipSelectCode(Player* player, uint32, uint32 gossipListId, char const* code) override
        {
            uint32 action = GetGossipActionFor(player, gossipListId);
            uint32 sender = GetGossipSenderFor(player, gossipListId);
            player->PlayerTalkClass->ClearMenus();
            auto& manager = CustomGameLobbyManager::Instance();
            std::string value = code ? code : "";

            if (action >= ACTION_RULE_FLAG_CAPS && action <= ACTION_RULE_NODE_BASE_TIME)
            {
                manager.SetRule(player, action, value);
                ShowChromieMenu(player);
                return true;
            }
            else
            {
                bool const add = action == ACTION_ADD_BOT || action == ACTION_ADD_DARK;
                bool const bot = action == ACTION_ADD_BOT || action == ACTION_REMOVE_BOT;
                manager.ChangeCloneRequest(player, sender, value, bot, add);
                ShowTeamMenu(player, sender);
                return true;
            }
        }
    };

    CreatureAI* GetAI(Creature* creature) const override { return new custom_game_lobby_npcAI(creature); }
};

class custom_game_lobby_player_script : public PlayerScript
{
public:
    custom_game_lobby_player_script() : PlayerScript("custom_game_lobby_player_script") { }
    void OnLogin(Player* player, bool) override { CustomGameLobbyManager::Instance().OnLogin(player); }
    void OnMapChanged(Player* player) override { CustomGameLobbyManager::Instance().OnMapChanged(player); }
    void OnUpdate(Player* player, uint32) override { CustomGameLobbyManager::Instance().OnPlayerUpdate(player); }
    void OnLogout(Player* player) override { CustomGameLobbyManager::Instance().OnLogout(player); }
};

class custom_game_lobby_world_script : public WorldScript
{
public:
    custom_game_lobby_world_script() : WorldScript("custom_game_lobby_world_script") { }
    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < 500)
            return;
        _timer = 0;
        CustomGameLobbyManager::Instance().Update();
    }

private:
    uint32 _timer = 0;
};
}

void AddSC_custom_game_lobby()
{
    new custom_game_lobby_npc();
    new custom_game_lobby_player_script();
    new custom_game_lobby_world_script();
}
