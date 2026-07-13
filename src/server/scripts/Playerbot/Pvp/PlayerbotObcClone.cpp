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

#include "PlayerbotObcClone.h"

#include "PlayerbotRandomBotParticipation.h"

#include "AccountMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "Duration.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerDump.h"
#include "Realm.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32 kBloodlustSpellId = 2825;
constexpr uint32 kBloodlustDurationMs = 30 * IN_MILLISECONDS;
constexpr uint32 kCloneTickThrottleMs = 1000;

struct ObcCloneConfig
{
    bool enabled = false;
    uint32 cloneAccountId = 0;
};

struct ObcCloneRecord
{
    ObjectGuid cloneGuid;
    uint32 oppositeTeam = 0;
    bool ported = false;
};

std::mutex g_ObcCloneLock;
ObcCloneConfig g_ObcCloneConfig;

// humanGuid -> clone record, plus the reverse cloneGuid -> humanGuid so the
// kill hook can resolve counterparts in either direction in O(1).
std::unordered_map<ObjectGuid, ObcCloneRecord> g_ClonesByHuman;
std::unordered_map<ObjectGuid, ObjectGuid> g_HumanByClone;

uint32 g_CloneTickAccumulatorMs = 0;

bool IsObcCloneFeatureConfigured()
{
    return g_ObcCloneConfig.enabled && g_ObcCloneConfig.cloneAccountId != 0;
}

uint32 OppositeTeam(uint32 team)
{
    return team == ALLIANCE ? HORDE : ALLIANCE;
}

// A human is a clone-eligible participant when it is a real (non-virtual,
// non-managed) player currently inside an in-progress-or-forming Obsidian
// Colosseum instance.
Battleground* GetHumanObcBattleground(Player* player)
{
    if (!player || !player->IsInWorld())
        return nullptr;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsVirtualSession())
        return nullptr;

    if (playerbot::IsManagedRandomBot(player))
        return nullptr;

    Battleground* bg = player->GetBattleground();
    if (!bg || bg->GetTypeID(true) != BATTLEGROUND_OBC)
        return nullptr;

    if (bg->GetStatus() == STATUS_WAIT_LEAVE || bg->GetStatus() == STATUS_NONE)
        return nullptr;

    return bg;
}

// WoW character names are letters-only; build a normalized (first upper, rest
// lower) unique login name for the throwaway clone character. The player never
// sees this - the displayed name is overridden to "Dark <name>" via the
// character cache after login.
std::string GenerateCloneLoginName()
{
    static std::atomic<uint32> counter{ 0 };

    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        uint32 value = counter.fetch_add(1, std::memory_order_relaxed) ^ (GameTime::GetGameTimeMS() & 0xFFFFu);
        std::string name = "Obcc"; // 4-letter prefix, already normalized
        for (int i = 0; i < 6; ++i)
        {
            name += static_cast<char>('a' + (value % 26));
            value /= 26;
        }

        if (!sCharacterCache->GetCharacterCacheByName(name))
            return name;
    }

    return std::string();
}

uint32 AcquireCloneVirtualSessionKey(uint32 lowGuid)
{
    constexpr uint32 kVirtualSessionNamespaceBit = 0x80000000u;
    constexpr uint32 kVirtualSessionProbeLimit = 64u;

    if (!lowGuid)
        return 0;

    uint32 candidateKey = kVirtualSessionNamespaceBit | lowGuid;
    for (uint32 probe = 0; probe < kVirtualSessionProbeLimit; ++probe)
    {
        if (!sWorld->FindSession(candidateKey))
            return candidateKey;

        ++candidateKey;
        if (!(candidateKey & kVirtualSessionNamespaceBit))
            candidateKey = kVirtualSessionNamespaceBit;
    }

    return 0;
}

