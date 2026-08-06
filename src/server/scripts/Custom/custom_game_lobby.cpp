/*
 * Private custom battleground/arena lobbies staged in server-only Map 1
 * subinstances. Nothing in this file is persisted across a world restart.
 */

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "GameTime.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbot/Pvp/PlayerbotObcClone.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "Playerbot/Pvp/PlayerbotResourceGovernor.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "TemporarySummon.h"
#include "Util.h"
#include "Weather.h"
#include "WorldSession.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr uint32 CUSTOM_GAME_HOST_ENTRY = 900001;
constexpr uint32 CUSTOM_GAME_BLUE_ENTRY = 900002;
constexpr uint32 CUSTOM_GAME_RED_ENTRY = 900003;
constexpr uint32 CUSTOM_GAME_CHROMIE_ENTRY = 900004;
constexpr uint32 CUSTOM_GAME_CHAIR_ENTRY = 178934;
constexpr uint32 CUSTOM_GAME_MAP_ID = 1;
constexpr uint32 CUSTOM_GAME_MAX_PLAYERS_PER_TEAM = 40;
constexpr uint32 CUSTOM_GAME_GOSSIP_PAGE_SIZE = 28;
constexpr uint32 BLUE_FLAG_VISUAL = 32609;
constexpr uint32 RED_FLAG_VISUAL = 32610;

Position const LobbyArrival = { 16218.700195f, 16403.599609f, -64.378899f, 6.283140f };
Position const BlueNpcPosition = { 16229.862305f, 16415.587891f, -64.378716f, 3.132112f };
Position const RedNpcPosition = { 16230.437500f, 16391.000000f, -64.378792f, 3.014295f };
Position const ChromieNpcPosition = { 16240.280273f, 16402.792969f, -64.378471f, 3.104628f };
Position const ChairPosition = { 16227.799805f, 16403.400391f, -64.380402f, 3.136040f };
QuaternionData const ChairRotation(0.0f, 0.0f, 0.999996f, 0.002776f);
WorldLocation const GurubashiGamesmasterLocation(0, -13235.707031f, 214.336441f, 31.276190f, 1.010225f);
constexpr float LOBBY_MAX_DISTANCE = 32.0f;

enum GossipAction : uint32
{
    ACTION_CREATE = 100,
    ACTION_STATUS = 199,
    ACTION_JOIN_TEAM = 200,
    ACTION_LEAVE_TEAM,
    ACTION_ADD_BOT,
    ACTION_ADD_RANDOM_BOT,
    ACTION_REMOVE_BOT_OR_CLONE,
    ACTION_DUPLICATE_TEAM_MEMBER,
    ACTION_DUPLICATE_HUMAN_MEMBER,
    ACTION_DUPLICATE_CLONE_MEMBER,
    ACTION_DUPLICATE_LIST_BACK,
    ACTION_DUPLICATE_PAGE = 240,
    ACTION_REMOVE_PAGE,
    ACTION_SELECT_GAME_MENU = 280,
    ACTION_SELECT_ARENA_MENU,
    ACTION_SELECT_BATTLEGROUND_MENU,
    ACTION_SELECT_GAME_BACK,
    ACTION_SELECT_LIST_BACK,
    ACTION_SELECT_WS = 300,
    ACTION_SELECT_TP,
    ACTION_SELECT_AB,
    ACTION_SELECT_EY,
    ACTION_SELECT_SCM,
    ACTION_SELECT_BRT,
    ACTION_SELECT_OBC,
    ACTION_SELECT_TRT,
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
    ACTION_SELECT_RANDOM_ARENA,
    // The ported arenas are addressed as a block instead of one constant each:
    // the action id is this base plus the arena's offset within
    // BATTLEGROUND_CUSTOM_ARENA_FIRST..LAST. Adding an arena then costs nothing
    // here, nothing in the menu builder and nothing in the select handler.
    // Occupies 330..343 today; the block above ends at 318 and the next starts
    // at 380, so there is room for 50 arenas before this needs revisiting.
    ACTION_SELECT_CUSTOM_ARENA_BASE = 330,
    ACTION_BATTLEGROUND_OPTIONS = 380,
    ACTION_BATTLEGROUND_OPTIONS_BACK,
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
    ACTION_KICK_PLAYER = 470,
    ACTION_START = 500,
    ACTION_CLOSE,
    ACTION_LEAVE_LOBBY,
    ACTION_REMOVE_LIST_BACK
};

constexpr uint32 ACTION_REMOVE_BOT_CHOICE_BASE = 1000;
constexpr uint32 ACTION_REMOVE_DARK_CHOICE_BASE = 1100;
constexpr uint32 ACTION_REMOVE_CHOICE_LIMIT = 100;

uint32 MakePagedTeamSender(uint32 team, uint32 page)
{
    return (page << 16) | (team & 0xFFFF);
}

uint32 GetPagedTeam(uint32 sender)
{
    return sender & 0xFFFF;
}

uint32 GetPagedPage(uint32 sender)
{
    return sender >> 16;
}

struct CloneRequest
{
    uint32 RosterSlotId = 0;
    ObjectGuid SourceGuid;
    std::string SourceName;
    uint32 Team = 0;
    bool IsPlayerbot = false;
};

struct LobbyMember
{
    WorldLocation ReturnLocation;
};

struct PendingLobbyInvite
{
    uint32 InstanceId = 0;
    ObjectGuid HostGuid;
    time_t ExpireTime = 0;
    bool Invalidated = false;
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
    bool RandomArena = false;
    BattlegroundCustomRules Rules;
    uint32 ActiveBattlegroundId = 0;
    BattlegroundTypeId ActiveBattlegroundType = BATTLEGROUND_TYPE_NONE;
    uint32 NextCloneRosterSlotId = 1;
    bool ClonesSpawned = false;
    bool Closing = false;
    uint32 LastBotShedNoticeMs = 0;
};

char const* TeamName(uint32 team)
{
    return team == ALLIANCE ? "Blue" : team == HORDE ? "Red" : "Spectator";
}

std::string BattlegroundNameByType(BattlegroundTypeId type)
{
    switch (type)
    {
        case BATTLEGROUND_WS: return "Warsong Gulch";
        case BATTLEGROUND_TP: return "Twin Peaks";
        case BATTLEGROUND_AB: return "Arathi Basin";
        case BATTLEGROUND_EY: return "Eye of the Storm";
        case BATTLEGROUND_SCM: return "Scarlet Chapel Deathmatch";
        case BATTLEGROUND_BRT: return "Blackrock Throne Deathmatch";
        case BATTLEGROUND_OBC: return "Obsidian Colosseum";
        case BATTLEGROUND_TRT: return "Tanaris Deathmatch";
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
        case BATTLEGROUND_CPE: return "Coliseum of Past Echoes";
        case BATTLEGROUND_IAT: return "Imperial Arena of Thakraj";
        case BATTLEGROUND_MXC: return "Maldraxxus Coliseum";
        case BATTLEGROUND_NGA: return "Nagrand Arena (Remastered)";
        case BATTLEGROUND_BEA: return "Blade's Edge Arena (Remastered)";
        case BATTLEGROUND_GDH: return "Guardian's Hall";
        case BATTLEGROUND_SOC: return "Spark of Creator";
        case BATTLEGROUND_BHA: return "Baradin Hold Arena";
        case BATTLEGROUND_OBS: return "Obelisk of the Stars";
        case BATTLEGROUND_TWN: return "The Twisting Nether";
        case BATTLEGROUND_BRH: return "Black Rook Hold Arena";
        case BATTLEGROUND_ASF: return "Ashamane's Fall";
        case BATTLEGROUND_INL: return "The Inventor's Library";
        case BATTLEGROUND_AOA: return "Amphitheater of Anguish";
        default: return "Unknown";
    }
}

