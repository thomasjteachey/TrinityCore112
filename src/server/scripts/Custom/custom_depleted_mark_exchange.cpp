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

#include "Item.h"
#include "Miscellaneous/DepletedMarks.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellScript.h"

class spell_depleted_mark_converter : public SpellScript
{
    PrepareSpellScript(spell_depleted_mark_converter);

public:
    spell_depleted_mark_converter() = default;

    SpellCastResult CheckRequirement()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return SPELL_FAILED_DONT_REPORT;

        _rewardEntry = Trinity::Custom::GetDepletedMarkEntryForPlayer(player);
        if (!_rewardEntry)
            return SPELL_FAILED_DONT_REPORT;

        if (!Trinity::Custom::HasEnoughIneligibleDepletedMarks(player, Trinity::Custom::DEPLETED_MARK_CONVERSION_COST))
            return SPELL_FAILED_NOT_ENOUGH_ITEMS;

        _dest.clear();
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, _dest, _rewardEntry, 1) != EQUIP_ERR_OK)
            return SPELL_FAILED_TOO_MANY_OF_ITEM;

        return SPELL_CAST_OK;
    }

    void HandleScript(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);

        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || !_rewardEntry)
            return;

        if (!Trinity::Custom::ConsumeIneligibleDepletedMarks(player, Trinity::Custom::DEPLETED_MARK_CONVERSION_COST))
        {
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, GetCastItem(), nullptr);
            return;
        }

        if (Item* newItem = player->StoreNewItem(_dest, _rewardEntry, true))
            player->SendNewItem(newItem, 1, true, false);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_depleted_mark_converter::CheckRequirement);
        OnEffectHitTarget += SpellEffectFn(spell_depleted_mark_converter::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }

private:
    ItemPosCountVec _dest;
    uint32 _rewardEntry = 0;
};

class spell_depleted_mark_converter_loader : public SpellScriptLoader
{
public:
    spell_depleted_mark_converter_loader() : SpellScriptLoader("spell_depleted_mark_converter") { }

    SpellScript* GetSpellScript() const override
    {
        return new spell_depleted_mark_converter();
    }
};

void AddSC_custom_depleted_mark_exchange()
{
    new spell_depleted_mark_converter_loader();
}
