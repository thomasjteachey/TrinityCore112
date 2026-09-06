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

// Bounty: killing people makes you worth killing. See custom_bounty.h for what
// the system is for; this file is how it works.

#include "custom_bounty.h"
#include "custom_notoriety.h"

#include "custom_barracks_hardcore.h"
#include "custom_loot_chest_helper.h"

#include "CombatManager.h"
#include "Configuration/Config.h"
#include "Duration.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "ObjectMgr.h"
#include "RBAC.h"
#include "WorldSession.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "ScriptedCreature.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "World.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Bounty
{
namespace
{
    bool s_enabled = false;
    uint32 s_spellId = 0;
    uint32 s_stacksPerKill = 1;
    float s_goldPercentPerStack = 1.0f;
    // The flat cost of dying, owed with or without a bounty. See TakeDeathTax.
    float s_deathTaxPercent = 10.0f;

    // And the bot version of it, which is progressive rather than flat.
    //
    // A person's purse is spent: on repairs, on reagents, on the auction house.
    // A bot's is not - it earns constantly, buys narrowly, and has nothing to
    // save for, so a flat rate that is fair to a person barely dents a bot with
    // two thousand gold. Brackets fix that without touching the poor end of the
    // fleet, where taking half of eight gold would stop a bot ever affording the
    // gear the economy expects it to buy.
    //
    // MARGINAL, in the way an income tax is: each slice of the purse is taxed at
    // its own rate, so there is no cliff and no point at which earning one more
    // copper costs a bot money. A bot on 2000g pays the top rate only on the
    // part above the last threshold.
    struct BotTaxBracket
    {
        uint64 UpToCopper = 0;   // 0 = everything above the previous threshold
        float Percent = 0.0f;
    };
    std::vector<BotTaxBracket> s_botTaxBrackets;
    float s_botLossMultiplier = 2.0f;
    uint32 s_chestEntry = 0;
    uint32 s_chestDespawnSeconds = 600;
    float s_huntRadiusBonusYards = 175.0f;
    uint32 s_drifterZoneBonus = 10;
    int32 s_aggroSlotBonus = 8;
    uint32 s_ignoreAggroBudgetStacks = 40;
    uint32 s_relentlessStacks = 10;
    uint32 s_relentlessIntervalSeconds = 30;
    uint32 s_pairUpStacks = 15;
    uint32 s_veteranStacks = 25;
    uint32 s_pvpBotStacks = 50;
    uint32 s_maxHuntersOnTarget = 6;
    uint32 s_guardStacks = 30;
    uint32 s_guardEntry = 900200;
    uint32 s_guardCount = 2;
    uint32 s_guardIntervalSeconds = 45;
    uint32 s_guardLifetimeSeconds = 180;

    // What a stack is worth, held here so the registry and the aura cannot
    // disagree about the ceiling: the DBC's CumulativeAura is the authority and
    // this is only a fallback for a spell that failed to load.
    uint32 s_fallbackMaxStacks = 50;

    // ----------------------------------------------------------------------
    // The registry.
    //
    // The aura is what the player sees; this is what the playerbot manager
    // reads. It has to exist separately because that manager wants a bounty
    // figure once a second from the WORLD thread, and walking a Unit's aura map
    // from there while the map thread may be writing it is a data race - the
    // same reason the human-position snapshot exists at all.
    //
    // Both are written together, at the same instants, on the map thread.
    // Expiry is carried as a deadline rather than swept: a reader past it sees
    // zero, which matches an aura that has quietly timed out, and no timer has
    // to be maintained to make that true.
    // ----------------------------------------------------------------------
    struct Record
    {
        uint32 Stacks = 0;
        time_t ExpiresAt = 0;
    };

    // Coin a death owes but has not yet paid, waiting for something to summon
    // the chest that will hold it. See TakePendingChestGold for why the debt is
    // recorded rather than collected on the spot.
    struct Debt
    {
        uint32 ChestCopper = 0;   // reaches the chest
        uint32 BurnCopper = 0;    // additionally destroyed (bots only)
    };

    std::mutex g_lock;
    std::unordered_map<uint64, Record> g_stacks;
    std::unordered_map<uint64, Debt> g_debts;
    // Guards summoned after each victim, so they can be dismissed the moment
    // the bounty is settled rather than loitering out their timer.
    std::unordered_map<uint64, std::vector<ObjectGuid>> g_guards;

    uint32 MaxStacks()
    {
        if (SpellInfo const* info = s_spellId ? sSpellMgr->GetSpellInfo(s_spellId) : nullptr)
            if (info->StackAmount)
                return info->StackAmount;

        return s_fallbackMaxStacks;
    }

    void LoadBountyConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.Bounty.Enable", false);
        s_spellId = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Bounty.AuraSpell", 90701)));
        s_stacksPerKill = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Bounty.StacksPerKill", 1), 0, 50));
        s_fallbackMaxStacks = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Bounty.MaxStacks", 50), 1, 255));
        s_goldPercentPerStack = std::clamp(
            sConfigMgr->GetFloatDefault("Centurion.Bounty.GoldPercentPerStack", 1.0f), 0.0f, 10.0f);
        s_botLossMultiplier = std::clamp(
            sConfigMgr->GetFloatDefault("Centurion.Bounty.BotLossMultiplier", 2.0f), 1.0f, 10.0f);
        s_deathTaxPercent = std::clamp(
            sConfigMgr->GetFloatDefault("Centurion.Hardcore.DeathGoldLossPercent", 10.0f), 0.0f, 100.0f);

        // "upToGold:percent" pairs, cheapest bracket first. An upTo of 0 means
        // "and everything above", and must be last; without one the purse above
        // the final threshold is simply untaxed.
        s_botTaxBrackets.clear();
        {
            std::string const spec = sConfigMgr->GetStringDefault(
                "Centurion.Hardcore.BotDeathTaxBrackets", "100:10,500:20,1000:35,2000:50,0:65");
            std::stringstream stream(spec);
            std::string token;
            while (std::getline(stream, token, ','))
            {
                size_t const colon = token.find(':');
                if (colon == std::string::npos)
                    continue;

                BotTaxBracket bracket;
                uint64 const gold = std::strtoull(token.substr(0, colon).c_str(), nullptr, 10);
                bracket.UpToCopper = gold ? gold * 10000ull : std::numeric_limits<uint64>::max();
                bracket.Percent = std::clamp(float(std::atof(token.substr(colon + 1).c_str())), 0.0f, 100.0f);
                s_botTaxBrackets.push_back(bracket);
            }

            // Ascending, because the marginal walk below depends on it and an
            // operator listing them out of order should get the tax they meant
            // rather than a silently wrong one.
            std::sort(s_botTaxBrackets.begin(), s_botTaxBrackets.end(),
                [](BotTaxBracket const& a, BotTaxBracket const& b) { return a.UpToCopper < b.UpToCopper; });

            std::ostringstream describe;
            uint64 previous = 0;
            for (BotTaxBracket const& bracket : s_botTaxBrackets)
            {
                describe << (describe.tellp() ? ", " : "") << (previous / 10000) << "g-";
                if (bracket.UpToCopper == std::numeric_limits<uint64>::max())
                    describe << "up";
                else
                    describe << (bracket.UpToCopper / 10000) << "g";
                describe << " @ " << bracket.Percent << "%";
                previous = bracket.UpToCopper;
            }
            TC_LOG_INFO("playerbots.hardcore", "Bot death tax brackets: {}",
                s_botTaxBrackets.empty() ? "none (bots pay the flat rate)" : describe.str());
        }

        // Defaults to the hardcore death chest, so a bounty payout and a gear
        // payout are the same object in the world and bots already know to go
        // and open it.
        s_chestEntry = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Bounty.ChestGameObjectId",
            sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestGameObjectId", 0))));
        s_chestDespawnSeconds = uint32(std::max(30, sConfigMgr->GetIntDefault("Centurion.Bounty.ChestDespawnSeconds",
            sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestDespawnSeconds", 600))));

        s_huntRadiusBonusYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Centurion.Bounty.HuntRadiusBonusYards", 175.0f));
        s_drifterZoneBonus = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.DrifterZoneBonus", 10), 0, 100));
        s_aggroSlotBonus = int32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.AggroSlotBonus", 8), 0, 200));
        s_ignoreAggroBudgetStacks = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.IgnoreAggroBudgetStacks", 40), 0, 255));
        s_relentlessStacks = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.RelentlessStacks", 10), 0, 255));
        s_relentlessIntervalSeconds = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.RelentlessIntervalSeconds", 30), 5, 3600));
        s_pairUpStacks = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.PairUpStacks", 15), 0, 255));
        s_veteranStacks = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.VeteranStacks", 25), 0, 255));
        s_pvpBotStacks = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.PvpBotStacks", 50), 0, 255));
        s_maxHuntersOnTarget = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.MaxHuntersOnTarget", 6), 1, 100));
        s_guardStacks = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.GuardStacks", 30), 0, 255));
        s_guardEntry = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Centurion.Bounty.GuardCreatureId", 900200)));
        s_guardCount = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.GuardCount", 2), 0, 20));
        s_guardIntervalSeconds = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.GuardIntervalSeconds", 45), 10, 3600));
        s_guardLifetimeSeconds = uint32(std::clamp(
            sConfigMgr->GetIntDefault("Centurion.Bounty.GuardLifetimeSeconds", 180), 30, 3600));

        TC_LOG_INFO("playerbots.hardcore",
            "Bounty config: enabled={} spell={} perKill={} goldPerStack={}% botMultiplier={} chest={}",
            uint32(s_enabled), s_spellId, s_stacksPerKill, s_goldPercentPerStack,
            s_botLossMultiplier, s_chestEntry);
    }

    // Somewhere a bounty means something.
    //
    // The Battle Ring, The Maul and every arena-flagged area are built for
    // people to kill each other in; a price on your head for doing the thing
    // the zone exists for would be nonsense, and the ring already settles up
    // through its own chest. Battlegrounds and arenas are the same argument
    // with a scoreboard attached.
    bool IsBountyContext(Player const* player)
    {
        if (!s_enabled || !s_spellId || !player || player->IsGameMaster())
            return false;

        if (player->InBattleground() || player->InArena())
            return false;

        if (player->IsInGurubashiBattleRing())
            return false;

        // IsInFFAPvPAreaByMap, NOT IsInFFAPvPArea. The latter is reassigned every
        // tick by the hardcore ruleset to mean 'this unit is FFA armed', and every
        // playerbot is permanently armed - so reading it here asked "is this a
        // bot" and refused a bounty for every bot kill on the realm, which is the
        // one case the whole feature exists for.
        return !player->pvpInfo.IsInFFAPvPAreaByMap;
    }

    void WriteRegistry(Player const* player, uint32 stacks, int32 durationMs)
    {
        uint64 const key = player->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> guard(g_lock);
        if (!stacks)
        {
            g_stacks.erase(key);
            return;
        }

        Record& record = g_stacks[key];
        record.Stacks = stacks;
        record.ExpiresAt = GameTime::GetGameTime() + std::max<int32>(1, durationMs / IN_MILLISECONDS);
    }

    // Put the aura on somebody, or push the one they already carry up a step
    // and start its fifteen minutes again.
    void AddStacks(Player* killer, uint32 count)
    {
        if (!count || !IsBountyContext(killer))
            return;

        // War Mode off is out of the PvP economy in both directions: you are
        // not hunted, and you do not build a price on your head either. The
        // flagger already refuses to disarm a bountied player, so together
        // these two make "bountied" and "War Mode off" mutually exclusive -
        // which is what keeps the fleet from being dispatched at somebody it
        // is then forbidden to touch.
        //
        // Bots are exempt, and must be: they have no War Mode setting to opt
        // into, so a bare opted-in test would deny a bounty to every bot on the
        // realm - and a bot's own bounty is exactly what it pays out when a
        // player finally puts it down. This is a rule about people opting out.
        if (!BarracksHardcore::IsPlayerbot(killer) && !BarracksHardcore::IsWarModeOptedIn(killer))
            return;

        Aura* aura = killer->GetAura(s_spellId, killer->GetGUID());
        if (!aura)
        {
            aura = killer->AddAura(s_spellId, killer);
            if (!aura)
            {
                TC_LOG_ERROR("playerbots.hardcore",
                    "Bounty: spell {} would not apply to {} - is it in Spell.dbc?",
                    s_spellId, killer->GetName());
                return;
            }

            // A fresh application already counts as the first stack.
            --count;
        }

        // ModStackAmount caps at the spell's own StackAmount and restarts the
        // duration on the way up - including AT the ceiling, where the stack no
        // longer moves but the clock still resets. That is the "refreshes on
        // every kill" rule, and it comes for free.
        if (count)
            aura->ModStackAmount(int32(count));

        WriteRegistry(killer, aura->GetStackAmount(), aura->GetDuration());

        // Map thread, player in hand, on the exact instruction that raises the
        // stack. A live contract banks its high-water mark here.
        Notoriety::OnStacksChanged(killer, aura->GetStackAmount());

        TC_LOG_DEBUG("playerbots.hardcore", "Bounty: {} now at {} stack(s).",
            killer->GetName(), uint32(aura->GetStackAmount()));
    }

    // Called the moment the bounty is settled. The guards were an escalation of
    // a debt that no longer exists, and they are hostile to EVERYONE - leaving
    // them standing over the corpse would mean whoever collected the bounty then
    // has to fight the collectors.
    //
    // The summon's own timed despawn stays as the backstop, so a guard whose
    // target logged out mid-fight still leaves on its own.
    void DismissGuards(Player* victim)
    {
        if (!victim)
            return;

        std::vector<ObjectGuid> guards;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            auto const itr = g_guards.find(victim->GetGUID().GetRawValue());
            if (itr == g_guards.end())
                return;

            guards.swap(itr->second);
            g_guards.erase(itr);
        }

        uint32 dismissed = 0;
        for (ObjectGuid const& guid : guards)
            if (Creature* guard = ObjectAccessor::GetCreature(*victim, guid))
            {
                guard->DespawnOrUnsummon();
                ++dismissed;
            }

        if (dismissed)
            TC_LOG_INFO("playerbots.hardcore", "Bounty: {} Centurion Guard(s) dismissed after {} died.",
                dismissed, victim->GetName());
    }

    // ClearBounty is declared in custom_bounty.h at namespace Bounty scope and
    // defined below, outside this anonymous namespace: the Notoriety turn-in
    // calls it, so it needs external linkage. Do NOT re-declare it in here -
    // that names Bounty::{anonymous}::ClearBounty, a different symbol, and the
    // callers below bind to it and fail at link.

    // Everyone who was fighting the victim when they went down.
    //
    // The killing blow is one name on a list, not the list: a rogue who opened,
    // a mage who did most of the damage and a warrior who landed the last hit
    // all took part, and crediting only the last of them would make the whole
    // mechanic a matter of who got the timing right.
    //
    // Combat references are the honest record of that, and the only reason this
    // is readable at all is that OnPlayerJustDied fires before CombatStop. Both
    // maps are walked: the PvP map is where a player-versus-player fight lives,
    // but a bot's pet or a charmed unit can hold a PvE reference to the same
    // victim, and resolving through GetCharmerOrOwnerPlayerOrPlayerItself puts
    // the credit on the person behind it either way.
    std::vector<Player*> CollectParticipants(Player* victim, Unit* killer)
    {
        std::vector<Player*> participants;
        std::unordered_set<uint64> seen;

        auto const consider = [&](Unit* unit)
        {
            if (!unit)
                return;

            Player* person = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (!person || person == victim || !person->IsInWorld())
                return;

            if (!seen.insert(person->GetGUID().GetRawValue()).second)
                return;

            participants.push_back(person);
        };

        CombatManager const& combat = victim->GetCombatManager();
        for (auto const& reference : combat.GetPvPCombatRefs())
            consider(reference.second->GetOther(victim));
        for (auto const& reference : combat.GetPvECombatRefs())
            consider(reference.second->GetOther(victim));

        // The killing blow always counts, whether or not a reference survived
        // to say so.
        consider(killer);
        return participants;
    }

    // What this death costs, worked out while the victim still has the aura.
    //
    // Bots pay the multiplier on top: the share that reaches the chest is the
    // same, and an equal share again is destroyed. At the cap that is half a
    // bot's gold to the finder and the other half out of the economy entirely,
    // which is the point - a bot on a killing spree is a gold sink, not a
    // transfer.
    // The plain cost of dying, owed by everybody.
    //
    // The bounty debt below only exists for somebody carrying stacks, so until
    // now a death cost an unbountied character nothing at all - and on a realm
    // where the fleet dies constantly that is a very large tap with no drain
    // under it. This is the drain: a flat share of the purse on every death,
    // bounty or no bounty.
    //
    // Taken BEFORE the bounty debt is worked out, so the two do not compound
    // into more than the percentages say. The bounty's share is then a share of
    // what is actually left, which is also the order a player would expect: the
    // house takes its cut, then the killer takes theirs.
    //
    // It is a SINK. The money is destroyed rather than added to the chest -
    // nobody picks this up, it simply leaves the economy. That is the point: it
    // is the counterweight to every copper the fleet mints by grinding.
    // Walk the brackets, taxing each slice at its own rate.
    uint64 ProgressiveBotTaxCopper(uint64 money)
    {
        uint64 taken = 0;
        uint64 floorCopper = 0;

        for (BotTaxBracket const& bracket : s_botTaxBrackets)
        {
            if (money <= floorCopper)
                break;

            uint64 const ceilingCopper = std::min(money, bracket.UpToCopper);
            if (ceilingCopper > floorCopper)
                taken += uint64(double(ceilingCopper - floorCopper) * double(bracket.Percent) / 100.0);

            floorCopper = bracket.UpToCopper;
        }

        return std::min(taken, money);
    }

    uint32 TakeDeathTax(Player* victim)
    {
        if (!victim)
            return 0;

        uint64 const money = victim->GetMoney();
        if (!money)
            return 0;

        // A bot pays the progressive schedule; a person pays the flat rate.
        bool const bot = BarracksHardcore::IsPlayerbot(victim);
        bool const progressive = bot && !s_botTaxBrackets.empty();
        if (!progressive && s_deathTaxPercent <= 0.0f)
            return 0;

        uint64 const owed = progressive
            ? ProgressiveBotTaxCopper(money)
            : uint64(double(money) * double(s_deathTaxPercent) / 100.0);

        uint32 const taxed = uint32(std::min<uint64>(money, owed));
        if (!taxed)
            return 0;

        victim->ModifyMoney(-int64(taxed));

        TC_LOG_INFO("playerbots.hardcore",
            "Death tax: {} lost {}c of {}c ({:.1f}% effective, {}) on death.",
            victim->GetName(), taxed, money,
            100.0 * double(taxed) / double(money),
            progressive ? "progressive" : "flat");
        return taxed;
    }

    void RecordDeathDebt(Player* victim)
    {
        uint32 const stacks = GetStacks(victim);
        if (!stacks)
            return;

        uint64 const money = victim->GetMoney();
        if (money)
        {
            float const percent = std::min(100.0f, float(stacks) * s_goldPercentPerStack);
            uint32 const chestCopper = uint32(double(money) * double(percent) / 100.0);

            uint32 burnCopper = 0;
            if (BarracksHardcore::IsPlayerbot(victim))
            {
                uint64 const total = std::min<uint64>(money,
                    uint64(double(chestCopper) * double(s_botLossMultiplier)));
                burnCopper = uint32(total - std::min<uint64>(total, chestCopper));
            }

            if (chestCopper || burnCopper)
            {
                std::lock_guard<std::mutex> guard(g_lock);
                Debt& debt = g_debts[victim->GetGUID().GetRawValue()];
                debt.ChestCopper = chestCopper;
                debt.BurnCopper = burnCopper;
            }

            TC_LOG_INFO("playerbots.hardcore",
                "Bounty: {} died at {} stack(s) owing {}c to a chest and burning {}c.",
                victim->GetName(), stacks, chestCopper, burnCopper);
        }

        ClearBounty(victim);
    }

    // Thirty stacks: the realm stops sending people and sends guards.
    //
    // MAP thread, from PlayerScript::OnUpdate. That is deliberate and necessary:
    // summoning into a map is a map-thread operation, and the rest of the bounty
    // escalation lives on the world thread only because it reads Group and queues
    // teleports. This needs neither - it wants the player it is already handed.
    //
    // They are summoned at a distance and walk in, rather than materialising on
    // top of somebody, and they despawn on their own timer so nothing has to keep
    // a register of them.
    void SummonGuardsFor(Player* player)
    {
        // Cheap deadline test FIRST. OnUpdate runs every tick for every player on
        // the realm, so almost every call must end on this line.
        static std::unordered_map<uint64, time_t> s_nextGuardAt;
        uint64 const key = player->GetGUID().GetRawValue();
        time_t const now = GameTime::GetGameTime();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto const itr = s_nextGuardAt.find(key);
            if (itr != s_nextGuardAt.end() && now < itr->second)
                return;
        }

        uint32 const stacks = GetStacks(player);
        if (!SummonsGuards(stacks) || !IsBountyContext(player) || !player->IsAlive())
            return;

        {
            std::lock_guard<std::mutex> guard(g_lock);
            s_nextGuardAt[key] = now + s_guardIntervalSeconds;
        }

        uint32 summoned = 0;
        for (uint32 i = 0; i < s_guardCount; ++i)
        {
            // Spread around the target rather than stacked on one side, and far
            // enough out that the arrival is seen coming.
            float const angle = float(i) * (2.0f * float(M_PI) / float(std::max<uint32>(1, s_guardCount)))
                + frand(0.0f, 1.0f);
            float x, y, z;
            player->GetNearPoint(player, x, y, z, 25.0f, angle);
            player->UpdateAllowedPositionZ(x, y, z);

            Creature* guard = player->SummonCreature(s_guardEntry, x, y, z,
                player->GetAbsoluteAngle(x, y) + float(M_PI),
                TEMPSUMMON_TIMED_DESPAWN, Milliseconds(s_guardLifetimeSeconds * IN_MILLISECONDS));
            if (!guard)
                continue;

            // As big as the zone, not the 55 the template inherited from the
            // Orgrimmar Grunt it was cloned from - which made a guard in Westfall
            // unfightable and one in the Plaguelands beneath notice.
            //
            // NOT capped to the victim's own level: somebody who has run their
            // bounty up in a zone far above them gets that zone's answer, which
            // is the correct one.
            //
            // UpdateLevelDependantStats must follow SetLevel and reads GetLevel()
            // to regenerate health, mana and damage from the creature base stats
            // table. Setting the level alone leaves a level 40 guard carrying a
            // level 55 health pool.
            if (uint8 const zoneLevel = BarracksHardcore::ZoneTopLevel(player->GetZoneId()))
            {
                guard->SetLevel(zoneLevel);
                guard->UpdateLevelDependantStats();
            }

            ++summoned;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                g_guards[key].push_back(guard->GetGUID());
            }

            if (guard->IsAIEnabled())
                guard->AI()->AttackStart(player);
        }

        if (summoned)
            TC_LOG_INFO("playerbots.hardcore",
                "Bounty {}: {} Centurion Guard(s) at level {} sent after {}.",
                stacks, summoned, uint32(BarracksHardcore::ZoneTopLevel(player->GetZoneId())),
                player->GetName());
    }

    // Somewhere for the coin to go when nothing else builds a chest.
    //
    // The hardcore ruleset already summons one for gear on most deaths and
    // takes the money on its way past, so this only runs for the deaths it does
    // not cover - a fall, drowning, lava, or a realm with hardcore switched
    // off entirely.
    void SummonBountyChestIfOwed(Player* victim)
    {
        if (!victim || !s_chestEntry)
            return;

        CustomLootChests::PlayerChestBuilder chest(victim, s_chestEntry, Seconds(s_chestDespawnSeconds));
        chest.AddMoney(TakePendingChestGold(victim));
        if (chest.HasLoot())
            chest.Summon();
    }
}

