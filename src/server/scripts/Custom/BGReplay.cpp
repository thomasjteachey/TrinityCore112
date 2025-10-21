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
#include "WorldSocket.h"
#include "Log.h"
#include "Map.h"
#include "World.h"
#include "ObjectDefines.h"
#include "ObjectMgr.h"
#include "GameTime.h"
#include "Random.h"
#include <boost/asio.hpp>
#include <unordered_map>
#include <unordered_set>
#include "DatabaseEnv.h"
#include <ObjectAccessor.h>
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "GameEventMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include <DBCStores.h>
#include <zlib.h>


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
struct MatchRecord {
    BattlegroundTypeId typeId;
    uint8 arenaTypeId;
    uint32 mapId;
    uint32 startTime = 0;
    BattlegroundBracketId bracketId = BG_BRACKET_ID_FIRST;
    ObjectGuid allianceRecorder;
    ObjectGuid hordeRecorder;
    std::deque<PacketRecord> packets;
    std::unordered_map<uint64, uint64> guidRemaps;
};

namespace
{
    constexpr uint32 ReplayFormatMagic = 0x42475250; // 'BGRP'

    bool IsAlternateGuidUsed(MatchRecord const& record, uint64 alternateRaw)
    {
        for (auto const& pair : record.guidRemaps)
            if (pair.second == alternateRaw)
                return true;
        return false;
    }
}

namespace
{
    ObjectGuid GenerateAlternateGuid(ObjectGuid guid)
    {
        uint64 raw = guid.GetRawValue();
        uint64 newRaw = raw;
        for (int i = 0; i < 6; ++i)
        {
            uint8 oldByte = (raw >> (i * 8)) & 0xFF;
            if (oldByte)
            {
                uint8 newByte = oldByte;
                while (newByte == oldByte)
                    newByte = uint8(urand(1, 255));
                newRaw &= ~(uint64(0xFF) << (i * 8));
                newRaw |= uint64(newByte) << (i * 8);
            }
        }
        return ObjectGuid(newRaw);
    }

    PvPDifficultyEntry const* GetFirstBracketForMap(uint32 mapId)
    {
        PvPDifficultyEntry const* firstEntry = nullptr;
        for (uint8 bracketId = BG_BRACKET_ID_FIRST; bracketId <= BG_BRACKET_ID_LAST; ++bracketId)
        {
            if (PvPDifficultyEntry const* entry = GetBattlegroundBracketById(mapId, BattlegroundBracketId(bracketId)))
            {
                if (!firstEntry || entry->MinLevel < firstEntry->MinLevel)
                    firstEntry = entry;
            }
        }

        return firstEntry;
    }

    PvPDifficultyEntry const* ResolveReplayBracket(Player const* spectator, MatchRecord const& record)
    {
        if (record.bracketId <= BG_BRACKET_ID_LAST)
            if (PvPDifficultyEntry const* bracket = GetBattlegroundBracketById(record.mapId, record.bracketId))
                return bracket;

        if (spectator)
            if (PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(record.mapId, spectator->GetLevel()))
                return bracket;

        if (PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(record.mapId, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)))
            return bracket;

