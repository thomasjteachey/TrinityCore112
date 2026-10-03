/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// ---------------------------------------------------------------------------
// Absorb shields on the player's own health bar and on their target's.
//
// A 3.3.5 client is never told how much a shield has left. SMSG_AURA_UPDATE
// carries flags, level, charges and duration but no effect amounts (Cataclysm
// added those), and the combat log only reports what each hit lost to a shield,
// never what the shield started with. So CENTURION_AbsorbBar cannot work it out
// and the server says it instead:
//
//     CCGAME \t ABS:<total>                 the player
//     CCGAME \t ABST:<0xGUID>:<total>       the player's current target
//
// <total> is every SCHOOL_ABSORB and MANA_SHIELD effect on the unit added up.
// The target line carries the target's GUID in the exact form UnitGUID()
// returns, so a value still in flight when the player tabs to someone else is
// recognised as stale and dropped. Each is sent when it changes (the target's
// also when the target itself changes), and again every few seconds while a
// shield is up so that a /reload picks the value back up.
//
// Polled from the player's own update (on the map thread that owns the player)
// rather than from a world-thread sweep: a shield's amount changes inside
// Unit::CalcAbsorbResist on that thread, and walking someone's aura lists from
// another thread is a race. The target is looked up on the player's own map, so
// it is owned by the same thread. A poll also needs no core edit. Absorbs change at
// several places (apply, recalculate, every hit, scripted absorbs), and hooking
// each of them would mean editing Unit.cpp.
// ---------------------------------------------------------------------------

#include "custom_barracks_hardcore.h"

#include "Chat.h"
#include "Configuration/Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    bool s_enabled = true;
    uint32 s_pollMs = 100;
    uint32 s_refreshMs = 3000;

    // What the client was last told on one line (self or target), which is
    // exactly what the addon is drawing for it.
    struct AbsorbChannel
    {
        uint32 sinceSendMs = 0;
        int64 lastSent = -1;    // -1 = nothing sent yet, so the first poll always reports
        ObjectGuid lastGuid;

        bool Due(uint32 total, ObjectGuid guid)
        {
            bool const changed = int64(total) != lastSent || guid != lastGuid;
            bool const refresh = total > 0 && sinceSendMs >= s_refreshMs;
            if (!changed && !refresh)
                return false;

            lastSent = total;
            lastGuid = guid;
            sinceSendMs = 0;
            return true;
        }
    };

    struct AbsorbFeedState
    {
        uint32 sinceCheckMs = 0;
        AbsorbChannel self;
        AbsorbChannel target;
    };

    // Players on different maps update on different threads.
    std::mutex s_stateLock;
    std::unordered_map<ObjectGuid::LowType, AbsorbFeedState> s_state;

    void LoadAbsorbFeedConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.AbsorbFeed.Enable", true);
        s_pollMs = uint32(std::max(50, sConfigMgr->GetIntDefault("Centurion.AbsorbFeed.PollMs", 100)));
        s_refreshMs = uint32(std::max(1000, sConfigMgr->GetIntDefault("Centurion.AbsorbFeed.RefreshMs", 3000)));
    }

    uint32 TotalAbsorb(Unit const* unit)
    {
        uint64 total = 0;
        for (AuraType type : { SPELL_AURA_SCHOOL_ABSORB, SPELL_AURA_MANA_SHIELD })
            for (AuraEffect const* effect : unit->GetAuraEffectsByType(type))
                // Scripted absorbs park a negative amount to mean "the script
                // decides" - there is no number to draw for those.
                if (effect->GetAmount() > 0)
                    total += uint32(effect->GetAmount());

        return uint32(std::min<uint64>(total, std::numeric_limits<uint32>::max()));
    }

    // The player's selection, if it is on their map. Anything the client can
    // target is, and only then does it update on this thread.
    Unit const* FindTarget(Player const* player)
    {
        ObjectGuid const guid = player->GetTarget();
        if (guid.IsEmpty())
            return nullptr;

        Unit const* unit = ObjectAccessor::GetUnit(*player, guid);
        return unit && unit->IsInWorld() ? unit : nullptr;
    }

    // The form UnitGUID() returns: "0x" and sixteen upper-case hex digits.
    std::string GuidText(ObjectGuid guid)
    {
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(guid.GetRawValue()));
        return buffer;
    }

    void SendAbsorbLine(Player* player, std::string const& payload)
    {
        // Empty-GUID overload: the WorldObject one used to force LANG_UNIVERSAL,
        // which turned addon whispers into visible ones.
        std::string const message = "CCGAME\t" + payload;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty,
            ObjectGuid::Empty, message, 0);
        player->SendDirectMessage(&data);
    }
}

class custom_absorb_feed_player : public PlayerScript
{
public:
    custom_absorb_feed_player() : PlayerScript("custom_absorb_feed_player") {}

    void OnUpdate(Player* player, uint32 diff) override
    {
        if (!s_enabled || !player || !player->IsInWorld() || BarracksHardcore::IsPlayerbot(player))
            return;

        bool sendSelf = false;
        bool sendTarget = false;
        uint32 selfTotal = 0;
        uint32 targetTotal = 0;
        ObjectGuid targetGuid;
        {
            std::lock_guard<std::mutex> guard(s_stateLock);
            AbsorbFeedState& state = s_state[player->GetGUID().GetCounter()];
            state.sinceCheckMs += diff;
            state.self.sinceSendMs += diff;
            state.target.sinceSendMs += diff;
            if (state.sinceCheckMs < s_pollMs)
                return;
            state.sinceCheckMs = 0;

            selfTotal = TotalAbsorb(player);
            sendSelf = state.self.Due(selfTotal, ObjectGuid::Empty);

            if (Unit const* target = FindTarget(player))
            {
                targetGuid = target->GetGUID();
                targetTotal = TotalAbsorb(target);
                sendTarget = state.target.Due(targetTotal, targetGuid);
            }
            else
                // Forget the last target, so selecting it again always reports.
                state.target = AbsorbChannel();
        }

        if (sendSelf)
            SendAbsorbLine(player, "ABS:" + std::to_string(selfTotal));
        if (sendTarget)
            SendAbsorbLine(player, "ABST:" + GuidText(targetGuid) + ":" + std::to_string(targetTotal));
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;

        std::lock_guard<std::mutex> guard(s_stateLock);
        s_state.erase(player->GetGUID().GetCounter());
    }
};

class custom_absorb_feed_world : public WorldScript
{
public:
    custom_absorb_feed_world() : WorldScript("custom_absorb_feed_world") {}

    void OnConfigLoad(bool /*reload*/) override { LoadAbsorbFeedConfig(); }
};

void AddSC_custom_absorb_feed()
{
    LoadAbsorbFeedConfig();
    new custom_absorb_feed_player();
    new custom_absorb_feed_world();
}
