//
// Arena Replay V7 replacement for the old BGReplay.cpp.
//
// Main changes:
// - Recording captures initial WAIT_JOIN visual packets so players actually exist in playback.
// - Replay playback is driven from PlayerScript::OnUpdate so it follows the actual viewer object after zoning; pre-start visual packets play immediately at replay time 0.
// - Playback never calls SkipStartDelay() before the replay map exists, avoiding the EndNow()/scoreboard/zone-out race.
// - Playback no longer calls EndNow()/LeaveBattleground() at EOF; it leaves you in the replay instance so the final frame stays visible.
// - Playback is driven by PlayerScript::OnUpdate and a monotonic playback clock, not OnBattlegroundUpdate.
// - Playback waits until the viewer is actually inside the replay battleground/map before sending frames, but it no longer blocks playback if countdown skipping fails.
// - Playback sends all due frames each tick, with a safety cap.
// - Compressed update-object replay now rewrites the uncompressed-size header after GUID replacement.
// - Replay startup force-opens arena doors and sets replay BG to IN_PROGRESS instead of waiting through prep.
// - Replay blobs written by this file contain a V2 header with participants.
// - During playback, original arena player GUIDs are rewritten to fake player GUIDs so the viewer can watch their own replays.
// - Fake player name-query responses are sent before playback starts.
//
// Install:
//   Replace your existing BGReplay.cpp with this file.
//   Keep your existing AddBGReplayScripts() loader entry.
//   Rebuild worldserver.
//
// Notes:
// - Existing V1 replay blobs still load, but they do not have participant metadata, so fake-GUID rewriting is only available
//   for new replays recorded after this file is installed.
// - GUID rewriting is byte-pattern based. It handles raw uint64 GUIDs, packed GUIDs, and compressed update-object payloads.
//   If you find an opcode that still references an original GUID, add a targeted parser later.
//

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <zlib.h>
#include <DBCStores.h>

namespace
{
    constexpr uint32 ARENA_REPLAY_V2_MAGIC = 0x32565241; // "ARV2" little-endian
    constexpr uint32 ARENA_REPLAY_V2_VERSION = 2;
    constexpr uint32 ARENA_REPLAY_FAKE_GUID_BASE = 0xF0000000u;
    constexpr uint32 ARENA_REPLAY_SEND_CAP_PER_UPDATE = 800;
    constexpr uint32 ARENA_REPLAY_LOAD_GRACE_MS = 15000;
    constexpr uint32 ARENA_REPLAY_START_DELAY_MS = 500;
    constexpr uint32 ARENA_REPLAY_PRELOAD_MS = 500;
    constexpr uint32 ARENA_REPLAY_DEDUPE_WINDOW_MS = 20;

