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

#include "PlayerbotVhrWaveDriver.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundVHR.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotObcClone.h"
#include "PlayerbotRandomBotParticipation.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "VioletHoldBoons.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "WorldSession.h"

#include <algorithm>
#include <mutex>
#include <array>
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
// Instances whose current run has already had its clones torn down. An
// instance id can be recycled by a later battleground, so entries are dropped
// as soon as the id stops referring to a live match.
std::unordered_set<uint32> g_TornDownInstances;

// The offline half of the bot roster: characters on the managed bot accounts
// and the PvP-only accounts that are not logged in. Read on the world thread
// and kept for a minute, so the waves of one run do not query the database one
// after another.
struct OfflineWaveBot
{
    uint32 lowGuid = 0;
    uint8 level = 0;
    bool pvpOnly = false;
};

// The same split the battleground fill makes (PlayerbotBgFillDriver.cpp): a
// bracket that reaches the level cap draws only from the PvP-only accounts, and
// every lower bracket only from the random population.
constexpr uint32 kPvpOnlyPoolBracketLevel = 60;

bool DrawsFromPvpOnlyPool(Battleground const* bg)
{
    return bg && bg->GetMaxLevel() >= kPvpOnlyPoolBracketLevel;
}

constexpr uint32 kOfflineBotPoolRefreshMs = 60 * IN_MILLISECONDS;
std::vector<OfflineWaveBot> g_OfflineWaveBots;
uint32 g_OfflineWaveBotsLoadedMs = 0;
bool g_OfflineWaveBotsLoaded = false;

// An offline bot has to be loaded from the database before it can be cloned,
// and the load answers on a party member's next session update. How long a
// wave or an ally waits for that before going on without it, and how many
// different offline characters one wave may load: plenty for variety, few
// enough that a forty-clone wave does not put forty character loads on a tick.
constexpr uint32 kOfflineLoadTimeoutMs = 15 * IN_MILLISECONDS;
constexpr size_t kMaxOfflineSourcesPerWave = 12;

// A wave that was dealt at least one offline bot. The battleground keeps a wave
// in AwaitingSpawn until NotifyWaveSpawnFulfilled, so its preparation countdown
// only starts once every clone that is coming is standing in its cell.
struct PendingWaveLoad
{
    uint32 waveNumber = 0;
    uint32 dispatchedMs = 0;
    uint32 outstanding = 0;
    uint32 spawned = 0;
    uint32 slots = 0;
};

// Instance id -> the wave waiting on offline loads, and instance id -> the
// Fellowship requests whose ally is an offline bot being loaded (request id ->
// dispatch time). The driver uses them on the world thread; load callbacks
// update them on whichever thread updates the party member's session.
std::mutex g_OfflineLoadLock;
std::unordered_map<uint32, PendingWaveLoad> g_WaveLoads;
std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_AllyLoads;

void RefreshOfflineWaveBotsIfStale()
{
    uint32 const nowMs = GameTime::GetGameTimeMS();
    if (g_OfflineWaveBotsLoaded && nowMs - g_OfflineWaveBotsLoadedMs < kOfflineBotPoolRefreshMs)
        return;

    g_OfflineWaveBotsLoaded = true;
    g_OfflineWaveBotsLoadedMs = nowMs;
    g_OfflineWaveBots.clear();

    // The PvP-only accounts are kept out of Playerbot.RandomPopulation.BotAccountIds
    // so nothing ever logs those characters in; they are only ever copied.
    std::vector<uint32> accounts = playerbot::RandomBotParticipationManager::GetConfiguredBotAccountIds();
    std::vector<uint32> const& pvpOnlyAccounts = playerbot::PveManager::GetConfig().pvpOnlyAccountIds;
    accounts.insert(accounts.end(), pvpOnlyAccounts.begin(), pvpOnlyAccounts.end());
    std::sort(accounts.begin(), accounts.end());
    accounts.erase(std::unique(accounts.begin(), accounts.end()), accounts.end());
    if (accounts.empty())
        return;

    std::string accountList;
    for (uint32 accountId : accounts)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    // Obcc names are the legacy on-disk Obsidian Colosseum clones, not characters.
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT guid, level, at_login, account FROM characters WHERE online = 0 AND account IN ({}) AND name NOT LIKE 'Obcc%'",
        accountList);
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();

        // Player::LoadFromDB refuses a character that is waiting to be renamed.
        if (fields[2].GetUInt16() & AT_LOGIN_RENAME)
            continue;

        OfflineWaveBot bot;
        bot.lowGuid = fields[0].GetUInt32();
        bot.level = fields[1].GetUInt8();
        bot.pvpOnly = std::binary_search(pvpOnlyAccounts.begin(), pvpOnlyAccounts.end(), fields[3].GetUInt32());
        g_OfflineWaveBots.push_back(bot);
    } while (result->NextRow());
}

