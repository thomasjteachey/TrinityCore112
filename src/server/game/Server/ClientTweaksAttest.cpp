/*
 * Client-tweaks attestation.
 *
 * Movement collision is enforced by each client on ITSELF (the client-tweaks
 * dinput8.dll), so it is only as reliable as client coverage: someone who
 * deletes the DLL simply walks through everyone. This module verifies the DLL
 * is present by a lightweight handshake, and kicks players who cannot prove it.
 *
 * PROTOCOL (proactive, per-player token - the DLL only needs to SEND):
 *   Every ~15s the DLL sends an addon message on the BATTLEGROUND channel:
 *       CCGACK\t<hex>
 *   where <hex> = FNV-1a64( Secret-bytes || guidLow(LE) || guidHigh(LE) ),
 *   16 lowercase hex chars. The GUID binding means a token captured from one
 *   account is useless on another. The server recomputes the expected value and
 *   compares; a match refreshes the player's "last attested" timestamp.
 *
 * ENFORCEMENT:
 *   On login a grace window opens (GraceSeconds). A player who has not produced
 *   a valid token within that window - or who then goes TimeoutSeconds without a
 *   fresh one (they unloaded the DLL after logging in) - is kicked.
 *
 * HONEST LIMITS (by design, documented so no one over-trusts it):
 *   This stops the CASUAL cheat (delete the DLL and walk through people) cold. A
 *   determined attacker can still (a) extract the compiled secret from the DLL
 *   and forge tokens, or (b) capture their own token and replay it. The channel
 *   is encrypted so passive sniffing is hard, and the token is GUID-bound so it
 *   is not transferable, but this is NOT cryptographic anti-cheat. For a
 *   replay-proof version the server would issue a nonce and the DLL would have to
 *   hook the receive path too (challenge-response) - deliberately out of scope.
 *
 * FAIL-OPEN on misconfiguration: if enabled but no Secret is set, it logs an
 * error and enforces nothing rather than kicking the whole realm.
 *
 * Config (worldserver.conf), all live-reloadable with `.reload config`:
 *   ClientTweaks.Attest.Enable         = 0   master switch
 *   ClientTweaks.Attest.Secret         = ""  MUST match the DLL's compiled secret
 *   ClientTweaks.Attest.GraceSeconds   = 60  time after login to first attest
 *   ClientTweaks.Attest.TimeoutSeconds = 90  max gap between valid tokens
 */

#include "ClientTweaksAttest.h"
#include "Common.h"
#include "Config.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldSession.h"

#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct AttestState
    {
        time_t login      = 0;   // when the grace window opened
        time_t lastAttest = 0;   // last VALID token, 0 = never
    };

    std::mutex                                    s_mutex;
    std::unordered_map<ObjectGuid, AttestState>   s_state;

    bool CfgEnabled()          { return sConfigMgr->GetBoolDefault("ClientTweaks.Attest.Enable", false); }
    std::string CfgSecret()    { return sConfigMgr->GetStringDefault("ClientTweaks.Attest.Secret", ""); }
    uint32 CfgGrace()          { int v = sConfigMgr->GetIntDefault("ClientTweaks.Attest.GraceSeconds", 60);   return v < 5 ? 5 : uint32(v); }
    uint32 CfgTimeout()        { int v = sConfigMgr->GetIntDefault("ClientTweaks.Attest.TimeoutSeconds", 90); return v < 10 ? 10 : uint32(v); }

    // FNV-1a 64-bit. Deterministic and byte-identical to the DLL's copy - do NOT
    // "optimise" one side without the other or every token stops matching.
    uint64 Fnv1a(uint8 const* data, size_t n, uint64 h)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= data[i];
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    // The token the DLL should have sent for this exact player, as 16 lowercase
    // hex chars. Mirrors the DLL: hash(secret bytes, then guidLow LE, guidHigh LE).
    std::string ExpectedToken(std::string const& secret, ObjectGuid guid)
    {
        uint64 raw = guid.GetRawValue();
        uint32 lo = uint32(raw & 0xFFFFFFFFu);
        uint32 hi = uint32(raw >> 32);

        uint64 h = 0xcbf29ce484222325ULL;
        h = Fnv1a(reinterpret_cast<uint8 const*>(secret.data()), secret.size(), h);
        uint8 le[8] = {
            uint8(lo), uint8(lo >> 8), uint8(lo >> 16), uint8(lo >> 24),
            uint8(hi), uint8(hi >> 8), uint8(hi >> 16), uint8(hi >> 24)
        };
        h = Fnv1a(le, 8, h);

        char buf[17];
        static char const* const HEX = "0123456789abcdef";
        for (int i = 0; i < 16; ++i)
            buf[15 - i] = HEX[(h >> (i * 4)) & 0xF];
        buf[16] = '\0';
        return std::string(buf);
    }

    // Constant-ish time compare (length-checked; not a timing-attack target here,
    // but avoids early-out surprises).
    bool TokenEquals(std::string const& a, std::string const& b)
    {
        if (a.size() != b.size())
            return false;
        uint8 diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
            diff |= uint8(a[i]) ^ uint8(b[i]);
        return diff == 0;
    }

    bool Exempt(Player* player)
    {
        // Staff are never kicked for this.
        return !player || player->IsGameMaster() ||
               player->GetSession()->GetSecurity() > SEC_PLAYER;
    }
}

