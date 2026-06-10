/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This file is free software; you can redistribute it and/or modify it
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

#include "ScriptMgr.h"
#include "AutoBalance/AutoBalanceConfig.h"
#include "AutoBalance/AutoBalanceCreature.h"
#include "AutoBalance/AutoBalanceCreatureInfo.h"
#include "AutoBalance/AutoBalanceMapData.h"
#include "Configuration/Config.h"
#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "Language.h"
#include "Map.h"
#include "MapManager.h"
#include "Player.h"
#include "RBAC.h"
#include "StringFormat.h"
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Trinity::ChatCommands;

namespace
{
    std::string FormatTimeSince(uint32 timestamp, uint32 now)
    {
        if (!timestamp)
            return "never";

        uint32 const diff = now >= timestamp ? now - timestamp : 0u;
        double const seconds = static_cast<double>(diff) / 1000.0;
        return Trinity::StringFormat("{:.3f}s ago", seconds);
    }

    Map* ResolveMap(ChatHandler* handler, Optional<uint32> mapIdArg, Optional<uint32> instanceIdArg, uint32& outMapId, uint32& outInstanceId)
    {
        Map* map = nullptr;

        if (mapIdArg)
        {
            outMapId = *mapIdArg;
            outInstanceId = instanceIdArg.value_or(0u);

            map = sMapMgr->FindMap(outMapId, outInstanceId);
            if (!map && outInstanceId == 0u)
                map = sMapMgr->FindBaseNonInstanceMap(outMapId);

            if (!map)
            {
                handler->PSendSysMessage("AutoBalance: map %u (instance %u) is not active.", outMapId, outInstanceId);
                handler->SetSentErrorMessage(true);
            }

            return map;
        }

        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->PSendSysMessage("AutoBalance: console usage requires map and instance arguments.");
            handler->SetSentErrorMessage(true);
            return nullptr;
        }

        map = player->GetMap();
        if (!map)
        {
            handler->SendSysMessage("AutoBalance: unable to resolve current map.");
            handler->SetSentErrorMessage(true);
            return nullptr;
        }

        outMapId = map->GetId();
        outInstanceId = map->GetInstanceId();
        return map;
    }

    bool IsMapEnabled(Map const* map, AutoBalance::ModuleConfig const& config)
    {
        if (!map)
            return false;

        if (std::find(config.DisabledInstances.begin(), config.DisabledInstances.end(), map->GetId()) != config.DisabledInstances.end())
            return false;

        if (!map->IsDungeon())
            return config.EnableWorldMaps;

        if (map->IsRaid())
            return config.EnableRaids;

        return config.EnableDungeons;
    }

    uint32 CalculateMinimumPlayers(Map const* map, AutoBalance::ModuleConfig const& config)
    {
        if (!map)
            return 0;

        auto const& overrides = map->IsHeroic() ? config.MinPlayersOverridesHeroic : config.MinPlayersOverridesNormal;
        if (auto const itr = overrides.find(map->GetId()); itr != overrides.end())
            return itr->second;

        return map->IsHeroic() ? config.MinimumPlayersHeroic : config.MinimumPlayers;
    }
}

class AutoBalanceCommandScript final : public CommandScript
{
public:
    AutoBalanceCommandScript() : CommandScript("AutoBalanceCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable autoBalanceCommandTable =
        {
            { "config", HandleAutoBalanceConfigCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "mapstat", HandleAutoBalanceMapStatCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "creaturestat", HandleAutoBalanceCreatureStatCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "offset", HandleAutoBalanceOffsetCommand, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "ab", autoBalanceCommandTable }
        };

        return commandTable;
    }

