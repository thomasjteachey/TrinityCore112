/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "BattlegroundQueue.h"
#include "BattlegroundMgr.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Globals/ObjectAccessor.h"
#include "Log.h"
#include "OfflineBotSessionManager.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Util.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
class PlayerbotsBattlegroundBridgeWorldScript : public WorldScript
{
public:
    PlayerbotsBattlegroundBridgeWorldScript() : WorldScript("PlayerbotsBattlegroundBridgeWorldScript") { }

    void OnStartup() override
    {
        OfflineBotSessionManager::Instance().Configure();
        OfflineBotSessionManager::Instance().RegisterHeadlessStartCallback([](ObjectGuid::LowType guid, uint32 bracketId)
        {
            TC_LOG_DEBUG("bg.battleground", "Headless start callback placeholder invoked for candidate {} bracket {}.", guid, bracketId);
            return false;
        });

        _enabled = sConfigMgr->GetBoolDefault("Playerbots.BG.WSG.AutoQueue.Enable", false);
        std::string configuredFillers = sConfigMgr->GetStringDefault("Playerbots.BG.WSG.AutoQueue.Fillers", "");

        _fillerNames.clear();
        _configuredFillers.clear();
        for (std::string_view token : Trinity::Tokenize(configuredFillers, ',', false))
        {
            std::string name(token);
            name.erase(name.begin(), std::find_if(name.begin(), name.end(), [](unsigned char c) { return !std::isspace(c); }));
            name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), name.end());
            if (!name.empty())
            {
                _fillerNames.push_back(name);
                LoadFillerMetadata(name);
            }
        }

        TC_LOG_INFO("bg.battleground", "Playerbots WSG auto-queue bridge {} ({} configured filler names, {} metadata entries).", _enabled ? "enabled" : "disabled", _fillerNames.size(), _configuredFillers.size());
    }

    void OnBattlegroundQueueNeedBots(BattlegroundQueue& /*queue*/, BattlegroundTypeId bgTypeId, uint32 bracketId, uint32 allianceMissingPlayers, uint32 hordeMissingPlayers) override
    {
        if (bgTypeId != BATTLEGROUND_WS)
            return;

        if (!allianceMissingPlayers && !hordeMissingPlayers)
            return;

        if (!_enabled || _fillerNames.empty())
        {
            TC_LOG_INFO("bg.battleground", "Playerbots BG bridge needs filler for WSG bracket {} (Alliance missing: {}, Horde missing: {}) but no auto-queue fillers are configured.", bracketId, allianceMissingPlayers, hordeMissingPlayers);
            return;
        }

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketById(489, BattlegroundBracketId(bracketId)); // WSG map id
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bracketEntry || !bgTemplate)
            return;

        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
        uint32 queuedCount = 0;
        uint32 offlineCandidateCount = 0;

        auto tryQueueFiller = [&](TeamId expectedTeam, uint32 countNeeded)
        {
            uint32 queuedForTeam = 0;

            for (FillerMetadata const& fillerMeta : _configuredFillers)
            {
                if (fillerMeta.Team != expectedTeam)
                    continue;

                Player* filler = ObjectAccessor::FindConnectedPlayerByName(fillerMeta.Name);
                if (!filler || filler->GetTeamId() != expectedTeam)
                {
                    ++offlineCandidateCount;
                    continue;
                }

                if (filler->InBattleground() || filler->isUsingLfg() || filler->HasAura(9454) || !filler->HasFreeBattlegroundQueueId())
                    continue;

                if (filler->InBattlegroundQueueForBattlegroundQueueType(bgQueueTypeId))
                    continue;

                if (PvPDifficultyEntry const* fillerBracket = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), filler->GetLevel()))
                    if (fillerBracket->GetBracketId() != bracketEntry->GetBracketId())
                        continue;

                GroupQueueInfo* ginfo = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId).AddGroup(filler, nullptr, bgTypeId, bracketEntry, 0, false, false, 0, 0);
                uint32 queueSlot = filler->AddBattlegroundQueueId(bgQueueTypeId);
                uint32 avgTime = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId).GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
                WorldPacket data;
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgTemplate, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, 0, 0);
                filler->SendDirectMessage(&data);
                ++queuedCount;
                ++queuedForTeam;

                if (queuedForTeam >= countNeeded)
                    break;
            }

            return queuedForTeam;
        };

        uint32 allianceQueued = tryQueueFiller(TEAM_ALLIANCE, allianceMissingPlayers);
        uint32 hordeQueued = tryQueueFiller(TEAM_HORDE, hordeMissingPlayers);

        if (allianceQueued < allianceMissingPlayers)
        {
            _pendingAllianceFillByBracket[bracketEntry->GetBracketId()] += (allianceMissingPlayers - allianceQueued);
            OfflineBotSessionManager::Instance().RequestOfflineFill(bracketEntry->GetBracketId(), TEAM_ALLIANCE, allianceMissingPlayers - allianceQueued);
        }

        if (hordeQueued < hordeMissingPlayers)
        {
            _pendingHordeFillByBracket[bracketEntry->GetBracketId()] += (hordeMissingPlayers - hordeQueued);
            OfflineBotSessionManager::Instance().RequestOfflineFill(bracketEntry->GetBracketId(), TEAM_HORDE, hordeMissingPlayers - hordeQueued);
        }

        if (queuedCount)
            sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());

        TC_LOG_INFO("bg.battleground", "Playerbots BG bridge processed WSG bracket {} (Alliance missing: {}, Horde missing: {}, queued fillers: {}, offline candidates seen: {}).", bracketId, allianceMissingPlayers, hordeMissingPlayers, queuedCount, offlineCandidateCount);
    }

    void OnUpdate(uint32 diff) override
    {
        OfflineBotSessionManager::Instance().Update(diff);

        if (!_enabled || _configuredFillers.empty())
            return;

        _retryTimer += diff;
        if (_retryTimer < 5000)
            return;
        _retryTimer = 0;

        RetryPendingForTeam(TEAM_ALLIANCE);
        RetryPendingForTeam(TEAM_HORDE);
    }

