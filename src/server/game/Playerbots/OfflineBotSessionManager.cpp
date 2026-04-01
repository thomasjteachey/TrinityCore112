/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "OfflineBotSessionManager.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"

#include <algorithm>
#include <utility>

OfflineBotSessionManager& OfflineBotSessionManager::Instance()
{
    static OfflineBotSessionManager instance;
    return instance;
}

void OfflineBotSessionManager::UpsertCandidate(Candidate candidate)
{
    auto itr = _candidates.find(candidate.Guid);
    if (itr != _candidates.end())
    {
        itr->second.Name = std::move(candidate.Name);
        itr->second.Team = candidate.Team;
        return;
    }

    _candidates[candidate.Guid] = std::move(candidate);
}

void OfflineBotSessionManager::Configure()
{
    _maxAttemptsPerUpdate = std::max<uint32>(1, sConfigMgr->GetIntDefault("Playerbots.BG.Offline.MaxAttemptsPerTick", 5));
}

void OfflineBotSessionManager::RegisterHeadlessStartCallback(HeadlessStartCallback callback)
{
    _headlessStartCallback = std::move(callback);
}

void OfflineBotSessionManager::RequestOfflineFill(uint32 bracketId, TeamId team, uint32 missingPlayers)
{
    if (!missingPlayers)
        return;

    ++_requestsReceived;
    _pendingRequests.push_back({ bracketId, team, missingPlayers });
}

void OfflineBotSessionManager::Update(uint32 diff)
{
    _requestLogTimer += diff;
    if (_requestLogTimer >= 10000)
    {
        _requestLogTimer = 0;
        if (!_pendingRequests.empty())
            TC_LOG_INFO("bg.battleground", "OfflineBotSessionManager pending requests: {}, known candidates: {}.", _pendingRequests.size(), _candidates.size());
    }

    if (_pendingRequests.empty())
        return;

    FillRequest request = _pendingRequests.front();
    _pendingRequests.pop_front();

    uint32 availableCandidates = 0;
    for (auto const& [_, candidate] : _candidates)
        if (candidate.Team == request.Team)
            ++availableCandidates;

    uint32 maxAttempts = std::min<uint32>(request.MissingPlayers, _maxAttemptsPerUpdate);
    uint32 started = 0;
    for (auto const& [_, candidate] : _candidates)
    {
        if (candidate.Team != request.Team)
            continue;

        if (!maxAttempts)
            break;

        ++_headlessStartAttempts;
        if (TryStartHeadlessSession(candidate, request.BracketId))
        {
            ++_headlessStartsSucceeded;
            ++started;
        }

        --maxAttempts;
    }

    if (request.MissingPlayers > started)
    {
        request.MissingPlayers -= started;
        _pendingRequests.push_back(request);
    }

    TC_LOG_INFO("bg.battleground", "OfflineBotSessionManager processed request: bracket {}, team {}, started {}, remaining {}, candidates {}, recv {}, attempts {}, successes {}, precheck_rejected {}.",
        request.BracketId, uint32(request.Team), started, request.MissingPlayers, availableCandidates, _requestsReceived, _headlessStartAttempts, _headlessStartsSucceeded, _headlessPrecheckRejected);
}

bool OfflineBotSessionManager::TryStartHeadlessSession(Candidate const& candidate, uint32 bracketId)
{
    uint64 nowMs = GameTime::GetGameTimeMS();
    auto itr = _candidates.find(candidate.Guid);
    if (itr != _candidates.end() && itr->second.RetryAfterMs > nowMs)
    {
        ++_headlessPrecheckRejected;
        return false;
    }

    QueryResult result = CharacterDatabase.Query("SELECT online FROM characters WHERE guid = {}", candidate.Guid);
    if (!result || result->Fetch()[0].GetUInt8() != 0)
    {
        if (itr != _candidates.end())
        {
            itr->second.FailedAttempts++;
            itr->second.RetryAfterMs = nowMs + 10000;
        }

        ++_headlessPrecheckRejected;
        return false;
    }

    if (_headlessStartCallback)
        return _headlessStartCallback(candidate.Guid, bracketId);

    TC_LOG_DEBUG("bg.battleground", "OfflineBotSessionManager headless-session placeholder for candidate '{}' ({}) bracket {}.",
        candidate.Name, candidate.Guid, bracketId);
    return false;
}