// The bot population a BotSourced wave draws from: managed random bots, logged
// in or not, whose level is close enough to the party's to be a fair fight.
//
// Widening RINGS rather than one strict band and then anybody. The old
// fallback took every online bot the moment the strict band came up empty,
// which was survivable while Violet Hold was one 1-80 bracket and everybody
// queued together. With real brackets it is not: a level 60 cloned into a
// 10-19 match is not a hard wave, it is an execution.
//
// Offline bots are only offered when a party member has a session to load them
// on (includeOffline). A ring keeps every online bot and at most
// kMaxOfflineSourcesPerWave offline ones, picked at random each time, so the
// whole roster still turns up over a run without being loaded all at once.
//
// Returning nothing is deliberately allowed. The battleground then clones the
// party itself, which is level-appropriate by definition - a better answer
// than a mismatched stranger, and the wave still cannot stall.
//
// pvpOnlyPool (DrawsFromPvpOnlyPool): the bracket reaches the level cap, so the
// PvP-only characters are the whole population and nobody else is offered.
std::vector<ObjectGuid> CollectBotWaveSources(uint32 partyMinLevel, uint32 partyMaxLevel, bool includeOffline,
    bool pvpOnlyPool)
{
    // Disjoint rings: a bot lands in the tightest one that contains it, so
    // returning the first non-empty ring returns the closest match available.
    static constexpr std::array<uint32, 3> kBands = { { 3, 6, 10 } };
    std::array<std::vector<ObjectGuid>, kBands.size()> onlineRings;
    std::array<std::vector<ObjectGuid>, kBands.size()> offlineRings;

    auto ringOf = [partyMinLevel, partyMaxLevel](uint32 level) -> size_t
    {
        for (size_t ring = 0; ring < kBands.size(); ++ring)
        {
            uint32 const low = partyMinLevel > kBands[ring] ? partyMinLevel - kBands[ring] : 1;
            uint32 const high = partyMaxLevel + kBands[ring];
            if (level >= low && level <= high)
                return ring;
        }
        return kBands.size();
    };

    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, candidate] : ObjectAccessor::GetPlayers())
        {
            if (!candidate || !candidate->IsInWorld())
                continue;

            if (!playerbot::IsManagedRandomBot(candidate))
                continue;

            if (pvpOnlyPool != playerbot::PveManager::IsPvpOnlyBot(candidate))
                continue;

            // A bot already inside a battleground or arena is mid-match somewhere;
            // cloning it works, but its double walking out of a cell while the
            // original fights elsewhere reads as a bug to anyone who knows it.
            if (candidate->InBattleground())
                continue;

            size_t const ring = ringOf(candidate->GetLevel());
            if (ring < kBands.size())
                onlineRings[ring].push_back(guid);
        }
    }

    // Outside the player lock above: FindConnectedPlayer takes it as well.
    if (includeOffline)
    {
        RefreshOfflineWaveBotsIfStale();
        for (OfflineWaveBot const& bot : g_OfflineWaveBots)
        {
            if (pvpOnlyPool != bot.pvpOnly)
                continue;

            size_t const ring = ringOf(bot.level);
            if (ring >= kBands.size())
                continue;

            // Logged in since the pool was read: the online pass owns it now.
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(bot.lowGuid);
            if (ObjectAccessor::FindConnectedPlayer(guid))
                continue;

            offlineRings[ring].push_back(guid);
        }
    }

    for (size_t ring = 0; ring < kBands.size(); ++ring)
    {
        if (onlineRings[ring].empty() && offlineRings[ring].empty())
            continue;

        std::vector<ObjectGuid>& offline = offlineRings[ring];
        if (offline.size() > kMaxOfflineSourcesPerWave)
        {
            Trinity::Containers::RandomShuffle(offline);
            offline.resize(kMaxOfflineSourcesPerWave);
        }

        std::vector<ObjectGuid> sources = std::move(onlineRings[ring]);
        sources.insert(sources.end(), offline.begin(), offline.end());
        return sources;
    }

    return {};
}