void ClearBounty(Player* player)
{
    if (!player)
        return;

    if (s_spellId)
        player->RemoveAurasDueToSpell(s_spellId);

    std::lock_guard<std::mutex> guard(g_lock);
    g_stacks.erase(player->GetGUID().GetRawValue());
}

bool Enabled()
{
    return s_enabled;
}

uint32 SpellId()
{
    return s_spellId;
}

uint32 GetStacks(ObjectGuid guid)
{
    if (!s_enabled)
        return 0;

    time_t const now = GameTime::GetGameTime();
    std::lock_guard<std::mutex> guard(g_lock);
    auto const itr = g_stacks.find(guid.GetRawValue());
    if (itr == g_stacks.end())
        return 0;

    return itr->second.ExpiresAt > now ? itr->second.Stacks : 0;
}

uint32 GetStacks(Player const* player)
{
    return player ? GetStacks(player->GetGUID()) : 0;
}

float Fraction(uint32 stacks)
{
    uint32 const cap = MaxStacks();
    if (!cap || !stacks)
        return 0.0f;

    return std::min(1.0f, float(stacks) / float(cap));
}

float HuntRadiusBonusYards(uint32 stacks)
{
    return Fraction(stacks) * s_huntRadiusBonusYards;
}

uint32 DrifterZoneBonus(uint32 stacks)
{
    return uint32(Fraction(stacks) * float(s_drifterZoneBonus) + 0.5f);
}

