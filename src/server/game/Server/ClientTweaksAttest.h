/*
 * Client-tweaks attestation: the anti-cheat handshake that verifies a player is
 * running the mandatory client-tweaks DLL (dinput8.dll). See ClientTweaksAttest.cpp.
 */

#ifndef CLIENT_TWEAKS_ATTEST_H
#define CLIENT_TWEAKS_ATTEST_H

#include "Define.h"
#include <string>

class Player;

namespace ClientTweaksAttest
{
    // Live config gate. When false nothing is enforced and no one is kicked.
    bool Enabled();

    // Session lifecycle - drives the per-player login grace window.
    void OnLogin(Player* player);
    void OnLogout(Player* player);

    // Called from the chat handler for every LANG_ADDON message. Returns true if
    // the message was an attestation token this consumed (valid OR invalid), so
    // the caller stops processing it as chat.
    bool HandleToken(Player* player, uint32 type, uint32 lang, std::string const& msg);

    // Periodic sweep: kicks players who have not produced a valid token within
    // the grace/timeout windows. Call from a WorldScript update on a timer.
    void Sweep();
}

#endif
