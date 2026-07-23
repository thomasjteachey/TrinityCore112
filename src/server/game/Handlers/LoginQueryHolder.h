/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef TRINITY_LOGIN_QUERY_HOLDER_H
#define TRINITY_LOGIN_QUERY_HOLDER_H

#include "DatabaseEnv.h"
#include "ObjectGuid.h"
#include "QueryHolder.h"

// Shared by the normal login flow and transient offline-character mirrors.
class TC_GAME_API LoginQueryHolder : public CharacterDatabaseQueryHolder
{
public:
    LoginQueryHolder(uint32 accountId, ObjectGuid guid) : _accountId(accountId), _guid(guid) { }

    ObjectGuid GetGuid() const { return _guid; }
    uint32 GetAccountId() const { return _accountId; }
    bool Initialize();

private:
    uint32 _accountId;
    ObjectGuid _guid;
};

#endif
