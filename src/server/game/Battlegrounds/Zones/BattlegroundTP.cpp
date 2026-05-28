/*
 * Copyright (C) ArkCORE
 * Copyright (C) 2019+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: http://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
*/

#include "BattlegroundTP.h"
#include "Creature.h"
#include "GameObject.h"
#include "Language.h"
#include "Log.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Duration.h"
#include "Map.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "Battleground.h"
#include <unordered_map>
#include <iomanip>
#include <sstream>

#include "ScriptMgr.h"
#include "Config.h"

namespace
{
    constexpr uint32 BG_TP_BUFF_RESPAWN_TIME = 150;

    TeamId GetOtherTwinPeaksTeamId(TeamId teamId)
    {
        return teamId == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    }
}

// these variables aren't used outside of this file, so declare them only here
enum BG_TP_Rewards
{
    BG_TP_WIN = 0,
    BG_TP_FLAG_CAP,
    BG_TP_MAP_COMPLETE,
    BG_TP_REWARD_NUM
};

uint32 BG_TP_Honor[BG_HONOR_MODE_NUM][BG_TP_REWARD_NUM] = {
    {20, 40, 40}, // normal honor
    {60, 40, 80}  // holiday
};

uint32 BG_TP_Reputation[BG_HONOR_MODE_NUM][BG_TP_REWARD_NUM] = {
    {0, 35, 0},   // normal honor
    {0, 45, 0}    // holiday
};

void BattlegroundTPScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(2); // Objectives Count
    data << uint32(FlagCaptures);
    data << uint32(FlagReturns);
}

BattlegroundTP::BattlegroundTP()
{
    BgObjects.resize(BG_TP_OBJECT_MAX);
    BgCreatures.resize(BG_CREATURES_MAX_TP);

    StartMessageIds[BG_STARTING_EVENT_FIRST]  = LANG_BG_TP_START_TWO_MINUTES;
    StartMessageIds[BG_STARTING_EVENT_SECOND] = LANG_BG_TP_START_ONE_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_THIRD]  = LANG_BG_TP_START_HALF_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = LANG_BG_TP_HAS_BEGUN;

    _flagKeepers[TEAM_ALLIANCE].Clear();
    _flagKeepers[TEAM_HORDE].Clear();
    _droppedFlagGUID[TEAM_ALLIANCE].Clear();
    _droppedFlagGUID[TEAM_HORDE].Clear();
    _flagState[TEAM_ALLIANCE]   = BG_TP_FLAG_STATE_ON_BASE;
    _flagState[TEAM_HORDE]      = BG_TP_FLAG_STATE_ON_BASE;
    _lastFlagCaptureTeam        = TEAM_NEUTRAL;
    _reputationCapture = 0;
    _honorWinKills = 0;
    _honorEndKills = 0;
}

BattlegroundTP::~BattlegroundTP() { }

char const* BattlegroundTP::GetCTFFlagStateToken(uint8 flagState) const
{
    switch (flagState)
    {
        case BG_TP_FLAG_STATE_ON_BASE: return "BASE";
        case BG_TP_FLAG_STATE_ON_PLAYER: return "PLAYER";
        case BG_TP_FLAG_STATE_ON_GROUND: return "GROUND";
        case BG_TP_FLAG_STATE_WAIT_RESPAWN: return "WAIT";
        default: return "BASE";
    }
}

std::string BattlegroundTP::FormatCTFCoord(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(4) << value;
    return stream.str();
}

bool BattlegroundTP::GetCTFFlagWorldPositionByIdentity(TeamId flagTeam, float& x, float& y) const
{
    if (flagTeam != TEAM_ALLIANCE && flagTeam != TEAM_HORDE)
        return false;

    if (_flagState[flagTeam] == BG_TP_FLAG_STATE_ON_PLAYER)
    {
        if (Player* carrier = ObjectAccessor::FindPlayer(_flagKeepers[flagTeam]))
        {
            x = carrier->GetPositionX();
            y = carrier->GetPositionY();
            return true;
        }
    }
    else if (_flagState[flagTeam] == BG_TP_FLAG_STATE_ON_GROUND)
    {
        if (Map* map = GetBgMap())
            if (GameObject* droppedFlag = map->GetGameObject(GetDroppedFlagGUID(flagTeam)))
            {
                x = droppedFlag->GetPositionX();
                y = droppedFlag->GetPositionY();
                return true;
            }
    }

    return false;
}

std::string BattlegroundTP::BuildCTFFlagFullPayload() const
{
    std::string allianceCarrier;
    std::string hordeCarrier;
    std::string allianceX;
    std::string allianceY;
    std::string hordeX;
    std::string hordeY;

    if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_PLAYER)
        if (Player* carrier = ObjectAccessor::FindPlayer(_flagKeepers[TEAM_ALLIANCE]))
            allianceCarrier = carrier->GetName();
    if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_PLAYER)
        if (Player* carrier = ObjectAccessor::FindPlayer(_flagKeepers[TEAM_HORDE]))
            hordeCarrier = carrier->GetName();

    float x = 0.0f;
    float y = 0.0f;
    if (GetCTFFlagWorldPositionByIdentity(TEAM_ALLIANCE, x, y))
    {
        Map2ZoneCoordinates(x, y, 5031);
        allianceX = FormatCTFCoord(x / 100.0f);
        allianceY = FormatCTFCoord(y / 100.0f);
    }
    if (GetCTFFlagWorldPositionByIdentity(TEAM_HORDE, x, y))
    {
        Map2ZoneCoordinates(x, y, 5031);
        hordeX = FormatCTFCoord(x / 100.0f);
        hordeY = FormatCTFCoord(y / 100.0f);
    }

    return std::string("FULL:") + allianceCarrier + ":" + hordeCarrier + ":" + GetCTFFlagStateToken(_flagState[TEAM_ALLIANCE]) + ":" +
        GetCTFFlagStateToken(_flagState[TEAM_HORDE]) + ":" + allianceX + ":" + allianceY + ":" + hordeX + ":" + hordeY;
}

