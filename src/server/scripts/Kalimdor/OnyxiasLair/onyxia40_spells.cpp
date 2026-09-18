/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Level 60 damage for the vanilla Onyxia's abilities.
 *
 * Both lairs cast the same spells, so these scripts return untouched unless the
 * caster is standing in the 40-player version. Upstream does this by rewriting
 * the SpellInfo globally at load, which changes the level 80 lair too - its own
 * code carries a "TODO: This is currently overriding 10 man spells" - so the
 * values are applied per cast here instead.
 *
 * TrinityCore computes an effect as BasePoints + irand(1, DieSides), so a
 * replacement value has to fold the roll in: upstream's
 * "BasePoints = 3062, DieSides = 875" becomes 3062 + irand(1, 875).
 *
 * Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0).
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

#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "Unit.h"
#include "VanillaRaids/VanillaRaids.h"

enum Onyxia40TunedSpells
{
    SPELL_FLAME_BREATH_40   = 18435,
    SPELL_WING_BUFFET_40    = 18500,
    SPELL_FIREBALL_40       = 18392,
    SPELL_ERUPTION_40       = 17731,
    SPELL_WARDER_BLAST_40   = 20203,

    // Every step of the deep breath leaves its own flame patch spell behind, so
    // the whole chain needs the vanilla spread, not just the one she casts.
    BREATH_CHAIN_DIE_SIDES  = 451
};

// 17086 and the rest of the deep breath chain, plus Flame Breath, Fireball,
// Eruption and the Onyxian Warder's blast - everything whose damage sits on
// effect 0.
class spell_onyxia40_damage : public SpellScript
{
    PrepareSpellScript(spell_onyxia40_damage);

    void CalculateDamage(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsOnyxia40(GetCaster()))
            return;

        switch (GetSpellInfo()->Id)
        {
            case SPELL_FLAME_BREATH_40:
                SetEffectValue(3062 + irand(1, 875));
                break;
            case SPELL_FIREBALL_40:
                SetEffectValue(1699 + irand(1, 601));
                break;
            case SPELL_ERUPTION_40:
                SetEffectValue(656 + irand(1, 375));
                break;
            case SPELL_WARDER_BLAST_40:
                SetEffectValue(463 + irand(1, 133));
                break;
            default:
                // The breath chain keeps each spell's own base and only narrows
                // the spread, exactly as upstream leaves it.
                SetEffectValue(GetSpellInfo()->GetEffect(EFFECT_0).BasePoints + irand(1, BREATH_CHAIN_DIE_SIDES));
                break;
        }
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_onyxia40_damage::CalculateDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 18500 - Wing Buffet. Effect 0 is the knockback; the damage is on effect 1.
class spell_onyxia40_wing_buffet : public SpellScript
{
    PrepareSpellScript(spell_onyxia40_wing_buffet);

    void CalculateDamage(SpellEffIndex /*effIndex*/)
    {
        if (!VanillaRaids::IsOnyxia40(GetCaster()))
            return;

        SetEffectValue(562 + irand(1, 375));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_onyxia40_wing_buffet::CalculateDamage, EFFECT_1, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

void AddSC_onyxia40_spells()
{
    RegisterSpellScript(spell_onyxia40_damage);
    RegisterSpellScript(spell_onyxia40_wing_buffet);
}
