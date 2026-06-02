//
// Created by romain-p on 17/10/2021.
//
#include "Player.h"
#include "Opcodes.h"
#include "Chat.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <unordered_map>
#include "DatabaseEnv.h"
#include <ObjectAccessor.h>
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "GameEventMgr.h"
#include "Player.h"
#include "Timer.h"
#include "WorldSession.h"
#include <DBCStores.h>
#include <algorithm>
#include <deque>
#include <sstream>
#include <vector>


std::vector<Opcodes> watchList = {
    SMSG_BATTLEGROUND_PLAYER_JOINED,
    SMSG_BATTLEGROUND_PLAYER_LEFT,
        SMSG_NOTIFICATION,
        SMSG_AURA_UPDATE,
        SMSG_WORLD_STATE_UI_TIMER_UPDATE,
        SMSG_COMPRESSED_UPDATE_OBJECT,
        SMSG_AURA_UPDATE_ALL,
        SMSG_NAME_QUERY_RESPONSE,
        SMSG_DESTROY_OBJECT,
        MSG_MOVE_START_FORWARD,
        MSG_MOVE_SET_FACING,
        MSG_MOVE_HEARTBEAT,
        MSG_MOVE_JUMP,
        SMSG_MONSTER_MOVE,
        MSG_MOVE_FALL_LAND,
        SMSG_PERIODICAURALOG,
        SMSG_ARENA_UNIT_DESTROYED,
        MSG_MOVE_START_STRAFE_RIGHT,
        MSG_MOVE_STOP_STRAFE,
        MSG_MOVE_START_STRAFE_LEFT,
        MSG_MOVE_STOP,
        MSG_MOVE_START_BACKWARD,
        MSG_MOVE_START_TURN_LEFT,
        MSG_MOVE_STOP_TURN,
        MSG_MOVE_START_TURN_RIGHT,
        SMSG_SPELL_START,
        SMSG_SPELL_GO,
        SMSG_FORCE_RUN_SPEED_CHANGE,
        SMSG_ATTACK_START,
        SMSG_POWER_UPDATE,
        SMSG_ATTACKERSTATEUPDATE,
        SMSG_SPELLDAMAGESHIELD,
        SMSG_SPELLHEALLOG,
        SMSG_SPELLENERGIZELOG,
        SMSG_SPELLNONMELEEDAMAGELOG,
        SMSG_ATTACK_STOP,
        SMSG_SPELLLOGEXECUTE,
        SMSG_EMOTE,
        SMSG_SPELL_DELAYED,
        SMSG_AI_REACTION,
        SMSG_PET_NAME_QUERY_RESPONSE,
        SMSG_CANCEL_AUTO_REPEAT,
        SMSG_UPDATE_OBJECT,
        SMSG_FORCE_FLIGHT_SPEED_CHANGE,
        SMSG_GAMEOBJECT_QUERY_RESPONSE,
        SMSG_FORCE_SWIM_SPEED_CHANGE,
        SMSG_GAMEOBJECT_DESPAWN_ANIM,
        SMSG_CANCEL_COMBAT
};


struct PacketRecord { uint32 timestamp; WorldPacket packet; };
struct MatchRecord
{
    BattlegroundTypeId typeId;
    uint8 arenaTypeId;
    uint32 mapId;
    std::deque<PacketRecord> packets;
    uint32 captureStartMs = 0;
    uint32 playbackStartMs = 0;
    uint32 lastDebugMs = 0;
};
std::unordered_map<uint32, MatchRecord> records;
std::unordered_map<uint32, MatchRecord> loadedReplays;

namespace
{
    constexpr uint32 ReplayCountdownMs = 5000;

    uint32 GetPlayerLowGuid(Player const* player)
    {
        return player->GetGUID().GetCounter();
    }

    bool IsWatchedReplayPacket(uint16 opcode)
    {
        return std::find(watchList.begin(), watchList.end(), opcode) != watchList.end();
    }


    char const* ReplayStatusName(BattlegroundStatus status)
    {
        switch (status)
        {
            case STATUS_WAIT_QUEUE:
                return "WAIT_QUEUE";
            case STATUS_WAIT_JOIN:
                return "WAIT_JOIN";
            case STATUS_IN_PROGRESS:
                return "IN_PROGRESS";
            case STATUS_WAIT_LEAVE:
                return "WAIT_LEAVE";
            default:
                return "UNKNOWN";
        }
    }