void BattlegroundTP::SendCTFFlagAddonMessage(std::string const& payload)
{
    std::string message = "CWSG\t" + payload;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty, ObjectGuid::Empty, message, 0);
    SendPacketToAll(&data);
}

void BattlegroundTP::BroadcastCTFFlagFullState() { SendCTFFlagAddonMessage(BuildCTFFlagFullPayload()); }

void BattlegroundTP::SendCTFFlagFullStateTo(Player* player)
{
    if (!player || !player->GetSession())
        return;

    std::string message = "CWSG\t" + BuildCTFFlagFullPayload();
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty, ObjectGuid::Empty, message, 0);
    player->SendDirectMessage(&data);
}

void BattlegroundTP::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        _bgEvents.Update(diff);
        switch (_bgEvents.ExecuteEvent())
        {
            case BG_TP_EVENT_UPDATE_GAME_TIME:
            case BG_TP_EVENT_NO_TIME_LEFT:
                break;
            case BG_TP_EVENT_RESPAWN_BOTH_FLAGS:
                _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_BASE;
                _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_BASE;
                SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
                SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
                UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 0);
                UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 0);
                UpdateFlagState(TEAM_ALLIANCE, 1);
                UpdateFlagState(TEAM_HORDE, 1);
                SendBroadcastText(LANG_BG_TP_F_PLACED, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                PlaySoundToAll(BG_TP_SOUND_FLAGS_RESPAWNED);
                SendCTFFlagAddonMessage("A:RETURN");
                SendCTFFlagAddonMessage("H:RETURN");
                BroadcastCTFFlagFullState();
                break;
            case BG_TP_EVENT_ALLIANCE_DROP_FLAG:
                RespawnFlagAfterDrop(TEAM_ALLIANCE);
                break;
            case BG_TP_EVENT_HORDE_DROP_FLAG:
                RespawnFlagAfterDrop(TEAM_HORDE);
                break;
            case BG_TP_EVENT_BOTH_FLAGS_KEPT10:
                if (Player* player = ObjectAccessor::GetPlayer(FindBgMap(), GetFlagPickerGUID(TEAM_ALLIANCE)))
                    player->CastSpell(player, BG_TP_SPELL_FOCUSED_ASSAULT, true);

                if (Player* player = ObjectAccessor::GetPlayer(FindBgMap(), GetFlagPickerGUID(TEAM_HORDE)))
                    player->CastSpell(player, BG_TP_SPELL_FOCUSED_ASSAULT, true);
                break;
            case BG_TP_EVENT_BOTH_FLAGS_KEPT15:
                if (Player* player = ObjectAccessor::GetPlayer(FindBgMap(), GetFlagPickerGUID(TEAM_ALLIANCE)))
                {
                    player->RemoveAurasDueToSpell(BG_TP_SPELL_FOCUSED_ASSAULT);
                    player->CastSpell(player, BG_TP_SPELL_BRUTAL_ASSAULT, true);
                }

                if (Player* player = ObjectAccessor::GetPlayer(FindBgMap(), GetFlagPickerGUID(TEAM_HORDE)))
                {
                    player->RemoveAurasDueToSpell(BG_TP_SPELL_FOCUSED_ASSAULT);
                    player->CastSpell(player, BG_TP_SPELL_BRUTAL_ASSAULT, true);
                }
                break;
        }
    }
}

void BattlegroundTP::StartingEventCloseDoors()
{
    for (uint32 i = BG_TP_OBJECT_DOOR_A_1; i <= BG_TP_OBJECT_DOOR_H_4; ++i)
    {
        DoorClose(i);
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);
    }

    for (uint32 i = BG_TP_OBJECT_A_FLAG; i <= BG_TP_OBJECT_BERSERKBUFF_2; ++i)
        SpawnBGObject(i, RESPAWN_ONE_DAY);
}

void BattlegroundTP::StartingEventOpenDoors()
{
    for (uint32 i = BG_TP_OBJECT_DOOR_A_1; i <= BG_TP_OBJECT_DOOR_H_4; ++i)
    {
        DoorOpen(i);
    }

    for (uint32 i = BG_TP_OBJECT_A_FLAG; i <= BG_TP_OBJECT_BERSERKBUFF_2; ++i)
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);

    // players joining later are not egible
    //StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, TP_EVENT_START_BATTLE);
    _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_BASE;
    _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_BASE;
    UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 0);
    UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 0);
    UpdateFlagState(TEAM_ALLIANCE, 1);
    UpdateFlagState(TEAM_HORDE, 1);
    UpdateWorldState(BG_TP_STATE_TIMER_ACTIVE, 0);
    BroadcastCTFFlagFullState();
}

