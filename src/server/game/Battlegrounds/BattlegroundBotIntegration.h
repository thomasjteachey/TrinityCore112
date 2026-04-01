/*
 * This file is part of the TrinityCore Project. See AUTHORS file for copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef TRINITY_BATTLEGROUNDBOTINTEGRATION_H
#define TRINITY_BATTLEGROUNDBOTINTEGRATION_H

#include "DBCEnums.h"
#include "SharedDefines.h"

#include <cstdint>
#include <functional>

class BattlegroundQueue;

class BattlegroundBotIntegration
{
public:
    using EnsureQueueHook = std::function<void(BattlegroundQueue& queue, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracketId, uint32 minPlayersPerTeam, uint32 maxPlayersPerTeam, uint32 allianceMissingPlayers, uint32 hordeMissingPlayers)>;

    static BattlegroundBotIntegration& Instance();

    void RegisterEnsureQueueHook(EnsureQueueHook hook);
    void EnsureQueue(BattlegroundQueue& queue, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracketId, uint32 minPlayersPerTeam, uint32 maxPlayersPerTeam, uint32 allianceMissingPlayers, uint32 hordeMissingPlayers) const;

private:
    EnsureQueueHook _ensureQueueHook;
};

#endif
