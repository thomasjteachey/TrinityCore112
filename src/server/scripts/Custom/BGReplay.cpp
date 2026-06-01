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
#include "Map.h"
#include "Player.h"
#include "WorldSession.h"
#include "TemporarySummon.h"
#include <DBCStores.h>


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


namespace
{
    constexpr uint32 REPLAY_PLAYER_CREATURE_ENTRY = 900001;
    constexpr uint32 REPLAY_FORMAT_MAGIC = 0x52504742; // BGPR
    constexpr uint32 REPLAY_FORMAT_VERSION = 1;
}

struct PacketRecord { uint32 timestamp; WorldPacket packet; };

struct ReplayPlayerSnapshot
{
    ObjectGuid originalGuid;
    ObjectGuid replayGuid;
    std::string name;
    uint32 team = 0;
    uint32 faction = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
};

struct MatchRecord
{
    BattlegroundTypeId typeId;
    uint8 arenaTypeId;
    uint32 mapId;
    std::vector<ReplayPlayerSnapshot> players;
    std::vector<ObjectGuid> replaySummons;
    std::deque<PacketRecord> packets;
};

std::unordered_map<uint32, MatchRecord> records;
std::unordered_map<uint64, MatchRecord> loadedReplays;


static ReplayPlayerSnapshot* FindReplayPlayerSnapshot(MatchRecord& record, ObjectGuid const& guid)
{
    auto itr = std::find_if(record.players.begin(), record.players.end(), [guid](ReplayPlayerSnapshot const& player)
    {
        return player.originalGuid == guid;
    });

    return itr != record.players.end() ? &(*itr) : nullptr;
}

static void CaptureReplayPlayers(Battleground* bg, MatchRecord& record)
{
    for (auto const& itr : bg->GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(itr.first);
        if (!player)
            continue;

        ReplayPlayerSnapshot* snapshot = FindReplayPlayerSnapshot(record, player->GetGUID());
        if (!snapshot)
        {
            record.players.push_back(ReplayPlayerSnapshot());
            snapshot = &record.players.back();
            snapshot->originalGuid = player->GetGUID();
        }

        snapshot->name = player->GetName();
        snapshot->team = itr.second.Team;
        snapshot->faction = player->GetFaction();
        snapshot->x = player->GetPositionX();
        snapshot->y = player->GetPositionY();
        snapshot->z = player->GetPositionZ();
        snapshot->o = player->GetOrientation();
    }
}

static void DespawnReplayCopies(Battleground* bg, MatchRecord& match)
{
    BattlegroundMap* map = bg ? bg->FindBgMap() : nullptr;
    if (!map)
    {
        match.replaySummons.clear();
        return;
    }

    for (ObjectGuid const& guid : match.replaySummons)
        if (Creature* creature = map->GetCreature(guid))
            creature->DespawnOrUnsummon();

    match.replaySummons.clear();
}


static bool SpawnReplayCopies(Battleground* bg, MatchRecord& match, ChatHandler& handler)
{
    BattlegroundMap* map = bg ? bg->FindBgMap() : nullptr;
    if (!map)
        return false;

    for (ReplayPlayerSnapshot& replayPlayer : match.players)
    {
        Position position(replayPlayer.x, replayPlayer.y, replayPlayer.z, replayPlayer.o);
        if (position.GetPositionX() == 0.0f && position.GetPositionY() == 0.0f && position.GetPositionZ() == 0.0f)
            if (Position const* startPosition = bg->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(replayPlayer.team)))
                position = *startPosition;

        TempSummon* summon = map->SummonCreature(REPLAY_PLAYER_CREATURE_ENTRY, position);
        if (!summon)
        {
            handler.PSendSysMessage("Couldn't create replay player copy for %s.", replayPlayer.name.c_str());
            handler.SetSentErrorMessage(true);
            DespawnReplayCopies(bg, match);
            return false;
        }

        Creature* creature = summon->ToCreature();
        creature->CopyAppearanceFromPlayerGuid(replayPlayer.originalGuid, true, true, false, false);
        creature->SetName(replayPlayer.name + " (Replay)");
        creature->SetFaction(replayPlayer.faction);
        creature->SetReactState(REACT_PASSIVE);
        replayPlayer.replayGuid = creature->GetGUID();
        match.replaySummons.push_back(creature->GetGUID());
    }

    return true;
}


static bool IsReplayMovementOpcode(uint16 opcode)
{
    switch (opcode)
    {
        case MSG_MOVE_START_FORWARD:
        case MSG_MOVE_SET_FACING:
        case MSG_MOVE_HEARTBEAT:
        case MSG_MOVE_JUMP:
        case MSG_MOVE_FALL_LAND:
        case MSG_MOVE_START_STRAFE_RIGHT:
        case MSG_MOVE_STOP_STRAFE:
        case MSG_MOVE_START_STRAFE_LEFT:
        case MSG_MOVE_STOP:
        case MSG_MOVE_START_BACKWARD:
        case MSG_MOVE_START_TURN_LEFT:
        case MSG_MOVE_STOP_TURN:
        case MSG_MOVE_START_TURN_RIGHT:
            return true;
        default:
            return false;
    }
}