// Log a freshly-created clone character in on its own virtual session, mirroring
// the managed-bot login path (see TryLoginBotCharacter in
// PlayerbotRandomBotParticipation.cpp). Returns the materialized Player* or
// nullptr on failure.
Player* LoginCloneCharacter(ObjectGuid::LowType cloneLowGuid, uint32 accountId)
{
    if (!cloneLowGuid || !accountId)
        return nullptr;

    uint32 const virtualSessionKey = AcquireCloneVirtualSessionKey(cloneLowGuid);
    if (!virtualSessionKey)
        return nullptr;

    std::string accountName;
    if (!sAccountMgr->GetName(accountId, accountName))
        return nullptr;

    ObjectGuid const cloneGuid = ObjectGuid::Create<HighGuid::Player>(cloneLowGuid);
    int32 const realmId = static_cast<int32>(realm.Id.Realm);
    AccountTypes const security = static_cast<AccountTypes>(sAccountMgr->GetSecurity(accountId, realmId));
    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));

    WorldSession* session = new WorldSession(accountId, std::move(accountName), nullptr, security, expansion, 0, Minutes(0),
        LOCALE_enUS, 0, false);
    session->SetSessionMapKey(virtualSessionKey);
    sWorld->AddSession(session);
    session->AllowCharacterLogin(cloneGuid);

    WorldPacket loginPacket(CMSG_PLAYER_LOGIN, 8);
    loginPacket << cloneGuid;
    session->HandlePlayerLoginOpcode(loginPacket);

    Player* clone = session->GetPlayer();
    if (!clone || clone->GetGUID() != cloneGuid)
    {
        session->KickPlayer("OBC clone login verification failed");
        return nullptr;
    }

    return clone;
}

void HardDeleteCloneCharacter(ObjectGuid cloneGuid, uint32 accountId)
{
    // If still online, log the session out first so DeleteFromDB is not racing
    // a live in-world Player.
    if (Player* clone = ObjectAccessor::FindConnectedPlayer(cloneGuid))
    {
        if (WorldSession* session = clone->GetSession())
        {
            session->LogoutPlayer(true);
            session->KickPlayer("OBC clone teardown");
        }
    }

    Player::DeleteFromDB(cloneGuid, accountId, true /*updateRealmChars*/, true /*deleteFinally*/);
}

// Build and seat a clone for a human already inside an OBC instance. Runs on the
// world thread from the clone tick, so it is safe to log in and teleport another
// player here.
bool ProvisionCloneForHuman(Player* human, Battleground* bg, uint32 cloneAccountId)
{
    if (!human || !bg)
        return false;

    ObjectGuid const humanGuid = human->GetGUID();
    uint32 const humanTeam = human->GetBGTeam();
    uint32 const oppositeTeam = OppositeTeam(humanTeam);

    // Persist the human's current state so the dump reflects live gear/talents.
    human->SaveToDB();

    std::string dump;
    if (PlayerDumpWriter().WriteDumpToString(dump, humanGuid.GetCounter()) != DUMP_SUCCESS)
    {
        TC_LOG_ERROR("playerbots.population", "OBC clone: failed to dump human {} ({}).", human->GetName(), humanGuid.ToString());
        return false;
    }

    std::string const loginName = GenerateCloneLoginName();
    if (loginName.empty())
    {
        TC_LOG_ERROR("playerbots.population", "OBC clone: could not allocate a unique clone login name for human {}.", humanGuid.ToString());
        return false;
    }

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();

    if (PlayerDumpReader().LoadDumpFromString(dump, cloneAccountId, loginName, cloneLowGuid) != DUMP_SUCCESS)
    {
        TC_LOG_ERROR("playerbots.population", "OBC clone: failed to load clone dump for human {} (account {}).", humanGuid.ToString(), cloneAccountId);
        return false;
    }

    Player* clone = LoginCloneCharacter(cloneLowGuid, cloneAccountId);
    if (!clone)
    {
        TC_LOG_ERROR("playerbots.population", "OBC clone: failed to log in clone character {} for human {}.", cloneLowGuid, humanGuid.ToString());
        Player::DeleteFromDB(ObjectGuid::Create<HighGuid::Player>(cloneLowGuid), cloneAccountId, true, true);
        return false;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();

    // Display override: show "Dark <name>" to all clients without touching the
    // varchar(12) DB name. UpdateCharacterData is an in-memory cache update that
    // also broadcasts InvalidatePlayer so clients re-query the name.
    sCharacterCache->UpdateCharacterData(cloneGuid, "Dark " + human->GetName());

    // Seat the clone into the human's exact instance on the opposite team,
    // following the canonical battlefield-port sequence (see
    // WorldSession::HandleBattleFieldPortOpcode). The actual AddPlayer fires when
    // the far teleport completes; for a virtual-session bot that finalization is
    // driven by TryFinalizePendingManagedBotTeleport on the lifecycle tick.
    clone->SetBattlegroundEntryPoint();
    clone->SetBattlegroundId(bg->GetInstanceID(), BATTLEGROUND_OBC);
    clone->SetBGTeam(oppositeTeam);
    sBattlegroundMgr->SendToBattleground(clone, bg->GetInstanceID(), BATTLEGROUND_OBC);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        ObcCloneRecord record;
        record.cloneGuid = cloneGuid;
        record.oppositeTeam = oppositeTeam;
        record.ported = true;
        g_ClonesByHuman[humanGuid] = record;
        g_HumanByClone[cloneGuid] = humanGuid;
    }

    TC_LOG_INFO("playerbots.population", "OBC clone: provisioned 'Dark {}' (clone guid={}) on team {} mirroring human {} in bg instance {}.",
        human->GetName(), cloneGuid.ToString(), oppositeTeam, humanGuid.ToString(), bg->GetInstanceID());
    return true;
}