int32 AggroSlotBonus(uint32 stacks)
{
    return int32(Fraction(stacks) * float(s_aggroSlotBonus) + 0.5f);
}

bool IgnoresAggroBudget(uint32 stacks)
{
    return s_enabled && s_ignoreAggroBudgetStacks && stacks >= s_ignoreAggroBudgetStacks;
}

bool IsHuntedRelentlessly(uint32 stacks)
{
    return s_enabled && s_relentlessStacks && stacks >= s_relentlessStacks;
}

uint32 HuntersPerWave(uint32 stacks)
{
    return (s_pairUpStacks && stacks >= s_pairUpStacks) ? 2u : 1u;
}

uint32 RelentlessIntervalSeconds(uint32 stacks)
{
    // Thickens with the bounty: the configured interval at the threshold, down
    // to a third of it at the cap. Never below five seconds, because the arrival
    // still has to walk in from 210 yards and a faster clock would only queue
    // bots behind each other.
    float const scale = 1.0f - (2.0f / 3.0f) * Fraction(stacks);
    return std::max<uint32>(5, uint32(float(s_relentlessIntervalSeconds) * scale));
}

bool DrawsFromVeterans(uint32 stacks)
{
    return s_enabled && s_veteranStacks && stacks >= s_veteranStacks;
}

