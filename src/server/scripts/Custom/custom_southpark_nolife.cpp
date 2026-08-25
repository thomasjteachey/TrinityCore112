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

/*
 * South Park "No Life" mage set (ItemSet 1077, items 100986-100989).
 *
 *   3pc  90607 No Life          - spells and melee have a 10% chance to summon
 *                                 three Scorpions for 30 sec.
 *   3pc  90610 Three Lives      - while wearing ONLY the three set pieces, death
 *                                 is refused three times. 30 min to recharge.
 *
 * The pool of lives (90611, one stack per life) is DURABLE: it survives death,
 * logout, zoning into a battleground, and taking the set off.
 *
 * THE RECHARGE IS A COOLDOWN, NOT AN AURA. It lives in the player's
 * SpellHistory keyed on 90612; the 90612 aura is only a display that the
 * periodic tick re-syncs from it. Two rounds of live testing were lost to
 * getting this backwards:
 *
 *   - When the aura WAS the cooldown, a player right-clicked the buff off and
 *     had all three lives back on the next tick. Auras are cancellable by
 *     design; cooldowns are not. Marking the aura negative so the client would
 *     refuse to cancel it is a client-side fix for a server-side rule, and
 *     SPELL_ATTR0_CANT_CANCEL on top of that is still only a second layer.
 *   - A cooldown also persists on its own (character_spell_cooldown, with no
 *     filter on either save or load), and cannot be dispelled, stolen, or
 *     stripped on arena entry - three more things an aura has to be flagged
 *     against one at a time.
 *
 * The window starts on the FIRST life spent and refills all three at once when
 * it lapses. Note that a life grants "every cooldown reset", so the absorb has
 * to carry the recharge across its own ResetAllCooldowns call - otherwise
 * spending the last life would clear the very cooldown that governs lives.
 *   4pc  90613 Cursed Communion - those three pieces PLUS the Cursed Skinning
 *                                 Knife make every melee swing an unavoidable,
 *                                 unmitigated critical that fully leeches.
 *
 * The two "only these pieces" bonuses are gated by a 1 sec periodic dummy on the
 * carrier rather than by an equip hook. Equipment can change through a lot of
 * paths (swap, break, unequip on death, a GM .additem), and a poll that re-reads
 * the live loadout cannot miss one; the cost is a single slot scan per second on
 * a character who is by definition wearing three items.
 *
 * The carriers themselves are applied by the ItemSet's own SetSpellID entries -
 * the core does that for free once the piece count is met - so nothing here has
 * to count pieces. These scripts only decide whether the STRICT loadout holds.
 */

#include "ScriptMgr.h"
#include "Creature.h"
#include "ScriptedCreature.h"
#include "PetAI.h"
#include "DBCStores.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "ThreatManager.h"
#include "TemporarySummon.h"


namespace
{
    enum NoLifeSpells
    {
        SPELL_NOLIFE_SCORPION_CARRIER  = 90607,
        SPELL_NOLIFE_SUMMON_SCORPIONS  = 90608,
        SPELL_NOLIFE_SCORPION_VENOM    = 90609,
        SPELL_NOLIFE_THREE_LIVES       = 90610,
        SPELL_NOLIFE_EXTRA_LIFE        = 90611,
        SPELL_NOLIFE_RECHARGE          = 90612,
        SPELL_NOLIFE_COMMUNION_CARRIER = 90613,
        SPELL_NOLIFE_COMMUNION_BUFF    = 90614,
        SPELL_NOLIFE_COMMUNION_HEAL    = 90621,
        SPELL_NOLIFE_COMMUNION_MANA    = 90622,
        SPELL_NOLIFE_CHARGES           = 90626,
    };

    enum NoLifeItems
    {
        ITEM_NOLIFE_HELM   = 100986,
        ITEM_NOLIFE_GLOVES = 100987,
        ITEM_NOLIFE_BOOTS  = 100988,
        ITEM_NOLIFE_KNIFE  = 100989,
    };

