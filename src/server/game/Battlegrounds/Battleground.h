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

#ifndef __BATTLEGROUND_H
#define __BATTLEGROUND_H

#include "ArenaScore.h"
#include "DBCEnums.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"
#include <algorithm>
#include <deque>
#include <map>

namespace WorldPackets
{
    namespace WorldState
    {
        class InitWorldStates;
    }
}

class BattlegroundMap;
class Creature;
class GameObject;
class Group;
class Player;
class Transport;
class Unit;
class WorldObject;
class WorldPacket;

struct BattlegroundScore;
struct PvPDifficultyEntry;
struct WorldSafeLocsEntry;

enum class BattlegroundNodeStatus : uint8
{
    Neutral = 0,
    FriendlyControlled,
    EnemyControlled,
    FriendlyContested,
    EnemyContested,
    FriendlyUnderAttack
};

struct BattlegroundNodeObjective
{
    uint32 NodeId = 0;
    BattlegroundNodeStatus Status = BattlegroundNodeStatus::Neutral;
    Position Location;
    ObjectGuid BannerGuid = ObjectGuid::Empty;
};

enum class BattlegroundCustomWeather : uint8
{
    Normal,
    Clear,
    Rain,
    Snow,
    Sandstorm,
    Thunderstorm,
    Fog,
    Max
};

struct BattlegroundCustomRules
{
    uint32 FlagCaptureLimit = 0;
    uint32 ResourceLimit = 0;
    uint32 ResourceGainPercent = 100;
    uint32 DeathmatchKillLimit = 0;
    uint32 ResurrectionIntervalMs = 0;
    uint32 NodeFlagCaptureTimeMs = 0;
    uint32 NodeBaseCaptureTimeMs = 0;
    bool ShowEnemyFlagOnMap = true;
    bool ShowAllyFlagOnMap = true;
    BattlegroundCustomWeather Weather = BattlegroundCustomWeather::Normal;
};

enum BattlegroundDesertionType
{
    BG_DESERTION_TYPE_LEAVE_BG        = 0, // player leaves the BG
    BG_DESERTION_TYPE_OFFLINE         = 1, // player is kicked from BG because offline
    BG_DESERTION_TYPE_LEAVE_QUEUE     = 2, // player is invited to join and refuses to do it
    BG_DESERTION_TYPE_NO_ENTER_BUTTON = 3, // player is invited to join and do nothing (time expires)
    BG_DESERTION_TYPE_INVITE_LOGOUT   = 4, // player is invited to join and logs out
};

enum BattlegroundCriteriaId
{
    BG_CRITERIA_CHECK_RESILIENT_VICTORY,
    BG_CRITERIA_CHECK_SAVE_THE_DAY,
    BG_CRITERIA_CHECK_EVERYTHING_COUNTS,
    BG_CRITERIA_CHECK_AV_PERFECTION,
    BG_CRITERIA_CHECK_DEFENSE_OF_THE_ANCIENTS,
    BG_CRITERIA_CHECK_NOT_EVEN_A_SCRATCH,
};

enum BattlegroundBroadcastTexts
{
    BG_TEXT_ALLIANCE_WINS       = 10633,
    BG_TEXT_HORDE_WINS          = 10634,

    BG_TEXT_START_TWO_MINUTES   = 18193,
    BG_TEXT_START_ONE_MINUTE    = 18194,
    BG_TEXT_START_HALF_MINUTE   = 18195,
    BG_TEXT_START_FIFTEEN_SEC   = 77867,
    BG_TEXT_BATTLE_HAS_BEGUN    = 18196,
};

enum BattlegroundSounds
{
    SOUND_HORDE_WINS                = 8454,
    SOUND_ALLIANCE_WINS             = 8455,
    SOUND_BG_START                  = 3439,
    SOUND_BG_START_L70ETC           = 11803
};

enum BattlegroundQuests
{
    SPELL_WS_QUEST_REWARD           = 43483,
    SPELL_AB_QUEST_REWARD           = 43484,
    SPELL_AV_QUEST_REWARD           = 43475,
    SPELL_AV_QUEST_KILLED_BOSS      = 23658,
    SPELL_EY_QUEST_REWARD           = 43477,
    SPELL_SA_QUEST_REWARD           = 61213,
    SPELL_AB_QUEST_REWARD_4_BASES   = 24061,
    SPELL_AB_QUEST_REWARD_5_BASES   = 24064
};

enum BattlegroundMarks
{
    SPELL_WS_MARK_LOSER             = 24950,
    SPELL_WS_MARK_WINNER            = 24951,
    SPELL_AB_MARK_LOSER             = 24952,
    SPELL_AB_MARK_WINNER            = 24953,
    SPELL_AV_MARK_LOSER             = 24954,
    SPELL_AV_MARK_WINNER            = 24955,
    SPELL_SA_MARK_WINNER            = 61160,
    SPELL_SA_MARK_LOSER             = 61159,
    ITEM_AV_MARK_OF_HONOR           = 20560,
    ITEM_WS_MARK_OF_HONOR           = 20558,
    ITEM_AB_MARK_OF_HONOR           = 20559,
    ITEM_EY_MARK_OF_HONOR           = 29024,
    ITEM_SA_MARK_OF_HONOR           = 42425
};

