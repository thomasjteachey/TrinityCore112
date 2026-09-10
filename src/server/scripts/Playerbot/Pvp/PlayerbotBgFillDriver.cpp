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

#include "PlayerbotBgFillDriver.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Configuration/Config.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotObcClone.h"
#include "PlayerbotPvpCore.h"
#include "PlayerbotRandomBotParticipation.h"
#include "PlayerbotResourceGovernor.h"
#include "SharedDefines.h"
#include "StringConvert.h"
#include "Timer.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
constexpr uint32 kTickIntervalMs = 500;
constexpr uint32 kQueueWakeIntervalMs = 2 * IN_MILLISECONDS;
constexpr uint32 kOfflinePoolRefreshMs = 60 * IN_MILLISECONDS;
// An offline copy is a background character load; if it has not produced a
// clone in this long the request is presumed lost (the session carrying the
// callback logged out, most likely) and the seat is tried again.
constexpr uint32 kOfflineCloneTimeoutMs = 10 * IN_MILLISECONDS;

struct BgFillConfig
{
    bool enabled = false;
    std::set<uint32> battlegroundTypes;
    uint32 queueWaitMs = 15 * IN_MILLISECONDS;
    uint32 maxPerTeam = 15;
    uint32 clonesPerTick = 2;
    bool useOfflineBots = true;
    bool allowHumanMirrors = true;
};

BgFillConfig g_Config;
uint32 g_TickAccumulatorMs = 0;
uint32 g_QueueWakeAccumulatorMs = 0;

// Offline characters on the managed-bot accounts, refreshed at most once a
// minute and only when a match actually runs out of online bots to copy.
struct OfflineCandidate
{
    uint32 lowGuid = 0;
    uint8 level = 1;
};

struct OfflinePool
{
    bool loaded = false;
    uint32 loadedMs = 0;
    std::vector<OfflineCandidate> entries;
};

OfflinePool g_OfflinePool;

// Copies requested from offline characters that have not materialised yet.
// They hold their seat so a slow load is not doubled up by the next tick.
struct PendingOfflineClone
{
    uint32 instanceId = 0;
    uint32 team = 0;
    ObjectGuid sourceGuid;
    uint32 requestedMs = 0;
};

std::vector<PendingOfflineClone> g_PendingOfflineClones;

// Instances announced in the log as being filled, so the notice goes out once
// per match rather than once per tick. Ids are recycled by later matches, so
// entries are dropped as soon as the id stops naming a live one.
std::unordered_set<uint32> g_AnnouncedInstances;

struct TeamTally
{
    uint32 humans = 0;          // people, plus seats held by people who dropped and may return
    uint32 virtualBots = 0;     // persistent managed bots that queued in on their own
    uint32 clones = 0;          // transient copies, this driver's and anyone else's
    std::vector<ObjectGuid> humanGuids;
};

struct MatchTally
{
    TeamTally teams[PVP_TEAMS_COUNT];

    uint32 Humans() const { return teams[TEAM_ALLIANCE].humans + teams[TEAM_HORDE].humans; }
    uint32 Clones() const { return teams[TEAM_ALLIANCE].clones + teams[TEAM_HORDE].clones; }
};

MatchTally TallyMatch(Battleground const* bg)
{
    MatchTally tally;
    for (auto const& [guid, bgPlayer] : bg->GetPlayers())
    {
        TeamId const teamIndex = Battleground::GetTeamIndexByTeamId(bgPlayer.Team);
        if (teamIndex >= PVP_TEAMS_COUNT)
            continue;

        TeamTally& team = tally.teams[teamIndex];
        Player const* player = ObjectAccessor::FindConnectedPlayer(guid);
        // A participant the server no longer has connected is a person who
        // dropped: the battleground keeps the seat for a while in case they
        // come back, and so does this count.
        WorldSession const* session = player ? player->GetSession() : nullptr;
        if (session && session->IsTransientPlayerSession())
            ++team.clones;
        else if (session && session->IsVirtualSession())
            ++team.virtualBots;
        else
        {
            ++team.humans;
            if (player)
                team.humanGuids.push_back(guid);
        }
    }

    return tally;
}