void TeardownCloneForHuman(ObjectGuid humanGuid)
{
    ObjectGuid cloneGuid;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto itr = g_ClonesByHuman.find(humanGuid);
        if (itr == g_ClonesByHuman.end())
            return;

        cloneGuid = itr->second.cloneGuid;
        g_ClonesByHuman.erase(itr);
        g_HumanByClone.erase(cloneGuid);
    }

    HardDeleteCloneCharacter(cloneGuid, g_ObcCloneConfig.cloneAccountId);
    TC_LOG_INFO("playerbots.population", "OBC clone: tore down clone {} for human {}.", cloneGuid.ToString(), humanGuid.ToString());
}
}

namespace playerbot
{
void PlayerbotObcCloneManager::LoadConfig()
{
    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    g_ObcCloneConfig.enabled = sConfigMgr->GetBoolDefault("Playerbot.PvpLifecycle.QueueOnly.ObsidianColosseum", false);
    g_ObcCloneConfig.cloneAccountId = static_cast<uint32>(std::max<int32>(0, sConfigMgr->GetIntDefault("Playerbot.PvpLifecycle.ObsidianColosseum.CloneAccountId", 0)));

    TC_LOG_INFO("server.loading", "OBC clone mirror config: enabled={} cloneAccountId={}.",
        g_ObcCloneConfig.enabled ? "true" : "false", g_ObcCloneConfig.cloneAccountId);
}

void PlayerbotObcCloneManager::OnStartupSweep()
{
    if (!g_ObcCloneConfig.cloneAccountId)
        return;

    // Every character on the dedicated clone account is an ephemeral clone; wipe
    // any that survived a crash so they do not accumulate or re-enter the world.
    QueryResult result = CharacterDatabase.PQuery("SELECT guid FROM characters WHERE account = {}", g_ObcCloneConfig.cloneAccountId);
    if (!result)
        return;

    uint32 swept = 0;
    do
    {
        ObjectGuid::LowType const lowGuid = (*result)[0].GetUInt32();
        Player::DeleteFromDB(ObjectGuid::Create<HighGuid::Player>(lowGuid), g_ObcCloneConfig.cloneAccountId, true, true);
        ++swept;
    } while (result->NextRow());

    if (swept)
        TC_LOG_INFO("server.loading", "OBC clone startup sweep: deleted {} leftover clone character(s) on account {}.", swept, g_ObcCloneConfig.cloneAccountId);
}

void PlayerbotObcCloneManager::OnWorldUpdate(uint32 diffMs)
{
    if (!IsObcCloneFeatureConfigured())
        return;

    g_CloneTickAccumulatorMs += diffMs;
    if (g_CloneTickAccumulatorMs < kCloneTickThrottleMs)
        return;
    g_CloneTickAccumulatorMs = 0;

    uint32 const cloneAccountId = g_ObcCloneConfig.cloneAccountId;

    // 1) Tear down clones whose human counterpart has left OBC / logged out.
    // Snapshot the tracked human guids under the lock, then evaluate them
    // outside it so we never hold g_ObcCloneLock while taking the player-map
    // lock (keeps a single, consistent lock ordering across this manager).
    std::vector<ObjectGuid> trackedHumans;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        trackedHumans.reserve(g_ClonesByHuman.size());
        for (auto const& [humanGuid, record] : g_ClonesByHuman)
            trackedHumans.push_back(humanGuid);
    }

