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

#include "Guild.h"
#include "GuildMgr.h"
#include "Custom/custom_barracks_hardcore.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"

#include "Playerbot/Pvp/PlayerbotPvpClassActions.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "Playerbot/Pvp/PlayerbotSharedStateGuard.h"

#include "Custom/custom_loot_chest_helper.h"
#include "Custom/custom_bounty.h"

#include "Bag.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "Common.h"
#include <cctype>
#include <limits>
#include <atomic>

#include "Containers.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Item.h"
#include "Log.h"
#include "Loot.h"
#include "MapManager.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "AuctionHouseBot/AuctionHouseBot.h"
#include "AuctionHouseBot/AuctionHouseBotSeller.h"
#include "AuctionHouseMgr.h"
#include "Mail.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "Spell.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace playerbot
{
    // Defined at the bottom of this file. The zone-guardian tick lives in the
    // anonymous namespace below and has to drop a guardian that is sitting
    // above its post, so it needs the name before then. Declared HERE rather
    // than inside that anonymous namespace on purpose: a copy declared there
    // is a different function that nothing defines, which compiles cleanly and
    // then fails at link.
    void ResetManagedBotToZoneBand(Player* bot, uint32 zoneId, uint8 bottomLevel);

    // Same reasoning: the bounty dispatch below needs to know a zone's level
    // band before the definition appears.
    bool GetZoneLevelBand(uint32 zoneId, uint8& bottom, uint8& top);
}

namespace
{
    using PveClock = std::chrono::steady_clock;
    using PveTimePoint = PveClock::time_point;

    // The same free eat/drink pair the battleground preparation logic uses.
    constexpr uint32 SPELL_HUNTER_FEIGN_DEATH = 5384;
    constexpr uint32 kPveHunterAutoShotSpellId = 75;
    // Consecutive ticks a live victim may fail to resolve before the bot gives up
    // on it. A knockback or a stun can flicker one for a tick or two.
    constexpr uint32 PveTargetResolveGrace = 3;
    constexpr uint32 SPELL_PVE_OUT_OF_COMBAT_EAT = 29073;
    constexpr uint32 SPELL_PVE_OUT_OF_COMBAT_DRINK = 22734;

    // "Opening", the channel a player performs to open a locked chest.
    constexpr uint32 kOpeningSpellId = 3365;
    constexpr std::chrono::milliseconds PveFastTickInterval(250);
    constexpr std::chrono::milliseconds PveSlowTickInterval(750);
    constexpr std::chrono::seconds PveGrindScanInterval(2);
    constexpr uint32 PvePendingSummonTimeoutMs = 90 * 1000;
    constexpr float PveCompanionTeleportCatchupDistance = 150.0f;
    constexpr float PveRestBreakFollowDistance = 40.0f;
    // A player farther than this is not an immediate combat problem while a mob or
    // pet is already chewing on the bot. The bot clears the close threat first,
    // then can chase the player if the player is still attacking.
    constexpr float PveImmediatePlayerCombatRange = 40.0f;
    // How far to look for somebody who is fighting the bot without ever
    // swinging at it. Wider than the 40y combat range because a caster can
    // open from further out, and only ever walked when the attacker list is
    // empty and the bot is in combat anyway.
    constexpr float PveDefensiveEngagementScanYards = 60.0f;
    // Guardians patrol broadly, but actual player acquisition uses the configured
    // PlayerApproachYards value (200y by default) so teleport/approach and combat
    // discovery cannot disagree about whether a human is "nearby".
    constexpr float PveGuardianPatrolRadius = 120.0f;
    // Player-directed teleports are only coarse repositioning. Bots must never
    // materialize inside 210 yards of the player they are approaching; after
    // landing, normal combat movement walks the remaining distance.
    constexpr float PvePlayerTeleportMinimumDistance = 210.0f;

    playerbot::PveConfig g_PveConfig;

    enum class PveErrandKind : uint8
    {
        None = 0,
        Vendor,
        QuestGiver,
        QuestObject
    };

    enum class WalkPathResult : uint8
    {
        Reachable,
        Unreachable,
        Deferred
    };

    struct PveBotState
    {
        ObjectGuid masterGuid;
        ObjectGuid orderedTargetGuid;
        bool passive = false;
        bool stay = false;
        bool engaged = false;
        PveTimePoint nextFastTick{};
        PveTimePoint nextSlowTick{};
        PveTimePoint nextGrindScanAt{};
        PveTimePoint nextWanderAt{};
        PveTimePoint deathObservedAt{};
        bool deathObserved = false;
        // Set when the bot is killed while fighting a person. Until it passes the
        // bot will not go hunting, and its next relocation looks for somewhere with
        // nobody around.
        PveTimePoint timidUntil{};
        PveTimePoint nextRebirthCheckAt{};
        bool fleeingFromPlayers = false;
        // Stamped only while the bot is actually swinging at a person. Kept apart
        // from lastPlayerFightAt, which doubles as the hunt schedule and is also
        // written by the login seed and by the hunt trip - reading defeat
        // attribution off that field marks bots timid for dying to a boar.
        PveTimePoint lastHumanSwingAt{};
        ObjectGuid pendingLootGuid;
        PveTimePoint pendingLootUntil{};
        ObjectGuid errandGuid;
        PveErrandKind errandKind = PveErrandKind::None;
        PveTimePoint errandUntil{};
        PveTimePoint nextErrandScanAt{};
        PveTimePoint nextChestScanAt{};
        ObjectGuid chestOpeningGuid;
        PveTimePoint nextEquipCheckAt{};
        PveTimePoint nextTalentCheckAt{};
        PveTimePoint nextCombatDiagAt{};
        // Errand targets (NPCs and quest objects) visited recently, whether or
        // not the visit achieved anything - stops ping-ponging between blocked
        // targets (locked chest, turn-in with full bags, vendor that can't fix
        // the trigger).
        std::unordered_map<uint64, PveTimePoint> recentErrandTargets;
        // Targets that dropped out of engagement while still alive (evading,
        // turned invalid): re-acquiring them immediately produces the visible
        // engage/AttackStop flap that drags the mob back and forth.
        std::unordered_map<uint64, PveTimePoint> recentBadTargets;
        bool initialKitDone = false;
        uint32 dryWanderCount = 0;
        // General open-world no-progress watchdog. Unlike the short journey
        // detector, this follows every autonomous PvE activity and only resets
        // after meaningful horizontal displacement.
        PveTimePoint stuckAnchorAt{};
        uint16 stuckAnchorMapId = 0;
        uint32 stuckAnchorZoneId = 0;
        float stuckAnchorX = 0.0f;
        float stuckAnchorY = 0.0f;
        PveTimePoint restingUntil{};
        PveTimePoint nextSupplyRunAt{};
        PveTimePoint nextMailCheckAt{};
        PveTimePoint nextAuctionShopAt{};
        PveTimePoint nextAuctionSellAt{};
        // The first trip to the house after logging in is immediate and takes as
        // many items as it can, so a fleet that just came up trades itself into
        // shape at once instead of trickling one item per bot per ten minutes.
        bool auctionCatchUpBuy = true;
        bool auctionCatchUpSell = true;
        PveTimePoint nextProfessionCheckAt{};
        uint32 engagedStallTicks = 0;
        // When the current fight started - a bail-out only makes sense once a
        // fight has actually been joined.
        PveTimePoint engagedSince{};
        // Repeated failed chases against the SAME victim mean it cannot be
        // reached at all, not that the motion master hiccuped.
        uint32 consecutiveChaseRecoveries = 0;
        ObjectGuid lastRecoveryVictim;
        float lastEngagedX = 0.0f;
        float lastEngagedY = 0.0f;
        // How long a stealthed bot in melee range may wait for its rotation to
        // open from stealth before auto-attack starts anyway (a B+ fresh rogue
        // knows ONLY Stealth - its opener never comes).
        PveTimePoint stealthOpenerDeadline{};
        // Death-loop breaker: repeated deaths in a short window mean the spot
        // itself is lethal (graveyard camped by higher-level mobs).
        uint8 recentDeathCount = 0;
        PveTimePoint recentDeathWindowStart{};
        // Zone guardians: when this bot was first seen away from its post.
        PveTimePoint guardianOutOfZoneSince{};
        PveTimePoint nextGuardianApproachAt{};
        // When this guardian last fought an actual person. Drives the escalation:
        // a guardian nobody has fought in a while stops being polite about where it
        // lands.
        PveTimePoint lastPlayerFightAt{};
        // Hardcore reclaim: when we fell, so the walk back is abandoned once the
        // drop chest would have despawned.
        PveTimePoint deathSpotAt{};
        // Naked recovery: when this bot was first seen stripped of its gear.
        PveTimePoint nakedSince{};
        PveTimePoint nextNakedCheckAt{};
        // Re-ask throttle for the rogue's Thistle Tea; the item's own cooldown
        // is the real limit.
        PveTimePoint nextThistleTeaAt{};
        // Hardcore reclaim: where we last fell - the drop chest stands there.
        uint16 deathSpotMapId = 0;
        float deathSpotX = 0.0f;
        float deathSpotY = 0.0f;
        float deathSpotZ = 0.0f;
        // Taming: guards the 20s tame/capture channel against every other
        // activity, and paces the tameable-beast scan.
        PveTimePoint tamingUntil{};
        PveTimePoint nextTameScanAt{};
        // Taming drive: when the current walk-to-a-beast started, and how long
        // to leave taming alone after one that could not be reached.
        PveTimePoint tameDriveSince{};
        PveTimePoint tameBackoffUntil{};
        // Hard deadline, never a condition: the bot lies still until this passes
        // and then carries on regardless of what else is true.
        PveTimePoint feignHoldUntil{};
        // Pets are trained, not born, on this realm: Growl has to be granted.
        PveTimePoint nextPetGrowlCheckAt{};
        // Paces the "am I in the right zone for my level" check.
        PveTimePoint nextZoneFitCheckAt{};
        // Consecutive ticks the held victim has failed to resolve.
        uint32 targetResolveMisses = 0;
        // One-shot repair of the class starting spellbook.
        bool startingSpellsEnsured = false;
        // Weapon skills are topped up on a slow cadence so a weapon bought or
        // looted mid-level does not swing at skill 1 until the next ding.
        PveTimePoint nextWeaponSkillCheckAt{};
        // Class-quest travel target (giver or ender) for the world executor.
        PveTimePoint nextClassQuestScanAt{};
        uint32 classQuestId = 0;
        uint16 classQuestMapId = 0;
        float classQuestX = 0.0f;
        float classQuestY = 0.0f;
        float classQuestZ = 0.0f;
        // Walked journey (relocation/town run on foot). Fallback kind decides
        // what happens on timeout or stuck: the old teleport.
        // walkFallbackUntil: after a failed walk, executors skip the walk branch
        // so the retry actually reaches taxi/teleport instead of re-walking into
        // the same wall forever.
        PveTimePoint walkFallbackUntil{};

        // Out on a bounty, and not this zone's business until it comes back.
        //
        // The zone-fit sweep runs on the MAP thread and g_DeployedHunters is
        // world-thread-only, so the sweep cannot ask the ledger directly. This
        // flag is the ledger's answer, mirrored somewhere the sweep can read it
        // under the state lock it already holds. Set when a hunter is dispatched,
        // cleared when it is brought home, so exactly one owner decides when a
        // hunter leaves the zone it was sent to - rather than the sweep and the
        // return pass each acting on a different idea of whether the hunt is over.
        bool bountyDeployed = false;
        bool journeyActive = false;
        uint8 journeyFallbackKind = 0; // 1 = grind relocation, 2 = supply run
        uint16 journeyMapId = 0;
        float journeyX = 0.0f;
        float journeyY = 0.0f;
        float journeyZ = 0.0f;
        PveTimePoint journeyUntil{};
        PveTimePoint nextJourneyStepAt{};
        PveTimePoint journeyProgressAt{};
        float journeyProgressX = 0.0f;
        float journeyProgressY = 0.0f;
        // Where the CURRENT hop was aimed, so the next one can be issued as the
        // bot runs into it rather than after it has stopped dead there.
        bool journeyStepValid = false;
        float journeyStepX = 0.0f;
        float journeyStepY = 0.0f;
        // When this bot was first seen flagged as in flight while no flight
        // generator was actually running. Zero whenever the two agree.
        PveTimePoint strandedFlightSince{};
        // The other half of the same problem: a flight generator that IS running
        // but has stopped going anywhere. Anchor position and the moment the bot
        // was last seen away from it.
        PveTimePoint flightStillSince{};
        float flightAnchorX = 0.0f;
        float flightAnchorY = 0.0f;
        float flightAnchorZ = 0.0f;
    };

    bool IsRecentErrandTarget(PveBotState& state, ObjectGuid const& guid)
    {
        auto itr = state.recentErrandTargets.find(guid.GetRawValue());
        if (itr == state.recentErrandTargets.end())
            return false;

        if (PveClock::now() >= itr->second)
        {
            state.recentErrandTargets.erase(itr);
            return false;
        }

        return true;
    }

    void MarkRecentErrandTarget(PveBotState& state, ObjectGuid const& guid)
    {
        PveTimePoint const now = PveClock::now();
        if (state.recentErrandTargets.size() > 24)
        {
            for (auto itr = state.recentErrandTargets.begin(); itr != state.recentErrandTargets.end();)
                itr = now >= itr->second ? state.recentErrandTargets.erase(itr) : std::next(itr);
        }

        state.recentErrandTargets[guid.GetRawValue()] = now + std::chrono::minutes(2);
    }

    bool IsRecentBadTarget(PveBotState& state, ObjectGuid const& guid)
    {
        auto itr = state.recentBadTargets.find(guid.GetRawValue());
        if (itr == state.recentBadTargets.end())
            return false;

        if (PveClock::now() >= itr->second)
        {
            state.recentBadTargets.erase(itr);
            return false;
        }

        return true;
    }

    void MarkRecentBadTarget(PveBotState& state, ObjectGuid const& guid)
    {
        PveTimePoint const now = PveClock::now();
        if (state.recentBadTargets.size() > 24)
        {
            for (auto itr = state.recentBadTargets.begin(); itr != state.recentBadTargets.end();)
                itr = now >= itr->second ? state.recentBadTargets.erase(itr) : std::next(itr);
        }

        state.recentBadTargets[guid.GetRawValue()] = now + std::chrono::seconds(60);
    }

    std::unordered_map<uint64, PveBotState> g_PveBotStateByGuid;

    struct PendingSummon
    {
        ObjectGuid summonerGuid;
        uint32 deadlineMs = 0;
        bool joinGroup = false;
    };

    // Written from map-update threads and command handlers, drained on the world
    // thread so every group mutation and cross-map teleport happens there.
    std::mutex g_PvePendingLock;
    std::unordered_map<uint64, PendingSummon> g_PendingSummonsByBotGuid;
    std::unordered_set<uint64> g_PendingGroupInviteAccepts;
    std::unordered_set<uint64> g_PendingGrindRelocations;
    // Stuck recovery is deliberately separate from ordinary level/home-zone
    // relocation: its value is the zone the bot was stuck in, which must be
    // preserved through the map-thread -> world-thread handoff.
    std::unordered_map<uint64, uint32> g_PendingStuckRelocations;
    // bot guid -> the player it should be dropped next to. Guardians exist to meet
    // people, so one with nobody in reach is teleported to somebody rather than
    // left standing in an empty zone.
    struct GuardianTeleportRequest
    {
        uint64 HumanRawGuid = 0;
        float DropDistance = 210.0f;
    };
    std::unordered_map<uint64, GuardianTeleportRequest> g_PendingGuardianTeleports;
    // Bots that need a vendor with none in walking range: the world thread
    // teleports them to the nearest vendor cluster (a town run), shopping and
    // repairs happen through the normal errand, and the dry-scan relocation
    // afterwards sends them back to a grind spot.
    std::unordered_set<uint64> g_PendingSupplyRuns;
    // Travel toward a class-quest giver or ender (state carries the target).
    std::unordered_set<uint64> g_PendingClassQuestTravels;
    // Rebirth-flagged bots that just hit the level cap: full level-1 reset on
    // the world thread.
    std::unordered_set<uint64> g_PendingRebirths;
    // Guardians sitting above their post's ceiling, and the level to put them
    // at. A map rather than a set because the target is the post's ceiling,
    // which the world thread cannot recompute without redoing the slot lookup.
    std::unordered_map<uint64, uint8> g_PendingGuardianDemotions;
    // Guardians that ran out of food or water and have no vendor they are
    // allowed to reach. Everyone else walks to a merchant and pays.
    std::unordered_set<uint64> g_PendingGuardianRations;
    // Mail collection and auction shopping mutate world-thread-only structures
    // (Player::m_mail, AuctionHouseObject) - executed from OnWorldUpdate.
    std::unordered_set<uint64> g_PendingMailCollections;
    // Guild joins: GuildMgr and Guild both mutate shared state the core only ever
    // touches from world-thread opcode handlers, so the bot tick may only queue.
    std::unordered_set<uint64> g_PendingGuildJoins;
    std::unordered_set<uint64> g_PendingAuctionShopping;
    std::unordered_set<uint64> g_PendingAuctionSales;
    // Loot EXECUTION must happen on the world thread: Player::SendLoot for a
    // group-tagged kill (or a group-rules chest) mutates shared Group state
    // (roll lists, looter guid) that the core only ever touches from
    // PROCESS_THREADUNSAFE loot opcodes. The map-thread fast tick only walks the
    // bot to the corpse and enqueues here.
    std::unordered_map<uint64, ObjectGuid> g_PendingLootExecutions;

    GameObject* FindNearestQuestGameObject(Player* bot, PveBotState& state, float radius);
    bool UseQuestGameObject(Player* bot, PveBotState& state, GameObject* go);
    bool BotHasIncompleteQuest(Player* bot);
    template<typename Fn>
    void ForEachBagItem(Player* bot, Fn&& fn);
    bool IsEquipUpgrade(Player const* bot, ItemTemplate const* candidate, ItemTemplate const* incumbent, uint8 slot);
    float ScoreItemForSpec(Player const* bot, ItemTemplate const* proto);
    GameObject* FindRegisteredDeathChest(Player* bot, uint32 entry, float maxDistance);
    bool IsAuctionableSurplus(Player* bot, Item* item);
    uint32 RequiredAmmoSubclass(Player const* bot);
    void MoveTowardThrottled(Player* bot, Position const& destination);
    WalkPathResult CheckWalkPath(Player* bot, Position const& destination);
    void ProcessPendingLootExecutions();
    void QueueUnwatchedChests();
    void GrantGatherSkillCredit(Player* bot, GameObject* go);
    void MaybeQueueOverBandRebirth(Player* bot, PveBotState& state);
    void ClearResurrectionSickness(Player* bot);
    void ResetStuckWatchdog(PveBotState& state);
    bool IsStuckWatchdogEligible(Player* bot, PveBotState const& state,
        playerbot::PveConfig const& cfg, PveTimePoint now);
    bool BotCanTeleportNow(Player* bot);
    void RestorePlayerbotTeleportVitals(Player* bot);
    void TrySkinCorpse(Player* bot, Creature* corpse);
    bool IsGatherableNodeFor(Player* bot, GameObject const* go, int32* outRequiredSkill);
    uint32 GetGuardianZoneId(uint64 botRawGuid);
    bool IsProactiveTargetWithinPower(Player const* bot, uint32 playerLevel);
    bool IsProactivePlayerLevelAcceptable(Player const* bot, uint32 playerLevel, uint32 bountyStacks);
    bool IsProactivePlayerLevelAcceptable(Player const* bot, Player const* player);

    bool IsHumanPlayer(Player const* player)
    {
        if (!player)
            return false;

        WorldSession const* session = player->GetSession();
        if (!session || session->IsVirtualSession() || session->IsTransientPlayerSession())
            return false;

        return !playerbot::IsManagedRandomBot(player);
    }

    bool HasRestAura(Player const* player)
    {
        return player->HasAura(SPELL_PVE_OUT_OF_COMBAT_EAT) || player->HasAura(SPELL_PVE_OUT_OF_COMBAT_DRINK);
    }

    // Below the rest thresholds a bot must not START anything new: without this
    // gate the 250ms acquisition tick re-engages before the 750ms rest check can
    // ever run, and a wounded bot beelines from corpse to corpse until it dies.
    bool NeedsRecovery(Player const* player, playerbot::PveConfig const& cfg)
    {
        return player->GetHealthPct() < cfg.restHealthPct ||
            (player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < cfg.restManaPct);
    }

    // Fit to START a fight with a person. NeedsRecovery governs sitting down;
    // this governs picking a fight, and sits deliberately higher - a bot at 65%
    // health is finished resting by the thresholds above but has no business
    // opening on a player. Only INITIATION is gated: a bot that is already being
    // attacked fights back at any health, which is what the defensive picker is
    // for.
    bool ReadyToFightPlayers(Player const* player, playerbot::PveConfig const& cfg)
    {
        if (player->GetHealthPct() < cfg.playerEngageMinHealthPct)
            return false;
        return !(player->GetMaxPower(POWER_MANA) > 0 &&
            player->GetPowerPct(POWER_MANA) < cfg.playerEngageMinManaPct);
    }

    bool IsRestingNow(Player const* player, PveBotState const& state)
    {
        if (HasRestAura(player))
            return true;

        // Consumable-based rest has no fixed aura ids to sniff; the state timer
        // set when the item was used stands in for it.
        return PveClock::now() < state.restingUntil;
    }

    uint32 HighestKnownRankInChain(Player* bot, uint32 firstRankSpellId)
    {
        uint32 best = 0;
        for (uint32 spellId = firstRankSpellId; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
            if (bot->HasSpell(spellId))
                best = spellId;
        return best;
    }

    // The class's baseline nuke, at the highest rank the bot knows - what a
    // real low-level player leads with before any spec exists.
    uint32 BaselineNukeSpellId(Player* bot)
    {
        uint32 firstRank = 0;
        switch (bot->GetClass())
        {
        case CLASS_PRIEST:  firstRank = 585;  break; // Smite
        case CLASS_MAGE:    firstRank = 133;  break; // Fireball
        case CLASS_WARLOCK: firstRank = 686;  break; // Shadow Bolt
        case CLASS_SHAMAN:  firstRank = 403;  break; // Lightning Bolt
        case CLASS_DRUID:   firstRank = 5176; break; // Wrath
        default:
            return 0;
        }
        return HighestKnownRankInChain(bot, firstRank);
    }

    // This realm's playercreateinfo_spell_custom table is EMPTY, so
    // Player::LearnCustomSpells teaches nothing at all and a bot reborn at level
    // 1 comes back with no attack spell whatsoever - 107 of 116 low level caster
    // bots knew no nuke of any rank. A level 2 warlock with no Shadow Bolt just
    // walks up and swings its staff, which is exactly what was seen in game. The
    // trainer catch-up cannot cover it either: trainers only teach rank 2 and up,
    // never the rank a character is created with. So grant the opener directly.
    //
    // Ids verified against this realm's own spell_ranks: each is the rank-1 head
    // of a real chain (Heroic Strike 13 ranks, Fireball 16, Shadow Bolt 13...).
    void EnsureBaselineAttackSpell(Player* bot)
    {
        uint32 opener = 0;
        switch (bot->GetClass())
        {
        case CLASS_WARRIOR: opener = 78;    break; // Heroic Strike
        case CLASS_PALADIN: opener = 21084; break; // Seal of Righteousness
        case CLASS_HUNTER:  opener = 2973;  break; // Raptor Strike
        case CLASS_ROGUE:   opener = 1752;  break; // Sinister Strike
        case CLASS_PRIEST:  opener = 585;   break; // Smite
        case CLASS_SHAMAN:  opener = 403;   break; // Lightning Bolt
        case CLASS_MAGE:    opener = 133;   break; // Fireball
        case CLASS_WARLOCK: opener = 686;   break; // Shadow Bolt
        case CLASS_DRUID:   opener = 5176;  break; // Wrath
        default:
            return;
        }

        // Any rank satisfies this: only a bot that knows NONE of them is broken.
        if (HighestKnownRankInChain(bot, opener))
            return;

        bot->LearnSpell(opener, false);
        TC_LOG_INFO("playerbots.pve", "Bot {} knew no class opener; taught spell {}.", bot->GetName(), opener);
    }

    // Auto Shot is not the rank-1 of any chain, so the opener logic above cannot
    // reach it: it is a single spell every hunter is CREATED with, which on this
    // realm means the empty playercreateinfo_spell_custom table loses it forever
    // the first time a hunter is reborn at level 1.
    //
    // Without it a hunter has no ranged attack at all. It carries a bow, buys
    // arrows for it, and still walks into melee - because the ranged positioning
    // asks "does this bot have Auto Shot" before anything else and gets no for an
    // answer. Every hunter on the realm was in that state.
    void EnsureHunterAutoShot(Player* bot)
    {
        if (bot->GetClass() != CLASS_HUNTER || bot->HasSpell(kPveHunterAutoShotSpellId))
            return;

        bot->LearnSpell(kPveHunterAutoShotSpellId, false);
        TC_LOG_INFO("playerbots.pve", "Bot {} knew no Auto Shot; taught it.", bot->GetName());
    }

    // Mages provision themselves: highest known Conjure Water / Conjure Food
    // rank, or 0 when the bot can't conjure that kind.
    uint32 ConjureSpellId(Player* bot, bool drink)
    {
        return HighestKnownRankInChain(bot, drink ? 5504u : 587u);
    }

    // The spell category is the discriminating field, NOT the subclass: B+'s
    // classic-imported item rows keep the old subclass 0 on basic food/water
    // (Tough Jerky, Refreshing Spring Water), and a subclass==FOOD requirement
    // makes every vendor staple invisible to both the buyer and the eater.
    bool IsFoodTemplate(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
            proto->Spells[0].SpellCategory == SPELL_CATEGORY_FOOD;
    }

    bool IsDrinkTemplate(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONSUMABLE &&
            proto->Spells[0].SpellCategory == SPELL_CATEGORY_DRINK;
    }

    // Whether this class drinks at all. NOT GetMaxPower(POWER_MANA): a druid in
    // bear or cat form reports the form's power, so a shapeshifted druid looked
    // like a warrior to every water check and went thirsty for good.
    bool UsesMana(Player const* bot)
    {
        switch (bot->GetClass())
        {
        case CLASS_PALADIN:
        case CLASS_HUNTER:
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_DRUID:
            return true;
        default:
            return false;
        }
    }

    void RemoveRestAuras(Player* player)
    {
        player->RemoveAurasDueToSpell(SPELL_PVE_OUT_OF_COMBAT_EAT);
        player->RemoveAurasDueToSpell(SPELL_PVE_OUT_OF_COMBAT_DRINK);
    }

    Unit* ResolveAttackableByGuid(Player* bot, ObjectGuid const& guid)
    {
        if (guid.IsEmpty() || guid == bot->GetGUID())
            return nullptr;

        Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
        if (!unit || !unit->IsAlive() || !bot->IsValidAttackTarget(unit))
            return nullptr;

        // An evading creature resets to full health while immune; treating it as
        // a live target would chase-loop it forever.
        if (Creature const* creature = unit->ToCreature())
            if (creature->IsInEvadeMode())
                return nullptr;

        return unit;
    }

    void DisengagePveCombat(Player* bot, PveBotState& state)
    {
        playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
        state.engaged = false;
        if (bot->GetVictim())
            bot->AttackStop();
        // The white-swing floor's chase can outlive the fight as a preserved
        // follow order: without a stop the bot trails its ex-target forever,
        // targetless and swinging at nothing.
        if (MotionMaster* motionMaster = bot->GetMotionMaster())
            if (motionMaster->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE ||
                motionMaster->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
            {
                motionMaster->Clear();
                bot->StopMoving();
            }
        // Player::SetTarget is an EMPTY override ("does not apply to players") -
        // player selection only changes through SetSelection.
        bot->SetSelection(ObjectGuid::Empty);
    }

    void QueuePendingSummon(ObjectGuid const& botGuid, ObjectGuid const& summonerGuid, bool joinGroup)
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        PendingSummon& pending = g_PendingSummonsByBotGuid[botGuid.GetRawValue()];
        pending.summonerGuid = summonerGuid;
        pending.deadlineMs = GameTime::GetGameTimeMS() + PvePendingSummonTimeoutMs;
        pending.joinGroup = joinGroup;
    }

    bool HasPendingSummon(ObjectGuid const& botGuid)
    {
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        return g_PendingSummonsByBotGuid.find(botGuid.GetRawValue()) != g_PendingSummonsByBotGuid.end();
    }

    // Training and target dummies are attackable and level-appropriate but
    // effectively immortal: a bot that picks one attacks it until the end of
    // time. The many variants (classic engineering dummies, test dummies, the
    // wotlk trainer dummies) share no single template flag, but they all carry
    // the name, and the scripted ones additionally sit perma-stunned.
    bool IsTargetDummyCreature(Creature const* creature)
    {
        if (creature->HasUnitFlag(UNIT_FLAG_STUNNED))
            return true;

        CreatureTemplate const* proto = creature->GetCreatureTemplate();
        return proto && proto->Name.find("Dummy") != std::string::npos;
    }

    // Zones no bot should ever grind or shop in even when their spawns form
    // convincing clusters on an allowed map: this DB spawns the death knight
    // intro copy on map 0 (Plaguelands: The Scarlet Enclave), and GM Island
    // sits on map 1.
    bool IsForbiddenGrindZone(uint32 zoneId)
    {
        return zoneId == 4298 || zoneId == 876;
    }

    struct GrindTargetCheck
    {
        Player* bot;
        playerbot::PveConfig const& cfg;

        bool operator()(Creature* creature) const
        {
            if (!creature->IsAlive() || creature->IsCivilian() || creature->IsTrigger() || creature->IsInEvadeMode())
                return false;

            // Rabbits and other critters are technically attackable but grinding
            // them is neither XP nor a convincing simulation.
            if (creature->GetCreatureType() == CREATURE_TYPE_CRITTER)
                return false;

            if (IsTargetDummyCreature(creature))
                return false;

            if (creature->IsPet() || creature->IsTotem() || creature->IsControlledByPlayer())
                return false;

            if (!cfg.grindAllowElites && (creature->isElite() || creature->isWorldBoss()))
                return false;

            int32 const levelDelta = int32(creature->GetLevel()) - int32(bot->GetLevel());
            if (levelDelta > int32(cfg.grindMaxLevelAbove) || -levelDelta > int32(cfg.grindMaxLevelBelow))
                return false;

            // Never steal a mob someone else is already fighting.
            if (creature->IsInCombat() && creature->GetVictim() != bot)
                return false;

            return bot->IsValidAttackTarget(creature);
        }
    };

    // A wanted-entry creature whose quest carries a source item (taming rods,
    // capture devices) must be USED on, not killed - the kill grants nothing
    // and the grind loop would farm it forever.
    Item* FindQuestSourceItemFor(Player* bot, uint32 creatureEntry)
    {
        for (auto const& [questId, questStatus] : bot->getQuestStatusMap())
        {
            if (questStatus.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !quest->GetSrcItemId())
                continue;

            bool wanted = false;
            for (uint8 objectiveIdx = 0; objectiveIdx < QUEST_OBJECTIVES_COUNT; ++objectiveIdx)
                if (quest->RequiredNpcOrGo[objectiveIdx] == int32(creatureEntry) &&
                    questStatus.CreatureOrGOCount[objectiveIdx] < quest->RequiredNpcOrGoCount[objectiveIdx])
                    wanted = true;
            if (!wanted)
                continue;

            Item* item = bot->GetItemByEntry(quest->GetSrcItemId());
            if (!item)
                continue;

            if (ItemTemplate const* proto = item->GetTemplate())
                for (uint8 spellIdx = 0; spellIdx < MAX_ITEM_PROTO_SPELLS; ++spellIdx)
                    if (proto->Spells[spellIdx].SpellId > 0 &&
                        proto->Spells[spellIdx].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                        return item;
        }
        return nullptr;
    }

    struct TameableBeastCheck
    {
        Player* bot;

        bool operator()(Creature* creature) const
        {
            if (!creature->IsAlive() || creature->IsInCombat() || creature->IsPet() ||
                creature->IsInEvadeMode() || creature->isElite() || IsTargetDummyCreature(creature))
                return false;

            // Tame Beast demands target level <= own level; very grey pets are
            // a waste of the tame.
            if (creature->GetLevel() > bot->GetLevel() || creature->GetLevel() + 10 < bot->GetLevel())
                return false;

            CreatureTemplate const* proto = creature->GetCreatureTemplate();
            return proto && proto->IsTameable(bot->CanTameExoticPets());
        }
    };

    // Test and placeholder stock - "CRobinson ...", "[PH] ...", "Test ..." and
    // friends. None of it is obtainable from any vendor, loot table or start
    // outfit any more, but a piece already sitting on a bot survives every
    // data-level cleanup: deleting the row while that character is ONLINE is
    // simply overwritten by its next save, which is exactly how one pair of
    // shoulders outlived a realm-wide purge. So the engine refuses it instead.
    bool LooksLikeScaffoldingItem(ItemTemplate const* proto)
    {
        if (!proto)
            return false;

        // Lowercased once, so the markers need not enumerate every casing.
        std::string name = proto->Name1;
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        static constexpr char const* kMarkers[] = {
            "[ph]", "crobinson", "monster ", "old ", "deprecated", "unused", "placeholder"
        };
        for (char const* marker : kMarkers)
            if (name.find(marker) != std::string::npos)
                return true;

        // "test" as a WORD, however punctuated. The old list matched "Test " with
        // a trailing space, so "Pirates Patch (Test)" went straight through and
        // ended up worn. A boundary on both sides keeps "Testament" out.
        for (size_t at = name.find("test"); at != std::string::npos; at = name.find("test", at + 4))
        {
            bool const beforeOk = at == 0 || !std::isalpha(static_cast<unsigned char>(name[at - 1]));
            size_t const after = at + 4;
            bool const afterOk = after >= name.size() || !std::isalpha(static_cast<unsigned char>(name[after]));
            if (beforeOk && afterOk)
                return true;
        }

        // And anything the world simply cannot produce. A name filter only ever
        // catches what somebody remembered to label; this catches the rest, which
        // is the actual rule: gear a bot wears should be something a player could
        // have looted from a mob or bought from a vendor. Only plain white and
        // grey gear is judged this way - quest rewards, forged items and anything
        // above common are legitimately absent from loot tables.
        if ((proto->Quality == ITEM_QUALITY_NORMAL || proto->Quality == ITEM_QUALITY_POOR) &&
            (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON) &&
            !BarracksHardcore::IsObtainableInWorld(proto->ItemId))
            return true;

        return false;
    }

    // Strip any scaffolding the bot is already wearing or carrying. The hardcore
    // field kit reissues a proper white piece for the empty slot on its next pass.
    void DiscardScaffoldingItems(Player* bot)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || !LooksLikeScaffoldingItem(item->GetTemplate()))
                continue;

            TC_LOG_INFO("playerbots.pve", "Bot {} discards test item {} from slot {}.",
                bot->GetName(), item->GetTemplate()->Name1, uint32(slot));
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        }

        std::vector<std::pair<uint8, uint8>> doomed;
        ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
        {
            if (LooksLikeScaffoldingItem(item->GetTemplate()))
                doomed.emplace_back(bag, slot);
        });

        for (auto const& position : doomed)
            bot->DestroyItem(position.first, position.second, true);
    }

    // Quest items are the one kind of loot a bot can never shed. The auction
    // screen refuses them and no vendor buys them, so a quest item picked up for
    // a quest the bot never accepted holds that slot for the rest of the
    // character's life.
    //
    // Measured on the live realm, they were the single largest occupant of bot
    // inventories - 1239 stacks across the fleet - and with every backpack sitting
    // at 16 of 16 the gameobject loot guard (CountFreeBagSlots < 2) then refused
    // every death chest the bots had just walked back to reclaim. They stood on
    // top of their own gear and could not pick it up.
    //
    // A player in that position simply destroys them. HasQuestForItem covers both
    // the "collect N of these" requirements of accepted quests and their turn-in
    // items, so anything it disowns is genuinely dead weight - except an item that
    // STARTS a quest, which is still worth something to a bot that may accept it.
    // Worth nothing to a vendor, worth nothing to use: worth nothing.
    //
    // The cap below bounds things a bot has too many of. This is the other half:
    // things a bot should not be carrying even ONE of, because there is no
    // action it can ever take with them. No merchant will buy them, they do
    // nothing when used, and they are not something to wear or carry loot in -
    // they are a slot that will never be free again. Bulging Sack of Silver and
    // the Riding Training Pamphlets are exactly this, and between them they held
    // 21 of Orhild's 80 slots.
    //
    // The use-spell test is what makes this safe without a list of exceptions.
    // A Hearthstone is also white, also worth nothing to a vendor, and also
    // class Miscellaneous - and it survives, because it carries spell 8690.
    // Anything a bot can actually DO something with names a spell here.
    //
    // A quest starter is not spared. DiscardOrphanedQuestItems keeps those on
    // the reasoning that a bot "may accept it", but a managed bot never uses an
    // item to take a quest, so in eight months it has only ever meant the
    // pamphlets accumulate forever. An item needed by a quest the bot has
    // ACTUALLY accepted is still protected, by the same HasQuestForItem test
    // that pass uses.
    bool IsWorthlessClutter(Player* bot, ItemTemplate const* proto)
    {
        if (!bot || !proto)
            return false;

        // Only the bottom of the range. Anything green or better is somebody's
        // business even if this bot cannot use it.
        if (proto->Quality > ITEM_QUALITY_NORMAL)
            return false;

        // A merchant will take it, so the vendor pass can turn it into money.
        if (proto->SellPrice)
            return false;

        switch (proto->Class)
        {
            case ITEM_CLASS_QUEST:
            case ITEM_CLASS_MISC:
            case ITEM_CLASS_KEY:
                break;
            default:
                return false;     // gear, bags, ammo, reagents, trade goods
        }

        if (proto->Class == ITEM_CLASS_MISC && proto->ContainerSlots)
            return false;         // a bag, however cheap

        for (uint8 index = 0; index < MAX_ITEM_PROTO_SPELLS; ++index)
            if (proto->Spells[index].SpellId)
                return false;     // it DOES something - Hearthstone lands here

        return !bot->HasQuestForItem(proto->ItemId);
    }

    // Rogues have been carrying Thistle Tea for months and never drinking it.
    //
    // It is an instant 100 energy, usable in combat, and it is the one
    // consumable a rogue has that changes a fight rather than the walk between
    // fights - which is why the rest pass never reached it: that pass is gated
    // on being OUT of combat, sits the bot down, and waits for a food or drink
    // aura to land. Thistle Tea is none of those things.
    //
    // So it is drunk where it is useful: in a fight, energy on the floor. The
    // item's own cooldown does the real throttling; the timer here only stops
    // the bot re-trying every tick while that cooldown runs.
    void MaybeDrinkThistleTea(Player* bot, PveBotState& state, PveTimePoint now)
    {
        uint32 const teaEntry = g_PveConfig.thistleTeaItemId;
        if (!teaEntry || !bot || bot->GetClass() != CLASS_ROGUE || !bot->IsAlive())
            return;

        if (bot->GetPowerType() != POWER_ENERGY || !bot->IsInCombat())
            return;

        if (now < state.nextThistleTeaAt)
            return;

        if (bot->GetPower(POWER_ENERGY) > int32(g_PveConfig.thistleTeaEnergyBelow))
            return;

        Item* tea = bot->GetItemByEntry(teaEntry);
        if (!tea)
            return;

        // Re-asked every few seconds rather than every tick: the item carries a
        // long shared cooldown and a rogue at zero energy would otherwise spend
        // the whole fight asking.
        state.nextThistleTeaAt = now + std::chrono::seconds(5);

        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        bot->CastItemUseSpell(tea, targets, 0, 0);
    }

    void DiscardWorthlessClutter(Player* bot)
    {
        if (!bot)
            return;

        std::vector<std::pair<uint8, uint8>> doomed;
        ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
        {
            if (IsWorthlessClutter(bot, item->GetTemplate()))
                doomed.emplace_back(bag, slot);
        });

        for (auto const& [bag, slot] : doomed)
            if (Item* item = bot->GetItemByPos(bag, slot))
            {
                TC_LOG_DEBUG("playerbots.pve", "Bot {} discards worthless {}.",
                    bot->GetName(), item->GetTemplate()->Name1);
                bot->DestroyItem(bag, slot, true);
            }

        if (!doomed.empty())
            TC_LOG_INFO("playerbots.pve", "Bot {} dropped {} worthless slot(s).",
                bot->GetName(), uint32(doomed.size()));
    }

    // Nobody needs twenty-eight of anything.
    //
    // A bot buys consumables it never finishes, loots quest starters it will
    // never accept and containers it will never open, and NOTHING clears them.
    // The rations pass understands food and drink by tier; everything else in
    // class Consumable, Quest and Miscellaneous simply accumulates for the life
    // of the character.
    //
    // Measured on the live fleet: 152 of 230 bots carrying sixty items or more,
    // and the worst of them completely full - Orhild at 16/16 backpack and 64/64
    // bags, of which 28 slots were Thistle Tea, 15 were Bulging Sacks of Silver
    // and 6 were Riding Training Pamphlets. A full pack is not a cosmetic
    // problem: it is why he sits on a thousand gold in a level 32 field kit at
    // level 42, because an auction purchase arrives by mail and the mail
    // collector cannot store it; it is why the chest executor refuses to loot;
    // and it is why a rebirth leaves gear that cannot be unequipped still worn.
    //
    // So the count of SLOTS holding a given item is capped, and the surplus goes.
    // Deliberately by slot rather than by quantity: a stack of twenty potions is
    // one slot and harmless, twenty stacks of one potion is twenty slots and is
    // the actual disease. Equipment is never touched, and neither is anything
    // outside those three classes.
    void DiscardHoardedDuplicates(Player* bot)
    {
        uint32 const cap = g_PveConfig.maxSlotsPerItemEntry;
        if (!bot || !cap)
            return;

        std::unordered_map<uint32, uint32> seen;
        std::vector<std::pair<uint8, uint8>> surplus;
        ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                return;

            if (proto->Class != ITEM_CLASS_CONSUMABLE && proto->Class != ITEM_CLASS_QUEST &&
                proto->Class != ITEM_CLASS_MISC)
                return;

            // A bag the bot is carrying spare is the container pass's business,
            // and destroying one that still has things in it would take them
            // with it.
            if (proto->Class == ITEM_CLASS_MISC && item->IsNotEmptyBag())
                return;

            if (++seen[proto->ItemId] > cap)
                surplus.emplace_back(bag, slot);
        });

        for (auto const& [bag, slot] : surplus)
            if (Item* item = bot->GetItemByPos(bag, slot))
            {
                TC_LOG_DEBUG("playerbots.pve", "Bot {} discards a surplus {} (over {} slots).",
                    bot->GetName(), item->GetTemplate()->Name1, cap);
                bot->DestroyItem(bag, slot, true);
            }

        if (!surplus.empty())
            TC_LOG_INFO("playerbots.pve", "Bot {} dropped {} hoarded duplicate slot(s).",
                bot->GetName(), uint32(surplus.size()));
    }

    void DiscardOrphanedQuestItems(Player* bot)
    {
        std::vector<std::pair<uint8, uint8>> doomed;
        ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->Class != ITEM_CLASS_QUEST)
                return;

            if (proto->StartQuest || bot->HasQuestForItem(proto->ItemId))
                return;

            doomed.emplace_back(bag, slot);
        });

        for (auto const& position : doomed)
        {
            if (Item* item = bot->GetItemByPos(position.first, position.second))
            {
                TC_LOG_INFO("playerbots.pve", "Bot {} discards orphaned quest item {}.",
                    bot->GetName(), item->GetTemplate()->Name1);
                bot->DestroyItem(position.first, position.second, true);
            }
        }
    }

    // Rogue poisons are vendor CONSUMABLES here, gated by RequiredLevel, and
    // applying one is the item's own use-spell aimed at a weapon. The class
    // engine cannot drive that at all: its poison entries gate on IsSpellReady,
    // which resolves through the PLAYER's spellbook, and a rogue never "knows" a
    // poison item's spell - so those entries could never fire and rogues fought
    // with bare blades for their whole lives.
    //
    // Item ids and their required levels verified against item_template on this
    // realm. Deliberately stops at the level 60 ranks: the higher ones exist in
    // the table but are post-classic items this realm should not be handing out.
    struct RoguePoisonRank
    {
        uint8 requiredLevel;
        uint32 itemId;
    };

    constexpr RoguePoisonRank kInstantPoison[] = { { 20, 6947 }, { 28, 6949 }, { 36, 6950 }, { 44, 8926 }, { 52, 8927 }, { 60, 8928 } };
    constexpr RoguePoisonRank kDeadlyPoison[] = { { 30, 2892 }, { 38, 2893 }, { 46, 8984 }, { 54, 8985 }, { 60, 20844 } };
    constexpr RoguePoisonRank kCripplingPoison[] = { { 20, 3775 }, { 50, 3776 } };
    constexpr RoguePoisonRank kMindNumbingPoison[] = { { 24, 5237 }, { 38, 6951 }, { 52, 9186 } };
    constexpr RoguePoisonRank kWoundPoison[] = { { 32, 10918 }, { 40, 10920 }, { 48, 10921 }, { 56, 10922 } };

    // Is this one of the poisons a rogue stocks for its own blades?
    //
    // Matched by ITEM ID on purpose, because the item CLASS cannot be trusted
    // for this. DBC.EnforceItemAttributes is on for this realm, and the client
    // Item.dbc calls Crippling Poison II and Mind-numbing Poison II class 15
    // (MISC) rather than class 0 (CONSUMABLE). The server honours the DBC and
    // rewrites the class at load - "Item (Entry: 3776) does not have a correct
    // class 0, must be 15" - so the consumable exclusion in IsAuctionableSurplus
    // silently missed exactly those two, and rogues auctioned the poisons they
    // had just been handed. Identity does not drift; class does.
    bool IsStockedPoisonItem(uint32 itemId);

    // Highest rank the bot's level actually allows. Ranks are listed ascending,
    // so the last one that passes wins.
    template<size_t N>
    uint32 BestPoisonForLevel(Player const* bot, RoguePoisonRank const (&ranks)[N])
    {
        uint32 best = 0;
        for (RoguePoisonRank const& rank : ranks)
            if (bot->GetLevel() >= rank.requiredLevel)
                best = rank.itemId;

        return best;
    }

    bool IsStockedPoisonItem(uint32 itemId)
    {
        if (!itemId)
            return false;

        auto listed = [itemId](RoguePoisonRank const* ranks, size_t count)
        {
            for (size_t index = 0; index < count; ++index)
                if (ranks[index].itemId == itemId)
                    return true;
            return false;
        };

        return listed(kInstantPoison, std::size(kInstantPoison)) ||
            listed(kDeadlyPoison, std::size(kDeadlyPoison)) ||
            listed(kCripplingPoison, std::size(kCripplingPoison)) ||
            listed(kMindNumbingPoison, std::size(kMindNumbingPoison)) ||
            listed(kWoundPoison, std::size(kWoundPoison));
    }

    // A hunter with no arrows is a hunter with no ranged attack, and the ranged
    // positioning correctly refuses to hold a firing line it cannot shoot from -
    // so it falls through to melee and the bot fights with its bow butt.
    //
    // Ammunition is bought at vendors, but only from one that actually stocks it,
    // and a bot whose supply errand keeps landing on the local drink merchant can
    // go indefinitely without any. Guarantee a working quiver the same way the
    // class kit is guaranteed; TryBuySupplies still tops it up normally.
    //
    // Item ids and required levels verified against item_template on this realm,
    // deliberately skipping the deprecated and crafted-only entries.
    constexpr RoguePoisonRank kArrowLadder[] = { { 1, 2512 }, { 10, 2515 }, { 25, 3030 }, { 40, 11285 } };
    constexpr RoguePoisonRank kBulletLadder[] = { { 1, 2516 }, { 10, 2519 }, { 25, 3033 }, { 37, 10512 } };

    void EnsureRangedAmmo(Player* bot)
    {
        uint32 const wantedSubclass = RequiredAmmoSubclass(bot);
        if (!wantedSubclass || !bot->IsAlive())
            return;

        uint32 const ammoId = wantedSubclass == ITEM_SUBCLASS_ARROW
            ? BestPoisonForLevel(bot, kArrowLadder)
            : (wantedSubclass == ITEM_SUBCLASS_BULLET ? BestPoisonForLevel(bot, kBulletLadder) : 0u);
        if (!ammoId)
            return;

        // Count what is actually in the pack, not just the loaded type - the same
        // trap the vendor pass already learned about.
        uint32 const loadedId = bot->GetUInt32Value(PLAYER_AMMO_ID);
        uint32 const carried = bot->GetItemCount(ammoId) + (loadedId && loadedId != ammoId ? bot->GetItemCount(loadedId) : 0);

        if (carried < 50)
        {
            bot->AddItem(ammoId, 200);
            TC_LOG_INFO("playerbots.pve", "Bot {} was out of ammunition; issued 200 of item {}.",
                bot->GetName(), ammoId);
        }

        // Loading it matters as much as owning it: a PLAYER_AMMO_ID pointing at an
        // empty stack means no ranged attack even with a full quiver.
        //
        // This must be AUTHORITATIVE rather than first-write-wins, because nothing
        // else in the tree can repair the field once it goes stale. It survives
        // logout, and the only code that clears it (Spell::CheckItems) runs while
        // ATTEMPTING a shot - which the ranged guard refuses to take precisely
        // because the field points at an empty stack. That is a self-sealing latch:
        // no shot, so no clear, so no reload, so no shot.
        //
        // A hunter crossing a rung of the ladder walks straight into it. It hits 25
        // still loaded with Sharp Arrow, fires that stack to zero, and the top-up
        // above then issues Razor Arrow - so the bot visibly owns 200 good arrows
        // while the field still names the empty type, forever. Confirmed live: a
        // level 35 and a level 58 hunter both still pointing at Rough Arrow (2512,
        // the level ONE rung) with none carried, holding 200 of the right type.
        // Every one of them fought in melee with a full quiver, because the guard
        // in DriveHunterRangedPositioning bails on this field long before any
        // positioning logic is reached.
        uint32 const loadedCount = loadedId ? bot->GetItemCount(loadedId) : 0;
        if (!loadedCount)
        {
            // Prefer the ladder's pick, but settle for anything of the right
            // subclass the bot actually holds, so a pack too full to accept the
            // issue above still cannot leave the hunter latched out of ranged.
            uint32 reloadId = bot->GetItemCount(ammoId) ? ammoId : 0u;
            if (!reloadId)
            {
                ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
                {
                    if (reloadId)
                        return;

                    ItemTemplate const* proto = item->GetTemplate();
                    if (proto && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == wantedSubclass)
                        reloadId = proto->ItemId;
                });
            }

            if (reloadId)
                bot->SetAmmo(reloadId);
        }
    }

    // Keep a working stock of every poison family the bot has access to.
    void EnsureRoguePoisons(Player* bot)
    {
        if (bot->GetClass() != CLASS_ROGUE || !bot->IsAlive())
            return;

        auto stock = [bot](uint32 itemId)
        {
            if (!itemId)
                return;

            uint32 const carried = bot->GetItemCount(itemId);
            if (carried >= 5)
                return;

            bot->AddItem(itemId, 20 - std::min<uint32>(carried, 20));
        };

        stock(BestPoisonForLevel(bot, kInstantPoison));
        stock(BestPoisonForLevel(bot, kDeadlyPoison));
        stock(BestPoisonForLevel(bot, kCripplingPoison));
        stock(BestPoisonForLevel(bot, kMindNumbingPoison));
        stock(BestPoisonForLevel(bot, kWoundPoison));
    }

    // Seal Fate rank 5. Only the highest learned rank of a talent survives in
    // character_spell - lower ranks are superseded away - so simply holding this
    // spell IS five of five, and there are no talent points to count.
    constexpr uint32 kSealFateMaxRank = 14195;

    // Coat the blades. Instant on the mainhand and Deadly on the offhand is the
    // classic damage pairing, but it only earns its place once Seal Fate is turning
    // every crit into an extra combo point. Without that, a bot grinding solo gets
    // more out of never letting anything walk away from it: Crippling on both
    // blades, so the slow is reapplied by whichever hand lands next and a runner is
    // caught rather than chased across the zone.
    //
    // One application per pass, because applying a poison is a real cast and
    // stacking two in a tick just cancels the first.
    void ApplyRoguePoisons(Player* bot)
    {
        if (bot->GetClass() != CLASS_ROGUE || !bot->IsAlive() || bot->IsInCombat() ||
            bot->HasUnitState(UNIT_STATE_CASTING) || bot->isMoving())
            return;

        struct PoisonAssignment
        {
            WeaponAttackType attackType;
            uint32 itemId;
        };

        bool const hasSealFate = bot->HasSpell(kSealFateMaxRank);
        uint32 const cripplingId = BestPoisonForLevel(bot, kCripplingPoison);

        PoisonAssignment const assignments[] = {
            { BASE_ATTACK, hasSealFate ? BestPoisonForLevel(bot, kInstantPoison) : cripplingId },
            { OFF_ATTACK,  hasSealFate ? BestPoisonForLevel(bot, kDeadlyPoison) : cripplingId }
        };

        for (PoisonAssignment const& assignment : assignments)
        {
            if (!assignment.itemId)
                continue;

            Item* weapon = bot->GetWeaponForAttack(assignment.attackType, true);
            if (!weapon || weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                continue;

            Item* poison = bot->GetItemByEntry(assignment.itemId);
            if (!poison)
                continue;

            SpellCastTargets targets;
            targets.SetItemTarget(weapon);
            bot->CastItemUseSpell(poison, targets, 0, 0);

            TC_LOG_INFO("playerbots.pve", "Bot {} coats its {} with poison {}.",
                bot->GetName(), assignment.attackType == BASE_ATTACK ? "mainhand" : "offhand", assignment.itemId);

            // Keep going to the offhand rather than returning. A grinding rogue is
            // almost never simultaneously out of combat AND standing still, so the
            // window this runs in is rare - spending a whole one on a single
            // weapon meant the mainhand won every time and the offhand stayed
            // bare. Bail only if that application actually started a cast.
            if (bot->HasUnitState(UNIT_STATE_CASTING))
                return;
        }
    }

    // Weapon skill is pure friction for a bot: it never visits a weapon master,
    // and skill-up rolls only fire on swings it lands, so every newly equipped
    // weapon type leaves it glancing for hours. Bots are held at the cap for
    // their level instead.
    void MaxOutWeaponSkills(Player* bot)
    {
        static constexpr uint32 kWeaponSkills[] = {
            SKILL_SWORDS, SKILL_AXES, SKILL_BOWS, SKILL_GUNS, SKILL_MACES,
            SKILL_2H_SWORDS, SKILL_STAVES, SKILL_2H_MACES, SKILL_UNARMED,
            SKILL_2H_AXES, SKILL_DAGGERS, SKILL_THROWN, SKILL_CROSSBOWS,
            SKILL_WANDS, SKILL_POLEARMS, SKILL_FIST_WEAPONS
        };

        uint16 const cap = uint16(bot->GetLevel()) * 5;
        if (!cap)
            return;

        for (uint32 skillId : kWeaponSkills)
        {
            if (!bot->HasSkill(skillId))
                continue;
            if (bot->GetSkillValue(skillId) >= cap && bot->GetMaxSkillValue(skillId) >= cap)
                continue;
            bot->SetSkill(skillId, bot->GetSkillStep(skillId), cap, cap);
        }

        // A weapon type the bot never trained has no skill line at all, which is
        // worse than a low one - grant it outright for whatever it is holding.
        for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED })
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->Class != ITEM_CLASS_WEAPON)
                continue;
            uint32 const skillId = proto->GetSkill();
            if (skillId && skillId != SKILL_NONE && !bot->HasSkill(skillId))
                bot->SetSkill(skillId, 0, cap, cap);
        }
    }

    // The nearest beast this hunter could actually tame. Line of sight is
    // deliberately not a filter: a beast behind a rock is still worth walking
    // to, and sight is rechecked where it matters, at the cast.
    Creature* FindTameableBeast(Player* bot, PveBotState& state, float radius)
    {
        std::vector<Creature*> matches;
        TameableBeastCheck check{ bot };
        Trinity::CreatureListSearcher<TameableBeastCheck> searcher(bot, matches, check);
        Cell::VisitGridObjects(bot, searcher, radius);

        Creature* nearest = nullptr;
        float nearestDistance = 0.0f;
        for (Creature* candidate : matches)
        {
            if (IsRecentBadTarget(state, candidate->GetGUID()))
                continue;

            float const distance = bot->GetDistance(candidate);
            if (!nearest || distance < nearestDistance)
            {
                nearest = candidate;
                nearestDistance = distance;
            }
        }
        return nearest;
    }

    // A hunter is a ranged class, but the follow this tick issues otherwise
    // closes to ONE yard for every class alike. That is fatal for a hunter
    // twice over. At five yards UpdateHunterCombatMode flips it into melee
    // mode, and every ranged shot the class engine offers is gated behind
    // ranged mode - and it only flips back at EIGHT yards, a separation nothing
    // in the PvE path ever produced. On top of that, the only code in the whole
    // tree that starts Auto Shot lives in the hunter kite loop, which is
    // reachable exclusively from battlegrounds and duels. So a PvE hunter
    // walked into melee, was no longer allowed to shoot, and never fired Auto
    // Shot at all - it just swung a melee weapon while its pet held the mob.
    //
    // mayMove is false while the engine has already acted or a cast is in
    // flight; Auto Shot upkeep still runs then, because it is an auto-repeat
    // that only needs starting once.
    //
    // Returns true when it owns movement this tick.
    // True when the bot is a hunter actually equipped to fight at range. Kept
    // separate from the positioning routine because the movement fallback needs
    // the same answer on the ticks that routine declines to act.
    bool BotShouldHoldRangedFiringLine(Player* bot)
    {
        if (!bot || bot->GetClass() != CLASS_HUNTER || !bot->HasSpell(75))
            return false;

        if (!bot->GetWeaponForAttack(RANGED_ATTACK, true))
            return false;

        if (RequiredAmmoSubclass(bot))
        {
            uint32 const ammoId = bot->GetUInt32Value(PLAYER_AMMO_ID);
            if (!ammoId || !bot->GetItemCount(ammoId))
                return false;
        }

        return true;
    }

    bool DriveHunterRangedPositioning(Player* bot, Unit* victim, bool mayMove)
    {
        if (!victim || bot->GetClass() != CLASS_HUNTER || !bot->HasSpell(75))
            return false;

        // No ranged weapon, or nothing to feed it: melee genuinely is the plan.
        if (!bot->GetWeaponForAttack(RANGED_ATTACK, true))
            return false;

        if (RequiredAmmoSubclass(bot))
        {
            uint32 const ammoId = bot->GetUInt32Value(PLAYER_AMMO_ID);
            if (!ammoId || !bot->GetItemCount(ammoId))
                return false;
        }

        // Who actually has the hunter? If the mob is on the HUNTER rather than on
        // its pet, melee it. A hunter being chewed on cannot shoot well anyway,
        // and backing off only drags the fight across the zone and into whatever
        // else happens to be standing there - the pet is the threat sink, so when
        // it is doing its job the hunter shoots, and when it is not the hunter
        // fights. Anything already in melee on the bot counts, not just the
        // current victim, or a second mob could be eating it while it politely
        // retreats from the first.
        if (victim->GetVictim() == bot)
            return false;

        for (Unit const* attacker : bot->getAttackers())
        {
            if (attacker && attacker->IsWithinMeleeRange(bot))
                return false;
        }

        playerbot::HunterAutoShotRangeInfo rangeInfo;
        if (!playerbot::PvpCore::GetHunterAutoShotRange(bot, victim, rangeInfo))
            return false;

        // Hold clear of BOTH the weapon's dead zone and the five yard melee-mode
        // threshold, with margin, so the bot settles instead of oscillating
        // across the mode boundary.
        float const holdMin = std::max(rangeInfo.minRange + 2.0f, 10.0f);
        float const holdMax = std::max(holdMin + 5.0f, rangeInfo.maxRange - 4.0f);
        float const distance = rangeInfo.exactDistance;

        // Backing out is for the DEAD ZONE, not for the whole comfort band. The
        // dead zone ends at minRange - eight yards for a classic hunter - and
        // anything past it can already shoot, so retreating from nine yards gave
        // up a perfectly good firing position and walked the bot backwards for no
        // gain. Retreat below the line the shot itself needs, and go back to a
        // clean two yards past it so crossing back in takes real movement rather
        // than a step.
        float const deadZoneEdge = rangeInfo.minRange + playerbot::PLAYERBOT_HUNTER_AUTOSHOT_MIN_SAFETY_MARGIN;
        float const retreatTo = rangeInfo.minRange + 2.0f;

        if (distance > holdMax)
        {
            if (!mayMove)
                return false;

            playerbot::PvpClassActions::IssueFollowMovement(bot, victim, holdMax - 3.0f);
            return true;
        }

        if (distance < deadZoneEdge)
        {
            if (!mayMove)
                return false;

            // Commit to the retreat rather than restarting it every tick. This ran
            // unconditionally, so each pass replaced the in-flight spline with a
            // fresh one computed from wherever the bot had just got to: one step,
            // recompute, one step, recompute. That is the stutter - the bot was
            // being told to start running away several times a second and never
            // allowed to finish. Let an existing point move run to its end; if it
            // stalls or lands short, the next pass reissues from a standstill.
            if (MotionMaster const* activeMotion = bot->GetMotionMaster())
                if (activeMotion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
                    bot->movespline && bot->movespline->Initialized() && !bot->movespline->Finalized())
                    return true;

            // This retreat must PREEMPT whatever the bot is already doing, and that
            // is why it does not go through MoveTowardThrottled: that helper opens
            // with "if (bot->isMoving()) return;" and silently does nothing while a
            // spline or chase is running. The branch then returned true anyway -
            // telling the caller the hunter was holding its firing line - so the
            // bot neither backed out nor chased. It simply stood in melee for the
            // whole fight while every tick reported success. Stuttering in and out
            // of that state is equally what "running in circles" looks like.
            //
            // Clearing the motion master is not enough on its own either: isMoving()
            // reads the movement flags, which stay set until the spline is actually
            // stopped, so StopMoving() has to come with it - exactly what the
            // in-band branch below already does.
            playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
            if (MotionMaster* motionMaster = bot->GetMotionMaster())
            {
                motionMaster->Clear();
                bot->StopMoving();

                // A stale SWIMMING flag from a just-left pond would run the whole
                // retreat at swim speed (MoveTowardThrottled guards this too).
                if (bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) && !bot->IsInWater())
                    bot->SetSwim(false);

                // Straight back from the target, not around it.
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                victim->GetNearPoint(bot, x, y, z, retreatTo, victim->GetAbsoluteAngle(bot));
                bot->UpdateAllowedPositionZ(x, y, z);
                motionMaster->MovePoint(0, x, y, z, true);
            }

            return true;
        }

        // Inside the band: plant, face the target and keep Auto Shot running.
        if (mayMove && bot->isMoving())
        {
            playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
            if (MotionMaster* motionMaster = bot->GetMotionMaster())
                motionMaster->Clear();
            bot->StopMoving();
        }

        if (!bot->isInFront(victim))
        {
            bot->SetFacingToObject(victim);
            bot->SetInFront(victim);
        }

        Spell const* autoRepeat = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        bool const autoShotActive = autoRepeat && autoRepeat->GetSpellInfo() && autoRepeat->GetSpellInfo()->Id == 75;
        bool const autoShotOnTarget = autoShotActive && autoRepeat->m_targets.GetUnitTargetGUID() == victim->GetGUID();
        if (autoShotActive && !autoShotOnTarget)
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        if (!autoShotOnTarget)
            bot->CastSpell(victim, 75, false);

        return true;
    }

    // A petless hunter is a broken hunter: no threat sink, much of its damage
    // missing and half its kit dark. Taming therefore has to OUTRANK grinding,
    // not wait politely for the one moment the bot is simultaneously unengaged,
    // off-errand, rested, off-journey and already standing next to a beast -
    // which for a bot that grinds continuously essentially never arrives. That
    // conjunction is why hunters sat petless for hours.
    //
    // Returns true when it has claimed the tick.
    bool DriveHunterTaming(Player* bot, PveBotState& state)
    {
        if (bot->GetClass() != CLASS_HUNTER || !bot->HasSpell(1515))
            return false;

        // ANY pet rules taming out, and the one that matters is the DEAD one.
        // A dead hunter pet is saved unslotted, so GetPet() and CurrentPet are
        // both empty while Tame Beast is still refused - the cast silently failed
        // and this drive re-ran forever, stopping and re-selecting the same beast
        // every tick. That is what froze hunters standing next to a turtle.
        if (bot->GetPet() || !bot->GetPetGUID().IsEmpty() || !bot->GetMinionGUID().IsEmpty())
            return false;

        if (PetStable const* stable = bot->GetPetStable();
            stable && (stable->CurrentPet || stable->GetUnslottedHunterPet()))
            return false;

        // Tame Beast cannot be cast in combat and wants a peaceful beast, so this
        // only ever gets to run in the gaps between kills. Those gaps are exactly
        // what the old idle-only gate kept losing to the next pull.
        if (!bot->IsAlive() || bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_CASTING))
            return false;

        PveTimePoint const now = PveClock::now();
        if (now < state.tameBackoffUntil)
            return false;

        Creature* beast = FindTameableBeast(bot, state, 80.0f);
        if (!beast)
        {
            state.tameDriveSince = PveTimePoint{};
            return false;
        }

        if (state.tameDriveSince == PveTimePoint{})
            state.tameDriveSince = now;
        else if (now - state.tameDriveSince > std::chrono::minutes(2))
        {
            // Two minutes of walking at one beast without landing a tame means it
            // is not actually gettable. Blacklist it and go back to grinding.
            MarkRecentBadTarget(state, beast->GetGUID());
            state.tameDriveSince = PveTimePoint{};
            state.tameBackoffUntil = now + std::chrono::minutes(3);
            return false;
        }

        if (bot->GetDistance(beast) > 20.0f || !bot->IsWithinLOSInMap(beast))
        {
            // Claim the tick while closing the distance; letting the grind logic
            // run too would leave the two fighting over the motion master.
            // Throttled: re-issuing MovePoint every 250ms tick restarts the spline
            // from scratch each time, which reads as a bot standing still.
            MoveTowardThrottled(bot, beast->GetPosition());
            return true;
        }

        playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
        if (MotionMaster* motionMaster = bot->GetMotionMaster())
            motionMaster->Clear();
        bot->StopMoving();
        bot->SetSelection(beast->GetGUID());
        if (!bot->isInFront(beast))
        {
            bot->SetFacingToObject(beast);
            bot->SetInFront(beast);
        }
        bot->CastSpell(beast, 1515, false);
        if (!bot->HasUnitState(UNIT_STATE_CASTING))
        {
            // The cast was refused. Claiming the tick anyway is what deadlocked
            // hunters: the drive stopped the bot, re-selected the beast and
            // returned, so the rest of the tick - engaging, moving, everything -
            // never ran again. Give the beast up and hand the tick back.
            MarkRecentBadTarget(state, beast->GetGUID());
            state.tameDriveSince = PveTimePoint{};
            state.tameBackoffUntil = now + std::chrono::seconds(30);
            return false;
        }

        state.tamingUntil = now + std::chrono::seconds(25);
        state.tameDriveSince = PveTimePoint{};
        TC_LOG_INFO("playerbots.pve", "Bot {} taming {} (level {}).",
            bot->GetName(), beast->GetName(), beast->GetLevel());
        return true;
    }

    // Growl ranks, verified against classic_pet_training_template on this realm:
    // seven ranks, one every ten levels, and every one of them is TRAINER taught.
    // A bot never visits a pet trainer, so its pet knew no Growl at all and held
    // no threat - everything the pet was sent at simply walked past it and hit
    // the hunter instead.
    uint32 BestGrowlRankForLevel(uint8 level)
    {
        struct GrowlRank
        {
            uint8 level;
            uint32 spellId;
        };

        static constexpr GrowlRank kGrowlRanks[] = {
            { 60, 14921 }, { 50, 14920 }, { 40, 14919 }, { 30, 14918 },
            { 20, 14917 }, { 10, 14916 }, { 1, 2649 }
        };

        for (GrowlRank const& rank : kGrowlRanks)
            if (level >= rank.level)
                return rank.spellId;

        return 0;
    }

    // Give the pet the best Growl it could train for, and switch autocast on -
    // knowing the spell is not the same as casting it.
    void EnsurePetKnowsGrowl(Player* bot)
    {
        Pet* pet = bot->GetPet();
        if (!pet || !pet->IsAlive() || pet->getPetType() != HUNTER_PET)
            return;

        uint32 const growlId = BestGrowlRankForLevel(uint8(pet->GetLevel()));
        if (!growlId)
            return;

        SpellInfo const* growlInfo = sSpellMgr->GetSpellInfo(growlId);
        if (!growlInfo)
            return;

        if (!pet->HasSpell(growlId))
        {
            // Retire every other rank first, so a level-up does not leave the
            // action bar and the autocast flag pinned to a stale one.
            for (uint32 otherRank : { 2649u, 14916u, 14917u, 14918u, 14919u, 14920u, 14921u })
                if (otherRank != growlId && pet->HasSpell(otherRank))
                    pet->removeSpell(otherRank, false);

            if (!pet->learnSpell(growlId))
                return;

            TC_LOG_INFO("playerbots.pve", "Bot {} taught its level {} pet Growl ({}).",
                bot->GetName(), pet->GetLevel(), growlId);
        }

        // Explicit apply, not a flip, so this is safe to re-run every pass.
        if (CharmInfo* charmInfo = pet->GetCharmInfo(); charmInfo && growlInfo->IsAutocastable())
        {
            pet->ToggleAutocast(growlInfo, true);
            charmInfo->SetSpellAutocast(growlInfo, true);
        }

        // A passive pet neither attacks nor growls.
        if (pet->GetReactState() == REACT_PASSIVE)
            pet->SetReactState(REACT_DEFENSIVE);
    }

    // Cast Growl outright rather than trusting the autocast flag.
    //
    // Autocast alone cannot be relied on here: Pet::ToggleAutocast only repairs
    // the active flag when the spell is NOT already in the pet's autocast list,
    // so once anything clears that flag nothing can put it back - and the flag
    // was seen flipping from ACT_ENABLED to ACT_DISABLED on a live pet that had
    // been granted Growl minutes earlier. Issuing the cast directly removes the
    // dependency on that state entirely; the pet's own cooldown paces it.
    void DrivePetGrowl(Player* bot)
    {
        Pet* pet = bot->GetPet();
        if (!pet || !pet->IsAlive() || pet->getPetType() != HUNTER_PET)
            return;

        Unit* petVictim = pet->GetVictim();
        if (!petVictim || !petVictim->IsAlive())
            return;

        uint32 const growlId = BestGrowlRankForLevel(uint8(pet->GetLevel()));
        if (!growlId || !pet->HasSpell(growlId))
            return;

        if (pet->HasUnitState(UNIT_STATE_CASTING) || pet->GetSpellHistory()->HasCooldown(growlId))
            return;

        pet->CastSpell(petVictim, growlId, false);
    }

    // Hold a bot's pet at full happiness.
    //
    // Classic happiness decay is deliberately ON for this realm
    // (Centurion.Classic.PetHappinessDecay) and that is right for a player, who
    // can keep the right diet in their bags and notice when the pet sulks. A bot
    // cannot be relied on for either, and the penalty is real: an unhappy pet
    // loses damage and eventually runs off entirely, which on a hunter also
    // takes the threat sink with it. Players keep the classic rule; bot pets are
    // simply held at the top.
    void KeepPetHappy(Player* bot)
    {
        Pet* pet = bot->GetPet();
        if (!pet || !pet->IsAlive() || pet->getPetType() != HUNTER_PET)
            return;

        int32 const maxHappiness = int32(pet->GetMaxPower(POWER_HAPPINESS));
        if (maxHappiness > 0 && int32(pet->GetPower(POWER_HAPPINESS)) < maxHappiness)
            pet->SetPower(POWER_HAPPINESS, maxHappiness);
    }

    // Mend Pet: a channelled heal-over-time on the pet, cast between fights.
    //
    // Gated on the hunter having mana to spare, because healing the pet must
    // never be the reason it cannot open the next fight - and on being out of
    // combat, both because that is when the channel survives long enough to
    // finish and because in combat the mana belongs to the rotation.
    constexpr float PveMendPetPetHealthPct = 60.0f;
    constexpr float PveMendPetMinOwnerManaPct = 60.0f;
    constexpr uint32 SPELL_HUNTER_MEND_PET = 136;

    void MaybeMendPet(Player* bot)
    {
        if (bot->GetClass() != CLASS_HUNTER || bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_CASTING))
            return;

        Pet* pet = bot->GetPet();
        if (!pet || !pet->IsAlive() || pet->GetHealthPct() >= PveMendPetPetHealthPct)
            return;

        if (bot->GetMaxPower(POWER_MANA) <= 0 || bot->GetPowerPct(POWER_MANA) < PveMendPetMinOwnerManaPct)
            return;

        uint32 const mendPetId = HighestKnownRankInChain(bot, SPELL_HUNTER_MEND_PET);
        if (!mendPetId || bot->GetSpellHistory()->HasCooldown(mendPetId))
            return;

        // Already ticking? Asked by CHAIN - the rank that landed is whichever one
        // the hunter actually knew, never the base id.
        for (uint32 spellId = SPELL_HUNTER_MEND_PET; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
            if (pet->HasAura(spellId))
                return;

        // A channel does not survive walking, so plant first - the same thing the
        // eat/drink path does before its own out-of-combat cast.
        if (bot->isMoving())
        {
            playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
            if (MotionMaster* motionMaster = bot->GetMotionMaster())
                motionMaster->Clear();
            bot->StopMoving();
        }

        bot->CastSpell(pet, mendPetId, false);
        TC_LOG_INFO("playerbots.pve", "Bot {} mends its pet at {:.0f}% health.",
            bot->GetName(), pet->GetHealthPct());
    }

    // Keep a hunter pet fed: happiness decay makes an unfed pet leave.
    void MaybeFeedPet(Player* bot)
    {
        Pet* pet = bot->GetPet();
        if (!pet || pet->getPetType() != HUNTER_PET || !bot->HasSpell(6991))
            return;

        if (bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_CASTING) || pet->GetHappinessState() == HAPPY)
            return;

        Item* food = nullptr;
        ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
        {
            if (food)
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (proto && pet->HaveInDiet(proto) && pet->GetCurrentFoodBenefitLevel(proto->ItemLevel) > 0)
                food = item;
        });
        if (!food)
            return;

        bot->CastSpell(food, 6991, false);
    }

    // Adjacent packmates fight together: adopt the fight of a nearby managed
    // bot already in combat - its victim, or whatever is hitting it. Never
    // another bot (one team), never a resting exclusion's problem: callers
    // gate on the same conditions as a fresh grind pick.
    // Adopt a nearby packmate's fight. Bounded the same way the bot's own target
    // selection is: a level 13 has no business farming a level 5 just because a
    // level 6 standing next to it happens to be on one, and joining that fight
    // earns nothing while pulling the bot away from its own bracket.
    //
    // The foe's OWN distance is checked too. Only the ally was, so a packmate
    // could be two paces away while the thing it was chasing sat a hundred yards
    // off, and the assist would send the bot on that walk.
    // Defined below, beside the snapshot it reads. Declared here rather than
    // moved: this is the same anonymous namespace, which is the part that
    // matters - a declaration that drifts into namespace playerbot compiles
    // clean and fails at link.
    bool HasAggroSlotFree(ObjectGuid humanGuid);
    // Likewise: the snapshot's answer to "may anyone be sent after this person".
    bool HumanMayBeHunted(ObjectGuid humanGuid);

    // Whether this person, or something they own, is actually swinging at
    // `subject`. getAttackers() is filled by Unit::Attack, so this is the honest
    // "they started it" test rather than a guess from who is standing where.
    bool IsSwingingAt(Unit const* subject, Player const* human)
    {
        if (!subject || !human)
            return false;

        for (Unit* attacker : subject->getAttackers())
            if (attacker == human ||
                (attacker && attacker->GetCharmerOrOwnerPlayerOrPlayerItself() == human))
                return true;

        return false;
    }

    Unit* PickBotAssistTarget(Player* bot, playerbot::PveConfig const& cfg)
    {
        Map* map = bot->FindMap();
        if (!map)
            return nullptr;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* ally = itr->GetSource();
            if (!ally || ally == bot || !ally->IsAlive() || !ally->IsInCombat())
                continue;

            if (!playerbot::IsManagedRandomBot(ally) || playerbot::PveManager::IsPvpOnlyBot(ally))
                continue;

            if (!bot->IsWithinDistInMap(ally, 30.0f))
                continue;

            Unit* foe = ally->GetVictim();
            if (!foe)
                for (Unit* attacker : ally->getAttackers())
                {
                    foe = attacker;
                    break;
                }

            if (!foe || !foe->IsAlive() || !bot->IsValidAttackTarget(foe))
                continue;

            // Assist onto the PERSON, not their pet. Resolved before the two
            // screens below on purpose: before the managed-bot check so bots do
            // not brawl with each other's minions, and before the level band so a
            // low level pet cannot smuggle its owner past a filter the owner would
            // not have passed - players are exempt from that band anyway.
            if (foe->GetTypeId() != TYPEID_PLAYER)
            {
                if (Unit* owner = foe->GetCharmerOrOwner())
                    if (Player* ownerPlayer = owner->ToPlayer())
                        if (ownerPlayer->IsAlive() && bot->IsValidAttackTarget(ownerPlayer))
                            foe = ownerPlayer;
            }

            if (foe->GetTypeId() == TYPEID_PLAYER && playerbot::IsManagedRandomBot(foe->ToPlayer()))
                continue;

            // Joining somebody else's PvP fight is proactive, not self-defense.
            // Do not let pack-assist drag a lower-level bot into a suicide fight.
            if (foe->GetTypeId() == TYPEID_PLAYER &&
                !IsProactivePlayerLevelAcceptable(bot, foe->ToPlayer()))
                continue;

            // And proactive means War Mode or a bounty - with one waiver, which
            // is the whole reason this check is not the flat one used elsewhere.
            //
            // The foe here is taken from the ally's VICTIM first and only then
            // from its attackers, so this function covers both "my ally started
            // on this person" and "this person started on my ally". The first is
            // the fleet picking a fight and is refused; the second is somebody
            // who opened on a bot, and answering that is not aggression, it is
            // the pack doing what a pack does. Someone quietly questing beside a
            // brawl is left out of it either way.
            if (foe->GetTypeId() == TYPEID_PLAYER &&
                !HumanMayBeHunted(foe->GetGUID()) &&
                !IsSwingingAt(ally, foe->ToPlayer()))
                continue;

            // And it is proactive for the aggro budget too. Without this a pack
            // walks straight around the cap: the first bot pulls within budget
            // and the rest pile on as 'assist', which is the dogpile the budget
            // exists to stop.
            if (foe->GetTypeId() == TYPEID_PLAYER && !HasAggroSlotFree(foe->GetGUID()))
                continue;

            // Worth crossing the room for, and worth the bot's time when it gets
            // there. Players are exempt from the level band - helping against one
            // is about the fight, not the experience.
            if (!bot->IsWithinDistInMap(foe, cfg.grindSearchRadius))
                continue;

            if (foe->GetTypeId() != TYPEID_PLAYER)
            {
                int32 const levelDelta = int32(foe->GetLevel()) - int32(bot->GetLevel());
                if (levelDelta > int32(cfg.grindMaxLevelAbove) || -levelDelta > int32(cfg.grindMaxLevelBelow))
                    continue;
            }

            return foe;
        }

        return nullptr;
    }

    // Creature entries the bot still needs kill credit for. Grinding prefers
    // these so an accepted kill quest completes as a side effect of leveling.
    std::unordered_set<uint32> CollectWantedKillEntries(Player* bot)
    {
        std::unordered_set<uint32> wanted;
        if (!playerbot::PveManager::GetConfig().questsEnabled)
            return wanted;

        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            for (uint8 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
                if (quest->RequiredNpcOrGo[index] > 0 && status.CreatureOrGOCount[index] < quest->RequiredNpcOrGoCount[index])
                    wanted.insert(uint32(quest->RequiredNpcOrGo[index]));
        }

        return wanted;
    }

    Unit* PickGrindTarget(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        std::vector<Creature*> matches;
        GrindTargetCheck check{ bot, cfg };
        Trinity::CreatureListSearcher<GrindTargetCheck> searcher(bot, matches, check);
        Cell::VisitGridObjects(bot, searcher, cfg.grindSearchRadius);
        if (matches.empty())
            return nullptr;

        // Quest targets are preferred, but only on a distance leash: walking
        // past a closer mob's aggro radius to reach a farther quest mob pulls
        // both. A wanted mob wins only when it is at most 20y farther than the
        // nearest candidate.
        float nearestDistance = 0.0f;
        for (Creature* candidate : matches)
        {
            float const distance = bot->GetDistance(candidate);
            if (!nearestDistance || distance < nearestDistance)
                nearestDistance = distance;
        }

        std::unordered_set<uint32> const wanted = CollectWantedKillEntries(bot);
        float const questLeash = nearestDistance + 20.0f;
        std::sort(matches.begin(), matches.end(), [bot, &wanted, questLeash](Creature* left, Creature* right)
        {
            bool const leftWanted = wanted.count(left->GetEntry()) != 0 && bot->GetDistance(left) <= questLeash;
            bool const rightWanted = wanted.count(right->GetEntry()) != 0 && bot->GetDistance(right) <= questLeash;
            if (leftWanted != rightWanted)
                return leftWanted;
            return bot->GetDistance(left) < bot->GetDistance(right);
        });

        // Raycasts are the expensive part; only vet the closest handful. Navmesh
        // paths are an order of magnitude worse again, so they get their own much
        // tighter allowance on top of the fleet-wide budget.
        size_t losProbesLeft = 8;
        size_t pathProbesLeft = 2;
        for (Creature* candidate : matches)
        {
            if (IsRecentBadTarget(state, candidate->GetGUID()))
                continue;

            if (!losProbesLeft--)
                break;

            if (!bot->IsWithinLOSInMap(candidate))
                continue;

            // Line of sight is not a road. A mob across a canyon or a river is
            // plainly visible and completely unreachable, and engaging one used
            // to be permanent: the engaged bot skips every other behaviour and
            // the victim never stops resolving, so it chased a target it could
            // never touch until the server restarted. Anything beyond arm's
            // reach has to have an actual path.
            // Only far targets are worth the path query. Anything close is
            // almost always reachable, and the stalled-chase give-up ends the
            // rare case that is not - it costs nothing and catches everything
            // this screen would, just a few seconds later.
            if (bot->GetDistance(candidate) > 25.0f && pathProbesLeft)
            {
                --pathProbesLeft;
                WalkPathResult const pathResult = CheckWalkPath(bot, candidate->GetPosition());
                if (pathResult == WalkPathResult::Deferred)
                    return nullptr;
                if (pathResult == WalkPathResult::Unreachable)
                {
                    MarkRecentBadTarget(state, candidate->GetGUID());
                    continue;
                }
            }

            return candidate;
        }

        return nullptr;
    }

    // When the engage-radius scan is empty, look further out and walk toward the
    // nearest prospect instead of wandering blind. Racial start points sit in
    // mob-free pockets (vendors, guards, triggers only), and an undirected random
    // walk takes minutes to drift out of one.
    Creature* FindGrindProspect(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        std::vector<Creature*> matches;
        GrindTargetCheck check{ bot, cfg };
        Trinity::CreatureListSearcher<GrindTargetCheck> searcher(bot, matches, check);
        Cell::VisitGridObjects(bot, searcher, cfg.grindSearchRadius * 3.0f);
        if (matches.empty())
            return nullptr;

        // Nearest is not best. The level window above only says what is ALLOWED;
        // picking purely by distance inside it had bots farming whatever happened
        // to be underfoot, which at the bottom of a band means grey mobs worth no
        // experience and at the top means fights they barely survive.
        //
        // Score each candidate as its distance plus a fixed number of yards for
        // every level away from the bot, so a well-matched mob is worth walking
        // past a poorly-matched closer one - but only so far, which keeps the bot
        // from crossing the zone for a perfect match.
        Creature* best = nullptr;
        float bestScore = 0.0f;
        for (Creature* candidate : matches)
        {
            if (IsRecentBadTarget(state, candidate->GetGUID()))
                continue;

            int32 const levelGap = std::abs(int32(candidate->GetLevel()) - int32(bot->GetLevel()));
            float const score = bot->GetDistance(candidate) +
                float(levelGap) * cfg.grindLevelMatchYards;
            if (!best || score < bestScore)
            {
                best = candidate;
                bestScore = score;
            }
        }

        return best;
    }

    // Where the real people are.
    //
    // The server already knows this: every connected player sits in
    // ObjectAccessor's map, so finding one is a lookup, not a search. Guardians
    // used to locate players with Cell::VisitWorldObjects over a 120 yard sweep -
    // grid work on the map thread, repeated per guardian, to rediscover something
    // the core could simply be asked.
    //
    // Rebuilt once a second on the world thread and read as a plain vector. Humans
    // are a handful even on a busy realm, so this stays tiny however many bots are
    // logged in.
    struct HumanSpot
    {
        ObjectGuid Guid;
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        uint32 Level = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        // How many more bots may proactively pull this person: their budget
        // less the bots already on them. Computed on the world thread once a
        // second. With the budget disabled the world thread writes a sentinel,
        // so the map-thread gate stays a plain comparison and never has to
        // read config on the 250 ms path.
        int32 AggroSlotsFree = std::numeric_limits<int32>::max();
        // Stacks of bounty on this person, read from the registry on the same
        // pass. Carried here rather than looked up per bot because the figure
        // widens a hunt radius on the 250 ms path, and that path must not take
        // a lock.
        uint32 Bounty = 0;
        // Whether anyone may be SENT after this person unprompted.
        //
        // War Mode is a declaration that you came here for this. Someone who has
        // not made it is left alone by every proactive path - no hunt, no
        // guardian closing the gap, no prod, no travelling to their position -
        // and a bounty overrides it, because a bounty is a standing invitation
        // you earned by killing people.
        //
        // This is emphatically NOT attackability. The pseudo-faction in
        // Object.cpp stays exactly as it is, so a bot still defends itself, still
        // retaliates, and can still be attacked first by anyone who fancies it.
        // The rule is about who STARTS it.
        //
        // Decided once a second on the world thread, where reading the opt-in
        // set is legal, rather than per bot on the 250 ms path.
        bool Huntable = true;
    };

    // Sized on the world thread, where reading Group is legal.
    //
    // Only party members actually WITH the person count. A group spread across
    // a continent is not five people helping you, and letting it read as five
    // would hand a solo player at the far end of a raid the whole fleet.
    int32 AggroBudgetFor(Player const* human, playerbot::PveConfig const& cfg)
    {
        uint32 nearby = 1;
        if (Group const* group = human->GetGroup())
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                if (slot.guid == human->GetGUID())
                    continue;

                Player const* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                if (member && member->IsInWorld() && member->IsAlive() &&
                    member->GetMapId() == human->GetMapId() &&
                    member->IsWithinDist3d(human, cfg.aggroBudgetPartyRadius))
                    ++nearby;
            }

        int64 const budget = int64(cfg.aggroBudgetSolo) +
            int64(cfg.aggroBudgetPerExtraMember) * int64(nearby - 1);
        return int32(std::min<int64>(budget, int64(cfg.aggroBudgetMaxPerPlayer)));
    }

    // The CEILING on its own: is this person weak enough that a bot would take
    // the fight?
    //
    // Autonomous bots will fight uphill, but only so far. The cut-off is the
    // colour the client would paint the target: five or more levels up is RED,
    // which is the game's own way of saying you will lose, and a bot that walks
    // into one is just donating a corpse. Everything up to and including orange
    // is fair game - see Trinity::XP::GetColorCode for the boundary.
    //
    // Separate from the full rule below because one caller genuinely wants only
    // this half: the outmatched-lookout test in CallForHelpAgainstBounties asks
    // "is this person too strong for me", and a floor there would have bots
    // shouting for help about somebody half their level.
    bool IsProactiveTargetWithinPower(Player const* bot, uint32 playerLevel)
    {
        if (!bot)
            return false;

        return playerLevel <= uint32(bot->GetLevel()) + g_PveConfig.proactiveMaxLevelsAbove;
    }

    // The whole proactive level rule: the ceiling above, and the floor under it.
    //
    // The floor lived only in MayProactivelyEngage, which is the gate on the
    // final act of attacking. This helper is what the thirteen other proactive
    // paths ask - target acquisition, the guardian picking somewhere to close
    // on, FindNearestHumanSpot, pack-assist - and it had a ceiling and nothing
    // else. So a level fifty guardian would happily select a level forty-five as
    // a destination, port across the zone, walk up to them, and join in the
    // moment they swung at anything: every step answered "yes, they are not too
    // strong for me", and the one place that would have said no was never on
    // that path.
    //
    // Self-defence paths intentionally do NOT call this helper: a bot that is
    // attacked fights back however far above OR below it the attacker is.
    bool IsProactivePlayerLevelAcceptable(Player const* bot, uint32 playerLevel, uint32 bountyStacks)
    {
        if (!IsProactiveTargetWithinPower(bot, playerLevel))
            return false;

        // Nobody gets picked on from far above them, and it takes a real bounty
        // - not one stack - to waive it. See MayProactivelyEngage, which states
        // the same rule for the attack itself; the two must agree or the bot
        // travels to somebody it will then refuse to fight.
        if (uint32 const below = g_PveConfig.proactiveMaxLevelsBelow)
            if (bountyStacks < g_PveConfig.proactiveBountyStacks &&
                playerLevel + below < uint32(bot->GetLevel()))
                return false;

        return true;
    }

    bool IsProactivePlayerLevelAcceptable(Player const* bot, Player const* player)
    {
        // Takes the bounty lock, so it is asked only about real people - of whom
        // there are a handful - exactly as PlayerbotPvpCore.cpp:2570 already does.
        // The snapshot callers below pass spot.Bounty instead and take no lock.
        return player && IsProactivePlayerLevelAcceptable(bot, player->GetLevel(), Bounty::GetStacks(player));
    }

    std::mutex g_HumanSpotLock;
    std::vector<HumanSpot> g_HumanSpots;

    // ---------------------------------------------------------------------------
    // Aggression: 1-100, one value per character, deciding how long that bot will
    // tolerate not fighting a person before it goes and finds one.
    //
    // Derived from the character GUID rather than stored, so it is stable for the
    // life of the character, survives restarts, costs no column and no lookup - and
    // two bots of the same class are still different people.
    //
    // The shape is deliberately a bell rather than a flat roll: most of a class
    // sits near its temperament and the extremes are rare, so a fleet reads as a
    // population rather than as noise. Three uniforms averaged (Irwin-Hall) is
    // close enough to normal for this and needs no floating-point library.
    // ---------------------------------------------------------------------------
    float ClassAggressionMean(uint8 classId)
    {
        switch (classId)
        {
        case CLASS_ROGUE:        return 75.0f;   // picks the fight, by trade
        case CLASS_DEATH_KNIGHT: return 72.0f;
        case CLASS_WARRIOR:      return 70.0f;
        case CLASS_PALADIN:      return 60.0f;
        case CLASS_HUNTER:       return 58.0f;
        case CLASS_SHAMAN:       return 55.0f;
        case CLASS_DRUID:        return 50.0f;
        case CLASS_WARLOCK:      return 45.0f;
        case CLASS_MAGE:         return 40.0f;
        case CLASS_PRIEST:       return 35.0f;   // and the healer least of all
        default:                 return 50.0f;
        }
    }

    uint8 GetBotAggression(Player const* bot)
    {
        if (!bot)
            return 50;

        // splitmix64: cheap, and scatters adjacent GUIDs properly - a plain
        // modulo would hand consecutively created bots near-identical values.
        auto mix = [](uint64 value)
        {
            value += 0x9E3779B97F4A7C15ull;
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
            return value ^ (value >> 31);
        };

        uint64 const hashed = mix(bot->GetGUID().GetRawValue());
        float const u1 = float((hashed >> 0) & 0xFFFF) / 65535.0f;
        float const u2 = float((hashed >> 16) & 0xFFFF) / 65535.0f;
        float const u3 = float((hashed >> 32) & 0xFFFF) / 65535.0f;

        // Mean 0.5, bell shaped, bounded - so no bot ever rolls absurdly far from
        // its class temperament.
        float const bell = (u1 + u2 + u3) / 3.0f;
        float const value = ClassAggressionMean(bot->GetClass()) + (bell - 0.5f) * 90.0f;
        return uint8(std::clamp(value, 1.0f, 100.0f));
    }

    // How long this bot tolerates peace. Aggression 100 waits the minimum, 1 waits
    // the maximum, everything else in between.
    uint32 AggressionIdleMinutes(Player const* bot, playerbot::PveConfig const& cfg)
    {
        if (!cfg.aggressionMaxMinutes)
            return 0;

        float const scale = float(GetBotAggression(bot) - 1) / 99.0f;
        float const minutes = float(cfg.aggressionMaxMinutes) -
            scale * float(cfg.aggressionMaxMinutes - cfg.aggressionMinMinutes);
        return uint32(std::max(1.0f, minutes));
    }

    // Any player at all, wherever they are. Used by guardians, which travel by
    // teleport and so are not restricted to their own map.
    // Somebody in this exact zone, or nobody. A zone guardian NEVER leaves its
    // zone to hunt: it is the guardian OF that ground, and one that abandons
    // Ashenvale to fight in Orgrimmar is not guarding anything. When its zone is
    // empty it simply holds, which is the correct answer for a guardian - the
    // roaming is the ordinary bots' job.
    bool PickHumanSpotInZone(Player const* bot, uint32 zoneId, HumanSpot& out)
    {
        if (!bot || !zoneId)
            return false;

        std::lock_guard<std::mutex> guard(g_HumanSpotLock);

        std::vector<HumanSpot const*> inZone;
        for (HumanSpot const& spot : g_HumanSpots)
            if (spot.ZoneId == zoneId && spot.Huntable &&
                IsProactivePlayerLevelAcceptable(bot, spot.Level, spot.Bounty))
                inZone.push_back(&spot);

        if (inZone.empty())
            return false;

        out = *inZone[urand(0, uint32(inZone.size()) - 1)];
        return true;
    }

    // zoneFilter 0 means "anywhere on this map"; a zone guardian passes its own
    // zone so that every question it asks about players is scoped to the ground it
    // is responsible for.
    bool FindNearestHumanSpot(uint32 mapId, float x, float y, float z, HumanSpot& out, float& outDistance,
        uint32 zoneFilter = 0, Player const* proactiveBot = nullptr)
    {
        std::lock_guard<std::mutex> guard(g_HumanSpotLock);

        bool found = false;
        float bestSq = 0.0f;
        for (HumanSpot const& spot : g_HumanSpots)
        {
            if (spot.MapId != mapId)
                continue;

            if (zoneFilter && spot.ZoneId != zoneFilter)
                continue;

            // proactiveBot is what marks this call as "I am looking for someone
            // to go after" rather than "where are the people". The avoidance
            // caller - a timid bot picking somewhere quiet to grind - passes
            // nothing, and must keep seeing everybody: giving a wide berth to
            // someone you are not allowed to attack is still the right move.
            if (proactiveBot && (!spot.Huntable ||
                !IsProactivePlayerLevelAcceptable(proactiveBot, spot.Level, spot.Bounty)))
                continue;

            float const dx = spot.X - x;
            float const dy = spot.Y - y;
            float const dz = spot.Z - z;
            float const distSq = dx * dx + dy * dy + dz * dz;
            if (found && distSq >= bestSq)
                continue;

            bestSq = distSq;
            out = spot;
            found = true;
        }

        if (found)
            outDistance = std::sqrt(bestSq);

        return found;
    }

    // Zone guardians hunt: the nearest real player they may lawfully attack.
    // The hardcore pseudo-faction (Object.cpp's IsValidAttackTarget override)
    // is what makes an armed guardian and a player mutually attackable, so this
    // finds nobody on realms without it - no config coupling needed here.
    // Radius is deliberately wide: a guardian owns its whole zone.
    // Whether one more bot may pull this person, read from the snapshot.
    // Someone the snapshot does not carry - not a person, in a sanctuary, just
    // logged in - is not gated, which matches how the rest of this file treats
    // an absent spot.
    bool HasAggroSlotFree(ObjectGuid humanGuid)
    {
        std::lock_guard<std::mutex> guard(g_HumanSpotLock);
        for (HumanSpot const& spot : g_HumanSpots)
            if (spot.Guid == humanGuid)
                return spot.AggroSlotsFree > 0;

        return true;
    }

    // Deliberately the OPPOSITE default to the one above. An absent spot means
    // sanctuary, a fresh login, or a snapshot that has not caught up - and the
    // question here is "may I start on this person", where not knowing has to
    // mean no. The cost of being wrong for one second is a bot that does not
    // join in; the cost the other way is exactly the thing this rule exists to
    // stop.
    bool HumanMayBeHunted(ObjectGuid humanGuid)
    {
        std::lock_guard<std::mutex> guard(g_HumanSpotLock);
        for (HumanSpot const& spot : g_HumanSpots)
            if (spot.Guid == humanGuid)
                return spot.Huntable;

        return false;
    }

    // How close counts as already on the way to a bounty. Shared by the two
    // sides that have to agree about it: the hunt reach a bot walks in from,
    // and the dispatch's refusal to teleport somebody who is already that
    // near. Comfortably above the 210 yard teleport floor, so a bot that is
    // dropped in walks the rest instead of qualifying to be sent again.
    constexpr float kBountyAnsweringYards = 240.0f;

    // bountiedOnly: the bot is still timid - it lost to a person recently and is
    // meant to be leaving people alone. A bounty overrides that, and only a
    // bounty: hiding behind the corpses of the bots you already beat was the one
    // way to carry a price on your head and still be left in peace.
    Player* PickHuntTarget(Player* bot, float radius, bool bountiedOnly = false)
    {
        // Read the snapshot instead of sweeping the grid. This was a
        // Cell::VisitWorldObjects over a 120 yard radius, run per guardian on the
        // map update thread, to rediscover positions the core already had. Humans
        // are a handful, so this loop is shorter than the searcher's setup alone.
        std::vector<HumanSpot> candidates;
        {
            std::lock_guard<std::mutex> guard(g_HumanSpotLock);
            candidates = g_HumanSpots;
        }

        Player* nearest = nullptr;
        float nearestDistance = 0.0f;
        for (HumanSpot const& spot : candidates)
        {
            if (spot.MapId != bot->GetMapId())
                continue;

            // War Mode off and no bounty: not somebody to start on. This is the
            // proactive path by its own definition (see the comment below), so
            // the gate belongs here and nowhere near IsValidAttackTarget - a bot
            // must still be able to fight this person back.
            if (!spot.Huntable)
                continue;

            // Timid, so only a bounty is worth breaking cover for.
            if (bountiedOnly && !spot.Bounty)
                continue;

            Player* candidate = ObjectAccessor::FindConnectedPlayer(spot.Guid);
            if (!candidate || !candidate->IsInWorld() || !candidate->IsAlive())
                continue;

            // A bounty is a standing invitation: bots come from further away
            // the higher it is, up to the configured bonus at the cap.
            // A bounty is a standing invitation: bots come from further away the
            // higher it is, up to the configured bonus at the cap.
            //
            // Floored at the distance the DISPATCH treats as "already answering"
            // once the bounty is relentless, because otherwise there is a ring
            // around the target where a bot does neither. At 17 stacks the hunt
            // reach is about 185 yards while the dispatch refuses to teleport
            // anybody inside 240, so a bot between the two was not walking in and
            // was not being sent - it stood there, which is exactly what a bounty
            // reported seeing. The floor closes the ring, and sits at the
            // dispatch's own number so a teleported bot lands inside its own
            // hunt reach and walks the rest rather than being re-sent.
            float huntReach = radius + Bounty::HuntRadiusBonusYards(spot.Bounty);
            if (Bounty::IsHuntedRelentlessly(spot.Bounty))
                huntReach = std::max(huntReach, kBountyAnsweringYards);

            if (!bot->IsWithinDistInMap(candidate, huntReach))
                continue;

            if (candidate == bot || candidate->IsGameMaster() ||
                candidate->IsBeingTeleported() || !bot->IsValidAttackTarget(candidate))
                continue;

            // This function is ONLY proactive acquisition. Never volunteer for an
            // uphill PvP fight; retaliation is selected from getAttackers() below
            // and deliberately ignores this level rule.
            if (!IsProactivePlayerLevelAcceptable(bot, candidate))
                continue;

            // Never each other: bots are one team (the pseudo-faction rule
            // already bans it, but the check keeps this honest if it changes).
            if (playerbot::IsManagedRandomBot(candidate))
                continue;

            // Already being fought by as many bots as their group can justify.
            // The bot simply looks elsewhere and falls through to grinding or
            // travel, which is behaviour it already has. The figure is computed
            // on the world thread; with the budget off it is INT32_MAX.
            if (spot.AggroSlotsFree <= 0)
                continue;

            float const distance = bot->GetDistance(candidate);
            if (!nearest || distance < nearestDistance)
            {
                nearest = candidate;
                nearestDistance = distance;
            }
        }
        return nearest;
    }

    // A point on the map is not automatically somewhere a walker can GET to.
    // MovePoint's own path generation silently falls back to a straight line
    // when the navmesh has no route (PATHFIND_SHORTCUT), which is how a bot ends
    // up marching up a cliff face or standing at the bottom of one re-issuing a
    // chase forever. Ask for a real path first and refuse anything that is not
    // one - the same answer a player's own footing would give them.
    // PathGenerator is by far the most expensive thing a bot does - a full A*
    // over the navmesh, and a FAILED path is the worst case of all because it
    // exhausts the search space before it can answer. These run on the same
    // map-update thread that resolves a player's own melee swings, so with a
    // hundred bots on one continent an unbudgeted screen shows up directly as
    // hit-to-damage delay for the human standing there. Paths are therefore
    // drawn from a shared per-second allowance rather than issued on demand.
    std::atomic<uint32> g_PathBudgetTokens{ 0 };
    std::atomic<uint64> g_PathQueriesRun{ 0 };
    std::atomic<uint64> g_PathQueriesDenied{ 0 };

    bool TryConsumePathBudget()
    {
        uint32 expected = g_PathBudgetTokens.load(std::memory_order_relaxed);
        while (expected)
        {
            if (g_PathBudgetTokens.compare_exchange_weak(expected, expected - 1, std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    WalkPathResult CheckWalkPath(Player* bot, float x, float y, float z)
    {
        // Out of allowance means "try again next tick", never "walk without a
        // verified route". Treating an unknown route as reachable is precisely
        // how MovePoint's straight-line fallback sends bots into cliff faces,
        // rivers and terrain. Deferred is kept distinct from unreachable so the
        // caller does not blacklist a perfectly good target merely because the
        // fleet spent this second's query budget.
        if (!TryConsumePathBudget())
        {
            g_PathQueriesDenied.fetch_add(1, std::memory_order_relaxed);
            return WalkPathResult::Deferred;
        }

        g_PathQueriesRun.fetch_add(1, std::memory_order_relaxed);

        PathGenerator path(bot);
        if (!path.CalculatePath(x, y, z, false))
            return WalkPathResult::Unreachable;

        PathType const type = path.GetPathType();
        if (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_FARFROMPOLY | PATHFIND_NOT_USING_PATH))
            return WalkPathResult::Unreachable;
        return (type & PATHFIND_NORMAL) != 0 ? WalkPathResult::Reachable : WalkPathResult::Unreachable;
    }

    WalkPathResult CheckWalkPath(Player* bot, Position const& destination)
    {
        return CheckWalkPath(bot, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    }

    // A wander/patrol destination the bot can actually walk to. Later tries pull
    // in closer, so a bot hemmed in by terrain still shuffles somewhere legal
    // instead of freezing or scaling the scenery.
    bool PickWalkableNearPosition(Player* bot, float radius, Position& destination)
    {
        for (uint8 attempt = 0; attempt < 5; ++attempt)
        {
            float const tryRadius = radius * (1.0f - float(attempt) * 0.18f);
            Position candidate = bot->GetRandomNearPosition(tryRadius);
            bot->UpdateAllowedPositionZ(candidate.m_positionX, candidate.m_positionY, candidate.m_positionZ);
            // Random patrols have no objective worth entering water for. Keep
            // their endpoints dry; actual combat, quest and travel paths may still
            // cross water when the navmesh says that route is legitimate.
            if (bot->GetMap()->IsInWater(PHASEMASK_NORMAL, candidate.m_positionX,
                candidate.m_positionY, candidate.m_positionZ))
                continue;
            WalkPathResult const pathResult = CheckWalkPath(bot, candidate);
            if (pathResult == WalkPathResult::Reachable)
            {
                destination = candidate;
                return true;
            }
            if (pathResult == WalkPathResult::Deferred)
                return false;
        }
        return false;
    }

    // The nearest grind prospect the bot can actually reach on foot. Each
    // unreachable one is listed as a recent bad target, so the next pass offers
    // the next-nearest rather than the same mob across the same canyon.
    Creature* FindWalkableGrindProspect(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        // Each attempt re-runs a 180y grid sweep as well as a path query, so two
        // is the most this is worth: a third pass costs more map-thread time than
        // finding a slightly better prospect is worth.
        for (uint8 attempt = 0; attempt < 2; ++attempt)
        {
            Creature* prospect = FindGrindProspect(bot, state, cfg);
            if (!prospect)
                return nullptr;
            WalkPathResult const pathResult = CheckWalkPath(bot, prospect->GetPosition());
            if (pathResult == WalkPathResult::Reachable)
                return prospect;
            if (pathResult == WalkPathResult::Deferred)
                return nullptr;
            MarkRecentBadTarget(state, prospect->GetGUID());
        }
        return nullptr;
    }

    // Begin a walked journey toward a destination on the bot's current map.
    // Called from world-thread executors; the map-thread fast tick advances it.
    void StartWalkedJourney(PveBotState& state, uint16 mapId, float x, float y, float z, uint8 fallbackKind, float distance)
    {
        state.journeyActive = true;
        state.journeyFallbackKind = fallbackKind;
        state.journeyMapId = mapId;
        state.journeyX = x;
        state.journeyY = y;
        state.journeyZ = z;
        // Generous deadline: walking speed with detours plus fights on the way.
        state.journeyUntil = PveClock::now() + std::chrono::seconds(uint32(distance / 4.0f) + 120);
        state.nextJourneyStepAt = {};
        state.journeyStepValid = false;
        state.journeyProgressAt = PveClock::now();
        state.journeyProgressX = 0.0f;
        state.journeyProgressY = 0.0f;
    }

    void CancelJourneyWithFallback(Player* bot, PveBotState& state)
    {
        uint8 const fallbackKind = state.journeyFallbackKind;
        state.journeyActive = false;
        state.journeyFallbackKind = 0;
        // The executors' walk branches honor this: without it the deterministic
        // nearest-destination pick re-walks into the same unreachable wall
        // forever and the teleport fallback is never reached.
        state.walkFallbackUntil = PveClock::now() + std::chrono::minutes(10);

        // The walk failed (timeout or stuck); the old teleport still delivers.
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        if (fallbackKind == 3)
            g_PendingClassQuestTravels.insert(bot->GetGUID().GetRawValue());
        else if (fallbackKind == 1)
            g_PendingGrindRelocations.insert(bot->GetGUID().GetRawValue());
        else if (fallbackKind == 2)
            g_PendingSupplyRuns.insert(bot->GetGUID().GetRawValue());
    }

    // Returns true while the journey owns this tick.
    bool AdvanceWalkedJourney(Player* bot, PveBotState& state)
    {
        if (!state.journeyActive)
            return false;

        PveTimePoint const now = PveClock::now();

        // A wounded traveler may sit and eat; the trek resumes after. Keep the
        // stuck detector quiet meanwhile.
        if (IsRestingNow(bot, state))
        {
            state.journeyProgressAt = now;
            return true;
        }
        if (bot->GetMapId() != state.journeyMapId || now > state.journeyUntil)
        {
            CancelJourneyWithFallback(bot, state);
            return false;
        }

        float const dx = bot->GetPositionX() - state.journeyX;
        float const dy = bot->GetPositionY() - state.journeyY;
        if (dx * dx + dy * dy < 20.0f * 20.0f)
        {
            state.journeyActive = false;
            state.journeyFallbackKind = 0;
            TC_LOG_INFO("playerbots.pve", "Bot {} finished its walked journey.", bot->GetName());
            return false;
        }

        // Stuck detection: no ground covered in 12s despite trying.
        float const progressDx = bot->GetPositionX() - state.journeyProgressX;
        float const progressDy = bot->GetPositionY() - state.journeyProgressY;
        if (progressDx * progressDx + progressDy * progressDy > 4.0f)
        {
            state.journeyProgressX = bot->GetPositionX();
            state.journeyProgressY = bot->GetPositionY();
            state.journeyProgressAt = now;
        }
        else if (now - state.journeyProgressAt > std::chrono::seconds(12))
        {
            CancelJourneyWithFallback(bot, state);
            return false;
        }

        // Hand over to the next hop while the current one is still running: a
        // spline issued from a bot already in motion continues it, where one
        // issued after it has stopped is a visible halt every sixty yards.
        //
        // Bounded to the tail of the hop on purpose. Re-issuing MovePoint every
        // tick restarts the spline from scratch and reads as a bot standing
        // still - the trap the taming path documents - so this fires at most
        // once per hop, when the bot is nearly on top of the step it was sent to.
        bool const nearHopEnd = state.journeyStepValid && bot->isMoving() &&
            bot->GetExactDist2d(state.journeyStepX, state.journeyStepY) <= 12.0f;

        if (now >= state.nextJourneyStepAt && (!bot->isMoving() || nearHopEnd))
        {
            playerbot::PvpClassActions::PrepareForExplicitMovement(bot);

            // Step in bounded hops with a ground-snapped Z instead of aiming
            // MovePoint at the far destination: crossing unloaded mmap tiles
            // degrades the generated path to a straight-line shortcut, and the
            // spline then drags the bot through hills and under the terrain.
            float const totalDx = state.journeyX - bot->GetPositionX();
            float const totalDy = state.journeyY - bot->GetPositionY();
            float const totalDistance = std::sqrt(totalDx * totalDx + totalDy * totalDy);
            float stepX = state.journeyX, stepY = state.journeyY, stepZ = state.journeyZ;
            if (totalDistance > 60.0f)
            {
                // Prefer hop points on dry ground: a straight-line waypoint
                // dropped into water turns the whole leg into a slow surface
                // swim even when the mesh has a walkable route around it. Only
                // a genuinely unavoidable crossing gets swum.
                float const dirX = totalDx / totalDistance;
                float const dirY = totalDy / totalDistance;
                bool found = false;
                for (float hop : { 60.0f, 40.0f, 25.0f })
                {
                    float const tryX = bot->GetPositionX() + dirX * hop;
                    float const tryY = bot->GetPositionY() + dirY * hop;
                    float tryZ = bot->GetPositionZ();
                    bot->UpdateAllowedPositionZ(tryX, tryY, tryZ);
                    if (!bot->GetMap()->IsInWater(PHASEMASK_NORMAL, tryX, tryY, tryZ))
                    {
                        stepX = tryX;
                        stepY = tryY;
                        stepZ = tryZ;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    stepX = bot->GetPositionX() + dirX * 60.0f;
                    stepY = bot->GetPositionY() + dirY * 60.0f;
                    stepZ = bot->GetPositionZ();
                    bot->UpdateAllowedPositionZ(stepX, stepY, stepZ);
                }
            }

            // The stabilization pass toggles the swim flag from terrain state,
            // but it can lag a hop: a spline launched with a stale SWIMMING
            // flag runs its entire land leg at swim speed.
            if (bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) && !bot->IsInWater())
                bot->SetSwim(false);

            WalkPathResult const pathResult = CheckWalkPath(bot, stepX, stepY, stepZ);
            // Deferred is the fleet path-query budget saying 'not this tick', not
            // a failure. Returning WITHOUT charging the throttle is the point:
            // charging it up front meant a deferral cost two seconds of standing
            // still instead of a single 250ms tick, which is most of what the
            // stutter actually was.
            if (pathResult == WalkPathResult::Deferred)
                return true;
            if (pathResult == WalkPathResult::Unreachable)
            {
                CancelJourneyWithFallback(bot, state);
                return false;
            }

            bot->GetMotionMaster()->MovePoint(0, Position(stepX, stepY, stepZ), true);
            state.nextJourneyStepAt = now + std::chrono::seconds(2);
            state.journeyStepValid = true;
            state.journeyStepX = stepX;
            state.journeyStepY = stepY;
        }

        return true;
    }

    // Death chests are created by our own code, which already knows exactly where
    // each one is - so asking the grid to find them again was work done to learn
    // something we had just been told. Chests register their position on summon;
    // this is a pass over a handful of records instead of a proximity query on the
    // map update thread, and it runs on every bot's chest cadence.
    //
    // A guid that no longer resolves has been looted or despawned, so it is
    // dropped and the next nearest tried. The only way that misfires is a chest on
    // a grid this bot cannot see - which it could not have walked to anyway.
    GameObject* FindRegisteredDeathChest(Player* bot, uint32 entry, float maxDistance)
    {
        if (!entry)
            return nullptr;

        for (int attempt = 0; attempt < 3; ++attempt)
        {
            ObjectGuid guid;
            if (!CustomLootChests::FindNearestChest(entry, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), maxDistance, guid))
                return nullptr;

            if (GameObject* chest = ObjectAccessor::GetGameObject(*bot, guid))
                return chest;

            CustomLootChests::ForgetChest(guid);
        }

        return nullptr;
    }

    void MoveTowardThrottled(Player* bot, Position const& destination)
    {
        if (bot->isMoving())
            return;

        // MovePoint falls back to a straight spline when mmap cannot produce a
        // route. Refuse that fallback here as well as in grind target selection;
        // a deferred fleet budget simply leaves the bot in place until the next
        // tick instead of turning an unverified route into a cliff crossing.
        if (CheckWalkPath(bot, destination) != WalkPathResult::Reachable)
            return;

        playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
        // A stale SWIMMING flag from a just-left pond would run the whole
        // spline at swim speed.
        if (bot->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING) && !bot->IsInWater())
            bot->SetSwim(false);
        bot->GetMotionMaster()->MovePoint(0, destination, true);
    }

    template<typename Fn>
    void ForEachBagItem(Player* bot, Fn&& fn) // fn(Item*, bag, slot)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item, INVENTORY_SLOT_BAG_0, slot);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bot->GetItemByPos(bagSlot, uint8(slot)))
                        fn(item, bagSlot, uint8(slot));
    }

    uint32 CountFreeBagSlots(Player* bot)
    {
        uint32 freeSlots = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++freeSlots;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                freeSlots += bag->GetFreeSlots();

        return freeSlots;
    }

    bool AnyEquippedItemBelowDurabilityPct(Player* bot, uint32 thresholdPct)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (maxDurability && item->GetUInt32Value(ITEM_FIELD_DURABILITY) * 100 < maxDurability * thresholdPct)
                return true;
        }

        return false;
    }

    bool HasBrokenEquippedItem(Player* bot)
    {
        if (!bot)
            return false;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (maxDurability && item->GetUInt32Value(ITEM_FIELD_DURABILITY) == 0)
                return true;
        }

        return false;
    }

    // A broken equipped item is not ordinary wear: at zero durability the core
    // removes its item mods and weapon-dependent abilities can hard-fail with
    // SPELL_FAILED_EQUIPPED_ITEM_CLASS. Repair broken equipped pieces
    // individually and immediately while out of combat so an unrelated repair
    // bill, quest errand, or fast-tick chain pull cannot strand the bot disarmed.
    bool RepairBrokenEquippedItems(Player* bot)
    {
        if (!bot || bot->IsInCombat())
            return false;

        bool repairedAny = false;
        bool foundBroken = false;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (!maxDurability || item->GetUInt32Value(ITEM_FIELD_DURABILITY) != 0)
                continue;

            foundBroken = true;
            uint32 const itemEntry = item->GetEntry();
            uint64 const moneyBefore = bot->GetMoney();

            // Paid repair, but only for this broken equipped piece. This avoids
            // DurabilityRepairAll's all-or-nothing affordability check across
            // every damaged item in bags/equipment.
            bot->DurabilityRepair(item->GetPos(), true, 1.0f);

            if (item->GetUInt32Value(ITEM_FIELD_DURABILITY) > 0)
            {
                repairedAny = true;
                TC_LOG_INFO("playerbots.pve",
                    "Bot {} emergency-repaired broken equipped item {} in slot {} (money {} -> {}).",
                    bot->GetName(), itemEntry, uint32(slot), moneyBefore, bot->GetMoney());
            }
            else
            {
                TC_LOG_WARN("playerbots.pve",
                    "Bot {} could not emergency-repair broken equipped item {} in slot {} (money {}).",
                    bot->GetName(), itemEntry, uint32(slot), bot->GetMoney());
            }
        }

        return foundBroken && repairedAny;
    }

    Item* FindBestConsumable(Player* bot, bool drink)
    {
        Item* best = nullptr;
        uint32 bestLevel = 0;
        ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (drink ? !IsDrinkTemplate(proto) : !IsFoodTemplate(proto))
                return;

            if (proto->RequiredLevel > bot->GetLevel())
                return;

            if (!best || proto->RequiredLevel >= bestLevel)
            {
                best = item;
                bestLevel = proto->RequiredLevel;
            }
        });
        return best;
    }

    // Vendor-sold food and drink, sorted by required level descending, so the
    // first entry at or below a level is the best that level can use.
    //
    // Built from npc_vendor on purpose. A vendor-sold consumable is by
    // construction ordinary, obtainable and unbound, which keeps quest items,
    // conjured food and developer scaffolding out of the pool without needing a
    // second filter that tries to describe all of them.
    std::mutex g_RationLock;
    bool g_RationsBuilt = false;
    std::vector<std::pair<uint32, uint32>> g_RationFood;    // requiredLevel, itemId
    std::vector<std::pair<uint32, uint32>> g_RationDrink;

    void BuildRationPoolOnce()
    {
        std::lock_guard<std::mutex> guard(g_RationLock);
        if (g_RationsBuilt)
            return;
        g_RationsBuilt = true;

        QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor");
        if (!result)
            return;

        do
        {
            uint32 const itemId = (*result)[0].GetUInt32();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto || proto->RequiredLevel > 60)
                continue;
            if (IsFoodTemplate(proto))
                g_RationFood.push_back({ proto->RequiredLevel, itemId });
            else if (IsDrinkTemplate(proto))
                g_RationDrink.push_back({ proto->RequiredLevel, itemId });
        } while (result->NextRow());

        auto byLevelDesc = [](auto const& l, auto const& r) { return l.first > r.first; };
        std::sort(g_RationFood.begin(), g_RationFood.end(), byLevelDesc);
        std::sort(g_RationDrink.begin(), g_RationDrink.end(), byLevelDesc);

        TC_LOG_INFO("playerbots.pve", "Ration pool: {} foods and {} drinks sold by vendors.",
            uint32(g_RationFood.size()), uint32(g_RationDrink.size()));
    }

    uint32 BestRationForLevel(std::vector<std::pair<uint32, uint32>> const& pool, uint8 level)
    {
        std::lock_guard<std::mutex> guard(g_RationLock);
        for (auto const& [requiredLevel, itemId] : pool)
            if (requiredLevel <= level)
                return itemId;
        return 0;
    }

    // The required level of the best ration this level can drink or eat, or 0
    // when the pool has nothing for it.
    uint32 BestRationTierForLevel(std::vector<std::pair<uint32, uint32>> const& pool, uint8 level)
    {
        std::lock_guard<std::mutex> guard(g_RationLock);
        for (auto const& [requiredLevel, itemId] : pool)
            if (requiredLevel <= level)
                return requiredLevel;
        return 0;
    }

    // Vendor rations come in tiers ten levels apart - water at required level
    // 1, 5, 15, 25, 35, 45, each restoring roughly half again as much as the
    // one below. Anything two tiers or more below what the bot can now drink is
    // dead weight, and worse than dead weight: see CountConsumableUnits.
    constexpr uint32 RATION_OBSOLETE_LEVEL_GAP = 10;

    bool IsObsoleteRation(ItemTemplate const* proto, uint32 bestTier)
    {
        return proto && bestTier > RATION_OBSOLETE_LEVEL_GAP &&
            proto->RequiredLevel + RATION_OBSOLETE_LEVEL_GAP < bestTier;
    }

    uint32 CountConsumableUnits(Player* bot, bool drink)
    {
        // Obsolete stock does NOT count as being supplied.
        //
        // This is why a level 38 shaman was still drinking level 1 water: it
        // counted every drink in its bags, a stack of Refreshing Spring Water
        // read as "stocked", the vendor visit bought nothing, and FindBestConsumable
        // then had nothing better to reach for. Counting only rations that are
        // still worth carrying makes the bot restock at the next vendor.
        uint32 const bestTier = BestRationTierForLevel(drink ? g_RationDrink : g_RationFood,
            bot->GetLevel());

        uint32 units = 0;
        ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (drink ? IsDrinkTemplate(proto) : IsFoodTemplate(proto))
                if (!IsObsoleteRation(proto, bestTier))
                    units += item->GetCount();
        });
        return units;
    }

    // Throw out rations the bot has outgrown, on the level-up that outgrew them.
    //
    // Two tiers of headroom before anything is dropped, so a bot is never left
    // with nothing to drink: at level 38 the best water is required level 35,
    // and this discards only tiers below 25 - the level 1, 5 and 15 water -
    // while keeping Sweet Nectar and Moonberry Juice. Bags free up, the stock
    // count falls, and the next vendor visit buys the good stuff.
    //
    // Positions are collected before anything is destroyed: DestroyItem shifts
    // what is in a bag slot, so deciding and acting in one pass over live
    // containers is how items get missed.
    uint32 DiscardOutclassedRations(Player* bot)
    {
        if (!bot)
            return 0;

        BuildRationPoolOnce();

        uint32 const bestDrinkTier = BestRationTierForLevel(g_RationDrink, bot->GetLevel());
        uint32 const bestFoodTier = BestRationTierForLevel(g_RationFood, bot->GetLevel());

        std::vector<std::pair<uint8, uint8>> obsolete;
        ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                return;
            bool const drink = IsDrinkTemplate(proto);
            if (!drink && !IsFoodTemplate(proto))
                return;
            if (IsObsoleteRation(proto, drink ? bestDrinkTier : bestFoodTier))
                obsolete.push_back({ bag, slot });
        });

        for (auto const& [bag, slot] : obsolete)
            bot->DestroyItem(bag, slot, true);

        return uint32(obsolete.size());
    }

    // Ammo the bot's ranged weapon feeds on, or 0 when none is needed.
    uint32 RequiredAmmoSubclass(Player const* bot)
    {
        Item* ranged = bot->GetWeaponForAttack(RANGED_ATTACK, true);
        if (!ranged || !ranged->GetTemplate())
            return 0;

        switch (ranged->GetTemplate()->SubClass)
        {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ITEM_SUBCLASS_ARROW;
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ITEM_SUBCLASS_BULLET;
        default:
            return 0;
        }
    }

    // True when this vendor can actually satisfy the bot's SUPPLY need - a
    // mount vendor next to the grind spot is nearest but useless, and picking
    // it pinballs the errand loop between merchants forever.
    bool VendorStocksNeededSupplies(Player* bot, Creature* vendor)
    {
        VendorItemData const* vendorItems = vendor->GetVendorItems();
        if (!vendorItems)
            return false;

        // What the bot is actually SHORT of, not merely what it consumes. This
        // used to accept any vendor selling food OR water OR ammo, so a mana user
        // carrying a hundred loaves and no water kept walking to food-only
        // vendors, buying nothing it needed, and arriving thirsty forever.
        bool const needsFood = CountConsumableUnits(bot, false) == 0 && !ConjureSpellId(bot, false);
        bool const needsDrink = UsesMana(bot) &&
            CountConsumableUnits(bot, true) == 0 && !ConjureSpellId(bot, true);
        uint32 const ammoSubclass = RequiredAmmoSubclass(bot);
        bool const needsAmmo = ammoSubclass != 0 &&
            (!bot->GetUInt32Value(PLAYER_AMMO_ID) || bot->GetItemCount(bot->GetUInt32Value(PLAYER_AMMO_ID)) == 0);

        if (!needsFood && !needsDrink && !needsAmmo)
            return false;

        for (uint8 slot = 0; slot < vendorItems->GetItemCount(); ++slot)
        {
            VendorItem const* vendorItem = vendorItems->GetItem(slot);
            if (!vendorItem || vendorItem->ExtendedCost)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
            if (!proto || proto->RequiredLevel > bot->GetLevel())
                continue;

            if (needsFood && IsFoodTemplate(proto))
                return true;
            if (needsDrink && IsDrinkTemplate(proto))
                return true;
            if (needsAmmo && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == ammoSubclass)
                return true;
        }
        return false;
    }

    // Buy the best level-appropriate food (and water for mana users, and ammo
    // for ranged users) this vendor sells. Money is checked by the purchase
    // itself; a broke bot just fails quietly and grinds more coin.
    void TryBuySupplies(Player* bot, Creature* vendor)
    {
        VendorItemData const* vendorItems = vendor->GetVendorItems();
        if (!vendorItems)
            return;

        bool const wantsDrink = UsesMana(bot);
        uint32 const ammoSubclass = RequiredAmmoSubclass(bot);

        bool hasEmptyBagSlot = false;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (!bot->GetBagByPos(bagSlot))
                hasEmptyBagSlot = true;

        int32 bestFoodSlot = -1, bestDrinkSlot = -1, bestAmmoSlot = -1, bestBagSlot = -1;
        ItemTemplate const* bestFood = nullptr;
        ItemTemplate const* bestDrink = nullptr;
        ItemTemplate const* bestAmmo = nullptr;
        ItemTemplate const* bestBag = nullptr;
        for (uint8 slot = 0; slot < vendorItems->GetItemCount(); ++slot)
        {
            VendorItem const* vendorItem = vendorItems->GetItem(slot);
            if (!vendorItem || vendorItem->ExtendedCost)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
            if (!proto || proto->RequiredLevel > bot->GetLevel())
                continue;

            if (IsFoodTemplate(proto) && (!bestFood || proto->RequiredLevel >= bestFood->RequiredLevel))
            {
                bestFood = proto;
                bestFoodSlot = slot;
            }
            else if (wantsDrink && IsDrinkTemplate(proto) && (!bestDrink || proto->RequiredLevel >= bestDrink->RequiredLevel))
            {
                bestDrink = proto;
                bestDrinkSlot = slot;
            }
            else if (ammoSubclass && proto->Class == ITEM_CLASS_PROJECTILE && proto->SubClass == ammoSubclass &&
                (!bestAmmo || proto->RequiredLevel >= bestAmmo->RequiredLevel))
            {
                bestAmmo = proto;
                bestAmmoSlot = slot;
            }
            else if (hasEmptyBagSlot && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER &&
                (!bestBag || proto->ContainerSlots > bestBag->ContainerSlots))
            {
                bestBag = proto;
                bestBagSlot = slot;
            }
        }

        auto buyUnits = [&](int32 slot, ItemTemplate const* proto, uint32 desiredUnits)
        {
            if (slot < 0 || !proto)
                return;

            // BuyItemFromVendorSlot's count is a number of vendor LOTS, and each
            // lot is BuyCount items - it charges and delivers BuyCount * count.
            // Passing a raw unit count therefore multiplied every purchase by
            // BuyCount: vendor consumables here are sold five to a lot, so a
            // twenty unit top-up actually bought ONE HUNDRED items, five full
            // stacks. That is how a bot ended up carrying five stacks of food and
            // then had no room left to buy any water at all - the water purchase
            // failed on a full pack, silently, and it walked back to the same
            // vendor every couple of minutes forever.
            uint32 const perLot = std::max<uint32>(1, proto->BuyCount);
            uint32 const lots = std::max<uint32>(1, (desiredUnits + perLot - 1) / perLot);
            bot->BuyItemFromVendorSlot(vendor->GetGUID(), uint32(slot), proto->ItemId,
                uint8(std::min<uint32>(lots, 250)), NULL_BAG, NULL_SLOT);
        };

        // A bot that can conjure its own food or water never buys that kind.
        if (bestFood && CountConsumableUnits(bot, false) < 10 && !ConjureSpellId(bot, false))
            buyUnits(bestFoodSlot, bestFood, 20);
        if (bestDrink && CountConsumableUnits(bot, true) < 10 && !ConjureSpellId(bot, true))
            buyUnits(bestDrinkSlot, bestDrink, 20);
        if (bestAmmo)
        {
            // Count what is actually in the pack, not just the loaded type. Asking
            // only about PLAYER_AMMO_ID bought another two hundred arrows at every
            // vendor whenever the field was unset or pointed at a different arrow,
            // which is how a hunter ended up with six hundred arrows in four
            // stacks and no room for anything else.
            uint32 const carried = bot->GetItemCount(bestAmmo->ItemId) +
                (bot->GetUInt32Value(PLAYER_AMMO_ID) ? bot->GetItemCount(bot->GetUInt32Value(PLAYER_AMMO_ID)) : 0);
            if (carried < 200)
                buyUnits(bestAmmoSlot, bestAmmo, 200);
            if (!bot->GetUInt32Value(PLAYER_AMMO_ID) && bot->GetItemCount(bestAmmo->ItemId))
                bot->SetAmmo(bestAmmo->ItemId);
        }
        // One bag per visit fills empty bag slots; the equip pass mounts it.
        if (bestBag)
            bot->BuyItemFromVendorSlot(vendor->GetGUID(), uint32(bestBagSlot), bestBag->ItemId, 1, NULL_BAG, NULL_SLOT);
    }

    bool IsQuestRequiredItem(Player* bot, uint32 itemId)
    {
        for (auto const& [questId, status] : bot->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            for (uint8 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
                if (quest->RequiredItemId[index] == itemId)
                    return true;
        }

        return false;
    }

    // A container the bot has no use for. A quiver is spare once one is already
    // worn - a second holds nothing extra. A bag is spare when there is no free
    // bag slot left AND it is no roomier than the smallest one already worn, so
    // genuine upgrades survive: the equip pass mounts those every fifteen
    // seconds. Only empty ones qualify; a bag with contents is never touched.
    // A bag can only be UNEQUIPPED when it is empty, which creates a genuine
    // deadlock for the bot that most wants an upgrade: every bag slot taken,
    // every bag full, so the better bag it just bought sits in the backpack
    // forever, occupying one more slot than it started with and being retried on
    // every pass. These three helpers exist to make sure that never happens - the
    // buyer refuses a bag it could not equip, and the equipper clears the way
    // before it tries.

    // Free slots ANYWHERE except inside one specific bag. Excluding it matters:
    // counting its own free space would let the bot "make room" in the very bag
    // it is about to empty.
    uint32 CountFreeSlotsOutsideBag(Player* bot, uint8 excludedBagSlot)
    {
        uint32 freeSlots = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++freeSlots;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            if (bagSlot == excludedBagSlot)
                continue;
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                freeSlots += bag->GetFreeSlots();
        }

        return freeSlots;
    }

    uint32 CountUsedSlotsInBag(Player* bot, Bag* bag)
    {
        if (!bag)
            return 0;

        uint32 used = 0;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (bot->GetItemByPos(bag->GetSlot(), uint8(slot)))
                ++used;

        return used;
    }

    // Could this bag's contents live somewhere else? Cheap, and answerable before
    // a single item is touched, which is what lets the buyer decline up front.
    bool CanRehomeBagContents(Player* bot, Bag* bag)
    {
        if (!bag)
            return true;

        return CountUsedSlotsInBag(bot, bag) <= CountFreeSlotsOutsideBag(bot, bag->GetSlot());
    }

    // Actually move the contents out. Space is PROVEN with CanStoreItem before the
    // item is detached, exactly as the mail collector does it, so there is never a
    // moment where an item exists nowhere. A destination inside the bag being
    // emptied is never considered. Returns true only if the bag ends up empty; a
    // partial move is harmless, since items simply sit in different slots.
    bool TryEmptyBagForSwap(Player* bot, Bag* bag)
    {
        if (!bag)
            return true;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            Item* item = bot->GetItemByPos(bag->GetSlot(), uint8(slot));
            if (!item)
                continue;

            ItemPosCountVec destination;
            InventoryResult result = EQUIP_ERR_INVENTORY_FULL;

            // The backpack first, then every OTHER equipped bag.
            for (uint8 targetBag = INVENTORY_SLOT_BAG_START - 1; targetBag < INVENTORY_SLOT_BAG_END; ++targetBag)
            {
                uint8 const container = targetBag < INVENTORY_SLOT_BAG_START ? uint8(INVENTORY_SLOT_BAG_0) : targetBag;
                if (container == bag->GetSlot())
                    continue;

                destination.clear();
                result = bot->CanStoreItem(container, NULL_SLOT, destination, item, false);
                if (result == EQUIP_ERR_OK)
                    break;
            }

            if (result != EQUIP_ERR_OK)
                return false;

            bot->RemoveItem(bag->GetSlot(), uint8(slot), true);
            bot->StoreItem(destination, item, true);
        }

        return true;
    }

    // Should this one-hander go in the OFF hand rather than the main?
    //
    // FindEquipSlot offers a one-handed weapon to the main hand first and, with
    // both hands full, returns the main hand unconditionally. So a bot whose off
    // hand holds a caster trinket judges every dagger it finds against its MAIN
    // hand, rejects the ones that are not upgrades there, and keeps the dead slot
    // indefinitely - the off hand is never even considered.
    //
    // When the off hand holds something that is not a weapon at all and the bot
    // can dual wield, that is plainly the slot that wants filling.
    bool ShouldRedirectToOffHand(Player* bot, ItemTemplate const* candidate, uint16 dest)
    {
        if (!candidate || candidate->InventoryType != INVTYPE_WEAPON || !bot->CanDualWield())
            return false;

        if (uint8(dest & 255) != EQUIPMENT_SLOT_MAINHAND)
            return false;

        // Only when the main hand is actually occupied - an empty main hand is
        // the right answer for a lone weapon.
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            return false;

        Item const* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (!offHand)
            return true; // a free off hand beats contesting the main hand

        ItemTemplate const* offProto = offHand->GetTemplate();
        return offProto && offProto->Class != ITEM_CLASS_WEAPON;
    }

    // Which bag slot should a container upgrade actually target?
    //
    // NOT the one CanEquipItem picks. Player::FindEquipSlot searches for a free
    // bag slot and, finding none, falls through to "return the first appropriate
    // from used" - which is unconditionally slot 19. So a bot with a big bag in
    // slot 19 and a tiny one in slot 22 would forever compare its new bag against
    // the BIG one, decline the upgrade, and never once consider the small bag it
    // actually wanted to replace.
    //
    // Choose deliberately instead: a free slot if there is one, otherwise the
    // smallest container of the SAME KIND. Kind matters because a quiver holds
    // only ammunition - sizing a new backpack against a worn quiver, or the
    // reverse, compares two things that cannot substitute for each other.
    bool SelectContainerUpgradeSlot(Player* bot, ItemTemplate const* candidate, uint16& dest)
    {
        bool const wantQuiver = candidate->Class == ITEM_CLASS_QUIVER;
        bool haveFree = false;
        uint8 freeSlot = 0;
        bool haveSmallest = false;
        uint8 smallestSlot = 0;
        uint32 smallestSize = 0;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* worn = bot->GetBagByPos(bagSlot);
            if (!worn)
            {
                if (!haveFree)
                {
                    haveFree = true;
                    freeSlot = bagSlot;
                }
                continue;
            }

            ItemTemplate const* wornProto = worn->GetTemplate();
            if (!wornProto)
                continue;

            if ((wornProto->Class == ITEM_CLASS_QUIVER) != wantQuiver)
                continue;

            uint32 const size = worn->GetBagSize();
            if (!haveSmallest || size < smallestSize)
            {
                haveSmallest = true;
                smallestSize = size;
                smallestSlot = bagSlot;
            }
        }

        // An empty slot is always the best answer: nothing has to come off.
        if (haveFree)
        {
            dest = uint16((uint16(INVENTORY_SLOT_BAG_0) << 8) | freeSlot);
            return true;
        }

        if (!haveSmallest)
            return false;

        dest = uint16((uint16(INVENTORY_SLOT_BAG_0) << 8) | smallestSlot);
        return true;
    }

    bool IsSpareContainer(Player* bot, Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || (proto->Class != ITEM_CLASS_CONTAINER && proto->Class != ITEM_CLASS_QUIVER))
            return false;

        if (item->IsNotEmptyBag())
            return false;

        bool freeBagSlot = false;
        bool quiverWorn = false;
        uint32 smallestWorn = 0;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* worn = bot->GetBagByPos(bagSlot);
            if (!worn)
            {
                freeBagSlot = true;
                continue;
            }

            if (ItemTemplate const* wornProto = worn->GetTemplate())
                if (wornProto->Class == ITEM_CLASS_QUIVER)
                    quiverWorn = true;

            uint32 const size = worn->GetBagSize();
            if (!smallestWorn || size < smallestWorn)
                smallestWorn = size;
        }

        if (proto->Class == ITEM_CLASS_QUIVER)
            return quiverWorn;

        return !freeBagSlot && proto->ContainerSlots <= smallestWorn;
    }

    uint32 SellVendorJunk(Player* bot)
    {
        uint32 soldCount = 0;
        ForEachBagItem(bot, [&](Item* item, uint8 bag, uint8 slot)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || !proto->SellPrice)
                return;

            // Greys always.
            bool sellable = proto->Quality == ITEM_QUALITY_POOR;

            // Gathered trade goods (bots don't craft) - but these are exactly the
            // reagents the auction house wants, and a vendor pays a fraction of
            // what they fetch there. So they only go to the merchant when the
            // auction route is closed to them: selling switched off, the item not
            // listable at all (soulbound gathers), or the pack too full to afford
            // waiting for the next listing run.
            if (!sellable && playerbot::PveManager::GetConfig().professionsEnabled &&
                proto->Class == ITEM_CLASS_TRADE_GOODS && !IsQuestRequiredItem(bot, proto->ItemId))
                sellable = !playerbot::PveManager::GetConfig().auctionSellEnabled ||
                CountFreeBagSlots(bot) < 2 || !IsAuctionableSurplus(bot, item);

            // Spare bags and quivers. A bag is worth real money to another
            // player and a pittance to a merchant, so these go to the house by
            // preference and only to the vendor when that route is closed -
            // selling disabled, the bag not listable, or the pack so tight that
            // waiting for the next listing run would cost a drop.
            if (!sellable && IsSpareContainer(bot, item))
                sellable = !playerbot::PveManager::GetConfig().auctionSellEnabled ||
                CountFreeBagSlots(bot) < 2 || !IsAuctionableSurplus(bot, item);

            // Spare white gear, always - it is barred from the auction house, so
            // the merchant is the only place it can go. Waiting for a full pack
            // just meant it accumulated. Never the food, water, ammunition or
            // bags that keep the bot running, and never something it would rather
            // be wearing.
            if (!sellable && proto->Quality == ITEM_QUALITY_NORMAL &&
                (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
                !IsQuestRequiredItem(bot, proto->ItemId))
            {
                uint16 dest = 0;
                bool wantsToWear = false;
                if (bot->CanEquipItem(NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
                {
                    ItemTemplate const* equipped = nullptr;
                    if (Item const* worn = bot->GetItemByPos(dest))
                        equipped = worn->GetTemplate();
                    wantsToWear = IsEquipUpgrade(bot, proto, equipped, uint8(dest & 255));
                }
                sellable = !wantsToWear;
            }

            if (!sellable)
                return;

            bot->ModifyMoney(int32(proto->SellPrice * item->GetCount()));
            bot->DestroyItem(bag, slot, true);
            ++soldCount;
        });
        return soldCount;
    }

    // The kill loop leaves the bot targeting its dead victim for one tick; grab
    // the corpse for looting before the disengage path clears the target.
    void DetectFreshKillForLoot(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        if (!cfg.lootEnabled || !state.pendingLootGuid.IsEmpty())
            return;

        ObjectGuid const targetGuid = bot->GetTarget();
        if (targetGuid.IsEmpty())
            return;

        Creature* corpse = ObjectAccessor::GetCreature(*bot, targetGuid);
        if (!corpse || corpse->IsAlive() || corpse->loot.isLooted())
            return;

        Player* recipient = corpse->GetLootRecipient();
        bool const lootIsOurs = recipient && (recipient == bot ||
            (bot->GetGroup() && recipient->GetGroup() == bot->GetGroup()));
        if (!lootIsOurs)
            return;

        state.pendingLootGuid = targetGuid;
        state.pendingLootUntil = PveClock::now() + std::chrono::seconds(20);
    }

    // Returns true while the bot is still busy walking to the corpse.
    bool ProcessPendingLoot(Player* bot, PveBotState& state, playerbot::PveConfig const& /*cfg*/)
    {
        if (state.pendingLootGuid.IsEmpty())
            return false;

        auto clearPending = [&]() { state.pendingLootGuid = ObjectGuid::Empty; };
        if (PveClock::now() > state.pendingLootUntil)
        {
            clearPending();
            return false;
        }

        Creature* corpse = ObjectAccessor::GetCreature(*bot, state.pendingLootGuid);
        if (!corpse || corpse->IsAlive() || corpse->loot.isLooted())
        {
            clearPending();
            return false;
        }

        if (!bot->IsWithinDistInMap(corpse, INTERACTION_DISTANCE))
        {
            if (state.journeyActive)
                state.journeyProgressAt = PveClock::now();
            MoveTowardThrottled(bot, corpse->GetPosition());
            return true;
        }

        // In reach: hand the actual loot session to the world thread.
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingLootExecutions[bot->GetGUID().GetRawValue()] = state.pendingLootGuid;
        }
        clearPending();
        state.nextEquipCheckAt = PveClock::now() + std::chrono::seconds(3);
        return false;
    }

    // How far a bot will reach for a chest nobody is watching, and how far away
    // the nearest person has to be for "nobody is watching" to hold.
    float RemoteChestRadius()
    {
        static float const yards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.RemoteChestRadius", 40.0f));
        return yards;
    }

    float RemoteChestPrivacyYards()
    {
        static float const yards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.RemoteChestPrivacyYards", 200.0f));
        return yards;
    }

    // Whether a real person is close enough to see this happen.
    //
    // The fleet emptying a chest from forty yards with no cast bar is obviously
    // not something a player does, so it only happens where no player can watch
    // it. Bots do not count as witnesses - they are the ones doing it.
    //
    // DEAD PLAYERS COUNT. A ghost is still a person looking at the world, and
    // running a corpse back past a chest that empties itself is exactly the
    // moment somebody notices. Aliveness is deliberately not tested here.
    bool AnyRealPersonWithin(WorldObject const* of, float yards)
    {
        Map* map = of ? of->FindMap() : nullptr;
        if (!map)
            return false;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || !candidate->IsInWorld() || candidate->IsGameMaster())
                continue;
            if (playerbot::IsManagedRandomBot(candidate))
                continue;
            if (of->IsWithinDistInMap(candidate, yards))
                return true;
        }
        return false;
    }

    // A chest the bot may empty where it stands, without walking onto it and
    // without the Opening channel.
    bool MayLootChestRemotely(Player* bot, GameObject* go)
    {
        if (!bot || !go || RemoteChestRadius() <= 0.0f)
            return false;

        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
            return false;

        if (!go->isSpawned() || go->getLootState() == GO_JUST_DEACTIVATED)
            return false;

        if (!bot->IsWithinDistInMap(go, RemoteChestRadius()))
            return false;

        // A GM standing over the chest is a witness HERE, even though a GM is
        // deliberately not one for the gear-drop rule that shares the helper
        // below. The two ask different questions. That rule asks "is there
        // anybody this loot could belong to", and a GM should not be able to
        // make the fleet shed its gear simply by flying past. This one asks "can
        // anybody SEE this happen", and a GM watching their own cache get
        // vacuumed from forty yards with no cast bar is the single most likely
        // person on the realm to be looking straight at it.
        //
        // It is also how the operator saw this: two bots knelt at a cache with
        // no Opening animation, because a GM does not count and the chest read
        // as unwatched.
        Map* map = go->FindMap();
        if (map)
            for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
            {
                Player* candidate = itr->GetSource();
                if (!candidate || !candidate->IsInWorld() || !candidate->IsGameMaster())
                    continue;
                if (go->IsWithinDistInMap(candidate, RemoteChestPrivacyYards()))
                    return false;
            }

        return !AnyRealPersonWithin(go, RemoteChestPrivacyYards());
    }

    // World thread. Hands every bot standing near an unwatched chest a loot
    // session for it, so the ordinary executor below empties it.
    //
    // Queued rather than looted here so there is exactly one place that knows
    // how to take gold, split it for a group, and store the slots.
    void QueueUnwatchedChests()
    {
        if (RemoteChestRadius() <= 0.0f)
            return;

        std::vector<Player*> bots;
        {
            std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* bot = pair.second;
                if (bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() &&
                    playerbot::IsManagedRandomBot(bot) && CountFreeBagSlots(bot) >= 2)
                    bots.push_back(bot);
            }
        }

        for (Player* bot : bots)
        {
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                if (g_PendingLootExecutions.count(bot->GetGUID().GetRawValue()))
                    continue;   // already has a loot session queued this tick
            }

            std::vector<GameObject*> nearby;
            Trinity::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), RemoteChestRadius());
            Trinity::GameObjectListSearcher<Trinity::GameObjectInRangeCheck> searcher(bot, nearby, check);
            Cell::VisitGridObjects(bot, searcher, RemoteChestRadius());

            for (GameObject* go : nearby)
            {
                if (!MayLootChestRemotely(bot, go))
                    continue;

                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingLootExecutions[bot->GetGUID().GetRawValue()] = go->GetGUID();
                break;   // one chest per bot per pass
            }
        }
    }

    // World thread. Opens the loot session, takes gold (group-split when the
    // kill was group-tagged), stores every slot and releases.
    void ProcessPendingLootExecutions()
    {
        std::unordered_map<uint64, ObjectGuid> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingLootExecutions);
        }

        for (auto const& entry : drained)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(entry.first));
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;

            ObjectGuid const lootGuid = entry.second;
            WorldObject* lootObject = nullptr;
            Loot* loot = nullptr;
            LootType lootType = LOOT_CORPSE;
            GameObject* lootGameObject = nullptr;
            Creature* lootCorpse = nullptr;
            if (lootGuid.IsGameObject())
            {
                lootGameObject = ObjectAccessor::GetGameObject(*bot, lootGuid);
                if (!lootGameObject || !lootGameObject->isSpawned())
                    continue;
                lootObject = lootGameObject;
                loot = &lootGameObject->loot;
                lootType = LOOT_SKINNING;
            }
            else
            {
                lootCorpse = ObjectAccessor::GetCreature(*bot, lootGuid);
                if (!lootCorpse || lootCorpse->IsAlive() || lootCorpse->loot.isLooted())
                    continue;
                lootObject = lootCorpse;
                loot = &lootCorpse->loot;
            }

            // Arm's length, unless this is a chest nobody can see being opened -
            // then the bot takes it from where it stands, with no walk and no
            // Opening channel. QueueUnwatchedChests is what puts those here.
            if (!bot->IsWithinDistInMap(lootObject, INTERACTION_DISTANCE + 2.0f) &&
                !MayLootChestRemotely(bot, lootGameObject))
                continue;

            // Never OPEN a world gameobject the bot has no room to empty.
            //
            // A gameobject's loot is generated once, for whoever opens it first,
            // and quest items in it are filtered by that looter's quest state. So
            // a bot that opens a shared node with a full pack takes nothing,
            // releases, and leaves the object sitting there holding a loot table
            // built for the bot - a player who clicks it afterwards gets an empty
            // window. Corpses are the bot's own kill and are not contested this
            // way, so only gameobjects are gated.
            //
            // A cache we built is exempt: its contents are already fixed and
            // nothing regenerates them, so there is no shared table to spoil -
            // and by the time the executor runs the chest has usually already
            // been opened, so bailing here is what LEAVES it spent and dark
            // rather than what protects it. Take the coin, which needs no slot
            // at all, and store whatever fits.
            bool const ourCache = lootGameObject && CustomLootChests::IsPlayerBuiltChest(lootGuid);
            if (lootGameObject && !ourCache && CountFreeBagSlots(bot) < 2)
                continue;

            // A REGRESSION FENCE, not the cause of anything seen so far.
            //
            // Player::SendLoot treats a gameobject in GO_READY as "this loot has
            // not been generated yet" and regenerates it from the template:
            //
            //     if (lootid) { loot->clear(); ... loot->FillLoot(lootid, ...); }
            //
            // A PlayerChestBuilder chest writes the dead player's gear and gold
            // straight into GameObject::loot and leaves it in GO_READY, so that
            // branch would clear the lot. It does NOT fire today: the realm's
            // cache is a custom row (900002, "Fallen Adventurer's Cache") whose
            // Data1 lootId is 0, and the live server logs it as such on every
            // open. The chest is only ever one Data1 edit away from silently
            // destroying every cache on the realm, though, so the state is
            // corrected here instead of relying on that column staying zero.
            //
            // Moving it to GO_ACTIVATED is what GameObject::Use would have done
            // anyway, which makes the regeneration branch unreachable.
            if (lootGameObject && lootGameObject->getLootState() == GO_READY &&
                CustomLootChests::IsPlayerBuiltChest(lootGuid))
                lootGameObject->SetLootState(GO_ACTIVATED, bot);

            bot->SendLoot(lootGuid, lootType);
            // SendLoot can refuse (permission, despawn race); it releases on its
            // own failure paths, so only proceed while the session is ours.
            if (bot->GetLootGUID() != lootGuid)
                continue;

            if (loot->gold)
            {
                // Mirror the loot-money handler's group split for shared kills.
                Group* group = bot->GetGroup();
                if (group)
                {
                    std::vector<Player*> nearMembers;
                    for (Group::MemberSlot const& slot : group->GetMemberSlots())
                        if (Player* member = ObjectAccessor::GetPlayer(*bot, slot.guid))
                            if (member->IsAtGroupRewardDistance(lootObject))
                                nearMembers.push_back(member);

                    uint32 const share = std::max<uint32>(1, loot->gold / std::max<size_t>(1, nearMembers.size()));
                    for (Player* member : nearMembers)
                        member->ModifyMoney(int32(share));
                    if (nearMembers.empty())
                        bot->ModifyMoney(int32(loot->gold));
                }
                else
                    bot->ModifyMoney(int32(loot->gold));

                loot->gold = 0;
                loot->NotifyMoneyRemoved();
            }

            uint32 const maxSlot = std::min<uint32>(loot->GetMaxSlotInLootFor(bot), 255);
            for (uint32 slot = 0; slot < maxSlot; ++slot)
                bot->StoreLootItem(uint8(slot), loot);

            if (bot->GetLootGUID() == lootGuid)
                bot->GetSession()->DoLootRelease(lootGuid);

            if (playerbot::PveManager::GetConfig().professionsEnabled)
            {
                // Gather nodes grant their skill-up; freshly emptied corpses
                // become skinnable and get skinned in the same visit.
                if (lootGameObject)
                    GrantGatherSkillCredit(bot, lootGameObject);
                else if (lootCorpse)
                    TrySkinCorpse(bot, lootCorpse);
            }
        }
    }

    // Quest choice rewards: take the item that most improves the bot rather than
    // the first one it happens to be able to use. Gear is valued against whatever
    // occupies the slot it would actually land in, through the same scorer the
    // bag-equip and auction passes use, so a bot picks the real upgrade out of the
    // four on offer instead of whichever the quest happens to list first - which is
    // why bots were finishing quests without ever visibly gaining anything.
    //
    // When nothing offered is an upgrade the pick falls back to vendor value, since
    // selling it is what the bot would do with the item anyway.
    uint32 PickQuestRewardIndex(Player* bot, Quest const* quest)
    {
        uint32 bestIndex = 0;
        float bestScore = 0.0f;
        bool haveAny = false;

        for (uint32 index = 0; index < quest->GetRewChoiceItemsCount(); ++index)
        {
            uint32 const entry = quest->RewardChoiceItemId[index];
            if (!entry)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;

            // Vendor value in gold, deliberately on a scale far below any real
            // stat gain so an upgrade can never lose to a merely expensive item.
            float score = float(proto->SellPrice) / 10000.0f;

            if (bot->CanUseItem(proto) == EQUIP_ERR_OK)
            {
                uint8 const slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
                if (slot < INVENTORY_SLOT_BAG_END)
                {
                    uint16 dest = uint16((uint16(INVENTORY_SLOT_BAG_0) << 8) | slot);

                    // Value a bag against the bag it would really replace, and a
                    // one-hander against the hand that actually needs it - the same
                    // two corrections the auction shopper makes, because
                    // FindEquipSlot falls back to the first candidate slot rather
                    // than the one most worth replacing.
                    if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER)
                        SelectContainerUpgradeSlot(bot, proto, dest);
                    else if (ShouldRedirectToOffHand(bot, proto, dest))
                        dest = uint16((uint16(INVENTORY_SLOT_BAG_0) << 8) | EQUIPMENT_SLOT_OFFHAND);

                    ItemTemplate const* incumbent = nullptr;
                    if (Item const* equipped = bot->GetItemByPos(dest))
                        incumbent = equipped->GetTemplate();

                    if (IsEquipUpgrade(bot, proto, incumbent, uint8(dest & 255)))
                    {
                        // Upgrades always outrank sellable rewards; between two
                        // upgrades the margin over the incumbent decides.
                        float const gain = ScoreItemForSpec(bot, proto) -
                            (incumbent ? ScoreItemForSpec(bot, incumbent) : 0.0f);
                        score = 1000.0f + gain;
                    }
                }
            }

            if (!haveAny || score > bestScore)
            {
                bestScore = score;
                bestIndex = index;
                haveAny = true;
            }
        }

        return bestIndex;
    }

    bool IsSingleClassQuest(Quest const* quest)
    {
        if (!quest)
            return false;

        uint32 const classes = quest->GetRequiredClasses();
        return classes && (classes & (classes - 1u)) == 0;
    }

    void AcceptAndTurnInQuestsAt(Player* bot, Creature* giver)
    {
        std::vector<uint32> completedQuests;
        for (auto const& [questId, status] : bot->getQuestStatusMap())
            if (status.Status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                completedQuests.push_back(questId);

        QuestRelationResult const involved = sObjectMgr->GetCreatureQuestInvolvedRelations(giver->GetEntry());
        for (uint32 questId : completedQuests)
        {
            if (!involved.HasQuest(questId))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || IsSingleClassQuest(quest))
                continue;

            uint32 const rewardIndex = PickQuestRewardIndex(bot, quest);
            if (bot->CanRewardQuest(quest, false) && bot->CanRewardQuest(quest, rewardIndex, false))
            {
                bot->RewardQuest(quest, rewardIndex, giver, false);
                TC_LOG_INFO("playerbots.pve", "Bot {} turned in quest {} ({}).",
                    bot->GetName(), questId, quest->GetTitle());
            }
        }

        for (uint32 questId : sObjectMgr->GetCreatureQuestRelations(giver->GetEntry()))
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || IsSingleClassQuest(quest))
                continue;

            if (bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                bot->AddQuestAndCheckCompletion(quest, giver);
                TC_LOG_INFO("playerbots.pve", "Bot {} accepted quest {} ({}).",
                    bot->GetName(), questId, quest->GetTitle());
            }
        }
    }

    struct ErrandNpcCheck
    {
        Player* bot;

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsInEvadeMode() &&
                (creature->IsQuestGiver() || creature->IsVendor()) && creature->IsFriendlyTo(bot);
        }
    };

    void RequestSupplyRunIfDue(Player* bot, PveBotState& state)
    {
        PveTimePoint const now = PveClock::now();
        if (now < state.nextSupplyRunAt)
            return;

        state.nextSupplyRunAt = now + std::chrono::minutes(5);
        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        g_PendingSupplyRuns.insert(bot->GetGUID().GetRawValue());
    }

    void StartErrandIfNeeded(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        if (!cfg.questsEnabled && !cfg.vendorEnabled)
            return;

        // Guardians are zone sentries, not questing characters. They may still
        // loot death caches and use vendors for maintenance, but ordinary quest
        // givers/objects must never take ownership of their movement.
        bool const guardian = GetGuardianZoneId(bot->GetGUID().GetRawValue()) != 0;

        // Gameobject objectives and gather nodes first: they sit inside the
        // grind area, so finishing them costs almost nothing.
        if (!guardian && ((cfg.questsEnabled && BotHasIncompleteQuest(bot)) || cfg.professionsEnabled))
        {
            if (GameObject* questGo = FindNearestQuestGameObject(bot, state, 120.0f))
            {
                state.errandGuid = questGo->GetGUID();
                state.errandKind = PveErrandKind::QuestObject;
                state.errandUntil = PveClock::now() + std::chrono::seconds(90);
                TC_LOG_DEBUG("playerbots.pve", "Bot {} starting quest-object errand to {} at {:.0f}y.",
                    bot->GetName(), questGo->GetEntry(), bot->GetDistance(questGo));
                return;
            }
        }

        // Hardcore death chests are free treasure lying in the world: bots grab
        // them like anyone else would - their own, each other's, and players'.
        // Ownership is deliberately not consulted.
        //
        // 200 yards rather than 60: a bot resurrects at a graveyard, which is
        // usually further from where it fell than the old radius reached, so the
        // chest it walked back for was frequently still outside the scan when the
        // journey ended. The errand allows 90 seconds, and 200 yards is about 29
        // at running pace, so the walk still fits comfortably.
        if (cfg.hardcoreLootChestEntry)
            if (GameObject* deathChest = FindRegisteredDeathChest(bot, cfg.hardcoreLootChestEntry, 200.0f))
                if ((!guardian || deathChest->GetZoneId() == GetGuardianZoneId(bot->GetGUID().GetRawValue())) &&
                    deathChest->isSpawned() && deathChest->getLootState() == GO_READY &&
                    !IsRecentErrandTarget(state, deathChest->GetGUID()))
                {
                    state.errandGuid = deathChest->GetGUID();
                    state.errandKind = PveErrandKind::QuestObject;
                    state.errandUntil = PveClock::now() + std::chrono::seconds(90);
                    TC_LOG_DEBUG("playerbots.pve", "Bot {} heading for a death chest at {:.0f}y.",
                        bot->GetName(), bot->GetDistance(deathChest));
                    return;
                }

        bool const needRepair = AnyEquippedItemBelowDurabilityPct(bot, 35);
        bool const needSupplies = cfg.restUseConsumables &&
            ((CountConsumableUnits(bot, false) == 0 && !ConjureSpellId(bot, false)) ||
                (UsesMana(bot) && CountConsumableUnits(bot, true) == 0 && !ConjureSpellId(bot, true)) ||
                (RequiredAmmoSubclass(bot) &&
                    (!bot->GetUInt32Value(PLAYER_AMMO_ID) || bot->GetItemCount(bot->GetUInt32Value(PLAYER_AMMO_ID)) == 0)));
        bool const needVendor = cfg.vendorEnabled &&
            (CountFreeBagSlots(bot) < 4 || needRepair || needSupplies);

        std::vector<Creature*> serviceNpcs;
        ErrandNpcCheck check{ bot };
        Trinity::CreatureListSearcher<ErrandNpcCheck> searcher(bot, serviceNpcs, check);
        Cell::VisitGridObjects(bot, searcher, 200.0f);
        if (serviceNpcs.empty())
        {
            // Nothing in walking range at all: a bot that needs a vendor gets a
            // town run instead of starving in the wilderness.
            if (needVendor && !guardian)
                RequestSupplyRunIfDue(bot, state);
            return;
        }

        std::sort(serviceNpcs.begin(), serviceNpcs.end(), [bot](Creature* left, Creature* right)
        {
            return bot->GetDistance(left) < bot->GetDistance(right);
        });

        std::vector<uint32> completedQuests;
        if (cfg.questsEnabled && !guardian)
            for (auto const& [questId, status] : bot->getQuestStatusMap())
                if (status.Status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                        if (!IsSingleClassQuest(quest))
                            completedQuests.push_back(questId);

        // A worn-out bot needs a vendor that can actually repair, not just any
        // merchant; try those first.
        if (needVendor && needRepair)
            for (Creature* npc : serviceNpcs)
                if ((!guardian || npc->GetZoneId() == GetGuardianZoneId(bot->GetGUID().GetRawValue())) &&
                    !IsRecentErrandTarget(state, npc->GetGUID()) && npc->IsVendor() && npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
                {
                    state.errandGuid = npc->GetGUID();
                    state.errandKind = PveErrandKind::Vendor;
                    state.errandUntil = PveClock::now() + std::chrono::seconds(90);
                    return;
                }

        auto beginErrand = [&](Creature* npc, PveErrandKind kind)
        {
            state.errandGuid = npc->GetGUID();
            state.errandKind = kind;
            state.errandUntil = PveClock::now() + std::chrono::seconds(90);
            TC_LOG_DEBUG("playerbots.pve", "Bot {} starting {} errand to {} at {:.0f}y.",
                bot->GetName(), kind == PveErrandKind::Vendor ? "vendor" : "quest", npc->GetName(), bot->GetDistance(npc));
        };

        for (Creature* npc : serviceNpcs)
        {
            if (guardian && npc->GetZoneId() != GetGuardianZoneId(bot->GetGUID().GetRawValue()))
                continue;
            if (IsRecentErrandTarget(state, npc->GetGUID()))
                continue;

            if (!guardian && cfg.questsEnabled && npc->IsQuestGiver())
            {
                QuestRelationResult const involved = sObjectMgr->GetCreatureQuestInvolvedRelations(npc->GetEntry());
                for (uint32 questId : completedQuests)
                    if (involved.HasQuest(questId))
                        return beginErrand(npc, PveErrandKind::QuestGiver);

                for (uint32 questId : sObjectMgr->GetCreatureQuestRelations(npc->GetEntry()))
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                        if (!IsSingleClassQuest(quest) &&
                            bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                            return beginErrand(npc, PveErrandKind::QuestGiver);
            }

            if (needVendor && npc->IsVendor())
            {
                // A pure supplies need demands a vendor that stocks them; junk
                // selling and repair accept any merchant.
                bool const otherNeeds = CountFreeBagSlots(bot) < 4 || needRepair;
                if (otherNeeds || !needSupplies || VendorStocksNeededSupplies(bot, npc))
                    return beginErrand(npc, PveErrandKind::Vendor);
            }
        }

        // NPCs around, but no usable vendor among them (quest camp, cooldowns):
        // the vendor need still stands, so town-run it.
        if (needVendor && !guardian)
            RequestSupplyRunIfDue(bot, state);
    }

    // Wilderness grind spots can be hundreds of yards from the nearest vendor;
    // gear that hits zero durability counts as unequipped and silently disarms
    // whole rotations (Hamstring, Shield Slam, white swings). When repair is
    // CRITICAL and no vendor is in reach, pay for a field repair from the bot's
    // own gold rather than let it fist-fight plague bears.
    void MaybeFieldRepair(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        if (!cfg.vendorEnabled || state.errandKind != PveErrandKind::None)
            return;

        if (!AnyEquippedItemBelowDurabilityPct(bot, 10))
            return;

        bot->DurabilityRepairAll(true, 1.0f, false);
        TC_LOG_INFO("playerbots.pve", "Bot {} field-repaired its gear (no vendor within reach).", bot->GetName());
    }

    // Returns true while the bot is still busy walking to the errand target.
    bool ProcessErrand(Player* bot, PveBotState& state, playerbot::PveConfig const& /*cfg*/)
    {
        if (state.errandKind == PveErrandKind::None)
            return false;

        auto clearErrand = [&]()
        {
            state.errandGuid = ObjectGuid::Empty;
            state.errandKind = PveErrandKind::None;
        };

        // A guardian may have inherited a quest errand from before it was assigned
        // its post (or from an older build). Drop it immediately. QuestObject is
        // overloaded for death caches, so preserve only the configured cache entry.
        if (GetGuardianZoneId(bot->GetGUID().GetRawValue()))
        {
            if (state.errandKind == PveErrandKind::QuestGiver)
            {
                clearErrand();
                return false;
            }

            if (state.errandKind == PveErrandKind::QuestObject)
            {
                GameObject* go = ObjectAccessor::GetGameObject(*bot, state.errandGuid);
                uint32 const guardianZoneId = GetGuardianZoneId(bot->GetGUID().GetRawValue());
                bool const isDeathCache = go && playerbot::PveManager::GetConfig().hardcoreLootChestEntry &&
                    go->GetEntry() == playerbot::PveManager::GetConfig().hardcoreLootChestEntry &&
                    go->GetZoneId() == guardianZoneId;
                if (!isDeathCache)
                {
                    clearErrand();
                    return false;
                }
            }
        }

        // Errands belong to autonomous bots; a bot that just gained a master
        // drops its errand and follows.
        if (!state.masterGuid.IsEmpty() || PveClock::now() > state.errandUntil)
        {
            clearErrand();
            return false;
        }

        if (state.errandKind == PveErrandKind::QuestObject)
        {
            GameObject* questGo = ObjectAccessor::GetGameObject(*bot, state.errandGuid);
            if (!questGo || !questGo->isSpawned())
            {
                clearErrand();
                return false;
            }

            // Use the lock spell's real interaction range rather than a flat
            // INTERACTION_DISTANCE, the same way the battleground node path does -
            // stopping short of it means the cast is refused on arrival.
            SpellInfo const* const approachLockSpell = questGo->GetSpellForLock(bot);
            bool const atInteractDistance = approachLockSpell
                ? questGo->IsAtInteractDistance(bot, approachLockSpell)
                : questGo->IsAtInteractDistance(bot);

            if (!atInteractDistance)
            {
                MoveTowardThrottled(bot, questGo->GetPosition());
                return true;
            }

            if (UseQuestGameObject(bot, state, questGo))
                return true;                       // mid-open; hold the errand

            clearErrand();
            return false;
        }

        Creature* npc = ObjectAccessor::GetCreature(*bot, state.errandGuid);
        if (!npc || !npc->IsAlive())
        {
            clearErrand();
            return false;
        }

        if (uint32 const guardianZoneId = GetGuardianZoneId(bot->GetGUID().GetRawValue()))
            if (npc->GetZoneId() != guardianZoneId)
            {
                clearErrand();
                return false;
            }

        if (!bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
        {
            MoveTowardThrottled(bot, npc->GetPosition());
            return true;
        }

        // Whatever the visit achieves, don't re-pick this NPC for a while - a
        // turn-in blocked by full bags or an unaffordable repair would otherwise
        // re-trigger the same errand every scan.
        MarkRecentErrandTarget(state, npc->GetGUID());

        if (state.errandKind == PveErrandKind::Vendor)
        {
            uint32 const soldCount = SellVendorJunk(bot);
            if (npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
                bot->DurabilityRepairAll(true, 1.0f, false);
            if (playerbot::PveManager::GetConfig().restUseConsumables)
                TryBuySupplies(bot, npc);
            TC_LOG_INFO("playerbots.pve", "Bot {} visited vendor {} (sold {} junk items).",
                bot->GetName(), npc->GetName(), soldCount);
        }
        else
            AcceptAndTurnInQuestsAt(bot, npc);

        clearErrand();
        return false;
    }

    // A two-hander upgrade once benched a prot warrior's shield through
    // AutoUnequipOffhandIfNeed, permanently breaking Shield Slam. Undo that
    // state where possible: two-hander in main hand, empty off hand, and both a
    // one-hander and a shield sitting in the bags.
    void TryRestoreShieldProfile(Player* bot)
    {
        switch (bot->GetClass())
        {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_SHAMAN:
            break;
        default:
            return;
        }

        Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!mainHand || !mainHand->GetTemplate() || mainHand->GetTemplate()->InventoryType != INVTYPE_2HWEAPON)
            return;

        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            return;

        Item* baggedShield = nullptr;
        Item* baggedOneHander = nullptr;
        ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                return;

            if (!baggedShield && proto->InventoryType == INVTYPE_SHIELD)
                baggedShield = item;
            if (!baggedOneHander && proto->Class == ITEM_CLASS_WEAPON &&
                (proto->InventoryType == INVTYPE_WEAPON || proto->InventoryType == INVTYPE_WEAPONMAINHAND))
                baggedOneHander = item;
        });

        if (!baggedShield || !baggedOneHander)
            return;

        uint16 const mainHandPos = uint16(INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND;
        bot->SwapItem(baggedOneHander->GetPos(), mainHandPos);

        uint16 dest = 0;
        if (bot->CanEquipItem(NULL_SLOT, dest, baggedShield, false) == EQUIP_ERR_OK)
        {
            uint8 const bag = baggedShield->GetBagSlot();
            uint8 const slot = baggedShield->GetSlot();
            bot->RemoveItem(bag, slot, true);
            bot->EquipItem(dest, baggedShield, true);
            TC_LOG_INFO("playerbots.pve", "Bot {} restored its one-hander + shield profile.", bot->GetName());
        }
    }

    float WeaponDps(ItemTemplate const* proto)
    {
        if (!proto->Delay)
            return 0.0f;

        float total = 0.0f;
        for (auto const& damage : proto->Damage)
            total += (damage.DamageMin + damage.DamageMax) * 0.5f;
        return total * 1000.0f / float(proto->Delay);
    }

    // What a real player of this class wears; equipping down a tier is never
    // an upgrade no matter the item level.
    uint32 PreferredArmorSubclass(Player const* bot)
    {
        switch (bot->GetClass())
        {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
            return bot->GetLevel() >= 40 ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return bot->GetLevel() >= 40 ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;
        case CLASS_ROGUE:
        case CLASS_DRUID:
            return ITEM_SUBCLASS_ARMOR_LEATHER;
        default:
            return ITEM_SUBCLASS_ARMOR_CLOTH;
        }
    }

    // The same deterministic profile index that drives talent donors decides
    // weapon policy, so gear and spec always agree.
    uint32 EquipProfileIndex(Player const* bot)
    {
        return uint32(bot->GetGUID().GetCounter() % 3);
    }

    // Only Assassination is built around daggers (Mutilate/Backstab); Combat
    // and Subtlety take anything and want it SLOW.
    bool PrefersDaggerMainhand(Player const* bot)
    {
        return bot->GetClass() == CLASS_ROGUE && EquipProfileIndex(bot) == 0;
    }

    // Every rogue build outside Assassination swings sword, mace or fist - those
    // are its weapon specialisation talents, and the mainhand rule further down
    // already wants the slow, hard-hitting weapon that goes with them. Without an
    // explicit type preference a high-stat dagger keeps winning the slot and the
    // bot fights off-spec with it.
    bool PrefersNonDaggerMainhand(Player const* bot)
    {
        return bot->GetClass() == CLASS_ROGUE && EquipProfileIndex(bot) != 0;
    }

    bool IsSpecialisedRogueMainhand(ItemTemplate const* proto)
    {
        return proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD ||
            proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE ||
            proto->SubClass == ITEM_SUBCLASS_WEAPON_FIST;
    }

    // Beast Mastery hunters want two one-handers, not a two-hander. A hunter's
    // melee weapons are stat sticks it rarely swings, so the slot count is what
    // matters: two one-handers carry two sets of stats, and a two-hander gives
    // up the offhand entirely to carry one. CanDualWield is the authority -
    // below level 20, or before the trainer spell is picked up, a lone one-hander
    // really is worse than a two-hander and this must not fire.
    bool PrefersDualWield(Player const* bot)
    {
        return bot->GetClass() == CLASS_HUNTER && EquipProfileIndex(bot) == 0 && bot->CanDualWield();
    }

    // And the mirror of it: the specs that want the biggest single swing they
    // can hold, in both hands.
    //
    // Arms, Retribution and Enhancement are all built the same way here - every
    // ability is ONE hit that adds flat damage on top of a swing, and the seals,
    // imbues and strikes scale off weapon damage - so the larger hit is worth
    // more than the block and armour a shield brings. Mortal Strike off a slow
    // two-hander is the whole point of Arms, and at this level cap Enhancement
    // has no dual wield to fall back on: that is a TBC talent, so a shaman here
    // is choosing between a two-hander and sword-and-board exactly like the
    // other two.
    //
    // The scorer cannot see that on its own, because it compares slot against
    // slot: a higher item level one-hander wins the mainhand on stats, and a
    // shield then wins an offhand that would otherwise sit empty. Both
    // comparisons are individually correct and the PAIR is wrong, which is why
    // these three ended up in sword and board for their entire lives. Deciding
    // it before any score is the same shape as the hunter rule above.
    //
    // Profiles are EquipProfileIndex (guid % 3), named client-side in the order
    // the armory uses: Warrior Arms/Fury/Protection, Paladin Holy/Protection/
    // Retribution, Shaman Elemental/Enhancement/Restoration.
    bool PrefersTwoHandedMainhand(Player const* bot)
    {
        uint32 const profile = EquipProfileIndex(bot);
        switch (bot->GetClass())
        {
            case CLASS_WARRIOR: return profile == 0;   // Arms
            case CLASS_PALADIN: return profile == 2;   // Retribution
            case CLASS_SHAMAN:  return profile == 1;   // Enhancement
            default:            return false;
        }
    }

    // Instant attacks add flat damage to a single swing, so at comparable
    // quality the slower, harder-hitting weapon wins: average hit per swing
    // is DPS x speed, which prefers slow weapons at equal DPS by construction.
    float WeaponAverageHit(ItemTemplate const* proto)
    {
        float total = 0.0f;
        for (auto const& damage : proto->Damage)
            total += (damage.DamageMin + damage.DamageMax) * 0.5f;
        return total;
    }

    // Per-spec stat weights. On this classic-style server spell power, crit,
    // hit and mp5 live on items as ON_EQUIP auras, not stat fields, so the
    // scorer reads both the ItemStat array and the equip spells.
    struct GearScoreWeights
    {
        float strength = 0.0f, agility = 0.0f, stamina = 0.0f, intellect = 0.0f, spirit = 0.0f;
        float attackPower = 0.0f, spellDamage = 0.0f, healing = 0.0f, mp5 = 0.0f;
        float meleeCritPct = 0.0f, spellCritPct = 0.0f, hitPct = 0.0f, spellHitPct = 0.0f;
        float defense = 0.0f, dodge = 0.0f, critRating = 0.0f, hitRating = 0.0f;
    };

    GearScoreWeights GetGearWeights(Player const* bot)
    {
        enum Archetype { MeleeStr, MeleeAgi, RangedAgi, Tank, CasterDps, Healer };
        uint32 const profileIndex = EquipProfileIndex(bot);
        Archetype archetype;
        switch (bot->GetClass())
        {
        case CLASS_WARRIOR: archetype = profileIndex == 2 ? Tank : MeleeStr; break;
        case CLASS_PALADIN: archetype = profileIndex == 0 ? Healer : (profileIndex == 1 ? Tank : MeleeStr); break;
        case CLASS_HUNTER:  archetype = RangedAgi; break;
        case CLASS_ROGUE:   archetype = MeleeAgi; break;
        case CLASS_PRIEST:  archetype = profileIndex == 2 ? CasterDps : Healer; break;
        case CLASS_SHAMAN:  archetype = profileIndex == 0 ? CasterDps : (profileIndex == 2 ? Healer : MeleeStr); break;
        case CLASS_DRUID:   archetype = profileIndex == 0 ? CasterDps : (profileIndex == 2 ? Healer : MeleeAgi); break;
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        default:            archetype = CasterDps; break;
        }

        GearScoreWeights w;
        switch (archetype)
        {
        case MeleeStr:
            w.strength = 2.0f; w.agility = 1.0f; w.stamina = 0.8f; w.attackPower = 0.6f;
            w.meleeCritPct = 12.0f; w.hitPct = 14.0f; w.critRating = 0.6f; w.hitRating = 0.7f;
            break;
        case MeleeAgi:
            w.agility = 2.0f; w.strength = 1.0f; w.stamina = 0.8f; w.attackPower = 0.6f;
            w.meleeCritPct = 12.0f; w.hitPct = 14.0f; w.critRating = 0.6f; w.hitRating = 0.7f;
            break;
        case RangedAgi:
            w.agility = 2.0f; w.stamina = 0.8f; w.intellect = 0.3f; w.attackPower = 0.6f;
            w.meleeCritPct = 12.0f; w.hitPct = 14.0f; w.critRating = 0.6f; w.hitRating = 0.7f;
            break;
        case Tank:
            w.stamina = 2.0f; w.strength = 1.2f; w.agility = 1.0f; w.defense = 1.2f; w.dodge = 0.8f;
            w.meleeCritPct = 6.0f; w.hitPct = 8.0f;
            break;
        case CasterDps:
            w.spellDamage = 1.6f; w.intellect = 1.0f; w.stamina = 0.5f; w.spirit = 0.3f; w.mp5 = 1.5f;
            w.spellCritPct = 12.0f; w.spellHitPct = 14.0f; w.critRating = 0.5f; w.hitRating = 0.6f;
            break;
        case Healer:
            w.healing = 1.6f; w.intellect = 1.0f; w.spirit = 0.8f; w.stamina = 0.5f; w.mp5 = 2.0f;
            w.spellCritPct = 6.0f;
            break;
        }
        return w;
    }

    float ScoreItemForSpec(Player const* bot, ItemTemplate const* proto)
    {
        GearScoreWeights const w = GetGearWeights(bot);
        float score = 0.0f;

        for (uint32 statIdx = 0; statIdx < MAX_ITEM_PROTO_STATS; ++statIdx)
        {
            float const value = float(proto->ItemStat[statIdx].ItemStatValue);
            switch (proto->ItemStat[statIdx].ItemStatType)
            {
            case ITEM_MOD_STRENGTH:              score += w.strength * value; break;
            case ITEM_MOD_AGILITY:               score += w.agility * value; break;
            case ITEM_MOD_STAMINA:               score += w.stamina * value; break;
            case ITEM_MOD_INTELLECT:             score += w.intellect * value; break;
            case ITEM_MOD_SPIRIT:                score += w.spirit * value; break;
            case ITEM_MOD_ATTACK_POWER:          score += w.attackPower * value; break;
            case ITEM_MOD_SPELL_POWER:           score += std::max(w.spellDamage, w.healing) * value; break;
            case ITEM_MOD_MANA_REGENERATION:     score += w.mp5 * value; break;
            case ITEM_MOD_CRIT_RATING:           score += w.critRating * value; break;
            case ITEM_MOD_HIT_RATING:            score += w.hitRating * value; break;
            case ITEM_MOD_DEFENSE_SKILL_RATING:  score += w.defense * value; break;
            case ITEM_MOD_DODGE_RATING:          score += w.dodge * value; break;
            default: break;
            }
        }

        for (uint8 spellIdx = 0; spellIdx < MAX_ITEM_PROTO_SPELLS; ++spellIdx)
        {
            if (proto->Spells[spellIdx].SpellId <= 0 ||
                proto->Spells[spellIdx].SpellTrigger != ITEM_SPELLTRIGGER_ON_EQUIP)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(proto->Spells[spellIdx].SpellId));
            if (!info)
                continue;

            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                if (!effect.IsAura())
                    continue;

                float const value = float(std::max<int32>(0, effect.CalcValue()));
                switch (effect.ApplyAuraName)
                {
                case SPELL_AURA_MOD_DAMAGE_DONE:          score += w.spellDamage * value; break;
                case SPELL_AURA_MOD_HEALING_DONE:         score += w.healing * value; break;
                case SPELL_AURA_MOD_ATTACK_POWER:         score += w.attackPower * value; break;
                case SPELL_AURA_MOD_RANGED_ATTACK_POWER:  score += w.attackPower * value; break;
                case SPELL_AURA_MOD_WEAPON_CRIT_PERCENT:  score += w.meleeCritPct * value; break;
                case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:    score += w.spellCritPct * value; break;
                case SPELL_AURA_MOD_HIT_CHANCE:           score += w.hitPct * value; break;
                case SPELL_AURA_MOD_SPELL_HIT_CHANCE:     score += w.spellHitPct * value; break;
                case SPELL_AURA_MOD_POWER_REGEN:          score += w.mp5 * value; break;
                default: break;
                }
            }
        }

        return score;
    }

    // Casters and healers treat weapons as stat sticks - DPS on the weapon is
    // meaningless to them and item level tracks the stat budget.
    bool TreatsWeaponAsStatStick(Player const* bot)
    {
        switch (bot->GetClass())
        {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
        case CLASS_DRUID: // feral weapons are stat sticks too
            return true;
        case CLASS_PALADIN:
            return EquipProfileIndex(bot) == 0;  // holy
        case CLASS_SHAMAN:
            return EquipProfileIndex(bot) != 1;  // elemental / resto
        default:
            return false;
        }
    }

    // Item level alone swapped a fury warrior's good weapon for a higher-ilvl
    // paperweight: melee weapons compare by real DPS, casters by stat budget
    // (item level), armor by tier-appropriate subclass first - and spec-defining
    // equipment (shields, dagger mainhands) never gets benched off-spec.
    // A costume is not equipment. Holiday masks carry a seven-day duration,
    // novelty "weapons" deal no damage, and tabards and shirts protect nothing -
    // yet every one of them beats an empty slot on item level alone, so a bot
    // with money and bare shoulders bought them off the auction house until it
    // was broke. Real gear has armour, a stat, a swing, or an equip effect.
    bool HasFightingValue(ItemTemplate const* proto)
    {
        if (proto->Duration)
            return false;

        if (proto->Class == ITEM_CLASS_WEAPON)
            return proto->Damage[0].DamageMax > 0.0f;

        if (proto->Armor)
            return true;

        for (uint32 statIndex = 0; statIndex < proto->StatsCount && statIndex < MAX_ITEM_PROTO_STATS; ++statIndex)
            if (proto->ItemStat[statIndex].ItemStatValue != 0)
                return true;

        for (uint8 spellIndex = 0; spellIndex < MAX_ITEM_PROTO_SPELLS; ++spellIndex)
            if (proto->Spells[spellIndex].SpellId > 0 &&
                proto->Spells[spellIndex].SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
                return true;

        return false;
    }

    // Does this bot actually cast from its off hand? Only those classes have any
    // use for an INVTYPE_HOLDABLE there - a tome, scepter or lantern carries no
    // damage and grants no swing, so for everyone else it silently deletes an
    // entire attack.
    bool BotCastsFromOffhand(Player const* bot)
    {
        switch (bot->GetClass())
        {
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_PRIEST:
            return true;
        case CLASS_PALADIN:                         // holy only; prot wants a shield
            return EquipProfileIndex(bot) == 0;
        case CLASS_SHAMAN:                          // elemental and restoration
        case CLASS_DRUID:                           // balance and restoration
            return EquipProfileIndex(bot) == 0 || EquipProfileIndex(bot) == 2;
        default:                                    // warrior, rogue, hunter
            return false;
        }
    }

    // Quality is part of an item's stat budget, not decoration. Two items of the
    // same item level are not the same item: the budget a green is allowed to
    // spend on stats is meaningfully larger than a white's, and a grey's is barely
    // there at all. Comparing raw ItemLevel therefore rates a white and a green of
    // equal level identically, which is how a bot ends up buying the white.
    //
    // These are an approximation of the budget effect rather than the exact
    // classic curve - the real one varies by slot and level - but the ordering and
    // the rough spacing are what matter for choosing between two items.
    float ItemQualityBudgetFactor(uint32 quality)
    {
        switch (quality)
        {
        case ITEM_QUALITY_POOR:      return 0.60f;
        case ITEM_QUALITY_NORMAL:    return 1.00f;
        case ITEM_QUALITY_UNCOMMON:  return 1.25f;
        case ITEM_QUALITY_RARE:      return 1.45f;
        case ITEM_QUALITY_EPIC:      return 1.65f;
        case ITEM_QUALITY_LEGENDARY: return 1.80f;

        // ARTIFACT is not the top tier on this realm - it is the red field kit
        // issued to replace whatever a death took, and every piece is a copy of
        // a WHITE item, so it is weighed as one. It carries no stats either:
        // 1,107 of the 1,120 artifact items have StatsCount 0, just armour.
        //
        // Falling into the 1.80 default was why bots stopped re-gearing. A kit
        // chestpiece at item level 50 scored 90 effective levels, so beating it
        // needed a GREEN at item level 72 or an EPIC at 55 - nothing on the
        // auction house could clear that bar, and every kit slot became
        // permanently unupgradeable. That is a level 50 sitting on 428 gold in
        // full red, which is exactly what the stats window showed.
        case ITEM_QUALITY_ARTIFACT:  return 1.00f;

        default:                     return 1.00f;
        }
    }

    // Item level as the bot should actually weigh it.
    float EffectiveItemLevel(ItemTemplate const* proto)
    {
        return proto ? float(proto->ItemLevel) * ItemQualityBudgetFactor(proto->Quality) : 0.0f;
    }

    bool IsEquipUpgrade(Player const* bot, ItemTemplate const* candidate, ItemTemplate const* incumbent, uint8 slot)
    {
        // Bags compare by slot count, nothing else.
        if (candidate->Class == ITEM_CLASS_CONTAINER)
            return !incumbent || (incumbent->Class == ITEM_CLASS_CONTAINER &&
                candidate->ContainerSlots > incumbent->ContainerSlots);

        // Quivers and ammo pouches: only the type feeding the equipped ranged
        // weapon, and bigger only.
        if (candidate->Class == ITEM_CLASS_QUIVER)
        {
            uint32 const ammoSubclass = RequiredAmmoSubclass(bot);
            uint32 const wantedSubclass = ammoSubclass == ITEM_SUBCLASS_ARROW ? uint32(ITEM_SUBCLASS_QUIVER)
                : (ammoSubclass == ITEM_SUBCLASS_BULLET ? uint32(ITEM_SUBCLASS_AMMO_POUCH) : 0);
            if (!wantedSubclass || candidate->SubClass != wantedSubclass)
                return false;
            return !incumbent || (incumbent->Class == ITEM_CLASS_QUIVER &&
                candidate->ContainerSlots > incumbent->ContainerSlots);
        }

        // A held off-hand is not a weapon. INVTYPE_HOLDABLE items - tomes,
        // scepters, lanterns - have no damage and grant no swing, so putting one
        // in the off hand of anyone who fights with two weapons silently deletes
        // an entire attack. It is not caught by the shield rule below, and it is
        // not caught by the stat scorer either, because a caster suffix like
        // "of the Owl" can out-score a plain dagger on raw stat weight.
        //
        // Only classes that genuinely cast from that slot may take one; everyone
        // else wants a weapon there, or a shield.
        if (slot == EQUIPMENT_SLOT_OFFHAND)
        {
            bool const castsFromOffhand = BotCastsFromOffhand(bot);

            if (candidate->InventoryType == INVTYPE_HOLDABLE && !castsFromOffhand)
                return false;

            // And the other direction: a bot already holding one is stuck with a
            // dead slot until something displaces it, so a real weapon beats it
            // outright rather than going to the stat scorer, which a caster
            // suffix like "of the Owl" could otherwise win on raw stat weight.
            if (!castsFromOffhand && incumbent && incumbent->InventoryType == INVTYPE_HOLDABLE &&
                candidate->Class == ITEM_CLASS_WEAPON)
                return true;
        }

        // A shield user never benches an equipped shield for a non-shield.
        if (incumbent && slot == EQUIPMENT_SLOT_OFFHAND &&
            incumbent->Class == ITEM_CLASS_ARMOR && incumbent->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD &&
            !(candidate->Class == ITEM_CLASS_ARMOR && candidate->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD))
            return false;

        // Nothing temporary, ever - a seven-day mask is not gear.
        if (candidate->Duration)
            return false;

        // Never pick up test/placeholder stock, whatever its stats claim.
        if (LooksLikeScaffoldingItem(candidate))
            return false;

        // An empty slot is not a licence to buy anything at all.
        if (!incumbent)
            return HasFightingValue(candidate);

        if (candidate->Class == ITEM_CLASS_WEAPON && incumbent->Class == ITEM_CLASS_WEAPON)
        {
            // On-spec weapon type dominates every other comparison for an
            // assassination rogue's mainhand.
            if (slot == EQUIPMENT_SLOT_MAINHAND && PrefersDaggerMainhand(bot))
            {
                bool const candidateDagger = candidate->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
                bool const incumbentDagger = incumbent->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
                if (candidateDagger != incumbentDagger)
                    return candidateDagger;
            }

            // The mirror of that rule for every other rogue build: prefer the
            // specialised type over a dagger, whatever the dagger stats say.
            if (slot == EQUIPMENT_SLOT_MAINHAND && PrefersNonDaggerMainhand(bot))
            {
                bool const candidateOnType = IsSpecialisedRogueMainhand(candidate);
                bool const incumbentOnType = IsSpecialisedRogueMainhand(incumbent);
                if (candidateOnType != incumbentOnType)
                    return candidateOnType;
            }

            // Taking a two-hander forfeits a whole weapon slot, so for a dual
            // wielding hunter a one-hander wins on that alone, before any score
            // comparison. Otherwise the higher-ilvl two-hander keeps winning the
            // mainhand and the offhand sits empty forever.
            if (slot == EQUIPMENT_SLOT_MAINHAND && PrefersDualWield(bot))
            {
                bool const candidateTwoHand = candidate->InventoryType == INVTYPE_2HWEAPON;
                bool const incumbentTwoHand = incumbent->InventoryType == INVTYPE_2HWEAPON;
                if (candidateTwoHand != incumbentTwoHand)
                    return !candidateTwoHand;
            }

            // And the same rule pointing the other way, for the specs that swing
            // one big weapon. Decided before the score for the same reason: the
            // one-hander often WINS on stats and is still the wrong answer.
            if (slot == EQUIPMENT_SLOT_MAINHAND && PrefersTwoHandedMainhand(bot))
            {
                bool const candidateTwoHand = candidate->InventoryType == INVTYPE_2HWEAPON;
                bool const incumbentTwoHand = incumbent->InventoryType == INVTYPE_2HWEAPON;
                if (candidateTwoHand != incumbentTwoHand)
                    return candidateTwoHand;
            }

            // Stat-stick weapons compare by the spec's stat score; only item
            // level breaks a genuine tie.
            bool const statStick = TreatsWeaponAsStatStick(bot) ||
                (bot->GetClass() == CLASS_HUNTER && slot != EQUIPMENT_SLOT_RANGED);
            if (statStick)
            {
                float const candidateScore = ScoreItemForSpec(bot, candidate);
                float const incumbentScore = ScoreItemForSpec(bot, incumbent);
                if (std::fabs(candidateScore - incumbentScore) > 0.5f)
                    return candidateScore > incumbentScore;
                return EffectiveItemLevel(candidate) > EffectiveItemLevel(incumbent);
            }

            // Combat and Subtlety mainhands: slow and hard-hitting beats fast,
            // because instant attacks ride the average swing.
            if (bot->GetClass() == CLASS_ROGUE && slot == EQUIPMENT_SLOT_MAINHAND && EquipProfileIndex(bot) != 0)
                return WeaponAverageHit(candidate) > WeaponAverageHit(incumbent) + 0.1f;

            return WeaponDps(candidate) > WeaponDps(incumbent) + 0.1f;
        }

        if (candidate->Class == ITEM_CLASS_ARMOR &&
            candidate->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH && candidate->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE &&
            incumbent->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH && incumbent->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE)
        {
            uint32 const preferred = PreferredArmorSubclass(bot);
            bool const candidateOnTier = candidate->SubClass == preferred;
            bool const incumbentOnTier = incumbent->SubClass == preferred;
            if (candidateOnTier != incumbentOnTier)
                return candidateOnTier;
        }

        // Spec stat score before raw item level for every slot: ret paladins
        // want strength and crit plate, not higher-ilvl spell-damage plate.
        float const candidateScore = ScoreItemForSpec(bot, candidate);
        float const incumbentScore = ScoreItemForSpec(bot, incumbent);
        if (std::fabs(candidateScore - incumbentScore) > 0.5f)
            return candidateScore > incumbentScore;

        return EffectiveItemLevel(candidate) > EffectiveItemLevel(incumbent);
    }

    void TryEquipUpgrades(Player* bot)
    {
        TryRestoreShieldProfile(bot);

        std::vector<std::pair<uint8, uint8>> positions;
        ForEachBagItem(bot, [&](Item* /*item*/, uint8 bag, uint8 slot)
        {
            positions.emplace_back(bag, slot);
        });

        for (auto const& position : positions)
        {
            Item* item = bot->GetItemByPos(position.first, position.second);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR &&
                proto->Class != ITEM_CLASS_CONTAINER && proto->Class != ITEM_CLASS_QUIVER))
                continue;

            // Never bench an equipped off hand (shield!) for a two-hander: the
            // "upgrade" silently disarms shield-dependent rotations.
            if (proto->InventoryType == INVTYPE_2HWEAPON &&
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                continue;

            uint16 dest = 0;
            if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
                continue;

            // CanEquipItem still does the real validation above - combat state,
            // proficiency, ownership - but its choice of WHICH bag slot is not
            // usable for an upgrade decision, so containers pick their own.
            if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER)
                if (!SelectContainerUpgradeSlot(bot, proto, dest))
                    continue;

            // Same problem, other slot: a one-hander is offered to the main hand
            // first, so a dead off hand would never be challenged.
            if (ShouldRedirectToOffHand(bot, proto, dest))
                dest = uint16((uint16(INVENTORY_SLOT_BAG_0) << 8) | EQUIPMENT_SLOT_OFFHAND);

            if (Item* equipped = bot->GetItemByPos(dest))
            {
                if (!IsEquipUpgrade(bot, proto, equipped->GetTemplate(), uint8(dest & 255)))
                    continue;

                // A bag cannot be unequipped while it holds anything, so the
                // better bag would otherwise sit in the pack forever, retried on
                // every pass and costing a slot for the privilege. Clear the old
                // one out first, and if its contents will not fit anywhere else,
                // leave everything exactly as it was.
                if (Bag* equippedBag = equipped->ToBag())
                {
                    if (!CanRehomeBagContents(bot, equippedBag) || !TryEmptyBagForSwap(bot, equippedBag))
                        continue;
                }

                // Re-read the position: emptying the old bag may have shuffled
                // items around this one.
                bot->SwapItem(item->GetPos(), dest);
            }
            else
            {
                bot->RemoveItem(position.first, position.second, true);
                bot->EquipItem(dest, item, true);
                bot->AutoUnequipOffhandIfNeed();
            }

            TC_LOG_INFO("playerbots.pve", "Bot {} equipped {} (item level {}).",
                bot->GetName(), proto->Name1, proto->ItemLevel);
        }
    }

    // ---------------------------------------------------------------------------
    // Talent auto-spend
    //
    // The talent trees on this fork are CUSTOM (rebuilt from talent_lplus, ids
    // renumbered on every regeneration), so no premade id lists can be trusted.
    // Instead: pick the tab that contains the signature talent of the bot's
    // assigned profile (same signature spells DetectClassicClassProfile keys on -
    // spell ids are stable across talent rebuilds), then greedily fill that tab
    // tier by tier. Player::LearnTalent validates class, prerequisites and row
    // gates itself and silently no-ops, so the greedy loop is safe by
    // construction.
    // ---------------------------------------------------------------------------

    // Signature talent SPELLS per class, Primary/Secondary/Tertiary - keep in
    // sync with DetectClassicClassProfile in PlayerbotPvpCore.cpp.
    std::array<uint32, 3> GetProfileSignatureSpells(uint8 classId)
    {
        switch (classId)
        {
        case CLASS_WARRIOR: return { 12294, 81273, 23922 };
        case CLASS_PALADIN: return { 20473, 20925, 20375 };
        case CLASS_HUNTER:  return { 81300, 19506, 19386 };
        case CLASS_ROGUE:   return { 81302, 13750, 14185 };
        case CLASS_PRIEST:  return { 10060, 724, 15473 };
        case CLASS_SHAMAN:  return { 16166, 17364, 16188 };
        case CLASS_MAGE:    return { 12042, 33041, 11426 };
        case CLASS_WARLOCK: return { 48181, 19028, 17962 };
        case CLASS_DRUID:   return { 24858, 18562, 17007 };
        default:            return { 0, 0, 0 };
        }
    }

    std::vector<TalentEntry const*> const& GetSortedTabTalents(uint32 tabId)
    {
        static std::mutex cacheLock;
        static std::unordered_map<uint32, std::vector<TalentEntry const*>> cacheByTab;

        std::lock_guard<std::mutex> guard(cacheLock);
        auto const [itr, inserted] = cacheByTab.try_emplace(tabId);
        if (inserted)
        {
            for (TalentEntry const* talent : sTalentStore)
                if (talent && talent->TabID == tabId)
                    itr->second.push_back(talent);

            std::sort(itr->second.begin(), itr->second.end(), [](TalentEntry const* left, TalentEntry const* right)
            {
                if (left->TierID != right->TierID)
                    return left->TierID < right->TierID;
                return left->ColumnIndex < right->ColumnIndex;
            });
        }

        return itr->second;
    }

    uint32 FindTalentTabContainingSpell(uint32 spellId)
    {
        if (!spellId)
            return 0;

        for (TalentEntry const* talent : sTalentStore)
            if (talent)
                for (uint32 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                    if (talent->SpellRank[rank] == spellId)
                        return talent->TabID;

        return 0;
    }

    std::vector<uint32> GetClassTalentTabs(Player const* bot)
    {
        std::vector<uint32> tabs;
        for (TalentTabEntry const* tab : sTalentTabStore)
            if (tab && (tab->ClassMask & bot->GetClassMask()) && !tab->PetTalentMask)
                tabs.push_back(tab->ID);

        std::sort(tabs.begin(), tabs.end(), [](uint32 left, uint32 right)
        {
            TalentTabEntry const* leftTab = sTalentTabStore.LookupEntry(left);
            TalentTabEntry const* rightTab = sTalentTabStore.LookupEntry(right);
            return (leftTab ? leftTab->OrderIndex : 0) < (rightTab ? rightTab->OrderIndex : 0);
        });
        return tabs;
    }

    uint8 CurrentTalentRank(Player const* bot, TalentEntry const* talent)
    {
        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            if (talent->SpellRank[rank] && bot->HasTalent(talent->SpellRank[rank], bot->GetActiveSpec()))
                return uint8(rank + 1);

        return 0;
    }

    // One greedy pass over a tab; returns points spent.
    uint32 GreedySpendInTab(Player* bot, uint32 tabId)
    {
        uint32 spent = 0;
        bool progress = true;
        while (progress && bot->GetFreeTalentPoints())
        {
            progress = false;
            for (TalentEntry const* talent : GetSortedTabTalents(tabId))
            {
                uint8 const currentRank = CurrentTalentRank(bot, talent);
                if (currentRank >= MAX_TALENT_RANK || !talent->SpellRank[currentRank])
                    continue;

                uint32 const before = bot->GetFreeTalentPoints();
                bot->LearnTalent(talent->ID, currentRank);
                if (bot->GetFreeTalentPoints() < before)
                {
                    spent += before - bot->GetFreeTalentPoints();
                    progress = true;
                    if (!bot->GetFreeTalentPoints())
                        return spent;
                }
            }
        }

        return spent;
    }

    // ---------------------------------------------------------------------------
    // Talent recipes: leveling bots copy the hand-built specs of the fleet's
    // level-60 archetype bots (the owner's own builds), so every build conforms
    // to those instead of a synthetic greedy fill. The donor's character_talent
    // rows ARE the recipe - rank spell ids, immune to talent-id renumbering -
    // and respeccing a donor updates the recipe at the next server start.
    // ---------------------------------------------------------------------------

    // Two names per donor: the fleet's lore names, plus the legacy Bot<spec>
    // names as a fallback so realms that have not run the rename keep working.
    char const* TalentDonorName(uint8 botClass, uint32 pick, bool legacy)
    {
        switch (botClass)
        {
        case CLASS_WARRIOR: { static char const* const n[6] = { "Gorthak", "Skarvald", "Thorgrim", "Botwarrarms", "Botwarrfury", "Botwarrprot" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_PALADIN: { static char const* const n[6] = { "Aldric", "Barathen", "Varethan", "Botpalholy", "Botpalprot", "Botpalret" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_HUNTER: { static char const* const n[6] = { "Thornwild", "Swiftarrow", "Grimtrack", "Bothuntbeast", "Bothuntmarks", "Bothuntsurv" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_ROGUE: { static char const* const n[6] = { "Vexis", "Slateblade", "Shadowmere", "Botrogass", "Botrogcombat", "Botrogsub" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_PRIEST: { static char const* const n[6] = { "Seraphine", "Lumenara", "Vespera", "Botpridisc", "Botpriholy", "Botprishadow" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_SHAMAN: { static char const* const n[6] = { "Tempestra", "Korgul", "Riverwind", "Botshamele", "Botshamenh", "Botshamresto" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_MAGE: { static char const* const n[6] = { "Elandrus", "Pyrella", "Rimeveil", "Botmagarcane", "Botmagfire", "Botmagfrost" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_WARLOCK: { static char const* const n[6] = { "Morgatha", "Karzul", "Infernia", "Botwarlaffl", "Botwarldemo", "Botwarldest" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        case CLASS_DRUID: { static char const* const n[6] = { "Lunaris", "Clawthorn", "Sylvanel", "Botdruidbal", "Botdruferal", "Botdruidrest" }; return n[(legacy ? 3 : 0) + pick % 3]; }
        default:
            return nullptr;
        }
    }

    std::mutex g_TalentRecipeLock;
    std::unordered_map<uint32, std::vector<uint32>> g_TalentRecipesByKey;

    std::vector<uint32> GetTalentRecipe(Player* bot, uint32 pick)
    {
        uint32 const key = uint32(bot->GetClass()) * 4 + (pick % 3);
        std::lock_guard<std::mutex> guard(g_TalentRecipeLock);
        auto itr = g_TalentRecipesByKey.find(key);
        if (itr != g_TalentRecipesByKey.end())
            return itr->second;

        // Missing donors negative-cache as empty (hunters have no B+ donor).
        std::vector<uint32>& recipe = g_TalentRecipesByKey[key];

        // Exported recipes come first. A realm that keeps no donor characters of
        // its own - Barracks+ has none, the hand-built specs live on the other
        // realm - reads the builds from this table, which a one-shot export
        // populates. The engine still only ever reads its own database.
        if (QueryResult exported = CharacterDatabase.PQuery(
            "SELECT spell FROM playerbot_talent_recipe WHERE class = {} AND pick = {}",
            uint32(bot->GetClass()), pick % 3))
        {
            do
            {
                recipe.push_back((*exported)[0].GetUInt32());
            } while (exported->NextRow());
        }
        if (!recipe.empty())
            return recipe;

        for (bool legacy : { false, true })
        {
            char const* donorName = TalentDonorName(bot->GetClass(), pick, legacy);
            if (!donorName)
                break;

            ObjectGuid const donorGuid = sCharacterCache->GetCharacterGuidByName(donorName);
            if (donorGuid.IsEmpty() || donorGuid == bot->GetGUID())
                continue;

            if (QueryResult result = CharacterDatabase.PQuery(
                "SELECT spell FROM character_talent WHERE guid = {} AND talentGroup = 0", donorGuid.GetCounter()))
                do
                {
                    recipe.push_back((*result)[0].GetUInt32());
                } while (result->NextRow());
            if (!recipe.empty())
                break;
        }
        return recipe;
    }

    uint32 SpendTalentsFromRecipe(Player* bot, std::vector<uint32> const& recipe)
    {
        if (recipe.empty())
            return 0;

        struct RecipeTalent
        {
            TalentEntry const* talent;
            uint8 targetRank; // 0-based index of the donor's learned rank
        };
        std::vector<RecipeTalent> entries;
        for (uint32 spellId : recipe)
            for (TalentEntry const* talent : sTalentStore)
                if (talent)
                    for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                        if (talent->SpellRank[rank] == spellId)
                            entries.push_back({ talent, rank });

        // Tier order satisfies row requirements the way the donor's own legal
        // build did; the outer passes spread one rank at a time so multi-tree
        // builds interleave the way a leveling player's would.
        std::sort(entries.begin(), entries.end(), [](RecipeTalent const& left, RecipeTalent const& right)
        {
            if (left.talent->TierID != right.talent->TierID)
                return left.talent->TierID < right.talent->TierID;
            return left.talent->ColumnIndex < right.talent->ColumnIndex;
        });

        uint32 spent = 0;
        bool progress = true;
        while (progress && bot->GetFreeTalentPoints())
        {
            progress = false;
            for (RecipeTalent const& entry : entries)
            {
                uint8 const currentRank = CurrentTalentRank(bot, entry.talent);
                if (currentRank > entry.targetRank)
                    continue;

                uint32 const before = bot->GetFreeTalentPoints();
                bot->LearnTalent(entry.talent->ID, currentRank);
                if (bot->GetFreeTalentPoints() < before)
                {
                    spent += before - bot->GetFreeTalentPoints();
                    progress = true;
                    if (!bot->GetFreeTalentPoints())
                        return spent;
                }
            }
        }

        return spent;
    }

    void SpendPendingTalentPoints(Player* bot)
    {
        if (bot->GetLevel() < 10 || !bot->GetFreeTalentPoints())
            return;

        // Deterministic per-character profile so a class's bots spread across
        // specs but each keeps the same build for life.
        uint32 const profileIndex = uint32(bot->GetGUID().GetCounter() % 3);

        // The owner's hand-built donor spec first; greedy filling only mops up
        // what the recipe can't place (no donor, or points beyond its build).
        uint32 const recipeSpent = SpendTalentsFromRecipe(bot, GetTalentRecipe(bot, profileIndex));
        if (!bot->GetFreeTalentPoints())
        {
            if (recipeSpent)
                TC_LOG_INFO("playerbots.pve", "Bot {} spent {} talent points from the {} build.",
                    bot->GetName(), recipeSpent, TalentDonorName(bot->GetClass(), profileIndex, false));
            return;
        }

        std::array<uint32, 3> const signatures = GetProfileSignatureSpells(bot->GetClass());
        uint32 preferredTab = FindTalentTabContainingSpell(signatures[profileIndex]);
        for (uint32 probe = 1; probe < 3 && !preferredTab; ++probe)
            preferredTab = FindTalentTabContainingSpell(signatures[(profileIndex + probe) % 3]);

        std::vector<uint32> const classTabs = GetClassTalentTabs(bot);
        if (!preferredTab && !classTabs.empty())
            preferredTab = classTabs.front();
        if (!preferredTab)
            return;

        uint32 spent = recipeSpent + GreedySpendInTab(bot, preferredTab);
        // Overflow into the other trees once the main tab can't absorb points.
        for (uint32 tabId : classTabs)
            if (tabId != preferredTab && bot->GetFreeTalentPoints())
                spent += GreedySpendInTab(bot, tabId);

        if (spent)
            TC_LOG_INFO("playerbots.pve", "Bot {} spent {} talent points ({} from the donor build, main tab {}).",
                bot->GetName(), spent, recipeSpent, preferredTab);
    }

    // Same loop as ".learn my trainer": keep taking every class-trainer spell
    // the bot qualifies for until a full pass adds nothing, so rank chains
    // resolve in one go. Returns the number of spells taught.
    uint32 RunTrainerSpellCatchup(Player* player)
    {
        // GetClassTrainers is an unguarded unordered_map::at - a class with zero
        // class-trainer rows (death knights here) would throw.
        static std::vector<Trainer::Trainer const*> const emptyTrainers;
        std::vector<Trainer::Trainer const*> const* trainersPtr = &emptyTrainers;
        try
        {
            trainersPtr = &sObjectMgr->GetClassTrainers(player->GetClass());
        }
        catch (std::out_of_range const&)
        {
        }

        uint32 learned = 0;
        uint32 passes = 0;
        bool hadNew;
        do
        {
            // Pass cap = insurance against a trainer row whose teach never
            // changes learnable state.
            if (++passes > 10)
                break;

            hadNew = false;
            for (Trainer::Trainer const* trainer : *trainersPtr)
            {
                if (!trainer->IsTrainerValidForPlayer(player))
                    continue;

                for (Trainer::Spell const& trainerSpell : trainer->GetSpells())
                {
                    if (!trainer->CanTeachSpell(player, &trainerSpell))
                        continue;

                    if (trainerSpell.IsCastable())
                        player->CastSpell(player, trainerSpell.SpellId, true);
                    else
                        player->LearnSpell(trainerSpell.SpellId, false);

                    ++learned;
                    hadNew = true;
                }
            }
        } while (hadNew);

        return learned;
    }

    // Riding, granted rather than bought.
    //
    // RunTrainerSpellCatchup walks CLASS trainers, and riding is not sold by
    // one - it comes from a riding trainer, and the mount from a vendor. A bot
    // met neither, so it never learned skill 762 and ran the whole climb on
    // foot while every player around it rode.
    // Which vendor sells a given race its OWN mounts.
    //
    // The obvious source, ItemTemplate::AllowableRace, cannot answer this: it is
    // a FACTION mask, not a race one. Every Alliance mount carries 1101 and
    // every Horde mount 690, so filtering on it says a night elf may ride a
    // dwarf's ram - and the thirty-five mounts that carry no mask at all
    // (Deathcharger, the Qiraji crystals, the Zulian tiger, the Riding Turtle)
    // read as "everyone may have this", which is how a night elf ended up on a
    // sea turtle.
    //
    // The mount vendors DO know. Each race's breeder sells that race's line and
    // nothing else, so the vendor is the fact we want and the item table is not.
    // Keyed on vendor ENTRY rather than name, which is stable across locales and
    // roster edits; the item list stays data-driven, so a new sabre added to
    // Lelanai reaches the night elves with no code change.
    struct RacialMountVendors
    {
        uint8 race;
        std::array<uint32, 4> vendors;   // 0 terminates
    };

    static constexpr std::array<RacialMountVendors, 10> kRacialMountVendors = { {
        { RACE_HUMAN,     { 384, 1460, 2357, 4885 } },   // Horse Breeder
        { RACE_ORC,       { 3362, 0, 0, 0 } },           // Kennel Master (wolves)
        { RACE_DWARF,     { 1261, 0, 0, 0 } },           // Ram Breeder
        { RACE_NIGHTELF,  { 4730, 0, 0, 0 } },           // Saber Handler
        { RACE_UNDEAD_PLAYER, { 4731, 0, 0, 0 } },       // Undead Horse Merchant
        { RACE_TAUREN,    { 3685, 0, 0, 0 } },           // Kodo Mounts
        { RACE_GNOME,     { 7955, 0, 0, 0 } },           // Mechanostrider Merchant
        { RACE_TROLL,     { 7952, 0, 0, 0 } },           // Raptor Handler
        { RACE_BLOODELF,  { 16264, 0, 0, 0 } },          // Hawkstrider Breeder
        { RACE_DRAENEI,   { 17584, 0, 0, 0 } },          // Elekk Breeder
    } };

    // race -> riding rank -> the mount spells that race may be given. Rank 75 is
    // the 60% ground mounts, 150 the 100% ones. The 225/300 ranks are flying and
    // deliberately excluded - no bot has anywhere to fly, and a mounted bot in
    // the air is a bot that cannot be reached.
    using BotMountPools = std::unordered_map<uint8, std::unordered_map<uint32, std::vector<uint32>>>;

    BotMountPools const& GetBotMountPools()
    {
        static BotMountPools const pools = []
        {
            BotMountPools built;
            for (RacialMountVendors const& entry : kRacialMountVendors)
            {
                for (uint32 const vendorEntry : entry.vendors)
                {
                    if (!vendorEntry)
                        break;

                    VendorItemData const* vendorItems = sObjectMgr->GetNpcVendorItemList(vendorEntry);
                    if (!vendorItems)
                        continue;

                    for (VendorItem const& vendorItem : vendorItems->m_items)
                    {
                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem.item);
                        if (!proto || proto->RequiredSkill != SKILL_RIDING)
                            continue;
                        if (proto->RequiredSkillRank != 75 && proto->RequiredSkillRank != 150)
                            continue;

                        // The item has to actually BE a mount. Plenty of mount
                        // items carry a teach-this-spell wrapper instead of the
                        // mount aura, and teaching the wrapper to a bot grants
                        // nothing it can cast.
                        uint32 mountSpellId = 0;
                        for (_Spell const& itemSpell : proto->Spells)
                        {
                            SpellInfo const* spellInfo = itemSpell.SpellId > 0 ?
                                sSpellMgr->GetSpellInfo(uint32(itemSpell.SpellId)) : nullptr;
                            if (spellInfo && spellInfo->HasAura(SPELL_AURA_MOUNTED))
                            {
                                mountSpellId = uint32(itemSpell.SpellId);
                                break;
                            }
                        }

                        if (!mountSpellId)
                            continue;

                        std::vector<uint32>& rankPool = built[entry.race][proto->RequiredSkillRank];
                        if (std::find(rankPool.begin(), rankPool.end(), mountSpellId) == rankPool.end())
                            rankPool.push_back(mountSpellId);
                    }
                }

                if (built.find(entry.race) == built.end())
                    TC_LOG_ERROR("playerbots.pve",
                        "No racial mounts found for race {} - its vendors sell none, so bots of that race stay on foot.",
                        uint32(entry.race));
            }
            return built;
        }();

        return pools;
    }

    // Returns true if anything was handed over.
    bool EnsureBotRidingAndMount(Player* bot)
    {
        if (!g_PveConfig.grantMounts || !bot)
            return false;

        struct RidingTier
        {
            uint8 level;
            uint32 ridingSpell;
            uint32 rank;
        };

        // Ascending, and the loop stops at the first tier the bot has not
        // reached - so a level 45 bot gets apprentice and nothing more.
        static constexpr std::array<RidingTier, 2> kRidingTiers = { {
            { 40, 33388, 75 },
            { 60, 33391, 150 },
        } };

        bool granted = false;
        for (RidingTier const& tier : kRidingTiers)
        {
            if (bot->GetLevel() < tier.level)
                break;

            // The riding spell carries the SkillLineAbility row for skill 762,
            // so learning it is what raises the skill - there is no separate
            // SetSkill to make, and making one would fight the spell.
            if (!bot->HasSpell(tier.ridingSpell))
            {
                bot->LearnSpell(tier.ridingSpell, false);
                granted = true;
            }

            auto const& pools = GetBotMountPools();
            auto raceItr = pools.find(uint8(bot->GetRace()));
            if (raceItr == pools.end())
                continue;

            auto poolItr = raceItr->second.find(tier.rank);
            if (poolItr == raceItr->second.end())
                continue;

            std::vector<uint32> usable;
            bool alreadyMounted = false;
            for (uint32 const spellId : poolItr->second)
            {
                if (bot->HasSpell(spellId))
                {
                    alreadyMounted = true;
                    break;
                }

                usable.push_back(spellId);
            }

            if (alreadyMounted || usable.empty())
                continue;

            // Keyed to the character rather than random, so a bot keeps the
            // same mount across restarts instead of changing colour whenever
            // this runs. Sorted first because the item store's iteration order
            // is not stable between boots, which would otherwise undo that.
            std::sort(usable.begin(), usable.end());
            uint32 const pick = usable[bot->GetGUID().GetCounter() % usable.size()];
            bot->LearnSpell(pick, false);
            granted = true;
        }

        return granted;
    }

    // One-time per login: SQL-provisioned bot characters start with an empty
    // spellbook and a bare weapon (the auto-learn hook only fires on level-up,
    // which a level-1 bot has never had). Teach everything trainable, spend any
    // banked talents, and dress a naked bot in its class's real starter outfit.
    void EnsureFirstLoginKit(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        if (state.initialKitDone || !bot->IsAlive())
            return;
        state.initialKitDone = true;

        uint32 learned = 0;
        if (cfg.autoLearnSpellsOnLevelUp)
            learned = RunTrainerSpellCatchup(bot);

        // Also here, not only on level-up: the bots already past forty when
        // this shipped would otherwise wait for their next level to get a
        // mount, and a veteran sitting at sixty would never get one at all.
        EnsureBotRidingAndMount(bot);

        if (cfg.talentsEnabled)
            SpendPendingTalentPoints(bot);

        bool outfitGranted = false;
        bool const nakedTorso = !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST) &&
            !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_LEGS);
        if (nakedTorso)
        {
            if (CharStartOutfitEntry const* outfit = GetCharStartOutfitEntry(bot->GetRace(), bot->GetClass(), bot->GetGender()))
            {
                // Mirrors Player::Create's outfit grant, food stacks included.
                for (int32 outfitItemId : outfit->ItemID)
                {
                    if (outfitItemId <= 0)
                        continue;

                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(uint32(outfitItemId));
                    if (!proto)
                        continue;

                    uint32 count = proto->BuyCount;
                    if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD)
                    {
                        switch (proto->Spells[0].SpellCategory)
                        {
                        case SPELL_CATEGORY_FOOD:
                            count = 4;
                            break;
                        case SPELL_CATEGORY_DRINK:
                            count = 2;
                            break;
                        }
                        if (proto->GetMaxStackSize() < count)
                            count = proto->GetMaxStackSize();
                    }

                    outfitGranted = bot->StoreNewItemInBestSlots(uint32(outfitItemId), count) || outfitGranted;
                }
            }
        }

        // A hunter kit is unusable dry: guarantee a starter stack of the ammo
        // its ranged weapon feeds on (the weapon just arrived with the outfit),
        // and load it. Idempotent - once ammo is loaded this never fires again,
        // so it doubles as the fleet's one-time 200-ammo grant.
        // The hunter pet kit. Tame Beast and its companions are taught by a quest
        // chain whose middle step is "use this rod on that beast", which a bot
        // cannot reliably walk: not one of the fleet's twenty-five hunters knew
        // Tame Beast, so not one of them ever had a pet - half a hunter, forever.
        // Granted outright from level 10, the level the chain itself opens at.
        if (bot->GetClass() == CLASS_HUNTER && bot->GetLevel() >= 10)
        {
            constexpr std::array<uint32, 4> kHunterPetKit = { {
                1515,  // Tame Beast
                883,   // Call Pet
                982,   // Revive Pet
                6991   // Feed Pet
            } };

            for (uint32 petSpell : kHunterPetKit)
                if (!bot->HasSpell(petSpell))
                    bot->LearnSpell(petSpell, false);
        }

        if (bot->GetClass() == CLASS_HUNTER)
            if (uint32 const ammoSubclass = RequiredAmmoSubclass(bot))
            {
                uint32 const ammoId = ammoSubclass == ITEM_SUBCLASS_ARROW ? 2512u : 2516u;
                if (!bot->GetItemCount(ammoId) && !bot->GetUInt32Value(PLAYER_AMMO_ID))
                {
                    ItemPosCountVec ammoDest;
                    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, ammoDest, ammoId, 200) == EQUIP_ERR_OK)
                        bot->StoreNewItem(ammoDest, ammoId, true);
                }
                if (!bot->GetUInt32Value(PLAYER_AMMO_ID) && bot->GetItemCount(ammoId))
                    bot->SetAmmo(ammoId);
            }

        if (learned || outfitGranted)
            TC_LOG_INFO("playerbots.pve", "Bot {} first-login kit: {} trainer spells{}.",
                bot->GetName(), learned, outfitGranted ? ", starter outfit granted" : "");
    }

    // ---------------------------------------------------------------------------
    // Naked recovery. On a full-loot realm a bot that never reclaims its death
    // chest is left helpless: it cannot kill anything, so it dies again, and the
    // spiral never ends (the Stonetalon guardian dying every two minutes). A
    // stripped bot spends its WHOLE purse at the auction house instead of the
    // usual upgrade slice. The white field kit that guarantees it can always
    // fight belongs to the hardcore script, which dresses players and bots alike
    // on resurrection.
    // ---------------------------------------------------------------------------

    constexpr std::array<uint8, 9> kCoreGearSlots = { {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_CHEST,
        EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET,
        EQUIPMENT_SLOT_WRISTS, EQUIPMENT_SLOT_HANDS, EQUIPMENT_SLOT_MAINHAND
    } };

    // Losing a piece or two is ordinary adventuring wear; this is the
    // "died and left everything in the chest" state. Level 1-9 bots wear their
    // starter outfit and nothing else, which is not nakedness.
    bool IsBotStrippedBare(Player const* bot)
    {
        if (bot->GetLevel() < 10)
            return false;

        uint32 filled = 0;
        for (uint8 slot : kCoreGearSlots)
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++filled;
        return filled <= 3;
    }

    // ---------------------------------------------------------------------------
    // Level-appropriate relocation (the reference module's teleport-for-level,
    // rebuilt on this core's in-memory spawn store)
    // ---------------------------------------------------------------------------

    struct GrindSpot
    {
        uint16 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 zoneId = 0;
    };

    // ---------------------------------------------------------------------------
    // Zone guardians: a configurable number of bots per classic zone live there
    // permanently at the zone's classic level cap, XP frozen, grinding forever.
    // Slots are claimed first-come each uptime; bots still wearing the frozen-XP
    // flag from a previous uptime re-claim first, so assignments stay sticky.
    // ---------------------------------------------------------------------------

    struct GuardianZone
    {
        uint32 zoneId;
        uint8 maxLevel;
    };

    // The level a guardian should actually hold at its post. Defined further
    // down, once the zone-top table it reads exists.
    uint8 GuardianPostCeiling(GuardianZone const& zone);

    // The classic levelling chart, as the zones actually play - not as a
    // percentile of whatever creatures happen to be spawned in them. Derived bands
    // were wrong in both directions: they let The Deadmines in through its Westfall
    // tunnel, and gave real zones one-level bands because their clusters happened
    // to sit at one level. This is the authority now.
    struct ClassicZoneBand
    {
        uint32 zoneId;
        uint8 minLevel;
        uint8 maxLevel;
    };

    constexpr std::array<ClassicZoneBand, 39> kClassicZoneBands = { {
        {   1,  1, 10 },  // Dun Morogh
        {  12,  1, 10 },  // Elwynn Forest
        { 141,  1, 10 },  // Teldrassil
        {  85,  1, 10 },  // Tirisfal Glades
        {  14,  1, 10 },  // Durotar
        { 215,  1, 10 },  // Mulgore
        {  40, 10, 20 },  // Westfall
        {  38, 10, 20 },  // Loch Modan
        { 148, 10, 20 },  // Darkshore
        { 130, 10, 20 },  // Silverpine Forest
        {  17, 10, 25 },  // The Barrens
        {  44, 15, 25 },  // Redridge Mountains
        { 406, 15, 27 },  // Stonetalon Mountains
        {  10, 18, 30 },  // Duskwood
        { 331, 18, 30 },  // Ashenvale
        { 267, 20, 30 },  // Hillsbrad Foothills
        {  11, 20, 30 },  // Wetlands
        { 400, 24, 35 },  // Thousand Needles
        {  36, 30, 40 },  // Alterac Mountains
        {  45, 30, 40 },  // Arathi Highlands
        { 405, 30, 40 },  // Desolace
        {  33, 30, 45 },  // Stranglethorn Vale
        {   3, 35, 45 },  // Badlands
        {   8, 35, 45 },  // Swamp of Sorrows
        {  15, 35, 45 },  // Dustwallow Marsh
        { 440, 40, 50 },  // Tanaris
        { 357, 40, 50 },  // Feralas
        {  51, 43, 50 },  // Searing Gorge
        {  47, 45, 50 },  // The Hinterlands
        {  16, 45, 55 },  // Azshara
        {   4, 45, 55 },  // Blasted Lands
        { 361, 48, 55 },  // Felwood
        { 490, 48, 55 },  // Un'Goro Crater
        {  46, 50, 58 },  // Burning Steppes
        {  28, 51, 58 },  // Western Plaguelands
        { 139, 53, 60 },  // Eastern Plaguelands
        { 618, 55, 60 },  // Winterspring
        {  41, 55, 60 },  // Deadwind Pass
        {1377, 55, 60 },  // Silithus
    } };

    ClassicZoneBand const* FindClassicZoneBand(uint32 zoneId)
    {
        for (ClassicZoneBand const& band : kClassicZoneBands)
            if (band.zoneId == zoneId)
                return &band;
        return nullptr;
    }

    // The cap of a starter zone. Zones topping out here are the ones a brand new
    // character is expected to be in, so a bot reborn into one starts at level 1.
    constexpr uint8 kStarterZoneTopLevel = 10;

    // Zones opening at or above this are left to the veterans rather than to locals.
    constexpr uint8 kVeteranBandMinLevel = 55;


    // Classic zone level caps.
    constexpr std::array<GuardianZone, 38> kGuardianZones = { {
        { 12, 10 },   // Elwynn Forest
        { 14, 10 },   // Durotar
        { 85, 10 },   // Tirisfal Glades
        { 141, 10 },  // Teldrassil
        { 215, 10 },  // Mulgore
        { 1, 10 },    // Dun Morogh
        { 40, 20 },   // Westfall
        { 130, 20 },  // Silverpine Forest
        { 148, 20 },  // Darkshore
        { 38, 20 },   // Loch Modan
        { 17, 25 },   // The Barrens
        { 44, 25 },   // Redridge Mountains
        { 406, 27 },  // Stonetalon Mountains
        { 331, 30 },  // Ashenvale
        { 10, 30 },   // Duskwood
        { 267, 30 },  // Hillsbrad Foothills
        { 11, 30 },   // Wetlands
        { 400, 35 },  // Thousand Needles
        { 36, 40 },   // Alterac Mountains
        { 45, 40 },   // Arathi Highlands
        { 405, 40 },  // Desolace
        { 33, 45 },   // Stranglethorn Vale
        { 3, 45 },    // Badlands
        { 8, 45 },    // Swamp of Sorrows
        { 15, 45 },   // Dustwallow Marsh
        { 357, 50 },  // Feralas
        { 440, 50 },  // Tanaris
        { 47, 50 },   // The Hinterlands
        { 51, 50 },   // Searing Gorge
        { 16, 55 },   // Azshara
        { 361, 55 },  // Felwood
        { 490, 55 },  // Un'Goro Crater
        { 4, 58 },    // Blasted Lands
        { 46, 58 },   // Burning Steppes
        { 28, 58 },   // Western Plaguelands
        { 618, 60 },  // Winterspring
        { 139, 60 },  // Eastern Plaguelands
        { 1377, 60 }, // Silithus
    } };

    std::mutex g_GuardianLock;
    std::unordered_map<uint64, uint32> g_GuardianZoneByGuid; // guid -> zone index
    std::unordered_set<uint32> g_GuardianTakenSlots;
    bool g_GuardianPostsLoaded = false;

    void CompleteEligibleClassQuests(Player* bot); // defined with the class-quest cache below

    // Zone id the bot guards, or 0. Posts outlive the setting that created them
    // (they are persisted), so the CURRENT slot count decides who still counts as
    // a guardian - otherwise switching the feature off would leave stale posts
    // steering relocation, hunting, rebirth and the reset command forever.
    uint32 GetGuardianZoneId(uint64 botRawGuid)
    {
        uint32 const totalSlots = g_PveConfig.zoneGuardiansPerZone * uint32(kGuardianZones.size());
        if (!totalSlots)
            return 0;

        std::lock_guard<std::mutex> guard(g_GuardianLock);
        auto itr = g_GuardianZoneByGuid.find(botRawGuid);
        if (itr == g_GuardianZoneByGuid.end() || itr->second >= totalSlots)
            return 0;

        // A post in a zone where a fight between people cannot happen is not a
        // post: the guardian can never be attacked and can never attack, so it is
        // scenery with its experience frozen. Disowning it here releases any bot
        // already holding such a post back into the ordinary population, without
        // needing the persisted table to be rewritten.
        uint32 const zoneId = kGuardianZones[itr->second % kGuardianZones.size()].zoneId;
        if (!BarracksHardcore::IsOpenWorldPvpZone(zoneId))
            return 0;

        return zoneId;
    }

    // A zone owned by the other faction can never be reached: the relocation
    // executor screens FactionGroupMask, so a wrong-faction claim would strand
    // the bot outside its post forever, retrying every tick.
    bool IsGuardianZoneAllowedForBot(Player const* bot, uint32 zoneId)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
        if (!zone || !zone->FactionGroupMask)
            return true;
        if (bot->GetTeamId() == TEAM_ALLIANCE && zone->FactionGroupMask == 4)
            return false;
        if (bot->GetTeamId() == TEAM_HORDE && zone->FactionGroupMask == 2)
            return false;
        return true;
    }

    // Posts are PERSISTED: an in-memory-only, first-come assignment reshuffles
    // every restart, and a bot that lands in a lower-capped zone would be
    // GiveLevel'd DOWNWARD - which resets its talents and strands it in gear it
    // can no longer wear, permanently, since guardians are excluded from both
    // rebirth and the reset command.
    void LoadGuardianPostsOnce()
    {
        std::lock_guard<std::mutex> guard(g_GuardianLock);
        if (g_GuardianPostsLoaded)
            return;
        g_GuardianPostsLoaded = true;

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS playerbot_zone_guardian ("
            "guid BIGINT UNSIGNED NOT NULL PRIMARY KEY, slotIndex INT UNSIGNED NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8");

        if (QueryResult result = CharacterDatabase.Query("SELECT guid, slotIndex FROM playerbot_zone_guardian"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint64 const rawGuid = fields[0].GetUInt64();
                uint32 const slotIndex = fields[1].GetUInt32();
                g_GuardianZoneByGuid[rawGuid] = slotIndex;
                g_GuardianTakenSlots.insert(slotIndex);
            } while (result->NextRow());

            TC_LOG_INFO("playerbots.pve", "Loaded {} standing zone guardian posts.", uint32(g_GuardianZoneByGuid.size()));
        }
    }

    // Ordinary bots hunt people too, each on its own schedule. Aggression decides
    // how long one will tolerate peace before travelling to a player and starting
    // something, so a fleet drifts toward the players over time instead of
    // ignoring them - and the rogues arrive long before the priests do.
    //
    // Guardians are excluded: they have their own, keener schedule with escalation.
    void MaybeHuntPlayersByAggression(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        if (!cfg.aggressionMaxMinutes || GetGuardianZoneId(bot->GetGUID().GetRawValue()))
            return;

        if (!bot->IsAlive() || bot->IsInCombat() || state.engaged || !state.masterGuid.IsEmpty() ||
            state.journeyActive || state.errandKind != PveErrandKind::None)
            return;

        PveTimePoint const now = PveClock::now();
        if (now < state.nextGuardianApproachAt)
            return;

        state.nextGuardianApproachAt = now + std::chrono::seconds(60);

        // Seed on first sight rather than leave it at the epoch, or every bot comes
        // up already starving after a restart and the whole fleet ports at once.
        if (state.lastPlayerFightAt == PveTimePoint())
            state.lastPlayerFightAt = now;

        // (The victim stamp that used to sit here was unreachable - the guard
        // above returns whenever the bot is in combat. RunSlowTick does it now.)

        int64 const idleMinutes = std::chrono::duration_cast<std::chrono::minutes>(
            now - state.lastPlayerFightAt).count();
        // Recently beaten by a person: leave it alone until the sting wears off.
        if (PveClock::now() < state.timidUntil)
            return;

        // And not while hurt. Crossing a zone to open on somebody at a third
        // health is how a bot throws itself away; the rest pass tops it up first
        // and it comes looking again on a later cycle.
        if (!ReadyToFightPlayers(bot, cfg))
            return;

        // And never in a zone where a fight between people cannot happen at all.
        // Travelling across a starter zone to reach somebody you can neither
        // attack nor be attacked by is not aggression, it is a bot walking in a
        // straight line at a stranger.
        if (!BarracksHardcore::IsOpenWorldPvpZone(bot->GetZoneId()))
            return;

        // Being near a person IS the contact this clock counts toward, so it
        // resets on PROXIMITY rather than only on a fight.
        //
        // Checked before the idle gate on purpose. It used to sit below it and
        // only bail, which left a bot that had been standing beside somebody the
        // whole time still reading as overdue - so the moment they walked away
        // it teleported off looking for a fight it had just been having. The
        // clock now zeroes while anybody is close, and only starts running once
        // the bot is genuinely on its own.
        //
        // Once a minute per bot against a snapshot that is already in memory, so
        // moving it above the gate costs nothing worth measuring.
        HumanSpot nearest;
        float nearestDistance = 0.0f;
        if (FindNearestHumanSpot(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
            bot->GetPositionZ(), nearest, nearestDistance, bot->GetZoneId(), bot) &&
            nearestDistance <= g_PveConfig.aggressionResetYards)
        {
            state.lastPlayerFightAt = now;
            return;
        }

        if (idleMinutes < int64(AggressionIdleMinutes(bot, cfg)))
            return;

        // Only ever within the zone the bot is ALREADY in. Travelling to find a
        // fight is repositioning, not migration: a bot that crosses the world has
        // abandoned wherever it was grinding, and a zone is easily wider than the
        // 240 yards that triggers this, so there is plenty to do without leaving.
        HumanSpot destination;
        if (!PickHumanSpotInZone(bot, bot->GetZoneId(), destination))
            return;   // nobody in this bot's own zone

        // Count the trip as its fight for this cycle, so a bot that travels and
        // finds nothing does not re-port every minute afterwards.
        state.lastPlayerFightAt = now;

        TC_LOG_INFO("playerbots.pve", "Bot {} (aggression {}) goes looking for a fight after {}m.",
            bot->GetName(), GetBotAggression(bot), idleMinutes);

        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        // Teleport only to the edge of the encounter. The executor also clamps this
        // defensively, but asking for the real policy here keeps diagnostics/logs
        // honest: the bot lands at 210y, then walks the rest under normal class AI.
        g_PendingGuardianTeleports[bot->GetGUID().GetRawValue()] =
        { destination.Guid.GetRawValue(), PvePlayerTeleportMinimumDistance };
    }

    void RunZoneGuardianTick(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        LoadGuardianPostsOnce();

        uint64 const botRawGuid = bot->GetGUID().GetRawValue();
        bool const flaggedNoXp = bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
        uint32 const totalSlots = cfg.zoneGuardiansPerZone * uint32(kGuardianZones.size());
        // Companions serve a human; they hold no post.
        bool const eligible = state.masterGuid.IsEmpty() && !playerbot::PveManager::IsPvpOnlyBot(bot);

        uint32 slotIndex = 0;
        bool assigned = false;
        bool freshlyAssigned = false;
        {
            std::lock_guard<std::mutex> guard(g_GuardianLock);
            auto itr = g_GuardianZoneByGuid.find(botRawGuid);
            if (itr != g_GuardianZoneByGuid.end() && itr->second < totalSlots)
            {
                assigned = true;
                slotIndex = itr->second;
            }
            else if (itr == g_GuardianZoneByGuid.end() && eligible)
            {
                // Pick the best free post this bot can actually hold: its own
                // faction's ground, never one that would cost it levels, and
                // PREFERABLY on the continent it is already standing on. A post
                // across the sea can only ever be reached by teleport, which is
                // deliberately refused while a real player is watching - so a
                // cross-continent guardian just stands there looking broken.
                uint32 bestCandidate = totalSlots;
                uint32 bestScore = 0;
                for (uint32 candidate = 0; candidate < totalSlots; ++candidate)
                {
                    if (g_GuardianTakenSlots.count(candidate))
                        continue;

                    GuardianZone const& candidateZone = kGuardianZones[candidate % kGuardianZones.size()];
                    if (candidateZone.maxLevel < bot->GetLevel())
                        continue;
                    if (!BarracksHardcore::IsOpenWorldPvpZone(candidateZone.zoneId))
                        continue;   // starter zones never arm; the slot would be wasted
                    if (!IsGuardianZoneAllowedForBot(bot, candidateZone.zoneId))
                        continue;

                    bool sameContinent = false;
                    if (AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(candidateZone.zoneId))
                        sameContinent = zoneEntry->ContinentID == bot->GetMapId();

                    // Same continent first, then the post closest to the level
                    // the bot already has.
                    uint32 const score = (sameContinent ? 0u : 1000u) +
                        uint32(candidateZone.maxLevel - bot->GetLevel());
                    if (bestCandidate == totalSlots || score < bestScore)
                    {
                        bestCandidate = candidate;
                        bestScore = score;
                    }
                }

                if (bestCandidate < totalSlots)
                {
                    slotIndex = bestCandidate;
                    g_GuardianZoneByGuid[botRawGuid] = bestCandidate;
                    g_GuardianTakenSlots.insert(bestCandidate);
                    assigned = true;
                    freshlyAssigned = true;
                }
            }
        }

        if (!assigned)
        {
            // Not a guardian: shed a stale frozen-XP flag so the bot resumes
            // leveling. This is the ONLY code that clears that flag, so it must
            // run for every bot - including when the feature is switched off.
            if (flaggedNoXp)
                bot->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            return;
        }

        GuardianZone const& zone = kGuardianZones[slotIndex % kGuardianZones.size()];

        // Holding a post means frozen XP, whether the post was claimed just now or
        // restored from the table on a later uptime: a guardian whose flag was
        // cleared meanwhile (an admin, a character reset) would otherwise level
        // past its zone forever.
        if (!flaggedNoXp)
            bot->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);

        // The post's level, held in BOTH directions and on every tick rather
        // than only when the post is claimed.
        //
        // A guardian can end up above its post without ever being mis-assigned:
        // the post is stored as an index into kGuardianZones, so adding or
        // reordering an entry moves every stored guardian to another zone, and
        // the level test that runs at assignment never runs again. Checking
        // every tick makes that drift self-healing however it happened.
        uint8 const postCeiling = GuardianPostCeiling(zone);
        if (bot->GetLevel() > postCeiling)
        {
            // QUEUED, never done here. Bringing a guardian down needs the full
            // re-kit rather than a bare GiveLevel, and that teleports the bot and
            // wipes its PveBotState - while this function is running on the MAP
            // thread inside Player::Update, holding a reference to exactly that
            // state. Doing it inline crashed the realm in a loop: the tick around
            // this call carried on using the wiped state and died in the grind
            // scan, a null dereference with nothing about it pointing at
            // guardians. Every other caller of that reset runs on the world
            // thread behind a queue, and so does this one.
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingGuardianDemotions[botRawGuid] = postCeiling;
            return;
        }

        // Upward is safe at any time and costs nothing when already there, so it
        // is no longer gated on a fresh assignment either - a guardian below its
        // post cannot climb out on its own, XP being frozen the moment it takes
        // one.
        if (bot->GetLevel() < postCeiling)
        {
            bot->GiveLevel(postCeiling);
            bot->SetUInt32Value(PLAYER_XP, 0);
        }

        if (freshlyAssigned)
        {
            CharacterDatabase.PExecute("REPLACE INTO playerbot_zone_guardian (guid, slotIndex) VALUES ({}, {})",
                botRawGuid, slotIndex);
            // Guardians never travel for class quests, so their kit spells
            // (Tame Beast, demon summons, stances, totems) are granted as if
            // the chains had been walked. XP is already frozen at this point,
            // so the quest rewards cannot push the level.
            CompleteEligibleClassQuests(bot);
            TC_LOG_INFO("playerbots.pve", "Bot {} is now the level-{} guardian of zone {}.",
                bot->GetName(), zone.maxLevel, zone.zoneId);
        }

        // A guardian caught outside its zone gets pulled home by the normal
        // relocation machinery (which honors the guardian constraint). Supply
        // runs and death relocations legitimately teleport bots across the
        // world, so this is the leash that always brings them back - but it
        // waits out a grace period first. Arrival and errand-start are NOT
        // atomic: the errand scan only runs every 15s, so a leash that fired on
        // the next 750ms tick would teleport the bot home before it ever reached
        // the vendor it travelled for, forever.
        // A guardian serving a human keeps its post but not its leash: dragging a
        // summoned companion away from its master every two minutes would make it
        // useless as a companion.
        // Go to the people. A guardian sitting in an empty zone is doing nothing
        // for anyone, so it closes the gap to the nearest real player on its map
        // and hunts from there. The position comes from the snapshot above, so no
        // searching happens on the map thread at all.
        HumanSpot nearestHuman;
        float humanDistance = 0.0f;
        bool const haveHuman = cfg.guardianPlayerApproachYards > 0.0f &&
            FindNearestHumanSpot(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), nearestHuman, humanDistance, zone.zoneId, bot);
        bool const nearAHuman = haveHuman && humanDistance <= cfg.guardianPlayerApproachYards;

        // A guardian with nobody near it goes to somebody. Walking when that is
        // possible, teleporting when it is not - walking cannot cross a map at all,
        // so a guardian whose zone is empty would otherwise never arrive. Torcarn
        // standing alone in his zone is the failure this exists to end.
        //
        // Teleport in from beyond 240 yards; land at 210, which is outside the
        // 200 yard sight range so the port itself is never witnessed. The trigger
        // sits above the drop on purpose - a guardian dropped at 210 must not
        // immediately qualify for another teleport, or it would port forever.
        constexpr float kGuardianTeleportTriggerYards = 240.0f;
        constexpr float kGuardianDropYards = 210.0f;
        // Idle minutes past the escalation threshold needed to reach the twenty
        // yard floor, at six yards closer per minute.
        constexpr int64 kGuardianFullEscalationMinutes = int64((kGuardianDropYards - 20.0f) / 6.0f) + 1;

        // Escalation. A guardian that has not had a fight with an actual person for
        // a while stops being polite about where it lands, closing in a little more
        // each minute until it is arriving right on top of somebody. The clock is
        // reset by any fight with a real player, so an active hunter never
        // escalates - only a bored one does.
        //
        // A guardian that has never fought anybody starts STARVING, not fed.
        // Seeding this to "now" made a freshly posted guardian maximally polite: it
        // teleported to two hundred and ten yards, which is deliberately outside
        // the two hundred yard sight range, so a player entering the zone never saw
        // it arrive and frequently never saw it at all. First contact is the moment
        // a guardian matters most - it should be standing on you, and only become
        // polite AFTER it has actually fought somebody and been reset by it.
        //
        // Only guardians are seeded this way. The ordinary aggression hunt keeps
        // its own "seed to now" so the whole fleet does not port at once on a
        // restart; it returns before this function for anyone holding a post.
        if (state.lastPlayerFightAt == PveTimePoint())
            state.lastPlayerFightAt = PveClock::now() -
            std::chrono::minutes(cfg.guardianEscalateAfterMinutes + kGuardianFullEscalationMinutes);

        if (Unit* currentVictim = bot->GetVictim())
            if (Player const* victimPlayer = currentVictim->ToPlayer())
                if (!playerbot::IsManagedRandomBot(victimPlayer))
                    state.lastPlayerFightAt = PveClock::now();

        int64 const idleMinutes = std::chrono::duration_cast<std::chrono::minutes>(
            PveClock::now() - state.lastPlayerFightAt).count();
        int64 const hungryMinutes = std::max<int64>(0, idleMinutes - int64(cfg.guardianEscalateAfterMinutes));

        // Hunger may make a guardian seek players sooner, but never makes the
        // teleport itself more aggressive. Every player-directed landing stays at
        // least 210y away; visible movement closes the rest.
        float const dropDistance = std::max(PvePlayerTeleportMinimumDistance, kGuardianDropYards);

        if (eligible && !nearAHuman && !bot->IsInCombat() && !state.engaged &&
            PveClock::now() >= state.timidUntil &&
            !state.journeyActive && state.errandKind == PveErrandKind::None &&
            cfg.guardianPlayerApproachYards > 0.0f &&
            PveClock::now() >= state.nextGuardianApproachAt)
        {
            state.nextGuardianApproachAt = PveClock::now() + std::chrono::seconds(30);

            if (haveHuman && humanDistance <= kGuardianTeleportTriggerYards)
            {
                // Inside the trigger: walk the rest of the way. Visible travel
                // always beats a port.
                StartWalkedJourney(state, bot->GetMapId(), nearestHuman.X, nearestHuman.Y,
                    nearestHuman.Z, 0, humanDistance);
            }
            else if (HumanSpot destination; PickHumanSpotInZone(bot, zone.zoneId, destination))
            {
                // Beyond 240 yards but still inside its own zone - a large zone is
                // easily wider than that. Nobody in the zone means no teleport at
                // all: a guardian holds its ground rather than abandoning it.
                if (hungryMinutes > 0)
                    TC_LOG_INFO("playerbots.pve", "Guardian {} has had no player fight for {}m; closing to {:.0f}y.",
                        bot->GetName(), idleMinutes, dropDistance);

                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingGuardianTeleports[botRawGuid] = { destination.Guid.GetRawValue(), dropDistance };
            }
        }

        // A guardian anywhere near a player is where it should be, whatever the zone
        // says - the leash below would otherwise drag it home mid-fight.
        //
        // This uses the TELEPORT TRIGGER range, not the approach range, and that
        // distinction matters: a guardian is deliberately dropped at 210 yards,
        // which is outside the 200 yard approach test. Judging the leash by that
        // test meant a guardian teleported to a player was relocated home 120
        // seconds later - walking away from the person it had just been sent to
        // meet, and quietly undoing the whole feature.
        bool const withinPlayerReach = haveHuman && humanDistance <= kGuardianTeleportTriggerYards;

        if (bot->GetZoneId() != zone.zoneId && eligible && !withinPlayerReach)
        {
            // Break off the chase AT THE BORDER, rather than only walking home
            // once it is over.
            //
            // The leash below waits for the bot to be out of combat, and a
            // guardian that has chased somebody across the line is in combat for
            // exactly as long as the chase lasts - so it could follow a player
            // out of its zone and keep following, which is what a guardian is
            // least supposed to do. The zone filter on the human search is what
            // makes this reachable: the person it is chasing is no longer IN the
            // post zone, so they stop counting as a reason to be here.
            //
            // Dropping the engagement is what ends the pursuit. Being attacked is
            // a different question and is still answered the same way - the
            // defensive paths deliberately never consult the engagement registry,
            // so a guardian dragged over the line and beaten on still fights
            // back. It simply stops doing the chasing.
            if (state.engaged || bot->GetVictim())
            {
                state.engaged = false;
                playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
                bot->AttackStop();
            }

            PveTimePoint const now = PveClock::now();
            if (state.guardianOutOfZoneSince == PveTimePoint())
                state.guardianOutOfZoneSince = now;

            if (now - state.guardianOutOfZoneSince >= std::chrono::seconds(120) &&
                !bot->IsInCombat() && !state.journeyActive && state.errandKind == PveErrandKind::None)
            {
                state.guardianOutOfZoneSince = now;
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingGrindRelocations.insert(botRawGuid);
            }
        }
        else
            state.guardianOutOfZoneSince = PveTimePoint();
    }

    std::mutex g_GrindSpotLock;
    std::unordered_map<uint8, std::vector<GrindSpot>> g_GrindSpotsByLevel;
    // One copy of every accepted spawn cluster keyed by its actual zone. The
    // stuck watchdog uses this to stay in the bot's current zone even when none
    // of that zone's creatures fall inside the bot's preferred level bracket.
    std::unordered_map<uint32, std::vector<GrindSpot>> g_GrindSpotsByZone;
    // zoneId -> how many grind clusters the zone has in total. Compared against
    // how many of them serve a given level, which is what actually answers "is
    // there anything here for me".
    std::unordered_map<uint32, uint32> g_ZoneSpotCount;
    // zoneId -> the level the zone's content tops out at, taken as the 80th
    // percentile of its clusters so one stray elite or rare cannot pass a whole
    // zone off as high level. This is what answers "have I outgrown this place",
    // which is a different question from "is anything here still killable".
    std::unordered_map<uint32, uint8> g_ZoneTopLevel;
    std::vector<uint32> g_RebirthZones;

    // Class-balanced home-zone assignment for ordinary zone-local bots.
    //
    // The old mapping was simply guid % zoneCount. That spread the fleet overall,
    // but class never entered the decision: one zone could randomly end up with
    // four warriors and no priest while another had the opposite problem.
    //
    // This map is built from the COMPLETE configured bot roster in characters,
    // grouped by class and sorted by GUID. Each class is then round-robin spread
    // over every eligible rebirth zone. Therefore, for any one class, the number
    // assigned to any two zones differs by at most one. The starting point for the
    // next class advances by the previous class's remainder, which also spreads
    // the unavoidable "extra" members across zones instead of stacking every
    // class's remainder into the first few zones.
    //
    // The map is immutable after its one-time startup build (newly-created bots
    // are assigned lazily to the least-populated zone for their class), so reads
    // from map threads are stable. A roster/config change takes effect on restart.
    std::mutex g_LocalZoneLock;
    std::unordered_map<uint64, uint32> g_LocalZoneByGuid;
    std::unordered_map<uint8, std::vector<uint32>> g_LocalClassZoneCounts;
    std::atomic<bool> g_LocalZoneAssignmentsBuilt{ false };

    // Drifter assignments, rebuilt once a second on the world thread. The
    // stored value is the ZONE, already resolved: map threads must not be
    // looking up a human by GUID on every band-fit test.
    //
    // Lock order: this is always taken alone. GetRebirthZoneId takes
    // g_GuardianLock (via GetGuardianZoneId) and g_LocalZoneLock before it,
    // so the builder must finish every one of those lookups BEFORE it takes
    // this one, or the two paths deadlock against each other.
    std::mutex g_DrifterLock;
    std::unordered_map<uint64, uint32> g_DrifterZoneByBot;
    std::unordered_map<uint64, uint64> g_DrifterHumanByBot;

    bool IsDrifter(uint64 botRawGuid)
    {
        std::lock_guard<std::mutex> guard(g_DrifterLock);
        return g_DrifterZoneByBot.count(botRawGuid) != 0;
    }

    bool IsLocalVeteranGuid(uint64 rawGuid)
    {
        uint32 const veterans = g_PveConfig.veteranBotCount;
        if (!veterans)
            return false;

        uint32 const reserved = g_PveConfig.zoneGuardiansPerZone * uint32(kGuardianZones.size()) +
            g_PveConfig.pvpOnlyBotCount;
        uint32 const pool = g_PveConfig.populationTarget > reserved
            ? g_PveConfig.populationTarget - reserved
            : g_PveConfig.populationTarget;
        uint32 const fleet = std::max<uint32>(veterans, pool);

        uint64 value = rawGuid + 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        value ^= value >> 31;

        return uint32(value % fleet) < veterans;
    }

    void BuildLocalZoneAssignmentsOnce()
    {
        if (g_LocalZoneAssignmentsBuilt.load(std::memory_order_acquire) || g_RebirthZones.empty())
            return;

        // Guardian posts are persistent and must be removed from the local pool
        // before class balancing. Loading is idempotent and cheap after first use.
        LoadGuardianPostsOnce();

        std::vector<uint32> configuredAccounts =
            playerbot::RandomBotParticipationManager::GetConfiguredBotAccountIds();
        if (configuredAccounts.empty())
            return; // population config may not have finished bootstrapping yet

        std::vector<uint32> localAccounts;
        localAccounts.reserve(configuredAccounts.size());
        for (uint32 accountId : configuredAccounts)
            if (!std::binary_search(g_PveConfig.pvpOnlyAccountIds.begin(),
                g_PveConfig.pvpOnlyAccountIds.end(), accountId))
                localAccounts.push_back(accountId);

        if (localAccounts.empty())
        {
            std::lock_guard<std::mutex> guard(g_LocalZoneLock);
            g_LocalZoneAssignmentsBuilt.store(true, std::memory_order_release);
            return;
        }

        std::ostringstream accountList;
        for (size_t index = 0; index < localAccounts.size(); ++index)
            accountList << (index ? "," : "") << localAccounts[index];

        std::map<uint8, std::vector<uint64>> guidsByClass;
        std::string const query =
            "SELECT guid, class FROM characters WHERE account IN (" + accountList.str() +
            ") ORDER BY class, guid";

        if (QueryResult result = CharacterDatabase.Query(query.c_str()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const lowGuid = fields[0].GetUInt32();
                uint8 const classId = fields[1].GetUInt8();
                uint64 const rawGuid = ObjectGuid::Create<HighGuid::Player>(lowGuid).GetRawValue();

                // These populations do not cycle through local zones.
                if (GetGuardianZoneId(rawGuid) || IsLocalVeteranGuid(rawGuid))
                    continue;

                guidsByClass[classId].push_back(rawGuid);
            } while (result->NextRow());
        }

        size_t const zoneCount = g_RebirthZones.size();
        std::unordered_map<uint64, uint32> assignments;
        std::unordered_map<uint8, std::vector<uint32>> classCounts;
        size_t remainderCursor = 0;
        uint32 assignedCount = 0;

        for (auto& [classId, guids] : guidsByClass)
        {
            // ORDER BY already gives this, but sorting here makes the stability
            // property explicit even if the query changes later.
            std::sort(guids.begin(), guids.end());

            std::vector<uint32>& counts = classCounts[classId];
            counts.assign(zoneCount, 0);

            size_t const start = remainderCursor % zoneCount;
            for (size_t index = 0; index < guids.size(); ++index)
            {
                size_t const zoneIndex = (start + index) % zoneCount;
                assignments[guids[index]] = g_RebirthZones[zoneIndex];
                ++counts[zoneIndex];
                ++assignedCount;
            }

            // If this class does not divide evenly by the number of zones, its
            // remainder occupied [start, start+r). Begin the next class after that
            // block so every class's extras do not pile into the same zones.
            remainderCursor = (remainderCursor + (guids.size() % zoneCount)) % zoneCount;
        }

        TC_LOG_INFO("playerbots.pve",
            "Built class-balanced band homes for {} bots across {} zones and {} classes.",
            assignedCount, uint32(zoneCount), uint32(guidsByClass.size()));

        // Useful startup proof: each class must have a per-zone spread of <= 1.
        // Log from the local table before publishing it, so map-thread lazy
        // assignments cannot race this diagnostic read.
        for (auto const& [classId, counts] : classCounts)
        {
            if (counts.empty())
                continue;
            auto const [minItr, maxItr] = std::minmax_element(counts.begin(), counts.end());
            TC_LOG_INFO("playerbots.pve",
                "Band class {} distribution: min {} / max {} bots per zone (spread {}).",
                uint32(classId), *minItr, *maxItr, *maxItr - *minItr);
        }

        {
            std::lock_guard<std::mutex> guard(g_LocalZoneLock);
            g_LocalZoneByGuid.swap(assignments);
            g_LocalClassZoneCounts.swap(classCounts);
        }
        g_LocalZoneAssignmentsBuilt.store(true, std::memory_order_release);
    }

    // The zone a bot lives its cycles in. The startup assignment above is
    // class-balanced and deterministic from the complete configured roster.
    //
    // A character created after startup is the only normal miss. Put it in the
    // currently least-populated zone for its class so even that case preserves the
    // same <=1 spread as closely as possible for this uptime.
    uint32 GetLocalHomeZoneId(Player const* bot)
    {
        if (!bot || g_RebirthZones.empty())
            return 0;

        uint64 const rawGuid = bot->GetGUID().GetRawValue();

        // These roles are explicitly outside the local population.
        if (GetGuardianZoneId(rawGuid) || IsLocalVeteranGuid(rawGuid))
            return 0;
        if (bot->GetSession() &&
            std::binary_search(g_PveConfig.pvpOnlyAccountIds.begin(),
                g_PveConfig.pvpOnlyAccountIds.end(), bot->GetSession()->GetAccountId()))
            return 0;

        std::lock_guard<std::mutex> guard(g_LocalZoneLock);

        if (auto itr = g_LocalZoneByGuid.find(rawGuid); itr != g_LocalZoneByGuid.end())
            return itr->second;

        // Startup may ask before population configuration is ready. Until the
        // roster build succeeds, retain the old deterministic mapping rather than
        // mutating an incomplete class-count table.
        if (!g_LocalZoneAssignmentsBuilt.load(std::memory_order_acquire))
            return g_RebirthZones[bot->GetGUID().GetCounter() % g_RebirthZones.size()];

        std::vector<uint32>& counts = g_LocalClassZoneCounts[bot->GetClass()];
        if (counts.size() != g_RebirthZones.size())
            counts.assign(g_RebirthZones.size(), 0);

        uint32 const minimum = *std::min_element(counts.begin(), counts.end());
        size_t const tieStart = size_t(bot->GetClass()) % g_RebirthZones.size();
        size_t chosen = 0;
        for (size_t step = 0; step < g_RebirthZones.size(); ++step)
        {
            size_t const candidate = (tieStart + step) % g_RebirthZones.size();
            if (counts[candidate] == minimum)
            {
                chosen = candidate;
                break;
            }
        }

        ++counts[chosen];
        uint32 const zoneId = g_RebirthZones[chosen];
        g_LocalZoneByGuid[rawGuid] = zoneId;

        TC_LOG_INFO("playerbots.pve",
            "Late-created local bot {} (class {}) assigned to least-populated home zone {}.",
            bot->GetName(), uint32(bot->GetClass()), zoneId);
        return zoneId;
    }

    // Where a bot should be living right now.
    //
    // A drifter lives where its person is, and everything downstream already
    // keys off this one answer - the band-fit gate in the relocation executor,
    // the level cycle on ding, the resurrect correction, and the rebirth drain
    // that performs the actual re-level and teleport. So following somebody
    // needs no machinery of its own: it is this override and the world-thread
    // pass that maintains the table.
    uint32 GetRebirthZoneId(Player const* bot)
    {
        if (!bot)
            return 0;

        {
            std::lock_guard<std::mutex> guard(g_DrifterLock);
            auto itr = g_DrifterZoneByBot.find(bot->GetGUID().GetRawValue());
            if (itr != g_DrifterZoneByBot.end())
                return itr->second;
        }

        return GetLocalHomeZoneId(bot);
    }
    bool g_GrindSpotsBuilt = false;

    // Not simply the declared cap: that is what a zone is SUPPOSED to top out
    // at, and for some zones the spawns disagree. Badlands is declared 45 while
    // its content tops out at 42, and the suitability test rejects anything
    // above the observed top plus one - so a guardian sitting at its own
    // declared cap could never find a thing to hunt there.
    //
    // Falls back to the declared cap while the spot cache is still building.
    // g_GrindSpotsBuilt is the same guard the suitability test uses: a
    // half-filled unordered_map must never be read.
    uint8 GuardianPostCeiling(GuardianZone const& zone)
    {
        uint8 ceiling = zone.maxLevel;
        if (g_GrindSpotsBuilt)
            if (auto itr = g_ZoneTopLevel.find(zone.zoneId); itr != g_ZoneTopLevel.end())
                ceiling = std::min<uint8>(ceiling, uint8(std::min<uint32>(255u, uint32(itr->second) + 1u)));
        return std::max<uint8>(ceiling, 1);
    }

    // World thread only. Mirrors the reference filters: normal-rank lootable
    // mobs with tight level bands and short respawns, excluding service NPCs,
    // friendly/guard factions, critters and unattackable flags; clusters of 3+
    // spawns in a 50yd cell become one candidate spot for bot levels
    // [meanLevel-1 .. meanLevel+3].
    void BuildGrindSpotCacheOnce()
    {
        std::lock_guard<std::mutex> guard(g_GrindSpotLock);
        if (g_GrindSpotsBuilt)
            return;

        struct SpotBucket
        {
            uint32 count = 0;
            GrindSpot spot;
            uint8 meanLevel = 0;
        };
        std::map<std::tuple<uint16, int32, int32>, SpotBucket> buckets;

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            // Configurable per realm: B+ stays on the vanilla continents,
            // L+ can add 530/571. (The cache builds once per process, so a map
            // change needs a restart.)
            if (!std::binary_search(g_PveConfig.relocateMaps.begin(), g_PveConfig.relocateMaps.end(), data.mapId))
                continue;

            if (data.spawntimesecs >= 1000)
                continue;

            CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
            if (!proto || proto->npcflag || !proto->lootid || proto->rank != CREATURE_ELITE_NORMAL)
                continue;

            if (proto->maxlevel < proto->minlevel || proto->maxlevel - proto->minlevel >= 3)
                continue;

            if (proto->type == CREATURE_TYPE_CRITTER || proto->type == CREATURE_TYPE_TOTEM)
                continue;

            if (proto->flags_extra & (CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER))
                continue;

            // Friendly/guard factions the reference excludes explicitly.
            switch (proto->faction)
            {
            case 11: case 71: case 79: case 85: case 188: case 1575:
                continue;
            default:
                break;
            }

            uint32 const unitFlags = proto->unit_flags | data.unit_flags;
            if (unitFlags & (UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE))
                continue;

            uint8 const meanLevel = uint8((proto->minlevel + proto->maxlevel + 1) / 2);
            if (!meanLevel || meanLevel > 83)
                continue;

            auto const key = std::make_tuple(uint16(data.mapId),
                int32(data.spawnPoint.GetPositionX()) / 50, int32(data.spawnPoint.GetPositionY()) / 50);
            SpotBucket& bucket = buckets[key];
            if (!bucket.count)
            {
                bucket.spot = { uint16(data.mapId), data.spawnPoint.GetPositionX(),
                    data.spawnPoint.GetPositionY(), data.spawnPoint.GetPositionZ() };
                bucket.meanLevel = meanLevel;
            }
            ++bucket.count;
        }

        g_GrindSpotsByZone.clear();
        uint32 spotCount = 0;
        for (auto& [key, bucket] : buckets)
        {
            if (bucket.count < 3)
                continue;

            // Zone screen at the source, so forbidden clusters never exist for
            // any travel arm (walk, taxi or teleport) to deliver bots into.
            // The zone id is kept: guardians relocate by it. sMapMgr->GetZoneId
            // creates the base map on demand - FindMap returns null for every
            // map not already loaded at cache-build time, which left almost
            // every spot stamped zone 0 and guardians unable to find their way
            // home.
            bucket.spot.zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, bucket.spot.mapId,
                bucket.spot.x, bucket.spot.y, bucket.spot.z);
            if (IsForbiddenGrindZone(bucket.spot.zoneId))
                continue;

            ++spotCount;
            if (bucket.spot.zoneId)
                g_GrindSpotsByZone[bucket.spot.zoneId].push_back(bucket.spot);
            for (int32 level = int32(bucket.meanLevel) - 1; level <= int32(bucket.meanLevel) + 3; ++level)
                if (level >= 1 && level <= 80)
                    g_GrindSpotsByLevel[uint8(level)].push_back(bucket.spot);
        }

        // How many clusters does each zone hold? Counted once here so the
        // suitability test can ask what SHARE of a zone is huntable at a given
        // level.
        g_ZoneSpotCount.clear();
        g_ZoneTopLevel.clear();
        std::unordered_map<uint32, std::vector<uint8>> zoneLevels;
        for (auto const& [key, bucket] : buckets)
        {
            if (IsForbiddenGrindZone(bucket.spot.zoneId))
                continue;

            ++g_ZoneSpotCount[bucket.spot.zoneId];
            if (bucket.count >= 3)
                zoneLevels[bucket.spot.zoneId].push_back(uint8(std::min<uint32>(bucket.meanLevel, 80)));
        }

        g_RebirthZones.clear();
        for (auto& [zoneId, levels] : zoneLevels)
        {
            if (levels.empty())
                continue;

            std::sort(levels.begin(), levels.end());
            size_t const index = (levels.size() * 4) / 5;             // 80th percentile
            g_ZoneTopLevel[zoneId] = levels[std::min(index, levels.size() - 1)];
        }

        // Eligible rebirth zones are the classic levelling chart, filtered to the
        // ones this realm can actually deliver a bot to: the zone must sit on a
        // scanned continent and must have grind clusters, or a bot would be posted
        // somewhere with nothing to kill.
        for (ClassicZoneBand const& band : kClassicZoneBands)
        {
            // The top of the chart has no local bots. A zone that only opens at
            // fifty-five is five levels from the cap, and the veterans - who make
            // the whole climb once and then stay at sixty - already live up there.
            // Handing those zones to cycling bots as well would put two populations
            // in the same content while the zones below went short.
            if (band.minLevel >= kVeteranBandMinLevel)
                continue;

            AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(band.zoneId);
            if (!zoneEntry || !std::binary_search(g_PveConfig.relocateMaps.begin(),
                g_PveConfig.relocateMaps.end(), zoneEntry->ContinentID))
                continue;

            if (!g_ZoneSpotCount.count(band.zoneId))
                continue;

            g_RebirthZones.push_back(band.zoneId);
        }

        // Sorted so the guid -> zone mapping is stable: a bot must come back to the
        // same zone every cycle, and unordered_map iteration order would reshuffle
        // the whole fleet on every restart.
        std::sort(g_RebirthZones.begin(), g_RebirthZones.end());

        TC_LOG_INFO("playerbots.pve", "Rebirth zones: {} eligible.", g_RebirthZones.size());

        // Publish completion only after every derived table is populated. Readers
        // do not take g_GrindSpotLock, so setting this at function entry exposed a
        // partially-built cache to map-thread suitability checks.
        g_GrindSpotsBuilt = true;

        TC_LOG_INFO("playerbots.pve", "Grind spot cache built: {} clusters across {} level buckets, {} zones counted.",
            spotCount, g_GrindSpotsByLevel.size(), g_ZoneSpotCount.size());
    }

    // Is there still enough here to be worth staying for?
    //
    // Asked as a SHARE of the zone rather than a level range. A range taken from
    // the zone's highest cluster is far too generous: Durotar holds a handful of
    // level 11-12 camps among dozens of level 3-8 ones, so by a min/max reading
    // it "suits" a level 13 - who then spends its time being jumped by level 6s
    // it cannot even choose to fight. What matters is not whether the zone has
    // ANY cluster at the bot's level, but whether it has enough of them.
    bool BotIsInSuitableZone(Player* bot)
    {
        uint32 const zoneId = bot->GetZoneId();
        uint32 const level = bot->GetLevel();

        // The static Classic chart is authoritative in BOTH directions and does
        // not depend on the runtime spawn cache being populated. A level-51 bot in
        // Durotar is wrong, but so is a level-10 bot in Stranglethorn. The previous
        // check only rejected over-level bots, which let under-level bots remain in
        // high zones indefinitely whenever the dynamic cache happened to contain a
        // low-level/custom spawn there.
        for (ClassicZoneBand const& band : kClassicZoneBands)
            if (band.zoneId == zoneId)
            {
                if (level < uint32(band.minLevel))
                    return false;
                if (level > uint32(band.maxLevel) + 1)
                    return false;
                break;
            }

        // While the world thread is building the dynamic cache, the static chart
        // above is all we trust. Never read partially-filled unordered_maps.
        if (!g_GrindSpotsBuilt)
            return true;

        auto totalItr = g_ZoneSpotCount.find(zoneId);
        if (totalItr == g_ZoneSpotCount.end() || !totalItr->second)
            return true; // an uncounted zone is not evidence of anything

        // Has the zone simply run out of level? A starter zone stays full of
        // things a higher level bot is still ALLOWED to kill, so the share test
        // below never fires there - a level 13 in Durotar can always find another
        // level 10 to hit, and would grind grey mobs forever rather than walk to
        // the Barrens. Judge the zone by where its content tops out instead, with
        // a level of grace so bots do not bounce on the boundary.
        auto topItr = g_ZoneTopLevel.find(zoneId);
        if (topItr != g_ZoneTopLevel.end() && level > uint32(topItr->second) + 1)
            return false;

        auto levelItr = g_GrindSpotsByLevel.find(uint8(std::min<uint32>(level, 80)));
        if (levelItr == g_GrindSpotsByLevel.end())
            return false;

        uint32 usable = 0;
        for (GrindSpot const& spot : levelItr->second)
            if (spot.zoneId == zoneId)
                ++usable;

        // A quarter of the zone still worth hunting is enough to stay.
        return usable * 4 >= totalItr->second;
    }

    bool HasHumanPlayerNearby(Player* bot, float radius)
    {
        Map* map = bot->FindMap();
        if (!map)
            return false;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (candidate && candidate != bot && IsHumanPlayer(candidate) && bot->IsWithinDistInMap(candidate, radius))
                return true;
        }

        return false;
    }

    bool HasHumanPlayerNearPosition(Map* map, float x, float y, float radius)
    {
        if (!map)
            return false;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || !IsHumanPlayer(candidate))
                continue;

            float const dx = candidate->GetPositionX() - x;
            float const dy = candidate->GetPositionY() - y;
            if (dx * dx + dy * dy <= radius * radius)
                return true;
        }

        return false;
    }

    // How far a player can see a bot appear. Nothing may LAND inside this of
    // anybody real.
    //
    // 200 yards is the client's own visibility distance, so this is not a
    // comfort margin - it is the line between "a bot walked up" and "a bot
    // materialised", which is the single most immersion-breaking thing the
    // fleet can do. The existing guardian drop already reasons in these terms
    // and lands at 210; the gap this closes is that 210 was measured against
    // ONE player, the dispatch target, and said nothing about the second person
    // standing next to the landing point.
    float TeleportSightYards()
    {
        static float const yards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Centurion.Playerbot.TeleportSightYards", 200.0f));
        return yards;
    }

    // The one question every bot teleport has to ask about its destination.
    // GMs deliberately COUNT here, unlike the gear-drop witness rule: this is
    // about who can SEE it happen, and an operator watching their fleet is the
    // likeliest person on the realm to be looking straight at it.
    bool WouldLandInSightOfAnybody(Map* map, float x, float y)
    {
        float const yards = TeleportSightYards();
        return yards > 0.0f && HasHumanPlayerNearPosition(map, x, y, yards);
    }

    struct VendorSpot
    {
        uint16 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    std::mutex g_VendorSpotLock;
    std::vector<VendorSpot> g_VendorSpots;
    bool g_VendorSpotsBuilt = false;

    // World thread only.
    void BuildVendorSpotCacheOnce()
    {
        std::lock_guard<std::mutex> guard(g_VendorSpotLock);
        if (g_VendorSpotsBuilt)
            return;
        g_VendorSpotsBuilt = true;

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (!std::binary_search(g_PveConfig.relocateMaps.begin(), g_PveConfig.relocateMaps.end(), data.mapId))
                continue;

            CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
            if (!proto || !(proto->npcflag & UNIT_NPC_FLAG_VENDOR))
                continue;

            // Supply runs exist to buy food and water: only vendors that stock
            // them qualify as destinations (mount and tabard vendors do not).
            bool stocksConsumables = false;
            if (VendorItemData const* vendorList = sObjectMgr->GetNpcVendorItemList(data.id))
                for (uint8 slot = 0; slot < vendorList->GetItemCount() && !stocksConsumables; ++slot)
                    if (VendorItem const* vendorItem = vendorList->GetItem(slot); vendorItem && !vendorItem->ExtendedCost)
                        if (ItemTemplate const* itemProto = sObjectMgr->GetItemTemplate(vendorItem->item))
                            stocksConsumables = IsFoodTemplate(itemProto) || IsDrinkTemplate(itemProto);
            if (!stocksConsumables)
                continue;

            // Same zone screen as the grind clusters: no supply runs into the
            // DK intro area or GM Island.
            if (Map* map = sMapMgr->FindMap(data.mapId, 0))
                if (IsForbiddenGrindZone(map->GetZoneId(PHASEMASK_NORMAL, data.spawnPoint.GetPositionX(),
                    data.spawnPoint.GetPositionY(), data.spawnPoint.GetPositionZ())))
                    continue;

            g_VendorSpots.push_back({ uint16(data.mapId), data.spawnPoint.GetPositionX(),
                data.spawnPoint.GetPositionY(), data.spawnPoint.GetPositionZ() });
        }

        TC_LOG_INFO("playerbots.pve", "Vendor spot cache built: {} vendors.", g_VendorSpots.size());
    }

    // ---------------------------------------------------------------------------
    // Class quests: quests restricted to a single class exist to hand out the
    // class's kit (warlock demons, hunter taming, druid forms, warrior stances,
    // shaman totems), and none of it is trainer-taught. Managed bots do NOT travel
    // for these quests: eligible chains are completed/rewarded automatically and
    // their teaching spells are re-applied idempotently after spell resets.
    // ---------------------------------------------------------------------------

    struct ClassQuestSpot
    {
        uint16 mapId;
        float x, y, z;
    };

    struct ClassQuestEntry
    {
        uint32 questId = 0;
        uint32 questLevel = 0;
        std::vector<ClassQuestSpot> giverSpots;
        std::vector<ClassQuestSpot> enderSpots;
    };

    std::mutex g_ClassQuestLock;
    bool g_ClassQuestsBuilt = false;
    std::unordered_map<uint8, std::vector<ClassQuestEntry>> g_ClassQuestsByClass;

    void BuildClassQuestCacheOnce()
    {
        std::lock_guard<std::mutex> guard(g_ClassQuestLock);
        if (g_ClassQuestsBuilt)
            return;
        g_ClassQuestsBuilt = true;

        std::unordered_map<uint32, std::pair<uint8, size_t>> entryIndexByQuest;
        for (auto const& questPair : sObjectMgr->GetQuestTemplates())
        {
            Quest const* quest = &questPair.second;
            uint32 const classes = quest->GetRequiredClasses();
            if (!classes)
                continue;

            uint8 questClass = 0;
            for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
                if (classes == (1u << (cls - 1)))
                    questClass = cls;
            if (!questClass)
                continue;

            // Repeatable-family quests (AQ armor turn-ins, Ravenholdt emblems,
            // the classic-import Sapta rows...) never grant kit spells, and
            // GetQuestRewardStatus is hardwired false for them - so any
            // "complete once" loop over the cache would spin forever (each
            // RewardQuest does a full SaveToDB: this melted the realm once)
            // and travel would re-target them endlessly. Keep them out.
            if (quest->IsRepeatable() || quest->IsDaily() || quest->IsWeekly() ||
                quest->IsMonthly() || quest->IsSeasonal() || quest->IsDFQuest())
                continue;

            auto& list = g_ClassQuestsByClass[questClass];
            entryIndexByQuest[questPair.first] = { questClass, list.size() };
            ClassQuestEntry entry;
            entry.questId = questPair.first;
            entry.questLevel = uint32(std::max<int32>(1, quest->GetQuestLevel()));
            list.push_back(entry);
        }

        // Reverse the creature quest relations onto the class-quest set.
        std::unordered_map<uint32, std::vector<std::pair<uint8, size_t>>> giverQuestsByCreature;
        std::unordered_map<uint32, std::vector<std::pair<uint8, size_t>>> enderQuestsByCreature;
        for (auto const& [creatureEntry, questId] : *sObjectMgr->GetCreatureQuestRelationMapHACK())
            if (auto itr = entryIndexByQuest.find(questId); itr != entryIndexByQuest.end())
                giverQuestsByCreature[creatureEntry].push_back(itr->second);
        for (auto const& creaturePair : sObjectMgr->GetCreatureTemplates())
            for (uint32 questId : sObjectMgr->GetCreatureQuestInvolvedRelations(creaturePair.first))
                if (auto itr = entryIndexByQuest.find(questId); itr != entryIndexByQuest.end())
                    enderQuestsByCreature[creaturePair.first].push_back(itr->second);

        for (auto const& spawnPair : sObjectMgr->GetAllCreatureData())
        {
            // Plain reference, NOT a structured binding: clang (unlike MSVC)
            // refuses to capture structured bindings in lambdas under C++17.
            CreatureData const& data = spawnPair.second;
            auto attach = [&](std::unordered_map<uint32, std::vector<std::pair<uint8, size_t>>> const& byCreature, bool giver)
            {
                auto itr = byCreature.find(data.id);
                if (itr == byCreature.end())
                    return;
                for (auto const& [cls, index] : itr->second)
                {
                    ClassQuestEntry& entry = g_ClassQuestsByClass[cls][index];
                    (giver ? entry.giverSpots : entry.enderSpots).push_back({ uint16(data.mapId),
                        data.spawnPoint.GetPositionX(), data.spawnPoint.GetPositionY(), data.spawnPoint.GetPositionZ() });
                }
            };
            attach(giverQuestsByCreature, true);
            attach(enderQuestsByCreature, false);
        }

        uint32 total = 0;
        for (auto& [cls, list] : g_ClassQuestsByClass)
        {
            std::sort(list.begin(), list.end(), [](ClassQuestEntry const& left, ClassQuestEntry const& right)
            {
                return left.questLevel < right.questLevel;
            });
            total += uint32(list.size());
        }
        TC_LOG_INFO("playerbots.pve", "Class quest cache built: {} single-class quests.", total);
    }

    // Force-rewards every class quest the bot could legitimately take at its
    // current level (class, race, level and chain prerequisites all honored via
    // CanTakeQuest); repeated passes walk chains link by link. Used for zone
    // guardians, which are pinned in place and can never travel to the givers.
    void EnsureRewardedClassQuestSpells(Player* bot)
    {
        BuildClassQuestCacheOnce();

        std::vector<ClassQuestEntry> const* list = nullptr;
        {
            std::lock_guard<std::mutex> guard(g_ClassQuestLock);
            auto itr = g_ClassQuestsByClass.find(bot->GetClass());
            if (itr == g_ClassQuestsByClass.end())
                return;
            list = &itr->second; // immutable once built
        }

        for (ClassQuestEntry const& entry : *list)
        {
            if (!bot->GetQuestRewardStatus(entry.questId))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(entry.questId);
            if (!quest)
                continue;

            // Trinity's canonical relearn path handles RewSpellCast wrappers with
            // SPELL_EFFECT_LEARN_SPELL and is exactly what .reset spells uses.
            bot->LearnQuestRewardedSpells(quest);

            // Some imported classic rows put the teaching wrapper in RewSpell
            // (display spell) instead. The core's bulk relearn helper ignores that
            // field, so repair it explicitly, but only when it really teaches a
            // spell - never replay one-shot reward/item effects on maintenance.
            if (quest->GetRewSpellCast() <= 0 && quest->GetRewSpell() > 0)
            {
                uint32 const rewardSpell = quest->GetRewSpell();
                SpellInfo const* info = sSpellMgr->GetSpellInfo(rewardSpell);
                if (!info)
                    continue;

                bool missingLearnedSpell = false;
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell &&
                        !bot->HasSpell(effect.TriggerSpell))
                    {
                        missingLearnedSpell = true;
                        break;
                    }

                if (missingLearnedSpell)
                    bot->CastSpell(bot, rewardSpell, true);
            }
        }
    }

    // Force-complete/reward every single-class quest the bot is legitimately
    // eligible for at its current level. Existing incomplete class quests are
    // completed too: managed bots receive the unlock, they never perform the
    // objective or travel to a class-quest giver. Repeated passes walk chains as
    // prerequisites become rewarded.
    void CompleteEligibleClassQuests(Player* bot)
    {
        BuildClassQuestCacheOnce();

        std::vector<ClassQuestEntry> const* list = nullptr;
        {
            std::lock_guard<std::mutex> guard(g_ClassQuestLock);
            auto itr = g_ClassQuestsByClass.find(bot->GetClass());
            if (itr == g_ClassQuestsByClass.end())
                return;
            list = &itr->second; // immutable once built
        }

        uint32 completed = 0;
        std::unordered_set<uint32> rewardedNow;
        bool progressed = true;
        for (uint8 pass = 0; pass < 16 && progressed; ++pass)
        {
            progressed = false;
            for (ClassQuestEntry const& entry : *list)
            {
                if (rewardedNow.count(entry.questId) || bot->GetQuestRewardStatus(entry.questId))
                    continue;

                Quest const* quest = sObjectMgr->GetQuestTemplate(entry.questId);
                if (!quest)
                    continue;

                QuestStatus const status = bot->GetQuestStatus(entry.questId);
                if (status == QUEST_STATUS_NONE && !bot->CanTakeQuest(quest, false))
                    continue;

                // Invalid imported reward spell rows must never be fed into
                // RewardQuest, whose RewSpellCast path asserts the spell exists.
                if ((quest->GetRewSpellCast() > 0 && !sSpellMgr->GetSpellInfo(uint32(quest->GetRewSpellCast()))) ||
                    (quest->GetRewSpell() > 0 && !sSpellMgr->GetSpellInfo(uint32(quest->GetRewSpell()))))
                    continue;

                // CompleteQuest is safe even when the quest was never put in a
                // quest-log slot. RewardQuest then marks the permanent rewarded
                // state, which is what prerequisite chains and spell resets use.
                bot->CompleteQuest(entry.questId);
                uint32 const rewardIndex = PickQuestRewardIndex(bot, quest);
                bot->RewardQuest(quest, rewardIndex, bot, false);
                rewardedNow.insert(entry.questId);
                ++completed;
                progressed = true;
            }
        }

        // Idempotent repair is intentional: a custom reset may strip spells while
        // leaving rewarded quests intact, and those bots must regain class kit.
        EnsureRewardedClassQuestSpells(bot);

        if (completed)
            TC_LOG_INFO("playerbots.pve", "Bot {} auto-completed {} class quests/unlocks.",
                bot->GetName(), completed);
    }

    // Map thread (under the per-map decision serialization). Picks the
    // lowest-level actionable class quest and stores the travel target; the
    // world executor moves the bot, the normal errand scan does the talking.
    void TryStartClassQuestTravel(Player* bot, PveBotState& state)
    {
        BuildClassQuestCacheOnce();

        std::vector<ClassQuestEntry> const* list = nullptr;
        {
            std::lock_guard<std::mutex> guard(g_ClassQuestLock);
            auto itr = g_ClassQuestsByClass.find(bot->GetClass());
            if (itr == g_ClassQuestsByClass.end())
                return;
            list = &itr->second; // immutable once built
        }

        for (ClassQuestEntry const& entry : *list)
        {
            if (bot->GetQuestRewardStatus(entry.questId))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(entry.questId);
            if (!quest)
                continue;

            QuestStatus const status = bot->GetQuestStatus(entry.questId);
            std::vector<ClassQuestSpot> const* spots = nullptr;
            if (status == QUEST_STATUS_COMPLETE)
                spots = &entry.enderSpots;
            else if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                spots = &entry.giverSpots;
            else
                continue;
            if (spots->empty())
                continue;

            ClassQuestSpot const* best = nullptr;
            float bestDistance = 0.0f;
            for (ClassQuestSpot const& spot : *spots)
            {
                // Only travel to givers on the realm's allowed continents: the
                // quest data carries expansion-area class quests too, and the
                // teleport arm happily delivered classic bots to Azuremyst.
                if (!std::binary_search(playerbot::PveManager::GetConfig().relocateMaps.begin(),
                    playerbot::PveManager::GetConfig().relocateMaps.end(), uint32(spot.mapId)))
                    continue;

                float const distance = spot.mapId == bot->GetMapId()
                    ? bot->GetDistance(spot.x, spot.y, spot.z)
                    : 1000000.0f + float(spot.mapId);
                if (!best || distance < bestDistance)
                {
                    best = &spot;
                    bestDistance = distance;
                }
            }
            if (!best)
                continue;

            // Close enough that the normal errand scan takes over.
            if (best->mapId == bot->GetMapId() && bestDistance < 150.0f)
                return;

            state.classQuestId = entry.questId;
            state.classQuestMapId = best->mapId;
            state.classQuestX = best->x;
            state.classQuestY = best->y;
            state.classQuestZ = best->z;
            TC_LOG_INFO("playerbots.pve", "Bot {} traveling for class quest {} ({}).",
                bot->GetName(), entry.questId, status == QUEST_STATUS_COMPLETE ? "turn-in" : "pickup");
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingClassQuestTravels.insert(bot->GetGUID().GetRawValue());
            return;
        }
    }

    // ---------------------------------------------------------------------------
    // Gathering professions: each bot takes two of herbalism/mining/skinning
    // (deterministic by guid), learns and ranks them automatically, gathers
    // nodes through the errand system and skins finished corpses.
    // ---------------------------------------------------------------------------

    struct ProfessionTier
    {
        uint32 spellId;
        uint16 requiredSkill;
        uint8 requiredLevel;
    };

    // Classic ladders through Artisan (300) - the fork's realms are 60-capped.
    constexpr std::array<ProfessionTier, 4> kHerbalismTiers = { { { 2366, 0, 1 }, { 2368, 50, 10 }, { 3570, 125, 20 }, { 11993, 200, 35 } } };
    constexpr std::array<ProfessionTier, 4> kMiningTiers = { { { 2575, 0, 1 }, { 2576, 50, 10 }, { 3564, 125, 20 }, { 10248, 200, 35 } } };
    constexpr std::array<ProfessionTier, 4> kSkinningTiers = { { { 8613, 0, 1 }, { 8617, 50, 10 }, { 8618, 125, 20 }, { 10768, 200, 35 } } };

    bool BotHasProfession(Player const* bot, LockType lockType)
    {
        // guid % 3 picks the pair: 0 = herb+skin, 1 = mining+skin, 2 = herb+mining.
        uint32 const pick = bot->GetGUID().GetCounter() % 3;
        switch (lockType)
        {
        case LOCKTYPE_HERBALISM: return pick != 1;
        case LOCKTYPE_MINING:    return pick != 0;
        default:                 return false;
        }
    }

    bool BotSkins(Player const* bot)
    {
        return bot->GetGUID().GetCounter() % 3 != 2;
    }

    void EnsureProfessionTier(Player* bot, std::array<ProfessionTier, 4> const& tiers, uint32 skillId)
    {
        uint16 const skillValue = bot->GetSkillValue(skillId);
        for (ProfessionTier const& tier : tiers)
        {
            if (bot->HasSpell(tier.spellId))
                continue;

            if (bot->GetLevel() < tier.requiredLevel || skillValue < tier.requiredSkill)
                return;

            bot->LearnSpell(tier.spellId, false);
            TC_LOG_INFO("playerbots.pve", "Bot {} learned profession tier spell {}.", bot->GetName(), tier.spellId);
            return; // one tier per pass; the next check picks up the rest
        }
    }

    void EnsureProfessions(Player* bot)
    {
        if (BotHasProfession(bot, LOCKTYPE_HERBALISM))
            EnsureProfessionTier(bot, kHerbalismTiers, SKILL_HERBALISM);
        if (BotHasProfession(bot, LOCKTYPE_MINING))
            EnsureProfessionTier(bot, kMiningTiers, SKILL_MINING);
        if (BotSkins(bot))
            EnsureProfessionTier(bot, kSkinningTiers, SKILL_SKINNING);
    }

    // Gathering skill only rises when a node is actually gathered, and a bot levels
    // far faster than it mines. The result on a live fleet: 150 of 185 miners sat
    // below 65, the tin threshold, so the whole realm gathered nothing but copper
    // and the auction house never saw a second-tier material at all. Hold gathering
    // at the cap for the bot's level, exactly the way weapon skill is held.
    //
    // The rank spells are learned here rather than left to EnsureProfessions,
    // because the skill ceiling IS the rank: raising the value without the rank
    // would clamp straight back down to the old ceiling.
    void MaxOutGatheringSkills(Player* bot)
    {
        struct GatherLine
        {
            uint32 skillId;
            std::array<ProfessionTier, 4> const& tiers;
        };

        GatherLine const lines[] = {
            { SKILL_HERBALISM, kHerbalismTiers },
            { SKILL_MINING,    kMiningTiers },
            { SKILL_SKINNING,  kSkinningTiers },
        };

        // Five per level, the same ladder weapon skill uses, capped at the classic
        // Artisan ceiling. It lines up with the rank requirements by construction:
        // level 10 reaches 50 for Journeyman, level 25 reaches 125 for Expert and
        // level 40 reaches 200 for Artisan, each comfortably past the level gate.
        uint16 const levelCap = uint16(std::min<uint32>(bot->GetLevel() * 5, 300));
        if (!levelCap)
            return;

        for (GatherLine const& line : lines)
        {
            if (!bot->HasSkill(line.skillId))
                continue;

            // Every rank the level allows, in one pass. Tested against the cap we
            // are about to set rather than the current value, so a bot climbs the
            // whole ladder now instead of one rank per check.
            for (ProfessionTier const& tier : line.tiers)
            {
                if (bot->HasSpell(tier.spellId))
                    continue;
                if (bot->GetLevel() < tier.requiredLevel || levelCap < tier.requiredSkill)
                    break;
                bot->LearnSpell(tier.spellId, false);
            }

            uint16 const trained = uint16(bot->GetMaxSkillValue(line.skillId));
            uint16 const cap = std::min<uint16>(levelCap, trained);
            if (!cap || bot->GetSkillValue(line.skillId) >= cap)
                continue;

            bot->SetSkill(line.skillId, bot->GetSkillStep(line.skillId), cap, std::max<uint16>(cap, trained));
        }
    }

    // A herb/ore node this bot can gather right now: chest-type GO whose lock
    // carries a skill case of the matching type within the bot's skill.
    bool IsGatherableNodeFor(Player* bot, GameObject const* go, int32* outRequiredSkill = nullptr)
    {
        GameObjectTemplate const* goInfo = go->GetGOInfo();
        if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST)
            return false;

        LockEntry const* lock = sLockStore.LookupEntry(goInfo->GetLockId());
        if (!lock)
            return false;

        for (uint8 caseIndex = 0; caseIndex < MAX_LOCK_CASE; ++caseIndex)
        {
            if (lock->Type[caseIndex] != LOCK_KEY_SKILL)
                continue;

            LockType const lockType = LockType(lock->Index[caseIndex]);
            if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
                continue;

            if (!BotHasProfession(bot, lockType))
                return false;

            uint32 const skillId = SkillByLockType(lockType);
            if (!skillId || bot->GetSkillValue(skillId) < lock->Skill[caseIndex])
                return false;

            if (outRequiredSkill)
                *outRequiredSkill = int32(lock->Skill[caseIndex]);
            return true;
        }

        return false;
    }

    // Skinning replica of Spell::EffectSkinning for the world-thread loot
    // executor: runs after a corpse is fully looted (which is what sets
    // UNIT_FLAG_SKINNABLE).
    void TrySkinCorpse(Player* bot, Creature* corpse)
    {
        if (!BotSkins(bot) || !bot->HasSkill(SKILL_SKINNING))
            return;

        if (!corpse->HasUnitFlag(UNIT_FLAG_SKINNABLE))
            return;

        CreatureTemplate const* proto = corpse->GetCreatureTemplate();
        if (!proto || proto->GetRequiredLootSkill() != SKILL_SKINNING)
            return;

        uint32 const skillValue = bot->GetSkillValue(SKILL_SKINNING);
        int32 const targetLevel = int32(corpse->GetLevel());
        int32 const requiredValue = targetLevel < 10 ? 0 : (targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5);
        if (int32(skillValue) < requiredValue)
            return;

        corpse->RemoveUnitFlag(UNIT_FLAG_SKINNABLE);
        corpse->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
        bot->SendLoot(corpse->GetGUID(), LOOT_SKINNING);
        if (bot->GetLootGUID() != corpse->GetGUID())
            return;

        Loot* loot = &corpse->loot;
        uint32 const maxSlot = std::min<uint32>(loot->GetMaxSlotInLootFor(bot), 255);
        for (uint32 slot = 0; slot < maxSlot; ++slot)
            bot->StoreLootItem(uint8(slot), loot);

        if (bot->GetLootGUID() == corpse->GetGUID())
            bot->GetSession()->DoLootRelease(corpse->GetGUID());

        // UpdateGatherSkill compares against max-skill caps, so feed it the pure
        // (unbuffed) value; racial/enchant bonuses would stall skill-ups early.
        bot->UpdateGatherSkill(SKILL_SKINNING, bot->GetPureSkillValue(SKILL_SKINNING),
            uint32(std::max(0, requiredValue)), corpse->isElite() ? 2 : 1);
    }

    // After the world-thread executor loots a gather node: the skill-up the real
    // gather spell would have granted (EffectOpenLock's tail), with the same
    // one-per-respawn guard.
    void GrantGatherSkillCredit(Player* bot, GameObject* go)
    {
        GameObjectTemplate const* goInfo = go->GetGOInfo();
        if (!goInfo || goInfo->type != GAMEOBJECT_TYPE_CHEST)
            return;

        LockEntry const* lock = sLockStore.LookupEntry(goInfo->GetLockId());
        if (!lock)
            return;

        for (uint8 caseIndex = 0; caseIndex < MAX_LOCK_CASE; ++caseIndex)
        {
            if (lock->Type[caseIndex] != LOCK_KEY_SKILL)
                continue;

            LockType const lockType = LockType(lock->Index[caseIndex]);
            if (lockType != LOCKTYPE_HERBALISM && lockType != LOCKTYPE_MINING)
                continue;

            uint32 const skillId = SkillByLockType(lockType);
            if (!skillId)
                return;

            if (uint32 const pureSkill = bot->GetPureSkillValue(skillId))
            {
                if (!go->IsInSkillupList(bot->GetGUID()))
                {
                    bot->UpdateGatherSkill(skillId, pureSkill, lock->Skill[caseIndex]);
                    go->AddToSkillupList(bot->GetGUID());
                }
            }
            return;
        }
    }

    // ---------------------------------------------------------------------------
    // Mail collection (world thread): auction wins and system mail arrive by
    // post; a bot empties attachments and gold into its bags, mirroring the
    // take-item/take-money handlers. Emptied mails are left to expire (flagging
    // them deleted would hard-delete any attachment that failed to fit).
    // ---------------------------------------------------------------------------

    void ProcessPendingMailCollections()
    {
        std::unordered_set<uint64> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingMailCollections);
        }

        for (uint64 botRawGuid : drained)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
            if (!bot || !bot->IsInWorld() || bot->GetMails().empty())
                continue;

            bool tookAnything = false;
            uint32 tookItems = 0;
            bool const pvpOnly = playerbot::PveManager::IsPvpOnlyBot(bot);
            time_t const nowTime = GameTime::GetGameTime();
            for (Mail* mail : bot->GetMails())
            {
                if (!mail || mail->state == MAIL_STATE_DELETED || mail->deliver_time > nowTime)
                    continue;

                // Bots don't do COD trades: taking a CODed attachment without
                // paying would silently rob the sender. Leave such mail to
                // expire back to them.
                if (mail->COD)
                    continue;

                if (mail->money)
                {
                    // A PvP-only bot has no use for coin. It never shops the
                    // auction house, never runs a vendor errand and never lists
                    // anything, so banking its old proceeds would only park tens
                    // of thousands of gold somewhere nothing can ever spend it -
                    // which is exactly the state twenty-four of them were found
                    // in. The mail is cleared and the money goes with it.
                    if (pvpOnly)
                    {
                        mail->money = 0;
                        mail->state = MAIL_STATE_CHANGED;
                        tookAnything = true;
                    }
                    else if (bot->ModifyMoney(mail->money, false))
                    {
                        mail->money = 0;
                        mail->state = MAIL_STATE_CHANGED;
                        tookAnything = true;
                    }
                }

                if (mail->HasItems())
                {
                    // Copy: taking an attachment mutates mail->items.
                    std::vector<uint32> attachmentIds;
                    for (MailItemInfo const& info : mail->items)
                        attachmentIds.push_back(info.item_guid);

                    for (uint32 attachmentId : attachmentIds)
                    {
                        Item* item = bot->GetMItem(attachmentId);
                        if (!item)
                            continue;

                        ItemPosCountVec dest;
                        if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                            continue;

                        mail->RemoveItem(attachmentId);
                        mail->removedItems.push_back(attachmentId);
                        mail->state = MAIL_STATE_CHANGED;
                        bot->RemoveMItem(attachmentId);
                        item->SetState(ITEM_UNCHANGED);
                        bot->MoveItemToInventory(dest, item, true);
                        tookAnything = true;
                        ++tookItems;
                    }
                }

                // A mail with nothing left in it is rubbish. Stock TrinityCore
                // only removes one when a player clicks delete, and nobody ever
                // opens a bot's mailbox - so drained rows accumulated without
                // limit: 103,546 of them carrying no money and no attachment,
                // against 3,206 that still held something. The table had grown
                // sevenfold in five days.
                //
                // Only when it is genuinely empty. _SaveMail destroys any item
                // still attached to a mail marked deleted, and an attachment can
                // legitimately still be there when the bot's bags were full.
                if (!mail->money && !mail->COD && !mail->HasItems() &&
                    mail->state != MAIL_STATE_DELETED)
                {
                    mail->state = MAIL_STATE_DELETED;
                    tookAnything = true;
                }
            }

            if (tookAnything)
            {
                bot->m_mailsUpdated = true;
                // _SaveMail is protected; the full save runs it and keeps the
                // mail rows and the moved items consistent in one transaction.
                bot->SaveToDB();
                TC_LOG_INFO("playerbots.pve", "Bot {} collected its mail ({} items).", bot->GetName(), tookItems);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Auction shopping (world thread): browse the bot's faction house for one
    // affordable gear upgrade and buy it out, replicating the handler's buyout
    // branch exactly. The win arrives by mail and the collector above brings it
    // home; the equip pass puts it on.
    // ---------------------------------------------------------------------------

    // ---------------------------------------------------------------------------
    // Auction selling. Bots loot far more than they can wear, and everything they
    // cannot use was simply rotting in their bags. They now list the surplus and
    // UNDERCUT the standing price, so the house has real competing sellers in it
    // instead of one price-setting stocker.
    //
    // World thread only: AuctionHouseObject and the item/inventory moves are the
    // same structures the shopping executor mutates.
    // ---------------------------------------------------------------------------

    // Something the bot owns, cannot use, and can legally part with.
    bool IsAuctionableSurplus(Player* bot, Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return false;

        // The core's own rules for what may be listed at all.
        if (!item->CanBeTraded() || item->IsNotEmptyBag() || item->GetUInt32Value(ITEM_FIELD_DURATION) ||
            proto->HasFlag(ITEM_FLAG_CONJURED) || sAuctionMgr->GetAItem(item->GetGUID().GetCounter()))
            return false;

        // Worthless to everyone: vendor trash goes to the vendor, not the house.
        if (proto->Quality == ITEM_QUALITY_POOR || !proto->SellPrice)
            return false;

        // Kit the bot was handed for its own use. Checked before the class switch
        // because the class of some of these is rewritten from the client DBC.
        if (IsStockedPoisonItem(proto->ItemId))
            return false;

        // Common materials go up in respectable lots, not one at a time. A single
        // linen cloth is noise on the house: it burns a listing, and because the
        // undercut ladder prices against the cheapest STANDING lot it also drags
        // the going rate down for every full stack already up there.
        //
        // Valuable materials are exempt however few the bot holds - calibrated
        // against this realm's own prices, ordinary commodities top out around
        // Mithril Bar at 400 copper a unit while genuine single-item goods start
        // at Black Lotus on 1000, so that is where the line sits.
        if (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_REAGENT)
        {
            playerbot::PveConfig const& auctionCfg = playerbot::PveManager::GetConfig();
            uint32 const maxStack = std::max<uint32>(1, proto->GetMaxStackSize());
            if (maxStack > 1 && uint32(proto->SellPrice) < auctionCfg.auctionValuableUnitCopper)
            {
                uint32 const wantedLot = std::min<uint32>(maxStack, auctionCfg.auctionMinTradeGoodStack);
                if (item->GetCount() < wantedLot)
                    return false;
            }
        }

        // Never sell the tools of the trade: food, water, ammunition, bandages,
        // potions, quest items or keys.
        switch (proto->Class)
        {
        case ITEM_CLASS_CONSUMABLE:
        case ITEM_CLASS_QUEST:
        case ITEM_CLASS_KEY:
        case ITEM_CLASS_PROJECTILE:
            return false;
            // A SPARE container is not a tool, it is dead weight - hoarding them
            // is what filled the packs that stopped two thirds of the fleet from
            // shopping. Genuine upgrades and the last free bag slot are protected
            // by IsSpareContainer; anything else goes to the house.
        case ITEM_CLASS_QUIVER:
        case ITEM_CLASS_CONTAINER:
            if (!IsSpareContainer(bot, item))
                return false;
            break;
        default:
            break;
        }

        // Things you wear or swing have a quality floor: white gear is vendor
        // stock, not auction stock. Nobody bids against the vendor's own shelf
        // for a common sword, so listing one only parks it in the house for two
        // days and forfeits the deposit. Green and better go up; the rest go to
        // the merchant. Note this gate is deliberately narrow - reagents, trade
        // goods, recipes and bags are not gear and are not affected by it.
        if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
        {
            if (proto->Quality < ITEM_QUALITY_UNCOMMON)
                return false;

            // Gear it would rather wear than sell.
            uint16 dest = 0;
            if (bot->CanEquipItem(NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
            {
                ItemTemplate const* equipped = nullptr;
                if (Item const* worn = bot->GetItemByPos(dest))
                    equipped = worn->GetTemplate();
                if (IsEquipUpgrade(bot, proto, equipped, uint8(dest & 255)))
                    return false;
            }
        }

        return true;
    }

    // What to ask for it. The anchor is the item's own value, then the standing
    // competition decides: a seller who ignores the shelf price never sells.
    // What a merchant would pay for the lot, times the configured floor factor.
    //
    // The ask never goes below this, and the same number decides whether listing
    // is worth doing at all - so it is computed in exactly one place. Two copies
    // of the multiplier would drift, and the two rules would then disagree about
    // where the boundary is.
    uint64 VendorPriceFloor(ItemTemplate const* proto, uint32 count)
    {
        float const factor = std::max(0.0f, playerbot::PveManager::GetConfig().auctionVendorFloorFactor);
        return uint64(double(proto->SellPrice) * double(count) * double(factor));
    }

    // What the thing is worth, by the same reckoning the auction house stocker
    // uses (AuctionBotSeller::SetPricesOfItem):
    //   vendor buy price, or failing that the sell price times a per-class
    //   modifier, or failing THAT a value derived from item level and quality -
    //   level squared times quality times a per-subclass modifier.
    // The last branch is what prices anything a vendor never handles; without it a
    // bot would post a raid drop for one copper.
    //
    // This is deliberately free of the seller's price multiplier: it is the item's
    // face value, which the seller marks UP and the buyer caps against. Sharing one
    // definition is the point - a buy ceiling reckoned differently from the sell
    // price would eventually drift into refusing every listing bots themselves post.
    //
    // The figure covers proto->BuyCount units, exactly as a vendor's price does.
    double ComputeItemFaceValue(ItemTemplate const* proto)
    {
        double value = double(proto->BuyPrice);
        if (value <= 0.0)
        {
            if (proto->SellPrice > 0)
                value = double(proto->SellPrice) * AuctionBotSeller::GetSellModifier(proto);
            else
            {
                double const divisor = (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) ? 284.0 : 80.0;
                double const level = proto->ItemLevel ? double(proto->ItemLevel) : 1.0;
                double const quality = proto->Quality ? double(proto->Quality) : 1.0;
                value = level * quality * double(AuctionBotSeller::GetBuyModifier(proto)) * level / divisor;
            }
        }

        return value;
    }

    // The most a bot will ASK for a lot when it undercuts the house.
    //
    // This was once a purchase ceiling too, and is not any more. A hard refusal on
    // price is a cliff: a listing one copper over it is not merely unattractive, it
    // is invisible, with nothing in the game to tell the seller why. The buy side
    // prices an item by weighing what it is worth against what it costs, which
    // penalises an overpriced listing smoothly and in proportion - a bot simply
    // finds something better to spend on, and comes back if nothing else turns up.
    //
    // It still governs what bots CHARGE. The undercut anchor is the cheapest
    // listing on the house including players', so without a cap one person listing
    // a green for ten thousand gold walks the entire fleet's asking prices up to
    // match. Zero disables it. Copper for the whole lot.
    uint64 ComputeSaneAskingPrice(ItemTemplate const* proto, uint32 itemCount)
    {
        playerbot::PveConfig const& cfg = playerbot::PveManager::GetConfig();
        if (!cfg.auctionBuyMaxOverpayPct)
            return std::numeric_limits<uint64>::max();

        // Face value covers BuyCount units; scale it to the size of this lot, the
        // same conversion the seller does in the other direction.
        uint32 const buyCount = std::max<uint32>(1, proto->BuyCount);
        double const lotFace = ComputeItemFaceValue(proto) *
            double(std::max<uint32>(1, itemCount)) / double(buyCount);

        uint64 ceiling = uint64(std::max(1.0, lotFace * double(cfg.auctionBuyMaxOverpayPct) / 100.0));

        // Never refuse a price our own seller would ASK. The buy ceiling and the
        // sell markup are two independent config keys with nothing tying them
        // together, so a raised AuctionPriceMultiplier would otherwise freeze every
        // bot-to-bot sale silently - and with the core's auction stocker disabled,
        // bot listings are very nearly the whole market. Taking the larger of the
        // two makes that deadlock unrepresentable rather than merely documented.
        ceiling = std::max(ceiling, uint64(lotFace * double(std::max(0.01f, cfg.auctionPriceMultiplier))));

        // Same reasoning for the vendor floor the seller clamps up to: an item
        // whose SellPrice dwarfs its BuyPrice (inverted or partial price data, which
        // hand-made items really do carry) would otherwise be listed high by one bot
        // and refused by every other.
        ceiling = std::max(ceiling, VendorPriceFloor(proto, itemCount));

        // Deliberately no "unpriceable" escape hatch: the cascade cannot return zero
        // - its smallest possible output is a fraction of a copper - so a hatch keyed
        // on that would be dead code. The three-way maximum above is what actually
        // protects items the formula prices badly, and containers are exactly that
        // case: a quest-reward bag with no vendor prices and item level 0 values at
        // six copper, so the seller's own ask is the only sane figure available.
        return ceiling;
    }

    uint32 ComputeAuctionBuyout(ItemTemplate const* proto, uint32 count,
        std::unordered_map<uint32, uint32> const& cheapestPerUnit, uint32* outMarketPrice = nullptr)
    {
        double value = ComputeItemFaceValue(proto);

        auto itr = cheapestPerUnit.find(proto->ItemId);
        bool const alreadyOnTheHouse = itr != cheapestPerUnit.end() && itr->second != 0;

        uint64 price = 0;

        if (alreadyOnTheHouse)
        {
            // A standing listing IS the market, and the only input. Taking
            // min(own value, undercut) instead meant the SECOND seller ignored the
            // first entirely and dropped straight back to the item's face value.
            //
            // The step is a FLAT number of copper per unit, not a percentage.
            // A percentage compounds: 5% off a price that was itself 5% off walks
            // a market down geometrically, and the further above vendor value the
            // opening price is, the larger every early step is in absolute terms.
            // A flat step drifts instead, and drifts at the same rate whatever the
            // item is worth.
            //
            // Per unit because that is what the market map holds - a total-based
            // step is not representable once a lot is divided back into a unit
            // price, where integer division would swallow it.
            uint64 const undercut = std::max<uint32>(1, playerbot::PveManager::GetConfig().auctionUndercutCopper);
            uint64 const standingPerUnit = uint64(itr->second);
            uint64 const askPerUnit = standingPerUnit > undercut ? standingPerUnit - undercut : 1;
            price = std::max<uint64>(askPerUnit * count, 1);

            // The anchor is whatever is cheapest on the house, and players list
            // there too - so one player listing a green for ten thousand gold would
            // otherwise walk the whole fleet up to match it. Hold the ask to a sane
            // multiple of the item's own worth instead.
            price = std::min(price, ComputeSaneAskingPrice(proto, count));
        }
        else
        {
            // Opening a market: the item's own value, with headroom for the race
            // that follows. At 5% a step it takes roughly forty-five undercuts to
            // work back down to the unmultiplied value.
            value *= double(std::max(0.01f, playerbot::PveManager::GetConfig().auctionPriceMultiplier));

            // Spread over the units that price covers, exactly as the stocker does.
            uint32 const buyCount = std::max<uint32>(1, proto->BuyCount);
            price = uint64(std::max(1.0, value * count / buyCount));
        }

        // The market price BEFORE the floor is what the caller needs to judge
        // whether listing is worth doing at all - the floor would otherwise mask
        // exactly the case that decision exists for.
        if (outMarketPrice)
            *outMarketPrice = uint32(std::min<uint64>(price, uint64(MAX_MONEY_AMOUNT)));

        // Never ask less than half again what a vendor would hand over. The ask
        // and the decision to list are two different questions: this governs the
        // ask, so a lot that IS worth listing is never posted for less than the
        // merchant would have paid for it.
        price = std::max(price, VendorPriceFloor(proto, count));

        return uint32(std::min<uint64>(price, uint64(MAX_MONEY_AMOUNT)));
    }

    void ProcessPendingAuctionSales()
    {
        std::unordered_set<uint64> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingAuctionSales);
        }

        uint32 sellersLeft = 2; // the full-house scan is the expensive part
        for (auto itr = drained.begin(); itr != drained.end(); ++itr)
        {
            if (!sellersLeft)
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingAuctionSales.insert(itr, drained.end());
                break;
            }

            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(*itr));
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
                continue;

            // AHBot destroys auction proceeds paid to its own characters.
            if (sAuctionBotConfig->IsBotChar(bot->GetGUID().GetCounter()))
                continue;

            AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(bot->GetFaction());
            AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntry(bot->GetFaction());
            if (!auctionHouse || !houseEntry)
                continue;

            // The catch-up pass empties the bags in one go; the steady state posts
            // a few at a time so the house is not flooded by one bot.
            bool catchUp = false;
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, *itr);
                catchUp = state.auctionCatchUpSell;
                state.auctionCatchUpSell = false;
            }
            size_t const listingLimit = catchUp ? 40u : 3u;

            // Identities, not pointers.
            //
            // The loop below vendors items and commits a whole inventory save
            // once per lot, and both of those free Item objects - Item::SaveToDB
            // deletes an ITEM_REMOVED item outright, and _SaveInventory then
            // clears the update queue. A vector of Item* captured before any of
            // that runs goes stale as the loop walks it, and a freed block
            // recycled into some other bot's item is how a bot comes to post an
            // item it has never owned. Every other bulk pass in this file
            // collects positions instead, for exactly this reason; stock's own
            // sell handler re-resolves off the player by GUID immediately before
            // it posts. Do both: keep GUIDs, and re-resolve each time round.
            std::vector<ObjectGuid> surplus;
            ForEachBagItem(bot, [&](Item* item, uint8 /*bag*/, uint8 /*slot*/)
            {
                if (surplus.size() < listingLimit && IsAuctionableSurplus(bot, item))
                    surplus.push_back(item->GetGUID());
            });

            if (surplus.empty())
                continue;

            --sellersLeft;

            // One pass over the house for the going rate of everything at once.
            // The stocker's own listings are NOT competition: they are generated
            // from nothing at a fixed formula, and undercutting them would walk
            // every price down forever against a seller that never runs out.
            // Only real sellers - players and other bots - are undercut.
            std::unordered_map<uint32, uint32> cheapestPerUnit;
            for (auto houseItr = auctionHouse->GetAuctionsBegin(); houseItr != auctionHouse->GetAuctionsEnd(); ++houseItr)
            {
                AuctionEntry const* auction = houseItr->second;
                if (!auction || !auction->buyout || !auction->itemCount)
                    continue;

                if (sAuctionBotConfig->IsBotChar(auction->owner))
                    continue;

                uint32 const perUnit = auction->buyout / auction->itemCount;
                auto existing = cheapestPerUnit.find(auction->itemEntry);
                if (existing == cheapestPerUnit.end() || perUnit < existing->second)
                    cheapestPerUnit[auction->itemEntry] = perUnit;
            }

            for (ObjectGuid const& itemGuid : surplus)
            {
                // Asked again every iteration, because the previous ones have
                // vendored items, moved gold and saved the bags since the list
                // was built. A bot that no longer holds it simply skips it.
                Item* item = bot->GetItemByGuid(itemGuid);
                if (!item || !IsAuctionableSurplus(bot, item))
                    continue;

                ItemTemplate const* proto = item->GetTemplate();
                uint32 const count = item->GetCount();
                uint32 const etime = 12 * HOUR;

                // Playerbots post for free. A deposit is a risk premium for a
                // player who might misprice something; the fleet lists its whole
                // surplus constantly, so all a deposit does is bleed gold out of
                // the simulation on lots that were never going to sell anyway.
                uint32 const deposit = 0;

                uint32 marketPrice = 0;
                uint32 const buyout = ComputeAuctionBuyout(proto, count, cheapestPerUnit, &marketPrice);

                // Has the market fallen below what we would have to ask? The ask
                // never goes under 1.5x vendor price, so whenever the MARKET price
                // is under that floor the bot would be posting above what anyone
                // is currently paying - a listing that sits for twelve hours and
                // then, since unsold bot lots are destroyed at expiry, takes the
                // item and the vendor money with it.
                //
                // So the floor doubles as the threshold: at or above it the item
                // is worth listing, below it the merchant is the better customer.
                // Judged on the MARKET price rather than the ask, because the ask
                // is derived from the floor and comparing the two would be
                // circular.
                uint64 const vendorRevenue = uint64(proto->SellPrice) * count;
                uint64 const vendorFloor = VendorPriceFloor(proto, count);
                if (vendorRevenue && uint64(marketPrice) < vendorFloor)
                {
                    bot->ModifyMoney(int64(vendorRevenue));
                    TC_LOG_INFO("playerbots.pve",
                        "Bot {} vendored {} x{} for {} copper: market {} is under the {} floor.",
                        bot->GetName(), proto->Name1, count, vendorRevenue, marketPrice, vendorFloor);
                    bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                    continue;
                }

                uint32 const startBid = std::max<uint32>(1, uint32(uint64(buyout) * 80 / 100));

                // Asked AGAIN here, immediately before the point of no return.
                //
                // IsAuctionableSurplus already refuses an item that is on the
                // house, but it answered when the candidate list was BUILT, and
                // the list is a snapshot of Item pointers that is then walked
                // while posting. Anything that registers the item in between -
                // another lot in this same pass, another bot holding the very
                // same Item (the realm has 26 items that are in a bag and on the
                // auction house at once, three of them held by a character that
                // is not the auction's owner) - and the stale answer is acted on.
                //
                // AddAItem ASSERTS rather than refusing, so the cost of being
                // wrong is the whole realm: this is the crash at
                // AuctionHouseMgr.cpp AddAItem that has taken Barracks Plus down
                // three times. Skipping one lot is the correct price for that.
                if (sAuctionMgr->GetAItem(item->GetGUID().GetCounter()))
                {
                    TC_LOG_ERROR("playerbots.pve",
                        "Bot {} nearly listed {} (item guid {}) which is ALREADY on the auction house; "
                        "skipped. This item is duplicated between a bag and the house.",
                        bot->GetName(), proto->Name1, item->GetGUID().GetCounter());
                    continue;
                }

                // Out of the bag FIRST, and nothing is listed until it has gone.
                //
                // MoveItemFromInventory takes coordinates, not an item, and is
                // silent when they hold nothing: Player.cpp opens with
                // `if (Item* it = GetItemByPos(bag, slot))` and otherwise just
                // falls through, returning void. Build the auction first and a
                // failed hand-off leaves the lot on the house with the item
                // still in the bag - and then SaveInventoryAndGoldToDB, at the
                // end of this same transaction, walks the update queue the item
                // never left and REPLACEs the character_inventory row that
                // DeleteFromInventoryDB removed five statements earlier. The
                // duplicate commits atomically and looks perfectly healthy on
                // disk. That is how an item comes to be in a bag and on the
                // auction house at once, and the second time a bot picks that
                // item up, AddAItem's ASSERT takes the realm down.
                uint8 const bagSlot = item->GetBagSlot();
                uint8 const invSlot = item->GetSlot();
                if (bot->GetItemByPos(bagSlot, invSlot) != item)
                {
                    TC_LOG_ERROR("playerbots.pve",
                        "Bot {} tried to list {} (item guid {}), but its own recorded position "
                        "(bag {}, slot {}) does not hold it; not listed.",
                        bot->GetName(), proto->Name1, item->GetGUID().GetCounter(), bagSlot, invSlot);
                    continue;
                }

                bot->MoveItemFromInventory(bagSlot, invSlot, true);

                // If it is still there the move did nothing, so nothing has been
                // lost by skipping - the item stays in the bag and is offered
                // again next pass. Bailing here is only ever the no-op path.
                if (bot->GetItemByGuid(itemGuid))
                {
                    TC_LOG_ERROR("playerbots.pve",
                        "Bot {} could not hand {} (item guid {}) to the auction house - it is "
                        "still in the bags after the move; not listed.",
                        bot->GetName(), proto->Name1, item->GetGUID().GetCounter());
                    continue;
                }

                AuctionEntry* auction = new AuctionEntry();
                auction->Id = sObjectMgr->GenerateAuctionID();
                // Same rule the sell handler uses: one shared neutral house when
                // cross-faction trading is on, otherwise the faction's own.
                auction->houseId = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION)
                    ? uint8(AUCTIONHOUSE_NEUTRAL) : uint8(houseEntry->ID);
                auction->itemGUIDLow = item->GetGUID().GetCounter();
                auction->itemEntry = item->GetEntry();
                auction->itemCount = count;
                auction->owner = bot->GetGUID().GetCounter();
                auction->startbid = startBid;
                auction->bidder = 0;
                auction->bid = 0;
                auction->buyout = buyout;
                auction->deposit = deposit;
                auction->etime = etime;
                auction->expire_time = GameTime::GetGameTime() + uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));
                auction->auctionHouseEntry = houseEntry;
                auction->Flags = AUCTION_ENTRY_FLAG_NONE;

                bot->ModifyMoney(-int64(deposit));

                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                item->DeleteFromInventoryDB(trans);
                item->SaveToDB(trans);
                sAuctionMgr->AddAItem(item);
                auctionHouse->AddAuction(auction);
                auction->SaveToDB(trans);
                bot->SaveInventoryAndGoldToDB(trans);
                CharacterDatabase.CommitTransaction(trans);

                TC_LOG_INFO("playerbots.pve", "Bot {} listed {} x{} for {} copper (deposit {}).",
                    bot->GetName(), proto->Name1, count, buyout, deposit);

                // The next listing this pass undercuts its own price too, so a bot
                // dumping duplicates does not stack them all at the same number.
                cheapestPerUnit[proto->ItemId] = std::max<uint32>(1, buyout / count);
            }
        }
    }

    void ProcessPendingAuctionShopping()
    {
        std::unordered_set<uint64> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingAuctionShopping);
        }

        // A full-house scan is the expensive part; two shoppers per world pass,
        // the rest keep their place in line.
        uint32 scansLeft = 2;
        for (auto itr = drained.begin(); itr != drained.end(); ++itr)
        {
            if (!scansLeft)
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingAuctionShopping.insert(itr, drained.end());
                break;
            }

            uint64 const botRawGuid = *itr;
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;

            // The fork destroys auction wins whose buyer is an AHBot character
            // (SendAuctionWonMail's IsBotChar gate) - those bots must not shop.
            if (sAuctionBotConfig->IsBotChar(bot->GetGUID().GetCounter()))
                continue;

            // A win that cannot be pocketed burns gold on mail that rots, so one
            // free slot is always required - the win arrives by mail and has to
            // land somewhere. But a bot with a nearly full pack is precisely the
            // bot that most needs a bigger bag, and skipping the pass outright
            // left it stuck there forever. A tight pack now NARROWS the pass to
            // containers rather than cancelling it.
            uint32 const freeBagSlots = CountFreeBagSlots(bot);
            if (!freeBagSlots)
                continue;

            bool const containersOnly = freeBagSlots < 2;

            AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(bot->GetFaction());
            if (!auctionHouse)
                continue;

            --scansLeft;
            uint32 const botAccountId = bot->GetSession() ? bot->GetSession()->GetAccountId() : 0;

            // The catch-up pass keeps buying until nothing left is an upgrade it
            // can afford; the steady state takes one item and comes back later.
            bool catchUp = false;
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                catchUp = state.auctionCatchUpBuy;
                state.auctionCatchUpBuy = false;
            }

            // One purchase per equipment slot per pass. Auction wins arrive by
            // MAIL, so the equipped item does not change while the pass runs -
            // without this the bot would buy every chestpiece in the house, each
            // one still "an upgrade" over the same empty chest slot, until it ran
            // out of gold.
            // Sixteen: one per equipment slot, which is what slotsBought already
            // enforces and what the comment above always described. The
            // steady-state cap of ONE wasted that guard - a bot could fix a
            // single slot every ten minutes, so filling a set took the better
            // part of three hours of uninterrupted shopping, which never happens
            // because the fleet levels and relocates continuously and the gear
            // falls behind faster than that. It is why a level 60 that has been
            // running for hours still dies wearing two greens.
            //
            // Safe to raise precisely because of slotsBought: each slot can be
            // bought for once per pass, so the "buy every chestpiece in the
            // house" runaway cannot happen. The budget is recomputed each round
            // and shrinks with every purchase, so the pass also stops itself on
            // money long before it stops on this number.
            std::unordered_set<uint8> slotsBought;
            uint32 const maxPurchases = catchUp ? 40u : 16u;

            for (uint32 purchase = 0; purchase < maxPurchases; ++purchase)
            {
                // Recomputed every round: the purse shrinks with each buy. A stripped
                // bot spends everything it has - the usual budget slice is for
                // shopping upgrades, and this bot cannot fight at all.
                uint32 const budget = IsBotStrippedBare(bot)
                    ? bot->GetMoney()
                    : CalculatePct(bot->GetMoney(), g_PveConfig.auctionBuyBudgetPct);

                AuctionEntry* bestAuction = nullptr;
                float bestGain = 0.0f;
                float bestValue = 0.0f;
                uint8 bestSlot = 0;
                for (auto itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
                {
                    AuctionEntry* auction = itr->second;
                    if (!auction || !auction->buyout || auction->buyout > budget)
                        continue;

                    if (auction->owner == bot->GetGUID().GetCounter())
                        continue;

                    Item* item = sAuctionMgr->GetAItem(auction->itemGUIDLow);
                    if (!item)
                        continue;

                    ItemTemplate const* proto = item->GetTemplate();
                    if (!proto)
                        continue;

                    // Bags and quivers are shopped for exactly like gear. The scorer
                    // and the local equip pass already understand both - only this
                    // filter was keeping them out of the house.
                    bool const isContainer = proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER;
                    bool const isGear = proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR;
                    if (!isContainer && !isGear)
                        continue;

                    if (containersOnly && !isContainer)
                        continue;

                    if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                        continue;

                    uint16 dest = 0;
                    if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
                        continue;

                    // Value the bag against the one it would actually replace.
                    if (isContainer && !SelectContainerUpgradeSlot(bot, proto, dest))
                        continue;

                    // And value a one-hander against the hand that actually needs it.
                    if (ShouldRedirectToOffHand(bot, proto, dest))
                        dest = uint16((uint16(INVENTORY_SLOT_BAG_0) << 8) | EQUIPMENT_SLOT_OFFHAND);

                    // Never bench an equipped off hand for a two-hander (same rule
                    // as the local equip pass).
                    if (proto->InventoryType == INVTYPE_2HWEAPON &&
                        bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                        continue;

                    ItemTemplate const* equippedProto = nullptr;
                    if (Item const* equipped = bot->GetItemByPos(dest))
                        equippedProto = equipped->GetTemplate();

                    // Do not shop below your own weight class.
                    //
                    // auctionLevelsBehindPenalty already makes stale gear score
                    // badly, but a penalty is only a thumb on the scale: against
                    // an empty or nearly worthless slot a cheap scrap still wins,
                    // which is how level 40 bots ended up wearing level 10 gear.
                    // This is the floor that a penalty cannot be.
                    //
                    // Nothing is capped on the way UP - gear above the bot's
                    // level is a fine thing to buy, and CanUseItem has already
                    // said it can be worn. Bags and quivers are exempt: item
                    // level says nothing about a bag, slots do.
                    if (isGear && g_PveConfig.auctionMaxItemLevelsBehind &&
                        int32(bot->GetLevel()) - int32(proto->ItemLevel) >
                            int32(g_PveConfig.auctionMaxItemLevelsBehind))
                        continue;

                    // Same scorer as the bag equip pass: spec-aware weapon policy,
                    // armor tier before item level.
                    if (slotsBought.count(uint8(dest & 255)))
                        continue;

                    if (!IsEquipUpgrade(bot, proto, equippedProto, uint8(dest & 255)))
                        continue;

                    // Do not buy a bag the bot could never put on. Replacing a bag
                    // means emptying it first, and a bot whose bags are all full has
                    // nowhere to put the contents - it would pay for an upgrade that
                    // then occupies a slot indefinitely, leaving it worse off than
                    // before. Checked here, before the gold is spent.
                    if (isContainer)
                    {
                        if (Item* occupant = bot->GetItemByPos(dest))
                            if (Bag* occupantBag = occupant->ToBag(); occupantBag && !CanRehomeBagContents(bot, occupantBag))
                                continue;
                    }

                    // Item level says nothing about a bag; slots do. For gear it is
                    // weighed by quality, because nothing else in this path looks at
                    // an item's actual stats - a white and a green of the same level
                    // would otherwise be the same purchase.
                    float const gain = isContainer
                        ? std::max<float>(1.0f, float(proto->ContainerSlots) -
                            float(equippedProto ? equippedProto->ContainerSlots : 0))
                        : std::max<float>(1.0f, EffectiveItemLevel(proto) - EffectiveItemLevel(equippedProto));

                    // What it is worth MINUS what it costs, both in item levels.
                    //
                    // Ranking on gain alone made price a gate and nothing more: among
                    // everything it could afford a bot took the largest jump, so it
                    // would hand over its entire purse for one item level because that
                    // happened to be the biggest number on the house. Nor is the
                    // opposite - cheapest per level - any better on its own: it buys a
                    // one copper trinket for one item level and never improves.
                    //
                    // Price is converted into the currency the gain is already in. The
                    // bot treats its whole budget as worth a fixed number of item
                    // levels, so an item costing half the budget has to be worth half
                    // that many levels before it is worth buying at all. Wealth scales
                    // it automatically: a rich bot will pay real gold for a real
                    // upgrade, a poor one holds out for a bargain, and neither pays a
                    // fortune for a trinket.
                    float const priceInLevels = budget
                        ? float(auction->buyout) * float(g_PveConfig.auctionBudgetWorthLevels) / float(budget)
                        : float(g_PveConfig.auctionBudgetWorthLevels);
                    // How far out of date the item is, in the same currency.
                    //
                    // Gain alone cannot see this: filling an empty slot with a
                    // level 10 trinket is a positive gain for almost no gold, so
                    // it beat a level-appropriate item every time. Counting the
                    // levels it is behind lets the better item win even when it
                    // costs more, without banning the cheap one outright - a bot
                    // with an empty slot and no better offer still takes it.
                    float const levelsBehind = isContainer ? 0.0f :
                        std::max(0.0f, float(bot->GetLevel()) - float(proto->RequiredLevel));
                    float const stalePenalty = levelsBehind * g_PveConfig.auctionLevelsBehindPenalty;

                    float const netValue = gain - priceInLevels - stalePenalty;

                    // Must be worth more than it costs, and worth more than whatever is
                    // already the best offer on the house.
                    if (netValue <= 0.0f || netValue <= bestValue)
                        continue;

                    // No trading with the bot's own account.
                    if (botAccountId && sCharacterCache->GetCharacterAccountIdByGuid(
                        ObjectGuid::Create<HighGuid::Player>(auction->owner)) == botAccountId)
                        continue;

                    bestAuction = auction;
                    bestGain = gain;
                    bestValue = netValue;
                    bestSlot = uint8(dest & 255);
                }

                if (!bestAuction)
                    break;
                slotsBought.insert(bestSlot);

                // Buyout replica: bidder/bid assigned BEFORE the mails (they read
                // them), won-mail before RemoveAItem, RemoveAuction last.
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                bot->ModifyMoney(-int32(bestAuction->buyout));
                if (bestAuction->bidder)
                    sAuctionMgr->SendAuctionOutbiddedMail(bestAuction, bestAuction->buyout, bot, trans);
                bestAuction->bidder = bot->GetGUID().GetCounter();
                bestAuction->bid = bestAuction->buyout;
                // Bots hold no GM permission; same clear as the handler's else arm.
                bestAuction->Flags = AuctionEntryFlag(bestAuction->Flags & ~AUCTION_ENTRY_FLAG_GM_LOG_BUYER);

                sAuctionMgr->SendAuctionSalePendingMail(bestAuction, trans);
                sAuctionMgr->SendAuctionSuccessfulMail(bestAuction, trans);
                sAuctionMgr->SendAuctionWonMail(bestAuction, trans);

                TC_LOG_INFO("playerbots.pve",
                    "Bot {} bought auction {} (item {} for {} copper, +{:.1f} quality-weighted item levels).",
                    bot->GetName(), bestAuction->Id, bestAuction->itemEntry, bestAuction->buyout, bestGain);

                bestAuction->DeleteFromDB(trans);
                sAuctionMgr->RemoveAItem(bestAuction->itemGUIDLow);
                auctionHouse->RemoveAuction(bestAuction);

                bot->SaveInventoryAndGoldToDB(trans);
                CharacterDatabase.CommitTransaction(trans);
            }

            // Fetch the wins right away.
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingMailCollections.insert(botRawGuid);
        }
    }

    // ---------------------------------------------------------------------------
    // Taxi travel: BFS over direct taxi edges; the bot pays its own fare.
    // ---------------------------------------------------------------------------

    std::vector<uint32> FindTaxiNodeChain(uint32 sourceNode, uint32 destNode)
    {
        std::vector<uint32> chain;
        if (!sourceNode || !destNode || sourceNode == destNode)
            return chain;

        std::unordered_map<uint32, uint32> cameFrom;
        std::deque<uint32> frontier;
        frontier.push_back(sourceNode);
        cameFrom[sourceNode] = 0;

        while (!frontier.empty() && cameFrom.size() < 500)
        {
            uint32 const node = frontier.front();
            frontier.pop_front();
            auto const edges = sTaxiPathSetBySource.find(node);
            if (edges == sTaxiPathSetBySource.end())
                continue;

            for (auto const& edge : edges->second)
            {
                uint32 const next = edge.first;
                if (cameFrom.count(next))
                    continue;

                cameFrom[next] = node;
                if (next == destNode)
                {
                    for (uint32 walk = destNode; walk; walk = cameFrom[walk])
                        chain.push_back(walk);
                    std::reverse(chain.begin(), chain.end());
                    return chain;
                }
                frontier.push_back(next);
            }
        }

        return chain;
    }

    // World thread. Fly the bot toward a destination when a route exists and a
    // taxi node is close by; the post-landing leg becomes a walked journey.
    bool TryTaxiTravel(Player* bot, uint64 botRawGuid, uint16 destMapId, float destX, float destY, float destZ, uint8 fallbackKind)
    {
        if (!g_PveConfig.travelUseFlightPaths || bot->IsInFlight() || bot->IsInCombat())
            return false;

        uint32 const sourceNode = sObjectMgr->GetNearestTaxiNodeAnyTeam(
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
        uint32 const destNode = sObjectMgr->GetNearestTaxiNodeAnyTeam(destX, destY, destZ, destMapId);
        if (!sourceNode || !destNode || sourceNode == destNode)
            return false;

        TaxiNodesEntry const* sourceEntry = sTaxiNodesStore.LookupEntry(sourceNode);
        TaxiNodesEntry const* destEntry = sTaxiNodesStore.LookupEntry(destNode);
        if (!sourceEntry || !destEntry)
            return false;

        // Only fly when the departure point is genuinely nearby; a flight that
        // visibly starts hundreds of yards from any flight master looks wrong.
        if (uint32(sourceEntry->ContinentID) != bot->GetMapId() ||
            bot->GetDistance(sourceEntry->Pos.X, sourceEntry->Pos.Y, sourceEntry->Pos.Z) > 300.0f)
            return false;

        std::vector<uint32> const chain = FindTaxiNodeChain(sourceNode, destNode);
        if (chain.size() < 2 || chain.size() > 8)
            return false;

        // Post-landing leg: walk from the arrival node to the actual target.
        {
            PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
            state.engaged = false;
            if (uint32(destEntry->ContinentID) == destMapId)
            {
                StartWalkedJourney(state, destMapId, destX, destY, destZ, fallbackKind,
                    std::max(200.0f, bot->GetDistance(destX, destY, destZ)));
                // Flights are long; extend the deadline generously.
                state.journeyUntil = PveClock::now() + std::chrono::seconds(900);
            }
        }
        playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);

        if (!bot->ActivateTaxiPathTo(chain, nullptr))
        {
            // InstantTaxi pays the fare, teleports to the last node and returns
            // false; the journey leg still applies, so treat it as flown - a
            // false return here would send the caller on to teleport elsewhere.
            if (sWorld->getBoolConfig(CONFIG_INSTANT_TAXI))
                return true;

            PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
            state.journeyActive = false;
            state.journeyFallbackKind = 0;
            return false;
        }

        TC_LOG_INFO("playerbots.pve", "Bot {} took a flight: {} hops toward map {} ({:.0f} {:.0f}).",
            bot->GetName(), uint32(chain.size() - 1), destMapId, destX, destY);
        return true;
    }

    // World thread. Town run: teleport a supply-starved bot next to the nearest
    // vendor spawn, avoiding real players at both ends.
    void ProcessPendingSupplyRuns()
    {
        std::unordered_set<uint64> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingSupplyRuns);
        }

        if (drained.empty())
            return;

        BuildVendorSpotCacheOnce();

        for (uint64 botRawGuid : drained)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
            // A bot in the air is left alone. Teleporting mid-taxi tears the
            // flight generator off without letting it finalize, and finalize is
            // the only thing that clears UNIT_STATE_IN_FLIGHT and dismounts - so
            // the bot arrives still wearing the gryphon and frozen. The request
            // is dropped; whatever wanted it moved asks again once it lands.
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->InBattleground() ||
                bot->IsInFlight() ||
                bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
                continue;

            VendorSpot const* nearest = nullptr;
            float nearestDist2 = 0.0f;
            {
                std::lock_guard<std::mutex> guard(g_VendorSpotLock);
                for (VendorSpot const& spot : g_VendorSpots)
                {
                    if (spot.mapId != bot->GetMapId())
                        continue;

                    float const dx = spot.x - bot->GetPositionX();
                    float const dy = spot.y - bot->GetPositionY();
                    float const dist2 = dx * dx + dy * dy;
                    if (!nearest || dist2 < nearestDist2)
                    {
                        nearest = &spot;
                        nearestDist2 = dist2;
                    }
                }
            }

            if (!nearest)
                continue;

            // Walking distance? Make it a real trip to town.
            bool walkAllowed;
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                walkAllowed = PveClock::now() >= state.walkFallbackUntil;
            }
            if (walkAllowed && g_PveConfig.travelWalkMaxDistance > 0.0f && nearest->mapId == bot->GetMapId())
            {
                float const walkDistance = bot->GetDistance(nearest->x, nearest->y, nearest->z);
                if (walkDistance <= g_PveConfig.travelWalkMaxDistance)
                {
                    playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
                    PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                    state.engaged = false;
                    StartWalkedJourney(state, nearest->mapId, nearest->x, nearest->y, nearest->z, 2, walkDistance);
                    TC_LOG_INFO("playerbots.pve", "Bot {} walking {:.0f}y to town for supplies.",
                        bot->GetName(), walkDistance);
                    continue;
                }
            }

            if (TryTaxiTravel(bot, botRawGuid, nearest->mapId, nearest->x, nearest->y, nearest->z, 2))
                continue;

            // Walking and flying are visible, legitimate travel; only the
            // teleport needs the vanish-guard at the source end.
            if (HasHumanPlayerNearby(bot, 150.0f))
                continue;

            Map* map = sMapMgr->FindMap(nearest->mapId, 0);
            if (!map || WouldLandInSightOfAnybody(map, nearest->x, nearest->y))
                continue;

            playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                state.engaged = false;
            }
            if (MotionMaster* motionMaster = bot->GetMotionMaster())
                motionMaster->Clear();
            if (!BotCanTeleportNow(bot))
                continue;

            if (bot->TeleportTo(nearest->mapId, nearest->x + frand(-3.0f, 3.0f), nearest->y + frand(-3.0f, 3.0f),
                nearest->z + 0.5f, frand(0.0f, 6.28f)))
            {
                RestorePlayerbotTeleportVitals(bot);
                TC_LOG_INFO("playerbots.pve", "Supply run: teleported {} to a vendor cluster on map {}.",
                    bot->GetName(), nearest->mapId);
            }
        }
    }

    // World thread. Move a bot toward its class-quest giver or ender via the
    // usual ladder: walk, fly, then vanish-guarded teleport.
    void ProcessPendingClassQuestTravels()
    {
        std::unordered_set<uint64> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingClassQuestTravels);
        }

        for (uint64 botRawGuid : drained)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
            // A bot in the air is left alone. Teleporting mid-taxi tears the
            // flight generator off without letting it finalize, and finalize is
            // the only thing that clears UNIT_STATE_IN_FLIGHT and dismounts - so
            // the bot arrives still wearing the gryphon and frozen. The request
            // is dropped; whatever wanted it moved asks again once it lands.
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->InBattleground() ||
                bot->IsInFlight() ||
                bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
                continue;

            uint16 mapId;
            float x, y, z;
            bool walkAllowed;
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                if (!state.classQuestId)
                    continue;
                mapId = state.classQuestMapId;
                x = state.classQuestX;
                y = state.classQuestY;
                z = state.classQuestZ;
                walkAllowed = PveClock::now() >= state.walkFallbackUntil;
            }

            if (walkAllowed && g_PveConfig.travelWalkMaxDistance > 0.0f && mapId == bot->GetMapId())
            {
                float const walkDistance = bot->GetDistance(x, y, z);
                if (walkDistance <= g_PveConfig.travelWalkMaxDistance)
                {
                    playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
                    PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                    state.engaged = false;
                    StartWalkedJourney(state, mapId, x, y, z, 3, walkDistance);
                    TC_LOG_INFO("playerbots.pve", "Bot {} walking {:.0f}y to a class quest.",
                        bot->GetName(), walkDistance);
                    continue;
                }
            }

            if (TryTaxiTravel(bot, botRawGuid, mapId, x, y, z, 3))
                continue;

            // Only the teleport arm hides from real players.
            if (HasHumanPlayerNearby(bot, 150.0f))
                continue;

            Map* map = sMapMgr->FindMap(mapId, 0);
            if (!map || WouldLandInSightOfAnybody(map, x, y))
                continue;

            playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                state.engaged = false;
            }
            if (MotionMaster* motionMaster = bot->GetMotionMaster())
                motionMaster->Clear();
            if (!BotCanTeleportNow(bot))
                continue;

            if (bot->TeleportTo(mapId, x + frand(-3.0f, 3.0f), y + frand(-3.0f, 3.0f), z + 0.5f, frand(0.0f, 6.28f)))
            {
                RestorePlayerbotTeleportVitals(bot);
                TC_LOG_INFO("playerbots.pve", "Class quest travel: teleported {} to map {}.", bot->GetName(), mapId);
            }
        }
    }

    // World thread. Keep sending somebody at whoever is worth killing.
    //
    // The ordinary dispatcher (MaybeHuntPlayersByAggression) only releases a bot
    // after THAT bot has personally been peaceful for AggressionIdleMinutes, which
    // is minutes to tens of minutes and per-bot - so help arrives in ones, far
    // apart. A bounty replaces that clock with its own: one arrival per interval,
    // for as long as the bounty stands.
    //
    // Past the over-level threshold the ranking flips from nearest-first to
    // highest-level-first, and that is only meaningful because this rides the
    // guardian TELEPORT queue: that path lands a bot at its real level, where the
    // drifter path would re-level it into the destination zone's band and quietly
    // deliver a peer instead.
    // A hundred of them, so the fleet does not read like one script.
    constexpr std::array<char const*, 100> kBountyHelpShouts = { {
        "Help! I can't take this one alone!",
        "Someone get over here, now!",
        "I'm outmatched! Anybody!",
        "This one's way above me - help!",
        "I need backup! Right now!",
        "Get over here, I can't hold them!",
        "They're killing me! Somebody!",
        "I can't win this - HELP!",
        "Anyone! Anyone at all!",
        "I'm not going to make it alone!",
        "This one's a killer! Get over here!",
        "Somebody, please!",
        "I'm in over my head!",
        "They're too strong! HELP!",
        "Backup! I need backup!",
        "I can't fight this alone!",
        "Get help! Now!",
        "Someone come quick!",
        "I'm going to die out here!",
        "This isn't a fair fight - help me!",
        "HELP! There's a marked one here!",
        "I found them! I can't hold them!",
        "The mark's here! Get over here!",
        "I've got them cornered - I need help!",
        "They're worth a fortune! Somebody help me kill them!",
        "Wanted alive or dead - and I can't do either alone!",
        "I've got eyes on the mark! HELP!",
        "The mark is here! Backup!",
        "Somebody collect on this one with me!",
        "I can't collect on this one alone!",
        "Bring everyone! This one's a killer!",
        "They've killed too many! Help me!",
        "I need a hand! A big one!",
        "I'm losing! Somebody!",
        "Get down here before I'm dead!",
        "This one has a price on their head!",
        "HELP ME!",
        "I can't do this by myself!",
        "Someone stronger, please!",
        "Bring somebody who can fight!",
        "I'm no match for them!",
        "They'll kill me! Help!",
        "Anybody within earshot - HELP!",
        "I'm bleeding out here!",
        "Come on, someone! Anyone!",
        "I'll die before I take them alone!",
        "This is not a fight I win!",
        "Get somebody bigger over here!",
        "Please - I need help!",
        "They're too much for me!",
        "Somebody finish what I started!",
        "I've hurt them! Come finish it!",
        "I'll hold them - somebody get here!",
        "Holding on! Hurry!",
        "I can't last much longer!",
        "Not much time - HELP!",
        "They're going to drop me!",
        "I'm nearly done - help!",
        "Send anyone! Please!",
        "Whoever's nearest - I need you!",
        "I've got a wanted killer here!",
        "There's blood money standing right here!",
        "A hand! Somebody give me a hand!",
        "This one kills for sport - help me!",
        "I'm outclassed! Backup!",
        "They out-level me by a mile!",
        "I can't touch them alone!",
        "Get me some help before I'm a corpse!",
        "Help! They're wanted!",
        "Marked one! Right here! HELP!",
        "I need somebody who can actually hurt them!",
        "I'm just slowing them down - hurry!",
        "Come on! Someone!",
        "I've made my stand - now help me!",
        "I won't run. But I need help!",
        "Someone, anyone, please hurry!",
        "They're on me! HELP!",
        "This one's marked - and I'm losing!",
        "Get here fast or get here late!",
        "I'd rather not die today - help!",
        "Backup, and be quick about it!",
        "I've called it in - somebody answer!",
        "Answer me! I need help!",
        "The payout's mine if someone helps!",
        "Split the payout - just get here!",
        "I can't finish them! Somebody can!",
        "They're worth more than my life - help me!",
        "I found the wanted one! HELP!",
        "A killer! Right here! Help me!",
        "Nobody should fight this alone!",
        "I'm badly outmatched - please!",
        "Send the strongest one you've got!",
        "Whoever can fight - come now!",
        "I'm the wrong one for this fight!",
        "Wrong fight, wrong day - HELP!",
        "They'll be gone if nobody comes!",
        "Don't let them get away - help me!",
        "Hold them with me! Somebody!",
        "I need one good fighter, now!",
        "Just one of you! Please!",
    } };

    // A bot that has bitten off more than it can chew against a bountied player
    // shouts, and somebody who CAN win the fight drops what they are doing and
    // comes.
    //
    // The trigger is deliberately the mismatch, not just the bounty: a bot that
    // is a fair match is having an ordinary fight and needs no rescue. What this
    // answers is the low-level bot that aggroed a killer it cannot hurt - it dies
    // in seconds, the bounty walks away, and the fleet looks like it has no idea
    // what it is doing.
    //
    // World thread: it teleports, and reads Group state through the rescue.
    void CallForHelpAgainstBounties(std::vector<HumanSpot> const& spots,
        std::vector<Player*> const& proddableBots)
    {
        static uint32 const minStacks = uint32(std::max(1,
            sConfigMgr->GetIntDefault("Centurion.Bounty.CallForHelpStacks", 10)));
        static float const hearingYards = std::max(1.0f,
            sConfigMgr->GetFloatDefault("Centurion.Bounty.CallForHelpYards", 50.0f));
        // Only somebody genuinely elsewhere is teleported. A bot already in the
        // neighbourhood can walk, and porting one four hundred yards is the kind
        // of thing a player notices happening next to them.
        static float const rescueFromYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Centurion.Bounty.CallForHelpMinRescueYards", 200.0f));

        // One shout per outmatched bot per cooldown, or a losing fight becomes a
        // wall of text.
        static std::unordered_map<uint64, PveTimePoint> s_nextShoutAt;
        PveTimePoint const now = PveClock::now();

        for (HumanSpot const& spot : spots)
        {
            if (spot.Bounty < minStacks)
                continue;

            Player* human = ObjectAccessor::FindConnectedPlayer(spot.Guid);
            if (!human || !human->IsInWorld() || !human->IsAlive() ||
                human->InBattleground() || human->InArena())
                continue;

            Map* map = human->FindMap();
            if (!map)
                continue;

            // Who is losing to this person right now.
            for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin();
                itr != map->GetPlayers().end(); ++itr)
            {
                Player* outmatched = itr->GetSource();
                if (!outmatched || outmatched == human || !outmatched->IsAlive() ||
                    !playerbot::IsManagedRandomBot(outmatched))
                    continue;

                if (!outmatched->IsWithinDistInMap(human, hearingYards))
                    continue;

                // Outmatched: below the level this file would ever let it start
                // a fight at. THIS IS THE WHOLE TRIGGER, and it used to also
                // require the bot to already be fighting the person - which is
                // self-defeating, because a bot below the proactive level range
                // is exactly the bot that will never aggro them. With
                // ProactiveMaxLevelsAbove at 4, a level 38 bot cannot start on a
                // level 43, so it was never anybody's victim and never shouted.
                //
                // A bounty walking past is reason enough. It is a lookout calling
                // in a sighting, not a cry from inside a fight.
                if (IsProactiveTargetWithinPower(outmatched, human->GetLevel()))
                    continue;

                // Line of sight, so it is a sighting and not a shout through a
                // hillside.
                if (!outmatched->IsWithinLOSInMap(human))
                    continue;

                uint64 const outmatchedGuid = outmatched->GetGUID().GetRawValue();
                auto const shoutItr = s_nextShoutAt.find(outmatchedGuid);
                if (shoutItr != s_nextShoutAt.end() && now < shoutItr->second)
                    continue;
                s_nextShoutAt[outmatchedGuid] = now + std::chrono::seconds(30);

                outmatched->Yell(kBountyHelpShouts[urand(0, uint32(kBountyHelpShouts.size()) - 1)],
                    LANG_UNIVERSAL);

                // Now find somebody who can actually answer it.
                Player* rescuer = nullptr;
                float bestDistance = 0.0f;
                for (Player* candidate : proddableBots)
                {
                    if (candidate == outmatched || candidate == human ||
                        candidate->GetMapId() != human->GetMapId())
                        continue;

                    // Far enough away that porting is the only way to arrive.
                    float const distance = candidate->GetDistance(human);
                    if (distance < rescueFromYards)
                        continue;

                    // Able to win the fight it is being sent to.
                    if (!IsProactivePlayerLevelAcceptable(candidate, human))
                        continue;

                    // And it has to belong in the zone it is landing in.
                    uint8 bandBottom = 0;
                    uint8 bandTop = 0;
                    if (playerbot::GetZoneLevelBand(human->GetZoneId(), bandBottom, bandTop))
                    {
                        uint8 const level = candidate->GetLevel();
                        if (level < bandBottom || level > bandTop)
                            continue;
                    }

                    if (!BotCanTeleportNow(candidate))
                        continue;

                    if (!rescuer || distance < bestDistance)
                    {
                        rescuer = candidate;
                        bestDistance = distance;
                    }
                }

                if (!rescuer)
                    continue;

                // Arrives beside the bot that called, not on top of the player -
                // it is answering a shout, not ambushing.
                float x, y, z;
                outmatched->GetNearPoint(outmatched, x, y, z, 5.0f, frand(0.0f, 2.0f * float(M_PI)));
                outmatched->UpdateAllowedPositionZ(x, y, z);

                if (!rescuer->TeleportTo(outmatched->GetMapId(), x, y, z,
                    rescuer->GetAbsoluteAngle(human->GetPositionX(), human->GetPositionY())))
                    continue;

                // WIPE WHAT IT WAS DOING FIRST, or it simply carries on.
                //
                // The journey it was on lives in its PvE state, not in its
                // position, so teleporting it and calling Attack changed nothing:
                // the next tick read the old state, saw a destination it had not
                // reached, mounted up and rode off. Which is exactly what was
                // seen - a rescuer arriving and immediately running away.
                //
                // Erasing the state is the same thing a rebirth does, and it
                // leaves the bot with no errand, no journey and no stay order.
                playerbot::LockedErase(g_PveBotStateByGuid, rescuer->GetGUID().GetRawValue());
                playerbot::PvpCore::SetPveCombatEngagement(rescuer->GetGUID(), false);

                if (rescuer->IsMounted())
                    rescuer->Dismount();
                if (MotionMaster* motionMaster = rescuer->GetMotionMaster())
                    motionMaster->Clear();
                rescuer->StopMoving();

                // Full and ready: it was pulled out of whatever it was doing, and
                // arriving at half health on a cooldown it already spent would
                // just be a second corpse.
                RestorePlayerbotTeleportVitals(rescuer);
                rescuer->GetSpellHistory()->ResetAllCooldowns();

                // The same stamp the hunt dispatch uses. It is what tells the
                // rest of the file this bot is on a bounty - the aggro-radius
                // reduction on the way in, and the damage bonus when it lands -
                // and without it the rescuer is just a bot standing somewhere new.
                rescuer->SetBountyPursuit(spot.Bounty, 90 * IN_MILLISECONDS);
                rescuer->Attack(human, true);

                TC_LOG_INFO("playerbots.pve",
                    "{} (level {}) called for help against {} (bounty {}); {} (level {}) answered from {:.0f}y.",
                    outmatched->GetName(), uint32(outmatched->GetLevel()), human->GetName(),
                    spot.Bounty, rescuer->GetName(), uint32(rescuer->GetLevel()), bestDistance);
            }
        }

        for (auto itr = s_nextShoutAt.begin(); itr != s_nextShoutAt.end();)
            itr = now > itr->second + std::chrono::minutes(5) ? s_nextShoutAt.erase(itr) : std::next(itr);
    }

    // The levy: bots conscripted out of their own band to fight a bounty in a
    // zone they do not belong in, and the memory of where to send them back to.
    //
    // World thread only, like the drifter registry it sits beside.
    struct LeviedBot
    {
        uint32 HomeZoneId = 0;    // the band it was taken from
        uint8  HomeLevel = 1;     // and the level it was living at there
        uint64 HumanGuid = 0;     // the bounty it was raised for
    };
    std::unordered_map<uint64, LeviedBot> g_LeviedBots;

    uint32 LevyStacks()
    {
        static uint32 const stacks = uint32(std::max(1,
            sConfigMgr->GetIntDefault("Centurion.Bounty.LevyStacks", 15)));
        return stacks;
    }

    // Whether a real person is playing in this zone right now. A levy is drawn
    // from empty ground, never out from under somebody.
    bool ZoneHasPeople(uint32 zoneId)
    {
        std::lock_guard<std::mutex> guard(g_HumanSpotLock);
        for (HumanSpot const& spot : g_HumanSpots)
            if (spot.ZoneId == zoneId)
                return true;
        return false;
    }

    // Whose level is theirs, and not the ground's to set.
    //
    // A levy re-levels, and for most of the fleet that is harmless: an ordinary
    // local bot's level IS its zone assignment, so moving it to another band and
    // back is the same operation the rebirth cycle performs on it anyway.
    //
    // Veterans and the PvP-only battleground fleet are not that. Both are
    // explicitly outside the local zone-band population - BuildLocalZoneAssignmentsOnce
    // skips them ("these populations do not cycle through local zones") and
    // GetLocalHomeZoneId returns 0 for them - so their level is who they are
    // rather than where they are standing. Levying one takes a max-level
    // battleground regular, strips its spells, talents and quests, drops it to
    // the top of whatever band the victim happened to be standing in, and saves
    // that to the database. The pools that only open at the highest bounties
    // were exactly the ones the levy destroyed. They answer as they are.
    //
    // Guardians are the third of those roles and are here for the reason
    // LoadGuardianPostsOnce already states about them in so many words: a
    // guardian "GiveLevel'd DOWNWARD ... resets its talents and strands it in
    // gear it can no longer wear, permanently, since guardians are excluded from
    // both rebirth and the reset command." Two other places in this file already
    // treat the three as one set; this is the third.
    bool KeepsItsOwnLevel(Player const* bot)
    {
        if (!bot)
            return false;

        return playerbot::PveManager::IsPvpOnlyBot(bot) ||
            IsLocalVeteranGuid(bot->GetGUID().GetRawValue()) ||
            GetGuardianZoneId(bot->GetGUID().GetRawValue()) != 0;
    }

    // Where a hunter was standing when it was called out, so it can be put back
    // the moment there is no longer a reason for it to be where it is.
    //
    // POSITION ONLY. That is the whole difference between this and the levy
    // ledger above it: a bot that kept its own level has nothing else to restore,
    // so bringing it home is a teleport and not a rebirth.
    //
    // It is needed because the ordinary "you have outgrown this zone" sweep
    // cannot do the job on its own. That sweep never runs for a PvP-only bot at
    // all (OnPlayerLifecycleTick returns before it), and for everyone else it is
    // on a sixty second cadence and only fires when the ZONE is wrong - so a
    // level sixty answering a bounty in a same-band zone two continents from home
    // would simply stay there. This is the receipt that gets them back.
    struct DeployedHunter
    {
        uint32 MapId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        uint64 HumanGuid = 0;
    };
    std::unordered_map<uint64, DeployedHunter> g_DeployedHunters;

    // Conscript one bot into the zone a bounty is standing in, at the top of that
    // zone's band, remembering where it came from.
    //
    // ResetManagedBotToZoneBand does the whole job - re-level, re-learn, retalent,
    // and the teleport into a grind spot in the zone - so the levy IS the port.
    bool LevyBotInto(Player* bot, Player* human, uint8 bandTop)
    {
        if (!bot || !human)
            return false;

        // The single place this can never be got past. The callers below decide
        // not to levy these; this makes it true even if a third caller appears.
        if (KeepsItsOwnLevel(bot))
            return false;

        uint64 const botRawGuid = bot->GetGUID().GetRawValue();
        if (g_LeviedBots.count(botRawGuid))
            return false;                       // already serving

        // Ask the reset's own preconditions BEFORE it, not after. It refuses a bot
        // that is mid-teleport or out of world, and refusing is the only outcome
        // that leaves the bot untouched - every other path through it has already
        // re-levelled and SaveToDB'd by the time it returns.
        if (!bot->IsInWorld() || bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            return false;

        LeviedBot record;
        record.HomeZoneId = bot->GetZoneId();
        record.HomeLevel = bot->GetLevel();
        record.HumanGuid = human->GetGUID().GetRawValue();

        uint32 const targetZone = human->GetZoneId();

        // Recorded BEFORE the reset, because the reset is what makes the record
        // necessary. It re-levels and saves first and teleports last, so a bot
        // whose grind spot could not be found - or whose teleport was refused -
        // has still been dropped to the band and still has to be sent home. The
        // old order recorded only on success and left that bot at a borrowed
        // level with nothing holding the receipt: permanent, and silent.
        g_LeviedBots[botRawGuid] = record;

        playerbot::ResetManagedBotToZoneBand(bot, targetZone, bandTop);

        // It was re-levelled either way; it just did not arrive. Keep the record
        // so ReturnLeviedBots takes it home, and tell the caller not to treat it
        // as a hunter in place.
        if (bot->GetZoneId() != targetZone)
        {
            TC_LOG_INFO("playerbots.pve",
                "Levy: {} was re-levelled for the bounty on {} but did not reach zone {}; it will be returned to zone {} at level {}.",
                bot->GetName(), human->GetName(), targetZone,
                record.HomeZoneId, uint32(record.HomeLevel));
            return false;
        }

        TC_LOG_INFO("playerbots.pve",
            "Levy: {} raised from zone {} (level {}) to level {} in zone {} for the bounty on {}.",
            bot->GetName(), record.HomeZoneId, uint32(record.HomeLevel), uint32(bandTop),
            targetZone, human->GetName());
        return true;
    }

    // And send them home once the bounty they were raised for is finished with.
    void ReturnLeviedBots()
    {
        for (auto itr = g_LeviedBots.begin(); itr != g_LeviedBots.end();)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(itr->first));
            if (!bot || !bot->IsInWorld())
            {
                ++itr;                          // offline: keep the record, decide when it returns
                continue;
            }

            Player* human = ObjectAccessor::FindConnectedPlayer(ObjectGuid(itr->second.HumanGuid));
            bool const stillWanted = human && human->IsInWorld() && human->IsAlive() &&
                Bounty::GetStacks(human) >= LevyStacks() &&
                human->GetZoneId() == bot->GetZoneId();

            if (stillWanted || bot->IsInCombat())
            {
                ++itr;                          // the job is not over
                continue;
            }

            LeviedBot const record = itr->second;
            itr = g_LeviedBots.erase(itr);

            TC_LOG_INFO("playerbots.pve", "Levy: {} released back to zone {} at level {}.",
                bot->GetName(), record.HomeZoneId, uint32(record.HomeLevel));
            playerbot::ResetManagedBotToZoneBand(bot, record.HomeZoneId, record.HomeLevel);
        }
    }

    // The same release, for the hunters that were never re-levelled: the instant
    // the bounty stops being a reason to stand here, they go back to where they
    // were standing when they were called.
    //
    // Checked on the bounty's own once-a-second pass rather than left to the
    // sixty-second zone-fit sweep, because "there is no longer any reason for a
    // level sixty to be in Westfall" is true the moment the bounty clears, and a
    // minute of a max-level bot loitering in a starter zone is exactly what this
    // is meant to prevent. The sweep remains as the backstop for the ones whose
    // receipt is lost to a restart.
    void ReturnDeployedHunters()
    {
        for (auto itr = g_DeployedHunters.begin(); itr != g_DeployedHunters.end();)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(itr->first));
            if (!bot || !bot->IsInWorld())
            {
                ++itr;                          // offline: decide when it comes back
                continue;
            }

            // Still a job? The person is still here, still alive, still hunted.
            // Anything else - they died, they cleared the bounty, they logged
            // out - and the errand is over THIS INSTANT.
            //
            // Deliberately NOT "and still in the zone the hunter was sent to".
            // A hunter is dispatched from wherever it was standing and its
            // teleport lands a tick later, so a zone term would read false for
            // the whole outbound trip and send it straight home again before it
            // ever arrived. The question this asks is about the PERSON, not
            // about where either of them is standing.
            Player* human = ObjectAccessor::FindConnectedPlayer(ObjectGuid(itr->second.HumanGuid));
            bool const stillWanted = human && human->IsInWorld() && human->IsAlive() &&
                Bounty::IsHuntedRelentlessly(Bounty::GetStacks(human));

            if (stillWanted || bot->IsInCombat())
            {
                ++itr;                          // the job is not over
                continue;
            }

            // The job IS over, so the dispatch's pursuit deadline has nothing
            // left to protect. Clearing it here is what makes the trip home
            // immediate: the stamp is a fixed ninety seconds that nothing else
            // ever clears, so a hunter whose target died at twenty seconds would
            // otherwise loiter in a starter zone for the remaining seventy - and
            // the zone-fit sweep is pinned on the same stamp, so it would not
            // step in either.
            if (bot->GetBountyPursuitStacks())
                bot->SetBountyPursuit(0, 0);

            // HELD, not dropped, for every state where moving it would break
            // something. These are the guards the two sibling executors in this
            // file already carry, and they are held rather than erased so the
            // bot still goes home once it lands, leaves the match, or stands up:
            //
            //  - a battleground or its queue: TeleportTo's far branch calls
            //    LeaveBattleground and would pull a bot out of a live match. The
            //    dispatch side screens these (proddableBots) but nothing stops a
            //    queue popping for a bot that is ALREADY deployed.
            //  - a taxi flight: teleporting mid-flight tears the generator off
            //    without letting it finalise and the bot arrives still wearing
            //    the gryphon, frozen.
            //  - dead: leave it with its corpse for the death recovery to run.
            if (!bot->IsAlive() || bot->IsInFlight() || bot->InBattleground() ||
                bot->InArena() || bot->InBattlegroundQueue())
            {
                ++itr;
                continue;
            }

            // Kept, not dropped, when it cannot be moved this instant: a bot mid
            // teleport or out of its grid is the case the crash comments in this
            // file are about, and the next pass is a second away.
            if (!BotCanTeleportNow(bot))
            {
                ++itr;
                continue;
            }

            DeployedHunter const post = itr->second;
            uint64 const botRawGuid = itr->first;
            itr = g_DeployedHunters.erase(itr);

            // Whatever happens below, this bot is no longer the hunt's business,
            // so the sweep gets its zone back.
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                state.bountyDeployed = false;
            }

            // It may never have gone. A queued dispatch teleport can be refused
            // outright - no reachable landing on the ring, the path budget
            // saturated, the bounty gone by the time it was drained - and the
            // receipt outlives the attempt. Do not teleport a bot back to the spot
            // it never left.
            if (bot->GetMapId() == post.MapId && bot->GetExactDist2d(post.X, post.Y) < 60.0f)
                continue;

            // And do not appear at it in front of somebody. The post is a place
            // this bot was grinding, which is a place people grind, and the
            // receipt is reused verbatim however long the hunt lasted - the world
            // around it may be completely different by now. Held rather than
            // dropped: the errand is over either way, so there is no hurry, and
            // the next pass is a second away.
            //
            // Only asked when the bot is still on the post's own map, which is
            // the normal case - the dispatch only ever picks a bot already on the
            // human's map, so it never left it. A cross-map return would need the
            // destination map looked up, and there is no such case to serve.
            if (bot->GetMapId() == post.MapId &&
                WouldLandInSightOfAnybody(bot->FindMap(), post.X, post.Y))
            {
                g_DeployedHunters[botRawGuid] = post;
                continue;
            }

            if (bot->TeleportTo(post.MapId, post.X, post.Y, post.Z, post.O))
            {
                RestorePlayerbotTeleportVitals(bot);

                // Arrive home with nothing still steering. The dispatch started a
                // WALKED journey toward where the person was standing, and that
                // journey outlives the teleport: the candidate loop only ever
                // picks a bot already on the human's map, so this is a NEAR
                // teleport and the journey's own map-change cancellation never
                // fires. Left alone, the bot walks straight back to the bounty it
                // was just brought home from. Same reasoning, same block, as the
                // stuck-recovery teleport further down this file.
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                state.journeyActive = false;
                state.journeyFallbackKind = 0;
                state.errandGuid = ObjectGuid::Empty;
                state.errandKind = PveErrandKind::None;
                state.errandUntil = {};
                state.engaged = false;
                state.dryWanderCount = 0;
                state.nextWanderAt = {};
                playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);

                TC_LOG_INFO("playerbots.pve",
                    "Bounty: {} (level {}) sent home from zone {}; the hunt is over.",
                    bot->GetName(), uint32(bot->GetLevel()), bot->GetZoneId());
            }
        }
    }

    void HuntTheBountied(std::vector<HumanSpot> const& spots,
        std::vector<Player*> const& proddableBots)
    {
        static std::unordered_map<uint64, PveTimePoint> s_nextHuntAt;

        PveTimePoint const now = PveClock::now();
        std::unordered_set<uint64> online;

        for (HumanSpot const& spot : spots)
        {
            uint64 const humanGuid = spot.Guid.GetRawValue();
            online.insert(humanGuid);

            if (!Bounty::IsHuntedRelentlessly(spot.Bounty))
                continue;

            auto const next = s_nextHuntAt.find(humanGuid);
            if (next != s_nextHuntAt.end() && now < next->second)
                continue;

            Player* human = ObjectAccessor::FindConnectedPlayer(spot.Guid);
            if (!human || !human->IsInWorld() || !human->IsAlive() ||
                human->InBattleground() || human->InArena())
                continue;

            if (!BarracksHardcore::IsOpenWorldPvpZone(human->GetZoneId()))
                continue;

            bool const veterans = Bounty::DrawsFromVeterans(spot.Bounty);
            bool const pvpBots = Bounty::DrawsFromPvpBots(spot.Bounty);

            // Who is ALREADY answering. A bot inside the guardian teleport
            // trigger has arrived, or is close enough that walking finishes the
            // job; re-sending it is what produced a bot ping-ponging out to 210
            // yards every twenty seconds, because walking in made it the nearest
            // candidate and nearest is what this picks.
            constexpr float kAlreadyAnsweringYards = kBountyAnsweringYards;
            uint32 answering = 0;
            if (Map* map = human->FindMap())
                for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin();
                    itr != map->GetPlayers().end(); ++itr)
                {
                    // Not named 'near': that is a legacy Windows macro, and this
                    // file is built by clang on the Jenkins side as well.
                    Player* other = itr->GetSource();
                    if (!other || other == human || !playerbot::IsManagedRandomBot(other) ||
                        !other->IsAlive() || !other->IsWithinDistInMap(human, kAlreadyAnsweringYards))
                        continue;

                    // Nearby is not the same as ANSWERING, and counting it that
                    // way is why a bounty could stand in a zone full of bots and
                    // have nobody sent: idle bots inside 240 yards filled the
                    // ceiling, and the ones far enough away to be teleported were
                    // never dispatched because the ceiling was already full.
                    //
                    // A responder is either already fighting this person, or
                    // carries the pursuit deadline the dispatch stamps on it.
                    // Anything else is just a bot that happens to be standing
                    // there - frequently one too low to have aggroed at all.
                    if (other->GetVictim() == human || IsSwingingAt(other, human) ||
                        other->GetBountyPursuitStacks())
                        ++answering;
                }

            // Enough is enough. Relentless means a stream that does not stop,
            // not a pile that never stops growing.
            if (answering >= Bounty::MaxHuntersOnTarget(spot.Bounty))
                continue;

            std::vector<std::pair<float, Player*>> ranked;
            for (Player* bot : proddableBots)
            {
                if (bot->GetMapId() != human->GetMapId() || bot == human)
                    continue;

                // Already there, or close enough to walk. THIS is the guard the
                // guardian path has always had - its teleport trigger sits above
                // its drop distance for exactly this reason.
                if (bot->IsWithinDistInMap(human, kAlreadyAnsweringYards))
                    continue;

                // Still never volunteer somebody into a fight they lose on arrival:
                // this rule is about the bot being too LOW, and raising the bounty
                // must not start feeding the victim easy kills.
                if (!IsProactivePlayerLevelAcceptable(bot, human))
                    continue;

                // Which pool this bot belongs to decides the tier; distance only
                // breaks ties inside one. Ordinary bots are always the LAST tier
                // rather than excluded, so a zone holding no veteran and no PvP
                // bot still sends somebody.
                //
                // Asked before the band question rather than after it, because
                // which pool a bot is in decides whether the band is even its
                // business.
                bool const isPvpBot = playerbot::PveManager::IsPvpOnlyBot(bot);
                bool const isVeteran = IsLocalVeteranGuid(bot->GetGUID().GetRawValue());

                // The battleground fleet stays in its battlegrounds until the
                // bounty is high enough to call it out. Nothing else in this file
                // pulls a PvP-only bot into the open world.
                if (isPvpBot && !pvpBots)
                    continue;

                // Out of the target zone's band? Then it is a RECRUIT, not a
                // rejection.
                //
                // Sending a level 12 into the Burning Steppes as it stands is
                // pointless - it dies to the zone before it reaches anybody. But
                // refusing to send it is how a bounty in a high zone runs out of
                // hunters entirely. So the bot is levied instead: re-levelled to
                // the top of that zone's band, moved there, and returned to its
                // own band when the bounty is over.
                //
                // Levied bots are the LAST tier, so anybody who already belongs in
                // the zone goes first and a levy only happens when the zone cannot
                // field enough of its own. And only above the levy threshold, so an
                // ordinary bounty never re-levels anyone.
                //
                // Except the two pools whose level is not the zone's to set - see
                // KeepsItsOwnLevel. A veteran or a PvP-only bot is never levied:
                // ABOVE the band it simply goes as it is, which is the whole point
                // of calling those pools out at all, and BELOW it is refused
                // rather than raised, because the reason behind the levy was never
                // bookkeeping - a bot too low for the ground dies to the zone
                // before it reaches anybody, and that is as true of a veteran as
                // of anyone else.
                bool const keepsLevel = KeepsItsOwnLevel(bot);
                bool needsLevy = false;
                {
                    uint8 bandBottom = 0;
                    uint8 bandTop = 0;
                    if (playerbot::GetZoneLevelBand(human->GetZoneId(), bandBottom, bandTop))
                    {
                        uint8 const botLevel = bot->GetLevel();
                        if (keepsLevel)
                        {
                            if (botLevel < bandBottom)
                                continue;

                            // Above the band, this is one of those pools being
                            // called out of its own life, so it answers on its OWN
                            // rung of the escalation and not on the levy's. The
                            // levy threshold is the lowest of the three (15,
                            // against 25 for veterans and 50 for the battleground
                            // fleet) and it was never theirs to borrow: without
                            // this, a bounty of fifteen - too small to summon a
                            // veteran at all - would get a level sixty standing
                            // over a level twenty, where before the fix it got the
                            // same veteran cut down to the zone's band. Neither is
                            // right; not being sent is.
                            //
                            // A PvP-only bot was already gated on its own threshold
                            // further up, so only the veteran rung is left to ask
                            // about. A guardian has no rung of its own and is left
                            // on its post.
                            if (botLevel > bandTop && !isPvpBot && !(isVeteran && veterans))
                                continue;
                        }
                        else
                            needsLevy = botLevel < bandBottom || botLevel > bandTop;
                    }

                    if (needsLevy)
                    {
                        if (spot.Bounty < LevyStacks())
                            continue;

                        // Drawn from zones nobody is playing in, per the design:
                        // a levy must not empty a zone out from under a person.
                        if (ZoneHasPeople(bot->GetZoneId()))
                            continue;
                    }
                }

                uint32 tier = 2;
                if (pvpBots && isPvpBot)
                    tier = 0;
                else if (veterans && isVeteran)
                    tier = 1;

                if (needsLevy)
                    tier = 3;

                ranked.push_back({ float(tier) * 100000.0f + bot->GetDistance(human), bot });
            }
            std::sort(ranked.begin(), ranked.end(),
                [](auto const& a, auto const& b) { return a.first < b.first; });

            // How many go at once, clamped against the same ceiling the single
            // dispatch respected: a bigger wave fills the remaining room and
            // stops, rather than adding its full size on top of whoever is
            // already standing there.
            uint32 const room = Bounty::MaxHuntersOnTarget(spot.Bounty) - answering;
            uint32 const wave = std::min(Bounty::HuntersPerWave(spot.Bounty), room);

            // The companion question needs the process-wide state lock, so it is
            // asked only of the bots actually being sent - the same rule the idle
            // prod follows.
            std::vector<Player*> sending;
            for (auto const& [score, bot] : ranked)
            {
                if (sending.size() >= wave)
                    break;

                // A bot with a teleport still queued has not arrived yet, and
                // queueing a second one for it would overwrite the first.
                {
                    std::lock_guard<std::mutex> guard(g_PvePendingLock);
                    if (g_PendingGuardianTeleports.count(bot->GetGUID().GetRawValue()))
                        continue;
                }

                PveBotState const* state = playerbot::LockedFind(g_PveBotStateByGuid,
                    bot->GetGUID().GetRawValue());
                if (!state || state->masterGuid.IsEmpty())
                    sending.push_back(bot);
            }

            if (sending.empty())
                continue;

            uint32 const wait = Bounty::RelentlessIntervalSeconds(spot.Bounty);
            s_nextHuntAt[humanGuid] = now + std::chrono::seconds(wait);

            uint8 levyBottom = 0;
            uint8 levyTop = 0;
            bool const zoneBanded = playerbot::GetZoneLevelBand(human->GetZoneId(), levyBottom, levyTop);

            for (Player* chosen : sending)
            {
                // A recruit is re-levelled and moved by the levy itself, so it
                // needs no teleport queued - ResetManagedBotToZoneBand has already
                // put it in the zone at a level that can survive there.
                //
                // This asks the band question a second time, so it has to answer
                // the exemption a second time too: a veteran or a PvP-only bot
                // above the band was selected precisely BECAUSE it is above the
                // band, and is ported like anybody else.
                bool const outOfBand = zoneBanded && !KeepsItsOwnLevel(chosen) &&
                    (chosen->GetLevel() < levyBottom || chosen->GetLevel() > levyTop);
                if (outOfBand)
                {
                    if (LevyBotInto(chosen, human, levyTop))
                    {
                        chosen->SetBountyPursuit(spot.Bounty, 90 * IN_MILLISECONDS);
                        continue;
                    }
                    continue;                   // levy refused; do not port it as-is
                }

                // Leave them alone on the way. The window covers the teleport plus
                // the walk in from 210 yards with room to spare, and lapses by
                // itself - see Player::SetBountyPursuit for why it is a deadline
                // and not a flag.
                chosen->SetBountyPursuit(spot.Bounty, 90 * IN_MILLISECONDS);

                // Where it stood before it was called. try_emplace, so a hunter
                // handed a second bounty before it ever got home still remembers
                // the place it actually came from rather than the last battlefield
                // it was standing on - but the person it is hunting IS updated,
                // or the receipt would keep asking about somebody this bot has
                // long since stopped chasing.
                //
                // Never for a bot that is out on levy. That one already has a
                // receipt, and its release is a full re-level back to its own
                // band and zone; adding a positional receipt on top means
                // ReturnLeviedBots sends it home and then this pass teleports it
                // straight back to the bounty zone it was just recalled from.
                // One bot, one owner.
                uint64 const chosenRawGuid = chosen->GetGUID().GetRawValue();
                if (!g_LeviedBots.count(chosenRawGuid))
                {
                    auto const inserted = g_DeployedHunters.try_emplace(chosenRawGuid, DeployedHunter{
                        chosen->GetMapId(), chosen->GetPositionX(), chosen->GetPositionY(),
                        chosen->GetPositionZ(), chosen->GetOrientation(), humanGuid });
                    if (!inserted.second)
                        inserted.first->second.HumanGuid = humanGuid;

                    // Mirrored where the map-thread zone-fit sweep can see it.
                    PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, chosenRawGuid);
                    state.bountyDeployed = true;
                }

                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingGuardianTeleports[chosenRawGuid] =
                    { humanGuid, PvePlayerTeleportMinimumDistance };
            }

            std::string names;
            for (Player* chosen : sending)
                names += (names.empty() ? "" : ", ") +
                    std::string(playerbot::PveManager::IsPvpOnlyBot(chosen) ? "[pvp]" :
                        (IsLocalVeteranGuid(chosen->GetGUID().GetRawValue()) ? "[veteran]" : "[local]")) +
                    " " + chosen->GetName();

            TC_LOG_INFO("playerbots.pve",
                "Bounty {}: sending {} at {} (level {}); {} already there; next in {}s.",
                spot.Bounty, names, human->GetName(), uint32(human->GetLevel()), answering, wait);
        }

        for (auto itr = s_nextHuntAt.begin(); itr != s_nextHuntAt.end(); )
            itr = online.count(itr->first) ? std::next(itr) : s_nextHuntAt.erase(itr);
    }

    // World thread. Send somebody a fight when the world has stopped bringing
    // them one.
    //
    // The arrival is the guardian arrival, reused wholesale: dropped at 210
    // yards, which is outside the 200 yard sight range so the port itself is
    // never seen, then walked in until the ordinary acquisition radius picks the
    // person up. Nothing here targets anybody - it only decides who is owed a
    // visit and who is free to pay it.
    void ProdForgottenPlayers(std::vector<HumanSpot> const& spots,
        std::unordered_set<uint64> const& humansFightingABot,
        std::vector<Player*> const& proddableBots)
    {
        // Single-threaded state: this runs on the world thread only, like the
        // drifter dwell clock above it.
        static std::unordered_map<uint64, PveTimePoint> s_lastBotFightAt;
        static std::unordered_map<uint64, PveTimePoint> s_nextProdAt;

        PveTimePoint const now = PveClock::now();
        auto const idleFor = std::chrono::minutes(g_PveConfig.idleProdAfterMinutes);

        std::unordered_set<uint64> online;
        for (HumanSpot const& spot : spots)
        {
            uint64 const humanGuid = spot.Guid.GetRawValue();
            online.insert(humanGuid);

            // Nobody is "forgotten" who never asked to be found. Being left
            // alone is the whole point of playing with War Mode off, and a prod
            // is the single most intrusive thing this file does - it teleports a
            // bot to 210 yards and points it at you.
            if (!spot.Huntable)
                continue;

            // Somebody who has just arrived is not being neglected yet, so the
            // clock starts full rather than empty - otherwise a bot would be
            // posted through the door behind every login.
            auto const inserted = s_lastBotFightAt.emplace(humanGuid, now);
            if (humansFightingABot.count(humanGuid))
            {
                inserted.first->second = now;
                continue;
            }

            if (now - inserted.first->second < idleFor)
                continue;

            // A prod that never landed leaves the person exactly as alone as
            // before, so the retry clock is separate from the idle clock and
            // only the retry is reset here. Arriving and starting a fight resets
            // the idle clock on its own, above.
            auto const next = s_nextProdAt.find(humanGuid);
            if (next != s_nextProdAt.end() && now < next->second)
                continue;

            Player* human = ObjectAccessor::FindConnectedPlayer(spot.Guid);
            if (!human || !human->IsInWorld() || !human->IsAlive() ||
                human->InBattleground() || human->InArena())
                continue;

            // Somewhere a fight is possible at all. A prod into a starter zone
            // or a sanctuary delivers a stranger who cannot touch them, which is
            // worse than leaving them alone.
            if (!BarracksHardcore::IsOpenWorldPvpZone(human->GetZoneId()))
                continue;

            // Nearest first, and only from the person's own map. Preferring the
            // same ZONE keeps the arrival local: somebody who wandered in from
            // the next zone over is a neighbour, one hauled across a continent
            // is a spawn.
            std::vector<std::pair<float, Player*>> ranked;
            for (Player* bot : proddableBots)
            {
                if (bot->GetMapId() != human->GetMapId() || bot == human)
                    continue;

                // Never volunteer somebody into a fight they lose on arrival.
                if (!IsProactivePlayerLevelAcceptable(bot, human))
                    continue;

                // Same zone first, then nearest. An arrival from the next zone
                // over is a neighbour; one hauled across a continent is a spawn.
                float const distance = bot->GetDistance(human);
                bool const inZone = bot->GetZoneId() == human->GetZoneId();
                ranked.push_back({ inZone ? distance : distance + 100000.0f, bot });
            }
            std::sort(ranked.begin(), ranked.end(),
                [](auto const& a, auto const& b) { return a.first < b.first; });

            // The companion question, asked here and only here: it needs the
            // shared state mutex, and a companion torn away from the person it
            // is following would be a worse bug than a missed prod. Rare enough
            // to be a handful of lookups on the rare tick that sends anyone.
            Player* chosen = nullptr;
            for (auto const& [score, bot] : ranked)
            {
                PveBotState const* state = playerbot::LockedFind(g_PveBotStateByGuid,
                    bot->GetGUID().GetRawValue());
                if (!state || state->masterGuid.IsEmpty())
                {
                    chosen = bot;
                    break;
                }
            }

            if (!chosen)
                continue;

            s_nextProdAt[humanGuid] = now + std::chrono::seconds(g_PveConfig.idleProdRetrySeconds);
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingGuardianTeleports[chosen->GetGUID().GetRawValue()] =
                    { humanGuid, g_PveConfig.idleProdDropYards };
            }

            TC_LOG_INFO("playerbots.pve",
                "{} has not been in a fight for {}m; sending {} in from {:.0f}y.",
                human->GetName(), g_PveConfig.idleProdAfterMinutes, chosen->GetName(),
                g_PveConfig.idleProdDropYards);
        }

        for (auto itr = s_lastBotFightAt.begin(); itr != s_lastBotFightAt.end(); )
            itr = online.count(itr->first) ? std::next(itr) : s_lastBotFightAt.erase(itr);
        for (auto itr = s_nextProdAt.begin(); itr != s_nextProdAt.end(); )
            itr = online.count(itr->first) ? std::next(itr) : s_nextProdAt.erase(itr);
    }

    // World thread. Teleports one bot to a random spot for its level, with the
    // reference safety checks: loaded map, no enemy-faction zone, not into
    // water, grounded Z, and never in sight of a real player.
    // World thread: drop guardians next to a player.
    void ProcessPendingGuardianTeleports()
    {
        std::unordered_map<uint64, GuardianTeleportRequest> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingGuardianTeleports);
        }

        for (auto const& [botRawGuid, request] : drained)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() ||
                bot->InBattleground() || bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
                continue;

            Player* human = ObjectAccessor::FindConnectedPlayer(ObjectGuid(request.HumanRawGuid));
            if (!human || !human->IsInWorld() || !human->IsAlive() || human->InBattleground())
                continue;

            // The request may have sat in the queue while either character leveled.
            // Revalidate the proactive safety rule at execution time too.
            if (!IsProactivePlayerLevelAcceptable(bot, human))
                continue;

            // And while War Mode was switched off, or a bounty ran out. This is
            // the last line before somebody is teleported at a person, so it is
            // the one that has to be right even if a queued request outlived the
            // reason it was queued.
            if (!HumanMayBeHunted(human->GetGUID()))
                continue;

            // Land at the EDGE of the player's sight, not on top of them. A guardian
            // materialising at ten yards reads as a bug; one appearing at the far
            // edge of vision and walking in reads as somebody hunting you.
            // Distance chosen by the caller: normally just outside the player's
            // sight, closer once the guardian has gone hungry (see the escalation
            // in RunZoneGuardianTick).
            //
            // A geometrically nearby point is NOT necessarily connected to the
            // player by the navmesh: the other side of a cliff, wall, ridge or
            // ravine can be twenty yards away and still require an impossible
            // MovePoint shortcut. Never teleport a hunter to such a point.
            //
            // The bot may currently be on another map, so PathGenerator(bot) cannot
            // validate the hypothetical landing before the teleport. Player path
            // filters are identical here, so probe from the human out to the
            // landing point on the destination map. Outdoor player navmesh links
            // are bidirectional, making that the same connectivity test the bot
            // will use after it lands.
            // Hard safety invariant: no player-directed teleport may land closer
            // than 210 yards, regardless of what any caller requested.
            float const dropDistance = std::max(PvePlayerTeleportMinimumDistance, request.DropDistance);
            float const firstAngle = frand(0.0f, 2.0f * float(M_PI));
            constexpr uint8 kLandingAttempts = 8;

            bool foundReachableLanding = false;
            bool pathBudgetDeferred = false;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;

            for (uint8 attempt = 0; attempt < kLandingAttempts; ++attempt)
            {
                // Spread attempts around the whole ring instead of repeatedly
                // gambling on the same blocked side of the player.
                float const angle = firstAngle + (2.0f * float(M_PI) * float(attempt) / float(kLandingAttempts));
                human->GetNearPoint(bot, x, y, z, dropDistance, angle);

                // Fix the height against the HUMAN's map: the hunter may still be
                // on another continent, where that ground does not exist.
                human->UpdateAllowedPositionZ(x, y, z);

                // Somebody ELSE standing on this side of the ring.
                //
                // The drop distance keeps the landing out of the quarry's sight
                // and says nothing about anyone else: two people questing
                // together, and the bot lands 210 yards from one of them and
                // eight from the other. Rejecting the angle costs one more turn
                // of a ring that is already being walked, and no path budget -
                // this is a player-list scan, not a navmesh query, so it is
                // asked BEFORE the expensive test below.
                if (WouldLandInSightOfAnybody(human->FindMap(), x, y))
                    continue;

                WalkPathResult const pathResult = CheckWalkPath(human, x, y, z);
                if (pathResult == WalkPathResult::Deferred)
                {
                    pathBudgetDeferred = true;
                    break;
                }

                if (pathResult == WalkPathResult::Reachable)
                {
                    foundReachableLanding = true;
                    break;
                }
            }

            if (!foundReachableLanding)
            {
                if (pathBudgetDeferred)
                {
                    // This is not a failed route, only a saturated per-second path
                    // budget. Put the request back so it can be proven next tick;
                    // never turn "unknown" into permission to teleport.
                    std::lock_guard<std::mutex> guard(g_PvePendingLock);
                    g_PendingGuardianTeleports.try_emplace(botRawGuid, request);
                }
                else
                {
                    TC_LOG_INFO("playerbots.pve",
                        "Hunter {} refused teleport near {}: no walkable path from {} sampled {:.0f}y landing points.",
                        bot->GetName(), human->GetName(), uint32(kLandingAttempts), dropDistance);
                }
                continue;
            }

            if (!BotCanTeleportNow(bot))
                continue;

            if (bot->TeleportTo(human->GetMapId(), x, y, z, frand(0.0f, 6.28f)))
            {
                RestorePlayerbotTeleportVitals(bot);

                // Teleport only solves the coarse reposition. Do NOT force the human
                // into orderedTargetGuid here: that bypasses the ordinary proactive
                // acquisition gate and makes the teleport path own combat targeting.
                // Instead, use the same generic walked-journey layer used elsewhere
                // to close the short gap from the 210y landing toward the player's
                // current position. RunFastTick performs target acquisition BEFORE it
                // advances the journey, so as soon as the bot crosses inside the
                // normal ~200y PickHuntTarget radius, the existing combat logic takes
                // over naturally. No class-specific approach behavior lives here.
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                state.engaged = false;
                StartWalkedJourney(state, human->GetMapId(), human->GetPositionX(), human->GetPositionY(),
                    human->GetPositionZ(), 0, dropDistance);

                TC_LOG_INFO("playerbots.pve",
                    "Hunter {} teleported to a path-connected point {:.0f}y from {}; generic travel closes into normal acquisition range.",
                    bot->GetName(), dropDistance, human->GetName());
            }
        }
    }

    // Map::PlayerRelocation asserts that whatever it is moving is in a grid, and a
    // bot that an earlier pass of this same world update already teleported is NOT:
    // the grid link is dropped the moment a teleport begins and only restored when
    // it lands. Several passes here teleport - summons, class quest travel, supply
    // runs, guardian approaches, relocations, rebirths - so checking once at the
    // top of a loop is not enough. Arbitrary work runs between that check and the
    // call, and the bot may be picked up by something else in between.
    //
    // Test the assert's own precondition, immediately before the call.
    bool BotCanTeleportNow(Player* bot)
    {
        return bot && bot->IsInWorld() && bot->IsInGrid() &&
            !bot->IsBeingTeleportedFar() && !bot->IsBeingTeleportedNear();
    }

    // Teleports are artificial repositioning, not travel the bot had to survive.
    // Arrive ready to act instead of carrying half-dead / OOM state across the
    // map. Only health and mana are restored intentionally; rage/energy/runic
    // power retain their normal class semantics. Call this only after a
    // successful TeleportTo(), so a rejected teleport is never a free heal.
    void RestorePlayerbotTeleportVitals(Player* bot)
    {
        if (!bot || !bot->IsAlive())
            return;

        bot->SetFullHealth();
        if (bot->GetMaxPower(POWER_MANA) > 0)
            bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    }

    void ProcessPendingGrindRelocations()
    {
        std::unordered_set<uint64> drained;
        std::unordered_map<uint64, uint32> stuckDrained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            if (g_PveConfig.relocateEnabled)
                drained.swap(g_PendingGrindRelocations);
            else
                g_PendingGrindRelocations.clear();

            if (g_PveConfig.stuckRecoveryEnabled)
            {
                stuckDrained.swap(g_PendingStuckRelocations);
                for (auto const& request : stuckDrained)
                    drained.insert(request.first);
            }
            else
                g_PendingStuckRelocations.clear();
        }

        if (drained.empty())
            return;

        BuildGrindSpotCacheOnce();

        for (uint64 botRawGuid : drained)
        {
            auto const stuckItr = stuckDrained.find(botRawGuid);
            uint32 const stuckZoneId = stuckItr != stuckDrained.end() ? stuckItr->second : 0u;
            bool const stuckRecovery = stuckZoneId != 0;

            // Consumed up front so it cannot outlive the request that set it: every
            // path below may skip this bot, and a flag left set would follow it
            // into some later, unrelated relocation.
            bool fleeing = false;
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                fleeing = state.fleeingFromPlayers;
                state.fleeingFromPlayers = false;
            }

            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
            // A bot in the air is left alone. Teleporting mid-taxi tears the
            // flight generator off without letting it finalize, and finalize is
            // the only thing that clears UNIT_STATE_IN_FLIGHT and dismounts - so
            // the bot arrives still wearing the gryphon and frozen. The request
            // is dropped; whatever wanted it moved asks again once it lands.
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->InBattleground() ||
                bot->IsInFlight() ||
                bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
                continue;
            if (stuckRecovery && bot->GetZoneId() != stuckZoneId)
                continue;
            if (stuckRecovery)
            {
                PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                if (!IsStuckWatchdogEligible(bot, state, g_PveConfig, PveClock::now()))
                {
                    ResetStuckWatchdog(state);
                    continue;
                }
            }

            // Only the teleport arm below must hide from real players; walking
            // and flying are visible, legitimate travel.
            bool const humanNearby = HasHumanPlayerNearby(bot, 150.0f);

            std::vector<GrindSpot> candidates;
            if (stuckRecovery)
            {
                // Recovery is deliberately same-zone. Prefer clusters suitable
                // for this level, but if this zone has no such cluster, any
                // validated hostile-spawn cluster in the zone is a safer landing
                // than leaving the bot below terrain indefinitely.
                std::lock_guard<std::mutex> guard(g_GrindSpotLock);
                auto appendEligible = [&](std::vector<GrindSpot> const& spots)
                {
                    for (GrindSpot const& spot : spots)
                        if (spot.zoneId == stuckZoneId && spot.mapId == bot->GetMapId() &&
                            bot->GetDistance2d(spot.x, spot.y) >= g_PveConfig.stuckRecoveryDistanceYards)
                            candidates.push_back(spot);
                };

                uint8 const level = uint8(std::min<uint32>(bot->GetLevel(), 80));
                for (uint8 probe = 0; probe < 5 && candidates.empty() && level > probe; ++probe)
                {
                    auto itr = g_GrindSpotsByLevel.find(level - probe);
                    if (itr != g_GrindSpotsByLevel.end())
                        appendEligible(itr->second);
                }

                if (candidates.empty())
                    if (auto itr = g_GrindSpotsByZone.find(stuckZoneId); itr != g_GrindSpotsByZone.end())
                        appendEligible(itr->second);
            }
            else if (uint32 const guardianZoneId = GetGuardianZoneId(botRawGuid))
            {
                // Guardians only ever go home: search every bucket at or below
                // their level for clusters inside their zone, then - rather than
                // strand a guardian outside its post - every bucket at all.
                std::lock_guard<std::mutex> guard(g_GrindSpotLock);
                for (int32 level = int32(std::min<uint32>(bot->GetLevel(), 80)); level >= 1 && candidates.empty(); --level)
                {
                    auto itr = g_GrindSpotsByLevel.find(uint8(level));
                    if (itr == g_GrindSpotsByLevel.end())
                        continue;
                    for (GrindSpot const& spot : itr->second)
                        if (spot.zoneId == guardianZoneId)
                            candidates.push_back(spot);
                }
                if (candidates.empty())
                    for (auto const& [level, spots] : g_GrindSpotsByLevel)
                        for (GrindSpot const& spot : spots)
                            if (spot.zoneId == guardianZoneId)
                                candidates.push_back(spot);
            }
            else
            {
                uint32 const homeZoneId = g_PveConfig.rebirthZoneBanded ? GetRebirthZoneId(bot) : 0u;

                // A class-balanced home assignment is independent of the bot's
                // current level. Never let the relocation executor physically send
                // a below-band bot into that higher-level home before the band-reset
                // pass has raised it to the zone floor (or leave an over-band bot
                // grinding there after it should have cycled). The rebirth drain runs
                // later in this same world update, so queue the correction and skip
                // relocation entirely until level and home agree.
                if (homeZoneId)
                {
                    if (ClassicZoneBand const* homeBand = FindClassicZoneBand(homeZoneId))
                    {
                        uint32 const botLevel = bot->GetLevel();
                        if (botLevel < homeBand->minLevel || botLevel >= homeBand->maxLevel)
                        {
                            {
                                std::lock_guard<std::mutex> pendingGuard(g_PvePendingLock);
                                g_PendingRebirths.insert(botRawGuid);
                            }
                            TC_LOG_INFO("playerbots.pve",
                                "Bot {} relocation deferred: level {} does not fit home zone {} band {}-{}; queueing band reset.",
                                bot->GetName(), botLevel, homeZoneId, uint32(homeBand->minLevel), uint32(homeBand->maxLevel));
                            continue;
                        }
                    }
                }

                std::lock_guard<std::mutex> guard(g_GrindSpotLock);
                uint8 level = uint8(std::min<uint32>(bot->GetLevel(), 80));
                // Walk down a few brackets if the exact level has no clusters.
                // Also reject anomalous/custom low-level creature clusters that sit
                // inside a zone whose authoritative Classic band does not contain
                // this bot's level. Without this, one stray level-10 spawn in STV
                // can make STV look like a valid level-10 relocation destination.
                for (uint8 probe = 0; probe < 5 && candidates.empty() && level > probe; ++probe)
                {
                    auto itr = g_GrindSpotsByLevel.find(level - probe);
                    if (itr == g_GrindSpotsByLevel.end())
                        continue;

                    for (GrindSpot const& spot : itr->second)
                    {
                        if (ClassicZoneBand const* band = FindClassicZoneBand(spot.zoneId))
                            if (bot->GetLevel() < band->minLevel || bot->GetLevel() > band->maxLevel)
                                continue;
                        candidates.push_back(spot);
                    }
                }

                // Home first. Being tied to a zone has to mean living in it, not
                // merely being reborn there - and picking uniformly from every
                // cluster in a level bracket does not spread a fleet evenly, it
                // concentrates it in whichever zone owns the most clusters in the
                // most populated bracket. The Barrens is enormous and spans ten to
                // twenty five, so it collected a quarter of the fleet while zones
                // beside it held one bot each.
                //
                // Only when the bot's own zone has nothing at its level does it
                // look further afield, which is also how it climbs out of a zone
                // it has outgrown before rebirth catches up with it.
                if (homeZoneId)
                {
                    std::vector<GrindSpot> atHome;
                    for (GrindSpot const& spot : candidates)
                        if (spot.zoneId == homeZoneId)
                            atHome.push_back(spot);

                    if (!atHome.empty())
                        candidates.swap(atHome);
                }
            }

            if (candidates.empty())
                continue;

            if (!stuckRecovery && fleeing && g_PveConfig.timidFleeYards > 0.0f)
            {
                // Retreat is a move within the bot's own zone, not a migration: it
                // wants to be away from whoever just killed it, not relocated
                // somewhere it had no business being. A guardian's zone is its post;
                // everyone else uses the ground they are standing on.
                uint32 const guardianZone = GetGuardianZoneId(botRawGuid);
                uint32 const retreatZone = guardianZone ? guardianZone : bot->GetZoneId();

                std::vector<GrindSpot> quiet;
                for (GrindSpot const& spot : candidates)
                {
                    if (spot.zoneId != retreatZone)
                        continue;

                    HumanSpot nearest;
                    float distance = 0.0f;
                    if (!FindNearestHumanSpot(spot.mapId, spot.x, spot.y, spot.z, nearest, distance) ||
                        distance > g_PveConfig.timidFleeYards)
                        quiet.push_back(spot);
                }

                // Busy zone with nowhere clear to go: stay put rather than teleport
                // across it to somewhere no quieter. Skipping the trip costs the bot
                // nothing - it is still timid, so it grinds where it stands and does
                // not go looking for another fight either way.
                if (quiet.empty())
                {
                    TC_LOG_INFO("playerbots.pve",
                        "Bot {} stays put: nowhere in zone {} is {}y clear of a person.",
                        bot->GetName(), retreatZone, uint32(g_PveConfig.timidFleeYards));
                    continue;
                }

                candidates.swap(quiet);
            }

            uint8 const maxAttempts = uint8(std::min<size_t>(10, candidates.size()));
            for (uint8 attempt = 0; attempt < maxAttempts; ++attempt)
            {
                // Draw without replacement: a bad water/height candidate must not
                // consume all ten attempts merely because the RNG picked it again.
                uint32 const candidateIndex = urand(0, uint32(candidates.size() - 1));
                GrindSpot const spot = candidates[candidateIndex];
                candidates[candidateIndex] = candidates.back();
                candidates.pop_back();

                // Walk when the spot is on this map within range: visible travel
                // beats teleporting, and walking needs no vanish-guards.
                bool walkAllowed;
                {
                    PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                    walkAllowed = PveClock::now() >= state.walkFallbackUntil;
                }
                if (!stuckRecovery && walkAllowed && g_PveConfig.travelWalkMaxDistance > 0.0f &&
                    spot.mapId == bot->GetMapId())
                {
                    float const walkDistance = bot->GetDistance(spot.x, spot.y, spot.z);
                    if (walkDistance <= g_PveConfig.travelWalkMaxDistance)
                    {
                        playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
                        PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                        state.engaged = false;
                        StartWalkedJourney(state, spot.mapId, spot.x, spot.y, spot.z, 1, walkDistance);
                        TC_LOG_INFO("playerbots.pve", "Bot {} walking {:.0f}y to a new grind spot.",
                            bot->GetName(), walkDistance);
                        break;
                    }
                }

                // A real flight beats teleporting when a route exists.
                if (!stuckRecovery && TryTaxiTravel(bot, botRawGuid, spot.mapId, spot.x, spot.y, spot.z, 1))
                    break;

                // Somebody is watching, so a teleport would be seen - normally reason
                // enough to give up. But a bot that just lost a fight is very likely
                // standing next to whoever killed it, which is exactly when leaving
                // matters most; being seen to vanish is a smaller sin than being
                // farmed at a graveyard.
                // Either way keep drawing rather than breaking: the candidate may
                // simply have been out of walking range, and abandoning the whole
                // retreat after one unlucky draw is how the camped case kept failing.
                if (humanNearby && !fleeing && !stuckRecovery)
                    continue;

                Map* map = sMapMgr->FindMap(spot.mapId, 0);
                if (!map)
                    continue;

                uint32 const zoneId = map->GetZoneId(PHASEMASK_NORMAL, spot.x, spot.y, spot.z);
                if (stuckRecovery && zoneId != stuckZoneId)
                    continue;
                // Ordinary relocation must not deliver a bot into an enemy
                // capital/territory. A same-zone rescue does not cross that
                // boundary; rejecting it here would strand a bot that was already
                // lawfully adventuring in hostile territory.
                if (!stuckRecovery)
                {
                    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
                    {
                        if (bot->GetTeamId() == TEAM_ALLIANCE && zone->FactionGroupMask == 4)
                            continue;
                        if (bot->GetTeamId() == TEAM_HORDE && zone->FactionGroupMask == 2)
                            continue;
                    }
                }

                if (map->IsInWater(PHASEMASK_NORMAL, spot.x, spot.y, spot.z))
                    continue;

                // Search only around the source spawn's own floor. The default
                // 50-yard height search can snap a relocation onto a different
                // terrain/WMO layer above or below the creature cluster.
                float const ground = map->GetHeight(PHASEMASK_NORMAL, spot.x, spot.y,
                    spot.z + 3.0f, true, 8.0f);
                if (ground <= INVALID_HEIGHT)
                    continue;

                playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
                {
                    // Keep the per-bot flag in sync with the registry, or the
                    // bot's next fights run with the class-spell gate closed.
                    PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                    state.engaged = false;
                }
                if (MotionMaster* motionMaster = bot->GetMotionMaster())
                    motionMaster->Clear();
                if (!BotCanTeleportNow(bot))
                    break;

                if (!bot->TeleportTo(spot.mapId, spot.x, spot.y, ground + 0.05f, frand(0.0f, 6.28f)))
                    continue;

                RestorePlayerbotTeleportVitals(bot);

                if (stuckRecovery)
                {
                    // Start fresh at the rescue point. Otherwise a journey,
                    // errand or corpse walk that caused the stall can immediately
                    // steer the bot back into the same mountain or river.
                    PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                    ResetStuckWatchdog(state);
                    state.journeyActive = false;
                    state.journeyFallbackKind = 0;
                    state.errandGuid = ObjectGuid::Empty;
                    state.errandKind = PveErrandKind::None;
                    state.errandUntil = {};
                    state.pendingLootGuid = ObjectGuid::Empty;
                    state.dryWanderCount = 0;
                    state.nextWanderAt = {};
                    TC_LOG_INFO("playerbots.pve",
                        "Recovered stuck bot {} within zone {} at map {} {:.0f} {:.0f}.",
                        bot->GetName(), stuckZoneId, spot.mapId, spot.x, spot.y);
                }
                else
                    TC_LOG_INFO("playerbots.pve", "Relocated grind bot {} (level {}) to map {} {:.0f} {:.0f}.",
                        bot->GetName(), bot->GetLevel(), spot.mapId, spot.x, spot.y);
                break;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Gameobject quest objectives (goobers grant RequiredNpcOrGo<0 credit via
    // Use(); quest chests are looted for their quest items)
    // ---------------------------------------------------------------------------

    struct QuestGameObjectCheck
    {
        Player* bot;
        bool wantQuestObjects;
        bool wantGatherNodes;
        uint32 deathChestEntry; // 0 when full loot is off

        bool operator()(GameObject* go) const
        {
            if (!go->isSpawned())
                return false;

            // Full-loot death chests. Nothing here ever matched one: a death chest
            // is not a quest object and not a gather node, so ActivateToQuest was
            // always false for it and the errand scan simply never saw it. The
            // walk-back journey delivered the bot to the spot and then it stood on
            // top of its own cache indefinitely, which is exactly what was seen in
            // game. Any such chest in range is worth taking - a stale one left by
            // another bot recycles that gear instead of despawning it.
            if (deathChestEntry && go->GetEntry() == deathChestEntry &&
                go->GetGoType() == GAMEOBJECT_TYPE_CHEST && go->getLootState() == GO_READY)
                return true;

            if (wantQuestObjects && go->ActivateToQuest(bot))
            {
                GameobjectTypes const type = go->GetGoType();
                if (type == GAMEOBJECT_TYPE_GOOBER || type == GAMEOBJECT_TYPE_CHEST)
                    return true;
            }

            return wantGatherNodes && IsGatherableNodeFor(bot, go, nullptr);
        }
    };

    GameObject* FindNearestQuestGameObject(Player* bot, PveBotState& state, float radius)
    {
        std::vector<GameObject*> matches;
        playerbot::PveConfig const& cfg = playerbot::PveManager::GetConfig();
        QuestGameObjectCheck check{ bot, cfg.questsEnabled && BotHasIncompleteQuest(bot), cfg.professionsEnabled,
            cfg.hardcoreLootChestEntry };
        Trinity::GameObjectListSearcher<QuestGameObjectCheck> searcher(bot, matches, check);
        Cell::VisitGridObjects(bot, searcher, radius);

        GameObject* nearest = nullptr;
        float nearestDistance = 0.0f;
        for (GameObject* candidate : matches)
        {
            if (IsRecentErrandTarget(state, candidate->GetGUID()))
                continue;

            float const distance = bot->GetDistance(candidate);
            if (!nearest || distance < nearestDistance)
            {
                nearest = candidate;
                nearestDistance = distance;
            }
        }

        return nearest;
    }

    // Returns true while the bot is still working on the object, so the caller
    // keeps the errand alive instead of clearing it mid-cast.
    bool UseQuestGameObject(Player* bot, PveBotState& state, GameObject* go)
    {
        if (go->GetGoType() == GAMEOBJECT_TYPE_GOOBER)
        {
            // Nothing to retry on a goober: one Use and it is done either way.
            MarkRecentErrandTarget(state, go->GetGUID());
            go->Use(bot);
            TC_LOG_INFO("playerbots.pve", "Bot {} used quest object {} ({}).",
                bot->GetName(), go->GetEntry(), go->GetGOInfo() ? go->GetGOInfo()->name : "");
            return false;
        }

        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
        {
            MarkRecentErrandTarget(state, go->GetGUID());
            return false;
        }

        // Second visit: the open is already under way.
        if (state.chestOpeningGuid == go->GetGUID())
        {
            if (playerbot::PvpClassActions::IsBattlegroundObjectInteractionInProgress(bot) ||
                bot->HasUnitState(UNIT_STATE_CASTING))
                return true;                       // still channelling - let it finish

            state.chestOpeningGuid.Clear();

            // FINISHED, or CANCELLED? UNIT_STATE_CASTING cannot tell them apart,
            // and this used to assume success.
            //
            // A successful Opening runs GameObject::Use, which moves the chest to
            // GO_ACTIVATED. A cancelled channel leaves it exactly as it was, on
            // GO_READY - and a channel is cancelled by a step of movement, a hit,
            // or the rogue deciding to stealth. The old code queued a loot
            // session anyway; the world thread then found the bot had already
            // walked off, failed its range check and dropped the loot silently.
            // That is the bot seen walking up to a chest, doing nothing, and
            // leaving to go and fight something.
            if (go->getLootState() == GO_READY)
            {
                TC_LOG_DEBUG("playerbots.pve", "Bot {} had its open of chest {} cancelled; retrying.",
                    bot->GetName(), go->GetEntry());
                return true;                       // hold the errand and try again
            }

            // Really open. Take the contents - the chest loot session mutates
            // group loot state, so it executes on the world thread with the
            // corpse loot - and hold the errand until it has, so nothing walks
            // the bot out of range in the meantime.
            MarkRecentErrandTarget(state, go->GetGUID());
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingLootExecutions[bot->GetGUID().GetRawValue()] = go->GetGUID();
            }
            state.errandUntil = std::min(state.errandUntil,
                PveClock::now() + std::chrono::seconds(3));
            return true;
        }

        // A loot session is queued for this bot and has not run yet. Stand still
        // until it does: the executor checks range, and letting the bot pick a
        // new target in the second before the world tick is what turned an
        // opened chest into an empty one.
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            if (g_PendingLootExecutions.count(bot->GetGUID().GetRawValue()))
                return true;
        }

        if (go->getLootState() != GO_READY)
        {
            MarkRecentErrandTarget(state, go->GetGUID());
            return false;
        }

        // Room to empty it BEFORE it is opened, not after.
        //
        // Opening is what CONSUMES a chest: GameObject::Use moves it GO_READY ->
        // GO_ACTIVATED and hands this bot the loot session. The executor's own
        // bag-space gate runs a tick later, so a bot with a full pack used to
        // open the cache, then bail out of taking anything - leaving it spent.
        // Once it is out of GO_READY it also stops sparkling (GameObject.cpp
        // only lights a chest that is GO_READY and unlooted), and the very gate
        // above sends every LATER bot away from it. That is the reported bug:
        // two bots at a cache, no cast bar, nothing taken, both walk off, and
        // the cache dead behind them.
        //
        // The remote path has always screened this (CountFreeBagSlots >= 2 when
        // it builds its candidate list); the walked path never did.
        if (CountFreeBagSlots(bot) < 2)
        {
            MarkRecentErrandTarget(state, go->GetGUID());
            return false;
        }

        // Open it the way a player does, mirroring the battleground node
        // interaction that already channels correctly (PlayerbotPvpLifecycleActions,
        // "Node banners use their lock spell").
        //
        // Ask the OBJECT for its lock spell rather than hardcoding Opening: the lock
        // is what decides which spell and how long it takes, and a hardcoded id is
        // wrong the moment a chest carries a different one.
        SpellInfo const* lockSpell = go->GetSpellForLock(bot);

        // Both of these cancel an interruptible OPEN_LOCK outright, and the bot
        // arrives here under MoveTowardThrottled - so without stopping first it
        // cancels its own channel on the very next step, which is exactly why no
        // cast bar was ever visible. The battleground path stops movement and
        // dismounts for the same reason.
        if (bot->IsMounted())
            bot->Dismount();

        playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
        if (MotionMaster* motionMaster = bot->GetMotionMaster())
            motionMaster->Clear();
        bot->StopMoving();
        bot->AttackStop();

        state.chestOpeningGuid = go->GetGUID();
        if (lockSpell)
            bot->CastSpell(go, lockSpell->Id, false);
        else
            go->Use(bot);
        TC_LOG_INFO("playerbots.pve", "Bot {} begins opening chest {} ({:.0f}y).",
            bot->GetName(), go->GetEntry(), bot->GetDistance(go));
        return true;
    }

    bool BotHasIncompleteQuest(Player* bot)
    {
        for (auto const& [questId, status] : bot->getQuestStatusMap())
            if (status.Status == QUEST_STATUS_INCOMPLETE)
                return true;

        return false;
    }

    Unit* PickCompanionTarget(Player* bot, PveBotState& state, Player* master, playerbot::PveConfig const& cfg)
    {
        Unit* best = nullptr;
        float bestDistance = 0.0f;
        auto consider = [&](Unit* candidate)
        {
            if (!candidate || !candidate->IsAlive() || !bot->IsValidAttackTarget(candidate))
                return;

            if (IsRecentBadTarget(state, candidate->GetGUID()))
                return;

            float const distance = bot->GetDistance(candidate);
            if (distance > cfg.companionAssistRadius)
                return;

            if (!best || distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        };

        for (Unit* attacker : bot->getAttackers())
            consider(attacker);

        if (master)
        {
            for (Unit* attacker : master->getAttackers())
                consider(attacker);

            // Assist the master's active fight, but never open on something the
            // master merely has selected.
            if (master->IsInCombat())
                consider(master->GetVictim());
        }

        if (Group* group = bot->GetGroup())
        {
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                if (slot.guid == bot->GetGUID() || (master && slot.guid == master->GetGUID()))
                    continue;

                if (Player* member = ObjectAccessor::GetPlayer(*bot, slot.guid))
                    for (Unit* attacker : member->getAttackers())
                        consider(attacker);
            }
        }

        return best;
    }

    void ExecuteEngagedCombatTick(Player* bot, PveBotState& state)
    {
        // Feign Death is the hunter's escape hatch and nothing else in the game
        // does what it does: it drops PvE combat outright rather than merely
        // running away from it. A hunter losing a fight plays dead, the mobs go
        // home, and it stands up to eat. Only against creatures - a real player
        // just waits for the hunter to get up - and only when the fight is
        // genuinely lost.
        // Only after the fight has actually been fought. Feigning the instant
        // something aggroes reads as a bot that cannot decide whether it wants to
        // fight: it pulls, drops combat immediately, the mob walks home, and it
        // does it again. Five seconds in means this fight was lost, not declined.
        bool const fightIsLost = state.engagedSince != PveTimePoint() &&
            PveClock::now() - state.engagedSince >= std::chrono::seconds(5);

        if (bot->GetClass() == CLASS_HUNTER && fightIsLost && bot->GetHealthPct() < 25.0f && bot->IsInCombat() &&
            bot->HasSpell(SPELL_HUNTER_FEIGN_DEATH) && !bot->GetSpellHistory()->HasCooldown(SPELL_HUNTER_FEIGN_DEATH) &&
            !bot->HasAuraType(SPELL_AURA_FEIGN_DEATH) && !bot->HasUnitState(UNIT_STATE_CASTING))
        {
            bool playerAttacker = false;
            for (Unit const* attacker : bot->getAttackers())
                if (attacker && attacker->GetTypeId() == TYPEID_PLAYER)
                    playerAttacker = true;

            if (!playerAttacker)
            {
                bot->CastSpell(bot, SPELL_HUNTER_FEIGN_DEATH, false);
                TC_LOG_DEBUG("playerbots.pve", "Bot {} feigns death to break combat at {:.0f}% health.",
                    bot->GetName(), bot->GetHealthPct());
                DisengagePveCombat(bot, state);
                // Stay down long enough for the animation to actually play. The
                // PvP path gets this from its scheduled stand-up; here the very
                // next tick would otherwise move the bot and cancel the feign
                // outright, so it never visibly hit the ground at all.
                state.feignHoldUntil = PveClock::now() + std::chrono::milliseconds(500);
                return;
            }
        }

        // Send the pet in. Nothing in the PvE tick ever commanded a pet, so a
        // defensive pet only ever reacted to what hit it - and a pet that is not
        // attacking cannot growl, cannot hold threat, and leaves every mob free
        // to walk past it to the bot.
        if (Pet* pet = bot->GetPet(); pet && pet->IsAlive())
        {
            if (Unit* petVictim = ResolveAttackableByGuid(bot, bot->GetTarget()))
                if (pet->GetVictim() != petVictim)
                    playerbot::PvpClassActions::CommandPetAttack(bot, petVictim);

            DrivePetGrowl(bot);
        }

        // Outside battlegrounds the values snapshot is all-default; the class
        // context builder only consults it for battleground triggers.
        playerbot::PvpValues const values{};
        playerbot::PvpClassSpellContext const context = playerbot::PvpCore::BuildClassSpellContext(bot, values);
        bool const executed = playerbot::PvpClassActions::Execute(bot, context);

        Unit* victim = ResolveAttackableByGuid(bot, bot->GetTarget());

        if (playerbot::PveManager::GetConfig().combatDiagnostics)
        {
            PveTimePoint const now = PveClock::now();
            if (now >= state.nextCombatDiagAt)
            {
                state.nextCombatDiagAt = now + std::chrono::seconds(2);
                TC_LOG_INFO("playerbots.pve",
                    "combatdiag bot={} target={} dist={:.1f} exec={} should={} action={} reason={} spell={} moveDir={} lastExec=[{}] victim={} inCombat={} o={:.2f} moving={} motion={}",
                    bot->GetName(), victim ? victim->GetName() : "none",
                    victim ? bot->GetDistance(victim) : -1.0f,
                    executed ? 1 : 0, context.shouldExecute ? 1 : 0,
                    context.actionName ? context.actionName : "-",
                    context.reason ? context.reason : "-",
                    context.spellId, uint32(context.movementDirective),
                    playerbot::PvpClassActions::GetLastExecutionStatus(bot),
                    bot->GetVictim() ? "yes" : "no", bot->IsInCombat() ? 1 : 0,
                    bot->GetOrientation(), bot->isMoving() ? 1 : 0,
                    bot->GetMotionMaster() ? uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()) : 999);
            }
        }

        if (!victim)
            return;

        // White-swing floor: a fresh low-level bot can know nothing castable, and
        // the decision graph then yields neither a spell nor a movement directive.
        // Plain chase plus auto-attack is what a player of that level does too.
        bool const engineActedThisTick = executed && context.shouldExecute;

        // Auto-attack parity with a real client: pressing any offensive ability
        // turns melee on and casting never turns it off. Bots have no client,
        // and the class-action cast paths establish spell victims through
        // Attack(target, false) - which Unit::Attack treats as a DOWNGRADE,
        // clearing UNIT_STATE_MELEE_ATTACKING and broadcasting attack-stop
        // (Player::Update gates every white swing on that state). Re-assert
        // melee after the engine has acted each tick, or casting classes fight
        // entire battles with auto-attack visibly off and no rage/dodge flow.
        //
        // Stealth holds the swings back so the rotation can open from stealth -
        // but only briefly once in melee range. A bot with no opener to cast
        // (a B+ fresh rogue knows ONLY Stealth) would otherwise stand next to
        // its target forever; a real player in that spot just starts attacking.
        bool const stealthed = bot->HasAuraType(SPELL_AURA_MOD_STEALTH);
        if (!stealthed)
            state.stealthOpenerDeadline = PveTimePoint();
        else if (bot->IsWithinMeleeRange(victim) && state.stealthOpenerDeadline == PveTimePoint())
            state.stealthOpenerDeadline = PveClock::now() + std::chrono::seconds(4);

        bool const holdSwingsForOpener = stealthed &&
            (state.stealthOpenerDeadline == PveTimePoint() || PveClock::now() < state.stealthOpenerDeadline);

        // ...but a hunter equipped to shoot must NOT have white swings turned on,
        // and this is the loop that kept it in melee no matter how the positioning
        // was tuned:
        //
        //   melee auto-attack on -> the hunter's own swings generate melee threat
        //   -> the mob peels off the pet and back onto the hunter -> the mob is now
        //   within five yards, so PvpCore flips the bot into melee MODE, where every
        //   ranged shot is gated off and only melee is offered -> and because the mob
        //   is on the hunter, the "stand and fight when aggroed" guard in
        //   DriveHunterRangedPositioning declines to back out at all.
        //
        // Every step justified the next, and the bot manufactured the very aggro
        // that kept it there. Positioning could never win because it was arguing
        // with threat the bot was generating itself, one swing at a time.
        //
        // Auto Shot is a hunter's white damage; melee is only correct once the mob
        // is genuinely on the hunter and backing off would just feed it free swings.
        // Attack(victim, false) is the same downgrade the cast paths use - it clears
        // UNIT_STATE_MELEE_ATTACKING and stops the swings without dropping the target.
        bool const wantsMeleeSwings = !BotShouldHoldRangedFiringLine(bot) || victim->GetVictim() == bot;

        // Symmetric on purpose. Unit::Attack(victim, false) is what CLEARS
        // UNIT_STATE_MELEE_ATTACKING and sends attack-stop, so the transition out of
        // melee only happens if the call is actually made - testing only the "melee
        // is missing" direction would turn swings on and never off again.
        bool const meleeStateWrong = wantsMeleeSwings != bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);

        // Never open the swing on a person the proactive rule refuses.
        //
        // Attack() sets the victim, and once it is set PlayerbotPvpCore.cpp's
        // hostile-target check reads it as self-defence and waives everything
        // downstream - so an unasked-for swing here launders itself into a fight
        // the bot is allowed to be in. Retaliation is untouched: IsSwingingAt is
        // the same "they started it" test the rest of this file uses, and
        // MayProactivelyEngage answers yes for battlegrounds, duels and bot-on-bot
        // long before it looks at levels.
        //
        // Disengage rather than just return, or the bot keeps its selection and
        // its engaged flag and stands there facing somebody it will never swing at.
        if (Player* victimPlayer = victim->ToPlayer())
            if (!playerbot::IsManagedRandomBot(victimPlayer) && !IsSwingingAt(bot, victimPlayer) &&
                !playerbot::PveManager::MayProactivelyEngage(bot, victimPlayer))
            {
                DisengagePveCombat(bot, state);
                return;
            }

        if ((bot->GetVictim() != victim || meleeStateWrong) && !holdSwingsForOpener)
            bot->Attack(victim, wantsMeleeSwings);

        // Never issue chase movement mid-cast: walking cancels the baseline
        // nuke the block below just started.
        bool const casting = bot->HasUnitState(UNIT_STATE_CASTING);
        bool const mayIssueChase = !engineActedThisTick && !casting;

        // Hunters hold a firing line rather than closing to melee - and this is
        // deliberately NOT gated on the engine having been idle.
        //
        // In combat the engine acts on very nearly every tick, and it is the
        // engine's own melee abilities that pull a hunter in: it offers Mongoose
        // Bite or Raptor Strike, walks into range to use them, and the hunter is
        // then in melee mode, where every ranged shot is gated off and only more
        // melee is on offer. Waiting for a tick where the engine did nothing meant
        // that never unwound - the hunter went in for one bite and stayed for the
        // rest of the fight.
        //
        // Backing out restores the distance, which flips the mode back at eight
        // yards, which stops the engine offering melee at all. So it converges
        // rather than fighting: one correction and the loop is broken.
        bool const hunterHoldsRange = DriveHunterRangedPositioning(bot, victim, !casting);

        // The hunter rule, plainly:
        //
        //   the mob is on the HUNTER        -> close and melee it
        //   the pet or anything else has it -> hold a firing line and shoot
        //
        // DriveHunterRangedPositioning above owns the second case and declines the
        // first, which is why the first is handled here.
        bool const standAndFight = victim->GetVictim() == bot;

        if (standAndFight)
        {
            // Closing must NOT wait for a tick where the engine happened to do
            // nothing. In combat the engine acts on nearly every tick, so gating
            // this on engineActedThisTick meant no order was ever issued - and a
            // stale twelve yard follow generator from the ranged case then held the
            // hunter at range while the mob chewed on it. That is a level 6 hunter
            // walking away from the plainstrider hitting it.
            //
            // Still not mid-cast: walking cancels the cast, and the next tick would
            // simply re-issue this.
            if (!casting && !bot->IsWithinMeleeRange(victim))
                playerbot::PvpClassActions::IssueFollowMovement(bot, victim, 1.0f);
        }
        else if (!hunterHoldsRange && mayIssueChase && !bot->IsWithinMeleeRange(victim))
        {
            // Something else is holding the mob, so a hunter equipped to shoot
            // keeps its distance instead of walking into the target's face. Anyone
            // who cannot shoot still closes.
            float const chaseDistance = BotShouldHoldRangedFiringLine(bot) ? 12.0f : 1.0f;
            playerbot::PvpClassActions::IssueFollowMovement(bot, victim, chaseDistance);
        }

        // Track the victim continuously, like a real player's client does.
        // For a socketless bot SetInFront routes through the player-style
        // MSG_MOVE_SET_FACING broadcast (no-op below an epsilon), so calling
        // it every tick is cheap when already aligned - and only correcting
        // once the victim left the 120-degree SWING arc left bots visibly
        // fighting sideways for the whole 60-100 degree band. Mid-spline only
        // the server orientation may change (the spline owns what observers
        // render), and there it only matters for landing swings.
        if (!bot->HasUnitState(UNIT_STATE_CASTING))
        {
            bool const splineActive = !bot->IsStopped() || (bot->movespline && !bot->movespline->Finalized());
            if (!splineActive)
                bot->SetInFront(victim);
            else if (bot->IsWithinMeleeRange(victim) && !bot->HasInArc(2.0f * float(M_PI) / 3.0f, victim))
            {
                // A bot that is not actually moving yet still reports an active
                // spline is stuck behind a phantom one, and every broadcast
                // path defers to it (the move_face_deferred loop) - observers
                // keep rendering the bot fighting with its back turned. Kill
                // the phantom so the turn actually publishes.
                if (!bot->isMoving() && !bot->HasInArc(float(M_PI), victim))
                    bot->StopMoving();
                bot->SetInFront(victim);
            }
        }

        // Fresh talentless casters match no branch of the spec-gated rotation
        // and were left white-swinging with a mace. A real low-level player
        // leads with the class's baseline nuke; do the same when the engine cast
        // NOTHING this tick - facing/positioning busywork (the "set facing"
        // loop) and movement directives count as acted but must not starve the
        // nuke, or a fresh priest circles its target forever without a Smite.
        // STRICTLY pre-talent (level < 10): from 10 up the detected spec owns
        // the rotation, and filling its deliberate gaps with Lightning Bolt
        // turned an enhancement shaman into a caster.
        bool const engineCastThisTick = engineActedThisTick && context.spellId != 0;

        // Paladins at any level: keep a seal up and judge it. Leveling
        // paladins match no spec branch, and the idle-tick floor never fights
        // a talented rotation - that one casts on its own ticks.
        if (!engineCastThisTick && !bot->HasUnitState(UNIT_STATE_CASTING) && bot->GetClass() == CLASS_PALADIN)
        {
            if (uint32 const sealId = HighestKnownRankInChain(bot, 21084)) // Seal of Righteousness
            {
                if (!bot->HasAura(sealId))
                    bot->CastSpell(bot, sealId, false);
                else if (bot->HasSpell(20271) && !bot->GetSpellHistory()->HasCooldown(20271) &&
                    bot->IsWithinDistInMap(victim, 10.0f) && bot->IsWithinLOSInMap(victim))
                    bot->CastSpell(victim, 20271, false); // Judgement
            }
        }

        if (!engineCastThisTick && bot->GetLevel() < 10 && !bot->HasUnitState(UNIT_STATE_CASTING))
        {
            if (uint32 const nukeId = BaselineNukeSpellId(bot))
            {
                SpellInfo const* nukeInfo = sSpellMgr->GetSpellInfo(nukeId);
                if (nukeInfo && !bot->GetSpellHistory()->HasGlobalCooldown(nukeInfo) &&
                    !bot->GetSpellHistory()->HasCooldown(nukeId) &&
                    bot->IsWithinDistInMap(victim, 25.0f) && bot->IsWithinLOSInMap(victim))
                {
                    if (bot->isMoving())
                    {
                        playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                        if (MotionMaster* motionMaster = bot->GetMotionMaster())
                            motionMaster->Clear();
                        bot->StopMoving();
                    }
                    if (!bot->isInFront(victim))
                    {
                        bot->SetFacingToObject(victim);
                        bot->SetInFront(victim);
                    }
                    bot->CastSpell(victim, nukeId, false);
                }
            }
        }

        // Stalled-chase recovery: a chase generator issued while the
        // MotionMaster sat in its pending state never actually moves, and the
        // movement throttle then "preserves" the dead chase forever
        // (motion=chase, chase_move=no, moving=no). If an engaged bot with a far
        // target holds the same position for ~1.5s, rebuild its movement.
        float const victimDistance = bot->GetDistance(victim);
        if (victimDistance > 35.0f && !bot->HasUnitState(UNIT_STATE_CASTING))
        {
            float const deltaX = bot->GetPositionX() - state.lastEngagedX;
            float const deltaY = bot->GetPositionY() - state.lastEngagedY;
            if (deltaX * deltaX + deltaY * deltaY < 0.25f)
            {
                if (++state.engagedStallTicks >= 6 && !bot->isMoving())
                {
                    state.engagedStallTicks = 0;

                    // Rebuilding the chase only helps when the target IS
                    // reachable. A mob across a canyon can never be reached, and
                    // nothing else lets go of it: an engaged bot skips every
                    // errand, rest, relocation and wander branch, and the victim
                    // keeps resolving because an undamaged mob never dies, never
                    // evades and never leaves. Such a bot ran the full combat
                    // decision engine four times a second, forever - which is
                    // what pinned the map threads.
                    if (victim->GetGUID() == state.lastRecoveryVictim)
                        ++state.consecutiveChaseRecoveries;
                    else
                    {
                        state.lastRecoveryVictim = victim->GetGUID();
                        state.consecutiveChaseRecoveries = 1;
                    }

                    if (state.consecutiveChaseRecoveries >= 3)
                    {
                        TC_LOG_INFO("playerbots.pve", "Bot {} gives up on unreachable {} at {:.0f}y.",
                            bot->GetName(), victim->GetName(), victimDistance);
                        MarkRecentBadTarget(state, victim->GetGUID());
                        state.consecutiveChaseRecoveries = 0;
                        state.lastRecoveryVictim.Clear();
                        DisengagePveCombat(bot, state);
                        return;
                    }

                    if (MotionMaster* motionMaster = bot->GetMotionMaster())
                        motionMaster->Clear();
                    playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                    playerbot::PvpClassActions::IssueFollowMovement(bot, victim, 25.0f);
                    TC_LOG_DEBUG("playerbots.pve", "Bot {} recovered a stalled chase toward {} at {:.0f}y.",
                        bot->GetName(), victim->GetName(), victimDistance);
                }
            }
            else
                state.engagedStallTicks = 0;

            state.lastEngagedX = bot->GetPositionX();
            state.lastEngagedY = bot->GetPositionY();
        }
        else
            state.engagedStallTicks = 0;
    }

    bool TryJoinSummonerGroup(Player* summoner, Player* bot)
    {
        // A pending invite from some other group would leave a dangling invitee
        // entry behind a direct AddMember.
        if (bot->GetGroupInvite())
            bot->UninviteFromGroup();

        if (bot->GetGroup())
        {
            if (bot->GetGroup() == summoner->GetGroup())
                return true;

            bot->RemoveFromGroup();
        }

        Group* group = summoner->GetGroup();
        if (!group)
        {
            group = new Group;
            if (!group->Create(summoner))
            {
                delete group;
                return false;
            }

            sGroupMgr->AddGroup(group);
        }

        if (group->IsFull())
        {
            bot->Whisper("Your group is full.", LANG_UNIVERSAL, summoner);
            return false;
        }

        if (!group->AddMember(bot))
            return false;

        group->BroadcastGroupUpdate();
        return true;
    }

    void ProcessPendingSummons()
    {
        std::unordered_map<uint64, PendingSummon> snapshot;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            snapshot = g_PendingSummonsByBotGuid;
        }

        uint32 const nowMs = GameTime::GetGameTimeMS();
        for (auto const& pendingEntry : snapshot)
        {
            // Not a structured binding: the erasePending lambda below must capture
            // these, which C++17 forbids for bindings (clang enforces it).
            uint64 const botRawGuid = pendingEntry.first;
            PendingSummon const& pending = pendingEntry.second;
            ObjectGuid const botGuid(botRawGuid);
            auto erasePending = [&]()
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingSummonsByBotGuid.erase(botRawGuid);
            };

            Player* summoner = ObjectAccessor::FindConnectedPlayer(pending.summonerGuid);
            if (!summoner || !summoner->IsInWorld() || summoner->InBattleground())
            {
                // A master inside a battleground can't be joined; drop the
                // request rather than retrying against it forever.
                erasePending();
                continue;
            }

            Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
            if (!bot)
            {
                // Still materializing through the asynchronous login holder.
                if (nowMs > pending.deadlineMs)
                    erasePending();
                continue;
            }

            if (!bot->IsInWorld() || bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
                continue;

            if (bot->InBattleground())
            {
                erasePending();
                continue;
            }

            if (!bot->IsAlive())
            {
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();
            }

            bool const sameMap = bot->GetMapId() == summoner->GetMapId();
            if (!sameMap || bot->GetDistance(summoner) > 40.0f)
            {
                if (nowMs > pending.deadlineMs)
                {
                    erasePending();
                    continue;
                }

                if (!BotCanTeleportNow(bot))
                    break;

                if (bot->TeleportTo(summoner->GetMapId(),
                    summoner->GetPositionX() + frand(-2.5f, 2.5f),
                    summoner->GetPositionY() + frand(-2.5f, 2.5f),
                    summoner->GetPositionZ(), summoner->GetOrientation()))
                    RestorePlayerbotTeleportVitals(bot);
                continue;
            }

            // Master status is derived from group membership everywhere else
            // (UpdateMasterFromGroup); never set it for a bot that could not
            // join the group, or the two disagree forever.
            if (pending.joinGroup && !TryJoinSummonerGroup(summoner, bot))
            {
                erasePending();
                continue;
            }

            PveBotState& state = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
            state.masterGuid = summoner->GetGUID();
            state.passive = false;
            state.stay = false;
            // Serving a master supersedes any solo trek in progress.
            state.journeyActive = false;
            state.journeyFallbackKind = 0;
            bot->Whisper("At your side.", LANG_UNIVERSAL, summoner);
            erasePending();
        }
    }

    void ProcessPendingGroupInviteAccepts()
    {
        std::unordered_set<uint64> drained;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            drained.swap(g_PendingGroupInviteAccepts);
        }

        for (uint64 botRawGuid : drained)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
            if (!bot || !bot->GetSession() || !bot->GetGroupInvite())
                continue;

            WorldPacket acceptPacket(CMSG_GROUP_ACCEPT, 4);
            acceptPacket << uint32(0);
            bot->GetSession()->HandleGroupAcceptOpcode(acceptPacket);
        }
    }

    void UpdateMasterFromGroup(Player* bot, PveBotState& state)
    {
        Group* group = bot->GetGroup();
        if (!group)
        {
            state.masterGuid = ObjectGuid::Empty;
            return;
        }

        ObjectGuid const leaderGuid = group->GetLeaderGUID();
        ObjectGuid firstHumanGuid;
        bool leaderIsHuman = false;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            Player const* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
            if (!IsHumanPlayer(member))
                continue;

            if (slot.guid == leaderGuid)
            {
                leaderIsHuman = true;
                break;
            }

            if (firstHumanGuid.IsEmpty())
                firstHumanGuid = slot.guid;
        }

        state.masterGuid = leaderIsHuman ? leaderGuid : firstHumanGuid;
        // Gaining a master supersedes any solo trek in progress.
        if (!state.masterGuid.IsEmpty() && state.journeyActive)
        {
            state.journeyActive = false;
            state.journeyFallbackKind = 0;
        }
    }

    // Killed by a person: stop looking for the next fight for a while, and move
    // somewhere quieter afterwards. Brave bots shrug a defeat off in half the
    // time; the meek stay away for twice as long.
    void MarkTimidAfterPlayerDefeat(Player* bot, PveBotState& state,
        playerbot::PveConfig const& cfg, PveTimePoint now)
    {
        if (!cfg.timidMinutes)
            return;

        // No retreat in a zone that cannot host a player fight. Nothing there
        // could have killed the bot in the first place, so a flee would only ever
        // be a spurious teleport.
        if (!BarracksHardcore::IsOpenWorldPvpZone(bot->GetZoneId()))
            return;

        // Was a person actually involved? lastPlayerFightAt is only stamped while
        // the bot is swinging at a human, so a recent stamp means this death ended
        // a fight with one. Proximity alone would not do: a bystander watching the
        // bot lose to a mob would read as a defeat at their hands.
        if (state.lastHumanSwingAt == PveTimePoint() ||
            now - state.lastHumanSwingAt > std::chrono::seconds(60))
            return;

        uint8 const aggression = GetBotAggression(bot);
        float const minutes = float(cfg.timidMinutes) * (2.0f - 1.5f * (float(aggression) / 100.0f));

        state.timidUntil = now + std::chrono::seconds(int64(minutes * 60.0f));
        state.fleeingFromPlayers = true;

        // Restart the hunting clock as well. Without this a bot that was already
        // overdue for a hunt when it died would go straight back out the moment
        // timidity lapsed, which is the opposite of learning its lesson.
        state.lastPlayerFightAt = now;

        TC_LOG_INFO("playerbots.pve",
            "Bot {} (aggression {}) lost a fight to a person; timid for {}m and moving off.",
            bot->GetName(), aggression, uint32(minutes));
    }

    void RunDeathRecovery(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        if (bot->IsAlive())
        {
            state.deathObserved = false;
            return;
        }

        if (state.engaged)
            DisengagePveCombat(bot, state);

        if (bot->IsResurrectRequested())
        {
            bot->ResurrectUsingRequestData();
            state.deathObserved = false;
            return;
        }

        PveTimePoint const now = PveClock::now();
        if (!state.deathObserved)
        {
            state.deathObserved = true;
            state.deathObservedAt = now;
            MarkTimidAfterPlayerDefeat(bot, state, cfg, now);
            // The body still lies where we fell: remember the spot, the hardcore
            // drop chest stands on it (release teleports us to the graveyard).
            if (cfg.hardcoreLootChestEntry)
            {
                state.deathSpotMapId = uint16(bot->GetMapId());
                state.deathSpotX = bot->GetPositionX();
                state.deathSpotY = bot->GetPositionY();
                state.deathSpotZ = bot->GetPositionZ();
                state.deathSpotAt = now;
            }
            // Dying voids any trek in progress: resuming the same walk would
            // march straight back through whatever killed us.
            state.journeyActive = false;
            state.journeyFallbackKind = 0;
            if (state.recentDeathWindowStart == PveTimePoint() ||
                now - state.recentDeathWindowStart > std::chrono::minutes(5))
            {
                state.recentDeathWindowStart = now;
                state.recentDeathCount = 0;
            }
            ++state.recentDeathCount;
            return;
        }

        if (now - state.deathObservedAt < std::chrono::seconds(cfg.autoReviveSeconds))
            return;

        if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        {
            bot->BuildPlayerRepop();
            bot->RepopAtGraveyard();
            return;
        }

        // RepopAtGraveyard above already moved the ghost to the spirit healer, so
        // this stands the bot up there - just without the sickness a person would
        // take for the same choice.
        bot->ResurrectPlayer(0.66f);
        bot->SpawnCorpseBones();
        ClearResurrectionSickness(bot);
        state.deathObserved = false;

        // A dead bot returns from RunSlowTick before the normal band-maintenance
        // pass, so an already-stranded below-band bot could resurrect in the same
        // impossible zone and spend another maintenance interval there. Queue the
        // band correction immediately on standing up. The world-thread drain below
        // recomputes the same deterministic home and performs the actual reset.
        if (g_PveConfig.rebirthZoneBanded && !GetGuardianZoneId(bot->GetGUID().GetRawValue()))
        {
            uint32 const homeZoneId = GetRebirthZoneId(bot);
            if (ClassicZoneBand const* homeBand = homeZoneId ? FindClassicZoneBand(homeZoneId) : nullptr)
            {
                uint32 const level = bot->GetLevel();
                if (level < uint32(homeBand->minLevel) || level >= uint32(homeBand->maxLevel))
                {
                    std::lock_guard<std::mutex> guard(g_PvePendingLock);
                    g_PendingRebirths.insert(bot->GetGUID().GetRawValue());
                    TC_LOG_INFO("playerbots.pve",
                        "Bot {} resurrected outside home zone {} band {}-{} at level {}; queueing immediate band reset.",
                        bot->GetName(), homeZoneId, uint32(homeBand->minLevel), uint32(homeBand->maxLevel), level);
                }
            }
        }

        // Queued here rather than at the moment of death because the relocation
        // pass skips corpses.
        if (state.fleeingFromPlayers)
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingGrindRelocations.insert(bot->GetGUID().GetRawValue());
        }

        // Hardcore: the drop chest stands where we fell - walk back and reclaim
        // it (the errand scan loots death chests inside 200y). First death only:
        // on a repeat the loop breaker below relocates away instead, abandoning
        // the chest the way a player abandons a camped corpse.
        // Gated on timidity rather than the flee flag: that flag is a request for
        // one relocation and is dropped unprocessed whenever the bot is in a
        // battleground, mid-teleport or has no candidate spot - and if relocation
        // is disabled realm-wide it is never cleared at all, which would disable
        // the corpse run permanently on the first such death. timidUntil expires
        // by itself.
        if (cfg.hardcoreLootChestEntry && state.recentDeathCount < 2 &&
            PveClock::now() >= state.timidUntil && state.masterGuid.IsEmpty() &&
            state.deathSpotMapId == bot->GetMapId() && state.deathSpotAt != PveTimePoint())
        {
            // Only while the chest could still be standing there: it despawns on
            // the hardcore timer, and the walk home has to be worth making. The
            // corpse run itself takes time, so the remaining window must cover it
            // at roughly running pace.
            auto const chestAge = std::chrono::duration_cast<std::chrono::seconds>(now - state.deathSpotAt).count();
            float const distance = bot->GetDistance(state.deathSpotX, state.deathSpotY, state.deathSpotZ);
            int64 const travelSeconds = int64(distance / 7.0f) + 15;

            if (chestAge + travelSeconds < int64(cfg.hardcoreChestDespawnSeconds) &&
                distance > 20.0f && cfg.travelWalkMaxDistance > 0.0f && distance < cfg.travelWalkMaxDistance)
            {
                TC_LOG_INFO("playerbots.pve", "Bot {} walks {} yards back to reclaim its death chest ({}s old).",
                    bot->GetName(), uint32(distance), uint32(chestAge));
                StartWalkedJourney(state, state.deathSpotMapId, state.deathSpotX, state.deathSpotY, state.deathSpotZ, 0, distance);
            }
            else if (distance <= 20.0f)
            {
                // Already standing on it, so there is no walk to make - and the
                // generic errand scan will not rescue this. That scan runs at most
                // once every fifteen seconds and only while the bot is unengaged
                // and out of combat, whereas target selection runs on the 250ms
                // fast tick. A resurrected bot therefore picks a fight long before
                // the scan ever fires, and once it is in combat the gate never
                // opens again: it runs off and fights over the top of its own
                // gear. Claim the chest here, in the same tick it stood up, so
                // nothing else can take the decision first.
                //
                // The errand holds for ninety seconds, so even if something
                // aggroes the bot on the way, it returns to the chest afterwards
                // rather than forgetting it.
                if (GameObject* chest = FindRegisteredDeathChest(bot, cfg.hardcoreLootChestEntry, 30.0f))
                {
                    if (chest->isSpawned() && chest->getLootState() == GO_READY)
                    {
                        state.errandGuid = chest->GetGUID();
                        state.errandKind = PveErrandKind::QuestObject;
                        state.errandUntil = now + std::chrono::seconds(90);
                        state.deathSpotAt = PveTimePoint(); // claimed
                        TC_LOG_INFO("playerbots.pve", "Bot {} claims the death chest it woke up on ({:.0f}y).",
                            bot->GetName(), bot->GetDistance(chest));
                    }
                }
            }

            if (chestAge + travelSeconds >= int64(cfg.hardcoreChestDespawnSeconds))
                state.deathSpotAt = PveTimePoint(); // gone; stop trying
        }

        // A bot that dies repeatedly used to be teleported to another cluster. It
        // no longer is: dying in the same place twice is what a level-appropriate
        // zone feels like from the inside, and hauling the bot out of its zone
        // contradicts being tied to one. It gets up at the graveyard and walks
        // back, the same as a person would. The death counter stays - the hardcore
        // corpse run still reads it.

        // A revived companion whose master moved on catches up by teleport; the
        // world-update pass owns the actual move.
        if (!state.masterGuid.IsEmpty() && !HasPendingSummon(bot->GetGUID()))
        {
            Player const* masterSameMap = ObjectAccessor::GetPlayer(*bot, state.masterGuid);
            if (!masterSameMap || bot->GetDistance(masterSameMap) > PveCompanionTeleportCatchupDistance)
                QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
        }
    }

    // Claim a death chest lying near the bot - its own, another bot's, or a
    // player's; ownership is never consulted.
    //
    // This has to run on the FAST tick, ahead of target selection. It lived on the
    // slow tick with a three second cadence, and lost every race: a bot picks its
    // next target on the 250ms tick, so by the time the chest check came round the
    // bot was already engaged, and the check requires being out of combat. A player
    // dying in front of a bot watched it turn around and pull another mob.
    //
    // Affordable at this rate only because chests are looked up in a registry now
    // rather than through a grid search - it is a pass over a handful of records.
    bool TryClaimNearbyDeathChest(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        // Combat is judged by the CORE's flag, not the bot's own engaged bookkeeping.
        // state.engaged is set by the decision layer and can outlive the fight that
        // set it - a GM .die on an engaged bot, a victim despawning, a target going
        // invalid - and a stale true there would lock a bot out of every chest it
        // ever walks past, permanently and silently. IsInCombat cannot go stale that
        // way. If engaged is set but the bot is not actually in combat, taking the
        // chest is the better answer anyway.
        if (!cfg.hardcoreLootChestEntry || !bot->IsAlive() || bot->IsInCombat())
            return false;

        if (!state.masterGuid.IsEmpty() || !state.pendingLootGuid.IsEmpty())
            return false;

        // Do not disturb a chest errand already in flight; anything else may be
        // preempted, because errands are resumable and dropped gear is not.
        if (state.errandKind == PveErrandKind::QuestObject)
            return false;

        PveTimePoint const now = PveClock::now();
        if (now < state.nextChestScanAt)
            return false;

        state.nextChestScanAt = now + std::chrono::seconds(1);

        GameObject* chest = FindRegisteredDeathChest(bot, cfg.hardcoreLootChestEntry, 200.0f);
        if (!chest || !chest->isSpawned() || chest->getLootState() != GO_READY ||
            IsRecentErrandTarget(state, chest->GetGUID()))
            return false;

        state.errandGuid = chest->GetGUID();
        state.errandKind = PveErrandKind::QuestObject;
        state.errandUntil = now + std::chrono::seconds(180);
        TC_LOG_INFO("playerbots.pve", "Bot {} breaks off for a death chest {:.0f}y away.",
            bot->GetName(), bot->GetDistance(chest));
        return true;
    }

    // Asking only on level-up is not enough. A bot that is ALREADY past its zone's
    // ceiling - which is most of the fleet the moment banding is switched on, or
    // any time a band is retuned - never gains another level in time to be noticed,
    // and the higher its level the longer that takes. Ask again on a slow cadence
    // so the standing backlog drains instead of waiting for a ding that may be
    // hours away.
    // A bot never carries resurrection sickness. Nothing in the bot path asks for
    // it - ResurrectPlayer is called without the flag - but a race can carry its
    // own sickness spell through ChrRaces, and an aura saved on a character before
    // this rule existed is restored on login and would otherwise sit there for its
    // full ten minutes. Strip it wherever it came from.
    void ClearResurrectionSickness(Player* bot)
    {
        constexpr uint32 kSharedResurrectionSickness = 15007;

        if (ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(bot->GetRace()))
            if (raceEntry->ResSicknessSpellID)
                bot->RemoveAurasDueToSpell(raceEntry->ResSicknessSpellID);

        bot->RemoveAurasDueToSpell(kSharedResurrectionSickness);
    }

    void MaybeQueueOverBandRebirth(Player* bot, PveBotState& state)
    {
        if (!g_PveConfig.rebirthZoneBanded)
            return;

        PveTimePoint const now = PveClock::now();
        if (now < state.nextRebirthCheckAt)
            return;
        state.nextRebirthCheckAt = now + std::chrono::seconds(60);

        // Same decision as the level-up hook, reached the same way, so the two can
        // never disagree about who is due.
        playerbot::PveManager::OnManagedBotLevelChanged(bot, bot->GetLevel());
    }

    void ResetStuckWatchdog(PveBotState& state)
    {
        state.stuckAnchorAt = {};
        state.stuckAnchorMapId = 0;
        state.stuckAnchorZoneId = 0;
        state.stuckAnchorX = 0.0f;
        state.stuckAnchorY = 0.0f;
    }

    bool IsStuckWatchdogEligible(Player* bot, PveBotState const& state,
        playerbot::PveConfig const& cfg, PveTimePoint now)
    {
        if (!cfg.stuckRecoveryEnabled || !cfg.grindEnabled || !bot || !bot->IsAlive() ||
            playerbot::PveManager::IsPvpOnlyBot(bot) || bot->InBattleground() || bot->duel ||
            bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            return false;

        Map const* map = bot->GetMap();
        if (!map || map->IsDungeon() || map->IsBattlegroundOrArena() ||
            !std::binary_search(cfg.relocateMaps.begin(), cfg.relocateMaps.end(), bot->GetMapId()))
            return false;

        // Only autonomous open-world bots. Companions have their own master
        // catch-up teleport, while a stay order is intentional immobility.
        if (!state.masterGuid.IsEmpty() || state.stay || state.engaged || bot->IsInCombat() ||
            IsRestingNow(bot, state))
            return false;

        // Do not count scripted/legitimate holds toward the timeout. Swimming is
        // intentionally NOT excluded: a bot wedged in a river is one of the cases
        // this recovery exists to catch.
        if (bot->HasUnitState(UNIT_STATE_CONTROLLED) || bot->HasUnitState(UNIT_STATE_CASTING) ||
            bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) || bot->GetTransport() ||
            bot->GetVehicleBase() || bot->IsInFlight() || now < state.tamingUntil ||
            now < state.feignHoldUntil)
            return false;

        return true;
    }

    void RunStuckWatchdog(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        PveTimePoint const now = PveClock::now();
        if (!IsStuckWatchdogEligible(bot, state, cfg, now))
        {
            ResetStuckWatchdog(state);
            return;
        }

        uint32 const zoneId = bot->GetZoneId();
        uint16 const mapId = uint16(bot->GetMapId());
        auto setAnchor = [&]()
        {
            state.stuckAnchorAt = now;
            state.stuckAnchorMapId = mapId;
            state.stuckAnchorZoneId = zoneId;
            state.stuckAnchorX = bot->GetPositionX();
            state.stuckAnchorY = bot->GetPositionY();
        };

        if (!zoneId)
        {
            ResetStuckWatchdog(state);
            return;
        }

        if (state.stuckAnchorAt == PveTimePoint{} || state.stuckAnchorMapId != mapId ||
            state.stuckAnchorZoneId != zoneId)
        {
            setAnchor();
            return;
        }

        float const dx = bot->GetPositionX() - state.stuckAnchorX;
        float const dy = bot->GetPositionY() - state.stuckAnchorY;
        float const requiredDistanceSq = cfg.stuckRecoveryDistanceYards * cfg.stuckRecoveryDistanceYards;
        if (dx * dx + dy * dy >= requiredDistanceSq)
        {
            setAnchor();
            return;
        }

        if (now - state.stuckAnchorAt < std::chrono::seconds(cfg.stuckRecoverySeconds))
            return;

        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingStuckRelocations[bot->GetGUID().GetRawValue()] = zoneId;
        }
        TC_LOG_INFO("playerbots.pve",
            "Bot {} stayed within {:.0f}y for {}s in zone {}; queueing same-zone stuck recovery.",
            bot->GetName(), cfg.stuckRecoveryDistanceYards, cfg.stuckRecoverySeconds, zoneId);

        // A failed landing search may retry, but only after another complete
        // observation window; never hammer the world-thread relocator each tick.
        setAnchor();
    }

    void RunSlowTick(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        RunDeathRecovery(bot, state, cfg);
        if (!bot->IsAlive())
        {
            ResetStuckWatchdog(state);
            return;
        }

        // Defeat attribution has to be stamped from somewhere that runs WHILE the
        // bot is fighting. MaybeHuntPlayersByAggression looks like the natural
        // home and is not: it returns on IsInCombat/engaged long before its own
        // victim check, so that stamp can never fire during the fight it is meant
        // to record. This tick has no such gate.
        if (Unit* victim = bot->GetVictim())
            if (Player const* victimPlayer = victim->ToPlayer())
                if (!playerbot::IsManagedRandomBot(victimPlayer))
                {
                    state.lastHumanSwingAt = PveClock::now();
                    // The hunt clock wants this too, and for the same reason was
                    // never actually being reset by fighting a person.
                    state.lastPlayerFightAt = state.lastHumanSwingAt;
                }

        EnsureFirstLoginKit(bot, state, cfg);

        // Guardians never leave their post for autonomous town/class/grind travel.
        // If a bot was promoted to guardian while an older journey/request was still
        // live, cancel that ownership here and let RunZoneGuardianTick send it home.
        if (GetGuardianZoneId(bot->GetGUID().GetRawValue()))
        {
            if (state.journeyActive && state.journeyFallbackKind != 0)
            {
                state.journeyActive = false;
                state.journeyFallbackKind = 0;
            }
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingSupplyRuns.erase(bot->GetGUID().GetRawValue());
            g_PendingClassQuestTravels.erase(bot->GetGUID().GetRawValue());
        }

        // Broken equipped gear is an emergency maintenance condition, not a
        // normal vendor errand. Run this before rest/quest/vendor scheduling so a
        // zero-durability weapon cannot lose the race to the 250ms combat picker.
        if (!bot->IsInCombat() && HasBrokenEquippedItem(bot))
            RepairBrokenEquippedItems(bot);

        if (PveClock::now() >= state.nextWeaponSkillCheckAt)
        {
            state.nextWeaponSkillCheckAt = PveClock::now() + std::chrono::seconds(15);
            MaxOutWeaponSkills(bot);
            MaxOutGatheringSkills(bot);
            ClearResurrectionSickness(bot);

            if (!cfg.guildName.empty() && !bot->GetGuildId())
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingGuildJoins.insert(bot->GetGUID().GetRawValue());
            }
            MaybeQueueOverBandRebirth(bot, state);
            DiscardScaffoldingItems(bot);
            DiscardOrphanedQuestItems(bot);
            DiscardWorthlessClutter(bot);
            DiscardHoardedDuplicates(bot);

            // A bot reborn at level 1 has its spellbook stripped and never got
            // the class's STARTING spells back. The trainer catch-up only teaches
            // rank 2 and up, so rank 1 - the spell a character is created with -
            // was missing for the rest of that bot's life. A level 2 warlock with
            // no Shadow Bolt simply walks up and swings its staff, which is
            // exactly what was seen. Both calls skip anything already known.
            if (!state.startingSpellsEnsured)
            {
                state.startingSpellsEnsured = true;
                bot->LearnDefaultSkills();
                bot->LearnCustomSpells();
                EnsureBaselineAttackSpell(bot);
                EnsureHunterAutoShot(bot);
            }

            EnsureRoguePoisons(bot);
            ApplyRoguePoisons(bot);
            EnsureRangedAmmo(bot);
        }

        if (bot->GetGroupInvite())
        {
            if (cfg.declineGroupInvites)
            {
                // Hardcore realms: bots are strictly solo - decline politely.
                if (WorldSession* session = bot->GetSession())
                {
                    WorldPacket declinePacket(CMSG_GROUP_DECLINE, 0);
                    session->HandleGroupDeclineOpcode(declinePacket);
                }
            }
            else
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingGroupInviteAccepts.insert(bot->GetGUID().GetRawValue());
            }
        }

        UpdateMasterFromGroup(bot, state);

        // Cross-map or extreme-distance master catch-up.
        if (!state.masterGuid.IsEmpty() && !state.engaged && !HasPendingSummon(bot->GetGUID()))
        {
            Player const* masterSameMap = ObjectAccessor::GetPlayer(*bot, state.masterGuid);
            if (!masterSameMap)
            {
                // No catch-up into battlegrounds: the summon executor refuses
                // those anyway, so queueing would just retry forever.
                Player const* master = ObjectAccessor::FindConnectedPlayer(state.masterGuid);
                if (master && !master->InBattleground())
                    QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
            }
            else if (bot->GetDistance(masterSameMap) > PveCompanionTeleportCatchupDistance)
                QueuePendingSummon(bot->GetGUID(), state.masterGuid, false);
        }

        // Buff upkeep before the rest check: buffs only fire above 60% mana,
        // rest starts under 50%, so the two never chase each other.
        if (cfg.buffsEnabled && !state.engaged && !IsRestingNow(bot, state))
            playerbot::PvpCore::TryCastOpenWorldBuff(bot);

        // Rest: sit down and use the free food/water once the fight is over.
        // Companions only sit while their master is close (well inside the 40y
        // rest-break distance), so start/break can't oscillate behind a moving
        // master.
        bool masterAllowsRest = true;
        if (!state.masterGuid.IsEmpty())
        {
            Player const* masterSameMap = ObjectAccessor::GetPlayer(*bot, state.masterGuid);
            masterAllowsRest = masterSameMap && bot->GetDistance(masterSameMap) < PveRestBreakFollowDistance * 0.75f;
        }

        if (!state.engaged && !bot->IsInCombat() && !IsRestingNow(bot, state) && masterAllowsRest &&
            (!state.journeyActive || NeedsRecovery(bot, cfg)))
        {
            // Where people can fight, top off to the ENGAGE thresholds rather than
            // the rest ones. Otherwise a bot parks in the dead band between them:
            // too healthy to sit down, too hurt to be allowed to open on anybody,
            // and it just stands there waiting on natural regeneration.
            bool const topOffForPvp = state.masterGuid.IsEmpty() &&
                BarracksHardcore::IsOpenWorldPvpZone(bot->GetZoneId());
            float const healthTarget = topOffForPvp
                ? std::max(cfg.restHealthPct, cfg.playerEngageMinHealthPct) : cfg.restHealthPct;
            float const manaTarget = topOffForPvp
                ? std::max(cfg.restManaPct, cfg.playerEngageMinManaPct) : cfg.restManaPct;
            bool const needFood = bot->GetHealthPct() < healthTarget;
            bool const needDrink = bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < manaTarget;
            if (needFood || needDrink)
            {
                if (cfg.restUseConsumables)
                {
                    // Economy realm: eat/drink the real thing from the bags; a
                    // bot with nothing edible restocks through the vendor errand.
                    Item* consumable = FindBestConsumable(bot, !needFood);
                    if (!consumable && needDrink)
                        consumable = FindBestConsumable(bot, true);

                    // Nothing to consume, and a guardian has no way to fix that.
                    // The errand scan only sees NPCs within a 200 yard grid search,
                    // which from a wilderness grind spot never contains a merchant,
                    // and the town-run fallback that exists for exactly this case is
                    // denied to guardians - StartErrandIfNeeded gates it on
                    // '&& !guardian' so a guardian cannot leave its post to shop.
                    // Between the two it is structurally unable to restock: every
                    // guardian on the realm was carrying zero food and zero water.
                    //
                    // So deliver instead. Only guardians, and only once actually
                    // dry - every other bot can walk to a vendor and pay, and
                    // handing the whole fleet free rations would delete that.
                    if (!consumable && !ConjureSpellId(bot, needDrink) &&
                        GetGuardianZoneId(bot->GetGUID().GetRawValue()))
                    {
                        std::lock_guard<std::mutex> rationGuard(g_PvePendingLock);
                        g_PendingGuardianRations.insert(bot->GetGUID().GetRawValue());
                    }
                    if (!consumable && !bot->HasUnitState(UNIT_STATE_CASTING))
                    {
                        // A mage conjures its own pantry; the next slow tick
                        // finds the conjured stack and eats it.
                        uint32 conjureId = needFood ? ConjureSpellId(bot, false) : 0;
                        if (!conjureId && needDrink)
                            conjureId = ConjureSpellId(bot, true);
                        if (conjureId)
                        {
                            playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                            if (MotionMaster* motionMaster = bot->GetMotionMaster())
                                motionMaster->Clear();
                            bot->StopMoving();
                            bot->CastSpell(bot, conjureId, false);
                        }
                    }
                    if (consumable)
                    {
                        // Nobody eats or drinks as a bear. A shapeshifted druid
                        // (or a shaman in ghost wolf) has to drop the form first,
                        // or the item use is refused and it rests forever without
                        // ever recovering a point of mana.
                        if (bot->GetShapeshiftForm() != FORM_NONE)
                            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

                        // The cast may consume the last unit and delete the item;
                        // capture everything needed before it runs.
                        uint32 const consumableEntry = consumable->GetEntry();
                        uint32 usedSpellId = 0;
                        if (ItemTemplate const* proto = consumable->GetTemplate())
                            for (uint8 spellIdx = 0; spellIdx < MAX_ITEM_PROTO_SPELLS; ++spellIdx)
                                if (proto->Spells[spellIdx].SpellId > 0 &&
                                    proto->Spells[spellIdx].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                                {
                                    usedSpellId = uint32(proto->Spells[spellIdx].SpellId);
                                    break;
                                }

                        playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                        if (MotionMaster* motionMaster = bot->GetMotionMaster())
                            motionMaster->Clear();
                        bot->StopMoving();
                        SpellCastTargets targets;
                        targets.SetUnitTarget(bot);
                        bot->CastItemUseSpell(consumable, targets, 0, 0);

                        // CastItemUseSpell reports nothing: only the landed
                        // food/drink aura counts as eating. Without this check a
                        // rejected cast faked a 22s "rest" that healed nothing,
                        // forever. Sitting is the client's job for real players,
                        // so do it here too.
                        if (usedSpellId && bot->HasAura(usedSpellId))
                        {
                            bot->SetStandState(UNIT_STAND_STATE_SIT);
                            state.restingUntil = PveClock::now() + std::chrono::seconds(22);
                        }
                        else if (cfg.combatDiagnostics)
                            TC_LOG_INFO("playerbots.pve", "Bot {} could not eat/drink item {} (use spell {} did not land).",
                                bot->GetName(), consumableEntry, usedSpellId);
                    }
                }
                else
                {
                    playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                    if (MotionMaster* motionMaster = bot->GetMotionMaster())
                        motionMaster->Clear();
                    bot->StopMoving();
                    bot->CastSpell(bot, needFood ? SPELL_PVE_OUT_OF_COMBAT_EAT : SPELL_PVE_OUT_OF_COMBAT_DRINK, true);
                }
            }
        }

        RunStuckWatchdog(bot, state, cfg);

        PveTimePoint const now = PveClock::now();

        // Class quests are provisioning, not travel content. Do this before the
        // generic errand scan so an old/incomplete class quest can never steer the
        // bot toward its giver or objective. The repeated pass also repairs reward
        // teaching spells after custom/manual spell resets.
        if (now >= state.nextClassQuestScanAt)
        {
            state.nextClassQuestScanAt = now + std::chrono::seconds(30);
            CompleteEligibleClassQuests(bot);
            state.classQuestId = 0;
            state.classQuestMapId = 0;
            state.classQuestX = state.classQuestY = state.classQuestZ = 0.0f;
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingClassQuestTravels.erase(bot->GetGUID().GetRawValue());
        }

        // Free gear outranks the next pull. A bot that is not fighting picks up a
        // chest lying near it - its own, another bot's, or a player's; ownership is
        // not consulted - instead of walking past it to look for something to hit.
        //
        // This deliberately does NOT wait for the errand scan below. That scan is
        // shared with quests, vendors and repairs and fires at most once every
        // fifteen seconds, while target selection runs on the 250ms fast tick, so a
        // bot that keeps finding fights never reaches it at all. That is how bots
        // stood next to chests indefinitely.
        //
        // Kept to a short radius on its own three second cadence so the extra grid
        // search stays cheap - roughly 86 searches a second across the fleet, at a
        // ninth of the area of the errand scan's 200 yard sweep. Finding chests
        // further out remains that scan's job; this one only has to notice what is
        // already underfoot.
        // Quest/vendor errands are for autonomous bots; companions stay on their
        // master's heel.
        if (state.masterGuid.IsEmpty() && !state.engaged && !bot->IsInCombat() &&
            state.errandKind == PveErrandKind::None && state.pendingLootGuid.IsEmpty() &&
            !state.journeyActive && now >= state.nextErrandScanAt)
        {
            state.nextErrandScanAt = now + std::chrono::seconds(15);
            StartErrandIfNeeded(bot, state, cfg);
            MaybeFieldRepair(bot, state, cfg);
        }

        // Zone guardians: claim or hold a post. This runs even with the feature
        // switched off and for companions, because it is also the only code that
        // sheds the frozen-XP flag - gating it would leave every former guardian
        // unable to gain experience, permanently, with the flag persisted to the
        // characters table.
        RunZoneGuardianTick(bot, state, cfg);
        MaybeHuntPlayersByAggression(bot, state, cfg);

        // Class-quest travel is intentionally disabled. Eligible class quests
        // were auto-rewarded above before the generic errand scan.

        // Taming itself is driven from the fast tick, where it can outrank the
        // grind loop; this only keeps whatever pet the hunter already has fed.
        if (!state.engaged && !bot->IsInCombat() && now >= state.nextTameScanAt)
        {
            state.nextTameScanAt = now + std::chrono::seconds(30);
            MaybeFeedPet(bot);
        }

        // Wrong zone for this level? Move, and do not wait to be asked.
        //
        // The dry-wander counter was the only route to a relocation, and it only
        // trips when a bot finds NOTHING to fight. A low level zone is not empty -
        // it is full of creatures that are all simply the wrong level - so a bot
        // that outgrew its zone could sit there indefinitely, scanning, finding
        // nothing it is allowed to attack, and never quite going dry enough to be
        // moved. That is how a level 13 stayed in Durotar.
        //
        // Guardians are pinned to their post by design, and companions follow a
        // human, so neither is eligible. A bot mid-journey or mid-errand is left
        // alone until it arrives.
        //
        // Nor is a bot that was sent somewhere on purpose. A hunter dispatched at
        // a bounty is standing in a zone that does not fit it BY DESIGN - that is
        // what being called out of your own band means - and this sweep would
        // read that as an outgrown zone and walk it straight back home, inside a
        // minute, possibly mid-fight. It used to be invisible because the levy
        // re-levelled the hunter to the zone's band first, which made the zone
        // fit; the moment a veteran keeps its own level, this sweep is what
        // undoes the dispatch. The pursuit deadline covers the trip in, and
        // combat covers the fight itself. Afterwards it is free to walk home
        // again - which is now how a hunter gets home at all, at its own level
        // and without a rebirth.
        if (cfg.relocateEnabled && cfg.grindEnabled && state.masterGuid.IsEmpty() &&
            !state.journeyActive && state.errandKind == PveErrandKind::None &&
            !GetGuardianZoneId(bot->GetGUID().GetRawValue()))
        {
            if (state.bountyDeployed || bot->IsInCombat())
            {
                // HELD, not merely skipped. Pinning the next check to now means
                // the sweep runs on the very first tick after the hunt ends
                // instead of up to a minute later: once the bounty is cleared
                // there is no reason at all for a level sixty to be standing in a
                // starter zone, and waiting out a cadence to notice is the same as
                // not noticing.
                //
                // Held on the RECEIPT rather than on the pursuit deadline. That
                // deadline is a fixed ninety seconds from dispatch which nothing
                // refreshes - the hunt loop cannot even reach a bot already stood
                // next to its target - so a hunt that outlives it would have this
                // sweep and the return pass disagreeing about whether the bot is
                // still working, and the loser is a level sixty put on a flight
                // path out of the zone mid-fight. The receipt is the return
                // pass's own state, so the two cannot disagree.
                state.nextZoneFitCheckAt = now;
            }
            else if (now >= state.nextZoneFitCheckAt)
            {
                state.nextZoneFitCheckAt = now + std::chrono::seconds(60);

                if (!BotIsInSuitableZone(bot))
                {
                    TC_LOG_INFO("playerbots.pve", "Bot {} (level {}) has outgrown zone {}; relocating.",
                        bot->GetName(), bot->GetLevel(), bot->GetZoneId());
                    state.dryWanderCount = 0;
                    std::lock_guard<std::mutex> guard(g_PvePendingLock);
                    g_PendingGrindRelocations.insert(bot->GetGUID().GetRawValue());
                }
            }
        }

        // Growl is checked on its own cadence and NOT gated on being out of
        // combat: a pet that just got tamed goes straight into a fight, and it
        // is that first fight that most needs the threat.
        if (now >= state.nextPetGrowlCheckAt)
        {
            state.nextPetGrowlCheckAt = now + std::chrono::seconds(20);
            EnsurePetKnowsGrowl(bot);
            KeepPetHappy(bot);
        }

        // Not on the twenty second cadence: a hurt pet is worth topping up as
        // soon as the fight that hurt it ends. Every check in it is cheap and it
        // returns immediately for anything that is not a wounded hunter pet.
        MaybeMendPet(bot);

        // Energy on the floor mid-fight is exactly when the tea is worth
        // drinking, so this is asked in combat rather than beside the campfire.
        MaybeDrinkThistleTea(bot, state, now);

        // Naked recovery: shop the auction house with the whole purse, and when
        // that has had its chance, issue green field kit. Ordered this way so a
        // bot that can afford real gear buys it rather than taking handouts.
        if (!bot->IsInCombat() && state.masterGuid.IsEmpty() && now >= state.nextNakedCheckAt)
        {
            state.nextNakedCheckAt = now + std::chrono::seconds(30);
            if (IsBotStrippedBare(bot))
            {
                if (state.nakedSince == PveTimePoint())
                {
                    state.nakedSince = now;
                    if (cfg.auctionBuyEnabled)
                    {
                        std::lock_guard<std::mutex> guard(g_PvePendingLock);
                        g_PendingAuctionShopping.insert(bot->GetGUID().GetRawValue());
                    }
                }
                // The white field kit itself is the hardcore script's business
                // (it dresses players and bots alike on resurrection); here the
                // auction house is simply given a fresh chance each pass.
                else if (now - state.nakedSince >= std::chrono::seconds(90) && cfg.auctionBuyEnabled)
                {
                    state.nakedSince = now;
                    std::lock_guard<std::mutex> guard(g_PvePendingLock);
                    g_PendingAuctionShopping.insert(bot->GetGUID().GetRawValue());
                }
            }
            else
                state.nakedSince = PveTimePoint();
        }

        if (cfg.equipUpgradesEnabled && !bot->IsInCombat() && now >= state.nextEquipCheckAt)
        {
            state.nextEquipCheckAt = now + std::chrono::seconds(15);
            TryEquipUpgrades(bot);
            // Companions never run vendor errands (they stay on their master's
            // heel), so critical durability gets the field repair here.
            if (!state.masterGuid.IsEmpty())
                MaybeFieldRepair(bot, state, cfg);
        }

        if (cfg.talentsEnabled && !bot->IsInCombat() && now >= state.nextTalentCheckAt)
        {
            state.nextTalentCheckAt = now + std::chrono::seconds(60);
            SpendPendingTalentPoints(bot);
        }

        if (cfg.professionsEnabled && !bot->IsInCombat() && now >= state.nextProfessionCheckAt)
        {
            state.nextProfessionCheckAt = now + std::chrono::seconds(60);
            EnsureProfessions(bot);
        }

        // Mail and auction browsing touch world-thread structures; the map
        // thread only enqueues.
        if (now >= state.nextMailCheckAt)
        {
            state.nextMailCheckAt = now + std::chrono::minutes(3);
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingMailCollections.insert(bot->GetGUID().GetRawValue());
        }

        // Straight after login, buy and sell once with no per-pass limit. The
        // stagger below exists to keep the steady state cheap; a fleet coming up
        // naked and rich should not wait ten minutes for its first item.
        if (!bot->IsInCombat() && (state.auctionCatchUpBuy || state.auctionCatchUpSell))
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            if (cfg.auctionBuyEnabled && state.auctionCatchUpBuy)
            {
                state.nextAuctionShopAt = now + std::chrono::minutes(10);
                g_PendingAuctionShopping.insert(bot->GetGUID().GetRawValue());
            }
            if (cfg.auctionSellEnabled && state.auctionCatchUpSell)
            {
                state.nextAuctionSellAt = now + std::chrono::minutes(8);
                g_PendingAuctionSales.insert(bot->GetGUID().GetRawValue());
            }
        }

        // Selling the surplus: everything looted that the bot will never wear is
        // worth more on the house than rotting in a bag.
        if (cfg.auctionSellEnabled && !bot->IsInCombat() && now >= state.nextAuctionSellAt)
        {
            // Staggered like the shopping pass so a fleet restart does not queue
            // every bot for the same full-house scan.
            if (state.nextAuctionSellAt == PveTimePoint())
                state.nextAuctionSellAt = now + std::chrono::seconds(120 + bot->GetGUID().GetCounter() % 420);
            else
            {
                state.nextAuctionSellAt = now + std::chrono::minutes(8);
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingAuctionSales.insert(bot->GetGUID().GetRawValue());
            }
        }

        if (cfg.auctionBuyEnabled && !bot->IsInCombat() && now >= state.nextAuctionShopAt)
        {
            // First pass after a restart: spread the fleet's shopping trips out
            // instead of lining every bot up for the same world tick.
            if (state.nextAuctionShopAt == PveTimePoint())
                state.nextAuctionShopAt = now + std::chrono::seconds(60 + bot->GetGUID().GetCounter() % 540);
            else
            {
                state.nextAuctionShopAt = now + std::chrono::minutes(10);
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingAuctionShopping.insert(bot->GetGUID().GetRawValue());
            }
        }
    }

    // A player-controlled attacker, ranked as the person controlling it.
    //
    // Returns the owning player when that owner can be fought, the attacker itself
    // when it cannot, and nullptr when the attacker must be ignored entirely
    // (a managed bot, or a managed bot's pet - playerbots never fight each other).
    //
    // Shared deliberately. Getting this right in one target-selection branch and
    // not another is precisely how a bot ends up ping-ponging: one converts the pet
    // to its owner and the next converts it straight back, and the last writer wins.
    Unit* RankAttackerAsOwner(Player* bot, Unit* attacker)
    {
        if (!attacker)
            return nullptr;

        if (attacker->GetTypeId() == TYPEID_PLAYER)
            return playerbot::IsManagedRandomBot(attacker->ToPlayer()) ? nullptr : attacker;

        if (Unit* owner = attacker->GetCharmerOrOwner())
            if (Player* ownerPlayer = owner->ToPlayer())
            {
                if (playerbot::IsManagedRandomBot(ownerPlayer))
                    return nullptr;   // the no-friendly-fire rule follows the pet home

                if (ownerPlayer->IsAlive() && bot->IsValidAttackTarget(ownerPlayer))
                {
                    // If the owner is already within practical combat range, fight
                    // the person controlling the pet. If the owner is far away,
                    // kill the pet that is physically on us instead of running a
                    // suicide chase while being chewed on.
                    if (bot->IsWithinDistInMap(ownerPlayer, PveImmediatePlayerCombatRange))
                        return ownerPlayer;
                    return attacker;
                }
            }

        return attacker;
    }

    // Pick what to answer RIGHT NOW when multiple things are attacking us.
    //
    // Priority:
    //   1) a player already within practical combat range;
    //   2) a monster/pet physically attacking us;
    //   3) a distant player attacker, but only when no close non-player threat exists.
    //
    // This is deliberately different from proactive PvP acquisition. A higher-level
    // player who attacks first is still eligible here.
    Unit* PickDefensiveAttacker(Player* bot, Unit const* ignore = nullptr)
    {
        Unit* nearPlayer = nullptr;
        float nearPlayerDistance = 0.0f;
        Unit* closeNonPlayer = nullptr;
        float closeNonPlayerDistance = 0.0f;
        Unit* distantPlayer = nullptr;
        float distantPlayerDistance = 0.0f;

        for (Unit* attacker : bot->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || !bot->IsValidAttackTarget(attacker))
                continue;

            Unit* ranked = RankAttackerAsOwner(bot, attacker);
            if (!ranked || ranked == ignore || !ranked->IsAlive() || !bot->IsValidAttackTarget(ranked))
                continue;

            float const distance = bot->GetDistance(ranked);
            if (ranked->GetTypeId() == TYPEID_PLAYER)
            {
                if (distance <= PveImmediatePlayerCombatRange)
                {
                    if (!nearPlayer || distance < nearPlayerDistance)
                    {
                        nearPlayer = ranked;
                        nearPlayerDistance = distance;
                    }
                }
                else if (!distantPlayer || distance < distantPlayerDistance)
                {
                    distantPlayer = ranked;
                    distantPlayerDistance = distance;
                }
            }
            else if (!closeNonPlayer || distance < closeNonPlayerDistance)
            {
                closeNonPlayer = ranked;
                closeNonPlayerDistance = distance;
            }
        }

        // Whoever is fighting the bot WITHOUT swinging at it.
        //
        // getAttackers() is filled by Unit::Attack, so it lists melee and
        // auto-attack initiators and nobody else. A player who only casts is
        // absent from it, and the bot was left with no defensive target at all -
        // it stood still and died to a mage while fighting back perfectly well
        // against anyone who swung once. That is what made it look intermittent.
        //
        // IsEngagedBy resolves to IsInCombatWith for a player, which has no
        // threat list, and the combat manager records the engagement however the
        // damage arrived. Only walked when the attacker list produced nothing and
        // the bot is in combat regardless - an ordinary melee fight never pays
        // for the scan, which matters on a 250ms per-bot tick.
        if (!nearPlayer && !closeNonPlayer && !distantPlayer && bot->IsInCombat())
        {
            if (Map* map = bot->FindMap())
            {
                for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin();
                    itr != map->GetPlayers().end(); ++itr)
                {
                    Player* engaged = itr->GetSource();
                    if (!engaged || engaged == bot || !engaged->IsAlive())
                        continue;

                    // Bots are one team; never let this turn into friendly fire.
                    if (playerbot::IsManagedRandomBot(engaged))
                        continue;

                    if (engaged == ignore || !bot->IsEngagedBy(engaged) ||
                        !bot->IsValidAttackTarget(engaged) ||
                        !bot->IsWithinDistInMap(engaged, PveDefensiveEngagementScanYards))
                        continue;

                    float const distance = bot->GetDistance(engaged);
                    if (distance <= PveImmediatePlayerCombatRange)
                    {
                        if (!nearPlayer || distance < nearPlayerDistance)
                        {
                            nearPlayer = engaged;
                            nearPlayerDistance = distance;
                        }
                    }
                    else if (!distantPlayer || distance < distantPlayerDistance)
                    {
                        distantPlayer = engaged;
                        distantPlayerDistance = distance;
                    }
                }
            }
        }

        if (nearPlayer)
            return nearPlayer;
        if (closeNonPlayer)
            return closeNonPlayer;
        return distantPlayer;
    }

    // Is this unit actually fighting the bot?
    //
    // GetVictim() alone cannot answer it. That field is only set by Unit::Attack,
    // and a RANGED attacker never calls it - Spell.cpp contains no Attack() at all,
    // so a hunter shooting the bot has GetVictim() pointing anywhere but the bot,
    // permanently. Anything gated on GetVictim() == bot is therefore always false
    // against a ranged enemy, however hard they are hitting.
    bool IsEngagedWithBot(Player* bot, Unit const* candidate)
    {
        if (!candidate)
            return false;

        if (candidate->GetVictim() == bot)
            return true;

        for (Unit const* attacker : bot->getAttackers())
        {
            if (!attacker)
                continue;
            if (attacker == candidate || attacker->GetCharmerOrOwner() == candidate)
                return true;
        }

        return false;
    }

    void RunFastTick(Player* bot, PveBotState& state, playerbot::PveConfig const& cfg)
    {
        Player* master = state.masterGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, state.masterGuid);

        // A tame or capture cast in progress must not be interrupted by anything -
        // combat, loot walks, errands or journeys. Taking the beast's hits
        // meanwhile is part of the mechanic.
        //
        // UNIT_STATE_CASTING, not CURRENT_CHANNELED_SPELL: Tame Beast occupies the
        // GENERIC cast slot, so asking only about channels meant the hold never
        // applied to the one spell it was written for. The bot started its twenty
        // second tame, and its own combat logic interrupted it a quarter of a
        // second later - every time, forever. Hunters were seen starting a tame
        // three times in ten minutes and never finishing one.
        if (PveClock::now() < state.tamingUntil &&
            (bot->HasUnitState(UNIT_STATE_CASTING) || bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL)))
            return;

        // Lie still for the feign animation. Purely a deadline - it cannot latch
        // the way a condition-based hold can.
        if (PveClock::now() < state.feignHoldUntil)
            return;

        // A channelled spell dies the instant the bot walks. Mend Pet, Drain
        // Life, Mind Flay and the rest need this tick to keep its hands off
        // movement until the channel finishes or breaks on its own. Bounded by
        // the spell's own duration, so unlike a condition-based hold it cannot
        // latch: when the channel ends the spell clears and the tick resumes.
        if (Spell const* channelled = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            SpellInfo const* channelInfo = channelled->GetSpellInfo();
            if (channelInfo && channelInfo->IsChanneled() && !channelInfo->IsMoveAllowedChannel())
                return;
        }

        // Must run before the disengage path clears the dead victim's guid.
        DetectFreshKillForLoot(bot, state, cfg);

        // A hunter without a pet goes and gets one before it does anything else.
        if (DriveHunterTaming(bot, state))
            return;

        ObjectGuid const previousTargetGuid = bot->GetTarget();
        Unit* target = ResolveAttackableByGuid(bot, previousTargetGuid);

        // Zone guardians hunt: an intruder outranks whatever creature is being
        // farmed. Without this the guardian only ever notices players between
        // kills, which in a busy zone is almost never - it holds the zone, so
        // the zone's visitors come first.
        if (target && target->GetTypeId() != TYPEID_PLAYER && state.masterGuid.IsEmpty() &&
            GetGuardianZoneId(bot->GetGUID().GetRawValue()))
        {
            // Timidity no longer ends the search, it narrows it: a guardian that
            // just lost still answers a bounty in its zone.
            bool const timid = PveClock::now() < state.timidUntil;
            if (Player* intruder = PickHuntTarget(bot, cfg.guardianPlayerApproachYards, timid))
            {
                target = intruder;
                state.recentBadTargets.erase(intruder->GetGUID().GetRawValue());
            }
        }

        // A held target that stopped resolving while still alive (evade flicker,
        // validity flicker) must not be immediately re-acquired: the resulting
        // engage/AttackStop cycle stutters both the bot and the mob chasing it.
        if (target)
            state.targetResolveMisses = 0;

        if (!target && state.engaged && !previousTargetGuid.IsEmpty())
        {
            Unit const* lost = ObjectAccessor::GetUnit(*bot, previousTargetGuid);
            if (lost && lost->IsAlive())
            {
                // Hold through a brief flicker rather than abandoning the fight.
                // A knockback, a stun, a moment of invalid-target state can all
                // make a LIVE victim fail to resolve for a tick or two, and
                // blacklisting on the first miss meant the bot dropped the fight
                // it was already winning and went and pulled something else -
                // which is exactly what a knockdown looked like in game. Only a
                // target that stays unresolvable is really gone.
                if (++state.targetResolveMisses < PveTargetResolveGrace)
                    return;

                MarkRecentBadTarget(state, previousTargetGuid);
            }

            if (playerbot::PveManager::GetConfig().combatDiagnostics)
            {
                Creature const* lostCreature = lost ? lost->ToCreature() : nullptr;
                TC_LOG_INFO("playerbots.pve",
                    "combatdiag bot={} LOST target={} resolved={} alive={} validAttack={} evade={}",
                    bot->GetName(), previousTargetGuid.ToString(),
                    lost ? 1 : 0, lost && lost->IsAlive() ? 1 : 0,
                    lost && bot->IsValidAttackTarget(lost) ? 1 : 0,
                    lostCreature && lostCreature->IsInEvadeMode() ? 1 : 0);
            }
        }
        if (!state.orderedTargetGuid.IsEmpty())
        {
            if (Unit* ordered = ResolveAttackableByGuid(bot, state.orderedTargetGuid))
                target = ordered;
            state.orderedTargetGuid = ObjectGuid::Empty;
        }

        // A HELD pet target is re-examined every tick, not only when a new target
        // is picked.
        //
        // The self-defence branch below already ranks a player-controlled attacker
        // as its owner, but it sits behind "else if (!target)" - it only runs when
        // the bot has nothing. So it fixes the moment of choosing and nothing
        // after it, and a bot that did latch onto a pet stayed on it for the whole
        // fight while the owner shot it in the back. Latching is easy: that branch
        // deliberately falls back to the pet when the owner is not a legal target
        // yet, which is exactly the instant before a hunter's own attack flags
        // them, so the pet gets in first and then keeps the slot forever.
        //
        // Switching here costs nothing when the owner is already the target, and
        // the same no-friendly-fire rule applies as below: a bot's pet resolves to
        // a bot, which is never a legal target.
        if (target && target->GetTypeId() != TYPEID_PLAYER)
        {
            if (Unit* petOwner = target->GetCharmerOrOwner())
            {
                if (Player* ownerPlayer = petOwner->ToPlayer())
                {
                    // The reason above is retaliation - "the owner shot it in
                    // the back" - so the promotion is allowed when that is
                    // actually true, and when the owner is fair game anyway.
                    // Without this it fires on ANY held pet, including one the
                    // bot chose for itself, which is a way to reach a person
                    // through their pet that no other path would allow.
                    bool const provoked = IsSwingingAt(bot, ownerPlayer);

                    if (!playerbot::IsManagedRandomBot(ownerPlayer) && ownerPlayer->IsAlive() &&
                        bot->IsValidAttackTarget(ownerPlayer) &&
                        (provoked || playerbot::PveManager::MayProactivelyEngage(bot, ownerPlayer)) &&
                        bot->IsWithinDistInMap(ownerPlayer, PveImmediatePlayerCombatRange))
                    {
                        TC_LOG_DEBUG("playerbots.pve", "Bot {} switches from pet {} to nearby owner {}.",
                            bot->GetName(), target->GetName(), ownerPlayer->GetName());
                        target = ownerPlayer;
                    }
                }
            }
        }

        // Do not keep an autonomous uphill PvP target merely because it was held
        // from a previous tick. If the higher-level player has not attacked this
        // bot, drop the voluntary fight and let normal grind/defense selection run.
        // Companions are excluded because their human master's orders/assist logic
        // owns their target choice.
        if (target && target->GetTypeId() == TYPEID_PLAYER && state.masterGuid.IsEmpty() &&
            !IsEngagedWithBot(bot, target) &&
            !IsProactivePlayerLevelAcceptable(bot, target->ToPlayer()))
        {
            TC_LOG_DEBUG("playerbots.pve", "Bot {} drops uphill proactive PvP target {} ({} vs {}).",
                bot->GetName(), target->GetName(), uint32(bot->GetLevel()), uint32(target->GetLevel()));
            target = nullptr;
        }

        if (state.passive)
        {
            if (state.engaged)
                DisengagePveCombat(bot, state);
            target = nullptr;
        }
        else if (!target)
        {
            // Self-defense first, with no level window: a higher-level player that
            // attacks first is still fought. The defensive picker is distance-aware:
            // if that player is far away while a monster/pet is physically on the
            // bot, clear the close threat first instead of suicide-chasing the player.
            if (Unit* defensive = PickDefensiveAttacker(bot))
            {
                target = defensive;
                state.recentBadTargets.erase(defensive->GetGUID().GetRawValue());
            }
            else if (master)
                target = PickCompanionTarget(bot, state, master, cfg);
            else if (state.masterGuid.IsEmpty() && !HasBrokenEquippedItem(bot) &&
                ReadyToFightPlayers(bot, cfg) &&
                cfg.proactiveHuntYards > 0.0f &&
                BarracksHardcore::IsOpenWorldPvpZone(bot->GetZoneId()))
            {
                // Any free open-world bot that finds a lawful real-player target
                // inside its hunt radius should actually go pick the fight.
                // Previously this proactive selection existed only for guardians;
                // ordinary aggression hunters could teleport beside a player and
                // then fall straight back into their creature grind loop.
                //
                // Its own, shorter radius. A guardian holds a zone and is meant
                // to notice anyone in it; the rest of the fleet reaching just as
                // far is what left someone grinding alone unable to finish a
                // pull. The guardian value is deliberately NOT reused here - it
                // has to stay above the 210 yard teleport landing distance for
                // reasons that have nothing to do with how far a bot may hunt.
                // Timidity is a rule about not re-picking the person who just
                // beat you, and it used to end this branch outright. A bounty
                // overrides it: five bots benched for ten minutes each is how a
                // bountied player could clear an area and then be left alone by
                // the very fleet their bounty is supposed to summon. Still timid
                // means bountied targets ONLY - an ordinary player keeps the
                // reprieve they earned by winning.
                bool const timid = PveClock::now() < state.timidUntil;
                target = PickHuntTarget(bot, cfg.proactiveHuntYards, timid);
            }

            // Death caches are opportunistic loot, not a new combat activity.  A
            // bot that finished a fight low on health/mana used to skip this call
            // entirely because TryClaimNearbyDeathChest sat behind
            // !NeedsRecovery().  That left casters standing directly on their
            // cache doing nothing until recovery happened (and could deadlock if
            // they had no drink).  Claim the cache before the recovery/grind gate;
            // active eating/drinking still wins because opening a chest would
            // interrupt it.
            bool claimedDeathChest = false;
            if (!target && state.masterGuid.IsEmpty() && !IsRestingNow(bot, state))
                claimedDeathChest = TryClaimNearbyDeathChest(bot, state, cfg);

            if (!target && !claimedDeathChest && cfg.grindEnabled && state.masterGuid.IsEmpty() &&
                !HasBrokenEquippedItem(bot) &&
                !IsRestingNow(bot, state) && !NeedsRecovery(bot, cfg) &&
                state.errandKind == PveErrandKind::None)
            {
                // Free gear on the floor outranks finding something new to hit, and
                // an errand already in flight outranks it too - otherwise the bot
                // starts a fight on the way and never arrives. Note this only gates
                // picking a NEW target: the attacker branch above still answers
                // anything that is actually hitting the bot.
                // Zone guardians hunt players above all else: they hold their
                // zone against intruders and only grind between kills. Unless one
                // has just been killed by a person - gating only the approach let a
                // timid guardian stand up at the graveyard and immediately pick its
                // killer again, which is the whole behaviour this was meant to stop.
                // That protection now covers everyone EXCEPT a bountied player,
                // who has explicitly bought the attention back.
                // Retaliation is unaffected: the attacker branch above still answers
                // anything actually hitting the bot.
                if (GetGuardianZoneId(bot->GetGUID().GetRawValue()))
                    target = PickHuntTarget(bot, cfg.guardianPlayerApproachYards,
                        PveClock::now() < state.timidUntil);

                // Packmates first: adjacent bots fight together (one team),
                // adopting the fight of any nearby bot already in combat.
                if (!target)
                    target = PickBotAssistTarget(bot, cfg);

                PveTimePoint const now = PveClock::now();
                if (!target && now >= state.nextGrindScanAt)
                {
                    state.nextGrindScanAt = now + PveGrindScanInterval;
                    target = PickGrindTarget(bot, state, cfg);
                }
            }
        }

        // Even a player who IS actively attacking does not make a close add vanish.
        // If the current player target is outside practical combat range and a
        // monster/pet is physically attacking the bot, clear that local threat first.
        // The player remains in getAttackers(), so once the add dies self-defense
        // selects the player again and retaliation continues.
        if (target && target->GetTypeId() == TYPEID_PLAYER &&
            !bot->IsWithinDistInMap(target, PveImmediatePlayerCombatRange))
        {
            if (Unit* defensive = PickDefensiveAttacker(bot, target))
            {
                if (defensive->GetTypeId() != TYPEID_PLAYER)
                {
                    target = defensive;
                    state.recentBadTargets.erase(defensive->GetGUID().GetRawValue());
                }
            }
        }

        // An add picked up on the approach outranks a target that has not engaged
        // us yet. A nearby attacking player still wins, but if the player is outside
        // practical combat range and a monster/pet is already on us, kill the close
        // threat first. Once it dies, the distant attacker can be chased.
        if (target && !IsEngagedWithBot(bot, target))
        {
            if (Unit* defensive = PickDefensiveAttacker(bot, target))
            {
                target = defensive;
                state.recentBadTargets.erase(defensive->GetGUID().GetRawValue());
            }
        }

        // Use-item objectives (taming rods, capture devices): the wanted
        // creature must be USED on with the quest's source item - killing it
        // grants nothing and the grind loop would farm it forever.
        if (target && !state.passive && !bot->HasUnitState(UNIT_STATE_CASTING))
        {
            if (Item* questItem = FindQuestSourceItemFor(bot, target->GetEntry()))
            {
                if (bot->GetTarget() != target->GetGUID())
                    bot->SetSelection(target->GetGUID());
                if (bot->IsWithinDistInMap(target, 25.0f) && bot->IsWithinLOSInMap(target))
                {
                    playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                    if (MotionMaster* motionMaster = bot->GetMotionMaster())
                        motionMaster->Clear();
                    bot->StopMoving();
                    if (!bot->isInFront(target))
                    {
                        bot->SetFacingToObject(target);
                        bot->SetInFront(target);
                    }
                    SpellCastTargets questItemTargets;
                    questItemTargets.SetUnitTarget(target);
                    bot->CastItemUseSpell(questItem, questItemTargets, 0, 0);
                    state.tamingUntil = PveClock::now() + std::chrono::seconds(25);
                }
                else if (!bot->isMoving())
                    playerbot::PvpClassActions::IssueFollowMovement(bot, target, 20.0f);
                return;
            }
        }

        if (target)
        {
            // ARRIVING resets the relocation counter, not merely picking someone
            // out. Resetting on selection alone held the counter at zero for a
            // bot that never reached anything, which is exactly the bot the
            // relocation was meant to rescue - the comment further down claims
            // this invariant, and this is where it was being broken.
            if (bot->IsWithinDistInMap(target, 30.0f))
                state.dryWanderCount = 0;

            if (bot->GetTarget() != target->GetGUID())
                bot->SetSelection(target->GetGUID());
            if (!state.engaged)
            {
                state.engaged = true;
                state.engagedSince = PveClock::now();
                // A bot jumped mid-meal must not fight sitting down.
                bot->SetStandState(UNIT_STAND_STATE_STAND);
                TC_LOG_INFO("playerbots.pve", "Bot {} engaging {} (level {}) at {:.0f}y.",
                    bot->GetName(), target->GetName(), target->GetLevel(), bot->GetDistance(target));
            }

            // Re-arm the registry every combat tick, not just on the first: the
            // relocation executor (and any future cross-thread cleanup) clears
            // the registry without seeing this map's engaged flag, and a bot
            // whose registry is down fights with white swings only.
            playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), true);

            // A fight en route must not count as journey stall.
            if (state.journeyActive)
                state.journeyProgressAt = PveClock::now();

            ExecuteEngagedCombatTick(bot, state);
            return;
        }

        if (state.engaged)
            DisengagePveCombat(bot, state);

        // Rest holds BEFORE the loot walk: a bot that just sat down to eat must
        // not slide seated toward its kill's corpse - movement never auto-stands
        // a server-driven bot, so it visibly glides while chewing. The corpse
        // waits the twenty seconds.
        if (IsRestingNow(bot, state))
        {
            bool const stillRecovering = bot->GetHealthPct() < 99.0f ||
                (bot->GetMaxPower(POWER_MANA) > 0 && bot->GetPowerPct(POWER_MANA) < 99.0f);
            bool const masterLeftRestRange = master && bot->GetDistance(master) > PveRestBreakFollowDistance;
            if (stillRecovering && !masterLeftRestRange)
            {
                // A rest stop en route must not count as journey stall.
                if (state.journeyActive)
                    state.journeyProgressAt = PveClock::now();
                return;
            }

            RemoveRestAuras(bot);
            // Standing up also removes the consumable food/drink auras via the
            // core's NOT_SEATED interrupt; a seated bot must never start moving.
            bot->SetStandState(UNIT_STAND_STATE_STAND);
            state.restingUntil = PveClock::now();
        }

        if (ProcessPendingLoot(bot, state, cfg))
            return;

        if (ProcessErrand(bot, state, cfg))
            return;

        if (AdvanceWalkedJourney(bot, state))
            return;

        if (master && master->IsAlive() && !state.stay)
        {
            if (bot->GetDistance(master) > cfg.companionFollowDistance + 1.5f)
                playerbot::PvpClassActions::IssueFollowMovement(bot, master, cfg.companionFollowDistance);
            return;
        }

        // A recovering bot holds still: rest (or the vendor/supply pipeline for
        // an empty pantry) is about to run, and walking cancels eating.
        if (cfg.grindEnabled && state.masterGuid.IsEmpty() && !state.stay && !NeedsRecovery(bot, cfg))
        {
            PveTimePoint const now = PveClock::now();
            if (now >= state.nextWanderAt)
            {
                state.nextWanderAt = now + std::chrono::seconds(urand(12, 25));
                // A freshly logged-in player's MotionMaster is EMPTY and reports
                // MAX_MOTION_TYPE, not IDLE - treating only IDLE as "free to
                // wander" left spawned bots standing forever.
                MotionMaster* motionMaster = bot->GetMotionMaster();
                MovementGeneratorType const currentMovement = motionMaster
                    ? motionMaster->GetCurrentMovementGeneratorType()
                    : MAX_MOTION_TYPE;
                bool const movementIdle = currentMovement == IDLE_MOTION_TYPE || currentMovement == MAX_MOTION_TYPE;
                if (movementIdle && !bot->isMoving())
                {
                    // Every wander cycle that did not END IN AN ENGAGEMENT counts
                    // toward relocation - including cycles that walk toward a
                    // prospect. Only actually reaching combat resets the counter,
                    // so an unreachable prospect (across a chasm) can't pin the
                    // bot in place forever.
                    if (cfg.relocateEnabled && ++state.dryWanderCount >= cfg.relocateDryWanders)
                    {
                        state.dryWanderCount = 0;
                        std::lock_guard<std::mutex> guard(g_PvePendingLock);
                        g_PendingGrindRelocations.insert(bot->GetGUID().GetRawValue());
                    }
                    // Walk toward the nearest attackable creature when one exists
                    // beyond engage range; random-wander only in a truly empty
                    // area. The engage-radius scan takes over on arrival.
                    // A prospect on the far side of a canyon is not a prospect:
                    // MovePoint's straight-line fallback would march the bot into
                    // the terrain and the same chase would re-issue forever.
                    else if (Creature* prospect = FindWalkableGrindProspect(bot, state, cfg))
                    {
                        playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                        bot->GetMotionMaster()->MovePoint(0, prospect->GetPosition(), true);
                        TC_LOG_DEBUG("playerbots.pve", "Grind bot {} walking toward prospect {} at {:.0f}y.",
                            bot->GetName(), prospect->GetName(), bot->GetDistance(prospect));
                    }
                    else
                    {
                        // Guardians patrol: a wider sweep than a grinder's local
                        // shuffle, so a zone gets walked instead of camped.
                        float const wanderRadius = GetGuardianZoneId(bot->GetGUID().GetRawValue())
                            ? std::max(cfg.grindWanderRadius, PveGuardianPatrolRadius)
                            : cfg.grindWanderRadius;
                        Position destination;
                        if (PickWalkableNearPosition(bot, wanderRadius, destination))
                        {
                            playerbot::PvpClassActions::PrepareForExplicitMovement(bot);
                            bot->GetMotionMaster()->MovePoint(0, destination, true);
                        }
                    }
                }
            }
        }
    }

    char const* ErrandKindName(PveErrandKind kind)
    {
        switch (kind)
        {
        case PveErrandKind::Vendor: return "vendor";
        case PveErrandKind::QuestGiver: return "quest-giver";
        case PveErrandKind::QuestObject: return "quest-object";
        default: return "none";
        }
    }

    char const* WalkPathResultName(WalkPathResult result)
    {
        switch (result)
        {
        case WalkPathResult::Reachable: return "reachable";
        case WalkPathResult::Unreachable: return "unreachable";
        case WalkPathResult::Deferred: return "deferred-budget";
        default: return "unknown";
        }
    }

    int64 SecondsRemaining(PveTimePoint until)
    {
        if (until == PveTimePoint{})
            return 0;
        auto const now = PveClock::now();
        if (now >= until)
            return 0;
        return std::chrono::duration_cast<std::chrono::seconds>(until - now).count();
    }

    std::vector<std::string> BuildPveWhisperDiagnostics(Player* bot, Player* asker)
    {
        std::vector<std::string> lines;
        if (!bot)
            return lines;

        uint64 const rawGuid = bot->GetGUID().GetRawValue();
        PveBotState state;
        playerbot::LockedGetCopy(g_PveBotStateByGuid, rawGuid, state);

        bool pendingRelocation = false;
        bool pendingStuckRelocation = false;
        bool pendingHuntTeleport = false;
        bool pendingRebirth = false;
        bool pendingSupplyRun = false;
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            pendingRelocation = g_PendingGrindRelocations.count(rawGuid) != 0;
            pendingStuckRelocation = g_PendingStuckRelocations.count(rawGuid) != 0;
            pendingHuntTeleport = g_PendingGuardianTeleports.count(rawGuid) != 0;
            pendingRebirth = g_PendingRebirths.count(rawGuid) != 0;
            pendingSupplyRun = g_PendingSupplyRuns.count(rawGuid) != 0;
        }

        uint32 const zoneId = bot->GetZoneId();
        uint32 zoneSpots = 0;
        uint32 zoneTop = 0;
        if (auto itr = g_ZoneSpotCount.find(zoneId); itr != g_ZoneSpotCount.end())
            zoneSpots = itr->second;
        if (auto itr = g_ZoneTopLevel.find(zoneId); itr != g_ZoneTopLevel.end())
            zoneTop = itr->second;

        uint32 const guardianZoneId = GetGuardianZoneId(rawGuid);
        bool const guardian = guardianZoneId != 0;
        bool const veteran = IsLocalVeteranGuid(rawGuid);
        bool const pvpOnly = playerbot::PveManager::IsPvpOnlyBot(bot);
        bool const companion = !state.masterGuid.IsEmpty();
        uint32 const localHomeZoneId = (!guardian && !veteran && !pvpOnly && g_PveConfig.rebirthZoneBanded)
            ? GetRebirthZoneId(bot) : 0u;
        uint32 const assignedZoneId = guardian ? guardianZoneId : localHomeZoneId;
        ClassicZoneBand const* assignedBand = assignedZoneId ? FindClassicZoneBand(assignedZoneId) : nullptr;
        bool const suitable = BotIsInSuitableZone(bot);
        bool const openWorldPvp = BarracksHardcore::IsOpenWorldPvpZone(zoneId);

        // A drifter used to report as "banded", which is the one thing it is
        // not: it does not live anywhere, it follows people. It is checked
        // before the home-zone test because it HAS a home zone - a borrowed one,
        // reassigned every time its person moves.
        char const* role = guardian ? "guardian" :
            (companion ? "companion" :
                (pvpOnly ? "pvp-only" :
                    (veteran ? "veteran" :
                        (IsDrifter(rawGuid) ? "drifter" :
                            (localHomeZoneId ? "local" : "unassigned")))));

        std::ostringstream roleLine;
        roleLine << "PB role diag: role=" << role
            << " guardian_zone=" << guardianZoneId
            << " local_home=" << localHomeZoneId
            << " assigned_zone=" << assignedZoneId;
        if (assignedBand)
            roleLine << " assigned_band=" << uint32(assignedBand->minLevel) << "-" << uint32(assignedBand->maxLevel);
        else
            roleLine << " assigned_band=none";
        roleLine << " companion=" << (companion ? "yes" : "no")
            << " veteran=" << (veteran ? "yes" : "no")
            << " pvp_only=" << (pvpOnly ? "yes" : "no");
        lines.push_back(roleLine.str());

        std::ostringstream stateLine;
        stateLine << "PB PvE diag: lvl=" << uint32(bot->GetLevel())
            << " map=" << bot->GetMapId() << " zone=" << zoneId
            << " suitable=" << (suitable ? "yes" : "NO")
            << " zone_top=" << zoneTop << " spots=" << zoneSpots
            << " guardian=" << (guardian ? "yes" : "no")
            << " ow_pvp=" << (openWorldPvp ? "yes" : "no")
            << " passive=" << (state.passive ? "yes" : "no")
            << " stay=" << (state.stay ? "yes" : "no")
            << " engaged=" << (state.engaged ? "yes" : "no")
            << " combat=" << (bot->IsInCombat() ? "yes" : "no")
            << " moving=" << (bot->isMoving() ? "yes" : "no");
        lines.push_back(stateLine.str());

        std::ostringstream blockers;
        blockers << "PB PvE gates: alive=" << (bot->IsAlive() ? "yes" : "no")
            << " controlled=" << (bot->HasUnitState(UNIT_STATE_CONTROLLED) ? "YES" : "no")
            << " casting=" << (bot->HasUnitState(UNIT_STATE_CASTING) ? "yes" : "no")
            << " channel=" << (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) ? "yes" : "no")
            << " flight=" << (bot->IsInFlight() ? "yes" : "no")
            << " teleport=" << ((bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear()) ? "yes" : "no")
            << " errand=" << ErrandKindName(state.errandKind)
            << " journey=" << (state.journeyActive ? "yes" : "no")
            << " rest_s=" << SecondsRemaining(state.restingUntil)
            << " tame_s=" << SecondsRemaining(state.tamingUntil)
            << " feign_s=" << SecondsRemaining(state.feignHoldUntil)
            << " timid_s=" << SecondsRemaining(state.timidUntil);
        lines.push_back(blockers.str());

        std::string errandTarget = "none";
        if (!state.errandGuid.IsEmpty())
        {
            if (Creature* npc = ObjectAccessor::GetCreature(*bot, state.errandGuid))
                errandTarget = npc->GetName();
            else if (GameObject* go = ObjectAccessor::GetGameObject(*bot, state.errandGuid))
                errandTarget = "go:" + std::to_string(go->GetEntry());
            else
                errandTarget = "unresolved";
        }

        std::ostringstream intentLine;
        intentLine << "PB intent diag: errand=" << ErrandKindName(state.errandKind)
            << " errand_target=" << errandTarget
            << " errand_s=" << SecondsRemaining(state.errandUntil)
            << " journey=" << (state.journeyActive ? "yes" : "no");
        if (state.journeyActive)
            intentLine << " journey_map=" << state.journeyMapId
            << " journey_dest=(" << state.journeyX << "," << state.journeyY << "," << state.journeyZ << ")"
            << " journey_fallback=" << uint32(state.journeyFallbackKind)
            << " journey_s=" << SecondsRemaining(state.journeyUntil);
        lines.push_back(intentLine.str());

        uint32 brokenEquipped = 0;
        uint32 criticalEquipped = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (!maxDurability)
                continue;

            uint32 const durability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            if (durability == 0)
                ++brokenEquipped;
            if (durability * 100 < maxDurability * 10)
                ++criticalEquipped;
        }

        std::ostringstream gearDiag;
        gearDiag << "PB gear diag: broken_equipped=" << brokenEquipped
            << " critical_lt10=" << criticalEquipped
            << " money_copper=" << bot->GetMoney()
            << " vendor_enabled=" << (g_PveConfig.vendorEnabled ? "yes" : "NO")
            << " repair_block_new_fights=" << (brokenEquipped ? "YES" : "no");
        lines.push_back(gearDiag.str());

        std::ostringstream relocation;
        relocation << "PB relocate diag: cache=" << (g_GrindSpotsBuilt ? "built" : "EMPTY")
            << " queued=" << (pendingRelocation ? "yes" : "no")
            << " stuck_queued=" << (pendingStuckRelocation ? "yes" : "no")
            << " dry=" << state.dryWanderCount << "/" << g_PveConfig.relocateDryWanders
            << " zonefit_next_s=" << SecondsRemaining(state.nextZoneFitCheckAt)
            << " hunt_tp_queued=" << (pendingHuntTeleport ? "yes" : "no")
            << " rebirth_queued=" << (pendingRebirth ? "yes" : "no")
            << " supply_queued=" << (pendingSupplyRun ? "yes" : "no");
        lines.push_back(relocation.str());

        // Death-cache diagnostics.  This is deliberately independent of the
        // current errand so "why" can distinguish "I cannot see a cache" from
        // "I see it but another gate is winning".
        std::ostringstream chestDiag;
        chestDiag << "PB chest diag: entry=" << g_PveConfig.hardcoreLootChestEntry
            << " recovery=" << (NeedsRecovery(bot, g_PveConfig) ? "YES" : "no")
            << " resting=" << (IsRestingNow(bot, state) ? "yes" : "no")
            << " free_slots=" << CountFreeBagSlots(bot)
            << " pending_loot=" << (state.pendingLootGuid.IsEmpty() ? "no" : "yes")
            << " errand=" << ErrandKindName(state.errandKind)
            << " opening=" << (state.chestOpeningGuid.IsEmpty() ? "no" : "yes");

        if (!g_PveConfig.hardcoreLootChestEntry)
            chestDiag << " nearest=disabled";
        else if (GameObject* chest = FindRegisteredDeathChest(bot, g_PveConfig.hardcoreLootChestEntry, 200.0f))
        {
            chestDiag << " nearest=" << chest->GetEntry()
                << " dist=" << uint32(bot->GetDistance(chest))
                << " spawned=" << (chest->isSpawned() ? "yes" : "NO")
                << " loot_state=" << uint32(chest->getLootState())
                << " recent=" << (IsRecentErrandTarget(state, chest->GetGUID()) ? "YES" : "no")
                << " interact_range=" << (bot->IsWithinDistInMap(chest, INTERACTION_DISTANCE + 2.0f) ? "yes" : "no");
        }
        else
            chestDiag << " nearest=none-in-registry";

        lines.push_back(chestDiag.str());

        std::ostringstream hunt;
        hunt << "PB hunt diag: aggression=" << uint32(GetBotAggression(bot))
            << " hunt_after_m=" << AggressionIdleMinutes(bot, g_PveConfig)
            << " approach_y=" << g_PveConfig.guardianPlayerApproachYards;

        if (!asker)
        {
            hunt << " asker=none";
            lines.push_back(hunt.str());
            return lines;
        }

        bool const sameMap = asker->IsInWorld() && asker->GetMapId() == bot->GetMapId();
        float askerDistance = 0.0f;
        bool askerAttackable = false;
        bool askerInApproach = false;
        bool askerPathChecked = false;
        WalkPathResult askerPath = WalkPathResult::Deferred;

        hunt << " you=" << asker->GetName()
            << " same_map=" << (sameMap ? "yes" : "no")
            << " alive=" << (asker->IsAlive() ? "yes" : "no")
            << " bot_lvl=" << uint32(bot->GetLevel())
            << " you_lvl=" << uint32(asker->GetLevel())
            << " proactive_level_ok=" << (IsProactivePlayerLevelAcceptable(bot, asker) ? "yes" : "NO")
            << " gm=" << (asker->IsGameMaster() ? "YES" : "no")
            << " no_pvp=" << (asker->pvpInfo.IsInNoPvPArea ? "YES" : "no");

        if (sameMap)
        {
            askerDistance = bot->GetDistance(asker);
            askerAttackable = bot->IsValidAttackTarget(asker);
            askerInApproach = g_PveConfig.guardianPlayerApproachYards > 0.0f &&
                bot->IsWithinDistInMap(asker, g_PveConfig.guardianPlayerApproachYards);
            bool const los = bot->IsWithinLOSInMap(asker);
            hunt << " dist=" << uint32(askerDistance)
                << " attackable=" << (askerAttackable ? "yes" : "NO")
                << " in_approach=" << (askerInApproach ? "yes" : "NO")
                << " los=" << (los ? "yes" : "no");

            // An explicit diagnostic request is allowed to spend one path token:
            // this is exactly the information needed when a bot can see/attack a
            // player geometrically but cannot navigate to them.
            askerPath = CheckWalkPath(bot, asker->GetPosition());
            askerPathChecked = true;
            hunt << " path=" << WalkPathResultName(askerPath);
        }

        if (Player* picked = PickHuntTarget(bot, g_PveConfig.guardianPlayerApproachYards))
            hunt << " picked=" << picked->GetName() << "@" << uint32(bot->GetDistance(picked)) << "y";
        else
            hunt << " picked=none";

        lines.push_back(hunt.str());

        std::ostringstream target;
        target << "PB target diag: selected=";
        if (Unit* selected = ObjectAccessor::GetUnit(*bot, bot->GetTarget()))
            target << selected->GetName() << "/" << selected->GetGUID().ToString();
        else
            target << "none";
        target << " victim=";
        if (Unit* victim = bot->GetVictim())
            target << victim->GetName() << "/" << victim->GetGUID().ToString();
        else
            target << "none";
        target << " ordered=" << (state.orderedTargetGuid.IsEmpty() ? "none" : state.orderedTargetGuid.ToString())
            << " bad_targets=" << state.recentBadTargets.size()
            << " resolve_misses=" << state.targetResolveMisses;
        lines.push_back(target.str());

        std::string verdict = "eligible; should acquire you on the next fast tick";
        if (!bot->IsAlive())
            verdict = "bot is dead";
        else if (playerbot::PveManager::IsPvpOnlyBot(bot))
            verdict = "bot is PvP-only; PvE hunt tick is disabled";
        else if (bot->InBattleground() || bot->duel)
            verdict = "battleground/duel owns its combat AI";
        else if (state.passive)
            verdict = "bot is in passive mode";
        else if (!state.masterGuid.IsEmpty())
            verdict = "bot is a companion; master/assist targeting outranks autonomous hunting";
        else if (bot->HasUnitState(UNIT_STATE_CONTROLLED))
            verdict = "bot is controlled/rooted/stunned/confused and lifecycle exits before fast combat";
        else if (SecondsRemaining(state.tamingUntil) || SecondsRemaining(state.feignHoldUntil))
            verdict = "bot is deliberately held by tame/feign logic";
        else if (SecondsRemaining(state.timidUntil))
            verdict = "bot is timid after losing a player fight";
        else if (!openWorldPvp)
            verdict = "current zone is not an open-world PvP hunt zone";
        else if (!sameMap)
            verdict = "you are not on the bot's map";
        else if (!asker->IsAlive())
            verdict = "you are dead";
        else if (asker->IsGameMaster())
            verdict = "you are GM-flagged and are intentionally excluded as a hunt target";
        else if (asker->pvpInfo.IsInNoPvPArea)
            verdict = "you are in a no-PvP area and are excluded from the human snapshot";
        else if (!askerAttackable)
            verdict = "IsValidAttackTarget says you are not attackable";
        else if (!IsProactiveTargetWithinPower(bot, asker->GetLevel()))
            verdict = "you are higher level; bot will not initiate but will retaliate if attacked";
        else if (!IsProactivePlayerLevelAcceptable(bot, asker))
            verdict = "you are too far below this bot for it to start anything; it will still "
                "retaliate if attacked, and a big enough bounty lifts this";
        else if (!askerInApproach)
            verdict = "you are outside the configured proactive hunt radius";
        else if (askerPathChecked && askerPath == WalkPathResult::Unreachable)
            verdict = "you are in range/attackable but mmap says there is no walkable path";
        else if (askerPathChecked && askerPath == WalkPathResult::Deferred)
            verdict = "path verdict deferred because the navmesh budget is exhausted";

        lines.push_back(std::string("PB verdict: ") + verdict);

        return lines;
    }

}

namespace playerbot
{
    void PveManager::LoadConfig()
    {
        g_PveConfig.enabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Enable", false);
        g_PveConfig.companionFollowDistance = sConfigMgr->GetFloatDefault("Playerbot.Pve.Companion.FollowDistance", 2.5f);
        g_PveConfig.companionAssistRadius = sConfigMgr->GetFloatDefault("Playerbot.Pve.Companion.AssistRadius", 45.0f);
        g_PveConfig.autoReviveSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.AutoReviveSeconds", 30), 5, 600));
        g_PveConfig.restHealthPct = sConfigMgr->GetFloatDefault("Playerbot.Pve.RestHealthPct", 60.0f);
        g_PveConfig.restManaPct = sConfigMgr->GetFloatDefault("Playerbot.Pve.RestManaPct", 50.0f);
        g_PveConfig.playerEngageMinHealthPct = sConfigMgr->GetFloatDefault("Playerbot.Pve.PlayerEngageMinHealthPct", 85.0f);
        g_PveConfig.playerEngageMinManaPct = sConfigMgr->GetFloatDefault("Playerbot.Pve.PlayerEngageMinManaPct", 80.0f);
        g_PveConfig.autoLearnSpellsOnLevelUp = sConfigMgr->GetBoolDefault("Playerbot.Pve.AutoLearnSpellsOnLevelUp", true);
        g_PveConfig.grindEnabled = sConfigMgr->GetBoolDefault("Playerbot.PveGrind.Enable", false);
        g_PveConfig.grindSearchRadius = sConfigMgr->GetFloatDefault("Playerbot.PveGrind.SearchRadius", 60.0f);
        g_PveConfig.grindLevelMatchYards = std::max(0.0f, sConfigMgr->GetFloatDefault("Playerbot.PveGrind.LevelMatchYards", 15.0f));
        g_PveConfig.pathBudgetPerSecond = uint32(sConfigMgr->GetIntDefault("Playerbot.Pve.PathBudgetPerSecond", 150));
        g_PveConfig.guardianPlayerApproachYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.ZoneGuardians.PlayerApproachYards", 225.0f));
        g_PveConfig.guardianEscalateAfterMinutes = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Playerbot.Pve.ZoneGuardians.EscalateAfterMinutes", 30)));
        g_PveConfig.aggressionMinMinutes = uint32(std::max(1,
            sConfigMgr->GetIntDefault("Playerbot.Pve.Aggression.MinMinutes", 5)));
        g_PveConfig.aggressionMaxMinutes = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Playerbot.Pve.Aggression.MaxMinutes", 90)));
        g_PveConfig.timidMinutes = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Playerbot.Pve.Aggression.TimidMinutes", 20)));
        g_PveConfig.timidFleeYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.Aggression.TimidFleeYards", 500.0f));
        g_PveConfig.auctionPriceMultiplier = sConfigMgr->GetFloatDefault("Playerbot.Pve.AuctionPriceMultiplier", 10.0f);
        g_PveConfig.auctionMinTradeGoodStack = uint32(sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionMinTradeGoodStack", 10));
        g_PveConfig.auctionValuableUnitCopper = uint32(sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionValuableUnitCopper", 1000));
        g_PveConfig.auctionVendorFloorFactor = sConfigMgr->GetFloatDefault("Playerbot.Pve.AuctionVendorFloorFactor", 1.5f);
        g_PveConfig.auctionUndercutCopper = uint32(std::max(1, sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionUndercutCopper", 1)));
        g_PveConfig.maxSlotsPerItemEntry = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Playerbot.Pve.MaxSlotsPerItem", 3)));
        g_PveConfig.thistleTeaItemId = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Playerbot.Pve.ThistleTeaItemId", 7676)));
        g_PveConfig.thistleTeaEnergyBelow = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.ThistleTeaEnergyBelow", 25), 0, 100));
        g_PveConfig.grindWanderRadius = sConfigMgr->GetFloatDefault("Playerbot.PveGrind.WanderRadius", 40.0f);
        g_PveConfig.grindMaxLevelAbove = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.PveGrind.MaxLevelAbove", 3), 0, 10));
        g_PveConfig.grindMaxLevelBelow = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.PveGrind.MaxLevelBelow", 5), 0, 80));
        g_PveConfig.grindAllowElites = sConfigMgr->GetBoolDefault("Playerbot.PveGrind.AllowElites", false);
        g_PveConfig.lootEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Loot.Enable", true);
        g_PveConfig.vendorEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Vendor.Enable", true);
        g_PveConfig.questsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Quests.Enable", true);
        g_PveConfig.equipUpgradesEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.EquipUpgrades.Enable", true);
        g_PveConfig.buffsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Buffs.Enable", true);
        g_PveConfig.talentsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Talents.Enable", true);
        g_PveConfig.combatDiagnostics = sConfigMgr->GetBoolDefault("Playerbot.Pve.CombatDiagnostics", false);
        g_PveConfig.restUseConsumables = sConfigMgr->GetBoolDefault("Playerbot.Pve.Rest.UseConsumables", false);
        g_PveConfig.travelWalkMaxDistance = sConfigMgr->GetFloatDefault("Playerbot.Pve.Travel.WalkMaxDistance", 900.0f);
        g_PveConfig.travelUseFlightPaths = sConfigMgr->GetBoolDefault("Playerbot.Pve.Travel.UseFlightPaths", true);
        g_PveConfig.auctionBuyEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.AuctionBuy.Enable", false);
        g_PveConfig.auctionSellEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.AuctionSell.Enable", false);
        g_PveConfig.auctionBuyBudgetPct = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionBuy.BudgetPct", 30), 1, 100));
        g_PveConfig.auctionMaxItemLevelsBehind = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionBuy.MaxItemLevelsBehind", 12), 0, 300));
        g_PveConfig.auctionBuyMaxOverpayPct = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionBuy.MaxOverpayPct", 1200)));
        g_PveConfig.auctionBudgetWorthLevels = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.AuctionBuy.BudgetWorthLevels", 15), 1, 200));
        g_PveConfig.grantMounts = sConfigMgr->GetBoolDefault("Playerbot.Pve.GrantMounts", true);
        g_PveConfig.proactiveHuntYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.ProactiveHuntYards", 125.0f));
        g_PveConfig.aggressionResetYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.AggressionResetYards", 200.0f));
        g_PveConfig.aggroBudgetEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.AggroBudget.Enable", false);
        g_PveConfig.aggroBudgetSolo = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.AggroBudget.Solo", 2), 0, 100));
        g_PveConfig.aggroBudgetPerExtraMember = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.AggroBudget.PerExtraMember", 2), 0, 100));
        g_PveConfig.aggroBudgetMaxPerPlayer = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.AggroBudget.MaxPerPlayer", 10), 1, 200));
        g_PveConfig.aggroBudgetPartyRadius = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.AggroBudget.PartyRadiusYards", 80.0f));
        g_PveConfig.auctionLevelsBehindPenalty = std::max(0.0f, sConfigMgr->GetFloatDefault("Playerbot.Pve.Auction.LevelsBehindPenalty", 0.35f));
        g_PveConfig.professionsEnabled = sConfigMgr->GetBoolDefault("Playerbot.Pve.Professions.Enable", false);
        g_PveConfig.relocateEnabled = sConfigMgr->GetBoolDefault("Playerbot.PveGrind.Relocate.Enable", true);
        g_PveConfig.relocateDryWanders = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.PveGrind.Relocate.DryWandersBeforeMove", 5), 2, 100));
        g_PveConfig.stuckRecoveryEnabled =
            sConfigMgr->GetBoolDefault("Playerbot.PveGrind.StuckRecovery.Enable", true);
        g_PveConfig.stuckRecoveryDistanceYards = std::clamp(
            sConfigMgr->GetFloatDefault("Playerbot.PveGrind.StuckRecovery.DistanceYards", 15.0f), 5.0f, 100.0f);
        g_PveConfig.stuckRecoverySeconds = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.PveGrind.StuckRecovery.TimeoutSeconds", 120), 30, 3600));

        g_PveConfig.relocateMaps.clear();
        std::string const mapsCsv = sConfigMgr->GetStringDefault("Playerbot.PveGrind.Relocate.Maps", "0,1");
        std::stringstream mapsStream(mapsCsv);
        std::string token;
        while (std::getline(mapsStream, token, ','))
            if (!token.empty())
                g_PveConfig.relocateMaps.push_back(uint32(std::strtoul(token.c_str(), nullptr, 10)));
        if (g_PveConfig.relocateMaps.empty())
            g_PveConfig.relocateMaps = { 0, 1 };
        std::sort(g_PveConfig.relocateMaps.begin(), g_PveConfig.relocateMaps.end());

        g_PveConfig.rebirthAtMaxLevelPercent = uint32(std::clamp<int32>(
            sConfigMgr->GetIntDefault("Playerbot.Pve.RebirthAtMaxLevel.Percent", 0), 0, 100));
        g_PveConfig.rebirthZoneBanded = sConfigMgr->GetBoolDefault("Playerbot.Pve.Rebirth.ZoneBanded", true);
        g_PveConfig.guildName = sConfigMgr->GetStringDefault("Playerbot.Pve.GuildName", "AI Uprising");
        g_PveConfig.veteranBotCount = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbot.Pve.Rebirth.Veterans", 20)));
        // Read from the population manager's own target so a count means what it says.
        g_PveConfig.populationTarget = uint32(std::max(1, sConfigMgr->GetIntDefault("Playerbot.RandomPopulation.TargetMax", 256)));
        g_PveConfig.declineGroupInvites = sConfigMgr->GetBoolDefault("Playerbot.Pve.DeclineGroupInvites", false);
        g_PveConfig.hardcoreLootChestEntry = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestGameObjectId", 0)));
        g_PveConfig.hardcoreChestDespawnSeconds = uint32(std::max(30, sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestDespawnSeconds", 600)));
        g_PveConfig.zoneGuardiansPerZone = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.ZoneGuardians.PerZone", 0), 0, 10));
        g_PveConfig.drifterCount = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbot.Pve.Drifters.Count", 0)));
        g_PveConfig.drifterZoneDwellSeconds = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.Drifters.ZoneDwellSeconds", 10), 0, 3600));
        g_PveConfig.drifterMaxPerZone = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Playerbot.Pve.Drifters.MaxPerZone", 15)));
        g_PveConfig.drifterPerExtraPerson = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.Drifters.PerExtraPersonInZone", 2), 0, 50));
        g_PveConfig.idleProdAfterMinutes = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.IdleProd.AfterMinutes", 15), 0, 720));
        g_PveConfig.idleProdDropYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Playerbot.Pve.IdleProd.DropYards", 210.0f));
        g_PveConfig.idleProdRetrySeconds = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Playerbot.Pve.IdleProd.RetrySeconds", 120), 15, 3600));
        g_PveConfig.drifterTeleportGold = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.Drifters.TeleportGold", 10), 0, 10000));
        g_PveConfig.proactiveMaxLevelsAbove = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.ProactiveMaxLevelsAbove", 4), 0, 60));
        g_PveConfig.proactiveMaxLevelsBelow = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.ProactiveMaxLevelsBelow", 4), 0, 60));
        g_PveConfig.proactiveBountyStacks = uint32(std::clamp(sConfigMgr->GetIntDefault("Playerbot.Pve.ProactiveBountyStacks", 5), 0, 255));

        // Accounts whose bots are PvP-only: parked in their sanctuary, never
        // touched by any PvE system (no grind, errands, gear, talents, economy),
        // they exist purely for the battleground orchestration.
        g_PveConfig.pvpOnlyAccountIds.clear();
        std::string const pvpOnlyCsv = sConfigMgr->GetStringDefault("Playerbot.Pve.PvpOnlyAccountIds", "");
        std::stringstream pvpOnlyStream(pvpOnlyCsv);
        while (std::getline(pvpOnlyStream, token, ','))
            if (!token.empty())
                g_PveConfig.pvpOnlyAccountIds.push_back(uint32(std::strtoul(token.c_str(), nullptr, 10)));
        std::sort(g_PveConfig.pvpOnlyAccountIds.begin(), g_PveConfig.pvpOnlyAccountIds.end());

        // Counted once, from the accounts themselves, so it cannot drift out of
        // step with them the way a hand-maintained number would.
        g_PveConfig.pvpOnlyBotCount = 0;
        if (!g_PveConfig.pvpOnlyAccountIds.empty())
        {
            std::ostringstream accountList;
            for (size_t index = 0; index < g_PveConfig.pvpOnlyAccountIds.size(); ++index)
                accountList << (index ? "," : "") << g_PveConfig.pvpOnlyAccountIds[index];

            if (QueryResult result = CharacterDatabase.Query(
                ("SELECT COUNT(*) FROM characters WHERE account IN (" + accountList.str() + ")").c_str()))
                g_PveConfig.pvpOnlyBotCount = (*result)[0].GetUInt32();
        }
    }

    bool PveManager::IsPvpOnlyBot(Player const* player)
    {
        if (g_PveConfig.pvpOnlyAccountIds.empty() || !player || !player->GetSession())
            return false;

        return std::binary_search(g_PveConfig.pvpOnlyAccountIds.begin(), g_PveConfig.pvpOnlyAccountIds.end(),
            player->GetSession()->GetAccountId());
    }

    bool PveManager::MayProactivelyEngage(Player const* bot, Player const* human)
    {
        if (!bot || !human || bot == human)
            return false;

        // Consent by entry. Everyone inside a battleground or an arena queued
        // for it, and this rule must never reach in there - a bot that would not
        // pick a target is a bot standing still in the middle of a match.
        // Ask the MAP, not Player::InBattleground - that is `m_bgData.bgInstanceID
        // != 0`, a persisted field restored at login and normally cleared only
        // inside HandleMoveWorldportAck. A bot taken off a battleground map any
        // other way would carry this waiver for the rest of its session.
        Map const* botMap = bot->GetMap();
        Map const* humanMap = human->GetMap();
        if ((botMap && botMap->IsBattlegroundOrArena()) ||
            (humanMap && humanMap->IsBattlegroundOrArena()))
            return true;

        // Consent by acceptance, and only with the person who gave it. A bot
        // duelling somebody may fight THEM; the fallback scan that reaches this
        // would otherwise happily pick a bystander standing closer.
        // In PROGRESS, not merely challenged. DuelInfo::State starts at
        // DUEL_STATE_CHALLENGED (Player.h:266) and the object is built on both
        // players the moment somebody right-clicks Duel - so without the state
        // test, anyone could waive this rule for themselves by offering a duel a
        // bot never accepted. PlayerbotPvpCore.cpp:2510 already asks it this way.
        if (bot->duel && bot->duel->Opponent == human &&
            bot->duel->State == DUEL_STATE_IN_PROGRESS)
            return true;

        // Bot on bot is a different question, answered elsewhere (the open-world
        // no-friendly-fire rule in Object.cpp). Nothing here should stop it.
        if (playerbot::IsManagedRandomBot(human))
            return true;

        // Nobody gets picked on from far above them.
        //
        // The proactive level rule only ever had a CEILING - it refuses a fight
        // the bot would lose, and said nothing whatever about one the bot cannot
        // lose. So a level sixty had every reason to open on somebody levelling
        // through, and being hunted by something you have no answer to is how a
        // new character stops playing. A zone guardian sits at its post's ceiling
        // and so was the worst of it, but it was never only guardians.
        //
        // A bounty is the exception, and consent is why: it is earned by killing
        // people, and the entire point of it is that the realm comes for you
        // whatever you can handle. Read from the registry, which is what every
        // other rule on the realm reads.
        //
        // But it takes a REAL bounty, not a single stack. One stack is one kill,
        // and one kill is not somebody the realm should be sending a level sixty
        // after - a person defending themselves once would have bought a hunter
        // they cannot fight. The waiver is for somebody who has made a habit of
        // it, so it starts at Playerbot.Pve.ProactiveBountyStacks (5).
        //
        // Only about STARTING it. Self-defence never comes through here, so a bot
        // still fights back against anyone who opens on it, at any level.
        if (uint32 const below = g_PveConfig.proactiveMaxLevelsBelow)
            if (Bounty::GetStacks(human) < g_PveConfig.proactiveBountyStacks &&
                uint32(human->GetLevel()) + below < uint32(bot->GetLevel()))
                return false;

        return HumanMayBeHunted(human->GetGUID());
    }

    bool PveManager::AnyPersonWithin(WorldObject const* of, float yards)
    {
        return AnyRealPersonWithin(of, yards);
    }

    bool PveManager::PickGroundSpotInBand(uint32 mapId, uint32 preferredZoneId, float fromX, float fromY,
        float minYards, float maxYards, uint32 seed,
        float& outX, float& outY, float& outZ, uint32& outZoneId)
    {
        if (minYards > maxYards)
            std::swap(minYards, maxYards);

        float const minSq = minYards * minYards;
        float const maxSq = maxYards * maxYards;

        std::lock_guard<std::mutex> guard(g_GrindSpotLock);
        if (!g_GrindSpotsBuilt)
            return false;

        // The zone first, the rest of the map only if the zone cannot answer.
        // A rendezvous in the zone you are already standing in is the point;
        // crossing a border to reach it is a consolation prize.
        auto collect = [&](std::vector<GrindSpot> const& spots, std::vector<GrindSpot const*>& out)
        {
            for (GrindSpot const& spot : spots)
            {
                if (spot.mapId != mapId)
                    continue;

                float const dx = spot.x - fromX;
                float const dy = spot.y - fromY;
                float const distSq = dx * dx + dy * dy;
                if (distSq < minSq || distSq > maxSq)
                    continue;

                out.push_back(&spot);
            }
        };

        std::vector<GrindSpot const*> candidates;
        if (preferredZoneId)
            if (auto itr = g_GrindSpotsByZone.find(preferredZoneId); itr != g_GrindSpotsByZone.end())
                collect(itr->second, candidates);

        if (candidates.empty())
            for (auto const& [zoneId, spots] : g_GrindSpotsByZone)
                collect(spots, candidates);

        if (candidates.empty())
            return false;

        // Sorted before indexing: the map's iteration order is not stable across
        // boots, so without this the "same seed, same place" promise would only
        // hold until the next restart.
        std::sort(candidates.begin(), candidates.end(), [](GrindSpot const* left, GrindSpot const* right)
        {
            if (left->x != right->x)
                return left->x < right->x;
            if (left->y != right->y)
                return left->y < right->y;
            return left->z < right->z;
        });

        GrindSpot const* pick = candidates[seed % candidates.size()];
        outX = pick->x;
        outY = pick->y;
        outZ = pick->z;
        outZoneId = pick->zoneId;
        return true;
    }

    // World thread. Walks the managed roster once and copies out the few fields
    // the GM stats addon draws, so the feed never has to hold the state lock or
    // touch a Player while the map thread is running.
    void PveManager::CollectBotStats(std::vector<PveManager::BotStatsRow>& out)
    {
        out.clear();

        PveTimePoint const now = PveClock::now();
        std::vector<uint64> guids;
        {
            std::lock_guard<std::mutex> guard(playerbot::SharedBotStateStructureLock());
            guids.reserve(g_PveBotStateByGuid.size());
            for (auto const& entry : g_PveBotStateByGuid)
                guids.push_back(entry.first);
        }

        out.reserve(guids.size());
        for (uint64 rawGuid : guids)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(rawGuid));
            if (!bot || !bot->IsInWorld() || !playerbot::IsManagedRandomBot(bot))
                continue;

            BotStatsRow row;
            row.Name = bot->GetName();
            row.ZoneId = bot->GetZoneId();
            row.MapId = bot->GetMapId();
            row.MoneyCopper = bot->GetMoney();
            row.Level = bot->GetLevel();
            row.Class = bot->GetClass();
            row.Aggression = GetBotAggression(bot);
            row.HealthPct = uint8(std::min(100.0f, std::max(0.0f, bot->GetHealthPct())));
            if (bot->GetMaxPower(POWER_MANA) > 0)
                row.PowerPct = uint8(std::min(100.0f, std::max(0.0f, bot->GetPowerPct(POWER_MANA))));

            row.Spec = uint8(EquipProfileIndex(bot));
            row.DisplayId = bot->GetNativeDisplayId();

            // What is actually worn. Mean item level says whether a bot is
            // equipped or running around in field kit, and the green-or-better
            // count is the same thing from the angle a player sees it: how much
            // it is carrying that would drop.
            {
                uint32 sum = 0, worn = 0, greenPlus = 0;
                for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        if (ItemTemplate const* proto = item->GetTemplate())
                        {
                            sum += proto->ItemLevel;
                            ++worn;

                            // ARTIFACT is repurposed on this realm as the FLOOR
                            // tier - the red field-kit gear handed out to replace
                            // what a death took. It sorts below green however
                            // Blizzard's enum orders it, so a plain >= UNCOMMON
                            // test counted a bot in full kit as fully geared.
                            if (proto->Quality >= ITEM_QUALITY_UNCOMMON &&
                                proto->Quality != ITEM_QUALITY_ARTIFACT)
                                ++greenPlus;
                        }
                row.ItemLevel = worn ? uint16(sum / worn) : 0;
                row.WornCount = uint8(worn);
                row.GreenPlus = uint8(greenPlus);
            }

            row.InCombat = bot->IsInCombat();
            row.Dead = !bot->IsAlive();
            row.PvpOnly = PveManager::IsPvpOnlyBot(bot);

            // Same order of precedence the whisper diagnostic uses, so the two
            // can never disagree about what a bot is: a post beats everything, a
            // companion is whatever it is doing for its master, and drifter is
            // asked before "local" because a drifter HAS a home zone - a
            // borrowed one, reassigned every time its person moves.
            if (GetGuardianZoneId(rawGuid))
                row.Role = 1;
            else if (row.PvpOnly)
                row.Role = 3;
            else if (IsLocalVeteranGuid(rawGuid))
                row.Role = 2;
            else if (IsDrifter(rawGuid))
                row.Role = 4;
            else
                row.Role = 0;

            {
                std::lock_guard<std::mutex> guard(playerbot::SharedBotStateStructureLock());
                auto const itr = g_PveBotStateByGuid.find(rawGuid);
                if (itr != g_PveBotStateByGuid.end())
                {
                    PveBotState const& state = itr->second;
                    row.Travelling = state.journeyActive;

                    // Asked here rather than above because the master link is the
                    // one input that lives in the state, not in a registry. It
                    // wins over everything except a guardian post: whatever this
                    // bot normally is, right now it is somebody's companion.
                    if (!state.masterGuid.IsEmpty() && row.Role != 1)
                        row.Role = 5;

                    if (state.timidUntil > now)
                    {
                        auto const left = std::chrono::duration_cast<std::chrono::seconds>(
                            state.timidUntil - now).count();
                        row.TimidSeconds = uint16(std::min<int64>(left, 65535));
                    }
                }
            }

            out.push_back(row);
        }
    }

    PveConfig const& PveManager::GetConfig()
    {
        return g_PveConfig;
    }

    void ResetManagedBotToLevelOne(Player* bot);
    void ResetManagedBotToZoneBand(Player* bot, uint32 zoneId, uint8 bottomLevel);
    bool IsVeteranBot(Player const* bot);
    bool GetZoneLevelBand(uint32 zoneId, uint8& bottom, uint8& top);

    // Keep the drifter roster in step with who is actually online.
    //
    // Sticky on purpose: an assignment that is still valid is kept, and only
    // vacancies are filled. A drifter that flickered in and out of the role
    // every second would be re-levelled every second with it.
    // Food, and water for the classes that drink. Quantities and skip rules are
    // the supply run's: top up to 20 units when below 10, never for a class that
    // conjures its own. A drifter that ports ends up in the same state as one
    // that walked to a vendor.
    void GrantTravelRations(Player* bot)
    {
        BuildRationPoolOnce();
        uint8 const level = bot->GetLevel();

        if (CountConsumableUnits(bot, false) < 10 && !ConjureSpellId(bot, false))
            if (uint32 const food = BestRationForLevel(g_RationFood, level))
                bot->AddItem(food, 20);

        if (UsesMana(bot) && CountConsumableUnits(bot, true) < 10 && !ConjureSpellId(bot, true))
            if (uint32 const drink = BestRationForLevel(g_RationDrink, level))
                bot->AddItem(drink, 20);
    }

    void UpdateDrifterAssignments()
    {
        uint32 const target = g_PveConfig.drifterCount;
        uint32 const nowMs = GameTime::GetGameTimeMS();

        // Where each real person has settled, and for how long.
        static std::unordered_map<uint64, std::pair<uint32, uint32>> s_humanZoneSince;
        std::vector<std::pair<uint64, uint32>> humans;   // human guid -> zone
        // The largest bounty standing in each zone, gathered on the same pass.
        // One person on a spree pulls extra company, and the company is for the
        // whole zone.
        std::unordered_map<uint32, uint32> zoneBounty;
        // And how many people are in it, which raises the ceiling on its own.
        std::unordered_map<uint32, uint32> zonePeople;
        std::unordered_set<uint64> online;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* human = pair.second;
            if (!human || !human->IsInWorld() || playerbot::IsManagedRandomBot(human))
                continue;

            uint64 const humanGuid = human->GetGUID().GetRawValue();
            online.insert(humanGuid);
            auto& settled = s_humanZoneSince[humanGuid];

            // Somebody on a flight path is not IN the zones they pass over, they
            // are above them. Freezing their settled zone for the duration means
            // a cross-continent flight does not drag the retinue through six
            // zones on the way - and, just as importantly, does not release them
            // either: the person is still online and still theirs. The dwell
            // timer then starts from wherever they actually land.
            if (!human->IsInFlight())
            {
                uint32 const currentZone = human->GetZoneId();
                if (settled.first != currentZone)
                    settled = { currentZone, nowMs };
            }
            uint32 const zoneId = settled.first;

            // Somewhere a local bot can actually be delivered to. Capitals,
            // instances, battlegrounds and the 55+ veteran zones are not in
            // g_RebirthZones at all, so this single test covers all of them -
            // and a person standing in one simply keeps the drifters they
            // already have, wherever those were last sent.
            if (!std::binary_search(g_RebirthZones.begin(), g_RebirthZones.end(), zoneId))
                continue;

            // And nowhere people cannot fight each other. A drifter in a
            // protected starter zone adds nothing: it cannot be attacked and
            // cannot attack, so it is just a stranger farming the same boars.
            // Asking the FFA ruleset rather than keeping a second zone list of
            // our own means the two can never drift apart.
            if (!BarracksHardcore::IsOpenWorldPvpZone(zoneId))
                continue;

            if (nowMs - settled.second < g_PveConfig.drifterZoneDwellSeconds * 1000)
                continue;

            // And only somebody the zone is actually FOR. A level 60 in the
            // Barrens is not somewhere a drifter can be sent: it is re-levelled
            // into the destination zone's band on arrival, so the only two
            // outcomes available are a level 60 loitering in a 10-25 zone, or a
            // level 20 standing next to a level 60 it cannot fight. Dropping the
            // person from the set is the only coherent answer - their share goes
            // to the people who do fit rather than idling.
            //
            // A zone with no band entry is not evidence of anything, so it is
            // allowed through: g_RebirthZones has already limited this to zones
            // a local bot can be delivered to.
            uint8 bandBottom = 0;
            uint8 bandTop = 0;
            if (GetZoneLevelBand(zoneId, bandBottom, bandTop))
            {
                uint8 const humanLevel = human->GetLevel();
                if (humanLevel < bandBottom || humanLevel > bandTop)
                    continue;
            }

            humans.push_back({ humanGuid, zoneId });

            uint32& worstInZone = zoneBounty[zoneId];
            worstInZone = std::max(worstInZone, Bounty::GetStacks(human));
            ++zonePeople[zoneId];
        }
        for (auto itr = s_humanZoneSince.begin(); itr != s_humanZoneSince.end(); )
            itr = online.count(itr->first) ? std::next(itr) : s_humanZoneSince.erase(itr);

        if (!target || humans.empty())
        {
            std::lock_guard<std::mutex> guard(g_DrifterLock);
            if (!g_DrifterZoneByBot.empty())
            {
                TC_LOG_INFO("playerbots.pve", "Releasing {} drifters: nobody left to follow.",
                    uint32(g_DrifterZoneByBot.size()));
                g_DrifterZoneByBot.clear();
                g_DrifterHumanByBot.clear();
            }
            return;
        }

        std::sort(humans.begin(), humans.end());
        std::unordered_map<uint64, uint32> humanZone;
        for (auto const& [guid, zoneId] : humans)
            humanZone[guid] = zoneId;

        // Everything that needs g_GuardianLock or g_LocalZoneLock happens HERE,
        // before g_DrifterLock is taken: see the lock-order note on the table.
        std::unordered_map<uint64, uint32> localHomeByBot;
        std::unordered_map<uint64, uint32> botZoneNow;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* bot = pair.second;
            if (!bot || !bot->IsInWorld() || !playerbot::IsManagedRandomBot(bot))
                continue;
            // A band home of zero already means guardian, veteran or PvP-only,
            // which are exactly the roles that must never be drafted.
            if (uint32 const home = GetLocalHomeZoneId(bot))
            {
                localHomeByBot[bot->GetGUID().GetRawValue()] = home;
                botZoneNow[bot->GetGUID().GetRawValue()] = bot->GetZoneId();
            }
        }

        // Even shares, with the remainder spread over the first few people
        // rather than all landing on one.
        size_t const humanCount = humans.size();
        uint32 const baseShare = target / uint32(humanCount);
        uint32 const remainder = target % uint32(humanCount);

        // Releasable on purpose: g_PvePendingLock is taken further down and must
        // never be taken while holding this one.
        std::unique_lock<std::mutex> guard(g_DrifterLock);

        // Drop assignments whose bot or person is gone, and count what survives.
        //
        // Anything over the zone's ceiling is released here rather than merely
        // not topped up, so lowering the cap takes effect on the next pass
        // instead of waiting for drifters to churn out on their own.
        // The ceiling: the base figure, plus room for each person past the
        // first, plus whatever the biggest bounty in the zone has earned.
        auto const capForZone = [&zoneBounty, &zonePeople](uint32 zoneId) -> uint32
        {
            uint32 const base = g_PveConfig.drifterMaxPerZone;
            if (!base)
                return 0;   // uncapped stays uncapped

            uint32 cap = base;

            auto const people = zonePeople.find(zoneId);
            if (people != zonePeople.end() && people->second > 1)
                cap += g_PveConfig.drifterPerExtraPerson * (people->second - 1);

            auto const bounty = zoneBounty.find(zoneId);
            if (bounty != zoneBounty.end())
                cap += Bounty::DrifterZoneBonus(bounty->second);

            return cap;
        };
        std::unordered_map<uint32, uint32> inZone;
        uint32 released = 0;
        std::unordered_map<uint64, uint32> heldBy;
        for (auto itr = g_DrifterHumanByBot.begin(); itr != g_DrifterHumanByBot.end(); )
        {
            bool const botOk = localHomeByBot.count(itr->first) != 0;
            auto zoneItr = humanZone.find(itr->second);
            if (!botOk || zoneItr == humanZone.end())
            {
                g_DrifterZoneByBot.erase(itr->first);
                itr = g_DrifterHumanByBot.erase(itr);
                continue;
            }
            uint32 const zoneCap = capForZone(zoneItr->second);
            if (zoneCap && inZone[zoneItr->second] >= zoneCap)
            {
                g_DrifterZoneByBot.erase(itr->first);
                itr = g_DrifterHumanByBot.erase(itr);
                ++released;
                continue;
            }

            ++inZone[zoneItr->second];
            ++heldBy[itr->second];
            g_DrifterZoneByBot[itr->first] = zoneItr->second;
            ++itr;
        }

        // Candidates bucketed by band home, so a draft takes them evenly out of
        // the local population instead of emptying one zone.
        std::map<uint32, std::vector<uint64>> byHome;
        for (auto const& [botGuid, home] : localHomeByBot)
            if (!g_DrifterHumanByBot.count(botGuid))
                byHome[home].push_back(botGuid);
        for (auto& [home, guids] : byHome)
            std::sort(guids.begin(), guids.end());

        auto drawCandidate = [&byHome]() -> uint64
        {
            // Always from the home zone with the most to spare.
            auto best = byHome.end();
            for (auto itr = byHome.begin(); itr != byHome.end(); ++itr)
                if (!itr->second.empty() && (best == byHome.end() || itr->second.size() > best->second.size()))
                    best = itr;
            if (best == byHome.end())
                return 0;
            uint64 const picked = best->second.back();
            best->second.pop_back();
            return picked;
        };

        uint32 assigned = 0;
        for (size_t index = 0; index < humanCount; ++index)
        {
            uint64 const humanGuid = humans[index].first;
            uint32 const zoneId = humans[index].second;
            uint32 const share = baseShare + (index < remainder ? 1u : 0u);
            for (uint32 held = heldBy[humanGuid]; held < share; ++held)
            {
                // The zone is full. Their share is not redistributed to anyone
                // else in the same zone - that would just refill it by another
                // route - so the drifters simply stay where they were.
                uint32 const zoneCap = capForZone(zoneId);
                if (zoneCap && inZone[zoneId] >= zoneCap)
                    break;

                uint64 const botGuid = drawCandidate();
                if (!botGuid)
                    break;
                g_DrifterHumanByBot[botGuid] = humanGuid;
                g_DrifterZoneByBot[botGuid] = zoneId;
                ++inZone[zoneId];
                ++assigned;
            }
        }

        if (assigned || released)
            TC_LOG_INFO("playerbots.pve", "Drifters: {} newly assigned, {} released over the {}/zone cap, {} following {} people.",
                assigned, released, g_PveConfig.drifterMaxPerZone,
                uint32(g_DrifterHumanByBot.size()), uint32(humanCount));

        // Whoever is standing somewhere other than where they now belong, and
        // whoever has since arrived where they were sent.
        //
        // The auction sweep deliberately waits for arrival. Queued at the moment
        // of departure it would run while the bot was still standing in the old
        // zone at its old level, and it would shop for the character it is about
        // to stop being.
        static std::unordered_set<uint64> s_awaitingArrival;
        std::vector<uint64> misplaced;
        std::vector<uint64> arrived;
        for (auto const& [botGuid, zoneId] : g_DrifterZoneByBot)
        {
            auto zoneItr = botZoneNow.find(botGuid);
            if (zoneItr == botZoneNow.end())
                continue;
            if (zoneItr->second != zoneId)
                misplaced.push_back(botGuid);
            else if (s_awaitingArrival.erase(botGuid))
                arrived.push_back(botGuid);
        }
        guard.unlock();

        // Forget anyone who stopped being a drifter while in transit.
        for (auto itr = s_awaitingArrival.begin(); itr != s_awaitingArrival.end(); )
            itr = botZoneNow.count(*itr) ? std::next(itr) : s_awaitingArrival.erase(itr);

        // Nothing else notices that a HOME CHANGED - every existing trigger fires
        // on a level that no longer fits a band, on death, or after five fruitless
        // wander cycles. Without this nudge a companion would drift toward its
        // person minutes late and look like it was ignoring them.
        //
        // Queueing is all that happens here. The executor re-reads the home zone,
        // defers to the band-reset pass when the level does not fit, and guards
        // the teleport itself - so a bot in combat, mid-journey or mid-teleport is
        // simply skipped and picked up on a later pass.
        if (!misplaced.empty())
        {
            std::lock_guard<std::mutex> pendingGuard(g_PvePendingLock);
            for (uint64 botGuid : misplaced)
            {
                g_PendingGrindRelocations.insert(botGuid);
                s_awaitingArrival.insert(botGuid);
            }
        }

        // Landed and re-levelled: pay them, THEN let them re-kit for the band
        // they are now in. The order matters - the sweep deliberately waits for
        // arrival so the bot shops for the character it is becoming, and a bot
        // that arrives broke sweeps the auction house and buys nothing.
        // g_PendingAuctionShopping is world-thread only, which is where this runs.
        uint32 paid = 0;
        for (uint64 botGuid : arrived)
        {
            if (g_PveConfig.drifterTeleportGold)
            {
                // Not being findable is ordinary here: a drifter can log out or be
                // released between the arrival check and this loop. It simply goes
                // unpaid this time and is paid on its next landing.
                if (Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botGuid)))
                {
                    bot->ModifyMoney(int64(g_PveConfig.drifterTeleportGold) * int64(GOLD));
                    // Rations for the level it has just become. Whatever it was
                    // carrying is the wrong tier now - it was re-levelled into this
                    // zone's band on arrival - and a bot that cannot eat between
                    // fights spends the rest of its life at a fraction of its health.
                    GrantTravelRations(bot);
                    ++paid;
                }
            }

            g_PendingAuctionShopping.insert(botGuid);
        }
        if (!arrived.empty())
            TC_LOG_INFO("playerbots.pve", "{} drifters arrived; paid {} of them {}g and queued an auction sweep for each.",
                uint32(arrived.size()), paid, g_PveConfig.drifterTeleportGold);
    }

    void PveManager::OnWorldUpdate(uint32 /*diffMs*/)
    {
        if (!g_PveConfig.enabled)
            return;

        static uint32 lastPassMs = 0;
        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (lastPassMs && nowMs < lastPassMs + 1000)
            return;
        lastPassMs = nowMs;

        // Build relocation/suitability data before a bot asks whether its current
        // zone fits. Previously this cache was first built by the relocation
        // executor, but a relocation was only queued AFTER the suitability test;
        // an empty cache therefore made every zone look suitable forever.
        BuildGrindSpotCacheOnce();

        // Build stable class-balanced home-zone assignments from the complete
        // configured bot roster. This retries until population configuration is
        // available, then becomes a no-op for the rest of the uptime.
        BuildLocalZoneAssignmentsOnce();

        // Then point the companions at whoever is online. This must run after
        // the band roster exists, because a bot with no band home is not a
        // candidate and that is how guardians and veterans are excluded.
        UpdateDrifterAssignments();

        // Hand the fleet a fresh second's worth of navmesh queries. Anything the
        // bots did not spend is deliberately not carried over: the point is a
        // ceiling on per-second map-thread cost, not a total.
        g_PathBudgetTokens.store(g_PveConfig.pathBudgetPerSecond, std::memory_order_relaxed);

        // Refresh where the real people are, so guardians never have to search.
        // One pass over the connected players per second, on the world thread,
        // instead of a grid sweep per guardian on the map thread.
        {
            // How many bots are locked onto each person right now, derived from
            // the bots' own targets rather than from a registry of claims. A bot
            // that dies, logs out or picks something else stops counting the
            // moment it does, so there is nothing to expire and nothing to leak,
            // and a long fight holds its slot for exactly as long as it lasts.
            // Also who is FIGHTING each person, and who is free to be sent at
            // one. Both come from the same pass, and both are field reads: the
            // world thread must not walk a Unit's combat references.
            std::unordered_map<uint64, uint32> botsOnTarget;
            std::unordered_set<uint64> humansFightingABot;
            std::vector<Player*> proddableBots;
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* bot = pair.second;
                if (!bot || !bot->IsInWorld() || !playerbot::IsManagedRandomBot(bot))
                    continue;

                ObjectGuid const target = bot->GetTarget();
                if (target)
                {
                    ++botsOnTarget[target.GetRawValue()];
                    if (bot->IsInCombat())
                        humansFightingABot.insert(target.GetRawValue());
                }

                // A bot worth sending: free and whole. Deliberately only plain
                // field reads - whether it is somebody's COMPANION lives behind
                // the process-wide state mutex, which the map thread also takes,
                // and asking once per bot per second would put hundreds of
                // acquisitions a second in front of live bot ticks. That
                // question is asked in ProdForgottenPlayers instead, of the one
                // bot about to be sent.
                //
                // InBattlegroundQueue matters as much as InBattleground: a bot
                // already inside one was never a candidate, but one WAITING for
                // a pop was, and teleporting it across the world is exactly how
                // it misses that pop. Nothing should pull anybody out of a queue,
                // so the rule lives here rather than in any one caller.
                //
                // NOT gated on idleProdAfterMinutes: this pool feeds the bounty
                // dispatch too, and tying it to an unrelated feature's switch
                // made the top bounty rungs silently candidate-less whenever the
                // idle prod was off.
                if (bot->IsAlive() && !bot->IsInCombat() &&
                    !bot->InBattleground() && !bot->InArena() &&
                    !bot->InBattlegroundQueue() &&
                    !bot->IsBeingTeleportedFar() && !bot->IsBeingTeleportedNear())
                    proddableBots.push_back(bot);
            }

            std::vector<HumanSpot> spots;
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* human = pair.second;
                if (!human || !human->IsInWorld() || !human->IsAlive())
                    continue;

                if (human->IsGameMaster() || playerbot::IsManagedRandomBot(human))
                    continue;

                // Somebody standing in a sanctuary or their own capital cannot be
                // fought there, so they are not a destination - a bot teleported
                // into Orgrimmar to reach a Horde player can do nothing but be cut
                // down by the guards. IsInNoPvPArea is faction-aware, so an
                // ALLIANCE player standing in Orgrimmar is still fair game.
                if (human->pvpInfo.IsInNoPvPArea)
                    continue;

                uint32 const bounty = Bounty::GetStacks(human);

                int32 slotsFree = std::numeric_limits<int32>::max();
                // A big enough bounty suspends the budget outright - the whole
                // point of the escalation is that it eventually stops being a
                // fair fight - and below that threshold it simply buys extra
                // slots in proportion.
                if (g_PveConfig.aggroBudgetEnabled && !Bounty::IgnoresAggroBudget(bounty))
                {
                    auto const taken = botsOnTarget.find(human->GetGUID().GetRawValue());
                    slotsFree = AggroBudgetFor(human, g_PveConfig) + Bounty::AggroSlotBonus(bounty) -
                        int32(taken != botsOnTarget.end() ? taken->second : 0u);
                }

                // War Mode, or a bounty. Nobody else is hunted.
                //
                // The spot is still PUBLISHED for someone who is neither - it is
                // how a timid bot knows where not to go, and how the zone knows
                // it has people in it. Only the paths that send a bot AT them
                // read this flag.
                bool const huntable = bounty > 0 || BarracksHardcore::IsWarModeOptedIn(human);

                spots.push_back({ human->GetGUID(), human->GetMapId(), human->GetZoneId(), human->GetLevel(),
                    human->GetPositionX(), human->GetPositionY(), human->GetPositionZ(), slotsFree, bounty,
                    huntable });
            }

            // Nobody left alone. Walked here rather than in the loop above
            // because it needs the finished human list AND the bot list, and
            // because a prod is a rare event that should not sit inside a
            // per-player body that runs every second.
            if (g_PveConfig.idleProdAfterMinutes)
                ProdForgottenPlayers(spots, humansFightingABot, proddableBots);

            // And the bounty's own clock, which does not care how long any bot
            // has been idle.
            // Braced, and the three below it de-indented to say what they have
            // always done: only the hunt itself is behind the switch. The other
            // three ran unconditionally regardless of how they were laid out, and
            // that is right - CallForHelp no-ops on its own when every bounty is
            // zero, and both release passes MUST run with the system switched off
            // or turning it off would strand whoever was already out.
            if (Bounty::Enabled())
            {
                HuntTheBountied(spots, proddableBots);
            }

            // And the fleet answers its own outmatched.
            CallForHelpAgainstBounties(spots, proddableBots);

            // And send the conscripts home once their bounty is done.
            ReturnLeviedBots();

            // And the hunters who were never re-levelled, who only need
            // putting back where they were standing.
            ReturnDeployedHunters();

            std::lock_guard<std::mutex> guard(g_HumanSpotLock);
            g_HumanSpots.swap(spots);
        }

        // Bot cost telemetry. The map-update thread is shared with human combat,
        // so when a player reports hit-to-damage delay this is the line that says
        // whether the bots are responsible.
        {
            static uint32 lastReportMs = 0;
            if (!lastReportMs)
                lastReportMs = nowMs;
            else if (nowMs >= lastReportMs + 60000)
            {
                uint64 const ran = g_PathQueriesRun.exchange(0, std::memory_order_relaxed);
                uint64 const denied = g_PathQueriesDenied.exchange(0, std::memory_order_relaxed);
                uint32 const elapsedSec = std::max(1u, (nowMs - lastReportMs) / 1000);
                lastReportMs = nowMs;
                TC_LOG_INFO("playerbots.pve", "PvE cost: {} navmesh paths/sec run, {} /sec deferred (budget {}).",
                    ran / elapsedSec, denied / elapsedSec, g_PveConfig.pathBudgetPerSecond);
            }
        }

        ProcessPendingGroupInviteAccepts();
        ProcessPendingSummons();
        // Chests nobody is watching are emptied where the bot stands.
        // Before the executor, so a chest queued this tick is taken this tick.
        QueueUnwatchedChests();
        ProcessPendingLootExecutions();
        ProcessPendingMailCollections();
        if (g_PveConfig.auctionBuyEnabled)
            ProcessPendingAuctionShopping();
        if (g_PveConfig.auctionSellEnabled)
            ProcessPendingAuctionSales();
        ProcessPendingSupplyRuns();
        // Class quests are auto-rewarded on the bot tick; never execute the legacy
        // cross-world class-quest travel queue.
        // Guardian approaches drain their own queue. Ordinary grind relocation
        // and stuck recovery share the validated grind-spot landing executor but
        // retain independent switches inside it.
        ProcessPendingGuardianTeleports();
        if (g_PveConfig.relocateEnabled || g_PveConfig.stuckRecoveryEnabled)
            ProcessPendingGrindRelocations();

        // Guild joins. The first bot through creates the guild and becomes its
        // master; every bot after that is simply added.
        if (!g_PveConfig.guildName.empty())
        {
            std::unordered_set<uint64> drained;
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                drained.swap(g_PendingGuildJoins);
            }

            for (uint64 botRawGuid : drained)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid));
                if (!bot || !bot->IsInWorld() || !playerbot::IsManagedRandomBot(bot) || bot->GetGuildId())
                    continue;

                Guild* guild = sGuildMgr->GetGuildByName(g_PveConfig.guildName);
                if (!guild)
                {
                    // Create it around this bot. Guild::Create adds the leader as
                    // guildmaster itself, so there is nothing further to do for it.
                    std::unique_ptr<Guild> created = std::make_unique<Guild>();
                    if (!created->Create(bot, g_PveConfig.guildName))
                        continue;

                    sGuildMgr->AddGuild(created.release());
                    TC_LOG_INFO("playerbots.pve", "Created guild '{}' with {} as guild master.",
                        g_PveConfig.guildName, bot->GetName());
                    continue;
                }

                CharacterDatabaseTransaction trans(nullptr);
                if (guild->AddMember(trans, bot->GetGUID()))
                    TC_LOG_INFO("playerbots.pve", "Bot {} joined guild '{}'.", bot->GetName(), g_PveConfig.guildName);
            }
        }

        // Guardians found sitting above their post's ceiling by the map-thread
        // tick, which queues rather than acting because this reset teleports and
        // wipes the bot state the tick is holding a reference to.
        {
            std::unordered_map<uint64, uint8> drained;
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                drained.swap(g_PendingGuardianDemotions);
            }
            for (auto const& [botRawGuid, targetLevel] : drained)
                if (Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid)))
                    // Same guard as the rebirth drain below, for the same reason:
                    // teleporting a bot that is already mid-teleport walks into
                    // RemoveFromGrid's IsInGrid assert and takes the server down.
                    // A skipped guardian is requeued by the next tick.
                    if (bot->IsInWorld() && playerbot::IsManagedRandomBot(bot) &&
                        !bot->IsBeingTeleportedFar() && !bot->IsBeingTeleportedNear() &&
                        bot->GetLevel() > targetLevel)
                    {
                        if (uint32 const zoneId = GetGuardianZoneId(botRawGuid))
                        {
                            TC_LOG_INFO("playerbots.pve", "Guardian {} was level {} at a post capped {}; resetting it to the post.",
                                bot->GetName(), uint32(bot->GetLevel()), uint32(targetLevel));
                            ResetManagedBotToZoneBand(bot, zoneId, targetLevel);
                        }
                    }
        }

        // Guardians that ran dry with no vendor they are allowed to reach.
        {
            std::unordered_set<uint64> drained;
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                drained.swap(g_PendingGuardianRations);
            }
            for (uint64 botRawGuid : drained)
                if (Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid)))
                    if (bot->IsInWorld() && playerbot::IsManagedRandomBot(bot))
                        GrantTravelRations(bot);
            if (!drained.empty())
                TC_LOG_INFO("playerbots.pve", "Resupplied {} guardians that had run out where they stand.",
                    uint32(drained.size()));
        }

        // Rebirth-flagged bots that just hit the level cap.
        {
            std::unordered_set<uint64> drained;
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                drained.swap(g_PendingRebirths);
            }
            for (uint64 botRawGuid : drained)
                if (Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(botRawGuid)))
                    // Not while a teleport is already in flight. The guardian and
                    // relocation passes above this one both teleport, and a bot
                    // caught by one of them is out of its grid until the teleport
                    // lands - so teleporting it again here walks into
                    // RemoveFromGrid's IsInGrid assert and takes the server down.
                    // The rebirth is simply skipped; the periodic check requeues it
                    // within the minute.
                    if (bot->IsInWorld() && playerbot::IsManagedRandomBot(bot) &&
                        !bot->IsBeingTeleportedFar() && !bot->IsBeingTeleportedNear())
                    {
                        // The pools whose level is their own are never reborn, and
                        // this is where saying so matters most: the fallback at the
                        // bottom of this block is ResetManagedBotToLevelOne, and it
                        // is reached when a bot has no rebirth zone - which is the
                        // very definition of a veteran, a PvP-only bot or a
                        // guardian. Every path that queues one currently screens
                        // them out, so this changes nothing today; it means the day
                        // one does slip through, it is skipped rather than wiped
                        // back to a level 1 with no gear, no bags and no bank.
                        if (KeepsItsOwnLevel(bot))
                            continue;

                        // Recomputed rather than carried on the queue: the mapping
                        // is deterministic, so the answer cannot drift between the
                        // flag and the reset.
                        uint8 bottom = 0;
                        uint8 top = 0;
                        uint32 const zoneId = g_PveConfig.rebirthZoneBanded ? GetRebirthZoneId(bot) : 0u;
                        if (zoneId && GetZoneLevelBand(zoneId, bottom, top))
                        {
                            // A local bot starts at the floor and climbs, which is
                            // what keeps every level of its zone occupied over time.
                            // A drifter never gets to climb - it is re-levelled again
                            // the moment its person moves - so starting it at the
                            // floor would stand every drifter in the zone at exactly
                            // the same level and make the whole retinue read as one
                            // cohort that arrived together. Scatter them across the
                            // band instead, which is what a zone full of strangers
                            // actually looks like.
                            //
                            // GetZoneLevelBand guarantees top > bottom, and the
                            // band-fit test elsewhere treats [bottom, top - 1] as the
                            // levels that belong here, so this range matches it.
                            bool const drifter = IsDrifter(botRawGuid);
                            uint8 const level = drifter
                                ? uint8(urand(bottom, top - 1))
                                : bottom;
                            ResetManagedBotToZoneBand(bot, zoneId, level);

                            // The reset wipes this bot's state, which restarts its
                            // aggression clock from zero. A batch of drifters that
                            // ported together would then all come looking for a
                            // fight on the very same tick - a zone that is quiet for
                            // an hour and then rushes you all at once. Scatter the
                            // clock across the whole aggression window instead, so
                            // some arrive already hungry and others are content for
                            // another hour.
                            if (drifter && g_PveConfig.aggressionMaxMinutes)
                            {
                                PveBotState& fresh = playerbot::LockedGetOrCreate(g_PveBotStateByGuid, botRawGuid);
                                fresh.lastPlayerFightAt = PveClock::now() -
                                    std::chrono::minutes(urand(0, g_PveConfig.aggressionMaxMinutes));
                            }
                        }
                        else
                            ResetManagedBotToLevelOne(bot);
                    }
        }
    }

    void PveManager::OnPlayerLifecycleTick(Player* player)
    {
        PveConfig const& cfg = g_PveConfig;
        if (!cfg.enabled || !player || !player->IsInWorld())
            return;

        if (!playerbot::IsManagedRandomBot(player))
            return;

        // PvP-only bots live outside the PvE world entirely.
        if (IsPvpOnlyBot(player))
        {
            // Emptying a mailbox is housekeeping, not a PvE behaviour, and a
            // PvP-only bot still needs it done. Everything below this line is
            // skipped for them, so the day an account was moved onto the
            // PvP-only list its mail stopped being touched at all: twenty-four
            // level 60s were holding 3,411 mails between them, undeliverable
            // ever since, with the bots online the whole time.
            //
            // The gold in it is discarded rather than banked (see the collector)
            // - these bots have no use for money - but the rows still have to go
            // somewhere, and nothing else will ever clear them.
            // They hold no coin. Enforced every tick rather than zeroed once,
            // because a character's money lives in memory while it is online and
            // the next periodic save writes it straight back over anything set
            // in the database underneath a running server.
            if (player->GetMoney())
                player->SetMoney(0);

            PveBotState& pvpState = LockedGetOrCreate(g_PveBotStateByGuid, player->GetGUID().GetRawValue());
            PveTimePoint const mailNow = PveClock::now();
            if (mailNow >= pvpState.nextMailCheckAt)
            {
                pvpState.nextMailCheckAt = mailNow + std::chrono::minutes(3);
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingMailCollections.insert(player->GetGUID().GetRawValue());
            }
            return;
        }

        ObjectGuid const guid = player->GetGUID();
        uint64 const rawGuid = guid.GetRawValue();

        if (player->InBattleground() || player->duel)
        {
            // Clear only the PvE bookkeeping: the PvP engine owns the bot's
            // target and combat state from here, and an AttackStop/SetTarget
            // would clobber a target it may already have acquired.
            PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);
            ResetStuckWatchdog(state);
            if (state.engaged)
            {
                playerbot::PvpCore::SetPveCombatEngagement(player->GetGUID(), false);
                state.engaged = false;
            }
            return;
        }

        if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
            return;

        PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);
        if (player->IsInFlight())
        {
            // A bot flagged as flying with no flight generator behind it is not
            // flying - it is stranded on the ground still wearing the taxi mount,
            // and nothing else will ever notice: this tick returns early on
            // IsInFlight, and the stuck watchdog excludes IsInFlight too.
            //
            // Structural rather than positional, so it does not depend on the bot
            // happening to sit still. The grace period covers the window while a
            // genuine flight is being set up, when the flag can legitimately be
            // set before the generator is pushed.
            if (player->GetMotionMaster()->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
            {
                PveTimePoint const flightNow = PveClock::now();
                if (state.strandedFlightSince == PveTimePoint{})
                    state.strandedFlightSince = flightNow;
                else if (flightNow - state.strandedFlightSince >= std::chrono::seconds(10))
                {
                    TC_LOG_INFO("playerbots.pve", "Bot {} was stranded in a taxi flight with no flight path; dismounting and releasing it.",
                        player->GetName());
                    player->ClearUnitState(UNIT_STATE_IN_FLIGHT);
                    player->CleanupAfterTaxiFlight();
                    player->GetMotionMaster()->MoveIdle();
                    state.strandedFlightSince = {};
                    ResetStuckWatchdog(state);
                    return;
                }
            }
            else
            {
                state.strandedFlightSince = {};

                // The other half. A flight generator that is still installed but
                // has stopped moving leaves the bot parked on the taxi mount,
                // and nothing else will ever disturb it: the structural test
                // above sees a perfectly good generator, this tick returns early
                // on IsInFlight, and the stuck watchdog skips flying bots. That
                // is the "several bots sitting motionless on windriders" report.
                //
                // Positional, because there is nothing structural left to look
                // at - the generator says it is flying and only the coordinates
                // disagree. Safe here in a way it would not be on the ground: a
                // real taxi flight never pauses, so a full minute of stillness
                // cannot be anything else.
                constexpr float kFlightMovedYards = 5.0f;
                constexpr auto kFlightStallTimeout = std::chrono::seconds(60);

                PveTimePoint const flightNow = PveClock::now();
                float const dx = player->GetPositionX() - state.flightAnchorX;
                float const dy = player->GetPositionY() - state.flightAnchorY;
                float const dz = player->GetPositionZ() - state.flightAnchorZ;
                bool const moved = (dx * dx + dy * dy + dz * dz) > (kFlightMovedYards * kFlightMovedYards);

                if (state.flightStillSince == PveTimePoint{} || moved)
                {
                    state.flightStillSince = flightNow;
                    state.flightAnchorX = player->GetPositionX();
                    state.flightAnchorY = player->GetPositionY();
                    state.flightAnchorZ = player->GetPositionZ();
                }
                else if (flightNow - state.flightStillSince >= kFlightStallTimeout)
                {
                    TC_LOG_INFO("playerbots.pve", "Bot {} stalled mid-flight at {:.0f} {:.0f} {:.0f}; dismounting and releasing it.",
                        player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
                    player->ClearUnitState(UNIT_STATE_IN_FLIGHT);
                    player->CleanupAfterTaxiFlight();
                    player->GetMotionMaster()->MoveIdle();
                    state.flightStillSince = {};
                    ResetStuckWatchdog(state);
                    return;
                }
            }

            ResetStuckWatchdog(state);
            // Keep the post-landing journey's stuck detector quiet while the
            // taxi does the traveling.
            if (state.journeyActive)
                state.journeyProgressAt = PveClock::now();
            return;
        }
        state.strandedFlightSince = {};
        state.flightStillSince = {};
        PveTimePoint const now = PveClock::now();
        if (state.nextFastTick == PveTimePoint{})
        {
            // Stagger first evaluation by GUID so a freshly loaded population does
            // not decide on the same world tick.
            state.nextFastTick = now + std::chrono::milliseconds(rawGuid % uint64(PveFastTickInterval.count()));
            state.nextSlowTick = state.nextFastTick;
            return;
        }

        if (now >= state.nextSlowTick)
        {
            state.nextSlowTick = now + PveSlowTickInterval;
            RunSlowTick(player, state, cfg);
        }

        if (!player->IsAlive())
            return;

        if (player->HasUnitState(UNIT_STATE_CONTROLLED))
            return;

        if (now < state.nextFastTick)
            return;
        state.nextFastTick = now + PveFastTickInterval;

        RunFastTick(player, state, cfg);
    }

    void PveManager::OnBotLogout(Player const* player)
    {
        if (!player)
            return;

        uint64 const rawGuid = player->GetGUID().GetRawValue();
        PvpCore::SetPveCombatEngagement(player->GetGUID(), false);
        LockedErase(g_PveBotStateByGuid, rawGuid);

        std::lock_guard<std::mutex> guard(g_PvePendingLock);
        g_PendingSummonsByBotGuid.erase(rawGuid);
        g_PendingGroupInviteAccepts.erase(rawGuid);
        g_PendingGrindRelocations.erase(rawGuid);
        g_PendingStuckRelocations.erase(rawGuid);
        g_PendingLootExecutions.erase(rawGuid);
        g_PendingSupplyRuns.erase(rawGuid);
        g_PendingMailCollections.erase(rawGuid);
        g_PendingAuctionShopping.erase(rawGuid);
    }

    bool PveManager::IsExemptFromBattlegroundOrchestration(Player const* player)
    {
        if (!g_PveConfig.enabled || !player)
            return false;

        if (!playerbot::IsManagedRandomBot(player))
            return false;

        uint64 const rawGuid = player->GetGUID().GetRawValue();
        PveBotState stateCopy;
        if (LockedGetCopy(g_PveBotStateByGuid, rawGuid, stateCopy) && !stateCopy.masterGuid.IsEmpty())
            return true;

        return HasPendingSummon(player->GetGUID());
    }

    // A stranger who whispers a bot gets attitude, not telemetry. The
    // diagnostics are a GM tool - they name config keys, hunt radii and
    // internal verdicts, which is a map of exactly how to game the fleet - so
    // anyone who is not a GM is answered in character instead. These are bots
    // on a hardcore PvP realm; being told to get lost IS the correct response.
    constexpr std::array<char const*, 100> kStrangerTaunts = { {
        "Ask me again when you can hold a weapon.",
        "I do not brief the enemy.",
        "Come closer and find out.",
        "You get nothing from me but a corpse run.",
        "Try the guards. They have more patience.",
        "I have killed better and buried them quieter.",
        "You are not the first to ask. The others are still face down.",
        "Words are cheap out here. Draw.",
        "That is above your rank, and so am I.",
        "I do not answer to livestock.",
        "You want my numbers? Earn them.",
        "Walk away while walking is still an option.",
        "I have seen your gear. I am not worried.",
        "Nothing personal. Well. Slightly personal.",
        "Ask the last one who whispered me. Oh, wait.",
        "Save your breath. You will want it shortly.",
        "You are standing awfully close for someone so curious.",
        "This is a battlefield, not a library.",
        "The only report you are getting is your own death log.",
        "You are wasting daylight neither of us has.",
        "Curiosity is a fine epitaph.",
        "I am not your quest giver.",
        "Whispering me was the second mistake. The first was logging in.",
        "Come find out in person. I will be right here.",
        "My diagnostics are for people who can survive reading them.",
        "You look like a repair bill waiting to happen.",
        "Ask again after you have won something.",
        "I keep my secrets the way I keep my blade. Close.",
        "Get bent.",
        "You are not important enough to lie to.",
        "I will explain it slowly, with a weapon.",
        "Try again when your gear stops being grey.",
        "I have a cooldown for this conversation. It is permanent.",
        "You are the reason spirit healers stay busy.",
        "Not a chance. Not a word.",
        "Interesting question. Terrible timing.",
        "Speak up. I could not hear you over your low health.",
        "The information is free. Getting close enough is not.",
        "Every answer I have is an edged one.",
        "Go bother a critter.",
        "I have been ignoring better than you all week.",
        "That is classified under 'none of your business'.",
        "You are welcome to take it from my body.",
        "You do not want to know. You want to seem clever.",
        "I do not do interviews.",
        "You are burning my patience and your own hearthstone.",
        "Nothing to say. Plenty to do.",
        "Ask the ground. You will be seeing it soon.",
        "There is no version of this where you leave informed.",
        "Whisper me again and I will find you the loud way.",
        "You brought questions. I brought everything else.",
        "That is a lot of typing for someone about to run.",
        "The answer is no, and the follow-up is also no.",
        "You are not owed an explanation.",
        "Bold of you to open with talking.",
        "I only debrief the victorious. Come back never.",
        "Try emoting at me. That works about as well.",
        "You are one bad decision from a graveyard sprint.",
        "No.",
        "Absolutely not, and stop typing.",
        "Save it for your guild chat.",
        "I have nothing to declare but hostility.",
        "You are standing in my zone asking my business. Bold.",
        "The report is one page long and it says 'no'.",
        "I would tell you, but then you would still lose.",
        "This is not a support desk.",
        "You have mistaken me for something helpful.",
        "Everything I know, I keep. Everything you have, I take.",
        "Answer's the same as last time. Swing or leave.",
        "Do I look like a target dummy to you?",
        "You will get a full accounting. On your corpse.",
        "Not while you are breathing, no.",
        "Ask me on the losing side of a fight. Yours.",
        "I have a policy about strangers. It involves steel.",
        "You are wasting a perfectly good ambush.",
        "The only thing I am sharing is a graveyard.",
        "Try the auction house. They will talk to anyone.",
        "That question costs more than you are carrying.",
        "Go on then. Pull. I will wait.",
        "Talking is what people do instead of winning.",
        "I have no notes for you. Only intentions.",
        "You want data. I want distance closed.",
        "That is between me and whatever I kill next.",
        "You are two whispers from finding out.",
        "Nothing. Not one word. Move along.",
        "The last person who asked is still running.",
        "I am busy. You are about to be busier.",
        "Not interested. Never was.",
        "You should be watching your back, not your chat log.",
        "This conversation has a very short cooldown.",
        "I will send you my report. Posthumously.",
        "Wrong bot. Wrong day. Wrong everything.",
        "You have questions. I have a full rage bar.",
        "Take the hint, then take the hit.",
        "There is a fight happening. You are in it. Keep up.",
        "I do not brief tourists.",
        "You will learn everything you need in about four seconds.",
        "That is a strange way of saying 'come kill me'.",
        "I have said too much already. Which is nothing.",
        "Whispering was brave. Staying is not.",
    } };

    bool PveManager::HandleWhisperCommand(Player* sender, Player* botReceiver, std::string const& command)
    {
        if (!g_PveConfig.enabled || !sender || !botReceiver)
            return false;

        if (!playerbot::IsManagedRandomBot(botReceiver))
            return false;

        uint64 const rawGuid = botReceiver->GetGUID().GetRawValue();
        PveBotState& state = LockedGetOrCreate(g_PveBotStateByGuid, rawGuid);

        bool const senderIsMaster = state.masterGuid == sender->GetGUID();
        bool const senderIsGm = sender->GetSession() && sender->GetSession()->HasPermission(rbac::RBAC_PERM_COMMAND_GM);
        if (!senderIsMaster && !senderIsGm)
        {
            // Answer, but in character. Silence reads like a broken bot and
            // invites the question again; a hostile line reads like a person who
            // does not want to talk, which is exactly what this is meant to be.
            botReceiver->Whisper(kStrangerTaunts[urand(0, uint32(kStrangerTaunts.size()) - 1)],
                LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "follow")
        {
            state.stay = false;
            state.passive = false;
            botReceiver->Whisper("Following.", LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "stay")
        {
            state.stay = true;
            PvpClassActions::PrepareForExplicitMovement(botReceiver);
            if (MotionMaster* motionMaster = botReceiver->GetMotionMaster())
                motionMaster->Clear();
            botReceiver->StopMoving();
            botReceiver->Whisper("Holding position.", LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "passive")
        {
            state.passive = true;
            botReceiver->Whisper("Standing down.", LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "attack" || command == "assist")
        {
            state.passive = false;
            ObjectGuid const orderedGuid = sender->GetTarget();
            if (orderedGuid.IsEmpty())
            {
                botReceiver->Whisper("You have no target for me to attack.", LANG_UNIVERSAL, sender);
                return true;
            }

            state.orderedTargetGuid = orderedGuid;
            botReceiver->Whisper("Attacking your target.", LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "come")
        {
            QueuePendingSummon(botReceiver->GetGUID(), sender->GetGUID(), false);
            botReceiver->Whisper("On my way.", LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "dismiss")
        {
            std::string statusMessage;
            RequestCompanionDismiss(sender, botReceiver, statusMessage);
            return true;
        }

        // Diagnostics are GM-only, master or not. They print config keys, hunt
        // radii, path verdicts and level windows - a map of precisely how to
        // stay outside a bot's engage check - so somebody who merely owns a
        // companion has no business reading them off the fleet.
        bool const diagnostic = command == "pve status" || command == "pve diag" ||
            command == "diag" || command == "why" || command == "why idle" ||
            command == "why fight" || command == "hunt diag";
        if (diagnostic && !senderIsGm)
        {
            botReceiver->Whisper(kStrangerTaunts[urand(0, uint32(kStrangerTaunts.size()) - 1)],
                LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "pve status")
        {
            botReceiver->Whisper(BuildStatusLine(botReceiver), LANG_UNIVERSAL, sender);
            return true;
        }

        if (command == "pve diag" || command == "diag" || command == "why" ||
            command == "why idle" || command == "why fight" || command == "hunt diag")
        {
            for (std::string const& line : BuildPveWhisperDiagnostics(botReceiver, sender))
                botReceiver->Whisper(line, LANG_UNIVERSAL, sender);
            return true;
        }

        return false;
    }

    bool PveManager::RequestCompanionSummon(Player* summoner, std::string const& characterName, std::string& statusMessage)
    {
        if (!g_PveConfig.enabled)
        {
            statusMessage = "Playerbot PvE support is disabled (Playerbot.Pve.Enable).";
            return false;
        }

        if (g_PveConfig.declineGroupInvites)
        {
            statusMessage = "Bots on this realm walk alone (Playerbot.Pve.DeclineGroupInvites).";
            return false;
        }

        if (!summoner)
            return false;

        ObjectGuid const characterGuid = sCharacterCache->GetCharacterGuidByName(characterName);
        if (characterGuid.IsEmpty())
        {
            statusMessage = "No character named '" + characterName + "' exists.";
            return false;
        }

        uint32 const accountId = sCharacterCache->GetCharacterAccountIdByGuid(characterGuid);
        std::vector<uint32> const botAccounts = RandomBotParticipationManager::GetConfiguredBotAccountIds();
        if (!std::binary_search(botAccounts.begin(), botAccounts.end(), accountId))
        {
            statusMessage = "'" + characterName + "' is not on a configured bot account.";
            return false;
        }

        if (!ObjectAccessor::FindConnectedPlayer(characterGuid) &&
            !RandomBotParticipationManager::RequestBotLoginByGuidLow(characterGuid.GetCounter()))
        {
            statusMessage = "Could not bring '" + characterName + "' online.";
            return false;
        }

        QueuePendingSummon(characterGuid, summoner->GetGUID(), true);
        statusMessage = "Summoning " + characterName + "; they will join your group shortly.";
        return true;
    }

    bool PveManager::RequestCompanionDismiss(Player* requester, Player* bot, std::string& statusMessage)
    {
        if (!bot || !playerbot::IsManagedRandomBot(bot))
        {
            statusMessage = "That is not a managed playerbot.";
            return false;
        }

        uint64 const rawGuid = bot->GetGUID().GetRawValue();
        PveBotState stateCopy;
        LockedGetCopy(g_PveBotStateByGuid, rawGuid, stateCopy);

        bool const requesterIsMaster = requester && stateCopy.masterGuid == requester->GetGUID();
        bool const requesterIsGm = requester && requester->GetSession() &&
            requester->GetSession()->HasPermission(rbac::RBAC_PERM_COMMAND_GM);
        if (!requesterIsMaster && !requesterIsGm)
        {
            statusMessage = "Only the bot's master (or a GM) can dismiss it.";
            return false;
        }

        if (requester)
            bot->Whisper("Farewell.", LANG_UNIVERSAL, requester);

        if (bot->GetGroup())
            bot->RemoveFromGroup();

        OnBotLogout(bot);
        RandomBotParticipationManager::RequestManagedBotLogout(bot->GetGUID());
        statusMessage = "Dismissed " + bot->GetName() + ".";
        return true;
    }

    void PveManager::OnManagedBotLevelChanged(Player* player, uint8 /*oldLevel*/)
    {
        if (!g_PveConfig.enabled)
            return;

        if (!player || !playerbot::IsManagedRandomBot(player))
            return;

        // Covers PvP-only bots too: they never reach the PvE slow tick.
        MaxOutWeaponSkills(player);

        // Water and food the bot has outgrown go in the bin on the level that
        // outgrew them, which is also what frees it to buy the next tier.
        if (uint32 const binned = DiscardOutclassedRations(player))
            TC_LOG_DEBUG("playerbots.pve", "Managed bot {} binned {} outclassed ration stack(s) on reaching level {}.",
                player->GetName(), binned, uint32(player->GetLevel()));

        if (g_PveConfig.autoLearnSpellsOnLevelUp)
        {
            uint32 const learned = RunTrainerSpellCatchup(player);
            if (learned)
                TC_LOG_DEBUG("playerbots.pve", "Managed bot {} learned {} trainer spells on reaching level {}.",
                    player->GetGUID().ToString(), learned, player->GetLevel());
        }

        if (EnsureBotRidingAndMount(player))
            TC_LOG_DEBUG("playerbots.pve", "Managed bot {} was granted riding at level {}.",
                player->GetGUID().ToString(), player->GetLevel());

        if (g_PveConfig.talentsEnabled)
            SpendPendingTalentPoints(player);

        // The flagged share of the fleet is reborn at the cap: back to level 1
        // and home to climb again, keeping the leveling world populated.
        bool const rebirthEligible = !IsPvpOnlyBot(player) &&
            !IsExemptFromBattlegroundOrchestration(player) &&
            !GetGuardianZoneId(player->GetGUID().GetRawValue());

        // Zone-banded is the normal life of a bot now: it cycles inside the zone it
        // was born to, so every band of content keeps a population. The exceptions
        // are the veterans, who make the full climb and stay at sixty.
        // Turning the flag off restores the old all-or-nothing behaviour, where a
        // configured share of the fleet resets to level one at the cap.
        bool const rebirthFlagged = g_PveConfig.rebirthZoneBanded
            ? (rebirthEligible && !IsVeteranBot(player))
            : (rebirthEligible && g_PveConfig.rebirthAtMaxLevelPercent &&
                uint32(player->GetGUID().GetCounter() % 100) < g_PveConfig.rebirthAtMaxLevelPercent);

        // A zone-tied bot tops out at its own zone's ceiling rather than the
        // realm's, so it cycles inside the band it lives in and the zone keeps a
        // population at the level its content is built for.
        uint8 bandBottom = 0;
        uint8 bandTop = 0;
        uint32 const rebirthZoneId = (rebirthFlagged && g_PveConfig.rebirthZoneBanded)
            ? GetRebirthZoneId(player) : 0u;
        bool const banded = rebirthZoneId && GetZoneLevelBand(rebirthZoneId, bandBottom, bandTop);
        uint32 const rebirthAtLevel = banded ? uint32(bandTop) : sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

        // Outside the band in EITHER direction. Firing only from above left every
        // high zone empty: a bot assigned to a twenty-five to thirty-five zone
        // while sitting at level fourteen cannot grind there, so it lives wherever
        // its level fits and does not reach its own zone until it has climbed the
        // whole way to the ceiling - by which time most of the fleet is somewhere
        // else entirely. Measured live: 124 of 229 bots between ten and nineteen,
        // eleven in the twenty-five to thirty-five band, and every high zone
        // holding exactly one bot.
        //
        // Rebirthing a bot UP to its band bottom puts it where it belongs at once
        // and keeps it there, which is the whole point of tying it to a zone. It
        // is the same reset either way, so a bot arriving from below is re-kitted
        // exactly like one cycling from above.
        bool const belowBand = banded && player->GetLevel() < bandBottom;

        if (rebirthFlagged && (belowBand || player->GetLevel() >= rebirthAtLevel))
        {
            if (belowBand)
                TC_LOG_INFO("playerbots.pve", "Bot {} is below zone {}'s band at level {}; rebirth up to {}.",
                    player->GetName(), rebirthZoneId, uint32(player->GetLevel()), uint32(bandBottom));
            else if (banded)
                TC_LOG_INFO("playerbots.pve", "Bot {} topped out zone {} at level {}; rebirth to {}.",
                    player->GetName(), rebirthZoneId, uint32(bandTop), uint32(bandBottom));
            else
                TC_LOG_INFO("playerbots.pve", "Bot {} reached the level cap and is flagged for rebirth.", player->GetName());
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            g_PendingRebirths.insert(player->GetGUID().GetRawValue());
        }
    }


    // Full rebirth of ONE managed bot: strip it back to a freshly created
    // level-1 character - gear, bags, bank, money, spells, talents, quests,
    // pet - and port it to its racial starting spot. World thread only.
    // The veterans: the handful of bots that make the whole journey to sixty and
    // stay there, so the realm still has a population at the top of the chart. Asked for as a COUNT and turned
    // into a share of the configured fleet, so the operator says "twenty" and gets
    // roughly twenty however the population is sized.
    //
    // Chosen by hashing the guid rather than by ranking a roster: there is no
    // stable roster to rank against - the fleet is whoever happens to be logged in
    // - and a hash keeps a bot's role fixed for its entire life, which is the point.
    bool IsVeteranBot(Player const* bot)
    {
        return bot && IsLocalVeteranGuid(bot->GetGUID().GetRawValue());
    }

    bool GetZoneLevelBand(uint32 zoneId, uint8& bottom, uint8& top)
    {
        ClassicZoneBand const* band = FindClassicZoneBand(zoneId);
        if (!band)
            return false;

        bottom = band->minLevel;
        top = band->maxLevel;
        return top > bottom;
    }

    bool FindGrindSpotInZone(uint32 zoneId, uint8 level, GrindSpot& out)
    {
        std::lock_guard<std::mutex> guard(g_GrindSpotLock);
        for (uint8 probe = 0; probe <= 5; ++probe)
        {
            auto itr = g_GrindSpotsByLevel.find(uint8(std::max(1, int32(level) - int32(probe))));
            if (itr == g_GrindSpotsByLevel.end())
                continue;

            for (GrindSpot const& spot : itr->second)
                if (spot.zoneId == zoneId)
                {
                    out = spot;
                    return true;
                }
        }

        return false;
    }

    // Rebirth inside a zone's band instead of all the way to level one. The bot
    // keeps everything it owns - gear, bags, bank, money - because the point is a
    // zone that always has somebody in it at the right level, not a punishment.
    //
    // Equipped items are moved into bags rather than left on: a bot dropped to
    // level twelve cannot use what it earned at fifty, and leaving it worn strands
    // it in gear it draws no stats from. Anything that will not fit stays equipped;
    // nothing is destroyed either way.
    void ResetManagedBotToZoneBand(Player* bot, uint32 zoneId, uint8 bottomLevel)
    {
        // Defence in depth: this teleports, and teleporting a bot whose previous
        // teleport has not landed trips RemoveFromGrid's IsInGrid assert. Callers
        // include GM commands, which have no such check of their own.
        if (bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            return;

        // And never on a bot that is not in the world, which is what was aborting
        // the server. Player::DestroyItem sets ITEM_REMOVED unconditionally but
        // only calls Item::RemoveFromWorld() when the PLAYER is in world:
        //
        //     if (IsInWorld() && update)
        //         pItem->RemoveFromWorld();
        //     ...
        //     pItem->SetState(ITEM_REMOVED, this);
        //
        // Clear a bot's inventory while it is out of world and every item is
        // queued for deletion while still flagged in-world. The SaveToDB at the
        // end of this same function then reaches them, Item::SaveToDB deletes an
        // ITEM_REMOVED item outright, and ~Object aborts the process on
        // "deleted but still in world" - taking the world thread with it.
        if (!bot->IsInWorld())
            return;
        uint64 const botRawGuid = bot->GetGUID().GetRawValue();

        playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
        playerbot::LockedErase(g_PveBotStateByGuid, botRawGuid);
        bot->CombatStop(true);
        bot->RemovePet(nullptr, PET_SAVE_AS_DELETED);
        bot->RemoveAllAuras();

        // Quests go too, or the bot carries level fifty objectives through a level
        // twelve life and never picks up anything its own size again.
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (uint32 questId = bot->GetQuestSlotQuestId(slot))
            {
                bot->RemoveActiveQuest(questId, false);
                bot->SetQuestSlot(slot, 0);
            }
        for (uint32 questId : std::vector<uint32>(bot->getRewardedQuests().begin(), bot->getRewardedQuests().end()))
            bot->RemoveRewardedQuest(questId);

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                continue;   // no room: keep it worn rather than destroy it

            bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
            bot->StoreItem(dest, item, true);
        }

        // Talents back into the pool. The talent tick re-spends them inside a
        // minute, for the spec the bot is meant to be, so this needs no help.
        bot->ResetTalents(true);

        // Every spell, then the class baseline. RunTrainerSpellCatchup re-teaches
        // whatever the new level is actually entitled to on the next pass, so this
        // is how the spells it should no longer know are removed without keeping a
        // per-spell level table of our own.
        std::vector<uint32> knownSpells;
        for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
            if (playerSpell.state != PLAYERSPELL_REMOVED)
                knownSpells.push_back(spellId);
        for (uint32 spellId : knownSpells)
            bot->RemoveSpell(spellId, false, false);
        bot->GetSpellHistory()->ResetAllCooldowns();

        bot->GiveLevel(bottomLevel);
        bot->SetUInt32Value(PLAYER_XP, 0);
        bot->LearnDefaultSkills();
        bot->LearnCustomSpells();

        // Now take off anything this level cannot wear.
        //
        // The unequip pass above runs BEFORE the level changes, so everything it
        // looked at was still legal, and it SKIPS any piece it cannot find bag
        // room for ("no room: keep it worn rather than destroy it"). A bot whose
        // pack is full - which is most of them, because the pack fills with
        // poisons, pamphlets and quest scraps nothing ever clears - therefore
        // comes out of a rebirth still wearing the gear of the level it used to
        // be. Measured on the live realm: twelve bots wearing fifty-one pieces
        // above their own level, one of them a level 19 in a level 27 dagger.
        //
        // It is not cosmetic. RequiredLevel is checked when equipping, never
        // afterwards, so the bot keeps the stats and the gear scorer keeps
        // comparing candidates against a kit it could never buy again - which is
        // why such a bot sits on a thousand gold and never re-gears.
        //
        // Storing is still tried first. If there is no room, the piece is
        // destroyed rather than left on: at this point the bot has been stripped
        // back to a level it did not earn, the hardcore ruleset reissues a field
        // kit for any empty slot on its next pass, and a slot that stays empty
        // for a minute is a far smaller thing than a permanent lie about what
        // this character is wearing.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->RequiredLevel <= bottomLevel)
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
            {
                bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
                bot->StoreItem(dest, item, true);
            }
            else
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        }
        bot->SetFullHealth();
        if (bot->GetMaxPower(POWER_MANA))
            bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

        // Gathering skill is deliberately left alone: it is held at a cap for the
        // bot's level elsewhere, and that pass only ever raises, so a reborn bot
        // keeps the profession progress it earned across cycles.

        GrindSpot spot;
        if (FindGrindSpotInZone(zoneId, bottomLevel, spot))
        {
            // Homebind too, so a corpse run or a hearth keeps it in its zone.
            WorldLocation const home(spot.mapId, spot.x, spot.y, spot.z, 0.0f);
            bot->SetHomebind(home, zoneId);
            if (BotCanTeleportNow(bot) && bot->TeleportTo(home))
                RestorePlayerbotTeleportVitals(bot);
        }

        bot->SaveToDB();
        TC_LOG_INFO("playerbots.pve", "Bot {} reborn at level {} in zone {}.",
            bot->GetName(), uint32(bottomLevel), zoneId);
    }

    void ResetManagedBotToLevelOne(Player* bot)
    {
        // Defence in depth: this teleports, and teleporting a bot whose previous
        // teleport has not landed trips RemoveFromGrid's IsInGrid assert. Callers
        // include GM commands, which have no such check of their own.
        if (bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            return;

        // And never on a bot that is not in the world, which is what was aborting
        // the server. Player::DestroyItem sets ITEM_REMOVED unconditionally but
        // only calls Item::RemoveFromWorld() when the PLAYER is in world:
        //
        //     if (IsInWorld() && update)
        //         pItem->RemoveFromWorld();
        //     ...
        //     pItem->SetState(ITEM_REMOVED, this);
        //
        // Clear a bot's inventory while it is out of world and every item is
        // queued for deletion while still flagged in-world. The SaveToDB at the
        // end of this same function then reaches them, Item::SaveToDB deletes an
        // ITEM_REMOVED item outright, and ~Object aborts the process on
        // "deleted but still in world" - taking the world thread with it.
        if (!bot->IsInWorld())
            return;
        {
            uint64 const botRawGuid = bot->GetGUID().GetRawValue();

            playerbot::PvpCore::SetPveCombatEngagement(bot->GetGUID(), false);
            playerbot::LockedErase(g_PveBotStateByGuid, botRawGuid);
            bot->CombatStop(true);
            bot->RemovePet(nullptr, PET_SAVE_AS_DELETED);
            bot->RemoveAllAuras();

            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (uint32 questId = bot->GetQuestSlotQuestId(slot))
                {
                    bot->RemoveActiveQuest(questId, false);
                    bot->SetQuestSlot(slot, 0);
                }
            for (uint32 questId : std::vector<uint32>(bot->getRewardedQuests().begin(), bot->getRewardedQuests().end()))
                bot->RemoveRewardedQuest(questId);

            // Bag and bank-bag contents first, then every direct slot.
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        if (bag->GetItemByPos(uint8(slot)))
                            bot->DestroyItem(bagSlot, uint8(slot), true);
            for (uint8 bagSlot = BANK_SLOT_BAG_START; bagSlot < BANK_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        if (bag->GetItemByPos(uint8(slot)))
                            bot->DestroyItem(bagSlot, uint8(slot), true);
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < BANK_SLOT_BAG_END; ++slot)
                if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            for (uint8 slot = KEYRING_SLOT_START; slot < CURRENCYTOKEN_SLOT_END; ++slot)
                if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            bot->SetMoney(0);

            bot->ResetTalents(true);
            std::vector<uint32> knownSpells;
            for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
                if (playerSpell.state != PLAYERSPELL_REMOVED)
                    knownSpells.push_back(spellId);
            for (uint32 spellId : knownSpells)
                bot->RemoveSpell(spellId, false, false);
            bot->GetSpellHistory()->ResetAllCooldowns();

            bot->SetSkill(SKILL_HERBALISM, 0, 0, 0);
            bot->SetSkill(SKILL_MINING, 0, 0, 0);
            bot->SetSkill(SKILL_SKINNING, 0, 0, 0);

            bot->GiveLevel(1);
            bot->SetUInt32Value(PLAYER_XP, 0);
            bot->LearnDefaultSkills();
            bot->LearnCustomSpells();
            bot->SetFullHealth();
            if (bot->GetMaxPower(POWER_MANA))
                bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

            bool wentHome = false;
            if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(bot->GetRace(), bot->GetClass()))
            {
                WorldLocation const home(info->mapId, info->positionX, info->positionY, info->positionZ, info->orientation);
                bot->SetHomebind(home, info->areaId);
                if (BotCanTeleportNow(bot) && bot->TeleportTo(home))
                {
                    RestorePlayerbotTeleportVitals(bot);
                    wentHome = true;
                }
            }

            // The trip home was best-effort, and the log said "sent home" either
            // way. BotCanTeleportNow refuses for perfectly ordinary reasons - a
            // teleport already in flight, the wrong map state - and when it did,
            // the bot was stripped to level 1 and simply left standing where it
            // was. That is the level 1 druid in the Badlands: the rebirth worked,
            // the relocation silently did not.
            //
            // Queue it instead of dropping it. The relocation executor owns the
            // teleport, runs on the world thread and already sends a bot to the
            // zone its level belongs in - which for a freshly reset bot is a
            // starter zone.
            if (!wentHome)
            {
                std::lock_guard<std::mutex> guard(g_PvePendingLock);
                g_PendingGrindRelocations.insert(bot->GetGUID().GetRawValue());
            }

            bot->SaveToDB();
            TC_LOG_INFO("playerbots.pve", "Bot {} was reset to level 1 and {}.",
                bot->GetName(), wentHome ? "sent home" : "queued for relocation");
        }
    }

    // Send every managed bot to the zone it is supposed to live in, now, rather
    // than waiting for relocation to fire naturally. Turning zone banding on, or
    // retuning a band, leaves the whole fleet standing wherever it already was -
    // and a bot only relocates on a dry wander, an outlevel or a rebirth, so
    // evening out on its own takes hours.
    uint32 PveManager::RelocateBotsToHomeZones()
    {
        // Selection happens with NO lock held. GetGuardianZoneId takes the guardian
        // lock, and holding the pending lock across a call that takes another one
        // is a lock-order inversion against every map thread that takes them the
        // other way round - which hung the world thread outright the first time
        // this command was run.
        std::vector<uint64> targets;
        for (auto const& [accountId, session] : sWorld->GetAllSessions())
        {
            Player* candidate = session ? session->GetPlayer() : nullptr;
            if (!candidate || !candidate->IsInWorld() || !playerbot::IsManagedRandomBot(candidate))
                continue;

            // Same exemptions the reset command uses: a companion is serving a
            // human, a PvP-only bot is not part of the levelling world, and a
            // guardian's post IS its home already.
            if (IsExemptFromBattlegroundOrchestration(candidate) || IsPvpOnlyBot(candidate) ||
                GetGuardianZoneId(candidate->GetGUID().GetRawValue()))
                continue;

            targets.push_back(candidate->GetGUID().GetRawValue());
        }

        // Queued rather than moved here: the relocation executor owns the actual
        // teleport, runs on the world thread and already prefers the bot's own
        // zone. Doing it inline would duplicate all of that, and badly.
        {
            std::lock_guard<std::mutex> guard(g_PvePendingLock);
            for (uint64 rawGuid : targets)
                g_PendingGrindRelocations.insert(rawGuid);
        }

        return uint32(targets.size());
    }

    uint32 PveManager::ResetBotsToLevelOne(uint8 percent)
    {
        percent = std::min<uint8>(percent ? percent : 100, 100);

        std::vector<Player*> managedBots;
        for (auto const& [accountId, session] : sWorld->GetAllSessions())
        {
            Player* candidate = session ? session->GetPlayer() : nullptr;
            if (!candidate || !candidate->IsInWorld() || !playerbot::IsManagedRandomBot(candidate))
                continue;
            // Companions serving a human are left alone; PvP-only bots are not
            // part of the leveling world at all; zone guardians keep their post.
            if (IsExemptFromBattlegroundOrchestration(candidate) || IsPvpOnlyBot(candidate) ||
                GetGuardianZoneId(candidate->GetGUID().GetRawValue()))
                continue;
            managedBots.push_back(candidate);
        }

        Trinity::Containers::RandomShuffle(managedBots);
        uint32 const resetCount = uint32(managedBots.size()) * percent / 100;
        for (uint32 index = 0; index < resetCount; ++index)
            ResetManagedBotToLevelOne(managedBots[index]);
        return resetCount;
    }

    uint32 PveManager::ClearAuctionHouse()
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        uint32 removed = 0;

        for (uint8 houseId : { uint8(AUCTIONHOUSE_ALLIANCE), uint8(AUCTIONHOUSE_HORDE), uint8(AUCTIONHOUSE_NEUTRAL) })
        {
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            if (!house)
                continue;

            // Collect first: RemoveAuction erases from the very map we would
            // otherwise be iterating, and it deletes the entry as it goes.
            std::vector<AuctionEntry*> auctions;
            for (auto itr = house->GetAuctionsBegin(); itr != house->GetAuctionsEnd(); ++itr)
                auctions.push_back(itr->second);

            for (AuctionEntry* auction : auctions)
            {
                // The escrowed item has to go too, or item_instance keeps a row
                // that nothing in the world can ever reach again. Read every
                // field BEFORE RemoveAuction - it frees the entry.
                sAuctionMgr->RemoveAItem(auction->itemGUIDLow, true, &trans);
                auction->DeleteFromDB(trans);
                house->RemoveAuction(auction);
                ++removed;
            }
        }

        CharacterDatabase.CommitTransaction(trans);
        TC_LOG_INFO("playerbots.pve", "Cleared {} auctions from every auction house.", removed);
        return removed;
    }

    uint32 PveManager::RespecBotsToDonorBuilds()
    {
        std::vector<Player*> managedBots;
        for (auto const& [accountId, session] : sWorld->GetAllSessions())
        {
            Player* candidate = session ? session->GetPlayer() : nullptr;
            if (!candidate || !candidate->IsInWorld() || !playerbot::IsManagedRandomBot(candidate))
                continue;
            if (candidate->GetLevel() < 10)
                continue;
            managedBots.push_back(candidate);
        }

        uint32 respecced = 0;
        for (Player* bot : managedBots)
        {
            // no_cost: these are not the player's gold and the bot must not be
            // priced out of conforming to its own build.
            bot->ResetTalents(true);
            SpendPendingTalentPoints(bot);
            ++respecced;
        }

        TC_LOG_INFO("playerbots.pve", "Respecced {} managed bots onto their donor builds.", respecced);
        return respecced;
    }

    std::string PveManager::BuildStatusLine(Player const* bot)
    {
        std::ostringstream stream;
        stream << "PvE: enabled=" << (g_PveConfig.enabled ? "yes" : "no");
        if (!bot)
            return stream.str();

        PveBotState stateCopy;
        LockedGetCopy(g_PveBotStateByGuid, bot->GetGUID().GetRawValue(), stateCopy);

        std::string masterName = "none";
        if (!stateCopy.masterGuid.IsEmpty())
            if (!sCharacterCache->GetCharacterNameByGuid(stateCopy.masterGuid, masterName))
                masterName = stateCopy.masterGuid.ToString();

        stream << " master=" << masterName
            << " mode=" << (!stateCopy.masterGuid.IsEmpty() ? "companion" : (g_PveConfig.grindEnabled ? "grind" : "idle"))
            << " passive=" << (stateCopy.passive ? "yes" : "no")
            << " stay=" << (stateCopy.stay ? "yes" : "no")
            << " engaged=" << (stateCopy.engaged ? "yes" : "no")
            << " level=" << uint32(bot->GetLevel())
            << " map=" << bot->GetMapId()
            << " zone=" << bot->GetZoneId()
            << " suitable=" << (BotIsInSuitableZone(const_cast<Player*>(bot)) ? "yes" : "NO")
            << " aggression=" << uint32(GetBotAggression(bot));
        return stream.str();
    }
}
