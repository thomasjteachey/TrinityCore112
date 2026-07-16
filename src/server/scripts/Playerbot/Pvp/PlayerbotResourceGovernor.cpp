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

#include "PlayerbotResourceGovernor.h"

#include "Common.h"
#include "Configuration/Config.h"
#include "GameTime.h"
#include "Log.h"

#include <algorithm>
#include <atomic>

namespace
{
struct GovernorConfig
{
    bool enabled = true;
    uint32 softUpdateTimeMs = 110;
    uint32 hardUpdateTimeMs = 180;
    uint32 hardSustainMs = 10000;
    uint32 cullCooldownMs = 20000;
    uint32 botShedCooldownMs = 5000;
    uint32 softMatchBotCap = 20;
    uint32 maxTotalCustomMatchBots = 160;
};

GovernorConfig g_GovernorConfig;

// Written on the world thread; read from map threads (lobby gossip handlers),
// so every cross-thread value is atomic. The exponential moving average keeps
// roughly the last few seconds of update time without storing samples.
std::atomic<uint32> g_AverageUpdateMs{ 0 };
std::atomic<uint8> g_PressureLevel{ uint8(playerbot::ResourcePressureLevel::Normal) };
std::atomic<uint32> g_HardPressureSinceMs{ 0 };
std::atomic<uint32> g_LastCullMs{ 0 };
std::atomic<uint32> g_LastBotShedMs{ 0 };

bool IsHardPressureSustained(uint32 nowMs)
{
    uint32 const hardSinceMs = g_HardPressureSinceMs.load(std::memory_order_relaxed);
    return hardSinceMs && nowMs >= hardSinceMs + g_GovernorConfig.hardSustainMs;
}
}

