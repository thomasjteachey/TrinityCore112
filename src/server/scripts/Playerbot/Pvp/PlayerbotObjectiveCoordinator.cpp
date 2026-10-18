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

#include "PlayerbotObjectiveCoordinator.h"
#include "PlayerbotObcClone.h"
#include "PlayerbotRandomBotParticipation.h"
#include "PlayerbotSharedStateGuard.h"
#include "Battleground.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStructure.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Vehicle.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    using playerbot::ObjectiveDuty;

    constexpr uint32 kNoTarget = std::numeric_limits<uint32>::max();

    constexpr uint32 kPlanRefreshMs = 500;
    constexpr uint32 kPlanPruneIntervalMs = 60 * IN_MILLISECONDS;
    constexpr uint32 kPlanStaleMs = 10 * MINUTE * IN_MILLISECONDS;

    // A commander is how these matches are won, and a handful of bodies only
    // feeds it one at a time: half the team goes, never fewer than four.
    constexpr uint32 kBossAssaultSharePct = 50;
    constexpr uint32 kMinBossAssault = 4;
    // Half the team works a gate while there are bases to hold as well.
    constexpr uint32 kSiegeSharePct = 50;
    // A bot further than this from a free vehicle is not sent for it: by the
    // time it arrives somebody nearer has it, or it has died on the way.
    constexpr float kVehicleClaimRange = 200.0f;
    // An enemy this close to the ground in front of one of our gates is a
    // threat to it.
    constexpr float kGateThreatRange = 60.0f;
    // Gunners, as a share of the team: the guns cover the ground in front of
    // the gates, and the gates still need the bodies at them.
    constexpr uint32 kTurretSharePct = 30;
    // A gunner already in its seat keeps the gun this far past its reach, so
    // an enemy on the edge of it does not have the bot in and out of the seat.
    constexpr float kTurretKeepMargin = 15.0f;
    constexpr uint32 kFlagHunters = 2;
    // What the duty a bot already has is worth, in yards of head start, so a
    // rebuild does not re-deal the whole team; and how long a bot keeps a duty
    // it was just handed whatever the distances say.
    constexpr float kStickinessYards = 60.0f;
    constexpr uint32 kMinDutyHoldMs = 8 * IN_MILLISECONDS;
    constexpr float kHoldDiscount = 10000.0f;

    struct DutyAssignment
    {
        ObjectiveDuty duty = ObjectiveDuty::None;
        uint32 targetId = kNoTarget;    // siege objective or boss index
        ObjectGuid vehicleGuid;         // DriveSiege
        uint32 sinceMs = 0;
    };

    struct TeamDutyPlan
    {
        uint32 lastRefreshMs = 0;
        uint32 lastSeenMs = 0;
        std::unordered_map<ObjectGuid, DutyAssignment> assignments;
        uint32 livingBots = 0;
        uint32 breachTarget = kNoTarget;
        uint32 bossTarget = kNoTarget;
        // The charge seen at the last rebuild. A bot already holding one keeps
        // planting it after the last source has been used up, when the
        // battleground no longer has a charge to describe.
        uint32 chargeItemId = 0;
        std::vector<std::string> summary;
    };

    std::unordered_map<uint64, TeamDutyPlan> g_DutyPlans;
    uint32 g_LastDutyPlanPruneMs = 0;

    // Vehicles a driver could not get to its gate, until when they are left
    // alone. Guarded by SharedBotStateStructureLock.
    std::unordered_map<ObjectGuid, uint32> g_VehicleGiveUpUntilMs;
    constexpr uint32 kVehicleGiveUpMs = 60 * IN_MILLISECONDS;

    std::atomic<bool> g_SiegeVehiclesEnabled{ true };

    bool IsVehicleGivenUp(ObjectGuid vehicleGuid, uint32 nowMs)
    {
        std::lock_guard<std::mutex> guard(playerbot::SharedBotStateStructureLock());
        auto const itr = g_VehicleGiveUpUntilMs.find(vehicleGuid);
        if (itr == g_VehicleGiveUpUntilMs.end())
            return false;
        if (int32(itr->second - nowMs) > 0)
            return true;
        g_VehicleGiveUpUntilMs.erase(itr);
        return false;
    }

    // Whether the seat that drives this vehicle is empty.
    bool IsControlSeatFree(Creature const* vehicle)
    {
        Vehicle const* kit = vehicle ? vehicle->GetVehicleKit() : nullptr;
        int8 const seat = playerbot::GetVehicleControlSeat(vehicle);
        if (!kit || seat < 0)
            return false;

        auto const itr = kit->Seats.find(seat);
        return itr != kit->Seats.end() && itr->second.IsEmpty();
    }

    uint64 BuildPlanKey(Battleground const* battleground, TeamId team)
    {
        return (uint64(battleground->GetTypeID()) << 40) |
            (uint64(battleground->GetInstanceID()) << 1) |
            uint64(team == TEAM_HORDE ? 1 : 0);
    }

    TeamId ResolveTeam(Battleground const* battleground, Player const* player)
    {
        if (!battleground || !player)
            return TEAM_NEUTRAL;

        uint32 team = battleground->GetPlayerTeam(player->GetGUID());
        if (!team && player->GetBattlegroundId() == battleground->GetInstanceID())
            team = player->GetBGTeam();

        switch (team)
        {
            case ALLIANCE: return TEAM_ALLIANCE;
            case HORDE: return TEAM_HORDE;
            default: return TEAM_NEUTRAL;
        }
    }

    bool IsBotControlled(Player const* player)
    {
        return player && (playerbot::IsManagedRandomBot(player) ||
            playerbot::PlayerbotObcCloneManager::IsActiveClone(player));
    }

    // Only a FAR teleport takes a player out of the match.
    bool IsAliveParticipant(Player const* player)
    {
        return player && player->IsInWorld() && player->IsAlive() &&
            !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) &&
            !player->IsBeingTeleportedFar() && !player->IsSpectator() && !player->IsGameMaster();
    }

    // The vehicle a bot is driving, if it sits in its driver's seat.
    Creature* GetDrivenVehicle(Player const* bot)
    {
        Unit* base = bot ? bot->GetVehicleBase() : nullptr;
        Creature* vehicle = base ? base->ToCreature() : nullptr;
        Vehicle const* kit = vehicle ? vehicle->GetVehicleKit() : nullptr;
        int8 const seat = playerbot::GetVehicleControlSeat(vehicle);
        if (!kit || seat < 0)
            return nullptr;

        auto const itr = kit->Seats.find(seat);
        return itr != kit->Seats.end() && itr->second.Passenger.Guid == bot->GetGUID() ? vehicle : nullptr;
    }

    TeamDutyPlan& TouchTeamPlan(uint64 key, uint32 nowMs)
    {
        std::lock_guard<std::mutex> guard(playerbot::SharedBotStateStructureLock());
        if (nowMs - g_LastDutyPlanPruneMs >= kPlanPruneIntervalMs)
        {
            g_LastDutyPlanPruneMs = nowMs;
            for (auto itr = g_DutyPlans.begin(); itr != g_DutyPlans.end();)
            {
                if (itr->first != key && nowMs - itr->second.lastSeenMs >= kPlanStaleMs)
                    itr = g_DutyPlans.erase(itr);
                else
                    ++itr;
            }
        }

        TeamDutyPlan& plan = g_DutyPlans[key];
        plan.lastSeenMs = nowMs;
        return plan;
    }

    struct SiegeView
    {
        uint32 index = 0;
        BattlegroundSiegeObjective objective;
        uint32 threat = 0;
    };

    void RefreshTeamPlan(Battleground* battleground, Map* map, TeamId team, ObjectGuid referenceGuid,
        TeamDutyPlan& plan, uint32 nowMs)
    {
        plan.lastRefreshMs = nowMs;
        plan.summary.clear();

        std::vector<Player*> bots;
        std::vector<Player*> living;
        std::vector<Player*> enemies;
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || member->IsSpectator() || member->IsGameMaster())
                continue;
            if (member->GetBattlegroundId() != battleground->GetInstanceID())
                continue;

            TeamId const memberTeam = ResolveTeam(battleground, member);
            if (memberTeam == TEAM_NEUTRAL)
                continue;

            if (memberTeam == team)
            {
                if (!IsBotControlled(member))
                    continue;

                bots.push_back(member);
                if (IsAliveParticipant(member))
                    living.push_back(member);
                continue;
            }

            // A stealthed enemy is not pressure the bots are allowed to feel;
            // see NodeCoordinator for the same rule on bases.
            if (IsAliveParticipant(member) && !member->HasStealthAura() && !member->HasInvisibilityAura())
                enemies.push_back(member);
        }

        std::unordered_map<ObjectGuid, DutyAssignment> previous;
        previous.swap(plan.assignments);
        plan.livingBots = uint32(living.size());

        uint32 const previousBreachTarget = plan.breachTarget;
        uint32 const previousBossTarget = plan.bossTarget;
        plan.breachTarget = kNoTarget;
        plan.bossTarget = kNoTarget;

        if (bots.empty())
            return;

        std::vector<bool> taken(living.size(), false);
        auto const previousDuty = [&](Player const* bot) -> DutyAssignment const*
        {
            auto const itr = previous.find(bot->GetGUID());
            return itr == previous.end() ? nullptr : &itr->second;
        };

        // Distance to the duty, discounted for a bot that already had it.
        auto const dutyCost = [&](std::size_t i, ObjectiveDuty duty, uint32 targetId, ObjectGuid vehicleGuid,
            Position const& where) -> float
        {
            float cost = living[i]->GetExactDist(where.GetPositionX(), where.GetPositionY(), where.GetPositionZ());
            if (DutyAssignment const* had = previousDuty(living[i]);
                had && had->duty == duty && had->targetId == targetId && had->vehicleGuid == vehicleGuid)
            {
                cost -= kStickinessYards;
                if (nowMs - had->sinceMs < kMinDutyHoldMs)
                    cost -= kHoldDiscount;
            }
            return cost;
        };

        auto const assign = [&](std::size_t i, ObjectiveDuty duty, uint32 targetId, ObjectGuid vehicleGuid)
        {
            taken[i] = true;
            DutyAssignment assignment;
            assignment.duty = duty;
            assignment.targetId = targetId;
            assignment.vehicleGuid = vehicleGuid;
            DutyAssignment const* had = previousDuty(living[i]);
            assignment.sinceMs = had && had->duty == duty && had->targetId == targetId &&
                had->vehicleGuid == vehicleGuid ? had->sinceMs : nowMs;
            plan.assignments[living[i]->GetGUID()] = assignment;
        };

        auto const untakenCount = [&]() -> uint32
        {
            return uint32(std::count(taken.begin(), taken.end(), false));
        };

        auto const assignNearest = [&](uint32 count, ObjectiveDuty duty, uint32 targetId, Position const& where) -> uint32
        {
            uint32 given = 0;
            while (given < count)
            {
                std::size_t best = living.size();
                float bestCost = std::numeric_limits<float>::max();
                for (std::size_t i = 0; i < living.size(); ++i)
                {
                    if (taken[i])
                        continue;

                    float const cost = dutyCost(i, duty, targetId, ObjectGuid::Empty, where);
                    if (cost < bestCost)
                    {
                        bestCost = cost;
                        best = i;
                    }
                }

                if (best == living.size())
                    break;

                assign(best, duty, targetId, ObjectGuid::Empty);
                ++given;
            }
            return given;
        };

        uint32 const livingCount = uint32(living.size());
        auto const share = [livingCount](uint32 pct) -> uint32
        {
            return (livingCount * pct + 99) / 100;
        };

        Position centroid;
        if (!living.empty())
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            for (Player const* bot : living)
            {
                x += bot->GetPositionX();
                y += bot->GetPositionY();
                z += bot->GetPositionZ();
            }
            centroid.Relocate(x / livingCount, y / livingCount, z / livingCount);
        }

        // A side with no bases to play (the defenders of Strand of the
        // Ancients) has nothing to hold back for: its duties take everybody.
        bool teamHasBases = false;
        for (uint32 nodeId = 0; nodeId < battleground->GetDynamicNodeCount() && !teamHasBases; ++nodeId)
        {
            BattlegroundNodeObjective node;
            teamHasBases = battleground->GetDynamicNodeInfo(referenceGuid, nodeId, node);
        }

        // 1. The enemy commander, once the battleground says it can be fought.
        //    The nearest engageable one: in Alterac Valley that takes a captain
        //    on the way to the general.
        if (livingCount)
        {
            uint32 chosen = kNoTarget;
            Position chosenWhere;
            float chosenDistance = std::numeric_limits<float>::max();
            for (uint32 index = 0; index < battleground->GetBossObjectiveCount(); ++index)
            {
                BattlegroundBossObjective boss;
                if (!battleground->GetBossObjectiveInfo(referenceGuid, index, boss) || boss.Friendly || !boss.Engageable)
                    continue;

                Creature* creature = map->GetCreature(boss.BossGuid);
                if (!creature || !creature->IsAlive())
                    continue;

                Position const where = boss.HasApproach ? boss.Approach : creature->GetPosition();
                float distance = centroid.GetExactDist(&where);
                if (index == previousBossTarget)
                    distance -= 100.0f;
                if (distance < chosenDistance)
                {
                    chosenDistance = distance;
                    chosen = index;
                    chosenWhere = where;
                }
            }

            if (chosen != kNoTarget)
            {
                plan.bossTarget = chosen;
                uint32 const want = std::min(livingCount, std::max(kMinBossAssault, share(kBossAssaultSharePct)));
                assignNearest(want, ObjectiveDuty::AssaultBoss, chosen, chosenWhere);
            }
        }

        // The gates, read once.
        std::vector<SiegeView> gates;
        for (uint32 index = 0; index < battleground->GetSiegeObjectiveCount(); ++index)
        {
            SiegeView view;
            view.index = index;
            if (!battleground->GetSiegeObjectiveInfo(referenceGuid, index, view.objective) || !view.objective.Breachable)
                continue;

            for (Player const* enemy : enemies)
                if (enemy->IsWithinDist3d(view.objective.Location.GetPositionX(), view.objective.Location.GetPositionY(),
                    view.objective.Location.GetPositionZ(), kGateThreatRange))
                    ++view.threat;

            gates.push_back(view);
        }

        // 2. The enemy's gate. One gate for the whole team, so the charges and
        //    the rams add up: the one it was already working on while it
        //    stands, otherwise the nearest.
        SiegeView const* breach = nullptr;
        for (SiegeView const& view : gates)
        {
            if (view.objective.Friendly)
                continue;
            if (view.index == previousBreachTarget)
            {
                breach = &view;
                break;
            }
            if (!breach || centroid.GetExactDist(&view.objective.Location) < centroid.GetExactDist(&breach->objective.Location))
                breach = &view;
        }

        BattlegroundSiegeChargeInfo charge;
        bool const hasCharge = battleground->GetSiegeChargeInfo(referenceGuid, charge) && charge.ItemId;
        if (hasCharge)
            plan.chargeItemId = charge.ItemId;

        if (breach && livingCount)
        {
            plan.breachTarget = breach->index;
            uint32 budget = std::min(untakenCount(), teamHasBases ? share(kSiegeSharePct) : untakenCount());

            // Driving is server-side movement of a creature a player controls,
            // the least proven thing the bots do here; it can be switched off
            // with a config reload, and the bots in a vehicle then climb out.
            bool const vehiclesAllowed = g_SiegeVehiclesEnabled.load(std::memory_order_relaxed);

            // A bot already driving keeps its vehicle: it was put there by this
            // plan, and a rebuild must never pull a driver out of a running ram.
            for (std::size_t i = 0; i < living.size() && budget && vehiclesAllowed; ++i)
            {
                if (taken[i])
                    continue;
                if (Creature* vehicle = GetDrivenVehicle(living[i]); vehicle && !IsVehicleGivenUp(vehicle->GetGUID(), nowMs))
                {
                    assign(i, ObjectiveDuty::DriveSiege, breach->index, vehicle->GetGUID());
                    --budget;
                }
            }

            // Then one driver for each free vehicle, nearest first.
            GuidVector vehicles;
            if (vehiclesAllowed)
                battleground->GetSiegeVehicles(referenceGuid, vehicles);
            for (ObjectGuid const& vehicleGuid : vehicles)
            {
                if (!budget)
                    break;

                Creature* vehicle = map->GetCreature(vehicleGuid);
                if (!vehicle || !vehicle->IsAlive() || !IsControlSeatFree(vehicle) || IsVehicleGivenUp(vehicleGuid, nowMs))
                    continue;

                std::size_t best = living.size();
                float bestCost = std::numeric_limits<float>::max();
                for (std::size_t i = 0; i < living.size(); ++i)
                {
                    if (taken[i] || living[i]->GetVehicle())
                        continue;

                    float const cost = dutyCost(i, ObjectiveDuty::DriveSiege, breach->index, vehicleGuid,
                        vehicle->GetPosition());
                    if (cost < bestCost)
                    {
                        bestCost = cost;
                        best = i;
                    }
                }

                if (best == living.size() || bestCost > kVehicleClaimRange)
                    continue;

                assign(best, ObjectiveDuty::DriveSiege, breach->index, vehicleGuid);
                --budget;
            }

            // The rest fetch charges. A bot already holding one goes first: it
            // has done the fetching.
            uint32 const chargeItemId = plan.chargeItemId;
            if (chargeItemId)
            {
                for (std::size_t i = 0; i < living.size() && budget; ++i)
                {
                    if (taken[i] || !living[i]->HasItemCount(chargeItemId, 1))
                        continue;

                    assign(i, ObjectiveDuty::CarryCharge, breach->index, ObjectGuid::Empty);
                    --budget;
                }
            }

            if (hasCharge && !charge.Sources.empty())
            {
                std::vector<Position> sources;
                for (ObjectGuid const& sourceGuid : charge.Sources)
                    if (GameObject const* source = map->GetGameObject(sourceGuid))
                        sources.push_back(source->GetPosition());

                while (budget && !sources.empty())
                {
                    std::size_t best = living.size();
                    float bestCost = std::numeric_limits<float>::max();
                    for (std::size_t i = 0; i < living.size(); ++i)
                    {
                        if (taken[i])
                            continue;

                        // The trip is to the nearest source first.
                        float nearest = std::numeric_limits<float>::max();
                        for (Position const& source : sources)
                            nearest = std::min(nearest, living[i]->GetExactDist(&source));

                        float cost = nearest;
                        if (DutyAssignment const* had = previousDuty(living[i]);
                            had && had->duty == ObjectiveDuty::CarryCharge && had->targetId == breach->index)
                        {
                            cost -= kStickinessYards;
                            if (nowMs - had->sinceMs < kMinDutyHoldMs)
                                cost -= kHoldDiscount;
                        }

                        if (cost < bestCost)
                        {
                            bestCost = cost;
                            best = i;
                        }
                    }

                    if (best == living.size())
                        break;

                    assign(best, ObjectiveDuty::CarryCharge, breach->index, ObjectGuid::Empty);
                    --budget;
                }
            }

            std::ostringstream line;
            line << "breach gate " << breach->index << " (" << breach->threat << " enemies at it)";
            plan.summary.push_back(line.str());
        }

        // 3. Our guns: one gunner for each that has an enemy in its reach, the
        //    busiest first, up to a share of the team. A gunner already seated
        //    keeps its gun while there is anything near it to shoot at.
        GuidVector turretGuids;
        battleground->GetTurrets(referenceGuid, turretGuids);
        if (!turretGuids.empty() && livingCount && untakenCount())
        {
            struct TurretView
            {
                Creature* turret = nullptr;
                float reach = 0.0f;
                uint32 inReach = 0;
                uint32 nearReach = 0;
                bool manned = false;
            };

            std::vector<TurretView> turrets;
            for (ObjectGuid const& turretGuid : turretGuids)
            {
                Creature* turret = map->GetCreature(turretGuid);
                if (!turret || !turret->IsAlive() || IsVehicleGivenUp(turretGuid, nowMs))
                    continue;

                TurretView view;
                view.turret = turret;
                view.reach = playerbot::GetTurretReach(turret);
                if (view.reach <= 0.0f)
                    continue;

                for (Player const* enemy : enemies)
                {
                    float const distance = turret->GetExactDist(enemy);
                    if (distance <= view.reach)
                        ++view.inReach;
                    if (distance <= view.reach + kTurretKeepMargin)
                        ++view.nearReach;
                }

                if (view.nearReach)
                    turrets.push_back(view);
            }

            std::sort(turrets.begin(), turrets.end(), [](TurretView const& a, TurretView const& b)
                { return a.inReach > b.inReach; });

            uint32 budget = std::min(untakenCount(), share(kTurretSharePct));
            for (std::size_t i = 0; i < living.size() && budget; ++i)
            {
                if (taken[i])
                    continue;

                Creature const* seated = GetDrivenVehicle(living[i]);
                if (!seated)
                    continue;

                for (TurretView& view : turrets)
                {
                    if (view.turret != seated || view.manned)
                        continue;

                    view.manned = true;
                    assign(i, ObjectiveDuty::ManTurret, kNoTarget, seated->GetGUID());
                    --budget;
                    break;
                }
            }

            uint32 manned = 0;
            for (TurretView& view : turrets)
            {
                if (view.manned)
                {
                    ++manned;
                    continue;
                }
                if (!budget)
                    break;
                if (!view.inReach || !IsControlSeatFree(view.turret))
                    continue;

                std::size_t best = living.size();
                float bestCost = std::numeric_limits<float>::max();
                for (std::size_t i = 0; i < living.size(); ++i)
                {
                    if (taken[i] || living[i]->GetVehicle())
                        continue;

                    float const cost = dutyCost(i, ObjectiveDuty::ManTurret, kNoTarget, view.turret->GetGUID(),
                        view.turret->GetPosition());
                    if (cost < bestCost)
                    {
                        bestCost = cost;
                        best = i;
                    }
                }

                if (best == living.size() || bestCost > kVehicleClaimRange)
                    continue;

                view.manned = true;
                assign(best, ObjectiveDuty::ManTurret, kNoTarget, view.turret->GetGUID());
                --budget;
                ++manned;
            }

            if (manned)
            {
                std::ostringstream line;
                line << "man " << manned << " of " << turretGuids.size() << " guns";
                plan.summary.push_back(line.str());
            }
        }

        // 4. Our gates under siege. A side with bases to play sends what the
        //    threat asks for; a side with none spreads everybody over its
        //    gates, the threatened ones first and heaviest.
        std::vector<SiegeView const*> ours;
        for (SiegeView const& view : gates)
            if (view.objective.Friendly)
                ours.push_back(&view);

        if (!ours.empty() && livingCount)
        {
            std::sort(ours.begin(), ours.end(), [](SiegeView const* a, SiegeView const* b)
                { return a->threat > b->threat || (a->threat == b->threat && a->index < b->index); });

            if (teamHasBases)
            {
                for (SiegeView const* view : ours)
                    if (view->threat)
                        assignNearest(view->threat + 1, ObjectiveDuty::GuardGate, view->index, view->objective.Location);
            }
            else
            {
                uint32 const remaining = untakenCount();
                uint32 weightSum = 0;
                for (SiegeView const* view : ours)
                    weightSum += view->threat + 1;

                for (SiegeView const* view : ours)
                {
                    uint32 const quota = std::max<uint32>(1, remaining * (view->threat + 1) / weightSum);
                    assignNearest(quota, ObjectiveDuty::GuardGate, view->index, view->objective.Location);
                }

                // Rounding leftovers go where the enemy is.
                assignNearest(untakenCount(), ObjectiveDuty::GuardGate, ours.front()->index, ours.front()->objective.Location);
            }
        }

        // 5. The neutral flag of a base-capture match (Eye of the Storm). One
        //    bot takes it and two run down an enemy carrying it; the rest
        //    keep the bases, which are where that flag is scored.
        if (battleground->GetDynamicNodeCount() > 0 && livingCount)
        {
            ObjectGuid flagGuid;
            for (Player const* bot : living)
            {
                flagGuid = battleground->GetFlagPickupGUID(bot->GetGUID());
                if (!flagGuid.IsEmpty())
                    break;
            }

            // Only a bot the battleground would let take it: one that just
            // dropped it (Recently Dropped Flag) or sits under an immunity is
            // passed over, or nobody would go for it at all.
            if (!flagGuid.IsEmpty())
            {
                if (GameObject const* flag = map->GetGameObject(flagGuid))
                {
                    std::size_t best = living.size();
                    float bestCost = std::numeric_limits<float>::max();
                    for (std::size_t i = 0; i < living.size(); ++i)
                    {
                        if (taken[i] || battleground->GetFlagPickupGUID(living[i]->GetGUID()).IsEmpty())
                            continue;

                        float const cost = dutyCost(i, ObjectiveDuty::FlagRunner, 0, ObjectGuid::Empty, flag->GetPosition());
                        if (cost < bestCost)
                        {
                            bestCost = cost;
                            best = i;
                        }
                    }

                    if (best != living.size())
                        assign(best, ObjectiveDuty::FlagRunner, 0, ObjectGuid::Empty);
                }
            }

            ObjectGuid const carrierGuid = battleground->GetFlagPickerGUID();
            if (!carrierGuid.IsEmpty())
                if (Player* carrier = ObjectAccessor::GetPlayer(map, carrierGuid);
                    carrier && carrier->IsAlive() && ResolveTeam(battleground, carrier) != team &&
                    ResolveTeam(battleground, carrier) != TEAM_NEUTRAL)
                    assignNearest(kFlagHunters, ObjectiveDuty::FlagHunt, 0, carrier->GetPosition());
        }

        // A dead bot keeps the duty it had, so it walks back to the same work
        // after the spirit healer. Flag and vehicle duties are re-dealt live.
        for (Player const* bot : bots)
        {
            ObjectGuid const guid = bot->GetGUID();
            if (plan.assignments.count(guid))
                continue;

            DutyAssignment const* had = previousDuty(bot);
            if (!had || IsAliveParticipant(bot))
                continue;

            if (had->duty == ObjectiveDuty::AssaultBoss || had->duty == ObjectiveDuty::CarryCharge ||
                had->duty == ObjectiveDuty::GuardGate)
                plan.assignments[guid] = *had;
        }

        std::unordered_map<uint8, uint32> perDuty;
        for (auto const& assignment : plan.assignments)
            ++perDuty[uint8(assignment.second.duty)];

        if (!plan.assignments.empty())
        {
            std::ostringstream line;
            line << "duties:";
            for (uint8 duty = uint8(ObjectiveDuty::CarryCharge); duty <= uint8(ObjectiveDuty::ManTurret); ++duty)
                if (perDuty[duty])
                    line << " " << playerbot::GetObjectiveDutyName(ObjectiveDuty(duty)) << "=" << perDuty[duty];
            plan.summary.push_back(line.str());
        }
    }

    // A planned duty turned into orders, everything it points at read again
    // now: the plan is up to a rebuild old, so a gate that fell or a boss that
    // died inside that window sends nobody after it. False when the duty no
    // longer stands.
    bool ResolveDutyOrders(Battleground* battleground, TeamDutyPlan const& plan, DutyAssignment const& assignment,
        Player const* bot, playerbot::ObjectiveBotOrders& orders)
    {
        ObjectGuid const botGuid = bot->GetGUID();
        switch (assignment.duty)
        {
            case ObjectiveDuty::CarryCharge:
            case ObjectiveDuty::DriveSiege:
            case ObjectiveDuty::GuardGate:
            {
                orders.siegeId = assignment.targetId;
                orders.vehicleGuid = assignment.vehicleGuid;

                BattlegroundSiegeObjective objective;
                if (!battleground->GetSiegeObjectiveInfo(botGuid, assignment.targetId, objective) || !objective.Breachable)
                {
                    // A driver whose gate just fell keeps its seat and waits for
                    // the next rebuild to point it at the next gate, instead of
                    // climbing out and back in.
                    Unit const* vehicle = bot->GetVehicleBase();
                    if (assignment.duty != ObjectiveDuty::DriveSiege || !vehicle || vehicle->GetGUID() != assignment.vehicleGuid)
                        return false;
                    break;
                }

                orders.gateGuid = objective.GateGuid;
                orders.gateLocation = objective.Location;

                if (assignment.duty == ObjectiveDuty::CarryCharge)
                {
                    BattlegroundSiegeChargeInfo charge;
                    if (battleground->GetSiegeChargeInfo(botGuid, charge) && charge.ItemId)
                    {
                        orders.chargeItemId = charge.ItemId;
                        orders.chargeSources = std::move(charge.Sources);
                    }
                    else
                        orders.chargeItemId = plan.chargeItemId;

                    // Nothing to fetch and nothing in the bag: the duty is over.
                    if (!orders.chargeItemId ||
                        (orders.chargeSources.empty() && !bot->HasItemCount(orders.chargeItemId, 1)))
                        return false;
                }
                break;
            }
            case ObjectiveDuty::AssaultBoss:
            {
                BattlegroundBossObjective boss;
                if (!battleground->GetBossObjectiveInfo(botGuid, assignment.targetId, boss) || boss.Friendly ||
                    !boss.Engageable)
                    return false;

                orders.bossGuid = boss.BossGuid;
                orders.bossHasApproach = boss.HasApproach;
                orders.bossApproach = boss.Approach;
                break;
            }
            case ObjectiveDuty::FlagRunner:
                orders.flagGuid = battleground->GetFlagPickupGUID(botGuid);
                if (orders.flagGuid.IsEmpty())
                    return false;
                break;
            case ObjectiveDuty::FlagHunt:
                orders.flagCarrierGuid = battleground->GetFlagPickerGUID();
                if (orders.flagCarrierGuid.IsEmpty())
                    return false;
                break;
            case ObjectiveDuty::ManTurret:
            {
                // Still standing, and its seat empty or this bot's own: a player
                // who climbed in first has the gun.
                Map* map = bot->FindMap();
                Creature* turret = map ? map->GetCreature(assignment.vehicleGuid) : nullptr;
                if (!turret || !turret->IsAlive() || (bot->GetVehicleBase() != turret && !IsControlSeatFree(turret)))
                    return false;

                orders.vehicleGuid = assignment.vehicleGuid;
                break;
            }
            case ObjectiveDuty::None:
            default:
                return false;
        }

        orders.duty = assignment.duty;
        return true;
    }

    // The plan for the team this player is on, brought up to date. Null
    // outside an in-progress match with objectives.
    TeamDutyPlan* GetFreshPlan(Battleground* battleground, Map* map, TeamId team, ObjectGuid referenceGuid)
    {
        if (!playerbot::ObjectiveCoordinator::HasObjectives(battleground) ||
            battleground->GetStatus() != STATUS_IN_PROGRESS || !map ||
            map->GetInstanceId() != battleground->GetInstanceID() || team == TEAM_NEUTRAL)
            return nullptr;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        TeamDutyPlan& plan = TouchTeamPlan(BuildPlanKey(battleground, team), nowMs);
        if (plan.lastRefreshMs == 0 || nowMs - plan.lastRefreshMs >= kPlanRefreshMs)
            RefreshTeamPlan(battleground, map, team, referenceGuid, plan, nowMs);

        return &plan;
    }
}

