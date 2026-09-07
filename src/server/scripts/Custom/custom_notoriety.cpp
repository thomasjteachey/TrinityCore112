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

// Notoriety: selling the price on your own head. See custom_notoriety.h for
// what the system is for; this file is how it works.

#include "custom_notoriety.h"

#include "custom_bounty.h"
#include "custom_barracks_hardcore.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"

#include "Chat.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Duration.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    bool s_enabled = false;
    uint32 s_questId = 60001;
    uint32 s_fenceEntry = 900201;
    uint32 s_contractStacks = 15;
    uint32 s_stacksPerTier = 5;
    uint32 s_maxTiers = 7;
    // Zero, because ClearStacksOnTurnIn already gates the repeat: selling the
    // page takes the marks with it, so another contract costs fifteen more
    // kills. A timer on top of that is dead waiting, not a limit.
    uint32 s_cooldownSeconds = 0;
    uint32 s_maxRerolls = 2;
    // The per-checkpoint debuffs. Base is the first spell id of a contiguous
    // block; the Nth threshold in s_checkpointStacks uses base + N. Zero
    // disables the whole thing, which is the DEFAULT and is deliberate: applying
    // an aura the client has no Spell.dbc row for shows the player a blank icon
    // with no name, so this stays off until the row ships.
    uint32 s_checkpointBase = 0;

    // The thresholds themselves, and NOT simply every fifth stack.
    //
    // A debuff is a promise that something changed. Twenty, thirty-five and
    // forty-five arm nothing: the contract payout tier steps and the continuous
    // scalars keep sliding, but they were sliding on the stack before and will
    // on the stack after. An icon there says "something happened" when nothing
    // did, and once one rung is filler the player stops reading the rest.
    //
    // So these are exactly the stack counts where a threshold in the bounty
    // engine actually fires. Kept as config rather than derived because the
    // thresholds live in a different translation unit's statics, and a list that
    // silently disagrees with them is worse than one somebody has to update.
    std::vector<uint32> s_checkpointStacks;

    // Last stack count each player's checkpoint auras were synced against.
    // SyncCheckpointAuras is called from the per-tick player update, so the
    // common case has to be one integer compare and nothing else.
    std::unordered_map<uint64, uint32> g_checkpointSynced;
    float s_minYards = 500.0f;
    float s_maxYards = 900.0f;
    float s_fenceAppearYards = 150.0f;
    bool s_voidOnDeath = true;
    // Selling the page settles the debt. The payout is computed from the banked
    // peak before this runs, so clearing costs the seller nothing.
    bool s_clearStacksOnTurnIn = true;

    // Money is copper per tier, experience is a share of the killer's own next
    // level - the same "a fraction of the bar" shape the rest of the realm
    // pays in, so it stays meaningful at 20 and at 60.
    uint32 s_moneyBase = 5000;
    uint32 s_moneyPerTier = 3000;
    float s_xpBubblesBase = 2.0f;
    float s_xpBubblesPerTier = 1.0f;

    struct Contract
    {
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        uint32 StacksAtAccept = 0;
        uint32 PeakStacks = 0;
        uint32 Rerolls = 0;
        time_t AcceptedAt = 0;
        // Where the registrar who wrote this was standing. There are 48 Grix
        // spawns on B+, so "back to Grix" has to mean the one you actually took
        // it from, not the nearest one to wherever the fence turned out to be.
        uint32 OriginMapId = 0;
        float OriginX = 0.0f;
        float OriginY = 0.0f;
        float OriginZ = 0.0f;
        float OriginO = 0.0f;
        bool HasOrigin = false;
    };

    std::mutex g_lock;
    std::unordered_map<uint64, Contract> g_contracts;
    // The fence currently standing for each holder. Map thread only - it is
    // written and read from the owner's own update tick and nowhere else.
    std::unordered_map<uint64, ObjectGuid> g_fenceByHolder;
    std::unordered_map<uint64, time_t> g_cooldownUntil;

    // Read on the hot paths before the lock is ever taken. A realm where nobody
    // is carrying a contract - which is almost always - pays one relaxed load.
    std::atomic<bool> g_anyContract{ false };

    void LoadNotorietyConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.Notoriety.Enable", false);
        s_questId = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.QuestId", 60001)));
        s_fenceEntry = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.FenceCreatureId", 900201)));
        s_contractStacks = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.ContractStacks", 15), 1, 255));
        s_stacksPerTier = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.StacksPerTier", 5), 1, 255));
        s_maxTiers = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.MaxTiers", 7), 0, 100));
        s_cooldownSeconds = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.CooldownSeconds", 0)));
        s_maxRerolls = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.MaxRerolls", 2), 0, 20));
        s_checkpointBase = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.CheckpointAuraBase", 0)));

        // Sorted ascending and de-duplicated, because the id a rung uses is its
        // INDEX in this list: an unsorted or repeated entry would quietly point
        // two rungs at one spell and leave another unused.
        s_checkpointStacks.clear();
        {
            std::stringstream stream(sConfigMgr->GetStringDefault(
                "Centurion.Notoriety.CheckpointStacks", "5,10,15,25,30,40,50"));
            std::string token;
            while (std::getline(stream, token, ','))
            {
                uint32 const stacks = uint32(std::strtoul(token.c_str(), nullptr, 10));
                if (stacks)
                    s_checkpointStacks.push_back(stacks);
            }
        }
        std::sort(s_checkpointStacks.begin(), s_checkpointStacks.end());
        s_checkpointStacks.erase(std::unique(s_checkpointStacks.begin(), s_checkpointStacks.end()),
            s_checkpointStacks.end());
        s_fenceAppearYards = std::max(20.0f,
            sConfigMgr->GetFloatDefault("Centurion.Notoriety.FenceAppearYards", 150.0f));
        s_minYards = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Notoriety.RendezvousMinYards", 500.0f));
        s_maxYards = std::max(s_minYards, sConfigMgr->GetFloatDefault("Centurion.Notoriety.RendezvousMaxYards", 900.0f));
        s_voidOnDeath = sConfigMgr->GetBoolDefault("Centurion.Notoriety.VoidOnDeath", true);
        s_clearStacksOnTurnIn = sConfigMgr->GetBoolDefault("Centurion.Notoriety.ClearStacksOnTurnIn", true);

        s_moneyBase = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.MoneyBaseCopper", 5000)));
        s_moneyPerTier = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.MoneyPerTierCopper", 3000)));
        s_xpBubblesBase = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Notoriety.XpBubblesBase", 2.0f));
        s_xpBubblesPerTier = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Notoriety.XpBubblesPerTier", 1.0f));
    }

    uint32 TierFor(uint32 stacks)
    {
        if (stacks <= s_contractStacks || !s_stacksPerTier)
            return 0;

        return std::min(s_maxTiers, (stacks - s_contractStacks) / s_stacksPerTier);
    }

    void EraseContract(uint64 rawGuid)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_contracts.erase(rawGuid);
            g_anyContract.store(!g_contracts.empty(), std::memory_order_relaxed);
        }

        CharacterDatabase.PExecute("DELETE FROM character_notoriety_contract WHERE guid = {}",
            ObjectGuid(rawGuid).GetCounter());
    }
}