    std::vector<Opcodes> const WatchList =
    {
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

    struct PacketRecord
    {
        uint32 TimestampMs = 0;
        WorldPacket Packet;
    };

    struct ReplayActor
    {
        ObjectGuid OriginalGuid;
        ObjectGuid FakeGuid;
        std::string Name;
        uint8 Race = 0;
        uint8 Class = 0;
        uint8 Gender = 0;
        uint32 Team = 0;
    };

    struct MatchRecord
    {
        BattlegroundTypeId TypeId = BATTLEGROUND_TYPE_NONE;
        uint8 ArenaTypeId = 0;
        uint32 MapId = 0;
        uint32 RecordStartMs = 0;
        uint32 InProgressStartMs = 0;
        uint32 PreStartPacketCount = 0;
        std::vector<ReplayActor> Actors;
        std::vector<PacketRecord> Packets;

        // Not serialized. Used only while recording to collapse duplicate cross-team broadcasts.
        std::unordered_map<uint64, uint32> RecentPacketHashTimes;
    };

    struct PlaybackState
    {
        MatchRecord Match;
        uint32 ViewerLowGuid = 0;
        uint32 BgInstanceId = 0;
        uint32 CreatedMs = 0;
        uint32 PlaybackStartMs = 0;
        size_t Cursor = 0;
        bool PlaybackClockStarted = false;
        bool SentInitialNameResponses = false;
        bool Finished = false;
    };

    // Real arena records by BG instance id.
    std::unordered_map<uint32, MatchRecord> Records;

    // Active playback states by viewer low guid.
    std::unordered_map<uint32, PlaybackState> ActiveReplays;

    bool IsWatchedOpcode(uint16 opcode)
    {
        return std::find(WatchList.begin(), WatchList.end(), opcode) != WatchList.end();
    }

    bool IsPreStartVisualOpcode(uint16 opcode)
    {
        switch (opcode)
        {
            case SMSG_UPDATE_OBJECT:
            case SMSG_COMPRESSED_UPDATE_OBJECT:
            case SMSG_DESTROY_OBJECT:
            case SMSG_NAME_QUERY_RESPONSE:
            case SMSG_PET_NAME_QUERY_RESPONSE:
            case SMSG_AURA_UPDATE:
            case SMSG_AURA_UPDATE_ALL:
            case SMSG_POWER_UPDATE:
            case SMSG_FORCE_RUN_SPEED_CHANGE:
            case SMSG_FORCE_FLIGHT_SPEED_CHANGE:
            case SMSG_FORCE_SWIM_SPEED_CHANGE:
                return true;
            default:
                return false;
        }
    }

    bool ShouldRecordPacket(Battleground const* bg, WorldPacket const& packet)
    {
        if (!bg)
            return false;

        BattlegroundStatus const status = bg->GetStatus();

        // The old version only recorded STATUS_IN_PROGRESS. That misses the initial update-object
        // create packets sent during arena prep/loading, so playback has movement/combat for GUIDs
        // the client never created. Record only visual/object packets during WAIT_JOIN, then record
        // the full watch list once the real match starts.
        if (status == STATUS_WAIT_JOIN)
            return IsPreStartVisualOpcode(packet.GetOpcode());

        if (status == STATUS_IN_PROGRESS)
            return IsWatchedOpcode(packet.GetOpcode());

        return false;
    }


    uint64 PacketHash(WorldPacket const& packet)
    {
        // FNV-1a 64-bit over opcode + payload.
        uint64 hash = 1469598103934665603ull;
        auto mixByte = [&hash](uint8 b)
        {
            hash ^= b;
            hash *= 1099511628211ull;
        };

        uint16 opcode = packet.GetOpcode();
        mixByte(uint8(opcode & 0xFF));
        mixByte(uint8((opcode >> 8) & 0xFF));

        uint8 const* data = packet.size() ? packet.contents() : nullptr;
        for (size_t i = 0; i < packet.size(); ++i)
            mixByte(data[i]);

        return hash;
    }

    bool IsDuplicateRecentPacket(MatchRecord& record, WorldPacket const& packet, uint32 nowMs)
    {
        uint64 hash = PacketHash(packet);
        auto itr = record.RecentPacketHashTimes.find(hash);
        if (itr != record.RecentPacketHashTimes.end() && nowMs - itr->second <= ARENA_REPLAY_DEDUPE_WINDOW_MS)
            return true;

        record.RecentPacketHashTimes[hash] = nowMs;

        // Keep the small recent hash map from growing forever in long games.
        if (record.RecentPacketHashTimes.size() > 4096)
        {
            for (auto it = record.RecentPacketHashTimes.begin(); it != record.RecentPacketHashTimes.end();)
            {
                if (nowMs - it->second > 1000)
                    it = record.RecentPacketHashTimes.erase(it);
                else
                    ++it;
            }
        }

        return false;
    }

    bool IsTeamRecorder(Battleground* bg, Player const* player)
    {
        if (!bg || !player)
            return false;

        // Keep the old "one player per team" idea to avoid duplicate broadcast packets,
        // but use a real clock for timestamps and a dedupe layer.
        for (auto const& it : bg->GetPlayers())
        {
            if (it.second.Team != player->GetBGTeam())
                continue;

            return it.first.GetRawValue() == player->GetGUID().GetRawValue();
        }

        return true;
    }

    uint32 MakeFakePlayerCounter(uint32 viewerLowGuid, uint32 index)
    {
        // Player high guid is 0x0000 on this branch, so fake player GUIDs are just high low counters.
        // Use high low-guid values that should never collide with real character IDs.
        uint32 base = ARENA_REPLAY_FAKE_GUID_BASE - ((viewerLowGuid & 0x0FFFu) << 4);
        return base - index - 1;
    }

    void AssignFakeGuids(MatchRecord& match, uint32 viewerLowGuid)
    {
        for (size_t i = 0; i < match.Actors.size(); ++i)
            match.Actors[i].FakeGuid = ObjectGuid::Create<HighGuid::Player>(MakeFakePlayerCounter(viewerLowGuid, uint32(i)));
    }

    void RefreshActorsFromBattleground(Battleground* bg, MatchRecord& match)
    {
        if (!bg)
            return;

        std::unordered_set<uint64> seen;
        for (ReplayActor const& actor : match.Actors)
            seen.insert(actor.OriginalGuid.GetRawValue());

        for (auto const& it : bg->GetPlayers())
        {
            if (seen.find(it.first.GetRawValue()) != seen.end())
                continue;

            Player* player = bg->_GetPlayer(it.first, it.second.OfflineRemoveTime != 0, "arena replay actor capture");
            if (!player)
                continue;

            ReplayActor actor;
            actor.OriginalGuid = player->GetGUID();
            actor.Name = player->GetName();
            actor.Race = player->GetRace();
            actor.Class = player->GetClass();
            actor.Gender = uint8(player->GetGender());
            actor.Team = it.second.Team;

            match.Actors.push_back(actor);
            seen.insert(actor.OriginalGuid.GetRawValue());
        }
    }

    MatchRecord& GetOrCreateRecord(Battleground* bg)
    {
        MatchRecord& record = Records[bg->GetInstanceID()];

        if (!record.RecordStartMs)
            record.RecordStartMs = getMSTime();

        record.TypeId = bg->GetTypeID(false);
        if (record.TypeId == BATTLEGROUND_AA)
            record.TypeId = bg->GetTypeID(true);

        record.ArenaTypeId = bg->GetArenaType();
        record.MapId = bg->GetMapId();

        RefreshActorsFromBattleground(bg, record);
        return record;
    }

    std::vector<uint8> ToRawGuidBytes(uint64 raw)
    {
        std::vector<uint8> bytes(8);
        for (uint8 i = 0; i < 8; ++i)
            bytes[i] = uint8((raw >> (i * 8)) & 0xFF);
        return bytes;
    }

    std::vector<uint8> ToPackedGuidBytes(uint64 raw)
    {
        std::vector<uint8> out;
        out.reserve(9);

        uint8 mask = 0;
        std::vector<uint8> nonZero;
        nonZero.reserve(8);

        for (uint8 i = 0; i < 8; ++i)
        {
            uint8 b = uint8((raw >> (i * 8)) & 0xFF);
            if (b)
            {
                mask |= uint8(1 << i);
                nonZero.push_back(b);
            }
        }

        out.push_back(mask);
        out.insert(out.end(), nonZero.begin(), nonZero.end());
        return out;
    }

    void ReplaceAllBytes(std::vector<uint8>& payload, std::vector<uint8> const& from, std::vector<uint8> const& to)
    {
        if (from.empty())
            return;

        auto it = payload.begin();
        while (it != payload.end())
        {
            it = std::search(it, payload.end(), from.begin(), from.end());
            if (it == payload.end())
                break;

            size_t pos = size_t(std::distance(payload.begin(), it));
            payload.erase(payload.begin() + pos, payload.begin() + pos + from.size());
            payload.insert(payload.begin() + pos, to.begin(), to.end());
            it = payload.begin() + pos + to.size();
        }
    }

    void RewritePayloadGuids(std::vector<uint8>& payload, MatchRecord const& match)
    {
        for (ReplayActor const& actor : match.Actors)
        {
            uint64 original = actor.OriginalGuid.GetRawValue();
            uint64 fake = actor.FakeGuid.GetRawValue();

            if (!original || !fake || original == fake)
                continue;

            ReplaceAllBytes(payload, ToRawGuidBytes(original), ToRawGuidBytes(fake));
            ReplaceAllBytes(payload, ToPackedGuidBytes(original), ToPackedGuidBytes(fake));
        }
    }

    bool RewriteCompressedUpdateObjectPayload(std::vector<uint8>& payload, MatchRecord const& match)
    {
        if (payload.size() < 4)
            return false;

        uint32 uncompressedSize =
            uint32(payload[0]) |
            (uint32(payload[1]) << 8) |
            (uint32(payload[2]) << 16) |
            (uint32(payload[3]) << 24);

        if (!uncompressedSize || uncompressedSize > 16 * 1024 * 1024)
            return false;

        std::vector<uint8> decompressed(uncompressedSize);
        uLongf actualSize = uncompressedSize;

        int zResult = uncompress(decompressed.data(), &actualSize, payload.data() + 4, uLong(payload.size() - 4));
        if (zResult != Z_OK || actualSize != uncompressedSize)
        {
            TC_LOG_DEBUG("arena.replay", "Replay compressed update rewrite skipped: zlib result={} expected={} actual={} payload={}",
                zResult, uncompressedSize, uint32(actualSize), uint32(payload.size()));
            return false;
        }

        RewritePayloadGuids(decompressed, match);

        // GUID rewriting can change packed GUID length. The compressed update-object header must
        // therefore use the post-rewrite uncompressed size, not the original size from the DB row.
        uint32 const rewrittenUncompressedSize = uint32(decompressed.size());

        uLongf compressedBound = compressBound(uLong(decompressed.size()));
        std::vector<uint8> compressed(compressedBound);

        zResult = compress2(compressed.data(), &compressedBound, decompressed.data(), uLong(decompressed.size()), Z_BEST_SPEED);
        if (zResult != Z_OK)
        {
            TC_LOG_DEBUG("arena.replay", "Replay compressed update recompress failed: zlib result={}", zResult);
            return false;
        }

        compressed.resize(compressedBound);

        payload.clear();
        payload.reserve(4 + compressed.size());
        payload.push_back(uint8(rewrittenUncompressedSize & 0xFF));
        payload.push_back(uint8((rewrittenUncompressedSize >> 8) & 0xFF));
        payload.push_back(uint8((rewrittenUncompressedSize >> 16) & 0xFF));
        payload.push_back(uint8((rewrittenUncompressedSize >> 24) & 0xFF));
        payload.insert(payload.end(), compressed.begin(), compressed.end());
        return true;
    }


    size_t CountBytes(std::vector<uint8> const& payload, std::vector<uint8> const& needle)
    {
        if (needle.empty())
            return 0;

        size_t count = 0;
        auto it = payload.begin();
        while (it != payload.end())
        {
            it = std::search(it, payload.end(), needle.begin(), needle.end());
            if (it == payload.end())
                break;

            ++count;
            ++it;
        }

        return count;
    }

    size_t CountActorGuidHitsInPayload(std::vector<uint8> const& payload, MatchRecord const& match)
    {
        size_t hits = 0;
        for (ReplayActor const& actor : match.Actors)
        {
            uint64 original = actor.OriginalGuid.GetRawValue();
            if (!original)
                continue;

            hits += CountBytes(payload, ToRawGuidBytes(original));
            hits += CountBytes(payload, ToPackedGuidBytes(original));
        }

        return hits;
    }

    bool ExtractCompressedUpdatePayloadForAudit(WorldPacket const& packet, std::vector<uint8>& decompressed)
    {
        if (packet.size() < 4)
            return false;

        uint8 const* payload = packet.contents();
        uint32 uncompressedSize =
            uint32(payload[0]) |
            (uint32(payload[1]) << 8) |
            (uint32(payload[2]) << 16) |
            (uint32(payload[3]) << 24);

        if (!uncompressedSize || uncompressedSize > 16 * 1024 * 1024)
            return false;

        decompressed.assign(uncompressedSize, 0);
        uLongf actualSize = uncompressedSize;
        int zResult = uncompress(decompressed.data(), &actualSize, payload + 4, uLong(packet.size() - 4));
        if (zResult != Z_OK || actualSize != uncompressedSize)
        {
            decompressed.clear();
            return false;
        }

        return true;
    }

    struct ReplayAudit
    {
        uint32 UpdatePackets = 0;
        uint32 CompressedUpdatePackets = 0;
        uint32 ZeroTimeUpdatePackets = 0;
        uint32 ActorGuidHits = 0;
        uint32 ZeroTimeActorGuidHits = 0;
    };

    ReplayAudit BuildReplayAudit(MatchRecord const& match)
    {
        ReplayAudit audit;

        for (PacketRecord const& frame : match.Packets)
        {
            if (frame.Packet.GetOpcode() == SMSG_UPDATE_OBJECT)
            {
                ++audit.UpdatePackets;
                if (frame.TimestampMs == 0)
                    ++audit.ZeroTimeUpdatePackets;

                std::vector<uint8> payload(frame.Packet.size());
                if (!payload.empty())
                    std::memcpy(payload.data(), frame.Packet.contents(), payload.size());

                size_t hits = CountActorGuidHitsInPayload(payload, match);
                audit.ActorGuidHits += uint32(hits);
                if (frame.TimestampMs == 0)
                    audit.ZeroTimeActorGuidHits += uint32(hits);
            }
            else if (frame.Packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT)
            {
                ++audit.CompressedUpdatePackets;
                if (frame.TimestampMs == 0)
                    ++audit.ZeroTimeUpdatePackets;

                std::vector<uint8> payload;
                if (ExtractCompressedUpdatePayloadForAudit(frame.Packet, payload))
                {
                    size_t hits = CountActorGuidHitsInPayload(payload, match);
                    audit.ActorGuidHits += uint32(hits);
                    if (frame.TimestampMs == 0)
                        audit.ZeroTimeActorGuidHits += uint32(hits);
                }
            }
        }

        return audit;
    }

    WorldPacket BuildPlaybackPacket(PacketRecord const& frame, MatchRecord const& match)
    {
        std::vector<uint8> payload;
        payload.resize(frame.Packet.size());

        if (!payload.empty())
            std::memcpy(payload.data(), frame.Packet.contents(), payload.size());

        if (frame.Packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT)
            RewriteCompressedUpdateObjectPayload(payload, match);
        else
            RewritePayloadGuids(payload, match);

        WorldPacket out(frame.Packet.GetOpcode(), payload.size());
        if (!payload.empty())
            out.append(payload.data(), payload.size());

        return out;
    }

    void SendReplayNameResponse(WorldSession* session, ReplayActor const& actor)
    {
        if (!session)
            return;

        WorldPacket data(SMSG_NAME_QUERY_RESPONSE, 8 + 1 + actor.Name.size() + 1 + 1 + 1 + 1 + 1);
        data << actor.FakeGuid.WriteAsPacked();
        data << uint8(0);              // name known
        data << actor.Name;            // player name
        data << uint8(0);              // realm name
        data << uint8(actor.Race);
        data << uint8(actor.Gender);
        data << uint8(actor.Class);
        data << uint8(0);              // declined names disabled
        session->SendPacket(&data);
    }

    bool IsViewerReadyForReplay(Player const* viewer, PlaybackState const& state)
    {
        if (!viewer)
            return false;

        Battleground const* bg = viewer->GetBattleground();
        if (!bg)
            return false;

        if (bg->GetInstanceID() != state.BgInstanceId)
            return false;

        if (!bg->IsReplay())
            return false;

        if (!viewer->IsInWorld())
            return false;

        if (!viewer->GetMap())
            return false;

        if (viewer->GetMapId() != state.Match.MapId)
            return false;

        return true;
    }

    bool TrySkipReplayCountdown(Player* viewer, PlaybackState const& state)
    {
        if (!viewer || !viewer->GetSession())
            return false;

        Battleground* bg = viewer->GetBattleground();
        if (!bg || !bg->IsReplay() || bg->GetInstanceID() != state.BgInstanceId)
            return false;

        if (!bg->FindBgMap())
        {
            // Important: do not force-start before the replay map exists.
            return false;
        }

        if (!bg->GetPlayersSize())
            return false;

        if (bg->GetStatus() == STATUS_IN_PROGRESS)
        {
            bg->StartingEventOpenDoors();
            return true;
        }

        if (bg->GetStatus() != STATUS_WAIT_JOIN)
        {
            ChatHandler(viewer->GetSession()).PSendSysMessage("Replay warning: replay battleground is in unexpected status=%u before playback. Forcing doors open anyway.", uint32(bg->GetStatus()));
            bg->StartingEventOpenDoors();
            return false;
        }

        // Do not call SkipStartDelay() from the replay script. It performs normal arena start work and
        // can still race with setup on local branches. Replays are visual-only, so force the BG lifecycle
        // into IN_PROGRESS and open the doors.
        bg->StartingEventOpenDoors();
        bg->SetStartDelayTime(0);
        bg->SetStatus(STATUS_IN_PROGRESS);

        WorldPacket status;
        BattlegroundQueueTypeId bgQueueTypeId = sBattlegroundMgr->BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
        uint32 queueSlot = viewer->GetBattlegroundQueueIndex(bgQueueTypeId);
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&status, bg, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime(), bg->GetArenaType(), viewer->GetBGTeam());
        viewer->SendDirectMessage(&status);

        return true;
    }


