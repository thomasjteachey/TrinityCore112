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
#ifndef __BATTLEGROUNDNL_H
#define __BATTLEGROUNDNL_H

#include "Arena.h"

// Nefarian's Arena
// Custom arena type 103 / map 1572.
enum BattlegroundNLObjectTypes
{
    BG_NL_OBJECT_DOOR_1      = 0,
    BG_NL_OBJECT_DOOR_2      = 1,
    BG_NL_OBJECT_WALL_1      = 2,
    BG_NL_OBJECT_BUFF_1      = 3,
    BG_NL_OBJECT_BUFF_2      = 4,
    BG_NL_OBJECT_MAX         = 5
};

enum BattlegroundNLGameObjects
{
    BG_NL_OBJECT_TYPE_DOOR   = 176966, // BWL Portcullis
    BG_NL_OBJECT_TYPE_WALL   = 179117, // BWL Portcullis / permanent blocker
    BG_NL_OBJECT_TYPE_BUFF_1 = 184663, // Shadow Sight
    BG_NL_OBJECT_TYPE_BUFF_2 = 184664  // Shadow Sight
};

class BattlegroundNL : public Arena
{
public:
    BattlegroundNL();
    ~BattlegroundNL() override = default;

    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;

    void HandleAreaTrigger(Player* player, uint32 trigger) override;
    bool SetupBattleground() override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;

private:
    void ApplyNonInteractableObjectFlags();
    void SetNefarianDoorClosed(uint32 type);
    void SetNefarianDoorOpen(uint32 type);
};

#endif