enum BattlegroundMarksCount
{
    ITEM_WINNER_COUNT               = 3,
    ITEM_LOSER_COUNT                = 1
};

enum BattlegroundCreatures
{
    BG_CREATURE_ENTRY_A_SPIRITGUIDE      = 13116,           // alliance
    BG_CREATURE_ENTRY_H_SPIRITGUIDE      = 13117            // horde
};

enum BattlegroundSpells
{
    SPELL_WAITING_FOR_RESURRECT     = 2584,                 // Waiting to Resurrect
    SPELL_SPIRIT_HEAL_CHANNEL       = 22011,                // Spirit Heal Channel
    SPELL_SPIRIT_HEAL               = 22012,                // Spirit Heal
    SPELL_RESURRECTION_VISUAL       = 24171,                // Resurrection Impact Visual
    SPELL_ARENA_PREPARATION         = 32727,                // use this one, 32728 not correct
    SPELL_PREPARATION               = 44521,                // Preparation
    SPELL_INSTANT_CAST              = 45813,                // instant cast spells in arena
    SPELL_SPIRIT_HEAL_MANA          = 44535,                // Spirit Heal
    SPELL_RECENTLY_DROPPED_FLAG     = 42792,                // Recently Dropped Flag
    SPELL_AURA_PLAYER_INACTIVE      = 43681,                // Inactive
    SPELL_HONORABLE_DEFENDER_25Y    = 68652,                // +50% honor when standing at a capture point that you control, 25yards radius (added in 3.2)
    SPELL_HONORABLE_DEFENDER_60Y    = 66157                 // +50% honor when standing at a capture point that you control, 60yards radius (added in 3.2), probably for 40+ player battlegrounds
};

enum BattlegroundTimeIntervals
{
    CHECK_PLAYER_POSITION_INVERVAL  = 1000,                 // ms
    RESURRECTION_INTERVAL           = 30000,                // ms
    //REMIND_INTERVAL                 = 10000,                // ms
    INVITATION_REMIND_TIME          = 10000,                // ms
    INVITE_ACCEPT_WAIT_TIME         = 30000,                // ms
    TIME_TO_AUTOREMOVE              = 60000,               // ms
    MAX_OFFLINE_TIME                = 300,                  // secs
    RESPAWN_ONE_DAY                 = 86400,                // secs
    RESPAWN_IMMEDIATELY             = 0,                    // secs
    BUFF_RESPAWN_TIME               = 180                   // secs
};

enum BattlegroundStartTimeIntervals
{
    BG_START_DELAY_2M               = 120000,               // ms (2 minutes)
    BG_START_DELAY_1M               = 60000,                // ms (1 minute)
    BG_START_DELAY_30S              = 30000,                // ms (30 seconds)
    BG_START_DELAY_20S              = 20000,                // ms (20 seconds)
    BG_START_DELAY_15S              = 15000,                // ms (15 seconds) Used only in arena
    BG_START_DELAY_10S              = 10000,                // ms (10 seconds)
    BG_START_DELAY_NONE             = 0                     // ms
};

enum BattlegroundBuffObjects
{
    BG_OBJECTID_SPEEDBUFF_ENTRY     = 179871,
    BG_OBJECTID_REGENBUFF_ENTRY     = 179904,
    BG_OBJECTID_BERSERKERBUFF_ENTRY = 179905
};

const uint32 Buff_Entries[3] = { BG_OBJECTID_SPEEDBUFF_ENTRY, BG_OBJECTID_REGENBUFF_ENTRY, BG_OBJECTID_BERSERKERBUFF_ENTRY };

enum BattlegroundStatus
{
    STATUS_NONE         = 0,                                // first status, should mean bg is not instance
    STATUS_WAIT_QUEUE   = 1,                                // means bg is empty and waiting for queue
    STATUS_WAIT_JOIN    = 2,                                // this means, that BG has already started and it is waiting for more players
    STATUS_IN_PROGRESS  = 3,                                // means bg is running
    STATUS_WAIT_LEAVE   = 4                                 // means some faction has won BG and it is ending
};

struct BattlegroundPlayer
{
    time_t OfflineRemoveTime;                              // for tracking and removing offline players from queue after 5 minutes
    uint32 Team;                                           // Player's team
};

struct BattlegroundObjectInfo
{
    BattlegroundObjectInfo() : object(nullptr), timer(0), spellid(0) { }

    GameObject  *object;
    int32       timer;
    uint32      spellid;
};

enum ArenaType
{
    ARENA_TYPE_2v2          = 2,
    ARENA_TYPE_3v3          = 3,
    ARENA_TYPE_4v4          = 4,
    ARENA_TYPE_5v5          = 5
};

