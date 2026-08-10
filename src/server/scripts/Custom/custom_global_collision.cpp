/*
 * Server-wide collision: give everyone a collision aura automatically.
 *
 * Rather than teaching the client and the bot code about a separate "global
 * collision" mode, the server just applies the configured aura to every player.
 * Both halves then work with no further changes:
 *
 *   - humans    : their client already reads the aura and clips their own
 *                 movement (movement is client-authoritative)
 *   - playerbots: PlayerCollisionServer already registers the aura, and the bot
 *                 movement code already clips destinations against it
 *
 * So the server-wide mode is the exact same mechanic players can be given
 * individually - there is no second code path to drift from the first.
 *
 * Config (worldserver.conf), all live-reloadable with `.reload config`:
 *
 *   PlayerCollision.GlobalAura = 90211        spell id applied to everyone;
 *                                             0 disables the feature entirely
 *   PlayerCollision.GlobalAura.CheckSeconds = 5   how often to re-assert it
 *
 * Rotating the weekly mode is a one-line config change:
 *   90210 Obstruction   enemies cannot pass you (one-way)
 *   90211 Immovable     nobody can pass you (one-way)
 *   90212 Bodycheck     mutual collision with enemies
 *   90213 Solid Form    mutual collision with everyone
 *
 * Two details worth knowing:
 *
 * The aura is re-asserted on a sweep rather than applied once at login, because
 * auras come off on death and can be stripped by any number of effects; a
 * login-only hook would quietly stop applying. The sweep lives in
 * WorldScript::OnUpdate because script objects are SINGLETONS - a timer member
 * on a PlayerScript would accumulate every player's diff and fire far too often.
 *
 * Rotation cleans up after itself: any of the four collision auras that is not
 * the configured one is stripped, so last week's mode does not linger. Setting
 * the config to 0 therefore removes the buff from everyone within one sweep.
 *
 * NOTE: this applies to players (including playerbots, which are Player objects).
 * Making creatures solid would need a different mechanism - there is no cheap
 * global creature list and UnitScript has no update hook - and it changes much
 * more (doorways, pets, escorts), so it is deliberately out of scope.
 */

#include "ScriptMgr.h"
#include "Config.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
    // Every aura this feature knows how to apply. Anything here that is not the
    // configured id gets stripped, which is what makes a rotation clean.
    constexpr uint32 KnownCollisionAuras[] = { 90210, 90211, 90212, 90213 };

    uint32 GlobalAuraId()
    {
        int32 const id = sConfigMgr->GetIntDefault("PlayerCollision.GlobalAura", 0);
        return id > 0 ? uint32(id) : 0u;
    }

    uint32 CheckIntervalMs()
    {
        int32 seconds = sConfigMgr->GetIntDefault("PlayerCollision.GlobalAura.CheckSeconds", 5);
        if (seconds < 1)
            seconds = 1;
        return uint32(seconds) * IN_MILLISECONDS;
    }

    void EnsureAura(Player* player, uint32 wanted)
    {
        if (!player || !player->IsInWorld())
            return;

        // Strip a stale mode first, so rotating the config (or clearing it)
        // takes effect rather than stacking.
        for (uint32 other : KnownCollisionAuras)
            if (other != wanted && player->HasAura(other))
                player->RemoveAurasDueToSpell(other);

        if (!wanted || !player->IsAlive())
            return;
        if (player->HasAura(wanted))
            return;

        player->AddAura(wanted, player);
    }
}

// Immediate application, so a player is solid the moment they arrive rather than
// up to one sweep later.
class global_collision_playerscript : public PlayerScript
{
public:
    global_collision_playerscript() : PlayerScript("global_collision_playerscript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (uint32 const wanted = GlobalAuraId())
            EnsureAura(player, wanted);
    }
};

// The sweep. One instance, one diff stream - the right place for a timer.
class global_collision_worldscript : public WorldScript
{
public:
    global_collision_worldscript() : WorldScript("global_collision_worldscript") { }

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < CheckIntervalMs())
            return;
        _timer = 0;

        uint32 const wanted = GlobalAuraId();

        // When disabled we still sweep once to strip whatever we applied before,
        // then go quiet until the config comes back.
        if (!wanted && _cleared)
            return;

        for (auto const& pair : sWorld->GetAllSessions())
        {
            if (WorldSession* session = pair.second)
                if (Player* player = session->GetPlayer())
                    EnsureAura(player, wanted);
        }

        _cleared = (wanted == 0);
    }

private:
    uint32 _timer = 0;
    bool   _cleared = false;
};

void AddSC_custom_global_collision()
{
    new global_collision_playerscript();
    new global_collision_worldscript();
}
