//
// Arena Replay V15 replacement for the old BGReplay.cpp.
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
// - Playback rewrites update-object packets structurally instead of doing unsafe global byte replacement.
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

#include "ArenaTeamMgr.h"
#include "Base32.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "UpdateFields.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <sstream>
#include <iomanip>
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
        bool SentReplayASInitial = false;
        bool Finished = false;
        uint32 LastNameColorUpdateMs = 0;
        uint8 NameColorUpdateBursts = 0;
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

    uint8 PackedGuidMask(uint64 raw)
    {
        uint8 mask = 0;
        for (uint8 i = 0; i < 8; ++i)
        {
            if (uint8((raw >> (i * 8)) & 0xFF))
                mask |= uint8(1 << i);
        }

        return mask;
    }

    uint32 MakeMaskStableFakePlayerCounter(uint32 originalCounter, uint32 salt)
    {
        // This is intentionally NOT a huge fake counter like 0xF0000000.
        //
        // V7 could crash the client because rewriting a packed player GUID could change the packed GUID length
        // inside SMSG_COMPRESSED_UPDATE_OBJECT create blocks. That shifts the update-object byte stream and the
        // 3.3.5 client can explode while parsing it.
        //
        // So the fake player low-guid must preserve the exact nonzero-byte mask of the original low-guid.
        // Example:
        //   original low 0x0000004D -> fake low must be 0x000000XX
        //   original low 0x0000124D -> fake low must be 0x0000XXYY
        //
        // Player high-guid is zero on this branch, so preserving the low-guid byte mask preserves the full packed
        // GUID mask/length for player GUIDs.
        uint32 counter = 0;

        for (uint8 i = 0; i < 4; ++i)
        {
            uint8 originalByte = uint8((originalCounter >> (i * 8)) & 0xFF);
            if (!originalByte)
                continue;

            uint8 candidate = uint8((uint32(originalByte) + 37u + salt * 53u + i * 29u) & 0xFFu);
            if (!candidate)
                candidate = uint8(1u + ((salt + i * 17u) % 255u));

            counter |= uint32(candidate) << (i * 8);
        }

        return counter;
    }

    void AssignFakeGuids(MatchRecord& match, uint32 viewerLowGuid)
    {
        std::unordered_set<uint32> usedCounters;

        for (ReplayActor const& actor : match.Actors)
        {
            if (actor.OriginalGuid.IsPlayer())
                usedCounters.insert(actor.OriginalGuid.GetCounter());
        }

        for (size_t i = 0; i < match.Actors.size(); ++i)
        {
            ReplayActor& actor = match.Actors[i];

            uint32 originalCounter = actor.OriginalGuid.GetCounter();
            uint8 originalMask = PackedGuidMask(actor.OriginalGuid.GetRawValue());

            uint32 chosenCounter = 0;
            for (uint32 attempt = 0; attempt < 2048; ++attempt)
            {
                uint32 salt = uint32(i + 1) * 97u + attempt + (viewerLowGuid & 0xFFu);
                uint32 candidate = MakeMaskStableFakePlayerCounter(originalCounter, salt);

                if (!candidate)
                    continue;

                if (usedCounters.find(candidate) != usedCounters.end())
                    continue;

                ObjectGuid fake = ObjectGuid::Create<HighGuid::Player>(candidate);
                if (fake == actor.OriginalGuid)
                    continue;

                if (PackedGuidMask(fake.GetRawValue()) != originalMask)
                    continue;

                chosenCounter = candidate;
                break;
            }

            if (!chosenCounter)
            {
                // Last resort: keep the original GUID rather than creating a packet with a different packed size.
                // This can make self-replay imperfect, but it is safer than crashing the client.
                actor.FakeGuid = actor.OriginalGuid;
                TC_LOG_ERROR("arena.replay", "Replay fake GUID fallback used for original={} mask={}",
                    actor.OriginalGuid.GetRawValue(), uint32(originalMask));
                continue;
            }

            usedCounters.insert(chosenCounter);
            actor.FakeGuid = ObjectGuid::Create<HighGuid::Player>(chosenCounter);

            TC_LOG_INFO("arena.replay", "Replay fake GUID map original={} fake={} originalMask={} fakeMask={}",
                actor.OriginalGuid.GetRawValue(), actor.FakeGuid.GetRawValue(),
                uint32(originalMask), uint32(PackedGuidMask(actor.FakeGuid.GetRawValue())));
        }
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

        // Never do length-changing replacements inside replay packets.
        // SMSG_UPDATE_OBJECT and movement payloads are structured binary streams; shifting bytes corrupts the
        // remainder of the packet and can crash the 3.3.5 client.
        if (from.size() != to.size())
        {
            TC_LOG_ERROR("arena.replay", "Skipped unsafe replay GUID rewrite: fromSize={} toSize={}",
                uint32(from.size()), uint32(to.size()));
            return;
        }

        auto it = payload.begin();
        while (it != payload.end())
        {
            it = std::search(it, payload.end(), from.begin(), from.end());
            if (it == payload.end())
                break;

            std::copy(to.begin(), to.end(), it);
            it += to.size();
        }
    }

    ReplayActor const* FindReplayActorByGuid(MatchRecord const& match, ObjectGuid guid)
    {
        if (!guid)
            return nullptr;

        for (ReplayActor const& actor : match.Actors)
        {
            if (actor.OriginalGuid == guid || actor.FakeGuid == guid)
                return &actor;
        }

        return nullptr;
    }

    ReplayActor const* FindReplayActorByOriginalGuid(MatchRecord const& match, ObjectGuid guid)
    {
        if (!guid)
            return nullptr;

        for (ReplayActor const& actor : match.Actors)
        {
            if (actor.OriginalGuid == guid)
                return &actor;
        }

        return nullptr;
    }

    bool IsReplayActorGuid(MatchRecord const& match, ObjectGuid guid)
    {
        return FindReplayActorByGuid(match, guid) != nullptr;
    }

    bool HasRemaining(std::vector<uint8> const& payload, size_t pos, size_t count)
    {
        return pos <= payload.size() && count <= payload.size() - pos;
    }

    bool ReadUInt8(std::vector<uint8> const& payload, size_t& pos, uint8& value)
    {
        if (!HasRemaining(payload, pos, 1))
            return false;

        value = payload[pos++];
        return true;
    }

    bool ReadUInt16(std::vector<uint8> const& payload, size_t& pos, uint16& value)
    {
        if (!HasRemaining(payload, pos, 2))
            return false;

        value = uint16(payload[pos]) | (uint16(payload[pos + 1]) << 8);
        pos += 2;
        return true;
    }

    bool ReadUInt32(std::vector<uint8> const& payload, size_t& pos, uint32& value)
    {
        if (!HasRemaining(payload, pos, 4))
            return false;

        value = uint32(payload[pos]) |
            (uint32(payload[pos + 1]) << 8) |
            (uint32(payload[pos + 2]) << 16) |
            (uint32(payload[pos + 3]) << 24);
        pos += 4;
        return true;
    }

    void WriteUInt16(std::vector<uint8>& payload, size_t pos, uint16 value)
    {
        if (!HasRemaining(payload, pos, 2))
            return;

        payload[pos] = uint8(value & 0xFF);
        payload[pos + 1] = uint8((value >> 8) & 0xFF);
    }

    void WriteUInt32(std::vector<uint8>& payload, size_t pos, uint32 value)
    {
        if (!HasRemaining(payload, pos, 4))
            return;

        payload[pos] = uint8(value & 0xFF);
        payload[pos + 1] = uint8((value >> 8) & 0xFF);
        payload[pos + 2] = uint8((value >> 16) & 0xFF);
        payload[pos + 3] = uint8((value >> 24) & 0xFF);
    }

    bool ReadUInt64At(std::vector<uint8> const& payload, size_t pos, uint64& value)
    {
        if (!HasRemaining(payload, pos, 8))
            return false;

        value = 0;
        for (uint8 i = 0; i < 8; ++i)
            value |= uint64(payload[pos + i]) << (i * 8);

        return true;
    }

    void WriteUInt64(std::vector<uint8>& payload, size_t pos, uint64 value)
    {
        if (!HasRemaining(payload, pos, 8))
            return;

        for (uint8 i = 0; i < 8; ++i)
            payload[pos + i] = uint8((value >> (i * 8)) & 0xFF);
    }

    uint32 CountSetBits(uint32 value)
    {
        uint32 count = 0;
        while (value)
        {
            value &= value - 1;
            ++count;
        }

        return count;
    }

    bool ReadPackedGuid(std::vector<uint8> const& payload, size_t& pos, ObjectGuid& guid)
    {
        uint8 mask = 0;
        if (!ReadUInt8(payload, pos, mask))
            return false;

        uint64 raw = 0;
        for (uint8 i = 0; i < 8; ++i)
        {
            if (!(mask & (1 << i)))
                continue;

            uint8 byte = 0;
            if (!ReadUInt8(payload, pos, byte))
                return false;

            raw |= uint64(byte) << (i * 8);
        }

        guid = ObjectGuid(raw);
        return true;
    }

    bool SkipPackedGuid(std::vector<uint8> const& payload, size_t& pos)
    {
        ObjectGuid ignored;
        return ReadPackedGuid(payload, pos, ignored);
    }

    bool WritePackedGuidInPlace(std::vector<uint8>& payload, size_t start, size_t end, ObjectGuid guid)
    {
        std::vector<uint8> packed = ToPackedGuidBytes(guid.GetRawValue());
        if (packed.size() != end - start)
        {
            TC_LOG_ERROR("arena.replay", "Replay packed GUID rewrite refused: oldSize={} newSize={} guid={}",
                uint32(end - start), uint32(packed.size()), guid.GetRawValue());
            return false;
        }

        if (!HasRemaining(payload, start, packed.size()))
            return false;

        std::copy(packed.begin(), packed.end(), payload.begin() + start);
        return true;
    }

    bool RewriteFirstPackedGuidIfReplayActor(std::vector<uint8>& payload, MatchRecord const& match)
    {
        size_t pos = 0;
        size_t guidStart = pos;

        ObjectGuid guid;
        if (!ReadPackedGuid(payload, pos, guid))
            return false;

        size_t guidEnd = pos;
        if (ReplayActor const* actor = FindReplayActorByOriginalGuid(match, guid))
            return WritePackedGuidInPlace(payload, guidStart, guidEnd, actor->FakeGuid);

        return false;
    }

    bool RewriteFirstRawGuidIfReplayActor(std::vector<uint8>& payload, MatchRecord const& match)
    {
        uint64 raw = 0;
        if (!ReadUInt64At(payload, 0, raw))
            return false;

        ObjectGuid guid(raw);
        if (ReplayActor const* actor = FindReplayActorByOriginalGuid(match, guid))
        {
            WriteUInt64(payload, 0, actor->FakeGuid.GetRawValue());
            return true;
        }

        return false;
    }

    bool IsMovementLikeOpcode(uint16 opcode)
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
            case SMSG_FORCE_RUN_SPEED_CHANGE:
            case SMSG_FORCE_FLIGHT_SPEED_CHANGE:
            case SMSG_FORCE_SWIM_SPEED_CHANGE:
            case SMSG_MONSTER_MOVE:
                return true;
            default:
                return false;
        }
    }

    bool RewritePackedGuidAt(std::vector<uint8>& payload, size_t& pos, MatchRecord const& match)
    {
        size_t guidStart = pos;

        ObjectGuid guid;
        if (!ReadPackedGuid(payload, pos, guid))
            return false;

        size_t guidEnd = pos;
        if (ReplayActor const* actor = FindReplayActorByOriginalGuid(match, guid))
            WritePackedGuidInPlace(payload, guidStart, guidEnd, actor->FakeGuid);

        return true;
    }

    bool RewriteRawGuidAt(std::vector<uint8>& payload, size_t pos, MatchRecord const& match)
    {
        uint64 raw = 0;
        if (!ReadUInt64At(payload, pos, raw))
            return false;

        ObjectGuid guid(raw);
        if (ReplayActor const* actor = FindReplayActorByOriginalGuid(match, guid))
            WriteUInt64(payload, pos, actor->FakeGuid.GetRawValue());

        return true;
    }

    bool RewriteRawGuidAndAdvance(std::vector<uint8>& payload, size_t& pos, MatchRecord const& match)
    {
        if (!HasRemaining(payload, pos, 8))
            return false;

        RewriteRawGuidAt(payload, pos, match);
        pos += 8;
        return true;
    }

    bool SkipReplayCString(std::vector<uint8> const& payload, size_t& pos)
    {
        while (pos < payload.size())
        {
            uint8 ch = payload[pos++];
            if (!ch)
                return true;
        }

        return false;
    }

    bool RewriteSpellTargetDataGuids(std::vector<uint8>& payload, size_t& pos, MatchRecord const& match)
    {
        constexpr uint32 REPLAY_TARGET_FLAG_UNIT            = 0x00000002;
        constexpr uint32 REPLAY_TARGET_FLAG_ITEM            = 0x00000010;
        constexpr uint32 REPLAY_TARGET_FLAG_SOURCE_LOCATION = 0x00000020;
        constexpr uint32 REPLAY_TARGET_FLAG_DEST_LOCATION   = 0x00000040;
        constexpr uint32 REPLAY_TARGET_FLAG_CORPSE_ENEMY    = 0x00000200;
        constexpr uint32 REPLAY_TARGET_FLAG_GAMEOBJECT      = 0x00000800;
        constexpr uint32 REPLAY_TARGET_FLAG_TRADE_ITEM      = 0x00001000;
        constexpr uint32 REPLAY_TARGET_FLAG_STRING          = 0x00002000;
        constexpr uint32 REPLAY_TARGET_FLAG_CORPSE_ALLY     = 0x00008000;
        constexpr uint32 REPLAY_TARGET_FLAG_UNIT_MINIPET    = 0x00010000;
        constexpr uint32 REPLAY_TARGET_FLAG_DEST_TARGET     = 0x00040000;

        uint32 targetFlags = 0;
        if (!ReadUInt32(payload, pos, targetFlags))
            return false;

        // Matches SpellCastTargets::Write() / SpellTargetData serialization in this source.
        if (targetFlags & (REPLAY_TARGET_FLAG_UNIT | REPLAY_TARGET_FLAG_CORPSE_ALLY | REPLAY_TARGET_FLAG_GAMEOBJECT |
                           REPLAY_TARGET_FLAG_CORPSE_ENEMY | REPLAY_TARGET_FLAG_UNIT_MINIPET))
        {
            if (!RewritePackedGuidAt(payload, pos, match))
                return false;
        }

        if (targetFlags & (REPLAY_TARGET_FLAG_ITEM | REPLAY_TARGET_FLAG_TRADE_ITEM))
        {
            if (!RewritePackedGuidAt(payload, pos, match))
                return false;
        }

        if (targetFlags & REPLAY_TARGET_FLAG_SOURCE_LOCATION)
        {
            if (!RewritePackedGuidAt(payload, pos, match))
                return false;

            if (!HasRemaining(payload, pos, 3 * 4))
                return false;

            pos += 3 * 4;
        }

        if (targetFlags & REPLAY_TARGET_FLAG_DEST_LOCATION)
        {
            if (!RewritePackedGuidAt(payload, pos, match))
                return false;

            if (!HasRemaining(payload, pos, 3 * 4))
                return false;

            pos += 3 * 4;
        }

        if (targetFlags & REPLAY_TARGET_FLAG_STRING)
        {
            if (!SkipReplayCString(payload, pos))
                return false;
        }

        // This flag is followed by one uint8 at the very end of SpellCastData in this source.
        // No GUID lives there, so do not consume it here; SpellCastData appends it after cast-flag optional data.
        (void)REPLAY_TARGET_FLAG_DEST_TARGET;

        return true;
    }

    bool RewriteSpellCastDataGuids(std::vector<uint8>& payload, MatchRecord const& match, bool hasGoTargets)
    {
        constexpr uint8 REPLAY_SPELL_MISS_REFLECT = 11;

        std::vector<uint8> original = payload;
        size_t pos = 0;

        if (!RewritePackedGuidAt(payload, pos, match)) // CasterGUID; can be item guid
        {
            payload.swap(original);
            return false;
        }

        if (!RewritePackedGuidAt(payload, pos, match)) // CasterUnit; this is the actor guid
        {
            payload.swap(original);
            return false;
        }

        // CastID + SpellID + CastFlags + CastTime
        if (!HasRemaining(payload, pos, 1 + 4 + 4 + 4))
        {
            payload.swap(original);
            return false;
        }

        pos += 1 + 4 + 4 + 4;

        if (hasGoTargets)
        {
            uint8 hitCount = 0;
            if (!ReadUInt8(payload, pos, hitCount))
            {
                payload.swap(original);
                return false;
            }

            for (uint8 i = 0; i < hitCount; ++i)
            {
                if (!RewriteRawGuidAndAdvance(payload, pos, match))
                {
                    payload.swap(original);
                    return false;
                }
            }

            uint8 missCount = 0;
            if (!ReadUInt8(payload, pos, missCount))
            {
                payload.swap(original);
                return false;
            }

            for (uint8 i = 0; i < missCount; ++i)
            {
                if (!RewriteRawGuidAndAdvance(payload, pos, match))
                {
                    payload.swap(original);
                    return false;
                }

                uint8 missReason = 0;
                if (!ReadUInt8(payload, pos, missReason))
                {
                    payload.swap(original);
                    return false;
                }

                if (missReason == REPLAY_SPELL_MISS_REFLECT)
                {
                    if (!HasRemaining(payload, pos, 1))
                    {
                        payload.swap(original);
                        return false;
                    }

                    pos += 1;
                }
            }
        }

        if (!RewriteSpellTargetDataGuids(payload, pos, match))
        {
            payload.swap(original);
            return false;
        }

        return true;
    }

    void RewriteTwoPackedGuids(std::vector<uint8>& payload, MatchRecord const& match)
    {
        std::vector<uint8> original = payload;
        size_t pos = 0;

        if (!RewritePackedGuidAt(payload, pos, match) || !RewritePackedGuidAt(payload, pos, match))
            payload.swap(original);
    }

    void RewritePacketPackedGuidAfterUInt32(std::vector<uint8>& payload, MatchRecord const& match)
    {
        std::vector<uint8> original = payload;
        size_t pos = 4;

        if (!RewritePackedGuidAt(payload, pos, match))
            payload.swap(original);
    }

    void RewriteRawGuidPair(std::vector<uint8>& payload, MatchRecord const& match)
    {
        RewriteRawGuidAt(payload, 0, match);
        RewriteRawGuidAt(payload, 8, match);
    }

    void RewriteNonUpdatePacketGuids(uint16 opcode, std::vector<uint8>& payload, MatchRecord const& match)
    {
        // V10 and earlier used global byte replacement for both raw and packed player GUIDs.
        // That is unsafe for low player GUIDs, for example raw 0x4D is:
        //   4D 00 00 00 00 00 00 00
        // which can appear in update masks, field values, timestamps, or padding.
        //
        // V12 still avoids global replacement, but now rewrites the understood spell/combat packet layouts too.
        if (IsMovementLikeOpcode(opcode))
        {
            RewriteFirstPackedGuidIfReplayActor(payload, match);
            return;
        }

        switch (opcode)
        {
            case SMSG_SPELL_START:
                RewriteSpellCastDataGuids(payload, match, false);
                return;
            case SMSG_SPELL_GO:
                RewriteSpellCastDataGuids(payload, match, true);
                return;
            case SMSG_ATTACK_START:
                RewriteRawGuidPair(payload, match);
                return;
            case SMSG_ATTACK_STOP:
                RewriteTwoPackedGuids(payload, match);
                return;
            case SMSG_ATTACKERSTATEUPDATE:
                RewritePacketPackedGuidAfterUInt32(payload, match); // attacker
                {
                    std::vector<uint8> original = payload;
                    size_t pos = 4;
                    if (RewritePackedGuidAt(payload, pos, match) && RewritePackedGuidAt(payload, pos, match))
                        return;

                    payload.swap(original);
                }
                return;
            case SMSG_SPELLNONMELEEDAMAGELOG:
            case SMSG_PERIODICAURALOG:
            case SMSG_SPELLHEALLOG:
            case SMSG_SPELLENERGIZELOG:
                RewriteTwoPackedGuids(payload, match);
                return;
            case SMSG_SPELLDAMAGESHIELD:
                RewriteRawGuidPair(payload, match);
                return;
            case SMSG_SPELLLOGEXECUTE:
            case SMSG_SPELL_DELAYED:
            case SMSG_POWER_UPDATE:
            case SMSG_CANCEL_AUTO_REPEAT:
            case SMSG_AURA_UPDATE:
            case SMSG_AURA_UPDATE_ALL:
                RewriteFirstPackedGuidIfReplayActor(payload, match);
                return;
            case SMSG_EMOTE:
                RewriteRawGuidAt(payload, 4, match);
                return;
            case SMSG_AI_REACTION:
            case SMSG_DESTROY_OBJECT:
            case SMSG_ARENA_UNIT_DESTROYED:
                RewriteFirstRawGuidIfReplayActor(payload, match);
                return;
            default:
                return;
        }
    }

    bool PatchUpdateValuesBlock(std::vector<uint8>& payload, size_t& pos, MatchRecord const& match, ObjectGuid blockGuid)
    {
        uint8 blockCount = 0;
        if (!ReadUInt8(payload, pos, blockCount))
            return false;

        std::vector<uint32> masks;
        masks.reserve(blockCount);

        for (uint8 i = 0; i < blockCount; ++i)
        {
            uint32 mask = 0;
            if (!ReadUInt32(payload, pos, mask))
                return false;

            masks.push_back(mask);
        }

        ReplayActor const* actor = FindReplayActorByOriginalGuid(match, blockGuid);
        uint64 fakeRaw = actor ? actor->FakeGuid.GetRawValue() : 0;
        uint32 fakeLow = uint32(fakeRaw & 0xFFFFFFFFu);
        uint32 fakeHigh = uint32((fakeRaw >> 32) & 0xFFFFFFFFu);

        // This is the only new client-packet target rewrite in V23.
        //
        // The previous v20 attempt rewrote multiple owner/channel fields and crashed the client.
        // V23 only rewrites UNIT_FIELD_TARGET, using the generated UpdateFields.h value from this branch.
        // In src(85), UNIT_FIELD_TARGET is OBJECT_END + 0x000C = 0x0012, size 2 LONG fields.
        size_t targetLowPos = std::numeric_limits<size_t>::max();
        size_t targetHighPos = std::numeric_limits<size_t>::max();
        uint32 targetLow = 0;
        uint32 targetHigh = 0;

        for (uint32 block = 0; block < masks.size(); ++block)
        {
            uint32 mask = masks[block];

            for (uint8 bit = 0; bit < 32; ++bit)
            {
                if (!(mask & (uint32(1) << bit)))
                    continue;

                if (!HasRemaining(payload, pos, 4))
                    return false;

                uint32 fieldIndex = block * 32 + bit;
                uint32 value =
                    uint32(payload[pos]) |
                    (uint32(payload[pos + 1]) << 8) |
                    (uint32(payload[pos + 2]) << 16) |
                    (uint32(payload[pos + 3]) << 24);

                // Patch the replay actor's own OBJECT_FIELD_GUID, same as the last known-good versions.
                if (actor)
                {
                    if (fieldIndex == OBJECT_FIELD_GUID)
                        WriteUInt32(payload, pos, fakeLow);
                    else if (fieldIndex == OBJECT_FIELD_GUID + 1)
                        WriteUInt32(payload, pos, fakeHigh);

                    // Record target pair positions, but don't write until both low/high halves are known.
                    // This avoids accidentally rewriting a half-present GUID field.
                    if (fieldIndex == UNIT_FIELD_TARGET)
                    {
                        targetLowPos = pos;
                        targetLow = value;
                    }
                    else if (fieldIndex == UNIT_FIELD_TARGET + 1)
                    {
                        targetHighPos = pos;
                        targetHigh = value;
                    }
                }

                pos += 4;
            }
        }

        if (actor
            && targetLowPos != std::numeric_limits<size_t>::max()
            && targetHighPos != std::numeric_limits<size_t>::max())
        {
            ObjectGuid originalTarget(uint64(targetLow) | (uint64(targetHigh) << 32));
            if (ReplayActor const* targetActor = FindReplayActorByOriginalGuid(match, originalTarget))
            {
                uint64 targetFakeRaw = targetActor->FakeGuid.GetRawValue();
                WriteUInt32(payload, targetLowPos, uint32(targetFakeRaw & 0xFFFFFFFFu));
                WriteUInt32(payload, targetHighPos, uint32((targetFakeRaw >> 32) & 0xFFFFFFFFu));
            }
        }

        return true;
    }

    bool SkipMovementCreateData(std::vector<uint8>& payload, size_t& pos, MatchRecord const& match, ObjectGuid blockGuid, uint8 objectTypeId = 0xFF)
    {
        // Mirrored from Object::BuildMovementUpdate().
        // This is intentionally conservative: if the layout looks unsafe, return false and leave the packet alone
        // rather than shifting bytes or making the client parse garbage.
        constexpr uint8 REPLAY_TYPEID_PLAYER = 4;

        constexpr uint16 REPLAY_UPDATEFLAG_SELF = 0x0001;
        constexpr uint16 REPLAY_UPDATEFLAG_TRANSPORT = 0x0002;
        constexpr uint16 REPLAY_UPDATEFLAG_HAS_TARGET = 0x0004;
        constexpr uint16 REPLAY_UPDATEFLAG_UNKNOWN = 0x0008;
        constexpr uint16 REPLAY_UPDATEFLAG_LOWGUID = 0x0010;
        constexpr uint16 REPLAY_UPDATEFLAG_LIVING = 0x0020;
        constexpr uint16 REPLAY_UPDATEFLAG_STATIONARY_POSITION = 0x0040;
        constexpr uint16 REPLAY_UPDATEFLAG_VEHICLE = 0x0080;
        constexpr uint16 REPLAY_UPDATEFLAG_POSITION = 0x0100;
        constexpr uint16 REPLAY_UPDATEFLAG_ROTATION = 0x0200;

        constexpr uint32 REPLAY_MOVEMENTFLAG_ONTRANSPORT = 0x00000200;
        constexpr uint32 REPLAY_MOVEMENTFLAG_FALLING = 0x00001000;
        constexpr uint32 REPLAY_MOVEMENTFLAG_SWIMMING = 0x00200000;
        constexpr uint32 REPLAY_MOVEMENTFLAG_FLYING = 0x02000000;
        constexpr uint32 REPLAY_MOVEMENTFLAG_SPLINE_ELEVATION = 0x04000000;
        constexpr uint32 REPLAY_MOVEMENTFLAG_SPLINE_ENABLED = 0x08000000;
        constexpr uint16 REPLAY_MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING = 0x0020;
        constexpr uint16 REPLAY_MOVEMENTFLAG2_INTERPOLATED_MOVEMENT = 0x0400;

        size_t flagsPos = pos;
        uint16 flags = 0;
        if (!ReadUInt16(payload, pos, flags))
            return false;

        bool clearedSelfFromPlayerCreate = false;

        // Packets recorded from a player's own session can have UPDATEFLAG_SELF on that player's create block.
        // During replay that actor is NOT the active client player, so leaving SELF set can make WoW treat a
        // replay ghost as the local player object and explode while parsing the create block.
        //
        // Important player-specific detail from Object::BuildMovementUpdate():
        // if TYPEID_PLAYER and UPDATEFLAG_LOWGUID are present, the payload is:
        //   self player create:     0x0000002F
        //   non-self player create: 0x00000008
        //
        // V9 cleared the SELF bit but left the lowguid payload as 0x2F. That can make player ghosts invalid
        // while normal units/pets still render. V10 also patches that LOWGUID payload to 0x08 below.
        if ((flags & REPLAY_UPDATEFLAG_SELF) && objectTypeId == REPLAY_TYPEID_PLAYER && IsReplayActorGuid(match, blockGuid))
        {
            flags &= ~REPLAY_UPDATEFLAG_SELF;
            WriteUInt16(payload, flagsPos, flags);
            clearedSelfFromPlayerCreate = true;
        }

        if (flags & REPLAY_UPDATEFLAG_LIVING)
        {
            size_t movementStart = pos;

            uint32 movementFlags = 0;
            uint16 extraMovementFlags = 0;

            if (!ReadUInt32(payload, pos, movementFlags))
                return false;

            if (!ReadUInt16(payload, pos, extraMovementFlags))
                return false;

            // time + x/y/z/o
            if (!HasRemaining(payload, pos, 4 + 16))
                return false;

            pos += 4 + 16;

            if (movementFlags & REPLAY_MOVEMENTFLAG_ONTRANSPORT)
            {
                if (!SkipPackedGuid(payload, pos))
                    return false;

                // transport x/y/z/o + transport time + transport seat
                if (!HasRemaining(payload, pos, 16 + 4 + 1))
                    return false;

                pos += 16 + 4 + 1;

                if (extraMovementFlags & REPLAY_MOVEMENTFLAG2_INTERPOLATED_MOVEMENT)
                {
                    if (!HasRemaining(payload, pos, 4))
                        return false;

                    pos += 4;
                }
            }

            if ((movementFlags & (REPLAY_MOVEMENTFLAG_SWIMMING | REPLAY_MOVEMENTFLAG_FLYING)) ||
                (extraMovementFlags & REPLAY_MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING))
            {
                if (!HasRemaining(payload, pos, 4))
                    return false;

                pos += 4;
            }

            // fall time
            if (!HasRemaining(payload, pos, 4))
                return false;

            pos += 4;

            if (movementFlags & REPLAY_MOVEMENTFLAG_FALLING)
            {
                if (!HasRemaining(payload, pos, 16))
                    return false;

                pos += 16;
            }

            if (movementFlags & REPLAY_MOVEMENTFLAG_SPLINE_ELEVATION)
            {
                if (!HasRemaining(payload, pos, 4))
                    return false;

                pos += 4;
            }

            // walk/run/runback/swim/swimback/flight/flightback/turn/pitch
            if (!HasRemaining(payload, pos, 9 * 4))
                return false;

            pos += 9 * 4;

            // Arena player create packets should not have a server-side spline create block. If they do,
            // parsing it here without a full MoveSpline parser is unsafe, so stop and leave the packet unchanged.
            if (movementFlags & REPLAY_MOVEMENTFLAG_SPLINE_ENABLED)
            {
                TC_LOG_ERROR("arena.replay", "Replay update-object parser refused spline-enabled movement block guid={} movementStart={}",
                    blockGuid.GetRawValue(), uint32(movementStart));
                return false;
            }
        }
        else
        {
            if (flags & REPLAY_UPDATEFLAG_POSITION)
            {
                if (!SkipPackedGuid(payload, pos))
                    return false;

                // position x/y/z + transport-or-absolute x/y/z + orientation + corpse orientation/zero
                if (!HasRemaining(payload, pos, 3 * 4 + 3 * 4 + 4 + 4))
                    return false;

                pos += 3 * 4 + 3 * 4 + 4 + 4;
            }
            else if (flags & REPLAY_UPDATEFLAG_STATIONARY_POSITION)
            {
                if (!HasRemaining(payload, pos, 4 * 4))
                    return false;

                pos += 4 * 4;
            }
        }

        if (flags & REPLAY_UPDATEFLAG_UNKNOWN)
        {
            if (!HasRemaining(payload, pos, 4))
                return false;

            pos += 4;
        }

        if (flags & REPLAY_UPDATEFLAG_LOWGUID)
        {
            if (!HasRemaining(payload, pos, 4))
                return false;

            if (clearedSelfFromPlayerCreate)
            {
                // Object::BuildMovementUpdate() writes 0x2F for self player creates and 0x08 for non-self
                // player creates. Since this replay actor is now a non-self ghost, patch the payload too.
                WriteUInt32(payload, pos, 0x00000008);
            }

            pos += 4;
        }

        if (flags & REPLAY_UPDATEFLAG_HAS_TARGET)
        {
            if (!SkipPackedGuid(payload, pos))
                return false;
        }

        if (flags & REPLAY_UPDATEFLAG_TRANSPORT)
        {
            if (!HasRemaining(payload, pos, 4))
                return false;

            pos += 4;
        }

        if (flags & REPLAY_UPDATEFLAG_VEHICLE)
        {
            if (!HasRemaining(payload, pos, 8))
                return false;

            pos += 8;
        }

        if (flags & REPLAY_UPDATEFLAG_ROTATION)
        {
            if (!HasRemaining(payload, pos, 8))
                return false;

            pos += 8;
        }

        return true;
    }

    bool RewriteUpdateObjectPayload(std::vector<uint8>& payload, MatchRecord const& match)
    {
        constexpr uint8 REPLAY_UPDATETYPE_VALUES = 0;
        constexpr uint8 REPLAY_UPDATETYPE_MOVEMENT = 1;
        constexpr uint8 REPLAY_UPDATETYPE_CREATE_OBJECT = 2;
        constexpr uint8 REPLAY_UPDATETYPE_CREATE_OBJECT2 = 3;
        constexpr uint8 REPLAY_UPDATETYPE_OUT_OF_RANGE_OBJECTS = 4;
        constexpr uint8 REPLAY_UPDATETYPE_NEAR_OBJECTS = 5;

        size_t pos = 0;
        uint32 blockCount = 0;

        if (!ReadUInt32(payload, pos, blockCount))
            return false;

        for (uint32 block = 0; block < blockCount; ++block)
        {
            uint8 updateType = 0;
            if (!ReadUInt8(payload, pos, updateType))
                return false;

            if (updateType == REPLAY_UPDATETYPE_OUT_OF_RANGE_OBJECTS || updateType == REPLAY_UPDATETYPE_NEAR_OBJECTS)
            {
                uint32 guidCount = 0;
                if (!ReadUInt32(payload, pos, guidCount))
                    return false;

                for (uint32 i = 0; i < guidCount; ++i)
                {
                    size_t guidStart = pos;
                    ObjectGuid guid;
                    if (!ReadPackedGuid(payload, pos, guid))
                        return false;

                    size_t guidEnd = pos;
                    if (ReplayActor const* actor = FindReplayActorByOriginalGuid(match, guid))
                        WritePackedGuidInPlace(payload, guidStart, guidEnd, actor->FakeGuid);
                }

                continue;
            }

            size_t guidStart = pos;
            ObjectGuid blockGuid;
            if (!ReadPackedGuid(payload, pos, blockGuid))
                return false;

            size_t guidEnd = pos;
            if (ReplayActor const* actor = FindReplayActorByOriginalGuid(match, blockGuid))
                WritePackedGuidInPlace(payload, guidStart, guidEnd, actor->FakeGuid);

            switch (updateType)
            {
                case REPLAY_UPDATETYPE_VALUES:
                {
                    if (!PatchUpdateValuesBlock(payload, pos, match, blockGuid))
                        return false;

                    break;
                }
                case REPLAY_UPDATETYPE_MOVEMENT:
                {
                    if (!SkipMovementCreateData(payload, pos, match, blockGuid))
                        return false;

                    break;
                }
                case REPLAY_UPDATETYPE_CREATE_OBJECT:
                case REPLAY_UPDATETYPE_CREATE_OBJECT2:
                {
                    uint8 objectTypeId = 0;
                    if (!ReadUInt8(payload, pos, objectTypeId))
                        return false;

                    if (!SkipMovementCreateData(payload, pos, match, blockGuid, objectTypeId))
                        return false;

                    if (!PatchUpdateValuesBlock(payload, pos, match, blockGuid))
                        return false;

                    break;
                }
                default:
                    TC_LOG_ERROR("arena.replay", "Replay update-object parser saw unknown updateType={} block={}/{} pos={}",
                        uint32(updateType), block, blockCount, uint32(pos));
                    return false;
            }
        }

        if (pos > payload.size())
            return false;

        return true;
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

        if (!RewriteUpdateObjectPayload(decompressed, match))
        {
            TC_LOG_ERROR("arena.replay", "Replay compressed update parser failed; sending original packet without GUID rewrite");
            return false;
        }


        // V11 rewrites update-object packets structurally. It does not do global raw/packed GUID replacement.
        // Therefore the uncompressed update-object size must stay stable. Keep this explicit so any future bad
        // rewrite is visible in logs instead of silently crashing the client.
        uint32 const rewrittenUncompressedSize = uint32(decompressed.size());
        if (rewrittenUncompressedSize != uncompressedSize)
        {
            TC_LOG_ERROR("arena.replay", "Replay compressed update changed uncompressed size old={} new={}; refusing rewrite",
                uncompressedSize, rewrittenUncompressedSize);
            return false;
        }

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
        uint32 AuraPackets = 0;
        uint32 AuraUpdatePackets = 0;
        uint32 AuraUpdateAllPackets = 0;
        uint32 ZeroTimeUpdatePackets = 0;
        uint32 ActorGuidHits = 0;
        uint32 ZeroTimeActorGuidHits = 0;
    };

    ReplayAudit BuildReplayAudit(MatchRecord const& match)
    {
        ReplayAudit audit;

        for (PacketRecord const& frame : match.Packets)
        {
            if (frame.Packet.GetOpcode() == SMSG_AURA_UPDATE)
            {
                ++audit.AuraPackets;
                ++audit.AuraUpdatePackets;
            }
            else if (frame.Packet.GetOpcode() == SMSG_AURA_UPDATE_ALL)
            {
                ++audit.AuraPackets;
                ++audit.AuraUpdateAllPackets;
            }

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
        {
            RewriteCompressedUpdateObjectPayload(payload, match);
        }
        else if (frame.Packet.GetOpcode() == SMSG_UPDATE_OBJECT)
        {
            RewriteUpdateObjectPayload(payload, match);
        }
        else
        {
            RewriteNonUpdatePacketGuids(frame.Packet.GetOpcode(), payload, match);
        }

        WorldPacket out(frame.Packet.GetOpcode(), payload.size());
        if (!payload.empty())
            out.append(payload.data(), payload.size());

        return out;
    }

    bool ReplayActorShouldUseFriendlyOverheadName(ReplayActor const& actor)
    {
        // Existing replay/addon convention uses team 67 as the green/allied side.
        return actor.Team == HORDE || actor.Team == 67;
    }

    uint8 ReplayNameQueryRaceForViewer(ReplayActor const& actor, Player const* viewer)
    {
        // Player overhead reaction can be influenced by the client's player-name cache.
        // For the green/allied replay side, report the viewer's own race in the fake name-query response,
        // while leaving the world model/update fields untouched.
        if (ReplayActorShouldUseFriendlyOverheadName(actor) && viewer && viewer->GetRace())
            return uint8(viewer->GetRace());

        return actor.Race;
    }

    uint32 ReplayArenaTeamAuraForActor(ReplayActor const& actor)
    {
        bool green = ReplayActorShouldUseFriendlyOverheadName(actor);
        bool horde = actor.Team == HORDE || actor.Team == 67;

        // 3.3.5 arena team visual/team-marker spells:
        //   32724 Gold Team (Alliance)
        //   32725 Green Team (Alliance)
        //   35774 Gold Team (Horde)
        //   35775 Green Team (Horde)
        if (green)
            return horde ? 35775u : 32725u;

        return horde ? 35774u : 32724u;
    }

    void SendReplayNameResponse(WorldSession* session, ReplayActor const& actor)
    {
        if (!session)
            return;

        Player const* viewer = session->GetPlayer();
        uint8 replayRace = ReplayNameQueryRaceForViewer(actor, viewer);

        WorldPacket data(SMSG_NAME_QUERY_RESPONSE, 8 + 1 + actor.Name.size() + 1 + 1 + 1 + 1 + 1);
        data << actor.FakeGuid.WriteAsPacked();
        data << uint8(0);              // name known
        data << actor.Name;            // player name
        data << uint8(0);              // realm name
        data << uint8(replayRace);
        data << uint8(actor.Gender);
        data << uint8(actor.Class);
        data << uint8(0);              // declined names disabled
        session->SendPacket(&data);

        TC_LOG_DEBUG("arena.replay", "Replay name-query response fake={} name={} team={} actorRace={} replayRace={} viewerRace={}",
            actor.FakeGuid.ToString(), actor.Name, actor.Team, uint32(actor.Race), uint32(replayRace),
            viewer ? uint32(viewer->GetRace()) : 0u);
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

    std::string ReplayASGuidString(ObjectGuid guid)
    {
        std::ostringstream ss;
        ss << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << guid.GetRawValue();
        return ss.str();
    }

    void SendReplayASRaw(Player* viewer, std::string const& payload)
    {
        if (!viewer || !viewer->GetSession())
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty, ObjectGuid::Empty, std::string("ASSUN\t") + payload, 0);
        viewer->GetSession()->SendPacket(&data);
    }

    void SendReplayASCommand(Player* viewer, ObjectGuid targetGuid, char const* prefix, std::string const& value)
    {
        if (!targetGuid.IsPlayer())
            return;

        SendReplayASRaw(viewer, ReplayASGuidString(targetGuid) + ";" + prefix + "=" + value + ";");
    }

    void SendReplayArenaTeamAura(Player* viewer, ReplayActor const& actor)
    {
        if (!viewer || !viewer->GetSession())
            return;

        uint32 spellId = ReplayArenaTeamAuraForActor(actor);
        if (!spellId)
            return;

        // Build a simple visible-aura update for the fake replay player.
        // This targets the client-side arena Gold/Green Team marker spells rather than faction template reaction.
        constexpr uint8 REPLAY_TEAM_AURA_SLOT = 63;
        constexpr uint8 REPLAY_AFLAG_EFF_INDEX_0 = 0x01;
        constexpr uint8 REPLAY_AFLAG_CASTER = 0x08;
        constexpr uint8 REPLAY_AFLAG_POSITIVE = 0x10;

        WorldPacket data(SMSG_AURA_UPDATE, 32);
        data << actor.FakeGuid.WriteAsPacked();
        data << uint8(REPLAY_TEAM_AURA_SLOT);
        data << uint32(spellId);
        data << uint8(REPLAY_AFLAG_EFF_INDEX_0 | REPLAY_AFLAG_CASTER | REPLAY_AFLAG_POSITIVE);
        data << uint8(viewer->GetLevel());
        data << uint8(0); // charges / stack

        viewer->GetSession()->SendPacket(&data);

        TC_LOG_DEBUG("arena.replay", "Replay arena team aura fake={} name={} team={} spell={}",
            actor.FakeGuid.ToString(), actor.Name, actor.Team, spellId);
    }

    void SendReplayOverheadNameColorPackets(Player* viewer, PlaybackState& state, char const* reason)
    {
        if (!viewer || !viewer->GetSession())
            return;

        uint32 sent = 0;
        for (ReplayActor const& actor : state.Match.Actors)
        {
            // Re-send the fake name query response too; the client name cache can affect player overhead reaction.
            SendReplayNameResponse(viewer->GetSession(), actor);

            // Send Gold/Green team marker aura.
            SendReplayArenaTeamAura(viewer, actor);
            ++sent;
        }

        TC_LOG_DEBUG("arena.replay", "Replay overhead name color packets viewer={} sent={} reason={}",
            viewer->GetGUID().GetCounter(), sent, reason ? reason : "");
    }

    void MaybeSendReplayOverheadNameColorPackets(Player* viewer, PlaybackState& state, uint32 nowMs)
    {
        // Repeat briefly after playback starts because fake player objects/name cache can be created in phases.
        constexpr uint8 MAX_NAME_COLOR_BURSTS = 20;
        constexpr uint32 NAME_COLOR_BURST_INTERVAL_MS = 250;

        if (state.NameColorUpdateBursts >= MAX_NAME_COLOR_BURSTS)
            return;

        if (state.LastNameColorUpdateMs && nowMs - state.LastNameColorUpdateMs < NAME_COLOR_BURST_INTERVAL_MS)
            return;

        state.LastNameColorUpdateMs = nowMs;
        ++state.NameColorUpdateBursts;

        SendReplayOverheadNameColorPackets(viewer, state, "periodic arena team aura/name-query update");
    }

    void SendReplayASCommand(Player* viewer, ObjectGuid targetGuid, char const* prefix, uint32 value)
    {
        SendReplayASCommand(viewer, targetGuid, prefix, std::to_string(value));
    }

    uint32 ReplayASPowerTypeForClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
                return POWER_RAGE;
            case CLASS_ROGUE:
                return POWER_ENERGY;
            case CLASS_DEATH_KNIGHT:
                return POWER_RUNIC_POWER;
            default:
                return POWER_MANA;
        }
    }

    void SendReplayASInitial(Player* viewer, PlaybackState& state)
    {
        if (!viewer || !viewer->GetSession())
            return;

        SendReplayASRaw(viewer, "ENABLE");
        SendReplayASRaw(viewer, "REQUESTRESET");

        uint32 durationSeconds = state.Match.Packets.empty() ? 0 : state.Match.Packets.back().TimestampMs / IN_MILLISECONDS;

        for (ReplayActor const& actor : state.Match.Actors)
        {
            ObjectGuid guid = actor.FakeGuid;
            uint32 powerType = ReplayASPowerTypeForClass(actor.Class);
            uint32 maxPower = powerType == POWER_RAGE || powerType == POWER_RUNIC_POWER ? 100u : (powerType == POWER_ENERGY ? 100u : 1u);
            uint32 currentPower = powerType == POWER_RAGE || powerType == POWER_RUNIC_POWER ? 0u : maxPower;
            uint32 team = actor.Team ? actor.Team : ALLIANCE;

            SendReplayASCommand(viewer, guid, "NME", actor.Name.empty() ? "Replay" : actor.Name);
            SendReplayASCommand(viewer, guid, "TEM", team);
            SendReplayASCommand(viewer, guid, "CLA", uint32(actor.Class));
            SendReplayASCommand(viewer, guid, "MHP", 100u);
            SendReplayASCommand(viewer, guid, "CHP", 100u);
            SendReplayASCommand(viewer, guid, "STA", 1u);
            SendReplayASCommand(viewer, guid, "PWT", powerType);
            SendReplayASCommand(viewer, guid, "MPW", maxPower);
            SendReplayASCommand(viewer, guid, "CPW", currentPower);
            SendReplayASCommand(viewer, guid, "PHP", 0u);
            SendReplayASCommand(viewer, guid, "PET", 0u);
            SendReplayASCommand(viewer, guid, "RES", 1u);
            SendReplayASCommand(viewer, guid, "CDC", 1u);
            SendReplayASCommand(viewer, guid, "TIM", durationSeconds);
        }

        state.SentReplayASInitial = true;
        SendReplayOverheadNameColorPackets(viewer, state, "initial replay addon setup");
    }

    constexpr uint32 REPLAY_OBJECT_FIELD_GUID_LOW       = 0x0000;
    constexpr uint32 REPLAY_OBJECT_FIELD_GUID_HIGH      = 0x0001;
    constexpr uint32 REPLAY_UNIT_FIELD_CHARMEDBY_LOW    = 0x000C;
    constexpr uint32 REPLAY_UNIT_FIELD_CHARMEDBY_HIGH   = 0x000D;
    constexpr uint32 REPLAY_UNIT_FIELD_SUMMONEDBY_LOW   = 0x000E;
    constexpr uint32 REPLAY_UNIT_FIELD_SUMMONEDBY_HIGH  = 0x000F;
    constexpr uint32 REPLAY_UNIT_FIELD_CREATEDBY_LOW    = 0x0010;
    constexpr uint32 REPLAY_UNIT_FIELD_CREATEDBY_HIGH   = 0x0011;
    constexpr uint32 REPLAY_UNIT_FIELD_HEALTH           = 0x0018;
    constexpr uint32 REPLAY_UNIT_FIELD_POWER1           = 0x0019;
    constexpr uint32 REPLAY_UNIT_FIELD_MAXHEALTH        = 0x0020;
    constexpr uint32 REPLAY_UNIT_FIELD_MAXPOWER1        = 0x0021;

    uint32 ReplayASPowerFieldForType(uint32 powerType)
    {
        return REPLAY_UNIT_FIELD_POWER1 + powerType;
    }

    uint32 ReplayASMaxPowerFieldForType(uint32 powerType)
    {
        return REPLAY_UNIT_FIELD_MAXPOWER1 + powerType;
    }

    bool ReplayASReadFieldMap(std::vector<uint8> const& payload, size_t& pos, std::unordered_map<uint32, uint32>& values)
    {
        uint8 maskBlockCount = 0;
        if (!ReadUInt8(payload, pos, maskBlockCount))
            return false;

        std::vector<uint32> masks;
        masks.reserve(maskBlockCount);

        for (uint8 i = 0; i < maskBlockCount; ++i)
        {
            uint32 mask = 0;
            if (!ReadUInt32(payload, pos, mask))
                return false;

            masks.push_back(mask);
        }

        for (uint32 block = 0; block < masks.size(); ++block)
        {
            uint32 mask = masks[block];

            for (uint8 bit = 0; bit < 32; ++bit)
            {
                if (!(mask & (uint32(1) << bit)))
                    continue;

                uint32 value = 0;
                if (!ReadUInt32(payload, pos, value))
                    return false;

                values[block * 32 + bit] = value;
            }
        }

        return true;
    }

    bool ReplayASGetField(std::unordered_map<uint32, uint32> const& values, uint32 field, uint32& value)
    {
        auto itr = values.find(field);
        if (itr == values.end())
            return false;

        value = itr->second;
        return true;
    }

    ObjectGuid ReplayASGuidFromFields(std::unordered_map<uint32, uint32> const& values, uint32 lowField, uint32 highField)
    {
        uint32 low = 0;
        uint32 high = 0;

        if (!ReplayASGetField(values, lowField, low) || !ReplayASGetField(values, highField, high))
            return ObjectGuid::Empty;

        uint64 raw = uint64(low) | (uint64(high) << 32);
        return ObjectGuid(raw);
    }

    ReplayActor const* FindReplayASPetCapableFallbackOwner(MatchRecord const& match)
    {
        ReplayActor const* fallback = nullptr;
        uint32 count = 0;

        for (ReplayActor const& actor : match.Actors)
        {
            // Hunter, Warlock, Death Knight, Mage. Hunter/Warlock are the important 3.3.5 pet classes;
            // DK/Mage are included for ghoul/water elemental edge cases.
            if (actor.Class != CLASS_HUNTER && actor.Class != CLASS_WARLOCK && actor.Class != CLASS_DEATH_KNIGHT && actor.Class != CLASS_MAGE)
                continue;

            fallback = &actor;
            ++count;
        }

        return count == 1 ? fallback : nullptr;
    }

    ReplayActor const* FindReplayASOwnerFromAnyGuidPair(MatchRecord const& match, std::unordered_map<uint32, uint32> const& values)
    {
        // First try the known owner-style unit fields.
        ObjectGuid ownerGuid = ReplayASGuidFromFields(values, REPLAY_UNIT_FIELD_CHARMEDBY_LOW, REPLAY_UNIT_FIELD_CHARMEDBY_HIGH);
        if (ReplayActor const* owner = FindReplayActorByGuid(match, ownerGuid))
            return owner;

        ownerGuid = ReplayASGuidFromFields(values, REPLAY_UNIT_FIELD_SUMMONEDBY_LOW, REPLAY_UNIT_FIELD_SUMMONEDBY_HIGH);
        if (ReplayActor const* owner = FindReplayActorByGuid(match, ownerGuid))
            return owner;

        ownerGuid = ReplayASGuidFromFields(values, REPLAY_UNIT_FIELD_CREATEDBY_LOW, REPLAY_UNIT_FIELD_CREATEDBY_HIGH);
        if (ReplayActor const* owner = FindReplayActorByGuid(match, ownerGuid))
            return owner;

        // Some pet-like units do not reliably carry the exact owner field in every replay update.
        // Scan adjacent update-field pairs for any replay actor GUID. This is intentionally only used
        // on non-player unit update packets after the object has already failed actor matching.
        for (auto const& lowItr : values)
        {
            auto highItr = values.find(lowItr.first + 1);
            if (highItr == values.end())
                continue;

            ObjectGuid candidate(uint64(lowItr.second) | (uint64(highItr->second) << 32));
            if (ReplayActor const* owner = FindReplayActorByGuid(match, candidate))
                return owner;
        }

        // Last fallback: if this replay has exactly one pet-capable player, attach the pet frame there.
        return FindReplayASPetCapableFallbackOwner(match);
    }

    void SendReplayASPlayerStatusFromValues(Player* viewer, MatchRecord const& match, ObjectGuid objectGuid, std::unordered_map<uint32, uint32> const& values)
    {
        ReplayActor const* actor = FindReplayActorByGuid(match, objectGuid);
        if (!actor)
            return;

        ObjectGuid uiGuid = actor->FakeGuid;

        uint32 health = 0;
        uint32 maxHealth = 0;
        if (ReplayASGetField(values, REPLAY_UNIT_FIELD_MAXHEALTH, maxHealth) && maxHealth > 0)
            SendReplayASCommand(viewer, uiGuid, "MHP", maxHealth);

        if (ReplayASGetField(values, REPLAY_UNIT_FIELD_HEALTH, health))
        {
            SendReplayASCommand(viewer, uiGuid, "CHP", health);
            SendReplayASCommand(viewer, uiGuid, "STA", health > 0 ? 1u : 0u);
        }

        uint32 powerType = ReplayASPowerTypeForClass(actor->Class);
        SendReplayASCommand(viewer, uiGuid, "PWT", powerType);

        uint32 maxPower = 0;
        if (ReplayASGetField(values, ReplayASMaxPowerFieldForType(powerType), maxPower) && maxPower > 0)
            SendReplayASCommand(viewer, uiGuid, "MPW", maxPower);

        uint32 currentPower = 0;
        if (ReplayASGetField(values, ReplayASPowerFieldForType(powerType), currentPower))
            SendReplayASCommand(viewer, uiGuid, "CPW", currentPower);

        // UI-only target feed for the addon.
        // This uses the same verified UNIT_FIELD_TARGET pair that v23 rewrites for the default Blizzard UI.
        // Because SendReplayASForPlaybackPacket receives the already-rewritten playback packet, this target
        // may already be the fake GUID; FindReplayActorByGuid accepts original or fake.
        ObjectGuid targetGuid = ReplayASGuidFromFields(values, UNIT_FIELD_TARGET, UNIT_FIELD_TARGET + 1);
        if (targetGuid)
        {
            if (ReplayActor const* targetActor = FindReplayActorByGuid(match, targetGuid))
                SendReplayASCommand(viewer, uiGuid, "TRG", ReplayASGuidString(targetActor->FakeGuid));
            else
                SendReplayASCommand(viewer, uiGuid, "TRG", 0u);
        }
    }

    void SendReplayASPetStatusFromValues(Player* viewer, MatchRecord const& match, std::unordered_map<uint32, uint32> const& values)
    {
        ReplayActor const* owner = FindReplayASOwnerFromAnyGuidPair(match, values);
        if (!owner)
            return;

        uint32 health = 0;
        uint32 maxHealth = 0;

        if (!ReplayASGetField(values, REPLAY_UNIT_FIELD_HEALTH, health))
            return;

        uint32 pct = 0;
        if (ReplayASGetField(values, REPLAY_UNIT_FIELD_MAXHEALTH, maxHealth) && maxHealth > 0)
            pct = std::min<uint32>(100u, uint32(std::ceil(double(health) * 100.0 / double(maxHealth))));
        else
            pct = health > 0 ? 100u : 0u;

        // Send PET before PHP so the icon texture is ready before UpdatePet shows the frame.
        // 0 is the addon's generic question-mark icon; PHP is what controls visibility.
        SendReplayASCommand(viewer, owner->FakeGuid, "PET", 0u);
        SendReplayASCommand(viewer, owner->FakeGuid, "PHP", pct);
    }

    bool SendReplayASStatusFromUpdatePayload(Player* viewer, MatchRecord const& match, std::vector<uint8>& payload)
    {
        constexpr uint8 REPLAY_UPDATETYPE_VALUES = 0;
        constexpr uint8 REPLAY_UPDATETYPE_MOVEMENT = 1;
        constexpr uint8 REPLAY_UPDATETYPE_CREATE_OBJECT = 2;
        constexpr uint8 REPLAY_UPDATETYPE_CREATE_OBJECT2 = 3;
        constexpr uint8 REPLAY_UPDATETYPE_OUT_OF_RANGE_OBJECTS = 4;
        constexpr uint8 REPLAY_UPDATETYPE_NEAR_OBJECTS = 5;

        size_t pos = 0;
        uint32 blockCount = 0;

        if (!ReadUInt32(payload, pos, blockCount))
            return false;

        for (uint32 block = 0; block < blockCount; ++block)
        {
            uint8 updateType = 0;
            if (!ReadUInt8(payload, pos, updateType))
                return false;

            if (updateType == REPLAY_UPDATETYPE_OUT_OF_RANGE_OBJECTS || updateType == REPLAY_UPDATETYPE_NEAR_OBJECTS)
            {
                uint32 guidCount = 0;
                if (!ReadUInt32(payload, pos, guidCount))
                    return false;

                for (uint32 i = 0; i < guidCount; ++i)
                {
                    if (!SkipPackedGuid(payload, pos))
                        return false;
                }

                continue;
            }

            ObjectGuid blockGuid;
            if (!ReadPackedGuid(payload, pos, blockGuid))
                return false;

            switch (updateType)
            {
                case REPLAY_UPDATETYPE_VALUES:
                {
                    std::unordered_map<uint32, uint32> values;
                    if (!ReplayASReadFieldMap(payload, pos, values))
                        return false;

                    if (FindReplayActorByGuid(match, blockGuid))
                        SendReplayASPlayerStatusFromValues(viewer, match, blockGuid, values);
                    else
                        SendReplayASPetStatusFromValues(viewer, match, values);

                    break;
                }
                case REPLAY_UPDATETYPE_MOVEMENT:
                {
                    if (!SkipMovementCreateData(payload, pos, match, blockGuid))
                        return false;

                    break;
                }
                case REPLAY_UPDATETYPE_CREATE_OBJECT:
                case REPLAY_UPDATETYPE_CREATE_OBJECT2:
                {
                    uint8 objectTypeId = 0;
                    if (!ReadUInt8(payload, pos, objectTypeId))
                        return false;

                    if (!SkipMovementCreateData(payload, pos, match, blockGuid, objectTypeId))
                        return false;

                    std::unordered_map<uint32, uint32> values;
                    if (!ReplayASReadFieldMap(payload, pos, values))
                        return false;

                    if (FindReplayActorByGuid(match, blockGuid))
                        SendReplayASPlayerStatusFromValues(viewer, match, blockGuid, values);
                    else
                        SendReplayASPetStatusFromValues(viewer, match, values);

                    break;
                }
                default:
                    return false;
            }
        }

        return true;
    }

    bool SendReplayASStatusFromPlaybackPacket(Player* viewer, WorldPacket const& packet, MatchRecord const& match)
    {
        if (packet.GetOpcode() == SMSG_UPDATE_OBJECT)
        {
            std::vector<uint8> payload(packet.size());
            if (!payload.empty())
                std::memcpy(payload.data(), packet.contents(), payload.size());

            return SendReplayASStatusFromUpdatePayload(viewer, match, payload);
        }

        if (packet.GetOpcode() == SMSG_COMPRESSED_UPDATE_OBJECT)
        {
            if (packet.size() < 4)
                return false;

            std::vector<uint8> payload(packet.size());
            std::memcpy(payload.data(), packet.contents(), payload.size());

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
                return false;

            return SendReplayASStatusFromUpdatePayload(viewer, match, decompressed);
        }

        return false;
    }

    bool ExtractReplayASSpell(WorldPacket const& packet, ObjectGuid& caster, uint32& spellId, int32& castTime)
    {
        // For the replay addon history, only emit the cast once.
        // SMSG_SPELL_START and SMSG_SPELL_GO both exist for the same cast, and some instant spells
        // can produce multiple GO-like packets. Sending both makes the addon history show duplicates.
        //
        // The actual replay client still receives SMSG_SPELL_GO normally; this only controls ASSUN UI feed.
        if (packet.GetOpcode() != SMSG_SPELL_START)
            return false;

        std::vector<uint8> payload(packet.size());
        if (!payload.empty())
            std::memcpy(payload.data(), packet.contents(), payload.size());

        size_t pos = 0;
        ObjectGuid casterGuid;
        ObjectGuid casterUnit;
        if (!ReadPackedGuid(payload, pos, casterGuid))
            return false;
        if (!ReadPackedGuid(payload, pos, casterUnit))
            return false;

        uint8 castId = 0;
        uint32 castFlags = 0;
        uint32 castTimeRaw = 0;
        if (!ReadUInt8(payload, pos, castId))
            return false;
        if (!ReadUInt32(payload, pos, spellId))
            return false;
        if (!ReadUInt32(payload, pos, castFlags))
            return false;
        if (!ReadUInt32(payload, pos, castTimeRaw))
            return false;

        caster = casterUnit.IsPlayer() ? casterUnit : casterGuid;
        castTime = packet.GetOpcode() == SMSG_SPELL_START ? int32(castTimeRaw) : 0;
        return caster.IsPlayer() && spellId != 0;
    }

    bool ExtractReplayASAttackStart(WorldPacket const& packet, ObjectGuid& attacker, ObjectGuid& victim)
    {
        if (packet.GetOpcode() != SMSG_ATTACK_START || packet.size() < 16)
            return false;

        std::vector<uint8> payload(packet.size());
        std::memcpy(payload.data(), packet.contents(), payload.size());

        uint64 a = 0;
        uint64 v = 0;
        if (!ReadUInt64At(payload, 0, a) || !ReadUInt64At(payload, 8, v))
            return false;

        attacker = ObjectGuid(a);
        victim = ObjectGuid(v);
        return attacker.IsPlayer();
    }


    struct ReplayASAuraSlotState
    {
        uint32 SpellId = 0;
        uint8 IsDebuff = 0;
        uint8 Stack = 0;
        std::string Caster = "0";
    };

    std::unordered_map<uint64, ReplayASAuraSlotState> ReplayASAuraSlotCache;

    uint64 ReplayASAuraCacheKey(Player* viewer, ObjectGuid targetGuid, uint8 slot)
    {
        uint64 viewerPart = viewer ? uint64(viewer->GetGUID().GetCounter()) : 0;
        return (viewerPart << 40) ^ (targetGuid.GetRawValue() << 8) ^ uint64(slot);
    }

    bool ReplayASShouldShowAuraInDefaultFrame(uint32 spellId, uint8 flags, uint32 maxDurationMs, uint32 remainingMs)
    {
        constexpr uint8 REPLAY_AFLAG_EFFECT1  = 0x01;
        constexpr uint8 REPLAY_AFLAG_EFFECT2  = 0x02;
        constexpr uint8 REPLAY_AFLAG_EFFECT3  = 0x04;
        constexpr uint8 REPLAY_AFLAG_POSITIVE = 0x10;
        constexpr uint8 REPLAY_AFLAG_NEGATIVE = 0x80;

        bool hasEffect = (flags & (REPLAY_AFLAG_EFFECT1 | REPLAY_AFLAG_EFFECT2 | REPLAY_AFLAG_EFFECT3)) != 0;
        bool hasDefaultFramePolarity = (flags & (REPLAY_AFLAG_POSITIVE | REPLAY_AFLAG_NEGATIVE)) != 0;

        if (!hasEffect || !hasDefaultFramePolarity)
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return false;

        constexpr uint32 REPLAY_SPELL_ATTR0_PASSIVE = 0x00000040;
        constexpr uint32 REPLAY_SPELL_ATTR0_HIDDEN_CLIENTSIDE = 0x00000080;

        if (spellInfo->Attributes & (REPLAY_SPELL_ATTR0_PASSIVE | REPLAY_SPELL_ATTR0_HIDDEN_CLIENTSIDE))
            return false;

        return true;
    }

    void SendReplayASAuraCommand(Player* viewer, ObjectGuid targetGuid, uint8 remove, uint8 stack, uint32 remainingMs,
        uint32 maxDurationMs, uint32 spellId, uint8 dispelType, uint8 isDebuff, std::string const& caster)
    {
        std::ostringstream ss;
        ss << uint32(remove) << ","
           << uint32(stack) << ","
           << remainingMs << ","
           << maxDurationMs << ","
           << spellId << ","
           << uint32(dispelType) << ","
           << uint32(isDebuff) << ","
           << caster;

        SendReplayASCommand(viewer, targetGuid, "AUR", ss.str());
    }

    bool SendReplayASAurasFromPlaybackPacket(Player* viewer, WorldPacket const& packet, MatchRecord const& match)
    {
        // Aura packet layout in this branch:
        //   SMSG_AURA_UPDATE_ALL: packed target guid, then repeated AuraApplication::BuildUpdatePacket()
        //   SMSG_AURA_UPDATE:     packed target guid, then one AuraApplication::BuildUpdatePacket()
        //
        // BuildUpdatePacket:
        //   uint8 slot
        //   uint32 spellId
        //   if spellId == 0: remove
        //   uint8 flags
        //   uint8 casterLevel
        //   uint8 stackOrCharges
        //   if !(flags & AFLAG_CASTER): packed caster guid
        //   if flags & AFLAG_DURATION: uint32 maxDuration, uint32 durationRemaining
        if (packet.GetOpcode() != SMSG_AURA_UPDATE && packet.GetOpcode() != SMSG_AURA_UPDATE_ALL)
            return false;        constexpr uint8 REPLAY_AFLAG_EFFECT1  = 0x01;
        constexpr uint8 REPLAY_AFLAG_EFFECT2  = 0x02;
        constexpr uint8 REPLAY_AFLAG_EFFECT3  = 0x04;
        constexpr uint8 REPLAY_AFLAG_CASTER   = 0x08;
        constexpr uint8 REPLAY_AFLAG_POSITIVE = 0x10;
        constexpr uint8 REPLAY_AFLAG_DURATION = 0x20;
        constexpr uint8 REPLAY_AFLAG_NEGATIVE = 0x80;
std::vector<uint8> payload(packet.size());
        if (!payload.empty())
            std::memcpy(payload.data(), packet.contents(), payload.size());

        size_t pos = 0;
        ObjectGuid targetGuid;
        if (!ReadPackedGuid(payload, pos, targetGuid))
            return false;

        ReplayActor const* targetActor = FindReplayActorByGuid(match, targetGuid);
        if (!targetActor)
            return false;

        ObjectGuid uiGuid = targetActor->FakeGuid;

        bool sentAny = false;
        while (pos < payload.size())
        {
            uint8 slot = 0;
            if (!ReadUInt8(payload, pos, slot))
                return sentAny;

            uint32 spellId = 0;
            if (!ReadUInt32(payload, pos, spellId))
                return sentAny;

            uint64 cacheKey = ReplayASAuraCacheKey(viewer, uiGuid, slot);

            if (spellId == 0)
            {
                auto itr = ReplayASAuraSlotCache.find(cacheKey);
                if (itr != ReplayASAuraSlotCache.end() && itr->second.SpellId)
                {
                    SendReplayASAuraCommand(viewer, uiGuid, 1, itr->second.Stack, 0, 0, itr->second.SpellId, 0,
                        itr->second.IsDebuff, itr->second.Caster);
                    ReplayASAuraSlotCache.erase(itr);
                    sentAny = true;
                }

                continue;
            }

            uint8 flags = 0;
            uint8 casterLevel = 0;
            uint8 stackOrCharges = 0;

            if (!ReadUInt8(payload, pos, flags) || !ReadUInt8(payload, pos, casterLevel) || !ReadUInt8(payload, pos, stackOrCharges))
                return sentAny;

            ObjectGuid casterGuid;
            std::string casterString = "0";

            if (!(flags & REPLAY_AFLAG_CASTER))
            {
                if (!ReadPackedGuid(payload, pos, casterGuid))
                    return sentAny;

                if (ReplayActor const* casterActor = FindReplayActorByGuid(match, casterGuid))
                    casterString = ReplayASGuidString(casterActor->FakeGuid);
                else if (casterGuid)
                    casterString = ReplayASGuidString(casterGuid);
            }
            else
            {
                casterString = ReplayASGuidString(uiGuid);
            }

            uint32 maxDurationMs = 0;
            uint32 remainingMs = 0;

            if (flags & REPLAY_AFLAG_DURATION)
            {
                if (!ReadUInt32(payload, pos, maxDurationMs) || !ReadUInt32(payload, pos, remainingMs))
                    return sentAny;
            }            if (!ReplayASShouldShowAuraInDefaultFrame(spellId, flags, maxDurationMs, remainingMs))
            {
                auto itr = ReplayASAuraSlotCache.find(cacheKey);
                if (itr != ReplayASAuraSlotCache.end())
                {
                    SendReplayASAuraCommand(viewer, uiGuid, 1, itr->second.Stack, 0, 0, itr->second.SpellId, 0,
                        itr->second.IsDebuff, itr->second.Caster);
                    ReplayASAuraSlotCache.erase(itr);
                }

                continue;
            }

            uint8 isDebuff = (flags & REPLAY_AFLAG_NEGATIVE) ? 1 : 0;
            // The aura packet does not include the dispel type in this branch. The addon still shows a normal icon border.
            uint8 dispelType = 0;

            ReplayASAuraSlotState state;
            state.SpellId = spellId;
            state.IsDebuff = isDebuff;
            state.Stack = stackOrCharges;
            state.Caster = casterString;
            ReplayASAuraSlotCache[cacheKey] = state;

            SendReplayASAuraCommand(viewer, uiGuid, 0, stackOrCharges, remainingMs, maxDurationMs, spellId, dispelType, isDebuff, casterString);
            sentAny = true;

            (void)casterLevel;
            (void)REPLAY_AFLAG_POSITIVE;
        }

        return sentAny;
    }

    void SendReplayASForPlaybackPacket(Player* viewer, WorldPacket const& packet, MatchRecord const& match)
    {
        // Feed real HP / max HP / power / max power / pet HP to the replay addon from the same
        // update-object packets that make replay actors move/render.
        SendReplayASStatusFromPlaybackPacket(viewer, packet, match);

        // Feed replay aura packets into WoW-style buff/debuff rows.
        SendReplayASAurasFromPlaybackPacket(viewer, packet, match);

        ObjectGuid caster;
        uint32 spellId = 0;
        int32 castTime = 0;
        if (ExtractReplayASSpell(packet, caster, spellId, castTime) && IsReplayActorGuid(match, caster))
        {
            SendReplayASCommand(viewer, caster, "SPE", std::to_string(spellId) + "," + std::to_string(castTime));
            return;
        }

        ObjectGuid attacker;
        ObjectGuid victim;
        if (ExtractReplayASAttackStart(packet, attacker, victim) && IsReplayActorGuid(match, attacker))
        {
            if (victim.IsPlayer())
            {
                if (ReplayActor const* victimActor = FindReplayActorByGuid(match, victim))
                    SendReplayASCommand(viewer, attacker, "TRG", ReplayASGuidString(victimActor->FakeGuid));
                else
                    SendReplayASCommand(viewer, attacker, "TRG", ReplayASGuidString(victim));
            }

            // 6603 = Attack. This gives the replay UI an icon/history tick for melee starts.
            SendReplayASCommand(viewer, attacker, "SPE", "6603,0");
            return;
        }
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

    std::string EscapeReplaySqlString(std::string value)
    {
        CharacterDatabase.EscapeString(value);
        return value;
    }

    std::string JoinActorGuidList(MatchRecord const& match, uint32 team)
    {
        std::string out;
        for (ReplayActor const& actor : match.Actors)
        {
            if (actor.Team != team)
                continue;

            if (!out.empty())
                out += ", ";

            out += std::to_string(actor.OriginalGuid.GetRawValue());
        }

        return out;
    }

    std::string JoinActorNameList(MatchRecord const& match, uint32 team)
    {
        std::string out;
        for (ReplayActor const& actor : match.Actors)
        {
            if (actor.Team != team)
                continue;

            if (!out.empty())
                out += " ";

            out += actor.Name;
        }

        return out.empty() ? "Unknown" : out;
    }

    uint32 NormalizeReplayWinnerTeam(MatchRecord const& match, uint32 winner)
    {
        if (winner == ALLIANCE || winner == HORDE)
            return winner;

        for (ReplayActor const& actor : match.Actors)
            if (actor.Team == ALLIANCE)
                return ALLIANCE;

        for (ReplayActor const& actor : match.Actors)
            if (actor.Team == HORDE)
                return HORDE;

        return ALLIANCE;
    }

    void GetReplayTeamInfo(Battleground* bg, MatchRecord const& match, uint32 team, std::string& teamName, uint32& rating, uint32& mmr)
    {
        teamName.clear();
        rating = 0;
        mmr = 0;

        if (bg && bg->isArena() && bg->isRated())
        {
            if (ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(bg->GetArenaTeamIdForTeam(team)))
            {
                teamName = arenaTeam->GetName();
                rating = arenaTeam->GetRating();
            }

            mmr = bg->GetArenaMatchmakerRating(team);
        }

        if (teamName.empty())
            teamName = JoinActorNameList(match, team);
    }

    uint32 GetNextArenaReplayIdForMessage()
    {
        QueryResult result = CharacterDatabase.Query("SELECT COALESCE(MAX(`id`), 0) FROM `character_arena_replays`");
        if (!result)
            return 0;

        return (*result)[0].GetUInt32() + 1;
    }

    bool ReplayExistsInAcTable(uint32 replayId)
    {
        QueryResult result = CharacterDatabase.PQuery("SELECT `id` FROM `character_arena_replays` WHERE `id` = {} LIMIT 1", replayId);
        return bool(result);
    }

    std::vector<uint32> LoadSavedReplayIds(uint32 characterId)
    {
        std::vector<uint32> replayIds;
        QueryResult result = CharacterDatabase.PQuery(
            "SELECT `replay_id` FROM `character_saved_replays` WHERE `character_id` = {} ORDER BY `id` DESC LIMIT 20", characterId);

        if (!result)
            return replayIds;

        do
        {
            replayIds.push_back((*result)[0].GetUInt32());
        }
        while (result->NextRow());

        return replayIds;
    }

    void FavoriteReplayForPlayer(Player* player, uint32 replayId)
    {
        if (!player || !player->GetSession())
            return;

        ChatHandler handler(player->GetSession());

        if (!ReplayExistsInAcTable(replayId))
        {
            handler.PSendSysMessage("Replay match ID %u does not exist in the new AC-style replay table.", replayId);
            return;
        }

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO `character_saved_replays` (`character_id`, `replay_id`) VALUES ({}, {})",
            player->GetGUID().GetCounter(), replayId);

        handler.PSendSysMessage("Replay match ID %u saved to your favorites.", replayId);
    }

    void DeleteOldArenaReplaysFromConfig()
    {
        int32 daysConfig = sConfigMgr->GetIntDefault("ArenaReplay.DeleteReplaysAfterDays", 30);
        uint32 days = daysConfig > 0 ? uint32(daysConfig) : 0;
        if (!days)
            return;

        bool deleteSaved = sConfigMgr->GetBoolDefault("ArenaReplay.DeleteSavedReplays", false);

        if (deleteSaved)
        {
            CharacterDatabase.Execute(
                "DELETE FROM `character_arena_replays` WHERE `timestamp` < (NOW() - INTERVAL {} DAY)", days);
            CharacterDatabase.Execute(
                "DELETE FROM `character_saved_replays` WHERE `replay_id` NOT IN (SELECT `id` FROM `character_arena_replays`)");
        }
        else
        {
            CharacterDatabase.Execute(
                "DELETE FROM `character_arena_replays` "
                "WHERE `timestamp` < (NOW() - INTERVAL {} DAY) "
                "AND `id` NOT IN (SELECT `replay_id` FROM `character_saved_replays`)", days);
        }
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

    bool DeserializeMatchDataFromBytes(MatchRecord& record, uint32 arenaTypeId, uint32 typeId, uint32 mapId, std::vector<uint8> const& data)
    {
        record.ArenaTypeId = uint8(arenaTypeId);
        record.TypeId = BattlegroundTypeId(typeId);
        record.MapId = mapId;

        if (data.empty())
            return false;

        ByteBuffer buffer;
        buffer.append(data.data(), data.size());

        if (buffer.size() < 4)
            return false;

        uint32 magic = 0;
        buffer >> magic;

        if (magic != ARENA_REPLAY_V2_MAGIC)
        {
            buffer.rpos(0);
            DeserializeV1Frames(record, buffer);
            return !record.Packets.empty();
        }

        uint32 version = 0;
        buffer >> version;

        if (version != ARENA_REPLAY_V2_VERSION)
        {
            TC_LOG_ERROR("arena.replay", "Unsupported arena replay version {}", version);
            return false;
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

        return !record.Packets.empty();
    }

    bool DeserializeMatchDataFromAcBase32(MatchRecord& record, Field* fields)
    {
        std::string encoded = fields[4].GetString();
        Optional<std::vector<uint8>> decoded = Trinity::Encoding::Base32::Decode(encoded);
        if (!decoded)
            return false;

        uint32 contentSize = fields[3].GetUInt32();
        if (contentSize && decoded->size() != contentSize)
            TC_LOG_ERROR("arena.replay", "Replay contentSize mismatch id={} contentSize={} decodedSize={}",
                fields[0].GetUInt32(), contentSize, uint32(decoded->size()));

        return DeserializeMatchDataFromBytes(record, fields[1].GetUInt32(), fields[2].GetUInt32(), fields[5].GetUInt32(), *decoded);
    }

    std::vector<uint32> LoadLast10Replays()
    {
        std::vector<uint32> replayIds;

        QueryResult result = CharacterDatabase.Query("SELECT `id` FROM `character_arena_replays` ORDER BY `id` DESC LIMIT 10");
        if (!result)
            return replayIds;

        do
        {
            replayIds.push_back((*result)[0].GetUInt32());
        }
        while (result->NextRow());

        return replayIds;
    }

    bool LoadReplayDataForPlayer(Player* player, uint32 matchId, MatchRecord& record)
    {
        // AC-style table only.
        QueryResult result = CharacterDatabase.PQuery(
            "SELECT `id`, `arenaTypeId`, `typeId`, `contentSize`, `contents`, `mapId`, `timesWatched` "
            "FROM `character_arena_replays` WHERE `id` = {} LIMIT 1", matchId);

        if (!result)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Replay data not found in character_arena_replays.");
            return false;
        }

        Field* fields = result->Fetch();
        if (!fields || !DeserializeMatchDataFromAcBase32(record, fields))
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Replay data not found or could not be decoded from character_arena_replays.");
            return false;
        }

        CharacterDatabase.Execute("UPDATE `character_arena_replays` SET `timesWatched` = `timesWatched` + 1 WHERE `id` = {}", matchId);

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
        ChatHandler(player->GetSession()).PSendSysMessage("Replay audit: update=%u compressedUpdate=%u auraPackets=%u auraUpdate=%u auraUpdateAll=%u zeroTimeUpdate=%u actorGuidHits=%u zeroTimeActorGuidHits=%u",
            audit.UpdatePackets, audit.CompressedUpdatePackets, audit.AuraPackets, audit.AuraUpdatePackets, audit.AuraUpdateAllPackets,
            audit.ZeroTimeUpdatePackets, audit.ActorGuidHits, audit.ZeroTimeActorGuidHits);

        if (!audit.AuraPackets)
            ChatHandler(player->GetSession()).PSendSysMessage("Replay aura warning: this replay row has 0 aura packets, so buff/debuff rows cannot show anything. Record a fresh arena after this patch to test aura rows.");

        ChatHandler(player->GetSession()).PSendSysMessage("Replay V69: v68 arena team aura/name-query colors with GetLevel compile fix.");
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

        // Replay entry bypasses the normal battleground queue/join path.
        // Normal BG exit uses Player::GetBattlegroundEntryPoint(), so save the viewer's
        // current world location before teleporting them into the replay instance.
        // Without this, LeaveBattlefield() can return the player to a stale entry point
        // such as Orgrimmar from a previous queue/homebind path.
        player->SetBattlegroundEntryPoint();

        WorldLocation const& replayReturnPos = player->GetBattlegroundEntryPoint();
        TC_LOG_INFO("arena.replay", "Replay saved BG entry point viewer={} map={} x={} y={} z={} o={}",
            viewerLowGuid,
            replayReturnPos.GetMapId(),
            replayReturnPos.GetPositionX(),
            replayReturnPos.GetPositionY(),
            replayReturnPos.GetPositionZ(),
            replayReturnPos.GetOrientation());

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

    void SaveReplay(Battleground* bg, uint32 winner)
    {
        if (!bg)
            return;

        if (!sConfigMgr->GetBoolDefault("ArenaReplay.Enable", true))
            return;

        if (!bg->isRated() && !sConfigMgr->GetBoolDefault("ArenaReplay.SaveUnratedArenas", true))
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

        uint32 durationMs = match.Packets.empty() ? 0 : match.Packets.back().TimestampMs;
        int32 validArenaSecondsConfig = sConfigMgr->GetIntDefault("ArenaReplay.ValidArenaDuration", 0);
        uint32 validArenaSeconds = validArenaSecondsConfig > 0 ? uint32(validArenaSecondsConfig) : 0;
        if (validArenaSeconds && durationMs < validArenaSeconds * IN_MILLISECONDS)
        {
            TC_LOG_INFO("arena.replay", "Skipped short arena replay instance={} durationMs={} minMs={}",
                bg->GetInstanceID(), durationMs, validArenaSeconds * IN_MILLISECONDS);
            Records.erase(recordItr);
            return;
        }

        ByteBuffer buffer;
        SerializeMatchData(match, buffer);
        std::vector<uint8> rawReplay = buffer.contentsAsVector();
        std::string encodedReplay = Trinity::Encoding::Base32::Encode(rawReplay);

        uint32 winnerTeam = NormalizeReplayWinnerTeam(match, winner);
        uint32 loserTeam = bg->GetOtherTeam(winnerTeam);

        std::string winnerPlayerGuids = JoinActorGuidList(match, winnerTeam);
        std::string loserPlayerGuids = JoinActorGuidList(match, loserTeam);

        std::string winnerTeamName;
        std::string loserTeamName;
        uint32 winnerTeamRating = 0;
        uint32 loserTeamRating = 0;
        uint32 winnerTeamMMR = 0;
        uint32 loserTeamMMR = 0;

        GetReplayTeamInfo(bg, match, winnerTeam, winnerTeamName, winnerTeamRating, winnerTeamMMR);
        GetReplayTeamInfo(bg, match, loserTeam, loserTeamName, loserTeamRating, loserTeamMMR);

        uint32 predictedReplayId = GetNextArenaReplayIdForMessage();

        CharacterDatabase.Execute(
            "INSERT INTO `character_arena_replays` "
            "(`arenaTypeId`, `typeId`, `contentSize`, `contents`, `mapId`, "
            "`winnerTeamName`, `winnerTeamRating`, `winnerTeamMMR`, "
            "`loserTeamName`, `loserTeamRating`, `loserTeamMMR`, "
            "`winnerPlayerGuids`, `loserPlayerGuids`) "
            "VALUES ({}, {}, {}, '{}', {}, '{}', {}, {}, '{}', {}, {}, '{}', '{}')",
            uint32(match.ArenaTypeId),
            uint32(match.TypeId),
            uint32(rawReplay.size()),
            encodedReplay,
            bg->GetMapId(),
            EscapeReplaySqlString(winnerTeamName),
            winnerTeamRating,
            winnerTeamMMR,
            EscapeReplaySqlString(loserTeamName),
            loserTeamRating,
            loserTeamMMR,
            EscapeReplaySqlString(winnerPlayerGuids),
            EscapeReplaySqlString(loserPlayerGuids));

        for (auto const& bgPlayer : bg->GetPlayers())
        {
            Player* player = bg->_GetPlayer(bgPlayer.first, bgPlayer.second.OfflineRemoveTime != 0, "arena replay save message");
            if (player && player->GetSession())
            {
                if (predictedReplayId)
                    ChatHandler(player->GetSession()).PSendSysMessage("Replay saved. Match ID: %u", predictedReplayId);
                else
                    ChatHandler(player->GetSession()).PSendSysMessage("Replay saved.");
            }
        }

        TC_LOG_INFO("arena.replay", "Saved arena replay AC-style instance={} predictedId={} map={} arenaType={} packets={} actors={} preStartPackets={} rawBytes={} base32Bytes={} winner='{}' loser='{}'",
            bg->GetInstanceID(), predictedReplayId, bg->GetMapId(), uint32(match.ArenaTypeId), uint32(match.Packets.size()),
            uint32(match.Actors.size()), match.PreStartPacketCount, uint32(rawReplay.size()), uint32(encodedReplay.size()), winnerTeamName, loserTeamName);

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

        if (!state.SentReplayASInitial)
            SendReplayASInitial(viewer, state);

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
            SendReplayASForPlaybackPacket(viewer, out, state.Match);

            TC_LOG_DEBUG("arena.replay", "PLAY viewer={} opcode={} due={} now={} late={} cursor={}/{} size={}",
                viewerLowGuid, out.GetOpcode(), frame.TimestampMs, elapsedMs,
                elapsedMs >= frame.TimestampMs ? elapsedMs - frame.TimestampMs : 0,
                uint32(state.Cursor), uint32(state.Match.Packets.size()), uint32(out.size()));

            ++state.Cursor;
            ++sentThisUpdate;
        }

        if (state.Cursor > 0)
            MaybeSendReplayOverheadNameColorPackets(viewer, state, nowMs);

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

    void OnConfigLoad(bool /*reload*/) override
    {
        DeleteOldArenaReplaysFromConfig();
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        // V6+ intentionally uses PlayerScript::OnUpdate for playback. WorldScript is left registered for
        // cleanup-on-config-load and so existing script registration stays harmless if another patch expects this name.
    }
};