enum BattlegroundStartingEvents
{
    BG_STARTING_EVENT_NONE  = 0x00,
    BG_STARTING_EVENT_1     = 0x01,
    BG_STARTING_EVENT_2     = 0x02,
    BG_STARTING_EVENT_3     = 0x04,
    BG_STARTING_EVENT_4     = 0x08
};

enum BattlegroundStartingEventsIds
{
    BG_STARTING_EVENT_FIRST     = 0,
    BG_STARTING_EVENT_SECOND    = 1,
    BG_STARTING_EVENT_THIRD     = 2,
    BG_STARTING_EVENT_FOURTH    = 3
};
#define BG_STARTING_EVENT_COUNT 4

enum BGHonorMode
{
    BG_NORMAL = 0,
    BG_HOLIDAY,
    BG_HONOR_MODE_NUM
};

#define BG_AWARD_ARENA_POINTS_MIN_LEVEL 71
#define ARENA_TIMELIMIT_POINTS_LOSS    -16

/*
This class is used to:
1. Add player to battleground
2. Remove player from battleground
3. some certain cases, same for all battlegrounds
4. It has properties same for all battlegrounds
*/
class TC_GAME_API Battleground
{
    public:
        Battleground();
        virtual ~Battleground();

        void Update(uint32 diff);

        virtual bool SetupBattleground()                    // must be implemented in BG subclass
        {
            return true;
        }
        virtual void Reset();                               // resets all common properties for battlegrounds, must be implemented and called in BG subclass
        virtual void StartingEventCloseDoors() { }
        virtual void StartingEventOpenDoors() { }
        virtual void ResetBGSubclass() { }                  // must be implemented in BG subclass

        virtual void DestroyGate(Player* /*player*/, GameObject* /*go*/) { }

        /* achievement req. */
        virtual bool IsAllNodesControlledByTeam(uint32 /*team*/) const { return false; }
        void StartTimedAchievement(AchievementCriteriaTimedTypes type, uint32 entry);
        virtual bool CheckAchievementCriteriaMeet(uint32 /*criteriaId*/, Player const* /*player*/, Unit const* /*target*/ = nullptr, uint32 /*miscvalue1*/ = 0);

        /* Battleground */
        // Get methods:
        std::string const& GetName() const  { return m_Name; }
        BattlegroundTypeId GetTypeID(bool GetRandom = false) const { return GetRandom ? m_RandomTypeID : m_TypeID; }
        BattlegroundBracketId GetBracketId() const { return m_BracketId; }
        uint32 GetInstanceID() const        { return m_InstanceID; }
        BattlegroundStatus GetStatus() const { return m_Status; }
        uint32 GetClientInstanceID() const  { return m_ClientInstanceID; }
        uint32 GetStartTime() const         { return m_StartTime; }
        uint32 GetEndTime() const           { return m_EndTime; }
        uint32 GetLastResurrectTime() const { return m_LastResurrectTime; }
        uint32 GetMaxPlayers() const        { return m_MaxPlayers; }
        uint32 GetMinPlayers() const        { return m_MinPlayers; }

        uint32 GetMinLevel() const          { return m_LevelMin; }
        uint32 GetMaxLevel() const          { return m_LevelMax; }

        uint32 GetMaxPlayersPerTeam() const { return m_MaxPlayersPerTeam; }
        uint32 GetMinPlayersPerTeam() const { return m_MinPlayersPerTeam; }

        int32 GetStartDelayTime() const     { return m_StartDelayTime; }
        uint8 GetArenaType() const          { return m_ArenaType; }
        PvPTeamId GetWinner() const { return _winnerTeamId; }
        uint32 GetScriptId() const          { return ScriptId; }
        uint32 GetFightId() const { return m_FightId; }
        uint32 GetReplayId() const { return m_ReplayId; }
        bool IsReplay() const { return m_IsReplay; }
        void SetReplay(bool isReplay) { m_IsReplay = isReplay; }
        void SetFightId(uint32 FightId) { m_FightId = FightId; }
        void SetReplayId(uint32 ReplayId) { m_ReplayId = ReplayId; }
        uint32 GetBonusHonorFromKill(uint32 kills) const;
        bool IsRandom() const { return m_IsRandom; }