namespace playerbot
{
char const* GetObjectiveDutyName(ObjectiveDuty duty)
{
    switch (duty)
    {
        case ObjectiveDuty::CarryCharge: return "carry-charge";
        case ObjectiveDuty::DriveSiege:  return "drive-siege";
        case ObjectiveDuty::GuardGate:   return "guard-gate";
        case ObjectiveDuty::AssaultBoss: return "assault-boss";
        case ObjectiveDuty::FlagRunner:  return "flag-runner";
        case ObjectiveDuty::FlagHunt:    return "flag-hunt";
        case ObjectiveDuty::ManTurret:   return "man-turret";
        case ObjectiveDuty::None:
        default:                         return "none";
    }
}

bool ObjectiveCoordinator::HasObjectives(Battleground const* battleground)
{
    // Base battlegrounds are included because a neutral flag can only be told
    // apart by asking a player; one without (Arathi Basin) just never deals a
    // duty.
    return battleground && !battleground->isArena() &&
        (battleground->GetSiegeObjectiveCount() > 0 || battleground->GetBossObjectiveCount() > 0 ||
            battleground->GetDynamicNodeCount() > 0);
}

bool ObjectiveCoordinator::GetOrders(Player const* bot, ObjectiveBotOrders& orders)
{
    orders = ObjectiveBotOrders();
    if (!bot || !bot->IsInWorld() || !bot->InBattleground())
        return false;

    Battleground* battleground = bot->GetBattleground();
    if (!battleground)
        return false;

    TeamDutyPlan* plan = GetFreshPlan(battleground, bot->FindMap(), ResolveTeam(battleground, bot), bot->GetGUID());
    if (!plan)
        return false;

    auto const assignmentItr = plan->assignments.find(bot->GetGUID());
    if (assignmentItr == plan->assignments.end())
        return false;

    if (!ResolveDutyOrders(battleground, *plan, assignmentItr->second, bot, orders))
    {
        orders = ObjectiveBotOrders();
        return false;
    }

    return true;
}

bool ObjectiveCoordinator::IsOnDuty(Battleground* battleground, Map* map, TeamId team, ObjectGuid referenceGuid,
    ObjectGuid botGuid)
{
    if (!battleground)
        return false;

    TeamDutyPlan* plan = GetFreshPlan(battleground, map, team, referenceGuid);
    if (!plan)
        return false;

    auto const itr = plan->assignments.find(botGuid);
    if (itr == plan->assignments.end() || itr->second.duty == ObjectiveDuty::None)
        return false;

    // Only a duty that still stands keeps a bot off the bases: one whose orders
    // no longer resolve would otherwise leave it with nothing to do at all.
    Player const* bot = ObjectAccessor::GetPlayer(map, botGuid);
    ObjectiveBotOrders orders;
    return bot && ResolveDutyOrders(battleground, *plan, itr->second, bot, orders);
}

void ObjectiveCoordinator::GiveUpVehicle(ObjectGuid vehicleGuid)
{
    playerbot::LockedSet(g_VehicleGiveUpUntilMs, vehicleGuid, GameTime::GetGameTimeMS() + kVehicleGiveUpMs);
}

void ObjectiveCoordinator::LoadConfig()
{
    g_SiegeVehiclesEnabled.store(sConfigMgr->GetBoolDefault("Playerbot.BgObjectives.SiegeVehicles", true),
        std::memory_order_relaxed);
}

int8 GetVehicleControlSeat(Unit const* vehicle)
{
    Vehicle const* kit = vehicle ? vehicle->GetVehicleKit() : nullptr;
    if (!kit)
        return -1;

    for (SeatMap::value_type const& seat : kit->Seats)
        if (seat.second.SeatInfo && seat.second.SeatInfo->HasFlag(VEHICLE_SEAT_FLAG_CAN_CONTROL))
            return seat.first;

    return -1;
}

void CollectTurretSpells(Creature const* turret, std::vector<SpellInfo const*>& spells)
{
    if (!turret)
        return;

    auto const hurtsPeople = [](SpellInfo const* spellInfo)
    {
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_SCHOOL_DAMAGE))
                return true;
        return false;
    };

    for (uint32 spellId : turret->m_spells)
    {
        SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!spellInfo)
            continue;

        // Directly, or by the missile or spell it launches (Rocket Blast).
        bool hurts = hurtsPeople(spellInfo);
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (!hurts && (effect.IsEffect(SPELL_EFFECT_TRIGGER_MISSILE) || effect.IsEffect(SPELL_EFFECT_TRIGGER_SPELL)) &&
                effect.TriggerSpell)
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                    hurts = hurtsPeople(triggered);

        if (hurts)
            spells.push_back(spellInfo);
    }
}

