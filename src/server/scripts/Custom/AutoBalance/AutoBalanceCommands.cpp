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
#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "Language.h"
#include "Map.h"
#include "MapManager.h"
#include "Player.h"
#include "RBAC.h"
#include "StringFormat.h"
#include <string>

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

        handler->PSendSysMessage("AutoBalance map stats for map %u (instance %u):", mapId, instanceId);
        handler->PSendSysMessage(" Active players: %u | Effective players: %u | Minimum players: %u",
            data.PlayerCount, data.EffectivePlayerCount, CalculateMinimumPlayers(map, config));
        handler->PSendSysMessage(" Global offset: %d | Applied offset: %d",
            AutoBalance::GetPlayerCountDifficultyOffset(), data.QueueOffset);
        handler->PSendSysMessage(" Last join: %s | Last leave: %s | Last count update: %s",
            FormatTimeSince(data.LastPlayerJoinTimeMS, now).c_str(),
            FormatTimeSince(data.LastPlayerLeaveTimeMS, now).c_str(),
            FormatTimeSince(data.LastPlayerCountUpdateTimeMS, now).c_str());
        handler->PSendSysMessage(" Combat locked: %s | Dirty state: %s",
            data.CombatLocked ? "yes" : "no",
            data.CombatStateDirty ? "yes" : "no");
        handler->PSendSysMessage(" Last combat start: %s | Last combat end: %s",
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

        handler->PSendSysMessage("AutoBalance stats for %s (entry %u):",
            creature->GetName().c_str(), creature->GetEntry());
        handler->PSendSysMessage(" Level: %u | Target players: %u | Effective players: %u",
            info->BaseLevel, info->TargetPlayerCount, info->EffectivePlayerCount);
        handler->PSendSysMessage(" Base Health: %u | Base Mana: %u | Base Armor: %u",
            info->BaseValues.Health, info->BaseValues.Mana, info->BaseValues.Armor);
        handler->PSendSysMessage(" Base Damage: %.3f - %.3f | Base AP: %.3f | Base RAP: %.3f",
            info->BaseValues.MinDamage, info->BaseValues.MaxDamage,
            info->BaseValues.AttackPower, info->BaseValues.RangedAttackPower);
        handler->PSendSysMessage(" Multipliers -> Health: %.3f | Mana: %.3f | Damage: %.3f | Armor: %.3f | CC: %.3f",
            info->Multipliers.Health, info->Multipliers.Mana, info->Multipliers.Damage,
            info->Multipliers.Armor, info->Multipliers.CrowdControlDuration);

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
