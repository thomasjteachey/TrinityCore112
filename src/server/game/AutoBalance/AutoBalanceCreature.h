#pragma once

#include "AutoBalance/AutoBalanceConfig.h"
#include "AutoBalance/AutoBalanceCreatureInfo.h"
#include "Optional.h"

class Creature;
class Map;
class Unit;

namespace AutoBalance
{
    struct ActiveInflectionInfo
    {
        InflectionPointSettings Settings;
        uint32 TargetPlayers = 1;
    };

    AutoBalanceCreatureInfo& GetCreatureInfo(Creature& creature);
    AutoBalanceCreatureInfo const& GetCreatureInfo(Creature const& creature);

    AutoBalanceCreatureInfo* TryGetCreatureInfo(Unit* unit);
    AutoBalanceCreatureInfo const* TryGetCreatureInfo(Unit const* unit);

    Optional<float> GetDamageHealingMultiplier(Unit const* unit);
    Optional<float> GetCrowdControlDurationMultiplier(Unit const* unit);

    // Empty unless AutoBalance.RewardScaling.XP / .Money are enabled.
    Optional<float> GetExperienceMultiplier(Unit const* unit);
    Optional<float> GetMoneyMultiplier(Unit const* unit);

    ActiveInflectionInfo GetInflectionInfoForMap(Map const* map, bool forBoss);

    void ScaleCreature(Creature* creature);
}
