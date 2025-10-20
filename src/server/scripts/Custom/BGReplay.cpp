// BGReplay.cpp ? Smooth playback + GUID remap (ghost self)
// Minimal, DB-free, TC-compatible. No <random>, no DB macros, no gossip.
// Hooks: ServerScript::OnPacketSend + BattlegroundScript::OnBattlegroundUpdate/End
// Requirements in core:
//  - Battleground::IsReplay() and Battleground::GetReplayId() (spectator low GUID)
//  - Battleground remains running while replaying (we only pace and send packets)
//
// What this file does:
//  ? Record from one fixed recorder per team (no source flipping)
//  ? Filter out client-origin MSG_MOVE_* (only server-authoritative SMSG_*)
//  ? Pace playback with a per-tick send cap to avoid teleport bursts
//  ? Remap spectator's own GUID to a "ghost" GUID on playback (mask-preserving),
//    so you can watch your own match without hijacking your live avatar

#include "ScriptMgr.h"
#include "Battleground.h"
#include "Chat.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstring> // memcpy
#include <zlib.h>

// ============================= WATCH/LISTS ==============================
// Only server-authoritative packets; remove MSG_MOVE_* to avoid client/server fights
static std::unordered_set<uint16> const kWatch = {
    SMSG_BATTLEGROUND_PLAYER_JOINED,
    SMSG_BATTLEGROUND_PLAYER_LEFT,
    SMSG_NOTIFICATION,
    SMSG_AURA_UPDATE,
    SMSG_WORLD_STATE_UI_TIMER_UPDATE,
    SMSG_COMPRESSED_UPDATE_OBJECT,
    SMSG_AURA_UPDATE_ALL,
    SMSG_NAME_QUERY_RESPONSE,
    SMSG_DESTROY_OBJECT,
    SMSG_MONSTER_MOVE,
    SMSG_PERIODICAURALOG,
    SMSG_ARENA_UNIT_DESTROYED,
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

static std::unordered_set<uint16> const kRewrite = kWatch; // safe rewrite set

static inline bool IsWatched(uint16 op)
{
    return kWatch.find(op) != kWatch.end();
}

// ============================= DATA TYPES ===============================
struct PacketRecord { uint32 timestamp; uint16 opcode; std::string payload; };
struct MatchRecord {
    BattlegroundTypeId typeId = BATTLEGROUND_AA; uint8 arenaTypeId = 0; uint32 mapId = 0; std::deque<PacketRecord> packets;
    ObjectGuid teamRecorder[2]; // fixed per-team recorder (TEAM_ALLIANCE=0, TEAM_HORDE=1)
};

// Live recordings keyed by BG instance
static std::unordered_map<uint32, MatchRecord> gRecords;
// Active playback queues keyed by BG instance (replay BG instance id)
static std::unordered_map<uint32, std::deque<PacketRecord>> gPlayback;
// Per-replay GUID remap (selfLow -> ghostLow) keyed by BG instance
static std::unordered_map<uint32, uint64> gGhostLow;
static std::unordered_map<uint32, bool>   gGhostActive;

// ============================= GUID HELPERS =============================
static inline uint8 BuildPackedMask(uint64 low)
{
    uint8 mask = 0; for (int i = 0; i < 8; ++i) if (((low >> (i * 8)) & 0xFF) != 0) mask |= (1u << i); return mask;
}

static std::string BuildPackedBytes(uint64 low, uint8 mask)
{
    std::string out; out.reserve(1 + 8); out.push_back((char)mask); for (int i = 0; i < 8; ++i) if (mask & (1u << i)) out.push_back((char)((low >> (i * 8)) & 0xFF)); return out;
}

static std::string BuildUnpackedBytes(uint64 low)
{
    std::string out; out.resize(8); for (int i = 0; i < 8; ++i) out[i] = (char)((low >> (i * 8)) & 0xFF); return out;
}

// Simple deterministic byte-mixer (no <random> required)
static inline uint8 MixByte(uint64 seed, int lane)
{
    uint64 x = seed ^ (0x9E3779B97F4A7C15ull + (uint64)lane * 0xBF58476D1CE4E5B9ull);
    x ^= (x >> 30); x *= 0xBF58476D1CE4E5B9ull; x ^= (x >> 27); x *= 0x94D049BB133111EBull; x ^= (x >> 31);
    uint8 b = (uint8)(x & 0xFFu); if (b == 0) b = 0x7F; return b;
}

static uint64 MakeRemappedGuidPreservingMask(uint64 src, uint64 avoid = 0)
{
    if (!src) return 0; uint64 out = 0; uint64 seed = (uint64)getMSTime() ^ (src * 0xD6E8FEB86659FD93ull);
    for (int i = 0; i < 8; ++i) {
        uint8 s = (uint8)((src >> (i * 8)) & 0xFF); uint8 b = 0; if (s != 0) {
            b = MixByte(seed, i); if (b == s) b ^= 0xA5; if (b == 0) b = 0x55;
        }
        out |= ((uint64)b) << (i * 8);
    }
    if (out == src || out == avoid) { // tweak one nonzero lane
        for (int i = 0; i < 8; ++i) if (((src >> (i * 8)) & 0xFF) != 0) { uint8 b = ((out >> (i * 8)) & 0xFF) ^ 0x3C; if (!b) b = 0x21; out &= ~((uint64)0xFF << (i * 8)); out |= ((uint64)b) << (i * 8); break; }
    }
    return out;
}

static inline void ReplaceAll(std::string& buf, std::string const& a, std::string const& b)
{
    if (a.empty() || a == b) return; size_t p = 0; while ((p = buf.find(a, p)) != std::string::npos) { buf.replace(p, a.size(), b); p += b.size(); }
}

static void RemapGuidInPayload(std::string& payload, uint64 src, uint64 dst)
{
    if (!src || src == dst) return; uint8 mask = BuildPackedMask(src);
    std::string packedSrc = BuildPackedBytes(src, mask), packedDst = BuildPackedBytes(dst, mask); ReplaceAll(payload, packedSrc, packedDst);
    std::string rawSrc = BuildUnpackedBytes(src), rawDst = BuildUnpackedBytes(dst); ReplaceAll(payload, rawSrc, rawDst);
}

static bool DecompressZlib(uint8 const* src, size_t srcSize, std::string& out)
{
    uLongf destLen = (uLongf)(srcSize * 8 + 4096); std::vector<uint8> buf(destLen);
    int r = uncompress(buf.data(), &destLen, src, (uLong)srcSize);
    if (r == Z_BUF_ERROR) { destLen = (uLongf)(srcSize * 16 + 8192); buf.assign(destLen, 0); r = uncompress(buf.data(), &destLen, src, (uLong)srcSize); }
    if (r != Z_OK) return false; out.assign((char const*)buf.data(), destLen); return true;
}

static WorldPacket BuildPacket(uint16 opcode, std::string const& payload)
{
    WorldPacket p(opcode, payload.size()); if (!payload.empty()) p.append(payload.data(), payload.size()); return p;
}

// ============================= SERVER SCRIPT ============================
class BGReplayServerScript : public ServerScript {
public:
    BGReplayServerScript() : ServerScript("BGReplayServerScript") {}

    void OnPacketSend(WorldSession* session, WorldPacket& packet) override {
        if (!session) return; Player* player = session->GetPlayer(); if (!player) return; Battleground* bg = player->GetBattleground();
        if (!bg || bg->IsReplay()) return; // don't record replays or non-BG
        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS) return;

        uint16 op = packet.GetOpcode(); if (!IsWatched(op)) return;

        // Fixed single recorder per team
        TeamId t = (player->GetBGTeam() == ALLIANCE) ? TEAM_ALLIANCE : TEAM_HORDE;
        MatchRecord& rec = gRecords[bg->GetInstanceID()];
        if (!rec.teamRecorder[t]) rec.teamRecorder[t] = player->GetGUID();
        if (rec.teamRecorder[t] != player->GetGUID()) return;

        uint32 ts = bg->GetStartTime();
        rec.typeId = (bg->GetTypeID(false) == BATTLEGROUND_AA) ? bg->GetTypeID(true) : bg->GetTypeID(false);
        rec.arenaTypeId = bg->GetArenaType(); rec.mapId = bg->GetMapId();

        std::string payload; if (packet.size() > 0) payload.assign((char const*)packet.contents(), packet.size());
        gRecords[bg->GetInstanceID()].packets.push_back({ ts, op, std::move(payload) });
    }
};