static bool ReadReplayMovementInfo(WorldPacket& source, MovementInfo& movementInfo)
{
    WorldPacket data(source);
    data.rpos(0);

    if (data.rpos() >= data.size())
        return false;

    data >> movementInfo.guid.ReadAsPacked();
    data >> movementInfo.flags;
    data >> movementInfo.flags2;
    data >> movementInfo.time;
    data >> movementInfo.pos.PositionXYZOStream();

    if (!movementInfo.pos.IsPositionValid())
        return false;

    if (movementInfo.HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
    {
        data >> movementInfo.transport.guid.ReadAsPacked();
        data >> movementInfo.transport.pos.PositionXYZOStream();
        data >> movementInfo.transport.time;
        data >> movementInfo.transport.seat;

        if (movementInfo.HasExtraMovementFlag(MOVEMENTFLAG2_INTERPOLATED_MOVEMENT))
            data >> movementInfo.transport.time2;
    }

    if (movementInfo.HasMovementFlag(MovementFlags(MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING)) || movementInfo.HasExtraMovementFlag(MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING))
        data >> movementInfo.pitch;

    data >> movementInfo.fallTime;

    if (movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING))
    {
        data >> movementInfo.jump.zspeed;
        data >> movementInfo.jump.sinAngle;
        data >> movementInfo.jump.cosAngle;
        data >> movementInfo.jump.xyspeed;
    }

    if (movementInfo.HasMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION))
        data >> movementInfo.splineElevation;

    return true;
}