class BGReplayBGScript : public BattlegroundScript
{
public:
    BGReplayBGScript() : BattlegroundScript("BGReplayBGScript") { }

    void OnBattlegroundEnd(Battleground* bg, uint32 winner) override
    {
        if (!bg || !bg->isArena())
            return;

        if (!bg->IsReplay())
            SaveReplay(bg, winner);
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

enum ReplayGossipActions : uint32
{
    REPLAY_GOSSIP_MATCH_ID = 900000001,
    REPLAY_GOSSIP_FAVORITE_MATCH_ID = 900000002,
    REPLAY_GOSSIP_MY_FAVORITES = 900000003,
    REPLAY_GOSSIP_BACK = 900000004
};

class ReplayGossip : public CreatureScript
{
public:
    ReplayGossip() : CreatureScript("ReplayGossip") { }

    struct replayAI : public ScriptedAI
    {
        replayAI(Creature* creature) : ScriptedAI(creature) { }

        void ShowMainMenu(Player* player)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Replay a Match ID", GOSSIP_SENDER_MAIN, REPLAY_GOSSIP_MATCH_ID, "Enter replay match ID", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Favorite a Match ID", GOSSIP_SENDER_MAIN, REPLAY_GOSSIP_FAVORITE_MATCH_ID, "Enter replay match ID to favorite", 0, true);
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, "My favorite matches", GOSSIP_SENDER_MAIN, REPLAY_GOSSIP_MY_FAVORITES);

            std::vector<uint32> matchIds = LoadLast10Replays();
            if (matchIds.empty())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No replays found.", GOSSIP_SENDER_MAIN, REPLAY_GOSSIP_BACK);
            else
            {
                for (uint32 matchId : matchIds)
                    AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay match " + std::to_string(matchId), GOSSIP_SENDER_MAIN, matchId);
            }

            SendGossipMenuFor(player, 1775757, me->GetGUID());
        }