// =========================== BATTLEGROUND SCRIPT ========================
class BGReplayBGScript : public BattlegroundScript {
public:
    BGReplayBGScript() : BattlegroundScript("BGReplayBGScript") {}

    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override {
        if (!bg || !bg->IsReplay()) return; if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS) return;

        // Ensure we have a playback queue for this replay instance; if not, try to adopt from a recorded instance with same map/type
        if (gPlayback.find(bg->GetInstanceID()) == gPlayback.end()) {
            // naive adoption: find any record and move it in (your launcher should fill this explicitly)
            for (auto it = gRecords.begin(); it != gRecords.end(); ++it) {
                gPlayback[bg->GetInstanceID()] = it->second.packets; // copy
                break;
            }
        }
        auto pit = gPlayback.find(bg->GetInstanceID()); if (pit == gPlayback.end()) return; auto& stream = pit->second; if (stream.empty()) return;

        uint32 spectatorLow = bg->GetReplayId(); Player* spectator = ObjectAccessor::FindPlayerByLowGUID(spectatorLow); if (!spectator) return; WorldSession* sess = spectator->GetSession(); if (!sess) return;

        if (!gGhostActive[bg->GetInstanceID()]) { gGhostLow[bg->GetInstanceID()] = MakeRemappedGuidPreservingMask((uint64)spectatorLow); gGhostActive[bg->GetInstanceID()] = true; }
        uint64 selfLow = spectatorLow, ghostLow = gGhostLow[bg->GetInstanceID()];