namespace Notoriety
{
    bool Enabled() { return s_enabled; }
    uint32 QuestId() { return s_questId; }
    uint32 ContractStacks() { return s_contractStacks; }

    bool HasLiveContract(ObjectGuid guid)
    {
        if (!g_anyContract.load(std::memory_order_relaxed))
            return false;

        std::lock_guard<std::mutex> guard(g_lock);
        return g_contracts.count(guid.GetRawValue()) != 0;
    }

    bool GetRendezvous(ObjectGuid guid, uint32& mapId, uint32& zoneId, Position& out)
    {
        if (!g_anyContract.load(std::memory_order_relaxed))
            return false;

        std::lock_guard<std::mutex> guard(g_lock);
        auto itr = g_contracts.find(guid.GetRawValue());
        if (itr == g_contracts.end())
            return false;

        mapId = itr->second.MapId;
        zoneId = itr->second.ZoneId;
        out.Relocate(itr->second.X, itr->second.Y, itr->second.Z);
        return true;
    }

    bool ShouldOffer(Player const* player)
    {
        if (!s_enabled || !player || BarracksHardcore::IsPlayerbot(player))
            return false;

        if (Bounty::GetStacks(player) < s_contractStacks)
            return false;

        uint64 const raw = player->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_contracts.count(raw))
            return false;

