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

#include "CharacterCache.h"
#include "Chat.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Duration.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "Miscellaneous/DepletedMarks.h"
#include "Miscellaneous/TournamentMode.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "Util.h"
#include "World.h"
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
    // 900202, not 900201: Grix Warbrand moved onto 900201 when the Custom Games
    // host took his old 900001, and that overwrote the fence that lived there.
    uint32 s_fenceEntry = 900202;
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
    uint32 s_contractAuraSpell = 0;
    bool s_botsSeeThroughStealth = true;

    // Copper to have the ledger amended. FLAT, not scaled by level: the point is
    // a way OUT of a spiral for somebody who has stopped enjoying being hunted,
    // and a price that climbs with level charges the most to exactly the players
    // who accumulate notoriety fastest. Three gold is the whole idea - it should
    // register as a decision and not as a bill.
    uint32 s_bribeCopper = 3 * GOLD;

    // The thresholds themselves, and NOT simply every fifth stack.
    //
    // A debuff is a promise that something changed. Twenty-five and thirty-five
    // arm nothing: the contract payout tier steps and the continuous
    // scalars keep sliding, but they were sliding on the stack before and will
    // on the stack after. An icon there says "something happened" when nothing
    // did, and once one rung is filler the player stops reading the rest.
    //
    // So these are exactly the stack counts where a threshold in the bounty
    // engine actually fires. Kept as config rather than derived because the
    // thresholds live in a different translation unit's statics, and a list that
    // silently disagrees with them is worse than one somebody has to update.
    // stack count -> the spell id that announces it.
    std::vector<std::pair<uint32, uint32>> s_checkpointStacks;

    // Last stack count each player's checkpoint auras were synced against.
    // SyncCheckpointAuras is called from the per-tick player update, so the
    // common case has to be one integer compare and nothing else.
    std::unordered_map<uint64, uint32> g_checkpointSynced;
    float s_minYards = 500.0f;
    float s_maxYards = 900.0f;
    float s_fenceAppearYards = 150.0f;
    // How often the map marker is pushed again while the holder is still walking.
    // Must stay under the client's own eight-minute fuse on a gossip POI - see
    // RefreshRendezvousPoi - with room for a tick that ran late.
    uint32 s_poiRefreshSeconds = 240;
    bool s_voidOnDeath = true;
    // Selling the page settles the debt. The payout is computed from the banked
    // peak before this runs, so clearing costs the seller nothing.
    bool s_clearStacksOnTurnIn = true;

    // Money is copper per tier. Below 60, experience is a share of the seller's
    // own next level; at 60 the same number of bubbles is converted to honor by
    // the realm's configured honor-per-level rate.
    uint32 s_moneyBase = 5000;
    uint32 s_moneyPerTier = 3000;
    float s_xpBubblesBase = 2.0f;
    float s_xpBubblesPerTier = 1.0f;
    // Applied to the level-60 honor value before it is converted to restored
    // Mark of Honor items. The default preserves the current 2x payout.
    float s_markRewardMultiplier = 2.0f;

    constexpr uint8 kHonorRewardLevel = 60;
    // Centurion runs at half the stock honor rate, so one Mark represents the
    // 100 honor value the quest would produce at the normal 1x rate.
    constexpr uint32 kHonorValuePerMark = 100;
    constexpr uint32 kDepletedMarksPerRecipient = 5;

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

    // Raid members carrying a copy of somebody else's page: bearer -> the holder
    // whose name is on it. See "The raid's copy" below.
    std::unordered_map<uint64, uint64> g_sharerOf;
    // A bearer who abandoned their copy of this holder's page is not handed it
    // again: bearer -> that holder. Forgotten when the contract ends.
    std::unordered_map<uint64, uint64> g_copyRefused;
    // The next second each player's copy is reconciled with their raid.
    std::unordered_map<uint64, time_t> g_nextShareSync;
    // When the map marker was last pushed to each client that shows it - the
    // holder's and every bearer's. Not persisted: a relog re-sends it anyway.
    // See RefreshRendezvousPoi.
    std::unordered_map<uint64, time_t> g_poiSentAt;

    // Work owed to a player that has to be done on their own map thread - see
    // QueueChore.
    struct PendingChore
    {
        bool Settle = false;     // clear their notoriety: their page was sold
        bool DropCopy = false;   // take the contract out of their log
        bool Failed = false;     // ...with the red "failed" line
        std::string Note;        // said to them when it is done
    };
    std::unordered_map<uint64, PendingChore> g_chores;

    // A ride home on offer - see "The ride home" below.
    struct PendingRide
    {
        uint32 MapId = 0;
        float X = 0.0f, Y = 0.0f, Z = 0.0f, O = 0.0f;
        uint32 ZoneId = 0;          // what the summons dialog names
        ObjectGuid Summoner;        // whose name it shows: the seller's
        time_t Until = 0;           // the offer lapses after this
        time_t NextAttempt = 0;     // earliest the dialog may go up
        time_t PromptExpires = 0;   // 0 = no dialog up; else when it lapses
    };
    std::unordered_map<uint64, PendingRide> g_pendingRide;

    // Chores or rides outstanding anywhere. Like g_anyContract, read before the
    // lock on the per-player tick.
    std::atomic<bool> g_anyChores{ false };

    void LoadNotorietyConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.Notoriety.Enable", false);
        s_questId = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.QuestId", 60001)));
        s_fenceEntry = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.FenceCreatureId", 900202)));
        s_contractStacks = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.ContractStacks", 15), 1, 255));
        s_stacksPerTier = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.StacksPerTier", 5), 1, 255));
        s_maxTiers = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.MaxTiers", 7), 0, 100));
        s_cooldownSeconds = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.CooldownSeconds", 0)));
        s_maxRerolls = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Notoriety.MaxRerolls", 2), 0, 20));
        s_checkpointBase = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.CheckpointAuraBase", 0)));
        // 0 in the dist for the same reason as CheckpointAuraBase: an aura with
        // no client Spell.dbc row is a blank, nameless icon. 90719 on B+.
        s_contractAuraSpell = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.ContractAuraSpell", 0)));
        // Read here too so the badge never claims a rule that is switched off.
        // Object.cpp reads the same key once at first use.
        s_botsSeeThroughStealth = sConfigMgr->GetBoolDefault("Centurion.Notoriety.BotsSeeThroughStealth", true);
        s_bribeCopper = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.BribeCopper", 3 * GOLD)));

        // Entries are "stacks:spellId", and the pairing is EXPLICIT for a reason
        // learned the moment a rung had to be inserted in the middle.
        //
        // This list used to be bare stack counts with the id derived from the
        // position - rung N used base+N. That is fine until a new threshold
        // appears between two existing ones: adding 20 to "5,10,15,25,..." slides
        // 25 onto the spell that says "Referred Upward", 30 onto 25's, and every
        // debuff below the insertion point starts lying about what armed. The
        // ids are already published in a client patch, so renumbering them means
        // reissuing the whole patch to fix text that was correct yesterday.
        //
        // A bare number is still accepted and still means base+index, so an old
        // config keeps working; it is only unsafe to INSERT into one.
        s_checkpointStacks.clear();
        {
            std::stringstream stream(sConfigMgr->GetStringDefault(
                "Centurion.Notoriety.CheckpointStacks",
                "5:90710,10:90711,15:90712,20:90717,30:90714,40:90715,45:90713,50:90716"));
            std::string token;
            uint32 index = 0;
            while (std::getline(stream, token, ','))
            {
                size_t const colon = token.find(':');
                uint32 const stacks = uint32(std::strtoul(token.c_str(), nullptr, 10));
                uint32 const spellId = colon == std::string::npos
                    ? s_checkpointBase + index
                    : uint32(std::strtoul(token.c_str() + colon + 1, nullptr, 10));

                if (stacks && spellId)
                    s_checkpointStacks.push_back({ stacks, spellId });

                ++index;
            }
        }

        // Sorted by stack count so the ladder is walked in order, and
        // de-duplicated on the stack count so two entries cannot fight over one
        // rung.
        std::sort(s_checkpointStacks.begin(), s_checkpointStacks.end(),
            [](auto const& l, auto const& r) { return l.first < r.first; });
        s_checkpointStacks.erase(std::unique(s_checkpointStacks.begin(), s_checkpointStacks.end(),
            [](auto const& l, auto const& r) { return l.first == r.first; }),
            s_checkpointStacks.end());
        s_fenceAppearYards = std::max(20.0f,
            sConfigMgr->GetFloatDefault("Centurion.Notoriety.FenceAppearYards", 150.0f));
        // Clamped well below 480: at the fuse itself the mark would blink out
        // between the expiry frame and the next tick.
        s_poiRefreshSeconds = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Notoriety.PoiRefreshSeconds", 240), 30, 400));
        s_minYards = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Notoriety.RendezvousMinYards", 500.0f));
        s_maxYards = std::max(s_minYards, sConfigMgr->GetFloatDefault("Centurion.Notoriety.RendezvousMaxYards", 900.0f));
        s_voidOnDeath = sConfigMgr->GetBoolDefault("Centurion.Notoriety.VoidOnDeath", true);
        s_clearStacksOnTurnIn = sConfigMgr->GetBoolDefault("Centurion.Notoriety.ClearStacksOnTurnIn", true);

        s_moneyBase = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.MoneyBaseCopper", 5000)));
        s_moneyPerTier = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Notoriety.MoneyPerTierCopper", 3000)));
        s_xpBubblesBase = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Notoriety.XpBubblesBase", 2.0f));
        s_xpBubblesPerTier = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Notoriety.XpBubblesPerTier", 1.0f));
        s_markRewardMultiplier = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Centurion.Notoriety.MarkRewardMultiplier", 2.0f));
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

    // The Mark of Honor items a sale pays, checked once per sale rather than
    // once per recipient. Every recipient gets their restored marks - the
    // level-60 share of the price - and five random configured depleted marks,
    // never a regular honor-points award. Full bags cannot eat the reward; the
    // Postmaster holds it instead.
    class MarkGrant
    {
    public:
        MarkGrant() : _restoredEntry(Trinity::Custom::GetRestoredMarkEntry())
        {
            _restoredValid = _restoredEntry && sObjectMgr->GetItemTemplate(_restoredEntry);

            for (uint32 const entry : Trinity::Custom::GetDepletedMarkEntries())
            {
                if (sObjectMgr->GetItemTemplate(entry))
                    _depletedEntries.push_back(entry);
                else
                    TC_LOG_ERROR("playerbots.hardcore",
                        "Notoriety: configured depleted Mark of Honor item {} does not exist; it will not be awarded.",
                        entry);
            }

            if (_depletedEntries.empty())
                TC_LOG_ERROR("playerbots.hardcore",
                    "Notoriety: no valid depleted Mark of Honor items are configured; the five-mark bonus cannot be awarded.");
        }

        // True when anything at all was handed over.
        bool Give(Player* recipient, uint32 restoredCount) const
        {
            if (!recipient || !recipient->IsInWorld())
                return false;

            if (restoredCount && !_restoredValid && !_loggedMissingRestored)
            {
                _loggedMissingRestored = true;
                TC_LOG_ERROR("playerbots.hardcore",
                    "Notoriety: configured Mark of Honor item {} does not exist; no restored marks were awarded.",
                    _restoredEntry);
            }

            bool received = false;
            auto grantItem = [&](uint32 entry, uint32 count)
            {
                if (!entry || !count)
                    return;

                ItemPosCountVec dest;
                if (recipient->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, count) == EQUIP_ERR_OK)
                {
                    if (Item* item = recipient->StoreNewItem(dest, entry, true))
                    {
                        recipient->SendNewItem(item, count, true, false);
                        received = true;
                        return;
                    }
                }

                recipient->SendItemRetrievalMail(entry, count);
                received = true;
            };

            if (restoredCount && _restoredValid)
                grantItem(_restoredEntry, restoredCount);

            if (!_depletedEntries.empty())
            {
                std::unordered_map<uint32, uint32> randomDepletedMarks;
                for (uint32 i = 0; i < kDepletedMarksPerRecipient; ++i)
                    ++randomDepletedMarks[_depletedEntries[urand(0, uint32(_depletedEntries.size() - 1))]];

                for (auto const& [entry, count] : randomDepletedMarks)
                    grantItem(entry, count);
            }

            return received;
        }

    private:
        uint32 _restoredEntry = 0;
        bool _restoredValid = false;
        std::vector<uint32> _depletedEntries;
        mutable bool _loggedMissingRestored = false;
    };

    // -----------------------------------------------------------------------
    // The raid's copy.
    //
    // The page is one person's - the HOLDER's. It is their notoriety that is
    // being sold, it burns when they die, and their banked peak is what it pays
    // out on. But the walk is the raid's, and the raid now carries the contract
    // with them: everybody in the holder's raid who is standing in the holder's
    // zone is handed a copy of the quest, so they see the mark on their own map
    // and the Quiet Man standing at the meeting, and any one of them can hand the
    // page over. One sale settles it for all of them - every other copy comes out
    // of its log the moment anybody sells, so a raid cannot sell one page once
    // per member.
    //
    // A copy is DERIVED state, reconciled on its bearer's own tick by
    // SyncSharedCopy: handed out to a member standing in the holder's zone, kept
    // for as long as they stay in the raid, taken back when they leave it or the
    // contract ends. Nothing about it is persisted - a restart forgets every
    // copy, the login check takes the orphaned quests back out, and the copies
    // are handed round again once the holder is back. Handed out by zone and
    // kept by raid on purpose: a raid walking to a meeting across a zone line
    // would otherwise watch the quest blink in and out of its logs every time the
    // holder stepped over it first.
    //
    // A copy is the contract as far as the fleet is concerned, too. The stealth
    // rule (NotorietyDefeatsStealth, Object.cpp) reads the QUEST, so a bearer is
    // as visible to the zone's bots as the holder - which is the point: any
    // bearer can sell, so otherwise the raid's rogue walks the page in unseen and
    // the walk never happens.
    // -----------------------------------------------------------------------

    // Whose page this player is on: their own, the holder they carry a copy for,
    // or 0. Caller holds g_lock.
    uint64 ContractOwnerLocked(uint64 raw)
    {
        if (g_contracts.count(raw))
            return raw;

        auto const itr = g_sharerOf.find(raw);
        if (itr != g_sharerOf.end() && g_contracts.count(itr->second))
            return itr->second;

        return 0;
    }

    // Unhook every copy of one holder's page and say whose they were. Caller
    // holds g_lock.
    std::vector<uint64> DetachCopiesLocked(uint64 holderRaw)
    {
        std::vector<uint64> bearers;
        for (auto itr = g_sharerOf.begin(); itr != g_sharerOf.end(); )
        {
            if (itr->second != holderRaw)
            {
                ++itr;
                continue;
            }

            bearers.push_back(itr->first);
            g_poiSentAt.erase(itr->first);
            itr = g_sharerOf.erase(itr);
        }

        for (auto itr = g_copyRefused.begin(); itr != g_copyRefused.end(); )
            itr = itr->second == holderRaw ? g_copyRefused.erase(itr) : std::next(itr);

        return bearers;
    }

    // Somebody a contract can be shared with, paid to or offered a ride home: a
    // person, inside the hardcore world. Grix writes no contracts for bots or
    // tournament characters, and the raid's share of one follows the same line.
    bool IsContractPerson(Player const* player)
    {
        return player && player->GetSession() && !BarracksHardcore::IsPlayerbot(player) &&
            !Tournament::IsTournamentCharacter(player);
    }

    std::string NameOf(ObjectGuid guid)
    {
        std::string name;
        if (!sCharacterCache->GetCharacterNameByGuid(guid, name) || name.empty())
            name = "the holder";
        return name;
    }

    // Caller holds g_lock.
    void RefreshChoreFlagLocked()
    {
        g_anyChores.store(!g_chores.empty() || !g_pendingRide.empty(), std::memory_order_relaxed);
    }

    // Work owed to somebody other than the player whose tick it happened on.
    //
    // A sale, a void or a raid change reaches people besides the one who caused
    // it, and those people may be standing on another map, which another thread
    // is updating. A player's quest log, auras and fences are only ever touched
    // from their OWN map thread, so the work is written down here and done by
    // RunChores on their next update - or after their next login, for somebody
    // who was away.
    void QueueChore(uint64 raw, bool settle, bool dropCopy, bool failed, std::string note)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        PendingChore& chore = g_chores[raw];
        chore.Settle = chore.Settle || settle;
        chore.DropCopy = chore.DropCopy || dropCopy;
        chore.Failed = chore.Failed || failed;
        if (!note.empty())
            chore.Note = std::move(note);
        RefreshChoreFlagLocked();
    }

    // Take the contract out of a log, whatever state it is in. Failed sends the
    // red "failed" line first. See VoidContract for why it is done this way and
    // not with FailQuest or a bare RemoveActiveQuest.
    void TakeQuestOutOfLog(Player* player, bool failed)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(s_questId);
        if (!player || !quest)
            return;

        // In the log by either measure - the status map or the replicated log
        // slot the client reads - since the point is to leave neither behind.
        QuestStatus const status = player->GetQuestStatus(s_questId);
        bool const inStatus = status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE ||
            status == QUEST_STATUS_FAILED;
        if (!inStatus && player->FindQuestSlot(s_questId) >= MAX_QUEST_LOG_SIZE)
            return;

        if (failed)
            player->SendQuestFailed(s_questId);

        if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED))
            player->RemoveTimedQuest(s_questId);

        player->TakeQuestSourceItem(s_questId, false);
        player->AbandonQuest(s_questId);

        uint16 const logSlot = player->FindQuestSlot(s_questId);
        if (logSlot < MAX_QUEST_LOG_SIZE)
            player->SetQuestSlot(logSlot, 0);

        // update defaults to true here, so SendQuestUpdate runs.
        player->RemoveActiveQuest(s_questId);
        player->RemoveRewardedQuest(s_questId);
    }

    // A copy comes out: the quest, its fence and its mark, and the bearer is
    // told why. On the bearer's own thread.
    void DropCopy(Player* player, std::string const& note, bool failed)
    {
        if (!player)
            return;

        TakeQuestOutOfLog(player, failed);
        Notoriety::DespawnFence(player);

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_poiSentAt.erase(player->GetGUID().GetRawValue());
        }

        if (!note.empty() && player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("%s", note.c_str());
    }

    // A bearer may only sell with the holder near: on the same map and in the
    // same zone. The page is the holder's - selling it settles THEIR notoriety -
    // and the walk is theirs to make; a raid that left its holder parked in a
    // city could otherwise send one member out to cash a page whose owner never
    // made the trip. True for the holder themselves, and for anybody with no
    // contract at all (the stock quest menu has nothing to show them).
    bool HolderIsNear(Player* player, ObjectGuid& holderGuid)
    {
        uint64 const raw = player->GetGUID().GetRawValue();
        uint64 owner = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            owner = ContractOwnerLocked(raw);
        }

        if (!owner || owner == raw)
            return true;

        holderGuid = ObjectGuid(owner);
        Player* holder = ObjectAccessor::FindPlayer(holderGuid);
        return holder && holder->IsInWorld() && holder->FindMap() == player->FindMap() &&
            holder->GetZoneId() == player->GetZoneId();
    }

    // Hand a raid member a copy of the page their raid is carrying, keep it
    // while they stay in the raid, and take it back when they leave it or the
    // contract ends. On the member's own tick, once a second.
    void SyncSharedCopy(Player* player)
    {
        if (!s_questId || !IsContractPerson(player) || !player->IsInWorld())
            return;

        uint64 const raw = player->GetGUID().GetRawValue();
        time_t const now = GameTime::GetGameTime();

        uint64 carryingFor = 0;
        bool carryingLive = false;
        {
            std::lock_guard<std::mutex> guard(g_lock);

            // A holder carries their own page, never a copy of somebody else's.
            if (g_contracts.count(raw))
                return;

            // Once a second is plenty: this is a raid roster and a quest log,
            // not a fight.
            time_t& next = g_nextShareSync[raw];
            if (now < next)
                return;
            next = now + 1;

            if (auto const itr = g_sharerOf.find(raw); itr != g_sharerOf.end())
            {
                carryingFor = itr->second;
                carryingLive = g_contracts.count(carryingFor) != 0;
            }
        }

        QuestStatus const status = player->GetQuestStatus(s_questId);
        bool const inLog = status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE;
        Group* group = player->GetGroup();

        if (carryingFor)
        {
            if (carryingLive && inLog && group && group->IsMember(ObjectGuid(carryingFor)))
                return;   // still on it

            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_sharerOf.erase(raw);
                g_poiSentAt.erase(raw);

                // Abandoned from the quest log: that is a no, and it is not
                // asked again for this page.
                if (!inLog && carryingLive)
                    g_copyRefused[raw] = carryingFor;
            }

            if (!inLog)
                Notoriety::DespawnFence(player);
            else
                DropCopy(player, carryingLive
                    ? "You are no longer in the raid carrying the Quiet Man's page. Your copy of the contract is gone."
                    : "The contract in your log is no longer standing.", false);
            return;
        }

        // In the log with nobody's name behind it. The page it was a copy of is
        // gone and nothing told this log - most likely a restart, which keeps
        // quest logs and forgets everything else.
        if (inLog)
        {
            DropCopy(player, "The contract in your log is no longer standing.", false);
            return;
        }

        if (!group)
            return;

        // Somebody in the raid holding a page...
        std::vector<ObjectGuid> holders;
        {
            std::vector<ObjectGuid> members;
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (slot.guid != player->GetGUID())
                    members.push_back(slot.guid);

            std::lock_guard<std::mutex> guard(g_lock);
            auto const refused = g_copyRefused.find(raw);
            for (ObjectGuid const& guid : members)
                if (g_contracts.count(guid.GetRawValue()) &&
                    (refused == g_copyRefused.end() || refused->second != guid.GetRawValue()))
                    holders.push_back(guid);
        }

        // ...and standing in this zone. Same map object, so the zone read is
        // this thread's own.
        for (ObjectGuid const& holderGuid : holders)
        {
            Player* holder = ObjectAccessor::FindPlayer(holderGuid);
            if (!holder || !holder->IsInWorld() || holder->FindMap() != player->FindMap() ||
                holder->GetZoneId() != player->GetZoneId())
                continue;

            // Not CanTakeQuest: the quest's conditions row (aura 90701 - "offered
            // only while the aura is up") is Grix's offer to a CARRIER, and most
            // of a raid carries no notoriety of its own. Status, level and a free
            // log slot are what a copy needs.
            Quest const* quest = sObjectMgr->GetQuestTemplate(s_questId);
            if (!quest || !player->SatisfyQuestStatus(quest, false) || !player->SatisfyQuestLevel(quest, false) ||
                !player->CanAddQuest(quest, false))
                return;

            player->AddQuestAndCheckCompletion(quest, nullptr);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_sharerOf[raw] = holderGuid.GetRawValue();
                g_poiSentAt.erase(raw);
            }

            std::string const holderName = holder->GetName();
            ChatHandler(player->GetSession()).PSendSysMessage("%s",
                Trinity::StringFormat("{} is carrying the Quiet Man's page, and your raid carries it with them. "
                    "Any of you can hand it over, and the sale pays everybody in the raid who is in the zone - "
                    "but the page is {}'s, and it burns if {} dies.", holderName, holderName, holderName).c_str());
            Notoriety::SendRendezvousPoi(player);
            return;
        }
    }

    // -----------------------------------------------------------------------
    // The ride home.
    //
    // Selling the page buys passage back to the registrar who wrote it - for the
    // whole raid, and as a SUMMONS rather than a shove. It used to teleport the
    // seller alone, two seconds after the sale, whether they wanted to go or
    // not: the rest of the raid was left six hundred yards into hostile ground,
    // and a seller who meant to stay was dragged off. Now everybody in the
    // seller's raid is shown the ordinary summons dialog and decides.
    //
    // Still a promise rather than a courtesy, so it is held open until it can be
    // made: the client cannot accept a summons dead or in combat, so for
    // somebody in either state the dialog waits until they are out of it, and
    // somebody who logs out is asked when they come back. Once the dialog is up
    // the answer is theirs - declined, or left to lapse, the offer is gone.
    // Accepting from a state the summons cannot be taken in puts the offer back
    // rather than losing it.
    //
    // The dialog is the stock one, sent by hand: Player::SendSummonRequestFrom
    // can only summon to where a summoner STANDS, and the registrar is the other
    // side of the zone. The answer comes back through OnCustomGameSummonResponse,
    // which runs ahead of the stock handler, matched on the person whose name
    // the dialog showed. And it is all done from the rider's OWN update rather
    // than from the sale: the raid may be spread over several maps, and only a
    // player's own map thread may touch them. That also keeps the delayed-
    // teleport trap off this path entirely - a TeleportTo from inside
    // Player::Update is parked and then thrown away for a ghost (Player.cpp:1796)
    // - because the teleport now runs from the answer's packet handler, as a
    // stock summons does.
    // -----------------------------------------------------------------------

    // Long enough to cover a corpse run and a relog, short enough that nobody is
    // offered a ride for a contract they have forgotten selling.
    constexpr uint32 kRideHomeWindowSeconds = 15 * 60;
    constexpr uint32 kRideHomeRetrySeconds = 3;
    // The first dialog waits a moment, so the reward panel and the fence's
    // parting whisper land before anything is put over them.
    constexpr uint32 kRideHomeFirstOfferSeconds = 2;

    // Put the dialog up if it is due and can be answered. On the rider's own
    // update.
    void OfferRideHome(Player* player)
    {
        uint64 const raw = player->GetGUID().GetRawValue();
        time_t const now = GameTime::GetGameTime();

        // Tournament characters are never summoned (TournamentMode.h).
        bool const never = Tournament::IsTournamentCharacter(player);

        // Not yet: dead or fighting (the client cannot accept a summons in
        // either), in a battleground or a custom-game lobby (not somewhere a
        // contract pulls anybody out of), mid-teleport, or already looking at
        // somebody else's summons.
        bool const notNow = !player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive() ||
            player->IsInCombat() || player->InBattleground() || player->InArena() ||
            player->IsInCustomGameLobby() || player->HasSummonPending();

        PendingRide ride;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = g_pendingRide.find(raw);
            if (itr == g_pendingRide.end())
                return;

            if (never || now > itr->second.Until)
            {
                g_pendingRide.erase(itr);
                RefreshChoreFlagLocked();
                return;
            }

            // A dialog is up. One nobody answered has lapsed, and that is an
            // answer too.
            if (itr->second.PromptExpires)
            {
                if (now > itr->second.PromptExpires)
                {
                    g_pendingRide.erase(itr);
                    RefreshChoreFlagLocked();
                }
                return;
            }

            if (now < itr->second.NextAttempt)
                return;

            if (notNow)
            {
                itr->second.NextAttempt = now + time_t(kRideHomeRetrySeconds);
                return;
            }

            itr->second.PromptExpires = now + time_t(MAX_PLAYER_SUMMON_DELAY);
            ride = itr->second;
        }

        // Laid out exactly as Player::SendSummonRequestFrom lays it.
        WorldPacket data(SMSG_SUMMON_REQUEST, 8 + 4 + 4);
        data << uint64(ride.Summoner.GetRawValue());              // whose name it shows
        data << uint32(ride.ZoneId);                               // where it says it goes
        data << uint32(MAX_PLAYER_SUMMON_DELAY * IN_MILLISECONDS); // auto-declined after
        player->SendDirectMessage(&data);

        ChatHandler(player->GetSession()).SendSysMessage(
            "The Quiet Man's people will see you back to Grix Warbrand. Accept the summons to go, or decline it to stay.");
    }

    // Everything owed to this player by somebody else's sale, void or raid
    // change - see QueueChore - and the ride home if one is on offer. On their
    // own update.
    void RunChores(Player* player)
    {
        uint64 const raw = player->GetGUID().GetRawValue();

        PendingChore chore;
        bool haveChore = false;
        bool haveRide = false;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (auto itr = g_chores.find(raw); itr != g_chores.end())
            {
                chore = std::move(itr->second);
                g_chores.erase(itr);
                haveChore = true;
                RefreshChoreFlagLocked();
            }

            haveRide = g_pendingRide.count(raw) != 0;
        }

        if (haveChore)
        {
            // Their page was sold by somebody else in the raid: the debt is
            // settled exactly as it is for a holder who sells their own.
            if (chore.Settle)
                Bounty::ClearBounty(player);

            if (chore.DropCopy)
                DropCopy(player, chore.Note, chore.Failed);
            else if (!chore.Note.empty())
                ChatHandler(player->GetSession()).PSendSysMessage("%s", chore.Note.c_str());
        }

        if (haveRide)
            OfferRideHome(player);
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

        // A bearer's meeting is the holder's: one page, one Quiet Man.
        std::lock_guard<std::mutex> guard(g_lock);
        auto itr = g_contracts.find(ContractOwnerLocked(guid.GetRawValue()));
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

        // Carrying a copy of somebody else's page fills the quest's one slot:
        // Grix could only put up a ! that the accept would then refuse.
        QuestStatus const status = player->GetQuestStatus(s_questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
            return false;

        uint64 const raw = player->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_contracts.count(raw))
            return false;

        auto itr = g_cooldownUntil.find(raw);
        return itr == g_cooldownUntil.end() || itr->second <= GameTime::GetGameTime();
    }

    // Push the mark to the client. Quiet skips the spoken bearing, for the
    // keep-alive that runs behind the player's back.
    //
    // The map guard is not paranoia: SMSG_GOSSIP_POI carries no map id, so the
    // client stamps the record with whatever map the player is standing on when
    // it arrives, and its landmark rebuild then filters on that stamp. Sent from
    // the wrong continent this plants the mark at those raw coordinates on the
    // CURRENT map and reports a nonsense distance; sent during a transfer it
    // stamps the wrong map and the pin silently never draws again.
    void SendRendezvousPoi(Player* player, bool quiet)
    {
        if (!player || !player->GetSession())
            return;

        uint32 mapId = 0;
        uint32 zoneId = 0;
        Position spot;
        if (!GetRendezvous(player->GetGUID(), mapId, zoneId, spot))
            return;

        if (player->GetMapId() != mapId)
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

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_poiSentAt[player->GetGUID().GetRawValue()] = GameTime::GetGameTime();
        }

        if (quiet)
            return;

        float const distance = player->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY());
        ChatHandler(player->GetSession()).PSendSysMessage(
            "The meeting is roughly %.0f paces off. Look for the mark on your map.", distance);
    }

    // Keep the mark alive on the way out.
    //
    // The client puts an EIGHT MINUTE fuse on a gossip POI and then forgets it:
    // the packet handler stamps time()+480 into a global (WoW.exe 0x007F48D9
    // add eax,0x1E0 -> 0x007F48DE), and the minimap paint path checks it every
    // rendered frame (0x007F5BD2) and zeroes the marker's coordinates when it
    // passes. Nothing re-arms it but another packet. A rendezvous is five to nine
    // hundred yards out and the walk there is a fight, so eight minutes is an
    // ordinary trip - and the mark dies mid-journey with the player still far
    // outside the range the fence appears at. That is the marker "disappearing as
    // I get close": not distance, elapsed time, which correlates with it.
    //
    // Re-sending is free. The client stores a gossip POI in ONE fixed slot - the
    // handler at 0x0058A870 passes a literal index (push 1 at 0x0058A8DE) to the
    // single store site 0x007F4870 - so a repeat REPLACES rather than stacking,
    // and GetNumMapLandmarks can only ever rise by one because of it.
    //
    // Not while the fence is already standing. The client wipes the mark by
    // itself within ten yards (0x007F64CA compares a squared 2D distance against
    // 100.0f), which is correct behaviour on arrival, and re-sending there would
    // resurrect a pin the player has walked to.
    void RefreshRendezvousPoi(Player* player)
    {
        if (!player)
            return;

        time_t const now = GameTime::GetGameTime();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            uint64 const raw = player->GetGUID().GetRawValue();
            if (!ContractOwnerLocked(raw))
                return;

            auto const sent = g_poiSentAt.find(raw);
            if (sent != g_poiSentAt.end() && now < sent->second + time_t(s_poiRefreshSeconds))
                return;
        }

        SendRendezvousPoi(player, /*quiet*/ true);
    }

    bool IssueContract(Player* player, WorldObject const* origin)
    {
        if (!s_enabled || !player)
            return false;

        // People only. Delivering a contract is a long walk to a fence summoned
        // for the holder, and a bot has no route to one - the errand scan looks
        // for NPCs inside 200 yards and the meeting is set between 400 and 900
        // away, so a bot that takes one simply stops. Keladan took the first one
        // ever written and stood in Kargath holding it.
        //
        // The bot side refuses the quest as well; this is the backstop, so a
        // contract can never exist without somebody who can carry it.
        if (BarracksHardcore::IsPlayerbot(player))
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

                // The raid's marks point at the old meeting. Forgetting when
                // each was sent makes every bearer's next tick push the new one.
                for (auto const& [bearer, holder] : g_sharerOf)
                    if (holder == raw)
                        g_poiSentAt.erase(bearer);

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
        // The holder only. A bearer dying, or paying Grix, voids nothing: the
        // page was never theirs, and the raid carries on with it.
        if (!player || !HasLiveContract(player->GetGUID()))
            return;

        uint64 const raw = player->GetGUID().GetRawValue();
        std::vector<uint64> bearers;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            bearers = DetachCopiesLocked(raw);
            g_poiSentAt.erase(raw);
        }

        EraseContract(raw);
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
        //
        // That sequence now lives in TakeQuestOutOfLog, shared with the raid's
        // copies.
        TakeQuestOutOfLog(player, /*failed*/ true);

        ChatHandler(player->GetSession()).PSendSysMessage("%s", reason);

        // Every copy burns with it, failed the same way, each on its bearer's
        // own update.
        if (!bearers.empty())
        {
            std::string const note = Trinity::StringFormat(
                "{}'s page is gone, and your copy of the contract with it.", player->GetName());
            for (uint64 const bearer : bearers)
                QueueChore(bearer, /*settle*/ false, /*dropCopy*/ true, /*failed*/ true, note);
        }

        TC_LOG_INFO("playerbots.hardcore", "Notoriety: {}'s contract was voided - {} ({} raid cop{} withdrawn)",
            player->GetName(), reason, bearers.size(), bearers.size() == 1 ? "y" : "ies");
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

        // Still walking: keep the mark from timing out under them.
        //
        // Right up to the doorstep, NOT merely to the fence's appear radius.
        // Gating this on s_fenceAppearYards left a hundred and fifty yard band
        // where nothing relit the fuse, and that band is the worst of the trip:
        // the last stretch into a rendezvous is hostile ground with a bounty on
        // the holder's head, and taking eight minutes to cross it fighting is
        // ordinary. The mark died exactly where the walk got hard, which reads
        // as "it disappears when I get close" - the same complaint the fuse fix
        // was supposed to answer, arrived at by a shorter route.
        //
        // No distance gate at all. The mark belongs to the CONTRACT: it goes away
        // when the contract does - sold, voided or failed - and at no other time.
        // Every threshold tried here was a way of quietly deciding the holder no
        // longer needed it, and each was wrong. First the fence's hundred and
        // fifty yard radius, which let the mark die in the last and hardest
        // stretch of the walk; then fifteen yards, the same mistake made smaller.
        //
        // The client does erase it by itself inside ten yards (a squared 2D
        // compare against 100.0f at WoW.exe 0x007F64CA) and nothing on this side
        // can stop that. What this can do is put it back. Standing on the
        // rendezvous that costs one redundant packet per refresh interval, which
        // is by a wide margin the cheaper of the two errors.
        RefreshRendezvousPoi(player);

        ObjectGuid existing;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = g_fenceByHolder.find(raw);
            if (itr != g_fenceByHolder.end())
                existing = itr->second;
        }

        Creature* standingFence = existing.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*player, existing);

        // Standing somewhere the meeting no longer is - the holder re-rolled it.
        // Without this a bearer who happened to be near the new spot kept talking
        // to a Quiet Man left behind at the old one.
        if (standingFence && standingFence->GetExactDist2d(spot.GetPositionX(), spot.GetPositionY()) > 5.0f)
        {
            DespawnFence(player);
            standingFence = nullptr;
        }

        bool const standing = standingFence != nullptr;

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

    // Owed the moment the page changes hands; offered as soon as it can be
    // answered. Only writes the record down - the dialog goes up from the
    // rider's own update (OfferRideHome), because the rider may be on another
    // map, which this thread must not touch.
    void ArmRideHome(ObjectGuid who, ObjectGuid summoner, uint32 mapId, float x, float y, float z, float o)
    {
        if (who.IsEmpty())
            return;

        uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, x, y, z);
        time_t const now = GameTime::GetGameTime();

        std::lock_guard<std::mutex> guard(g_lock);

        // A ride owed to somebody who logged out and never came back has
        // nothing left to expire it - the check that would have noticed only
        // runs while they are in the world. Sweep here instead; the map holds a
        // raid's worth of entries per sale in flight and is never big.
        for (auto itr = g_pendingRide.begin(); itr != g_pendingRide.end(); )
            itr = now > itr->second.Until ? g_pendingRide.erase(itr) : std::next(itr);

        PendingRide& ride = g_pendingRide[who.GetRawValue()];
        ride.MapId = mapId;
        ride.X = x;
        ride.Y = y;
        ride.Z = z;
        ride.O = o;
        ride.ZoneId = zoneId;
        ride.Summoner = summoner;
        ride.Until = now + time_t(kRideHomeWindowSeconds);
        ride.NextAttempt = now + time_t(kRideHomeFirstOfferSeconds);
        ride.PromptExpires = 0;
        RefreshChoreFlagLocked();
    }

    // A ride owed to somebody who logged out before answering. Called from the
    // login hook. A dialog that was up when they left died with their client,
    // so it is put up again rather than waited on. The record is in memory, so
    // a worldserver restart inside the window is the one thing that can still
    // lose it.
    void ResumeRideHome(Player* player)
    {
        if (!player)
            return;

        std::lock_guard<std::mutex> guard(g_lock);
        auto itr = g_pendingRide.find(player->GetGUID().GetRawValue());
        if (itr == g_pendingRide.end())
            return;

        itr->second.PromptExpires = 0;
        itr->second.NextAttempt = GameTime::GetGameTime() + time_t(kRideHomeRetrySeconds);
        RefreshChoreFlagLocked();
    }

    // The answer to the dialog OfferRideHome put up. False when it was not ours,
    // so the stock summons handler - or the custom-game lobby - gets it instead.
    bool AnswerRideHome(Player* player, ObjectGuid summoner, bool agree)
    {
        if (!player)
            return false;

        time_t const now = GameTime::GetGameTime();
        bool notNow = false;
        PendingRide ride;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = g_pendingRide.find(player->GetGUID().GetRawValue());
            if (itr == g_pendingRide.end() || !itr->second.PromptExpires || itr->second.Summoner != summoner)
                return false;

            // Declined, or answered after the dialog lapsed: the offer is gone.
            if (!agree || now > itr->second.PromptExpires)
            {
                g_pendingRide.erase(itr);
                RefreshChoreFlagLocked();
                return true;
            }

            // Accepted from a state a summons cannot be taken in - the same test
            // the stock handler makes. Put the offer back instead of losing it.
            if (!player->IsAlive() || player->IsInCombat())
            {
                itr->second.PromptExpires = 0;
                itr->second.NextAttempt = now + time_t(kRideHomeRetrySeconds);
                notNow = true;
            }
            else
            {
                ride = itr->second;
                g_pendingRide.erase(itr);
                RefreshChoreFlagLocked();
            }
        }

        if (notNow)
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "Not while you are dead or fighting. The offer stands, and will be made again.");
            return true;
        }

        // As Player::SummonIfPossible does it.
        player->FinishTaxiFlight();
        player->TeleportTo(ride.MapId, ride.X, ride.Y, ride.Z, ride.O);
        return true;
    }

    uint32 BribeCost(Player const* player)
    {
        if (!s_enabled || !s_bribeCopper || !player || !Bounty::GetStacks(player))
            return 0;

        return s_bribeCopper;
    }

    // Buy your way out of the ledger.
    //
    // This is a deliberate hole in the rule the flagger states one function
    // away - "a bounty cannot be walked off, the debt has to lapse on its own".
    // That rule exists to stop War Mode being an escape hatch: earn a price,
    // stroll back, disarm, and stay untouchable while hunters are still on the
    // way. Money is a different thing from a free toggle. It is a real cost paid
    // at the moment of use, it does not scale away, and somebody who has decided
    // they are done being hunted should have a door that is not "log out".
    //
    // The contract goes with it, and that is the honest reading rather than a
    // penalty: the page has your name on it, and you have just paid to have the
    // name struck out. Selling a contract for notoriety you no longer carry
    // would be selling a blank sheet.
    bool AcceptBribe(Player* player)
    {
        uint32 const cost = BribeCost(player);
        if (!cost || player->GetMoney() < cost)
            return false;

        player->ModifyMoney(-int64(cost));

        // Order matters: void first, because VoidContract refuses once there is
        // no live contract and ClearBounty is what the fence's own turn-in uses
        // to settle. Doing it the other way round leaves the contract standing.
        VoidContract(player, "You paid for the page. It is ash now.");
        Bounty::ClearBounty(player);

        TC_LOG_INFO("playerbots.hardcore",
            "Notoriety: {} (level {}) bribed the ledger clean for {}c.",
            player->GetName(), player->GetLevel(), cost);
        return true;
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

        for (auto const& [rung, spellId] : s_checkpointStacks)
        {

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

    // Carrying a contract is loud, and until now it was silent about it.
    //
    // While the page is in the log, every managed playerbot in the zone sees
    // through Stealth and Prowl - NotorietyDefeatsStealth in Object.cpp, the
    // answer to a rogue sneaking the whole walk to the fence. Nothing on the
    // player said the rule was in force, so a rogue found out by being opened on
    // mid-stealth. This aura is the notice. It is not the mechanism: the rule
    // still reads the quest itself, so the two can disagree for one tick at most.
    //
    // Same test as the rule, deliberately - INCOMPLETE or COMPLETE, and REWARDED
    // is off, because stealth comes back the moment the fence pays out.
    void SyncContractAura(Player* player)
    {
        if (!s_contractAuraSpell || !player)
            return;

        bool carrying = false;
        if (s_enabled && s_questId && s_botsSeeThroughStealth)
        {
            QuestStatus const status = player->GetQuestStatus(s_questId);
            carrying = status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE;
        }

        bool const wearing = player->HasAura(s_contractAuraSpell);
        if (carrying && !wearing)
            player->AddAura(s_contractAuraSpell, player);
        else if (!carrying && wearing)
            player->RemoveAurasDueToSpell(s_contractAuraSpell);
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
            if (!player)
                return false;

            // A copy is only good with its holder in these parts - see
            // HolderIsNear. Turned away here, before the objective ticks and
            // before any quest menu is built, so there is nothing to click.
            ObjectGuid holderGuid;
            if (!HolderIsNear(player, holderGuid))
            {
                me->Whisper(Trinity::StringFormat("That page has {}'s name on it. I deal when they are in these "
                    "parts, not before.", NameOf(holderGuid)), LANG_UNIVERSAL, player);
                return true;
            }

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
            player->KilledMonsterCredit(me->GetEntry(), me->GetGUID());

            return false;   // let the quest menu through
        }

        void OnQuestReward(Player* player, Quest const* quest, uint32 /*opt*/) override
        {
            if (!player || !quest || quest->GetQuestId() != Notoriety::QuestId())
                return;

            // The seller is the holder or a bearer; either way the page, its
            // price and its ride home are the holder's contract. Read it all out
            // under one lock, because everything after EraseContract has
            // forgotten it - and unhook the copies in the same breath, so no
            // second bearer can sell the page between this read and the erase.
            uint64 const sellerRaw = player->GetGUID().GetRawValue();
            uint64 holderRaw = 0;
            Contract contract;
            std::vector<uint64> bearers;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                holderRaw = ContractOwnerLocked(sellerRaw);
                if (!holderRaw)
                    return;

                contract = g_contracts[holderRaw];
                bearers = DetachCopiesLocked(holderRaw);
                g_poiSentAt.erase(sellerRaw);
                g_poiSentAt.erase(holderRaw);
            }

            bool const sellerHolds = holderRaw == sellerRaw;
            uint32 const stacks = std::max(contract.PeakStacks, contract.StacksAtAccept);
            uint32 const tier = TierFor(stacks);

            // Paid HERE and not from quest_template, and that is not taste.
            // Player.cpp: a repeatable quest pays its template XP exactly once
            // per character, ever, and silently zero on every hand-in after
            // that. The first turn-in would work and every one after it would
            // quietly pay nothing.
            uint32 const money = s_moneyBase + s_moneyPerTier * tier;
            float const bubbles = s_xpBubblesBase + s_xpBubblesPerTier * float(tier);
            uint32 const honorPerLevel = sWorld->getIntConfig(CONFIG_CENTURION_BG_XP_HONOR_PER_LEVEL);

            // Selling the page settles the debt - the HOLDER's, whoever hands it
            // over. A holder selling their own is settled here and now, before
            // paying, as it always was. One whose bearer sold it is settled on
            // their own update (they may be on another map thread), alongside
            // the rest of the raid's copies below. The payout cannot double-count
            // either way: it is priced off the banked peak through the tier, and
            // the notoriety bonus is held off it entirely (ScopedXpBonusSuppression
            // below) - the holder's stacks and anybody else's in the group.
            if (s_clearStacksOnTurnIn && sellerHolds)
                Bounty::ClearBounty(player);

            // The raid's share: everybody in the seller's raid standing in the
            // seller's zone is paid the whole price, each at their own level -
            // the coin, then a share of their own next level below 60, or Marks
            // of Honor at it. People only. The same map object as well as the
            // same zone, so every purse and log touched here belongs to this
            // thread.
            std::vector<Player*> recipients{ player };
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member || member == player || !IsContractPerson(member) || !member->IsInWorld() ||
                        member->FindMap() != player->FindMap() || member->GetZoneId() != player->GetZoneId())
                        continue;

                    recipients.push_back(member);
                }
            }

            MarkGrant const marks;
            uint32 sellerXp = 0;
            uint32 sellerMarks = 0;
            uint32 markedCount = 0;
            {
                Bounty::ScopedXpBonusSuppression const noBonus;

                for (Player* recipient : recipients)
                {
                    uint8 const level = recipient->GetLevel();
                    uint32 const xp = level < kHonorRewardLevel
                        ? uint32(float(sObjectMgr->GetXPForLevel(level)) * bubbles / 20.0f)
                        : 0;
                    uint32 const honor = level >= kHonorRewardLevel
                        ? uint32(float(honorPerLevel) * bubbles / 20.0f * s_markRewardMultiplier)
                        : 0;
                    uint32 const markCount = honor / kHonorValuePerMark;

                    if (money)
                        recipient->ModifyMoney(int32(money));
                    if (xp)
                        recipient->GiveXP(xp, nullptr);

                    bool const marked = marks.Give(recipient, markCount);
                    if (marked)
                        ++markedCount;

                    Notoriety::Payout payout;
                    payout.Who = recipient;
                    payout.Stacks = stacks;
                    payout.Tier = tier;
                    payout.ZoneId = recipient->GetZoneId();
                    payout.MoneyPaid = money;
                    payout.XpPaid = xp;
                    payout.MarksPaid = marked ? markCount : 0;
                    payout.Level = level;
                    Notoriety::GrantGoodieBag(payout);

                    if (recipient == player)
                    {
                        sellerXp = xp;
                        sellerMarks = markCount;
                    }
                    else
                        ChatHandler(recipient->GetSession()).PSendSysMessage("%s", Trinity::StringFormat(
                            "{} sold the page to the Quiet Man, and the sale pays everybody in the raid who is in the "
                            "zone. Your share has been paid.", player->GetName()).c_str());
                }
            }

            // One sale settles it for everybody: every other copy comes out of
            // its log, on its bearer's own update. The seller's own quest has
            // just been rewarded by the stock path; a holder whose bearer sold it
            // loses the page and the notoriety together.
            std::string const settled = Trinity::StringFormat(
                "{} handed the page to the Quiet Man. The contract is settled.", player->GetName());
            for (uint64 const bearer : bearers)
                if (bearer != sellerRaw)
                    QueueChore(bearer, /*settle*/ false, /*dropCopy*/ true, /*failed*/ false, settled);
            if (!sellerHolds)
                QueueChore(holderRaw, /*settle*/ s_clearStacksOnTurnIn, /*dropCopy*/ true, /*failed*/ false, settled);

            EraseContract(holderRaw);
            Notoriety::DespawnFence(player);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_cooldownUntil[holderRaw] = GameTime::GetGameTime() + time_t(s_cooldownSeconds);
            }

            me->Whisper("Nobody will hear this from me.", LANG_UNIVERSAL, player);

            // The ride home, offered to the whole raid - wherever they are - as
            // a summons each of them answers for themselves. The walk out is the
            // feature; walking back is the same ground again with nothing
            // hunting you, because selling the page took the notoriety with it.
            // See the ride-home note above OfferRideHome. Connected people only:
            // a bot has no dialog to answer, and somebody offline was not on
            // the walk.
            uint32 offered = 0;
            if (contract.HasOrigin)
            {
                std::vector<ObjectGuid> riders{ player->GetGUID() };
                if (Group* group = player->GetGroup())
                    for (Group::MemberSlot const& slot : group->GetMemberSlots())
                        if (slot.guid != player->GetGUID())
                            riders.push_back(slot.guid);

                for (ObjectGuid const& rider : riders)
                {
                    Player* member = ObjectAccessor::FindConnectedPlayer(rider);
                    if (!member || BarracksHardcore::IsPlayerbot(member))
                        continue;

                    Notoriety::ArmRideHome(rider, player->GetGUID(), contract.OriginMapId,
                        contract.OriginX, contract.OriginY, contract.OriginZ, contract.OriginO);
                    ++offered;
                }
            }

            TC_LOG_INFO("playerbots.hardcore",
                "Notoriety: {} (level {}) sold {} contract at {} peak stack(s), tier {}: {}c each to {} raid "
                "member(s) in zone {} ({} given marks; the seller {} xp, {} restored mark(s) plus {} random "
                "depleted), {} cop{} withdrawn, ride home offered to {} ({} reroll(s)).",
                player->GetName(), player->GetLevel(),
                sellerHolds ? std::string("their own") : NameOf(ObjectGuid(holderRaw)) + "'s",
                stacks, tier, money, recipients.size(), player->GetZoneId(), markedCount, sellerXp, sellerMarks,
                kDepletedMarksPerRecipient, bearers.size(), bearers.size() == 1 ? "y" : "ies", offered,
                contract.Rerolls);
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

        // Before the quest check, not after: somebody owed a ride home has
        // already handed the page over, so the quest is gone from their log.
        Notoriety::ResumeRideHome(player);

        QuestStatus const status = player->GetQuestStatus(s_questId);
        if (status == QUEST_STATUS_NONE)
            return;

        QueryResult result = CharacterDatabase.PQuery(
            "SELECT mapId, zoneId, posX, posY, posZ, stacksAtAccept, peakStacks, acceptedAt, rerolls, "
            "originMapId, originX, originY, originZ, originO "
            "FROM character_notoriety_contract WHERE guid = {}", player->GetGUID().GetCounter());
        if (!result)
        {
            // Not a holder. A copy of somebody's page comes back with its mark;
            // one with nobody's name behind it any more - the contract ended
            // while this player was away, or the realm restarted, which keeps
            // quest logs and forgets every copy - comes out of the log. Here and
            // not on the update tick, because after a restart nothing may be
            // running that tick.
            if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
                return;

            bool carrying = false;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                carrying = ContractOwnerLocked(player->GetGUID().GetRawValue()) != 0;
            }

            if (carrying)
                Notoriety::SendRendezvousPoi(player);
            else
                DropCopy(player, "The contract in your log is no longer standing.", false);
            return;
        }

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

        // Scoped, because SendRendezvousPoi below takes g_lock itself.
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_contracts[player->GetGUID().GetRawValue()] = contract;
            g_anyContract.store(true, std::memory_order_relaxed);
        }

        // The contract came back; the marker did not. Nothing re-sent it on
        // login, so every relog left a live contract, a quest still reading
        // "Find the Quiet Man", and a blank map - with no way back to it but
        // walking to a Grix who might be nine hundred yards behind you.
        //
        // Spoken rather than quiet: somebody who has just logged in has no idea
        // how far off the meeting is, and the bearing is half of what the mark
        // is for.
        Notoriety::SendRendezvousPoi(player);
    }

    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        // Two relaxed atomic loads for every player on a realm where nobody is
        // carrying a contract and nothing is owed, which is almost always.
        if (!s_enabled || !player)
            return;

        bool const contracts = g_anyContract.load(std::memory_order_relaxed);
        bool const chores = g_anyChores.load(std::memory_order_relaxed);
        if (!contracts && !chores)
            return;

        // Bots hold no contract, carry no copy, are owed nothing and are never
        // offered a ride - everything below is a lookup that finds nothing.
        if (BarracksHardcore::IsPlayerbot(player))
            return;

        if (chores)
            RunChores(player);

        if (contracts)
        {
            SyncSharedCopy(player);
            Notoriety::UpdateFence(player);
        }
    }

    void OnLogout(Player* player) override
    {
        // The summon is private to this player, so it has nobody left to be
        // visible to. It comes back when they walk in again.
        Notoriety::DespawnFence(player);

        // Their client forgets the map mark at logout; the login hook sends it
        // again. A copy itself stays put - it is theirs until they leave the
        // raid or the contract ends.
        if (player)
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_poiSentAt.erase(player->GetGUID().GetRawValue());
            g_nextShareSync.erase(player->GetGUID().GetRawValue());
        }
    }

    // The ride-home summons coming back. Runs ahead of the stock handler, which
    // still gets every answer that is not to one of these.
    bool OnCustomGameSummonResponse(Player* invitee, ObjectGuid summonerGuid, bool agree) override
    {
        if (!s_enabled)
            return false;

        return Notoriety::AnswerRideHome(invitee, summonerGuid, agree);
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
            uint64 const raw = target->GetGUID().GetRawValue();
            uint64 owner = 0;
            Contract c;
            {
                // find(), never operator[]: a bearer has no contract of their
                // own, and indexing would invent an empty one in their name.
                std::lock_guard<std::mutex> guard(g_lock);
                owner = ContractOwnerLocked(raw);
                if (auto const itr = g_contracts.find(owner); itr != g_contracts.end())
                    c = itr->second;
            }

            if (owner && owner != raw)
                handler->PSendSysMessage("  carrying a COPY of %s's contract", NameOf(ObjectGuid(owner)).c_str());
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