void BattlegroundTP::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);
    if (!isInBattleground)
    {
        BattlegroundTPScore* scoreEntry = new BattlegroundTPScore(player->GetGUID());
        if (player->GetTeam() == HORDE)
        {
            scoreEntry->BonusHonor = 1;
        }
        PlayerScores[player->GetGUID().GetCounter()] = scoreEntry;
    }
    if (GetStatus() == STATUS_IN_PROGRESS)
        SendCTFFlagFullStateTo(player);
}



void BattlegroundTP::RespawnFlagAfterDrop(TeamId teamId)
{
    if (GetStatus() != STATUS_IN_PROGRESS || GetFlagState(teamId) != BG_TP_FLAG_STATE_ON_GROUND)
        return;

    _flagState[teamId] = BG_TP_FLAG_STATE_ON_BASE;
    UpdateWorldState(teamId == TEAM_ALLIANCE ? BG_TP_FLAG_UNK_ALLIANCE : BG_TP_FLAG_UNK_HORDE, 0);
    UpdateFlagState(GetOtherTwinPeaksTeamId(teamId), 1);
    SpawnBGObject(teamId == TEAM_ALLIANCE ? BG_TP_OBJECT_A_FLAG : BG_TP_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
    SendBroadcastText(teamId == TEAM_ALLIANCE ? LANG_BG_TP_ALLIANCE_FLAG_RESPAWNED : LANG_BG_TP_HORDE_FLAG_RESPAWNED, CHAT_MSG_BG_SYSTEM_NEUTRAL);
    PlaySoundToAll(BG_TP_SOUND_FLAGS_RESPAWNED);

    if (GameObject* flag = GetBgMap()->GetGameObject(GetDroppedFlagGUID(teamId)))
        flag->Delete();

    SetDroppedFlagGUID(ObjectGuid::Empty, teamId);
    _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT10);
    _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT15);
    RemoveAssaultAuras();
    HandleFlagRoomCapturePoint(GetOtherTwinPeaksTeamId(teamId));
    SendCTFFlagAddonMessage(teamId == TEAM_ALLIANCE ? "A:RETURN" : "H:RETURN");
    BroadcastCTFFlagFullState();
}

void BattlegroundTP::EventPlayerCapturedFlag(Player* player)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
    RemoveAssaultAuras();

    AddPoints(player->GetTeamId(), 1);
    TeamId capturedFlagTeam = GetOtherTwinPeaksTeamId(player->GetTeamId());
    SetFlagPicker(ObjectGuid::Empty, capturedFlagTeam);
    _flagState[capturedFlagTeam] = BG_TP_FLAG_STATE_WAIT_RESPAWN;
    _flagState[player->GetTeamId()] = BG_TP_FLAG_STATE_WAIT_RESPAWN;
    UpdateWorldState(capturedFlagTeam == TEAM_ALLIANCE ? BG_TP_FLAG_UNK_ALLIANCE : BG_TP_FLAG_UNK_HORDE, 0);
    UpdateFlagState(player->GetTeamId(), 1);
    if (player->GetTeamId() == TEAM_ALLIANCE)
    {
        player->RemoveAurasDueToSpell(BG_TP_SPELL_HORDE_FLAG);
        PlaySoundToAll(BG_TP_SOUND_FLAG_CAPTURED_ALLIANCE);
        SendBroadcastText(LANG_BG_TP_CAPTURED_HF, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
    }
    else
    {
        player->RemoveAurasDueToSpell(BG_TP_SPELL_ALLIANCE_FLAG);
        PlaySoundToAll(BG_TP_SOUND_FLAG_CAPTURED_HORDE);
        SendBroadcastText(LANG_BG_TP_CAPTURED_AF, CHAT_MSG_BG_SYSTEM_HORDE, player);
    }

    SpawnBGObject(BG_TP_OBJECT_H_FLAG, BG_TP_FLAG_RESPAWN_TIME);
    SpawnBGObject(BG_TP_OBJECT_A_FLAG, BG_TP_FLAG_RESPAWN_TIME);
    SendCTFFlagAddonMessage(capturedFlagTeam == TEAM_ALLIANCE ? "A:CAPTURE" : "H:CAPTURE");
    SendCTFFlagAddonMessage("A:WAIT");
    SendCTFFlagAddonMessage("H:WAIT");
    BroadcastCTFFlagFullState();

    UpdateWorldState(player->GetTeamId() == TEAM_ALLIANCE ? BG_TP_FLAG_CAPTURES_ALLIANCE : BG_TP_FLAG_CAPTURES_HORDE, GetTeamScore(player->GetTeamId()));
    UpdatePlayerScore(player, SCORE_FLAG_CAPTURES, 1);      // +1 flag captures
    _lastFlagCaptureTeam = player->GetTeamId();

    RewardHonorToTeam(GetBonusHonorFromKill(2), player->GetTeamId());

    if (GetTeamScore(TEAM_ALLIANCE) == BG_TP_MAX_TEAM_SCORE || GetTeamScore(TEAM_HORDE) == BG_TP_MAX_TEAM_SCORE)
    {
        UpdateWorldState(BG_TP_STATE_TIMER_ACTIVE, 0);
        EndBattleground(GetTeamScore(TEAM_HORDE) == BG_TP_MAX_TEAM_SCORE ? TEAM_HORDE : TEAM_ALLIANCE);
    }
    else
        _bgEvents.ScheduleEvent(BG_TP_EVENT_RESPAWN_BOTH_FLAGS, Milliseconds(BG_TP_FLAG_RESPAWN_TIME));

    _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT10);
    _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT15);
}

