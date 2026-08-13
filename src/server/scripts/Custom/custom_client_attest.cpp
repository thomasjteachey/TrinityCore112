/*
 * Enforcement hooks for client-tweaks attestation. The actual logic (token
 * validation, per-player state, the kick decision) lives in
 * game/Server/ClientTweaksAttest.{h,cpp}, which the chat handler also calls.
 * This file is only the ScriptMgr wiring:
 *
 *   - OnLogin  opens the per-player grace window.
 *   - OnLogout drops the player's tracking.
 *   - OnUpdate runs the periodic sweep that kicks the unattested.
 *
 * The sweep lives in WorldScript::OnUpdate because script objects are SINGLETONS
 * - a timer member on a PlayerScript would accumulate every player's diff.
 */

#include "ClientTweaksAttest.h"
#include "Player.h"
#include "ScriptMgr.h"

namespace
{
    constexpr uint32 SWEEP_INTERVAL_MS = 5000;   // check for the unattested every 5s
}

class client_attest_playerscript : public PlayerScript
{
public:
    client_attest_playerscript() : PlayerScript("client_attest_playerscript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        ClientTweaksAttest::OnLogin(player);
    }

    void OnLogout(Player* player) override
    {
        ClientTweaksAttest::OnLogout(player);
    }
};

class client_attest_worldscript : public WorldScript
{
public:
    client_attest_worldscript() : WorldScript("client_attest_worldscript") { }

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < SWEEP_INTERVAL_MS)
            return;
        _timer = 0;

        if (ClientTweaksAttest::Enabled())
            ClientTweaksAttest::Sweep();
    }

private:
    uint32 _timer = 0;
};

void AddSC_custom_client_attest()
{
    new client_attest_playerscript();
    new client_attest_worldscript();
}