bool DrawsFromPvpBots(uint32 stacks)
{
    return s_enabled && s_pvpBotStacks && stacks >= s_pvpBotStacks;
}

uint32 MaxHuntersOnTarget(uint32 stacks)
{
    // Grows with the bounty, so the ceiling escalates alongside everything
    // else instead of capping the top rungs at the bottom rung's crowd.
    return s_maxHuntersOnTarget + uint32(Fraction(stacks) * float(s_maxHuntersOnTarget));
}

bool SummonsGuards(uint32 stacks)
{
    return s_enabled && s_guardStacks && s_guardEntry && s_guardCount &&
        stacks >= s_guardStacks;
}

uint32 TakePendingChestGold(Player* victim)
{
    if (!victim)
        return 0;

    Debt debt;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto const itr = g_debts.find(victim->GetGUID().GetRawValue());
        if (itr == g_debts.end())
            return 0;

        debt = itr->second;
        g_debts.erase(itr);
    }

    // Deducted HERE rather than at the moment of death, so a death that never
    // produces a chest cannot quietly take the gold with it. Re-clamped against
    // what the player has now for the same reason.
    uint64 const money = victim->GetMoney();
    uint32 const chestCopper = uint32(std::min<uint64>(money, debt.ChestCopper));
    uint32 const burnCopper = uint32(std::min<uint64>(money - chestCopper, debt.BurnCopper));
    if (uint32 const total = chestCopper + burnCopper)
        victim->ModifyMoney(-int32(total));

    return chestCopper;
}
}