        auto itr = g_cooldownUntil.find(raw);
        return itr == g_cooldownUntil.end() || itr->second <= GameTime::GetGameTime();
    }

    void SendRendezvousPoi(Player* player)
    {
        if (!player)
            return;

        uint32 mapId = 0;
        uint32 zoneId = 0;
        Position spot;
        if (!GetRendezvous(player->GetGUID(), mapId, zoneId, spot))
            return;

        // Hand-built because PlayerMenu::SendPointOfInterest can only send a row
        // out of points_of_interest, and this marker is different for every
        // contract. Field order is that function's, verbatim.
        WorldPacket data(SMSG_GOSSIP_POI, 4 + 4 + 4 + 4 + 4 + 16);
        data << uint32(0);                      // flags
        data << float(spot.GetPositionX());
        data << float(spot.GetPositionY());
        data << uint32(7);                      // icon: a plain marker
        data << uint32(0);                      // importance
        data << "The Quiet Man";
        player->GetSession()->SendPacket(&data);

        float const distance = player->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
        ChatHandler(player->GetSession()).PSendSysMessage(
            "The meeting is roughly %.0f paces off. Look for the mark on your map.", distance);
    }

    bool IssueContract(Player* player, WorldObject const* origin)
    {
        if (!s_enabled || !player)
            return false;

        uint32 const stacks = Bounty::GetStacks(player);

        // Seeded from the player and the moment, so a re-roll lands somewhere
        // else while a re-read of the same contract lands in the same place.
        uint32 const seed = uint32(player->GetGUID().GetCounter()) * 2654435761u
            + uint32(GameTime::GetGameTime());

        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 zoneId = 0;
        if (!playerbot::PveManager::PickGroundSpotInBand(player->GetMapId(), player->GetZoneId(),
            player->GetPositionX(), player->GetPositionY(), s_minYards, s_maxYards, seed, x, y, z, zoneId))
        {
            TC_LOG_INFO("playerbots.hardcore",
                "Notoriety: no rendezvous ground within {:.0f}-{:.0f}y of {} in zone {}.",
                s_minYards, s_maxYards, player->GetName(), player->GetZoneId());
            return false;
        }

        Contract contract;
        contract.MapId = player->GetMapId();
        contract.ZoneId = zoneId;
        contract.X = x;
        contract.Y = y;
        contract.Z = z;
        contract.StacksAtAccept = stacks;
        contract.PeakStacks = stacks;
        contract.AcceptedAt = GameTime::GetGameTime();

        // Where to send them back to. The registrar if we were handed one, the
        // contract's existing origin on a re-roll, and the player's own feet as
        // the last resort - they are standing at Grix when they accept, so that
        // is very nearly the same answer anyway.
        if (origin)
        {
            contract.OriginMapId = origin->GetMapId();
            contract.OriginX = origin->GetPositionX();
            contract.OriginY = origin->GetPositionY();
            contract.OriginZ = origin->GetPositionZ();
            contract.OriginO = origin->GetOrientation();
            contract.HasOrigin = true;
        }

        uint64 const raw = player->GetGUID().GetRawValue();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (auto existing = g_contracts.find(raw); existing != g_contracts.end())
            {
                contract.Rerolls = existing->second.Rerolls;

                // A re-roll moves the meeting, not the man who arranged it.
                if (!contract.HasOrigin && existing->second.HasOrigin)
                {
                    contract.OriginMapId = existing->second.OriginMapId;
                    contract.OriginX = existing->second.OriginX;
                    contract.OriginY = existing->second.OriginY;
                    contract.OriginZ = existing->second.OriginZ;
                    contract.OriginO = existing->second.OriginO;
                    contract.HasOrigin = true;
                }
            }

            if (!contract.HasOrigin)
            {
                contract.OriginMapId = player->GetMapId();
                contract.OriginX = player->GetPositionX();
                contract.OriginY = player->GetPositionY();
                contract.OriginZ = player->GetPositionZ();
                contract.OriginO = player->GetOrientation();
                contract.HasOrigin = true;
            }

            g_contracts[raw] = contract;
            g_anyContract.store(true, std::memory_order_relaxed);
        }

        CharacterDatabase.PExecute(
            "REPLACE INTO character_notoriety_contract "
            "(guid, mapId, zoneId, posX, posY, posZ, stacksAtAccept, peakStacks, acceptedAt, rerolls, "
            "originMapId, originX, originY, originZ, originO) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
            player->GetGUID().GetCounter(), contract.MapId, contract.ZoneId,
            contract.X, contract.Y, contract.Z, contract.StacksAtAccept, contract.PeakStacks,
            uint32(contract.AcceptedAt), contract.Rerolls,
            contract.OriginMapId, contract.OriginX, contract.OriginY, contract.OriginZ,
            contract.OriginO);

        SendRendezvousPoi(player);

        TC_LOG_INFO("playerbots.hardcore",
            "Notoriety: {} (level {}) took a contract at {} stack(s); fence at {:.0f},{:.0f} in zone {} ({:.0f}y).",
            player->GetName(), player->GetLevel(), stacks, x, y, zoneId,
            player->GetExactDist2d(x, y));
        return true;
    }

    bool RerollContract(Player* player)
    {
        if (!player)
            return false;

        uint64 const raw = player->GetGUID().GetRawValue();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = g_contracts.find(raw);
            if (itr == g_contracts.end())
                return false;
            if (itr->second.Rerolls >= s_maxRerolls)
                return false;

            ++itr->second.Rerolls;
        }

        return IssueContract(player);
    }

    void VoidContract(Player* player, char const* reason)
    {
        if (!player || !HasLiveContract(player->GetGUID()))
            return;

        EraseContract(player->GetGUID().GetRawValue());
        DespawnFence(player);

        // FAILED in the log where the player is standing, not silently left to
        // fail at the fence four hundred yards later.
        //
        // This was previously RemoveActiveQuest(id, false) and RemoveRewardedQuest,
        // which between them told the CLIENT nothing at all. The status map lost
        // the quest server-side, so the fence would have refused it - but the
        // false suppresses SendQuestUpdate, and neither call touches the quest LOG
        // SLOT, which is the replicated PLAYER_QUEST_LOG_* field the client
        // actually reads. The contract therefore sat in the player's log looking
        // perfectly alive until they relogged.
        //
        // Player::FailQuest is not the answer either, and would be a coin flip
        // if it were: it returns immediately unless the quest is INCOMPLETE, or
        // COMPLETE and both TIMED and COMPLETED_AT_START (Player.cpp:17080).
        // The contract is INCOMPLETE for the length of the walk and COMPLETE the
        // instant the fence gives its speak-to credit, so whether FailQuest did
        // anything at all would depend on which side of that click you died on.
        //
        // So the failure is sent explicitly and then the quest is taken out by
        // the same route the abandon handler uses (QuestHandler.cpp:411-447),
        // which does not care what the status was.
        //
        // Removed rather than left sitting as FAILED on purpose: a quest in any
        // status but NONE fails SatisfyQuestStatus, so a failed row left in the
        // log would block the next contract forever.
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(s_questId))
        {
            player->SendQuestFailed(s_questId);

            if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED))
                player->RemoveTimedQuest(s_questId);

            player->TakeQuestSourceItem(s_questId, false);
            player->AbandonQuest(s_questId);

            uint16 const logSlot = player->FindQuestSlot(s_questId);
            if (logSlot < MAX_QUEST_LOG_SIZE)
                player->SetQuestSlot(logSlot, 0);

            // update defaults to true here, unlike before, so SendQuestUpdate runs.
            player->RemoveActiveQuest(s_questId);
            player->RemoveRewardedQuest(s_questId);
        }

        ChatHandler(player->GetSession()).PSendSysMessage("%s", reason);

        TC_LOG_INFO("playerbots.hardcore", "Notoriety: {}'s contract was voided - {}",
            player->GetName(), reason);
    }

    void OnStacksChanged(Player* player, uint32 newStacks)
    {
        if (!s_enabled || !player || !g_anyContract.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> guard(g_lock);
        auto itr = g_contracts.find(player->GetGUID().GetRawValue());
        if (itr == g_contracts.end() || newStacks <= itr->second.PeakStacks)
            return;

        // Bank the high-water mark. This is what makes the walk aggressive
        // rather than fearful: every hunter you put down on the road raises the
        // payout, and nothing that happens afterwards can lower it.
        itr->second.PeakStacks = newStacks;
        CharacterDatabase.PExecute(
            "UPDATE character_notoriety_contract SET peakStacks = {} WHERE guid = {}",
            newStacks, player->GetGUID().GetCounter());
    }

    // Take the fence away. Safe to call for somebody who never had one.
    void DespawnFence(Player* player)
    {
        if (!player)
            return;

        // Under g_lock, like every other file-scope container here. It was not,
        // and with a single contract holder that never mattered because only one
        // player's tick ever touched the map. The moment TWO people carry
        // contracts it is a concurrent find/erase/insert on one unordered_map,
        // which is undefined behaviour, and the first report of anything odd
        // came from a party where two members had one each.
        //
        // The guid is taken under the lock and the despawn happens outside it:
        // DespawnOrUnsummon reaches into the map, and holding a global mutex
        // across that is how two locks come to be taken in two orders.
        ObjectGuid fenceGuid;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = g_fenceByHolder.find(player->GetGUID().GetRawValue());
            if (itr == g_fenceByHolder.end())
                return;

            fenceGuid = itr->second;
            g_fenceByHolder.erase(itr);
        }

        if (Creature* fence = ObjectAccessor::GetCreature(*player, fenceGuid))
            fence->DespawnOrUnsummon();
    }

    // Put the fence on the ground when its buyer gets close, and take it away
    // again when they leave. Summoned rather than spawned because the meeting is
    // different for every contract, and visibleBySummonerOnly because it is
    // nobody else's meeting - which also means bots cannot see, target or path
    // to it, so the errand adds no new work to the fleet.
    void UpdateFence(Player* player)
    {
        if (!s_enabled || !player || !player->IsInWorld())
            return;

        uint64 const raw = player->GetGUID().GetRawValue();

        uint32 mapId = 0;
        uint32 zoneId = 0;
        Position spot;
        if (!GetRendezvous(player->GetGUID(), mapId, zoneId, spot) || player->GetMapId() != mapId)
        {
            DespawnFence(player);
            return;
        }

        float const distance = player->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());

        ObjectGuid existing;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = g_fenceByHolder.find(raw);
            if (itr != g_fenceByHolder.end())
                existing = itr->second;
        }

        bool const standing = !existing.IsEmpty() &&
            ObjectAccessor::GetCreature(*player, existing) != nullptr;

        // Hysteresis on purpose: summon inside the fade-in band, remove well
        // outside it, so somebody standing on the boundary does not watch him
        // blink in and out once a second.
        if (distance <= s_fenceAppearYards)
        {
            if (standing)
                return;

            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_fenceByHolder.erase(raw);
            }

            if (Creature* fence = player->SummonCreature(s_fenceEntry, spot,
                TEMPSUMMON_MANUAL_DESPAWN, 0s, 0, 0, /*visibleBySummonerOnly*/ true))
            {
                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    g_fenceByHolder[raw] = fence->GetGUID();
                }

                TC_LOG_INFO("playerbots.hardcore",
                    "Notoriety: the fence stands for {} at {:.0f},{:.0f} in zone {}.",
                    player->GetName(), spot.GetPositionX(), spot.GetPositionY(), zoneId);
            }
            else
                TC_LOG_ERROR("playerbots.hardcore",
                    "Notoriety: could not summon fence {} for {} - the contract cannot be sold.",
                    s_fenceEntry, player->GetName());
            return;
        }

        if (distance > s_fenceAppearYards * 2.0f)
            DespawnFence(player);
    }

    // Keep the ladder of checkpoint debuffs in step with the notoriety count.
    //
    // Driven from the per-player update tick rather than from the handful of
    // places the count changes, and that is the whole point. Stacks move on a
    // kill, on the aura expiring, on death, on a contract turn-in, on login and
    // on a GM command, and enumerating those was exactly the mistake that let a
    // feared bot keep swinging - one path nobody thought of and the state is
    // wrong until something else happens to fix it. Reading the answer every
    // tick cannot be wrong, so long as it is cheap, and it is: one lookup and
    // one integer compare unless the count actually moved.
    void SyncCheckpointAuras(Player* player)
    {
        if (!s_enabled || !s_checkpointBase || s_checkpointStacks.empty() || !player)
            return;

        uint64 const key = player->GetGUID().GetRawValue();
        uint32 const stacks = Bounty::GetStacks(player);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto const itr = g_checkpointSynced.find(key);
            if (itr != g_checkpointSynced.end() && itr->second == stacks)
                return;

            g_checkpointSynced[key] = stacks;
        }

        for (size_t index = 0; index < s_checkpointStacks.size(); ++index)
        {
            uint32 const rung = s_checkpointStacks[index];
            uint32 const spellId = s_checkpointBase + uint32(index);

            if (stacks >= rung)
            {
                // AddAura is a no-op when it is already there, but asking first
                // keeps this off the aura-application path entirely on the ticks
                // where only ONE rung changed - which is every tick that is not
                // the very first.
                if (!player->HasAura(spellId))
                    player->AddAura(spellId, player);
            }
            else
                player->RemoveAurasDueToSpell(spellId);
        }
    }

    void ForgetCheckpointSync(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_checkpointSynced.erase(guid.GetRawValue());
    }

    void GrantGoodieBag(Payout const& payout)
    {
        // THE SEAM. Money and experience are already paid when this runs.
        //
        // When the bag lands it goes here: build a level-appropriate loot set
        // from payout.Level and payout.Tier and hand it over - most likely
        // through CustomLootChests::PlayerChestBuilder, which already summons a
        // chest, registers it and is understood by the bot fleet.
        (void)payout;
    }
}