bool IsFillCandidate(Battleground const* bg, uint32 instanceId)
{
    if (!instanceId || !bg || !bg->isBattleground() || bg->IsCustomGame() || bg->isRated())
        return false;

    // A Random Battleground instance carries the queued-for type and the
    // rolled one; both have to be covered, or the roll could land on a
    // battleground the operator excluded.
    return sBattlegroundMgr->IsBotFillBattleground(bg->GetTypeID()) &&
        sBattlegroundMgr->IsBotFillBattleground(bg->GetTypeID(true));
}

uint32 FillCapForMatch(Battleground const* bg)
{
    uint32 const battlegroundMax = bg->GetMaxPlayersPerTeam();
    if (!g_Config.maxPerTeam)
        return battlegroundMax;

    return std::min(g_Config.maxPerTeam, battlegroundMax);
}

// Sources already represented in this match, so nobody is dealt twice into one
// game: every live clone's source plus every offline copy still on its way.
std::unordered_set<ObjectGuid> CollectUsedSources(uint32 instanceId,
    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const& clones)
{
    std::unordered_set<ObjectGuid> used;
    for (auto const& clone : clones)
        used.insert(clone.sourceGuid);

    for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        if (pending.instanceId == instanceId)
            used.insert(pending.sourceGuid);

    return used;
}

// Online managed bots whose level sits inside the bracket, shuffled. A bot
// already inside a battleground is mid-match somewhere; cloning it works, but
// its double walking around while the original fights elsewhere reads as a
// bug to anyone who knows it.
std::vector<ObjectGuid> CollectOnlineSources(uint32 minLevel, uint32 maxLevel, std::unordered_set<ObjectGuid> const& excluded)
{
    std::vector<ObjectGuid> sources;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, candidate] : ObjectAccessor::GetPlayers())
        {
            if (!candidate || !candidate->IsInWorld())
                continue;

            if (!playerbot::IsManagedRandomBot(candidate))
                continue;

            if (candidate->InBattleground())
                continue;

            uint32 const level = candidate->GetLevel();
            if (level < minLevel || level > maxLevel)
                continue;

            if (excluded.count(guid))
                continue;

            sources.push_back(guid);
        }
    }

    Trinity::Containers::RandomShuffle(sources);
    return sources;
}

void RefreshOfflinePoolIfStale(uint32 nowMs)
{
    if (g_OfflinePool.loaded && nowMs - g_OfflinePool.loadedMs < kOfflinePoolRefreshMs)
        return;

    g_OfflinePool.loaded = true;
    g_OfflinePool.loadedMs = nowMs;
    g_OfflinePool.entries.clear();

    std::vector<uint32> const accounts = playerbot::RandomBotParticipationManager::GetConfiguredBotAccountIds();
    if (accounts.empty())
        return;

    std::string accountList;
    accountList.reserve(accounts.size() * 6);
    for (uint32 accountId : accounts)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    // Same shape as the population manager's own pool query. The Obcc names
    // are the legacy on-disk Obsidian Colosseum clones: not real characters.
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT guid, level FROM characters WHERE online = 0 AND account IN ({}) AND name NOT LIKE 'Obcc%'", accountList);
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        OfflineCandidate candidate;
        candidate.lowGuid = fields[0].GetUInt32();
        candidate.level = fields[1].GetUInt8();
        g_OfflinePool.entries.push_back(candidate);
    } while (result->NextRow());

    TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: offline bot pool refreshed, {} characters.", uint32(g_OfflinePool.entries.size()));
}

std::vector<ObjectGuid> CollectOfflineSources(uint32 minLevel, uint32 maxLevel, std::unordered_set<ObjectGuid> const& excluded)
{
    std::vector<ObjectGuid> sources;
    for (OfflineCandidate const& candidate : g_OfflinePool.entries)
    {
        if (candidate.level < minLevel || candidate.level > maxLevel)
            continue;

        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(candidate.lowGuid);
        if (excluded.count(guid))
            continue;

        // Came online since the pool was read: the online pass owns it now,
        // and it may well be in a match already.
        if (ObjectAccessor::FindConnectedPlayer(guid))
            continue;

        sources.push_back(guid);
    }

    Trinity::Containers::RandomShuffle(sources);
    return sources;
}