private:
    struct FillerMetadata
    {
        std::string Name;
        ObjectGuid::LowType Guid = 0;
        TeamId Team = TEAM_ALLIANCE;
    };

    void LoadFillerMetadata(std::string const& name)
    {
        std::string escapedName = name;
        CharacterDatabase.EscapeString(escapedName);

        QueryResult result = CharacterDatabase.Query("SELECT guid, race FROM characters WHERE name = '{}' LIMIT 1", escapedName);
        if (!result)
            return;

        Field* fields = result->Fetch();
        FillerMetadata metadata;
        metadata.Name = name;
        metadata.Guid = fields[0].GetUInt32();
        metadata.Team = Player::TeamForRace(fields[1].GetUInt8()) == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE;
        _configuredFillers.push_back(metadata);
        OfflineBotSessionManager::Instance().UpsertCandidate({ metadata.Guid, metadata.Team, metadata.Name });
    }

    bool _enabled = false;
    uint32 _retryTimer = 0;
    std::vector<std::string> _fillerNames;
    std::vector<FillerMetadata> _configuredFillers;
    std::unordered_map<uint32, uint32> _pendingAllianceFillByBracket;
    std::unordered_map<uint32, uint32> _pendingHordeFillByBracket;

    void RetryPendingForTeam(TeamId team)
    {
        std::unordered_map<uint32, uint32>& pendingMap = team == TEAM_ALLIANCE ? _pendingAllianceFillByBracket : _pendingHordeFillByBracket;
        BattlegroundTypeId bgTypeId = BATTLEGROUND_WS;
        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            return;

        for (auto itr = pendingMap.begin(); itr != pendingMap.end();)
        {
            if (!itr->second)
            {
                itr = pendingMap.erase(itr);
                continue;
            }

            uint32 bracketId = itr->first;
            PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketById(bgTemplate->GetMapId(), BattlegroundBracketId(bracketId));
            if (!bracketEntry)
            {
                ++itr;
                continue;
            }

            uint32 queuedNow = 0;
            for (FillerMetadata const& fillerMeta : _configuredFillers)
            {
                if (!itr->second)
                    break;

                if (fillerMeta.Team != team)
                    continue;

                Player* filler = ObjectAccessor::FindConnectedPlayerByName(fillerMeta.Name);
                if (!filler || filler->GetTeamId() != team || filler->InBattleground() || filler->isUsingLfg() || filler->HasAura(9454) || !filler->HasFreeBattlegroundQueueId())
                    continue;

                if (filler->InBattlegroundQueueForBattlegroundQueueType(bgQueueTypeId))
                    continue;

                if (PvPDifficultyEntry const* fillerBracket = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), filler->GetLevel()))
                    if (fillerBracket->GetBracketId() != bracketEntry->GetBracketId())
                        continue;

                GroupQueueInfo* ginfo = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId).AddGroup(filler, nullptr, bgTypeId, bracketEntry, 0, false, false, 0, 0);
                uint32 queueSlot = filler->AddBattlegroundQueueId(bgQueueTypeId);
                uint32 avgTime = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId).GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
                WorldPacket data;
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgTemplate, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, 0, 0);
                filler->SendDirectMessage(&data);
                ++queuedNow;
                --itr->second;
            }

            if (queuedNow)
                sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());

            if (!itr->second)
                itr = pendingMap.erase(itr);
            else
                ++itr;
        }
    }
};
}

void AddSC_custom_playerbots_bg_bridge()
{
    new PlayerbotsBattlegroundBridgeWorldScript();
}