        bool OnGossipHello(Player* player) override
        {
            ShowMainMenu(player);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 action = GetGossipActionFor(player, gossipListId);
            player->PlayerTalkClass->ClearMenus();

            if (action == REPLAY_GOSSIP_MY_FAVORITES)
            {
                std::vector<uint32> matchIds = LoadSavedReplayIds(player->GetGUID().GetCounter());
                if (matchIds.empty())
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No favorite replays found.", GOSSIP_SENDER_MAIN, REPLAY_GOSSIP_BACK);
                else
                    for (uint32 matchId : matchIds)
                        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Replay favorite match " + std::to_string(matchId), GOSSIP_SENDER_MAIN, matchId);

                AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Back", GOSSIP_SENDER_MAIN, REPLAY_GOSSIP_BACK);
                SendGossipMenuFor(player, 1775757, me->GetGUID());
                return true;
            }

            if (action == REPLAY_GOSSIP_BACK)
            {
                ShowMainMenu(player);
                return true;
            }

            StartReplay(player, action);
            CloseGossipMenuFor(player);
            return true;
        }

        bool OnGossipSelectCode(Player* player, uint32 /*menuId*/, uint32 gossipListId, char const* code) override
        {
            uint32 action = GetGossipActionFor(player, gossipListId);
            player->PlayerTalkClass->ClearMenus();

            if (!code || !*code)
            {
                CloseGossipMenuFor(player);
                return false;
            }

            uint32 replayId = 0;
            try
            {
                replayId = uint32(std::stoul(code));
            }
            catch (...)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Invalid replay match ID.");
                CloseGossipMenuFor(player);
                return false;
            }

            if (action == REPLAY_GOSSIP_MATCH_ID)
            {
                StartReplay(player, replayId);
                CloseGossipMenuFor(player);
                return true;
            }

            if (action == REPLAY_GOSSIP_FAVORITE_MATCH_ID)
            {
                FavoriteReplayForPlayer(player, replayId);
                CloseGossipMenuFor(player);
                return true;
            }

            CloseGossipMenuFor(player);
            return false;
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