    for (ObjectGuid const& humanGuid : trackedHumans)
    {
        Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
        if (!human || !GetHumanObcBattleground(human))
            TeardownCloneForHuman(humanGuid);
    }

    // 2) Provision clones for un-mirrored humans currently in an OBC instance.
    // Snapshot candidate humans under the player-map lock only.
    std::vector<ObjectGuid> obcHumans;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (GetHumanObcBattleground(player))
                obcHumans.push_back(guid);
    }

    for (ObjectGuid const& humanGuid : obcHumans)
    {
        {
            std::lock_guard<std::mutex> lock(g_ObcCloneLock);
            if (g_ClonesByHuman.find(humanGuid) != g_ClonesByHuman.end())
                continue;
        }

        Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
        Battleground* bg = human ? GetHumanObcBattleground(human) : nullptr;
        if (!human || !bg)
            continue;

        ProvisionCloneForHuman(human, bg, cloneAccountId);
    }
}

void PlayerbotObcCloneManager::OnPvpKill(Player* killer, Player* killed)
{
    if (!killer || !killed || killer == killed)
        return;

    bool areCounterparts = false;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);

        // human kills their own clone
        auto humanItr = g_ClonesByHuman.find(killer->GetGUID());
        if (humanItr != g_ClonesByHuman.end() && humanItr->second.cloneGuid == killed->GetGUID())
            areCounterparts = true;

        // clone kills its own human
        if (!areCounterparts)
        {
            auto cloneItr = g_HumanByClone.find(killer->GetGUID());
            if (cloneItr != g_HumanByClone.end() && cloneItr->second == killed->GetGUID())
                areCounterparts = true;
        }
    }

    if (!areCounterparts)
        return;

    // The killer (whichever side landed the killing blow) gets Bloodlust for
    // 30s. AddAura applies just the Bloodlust aura itself, skipping the Sated
    // exhaustion debuff that a normal Bloodlust cast would attach.
    if (Aura* aura = killer->AddAura(kBloodlustSpellId, killer))
        aura->SetDuration(kBloodlustDurationMs);
}

void PlayerbotObcCloneManager::OnPlayerLogout(Player const* player)
{
    if (!player)
        return;

    ObjectGuid const guid = player->GetGUID();

    bool isHumanWithClone = false;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        isHumanWithClone = g_ClonesByHuman.find(guid) != g_ClonesByHuman.end();
    }

    if (isHumanWithClone)
        TeardownCloneForHuman(guid);
}

bool PlayerbotObcCloneManager::IsActiveClone(Player const* player)
{
    if (!player)
        return false;

    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    return g_HumanByClone.find(player->GetGUID()) != g_HumanByClone.end();
}
}