// A real player's session in the match to hang an offline character load on;
// the callback runs when that session next updates. Anyone will do, a
// teammate of the seat being filled by preference so the request outlives the
// other side emptying out.
WorldSession* FindCallbackSession(MatchTally const& tally, TeamId preferredTeam)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        TeamId const teamIndex = pass == 0 ? preferredTeam : (preferredTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE);
        for (ObjectGuid const& guid : tally.teams[teamIndex].humanGuids)
            if (Player* human = ObjectAccessor::FindConnectedPlayer(guid))
                if (WorldSession* session = human->GetSession())
                    return session;
    }

    return nullptr;
}

// Drop pending offline requests that have produced their clone, timed out, or
// belong to a match that no longer exists.
void ReconcilePendingOfflineClones(uint32 nowMs)
{
    for (auto itr = g_PendingOfflineClones.begin(); itr != g_PendingOfflineClones.end();)
    {
        bool done = nowMs - itr->requestedMs >= kOfflineCloneTimeoutMs;
        if (!done)
        {
            if (!sBattlegroundMgr->GetBattleground(itr->instanceId, BATTLEGROUND_TYPE_NONE))
                done = true;
            else
                for (auto const& clone : playerbot::PlayerbotObcCloneManager::GetCustomGameClones(itr->instanceId))
                    if (clone.sourceGuid == itr->sourceGuid)
                    {
                        done = true;
                        break;
                    }
        }

        if (done)
            itr = g_PendingOfflineClones.erase(itr);
        else
            ++itr;
    }
}

uint32 CountPendingOfflineClones(uint32 instanceId, uint32 team)
{
    uint32 count = 0;
    for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        if (pending.instanceId == instanceId && pending.team == team)
            ++count;
    return count;
}

// Remove `count` of this driver's clones from one team, dead ones first.
void ShedClonesFromTeam(Battleground* bg, uint32 team, uint32 count,
    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const& clones)
{
    std::vector<ObjectGuid> victims;
    for (auto const& clone : clones)
    {
        if (clone.team != team)
            continue;

        Player const* player = ObjectAccessor::FindConnectedPlayer(clone.cloneGuid);
        if (player && !player->IsAlive())
            victims.insert(victims.begin(), clone.cloneGuid);
        else
            victims.push_back(clone.cloneGuid);
    }

    if (victims.size() > count)
        victims.resize(count);

    for (ObjectGuid const& victim : victims)
    {
        if (playerbot::PlayerbotObcCloneManager::DestroyCustomGameClone(victim))
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: removed surplus clone {} from team {} of instance {}.",
                victim.ToString(), team, bg->GetInstanceID());
    }
}