        return GetFirstBracketForMap(record.mapId);
    }

    void ReplaceGuid(WorldPacket& packet, ObjectGuid const& oldGuid, ObjectGuid const& newGuid)
    {
        if (oldGuid == newGuid || oldGuid.IsEmpty())
            return;

        bool compressed = packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT;
        ByteBuffer buffer;

        if (compressed)
        {
            if (packet.size() < sizeof(uint32))
                return;

            uint32 size = packet.read<uint32>(0);
            if (!size)
                return;

            buffer.resize(size);
            uLongf realSize = size;
            if (uncompress(buffer.contents(), &realSize, packet.contents() + sizeof(uint32), packet.size() - sizeof(uint32)) != Z_OK)
                return;
            buffer.resize(realSize);
            TC_LOG_DEBUG("bg.replay", "Decompressed packet opcode {} from {} to {} bytes", GetOpcodeNameForLogging(static_cast<Opcodes>(packet.GetOpcode())), packet.size(), realSize);

        }
        else
        {
            buffer.resize(packet.size());
            if (packet.size() > 0)
                memcpy(buffer.contents(), packet.contents(), packet.size());
        }

        uint64 oldRaw = oldGuid.GetRawValue();
        uint64 newRaw = newGuid.GetRawValue();

        size_t len = buffer.size();
        uint8* data = buffer.contents();
        size_t replaced = 0;
        for (size_t i = 0; i + sizeof(uint64) <= len; ++i)
        {
            uint64 val;
            memcpy(&val, data + i, sizeof(uint64));
            if (val == oldRaw)
            {
                memcpy(data + i, &newRaw, sizeof(uint64));
                ++replaced;
            }
        }

        ByteBuffer oldPack; oldPack << oldGuid.WriteAsPacked();
        ByteBuffer newPack; newPack << newGuid.WriteAsPacked();
        auto oldVec = oldPack.contentsAsVector();
        auto newVec = newPack.contentsAsVector();

        if (!oldVec.empty())
        {
            if (oldVec.size() == newVec.size())
            {
                for (size_t i = 0; i + oldVec.size() <= len; ++i)
                    if (memcmp(data + i, oldVec.data(), oldVec.size()) == 0)
                        memcpy(data + i, newVec.data(), newVec.size());
            }
            else
            {
                std::vector<uint8> newData;
                newData.reserve(len); // approximate
                for (size_t i = 0; i < len;)
                {
                    if (i + oldVec.size() <= len && memcmp(data + i, oldVec.data(), oldVec.size()) == 0)
                    {
                        newData.insert(newData.end(), newVec.begin(), newVec.end());
                        i += oldVec.size();
                    }
                    else
                    {
                        newData.push_back(data[i]);
                        ++i;
                    }
                }
                buffer.clear();
                buffer.append(newData.data(), newData.size());
                len = buffer.size();
                data = buffer.contents();
            }
        }

        TC_LOG_DEBUG("bg.replay", "ReplaceGuid {} -> {} replaced {} raw occurrences", oldGuid.ToString(), newGuid.ToString(), replaced);

        packet.clear();
        if (compressed)
        {
            uLongf destSize = compressBound(len);
            packet.resize(destSize + sizeof(uint32));
            packet.put<uint32>(0, len);
            if (compress(packet.contents() + sizeof(uint32), &destSize, buffer.contents(), len) != Z_OK)
                return;
            packet.resize(destSize + sizeof(uint32));
            packet.SetOpcode(SMSG_COMPRESSED_UPDATE_OBJECT);
        }
        else
        {
            packet.append(buffer.contents(), len);
        }
    }
}
std::unordered_map<uint32, MatchRecord> records;
std::unordered_map<uint32, MatchRecord> loadedReplays;
// Headless spectators used for recording packets per battleground instance
std::unordered_map<uint32, Player*> replayBots;
// Helper container to avoid recursive bot creation
std::unordered_set<uint32> creatingReplayBots;

namespace
{
    // Dummy socket used so the headless spectator can receive packets
    class ReplaySocket : public WorldSocket
    {
    public:
        ReplaySocket() : WorldSocket(Create()) {}