    void WhisperReplayDebug(Player* player, std::string_view message)
    {
        if (!player)
            return;

        ObjectGuid replayGuid = ObjectGuid::Create<HighGuid::Player>(1);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_FOREIGN, LANG_UNIVERSAL, replayGuid, player->GetGUID(), message, 0, "ReplayDebug");
        player->SendDirectMessage(&data);
    }

    bool ShouldSendReplayDebug(MatchRecord& match, uint32 intervalMs = 5000)
    {
        uint32 const now = getMSTime();
        if (match.lastDebugMs && getMSTimeDiff(match.lastDebugMs, now) < intervalMs)
            return false;

        match.lastDebugMs = now;
        return true;
    }

    void WhisperReplayState(Player* player, MatchRecord& match, Battleground const* bg, char const* reason)
    {
        if (!ShouldSendReplayDebug(match))
            return;

        std::ostringstream ss;
        ss << "[" << reason << "]";
        ss << " status=" << (bg ? ReplayStatusName(bg->GetStatus()) : "NO_BG");
        if (bg)
        {
            ss << " startDelay=" << bg->GetStartDelayTime();
            ss << " startTime=" << bg->GetStartTime();
            ss << " players=" << bg->GetPlayersSize();
            ss << " replayId=" << bg->GetReplayId();
        }

        ss << " packets=" << match.packets.size();
        if (!match.packets.empty())
            ss << " nextTs=" << match.packets.front().timestamp;
        ss << " playbackStart=" << match.playbackStartMs;

        if (player)
        {
            ss << " pInWorld=" << player->IsInWorld();
            ss << " pTeleport=" << player->IsBeingTeleported();
            ss << " pBgId=" << player->GetBattlegroundId();
            ss << " pMap=" << player->GetMapId();
            ss << " pHasMap=" << (player->FindMap() ? 1 : 0);
            ss << " pBattleArena=" << (player->FindMap() && player->FindMap()->IsBattleArena() ? 1 : 0);
        }

        WhisperReplayDebug(player, ss.str());
    }

    bool IsSpectatorReadyForReplay(Player const* player, Battleground const* bg)
    {
        return player
            && bg
            && player->IsInWorld()
            && !player->IsBeingTeleported()
            && player->GetBattlegroundId() == bg->GetInstanceID()
            && player->GetMapId() == bg->GetMapId()
            && player->FindMap()
            && player->FindMap()->IsBattleArena();
    }

    void EndReplayForSpectator(uint32 spectatorLowGuid, Battleground* bg)
    {
        if (!bg)
            return;

        bg->EndNow();
        bg->toggleReplay(0);

        if (Player* player = ObjectAccessor::FindPlayerByLowGUID(spectatorLowGuid))
            player->LeaveBattleground(bg);
    }

    bool PumpReplayPackets(uint32 spectatorLowGuid, Battleground* bg)
    {
        auto it = loadedReplays.find(spectatorLowGuid);
        if (it == loadedReplays.end())
            return false;

        MatchRecord& match = it->second;
        if (match.packets.empty() || !bg)
        {
            if (Player* player = ObjectAccessor::FindPlayerByLowGUID(spectatorLowGuid))
                WhisperReplayState(player, match, bg, match.packets.empty() ? "ending-empty-packets" : "ending-no-bg");
            EndReplayForSpectator(spectatorLowGuid, bg);
            return true;
        }

        Player* player = ObjectAccessor::FindPlayerByLowGUID(spectatorLowGuid);
        if (!player)
        {
            EndReplayForSpectator(spectatorLowGuid, bg);
            return true;
        }

        if (!IsSpectatorReadyForReplay(player, bg))
        {
            WhisperReplayState(player, match, bg, "waiting-spectator-ready");
            return false;
        }

        uint32 elapsed = 0;
        bool const replayGatesOpen = bg->GetStatus() == BattlegroundStatus::STATUS_IN_PROGRESS || bg->GetStartDelayTime() <= 0;
        if (replayGatesOpen)
        {
            if (!match.playbackStartMs)
                match.playbackStartMs = getMSTime();

            elapsed = getMSTimeDiff(match.playbackStartMs, getMSTime());
        }
        else if (bg->GetStatus() != BattlegroundStatus::STATUS_WAIT_JOIN)
        {
            WhisperReplayState(player, match, bg, "waiting-status");
            return false;
        }

        WhisperReplayState(player, match, bg, replayGatesOpen ? "pumping" : "waiting-gates");

        uint32 sentPackets = 0;
        while (!match.packets.empty() && match.packets.front().timestamp <= elapsed)
        {
            player->GetSession()->SendPacket(&match.packets.front().packet);
            match.packets.pop_front();
            ++sentPackets;
        }

        if (sentPackets)
        {
            std::ostringstream ss;
            ss << "[sent] count=" << sentPackets << " remaining=" << match.packets.size();
            if (!match.packets.empty())
                ss << " nextTs=" << match.packets.front().timestamp << " elapsed=" << elapsed;
            WhisperReplayDebug(player, ss.str());
        }

        if (match.packets.empty())
        {
            WhisperReplayDebug(player, "[finished] no replay packets remain; ending replay");
            EndReplayForSpectator(spectatorLowGuid, bg);
            return true;
        }

        return false;
    }
}