    constexpr uint32 NOLIFE_SCORPION_ENTRY = 900118;
    constexpr uint32 NOLIFE_SCORPION_COUNT = 3;
    constexpr uint32 NOLIFE_SCORPION_HEALTH = 1500;
    constexpr Milliseconds NOLIFE_SCORPION_DURATION = Milliseconds(30 * IN_MILLISECONDS);
    constexpr uint8  NOLIFE_LIVES = 3;

    uint32 EquippedEntry(Player const* player, uint8 slot)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item ? item->GetEntry() : 0;
    }

    // "If you are ONLY wearing these 3 pieces." Every equipment slot must be
    // empty except the three set pieces and the ones the bonus explicitly
    // allows: a weapon, an off-hand, a wand and two trinkets.
    //
    // Shirt and tabard are also tolerated. They carry no stats and cannot
    // affect the power the restriction exists to limit; banning them would only
    // stop someone wearing a guild tabard while playing the joke build.
    bool WearsOnlyNoLife(Player const* player, bool requireKnife)
    {
        if (!player)
            return false;

        if (EquippedEntry(player, EQUIPMENT_SLOT_HEAD) != ITEM_NOLIFE_HELM ||
            EquippedEntry(player, EQUIPMENT_SLOT_HANDS) != ITEM_NOLIFE_GLOVES ||
            EquippedEntry(player, EQUIPMENT_SLOT_FEET) != ITEM_NOLIFE_BOOTS)
            return false;

        if (requireKnife && EquippedEntry(player, EQUIPMENT_SLOT_MAINHAND) != ITEM_NOLIFE_KNIFE)
            return false;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            switch (slot)
            {
                case EQUIPMENT_SLOT_HEAD:       // the set itself
                case EQUIPMENT_SLOT_HANDS:
                case EQUIPMENT_SLOT_FEET:
                case EQUIPMENT_SLOT_MAINHAND:   // explicitly permitted
                case EQUIPMENT_SLOT_OFFHAND:
                case EQUIPMENT_SLOT_RANGED:
                case EQUIPMENT_SLOT_TRINKET1:
                case EQUIPMENT_SLOT_TRINKET2:
                case EQUIPMENT_SLOT_BODY:       // cosmetic only
                case EQUIPMENT_SLOT_TABARD:
                    continue;
                default:
                    break;
            }

            if (EquippedEntry(player, slot))
                return false;
        }

        return true;
    }

    constexpr Minutes NOLIFE_RECHARGE_TIME = Minutes(30);

    // How long until lives come back. THIS is the cooldown - a real entry in the
    // player's SpellHistory, not an aura.
    //
    // The recharge used to BE the aura 90612, which meant a player could
    // right-click the buff off and have all three lives back on the next tick.
    // An aura is player-cancellable and a cooldown is not; a cooldown also
    // persists across logout on its own (character_spell_cooldown) and cannot be
    // dispelled, stolen, or stripped on zoning into a battleground. The aura is
    // now only the display.
    uint32 RechargeRemainingMs(Player const* player)
    {
        return player->GetSpellHistory()->GetRemainingCooldown(
            sSpellMgr->AssertSpellInfo(SPELL_NOLIFE_RECHARGE));
    }

    // Keep the cosmetic marker in step with the real cooldown. Re-applying it
    // after a cancel is what makes right-clicking it off pointless rather than
    // rewarding, and the duration is taken from the cooldown each time so the
    // two can never drift apart.
    void SyncRechargeMarker(Player* player, uint32 remainingMs)
    {
        if (!remainingMs)
        {
            player->RemoveAurasDueToSpell(SPELL_NOLIFE_RECHARGE);
            return;
        }

        if (!player->HasAura(SPELL_NOLIFE_RECHARGE))
            player->CastSpell(player, SPELL_NOLIFE_RECHARGE, true);

        if (Aura* mark = player->GetAura(SPELL_NOLIFE_RECHARGE))
        {
            mark->SetMaxDuration(int32(remainingMs));
            mark->SetDuration(int32(remainingMs));
        }
    }

    uint8 ChargesLeft(Player const* player)
    {
        Aura const* pool = player->GetAura(SPELL_NOLIFE_CHARGES);
        return pool ? pool->GetStackAmount() : 0;
    }

    // The pool (90626) is hidden and durable; the buff the player actually sees
    // (90611) is created and destroyed with the set and only ever MIRRORS it.
    //
    // Splitting the two is what lets the display vanish when the set comes off
    // without the lives going with it. One combined aura cannot do both jobs:
    // removing it on unequip silently spent the remaining lives, and keeping it
    // meant a set bonus sat in the buff frame while the set was in the bank.
    void SyncLivesDisplay(Player* player)
    {
        uint8 const charges = ChargesLeft(player);
        if (!charges)
        {
            player->RemoveAurasDueToSpell(SPELL_NOLIFE_EXTRA_LIFE);
            return;
        }

        if (!player->HasAura(SPELL_NOLIFE_EXTRA_LIFE))
            player->CastSpell(player, SPELL_NOLIFE_EXTRA_LIFE, true);

        if (Aura* shown = player->GetAura(SPELL_NOLIFE_EXTRA_LIFE))
            if (shown->GetStackAmount() != charges)
                shown->SetStackAmount(charges);
    }

    // Keep `buff` present exactly while `wanted` holds. Applying an aura that is
    // already there would refresh it every tick, which for a permanent buff is
    // just churn on the client's aura frame.
    void SyncGatedBuff(Unit* target, uint32 buff, bool wanted)
    {
        if (wanted)
        {
            if (!target->HasAura(buff))
                target->CastSpell(target, buff, true);
        }
        else if (target->HasAura(buff))
            target->RemoveAurasDueToSpell(buff);
    }
}

