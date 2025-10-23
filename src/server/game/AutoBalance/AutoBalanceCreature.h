#pragma once

#include "AutoBalance/AutoBalanceCreatureInfo.h"

class Creature;

namespace AutoBalance
{
    AutoBalanceCreatureInfo& GetCreatureInfo(Creature& creature);
    AutoBalanceCreatureInfo const& GetCreatureInfo(Creature const& creature);

    void ScaleCreature(Creature* creature);
}
