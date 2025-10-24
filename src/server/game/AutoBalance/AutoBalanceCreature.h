#pragma once

#include "AutoBalance/AutoBalanceCreatureInfo.h"
#include "Optional.h"

class Creature;
class Unit;

namespace AutoBalance
{
    AutoBalanceCreatureInfo& GetCreatureInfo(Creature& creature);
    AutoBalanceCreatureInfo const& GetCreatureInfo(Creature const& creature);

    AutoBalanceCreatureInfo* TryGetCreatureInfo(Unit* unit);
    AutoBalanceCreatureInfo const* TryGetCreatureInfo(Unit const* unit);

    Optional<float> GetDamageHealingMultiplier(Unit const* unit);
    Optional<float> GetCrowdControlDurationMultiplier(Unit const* unit);

    void ScaleCreature(Creature* creature);
}