    static bool HandleAutoBalanceConfigCommand(ChatHandler* handler)
    {
        AutoBalance::ConfigLoadInfo const& loadInfo = AutoBalance::GetConfigLoadInfo();
        AutoBalance::ModuleConfig const& config = AutoBalance::GetConfig();

        handler->PSendSysMessage("---");

        std::string requested = loadInfo.RequestedPath.empty() ? std::string("<default>") : loadInfo.RequestedPath;
        handler->PSendSysMessage("Requested file: %s", requested.c_str());

        if (loadInfo.Loaded)
        {
            std::string resolved = loadInfo.ResolvedPath.empty() ? requested : loadInfo.ResolvedPath;
            handler->PSendSysMessage("Loaded file: %s%s", resolved.c_str(), loadInfo.UsedFallback ? " (fallback)" : "");
        }
        else if (!loadInfo.RequestedPath.empty())
        {
            if (loadInfo.Error.empty())
                handler->PSendSysMessage("Loaded file: FAILED");
            else
                handler->PSendSysMessage("Loaded file: FAILED (%s)", loadInfo.Error.c_str());
        }
        else
            handler->PSendSysMessage("Loaded file: <none>");

        if (!loadInfo.Attempts.empty())
        {
            std::string attempts = loadInfo.Attempts.front();
            for (size_t i = 1; i < loadInfo.Attempts.size(); ++i)
                attempts += ", " + loadInfo.Attempts[i];
            handler->PSendSysMessage("Attempted paths: %s", attempts.c_str());
        }

        std::vector<std::string> keys = sConfigMgr->GetKeysByString("AutoBalance.");
        std::unordered_set<std::string> keySet(keys.begin(), keys.end());
        handler->PSendSysMessage("Loaded AutoBalance keys: %zu", keySet.size());

        auto sourceLabel = [&](std::string const& key) -> std::string
        {
            if (key.empty())
                return "default";

            return keySet.count(key) ? "config" : "default";
        };

        auto printInflection = [&](char const* label, char const* baseKey, AutoBalance::InflectionPointSettings const& settings)
        {
            std::string base(baseKey ? baseKey : "");
            std::string floorKey = base.empty() ? std::string() : base + ".CurveFloor";
            std::string ceilingKey = base.empty() ? std::string() : base + ".CurveCeiling";
            std::string bossKey = base.empty() ? std::string() : base + ".BossModifier";

            std::string valueSource = sourceLabel(base);
            std::string floorSource = sourceLabel(floorKey);
            std::string ceilingSource = sourceLabel(ceilingKey);
            std::string bossSource = sourceLabel(bossKey);

            handler->PSendSysMessage("%s: ratio %.3f (%s) | floor %.3f (%s) | ceiling %.3f (%s) | boss %.3f (%s)",
                label,
                settings.Value,
                valueSource.c_str(),
                settings.CurveFloor,
                floorSource.c_str(),
                settings.CurveCeiling,
                ceilingSource.c_str(),
                settings.BossModifier,
                bossSource.c_str());
        };

        printInflection("Dungeon", "AutoBalance.InflectionPoint", config.DungeonInflection);
        printInflection("Dungeon Heroic", "AutoBalance.InflectionPointHeroic", config.DungeonHeroicInflection);
        printInflection("Raid", "AutoBalance.InflectionPointRaid", config.RaidInflection);
        printInflection("Raid Heroic", "AutoBalance.InflectionPointRaidHeroic", config.RaidHeroicInflection);
        printInflection("Raid 10", "AutoBalance.InflectionPointRaid10M", config.RaidInflection10);
        printInflection("Raid 15", "AutoBalance.InflectionPointRaid15M", config.RaidInflection15);
        printInflection("Raid 20", "AutoBalance.InflectionPointRaid20M", config.RaidInflection20);
        printInflection("Raid 25", "AutoBalance.InflectionPointRaid25M", config.RaidInflection25);
        printInflection("Raid 40", "AutoBalance.InflectionPointRaid40M", config.RaidInflection40);
        printInflection("Raid 10 Heroic", "AutoBalance.InflectionPointRaid10MHeroic", config.RaidHeroicInflection10);
        printInflection("Raid 25 Heroic", "AutoBalance.InflectionPointRaid25MHeroic", config.RaidHeroicInflection25);

        auto minSource = [&](char const* key)
        {
            return sourceLabel(key);
        };

        handler->PSendSysMessage("Minimum players: normal %u (%s) | heroic %u (%s) | raid %u (%s) | raid heroic %u (%s)",
            config.MinimumPlayers, minSource("AutoBalance.MinPlayers").c_str(),
            config.MinimumPlayersHeroic, minSource("AutoBalance.MinPlayers.Heroic").c_str(),
            config.MinimumPlayersRaid, minSource("AutoBalance.MinPlayers.Raid").c_str(),
            config.MinimumPlayersRaidHeroic, minSource("AutoBalance.MinPlayers.RaidHeroic").c_str());

        std::string offsetSource = sourceLabel("AutoBalance.playerCountDifficultyOffset");
        handler->PSendSysMessage("Global player offset: %d (%s)", AutoBalance::GetPlayerCountDifficultyOffset(), offsetSource.c_str());

        handler->PSendSysMessage("Raid size inflection overrides: normal %zu | heroic %zu",
            config.RaidInflectionOverrides.size(), config.RaidHeroicInflectionOverrides.size());
        handler->PSendSysMessage("Instance inflection overrides: %zu | boss overrides: %zu",
            config.InflectionOverridesByInstance.size(), config.InflectionBossOverridesByInstance.size());

        return true;
    }

