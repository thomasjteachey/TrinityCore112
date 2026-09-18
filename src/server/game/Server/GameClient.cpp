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

#include "GameClient.h"
#include "WorldSession.h"
#include "ObjectAccessor.h"
#include "Unit.h"
#include "Player.h"

GameClient::GameClient(WorldSession* sessionToServer)
{
    _sessionToServer = sessionToServer;
    _activelyMovedUnit = nullptr;
}

void GameClient::AddAllowedMover(Unit* unit)
{
    ASSERT(!unit->GetGameClientMovingMe() || unit->GetGameClientMovingMe() == this);

    _allowedMovers.insert(unit->GetGUID());
    unit->SetGameClientMovingMe(this);
}

void GameClient::RemoveAllowedMover(Unit* unit)
{
    unit->PurgeAndApplyPendingMovementChanges();
    _allowedMovers.erase(unit->GetGUID());
    if (unit->GetGameClientMovingMe() == this)
    {
        unit->SetGameClientMovingMe(nullptr);
        SetActivelyMovedUnit(nullptr);
    }
}

void GameClient::ReleaseAllMovers()
{
    // An allowed mover is always on the owner's map, so this is the last moment the
    // units can be found at all: once the owner leaves, a possessed creature left
    // behind keeps pointing at a GameClient that is about to be freed with the
    // session, and the next thing to touch it reads freed memory.
    //
    // The set is swapped out first because RemoveAllowedMover erases from it as it
    // goes. Going through it rather than clearing the back-pointer directly matters:
    // it is also what drains the movement changes the unit is still owed acks for,
    // and a unit left holding those with no controller is a null dereference on its
    // very next update.
    Player* owner = GetBasePlayer();
    GuidUnorderedSet movers;
    movers.swap(_allowedMovers);

    for (ObjectGuid const& guid : movers)
        if (Unit* unit = owner ? ObjectAccessor::GetUnit(*owner, guid) : nullptr)
            if (unit->GetGameClientMovingMe() == this)
                RemoveAllowedMover(unit);

    SetActivelyMovedUnit(nullptr);
}

bool GameClient::IsAllowedToMove(Unit* unit) const
{
    return _allowedMovers.count(unit->GetGUID());
}

bool GameClient::IsAllowedToMove(ObjectGuid guid) const
{
    return _allowedMovers.count(guid);
}

void GameClient::SetMovedUnit(Unit* target, bool allowMove)
{
    if (allowMove)
        AddAllowedMover(target);
    else
        RemoveAllowedMover(target);
}

void GameClient::SendDirectMessage(WorldPacket const* data) const
{
    GetBasePlayer()->SendDirectMessage(data);
}

std::string GameClient::GetDebugInfo() const
{
    std::stringstream sstr;
    sstr << "GetBasePlayer(): " << (GetBasePlayer() ? GetBasePlayer()->GetGUID().ToString().c_str() : "NULL");
    return sstr.str();
}