        // pace
        uint32 sent = 0; static constexpr uint32 kMaxPerTick = 300; uint32 nowTs = bg->GetStartTime();
        while (!stream.empty() && stream.front().timestamp <= nowTs && sent < kMaxPerTick)
        {
            PacketRecord pr = std::move(stream.front()); stream.pop_front();
            std::string payload = std::move(pr.payload);

            if (pr.opcode == SMSG_COMPRESSED_UPDATE_OBJECT)
            {
                if (payload.size() >= sizeof(uint32))
                {
                    uint32 compSize = 0; std::memcpy(&compSize, payload.data(), sizeof(uint32));
                    if (payload.size() >= sizeof(uint32) + compSize)
                    {
                        std::string decomp; if (DecompressZlib((uint8 const*)payload.data() + sizeof(uint32), compSize, decomp))
                        {
                            RemapGuidInPayload(decomp, selfLow, ghostLow); WorldPacket out = BuildPacket(SMSG_UPDATE_OBJECT, decomp); sess->SendPacket(&out); ++sent; continue;
                        }
                    }
                }
                ++sent; continue; // drop if we can't safely handle
            }

            if (kRewrite.count(pr.opcode)) RemapGuidInPayload(payload, selfLow, ghostLow);
            WorldPacket out = BuildPacket(pr.opcode, payload); sess->SendPacket(&out); ++sent;
        }

        if (stream.empty()) { gPlayback.erase(pit); gGhostLow.erase(bg->GetInstanceID()); gGhostActive.erase(bg->GetInstanceID()); }
    }

    void OnBattlegroundEnd(Battleground* bg, uint32 /*winner*/) override {
        if (!bg) return; gRecords.erase(bg->GetInstanceID()); gPlayback.erase(bg->GetInstanceID()); gGhostLow.erase(bg->GetInstanceID()); gGhostActive.erase(bg->GetInstanceID());
    }
};

// =========================== SCRIPT REGISTRATION ========================
void AddBGReplayScripts() { new BGReplayServerScript(); new BGReplayBGScript(); }
extern "C" void AddArenaReplayScripts() { AddBGReplayScripts(); }