using namespace Bounty;

class custom_bounty_player : public PlayerScript
{
public:
    custom_bounty_player() : PlayerScript("custom_bounty_player") {}

    // The one hook that can still see who was fighting the victim, and the only
    // one that fires for a death nobody dealt.
    void OnPlayerJustDied(Player* victim, Unit* killer) override
    {
        if (!s_enabled || !victim)
            return;

        // Whoever helped put them down is now worth putting down. Read the
        // participants before anything else touches the victim.
        if (s_stacksPerKill && IsBountyContext(victim))
        {
            std::vector<Player*> const participants = CollectParticipants(victim, killer);
            std::string credited;
            for (Player* participant : participants)
            {
                AddStacks(participant, s_stacksPerKill);
                credited += (credited.empty() ? "" : ", ") + participant->GetName();
            }

            // INFO, not DEBUG: Logger.playerbots sits at INFO, so a DEBUG line
            // here is discarded - which is why the first failure looked like
            // silence and cost a whole build cycle to tell a denied gate from an
            // empty participant list.
            TC_LOG_INFO("playerbots.hardcore", "Bounty: {} died; credited {}.",
                victim->GetName(), credited.empty() ? "nobody" : credited);
        }
        else if (s_stacksPerKill)
            TC_LOG_INFO("playerbots.hardcore",
                "Bounty: {} died somewhere a bounty does not apply (zone {} area {}); nobody credited.",
                victim->GetName(), victim->GetZoneId(), victim->GetAreaId());

        // And the victim settles up - but only where the bounty applied in the
        // first place. Dying in the Battle Ring deliberately does NOT clear a
        // bounty: if it did, walking into the ring would be the cheapest way to
        // shed one.
        //
        // The flat death tax comes off FIRST and is owed whether or not there is
        // a bounty; RecordDeathDebt then works the chest's share out of what is
        // left, so the two never compound past what the percentages say. Same
        // gate as the debt, so a battleground, an arena, the ring and a GM are
        // all free of it - dying somewhere a bounty does not apply should not
        // cost a purse either.
        if (IsBountyContext(victim))
        {
            TakeDeathTax(victim);
            RecordDeathDebt(victim);
        }

        // Whatever the zone rules said about the debt, the guards were sent for
        // a target who is now dead, so they go either way.
        DismissGuards(victim);
    }