// A party member whose session carries this run's offline character loads.
// Null when nobody in the party is connected.
WorldSession* FindPartyLoadSession(BattlegroundVHR* bg, ObjectGuid& ownerGuid)
{
    std::vector<Player const*> humans;
    bg->CollectHumanPlayers(humans);
    for (Player const* human : humans)
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(human->GetGUID());
        WorldSession* session = member ? member->GetSession() : nullptr;
        if (!session || session->IsVirtualSession() || session->IsTransientPlayerSession())
            continue;

        ownerGuid = member->GetGUID();
        return session;
    }

    return nullptr;
}

// A load answers wherever the party member's session is updated: this
// battleground's own map update, or the world update the driver itself runs in,
// and seating clones is safe from either. If they have just left the Hold it
// would be some other map's thread instead, so the load is dropped.
bool CanSeatFromLoadCallback(Battleground const* bg, ObjectGuid ownerGuid)
{
    Player* owner = ObjectAccessor::FindConnectedPlayer(ownerGuid);
    Map const* map = bg->FindBgMap();
    return owner && map && owner->FindMap() == map;
}

BattlegroundVHR* FindRunInProgress(uint32 instanceId, BattlegroundTypeId bgType)
{
    Battleground* bg = sBattlegroundMgr->GetBattleground(instanceId, bgType);
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR || bg->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    return static_cast<BattlegroundVHR*>(bg);
}

// The clone manager seats a new clone - and summons its hunter pet - at its
// team's start position, so point that at the spot for the moment of creation
// and put it back straight after. Teleporting afterwards moved the clone but
// left the pet standing wherever it was made.
Player* SeatClone(BattlegroundVHR* bg, Player* source, uint32 team, Position const& where,
    Position const& restoreIfUnset, std::string const& displayPrefix)
{
    TeamId const teamIndex = Battleground::GetTeamIndexByTeamId(team);
    Position const* previous = bg->GetTeamStartPosition(teamIndex);
    Position const restore = previous ? *previous : restoreIfUnset;
    bg->SetTeamStartPosition(teamIndex, where);
    Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, team, displayPrefix);
    bg->SetTeamStartPosition(teamIndex, restore);
    return clone;
}

bool HasAllyRequest(BattlegroundVHR const* bg, uint32 requestId)
{
    for (VhrAllyRequest const& request : bg->GetPendingAllyRequests())
        if (request.id == requestId)
            return true;

    return false;
}

void ClearAllyLoad(uint32 instanceId, uint32 requestId)
{
    std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
    auto itr = g_AllyLoads.find(instanceId);
    if (itr != g_AllyLoads.end())
        itr->second.erase(requestId);
}