// ---------------------------------------------------------------------------
// The fence.
// ---------------------------------------------------------------------------
class npc_notoriety_fence : public CreatureScript
{
public:
    npc_notoriety_fence() : CreatureScript("npc_notoriety_fence") { }

    struct npc_notoriety_fenceAI : public ScriptedAI
    {
        npc_notoriety_fenceAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            me->Whisper("Quietly, now. Have you got the page?", LANG_UNIVERSAL, player);

            // Tick the objective BEFORE the menu is built, not after.
            //
            // quest_template gives 60001 RequiredNpcOrGo1 = this creature, so
            // the contract now sits in the log as "Speak to the Quiet Man 0/1"
            // for the whole walk instead of arriving already complete. Finding
            // him is the objective, and this is the moment he is found.
            //
            // Returning false hands the click on to NPCHandler, which calls
            // PrepareGossipMenu -> PrepareQuestMenu, and that reads the quest's
            // status as it is right now. Award the credit afterwards and the
            // player is looking at a menu built from the incomplete state,
            // having to shut the window and click again to hand in.
            //
            // KilledMonsterCredit is a no-op for anyone not on the quest, so it
            // needs no guard of its own.
            if (player)
                player->KilledMonsterCredit(me->GetEntry(), me->GetGUID());

            return false;   // let the quest menu through
        }