    // Thirty stacks and up. Map thread, because summoning is.
    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        if (s_enabled && player)
            SummonGuardsFor(player);
    }

    // The safety nets, for deaths the hardcore chest does not cover.
    void OnPlayerRepop(Player* player) override
    {
        // Still standing where the body fell: BuildPlayerRepop calls this
        // before RepopAtGraveyard moves anyone.
        SummonBountyChestIfOwed(player);
    }

    void OnLogout(Player* player) override
    {
        // A corpse that logs out without releasing would otherwise take the
        // debt with it.
        SummonBountyChestIfOwed(player);

        std::lock_guard<std::mutex> guard(g_lock);
        g_stacks.erase(player->GetGUID().GetRawValue());
        g_debts.erase(player->GetGUID().GetRawValue());
        // The guards themselves are left to their timed despawn: the player is
        // gone, so there is nothing to resolve them against here.
        g_guards.erase(player->GetGUID().GetRawValue());
    }

    // A bounty survives a logout in the aura table, so the registry has to be
    // told about it again on the way back in - otherwise the player would see
    // the stacks and the bots would not.
    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (!s_enabled || !s_spellId || !player)
            return;

        if (Aura const* aura = player->GetAura(s_spellId, player->GetGUID()))
            WriteRegistry(player, aura->GetStackAmount(), aura->GetDuration());
    }
};

