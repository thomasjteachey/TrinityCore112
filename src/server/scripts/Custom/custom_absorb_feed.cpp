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
// Absorb shields on the player's own health bar.
//
// A 3.3.5 client is never told how much a shield has left. SMSG_AURA_UPDATE
// carries flags, level, charges and duration but no effect amounts (Cataclysm
// added those), and the combat log only reports what each hit lost to a shield,
// never what the shield started with. So CENTURION_AbsorbBar cannot work it out
// and the server says it instead:
//
//     CCGAME \t ABS:<total>
//
// <total> is every SCHOOL_ABSORB and MANA_SHIELD effect on the player added up.
// It is sent when the total changes, and again every few seconds while a shield
// is up so that a /reload picks the value back up.
//
// Polled from the player's own update (on the map thread that owns the player)
// rather than from a world-thread sweep: a shield's amount changes inside
// Unit::CalcAbsorbResist on that thread, and walking someone's aura lists from
// another thread is a race. A poll also needs no core edit. Absorbs change at
// several places (apply, recalculate, every hit, scripted absorbs), and hooking
// each of them would mean editing Unit.cpp.
// ---------------------------------------------------------------------------

#include "custom_barracks_hardcore.h"

#include "Chat.h"
#include "Configuration/Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    bool s_enabled = true;
    uint32 s_pollMs = 100;
    uint32 s_refreshMs = 3000;

    struct AbsorbFeedState
    {
        uint32 sinceCheckMs = 0;
        uint32 sinceSendMs = 0;
        int64 lastSent = -1;    // -1 = nothing sent yet, so the first poll always reports
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

    uint32 TotalAbsorb(Player const* player)
    {
        uint64 total = 0;
        for (AuraType type : { SPELL_AURA_SCHOOL_ABSORB, SPELL_AURA_MANA_SHIELD })
            for (AuraEffect const* effect : player->GetAuraEffectsByType(type))
                // Scripted absorbs park a negative amount to mean "the script
                // decides" - there is no number to draw for those.
                if (effect->GetAmount() > 0)
                    total += uint32(effect->GetAmount());

        return uint32(std::min<uint64>(total, std::numeric_limits<uint32>::max()));
    }

    void SendAbsorb(Player* player, uint32 total)
    {
        // Empty-GUID overload: the WorldObject one used to force LANG_UNIVERSAL,
        // which turned addon whispers into visible ones.
        std::string const message = "CCGAME\tABS:" + std::to_string(total);
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

        uint32 total = 0;
        {
            std::lock_guard<std::mutex> guard(s_stateLock);
            AbsorbFeedState& state = s_state[player->GetGUID().GetCounter()];
            state.sinceCheckMs += diff;
            state.sinceSendMs += diff;
            if (state.sinceCheckMs < s_pollMs)
                return;
            state.sinceCheckMs = 0;

            total = TotalAbsorb(player);
            bool const changed = int64(total) != state.lastSent;
            bool const refresh = total > 0 && state.sinceSendMs >= s_refreshMs;
            if (!changed && !refresh)
                return;

            state.lastSent = total;
            state.sinceSendMs = 0;
        }

        SendAbsorb(player, total);
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