    static bool HandleAutoBalanceMapStatCommand(ChatHandler* handler, Optional<uint32> mapIdArg, Optional<uint32> instanceIdArg)
    {
        uint32 mapId = 0;
        uint32 instanceId = 0;
        Map* map = ResolveMap(handler, mapIdArg, instanceIdArg, mapId, instanceId);
        if (!map)
            return false;

        AutoBalance::ModuleConfig const& config = AutoBalance::GetConfig();
        Map::CustomData::AutoBalanceData const& data = map->GetCustomData().AutoBalance;
        uint32 const now = GameTime::GetGameTimeMS();

        InstanceMap* instanceMap = map->ToInstanceMap();
        uint32 const maxPlayers = instanceMap ? instanceMap->GetMaxPlayers() : 0u;
        bool const heroic = instanceMap ? instanceMap->IsHeroic() : map->IsHeroic();
        bool const enabled = IsMapEnabled(map, config) && AutoBalance::IsEnabled();

        handler->PSendSysMessage("---");
        handler->PSendSysMessage("%s (%u-player %s) | ID %u-%u%s",
            map->GetMapName(),
            maxPlayers,
            heroic ? "Heroic" : "Normal",
            mapId,
            instanceId,
            enabled ? "" : " | AutoBalance DISABLED");

        uint32 nonGMPlayers = 0;
        uint8 lowestLevel = 0;
        uint8 highestLevel = 0;
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            if (Player const* player = itr->GetSource())
            {
                if (player->IsGameMaster())
                    continue;

                ++nonGMPlayers;
                uint8 level = player->GetLevel();
                if (!lowestLevel || level < lowestLevel)
                    lowestLevel = level;
                if (level > highestLevel)
                    highestLevel = level;
            }
        }

        handler->PSendSysMessage("Players on map: %u (Lvl %u - %u)", nonGMPlayers, lowestLevel, highestLevel);

        uint32 const minimumPlayers = CalculateMinimumPlayers(map, config);
        if (data.CombatLocked && data.CombatLockTripped)
            handler->PSendSysMessage("Adjusted player count: %u (combat locked)", data.EffectivePlayerCount);
        else if (nonGMPlayers < minimumPlayers && data.QueueOffset)
            handler->PSendSysMessage("Adjusted player count: %u (map minimum + offset %d)", data.EffectivePlayerCount, data.QueueOffset);
        else if (nonGMPlayers < minimumPlayers)
            handler->PSendSysMessage("Adjusted player count: %u (map minimum)", data.EffectivePlayerCount);
        else if (data.QueueOffset)
            handler->PSendSysMessage("Adjusted player count: %u (offset %d)", data.EffectivePlayerCount, data.QueueOffset);
        else
            handler->PSendSysMessage("Adjusted player count: %u", data.EffectivePlayerCount);

        handler->PSendSysMessage("Minimum players: %u | Global offset: %d",
            minimumPlayers, AutoBalance::GetPlayerCountDifficultyOffset());

        AutoBalance::ActiveInflectionInfo inflectionInfo = AutoBalance::GetInflectionInfoForMap(map, false);
        AutoBalance::ActiveInflectionInfo bossInflectionInfo = AutoBalance::GetInflectionInfoForMap(map, true);

        handler->PSendSysMessage("Inflection target players: %u", inflectionInfo.TargetPlayers);

        auto computeRatio = [](AutoBalance::ActiveInflectionInfo const& info)
        {
            float const players = static_cast<float>(std::max<uint32>(info.TargetPlayers, 1u));
            if (players <= 0.0f)
                return info.Settings.Value;
            return info.Settings.Value / players;
        };

        float const nonBossRatio = computeRatio(inflectionInfo);
        handler->PSendSysMessage("Inflection (non-boss): ratio %.3f | value %.3f | floor %.3f | ceiling %.3f | boss modifier %.3f",
            nonBossRatio,
            inflectionInfo.Settings.Value,
            inflectionInfo.Settings.CurveFloor,
            inflectionInfo.Settings.CurveCeiling,
            inflectionInfo.Settings.BossModifier);

        float const bossRatio = computeRatio(bossInflectionInfo);
        std::string bossNote;
        if (bossInflectionInfo.Settings.Value == inflectionInfo.Settings.Value &&
            bossInflectionInfo.Settings.BossModifier == inflectionInfo.Settings.BossModifier)
        {
            bossNote = " (same as non-boss)";
        }

