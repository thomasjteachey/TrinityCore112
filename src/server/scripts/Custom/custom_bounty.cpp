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

#include "custom_barracks_hardcore.h"
#include "custom_loot_chest_helper.h"

#include "CombatManager.h"
#include "Configuration/Config.h"
#include "Duration.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "World.h"

#include <algorithm>
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
    float s_botLossMultiplier = 2.0f;
    uint32 s_chestEntry = 0;
    uint32 s_chestDespawnSeconds = 600;
    float s_huntRadiusBonusYards = 175.0f;
    uint32 s_drifterZoneBonus = 10;
    int32 s_aggroSlotBonus = 8;
    uint32 s_ignoreAggroBudgetStacks = 40;
    uint32 s_relentlessStacks = 10;
    uint32 s_relentlessIntervalSeconds = 30;
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
        TC_LOG_DEBUG("playerbots.hardcore", "Bounty: {} now at {} stack(s).",
            killer->GetName(), uint32(aura->GetStackAmount()));
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

            ++summoned;
            if (guard->IsAIEnabled())
                guard->AI()->AttackStart(player);
        }

        if (summoned)
            TC_LOG_INFO("playerbots.hardcore",
                "Bounty {}: {} Centurion Guard(s) sent after {}.",
                stacks, summoned, player->GetName());
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
        if (IsBountyContext(victim))
            RecordDeathDebt(victim);
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

void AddSC_custom_bounty()
{
    LoadBountyConfig();
    new custom_bounty_player();
    new custom_bounty_world();
}