class custom_bounty_world : public WorldScript
{
public:
    custom_bounty_world() : WorldScript("custom_bounty_world") {}

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadBountyConfig();
    }
};

// A Centurion Guard is sent after ONE person and has no other business.
//
// The template carries faction 14 - "Monster", hostile to everything alive -
// because that is what makes it able to attack a player at all. The side effect
// was that it also aggroed anyone who happened to walk past: a guard summoned
// onto a bounty in Westfall would break off and chase a level 20 who had never
// killed anybody, which is precisely the thing a person turns War Mode off to
// avoid.
//
// So the guard is given a target list of one.
//
// MoveInLineOfSight is the lever that matters: CreatureAI's version is what
// turns "somebody walked past" into EngageWithTarget, and it is the only way a
// bystander gets onto the threat table without hitting the guard first.
// CanAIAttack covers the rest - re-selection once the quarry is dead or gone,
// which is exactly when a disengaged guard would otherwise look around and pick
// the nearest warm body. Neither touches the explicit AttackStart the summon
// issues, and neither can stop the guard answering somebody who hits it.
struct centurion_guardAI : public ScriptedAI
{
    explicit centurion_guardAI(Creature* creature) : ScriptedAI(creature) {}

    // The one person it was sent after, anyone hitting it, and anyone who armed
    // War Mode or earned a bounty of their own.
    bool MayEngage(Unit const* target) const
    {
        if (!target)
            return false;

        // Not in a sanctuary, for anybody, including the person it was sent
        // after. The stock sanctuary rule only covers PLAYER versus player -
        // IsValidAttackTarget requires UNIT_FLAG_PLAYER_CONTROLLED on both
        // sides - so a hostile creature is not stopped by it at all, and this
        // one is hostile to everything by design. A bounty can therefore be
        // outrun into a capital, which is the same answer the bots already give:
        // the human snapshot skips anyone in a no-PvP area.
        if (target->IsInSanctuary() || me->IsInSanctuary())
            return false;

        // TempSummon remembers its summoner, so no registry lookup and no lock
        // on the AI path.
        if (TempSummon const* summon = me->ToTempSummon())
            if (summon->GetSummonerGUID() == target->GetGUID())
                return true;

        // Whoever is hitting it, or owns the thing that is. Self-defence is not
        // negotiable, and it is what "unless provoked" means.
        for (Unit* attacker : me->getAttackers())
            if (attacker == target ||
                (attacker && attacker->GetCharmerOrOwnerPlayerOrPlayerItself() == target))
                return true;

        // Anything that is not a person - a bot, a mob that picked a fight -
        // is treated exactly as before.
        Player const* person = target->ToPlayer();
        if (!person)
            return true;

        // And a bystander is only fair game if they came looking for it.
        return BarracksHardcore::IsWarModeOptedIn(person) || GetStacks(person) > 0;
    }