    void SendInitialReplayNameResponses(Player* viewer, PlaybackState& state)
    {
        if (!viewer || !viewer->GetSession())
            return;

        for (ReplayActor const& actor : state.Match.Actors)
            SendReplayNameResponse(viewer->GetSession(), actor);

        state.SentInitialNameResponses = true;
    }

    bool TrySendFakeNameQueryResponse(WorldSession* session, ObjectGuid guid)
    {
        if (!session || !session->GetPlayer())
            return false;

        uint32 viewerLowGuid = session->GetPlayer()->GetGUID().GetCounter();
        auto active = ActiveReplays.find(viewerLowGuid);
        if (active == ActiveReplays.end())
            return false;

        for (ReplayActor const& actor : active->second.Match.Actors)
        {
            if (actor.FakeGuid == guid)
            {
                SendReplayNameResponse(session, actor);
                return true;
            }
        }

        return false;
    }

    void FinishPlaybackForViewer(uint32 viewerLowGuid, PlaybackState& state)
    {
        state.Finished = true;

        Player* viewer = ObjectAccessor::FindPlayerByLowGUID(viewerLowGuid);
        if (!viewer || !viewer->GetSession())
            return;

        ChatHandler(viewer->GetSession()).PSendSysMessage(
            "Replay packet stream finished. Staying in the replay instance so the final frame remains visible; leave the battleground when you're done.");
    }

