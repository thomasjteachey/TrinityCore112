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

#include "AutoBalanceMgr.h"

#include "Configuration/Config.h"
#include "Creature.h"
#include "InstanceMap.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>

namespace AutoBalance
{
namespace
{
    struct AutoBalanceSettings
    {
        bool Enabled = true;
        bool ScaleDungeons = true;
        bool ScaleRaids = true;
        float MinScale = 0.3f;
        float MaxScale = 1.5f;
        float HealthMultiplier = 1.0f;
        float BossHealthMultiplier = 1.0f;
        float DamageMultiplier = 1.0f;
        float BossDamageMultiplier = 1.0f;
        uint32 MinPlayers = 1u;
        int32 PlayerDifficultyOffset = 0;
        uint32 DefaultDungeonPlayers = 5u;
        uint32 DefaultRaidPlayers = 10u;
        bool DebugLogging = false;
    } sSettings;

    uint32 GetEffectivePlayerCount(InstanceMap* map)
    {
        uint32 count = map->GetPlayersCountExceptGMs();

        if (sSettings.PlayerDifficultyOffset != 0)
        {
            if (sSettings.PlayerDifficultyOffset > 0)
                count += uint32(sSettings.PlayerDifficultyOffset);
            else
            {
                uint32 offset = uint32(std::abs(sSettings.PlayerDifficultyOffset));
                if (count > offset)
                    count -= offset;
                else
                    count = 0;
            }
        }

        if (count < sSettings.MinPlayers)
            count = sSettings.MinPlayers;

        return count;
    }

    uint32 GetMaxPlayers(InstanceMap* map)
    {
        uint32 maxPlayers = map->GetMaxPlayers();
        if (!maxPlayers)
            maxPlayers = map->IsRaid() ? sSettings.DefaultRaidPlayers : sSettings.DefaultDungeonPlayers;

        return std::max<uint32>(1u, maxPlayers);
    }

    bool IsMapEligible(InstanceMap* map)
    {
        if (!map)
            return false;

        if (map->IsRaid() && !sSettings.ScaleRaids)
            return false;

        if (!map->IsRaid() && !sSettings.ScaleDungeons)
            return false;

        return true;
    }

    float CalculateBaseScale(InstanceMap* map)
    {
        if (!IsMapEligible(map))
            return 1.0f;

        uint32 players = GetEffectivePlayerCount(map);
        uint32 maxPlayers = GetMaxPlayers(map);

        float scale = static_cast<float>(players) / static_cast<float>(maxPlayers);
        return std::clamp(scale, sSettings.MinScale, sSettings.MaxScale);
    }

    bool IsCreatureEligible(Creature const* creature)
    {
        if (!creature)
            return false;

        if (creature->IsPet() || creature->IsTotem() || creature->IsSummon() || creature->IsControlledByPlayer())
            return false;

        Map* map = creature->GetMap();
        if (!map || !map->IsDungeon())
            return false;

        return true;
    }

    float GetHealthScaleInternal(Creature const* creature)
    {
        InstanceMap* instance = creature->GetMap()->ToInstanceMap();
        if (!instance)
            return 1.0f;

        float scale = CalculateBaseScale(instance);
        scale *= sSettings.HealthMultiplier;

        if (creature->IsDungeonBoss() || creature->isWorldBoss())
            scale *= sSettings.BossHealthMultiplier;

        return std::clamp(scale, sSettings.MinScale, sSettings.MaxScale);
    }

    float GetDamageScaleInternal(Creature const* creature)
    {
        InstanceMap* instance = creature->GetMap()->ToInstanceMap();
        if (!instance)
            return 1.0f;

        float scale = CalculateBaseScale(instance);
        scale *= sSettings.DamageMultiplier;

        if (creature->IsDungeonBoss() || creature->isWorldBoss())
            scale *= sSettings.BossDamageMultiplier;

        return std::clamp(scale, sSettings.MinScale, sSettings.MaxScale);
    }

