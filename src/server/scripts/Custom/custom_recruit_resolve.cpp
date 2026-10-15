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
 * visible buff that weakens with each level and is gone at 15.
 *
 *   level 1-10  92330  +50% damage dealt to playerbots, -50% damage taken from them
 *   level 11    92331  +40% / -40%
 *   level 12    92332  +30% / -30%
 *   level 13    92333  +20% / -20%
 *   level 14    92334  +10% / -10%
 *   level 15+   nothing
 *
 * One spell row per level rather than one row scaled with
 * EffectRealPointsPerLevel: the buff's tooltip is drawn from the CLIENT's copy
 * of the row, and a fixed number in each row is the only way to be certain the
 * percentage a player reads is the one the server applies.
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
#include "Unit.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>

namespace
{
    // Full strength at or below RECRUIT_FULL_LEVEL, then one row per level, weaker each time.
    constexpr std::array<uint32, 5> RecruitResolveSpells = { 92330, 92331, 92332, 92333, 92334 };
    constexpr uint8 RECRUIT_FULL_LEVEL = 10;
    constexpr uint8 RECRUIT_END_LEVEL = uint8(RECRUIT_FULL_LEVEL + RecruitResolveSpells.size());

    uint32 RecruitResolveSpellFor(uint8 level)
    {
        if (level >= RECRUIT_END_LEVEL)
            return 0;

        return RecruitResolveSpells[level <= RECRUIT_FULL_LEVEL ? 0 : level - RECRUIT_FULL_LEVEL];
    }

    bool IsRecruitResolveSpell(uint32 spellId)
    {
        return spellId >= RecruitResolveSpells.front() && spellId <= RecruitResolveSpells.back();
    }

    bool IsPlayerbot(Player const* player)
    {
        WorldSession const* session = player->GetSession();
        return session && (session->IsVirtualSession() || session->IsTransientPlayerSession());
    }

    // The worn aura's amount on one effect, or 0 when the person is not wearing it.
    int32 RecruitResolveAmount(Player const* person, SpellEffIndex effIndex)
    {
        for (AuraEffect const* aurEff : person->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (aurEff->GetEffIndex() == effIndex && IsRecruitResolveSpell(aurEff->GetId()))
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

    // Puts on the row for the player's level and takes off any other. Bots never wear it.
    void UpdateRecruitResolve(Player* player)
    {
        uint32 const wanted = IsPlayerbot(player) ? 0 : RecruitResolveSpellFor(player->GetLevel());

        for (uint32 spellId : RecruitResolveSpells)
            if (spellId != wanted)
                player->RemoveAurasDueToSpell(spellId);

        if (wanted && !player->HasAura(wanted))
            player->AddAura(wanted, player);
    }
}

// The rows are permanent, death-persistent, kept on arena entry and cannot be
// clicked off, so login and a level change are what normally move them. A map
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