    void SerializeMatchData(MatchRecord const& match, ByteBuffer& buffer)
    {
        buffer << uint32(ARENA_REPLAY_V2_MAGIC);
        buffer << uint32(ARENA_REPLAY_V2_VERSION);

        buffer << uint32(match.Actors.size());
        for (ReplayActor const& actor : match.Actors)
        {
            buffer << uint64(actor.OriginalGuid.GetRawValue());
            buffer << actor.Name;
            buffer << uint8(actor.Race);
            buffer << uint8(actor.Class);
            buffer << uint8(actor.Gender);
            buffer << uint32(actor.Team);
        }

        buffer << uint32(match.Packets.size());
        for (PacketRecord const& frame : match.Packets)
        {
            buffer << uint32(frame.Packet.size());
            buffer << uint32(frame.TimestampMs);
            buffer << uint16(frame.Packet.GetOpcode());

            if (frame.Packet.size() > 0)
                buffer.append(frame.Packet.contents(), frame.Packet.size());
        }
    }

    void DeserializeV1Frames(MatchRecord& record, ByteBuffer& buffer)
    {
        uint32 packetSize = 0;
        uint32 packetTimestamp = 0;
        uint16 opcode = 0;

        while (buffer.rpos() <= buffer.size() - 1)
        {
            if (buffer.size() - buffer.rpos() < 10)
                break;

            buffer >> packetSize;
            buffer >> packetTimestamp;
            buffer >> opcode;

            WorldPacket packet(opcode, packetSize);
            if (packetSize > 0)
            {
                if (buffer.size() - buffer.rpos() < packetSize)
                    break;

                std::vector<uint8> tmp(packetSize, 0);
                buffer.read(tmp.data(), packetSize);
                packet.append(tmp.data(), packetSize);
            }

            record.Packets.push_back({ packetTimestamp, packet });
        }
    }