// 90607 needs no script. It is a PROC_TRIGGER_SPELL carrier whose trigger is
// 90608, and 90608 is a real SPELL_EFFECT_SUMMON (effect 28) pointing at
// SummonProperties 1562 - the identical shape Force of Nature uses to put
// down three treants for 30 sec. The core owns the summon, which means it
// also owns ownership, faction, UNIT_FLAG_PLAYER_CONTROLLED and the guardian
// AI that follows the owner's target. The hand-rolled version that used to
// live here could never work: it called PreventDefaultAction(), suppressing
// the very trigger that does the job, and then summoned a plain creature
// whose creature-vs-creature faction check refused a neutral target dummy.

// The Scorpions' own bite. Applied by the creature rather than by a spell list
// so the poison follows the summon wherever it is used.
// Derives from PetAI, NOT ScriptedAI. These are guardians (SummonProperties
// 1562), and everything that makes a guardian useful - following the owner,
// picking up the owner's target, re-engaging when the owner is attacked - lives
// in PetAI. A script that hands back a ScriptedAI REPLACES that, which is why
// the scorpions used to spawn and then just stand there.
//
// Same shape the Shadowfiend uses (pet_priest.cpp), including registration via
// RegisterCreatureAI. creature_template.AIName must stay empty for 900118: a
// row carrying both AIName and ScriptName is contradictory and the AIName copy
// would be what the factory honours.
struct npc_nolife_scorpion : public PetAI
{
    npc_nolife_scorpion(Creature* creature) : PetAI(creature) { }

    // No base call - PetAI::JustAppeared is deliberately empty because it
    // controls following itself.
    void JustAppeared() override
    {
        // 1500 is a stated number, so pin it rather than chase it through
        // HealthModifier, which multiplies against creature_classlevelstats
        // and would drift with any future stat pass.
        me->SetMaxHealth(NOLIFE_SCORPION_HEALTH);
        me->SetHealth(NOLIFE_SCORPION_HEALTH);
    }

    void DamageDealt(Unit* victim, uint32& /*damage*/, DamageEffectType damageType) override
    {
        // Auto attacks only: the poison is the bite, not a spell effect.
        if (damageType != DIRECT_DAMAGE || !victim || !victim->IsAlive())
            return;

        if (!victim->HasAura(SPELL_NOLIFE_SCORPION_VENOM))
            me->CastSpell(victim, SPELL_NOLIFE_SCORPION_VENOM, true);
    }
};

// 90610 - Three Lives. The periodic tick owns the gate; the absorb on 90611
// owns the actual refusal to die.
class spell_nolife_three_lives : public AuraScript
{
    PrepareAuraScript(spell_nolife_three_lives);