// Boon of Fellowship: one clone of a random level-appropriate managed bot per
// pending request, on the HUMAN team, beside whoever asked. An offline bot is
// loaded first and seated when its load answers. An empty bot pool leaves the
// requests queued and comes back in a couple of seconds.
void FulfilAllyRequests(BattlegroundVHR* bg)
{
    if (!bg->AreAllyRequestsDue())
        return;

    uint32 const instanceId = bg->GetInstanceID();
    BattlegroundTypeId const bgType = bg->GetTypeID();
    uint32 const nowMs = GameTime::GetGameTimeMS();
    ObjectGuid loadOwnerGuid;
    WorldSession* const loadSession = FindPartyLoadSession(bg, loadOwnerGuid);

    // Copy: NotifyAllySpawned edits the queue.
    std::vector<VhrAllyRequest> const requests = bg->GetPendingAllyRequests();

    for (VhrAllyRequest const& request : requests)
    {
        // Its ally is already being loaded. A load that has not answered in
        // time is given up on, and the request is dealt a bot afresh.
        {
            std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
            std::unordered_map<uint32, uint32>& loads = g_AllyLoads[instanceId];
            auto itr = loads.find(request.id);
            if (itr != loads.end())
            {
                if (nowMs - itr->second < kOfflineLoadTimeoutMs)
                    continue;

                loads.erase(itr);
            }
        }

        std::vector<ObjectGuid> bots = CollectBotWaveSources(request.partyMinLevel, request.partyMaxLevel,
            loadSession != nullptr, DrawsFromPvpOnlyPool(bg));
        Player* source = nullptr;
        ObjectGuid offlineSource;
        while (!bots.empty() && !source && !offlineSource)
        {
            uint32 const idx = urand(0, uint32(bots.size()) - 1);
            ObjectGuid const guid = bots[idx];
            bots.erase(bots.begin() + idx);

            if (Player* onlineBot = ObjectAccessor::FindPlayer(guid))
                source = onlineBot;
            else if (loadSession && !ObjectAccessor::FindConnectedPlayer(guid))
                offlineSource = guid;
        }

        if (!source && offlineSource)
        {
            {
                std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
                g_AllyLoads[instanceId][request.id] = nowMs;
            }

            uint32 const requestId = request.id;
            Position const where = request.where;
            bool const queued = playerbot::PlayerbotObcCloneManager::LoadOfflineCloneSource(offlineSource, loadSession,
                [instanceId, bgType, requestId, where, loadOwnerGuid](Player* loaded)
                {
                    ClearAllyLoad(instanceId, requestId);

                    BattlegroundVHR* target = FindRunInProgress(instanceId, bgType);
                    if (!target || !CanSeatFromLoadCallback(target, loadOwnerGuid) || !HasAllyRequest(target, requestId))
                        return;

                    Player* ally = loaded ? SeatClone(target, loaded, target->GetHumanTeam(), where, where, "") : nullptr;
                    if (!ally)
                    {
                        TC_LOG_WARN("playerbots", "PlayerbotVhrWaveDriver: could not field an offline bot as a Fellowship ally for instance {}.",
                            instanceId);
                        target->DeferAllyRequests(2 * IN_MILLISECONDS);
                        return;
                    }

                    target->NotifyAllySpawned(requestId, ally->GetGUID());
                });

            if (queued)
                continue;

            ClearAllyLoad(instanceId, request.id);
        }

        if (!source)
        {
            TC_LOG_DEBUG("playerbots", "PlayerbotVhrWaveDriver: no eligible bot for a Fellowship ally in instance {}; retrying later.",
                instanceId);
            bg->DeferAllyRequests(2 * IN_MILLISECONDS);
            return;
        }

        // Pointed at the requester for the moment of creation and put back, so a
        // party member relogging into the run still lands where the run starts them.
        Player* ally = SeatClone(bg, source, bg->GetHumanTeam(), request.where, request.where, "");
        if (!ally)
        {
            TC_LOG_WARN("playerbots", "PlayerbotVhrWaveDriver: failed to clone {} as a Fellowship ally for instance {}.",
                source->GetName(), instanceId);
            bg->DeferAllyRequests(2 * IN_MILLISECONDS);
            return;
        }

        bg->NotifyAllySpawned(request.id, ally->GetGUID());
    }
}

