/*
 * This file is part of the TrinityCore Project. See AUTHORS file for copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "BattlegroundBotIntegration.h"
#include "ScriptMgr.h"

#include <utility>

BattlegroundBotIntegration& BattlegroundBotIntegration::Instance()
{
    static BattlegroundBotIntegration instance;
    return instance;
}

void BattlegroundBotIntegration::RegisterEnsureQueueHook(EnsureQueueHook hook)
{
    _ensureQueueHook = std::move(hook);
}

void BattlegroundBotIntegration::EnsureQueue(BattlegroundQueue& queue, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracketId, uint32 minPlayersPerTeam, uint32 maxPlayersPerTeam, uint32 allianceMissingPlayers, uint32 hordeMissingPlayers) const
{
    if (_ensureQueueHook)
    {
        _ensureQueueHook(queue, bgTypeId, bracketId, minPlayersPerTeam, maxPlayersPerTeam, allianceMissingPlayers, hordeMissingPlayers);
        return;
    }

    sScriptMgr->OnBattlegroundQueueNeedBots(queue, bgTypeId, uint32(bracketId), allianceMissingPlayers, hordeMissingPlayers);
}
