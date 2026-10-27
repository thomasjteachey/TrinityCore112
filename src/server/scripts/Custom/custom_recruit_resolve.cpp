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
 * Recruit's Resolve: a new player's edge over the playerbot fleet, worn as a
 * visible buff that loses a stack with each level and is gone at 15.
 *
 *   level 1-10  5 stacks  +50% damage dealt to playerbots, -50% damage taken from them
 *   level 11    4 stacks  +40% / -40%
 *   level 12    3 stacks  +30% / -30%
 *   level 13    2 stacks  +20% / -20%
 *   level 14    1 stack   +10% / -10%
 *   level 15+   nothing
 *
 * One spell, 94303: stack limit 5, 10% per stack. The client draws the stack
 * count on the buff icon, and the core multiplies each effect's amount by the
 * stack count, so the worn aura already holds the total. The tooltip reads
 * "10% per stack" because a 3.3.5 client never learns aura amounts, only the
 * row's own numbers.
 *
 * It replaced 92330..92334, one fixed row per level. Anyone still wearing one
 * of those has it swapped for the stacking buff on the next login, level or
 * map change.
 *
 * THE NUMBERS LIVE ON THE AURA. EFFECT_0 is the bonus to damage dealt and
 * EFFECT_1 the cut to damage taken, both DUMMY, and the hooks below read the
 * worn aura's own amounts. A retune is a Spell.dbc edit, and a player who is
 * not wearing the buff gets stock numbers.
 *
 * WHO IS A PLAYERBOT. Anything played by the server rather than by a person:
 * a socketless session (the managed fleet) or a transient one (battleground
 * fill, custom-game and Violet Hold clones, "Dark <name>" mirrors). Pets,
 * guardians and totems count as whoever owns them, on both sides, so a
 * hunter's pet shares its owner's edge and a bot's imp is a bot.
 *
 * WHERE. The script hooks run before absorbs, resists and the combat log, so
 * the number a player sees on screen is the number that lands: melee and
 * ranged swings, spell hits (weapon specials included), damage-over-time and
 * drain ticks.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "Unit.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>

namespace
{
    constexpr uint32 SPELL_RECRUIT_RESOLVE = 94303;
    constexpr uint8 RECRUIT_MAX_STACKS = 5;
    constexpr uint8 RECRUIT_END_LEVEL = 15;     // the first level with no stacks left

    // The one-row-per-level version this replaced, taken off wherever it is still worn.
    constexpr std::array<uint32, 5> LegacyRecruitResolveSpells = { 92330, 92331, 92332, 92333, 92334 };

    // 5 through level 10, then one fewer each level: 4 at 11 ... 1 at 14, none from 15.
    uint8 RecruitResolveStacksFor(uint8 level)
    {
        if (level >= RECRUIT_END_LEVEL)
            return 0;

        return uint8(std::min<uint32>(RECRUIT_MAX_STACKS, RECRUIT_END_LEVEL - level));
    }

    bool IsPlayerbot(Player const* player)
    {
        WorldSession const* session = player->GetSession();
        return session && (session->IsVirtualSession() || session->IsTransientPlayerSession());
    }

    // The worn aura's amount on one effect (already times its stacks), or 0
    // when the person is not wearing it.
    int32 RecruitResolveAmount(Player const* person, SpellEffIndex effIndex)
    {
        for (AuraEffect const* aurEff : person->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (aurEff->GetEffIndex() == effIndex && aurEff->GetId() == SPELL_RECRUIT_RESOLVE)
                return aurEff->GetAmount();

        return 0;
    }

    // What this blow lands for, as a percentage of what it would have.
    //
    // 100 unless a person on one side wears the buff and a playerbot is on the
    // other. Runs for every blow in the world, so the cheapest test goes first:
    // wildlife on either side is never player controlled.
    uint32 RecruitResolvePct(Unit const* target, Unit const* attacker)
    {
        if (!target || !attacker || target == attacker ||
            !target->IsControlledByPlayer() || !attacker->IsControlledByPlayer())
            return 100;

        Player const* attackerPlayer = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player const* targetPlayer = target->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!attackerPlayer || !targetPlayer || attackerPlayer == targetPlayer)
            return 100;

        bool const attackerIsBot = IsPlayerbot(attackerPlayer);
        if (attackerIsBot == IsPlayerbot(targetPlayer))
            return 100;

        // A bot hitting a person: the person's cut. A person hitting a bot:
        // the person's bonus. A bot that somehow wears the buff (a clone of
        // a low-level person carries their auras) gets nothing from it.
        if (attackerIsBot)
            return 100 - uint32(std::clamp(RecruitResolveAmount(targetPlayer, EFFECT_1), 0, 100));

        return 100 + uint32(std::max(RecruitResolveAmount(attackerPlayer, EFFECT_0), 0));
    }

    // Sets the buff to the stack count for the player's level, or takes it off.
    // Bots never wear it.
    void UpdateRecruitResolve(Player* player)
    {
        for (uint32 spellId : LegacyRecruitResolveSpells)
            player->RemoveAurasDueToSpell(spellId);

        uint8 const stacks = IsPlayerbot(player) ? 0 : RecruitResolveStacksFor(player->GetLevel());
        if (!stacks)
        {
            player->RemoveAurasDueToSpell(SPELL_RECRUIT_RESOLVE);
            return;
        }

        Aura* aura = player->GetAura(SPELL_RECRUIT_RESOLVE);
        if (!aura)
            aura = player->AddAura(SPELL_RECRUIT_RESOLVE, player);

        // SetStackAmount recalculates both effect amounts and sends the new
        // count to the client.
        if (aura && aura->GetStackAmount() != stacks)
            aura->SetStackAmount(stacks);
    }
}

// The buff is permanent, death-persistent, kept on arena entry and cannot be
// clicked off, so login and a level change are what normally move it. A map
// change re-checks too, which puts it back after anything that strips auras
// wholesale on the way into a battleground or a custom game.
class custom_recruit_resolve_player : public PlayerScript
{
public:
    custom_recruit_resolve_player() : PlayerScript("custom_recruit_resolve_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        UpdateRecruitResolve(player);
    }

    void OnLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        UpdateRecruitResolve(player);
    }

    void OnMapChanged(Player* player) override
    {
        UpdateRecruitResolve(player);
    }
};

class custom_recruit_resolve_damage : public UnitScript
{
public:
    custom_recruit_resolve_damage() : UnitScript("custom_recruit_resolve_damage") { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (uint32 const pct = RecruitResolvePct(target, attacker); pct != 100)
            damage = CalculatePct(damage, pct);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage) override
    {
        if (uint32 const pct = RecruitResolvePct(target, attacker); pct != 100)
            damage = CalculatePct(damage, pct);
    }

    // Periodic damage ticks.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (uint32 const pct = RecruitResolvePct(target, attacker); pct != 100)
            damage = CalculatePct(damage, pct);
    }

    // Drain ticks - and, because the core calls this same overload from the
    // periodic HEAL handler, heal-over-time ticks too. Only a tick between two
    // sides that may fight is a drain; a bot's Renew on a person in its own
    // battleground team is left alone.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (uint32 const pct = RecruitResolvePct(target, attacker); pct != 100 && attacker->IsValidAttackTarget(target))
            damage = CalculatePct(damage, pct);
    }
};

void AddSC_custom_recruit_resolve()
{
    new custom_recruit_resolve_player();
    new custom_recruit_resolve_damage();
}
