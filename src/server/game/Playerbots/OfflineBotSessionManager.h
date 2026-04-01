/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef TRINITY_OFFLINEBOTSESSIONMANAGER_H
#define TRINITY_OFFLINEBOTSESSIONMANAGER_H

#include "Common.h"
#include "SharedDefines.h"

#include <string>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

class TC_GAME_API OfflineBotSessionManager
{
public:
    struct Candidate
    {
        ObjectGuid::LowType Guid = 0;
        TeamId Team = TEAM_ALLIANCE;
        std::string Name;
        uint32 FailedAttempts = 0;
        uint64 RetryAfterMs = 0;
    };

    static OfflineBotSessionManager& Instance();

    using HeadlessStartCallback = std::function<bool(ObjectGuid::LowType guid, uint32 bracketId)>;

    void Configure();
    void RegisterHeadlessStartCallback(HeadlessStartCallback callback);
    void UpsertCandidate(Candidate candidate);

    // Placeholder for phase-2 implementation.
    void RequestOfflineFill(uint32 bracketId, TeamId team, uint32 missingPlayers);
    void Update(uint32 diff);

private:
    struct FillRequest
    {
        uint32 BracketId = 0;
        TeamId Team = TEAM_ALLIANCE;
        uint32 MissingPlayers = 0;
    };

    bool TryStartHeadlessSession(Candidate const& candidate, uint32 bracketId);

    std::unordered_map<ObjectGuid::LowType, Candidate> _candidates;
    HeadlessStartCallback _headlessStartCallback;
    std::deque<FillRequest> _pendingRequests;
    uint32 _requestLogTimer = 0;
    uint32 _maxAttemptsPerUpdate = 5;
    uint64 _requestsReceived = 0;
    uint64 _headlessStartAttempts = 0;
    uint64 _headlessStartsSucceeded = 0;
    uint64 _headlessPrecheckRejected = 0;
};

#endif