void BattlegroundTP::HandleFlagRoomCapturePoint(TeamId teamId)
{
    Player* flagCarrier = ObjectAccessor::GetPlayer(GetBgMap(), GetFlagPickerGUID(teamId));
    uint32 areaTrigger = teamId == TEAM_ALLIANCE ? 5905 : 5904;
    if (flagCarrier && flagCarrier->IsInAreaTriggerRadius(sAreaTriggerStore.LookupEntry(areaTrigger)))
        EventPlayerCapturedFlag(flagCarrier);
}

void BattlegroundTP::EventPlayerDroppedFlag(Player* player)
{
    if (GetFlagPickerGUID(TEAM_HORDE) != player->GetGUID() && GetFlagPickerGUID(TEAM_ALLIANCE) != player->GetGUID())
        return;

    SetFlagPicker(ObjectGuid::Empty, GetOtherTwinPeaksTeamId(player->GetTeamId()));
    player->RemoveAurasDueToSpell(BG_TP_SPELL_HORDE_FLAG);
    player->RemoveAurasDueToSpell(BG_TP_SPELL_ALLIANCE_FLAG);
    player->RemoveAurasDueToSpell(BG_TP_SPELL_FOCUSED_ASSAULT);
    player->RemoveAurasDueToSpell(BG_TP_SPELL_BRUTAL_ASSAULT);

    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (player->GetTeamId() == TEAM_ALLIANCE)
    {
        _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_GROUND;
        UpdateFlagState(TEAM_ALLIANCE, 1);
        UpdateWorldState(BG_TP_FLAG_UNK_HORDE, uint32(-1));
        player->CastSpell(player, BG_TP_SPELL_HORDE_FLAG_DROPPED, true);
        SendBroadcastText(LANG_BG_TP_DROPPED_HF, CHAT_MSG_BG_SYSTEM_HORDE, player);
        _bgEvents.RescheduleEvent(BG_TP_EVENT_HORDE_DROP_FLAG, Milliseconds(BG_TP_FLAG_DROP_TIME));
        float x = player->GetPositionX();
        float y = player->GetPositionY();
        Map2ZoneCoordinates(x, y, 5031);
        SendCTFFlagAddonMessage("H:DROP:" + FormatCTFCoord(x / 100.0f) + ":" + FormatCTFCoord(y / 100.0f));
        BroadcastCTFFlagFullState();
    }
    else
    {
        _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_GROUND;
        UpdateFlagState(TEAM_HORDE, 1);
        UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, uint32(-1));
        player->CastSpell(player, BG_TP_SPELL_ALLIANCE_FLAG_DROPPED, true);
        SendBroadcastText(LANG_BG_TP_DROPPED_AF, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
        _bgEvents.RescheduleEvent(BG_TP_EVENT_ALLIANCE_DROP_FLAG, Milliseconds(BG_TP_FLAG_DROP_TIME));
        float x = player->GetPositionX();
        float y = player->GetPositionY();
        Map2ZoneCoordinates(x, y, 5031);
        SendCTFFlagAddonMessage("A:DROP:" + FormatCTFCoord(x / 100.0f) + ":" + FormatCTFCoord(y / 100.0f));
        BroadcastCTFFlagFullState();
    }
}

