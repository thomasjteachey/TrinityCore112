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

#include "BotPvePower.h"
#include "Config.h"
#include "Log.h"
#include "Player.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Written on the world thread when the config loads, read from every map
    // update thread.
    std::atomic<bool> s_enabled{ true };
    std::atomic<uint32> s_startLevel{ 30 };
    std::atomic<uint32> s_fullLevel{ 50 };
    std::atomic<uint32> s_donePctPerStack{ 15 };
    std::atomic<uint32> s_takenCutPctPerStack{ 8 };
    std::atomic<uint32> s_maxStacks{ 5 };
    std::atomic<uint32> s_halfLifeSeconds{ 3600 };

    struct Entry
    {
        float stacks = 0.0f;
        Clock::time_point at;
    };

    // Deaths are written from the dying bot's map thread and damage reads from
    // every map thread, so the table is shared. Reads vastly outnumber writes
    // (one write per death, a read per bot-versus-wildlife blow).
    std::shared_mutex s_lock;
    std::unordered_map<uint64, Entry> s_entries;

    float Decayed(Entry const& entry, Clock::time_point now)
    {
        uint32 const halfLife = s_halfLifeSeconds.load(std::memory_order_relaxed);
        if (!halfLife || entry.stacks <= 0.0f)
            return entry.stacks;

        float const elapsed = std::chrono::duration<float>(now - entry.at).count();
        return entry.stacks * std::exp2(-elapsed / float(halfLife));
    }

    // 0 below StartLevel, 1 from FullLevel, linear between.
    float LevelScale(uint8 level)
    {
        uint32 const start = s_startLevel.load(std::memory_order_relaxed);
        uint32 const full = s_fullLevel.load(std::memory_order_relaxed);
        if (full <= start)
            return level >= full ? 1.0f : 0.0f;

        return std::clamp((float(level) - float(start)) / float(full - start), 0.0f, 1.0f);
    }

    // Stacks after decay, already scaled by the bot's CURRENT level.
    float ScaledStacks(Player const* bot)
    {
        if (!bot || !s_enabled.load(std::memory_order_relaxed))
            return 0.0f;

        float const scale = LevelScale(bot->GetLevel());
        if (scale <= 0.0f)
            return 0.0f;

        float stacks = 0.0f;
        {
            std::shared_lock<std::shared_mutex> guard(s_lock);
            auto const itr = s_entries.find(bot->GetGUID().GetRawValue());
            if (itr == s_entries.end())
                return 0.0f;
            stacks = Decayed(itr->second, Clock::now());
        }
        return stacks * scale;
    }
}

namespace BotPvePower
{
    void LoadConfig()
    {
        bool const enabled = sConfigMgr->GetBoolDefault("Centurion.Playerbot.PvePower.Enable", true);
        uint32 const startLevel = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Playerbot.PvePower.StartLevel", 30), 1, 255));
        uint32 const fullLevel = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Playerbot.PvePower.FullLevel", 50), 1, 255));
        uint32 const donePct = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Playerbot.PvePower.DamageDonePctPerDeath", 15)));
        uint32 const takenCutPct = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Playerbot.PvePower.DamageTakenCutPctPerDeath", 8), 0, 95));
        uint32 const maxStacks = uint32(std::max(1, sConfigMgr->GetIntDefault("Centurion.Playerbot.PvePower.MaxDeaths", 5)));
        uint32 const halfLifeMinutes = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Playerbot.PvePower.HalfLifeMinutes", 60)));

        s_enabled.store(enabled, std::memory_order_relaxed);
        s_startLevel.store(startLevel, std::memory_order_relaxed);
        s_fullLevel.store(fullLevel, std::memory_order_relaxed);
        s_donePctPerStack.store(donePct, std::memory_order_relaxed);
        s_takenCutPctPerStack.store(takenCutPct, std::memory_order_relaxed);
        s_maxStacks.store(maxStacks, std::memory_order_relaxed);
        s_halfLifeSeconds.store(halfLifeMinutes * 60, std::memory_order_relaxed);

        TC_LOG_INFO("server.loading", "Bot PvE power: {}, levels {}-{}, per death +{}% damage dealt / -{}% damage taken, up to {} deaths, half-life {} min.",
            enabled ? "on" : "off", startLevel, fullLevel, donePct, takenCutPct, maxStacks, halfLifeMinutes);
    }

    bool IsEnabled()
    {
        return s_enabled.load(std::memory_order_relaxed);
    }

    void RecordDeath(Player const* bot)
    {
        if (!bot || !IsEnabled())
            return;

        Clock::time_point const now = Clock::now();
        float const cap = float(s_maxStacks.load(std::memory_order_relaxed));
        float stacks = 0.0f;
        {
            std::unique_lock<std::shared_mutex> guard(s_lock);

            // Bounded by the bot roster, but a long uptime should not keep a row
            // for every bot that ever died once and has long since recovered.
            if (s_entries.size() > 1024)
                for (auto itr = s_entries.begin(); itr != s_entries.end();)
                    itr = Decayed(itr->second, now) < 0.01f ? s_entries.erase(itr) : std::next(itr);

            Entry& entry = s_entries[bot->GetGUID().GetRawValue()];
            stacks = std::min(cap, Decayed(entry, now) + 1.0f);
            entry.stacks = stacks;
            entry.at = now;
        }

        TC_LOG_INFO("playerbots.pve", "Bot {} PvE power now {:.1f} death(s): +{}% damage to wildlife, takes {}% of its damage at level {}.",
            bot->GetName(), stacks, GetDamageDoneBonusPct(bot), GetDamageTakenPct(bot), uint32(bot->GetLevel()));
    }

    float GetStacks(Player const* bot)
    {
        if (!bot || !IsEnabled())
            return 0.0f;

        std::shared_lock<std::shared_mutex> guard(s_lock);
        auto const itr = s_entries.find(bot->GetGUID().GetRawValue());
        return itr == s_entries.end() ? 0.0f : Decayed(itr->second, Clock::now());
    }

    uint32 GetDamageDoneBonusPct(Player const* bot)
    {
        float const stacks = ScaledStacks(bot);
        if (stacks <= 0.0f)
            return 0;
        return uint32(std::lround(stacks * float(s_donePctPerStack.load(std::memory_order_relaxed))));
    }

    uint32 GetDamageTakenPct(Player const* bot)
    {
        float const stacks = ScaledStacks(bot);
        if (stacks <= 0.0f)
            return 100;

        // Never below 5%: a bot that cannot be hurt at all would stand in a
        // camp forever and drain it, which is its own kind of broken.
        long const cut = std::lround(stacks * float(s_takenCutPctPerStack.load(std::memory_order_relaxed)));
        return uint32(100 - std::clamp<long>(cut, 0, 95));
    }
}