    void MoveInLineOfSight(Unit* who) override
    {
        if (!MayEngage(who))
            return;

        ScriptedAI::MoveInLineOfSight(who);
    }

    bool CanAIAttack(Unit const* target) const override
    {
        return MayEngage(target) && ScriptedAI::CanAIAttack(target);
    }
};

class npc_centurion_guard : public CreatureScript
{
public:
    npc_centurion_guard() : CreatureScript("npc_centurion_guard") {}

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new centurion_guardAI(creature);
    }
};

// Why are the guards not coming?
//
// Every gate in SummonGuardsFor is silent: it returns, it does not log, and
// there is no way from inside the game to tell "the bounty is too small" from
// "you are a GM" from "this area is FFA by map" from "the creature id in the
// config does not exist". This prints all of them at once for one player, in
// the order they are actually asked, so the first NO is the answer.
class custom_bounty_commands : public CommandScript
{
public:
    custom_bounty_commands() : CommandScript("custom_bounty_commands") { }

    Trinity::ChatCommands::ChatCommandTable GetCommands() const override
    {
        using namespace Trinity::ChatCommands;
        static ChatCommandTable bountyTable =
        {
            { "why", HandleBountyWhy, rbac::RBAC_PERM_COMMAND_GM, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "bounty", bountyTable },
        };
        return commandTable;
    }

    // .bounty why [name] - defaults to yourself.
    static bool HandleBountyWhy(ChatHandler* handler, Optional<std::string> who)
    {
        Player* target = nullptr;
        if (who && !who->empty())
            target = ObjectAccessor::FindPlayerByName(*who);
        else if (handler->GetSession())
            target = handler->GetSession()->GetPlayer();

        if (!target)
        {
            handler->SendSysMessage("bounty: nobody by that name is online.");
            return true;
        }

        uint32 const registry = GetStacks(target);
        Aura const* aura = s_spellId ? target->GetAura(s_spellId, target->GetGUID()) : nullptr;
        uint32 const onAura = aura ? aura->GetStackAmount() : 0u;

        handler->PSendSysMessage("bounty: %s - registry %u stack(s), aura %u stack(s)%s.",
            target->GetName().c_str(), registry, onAura,
            (registry != onAura) ? " |cffff2020(MISMATCH - the registry is what every rule reads)|r" : "");

        // The context gates, in the order IsBountyContext asks them.
        handler->PSendSysMessage("  system enabled: %s   aura spell: %u",
            s_enabled ? "yes" : "NO", s_spellId);
        handler->PSendSysMessage("  gamemaster: %s   battleground/arena: %s   gurubashi ring: %s",
            target->IsGameMaster() ? "YES - blocks everything" : "no",
            (target->InBattleground() || target->InArena()) ? "YES - blocks" : "no",
            target->IsInGurubashiBattleRing() ? "YES - blocks" : "no");
        handler->PSendSysMessage("  FFA-by-map area: %s   alive: %s",
            target->pvpInfo.IsInFFAPvPAreaByMap ? "YES - blocks" : "no",
            target->IsAlive() ? "yes" : "NO - blocks");
        handler->PSendSysMessage("  => bounty context: %s",
            IsBountyContext(target) ? "|cff20ff20yes|r" : "|cffff2020NO|r");

        // The guard gates.
        handler->PSendSysMessage("  guards at: %u stacks   count: %u   creature: %u   every %us for %us",
            s_guardStacks, s_guardCount, s_guardEntry, s_guardIntervalSeconds, s_guardLifetimeSeconds);
        handler->PSendSysMessage("  => SummonsGuards(%u): %s", registry,
            SummonsGuards(registry) ? "|cff20ff20yes|r" : "|cffff2020NO|r");

        if (!sObjectMgr->GetCreatureTemplate(s_guardEntry))
            handler->PSendSysMessage("  |cffff2020creature %u is not in creature_template on this realm|r",
                s_guardEntry);

        handler->PSendSysMessage("  zone %u top level: %u (the level a guard is set to)",
            target->GetZoneId(), uint32(BarracksHardcore::ZoneTopLevel(target->GetZoneId())));
        return true;
    }
};

void AddSC_custom_bounty()
{
    LoadBountyConfig();
    new custom_bounty_player();
    new custom_bounty_world();
    new npc_centurion_guard();
    new custom_bounty_commands();
}