    float SafeInverse(float value)
    {
        if (value <= 0.0f)
            return 1.0f;

        return 1.0f / value;
    }
}

void LoadConfig(bool reload)
{
    sSettings.Enabled = sConfigMgr->GetBoolDefault("AutoBalance.Enable", true);
    sSettings.ScaleDungeons = sConfigMgr->GetBoolDefault("AutoBalance.ScaleDungeons", true);
    sSettings.ScaleRaids = sConfigMgr->GetBoolDefault("AutoBalance.ScaleRaids", true);
    sSettings.MinScale = std::max(0.05f, sConfigMgr->GetFloatDefault("AutoBalance.MinScale", 0.3f));
    sSettings.MaxScale = std::max(sSettings.MinScale, sConfigMgr->GetFloatDefault("AutoBalance.MaxScale", 1.5f));
    sSettings.HealthMultiplier = std::max(0.01f, sConfigMgr->GetFloatDefault("AutoBalance.HealthMultiplier", 1.0f));
    sSettings.BossHealthMultiplier = std::max(0.01f, sConfigMgr->GetFloatDefault("AutoBalance.BossHealthMultiplier", 1.0f));
    sSettings.DamageMultiplier = std::max(0.01f, sConfigMgr->GetFloatDefault("AutoBalance.DamageMultiplier", 1.0f));
    sSettings.BossDamageMultiplier = std::max(0.01f, sConfigMgr->GetFloatDefault("AutoBalance.BossDamageMultiplier", 1.0f));
    sSettings.MinPlayers = std::max<uint32>(1u, sConfigMgr->GetIntDefault("AutoBalance.MinPlayers", 1));
    sSettings.PlayerDifficultyOffset = sConfigMgr->GetIntDefault("AutoBalance.PlayerDifficultyOffset", 0);
    sSettings.DefaultDungeonPlayers = std::max<uint32>(1u, sConfigMgr->GetIntDefault("AutoBalance.DefaultDungeonPlayers", 5));
    sSettings.DefaultRaidPlayers = std::max<uint32>(1u, sConfigMgr->GetIntDefault("AutoBalance.DefaultRaidPlayers", 10));
    sSettings.DebugLogging = sConfigMgr->GetBoolDefault("AutoBalance.DebugLogging", false);

    if (!reload)
        TC_LOG_INFO("misc", "AutoBalance: configuration loaded (enabled: %s).", sSettings.Enabled ? "true" : "false");
    else
        TC_LOG_INFO("misc", "AutoBalance: configuration reloaded (enabled: %s).", sSettings.Enabled ? "true" : "false");
}

bool IsEnabled()
{
    return sSettings.Enabled;
}

void NotifyPlayerEvent(Map* map)
{
    if (!IsEnabled() || !map || !map->IsDungeon())
        return;

    if (InstanceMap* instance = map->ToInstanceMap())
    {
        if (!IsMapEligible(instance))
            return;

        float scale = CalculateBaseScale(instance);
        uint32 players = GetEffectivePlayerCount(instance);
        if (sSettings.DebugLogging)
            TC_LOG_DEBUG("misc", "AutoBalance: map %u instance %u playerCount=%u baseScale=%.3f", map->GetId(), instance->GetInstanceId(), players, scale);
    }
}

void ModifyDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    if (!IsEnabled() || !damage)
        return;

    Creature* victimCreature = victim ? victim->ToCreature() : nullptr;
    Creature* attackerCreature = attacker ? attacker->ToCreature() : nullptr;

    if (victimCreature && IsCreatureEligible(victimCreature))
    {
        float healthScale = GetHealthScaleInternal(victimCreature);
        float modifier = SafeInverse(healthScale);
        damage = uint32(std::max(1.0f, std::floor(damage * modifier + 0.5f)));

        if (sSettings.DebugLogging)
        {
            InstanceMap* instance = victimCreature->GetMap()->ToInstanceMap();
            uint32 players = instance ? GetEffectivePlayerCount(instance) : 0;
            TC_LOG_DEBUG("misc", "AutoBalance: player damage to creature %s (%u) scaled by %.3f (players=%u)", victimCreature->GetName().c_str(), victimCreature->GetEntry(), modifier, players);
        }
        return;
    }

    if (attackerCreature && IsCreatureEligible(attackerCreature))
    {
        float damageScale = GetDamageScaleInternal(attackerCreature);
        damage = uint32(std::max(1.0f, std::floor(damage * damageScale + 0.5f)));

        if (sSettings.DebugLogging)
        {
            InstanceMap* instance = attackerCreature->GetMap()->ToInstanceMap();
            uint32 players = instance ? GetEffectivePlayerCount(instance) : 0;
            TC_LOG_DEBUG("misc", "AutoBalance: creature damage from %s (%u) scaled by %.3f (players=%u)", attackerCreature->GetName().c_str(), attackerCreature->GetEntry(), damageScale, players);
        }
    }
}

float GetMapScale(InstanceMap* map)
{
    if (!IsEnabled() || !map)
        return 1.0f;

    return CalculateBaseScale(map);
}
}