std::string BattlegroundName(CustomGameLobby const& lobby)
{
    if (lobby.RandomArena)
        return "Random Arena";

    return BattlegroundNameByType(lobby.SelectedType);
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

Position LobbyClonePosition(uint32 team, uint32 teamIndex)
{
    // Five tidy columns per side, expanding away from the central aisle.
    // Forty previews still fit within the jail's enforced lobby boundary.
    uint32 const column = teamIndex % 5;
    uint32 const row = teamIndex / 5;
    constexpr float columnX[5] = { 16223.8f, 16226.4f, 16233.2f, 16235.8f, 16238.4f };
    float const x = columnX[column];
    float const y = team == ALLIANCE ? 16411.8f + 1.30f * row : 16395.2f - 1.30f * row;
    return Position(x, y, -64.3788f, 3.141593f);
}

bool IsArenaSelection(BattlegroundTypeId type)
{
    switch (type)
    {
        case BATTLEGROUND_NA:
        case BATTLEGROUND_BE:
        case BATTLEGROUND_RL:
        case BATTLEGROUND_NL:
            return true;
        default:
            return IsCustomArena(type);
    }
}

uint32 DefaultResourceLimit(BattlegroundTypeId type)
{
    switch (type)
    {
        case BATTLEGROUND_AB:
        case BATTLEGROUND_EY: return 1600;
        case BATTLEGROUND_BFG: return 2000;
        default: return 0;
    }
}

uint32 DefaultDeathmatchKillLimit(BattlegroundTypeId type)
{
    switch (type)
    {
        case BATTLEGROUND_OBC: return 50;
        default: return 30;
    }
}

std::string ConfiguredValue(uint32 configured, uint32 defaultValue)
{
    return std::to_string(configured ? configured : defaultValue) + (configured ? "" : " (default)");
}

std::string ConfiguredSeconds(uint32 configuredMs, uint32 defaultMs)
{
    uint32 const valueMs = configuredMs ? configuredMs : defaultMs;
    std::string value;
    if (valueMs % IN_MILLISECONDS)
        value = std::to_string(valueMs / IN_MILLISECONDS) + "." +
            std::to_string((valueMs % IN_MILLISECONDS) / 100) + " sec";
    else
        value = std::to_string(valueMs / IN_MILLISECONDS) + " sec";
    return value + (configuredMs ? "" : " (default)");
}

void Notify(Player* player, std::string const& message)
{
    if (player && player->GetSession())
        player->GetSession()->SendNotification("%s", message.c_str());
}

void PopulateLobbyMap(Map* map)
{
    if (!map)
        return;

    map->SummonCreature(CUSTOM_GAME_BLUE_ENTRY, BlueNpcPosition);
    map->SummonCreature(CUSTOM_GAME_RED_ENTRY, RedNpcPosition);
    map->SummonCreature(CUSTOM_GAME_CHROMIE_ENTRY, ChromieNpcPosition);

    GameObject* chair = new GameObject();
    if (!chair->Create(map->GenerateLowGuid<HighGuid::GameObject>(), CUSTOM_GAME_CHAIR_ENTRY, map,
        PHASEMASK_NORMAL, ChairPosition, ChairRotation, 0, GO_STATE_READY) || !map->AddToMap(chair))
    {
        delete chair;
        TC_LOG_ERROR("scripts.custom", "Custom-game lobby could not spawn chair entry {} in instance {}.",
            CUSTOM_GAME_CHAIR_ENTRY, map->GetInstanceId());
    }
}

class CustomGameLobbyManager
{
public:
    static CustomGameLobbyManager& Instance()
    {
        // Script-owned lobby state has process lifetime. A function-static
        // value would be destructed from __run_exit_handlers after world and
        // script teardown, where active lobby containers are no longer safe
        // to unwind. The OS reclaims this small manager allocation at exit.
        static CustomGameLobbyManager* manager = new CustomGameLobbyManager();
        return *manager;
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

        auto addMember = [&](Player* member)
        {
            if (!member || member->InBattleground() || member->InBattlegroundQueue() || playerbot::IsManagedRandomBot(member))
                return;

            LobbyMember snapshot;
            snapshot.ReturnLocation = WorldLocation(member->GetMapId(), member->GetPositionX(), member->GetPositionY(), member->GetPositionZ(), member->GetOrientation());
            lobby->Members.emplace(member->GetGUID(), snapshot);
        };

        if (importGroup)
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                addMember(ObjectAccessor::FindConnectedPlayer(slot.guid));
        else
            addMember(owner);

        if (lobby->Members.find(owner->GetGUID()) == lobby->Members.end())
        {
            sMapMgr->DestroyWorldSubMap(CUSTOM_GAME_MAP_ID, lobby->InstanceId);
            Notify(owner, "No eligible players were found for the custom lobby.");
            return false;
        }

        uint32 const instanceId = lobby->InstanceId;
        _lobbies.emplace(instanceId, std::move(lobby));

        PopulateLobbyMap(lobbyMap);

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

    bool InvitePlayer(Player* host, Player* invited)
    {
        CustomGameLobby* lobby = GetLobby(host);
        if (!IsOwner(host, lobby) || !invited || invited == host || lobby->ActiveBattlegroundId || lobby->Closing)
            return false;

        if (GetLobby(invited) || invited->IsInCustomGameLobby() || invited->InBattleground() ||
            invited->InBattlegroundQueue() || playerbot::IsManagedRandomBot(invited))
        {
            Notify(host, invited->GetName() + " cannot join this custom-game lobby.");
            return false;
        }

        auto pendingItr = _pendingInvites.find(invited->GetGUID());
        if (pendingItr != _pendingInvites.end() && pendingItr->second.ExpireTime < GameTime::GetGameTime())
            pendingItr = _pendingInvites.erase(pendingItr);
        if (pendingItr != _pendingInvites.end())
        {
            Notify(host, invited->GetName() + " already has a pending custom-game invitation.");
            return false;
        }

        _pendingInvites.emplace(invited->GetGUID(), PendingLobbyInvite{
            lobby->InstanceId, lobby->OwnerGuid, GameTime::GetGameTime() + MAX_PLAYER_SUMMON_DELAY, false });
        return true;
    }

    bool HandleSummonResponse(Player* invited, ObjectGuid summonerGuid, bool agree)
    {
        if (!invited)
            return false;

        auto inviteItr = _pendingInvites.find(invited->GetGUID());
        if (inviteItr == _pendingInvites.end() || inviteItr->second.HostGuid != summonerGuid)
            return false;

        if (inviteItr->second.ExpireTime < GameTime::GetGameTime())
        {
            _pendingInvites.erase(inviteItr);
            Notify(invited, "That custom-game invitation has expired.");
            return true;
        }

        // Retain invalidated requests until their client popup expires so a
        // late response cannot fall through into an unrelated normal summon.
        if (inviteItr->second.Invalidated)
        {
            _pendingInvites.erase(inviteItr);
            return true;
        }

        return agree ? AcceptInvite(invited) : DeclineInvite(invited);
    }

    bool AcceptInvite(Player* invited)
    {
        if (!invited)
            return false;

        auto inviteItr = _pendingInvites.find(invited->GetGUID());
        if (inviteItr == _pendingInvites.end())
            return false;

        PendingLobbyInvite const invitation = inviteItr->second;
        _pendingInvites.erase(inviteItr);

        auto lobbyItr = _lobbies.find(invitation.InstanceId);
        CustomGameLobby* lobby = lobbyItr == _lobbies.end() ? nullptr : lobbyItr->second.get();
        Player* host = ObjectAccessor::FindConnectedPlayer(invitation.HostGuid);
        if (!lobby || lobby->OwnerGuid != invitation.HostGuid || lobby->Closing ||
            lobby->ActiveBattlegroundId || !host || !IsOwner(host, lobby) ||
            host->GetMapId() != CUSTOM_GAME_MAP_ID || host->GetInstanceId() != lobby->InstanceId)
        {
            Notify(invited, "The custom game lobby has been destroyed.");
            return true;
        }

        if (GetLobby(invited) || invited->IsInCustomGameLobby() || invited->InBattleground() ||
            invited->InBattlegroundQueue() || playerbot::IsManagedRandomBot(invited))
        {
            Notify(invited, "You cannot join that custom-game lobby right now.");
            Notify(host, invited->GetName() + " could not join the custom-game lobby.");
            return true;
        }

        Map* lobbyMap = sMapMgr->FindMap(CUSTOM_GAME_MAP_ID, lobby->InstanceId);
        if (!lobbyMap)
        {
            Notify(invited, "The custom game lobby has been destroyed.");
            return true;
        }

        LobbyMember snapshot;
        snapshot.ReturnLocation = WorldLocation(invited->GetMapId(), invited->GetPositionX(), invited->GetPositionY(),
            invited->GetPositionZ(), invited->GetOrientation());
        lobby->Members.emplace(invited->GetGUID(), snapshot);

        if (invited->GetGroupInvite())
            invited->UninviteFromGroup();
        if (invited->GetGroup())
            invited->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);

        invited->CombatStopWithPets(true);
        invited->SetWorldSubMap(CUSTOM_GAME_MAP_ID, lobby->InstanceId);
        invited->TeleportTo(CUSTOM_GAME_MAP_ID, LobbyArrival.GetPositionX(), LobbyArrival.GetPositionY(),
            LobbyArrival.GetPositionZ(), LobbyArrival.GetOrientation());
        Notify(invited, "You joined " + host->GetName() + "'s custom-game lobby.");
        Notify(host, invited->GetName() + " accepted your custom-game invitation.");
        return true;
    }

    bool DeclineInvite(Player* invited)
    {
        if (!invited)
            return false;

        auto inviteItr = _pendingInvites.find(invited->GetGUID());
        if (inviteItr == _pendingInvites.end())
            return false;

        ObjectGuid const hostGuid = inviteItr->second.HostGuid;
        _pendingInvites.erase(inviteItr);
        if (Player* host = ObjectAccessor::FindConnectedPlayer(hostGuid))
            Notify(host, invited->GetName() + " declined your custom-game invitation.");
        return true;
    }

    void SetTeam(Player* player, uint32 team)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId || (team != ALLIANCE && team != HORDE))
            return;

        ObjectGuid const playerGuid = player->GetGUID();
        auto const currentItr = lobby->Teams.find(playerGuid);
        if (currentItr != lobby->Teams.end() && currentItr->second == team)
        {
            Notify(player, std::string("You are already on team ") + TeamName(team) + ".");
            return;
        }

        uint32 teamSize = 0;
        for (auto const& [guid, assignedTeam] : lobby->Teams)
            if (assignedTeam == team && guid != playerGuid)
                ++teamSize;
        for (CloneRequest const& request : lobby->CloneRequests)
            if (request.Team == team)
                ++teamSize;
        if (teamSize >= CUSTOM_GAME_MAX_PLAYERS_PER_TEAM)
        {
            Notify(player, std::string("Team ") + TeamName(team) + " already has the maximum of " +
                std::to_string(CUSTOM_GAME_MAX_PLAYERS_PER_TEAM) + " participants.");
            return;
        }

        lobby->Teams[playerGuid] = team;
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

    bool AddCloneRequest(Player* player, uint32 team, std::string name)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId || !normalizePlayerName(name))
            return false;

        if (!CanAddCloneUnderResourceBudget(player, *lobby))
            return false;

        ObjectGuid const sourceGuid = sCharacterCache->GetCharacterGuidByName(name);
        CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
        if (!sourceGuid || !characterInfo)
        {
            Notify(player, "No character with that name exists.");
            return false;
        }

        Player* source = ObjectAccessor::FindConnectedPlayer(sourceGuid);
        std::vector<uint32> const botAccountIds = playerbot::RandomBotParticipationManager::GetConfiguredBotAccountIds();
        bool const configuredBotAccount = std::find(botAccountIds.begin(), botAccountIds.end(), characterInfo->AccountId) != botAccountIds.end();
        bool const managedBot = configuredBotAccount || (source && playerbot::IsManagedRandomBot(source));

        lobby->CloneRequests.push_back({ lobby->NextCloneRosterSlotId++, sourceGuid, characterInfo->Name, team,
            managedBot });
        RefreshLobbyClonePreviews(*lobby);

        return true;
    }

    bool AddRandomPlayerbotClone(Player* player, uint32 team)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId || (team != ALLIANCE && team != HORDE))
            return false;

        if (!CanAddCloneUnderResourceBudget(player, *lobby))
            return false;

        uint32 teamSize = 0;
        for (auto const& [guid, assignedTeam] : lobby->Teams)
            if (assignedTeam == team)
                ++teamSize;
        for (CloneRequest const& request : lobby->CloneRequests)
            if (request.Team == team)
                ++teamSize;
        if (teamSize >= CUSTOM_GAME_MAX_PLAYERS_PER_TEAM)
        {
            Notify(player, std::string("Team ") + TeamName(team) + " already has the maximum of " +
                std::to_string(CUSTOM_GAME_MAX_PLAYERS_PER_TEAM) + " participants.");
            return false;
        }

        std::vector<uint32> const botAccountIds = playerbot::RandomBotParticipationManager::GetConfiguredBotAccountIds();
        if (botAccountIds.empty())
        {
            Notify(player, "No playerbot account IDs are configured.");
            return false;
        }

        std::vector<ObjectGuid> allCandidates = sCharacterCache->GetCharacterGuidsByAccountIds(botAccountIds);
        allCandidates.erase(std::remove_if(allCandidates.begin(), allCandidates.end(), [&](ObjectGuid sourceGuid)
        {
            CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
            return !characterInfo || characterInfo->Name.rfind("Obcm", 0) == 0;
        }), allCandidates.end());

        if (allCandidates.empty())
        {
            Notify(player, "No eligible characters exist on the configured playerbot accounts.");
            return false;
        }

        std::vector<ObjectGuid> uniqueCandidates;
        std::copy_if(allCandidates.begin(), allCandidates.end(), std::back_inserter(uniqueCandidates),
            [&](ObjectGuid sourceGuid)
            {
                return std::none_of(lobby->CloneRequests.begin(), lobby->CloneRequests.end(),
                    [&](CloneRequest const& request)
                    {
                        return request.SourceGuid == sourceGuid && request.IsPlayerbot;
                    });
            });

        // Prefer a character not yet represented anywhere in this lobby. Once
        // every configured bot has been used, random additions may repeat one.
        std::vector<ObjectGuid> const& candidates = uniqueCandidates.empty() ? allCandidates : uniqueCandidates;
        ObjectGuid const sourceGuid = candidates[urand(0u, static_cast<uint32>(candidates.size() - 1))];
        CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
        if (!characterInfo)
            return false;

        lobby->CloneRequests.push_back({ lobby->NextCloneRosterSlotId++, sourceGuid, characterInfo->Name, team, true });
        RefreshLobbyClonePreviews(*lobby);
        Notify(player, "Added playerbot " + characterInfo->Name + " to team " + TeamName(team) + ".");
        return true;
    }

    bool DuplicateTeamMember(Player* player, uint32 expectedTeam, uint32 selectionId, bool cloneRequest)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId || (expectedTeam != ALLIANCE && expectedTeam != HORDE))
            return false;

        if (!CanAddCloneUnderResourceBudget(player, *lobby))
            return false;

        ObjectGuid sourceGuid;
        std::string sourceName;
        uint32 team = 0;
        bool isPlayerbot = false;

        if (cloneRequest)
        {
            auto requestItr = std::find_if(lobby->CloneRequests.begin(), lobby->CloneRequests.end(),
                [selectionId](CloneRequest const& request) { return request.RosterSlotId == selectionId; });
            if (requestItr == lobby->CloneRequests.end() || requestItr->Team != expectedTeam)
            {
                Notify(player, "That playerbot or player clone is no longer in the team roster.");
                return false;
            }

            sourceGuid = requestItr->SourceGuid;
            sourceName = requestItr->SourceName;
            team = requestItr->Team;
            isPlayerbot = requestItr->IsPlayerbot;
        }
        else
        {
            sourceGuid = ObjectGuid::Create<HighGuid::Player>(selectionId);
            auto teamItr = lobby->Teams.find(sourceGuid);
            if (teamItr == lobby->Teams.end() || teamItr->second != expectedTeam)
            {
                Notify(player, "That player is no longer on this custom-game team.");
                return false;
            }

            team = teamItr->second;
            if (Player* source = ObjectAccessor::FindConnectedPlayer(sourceGuid))
                sourceName = source->GetName();
            else if (CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid))
                sourceName = characterInfo->Name;

            // A real lobby member always produces a dark player clone, even
            // if that character happens to use a configured playerbot account.
            isPlayerbot = false;
        }

        if ((team != ALLIANCE && team != HORDE) || !sourceGuid || sourceName.empty())
        {
            Notify(player, "That team member can no longer be duplicated.");
            return false;
        }

        uint32 teamSize = 0;
        for (auto const& [guid, assignedTeam] : lobby->Teams)
            if (assignedTeam == team)
                ++teamSize;
        for (CloneRequest const& request : lobby->CloneRequests)
            if (request.Team == team)
                ++teamSize;

        if (teamSize >= CUSTOM_GAME_MAX_PLAYERS_PER_TEAM)
        {
            Notify(player, std::string("Team ") + TeamName(team) + " already has the maximum of " +
                std::to_string(CUSTOM_GAME_MAX_PLAYERS_PER_TEAM) + " participants.");
            return false;
        }

        lobby->CloneRequests.push_back({ lobby->NextCloneRosterSlotId++, sourceGuid, sourceName, team, isPlayerbot });
        RefreshLobbyClonePreviews(*lobby);
        Notify(player, "Duplicated " + sourceName + (isPlayerbot ? " as a playerbot." : " as a player clone."));
        return true;
    }

    bool RemoveCloneRequest(Player* player, uint32 team, bool playerbotClone, uint32 filteredIndex)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby || lobby->ActiveBattlegroundId || (team != ALLIANCE && team != HORDE))
            return false;

        uint32 currentIndex = 0;
        for (auto itr = lobby->CloneRequests.begin(); itr != lobby->CloneRequests.end(); ++itr)
        {
            if (itr->Team != team || itr->IsPlayerbot != playerbotClone)
                continue;
            if (currentIndex++ != filteredIndex)
                continue;

            playerbot::PlayerbotObcCloneManager::DestroyCustomGameLobbyClone(lobby->InstanceId, itr->RosterSlotId);
            lobby->CloneRequests.erase(itr);
            RefreshLobbyClonePreviews(*lobby);
            return true;
        }

        Notify(player, "That playerbot or player clone is no longer in the team roster.");
        return false;
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

        if (lobby->RandomArena || lobby->SelectedType != type)
            lobby->Rules = BattlegroundCustomRules();
        lobby->SelectedType = type;
        lobby->ArenaType = arenaType;
        lobby->RandomArena = false;
    }

    void SelectRandomArena(Player* player)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!IsOwner(player, lobby) || lobby->ActiveBattlegroundId)
            return;

        if (!lobby->RandomArena)
            lobby->Rules = BattlegroundCustomRules();
        // Keep a real arena type as the backing selection for arena-specific
        // option visibility. StartGame resolves the actual map independently.
        lobby->SelectedType = BATTLEGROUND_NA;
        lobby->ArenaType = ARENA_TYPE_5v5;
        lobby->RandomArena = true;
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

    bool KickPlayer(Player* host, std::string name)
    {
        CustomGameLobby* lobby = GetLobby(host);
        if (!IsOwner(host, lobby) || !normalizePlayerName(name))
            return false;

        ObjectGuid const targetGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (!targetGuid || lobby->Members.find(targetGuid) == lobby->Members.end())
        {
            Notify(host, "That player is not in this custom lobby.");
            return false;
        }

        if (targetGuid == lobby->OwnerGuid)
        {
            Notify(host, "The host cannot be kicked. Close the lobby instead.");
            return false;
        }

        Player* target = ObjectAccessor::FindConnectedPlayer(targetGuid);
        if (!target)
        {
            Notify(host, "That player is no longer online.");
            return false;
        }

        if (lobby->ActiveBattlegroundId && target->GetBattlegroundId() == lobby->ActiveBattlegroundId)
            if (Battleground* bg = sBattlegroundMgr->GetBattleground(lobby->ActiveBattlegroundId, lobby->ActiveBattlegroundType))
                bg->RemovePlayerAtLeave(targetGuid, false, true);

        RemoveLobbyMember(target, *lobby, false);
        target->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE);
        target->SetBGTeam(0);
        target->SetPendingSpectatorForBG(0);
        Notify(target, "The host removed you from the custom game.");
        target->TeleportTo(GurubashiGamesmasterLocation);
        Notify(host, name + " was removed from the custom game.");
        return true;
    }

    bool StartGame(Player* owner)
    {
        CustomGameLobby* lobby = GetLobby(owner);
        if (!IsOwner(owner, lobby) || lobby->ActiveBattlegroundId)
            return false;

        uint32 blueCount = 0;
        uint32 redCount = 0;
        bool hasHumanTeamParticipant = false;
        for (auto const& [guid, team] : lobby->Teams)
            if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
                if (member->GetMapId() == CUSTOM_GAME_MAP_ID && member->GetInstanceId() == lobby->InstanceId)
                {
                    team == ALLIANCE ? ++blueCount : ++redCount;
                    hasHumanTeamParticipant = true;
                }
        for (CloneRequest const& request : lobby->CloneRequests)
            request.Team == ALLIANCE ? ++blueCount : ++redCount;

        if (!blueCount || !redCount)
        {
            Notify(owner, "Both teams need at least one player, playerbot, or player clone.");
            return false;
        }

        if (blueCount > CUSTOM_GAME_MAX_PLAYERS_PER_TEAM || redCount > CUSTOM_GAME_MAX_PLAYERS_PER_TEAM)
        {
            Notify(owner, "Custom games support up to 40 participants per team.");
            return false;
        }

        // Human-participant matches always start. Bot-only matches (humans at
        // most spectating) are the first load shed under resource pressure.
        if (!hasHumanTeamParticipant &&
            playerbot::ResourceGovernor::GetPressureLevel() == playerbot::ResourcePressureLevel::Hard)
        {
            Notify(owner, "Server load is too high to start a bot-only match right now. Please try again shortly.");
            return false;
        }

        auto resolveBracket = [owner](BattlegroundTypeId type) -> PvPDifficultyEntry const*
        {
            Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(type);
            if (!bgTemplate)
                return nullptr;

            PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), owner->GetLevel());
            return bracket ? bracket : GetBattlegroundBracketById(bgTemplate->GetMapId(), BG_BRACKET_ID_FIRST);
        };

        BattlegroundTypeId matchType = lobby->SelectedType;
        if (lobby->RandomArena)
        {
            // The candidate arenas come from `battleground_random_pool` so this
            // list and the one behind the All Arenas queue cannot drift apart,
            // and so an arena can be added or taken out of rotation with an
            // UPDATE and `.reload battleground_template`.
            std::vector<BattlegroundTypeId> arenaTypes =
                sBattlegroundMgr->GetRandomPoolMembers(BATTLEGROUND_AA);

            // Only if the pool has not been configured at all. Keeps a fresh or
            // half-migrated database working rather than offering no arenas.
            if (arenaTypes.empty())
                arenaTypes = { BATTLEGROUND_NA, BATTLEGROUND_BE, BATTLEGROUND_RL,
                               BATTLEGROUND_NL, BATTLEGROUND_TV, BATTLEGROUND_TTP };

            std::vector<BattlegroundTypeId> compatibleArenas;
            compatibleArenas.reserve(arenaTypes.size());
            for (BattlegroundTypeId type : arenaTypes)
                if (resolveBracket(type))
                    compatibleArenas.push_back(type);

            if (compatibleArenas.empty())
            {
                Notify(owner, "No compatible arena has a bracket for your level.");
                return false;
            }

            matchType = compatibleArenas[urand(0, uint32(compatibleArenas.size() - 1))];
        }

        PvPDifficultyEntry const* bracket = resolveBracket(matchType);
        if (!bracket)
        {
            Notify(owner, "That battleground has no bracket for your level.");
            return false;
        }

        Battleground* bg = sBattlegroundMgr->CreateNewBattleground(matchType, bracket, lobby->ArenaType, false, true);
        if (!bg)
        {
            Notify(owner, "The private battleground could not be created.");
            return false;
        }

        bg->ConfigureCustomGame(lobby->Rules);
        bg->SetCustomGameBotOnlyPreparation(!hasHumanTeamParticipant);
        bg->SetCustomGamePendingCloneCount(uint32(lobby->CloneRequests.size()));
        // Custom matches enter directly and never use the public queue's arena
        // or battleground population requirements. Forty is the underlying
        // battleground-raid limit, so asymmetric matches such as 40v4 work.
        bg->SetMaxPlayersPerTeam(CUSTOM_GAME_MAX_PLAYERS_PER_TEAM);
        bg->SetMaxPlayers(CUSTOM_GAME_MAX_PLAYERS_PER_TEAM * 2);
        bg->SetMinPlayersPerTeam(0);
        bg->SetMinPlayers(0);
        bg->StartBattleground();

        // Lobby previews are not combat participants. Destroy them before the
        // requested copies are provisioned inside the battleground instance.
        playerbot::PlayerbotObcCloneManager::DestroyCustomGameLobbyClones(lobby->InstanceId);

        lobby->ActiveBattlegroundId = bg->GetInstanceID();
        lobby->ActiveBattlegroundType = matchType;
        lobby->ClonesSpawned = false;
        InvalidateLobbyInvites(lobby->InstanceId);

        for (auto const& [guid, member] : lobby->Members)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player || player->GetMapId() != CUSTOM_GAME_MAP_ID || player->GetInstanceId() != lobby->InstanceId)
                continue;

            ApplyTeamVisual(player, 0);
            player->SetVisible(true);
            player->RemoveUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE |
                UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
            player->SetBattlegroundEntryPoint();

            auto teamItr = lobby->Teams.find(guid);
            if (teamItr == lobby->Teams.end())
            {
                // A spectator still needs a deterministic arena side for the
                // client to color nearby players. Assign the Alliance viewing
                // side before worldport so Alliance is green and Horde is red,
                // while the spectator remains outside the participant roster.
                constexpr uint32 spectatorViewTeam = ALLIANCE;
                player->SetFactionForRace(RACE_HUMAN);
                player->SetBGTeam(spectatorViewTeam);
                player->SetIsSpectator(true);
                player->SetUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
                player->SetVisible(false);
                player->SetPendingSpectatorForBG(bg->GetInstanceID());
                player->SetBattlegroundId(bg->GetInstanceID(), bg->GetTypeID(), 0, true, false,
                    Battleground::GetTeamIndexByTeamId(spectatorViewTeam));
                sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bg->GetTypeID());
                continue;
            }

            uint32 const team = teamItr->second;
            BattlegroundQueueTypeId const queueType = BattlegroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
            uint32 const queueSlot = player->AddBattlegroundQueueId(queueType);
            if (queueSlot >= PLAYER_MAX_BATTLEGROUND_QUEUES)
                continue;

            player->SetInviteForBattlegroundQueueType(queueType, bg->GetInstanceID());
            // Match the normal battleground port ordering: the temporary team
            // faction must be present before the destination map builds this
            // player for nearby clients. Changing it after worldport leaves
            // overhead name colors stale until the unit is targeted.
            player->SetFactionForRace(team == HORDE ? RACE_BLOODELF : RACE_HUMAN);
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
        if (!IsOwner(owner, lobby))
            return;

        playerbot::PlayerbotObcCloneManager::DestroyCustomGameLobbyClones(lobby->InstanceId);
        InvalidateLobbyInvites(lobby->InstanceId);
        lobby->Closing = true;

        // The owner can already be back in the retained lobby while the match
        // still owns a stale battleground id. Let Chromie close that state too;
        // Update() will end the active match, destroy its clones, and return all
        // retained members once the battleground teardown is safe to process.
        if (lobby->ActiveBattlegroundId)
            return;

        for (auto const& [guid, member] : lobby->Members)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            {
                ApplyTeamVisual(player, 0);
                player->SetVisible(true);
                player->RemoveUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE |
                    UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
                player->ClearWorldSubMap();
                player->TeleportTo(member.ReturnLocation);
            }
    }

    bool LeaveLobbyForGurubashi(Player* player)
    {
        CustomGameLobby* lobby = GetLobby(player);
        if (!player || !lobby || IsOwner(player, lobby))
            return false;

        if (lobby->ActiveBattlegroundId)
        {
            Notify(player, "The custom match has already started.");
            return false;
        }

        RemoveLobbyMember(player, *lobby, false);
        player->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE);
        player->SetBGTeam(0);
        player->SetPendingSpectatorForBG(0);
        Notify(player, "You left the custom-game lobby.");
        player->TeleportTo(GurubashiGamesmasterLocation);
        return true;
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
                Battleground* bg = sBattlegroundMgr->GetBattleground(lobby->ActiveBattlegroundId, lobby->ActiveBattlegroundType);
                if (IsOwner(player, lobby) && lobby->Members.size() == 1)
                {
                    // Leaving a solo custom match is equivalent to ending that
                    // attempt. Keep its sole member on the retained roster so the
                    // normal match-completion path can rebuild the same setup.
                    // The map-change callback may run before or after the normal
                    // battleground leave path clears Player::GetBattlegroundId(),
                    // so physical arrival in the retained lobby is authoritative.
                    if (bg && (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS))
                        bg->EndBattleground(PVP_TEAM_NEUTRAL);
                }
                else if (bg && (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS) &&
                    player->GetBattlegroundId() != lobby->ActiveBattlegroundId)
                {
                    RemoveLobbyMember(player, *lobby, true);
                    return;
                }
            }

            player->SetVisible(true);
            ApplyLobbySafetyFlags(player);
            player->CombatStopWithPets(true);
            if (player->GetTradeData())
                player->TradeCancel(true);
            if (player->duel)
                player->DuelComplete(DUEL_INTERRUPTED);
            if (player->IsSpectator())
                player->SetIsSpectator(false);

            // The staging box sits in a void zone with no weather record, so a
            // custom-weather override from a finished match would otherwise
            // persist on the client indefinitely. The lingering max-grade
            // emitter is also what crashed clients here (WoW.exe+0x41D559).
            Weather::SendFineWeatherUpdateToPlayer(player);

            player->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

            auto teamItr = lobby->Teams.find(player->GetGUID());
            ApplyTeamVisual(player, teamItr == lobby->Teams.end() ? 0 : teamItr->second);
            return;
        }

        if (lobby->ActiveBattlegroundId && player->GetBattlegroundId() == lobby->ActiveBattlegroundId)
        {
            Battleground* bg = sBattlegroundMgr->GetBattleground(lobby->ActiveBattlegroundId, lobby->ActiveBattlegroundType);
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

        ApplyLobbySafetyFlags(player);
        if (!player->IsAlive())
        {
            player->ResurrectPlayer(1.0f);
            player->SpawnCorpseBones();
        }
        player->SetFullHealth();
        if (player->GetTradeData())
            player->TradeCancel(true);
        if (player->duel)
            player->DuelComplete(DUEL_INTERRUPTED);

        if (player->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            player->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);
            auto teamItr = lobby->Teams.find(player->GetGUID());
            ApplyTeamVisual(player, teamItr == lobby->Teams.end() ? 0 : teamItr->second);
        }

        if (player->GetDistance(LobbyArrival) > LOBBY_MAX_DISTANCE || player->GetPositionZ() < -80.0f || player->GetPositionZ() > -45.0f)
            player->NearTeleportTo(LobbyArrival.GetPositionX(), LobbyArrival.GetPositionY(), LobbyArrival.GetPositionZ(), LobbyArrival.GetOrientation());
    }

    void OnLogout(Player* player)
    {
        _pendingInvites.erase(player->GetGUID());
        _pendingGurubashiReturn.erase(player->GetGUID());

        CustomGameLobby* lobby = GetLobby(player);
        if (!lobby)
            return;

        // Crash telemetry: a client that crashes never sends a logout request,
        // so its session is torn down with the logout timer unset. A clean exit
        // (or a /logout) always goes through that timer. This is the closest
        // the server gets to observing a client crash - correlate the DROP
        // timestamp with a MANNEQUIN spawn line to identify what was being
        // built when it died. Alt-F4 and genuine connection loss look the same
        // as a crash here, so treat it as a signal, not proof.
        WorldSession const* session = player->GetSession();
        bool const graceful = session && session->isLogingOut();
        TC_LOG_INFO("custom.lobby", "{} player='{}' guid={} lobbyOwner={} "
            "members={} pos=({:.2f},{:.2f},{:.2f})",
            graceful ? "LEAVE  graceful" : "DROP   ungraceful(crash suspect)",
            player->GetName(), player->GetGUID().ToString(),
            lobby->OwnerGuid.ToString(), uint32(lobby->Teams.size()),
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());

        RemoveLobbyMember(player, *lobby, false);
    }

    void OnBeforeMapLoad(Player* player, uint32& mapId, uint32& instanceId)
    {
        if (!player || mapId != CUSTOM_GAME_MAP_ID ||
            player->GetDistance(LobbyArrival) > LOBBY_MAX_DISTANCE ||
            player->GetPositionZ() < -80.0f || player->GetPositionZ() > -45.0f)
            return;

        // Server-only world submaps and lobby membership intentionally do not
        // survive logout/restart. Repair the saved location before the login
        // map is created so the client enters Gurubashi directly; sending a
        // far teleport immediately after initial login corrupts the client's
        // initial name/item data stream until another relog.
        mapId = GurubashiGamesmasterLocation.GetMapId();
        instanceId = 0;
        player->Relocate(GurubashiGamesmasterLocation.GetPositionX(),
            GurubashiGamesmasterLocation.GetPositionY(),
            GurubashiGamesmasterLocation.GetPositionZ(),
            GurubashiGamesmasterLocation.GetOrientation());
        _pendingGurubashiReturn.insert(player->GetGUID());
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

        if (_pendingGurubashiReturn.erase(player->GetGUID()))
            Notify(player, "Your custom-game lobby is no longer active. You have been returned to Gurubashi Arena.");
    }

    void Update()
    {
        time_t const now = GameTime::GetGameTime();
        for (auto itr = _pendingInvites.begin(); itr != _pendingInvites.end();)
            if (itr->second.ExpireTime < now)
                itr = _pendingInvites.erase(itr);
            else
                ++itr;

        CullCustomMatchesForResources();

        std::vector<uint32> eraseIds;
        std::vector<uint32> completedMatchIds;
        for (auto& [instanceId, lobbyPtr] : _lobbies)
        {
            CustomGameLobby& lobby = *lobbyPtr;
            if (lobby.Closing && lobby.ActiveBattlegroundId)
            {
                playerbot::PlayerbotObcCloneManager::DestroyCustomGameClones(lobby.ActiveBattlegroundId);
                if (Battleground* bg = sBattlegroundMgr->GetBattleground(lobby.ActiveBattlegroundId, lobby.ActiveBattlegroundType))
                    if (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS)
                        bg->EndBattleground(PVP_TEAM_NEUTRAL);
                lobby.ActiveBattlegroundId = 0;
                lobby.ActiveBattlegroundType = BATTLEGROUND_TYPE_NONE;
                lobby.ClonesSpawned = false;
            }

            if (lobby.ActiveBattlegroundId)
            {
                Battleground* bg = sBattlegroundMgr->GetBattleground(lobby.ActiveBattlegroundId, lobby.ActiveBattlegroundType);
                if (bg && bg->FindBgMap() && !lobby.ClonesSpawned)
                {
                    WorldSession* callbackSession = nullptr;
                    for (auto const& [guid, member] : lobby.Members)
                        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                            if (player->GetSession() && !player->GetSession()->IsVirtualSession())
                            {
                                callbackSession = player->GetSession();
                                break;
                            }

                    for (CloneRequest const& request : lobby.CloneRequests)
                        playerbot::PlayerbotObcCloneManager::QueueCustomGameClone(request.SourceGuid, callbackSession,
                            bg, request.Team, request.IsPlayerbot ? "" : "Dark ");
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
                playerbot::PlayerbotObcCloneManager::DestroyCustomGameLobbyClones(instanceId);
                for (auto const& [guid, member] : lobby.Members)
                    if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                        if (player->HasWorldSubMap())
                        {
                            ApplyTeamVisual(player, 0);
                            player->SetVisible(true);
                            player->RemoveUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE |
                                UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
                            if (player->IsSpectator())
                                player->SetIsSpectator(false);
                            player->SetPendingSpectatorForBG(0);
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
    uint32 CountActiveCustomMatchBots() const
    {
        uint32 total = 0;
        for (auto const& [instanceId, lobbyPtr] : _lobbies)
            if (lobbyPtr && lobbyPtr->ActiveBattlegroundId)
                total += uint32(lobbyPtr->CloneRequests.size());
        return total;
    }

    bool CanAddCloneUnderResourceBudget(Player* requester, CustomGameLobby const& lobby)
    {
        if (playerbot::ResourceGovernor::CanAddCustomMatchBot(uint32(lobby.CloneRequests.size()), CountActiveCustomMatchBots()))
            return true;

        Notify(requester, "The server is near its resource limits; playerbot and clone additions are restricted right now.");
        return false;
    }

    // Under sustained hard resource pressure, shed load in strict priority
    // order. Tier 1: end bot-only matches nobody is even spectating. Tier 2:
    // end bot matches humans are only spectating. Tier 3: once no cullable
    // match remains, remove bots one at a time from human-participant matches
    // (largest bot roster first) until pressure clears. Ending a battleground
    // reuses the normal completion flow, so members return to their lobby.
    void CullCustomMatchesForResources()
    {
        bool const cullReady = playerbot::ResourceGovernor::ShouldCullNow();
        bool const shedReady = playerbot::ResourceGovernor::ShouldShedBotNow();
        if (!cullReady && !shedReady)
            return;

        CustomGameLobby* victim = nullptr;
        Battleground* victimBattleground = nullptr;
        uint32 victimSpectators = 0;
        uint32 victimBots = 0;

        CustomGameLobby* shedTarget = nullptr;
        uint32 shedTargetBots = 0;

        for (auto& [instanceId, lobbyPtr] : _lobbies)
        {
            CustomGameLobby& lobby = *lobbyPtr;
            if (!lobby.ActiveBattlegroundId || lobby.Closing || lobby.CloneRequests.empty())
                continue;

            Battleground* bg = sBattlegroundMgr->GetBattleground(lobby.ActiveBattlegroundId, lobby.ActiveBattlegroundType);
            if (!bg || (bg->GetStatus() != STATUS_WAIT_JOIN && bg->GetStatus() != STATUS_IN_PROGRESS))
                continue;

            uint32 humanParticipants = 0;
            uint32 humanSpectators = 0;
            for (auto const& [guid, member] : lobby.Members)
            {
                Player* human = ObjectAccessor::FindConnectedPlayer(guid);
                if (!human || human->GetBattlegroundId() != lobby.ActiveBattlegroundId)
                    continue;

                if (lobby.Teams.find(guid) != lobby.Teams.end())
                    ++humanParticipants;
                else
                    ++humanSpectators;
            }

            uint32 const bots = uint32(lobby.CloneRequests.size());
            if (humanParticipants)
            {
                if (bots > shedTargetBots)
                {
                    shedTarget = &lobby;
                    shedTargetBots = bots;
                }
                continue;
            }

            bool const candidateUnwatched = humanSpectators == 0;
            bool const victimUnwatched = victim && victimSpectators == 0;
            if (!victim ||
                (candidateUnwatched && !victimUnwatched) ||
                (candidateUnwatched == victimUnwatched && bots > victimBots))
            {
                victim = &lobby;
                victimBattleground = bg;
                victimSpectators = humanSpectators;
                victimBots = bots;
            }
        }

        // Tiers 1-2: a cullable (no human participants) match exists. It is
        // always shed before any bot leaves a human match, even if that means
        // waiting out the cull cooldown.
        if (victim && victimBattleground)
        {
            if (!cullReady)
                return;

            for (auto const& [guid, member] : victim->Members)
                if (Player* human = ObjectAccessor::FindConnectedPlayer(guid))
                    Notify(human, "Your custom match has been culled for resource considerations: the server is under heavy load and bot-only matches are ended first.");

            TC_LOG_INFO("playerbots.governor",
                "Resource governor culled custom match: bgInstanceId={} bgType={} bots={} spectators={}.",
                victim->ActiveBattlegroundId, uint32(victim->ActiveBattlegroundType), victimBots, victimSpectators);

            victimBattleground->EndBattleground(PVP_TEAM_NEUTRAL);
            playerbot::ResourceGovernor::NoteCullExecuted();
            return;
        }

        // Tier 3: only human-participant matches remain. Remove one bot from
        // the match with the largest bot roster, then wait for the shed
        // cooldown so the update-time average can react before the next one.
        if (!shedReady || !shedTarget)
            return;

        if (!playerbot::PlayerbotObcCloneManager::ShedOneCustomGameClone(shedTarget->ActiveBattlegroundId))
            return;

        playerbot::ResourceGovernor::NoteBotShedExecuted();

        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (!shedTarget->LastBotShedNoticeMs || nowMs >= shedTarget->LastBotShedNoticeMs + 30000)
        {
            shedTarget->LastBotShedNoticeMs = nowMs;
            for (auto const& [guid, member] : shedTarget->Members)
                if (Player* human = ObjectAccessor::FindConnectedPlayer(guid))
                    Notify(human, "The server is under heavy load: bots are being removed from your match one at a time until performance recovers.");
        }
    }

    void InvalidateLobbyInvites(uint32 instanceId)
    {
        for (auto& [invitedGuid, invitation] : _pendingInvites)
        {
            if (invitation.InstanceId != instanceId || invitation.Invalidated)
                continue;

            invitation.Invalidated = true;
            if (Player* invited = ObjectAccessor::FindConnectedPlayer(invitedGuid))
                Notify(invited, "The custom game lobby has been destroyed.");
        }
    }

    void RefreshLobbyClonePreviews(CustomGameLobby& lobby)
    {
        if (lobby.ActiveBattlegroundId || lobby.Closing)
            return;

        WorldSession* callbackSession = nullptr;
        for (auto const& [guid, member] : lobby.Members)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                if (player->GetSession() && !player->GetSession()->IsVirtualSession())
                {
                    callbackSession = player->GetSession();
                    break;
                }

        uint32 blueIndex = 0;
        uint32 redIndex = 0;
        for (CloneRequest const& request : lobby.CloneRequests)
        {
            uint32 const teamIndex = request.Team == ALLIANCE ? blueIndex++ : redIndex++;
            Position const position = LobbyClonePosition(request.Team, teamIndex);
            playerbot::PlayerbotObcCloneManager::QueueCustomGameLobbyClone(request.SourceGuid, callbackSession,
                CUSTOM_GAME_MAP_ID, lobby.InstanceId, request.RosterSlotId, request.Team, request.IsPlayerbot, position,
                request.IsPlayerbot ? "" : "Dark ");
            playerbot::PlayerbotObcCloneManager::SetCustomGameLobbyClonePosition(lobby.InstanceId,
                request.RosterSlotId, position);
        }
    }

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

        if (wasOwner)
        {
            InvalidateLobbyInvites(lobby.InstanceId);
            lobby.Closing = true;
            for (auto const& [guid, member] : lobby.Members)
                if (Player* remaining = ObjectAccessor::FindConnectedPlayer(guid))
                    Notify(remaining, "The custom-game host left, so the lobby is closing.");
        }
        else if (lobby.Members.empty())
            lobby.Closing = true;
    }

    void RecreateLobbyAfterMatch(uint32 oldInstanceId)
    {
        auto lobbyItr = _lobbies.find(oldInstanceId);
        if (lobbyItr == _lobbies.end())
            return;

        CustomGameLobby& lobby = *lobbyItr->second;
        uint32 const battlegroundId = lobby.ActiveBattlegroundId;
        Battleground* bg = sBattlegroundMgr->GetBattleground(battlegroundId, lobby.ActiveBattlegroundType);
        playerbot::PlayerbotObcCloneManager::DestroyCustomGameClones(battlegroundId);

        auto detachFromMatch = [bg](Player* player)
        {
            if (bg && bg->IsPlayerInBattleground(player->GetGUID()))
                bg->RemovePlayerAtLeave(player->GetGUID(), false, true);
            else
            {
                player->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE);
                player->SetBGTeam(0);
                player->SetVisible(true);
                player->RemoveUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
                if (player->IsSpectator())
                    player->SetIsSpectator(false);
                // Spectators and already-removed members never pass through
                // RemovePlayerAtLeave, which is where the custom-weather
                // override is normally cleared for the client.
                if (bg && bg->HasCustomWeatherOverride())
                    Weather::SendFineWeatherUpdateToPlayer(player);
            }
        };

        std::vector<ObjectGuid> returningGuids;
        bool const isSoloLobby = lobby.Members.size() == 1 && lobby.Members.find(lobby.OwnerGuid) != lobby.Members.end();
        for (auto const& [guid, member] : lobby.Members)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                if (player->GetBattlegroundId() == battlegroundId ||
                    (isSoloLobby && guid == lobby.OwnerGuid && player->GetMapId() == CUSTOM_GAME_MAP_ID &&
                        player->GetInstanceId() == oldInstanceId))
                    returningGuids.push_back(guid);

        if (returningGuids.empty())
        {
            lobby.Members.clear();
            lobby.Teams.clear();
            lobby.ActiveBattlegroundId = 0;
            lobby.ActiveBattlegroundType = BATTLEGROUND_TYPE_NONE;
            lobby.Closing = true;
            return;
        }

        if (std::find(returningGuids.begin(), returningGuids.end(), lobby.OwnerGuid) == returningGuids.end())
        {
            for (ObjectGuid guid : returningGuids)
                if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                {
                    detachFromMatch(player);
                    player->ClearWorldSubMap();
                    player->TeleportTo(lobby.Members.at(guid).ReturnLocation);
                }

            lobby.Members.clear();
            lobby.Teams.clear();
            lobby.ActiveBattlegroundId = 0;
            lobby.ActiveBattlegroundType = BATTLEGROUND_TYPE_NONE;
            lobby.Closing = true;
            return;
        }

        Map* newLobbyMap = sMapMgr->CreateWorldSubMap(CUSTOM_GAME_MAP_ID);
        if (!newLobbyMap)
        {
            for (ObjectGuid guid : returningGuids)
                if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                {
                    detachFromMatch(player);
                    player->ClearWorldSubMap();
                    player->TeleportTo(lobby.Members.at(guid).ReturnLocation);
                }
            lobby.ActiveBattlegroundId = 0;
            lobby.ActiveBattlegroundType = BATTLEGROUND_TYPE_NONE;
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
        lobby.ActiveBattlegroundType = BATTLEGROUND_TYPE_NONE;
        lobby.ClonesSpawned = false;
        lobby.Closing = false;

        auto lobbyNode = _lobbies.extract(lobbyItr);
        lobbyNode.key() = newInstanceId;
        _lobbies.insert(std::move(lobbyNode));

        PopulateLobbyMap(newLobbyMap);

        for (ObjectGuid guid : returningGuids)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            {
                detachFromMatch(player);

                player->SetWorldSubMap(CUSTOM_GAME_MAP_ID, newInstanceId);
                player->TeleportTo(CUSTOM_GAME_MAP_ID, LobbyArrival.GetPositionX(), LobbyArrival.GetPositionY(),
                    LobbyArrival.GetPositionZ(), LobbyArrival.GetOrientation());
            }

        RefreshLobbyClonePreviews(lobby);

        if (Map* oldLobbyMap = sMapMgr->FindMap(CUSTOM_GAME_MAP_ID, oldInstanceId))
            if (!oldLobbyMap->HavePlayers())
                sMapMgr->DestroyWorldSubMap(CUSTOM_GAME_MAP_ID, oldInstanceId);
    }

    static void ApplyLobbySafetyFlags(Player* player)
    {
        if (!player)
            return;

        player->SetUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
        player->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_ATTACKABLE_1 |
            UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_UNINTERACTIBLE);
        player->ClearUnitState(UNIT_STATE_UNATTACKABLE);
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
    std::unordered_map<ObjectGuid, PendingLobbyInvite> _pendingInvites;
    std::unordered_set<ObjectGuid> _pendingGurubashiReturn;
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
                std::string(TeamName(team)) + " roster: " + std::to_string(realCount) + " players, " + std::to_string(copyCount) + " bots/clones",
                team, ACTION_STATUS);

            auto itr = lobby->Teams.find(player->GetGUID());
            if (itr != lobby->Teams.end() && itr->second == team)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Leave team", team, ACTION_LEAVE_TEAM);
            else
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, std::string("Join team ") + TeamName(team), team, ACTION_JOIN_TEAM);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Add playerbot", team, ACTION_ADD_BOT, "Enter player name", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Add random playerbot", team, ACTION_ADD_RANDOM_BOT);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Duplicate existing team member...", team, ACTION_DUPLICATE_TEAM_MEMBER);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Remove playerbot", team, ACTION_REMOVE_BOT_OR_CLONE);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowDuplicateTeamMemberMenu(Player* player, uint32 team, uint32 page = 0)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby || (team != ALLIANCE && team != HORDE))
                return;

            struct DuplicateMenuEntry
            {
                std::string Label;
                uint32 Sender;
                uint32 Action;
            };

            std::vector<DuplicateMenuEntry> entries;
            entries.reserve(lobby->Teams.size() + lobby->CloneRequests.size());

            for (auto const& [guid, assignedTeam] : lobby->Teams)
            {
                if (assignedTeam != team)
                    continue;

                std::string name;
                if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
                    name = member->GetName();
                else if (CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(guid))
                    name = characterInfo->Name;

                if (!name.empty())
                    entries.push_back({ "Duplicate " + name + " (player)", guid.GetCounter(), ACTION_DUPLICATE_HUMAN_MEMBER });
            }

            for (CloneRequest const& request : lobby->CloneRequests)
            {
                if (request.Team != team)
                    continue;

                entries.push_back({
                    "Duplicate " + request.SourceName + (request.IsPlayerbot ? " (playerbot)" : " (player clone)"),
                    request.RosterSlotId, ACTION_DUPLICATE_CLONE_MEMBER });
            }

            std::sort(entries.begin(), entries.end(), [](DuplicateMenuEntry const& left, DuplicateMenuEntry const& right)
            {
                if (left.Label != right.Label)
                    return left.Label < right.Label;
                if (left.Action != right.Action)
                    return left.Action < right.Action;
                return left.Sender < right.Sender;
            });

            uint32 const pageCount = std::max<uint32>(1,
                (static_cast<uint32>(entries.size()) + CUSTOM_GAME_GOSSIP_PAGE_SIZE - 1) / CUSTOM_GAME_GOSSIP_PAGE_SIZE);
            page = std::min(page, pageCount - 1);
            uint32 const begin = page * CUSTOM_GAME_GOSSIP_PAGE_SIZE;
            uint32 const end = std::min<uint32>(begin + CUSTOM_GAME_GOSSIP_PAGE_SIZE, static_cast<uint32>(entries.size()));

            for (uint32 index = begin; index < end; ++index)
            {
                DuplicateMenuEntry const& entry = entries[index];
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, entry.Label, entry.Sender, entry.Action);
            }

            if (entries.empty())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No team members are available to duplicate.", team, ACTION_STATUS);

            if (page > 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Previous page (" + std::to_string(page) + "/" + std::to_string(pageCount) + ")",
                    MakePagedTeamSender(team, page - 1), ACTION_DUPLICATE_PAGE);
            if (page + 1 < pageCount)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Next page (" + std::to_string(page + 2) + "/" + std::to_string(pageCount) + ")",
                    MakePagedTeamSender(team, page + 1), ACTION_DUPLICATE_PAGE);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", team, ACTION_DUPLICATE_LIST_BACK);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowCloneRemovalMenu(Player* player, uint32 team, uint32 page = 0)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby || (team != ALLIANCE && team != HORDE))
                return;

            struct RemovalMenuEntry
            {
                std::string Label;
                uint32 Action;
            };

            std::vector<RemovalMenuEntry> entries;
            entries.reserve(lobby->CloneRequests.size());

            uint32 playerbotIndex = 0;
            uint32 playerCloneIndex = 0;
            for (CloneRequest const& request : lobby->CloneRequests)
            {
                if (request.Team != team)
                    continue;

                uint32& filteredIndex = request.IsPlayerbot ? playerbotIndex : playerCloneIndex;
                if (filteredIndex < ACTION_REMOVE_CHOICE_LIMIT)
                {
                    uint32 const actionBase = request.IsPlayerbot ? ACTION_REMOVE_BOT_CHOICE_BASE : ACTION_REMOVE_DARK_CHOICE_BASE;
                    entries.push_back({
                        "Remove " + request.SourceName + (request.IsPlayerbot ? " (playerbot)" : " (player clone)"),
                        actionBase + filteredIndex });
                }

                ++filteredIndex;
            }

            uint32 const pageCount = std::max<uint32>(1,
                (static_cast<uint32>(entries.size()) + CUSTOM_GAME_GOSSIP_PAGE_SIZE - 1) / CUSTOM_GAME_GOSSIP_PAGE_SIZE);
            page = std::min(page, pageCount - 1);
            uint32 const begin = page * CUSTOM_GAME_GOSSIP_PAGE_SIZE;
            uint32 const end = std::min<uint32>(begin + CUSTOM_GAME_GOSSIP_PAGE_SIZE, static_cast<uint32>(entries.size()));
            uint32 const pagedSender = MakePagedTeamSender(team, page);

            for (uint32 index = begin; index < end; ++index)
            {
                RemovalMenuEntry const& entry = entries[index];
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, entry.Label, pagedSender, entry.Action);
            }

            if (entries.empty())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No playerbots or player clones are on this team.", team, ACTION_STATUS);

            if (page > 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Previous page (" + std::to_string(page) + "/" + std::to_string(pageCount) + ")",
                    MakePagedTeamSender(team, page - 1), ACTION_REMOVE_PAGE);
            if (page + 1 < pageCount)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Next page (" + std::to_string(page + 2) + "/" + std::to_string(pageCount) + ")",
                    MakePagedTeamSender(team, page + 1), ACTION_REMOVE_PAGE);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", team, ACTION_REMOVE_LIST_BACK);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowChromieMenu(Player* player)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby)
                return;

            if (!CustomGameLobbyManager::Instance().IsOwner(player, lobby))
            {
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Current: " + BattlegroundName(*lobby), GOSSIP_SENDER_MAIN, ACTION_STATUS);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Only the lobby host can configure or start the match.", GOSSIP_SENDER_MAIN, ACTION_STATUS);
                AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Leave custom lobby", GOSSIP_SENDER_MAIN, ACTION_LEAVE_LOBBY,
                    "Leave this lobby and return to Gurubashi Arena?", 0, false);
                SendGossipMenuFor(player, 1, me->GetGUID());
                return;
            }

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                "Select Battleground (Current " + BattlegroundName(*lobby) + ")",
                GOSSIP_SENDER_MAIN, ACTION_SELECT_GAME_MENU);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Battleground options", GOSSIP_SENDER_MAIN, ACTION_BATTLEGROUND_OPTIONS);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Kick player...", GOSSIP_SENDER_MAIN, ACTION_KICK_PLAYER,
                "Enter player name", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "START PRIVATE GAME", GOSSIP_SENDER_MAIN, ACTION_START, "Start with the current teams and rules?", 0, false);
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Close lobby and return everyone", GOSSIP_SENDER_MAIN, ACTION_CLOSE, "Close this custom lobby?", 0, false);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowGameSelectionMenu(Player* player)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby || !CustomGameLobbyManager::Instance().IsOwner(player, lobby))
                return;

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_ARENA_MENU);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Battleground", GOSSIP_SENDER_MAIN, ACTION_SELECT_BATTLEGROUND_MENU);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_SELECT_GAME_BACK);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowArenaSelectionMenu(Player* player)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby || !CustomGameLobbyManager::Instance().IsOwner(player, lobby))
                return;

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Random Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_RANDOM_ARENA);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Nagrand Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_NA);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Blade's Edge Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_BE);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Ruins of Lordaeron", GOSSIP_SENDER_MAIN, ACTION_SELECT_RL);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Nefarian's Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_NL);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Tol'Viron Arena", GOSSIP_SENDER_MAIN, ACTION_SELECT_TV);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Tiger's Peak", GOSSIP_SENDER_MAIN, ACTION_SELECT_TTP);

            // Listed by walking the id range, so an arena appears here as soon
            // as it has a battleground_template row. Arenas whose terrain is not
            // on this realm yet have no template and are skipped, which keeps a
            // half-deployed install from offering a match it cannot start.
            for (uint32 type = BATTLEGROUND_CUSTOM_ARENA_FIRST; type <= BATTLEGROUND_CUSTOM_ARENA_LAST; ++type)
            {
                BattlegroundTypeId const bgTypeId = BattlegroundTypeId(type);
                if (!sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId))
                    continue;

                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, BattlegroundNameByType(bgTypeId), GOSSIP_SENDER_MAIN,
                    ACTION_SELECT_CUSTOM_ARENA_BASE + (type - BATTLEGROUND_CUSTOM_ARENA_FIRST));
            }

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_SELECT_LIST_BACK);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowBattlegroundSelectionMenu(Player* player)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby || !CustomGameLobbyManager::Instance().IsOwner(player, lobby))
                return;

            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Warsong Gulch", GOSSIP_SENDER_MAIN, ACTION_SELECT_WS);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Twin Peaks", GOSSIP_SENDER_MAIN, ACTION_SELECT_TP);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Arathi Basin", GOSSIP_SENDER_MAIN, ACTION_SELECT_AB);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Eye of the Storm", GOSSIP_SENDER_MAIN, ACTION_SELECT_EY);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Scarlet Chapel Deathmatch", GOSSIP_SENDER_MAIN, ACTION_SELECT_SCM);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Blackrock Throne Deathmatch", GOSSIP_SENDER_MAIN, ACTION_SELECT_BRT);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Obsidian Colosseum", GOSSIP_SENDER_MAIN, ACTION_SELECT_OBC);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Tanaris Deathmatch", GOSSIP_SENDER_MAIN, ACTION_SELECT_TRT);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Battle for Gilneas", GOSSIP_SENDER_MAIN, ACTION_SELECT_BFG);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Alterac Valley", GOSSIP_SENDER_MAIN, ACTION_SELECT_AV);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Strand of the Ancients", GOSSIP_SENDER_MAIN, ACTION_SELECT_SA);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Isle of Conquest", GOSSIP_SENDER_MAIN, ACTION_SELECT_IC);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_SELECT_LIST_BACK);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        void ShowBattlegroundOptions(Player* player)
        {
            CustomGameLobby* lobby = CustomGameLobbyManager::Instance().GetLobby(player);
            if (!lobby || !CustomGameLobbyManager::Instance().IsOwner(player, lobby))
                return;

            if (lobby->SelectedType == BATTLEGROUND_WS || lobby->SelectedType == BATTLEGROUND_TP)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Flag captures: " + ConfiguredValue(lobby->Rules.FlagCaptureLimit, 3),
                    GOSSIP_SENDER_MAIN, ACTION_RULE_FLAG_CAPS, "Winning flag captures (0 restores default)", 0, true);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    std::string("Enemy team's flag on map: ") + (lobby->Rules.ShowEnemyFlagOnMap ? "Visible" : "Hidden"),
                    GOSSIP_SENDER_MAIN, ACTION_TOGGLE_ENEMY_FLAG);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    std::string("Your team's flag on map: ") + (lobby->Rules.ShowAllyFlagOnMap ? "Visible" : "Hidden"),
                    GOSSIP_SENDER_MAIN, ACTION_TOGGLE_ALLY_FLAG);
            }

            uint32 const resourceLimit = DefaultResourceLimit(lobby->SelectedType);
            if (resourceLimit)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Resource limit: " + ConfiguredValue(lobby->Rules.ResourceLimit, resourceLimit),
                    GOSSIP_SENDER_MAIN, ACTION_RULE_RESOURCES, "Winning resource total (0 restores default)", 0, true);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Resource gain: " + std::to_string(lobby->Rules.ResourceGainPercent) + "%",
                    GOSSIP_SENDER_MAIN, ACTION_RULE_RESOURCE_RATE, "Percent (100 is normal)", 0, true);
            }

            // Every custom battleground is scored by kills, so they all expose
            // the kill-limit rule.
            if (IsCustomBattleground(lobby->SelectedType))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Deathmatch kills: " + ConfiguredValue(lobby->Rules.DeathmatchKillLimit, DefaultDeathmatchKillLimit(lobby->SelectedType)),
                    GOSSIP_SENDER_MAIN, ACTION_RULE_KILLS, "Winning kill total (0 restores default)", 0, true);

            if (!IsArenaSelection(lobby->SelectedType))
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Resurrection: " + ConfiguredSeconds(lobby->Rules.ResurrectionIntervalMs, RESURRECTION_INTERVAL),
                    GOSSIP_SENDER_MAIN, ACTION_RULE_REZ_TIME, "Seconds (5-120; 0 restores default)", 0, true);
            }

            if (lobby->SelectedType == BATTLEGROUND_AB || lobby->SelectedType == BATTLEGROUND_BFG)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Flag interaction: " + ConfiguredSeconds(lobby->Rules.NodeFlagCaptureTimeMs, 5000),
                    GOSSIP_SENDER_MAIN, ACTION_RULE_NODE_FLAG_TIME, "Seconds (1-120; 0 restores DBC default)", 0, true);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "Base capture after assault: " + ConfiguredSeconds(lobby->Rules.NodeBaseCaptureTimeMs, 60000),
                    GOSSIP_SENDER_MAIN, ACTION_RULE_NODE_BASE_TIME, "Seconds (1-600; 0 restores default)", 0, true);
            }

            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                std::string("Weather: ") + WeatherName(lobby->Rules.Weather) + " (click to change)",
                GOSSIP_SENDER_MAIN, ACTION_CYCLE_WEATHER);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_BATTLEGROUND_OPTIONS_BACK);
            SendGossipMenuFor(player, 1, me->GetGUID());
        }

        bool OnGossipHello(Player* player) override
        {
            player->PlayerTalkClass->ClearMenus();
            if (me->GetEntry() == CUSTOM_GAME_HOST_ENTRY)
            {
                Group const* group = player->GetGroup();
                std::string const rosterName = !group ? "solo" : group->isRaidGroup() ? "raid" : "party";
                std::string const option = !group ? "Create a custom-game lobby" :
                    "Create a custom-game lobby for my " + rosterName;
                std::string const confirmation = !group ? "Create a custom-game lobby?" :
                    "Create a custom-game lobby and bring your current " + rosterName + "?";
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, option, GOSSIP_SENDER_MAIN, ACTION_CREATE,
                    confirmation, 0, false);
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

            if (action >= ACTION_REMOVE_BOT_CHOICE_BASE &&
                action < ACTION_REMOVE_BOT_CHOICE_BASE + ACTION_REMOVE_CHOICE_LIMIT)
            {
                uint32 const team = GetPagedTeam(sender);
                uint32 const page = GetPagedPage(sender);
                manager.RemoveCloneRequest(player, team, true, action - ACTION_REMOVE_BOT_CHOICE_BASE);
                ShowCloneRemovalMenu(player, team, page);
                return true;
            }

            if (action >= ACTION_REMOVE_DARK_CHOICE_BASE &&
                action < ACTION_REMOVE_DARK_CHOICE_BASE + ACTION_REMOVE_CHOICE_LIMIT)
            {
                uint32 const team = GetPagedTeam(sender);
                uint32 const page = GetPagedPage(sender);
                manager.RemoveCloneRequest(player, team, false, action - ACTION_REMOVE_DARK_CHOICE_BASE);
                ShowCloneRemovalMenu(player, team, page);
                return true;
            }

            // The ported arenas occupy a contiguous block of action ids, which a
            // switch cannot match, so they are resolved here alongside the other
            // range-addressed menus above.
            if (action >= ACTION_SELECT_CUSTOM_ARENA_BASE &&
                action <= ACTION_SELECT_CUSTOM_ARENA_BASE + (BATTLEGROUND_CUSTOM_ARENA_LAST - BATTLEGROUND_CUSTOM_ARENA_FIRST))
            {
                BattlegroundTypeId const bgTypeId =
                    BattlegroundTypeId(BATTLEGROUND_CUSTOM_ARENA_FIRST + (action - ACTION_SELECT_CUSTOM_ARENA_BASE));
                manager.SelectBattleground(player, bgTypeId, ARENA_TYPE_5v5);
                ShowChromieMenu(player);
                return true;
            }

            switch (action)
            {
                case ACTION_CREATE: manager.CreateLobby(player); break;
                case ACTION_JOIN_TEAM: manager.SetTeam(player, sender); ShowTeamMenu(player, sender); return true;
                case ACTION_LEAVE_TEAM: manager.LeaveTeam(player); ShowTeamMenu(player, sender); return true;
                case ACTION_ADD_RANDOM_BOT: manager.AddRandomPlayerbotClone(player, sender); ShowTeamMenu(player, sender); return true;
                case ACTION_DUPLICATE_TEAM_MEMBER: ShowDuplicateTeamMemberMenu(player, sender); return true;
                case ACTION_DUPLICATE_PAGE:
                    ShowDuplicateTeamMemberMenu(player, GetPagedTeam(sender), GetPagedPage(sender)); return true;
                case ACTION_REMOVE_PAGE:
                    ShowCloneRemovalMenu(player, GetPagedTeam(sender), GetPagedPage(sender)); return true;
                case ACTION_DUPLICATE_HUMAN_MEMBER:
                {
                    uint32 const team = me->GetEntry() == CUSTOM_GAME_BLUE_ENTRY ? ALLIANCE : HORDE;
                    manager.DuplicateTeamMember(player, team, sender, false);
                    ShowDuplicateTeamMemberMenu(player, team);
                    return true;
                }
                case ACTION_DUPLICATE_CLONE_MEMBER:
                {
                    uint32 const team = me->GetEntry() == CUSTOM_GAME_BLUE_ENTRY ? ALLIANCE : HORDE;
                    manager.DuplicateTeamMember(player, team, sender, true);
                    ShowDuplicateTeamMemberMenu(player, team);
                    return true;
                }
                case ACTION_DUPLICATE_LIST_BACK: ShowTeamMenu(player, sender); return true;
                case ACTION_REMOVE_BOT_OR_CLONE: ShowCloneRemovalMenu(player, sender); return true;
                case ACTION_REMOVE_LIST_BACK: ShowTeamMenu(player, sender); return true;
                case ACTION_SELECT_GAME_MENU: ShowGameSelectionMenu(player); return true;
                case ACTION_SELECT_ARENA_MENU: ShowArenaSelectionMenu(player); return true;
                case ACTION_SELECT_BATTLEGROUND_MENU: ShowBattlegroundSelectionMenu(player); return true;
                case ACTION_SELECT_GAME_BACK: ShowChromieMenu(player); return true;
                case ACTION_SELECT_LIST_BACK: ShowGameSelectionMenu(player); return true;
                case ACTION_SELECT_WS: manager.SelectBattleground(player, BATTLEGROUND_WS, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_TP: manager.SelectBattleground(player, BATTLEGROUND_TP, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_AB: manager.SelectBattleground(player, BATTLEGROUND_AB, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_EY: manager.SelectBattleground(player, BATTLEGROUND_EY, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_SCM: manager.SelectBattleground(player, BATTLEGROUND_SCM, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_BRT: manager.SelectBattleground(player, BATTLEGROUND_BRT, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_OBC: manager.SelectBattleground(player, BATTLEGROUND_OBC, 0); ShowChromieMenu(player); return true;
                case ACTION_SELECT_TRT: manager.SelectBattleground(player, BATTLEGROUND_TRT, 0); ShowChromieMenu(player); return true;
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
                case ACTION_SELECT_RANDOM_ARENA: manager.SelectRandomArena(player); ShowChromieMenu(player); return true;
                // Counterpart to the block of gossip entries built above. A
                // switch cannot express a range, so this is handled ahead of it
                // in the same function -- see the check before the switch.
                case ACTION_BATTLEGROUND_OPTIONS: ShowBattlegroundOptions(player); return true;
                case ACTION_BATTLEGROUND_OPTIONS_BACK: ShowChromieMenu(player); return true;
                case ACTION_TOGGLE_ENEMY_FLAG:
                case ACTION_TOGGLE_ALLY_FLAG:
                case ACTION_CYCLE_WEATHER:
                    manager.ToggleRule(player, action);
                    ShowBattlegroundOptions(player);
                    return true;
                case ACTION_START: manager.StartGame(player); break;
                case ACTION_CLOSE: manager.CloseLobby(player); break;
                case ACTION_LEAVE_LOBBY: manager.LeaveLobbyForGurubashi(player); break;
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

            if (action == ACTION_KICK_PLAYER)
            {
                manager.KickPlayer(player, value);
                ShowChromieMenu(player);
                return true;
            }
            else if (action >= ACTION_RULE_FLAG_CAPS && action <= ACTION_RULE_NODE_BASE_TIME)
            {
                manager.SetRule(player, action, value);
                ShowBattlegroundOptions(player);
                return true;
            }
            else
            {
                if (action == ACTION_ADD_BOT)
                    manager.AddCloneRequest(player, sender, value);
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
    void OnBeforeMapLoad(Player* player, uint32& mapId, uint32& instanceId) override
    {
        CustomGameLobbyManager::Instance().OnBeforeMapLoad(player, mapId, instanceId);
    }
    void OnLogin(Player* player, bool) override { CustomGameLobbyManager::Instance().OnLogin(player); }
    void OnMapChanged(Player* player) override { CustomGameLobbyManager::Instance().OnMapChanged(player); }
    void OnUpdate(Player* player, uint32) override { CustomGameLobbyManager::Instance().OnPlayerUpdate(player); }
    void OnLogout(Player* player) override { CustomGameLobbyManager::Instance().OnLogout(player); }
    bool OnCustomGameInvite(Player* inviter, Player* invitee) override { return CustomGameLobbyManager::Instance().InvitePlayer(inviter, invitee); }
    bool OnCustomGameSummonResponse(Player* invitee, ObjectGuid summonerGuid, bool agree) override
    {
        return CustomGameLobbyManager::Instance().HandleSummonResponse(invitee, summonerGuid, agree);
    }
};

class custom_game_lobby_unit_script : public UnitScript
{
public:
    custom_game_lobby_unit_script() : UnitScript("custom_game_lobby_unit_script") { }

    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (Player* player = victim ? victim->ToPlayer() : nullptr)
            if (player->IsInCustomGameLobby())
                damage = 0;
    }
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
    new custom_game_lobby_unit_script();
    new custom_game_lobby_world_script();
}
