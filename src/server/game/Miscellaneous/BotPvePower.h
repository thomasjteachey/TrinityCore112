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

#ifndef TRINITYCORE_BOT_PVE_POWER_H
#define TRINITYCORE_BOT_PVE_POWER_H

#include "Define.h"

class Player;

// Power a managed playerbot earns against wildlife by dying to it.
//
// Every open-world death adds one stack; stacks fade on a half-life, so a bot on
// a losing streak gets steadily harder to kill and a bot that has stopped dying
// drifts back to stock. Each stack makes the bot (and its pet) deal more damage
// to ownerless creatures and take less from them, scaled by level: nothing below
// StartLevel, full strength from FullLevel. Players are never affected on either
// side - Unit::DealDamage applies it only between a bot and wildlife.
//
// Kept out of Player so it survives the re-level a drifter landing or rebirth
// does, which is exactly when a bot dies most. Lost on logout, which bots do not.
namespace BotPvePower
{
    // Loaded from World::LoadConfigSettings; every key applies on `.reload config`.
    void LoadConfig();

    bool IsEnabled();

    // A managed bot died in the open world. The caller decides what counts.
    void RecordDeath(Player const* bot);

    // Stacks after decay. 0 when the bot has none or the system is off.
    float GetStacks(Player const* bot);

    // Percent ADDED to what the bot deals to wildlife (0 = stock).
    uint32 GetDamageDoneBonusPct(Player const* bot);

    // Percent of a wildlife blow that still lands on the bot (100 = stock).
    uint32 GetDamageTakenPct(Player const* bot);
}

#endif
