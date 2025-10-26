#pragma once

#include "Define.h"
#include <string>

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
        CreatureBaseValues LevelScaledBaseValues;
        CreatureMultipliers BaseMultipliers;
        CreatureMultipliers Multipliers;
        uint8 BaseLevel = 0;
        uint8 UnmodifiedLevel = 0;
        uint8 SelectedLevel = 0;
        uint32 TargetPlayerCount = 0;
        uint32 EffectivePlayerCount = 0;
        uint32 InstancePlayerCount = 0;
        float XPModifier = 1.0f;
        float MoneyModifier = 1.0f;
        bool IsBoss = false;
        bool IsSummon = false;
        bool IsSummonClone = false;
        bool ActiveForMapStats = true;
        std::string SummonerName;
        uint8 SummonerLevel = 0;
        bool Initialized = false;
    };
}