namespace playerbot
{
void ResourceGovernor::LoadConfig()
{
    g_GovernorConfig.enabled = sConfigMgr->GetBoolDefault("Playerbot.Governor.Enable", true);
    g_GovernorConfig.softUpdateTimeMs = sConfigMgr->GetIntDefault("Playerbot.Governor.SoftUpdateTimeMs", 110);
    g_GovernorConfig.hardUpdateTimeMs = std::max<uint32>(sConfigMgr->GetIntDefault("Playerbot.Governor.HardUpdateTimeMs", 180),
        g_GovernorConfig.softUpdateTimeMs + 1);
    g_GovernorConfig.hardSustainMs = sConfigMgr->GetIntDefault("Playerbot.Governor.HardSustainSeconds", 10) * IN_MILLISECONDS;
    g_GovernorConfig.cullCooldownMs = sConfigMgr->GetIntDefault("Playerbot.Governor.CullCooldownSeconds", 20) * IN_MILLISECONDS;
    g_GovernorConfig.botShedCooldownMs = sConfigMgr->GetIntDefault("Playerbot.Governor.BotShedCooldownSeconds", 5) * IN_MILLISECONDS;
    g_GovernorConfig.softMatchBotCap = sConfigMgr->GetIntDefault("Playerbot.Governor.SoftMatchBotCap", 20);
    g_GovernorConfig.maxTotalCustomMatchBots = sConfigMgr->GetIntDefault("Playerbot.Governor.MaxTotalCustomMatchBots", 160);

    TC_LOG_INFO("server.loading",
        "Playerbot resource governor loaded (enabled: {}, soft: {} ms, hard: {} ms, sustain: {} ms, cullCooldown: {} ms, botShedCooldown: {} ms, softMatchBotCap: {}, maxTotalCustomMatchBots: {}).",
        g_GovernorConfig.enabled ? "true" : "false", g_GovernorConfig.softUpdateTimeMs, g_GovernorConfig.hardUpdateTimeMs,
        g_GovernorConfig.hardSustainMs, g_GovernorConfig.cullCooldownMs, g_GovernorConfig.botShedCooldownMs,
        g_GovernorConfig.softMatchBotCap, g_GovernorConfig.maxTotalCustomMatchBots);
}

void ResourceGovernor::NoteWorldUpdate(uint32 diffMs)
{
    if (!g_GovernorConfig.enabled)
        return;

    // EMA with 1/8 weight: reacts within a few seconds without flapping on a
    // single long tick (loading screens, database hiccups).
    uint32 const previousAverage = g_AverageUpdateMs.load(std::memory_order_relaxed);
    uint32 const newAverage = previousAverage == 0
        ? diffMs
        : previousAverage + (int32(diffMs) - int32(previousAverage)) / 8;
    g_AverageUpdateMs.store(newAverage, std::memory_order_relaxed);

    ResourcePressureLevel level = ResourcePressureLevel::Normal;
    if (newAverage >= g_GovernorConfig.hardUpdateTimeMs)
        level = ResourcePressureLevel::Hard;
    else if (newAverage >= g_GovernorConfig.softUpdateTimeMs)
        level = ResourcePressureLevel::Soft;

    uint8 const previousLevel = g_PressureLevel.exchange(uint8(level), std::memory_order_relaxed);
    uint32 const nowMs = GameTime::GetGameTimeMS();

    if (level == ResourcePressureLevel::Hard)
    {
        uint32 expected = 0;
        g_HardPressureSinceMs.compare_exchange_strong(expected, nowMs ? nowMs : 1, std::memory_order_relaxed);
    }
    else
        g_HardPressureSinceMs.store(0, std::memory_order_relaxed);

    if (previousLevel != uint8(level))
        TC_LOG_INFO("playerbots.governor",
            "Playerbot resource governor pressure change: {} -> {} (avg world update {} ms).",
            previousLevel, uint8(level), newAverage);
}

ResourcePressureLevel ResourceGovernor::GetPressureLevel()
{
    if (!g_GovernorConfig.enabled)
        return ResourcePressureLevel::Normal;

    return ResourcePressureLevel(g_PressureLevel.load(std::memory_order_relaxed));
}

bool ResourceGovernor::CanAddCustomMatchBot(uint32 currentMatchBots, uint32 totalActiveCustomMatchBots)
{
    if (!g_GovernorConfig.enabled)
        return true;

    if (g_GovernorConfig.maxTotalCustomMatchBots &&
        totalActiveCustomMatchBots + currentMatchBots >= g_GovernorConfig.maxTotalCustomMatchBots)
        return false;

    switch (GetPressureLevel())
    {
        case ResourcePressureLevel::Hard:
            return false;
        case ResourcePressureLevel::Soft:
            return currentMatchBots < g_GovernorConfig.softMatchBotCap;
        case ResourcePressureLevel::Normal:
        default:
            return true;
    }
}

bool ResourceGovernor::ShouldCullNow()
{
    if (!g_GovernorConfig.enabled)
        return false;

    if (GetPressureLevel() != ResourcePressureLevel::Hard)
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    if (!IsHardPressureSustained(nowMs))
        return false;

    uint32 const lastCullMs = g_LastCullMs.load(std::memory_order_relaxed);
    return !lastCullMs || nowMs >= lastCullMs + g_GovernorConfig.cullCooldownMs;
}

void ResourceGovernor::NoteCullExecuted()
{
    g_LastCullMs.store(GameTime::GetGameTimeMS(), std::memory_order_relaxed);
}

bool ResourceGovernor::ShouldShedBotNow()
{
    if (!g_GovernorConfig.enabled)
        return false;

    if (GetPressureLevel() != ResourcePressureLevel::Hard)
        return false;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    if (!IsHardPressureSustained(nowMs))
        return false;

    uint32 const lastShedMs = g_LastBotShedMs.load(std::memory_order_relaxed);
    return !lastShedMs || nowMs >= lastShedMs + g_GovernorConfig.botShedCooldownMs;
}

void ResourceGovernor::NoteBotShedExecuted()
{
    g_LastBotShedMs.store(GameTime::GetGameTimeMS(), std::memory_order_relaxed);
}

ResourceGovernorSnapshot ResourceGovernor::GetSnapshot()
{
    ResourceGovernorSnapshot snapshot;
    snapshot.enabled = g_GovernorConfig.enabled;
    snapshot.level = GetPressureLevel();
    snapshot.averageWorldUpdateMs = g_AverageUpdateMs.load(std::memory_order_relaxed);
    snapshot.softUpdateTimeMs = g_GovernorConfig.softUpdateTimeMs;
    snapshot.hardUpdateTimeMs = g_GovernorConfig.hardUpdateTimeMs;
    snapshot.softMatchBotCap = g_GovernorConfig.softMatchBotCap;
    snapshot.maxTotalCustomMatchBots = g_GovernorConfig.maxTotalCustomMatchBots;
    return snapshot;
}
}