// Everything a wave clone carries that the clone manager does not give it.
void DressWaveClone(BattlegroundVHR* bg, Player* clone, Player* source, bool isActualHuman, uint32 waveNumber,
    uint8 diminishedStacks)
{
    // A clone of a party member is that member, boons included - the
    // clone manager copies the spellbook and gear but wipes auras. The
    // pairing goes to the battleground as well, which re-copies as the
    // cells open so boons bought during the countdown still land.
    if (isActualHuman)
    {
        VioletHoldBoons::CopyBoonsTo(source, clone);
        bg->NotifyCloneSource(clone->GetGUID(), source->GetGUID());
    }

    // Deep waves reinforce everything that is NOT a party copy: +5%
    // Stamina per wave past BG_VHR_REINFORCE_FROM_WAVE, so the bot-sourced
    // ranks keep pace once the count alone stops being the difficulty.
    if (!isActualHuman && waveNumber > BG_VHR_REINFORCE_FROM_WAVE)
    {
        uint32 const stacks = std::min<uint32>(waveNumber - BG_VHR_REINFORCE_FROM_WAVE, 255);
        if (Aura* reinforced = clone->AddAura(BG_VHR_SPELL_REINFORCED, clone))
        {
            reinforced->SetStackAmount(uint8(stacks));
            // The clone was filled up before this landed, and a percentage
            // stat aura raises the MAXIMUM without touching the current
            // value - so top it up or the wave walks out already hurt.
            clone->SetFullHealth();
        }
        else
            TC_LOG_WARN("playerbots", "PlayerbotVhrWaveDriver: could not reinforce {} for wave {} of instance {}.",
                clone->GetName(), waveNumber, bg->GetInstanceID());
    }

    // The wave's trailing clone is usually a partial one - the difficulty
    // curve advances a quarter of a clone per wave, and Diminished is how
    // the fractions are expressed. Stacks are the percentage removed, so a
    // clone meant to fight at 25% power carries 75 of them.
    //
    // Applied rather than cast: the clone has just been seated and this
    // needs to land before it engages, with no cast time, GCD or line of
    // sight in the way. MOD_INCREASE_HEALTH_PERCENT preserves the current
    // health percentage, so a clone at full health stays full at its new
    // lower maximum.
    if (diminishedStacks)
    {
        if (Aura* diminished = clone->AddAura(BG_VHR_SPELL_DIMINISHED, clone))
            diminished->SetStackAmount(diminishedStacks);
        else
            TC_LOG_WARN("playerbots", "PlayerbotVhrWaveDriver: could not apply Diminished to {} for wave {} of instance {}.",
                clone->GetName(), waveNumber, bg->GetInstanceID());
    }
}

// Seats every wave slot whose source is in the world right now. The indices of
// the slots whose source is not are appended to unseated, when one is given.
uint32 SeatOnlineWaveSlots(BattlegroundVHR* bg, uint32 waveNumber, uint32 enemyTeam,
    std::vector<ObjectGuid> const& sources, std::vector<Position> const& positions,
    std::vector<uint8> const& handicap, std::vector<size_t>* unseated)
{
    TeamId const enemyTeamIndex = Battleground::GetTeamIndexByTeamId(enemyTeam);

    // The seating trick below rewrites the enemy team start position once per
    // clone, and whatever it held last outlives the wave. Anything that reads
    // team starts afterwards - bot objective movement, a relog into the run -
    // would be pointed inside a cell, so put it back when the wave is built.
    Position const enemyStartBeforeWave = bg->GetTeamStartPosition(enemyTeamIndex)
        ? *bg->GetTeamStartPosition(enemyTeamIndex) : Position();

    uint32 spawned = 0;
    for (size_t i = 0; i < sources.size() && i < positions.size(); ++i)
    {
        Player* source = ObjectAccessor::FindPlayer(sources[i]);
        if (!source)
        {
            if (unseated)
                unseated->push_back(i);
            continue;
        }

        // The clone manager seats a new clone - and summons its hunter pet -
        // at the team start position, so aim that at the clone's cell slot
        // before creating it. Teleporting afterwards moved the clone but left
        // the pet standing in the middle of the hold.
        bg->SetTeamStartPosition(enemyTeamIndex, positions[i]);

        // "Dark" is reserved for a memory copied from a real human. Managed
        // playerbots (BotMagArcane and the rest of the random-bot roster) are
        // source material too, but they are not a dark reflection of an
        // actual player and should keep their ordinary display name.
        WorldSession const* sourceSession = source->GetSession();
        bool const isActualHuman = sourceSession && !sourceSession->IsVirtualSession() &&
            !sourceSession->IsTransientPlayerSession() && !playerbot::IsManagedRandomBot(source);
        std::string const displayPrefix = isActualHuman ? "Dark " : "";

        Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, enemyTeam, displayPrefix);
        if (!clone)
        {
            TC_LOG_WARN("playerbots", "PlayerbotVhrWaveDriver: failed to clone {} for wave {} of instance {}.",
                source->GetName(), waveNumber, bg->GetInstanceID());
            continue;
        }

        DressWaveClone(bg, clone, source, isActualHuman, waveNumber, i < handicap.size() ? handicap[i] : 0);
        ++spawned;
    }

    bg->SetTeamStartPosition(enemyTeamIndex, enemyStartBeforeWave);
    return spawned;
}