        void OnQuestReward(Player* player, Quest const* quest, uint32 /*opt*/) override
        {
            if (!player || !quest || quest->GetQuestId() != Notoriety::QuestId())
                return;

            uint32 stacks = 0;
            uint32 rerolls = 0;
            // Read the ride home out under the same lock as the payout, because
            // everything below EraseContract has already forgotten it.
            bool hasOrigin = false;
            uint32 originMap = 0;
            float originX = 0.0f, originY = 0.0f, originZ = 0.0f, originO = 0.0f;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                auto itr = g_contracts.find(player->GetGUID().GetRawValue());
                if (itr == g_contracts.end())
                    return;

                stacks = std::max(itr->second.PeakStacks, itr->second.StacksAtAccept);
                rerolls = itr->second.Rerolls;
                hasOrigin = itr->second.HasOrigin;
                originMap = itr->second.OriginMapId;
                originX = itr->second.OriginX;
                originY = itr->second.OriginY;
                originZ = itr->second.OriginZ;
                originO = itr->second.OriginO;
            }

            uint32 const tier = TierFor(stacks);

            // Paid HERE and not from quest_template, and that is not taste.
            // Player.cpp: a repeatable quest pays its template XP exactly once
            // per character, ever, and silently zero on every hand-in after
            // that. The first turn-in would work and every one after it would
            // quietly pay nothing.
            uint32 const money = s_moneyBase + s_moneyPerTier * tier;
            float const bubbles = s_xpBubblesBase + s_xpBubblesPerTier * float(tier);
            uint32 const levelXp = sObjectMgr->GetXPForLevel(player->GetLevel());
            uint32 const xp = uint32(float(levelXp) * bubbles / 20.0f);