    void DeserializeMatchData(MatchRecord& record, Field* fields)
    {
        record.ArenaTypeId = uint8(fields[1].GetUInt32());
        record.TypeId = BattlegroundTypeId(fields[2].GetUInt32());
        record.MapId = uint32(fields[5].GetUInt32());

        std::vector<uint8> data = fields[4].GetBinary();
        if (data.empty())
            return;

        ByteBuffer buffer;
        buffer.append(data.data(), data.size());

        if (buffer.size() < 4)
            return;

        uint32 magic = 0;
        buffer >> magic;

        if (magic != ARENA_REPLAY_V2_MAGIC)
        {
            buffer.rpos(0);
            DeserializeV1Frames(record, buffer);
            return;
        }

        uint32 version = 0;
        buffer >> version;

        if (version != ARENA_REPLAY_V2_VERSION)
        {
            TC_LOG_ERROR("arena.replay", "Unsupported arena replay version {}", version);
            return;
        }

        uint32 actorCount = 0;
        buffer >> actorCount;

        for (uint32 i = 0; i < actorCount; ++i)
        {
            uint64 originalRaw = 0;
            ReplayActor actor;

            buffer >> originalRaw;
            actor.OriginalGuid = ObjectGuid(originalRaw);
            buffer >> actor.Name;
            buffer >> actor.Race;
            buffer >> actor.Class;
            buffer >> actor.Gender;
            buffer >> actor.Team;

            record.Actors.push_back(actor);
        }

        uint32 packetCount = 0;
        buffer >> packetCount;

        for (uint32 i = 0; i < packetCount; ++i)
        {
            if (buffer.size() - buffer.rpos() < 10)
                break;

            uint32 packetSize = 0;
            uint32 packetTimestamp = 0;
            uint16 opcode = 0;

            buffer >> packetSize;
            buffer >> packetTimestamp;
            buffer >> opcode;

            WorldPacket packet(opcode, packetSize);
            if (packetSize > 0)
            {
                if (buffer.size() - buffer.rpos() < packetSize)
                    break;

                std::vector<uint8> tmp(packetSize, 0);
                buffer.read(tmp.data(), packetSize);
                packet.append(tmp.data(), packetSize);
            }

            record.Packets.push_back({ packetTimestamp, packet });
        }
    }