class BGReplayServerScript : public ServerScript {
public:
    BGReplayServerScript() : ServerScript("BGReplayServerScript") {}

    void OnPacketSend(WorldSession* session, WorldPacket& packet) override {
        if (session == nullptr || session->GetPlayer() == nullptr) return;

        Battleground* bg = session->GetPlayer()->GetBattleground();

        //ignore packet when no bg or casual games
        if (bg == nullptr || bg->IsReplay()) return;
        if (!bg->isArena())
            return;
        // Record setup packets during the join countdown as well as live arena packets.
        // Initial object-create/update packets are sent before STATUS_IN_PROGRESS;
        // without them replay viewers enter an empty arena when the gates open.
        if (bg->GetStatus() != BattlegroundStatus::STATUS_WAIT_JOIN && bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS) return;
        //record packets from 1 player of each team
        //iterate just in case a player leaves and used as reference
        for (auto it : bg->GetPlayers()) {
            if (it.second.Team == session->GetPlayer()->GetBGTeam()) {
                if (it.first.GetCounter() != GetPlayerLowGuid(session->GetPlayer()))
                    return; else break;
            }
        }
        //ignore packets not in watch list
        if (!IsWatchedReplayPacket(packet.GetOpcode()))
            return;

        if (records.find(bg->GetInstanceID()) == records.end())
            records[bg->GetInstanceID()].packets.clear();
        MatchRecord& record = records[bg->GetInstanceID()];

        uint32 timestamp = 0;
        if (bg->GetStatus() == BattlegroundStatus::STATUS_IN_PROGRESS)
        {
            if (!record.captureStartMs)
                record.captureStartMs = getMSTime();

            timestamp = getMSTimeDiff(record.captureStartMs, getMSTime());
        }

        record.typeId = bg->GetTypeID(false);
        if (record.typeId == BATTLEGROUND_AA)
        {
            record.typeId = bg->GetTypeID(true);
        }
        record.arenaTypeId = bg->GetArenaType();
        record.mapId = bg->GetMapId();

        //push back packet inside queue of matchId 0
        record.packets.push_back({ timestamp, /* copy */ WorldPacket(packet) });
    }
};


class BGReplayWorldScript : public WorldScript
{
public:
    BGReplayWorldScript() : WorldScript("BGReplayWorldScript") { }

    void OnUpdate(uint32 /*diff*/) override
    {
        for (auto it = loadedReplays.begin(); it != loadedReplays.end();)
        {
            uint32 const spectatorLowGuid = it->first;
            Player* player = ObjectAccessor::FindPlayerByLowGUID(spectatorLowGuid);
            if (!player)
            {
                it = loadedReplays.erase(it);
                continue;
            }

            Battleground* bg = player->GetBattleground();
            if (!bg)
            {
                ++it;
                continue;
            }

            if (!bg->IsReplay() || bg->GetReplayId() != spectatorLowGuid)
            {
                it = loadedReplays.erase(it);
                continue;
            }

            if (PumpReplayPackets(spectatorLowGuid, bg))
                it = loadedReplays.erase(it);
            else
                ++it;
        }
    }
};

class BGReplayBGScript : public BattlegroundScript {
public:
    BGReplayBGScript() : BattlegroundScript("BGReplayBGScript") {}