            if (money)
                player->ModifyMoney(int64(money));
            if (xp)
                player->GiveXP(xp, nullptr);

            Notoriety::Payout payout;
            payout.Who = player;
            payout.Stacks = stacks;
            payout.Tier = tier;
            payout.ZoneId = player->GetZoneId();
            payout.MoneyPaid = money;
            payout.XpPaid = xp;
            payout.Level = player->GetLevel();
            Notoriety::GrantGoodieBag(payout);

            EraseContract(player->GetGUID().GetRawValue());
            Notoriety::DespawnFence(player);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_cooldownUntil[player->GetGUID().GetRawValue()] =
                    GameTime::GetGameTime() + time_t(s_cooldownSeconds);
            }

            if (s_clearStacksOnTurnIn)
                Bounty::ClearBounty(player);

            me->Whisper("Nobody will hear this from me.", LANG_UNIVERSAL, player);

            // The ride home. The walk out is the feature; walking back is just
            // the same ground again with nothing hunting you, because selling
            // the page took the notoriety with it.
            //
            // Delayed rather than immediate so the reward panel and the whisper
            // land first - a player yanked away mid-sentence has no idea what
            // they were paid. Captured by GUID, never by pointer: this fires two
            // seconds later on the map thread, by which time the player may have
            // logged out and the fence is certainly gone.
            if (hasOrigin)
            {
                ObjectGuid const who = player->GetGUID();
                player->m_Events.AddEventAtOffset([who, originMap, originX, originY, originZ, originO]()
                {
                    if (Player* seller = ObjectAccessor::FindPlayer(who))
                        if (seller->IsInWorld() && seller->IsAlive() && !seller->IsInCombat())
                            seller->TeleportTo(originMap, originX, originY, originZ, originO);
                }, Seconds(2));
            }

            TC_LOG_INFO("playerbots.hardcore",
                "Notoriety: {} (level {}) sold a contract at {} peak stack(s), tier {}, "
                "for {}c and {} xp ({} reroll(s)).",
                player->GetName(), player->GetLevel(), stacks, tier, money, xp, rerolls);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_notoriety_fenceAI(creature);
    }
};

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
class custom_notoriety_world : public WorldScript
{
public:
    custom_notoriety_world() : WorldScript("custom_notoriety_world") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadNotorietyConfig();
        if (s_enabled)
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_notoriety_contract ("
                "guid INT UNSIGNED NOT NULL PRIMARY KEY,"
                "mapId SMALLINT UNSIGNED NOT NULL,"
                "zoneId INT UNSIGNED NOT NULL,"
                "posX FLOAT NOT NULL, posY FLOAT NOT NULL, posZ FLOAT NOT NULL,"
                "stacksAtAccept TINYINT UNSIGNED NOT NULL DEFAULT 0,"
                "peakStacks TINYINT UNSIGNED NOT NULL DEFAULT 0,"
                "acceptedAt INT UNSIGNED NOT NULL,"
                "rerolls TINYINT UNSIGNED NOT NULL DEFAULT 0,"
                // Where the registrar who wrote it was standing, so the fence
                // can send the seller back to that one. Defaulted rather than
                // NOT NULL so a table predating these columns and ALTERed in
                // place reads the same as a fresh one; the load treats an
                // all-zero position as "no origin" rather than as map 0.
                "originMapId SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
                "originX FLOAT NOT NULL DEFAULT 0,"
                "originY FLOAT NOT NULL DEFAULT 0,"
                "originZ FLOAT NOT NULL DEFAULT 0,"
                "originO FLOAT NOT NULL DEFAULT 0"
                ") ENGINE=InnoDB");
    }
};