    std::vector<uint32> LoadLast10Replays()
    {
        std::vector<uint32> replayIds;

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_LAST_10_ARENA_REPLAYS);
        PreparedQueryResult result = CharacterDatabase.Query(stmt);
        if (!result)
            return replayIds;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                break;

            replayIds.push_back(fields[0].GetUInt32());
        }
        while (result->NextRow());

        return replayIds;
    }

    bool LoadReplayDataForPlayer(Player* player, uint32 matchId, MatchRecord& record)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ARENA_REPLAYS);
        stmt->setUInt32(0, matchId);

        PreparedQueryResult result = CharacterDatabase.Query(stmt);
        if (!result)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Replay data not found.");
            return false;
        }

        Field* fields = result->Fetch();
        if (!fields)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Replay data not found.");
            return false;
        }

        DeserializeMatchData(record, fields);
        if (record.Packets.empty())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Replay has no packets.");
            return false;
        }

        if (record.Actors.empty())
            ChatHandler(player->GetSession()).PSendSysMessage("Replay warning: no actor metadata found. Record a new arena after this patch for visible fake players.");

        AssignFakeGuids(record, player->GetGUID().GetCounter());

        ReplayAudit const audit = BuildReplayAudit(record);

        ChatHandler(player->GetSession()).PSendSysMessage("Replay loaded: packets=%u, actors=%u, firstTime=%u, lastTime=%u, firstOpcode=%u",
            uint32(record.Packets.size()), uint32(record.Actors.size()),
            record.Packets.empty() ? 0 : record.Packets.front().TimestampMs,
            record.Packets.empty() ? 0 : record.Packets.back().TimestampMs,
            record.Packets.empty() ? 0 : record.Packets.front().Packet.GetOpcode());
        ChatHandler(player->GetSession()).PSendSysMessage("Replay audit: update=%u compressedUpdate=%u zeroTimeUpdate=%u actorGuidHits=%u zeroTimeActorGuidHits=%u",
            audit.UpdatePackets, audit.CompressedUpdatePackets, audit.ZeroTimeUpdatePackets, audit.ActorGuidHits, audit.ZeroTimeActorGuidHits);
        return true;
    }

    bool StartReplay(Player* player, uint32 replayId)
    {
        if (!player || !player->GetSession())
            return false;

        ChatHandler handler(player->GetSession());

        MatchRecord record;
        if (!LoadReplayDataForPlayer(player, replayId, record))
            return false;

        Battleground* bg = sBattlegroundMgr->CreateNewBattleground(record.TypeId, GetBattlegroundBracketByLevel(record.MapId, 60), record.ArenaTypeId, false);
        if (!bg)
        {
            handler.PSendSysMessage("Couldn't create arena map!");
            handler.SetSentErrorMessage(true);
            return false;
        }

        uint32 viewerLowGuid = player->GetGUID().GetCounter();

        player->SetIsSpectator(true);
        bg->toggleReplay(player->GetGUID());
        player->SetPendingSpectatorForBG(bg->GetInstanceID());
        bg->StartBattleground();

        // Do NOT call SkipStartDelay() here.
        // On this core, SkipStartDelay() calls EndNow() if the battleground map is not created yet.
        // At this point the viewer has not zoned in, so FindBgMap() can still be null.
        // The WorldScript below skips the countdown only after the viewer is actually inside the replay map.

        BattlegroundTypeId bgTypeId = bg->GetTypeID();
        uint32 queueSlot = 0;
        TeamId teamId = TEAM_NEUTRAL;
        WorldPacket status;

        player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId, queueSlot, true, false, TEAM_NEUTRAL);
        sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bgTypeId);
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&status, bg, queueSlot, STATUS_IN_PROGRESS, 0, 0, bg->GetArenaType(), teamId);
        player->GetSession()->SendPacket(&status);

        PlaybackState state;
        state.Match = std::move(record);
        state.ViewerLowGuid = viewerLowGuid;
        state.BgInstanceId = bg->GetInstanceID();
        state.CreatedMs = getMSTime();

        ActiveReplays[viewerLowGuid] = std::move(state);

        handler.PSendSysMessage("Replay begins. Waiting for map load before force-opening doors and starting playback.");
        return true;
    }

    void SaveReplay(Battleground* bg)
    {
        if (!bg)
            return;

        auto recordItr = Records.find(bg->GetInstanceID());
        if (recordItr == Records.end())
            return;

        MatchRecord& match = recordItr->second;
        RefreshActorsFromBattleground(bg, match);

        if (match.Packets.empty())
        {
            Records.erase(recordItr);
            return;
        }

        ByteBuffer buffer;
        SerializeMatchData(match, buffer);

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ARENA_REPLAYS);
        stmt->setUInt32(0, uint32(match.ArenaTypeId));
        stmt->setUInt32(1, uint32(match.TypeId));
        stmt->setUInt32(2, buffer.size());
        stmt->setBinary(3, buffer.contentsAsVector());
        stmt->setUInt32(4, bg->GetMapId());
        CharacterDatabase.Execute(stmt);

        TC_LOG_INFO("arena.replay", "Saved arena replay instance={} map={} arenaType={} packets={} actors={} preStartPackets={} bytes={}",
            bg->GetInstanceID(), bg->GetMapId(), uint32(match.ArenaTypeId), uint32(match.Packets.size()), uint32(match.Actors.size()), match.PreStartPacketCount, uint32(buffer.size()));

        Records.erase(recordItr);
    }
}