    // One rule: while the strict loadout holds and the recharge is NOT running,
    // the pool sits at full. Everything else follows from that.
    //
    // The pool deliberately does NOT get torn down when the set comes off. It
    // used to, and re-equipping then rebuilt it from scratch - three lives on
    // demand, for the price of two inventory clicks. Now the stack IS the
    // durable record of how many lives are left, so taking the set off and
    // putting it back changes nothing. What stops an unequipped player from
    // spending those lives is the loadout check inside the absorb, not the
    // absence of the aura.
    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        uint32 const remainingMs = RechargeRemainingMs(player);

        // Not the strict loadout, so the bonus is inactive and nothing VISIBLE
        // belongs on the player. Only the two DISPLAYS come off here - the pool
        // and the cooldown behind them are untouched, which is what makes
        // taking the set off and putting it back on a no-op rather than a cost.
        if (!WearsOnlyNoLife(player, false))
        {
            player->RemoveAurasDueToSpell(SPELL_NOLIFE_EXTRA_LIFE);
            player->RemoveAurasDueToSpell(SPELL_NOLIFE_RECHARGE);
            return;
        }

        // The gate is the cooldown, never the aura - the marker is re-synced
        // from it here, so cancelling the buff just puts it straight back with
        // the correct time remaining.
        SyncRechargeMarker(player, remainingMs);

        // Off cooldown: refill the pool. This also restores a partial pool left
        // over from a life spent earlier in the window.
        if (!remainingMs)
        {
            if (Aura* pool = player->GetAura(SPELL_NOLIFE_CHARGES))
            {
                if (pool->GetStackAmount() < NOLIFE_LIVES)
                    pool->SetStackAmount(NOLIFE_LIVES);
            }
            else
            {
                player->CastSpell(player, SPELL_NOLIFE_CHARGES, true);
                if (Aura* pool = player->GetAura(SPELL_NOLIFE_CHARGES))
                    pool->SetStackAmount(NOLIFE_LIVES);
            }
        }

        SyncLivesDisplay(player);
    }

    // The carrier owns the pool, so the carrier's removal is what tears it down.
    //
    // This CANNOT live in the periodic tick. The tick stops the instant the
    // carrier is gone - which is precisely the moment the cleanup is needed -
    // so taking every set piece off left the lives and the recharge display
    // sitting on the player indefinitely. The tick only ever sees the lesser
    // case: still wearing the set, but no longer the strict loadout.
    //
    // ONLY the displays. The pool (90626) and the recharge cooldown both stay,
    // because they are the durable state - dropping the pool here would quietly
    // spend whatever lives were left the moment the set came off, which is
    // exactly what unequipping used to cost.
    void OnCarrierRemoved(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        player->RemoveAurasDueToSpell(SPELL_NOLIFE_EXTRA_LIFE);
        player->RemoveAurasDueToSpell(SPELL_NOLIFE_RECHARGE);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_nolife_three_lives::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_nolife_three_lives::OnCarrierRemoved, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90611 - Extra Life: refuse the killing blow.
class spell_nolife_extra_life : public AuraScript
{
    PrepareAuraScript(spell_nolife_extra_life);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        // An unlimited shield that only ever absorbs the ONE blow that would
        // have been lethal. A finite pool would be chewed away by ordinary hits
        // and the life would be gone before the death it exists to prevent.
        amount = -1;
        canBeRecalculated = false;
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        absorbAmount = 0;

        Unit* target = GetTarget();
        if (dmgInfo.GetDamage() < target->GetHealth())
            return;     // survivable - let it through untouched

        Player* player = target->ToPlayer();
        if (!player)
            return;

        // The pool now outlives the set, so this is the ONLY thing keeping an
        // unequipped player from cashing in the lives they are still visibly
        // carrying. Without it, stripping the set would be strictly better:
        // full stats and a death save.
        if (!WearsOnlyNoLife(player, false))
            return;

        // The buff this script rides on is only a mirror; 90626 is the record.
        // If the two are out of step, refuse rather than hand out a free life.
        Aura* pool = player->GetAura(SPELL_NOLIFE_CHARGES);
        if (!pool || !pool->GetStackAmount())
            return;

        // Read the recharge BEFORE the reset below. "Every cooldown reset" is
        // part of what a life gives you, and it would otherwise wipe the one
        // cooldown that decides when lives come back - handing out a fresh
        // three every time the pool ran dry.
        uint32 const carriedMs = RechargeRemainingMs(player);

        // Eat the whole blow, then restore. Absorbing rather than resurrecting
        // keeps the player from ever entering the dead state, so no corpse, no
        // release, no spirit healer.
        absorbAmount = dmgInfo.GetDamage();

        player->SetFullHealth();
        player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
        player->GetSpellHistory()->ResetAllCooldowns();

        // The cooldown starts on the FIRST life spent and the pool refills to
        // three in one go when it lapses. Starting it on the last life instead
        // would strand anyone who spent one or two lives at that reduced count
        // permanently, since a partial pool is never topped up while it is the
        // full pool being waited for. A window already running is carried over
        // untouched rather than restarted.
        player->GetSpellHistory()->AddCooldown(SPELL_NOLIFE_RECHARGE, 0,
            carriedMs ? Milliseconds(carriedMs) : Milliseconds(NOLIFE_RECHARGE_TIME));
        SyncRechargeMarker(player, RechargeRemainingMs(player));

        // Spend from the pool, then bring the visible mirror into line. The
        // periodic tick would do the same a fraction of a second later, but
        // waiting would show a stale life count on the frame that mattered.
        uint8 const remaining = pool->GetStackAmount() > 1 ? pool->GetStackAmount() - 1 : 0;
        if (remaining)
        {
            pool->SetStackAmount(remaining);
            SetStackAmount(remaining);
        }
        else
        {
            player->RemoveAurasDueToSpell(SPELL_NOLIFE_CHARGES);
            Remove();   // out of lives; the cooldown above governs the way back
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_nolife_extra_life::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_nolife_extra_life::Absorb, EFFECT_0);
    }
};