class custom_notoriety_player : public PlayerScript
{
public:
    custom_notoriety_player() : PlayerScript("custom_notoriety_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        // Only for somebody who actually has the quest. The bot fleet is
        // hundreds of logins and none of them can carry a contract, so this
        // costs them an in-memory quest-status read and no query at all.
        if (!s_enabled || !player || BarracksHardcore::IsPlayerbot(player))
            return;
        if (player->GetQuestStatus(s_questId) == QUEST_STATUS_NONE)
            return;

        QueryResult result = CharacterDatabase.PQuery(
            "SELECT mapId, zoneId, posX, posY, posZ, stacksAtAccept, peakStacks, acceptedAt, rerolls, "
            "originMapId, originX, originY, originZ, originO "
            "FROM character_notoriety_contract WHERE guid = {}", player->GetGUID().GetCounter());
        if (!result)
            return;

        Field* fields = result->Fetch();
        Contract contract;
        contract.MapId = fields[0].GetUInt32();
        contract.ZoneId = fields[1].GetUInt32();
        contract.X = fields[2].GetFloat();
        contract.Y = fields[3].GetFloat();
        contract.Z = fields[4].GetFloat();
        contract.StacksAtAccept = fields[5].GetUInt8();
        contract.PeakStacks = fields[6].GetUInt8();
        contract.AcceptedAt = time_t(fields[7].GetUInt32());
        contract.Rerolls = fields[8].GetUInt8();
        contract.OriginMapId = fields[9].GetUInt16();
        contract.OriginX = fields[10].GetFloat();
        contract.OriginY = fields[11].GetFloat();
        contract.OriginZ = fields[12].GetFloat();
        contract.OriginO = fields[13].GetFloat();
        // Contracts written before the origin columns existed default to all
        // zeroes, which is a real position on map 0 and not one anybody wants to
        // be teleported into. Treat the origin as absent instead.
        contract.HasOrigin = contract.OriginX != 0.0f || contract.OriginY != 0.0f;

        std::lock_guard<std::mutex> guard(g_lock);
        g_contracts[player->GetGUID().GetRawValue()] = contract;
        g_anyContract.store(true, std::memory_order_relaxed);
    }

    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        // One relaxed atomic load for every player on a realm where nobody is
        // carrying a contract, which is almost always.
        if (!s_enabled || !g_anyContract.load(std::memory_order_relaxed) || !player)
            return;

        Notoriety::UpdateFence(player);
    }

    void OnLogout(Player* player) override
    {
        // The summon is private to this player, so it has nobody left to be
        // visible to. It comes back when they walk in again.
        Notoriety::DespawnFence(player);
    }

    void OnPlayerJustDied(Player* victim, Unit* /*killer*/) override
    {
        if (!s_enabled || !s_voidOnDeath || !victim)
            return;

        // At the corpse, immediately, and said out loud. The notoriety itself
        // clears on this same event, so the contract has nothing left to sell -
        // failing it here rather than silently at the fence is the difference
        // between a rule and a trick.
        Notoriety::VoidContract(victim, "The page burns. Nobody buys a dead man's name.");
    }
};