void BattlegroundTP::EventPlayerClickedOnFlag(Player* player, GameObject* gameObject)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);

    // Alliance Flag picked up from base
    if (player->GetTeamId() == TEAM_HORDE && GetFlagState(TEAM_ALLIANCE) == BG_TP_FLAG_STATE_ON_BASE && BgObjects[BG_TP_OBJECT_A_FLAG] == gameObject->GetGUID())
    {
        SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_ONE_DAY);
        SetFlagPicker(player->GetGUID(), TEAM_ALLIANCE);
        _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_PLAYER;
        UpdateFlagState(TEAM_HORDE, BG_TP_FLAG_STATE_ON_PLAYER);
        UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 1);
        player->CastSpell(player, BG_TP_SPELL_ALLIANCE_FLAG, true);
        player->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_TARGET, BG_TP_SPELL_ALLIANCE_FLAG_PICKED);

        PlaySoundToAll(BG_TP_SOUND_ALLIANCE_FLAG_PICKED_UP);
        SendBroadcastText(LANG_BG_TP_PICKEDUP_AF, CHAT_MSG_BG_SYSTEM_HORDE, player);
        float x = player->GetPositionX();
        float y = player->GetPositionY();
        Map2ZoneCoordinates(x, y, 5031);
        SendCTFFlagAddonMessage(std::string("A:PICKUP:") + player->GetName() + ":" + FormatCTFCoord(x / 100.0f) + ":" + FormatCTFCoord(y / 100.0f));
        BroadcastCTFFlagFullState();

        if (GetFlagState(TEAM_HORDE) != BG_TP_FLAG_STATE_ON_BASE)
        {
            _bgEvents.RescheduleEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT10, Milliseconds(BG_TP_SPELL_FORCE_TIME));
            _bgEvents.RescheduleEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT15, Milliseconds(BG_TP_SPELL_BRUTAL_TIME));
        }
        return;
    }

    // Horde Flag picked up from base
    if (player->GetTeamId() == TEAM_ALLIANCE && GetFlagState(TEAM_HORDE) == BG_TP_FLAG_STATE_ON_BASE && BgObjects[BG_TP_OBJECT_H_FLAG] == gameObject->GetGUID())
    {
        SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_ONE_DAY);
        SetFlagPicker(player->GetGUID(), TEAM_HORDE);
        _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_PLAYER;
        UpdateFlagState(TEAM_ALLIANCE, BG_TP_FLAG_STATE_ON_PLAYER);
        UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 1);
        player->CastSpell(player, BG_TP_SPELL_HORDE_FLAG, true);
        player->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_TARGET, BG_TP_SPELL_HORDE_FLAG_PICKED);

        PlaySoundToAll(BG_TP_SOUND_HORDE_FLAG_PICKED_UP);
        SendBroadcastText(LANG_BG_TP_PICKEDUP_HF, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
        float x = player->GetPositionX();
        float y = player->GetPositionY();
        Map2ZoneCoordinates(x, y, 5031);
        SendCTFFlagAddonMessage(std::string("H:PICKUP:") + player->GetName() + ":" + FormatCTFCoord(x / 100.0f) + ":" + FormatCTFCoord(y / 100.0f));
        BroadcastCTFFlagFullState();

        if (GetFlagState(TEAM_ALLIANCE) != BG_TP_FLAG_STATE_ON_BASE)
        {
            _bgEvents.RescheduleEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT10, Milliseconds(BG_TP_SPELL_FORCE_TIME));
            _bgEvents.RescheduleEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT15, Milliseconds(BG_TP_SPELL_BRUTAL_TIME));
        }
        return;
    }
    if (player->IsMounted())
    {
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }
    // Alliance Flag on ground
    if (GetFlagState(TEAM_ALLIANCE) == BG_TP_FLAG_STATE_ON_GROUND && player->IsWithinDistInMap(gameObject, 10.0f) && gameObject->GetEntry() == BG_OBJECT_A_FLAG_GROUND_TP_ENTRY)
    {
        SetDroppedFlagGUID(ObjectGuid::Empty, TEAM_ALLIANCE);
        if (player->GetTeamId() == TEAM_ALLIANCE)
        {
            _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_BASE;
            UpdateFlagState(TEAM_HORDE, 1);
            UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 0);
            SpawnBGObject(BG_TP_OBJECT_A_FLAG, RESPAWN_IMMEDIATELY);
            UpdatePlayerScore(player, SCORE_FLAG_RETURNS, 1);

            PlaySoundToAll(BG_TP_SOUND_FLAG_RETURNED);
            SendBroadcastText(LANG_BG_TP_RETURNED_AF, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT10);
            _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT15);
            RemoveAssaultAuras();
            HandleFlagRoomCapturePoint(TEAM_HORDE);
            SendCTFFlagAddonMessage("A:RETURN");
            BroadcastCTFFlagFullState();
            return;
        }
        else
        {
            SetFlagPicker(player->GetGUID(), TEAM_ALLIANCE);
            _flagState[TEAM_ALLIANCE] = BG_TP_FLAG_STATE_ON_PLAYER;
            UpdateFlagState(TEAM_HORDE, BG_TP_FLAG_STATE_ON_PLAYER);
            UpdateWorldState(BG_TP_FLAG_UNK_ALLIANCE, 1);
            player->CastSpell(player, BG_TP_SPELL_ALLIANCE_FLAG, true);
            if (uint32 assaultSpellId = GetAssaultSpellId())
              player->CastSpell(player, assaultSpellId, true);

            PlaySoundToAll(BG_TP_SOUND_ALLIANCE_FLAG_PICKED_UP);
            SendBroadcastText(LANG_BG_TP_PICKEDUP_AF, CHAT_MSG_BG_SYSTEM_HORDE, player);
            float x = player->GetPositionX();
            float y = player->GetPositionY();
            Map2ZoneCoordinates(x, y, 5031);
            SendCTFFlagAddonMessage(std::string("A:PICKUP:") + player->GetName() + ":" + FormatCTFCoord(x / 100.0f) + ":" + FormatCTFCoord(y / 100.0f));
            BroadcastCTFFlagFullState();
            return;
        }
    }

    // Horde Flag on ground
    if (GetFlagState(TEAM_HORDE) == BG_TP_FLAG_STATE_ON_GROUND && player->IsWithinDistInMap(gameObject, 10.0f) && gameObject->GetEntry() == BG_OBJECT_H_FLAG_GROUND_TP_ENTRY)
    {
        SetDroppedFlagGUID(ObjectGuid::Empty, TEAM_HORDE);
        if (player->GetTeamId() == TEAM_HORDE)
        {
            _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_BASE;
            UpdateFlagState(TEAM_ALLIANCE, 1);
            UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 0);
            SpawnBGObject(BG_TP_OBJECT_H_FLAG, RESPAWN_IMMEDIATELY);
            UpdatePlayerScore(player, SCORE_FLAG_RETURNS, 1);

            PlaySoundToAll(BG_TP_SOUND_FLAG_RETURNED);
            SendBroadcastText(LANG_BG_TP_RETURNED_HF, CHAT_MSG_BG_SYSTEM_HORDE, player);
            _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT10);
            _bgEvents.CancelEvent(BG_TP_EVENT_BOTH_FLAGS_KEPT15);
            RemoveAssaultAuras();
            HandleFlagRoomCapturePoint(TEAM_ALLIANCE);
            SendCTFFlagAddonMessage("H:RETURN");
            BroadcastCTFFlagFullState();
            return;
        }
        else
        {
            SetFlagPicker(player->GetGUID(), TEAM_HORDE);
            _flagState[TEAM_HORDE] = BG_TP_FLAG_STATE_ON_PLAYER;
            UpdateFlagState(TEAM_ALLIANCE, BG_TP_FLAG_STATE_ON_PLAYER);
            UpdateWorldState(BG_TP_FLAG_UNK_HORDE, 1);
            player->CastSpell(player, BG_TP_SPELL_HORDE_FLAG, true);
            if (uint32 assaultSpellId = GetAssaultSpellId())
              player->CastSpell(player, assaultSpellId, true);

            PlaySoundToAll(BG_TP_SOUND_HORDE_FLAG_PICKED_UP);
            SendBroadcastText(LANG_BG_TP_PICKEDUP_HF, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
            float x = player->GetPositionX();
            float y = player->GetPositionY();
            Map2ZoneCoordinates(x, y, 5031);
            SendCTFFlagAddonMessage(std::string("H:PICKUP:") + player->GetName() + ":" + FormatCTFCoord(x / 100.0f) + ":" + FormatCTFCoord(y / 100.0f));
            BroadcastCTFFlagFullState();
            return;
        }
    }
}