    void OnBattlegroundEnd(Battleground* bg, uint32 winner) override
    {
        // Only record arena matches
        if (!bg->isArena())
            return;

        //save replay when a bg ends
        if (!bg->IsReplay()) {
            saveReplay(bg);
            return;
        }
    }

    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override {

        if (!bg->isArena())
            return;

        if (!bg->IsReplay()) return;
        int32 startDelayTime = bg->GetStartDelayTime();
        if (startDelayTime > int32(ReplayCountdownMs))
        {
            bg->SetStartDelayTime(ReplayCountdownMs);
            bg->SetStartTime(bg->GetStartTime() + (startDelayTime - ReplayCountdownMs));
        }

        uint32 const spectatorLowGuid = bg->GetReplayId();
        if (PumpReplayPackets(spectatorLowGuid, bg))
            loadedReplays.erase(spectatorLowGuid);
    }

    void saveReplay(Battleground* bg) {
        //retrieve replay data
        auto it = records.find(bg->GetInstanceID());
        if (it == records.end()) return;
        MatchRecord& match = it->second;
        if (match.packets.empty())
        {
            records.erase(it);
            return;
        }

        //serialize arena replay data
        ByteBuffer buffer;
        uint32 headerSize;
        uint32 timestamp;
        for (auto it : match.packets) {
            headerSize = it.packet.size(); //header 4Bytes packet size
            timestamp = it.timestamp;

            buffer << headerSize; //4 bytes
            buffer << timestamp; //4 bytes
            buffer << it.packet.GetOpcode(); // 2 bytes
            if (headerSize > 0)
                buffer.append(it.packet.contents(), it.packet.size()); // headerSize bytes
        }
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ARENA_REPLAYS);
        //stmt->setUInt32(0, id);
        stmt->setUInt32(0, uint32(match.arenaTypeId));
        stmt->setUInt32(1, uint32(match.typeId));
        stmt->setUInt32(2, buffer.size());
        stmt->setBinary(3, buffer.contentsAsVector());
        stmt->setUInt32(4, bg->GetMapId());
        CharacterDatabase.Execute(stmt);
        records.erase(it);
    }
};


class ReplayGossip : public CreatureScript
{
public:
    ReplayGossip() : CreatureScript("ReplayGossip") {}
    struct replayAI : public ScriptedAI
    {
        replayAI(Creature* creature) : ScriptedAI(creature) {}
        bool OnGossipHello(Player* player) override
        {
            auto matchIds = loadLast10Replays();
            for (uint32 matchId : matchIds)
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay match " + std::to_string(matchId), GOSSIP_SENDER_MAIN, matchId);
            SendGossipMenuFor(player, 1775757, me->GetGUID());
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override {
            uint32 replayId = GetGossipActionFor(player, gossipListId);
            player->PlayerTalkClass->ClearMenus();
            StartReplay(player, replayId);
            CloseGossipMenuFor(player);
            return true;
        }

        bool StartReplay(Player* player, uint32 replayId) {
            auto handler = ChatHandler(player->GetSession());

            WhisperReplayDebug(player, "[start] loading replay " + std::to_string(replayId));
            if (!loadReplayDataForPlayer(player, replayId))
                return false;

            MatchRecord record = loadedReplays[GetPlayerLowGuid(player)];
            {
                std::ostringstream ss;
                ss << "[start] loaded packets=" << record.packets.size() << " type=" << uint32(record.typeId) << " arenaType=" << uint32(record.arenaTypeId) << " map=" << record.mapId;
                if (!record.packets.empty())
                    ss << " firstTs=" << record.packets.front().timestamp << " lastTs=" << record.packets.back().timestamp;
                WhisperReplayDebug(player, ss.str());
            }
            Battleground* bg = sBattlegroundMgr->CreateNewBattleground(record.typeId, GetBattlegroundBracketByLevel(record.mapId, 60), record.arenaTypeId, false);
            if (!bg) {
                loadedReplays.erase(GetPlayerLowGuid(player));
                handler.PSendSysMessage("Couldn't create arena map!");
                handler.SetSentErrorMessage(true);
                return false;
            }
            player->SetIsSpectator(true);
            bg->toggleReplay(GetPlayerLowGuid(player));
            player->SetPendingSpectatorForBG(bg->GetInstanceID());
            bg->StartBattleground();
            {
                std::ostringstream ss;
                ss << "[start] created bg instance=" << bg->GetInstanceID() << " type=" << uint32(bg->GetTypeID()) << " map=" << bg->GetMapId() << " status=" << ReplayStatusName(bg->GetStatus()) << " startDelay=" << bg->GetStartDelayTime();
                WhisperReplayDebug(player, ss.str());
            }

            BattlegroundTypeId bgTypeId = bg->GetTypeID();

            uint32 queueSlot = 0;
            WorldPacket data;
            TeamId teamId = TEAM_NEUTRAL;

            player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId, queueSlot, true, false, TEAM_NEUTRAL);
            sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bgTypeId);
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime(), bg->GetArenaType(), teamId);
            player->GetSession()->SendPacket(&data);