        // Set methods:
        void SetName(std::string const& name) { m_Name = name; }
        void SetTypeID(BattlegroundTypeId TypeID) { m_TypeID = TypeID; }
        void SetRandomTypeID(BattlegroundTypeId TypeID) { m_RandomTypeID = TypeID; }
        //here we can count minlevel and maxlevel for players
        void SetBracket(PvPDifficultyEntry const* bracketEntry);
        void SetInstanceID(uint32 InstanceID) { m_InstanceID = InstanceID; }
        void SetStatus(BattlegroundStatus Status) { m_Status = Status; }
        void SetClientInstanceID(uint32 InstanceID) { m_ClientInstanceID = InstanceID; }
        void SetStartTime(uint32 Time)      { m_StartTime = Time; }
        void SetEndTime(uint32 Time)        { m_EndTime = Time; }
        void SetLastResurrectTime(uint32 Time) { m_LastResurrectTime = Time; }
        void SetMaxPlayers(uint32 MaxPlayers) { m_MaxPlayers = MaxPlayers; }
        void SetMinPlayers(uint32 MinPlayers) { m_MinPlayers = MinPlayers; }
        void SetLevelRange(uint32 min, uint32 max) { m_LevelMin = min; m_LevelMax = max; }
        void SetRated(bool state)           { m_IsRated = state; }
        void SetArenaType(uint8 type)       { m_ArenaType = type; }
        void SetArenaorBGType(bool _isArena) { m_IsArena = _isArena; }
        void SetWinner(PvPTeamId winnerTeamId) { _winnerTeamId = winnerTeamId; }
        void SetScriptId(uint32 scriptId)   { ScriptId = scriptId; }

        void ModifyStartDelayTime(int diff) { m_StartDelayTime -= diff; }
        void SetStartDelayTime(int Time)    { m_StartDelayTime = Time; }
        bool SkipStartDelay();

        void SetMaxPlayersPerTeam(uint32 MaxPlayers) { m_MaxPlayersPerTeam = MaxPlayers; }
        void SetMinPlayersPerTeam(uint32 MinPlayers) { m_MinPlayersPerTeam = MinPlayers; }

        void AddToBGFreeSlotQueue();                        //this queue will be useful when more battlegrounds instances will be available
        void RemoveFromBGFreeSlotQueue();                   //this method could delete whole BG instance, if another free is available

        void DecreaseInvitedCount(uint32 team)
        {
            if (team == ALLIANCE)
            {
                if (m_InvitedAlliance > 0)
                    --m_InvitedAlliance;
            }
            else if (m_InvitedHorde > 0)
                --m_InvitedHorde;
        }
        void IncreaseInvitedCount(uint32 team)      { (team == ALLIANCE) ? ++m_InvitedAlliance : ++m_InvitedHorde; }

        void SetRandom(bool isRandom) { m_IsRandom = isRandom; }
        uint32 GetInvitedCount(uint32 team) const   { return (team == ALLIANCE) ? m_InvitedAlliance : m_InvitedHorde; }
        bool HasFreeSlots() const;
        uint32 GetFreeSlotsForTeam(uint32 Team) const;

        bool isArena() const        { return m_IsArena; }
        bool isBattleground() const { return !m_IsArena; }
        bool isRated() const        { return m_IsRated; }
        bool IsCustomGame() const   { return m_IsCustomGame; }
        void ConfigureCustomGame(BattlegroundCustomRules const& rules) { m_IsCustomGame = true; m_CustomRules = rules; }
        void SetCustomGameBotOnlyPreparation(bool enabled) { m_CustomGameBotOnlyPreparation = enabled; }
        bool HasCustomGameBotOnlyPreparation() const { return m_CustomGameBotOnlyPreparation; }
        void SetCustomGamePendingCloneCount(uint32 count) { m_CustomGamePendingCloneCount = count; }
        void ResolveCustomGamePendingClone()
        {
            if (m_CustomGamePendingCloneCount)
                --m_CustomGamePendingCloneCount;
        }
        bool HasCustomGamePendingClones() const { return m_IsCustomGame && m_CustomGamePendingCloneCount != 0; }
        BattlegroundCustomRules const& GetCustomRules() const { return m_CustomRules; }
        void SendCustomGameRulesTo(Player* player) const;
        void SendSpectatorBattlefieldStatusTo(Player* player);
        bool HasCustomWeatherOverride() const { return m_IsCustomGame && m_CustomRules.Weather != BattlegroundCustomWeather::Normal; }
        bool ShouldShowFlagOnMapTo(Player const* viewer, TeamId flagTeam) const;
        uint32 GetFlagCaptureLimit(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.FlagCaptureLimit ? m_CustomRules.FlagCaptureLimit : defaultValue; }
        uint32 GetResourceLimit(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.ResourceLimit ? m_CustomRules.ResourceLimit : defaultValue; }
        uint32 GetDeathmatchKillLimit(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.DeathmatchKillLimit ? m_CustomRules.DeathmatchKillLimit : defaultValue; }
        uint32 GetNodeFlagCaptureTime(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.NodeFlagCaptureTimeMs ? m_CustomRules.NodeFlagCaptureTimeMs : defaultValue; }
        uint32 GetNodeBaseCaptureTime(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.NodeBaseCaptureTimeMs ? m_CustomRules.NodeBaseCaptureTimeMs : defaultValue; }
        uint32 ScaleResourceGain(uint32 value) const { return m_IsCustomGame ? std::max<uint32>(1, value * m_CustomRules.ResourceGainPercent / 100) : value; }

