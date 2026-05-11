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

#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"
#include "Util.h"

namespace
{
enum SurpriseBearSpells
{
    SPELL_DRUID_SURPRISE_BEAR      = 89759,
    SPELL_DRUID_PRIMAL_FRENZY_R1   = 16952
};

bool IsSuccessfulComboHit(SpellInfo const* spellInfo, SpellMissInfo missInfo)
{
    if (missInfo == SPELL_MISS_NONE)
        return true;

    return missInfo == SPELL_MISS_BLOCK && !spellInfo->HasAttribute(SPELL_ATTR3_COMPLETELY_BLOCKED);
}

bool RollPrimalFrenzyBonus(Unit* caster)
{
    AuraEffect const* primalFrenzy = caster->GetAuraEffectOfRankedSpell(SPELL_DRUID_PRIMAL_FRENZY_R1, EFFECT_0, caster->GetGUID());
    if (!primalFrenzy)
        primalFrenzy = caster->GetAuraEffectOfRankedSpell(SPELL_DRUID_PRIMAL_FRENZY_R1, EFFECT_0);

    if (!primalFrenzy)
        return false;

    float chance = float(primalFrenzy->GetSpellInfo()->ProcChance);
    if (Player* modOwner = caster->GetSpellModOwner())
        modOwner->ApplySpellMod(primalFrenzy->GetId(), SPELLMOD_CHANCE_OF_SUCCESS, chance);

    return roll_chance_f(chance);
}
}

class spell_dru_surprise_bear_combo : public SpellScript
{
    PrepareSpellScript(spell_dru_surprise_bear_combo);

public:
    spell_dru_surprise_bear_combo() = default;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_DRUID_SURPRISE_BEAR, SPELL_DRUID_PRIMAL_FRENZY_R1 });
    }

    void CheckHit(SpellMissInfo missInfo)
    {
        _canAwardComboPoint = IsSuccessfulComboHit(GetSpellInfo(), missInfo);
    }

    void AwardComboPoint()
    {
        if (!_canAwardComboPoint)
            return;

        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_DRUID_SURPRISE_BEAR))
            return;

        if (!_comboPointTarget)
        {
            _comboPointTarget = target;
            GetSpell()->AddComboPointGain(_comboPointTarget, 1);
        }

        if (!_primalFrenzyChecked && (GetSpell()->GetHitMask() & PROC_HIT_CRITICAL))
        {
            _primalFrenzyChecked = true;
            if (RollPrimalFrenzyBonus(caster))
                GetSpell()->AddComboPointGain(_comboPointTarget, 1);
        }
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_dru_surprise_bear_combo::CheckHit);
        AfterHit += SpellHitFn(spell_dru_surprise_bear_combo::AwardComboPoint);
    }

private:
    bool _canAwardComboPoint = false;
    bool _primalFrenzyChecked = false;
    Unit* _comboPointTarget = nullptr;
};

class spell_dru_surprise_bear_combo_loader : public SpellScriptLoader
{
public:
    spell_dru_surprise_bear_combo_loader() : SpellScriptLoader("spell_dru_surprise_bear_combo") { }

    SpellScript* GetSpellScript() const override
    {
        return new spell_dru_surprise_bear_combo();
    }
};

void AddSC_custom_surprise_bear()
{
    new spell_dru_surprise_bear_combo_loader();
}