// Called as each offline bot's load answers, whether or not it fielded clones.
void ResolveWaveLoad(uint32 instanceId, uint32 waveNumber, uint32 fielded)
{
    std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
    auto itr = g_WaveLoads.find(instanceId);
    if (itr == g_WaveLoads.end() || itr->second.waveNumber != waveNumber)
        return;

    itr->second.spawned += fielded;
    if (itr->second.outstanding)
        --itr->second.outstanding;
}

// The battleground an offline wave load was made for, while that wave is still
// the one waiting to be summoned there and nobody has given up on the load.
BattlegroundVHR* FindWaveAwaitingSpawn(uint32 instanceId, BattlegroundTypeId bgType, uint32 waveNumber)
{
    {
        std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
        auto itr = g_WaveLoads.find(instanceId);
        if (itr == g_WaveLoads.end() || itr->second.waveNumber != waveNumber)
            return nullptr;
    }

    BattlegroundVHR* bg = FindRunInProgress(instanceId, bgType);
    if (!bg)
        return nullptr;

    VhrWaveSpawnRequest const* pending = bg->GetPendingWaveSpawnRequest();
    return pending && pending->waveNumber == waveNumber ? bg : nullptr;
}

// A wave already waiting on offline loads: keep waiting until the last one has
// answered or the timeout has passed, then report the wave once. False when
// this instance has no such wave, which makes the request a new one.
bool AdvanceWaveAwaitingLoads(BattlegroundVHR* bg, VhrWaveSpawnRequest const& request)
{
    uint32 const instanceId = bg->GetInstanceID();
    PendingWaveLoad finished;
    {
        std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
        auto itr = g_WaveLoads.find(instanceId);
        if (itr == g_WaveLoads.end())
            return false;

        if (itr->second.waveNumber != request.waveNumber)
        {
            // Left over from a wave this instance has already moved past.
            g_WaveLoads.erase(itr);
            return false;
        }

        if (itr->second.outstanding && GameTime::GetGameTimeMS() - itr->second.dispatchedMs < kOfflineLoadTimeoutMs)
            return true;

        finished = itr->second;
        g_WaveLoads.erase(itr);
    }

    if (finished.outstanding)
        TC_LOG_WARN("playerbots", "PlayerbotVhrWaveDriver: {} offline bot loads for wave {} of instance {} did not answer in time; the wave goes ahead without them.",
            finished.outstanding, finished.waveNumber, instanceId);

    // Nothing arrived at all. An empty wave would be a free one, so fall back
    // on the party picks the battleground sent, as a wave with no eligible
    // bots always has. Copied first: seating clones can touch the request.
    if (!finished.spawned)
    {
        uint32 const enemyTeam = request.enemyTeam;
        std::vector<ObjectGuid> const partySources = request.sourceGuids;
        std::vector<Position> const positions = request.spawnPositions;
        std::vector<uint8> const handicap = request.diminishedStacks;
        finished.spawned = SeatOnlineWaveSlots(bg, finished.waveNumber, enemyTeam, partySources, positions, handicap, nullptr);
    }

    TC_LOG_DEBUG("playerbots", "PlayerbotVhrWaveDriver: wave {} of instance {} fielded {}/{} clones.",
        finished.waveNumber, instanceId, finished.spawned, finished.slots);

    bg->NotifyWaveSpawnFulfilled(finished.waveNumber, finished.spawned);
    return true;
}