void BattlegroundTP::RemovePlayer(Player* player, ObjectGuid /*guid*/, uint32 /*team*/)
{
    if (player && (GetFlagPickerGUID(TEAM_ALLIANCE) == player->GetGUID() || GetFlagPickerGUID(TEAM_HORDE) == player->GetGUID()))
        EventPlayerDroppedFlag(player);
}

void BattlegroundTP::UpdateFlagState(TeamId teamId, uint32 value)
{
    if (teamId == TEAM_ALLIANCE)
        UpdateWorldState(BG_TP_FLAG_STATE_ALLIANCE, value);
    else
        UpdateWorldState(BG_TP_FLAG_STATE_HORDE, value);
}

void BattlegroundTP::HandleAreaTrigger(Player* player, uint32 trigger)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    switch(trigger)
    {
        case 5904:                                          // Alliance Flag spawn
            if (GetFlagState(TEAM_ALLIANCE) == BG_TP_FLAG_STATE_ON_BASE && GetFlagPickerGUID(TEAM_HORDE) == player->GetGUID())
                    EventPlayerCapturedFlag(player);
            break;
        case 5905:                                          // Horde Flag spawn
            if (GetFlagState(TEAM_HORDE) == BG_TP_FLAG_STATE_ON_BASE && GetFlagPickerGUID(TEAM_ALLIANCE) == player->GetGUID())
                EventPlayerCapturedFlag(player);
            break;
        case 5908:                                          // Horde Tower
        case 5909:                                          // Twin Peak House big
        case 5910:                                          // Horde House
        case 5911:                                          // Twin Peak House small
        case 5914:                                          // Alliance Start right
        case 5916:                                          // Alliance Start
        case 5917:                                          // Alliance Start left
        case 5918:                                          // Horde Start
        case 5920:                                          // Horde Start Front entrance
        case 5921:                                          // Horde Start left Water channel
            break;
        // default:
        //     Battleground::HandleAreaTrigger(player, trigger);
        //     break;
    }
}