float GetTurretReach(Creature const* turret)
{
    std::vector<SpellInfo const*> spells;
    CollectTurretSpells(turret, spells);

    float reach = 0.0f;
    for (SpellInfo const* spellInfo : spells)
        reach = std::max(reach, spellInfo->GetMaxRange(false));
    return reach;
}

std::vector<std::string> ObjectiveCoordinator::DescribeTeams(Player const* observer)
{
    std::vector<std::string> lines;
    if (!observer || !observer->InBattleground())
        return lines;

    Battleground* battleground = observer->GetBattleground();
    if (!HasObjectives(battleground))
    {
        lines.emplace_back("No siege, commander or neutral flag in this battleground.");
        return lines;
    }

    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        TeamDutyPlan const* plan = LockedFind(g_DutyPlans, BuildPlanKey(battleground, team));
        if (!plan)
            continue;

        std::ostringstream header;
        header << (team == TEAM_ALLIANCE ? "Alliance" : "Horde") << ": " << plan->livingBots << " bots up, "
            << plan->assignments.size() << " on duty.";
        lines.push_back(header.str());
        for (std::string const& line : plan->summary)
            lines.push_back("  " + line);
    }

    if (lines.empty())
        lines.emplace_back("No duty plan yet: no bots have evaluated this match.");

    return lines;
}
}