static void ApplyReplayMovementPacket(Battleground* bg, MatchRecord& match, WorldPacket& packet)
{
    if (!IsReplayMovementOpcode(packet.GetOpcode()))
        return;

    MovementInfo movementInfo;
    if (!ReadReplayMovementInfo(packet, movementInfo))
        return;

    ReplayPlayerSnapshot* replayPlayer = FindReplayPlayerSnapshot(match, movementInfo.guid);
    if (!replayPlayer || !replayPlayer->replayGuid)
        return;

    BattlegroundMap* map = bg ? bg->FindBgMap() : nullptr;
    if (!map)
        return;

    Creature* creature = map->GetCreature(replayPlayer->replayGuid);
    if (!creature)
        return;

    movementInfo.guid = replayPlayer->replayGuid;
    creature->m_movementInfo = movementInfo;
    creature->UpdatePosition(movementInfo.pos.GetPositionX(), movementInfo.pos.GetPositionY(), movementInfo.pos.GetPositionZ(), movementInfo.pos.GetOrientation(), false);

    WorldPacket data(packet.GetOpcode(), packet.size());
    WorldSession::WriteMovementInfo(&data, &creature->m_movementInfo);
    creature->SendMessageToSet(&data, true);
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
        //ignore packets until arena started
        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS) return;
        //record packets from 1 player of each team
        //iterate just in case a player leaves and used as reference
        for (auto it : bg->GetPlayers()) {
            if (it.second.Team == session->GetPlayer()->GetBGTeam()) {
                if (it.first.GetRawValue() != session->GetPlayer()->GetGUID())
                    return; else break;
            }
        }
        //ignore packets not in watch list
        if (std::find(watchList.begin(), watchList.end(), packet.GetOpcode()) == watchList.end())
        {
            return;
        }

        if (records.find(bg->GetInstanceID()) == records.end())
            records[bg->GetInstanceID()].packets.clear();
        MatchRecord& record = records[bg->GetInstanceID()];
        CaptureReplayPlayers(bg, record);

        uint32 timestamp = bg->GetStartTime();
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
        // Only record arena matches
        if (!bg->isArena())
            return;

        //save replay when a bg ends
        if (!bg->IsReplay()) {
            saveReplay(bg);
            return;
        }
    }

    void OnBattlegroundUpdate(Battleground* bg, uint32 diff) override {

        if (!bg->isArena())
            return;

        if (!bg->IsReplay()) return;
        int32 startDelayTime = bg->GetStartDelayTime();
        if (startDelayTime > 5000)
        {
            bg->SetStartDelayTime(5000);
            bg->SetStartTime(bg->GetStartTime() + (startDelayTime - 5000));
        }
        //retrieve replay data
        auto it = loadedReplays.find(bg->GetReplayId());
        if (it == loadedReplays.end()) return;
        MatchRecord& match = it->second;

        uint32 playerGUID = bg->GetReplayId();
        Player* player = ObjectAccessor::FindPlayerByLowGUID(playerGUID);
        if (!player)
        {
            DespawnReplayCopies(bg, match);
            loadedReplays.erase(it);
            bg->toggleReplay(0);
            return;
        }

        if (bg->GetStatus() == BattlegroundStatus::STATUS_WAIT_JOIN)
        {
            // Replay viewers are spectators, not battleground participants. A normal arena only advances
            // from WAIT_JOIN after a participant joins, so force the replay arena to start once the
            // spectator transfer has created the map.
            if (bg->FindBgMap())
                bg->SkipStartDelay();

            return;
        }

        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS) return;

        bool spectatorInReplay = player->IsInWorld() && player->GetBattleground() == bg && player->IsSpectator();

        // If the spectator has not finished teleporting into the replay map yet, keep the replay data
        // loaded and wait instead of treating the empty participant list as a finished replay.
        if (!spectatorInReplay)
            return;

        //if replay ends > free replay data and return the spectator without sending the arena score screen
        if (match.packets.empty()) {
            DespawnReplayCopies(bg, match);
            loadedReplays.erase(it);
            bg->toggleReplay(0);
            player->LeaveBattleground(bg);
            return;
        }

        if (!match.players.empty() && match.replaySummons.empty())
        {
            // Battleground maps are created when the spectator transfer completes, so delay replay-copy spawning until then.
            if (!bg->FindBgMap())
                return;

            ChatHandler handler(player->GetSession());
            if (!SpawnReplayCopies(bg, match, handler))
            {
                loadedReplays.erase(it);
                bg->EndNow();
                bg->toggleReplay(0);
                player->LeaveBattleground(bg);
                return;
            }
        }

        //apply replay data to the server-spawned replay copies
        while (!match.packets.empty() && match.packets.front().timestamp <= bg->GetStartTime()) {
            ApplyReplayMovementPacket(bg, match, match.packets.front().packet);
            match.packets.pop_front();
        }
    }

    void saveReplay(Battleground* bg) {
        //retrieve replay data
        auto it = records.find(bg->GetInstanceID());
        if (it == records.end()) return;
        MatchRecord& match = it->second;

        //serialize arena replay data
        ByteBuffer buffer;
        buffer << REPLAY_FORMAT_MAGIC;
        buffer << REPLAY_FORMAT_VERSION;
        buffer << uint32(match.players.size());
        for (ReplayPlayerSnapshot const& replayPlayer : match.players)
        {
            buffer << replayPlayer.originalGuid;
            buffer << replayPlayer.name;
            buffer << replayPlayer.team;
            buffer << replayPlayer.faction;
            buffer << replayPlayer.x;
            buffer << replayPlayer.y;
            buffer << replayPlayer.z;
            buffer << replayPlayer.o;
        }

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

            if (!loadReplayDataForPlayer(player, replayId))
                return false;

            MatchRecord record = loadedReplays[player->GetGUID()];
            Battleground* bg = sBattlegroundMgr->CreateNewBattleground(record.typeId, GetBattlegroundBracketByLevel(record.mapId, 60), record.arenaTypeId, false);
            if (!bg) {
                handler.PSendSysMessage("Couldn't create arena map!");
                handler.SetSentErrorMessage(true);
                return false;
            }
            bg->toggleReplay(player->GetGUID());
            player->SetPendingSpectatorForBG(bg->GetInstanceID());
            bg->StartBattleground();

            BattlegroundTypeId bgTypeId = bg->GetTypeID();

            player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId, PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
            player->SetEntryPoint();
            sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bgTypeId);

            handler.PSendSysMessage("Replay begins.");
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

            loadedReplays[p->GetGUID()] = std::move(record);
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
            std::vector<uint8> data = fields[4].GetBinary();
            record.mapId = uint32(fields[5].GetUInt32());
            ByteBuffer buffer;
            if (!data.empty())
                buffer.append(data.data(), data.size());

            /** deserialize replay binary data **/
            uint32 packetSize;
            uint32 packetTimestamp;
            uint16 opcode;

            if (buffer.size() >= sizeof(uint32))
            {
                uint32 magic = buffer.read<uint32>();
                if (magic == REPLAY_FORMAT_MAGIC)
                {
                    uint32 version = buffer.read<uint32>();
                    if (version == REPLAY_FORMAT_VERSION)
                    {
                        uint32 playerCount = buffer.read<uint32>();
                        record.players.reserve(playerCount);
                        for (uint32 i = 0; i < playerCount; ++i)
                        {
                            ReplayPlayerSnapshot replayPlayer;
                            buffer >> replayPlayer.originalGuid;
                            buffer >> replayPlayer.name;
                            buffer >> replayPlayer.team;
                            buffer >> replayPlayer.faction;
                            buffer >> replayPlayer.x;
                            buffer >> replayPlayer.y;
                            buffer >> replayPlayer.z;
                            buffer >> replayPlayer.o;
                            record.players.push_back(replayPlayer);
                        }
                    }
                }
                else
                    buffer.rpos(0);
            }

            while (buffer.rpos() + sizeof(uint32) + sizeof(uint32) + sizeof(uint16) <= buffer.size()) {
                buffer >> packetSize;
                buffer >> packetTimestamp;
                buffer >> opcode;

                if (buffer.rpos() + packetSize > buffer.size())
                    break;

                WorldPacket packet(opcode, packetSize);

                if (packetSize > 0) {
                    std::vector<uint8> tmp(packetSize, 0);
                    buffer.read(tmp.data(), packetSize);
                    packet.append(tmp.data(), packetSize);
                }

                record.packets.push_back({ packetTimestamp, packet });
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
    new BGReplayBGScript();
    new ReplayGossip();
}