bool BattlegroundTP::SetupBattleground()
{
        // flags
    AddObject(BG_TP_OBJECT_A_FLAG, BG_OBJECT_A_FLAG_TP_ENTRY, 2118.210f, 191.621f, 44.052f, 5.741259f, 0, 0, 0.9996573f, 0.02617699f, BG_TP_FLAG_RESPAWN_TIME/1000);
    AddObject(BG_TP_OBJECT_H_FLAG, BG_OBJECT_H_FLAG_TP_ENTRY, 1578.380f, 344.037f, 2.419f, 3.055978f, 0, 0, 0.008726535f, 0.9999619f, BG_TP_FLAG_RESPAWN_TIME/1000);
        // buffs
    AddObject(BG_TP_OBJECT_SPEEDBUFF_1, BG_OBJECTID_SPEEDBUFF_ENTRY, 1545.402f, 304.028f, 0.5923f, -1.64061f, 0, 0, 0.7313537f, -0.6819983f, BG_TP_BUFF_RESPAWN_TIME);
    AddObject(BG_TP_OBJECT_SPEEDBUFF_2, BG_OBJECTID_SPEEDBUFF_ENTRY, 2171.279f, 222.334f, 43.8001f, 2.663309f, 0, 0, 0.7313537f, 0.6819984f, BG_TP_BUFF_RESPAWN_TIME);
    AddObject(BG_TP_OBJECT_REGENBUFF_1, BG_OBJECTID_REGENBUFF_ENTRY, 1753.957f, 242.092f, -14.1170f, 1.105848f, 0, 0, 0.1305263f, -0.9914448f, BG_TP_BUFF_RESPAWN_TIME);
    AddObject(BG_TP_OBJECT_REGENBUFF_2, BG_OBJECTID_REGENBUFF_ENTRY, 1952.121f, 383.857f, -10.2870f, 4.192612f, 0, 0, 0.333807f, -0.9426414f, BG_TP_BUFF_RESPAWN_TIME);
    AddObject(BG_TP_OBJECT_BERSERKBUFF_1, BG_OBJECTID_BERSERKERBUFF_ENTRY, 1934.369f, 226.064f, -17.0441f, 2.499154f, 0, 0, 0.5591929f, 0.8290376f, BG_TP_BUFF_RESPAWN_TIME);
    AddObject(BG_TP_OBJECT_BERSERKBUFF_2, BG_OBJECTID_BERSERKERBUFF_ENTRY, 1725.240f, 446.431f, -7.8327f, 5.709677f, 0, 0, 0.9396926f, -0.3420201f, BG_TP_BUFF_RESPAWN_TIME);
        // alliance gates
    AddObject(BG_TP_OBJECT_DOOR_A_1, BG_OBJECT_DOOR_A_1_TP_ENTRY, 2115.399f, 150.175f, 43.526f, 2.544690f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);
    AddObject(BG_TP_OBJECT_DOOR_A_2, BG_OBJECT_DOOR_A_2_TP_ENTRY, 2156.803f, 220.331f, 43.482f, 2.544690f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);
    AddObject(BG_TP_OBJECT_DOOR_A_3, BG_OBJECT_DOOR_A_3_TP_ENTRY, 2127.512f, 223.711f, 43.640f, 2.544690f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);
    AddObject(BG_TP_OBJECT_DOOR_A_4, BG_OBJECT_DOOR_A_4_TP_ENTRY, 2096.102f, 166.920f, 54.230f, 2.544690f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);
        // horde gates
    AddObject(BG_TP_OBJECT_DOOR_H_1, BG_OBJECT_DOOR_H_1_TP_ENTRY, 1556.595f, 314.502f, 1.2230f, 6.179126f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);
    AddObject(BG_TP_OBJECT_DOOR_H_2, BG_OBJECT_DOOR_H_2_TP_ENTRY, 1587.093f, 319.853f, 1.5233f, 6.179126f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);
    AddObject(BG_TP_OBJECT_DOOR_H_3, BG_OBJECT_DOOR_H_3_TP_ENTRY, 1591.463f, 365.732f, 13.494f, 6.179126f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);
    AddObject(BG_TP_OBJECT_DOOR_H_4, BG_OBJECT_DOOR_H_4_TP_ENTRY, 1558.315f, 372.709f, 1.4840f, 6.179126f, 0, 0, 0, 0, RESPAWN_IMMEDIATELY);

    WorldSafeLocsEntry const* sg = sWorldSafeLocsStore.LookupEntry(TP_GRAVEYARD_MIDDLE_ALLIANCE);
    AddSpiritGuide(TP_SPIRIT_ALLIANCE, sg->Loc.X, sg->Loc.Y, sg->Loc.Z, 3.641396f, TEAM_ALLIANCE);

    sg = sWorldSafeLocsStore.LookupEntry(TP_GRAVEYARD_START_ALLIANCE);
    AddSpiritGuide(TP_SPIRIT_ALLIANCE, sg->Loc.X, sg->Loc.Y, sg->Loc.Z, 3.641396f, TEAM_ALLIANCE);

    sg = sWorldSafeLocsStore.LookupEntry(TP_GRAVEYARD_MIDDLE_HORDE);
    AddSpiritGuide(TP_SPIRIT_HORDE, sg->Loc.X, sg->Loc.Y, sg->Loc.Z, 3.641396f, TEAM_HORDE);

    sg = sWorldSafeLocsStore.LookupEntry(TP_GRAVEYARD_START_HORDE);
    AddSpiritGuide(TP_SPIRIT_ALLIANCE, sg->Loc.X, sg->Loc.Y, sg->Loc.Z, 3.641396f, TEAM_HORDE);

    for (uint32 i = BG_TP_OBJECT_DOOR_A_1; i < BG_TP_OBJECT_MAX; ++i)
        if (!BgObjects[i])
        {
            TC_LOG_ERROR("sql.sql", "BatteGroundTP: Failed to spawn some object Battleground not created!");
            return false;
        }

    for (uint32 i = TP_SPIRIT_ALLIANCE; i < BG_CREATURES_MAX_TP; ++i)
        if (!BgCreatures[i])
        {
            TC_LOG_ERROR("sql.sql", "BatteGroundTP: Failed to spawn spirit guides Battleground not created!");
            return false;
        }

    return true;
}

void BattlegroundTP::Init()
{
    //call parent's class reset

    _bgEvents.Reset();
    _flagKeepers[TEAM_ALLIANCE].Clear();
    _flagKeepers[TEAM_HORDE].Clear();
    _droppedFlagGUID[TEAM_ALLIANCE].Clear();
    _droppedFlagGUID[TEAM_HORDE].Clear();
    _flagState[TEAM_ALLIANCE]       = BG_TP_FLAG_STATE_ON_BASE;
    _flagState[TEAM_HORDE]          = BG_TP_FLAG_STATE_ON_BASE;
    _lastFlagCaptureTeam            = TEAM_NEUTRAL;

    if (sBattlegroundMgr->IsBGWeekend(GetTypeID()))
    {
        _reputationCapture = 45;
        _honorWinKills = 3;
        _honorEndKills = 4;
    }
    else
    {
        _reputationCapture = 35;
        _honorWinKills = 1;
        _honorEndKills = 2;
    }
}

void BattlegroundTP::EndBattleground(uint32 winnerTeamId)
{
    // Win reward
    RewardHonorToTeam(GetBonusHonorFromKill(_honorWinKills), winnerTeamId);

    // Complete map_end rewards (even if no team wins)
    RewardHonorToTeam(GetBonusHonorFromKill(_honorEndKills), TEAM_ALLIANCE);
    RewardHonorToTeam(GetBonusHonorFromKill(_honorEndKills), TEAM_HORDE);

    Battleground::EndBattleground(winnerTeamId);
}

