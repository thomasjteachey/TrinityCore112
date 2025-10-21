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

#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Unit.h"

namespace
{
    void NotifyMap(Map* map)
    {
        if (!map)
            return;

        AutoBalance::NotifyPlayerEvent(map);
    }
}

class CustomAutoBalanceWorldScript : public WorldScript
{
public:
    CustomAutoBalanceWorldScript() : WorldScript("CustomAutoBalanceWorldScript") { }

    void OnConfigLoad(bool reload) override
    {
        AutoBalance::LoadConfig(reload);
    }
};

class CustomAutoBalancePlayerScript : public PlayerScript
{
public:
    CustomAutoBalancePlayerScript() : PlayerScript("CustomAutoBalancePlayerScript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        NotifyMap(player ? player->GetMap() : nullptr);
    }

    void OnLogout(Player* player) override
    {
        NotifyMap(player ? player->GetMap() : nullptr);
    }

    void OnMapChanged(Player* player) override
    {
        NotifyMap(player ? player->GetMap() : nullptr);
    }
};

class CustomAutoBalanceUnitScript : public UnitScript
{
public:
    CustomAutoBalanceUnitScript() : UnitScript("CustomAutoBalanceUnitScript") { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        AutoBalance::ModifyDamage(attacker, victim, damage);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        AutoBalance::ModifyMeleeDamage(target, attacker, damage);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage) override
    {
        AutoBalance::ModifyPeriodicDamage(attacker, target, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage) override
    {
        AutoBalance::ModifySpellDamage(target, attacker, damage);
    }
};

void AddAutoBalanceScripts()
{
    AutoBalance::LoadConfig(false);

    new CustomAutoBalanceWorldScript();
    new CustomAutoBalancePlayerScript();
    new CustomAutoBalanceUnitScript();
}