// Seat up to `wanted` clones on one team. Returns how many were created or
// requested. `matchClones` and `totalClones` feed the governor and are
// advanced as seats are taken.
uint32 AddClonesToTeam(Battleground* bg, uint32 team, uint32 wanted, MatchTally const& tally,
    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const& clones,
    uint32& matchClones, uint32& totalClones, uint32 nowMs)
{
    if (!wanted)
        return 0;

    uint32 const minLevel = bg->GetMinLevel();
    uint32 const maxLevel = std::max(bg->GetMaxLevel(), minLevel);
    TeamId const teamIndex = Battleground::GetTeamIndexByTeamId(team);
    std::unordered_set<ObjectGuid> used = CollectUsedSources(bg->GetInstanceID(), clones);

    uint32 added = 0;
    auto governorAllows = [&]()
    {
        return playerbot::ResourceGovernor::CanAddCustomMatchBot(matchClones, totalClones);
    };

    // 1. Online bots in the bracket.
    std::vector<ObjectGuid> online = CollectOnlineSources(minLevel, maxLevel, used);
    for (ObjectGuid const& sourceGuid : online)
    {
        if (added >= wanted || !governorAllows())
            return added;

        Player* source = ObjectAccessor::FindConnectedPlayer(sourceGuid);
        if (!source)
            continue;

        // Managed bots keep their own names; "Dark" is reserved for a copy of
        // a real person, as in the Violet Hold waves.
        Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, team, "");
        if (!clone)
        {
            TC_LOG_WARN("playerbots.bgfill", "PlayerbotBgFillDriver: failed to clone {} into instance {}.", source->GetName(), bg->GetInstanceID());
            continue;
        }

        used.insert(sourceGuid);
        ++added;
        ++matchClones;
        ++totalClones;
        TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: seated {} (copy of online bot {}) on team {} of instance {}.",
            clone->GetName(), source->GetName(), team, bg->GetInstanceID());
    }

    // 2. Offline characters from the bot accounts, loaded in the background.
    if (added < wanted && g_Config.useOfflineBots)
    {
        RefreshOfflinePoolIfStale(nowMs);
        std::vector<ObjectGuid> offline = CollectOfflineSources(minLevel, maxLevel, used);
        WorldSession* callbackSession = offline.empty() ? nullptr : FindCallbackSession(tally, teamIndex);
        for (ObjectGuid const& sourceGuid : offline)
        {
            if (added >= wanted || !callbackSession || !governorAllows())
                break;

            if (!playerbot::PlayerbotObcCloneManager::QueueCustomGameClone(sourceGuid, callbackSession, bg, team, ""))
                continue;

            PendingOfflineClone pending;
            pending.instanceId = bg->GetInstanceID();
            pending.team = team;
            pending.sourceGuid = sourceGuid;
            pending.requestedMs = nowMs;
            g_PendingOfflineClones.push_back(pending);

            used.insert(sourceGuid);
            ++added;
            ++matchClones;
            ++totalClones;
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: requested a copy of offline character {} for team {} of instance {}.",
                sourceGuid.ToString(), team, bg->GetInstanceID());
        }
    }

    // 3. Mirrors of the people on the other side, one each.
    if (added < wanted && g_Config.allowHumanMirrors)
    {
        TeamId const otherIndex = teamIndex == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
        for (ObjectGuid const& humanGuid : tally.teams[otherIndex].humanGuids)
        {
            if (added >= wanted || !governorAllows())
                break;

            if (used.count(humanGuid))
                continue;

            Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
            if (!human || !human->IsInWorld())
                continue;

            Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(human, bg, team, "Dark ");
            if (!clone)
                continue;

            used.insert(humanGuid);
            ++added;
            ++matchClones;
            ++totalClones;
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: no bot fits levels {}-{}; seated a mirror of {} on team {} of instance {}.",
                minLevel, maxLevel, human->GetName(), team, bg->GetInstanceID());
        }
    }

    return added;
}

struct LiveMatch
{
    Battleground* bg = nullptr;
    uint32 instanceId = 0;
    MatchTally tally;
};

// Every live public battleground this driver is responsible for. Instances
// are stored under the type that was queued for, and every such type has a
// queue, so walking the queue types reaches all of them.
std::vector<LiveMatch> CollectLiveMatches()
{
    std::vector<LiveMatch> matches;
    for (int queueType = BATTLEGROUND_QUEUE_NONE + 1; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
    {
        BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(queueType));
        if (!sBattlegroundMgr->IsBotFillBattleground(bgTypeId))
            continue;

        BattlegroundContainer const* instances = sBattlegroundMgr->GetBattlegroundsByType(bgTypeId);
        if (!instances)
            continue;

        for (auto const& [instanceId, bg] : *instances)
        {
            if (!IsFillCandidate(bg, instanceId))
                continue;

            BattlegroundStatus const status = bg->GetStatus();
            if (status != STATUS_WAIT_JOIN && status != STATUS_IN_PROGRESS)
                continue;

            LiveMatch match;
            match.bg = bg;
            match.instanceId = instanceId;
            match.tally = TallyMatch(bg);
            matches.push_back(std::move(match));
        }
    }

    return matches;
}

