#pragma once

#include "Define.h"

namespace AutoBalance
{
    struct CreatureBaseValues
    {
        uint32 Health = 0;
        uint32 Mana = 0;
        uint32 Armor = 0;
        float MinDamage = 0.0f;
        float MaxDamage = 0.0f;
        float AttackPower = 0.0f;
        float RangedAttackPower = 0.0f;
    };

    struct CreatureMultipliers
    {
        float Health = 1.0f;
        float Mana = 1.0f;
        float Damage = 1.0f;
        float Armor = 1.0f;
        float CrowdControlDuration = 1.0f;
    };

    struct AutoBalanceCreatureInfo
    {
        CreatureBaseValues BaseValues;
        CreatureMultipliers Multipliers;
        uint8 BaseLevel = 0;
        uint32 TargetPlayerCount = 0;
        uint32 EffectivePlayerCount = 0;
        bool Initialized = false;
    };
}