// ---------------------------------------------------------------------------
// Diagnostics. Every silent gate above gets one visible answer here.
// ---------------------------------------------------------------------------
class custom_notoriety_commands : public CommandScript
{
public:
    custom_notoriety_commands() : CommandScript("custom_notoriety_commands") { }

    Trinity::ChatCommands::ChatCommandTable GetCommands() const override
    {
        using namespace Trinity::ChatCommands;
        static ChatCommandTable notorietyTable =
        {
            { "why", HandleNotorietyWhy, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "go", HandleNotorietyGo, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "reroll", HandleNotorietyReroll, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "bag", HandleNotorietyBag, rbac::RBAC_PERM_COMMAND_GM, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "notoriety", notorietyTable },
        };
        return commandTable;
    }

    static Player* Resolve(ChatHandler* handler, Optional<std::string> const& name)
    {
        if (!name || name->empty())
            return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;

        return ObjectAccessor::FindPlayerByName(*name);
    }

    static bool HandleNotorietyWhy(ChatHandler* handler, Optional<std::string> name)
    {
        Player* target = Resolve(handler, name);
        if (!target)
        {
            handler->SendSysMessage("No such player.");
            return true;
        }

        handler->PSendSysMessage("Notoriety for %s:", target->GetName().c_str());
        handler->PSendSysMessage("  enabled=%s  quest=%u  contract_at=%u stacks",
            s_enabled ? "yes" : "NO", s_questId, s_contractStacks);
        handler->PSendSysMessage("  live stacks=%u  should_offer=%s",
            Bounty::GetStacks(target), Notoriety::ShouldOffer(target) ? "yes" : "NO");

        uint32 mapId = 0, zoneId = 0;
        Position spot;
        if (Notoriety::GetRendezvous(target->GetGUID(), mapId, zoneId, spot))
        {
            std::lock_guard<std::mutex> guard(g_lock);
            Contract const& c = g_contracts[target->GetGUID().GetRawValue()];
            handler->PSendSysMessage("  contract: peak=%u accepted_at=%u rerolls=%u tier=%u",
                c.PeakStacks, c.StacksAtAccept, c.Rerolls, TierFor(std::max(c.PeakStacks, c.StacksAtAccept)));
            handler->PSendSysMessage("  fence: map %u zone %u at %.0f,%.0f,%.0f (%.0fy away)",
                mapId, zoneId, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(),
                target->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()));
        }
        else
            handler->SendSysMessage("  contract: none");

        return true;
    }

    static bool HandleNotorietyGo(ChatHandler* handler, Optional<std::string> name)
    {
        Player* target = Resolve(handler, name);
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!target || !me)
        {
            handler->SendSysMessage("No such player.");
            return true;
        }

        uint32 mapId = 0, zoneId = 0;
        Position spot;
        if (!Notoriety::GetRendezvous(target->GetGUID(), mapId, zoneId, spot))
        {
            handler->SendSysMessage("That player has no contract.");
            return true;
        }

        // The fence is only visible to its own contract holder, so this is the
        // only way to look at the ground it is standing on.
        me->TeleportTo(mapId, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), 0.0f);
        return true;
    }

    static bool HandleNotorietyReroll(ChatHandler* handler, Optional<std::string> name)
    {
        Player* target = Resolve(handler, name);
        if (!target)
        {
            handler->SendSysMessage("No such player.");
            return true;
        }

        handler->PSendSysMessage("Reroll %s.", Notoriety::RerollContract(target) ? "done" : "REFUSED");
        return true;
    }

    static bool HandleNotorietyBag(ChatHandler* handler, uint32 stacks)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        uint8 const level = me ? me->GetLevel() : 60;
        uint32 const levelXp = sObjectMgr->GetXPForLevel(level);

        handler->PSendSysMessage("Reward curve at level %u (bar %u):", level, levelXp);
        for (uint32 s = s_contractStacks; s <= stacks; s += s_stacksPerTier)
        {
            uint32 const tier = TierFor(s);
            uint32 const money = s_moneyBase + s_moneyPerTier * tier;
            float const bubbles = s_xpBubblesBase + s_xpBubblesPerTier * float(tier);
            handler->PSendSysMessage("  %2u stacks -> tier %u: %ug %us %uc, %.1f bubbles = %u xp",
                s, tier, money / 10000, (money % 10000) / 100, money % 100,
                bubbles, uint32(float(levelXp) * bubbles / 20.0f));
        }
        return true;
    }
};

void AddSC_custom_notoriety()
{
    LoadNotorietyConfig();
    new custom_notoriety_world();
    new custom_notoriety_player();
    new npc_notoriety_fence();
    new custom_notoriety_commands();
}