void BattlegroundTP::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    EventPlayerDroppedFlag(player);
    Battleground::HandleKillPlayer(player, killer);
}

bool BattlegroundTP::UpdatePlayerScore(Player* player, uint32 type, uint32 value, bool doAddHonor)
{
    if (!Battleground::UpdatePlayerScore(player, type, value, doAddHonor))
        return false;

    switch(type)
    {
        case SCORE_FLAG_CAPTURES:                           // flags captured
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, TP_OBJECTIVE_CAPTURE_FLAG);
            break;
        case SCORE_FLAG_RETURNS:                            // flags returned
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BG_OBJECTIVE_CAPTURE, TP_OBJECTIVE_RETURN_FLAG);
            break;
        default:
            break;
    }

    return true;
}

WorldSafeLocsEntry const* BattlegroundTP::GetClosestGraveyard(Player* player)
{
    if (GetStatus() == STATUS_IN_PROGRESS)
        return sWorldSafeLocsStore.LookupEntry(player->GetTeamId() == TEAM_ALLIANCE ? TP_GRAVEYARD_MIDDLE_ALLIANCE : TP_GRAVEYARD_MIDDLE_HORDE);

    return sWorldSafeLocsStore.LookupEntry(player->GetTeamId() == TEAM_ALLIANCE ? TP_GRAVEYARD_FLAGROOM_ALLIANCE : TP_GRAVEYARD_FLAGROOM_HORDE);
}

void BattlegroundTP::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
  packet.Worldstates.emplace_back(BG_TP_FLAG_CAPTURES_ALLIANCE, GetTeamScore(TEAM_ALLIANCE));
  packet.Worldstates.emplace_back(BG_TP_FLAG_CAPTURES_HORDE, GetTeamScore(TEAM_HORDE));
  packet.Worldstates.emplace_back(BG_TP_FLAG_CAPTURES_MAX, BG_TP_MAX_TEAM_SCORE);

  packet.Worldstates.emplace_back(BG_TP_STATE_TIMER_ACTIVE, 0);

  if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_GROUND)
      packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_ALLIANCE, uint32(-1));
  else if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_PLAYER)
      packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_ALLIANCE, 1);
  else
      packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_ALLIANCE, 0);

  if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_GROUND)
      packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_HORDE, uint32(-1));
  else if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_PLAYER)
      packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_HORDE, 1);
  else
      packet.Worldstates.emplace_back(BG_TP_FLAG_UNK_HORDE, 0);

  if (_flagState[TEAM_HORDE] == BG_TP_FLAG_STATE_ON_PLAYER)
      packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_HORDE, 2);
  else
      packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_HORDE, 1);

  if (_flagState[TEAM_ALLIANCE] == BG_TP_FLAG_STATE_ON_PLAYER)
      packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_ALLIANCE, 2);
  else
      packet.Worldstates.emplace_back(BG_TP_FLAG_STATE_ALLIANCE, 1);
}

uint32 BattlegroundTP::GetPrematureWinner()
{
    if (GetTeamScore(TEAM_ALLIANCE) > GetTeamScore(TEAM_HORDE))
        return TEAM_ALLIANCE;
    else if (GetTeamScore(TEAM_HORDE) > GetTeamScore(TEAM_ALLIANCE))
        return TEAM_HORDE;

    return Battleground::GetPrematureWinner();
}

uint32 BattlegroundTP::GetAssaultSpellId() const
{
    if ((!GetFlagPickerGUID(TEAM_ALLIANCE) && GetFlagState(TEAM_ALLIANCE) != BG_TP_FLAG_STATE_ON_GROUND) ||
        (!GetFlagPickerGUID(TEAM_HORDE) && GetFlagState(TEAM_HORDE) != BG_TP_FLAG_STATE_ON_GROUND) ||
        _bgEvents.HasEventScheduled(BG_TP_EVENT_BOTH_FLAGS_KEPT10))
    return 0;

    return _bgEvents.HasEventScheduled(BG_TP_EVENT_BOTH_FLAGS_KEPT15) ? BG_TP_SPELL_FOCUSED_ASSAULT : BG_TP_SPELL_BRUTAL_ASSAULT;
}

void BattlegroundTP::RemoveAssaultAuras()
{
    if (Player* player = ObjectAccessor::GetPlayer(FindBgMap(), GetFlagPickerGUID(TEAM_ALLIANCE)))
    {
        player->RemoveAurasDueToSpell(BG_TP_SPELL_FOCUSED_ASSAULT);
        player->RemoveAurasDueToSpell(BG_TP_SPELL_BRUTAL_ASSAULT);
    }
    if (Player* player = ObjectAccessor::GetPlayer(FindBgMap(), GetFlagPickerGUID(TEAM_HORDE)))
    {
        player->RemoveAurasDueToSpell(BG_TP_SPELL_FOCUSED_ASSAULT);
        player->RemoveAurasDueToSpell(BG_TP_SPELL_BRUTAL_ASSAULT);
    }
}

bool BattlegroundTP::CheckAchievementCriteriaMeet(uint32 criteriaId, Player const* player, Unit const* target, uint32 miscValue)
{
    switch (criteriaId)
    {
        case BG_CRITERIA_CHECK_SAVE_THE_DAY:
            return GetFlagState(player->GetTeamId()) == BG_TP_FLAG_STATE_ON_BASE;
    }

    return Battleground::CheckAchievementCriteriaMeet(criteriaId, player, target, miscValue);
}