void FillMatch(LiveMatch& match, uint32& totalClones, uint32 nowMs)
{
    Battleground* bg = match.bg;
    uint32 const cap = FillCapForMatch(bg);

    if (!bg->IsBotFillMatch())
    {
        bg->SetBotFillMatch(true);
        // The premature-finish rule ends a match whose team is below the
        // template minimum; a cap under that minimum would trip it on purpose.
        if (cap < bg->GetMinPlayersPerTeam())
            bg->SetMinPlayersPerTeam(cap);
    }

    // Nobody real inside yet - invites still pending, or everyone gone. A
    // clone is only worth seating when there is someone to fight, and the
    // stock rules end or delete the shell on their own. The map is also only
    // guaranteed to exist once somebody has actually teleported in.
    if (!match.tally.Humans() || !bg->FindBgMap())
        return;

    if (g_AnnouncedInstances.insert(match.instanceId).second)
        TC_LOG_INFO("playerbots.bgfill", "PlayerbotBgFillDriver: filling {} instance {} (levels {}-{}) around {} real player(s), up to {} a side.",
            bg->GetName(), match.instanceId, bg->GetMinLevel(), bg->GetMaxLevel(), match.tally.Humans(), cap);

    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const clones =
        playerbot::PlayerbotObcCloneManager::GetCustomGameClones(match.instanceId);
    uint32 matchClones = match.tally.Clones();

    for (uint32 teamIndex = TEAM_ALLIANCE; teamIndex < PVP_TEAMS_COUNT; ++teamIndex)
    {
        uint32 const team = teamIndex == TEAM_ALLIANCE ? ALLIANCE : HORDE;
        TeamTally const& tally = match.tally.teams[teamIndex];

        // Seats that are not the clones' to take: people in, persistent bots
        // in, and invites still outstanding (a person on the way in).
        uint32 const taken = tally.humans + tally.virtualBots + bg->GetInvitedCount(team);
        uint32 const desired = cap > taken ? cap - taken : 0;
        uint32 const present = tally.clones + CountPendingOfflineClones(match.instanceId, team);

        if (present > desired)
            ShedClonesFromTeam(bg, team, present - desired, clones);
        else if (present < desired)
        {
            uint32 const wanted = std::min(desired - present, std::max<uint32>(g_Config.clonesPerTick, 1));
            AddClonesToTeam(bg, team, wanted, match.tally, clones, matchClones, totalClones, nowMs);
        }
    }
}

// Under sustained hard pressure the governor asks for one bot at a time to go;
// the custom-game lobby answers for its matches, this answers for these.
void ShedForGovernor(std::vector<LiveMatch> const& matches)
{
    if (!playerbot::ResourceGovernor::ShouldShedBotNow())
        return;

    LiveMatch const* target = nullptr;
    for (LiveMatch const& match : matches)
        if (match.tally.Humans() && match.tally.Clones() && (!target || match.tally.Clones() > target->tally.Clones()))
            target = &match;

    if (!target)
        return;

    if (playerbot::PlayerbotObcCloneManager::ShedOneCustomGameClone(target->instanceId))
        playerbot::ResourceGovernor::NoteBotShedExecuted();
}