        static tcp::socket Create()
        {
            static boost::asio::io_context io;
            tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
            tcp::socket server(io);
            tcp::socket client(io);
            client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(),
                                         acceptor.local_endpoint().port()));
            acceptor.accept(server);
            io.poll();
            acceptor.close();
            server.close();
            return client;
        }

        void Start() override {}
        bool Update() override { return true; }
    };

    Player* CreateReplayBot(Battleground* bg)
    {
        if (!bg->FindBgMap())
            return nullptr;
        WorldSession* botSession = new WorldSession(0, "ReplayBot", std::make_shared<ReplaySocket>(), SEC_ADMINISTRATOR,
            2, 0, Minutes(0), LOCALE_enUS, 0, false);
        Player* bot = new Player(botSession);

        struct ReplayBotCreateInfo : CharacterCreateInfo
        {
            ReplayBotCreateInfo()
            {
                Name = "ReplayBot";
                Race = RACE_HUMAN;
                Class = CLASS_MAGE;
                Gender = GENDER_MALE;
            }
        } createInfo;

        bot->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &createInfo);
        botSession->SetPlayer(bot);
        bot->GetMotionMaster()->Initialize();

        bot->SetGameMaster(true);
        bot->SetGMVisible(false);
        bot->SetIsSpectator(true);

        Position const* pos = bg->GetTeamStartPosition(TEAM_ALLIANCE);
        bot->Relocate(*pos);
        bot->SetBattlegroundId(bg->GetInstanceID(), bg->GetTypeID(), PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
        bot->ResetMap();
        bot->SetMap(bg->GetBgMap());
        bg->GetBgMap()->AddPlayerToMap(bot);
        bg->AddSpectator(bot);
        TC_LOG_DEBUG("bg.replay", "Created replay bot for instance {}", bg->GetInstanceID());
        return bot;
    }

    void DestroyReplayBot(Battleground* bg)
    {
        auto itr = replayBots.find(bg->GetInstanceID());
        if (itr == replayBots.end())
            return;
        Player* bot = itr->second;
        WorldSession* session = bot->GetSession();
        bot->CleanupsBeforeDelete();
        if (bot->GetMap())
            bot->GetMap()->RemovePlayerFromMap(bot, true);
        if (session)
            session->SetPlayer(nullptr);
        delete session;
        replayBots.erase(itr);
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
        // ignore packets before players have entered the battleground
        if (bg->GetStatus() <= BattlegroundStatus::STATUS_WAIT_QUEUE) return;


        // ensure the record container exists for this battleground instance
        if (records.find(bg->GetInstanceID()) == records.end())
            records[bg->GetInstanceID()] = MatchRecord();


        MatchRecord& record = records[bg->GetInstanceID()];

        BattlegroundMap* map = bg->FindBgMap();
        if (!map)
            return; // map hasn't been created yet

        // player hasn't teleported into the battleground instance
        if (session->GetPlayer()->GetMap() != map)
            return;

        uint32 instanceId = bg->GetInstanceID();
        if (!replayBots[instanceId])
        {
            if (creatingReplayBots.count(instanceId))
                return;

            creatingReplayBots.insert(instanceId);
            replayBots[instanceId] = CreateReplayBot(bg);
            creatingReplayBots.erase(instanceId);
            return;
        }
        if (session->GetPlayer() != replayBots[instanceId])
            return;
        //ignore packets not in watch list
        if (std::find(watchList.begin(), watchList.end(), packet.GetOpcode()) == watchList.end())
            return;

        TC_LOG_TRACE("bg.replay", "Recording opcode {} size {}", GetOpcodeNameForLogging(static_cast<Opcodes>(packet.GetOpcode())), packet.size());

        if (record.startTime == 0)
        {
            record.startTime = GameTime::GetGameTimeMS() - bg->GetStartTime();
            record.bracketId = bg->GetBracketId();
            for (auto const& itr : bg->GetPlayers())
            {
                ObjectGuid guid = itr.first;
                if (Player* plr = ObjectAccessor::FindPlayer(guid))
                {
                    if (!plr->IsSpectator())
                    {
                        uint64 rawGuid = guid.GetRawValue();
                        if (record.guidRemaps.find(rawGuid) == record.guidRemaps.end())
                        {
                            ObjectGuid alternate;
                            do
                            {
                                alternate = GenerateAlternateGuid(guid);
                            } while (alternate.IsEmpty() || alternate == guid || IsAlternateGuidUsed(record, alternate.GetRawValue()));

                            record.guidRemaps[rawGuid] = alternate.GetRawValue();
                        }
                    }

                    uint32 team = plr->GetBGTeam();
                    if (team == ALLIANCE && record.allianceRecorder.IsEmpty())
                        record.allianceRecorder = plr->GetGUID();
                    else if (team == HORDE && record.hordeRecorder.IsEmpty())
                        record.hordeRecorder = plr->GetGUID();

                    if (!record.allianceRecorder.IsEmpty() && !record.hordeRecorder.IsEmpty())
                        break;
                }
            }
        }
        else
        {
            for (auto const& itr : bg->GetPlayers())
            {
                ObjectGuid guid = itr.first;
                if (record.guidRemaps.find(guid.GetRawValue()) != record.guidRemaps.end())
                    continue;

                if (Player* plr = ObjectAccessor::FindPlayer(guid))
                {
                    if (plr->IsSpectator())
                        continue;

                    ObjectGuid alternate;
                    do
                    {
                        alternate = GenerateAlternateGuid(guid);
                    } while (alternate.IsEmpty() || alternate == guid || IsAlternateGuidUsed(record, alternate.GetRawValue()));

                    record.guidRemaps[guid.GetRawValue()] = alternate.GetRawValue();
                }
            }
        }

        uint32 timestamp = GameTime::GetGameTimeMS() - record.startTime;
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


class BGReplayBGScript : public BattlegroundScript {
public:
    BGReplayBGScript() : BattlegroundScript("BGReplayBGScript") {}

    void OnBattlegroundEnd(Battleground* bg, uint32 winner) override
    {
        //save replay when a bg ends
        if (!bg->IsReplay()) {
            saveReplay(bg);
        }
        DestroyReplayBot(bg);
        return;
    }

    void OnBattlegroundUpdate(Battleground* bg, uint32 diff) override {

        if (!bg->IsReplay()) return;
        int32 startDelayTime = bg->GetStartDelayTime();
        if (startDelayTime > 5000)
        {
            bg->SetStartDelayTime(5000);
            bg->SetStartTime(bg->GetStartTime() + (startDelayTime - 5000));
        }
        if (bg->GetStatus() < BattlegroundStatus::STATUS_WAIT_JOIN) return;

        //retrieve replay data
        auto it = loadedReplays.find(bg->GetReplayId());
        if (it == loadedReplays.end()) return;
        MatchRecord& match = it->second;

        // ensure spectator has finished loading into the battleground
        Player* spectator = ObjectAccessor::FindPlayerByLowGUID(bg->GetReplayId());
        if (!spectator || spectator->GetMapId() != bg->GetMapId())
            return;

        // free data once all spectators have left or the replay is finished
        if (match.packets.empty() || !bg->HaveSpectators()) {
            loadedReplays.erase(it);
            if (bg->HaveSpectators())
            {
                uint32 playerGUID = bg->GetReplayId();
                bg->EndNow();
                bg->toggleReplay(0);
                if (Player* player = ObjectAccessor::FindPlayerByLowGUID(playerGUID))
                    player->LeaveBattleground(bg);
            }
            return;
        }

        //send replay data to spectator
        while (!match.packets.empty() && match.packets.front().timestamp <= bg->GetStartTime()) {
            if (!bg->HaveSpectators())
                break;

            TC_LOG_TRACE("bg.replay", "Sending opcode {} size {}", GetOpcodeNameForLogging(static_cast<Opcodes>(match.packets.front().packet.GetOpcode())), match.packets.front().packet.size());
            spectator->GetSession()->SendPacket(&match.packets.front().packet);
            match.packets.pop_front();
        }
    }

    void saveReplay(Battleground* bg) {
        //retrieve replay data
        auto it = records.find(bg->GetInstanceID());
        if (it == records.end()) return;
        MatchRecord& match = it->second;

        if (match.typeId == BATTLEGROUND_TYPE_NONE)
        {
            match.typeId = bg->GetTypeID(false);
            if (match.typeId == BATTLEGROUND_AA)
                match.typeId = bg->GetTypeID(true);
        }
        if (!match.arenaTypeId)
            match.arenaTypeId = bg->GetArenaType();
        if (!match.mapId)
            match.mapId = bg->GetMapId();


        if (match.packets.empty())
        {
            TC_LOG_DEBUG("bg.replay", "Replay for instance {} contains no packets and will not be saved", bg->GetInstanceID());
            records.erase(it);
            return;
        }

        TC_LOG_INFO("bg.replay", "Saving replay for instance {} with {} packets", bg->GetInstanceID(), match.packets.size());

        // serialize arena replay data
        ByteBuffer buffer;
        buffer << ReplayFormatMagic;
        buffer << uint16(2);
        buffer << match.startTime;
        buffer << uint8(match.bracketId);
        buffer << match.allianceRecorder;
        buffer << match.hordeRecorder;
        buffer << uint32(match.guidRemaps.size());
        for (auto const& [originalRaw, mappedRaw] : match.guidRemaps)
        {
            buffer << ObjectGuid(originalRaw);
            buffer << ObjectGuid(mappedRaw);
        }
        uint32 headerSize;
        uint32 timestamp;
        for (auto it : match.packets)
        {
            headerSize = it.packet.size(); // header 4 Bytes packet size
            timestamp = it.timestamp;

            buffer << headerSize; // 4 bytes
            buffer << timestamp; // 4 bytes
            buffer << it.packet.GetOpcode(); // 2 bytes
            if (headerSize > 0)
                buffer.append(it.packet.contents(), it.packet.size()); // headerSize bytes
        }

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ARENA_REPLAYS);
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
    ReplayGossip() : CreatureScript("ReplayGossip") { }
    struct replayAI : public ScriptedAI
    {
        replayAI(Creature* creature) : ScriptedAI(creature) { }
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

            if (!loadReplayDataForPlayer(player, replayId))
                return false;

            uint32 const spectatorLowGuid = player->GetGUID().GetCounter();
            if (loadedReplays[spectatorLowGuid].packets.empty())
            {
                handler.PSendSysMessage("Replay data not found.");
                handler.SetSentErrorMessage(true);
                return false;
            }

            MatchRecord record = loadedReplays[spectatorLowGuid];
            PvPDifficultyEntry const* bracketEntry = ResolveReplayBracket(player, record);
            if (!bracketEntry)
            {
                handler.PSendSysMessage("Couldn't locate a valid bracket for replay map!");
                handler.SetSentErrorMessage(true);
                return false;
            }

            Battleground* bg = sBattlegroundMgr->CreateNewBattleground(record.typeId, bracketEntry, record.arenaTypeId, false);
            if (!bg) {
                handler.PSendSysMessage("Couldn't create arena map!");
                handler.SetSentErrorMessage(true);
                return false;
            }
            bg->SetMapId(record.mapId);
            player->SetIsSpectator(true);
            bg->toggleReplay(spectatorLowGuid);
            player->SetPendingSpectatorForBG(bg->GetInstanceID());
            bg->StartBattleground();

            BattlegroundTypeId bgTypeId = bg->GetTypeID();

            uint32 queueSlot = 0;
            WorldPacket data;
            TeamId teamId = TEAM_NEUTRAL;

            player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId, queueSlot, true, false, TEAM_NEUTRAL);
            sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bgTypeId);
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime(), bg->GetArenaType(), teamId);
            player->GetSession()->SendPacket(&data);

            handler.PSendSysMessage("Replay begins.");
            return true;
        }

        bool loadReplayDataForPlayer(Player* p, uint32 matchId) {
            TC_LOG_INFO("bg.replay", "Loading replay {} for player {} ({})", matchId, p->GetName(), p->GetGUID().ToString());
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
            TC_LOG_INFO("bg.replay", "Deserialized replay: start {} map {} packets {}", record.startTime, record.mapId, record.packets.size());

            ObjectGuid spectator = p->GetGUID();
            if (record.guidRemaps.empty() && (record.allianceRecorder == spectator || record.hordeRecorder == spectator))
            {
                ObjectGuid newGuid = ObjectGuid::Create<HighGuid::Player>(sObjectMgr->GetGenerator<HighGuid::Player>().Generate());
                TC_LOG_INFO("bg.replay", "Replacing spectator GUID {} with {}", spectator.ToString(), newGuid.ToString());
                for (PacketRecord& r : record.packets)
                    ReplaceGuid(r.packet, spectator, newGuid);

                if (record.allianceRecorder == spectator)
                    record.allianceRecorder = newGuid;
                if (record.hordeRecorder == spectator)
                    record.hordeRecorder = newGuid;

                record.guidRemaps[spectator.GetRawValue()] = newGuid.GetRawValue();
            }

            uint32 const spectatorLowGuid = p->GetGUID().GetCounter();
            loadedReplays[spectatorLowGuid] = std::move(record);
            TC_LOG_INFO("bg.replay", "Loaded replay {} packets {} for spectator {}", matchId, loadedReplays[spectatorLowGuid].packets.size(), p->GetGUID().ToString());
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
            int size = uint32(fields[3].GetUInt32());
            std::vector<uint8> data = fields[4].GetBinary();
            record.mapId = uint32(fields[5].GetUInt32());
            ByteBuffer buffer;
            buffer.append(&data[0], data.size());

            uint32 maybeMagic;
            buffer >> maybeMagic;

            uint16 version = 0;
            if (maybeMagic == ReplayFormatMagic)
            {
                buffer >> version;
                buffer >> record.startTime;

                if (version >= 2)
                {
                    uint8 bracketIdValue;
                    buffer >> bracketIdValue;
                    if (bracketIdValue <= BG_BRACKET_ID_LAST)
                        record.bracketId = static_cast<BattlegroundBracketId>(bracketIdValue);
                }
            }
            else
            {
                record.startTime = maybeMagic;
            }

            buffer >> record.allianceRecorder;
            buffer >> record.hordeRecorder;

            if (version >= 1)
            {
                uint32 remapCount;
                buffer >> remapCount;
                for (uint32 i = 0; i < remapCount; ++i)
                {
                    ObjectGuid original;
                    ObjectGuid mapped;
                    buffer >> original;
                    buffer >> mapped;
                    record.guidRemaps[original.GetRawValue()] = mapped.GetRawValue();
                }
            }

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
                uint32 const firstTimestamp = record.packets.front().timestamp;
                if (firstTimestamp)
                {
                    for (PacketRecord& packetRecord : record.packets)
                    {
                        if (packetRecord.timestamp <= firstTimestamp)
                            packetRecord.timestamp = 0;
                        else
                            packetRecord.timestamp -= firstTimestamp;
                    }
                }
            }

            if (!record.guidRemaps.empty())
            {
                for (auto const& [originalRaw, mappedRaw] : record.guidRemaps)
                {
                    ObjectGuid original(originalRaw);
                    ObjectGuid mapped(mappedRaw);

                    if (record.allianceRecorder == original)
                        record.allianceRecorder = mapped;
                    if (record.hordeRecorder == original)
                        record.hordeRecorder = mapped;

                    for (PacketRecord& packetRecord : record.packets)
                        ReplaceGuid(packetRecord.packet, original, mapped);
                }
            }
        }
    };
    CreatureAI* GetAI(Creature* creature) const override
    {
        return new replayAI(creature);
    }
};

class BGReplayPlayerScript : public PlayerScript
{
public:
    BGReplayPlayerScript() : PlayerScript("BGReplayPlayerScript") { }

    void OnMapChanged(Player* player) override
    {
        if (Battleground* bg = player->GetBattleground())
            if (bg->IsReplay())
                if (BattlegroundMap* map = bg->FindBgMap())
                    map->SetVisibilityRange(MAX_VISIBILITY_DISTANCE);
    }
};

void AddBGReplayScripts() {
    new BGReplayServerScript();
    new BGReplayBGScript();
    new ReplayGossip();
    new BGReplayPlayerScript();
}