// 90613 - Cursed Communion. Periodic tick gates the buff that carries the crit /
// hit / armour-ignore effects; the proc does the leech, which no aura type
// expresses for melee.
class spell_nolife_cursed_communion : public AuraScript
{
    PrepareAuraScript(spell_nolife_cursed_communion);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            SyncGatedBuff(player, SPELL_NOLIFE_COMMUNION_BUFF, WearsOnlyNoLife(player, true));
    }

    // Same reason as the pool on 90610: SyncGatedBuff can only retract the buff
    // while the tick is still running, and unequipping the set stops the tick
    // in the same instant it invalidates the bonus.
    void OnCarrierRemoved(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->RemoveAurasDueToSpell(SPELL_NOLIFE_COMMUNION_BUFF);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->HasAura(SPELL_NOLIFE_COMMUNION_BUFF))
            return;

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        int32 const amount = int32(damageInfo->GetDamage());

        // Cast real spells rather than calling ModifyHealth/ModifyPower.
        // Those move the bars silently: nothing reaches the combat log, so the
        // wearer cannot see the leech happening or check the numbers. Casting
        // 90621/90622 with the amount as the base value produces ordinary heal
        // and energize log lines, and picks up the rest of the healing pipeline
        // (overheal reporting, healing done, absorbs) for free.
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(amount);
        player->CastSpell(player, SPELL_NOLIFE_COMMUNION_HEAL, args);

        if (player->GetPowerType() == POWER_MANA)
        {
            CastSpellExtraArgs manaArgs(TRIGGERED_FULL_MASK);
            manaArgs.AddSpellBP0(amount);
            player->CastSpell(player, SPELL_NOLIFE_COMMUNION_MANA, manaArgs);
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_nolife_cursed_communion::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_nolife_cursed_communion::OnCarrierRemoved, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectProc += AuraEffectProcFn(spell_nolife_cursed_communion::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

void AddSC_custom_southpark_nolife()
{
    RegisterSpellScript(spell_nolife_three_lives);
    RegisterSpellScript(spell_nolife_extra_life);
    RegisterSpellScript(spell_nolife_cursed_communion);
    RegisterCreatureAI(npc_nolife_scorpion);
}