class BGReplayServerScript : public ServerScript
{
public:
    BGReplayServerScript() : ServerScript("BGReplayServerScript") { }

    void OnPacketSend(WorldSession* session, WorldPacket& packet) override
    {
        if (!session || !session->GetPlayer())
            return;

        Player* player = session->GetPlayer();
        Battleground* bg = player->GetBattleground();

        if (!bg || bg->IsReplay())
            return;

        if (!bg->isArena())
            return;

        if (!ShouldRecordPacket(bg, packet))
            return;

        if (!IsTeamRecorder(bg, player))
            return;

        MatchRecord& record = GetOrCreateRecord(bg);
        uint32 nowMs = getMSTime();

        if (IsDuplicateRecentPacket(record, packet, nowMs))
            return;

        uint32 timestamp = 0;
        if (bg->GetStatus() == STATUS_WAIT_JOIN)
        {
            // Pre-start visual packets are needed to create players client-side, but the viewer
            // should not wait through arena prep. Send these immediately at replay start.
            timestamp = 0;
            ++record.PreStartPacketCount;
        }
        else
        {
            if (!record.InProgressStartMs)
            {
                record.InProgressStartMs = nowMs;
                TC_LOG_INFO("arena.replay", "Arena replay combat clock started instance={} packetsBeforeStart={}",
                    bg->GetInstanceID(), record.PreStartPacketCount);
            }

            timestamp = ARENA_REPLAY_PRELOAD_MS + (nowMs - record.InProgressStartMs);
        }

        record.Packets.push_back({ timestamp, WorldPacket(packet) });

        TC_LOG_DEBUG("arena.replay", "REC instance={} status={} opcode={} t={} delta={} size={} packets={} preStart={}",
            bg->GetInstanceID(), uint32(bg->GetStatus()), packet.GetOpcode(), timestamp,
            record.Packets.size() > 1 ? timestamp - record.Packets[record.Packets.size() - 2].TimestampMs : 0,
            uint32(packet.size()), uint32(record.Packets.size()), record.PreStartPacketCount);
    }