// The queue only re-examines a bracket when something happens in it or on its
// fifteen-second sweep. A person whose wait has run out deserves better than
// the sweep, so nudge the queue for them.
void WakeQueuesForWaitingPlayers(uint32 nowMs)
{
    for (int queueType = BATTLEGROUND_QUEUE_NONE + 1; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
    {
        BattlegroundQueueTypeId const queueTypeId = BattlegroundQueueTypeId(queueType);
        BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        if (!sBattlegroundMgr->IsBotFillBattleground(bgTypeId))
            continue;

        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        for (uint32 bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
        {
            bool wake = false;
            for (uint32 index = BG_QUEUE_PREMADE_ALLIANCE; index < BG_QUEUE_GROUP_TYPES_COUNT && !wake; ++index)
            {
                for (GroupQueueInfo const* ginfo : queue.m_QueuedGroups[bracket][index])
                {
                    if (ginfo->IsInvitedToBGInstanceGUID || getMSTimeDiff(ginfo->JoinTime, nowMs) < g_Config.queueWaitMs)
                        continue;

                    for (auto const& [playerGuid, playerInfo] : ginfo->Players)
                    {
                        (void)playerInfo;
                        Player const* player = ObjectAccessor::FindConnectedPlayer(playerGuid);
                        WorldSession const* session = player ? player->GetSession() : nullptr;
                        if (session && !session->IsVirtualSession())
                        {
                            wake = true;
                            break;
                        }
                    }

                    if (wake)
                        break;
                }
            }

            if (wake)
                sBattlegroundMgr->ScheduleQueueUpdate(0, 0, queueTypeId, bgTypeId, BattlegroundBracketId(bracket));
        }
    }
}
}

namespace playerbot
{
void PlayerbotBgFillDriver::LoadConfig()
{
    BgFillConfig config;
    config.enabled = sConfigMgr->GetBoolDefault("Playerbot.BgFill.Enable", false) && PvpCore::GetConfig().moduleEnabled;
    config.queueWaitMs = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.QueueWaitSeconds", 15), 0)) * IN_MILLISECONDS;
    config.maxPerTeam = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.MaxPerTeam", 15), 0));
    config.clonesPerTick = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.ClonesPerTick", 2), 1));
    config.useOfflineBots = sConfigMgr->GetBoolDefault("Playerbot.BgFill.UseOfflineBots", true);
    config.allowHumanMirrors = sConfigMgr->GetBoolDefault("Playerbot.BgFill.AllowHumanMirrors", true);

    std::string const types = sConfigMgr->GetStringDefault("Playerbot.BgFill.BattlegroundTypes", "");
    for (std::string_view token : Trinity::Tokenize(types, ',', false))
        if (Optional<uint32> typeId = Trinity::StringTo<uint32>(token))
            if (*typeId)
                config.battlegroundTypes.insert(*typeId);

    g_Config = std::move(config);
    g_OfflinePool.loaded = false;

    BattlegroundMgr::BotFillPolicy policy;
    policy.enabled = g_Config.enabled;
    policy.battlegroundTypes = g_Config.battlegroundTypes;
    policy.queueWaitMs = g_Config.queueWaitMs;
    sBattlegroundMgr->SetBotFillPolicy(std::move(policy));

    if (g_Config.enabled)
    {
        PvpCoreConfig const& pvp = PvpCore::GetConfig();
        if (!pvp.pvpCoreEnabled || !pvp.pvpLifecycleEnabled)
            TC_LOG_WARN("server.loading", "Playerbot battleground fill is enabled but the PvP core/lifecycle modules are off; clones would stand idle.");
    }

    TC_LOG_INFO("server.loading", "Playerbot battleground fill config: enabled={}, types={}, queueWait={} ms, maxPerTeam={}, clonesPerTick={}, offlineBots={}, humanMirrors={}.",
        g_Config.enabled ? "true" : "false", g_Config.battlegroundTypes.empty() ? "all" : types,
        g_Config.queueWaitMs, g_Config.maxPerTeam, g_Config.clonesPerTick,
        g_Config.useOfflineBots ? "true" : "false", g_Config.allowHumanMirrors ? "true" : "false");
}

void PlayerbotBgFillDriver::OnWorldUpdate(uint32 diffMs)
{
    if (!g_Config.enabled)
        return;

    uint32 const nowMs = GameTime::GetGameTimeMS();

    g_QueueWakeAccumulatorMs += diffMs;
    if (g_QueueWakeAccumulatorMs >= kQueueWakeIntervalMs)
    {
        g_QueueWakeAccumulatorMs = 0;
        WakeQueuesForWaitingPlayers(nowMs);
    }

    g_TickAccumulatorMs += diffMs;
    if (g_TickAccumulatorMs < kTickIntervalMs)
        return;
    g_TickAccumulatorMs = 0;

    ReconcilePendingOfflineClones(nowMs);

    std::vector<LiveMatch> matches = CollectLiveMatches();

    std::unordered_set<uint32> liveIds;
    uint32 totalClones = 0;
    for (LiveMatch const& match : matches)
    {
        liveIds.insert(match.instanceId);
        totalClones += match.tally.Clones();
    }
    for (auto itr = g_AnnouncedInstances.begin(); itr != g_AnnouncedInstances.end();)
    {
        if (liveIds.find(*itr) == liveIds.end())
            itr = g_AnnouncedInstances.erase(itr);
        else
            ++itr;
    }

    ShedForGovernor(matches);

    for (LiveMatch& match : matches)
        FillMatch(match, totalClones, nowMs);
}
}
