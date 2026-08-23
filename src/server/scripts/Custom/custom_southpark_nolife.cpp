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
#include "DBCStores.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "SpellScript.h"
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

// 90607 - No Life: spells and melee attacks summon three Scorpions.
// The proc flags and the 10% chance live in spell_proc; this only performs the
// summon, so a retune of the chance never needs a rebuild.
class spell_nolife_scorpions : public AuraScript
{
    PrepareAuraScript(spell_nolife_scorpions);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        Unit* victim = eventInfo.GetProcTarget();

        for (uint32 i = 0; i < NOLIFE_SCORPION_COUNT; ++i)
        {
            Position pos = player->GetRandomNearPosition(3.0f);
            TempSummon* scorpion = player->SummonCreature(NOLIFE_SCORPION_ENTRY, pos,
                TEMPSUMMON_TIMED_DESPAWN, NOLIFE_SCORPION_DURATION);
            if (!scorpion)
                continue;

            // Pin the health rather than chase it through HealthModifier: that
            // multiplier is applied against creature_classlevelstats and would
            // drift with any future stat pass, and 1500 is a stated number.
            scorpion->SetMaxHealth(NOLIFE_SCORPION_HEALTH);
            scorpion->SetHealth(NOLIFE_SCORPION_HEALTH);
            scorpion->SetLevel(player->GetLevel());
            scorpion->SetFaction(player->GetFaction());
            scorpion->SetOwnerGUID(player->GetGUID());

            if (victim && player->IsValidAttackTarget(victim))
                scorpion->AI()->AttackStart(victim);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_nolife_scorpions::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// The Scorpions' own bite. Applied by the creature rather than by a spell list
// so the poison follows the summon wherever it is used.
class npc_nolife_scorpion : public CreatureScript
{
public:
    npc_nolife_scorpion() : CreatureScript("npc_nolife_scorpion") { }

    struct npc_nolife_scorpionAI : public ScriptedAI
    {
        npc_nolife_scorpionAI(Creature* creature) : ScriptedAI(creature) { }

        void DamageDealt(Unit* victim, uint32& /*damage*/, DamageEffectType damageType) override
        {
            // Auto attacks only: the poison is the bite, not a spell effect.
            if (damageType != DIRECT_DAMAGE || !victim || !victim->IsAlive())
                return;

            if (!victim->HasAura(SPELL_NOLIFE_SCORPION_VENOM))
                me->CastSpell(victim, SPELL_NOLIFE_SCORPION_VENOM, true);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_nolife_scorpionAI(creature);
    }
};

// 90610 - Three Lives. The periodic tick owns the gate; the absorb on 90611
// owns the actual refusal to die.
class spell_nolife_three_lives : public AuraScript
{
    PrepareAuraScript(spell_nolife_three_lives);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        if (!WearsOnlyNoLife(player, false))
        {
            player->RemoveAurasDueToSpell(SPELL_NOLIFE_EXTRA_LIFE);
            return;
        }

        // Recharging: no lives until the 30 min marker lapses. Checked before
        // the re-grant so a spent set cannot be topped up by re-equipping.
        if (player->HasAura(SPELL_NOLIFE_RECHARGE))
            return;

        if (!player->HasAura(SPELL_NOLIFE_EXTRA_LIFE))
        {
            player->CastSpell(player, SPELL_NOLIFE_EXTRA_LIFE, true);
            if (Aura* lives = player->GetAura(SPELL_NOLIFE_EXTRA_LIFE))
                lives->SetStackAmount(NOLIFE_LIVES);
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_nolife_three_lives::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
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

        // Eat the whole blow, then restore. Absorbing rather than resurrecting
        // keeps the player from ever entering the dead state, so no corpse, no
        // release, no spirit healer.
        absorbAmount = dmgInfo.GetDamage();

        player->SetFullHealth();
        player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
        player->GetSpellHistory()->ResetAllCooldowns();

        uint8 const remaining = GetStackAmount() > 1 ? GetStackAmount() - 1 : 0;
        if (remaining)
            SetStackAmount(remaining);
        else
        {
            // Last life spent: drop the buff and start the 30 min recharge. The
            // periodic gate above refuses to re-grant while that marker is up.
            player->CastSpell(player, SPELL_NOLIFE_RECHARGE, true);
            Remove();
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

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->HasAura(SPELL_NOLIFE_COMMUNION_BUFF))
            return;

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        uint32 const amount = damageInfo->GetDamage();
        player->ModifyHealth(int32(amount));

        if (player->GetPowerType() == POWER_MANA)
            player->ModifyPower(POWER_MANA, int32(amount));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_nolife_cursed_communion::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        OnEffectProc += AuraEffectProcFn(spell_nolife_cursed_communion::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

void AddSC_custom_southpark_nolife()
{
    RegisterSpellScript(spell_nolife_scorpions);
    RegisterSpellScript(spell_nolife_three_lives);
    RegisterSpellScript(spell_nolife_extra_life);
    RegisterSpellScript(spell_nolife_cursed_communion);
    new npc_nolife_scorpion();
}