    void OnPacketReceive(WorldSession* session, WorldPacket& packet) override
    {
        if (!session || !session->GetPlayer())
            return;

        if (packet.GetOpcode() != CMSG_NAME_QUERY)
            return;

        WorldPacket copy(packet);
        copy.rpos(0);

        ObjectGuid guid;
        copy >> guid;

        // Cannot consume the packet here because ServerScript receives a copy.
        // Sending the known response early still prevents the fake replay actors from staying "Unknown" in practice.
        TrySendFakeNameQueryResponse(session, guid);
    }
};

    bool UpdatePlaybackForViewer(Player* viewer)
    {
        if (!viewer || !viewer->GetSession())
            return false;

        uint32 viewerLowGuid = viewer->GetGUID().GetCounter();
        auto activeItr = ActiveReplays.find(viewerLowGuid);
        if (activeItr == ActiveReplays.end())
            return false;

        PlaybackState& state = activeItr->second;
        uint32 nowMs = getMSTime();

        if (state.Finished)
        {
            Battleground* currentBg = viewer->GetBattleground();
            if (!currentBg || !currentBg->IsReplay() || currentBg->GetInstanceID() != state.BgInstanceId)
                ActiveReplays.erase(activeItr);

            return true;
        }

        if (!IsViewerReadyForReplay(viewer, state))
        {
            if (nowMs - state.CreatedMs > ARENA_REPLAY_LOAD_GRACE_MS)
            {
                ChatHandler(viewer->GetSession()).PSendSysMessage("Replay failed to start: viewer never entered the replay map. bg=%u map=%u expectedMap=%u inWorld=%u",
                    viewer->GetBattleground() ? viewer->GetBattleground()->GetInstanceID() : 0,
                    viewer->GetMapId(), state.Match.MapId, viewer->IsInWorld() ? 1u : 0u);
                ActiveReplays.erase(activeItr);
                return true;
            }

            return true;
        }

        if (!state.PlaybackClockStarted)
        {
            bool skippedCountdown = TrySkipReplayCountdown(viewer, state);

            state.PlaybackStartMs = nowMs + ARENA_REPLAY_START_DELAY_MS;
            state.PlaybackClockStarted = true;

            TC_LOG_INFO("arena.replay", "Replay playback armed viewer={} bg={} map={} packets={} actors={} startDelay={} skippedCountdown={}",
                viewerLowGuid, state.BgInstanceId, state.Match.MapId,
                uint32(state.Match.Packets.size()), uint32(state.Match.Actors.size()), ARENA_REPLAY_START_DELAY_MS, skippedCountdown ? 1u : 0u);

            ChatHandler(viewer->GetSession()).PSendSysMessage("Replay playback armed: packets=%u, actors=%u, durationMs=%u, forcedStart=%u. Playback starts in %u ms.",
                uint32(state.Match.Packets.size()), uint32(state.Match.Actors.size()),
                state.Match.Packets.empty() ? 0 : state.Match.Packets.back().TimestampMs,
                skippedCountdown ? 1u : 0u, ARENA_REPLAY_START_DELAY_MS);
        }

        if (!state.SentInitialNameResponses)
            SendInitialReplayNameResponses(viewer, state);

        if (nowMs < state.PlaybackStartMs)
            return true;

        uint32 elapsedMs = nowMs - state.PlaybackStartMs;
        uint32 sentThisUpdate = 0;

        while (state.Cursor < state.Match.Packets.size()
            && state.Match.Packets[state.Cursor].TimestampMs <= elapsedMs
            && sentThisUpdate < ARENA_REPLAY_SEND_CAP_PER_UPDATE)
        {
            PacketRecord const& frame = state.Match.Packets[state.Cursor];
            WorldPacket out = BuildPlaybackPacket(frame, state.Match);
            viewer->GetSession()->SendPacket(&out);

            TC_LOG_DEBUG("arena.replay", "PLAY viewer={} opcode={} due={} now={} late={} cursor={}/{} size={}",
                viewerLowGuid, out.GetOpcode(), frame.TimestampMs, elapsedMs,
                elapsedMs >= frame.TimestampMs ? elapsedMs - frame.TimestampMs : 0,
                uint32(state.Cursor), uint32(state.Match.Packets.size()), uint32(out.size()));

            ++state.Cursor;
            ++sentThisUpdate;
        }

        if (sentThisUpdate > 0 && (state.Cursor == sentThisUpdate || (state.Cursor % 100) < sentThisUpdate))
        {
            ChatHandler(viewer->GetSession()).PSendSysMessage("Replay sent packets: cursor=%u/%u elapsedMs=%u",
                uint32(state.Cursor), uint32(state.Match.Packets.size()), elapsedMs);
        }

        if (sentThisUpdate >= ARENA_REPLAY_SEND_CAP_PER_UPDATE)
        {
            TC_LOG_DEBUG("arena.replay", "PLAY throttle viewer={} elapsed={} cursor={}/{}",
                viewerLowGuid, elapsedMs, uint32(state.Cursor), uint32(state.Match.Packets.size()));
        }

        if (state.Cursor >= state.Match.Packets.size())
            FinishPlaybackForViewer(viewerLowGuid, state);

        return true;
    }

class BGReplayPlayerScript : public PlayerScript
{
public:
    BGReplayPlayerScript() : PlayerScript("BGReplayPlayerScript") { }

    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        UpdatePlaybackForViewer(player);
    }

    void OnMapChanged(Player* player) override
    {
        if (UpdatePlaybackForViewer(player))
            if (player && player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage("Replay map-change detected; arming playback when the arena map is ready.");
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        ActiveReplays.erase(player->GetGUID().GetCounter());
    }
};

class BGReplayWorldScript : public WorldScript
{
public:
    BGReplayWorldScript() : WorldScript("BGReplayWorldScript") { }

    void OnUpdate(uint32 /*diff*/) override
    {
        // V6 intentionally uses PlayerScript::OnUpdate for playback. WorldScript is left registered only
        // so existing script registration stays harmless if another local patch expects this script name.
    }
};

class BGReplayBGScript : public BattlegroundScript
{
public:
    BGReplayBGScript() : BattlegroundScript("BGReplayBGScript") { }

    void OnBattlegroundEnd(Battleground* bg, uint32 /*winner*/) override
    {
        if (!bg || !bg->isArena())
            return;

        if (!bg->IsReplay())
            SaveReplay(bg);
    }

    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
    {
        // Playback is intentionally not done here anymore.
        // Do not erase ActiveReplays from this hook. During replay creation the viewer can still be zoning/loading
        // while bg->GetPlayers() is temporarily empty, and erasing here makes the replay start with no packets visible.
        if (!bg || !bg->isArena() || !bg->IsReplay())
            return;

        // Do not call SkipStartDelay() here. On replay creation this hook can run before the viewer has
        // finished zoning into the map; calling SkipStartDelay() that early can call EndNow() and kick the viewer.
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
            std::vector<uint32> matchIds = LoadLast10Replays();
            for (uint32 matchId : matchIds)
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay match " + std::to_string(matchId), GOSSIP_SENDER_MAIN, matchId);

            SendGossipMenuFor(player, 1775757, me->GetGUID());
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 replayId = GetGossipActionFor(player, gossipListId);
            player->PlayerTalkClass->ClearMenus();

            StartReplay(player, replayId);

            CloseGossipMenuFor(player);
            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new replayAI(creature);
    }
};

void AddBGReplayScripts()
{
    new BGReplayServerScript();
    new BGReplayWorldScript();
    new BGReplayPlayerScript();
    new BGReplayBGScript();
    new ReplayGossip();
}