        handler->PSendSysMessage("Inflection (boss): ratio %.3f | value %.3f | boss modifier %.3f%s",
            bossRatio,
            bossInflectionInfo.Settings.Value,
            bossInflectionInfo.Settings.BossModifier,
            bossNote.c_str());

        handler->PSendSysMessage("Last join: %s | Last leave: %s | Last count update: %s",
            FormatTimeSince(data.LastPlayerJoinTimeMS, now).c_str(),
            FormatTimeSince(data.LastPlayerLeaveTimeMS, now).c_str(),
            FormatTimeSince(data.LastPlayerCountUpdateTimeMS, now).c_str());
        handler->PSendSysMessage("Combat locked: %s | Dirty state: %s",
            data.CombatLocked ? "yes" : "no",
            data.CombatStateDirty ? "yes" : "no");
        handler->PSendSysMessage("Last combat start: %s | Last combat end: %s",
            FormatTimeSince(data.LastCombatStartTimeMS, now).c_str(),
            FormatTimeSince(data.LastCombatEndTimeMS, now).c_str());

        return true;
    }

    static bool HandleAutoBalanceCreatureStatCommand(ChatHandler* handler)
    {
        Creature* creature = handler->getSelectedCreature();
        if (!creature)
        {
            handler->SendSysMessage(LANG_SELECT_CREATURE);
            handler->SetSentErrorMessage(true);
            return false;
        }

        AutoBalance::AutoBalanceCreatureInfo const* info = AutoBalance::TryGetCreatureInfo(creature);
        if (!info)
        {
            handler->PSendSysMessage("AutoBalance: no cached data available for creature %s (entry %u).",
                creature->GetName().c_str(), creature->GetEntry());
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("---");

        std::string levelInfo = Trinity::StringFormat("%u", info->UnmodifiedLevel);
        if (info->SelectedLevel && info->SelectedLevel != info->UnmodifiedLevel)
            levelInfo += Trinity::StringFormat("->%u", info->SelectedLevel);

        std::string flags;
        if (info->IsBoss)
            flags += " | Boss";

        handler->PSendSysMessage("%s (%s%s), %s",
            creature->GetName().c_str(), levelInfo.c_str(), flags.c_str(),
            info->ActiveForMapStats ? "active for map stats" : "ignored for map stats");

        handler->PSendSysMessage("Creature difficulty level: %u", info->InstancePlayerCount);

        if (info->IsSummon)
        {
            if (!info->SummonerName.empty())
                handler->PSendSysMessage("Summon of %s (Lvl %u)", info->SummonerName.c_str(), info->SummonerLevel);
            else
                handler->PSendSysMessage("Summon without summoner data");
        }

        auto printMultiplier = [&](char const* label, float baseMultiplier, float finalMultiplier)
        {
            if (info->SelectedLevel && info->SelectedLevel != info->UnmodifiedLevel)
                handler->PSendSysMessage("%s multiplier: %.3f -> %.3f", label, baseMultiplier, finalMultiplier);
            else
                handler->PSendSysMessage("%s multiplier: %.3f", label, finalMultiplier);
        };

        printMultiplier("Health", info->BaseMultipliers.Health, info->Multipliers.Health);
        printMultiplier("Mana", info->BaseMultipliers.Mana, info->Multipliers.Mana);
        printMultiplier("Armor", info->BaseMultipliers.Armor, info->Multipliers.Armor);
        printMultiplier("Damage", info->BaseMultipliers.Damage, info->Multipliers.Damage);
        handler->PSendSysMessage("CC duration multiplier: %.3f", info->Multipliers.CrowdControlDuration);
        handler->PSendSysMessage("XP multiplier: %.3f | Money multiplier: %.3f", info->XPModifier, info->MoneyModifier);

        return true;
    }

    static bool HandleAutoBalanceOffsetCommand(ChatHandler* handler, Optional<int32> offsetArg)
    {
        if (!offsetArg)
        {
            handler->PSendSysMessage("AutoBalance player offset is currently %d.", AutoBalance::GetPlayerCountDifficultyOffset());
            return true;
        }

        AutoBalance::SetPlayerCountDifficultyOffset(*offsetArg);

        sMapMgr->DoForAllMaps([](Map* map)
        {
            AutoBalance::RefreshEffectivePlayerCount(map);
        });

        handler->PSendSysMessage("AutoBalance player offset updated to %d.", *offsetArg);
        return true;
    }
};

void AddAutoBalanceCommandScripts()
{
    new AutoBalanceCommandScript();
}