void FulfilWaveRequest(BattlegroundVHR* bg)
{
    uint32 const instanceId = bg->GetInstanceID();
    VhrWaveSpawnRequest const* request = bg->GetPendingWaveSpawnRequest();
    if (!request)
    {
        std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
        g_WaveLoads.erase(instanceId);
        return;
    }

    if (AdvanceWaveAwaitingLoads(bg, *request))
        return;

    // A pending request means the previous wave is fully dead (that is the
    // only way a new wave begins), so every ENEMY clone this instance still
    // holds is a corpse. Take them down now rather than letting forty waves of
    // bodies and their transient sessions pile up in the player roster. The
    // party's Fellowship allies are clones on the human team and stay.
    playerbot::PlayerbotObcCloneManager::DestroyCustomGameClones(instanceId, request->enemyTeam);

    // Copy what we need before spawning: the first successful clone mutates
    // the battleground's player roster, and AddPlayer paths may touch the
    // request's storage via world-state updates.
    uint32 const waveNumber = request->waveNumber;
    uint32 const enemyTeam = request->enemyTeam;
    std::vector<ObjectGuid> sources = request->sourceGuids;
    std::vector<Position> const positions = request->spawnPositions;
    std::vector<uint8> const handicap = request->diminishedStacks;

    ObjectGuid loadOwnerGuid;
    WorldSession* const loadSession = FindPartyLoadSession(bg, loadOwnerGuid);

    // Most waves mirror the bot population, not the party. The battleground
    // cannot see the bot roster from the game lib, so it sends party picks as
    // a fallback and this driver re-sources here.
    if (request->composition == VhrWaveComposition::BotSourced)
    {
        std::vector<ObjectGuid> bots = CollectBotWaveSources(request->partyMinLevel, request->partyMaxLevel,
            loadSession != nullptr, DrawsFromPvpOnlyPool(bg));
        if (!bots.empty())
        {
            // Shuffle and deal without repeats until the pool runs dry, so a
            // wave shows as many different bots as the population allows.
            Trinity::Containers::RandomShuffle(bots);
            for (size_t i = 0; i < sources.size(); ++i)
                sources[i] = bots[i % bots.size()];
        }
        else
            TC_LOG_DEBUG("playerbots", "PlayerbotVhrWaveDriver: no eligible bots for wave {} of instance {}, using party fallback.",
                waveNumber, instanceId);
    }

    std::vector<size_t> unseated;
    uint32 const spawned = SeatOnlineWaveSlots(bg, waveNumber, enemyTeam, sources, positions, handicap, &unseated);
    uint32 const slotCount = uint32(std::min(sources.size(), positions.size()));

    // The slots dealt to offline bots: one load per character, however many
    // slots it fills.
    std::map<ObjectGuid, std::vector<size_t>> offlineSlots;
    if (loadSession)
        for (size_t index : unseated)
            if (!ObjectAccessor::FindConnectedPlayer(sources[index]))
                offlineSlots[sources[index]].push_back(index);

    if (offlineSlots.empty())
    {
        TC_LOG_DEBUG("playerbots", "PlayerbotVhrWaveDriver: wave {} of instance {} fielded {}/{} clones.",
            waveNumber, instanceId, spawned, slotCount);

        // Report back even at zero: the battleground's wipe check will close the
        // run out, rather than this driver retrying a wave that cannot spawn.
        bg->NotifyWaveSpawnFulfilled(waveNumber, spawned);
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
        PendingWaveLoad& load = g_WaveLoads[instanceId];
        load = PendingWaveLoad();
        load.waveNumber = waveNumber;
        load.dispatchedMs = GameTime::GetGameTimeMS();
        load.outstanding = uint32(offlineSlots.size());
        load.spawned = spawned;
        load.slots = slotCount;
    }

    BattlegroundTypeId const bgType = bg->GetTypeID();
    for (auto const& entry : offlineSlots)
    {
        std::vector<Position> slotPositions;
        std::vector<uint8> slotHandicap;
        for (size_t index : entry.second)
        {
            slotPositions.push_back(positions[index]);
            slotHandicap.push_back(index < handicap.size() ? handicap[index] : 0);
        }

        bool const queued = playerbot::PlayerbotObcCloneManager::LoadOfflineCloneSource(entry.first, loadSession,
            [instanceId, bgType, waveNumber, enemyTeam, loadOwnerGuid, slotPositions, slotHandicap](Player* loaded)
            {
                uint32 fielded = 0;
                BattlegroundVHR* target = loaded ? FindWaveAwaitingSpawn(instanceId, bgType, waveNumber) : nullptr;
                if (target && CanSeatFromLoadCallback(target, loadOwnerGuid))
                {
                    for (size_t slot = 0; slot < slotPositions.size(); ++slot)
                    {
                        // A bot is never a copy of a party member: no "Dark", no boons.
                        Player* clone = SeatClone(target, loaded, enemyTeam, slotPositions[slot], Position(), "");
                        if (!clone)
                        {
                            TC_LOG_WARN("playerbots", "PlayerbotVhrWaveDriver: failed to clone offline bot {} for wave {} of instance {}.",
                                loaded->GetName(), waveNumber, instanceId);
                            continue;
                        }

                        DressWaveClone(target, clone, loaded, false, waveNumber, slotHandicap[slot]);
                        ++fielded;
                    }
                }

                ResolveWaveLoad(instanceId, waveNumber, fielded);
            });

        if (!queued)
            ResolveWaveLoad(instanceId, waveNumber, 0);
    }
}
}