        typedef std::map<ObjectGuid, BattlegroundPlayer> BattlegroundPlayerMap;
        BattlegroundPlayerMap const& GetPlayers() const { return m_Players; }
        Player* _GetPlayer(ObjectGuid guid, bool offlineRemove, char const* context) const;
        Player* _GetPlayer(BattlegroundPlayerMap::iterator itr, char const* context) { return _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, context); }
        Player* _GetPlayer(BattlegroundPlayerMap::const_iterator itr, char const* context) const { return _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, context); }
        uint32 GetPlayersSize() const { return m_Players.size(); }

        typedef std::map<uint32, BattlegroundScore*> BattlegroundScoreMap;
        uint32 GetPlayerScoresSize() const { return PlayerScores.size(); }

        uint32 GetReviveQueueSize() const { return m_ReviveQueue.size(); }

        void AddPlayerToResurrectQueue(ObjectGuid npc_guid, ObjectGuid player_guid);
        void RemovePlayerFromResurrectQueue(ObjectGuid player_guid);
        uint32 GetTimeUntilResurrection() const;
        bool IsPlayerInResurrectQueue(ObjectGuid player_guid) const;

        /// Relocate all players in ReviveQueue to the closest graveyard
        void RelocateDeadPlayers(ObjectGuid guideGuid);

        void StartBattleground();

        void toggleReplay(uint32 replayId) { m_IsReplay = replayId != 0; m_ReplayId = replayId; }

        GameObject* GetBGObject(uint32 type, bool logError = true);
        Creature* GetBGCreature(uint32 type, bool logError = true);

        // Location
        void SetMapId(uint32 MapID) { m_MapId = MapID; }
        uint32 GetMapId() const { return m_MapId; }

        // Map pointers
        void SetBgMap(BattlegroundMap* map) { m_Map = map; }
        BattlegroundMap* GetBgMap() const { ASSERT(m_Map); return m_Map; }
        BattlegroundMap* FindBgMap() const { return m_Map; }

        void SetTeamStartPosition(TeamId teamId, Position const& pos);
        Position const* GetTeamStartPosition(TeamId teamId) const;

        void SetStartMaxDist(float startMaxDist) { m_StartMaxDist = startMaxDist; }
        float GetStartMaxDist() const { return m_StartMaxDist; }

        // Packet Transfer
        // method that should fill worldpacket with actual world states (not yet implemented for all battlegrounds!)
        virtual void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& /*packet*/) { }
        void SendPacketToTeam(uint32 TeamID, WorldPacket const* packet, Player* sender = nullptr, bool self = true);
        void SendPacketToAll(WorldPacket const* packet);

        void SendChatMessage(Creature* source, uint8 textId, WorldObject* target = nullptr);
        void SendBroadcastText(uint32 id, ChatMsg msgType, WorldObject const* target = nullptr);

        template<class Do>
        void BroadcastWorker(Do& _do);

        void SaveReplay();
        void PlaySoundToTeam(uint32 soundID, uint32 teamID);
        void PlaySoundToAll(uint32 soundID);
        void CastSpellOnTeam(uint32 SpellID, uint32 TeamID);
        void RemoveAuraOnTeam(uint32 SpellID, uint32 TeamID);
        void RewardHonorToTeam(uint32 Honor, uint32 TeamID);
        void CenturionRewardHonorToTeam(uint32 Honor, uint32 TeamID);
        void RewardReputationToTeam(uint32 faction_id, uint32 Reputation, uint32 TeamID);
        void UpdateWorldState(uint32 variable, uint32 value);
        virtual void EndBattleground(uint32 winner);
        void BlockMovement(Player* player);

        void SendWarningToAll(uint32 entry, ...);
        void SendMessageToAll(uint32 entry, ChatMsg type, Player const* source = nullptr);
        void PSendMessageToAll(uint32 entry, ChatMsg type, Player const* source, ...);

        // Raid Group
        Group* GetBgRaid(uint32 TeamID) const { return TeamID == ALLIANCE ? m_BgRaids[TEAM_ALLIANCE] : m_BgRaids[TEAM_HORDE]; }
        void SetBgRaid(uint32 TeamID, Group* bg_raid);

        void BuildPvPLogDataPacket(WorldPacket& data);
        virtual bool UpdatePlayerScore(Player* player, uint32 type, uint32 value, bool doAddHonor = true);

        static TeamId GetTeamIndexByTeamId(uint32 Team) { return Team == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE; }
        uint32 GetPlayersCountByTeam(uint32 Team) const { return m_PlayersCount[GetTeamIndexByTeamId(Team)]; }
        uint32 GetAlivePlayersCountByTeam(uint32 Team) const;   // used in arenas to correctly handle death in spirit of redemption / last stand etc. (killer = killed) cases
        void UpdatePlayersCountByTeam(uint32 Team, bool remove)
        {
            if (remove)
                --m_PlayersCount[GetTeamIndexByTeamId(Team)];
            else
                ++m_PlayersCount[GetTeamIndexByTeamId(Team)];
        }

        virtual void CheckWinConditions() { }

        // used for rated arena battles
        void SetArenaTeamIdForTeam(uint32 Team, uint32 ArenaTeamId) { m_ArenaTeamIds[GetTeamIndexByTeamId(Team)] = ArenaTeamId; }
        uint32 GetArenaTeamIdForTeam(uint32 Team) const             { return m_ArenaTeamIds[GetTeamIndexByTeamId(Team)]; }
        uint32 GetArenaTeamIdByIndex(uint32 index) const { return m_ArenaTeamIds[index]; }
        void SetArenaMatchmakerRating(uint32 Team, uint32 MMR){ m_ArenaTeamMMR[GetTeamIndexByTeamId(Team)] = MMR; }
        uint32 GetArenaMatchmakerRating(uint32 Team) const          { return m_ArenaTeamMMR[GetTeamIndexByTeamId(Team)]; }

        // Triggers handle
        // must be implemented in BG subclass
        virtual void HandleAreaTrigger(Player* /*player*/, uint32 /*Trigger*/);
        // must be implemented in BG subclass if need AND call base class generic code
        virtual void HandleKillPlayer(Player* player, Player* killer);
        virtual void HandleKillUnit(Creature* /*creature*/, Player* /*killer*/) { }

        // Battleground events
        virtual void EventPlayerDroppedFlag(Player* /*player*/) { }
        virtual void EventPlayerClickedOnFlag(Player* /*player*/, GameObject* /*target_obj*/) { }
        void EventPlayerLoggedIn(Player* player);
        void EventPlayerLoggedOut(Player* player);
        virtual void ProcessEvent(WorldObject* /*obj*/, uint32 /*eventId*/, WorldObject* /*invoker*/ = nullptr) { }

        // this function can be used by spell to interact with the BG map
        virtual void DoAction(uint32 /*action*/, ObjectGuid /*var*/) { }

        virtual void HandlePlayerResurrect(Player* /*player*/) { }

        // Death related
        virtual WorldSafeLocsEntry const* GetClosestGraveyard(Player* player);

        virtual void AddPlayer(Player* player);                // must be implemented in BG subclass

        void AddOrSetPlayerToCorrectBgGroup(Player* player, uint32 team);

        virtual void RemovePlayerAtLeave(ObjectGuid guid, bool Transport, bool SendPacket);
                                                            // can be extended in in BG subclass

        void HandleTriggerBuff(ObjectGuid go_guid);

        // Whether this player may set off an environmental powerup - the Speed,
        // Restoration and Berserking runes and any custom one. Consulted by the
        // ownerless-trap search in GameObject::Update, which is the only thing
        // that fires those. Default is "anyone standing on it"; Violet Hold
        // narrows it to the human side so the clone waves cannot eat the
        // rewards the party is meant to be racing them for.
        virtual bool CanPickUpPowerup(Player const* /*player*/) const { return true; }

        // Whether a dead player may walk back to their own corpse and reclaim
        // it. Arenas refuse this outright via Player::InArena, and this is the
        // same rule made available to other modes.
        //
        // Refusing the reclaim does NOT remove the corpse or block being raised
        // by someone else: HandleReclaimCorpse is only the self-service route.
        // Rebirth, soulstones and Reincarnation all still work, exactly as they
        // do in an arena. Public because that handler is the caller.
        virtual bool AllowsCorpseReclaim() const { return true; }
        void SetHoliday(bool is_holiday);

        /// @todo make this protected:
        GuidVector BgObjects;
        GuidVector BgCreatures;
        void SpawnBGObject(uint32 type, uint32 respawntime);
        virtual bool AddObject(uint32 type, uint32 entry, float x, float y, float z, float o, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime = 0, GOState goState = GO_STATE_READY);
        bool AddObject(uint32 type, uint32 entry, Position const& pos, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime = 0, GOState goState = GO_STATE_READY);
        virtual Creature* AddCreature(uint32 entry, uint32 type, float x, float y, float z, float o, TeamId teamId = TEAM_NEUTRAL, uint32 respawntime = 0, Transport* transport = nullptr);
        Creature* AddCreature(uint32 entry, uint32 type, Position const& pos, TeamId teamId = TEAM_NEUTRAL, uint32 respawntime = 0, Transport* transport = nullptr);
        bool DelCreature(uint32 type);
        bool DelObject(uint32 type);
        bool RemoveObjectFromWorld(uint32 type);
        virtual bool AddSpiritGuide(uint32 type, float x, float y, float z, float o, TeamId teamId = TEAM_NEUTRAL);
        bool AddSpiritGuide(uint32 type, Position const& pos, TeamId teamId = TEAM_NEUTRAL);
        int32 GetObjectType(ObjectGuid guid);

        void DoorOpen(uint32 type);
        void DoorClose(uint32 type);

        virtual bool HandlePlayerUnderMap(Player* /*player*/) { return false; }

        // since arenas can be AvA or Hvh, we have to get the "temporary" team of a player
        uint32 GetPlayerTeam(ObjectGuid guid) const;
        uint32 GetOtherTeam(uint32 teamId) const;
        bool IsPlayerInBattleground(ObjectGuid guid) const;

        bool ToBeDeleted() const { return m_SetDeleteThis; }
        void SetDeleteThis() { m_SetDeleteThis = true; }

        void RewardXPAtKill(Player* killer, Player* victim);
        bool CanAwardArenaPoints() const { return m_LevelMin >= BG_AWARD_ARENA_POINTS_MIN_LEVEL; }

        virtual ObjectGuid GetFlagPickerGUID(int32 /*team*/ = -1) const { return ObjectGuid::Empty; }
        virtual void SetDroppedFlagGUID(ObjectGuid /*guid*/, int32 /*team*/ = -1) { }
        // Flag battlegrounds can expose their currently available pickup and
        // the scoring destination for a specific carrier. Keeping
        // this in the battleground contract lets objective consumers remain
        // independent of individual battleground layouts.
        virtual ObjectGuid GetFlagPickupGUID(ObjectGuid /*playerGuid*/) const { return ObjectGuid::Empty; }
        virtual bool GetFlagCapturePosition(ObjectGuid /*carrierGuid*/, Position& /*position*/) const { return false; }
        virtual uint32 GetDynamicNodeCount() const { return 0; }
        virtual bool GetDynamicNodeInfo(ObjectGuid /*playerGuid*/, uint32 /*nodeId*/, BattlegroundNodeObjective& /*node*/) const { return false; }
        bool GetNodeObjective(ObjectGuid playerGuid, BattlegroundNodeObjective& objective) const;
        virtual void HandleQuestComplete(uint32 /*questid*/, Player* /*player*/) { }
        virtual bool CanActivateGO(int32 /*entry*/, uint32 /*team*/) const { return true; }
        virtual bool IsSpellAllowed(uint32 /*spellId*/, Player const* /*player*/) const { return true; }
        uint32 GetTeamScore(uint32 TeamID) const;

        virtual uint32 GetPrematureWinner();

        // because BattleGrounds with different types and same level range has different m_BracketId
        uint8 GetUniqueBracketId() const;

        //spectators
        typedef std::set<Player*> SpectatorList;
        typedef std::map<ObjectGuid, ObjectGuid> ToBeTeleportedMap;
        void AddSpectator(Player* p) { m_Spectators.insert(p); }
        void RemoveSpectator(Player* p) { m_Spectators.erase(p); }
        bool HaveSpectators() { return !m_Spectators.empty(); }
        [[nodiscard]] const SpectatorList& GetSpectators() const { return m_Spectators; }
        void AddToBeTeleported(ObjectGuid spectator, ObjectGuid participant) { m_ToBeTeleported[spectator] = participant; }
        void RemoveToBeTeleported(ObjectGuid spectator) { ToBeTeleportedMap::iterator itr = m_ToBeTeleported.find(spectator); if (itr != m_ToBeTeleported.end()) m_ToBeTeleported.erase(itr); }
        void SpectatorsSendPacket(WorldPacket& data);

        // this method is called, when BG cannot spawn its own spirit guide, or something is wrong, It correctly ends Battleground
        void EndNow();

    protected:
        void PlayerAddedToBGCheckIfBGIsRunning(Player* player);
        Player* _GetPlayerForTeam(uint32 teamId, BattlegroundPlayerMap::const_iterator itr, char const* context) const;

        void _ProcessOfflineQueue();
        void _ProcessResurrect(uint32 diff);
        void _ProcessProgress(uint32 diff);
        void _ProcessLeave(uint32 diff);
        void _ProcessJoin(uint32 diff);
        void _CheckSafePositions(uint32 diff);

        uint32 GetConfiguredResurrectionInterval(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.ResurrectionIntervalMs ? m_CustomRules.ResurrectionIntervalMs : defaultValue; }
        virtual uint32 GetResurrectionInterval() const { return GetConfiguredResurrectionInterval(RESURRECTION_INTERVAL); }
        virtual uint32 GetBuffRespawnTime(uint32 type) const { return BUFF_RESPAWN_TIME; }

        // Whether an under-populated team should start the premature-finish
        // countdown. Modes that legitimately run with one side empty - Violet
        // Hold sits at zero enemies between waves - opt out here.
        virtual bool AllowsPrematureFinish() const { return true; }

        // Scorekeeping
        BattlegroundScoreMap PlayerScores;                // Player scores
        // must be implemented in BG subclass
        virtual void RemovePlayer(Player* /*player*/, ObjectGuid /*guid*/, uint32 /*team*/) { }
        virtual void ModifyEndOfMatchHonorRewards(uint32 /*winner*/, uint32 /*team*/, uint32& /*winnerHonor*/, uint32& /*loserHonor*/) const { }
        // Same hook for the copper payout: sees the amounts as they will be
        // paid (arena multiplier already applied for arenas), before the
        // 15-second minimum-duration gate.
        virtual void ModifyEndOfMatchMoneyRewards(uint32 /*winner*/, uint32 /*team*/, uint32& /*winnerMoney*/, uint32& /*loserMoney*/) const { }

        // Player lists, those need to be accessible by inherited classes
        BattlegroundPlayerMap m_Players;
        SpectatorList m_Spectators;
        ToBeTeleportedMap m_ToBeTeleported;
        // Spirit Guide guid + Player list GUIDS
        std::map<ObjectGuid, GuidVector> m_ReviveQueue;

        // these are important variables used for starting messages
        uint8 m_Events;
        BattlegroundStartTimeIntervals StartDelayTimes[BG_STARTING_EVENT_COUNT];
        // this must be filled in constructors!
        uint32 StartMessageIds[BG_STARTING_EVENT_COUNT];

        bool   m_BuffChange;
        bool   m_IsRandom;

        BGHonorMode m_HonorMode;
        int32 m_TeamScores[PVP_TEAMS_COUNT];

        ArenaTeamScore _arenaTeamScores[PVP_TEAMS_COUNT];

    private:
        // Battleground
        BattlegroundTypeId m_TypeID;
        BattlegroundTypeId m_RandomTypeID;
        uint32 m_InstanceID;                                // Battleground Instance's GUID!
        BattlegroundStatus m_Status;
        uint32 m_ClientInstanceID;                          // the instance-id which is sent to the client and without any other internal use
        uint32 m_StartTime;
        uint32 m_ResetStatTimer;
        uint32 m_ValidStartPositionTimer;
        int32 m_EndTime;                                    // it is set to 120000 when bg is ending and it decreases itself
        uint32 m_LastResurrectTime;
        BattlegroundBracketId m_BracketId;
        uint8  m_ArenaType;                                 // 2=2v2, 3=3v3, 5=5v5
        bool   m_InBGFreeSlotQueue;                         // used to make sure that BG is only once inserted into the BattlegroundMgr.BGFreeSlotQueue[bgTypeId] deque
        bool   m_SetDeleteThis;                             // used for safe deletion of the bg after end / all players leave
        bool   m_IsCustomGame;
        bool   m_CustomGameBotOnlyPreparation;
        uint32 m_CustomGamePendingCloneCount;
        BattlegroundCustomRules m_CustomRules;
        bool   m_IsArena;
        PvPTeamId _winnerTeamId;
        int32  m_StartDelayTime;
        bool   m_IsRated;                                   // is this battle rated?
        bool   m_PrematureCountDown;
        bool   m_HasEverHadNonVirtualHumanParticipant;
        uint32 m_NoNonVirtualHumanElapsed;
        bool   m_IsReplay;
        uint32 m_ReplayId;
        uint32 m_FightId;
        uint32 m_PrematureCountDownTimer;
        std::string m_Name;

        /* Pre- and post-update hooks */

        /**
         * @brief Pre-update hook.
         *
         * Will be called before battleground update is started. Depending on
         * the result of this call actual update body may be skipped.
         *
         * @param diff a time difference between two worldserver update loops in
         * milliseconds.
         *
         * @return @c true if update must be performed, @c false otherwise.
         *
         * @see Update(), PostUpdateImpl().
         */
        virtual bool PreUpdateImpl(uint32 /* diff */) { return true; }

        /**
         * @brief Post-update hook.
         *
         * Will be called after battleground update has passed. May be used to
         * implement custom update effects in subclasses.
         *
         * @param diff a time difference between two worldserver update loops in
         * milliseconds.
         *
         * @see Update(), PreUpdateImpl().
         */
        virtual void PostUpdateImpl(uint32 /* diff */) { }

        // Player lists
        GuidVector m_ResurrectQueue;                        // Player GUID
        std::deque<ObjectGuid> m_OfflineQueue;              // Player GUID

        // Invited counters are useful for player invitation to BG - do not allow, if BG is started to one faction to have 2 more players than another faction
        // Invited counters will be changed only when removing already invited player from queue, removing player from battleground and inviting player to BG
        // Invited players counters
        uint32 m_InvitedAlliance;
        uint32 m_InvitedHorde;

        // Raid Group
        Group* m_BgRaids[PVP_TEAMS_COUNT];                   // 0 - alliance, 1 - horde

        // Players count by team
        uint32 m_PlayersCount[PVP_TEAMS_COUNT];

        // Arena team ids by team
        uint32 m_ArenaTeamIds[PVP_TEAMS_COUNT];

        uint32 m_ArenaTeamMMR[PVP_TEAMS_COUNT];

        // Limits
        uint32 m_LevelMin;
        uint32 m_LevelMax;
        uint32 m_MaxPlayersPerTeam;
        uint32 m_MaxPlayers;
        uint32 m_MinPlayersPerTeam;
        uint32 m_MinPlayers;

        // Start location
        uint32 m_MapId;
        BattlegroundMap* m_Map;
        Position StartPosition[PVP_TEAMS_COUNT];
        float m_StartMaxDist;
        uint32 ScriptId;
};
#endif