            handler.PSendSysMessage("Replay begins.");
            WhisperReplayDebug(player, "[start] teleport sent; diagnostics will continue every 5 seconds while waiting/pumping");
            return true;
        }

        bool loadReplayDataForPlayer(Player* p, uint32 matchId) {
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ARENA_REPLAYS);
            stmt->setUInt32(0, matchId);

            PreparedQueryResult result = CharacterDatabase.Query(stmt);
            if (!result) {
                ChatHandler(p->GetSession()).PSendSysMessage("Replay data not found.");
                return false;
            }

            Field* fields = result->Fetch();
            if (!fields) {
                ChatHandler(p->GetSession()).PSendSysMessage("Replay data not found.");
                return false;
            }
            MatchRecord record;
            deserializeMatchData(record, fields);
            {
                std::ostringstream ss;
                ss << "[load] db replay=" << matchId << " packets=" << record.packets.size() << " contentSize=" << fields[3].GetUInt32() << " blobBytes=" << fields[4].GetBinary().size();
                if (!record.packets.empty())
                    ss << " firstTs=" << record.packets.front().timestamp << " lastTs=" << record.packets.back().timestamp;
                WhisperReplayDebug(p, ss.str());
            }

            loadedReplays[GetPlayerLowGuid(p)] = std::move(record);
            return true;
        }

        std::vector<uint32> loadLast10Replays() {
            std::vector<uint32> records;
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_LAST_10_ARENA_REPLAYS);
            PreparedQueryResult result = CharacterDatabase.Query(stmt);
            if (!result)
                return records;

            do {
                Field* fields = result->Fetch();
                if (!fields)
                    return records;

                uint32 matchId = fields[0].GetUInt32();
                records.push_back(matchId);
            } while (result->NextRow());

            return records;
        }

        void deserializeMatchData(MatchRecord& record, Field* fields) {
            record.arenaTypeId = uint8(fields[1].GetUInt32());
            record.typeId = BattlegroundTypeId(fields[2].GetUInt32());
            uint32 const size = fields[3].GetUInt32();
            std::vector<uint8> data = fields[4].GetBinary();
            record.mapId = uint32(fields[5].GetUInt32());
            if (!size || data.empty())
                return;

            ByteBuffer buffer;
            buffer.append(data.data(), std::min<size_t>(size, data.size()));

            /** deserialize replay binary data **/
            uint32 packetSize;
            uint32 packetTimestamp;
            uint16 opcode;
            while (buffer.rpos() <= buffer.size() - 1) {
                buffer >> packetSize;
                buffer >> packetTimestamp;
                buffer >> opcode;

                WorldPacket packet(opcode, packetSize);

                if (packetSize > 0) {
                    std::vector<uint8> tmp(packetSize, 0);
                    buffer.read(&tmp[0], packetSize);
                    packet.append(&tmp[0], packetSize);
                }

                record.packets.push_back({ packetTimestamp, packet });
            }

            if (!record.packets.empty())
            {
                uint32 const replayStartTimestamp = record.packets.front().timestamp;
                if (replayStartTimestamp)
                    for (PacketRecord& packetRecord : record.packets)
                        packetRecord.timestamp -= replayStartTimestamp;
            }
        }
    };
    CreatureAI* GetAI(Creature* creature) const override
    {
        return new replayAI(creature);
    }
};

void AddBGReplayScripts() {
    new BGReplayServerScript();
    new BGReplayWorldScript();
    new BGReplayBGScript();
    new ReplayGossip();
}