namespace playerbot
{
void PlayerbotVhrWaveDriver::OnWorldUpdate(uint32 /*diffMs*/)
{
    BattlegroundContainer const* instances = sBattlegroundMgr->GetBattlegroundsByType(BATTLEGROUND_VHR);
    if (!instances)
    {
        g_TornDownInstances.clear();

        std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
        g_WaveLoads.clear();
        g_AllyLoads.clear();
        return;
    }

    std::unordered_set<uint32> liveIds;
    for (auto const& [instanceId, bg] : *instances)
    {
        // Entry 0 is the template, never a live match.
        if (!instanceId || !bg)
            continue;

        liveIds.insert(instanceId);

        BattlegroundVHR* vhr = static_cast<BattlegroundVHR*>(bg);
        switch (vhr->GetStatus())
        {
            case STATUS_IN_PROGRESS:
                FulfilWaveRequest(vhr);
                FulfilAllyRequests(vhr);
                break;
            case STATUS_WAIT_LEAVE:
                // The run is over; take the surviving clones down with it.
                // Guarded so the teardown runs once, not every tick of the
                // leave window.
                if (g_TornDownInstances.insert(instanceId).second)
                    PlayerbotObcCloneManager::DestroyCustomGameClones(instanceId);
                break;
            default:
                break;
        }
    }

    // Forget ids that no longer name a live instance so their eventual reuse
    // is not mistaken for an already-handled run.
    for (auto itr = g_TornDownInstances.begin(); itr != g_TornDownInstances.end();)
    {
        if (liveIds.find(*itr) == liveIds.end())
            itr = g_TornDownInstances.erase(itr);
        else
            ++itr;
    }

    // Loads still out for a run that no longer exists belong to nobody; their
    // callbacks find nothing to seat and simply go away.
    std::lock_guard<std::mutex> guard(g_OfflineLoadLock);
    for (auto itr = g_WaveLoads.begin(); itr != g_WaveLoads.end();)
    {
        if (liveIds.find(itr->first) == liveIds.end())
            itr = g_WaveLoads.erase(itr);
        else
            ++itr;
    }

    for (auto itr = g_AllyLoads.begin(); itr != g_AllyLoads.end();)
    {
        if (itr->second.empty() || liveIds.find(itr->first) == liveIds.end())
            itr = g_AllyLoads.erase(itr);
        else
            ++itr;
    }
}
}
