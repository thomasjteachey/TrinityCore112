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

#ifndef TRINITY_AUTOBALANCEMGR_H
#define TRINITY_AUTOBALANCEMGR_H

#include "Define.h"

class Creature;
class InstanceMap;
class Map;
class Unit;

namespace AutoBalance
{
    void LoadConfig(bool reload);
    bool IsEnabled();

    void NotifyPlayerEvent(Map* map);

    void ModifyDamage(Unit* attacker, Unit* victim, uint32& damage);

    float GetMapScale(InstanceMap* map);
}

#endif // TRINITY_AUTOBALANCEMGR_H