namespace ClientTweaksAttest
{
    bool Enabled()
    {
        return CfgEnabled();
    }

    void OnLogin(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(s_mutex);
        AttestState& st = s_state[player->GetGUID()];
        st.login = GameTime::GetGameTime();
        st.lastAttest = 0;
    }

    void OnLogout(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(s_mutex);
        s_state.erase(player->GetGUID());
    }

    bool HandleToken(Player* player, uint32 type, uint32 lang, std::string const& msg)
    {
        if (!player || lang != LANG_ADDON || type != CHAT_MSG_BATTLEGROUND)
            return false;

        std::size_t const sep = msg.find('\t');
        if (sep == std::string::npos || msg.compare(0, sep, "CCGACK") != 0)
            return false;                       // not ours - let other handlers see it

        // It IS an attestation token; consume it regardless of validity.
        std::string const got = msg.substr(sep + 1);

        std::string const secret = CfgSecret();
        if (!secret.empty())
        {
            std::string const want = ExpectedToken(secret, player->GetGUID());
            if (TokenEquals(got, want))
            {
                std::lock_guard<std::mutex> guard(s_mutex);
                s_state[player->GetGUID()].lastAttest = GameTime::GetGameTime();
            }
            else
            {
                TC_LOG_DEBUG("network", "ClientTweaksAttest: bad token from {} ({})",
                    player->GetName(), player->GetGUID().ToString());
            }
        }
        return true;
    }

    void Sweep()
    {
        if (!CfgEnabled())
            return;

        std::string const secret = CfgSecret();
        if (secret.empty())
        {
            // Misconfigured: never kick the whole realm because the secret is
            // missing. Log once in a while and do nothing.
            static time_t s_lastWarn = 0;
            time_t const now = GameTime::GetGameTime();
            if (now - s_lastWarn >= 60)
            {
                s_lastWarn = now;
                TC_LOG_ERROR("server", "ClientTweaks.Attest.Enable is on but "
                    "ClientTweaks.Attest.Secret is empty - attestation disabled "
                    "(set the secret to match the DLL, or turn Enable off).");
            }
            return;
        }

        time_t const now   = GameTime::GetGameTime();
        uint32 const grace = CfgGrace();
        uint32 const tmo   = CfgTimeout();

        // Collect victims under the lock, kick outside it (KickPlayer touches the
        // session and we do not want to hold the map mutex across that).
        std::vector<ObjectGuid> toKick;
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            for (auto const& pair : s_state)
            {
                AttestState const& st = pair.second;
                if (st.login == 0)
                    continue;
                bool const graceOver = (now - st.login) >= time_t(grace);
                if (!graceOver)
                    continue;                    // still has time to attest
                bool const fresh = st.lastAttest != 0 && (now - st.lastAttest) < time_t(tmo);
                if (!fresh)
                    toKick.push_back(pair.first);
            }
        }

        for (ObjectGuid guid : toKick)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player || Exempt(player))
            {
                std::lock_guard<std::mutex> guard(s_mutex);
                s_state.erase(guid);             // gone or exempt: stop tracking
                continue;
            }
            TC_LOG_INFO("server", "ClientTweaksAttest: kicking {} ({}) - no valid "
                "client-tweaks attestation.", player->GetName(), guid.ToString());
            player->GetSession()->KickPlayer("ClientTweaksAttest: client-tweaks required");
            std::lock_guard<std::mutex> guard(s_mutex);
            s_state.erase(guid);
        }
    }
}
