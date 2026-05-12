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
#include "ItemTemplate.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellScript.h"

namespace PolearmStaffInnerAuras
{
namespace
{
struct AuraMapping
{
    uint32 OuterSpellId;
    uint32 InnerSpellId;
};

constexpr AuraMapping AuraMappings[] =
{
    { 12165, 89769 },
    { 12830, 89770 },
    { 12831, 89771 },
    { 12832, 89772 },
    { 12833, 89773 }
};

bool IsPolearmOrStaffEquipped(Player const* player)
{
    if (!player)
        return false;

    Item const* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!weapon)
        return false;

    ItemTemplate const* weaponTemplate = weapon->GetTemplate();
    if (!weaponTemplate || weaponTemplate->Class != ITEM_CLASS_WEAPON)
        return false;

    return weaponTemplate->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM || weaponTemplate->SubClass == ITEM_SUBCLASS_WEAPON_STAFF;
}

uint32 GetInnerSpellForOuter(uint32 outerSpellId)
{
    for (AuraMapping const& mapping : AuraMappings)
        if (mapping.OuterSpellId == outerSpellId)
            return mapping.InnerSpellId;

    return 0;
}

bool ShouldHaveInnerAura(Player const* player, AuraMapping const& mapping)
{
    return player->HasAura(mapping.OuterSpellId) && IsPolearmOrStaffEquipped(player);
}
}

void Sync(Player* player)
{
    if (!player)
        return;

    for (AuraMapping const& mapping : AuraMappings)
    {
        bool const shouldHaveInnerAura = ShouldHaveInnerAura(player, mapping);
        bool const hasInnerAura = player->HasAura(mapping.InnerSpellId);

        if (shouldHaveInnerAura)
        {
            if (!hasInnerAura)
                player->CastSpell(player, mapping.InnerSpellId, true);
        }
        else if (hasInnerAura)
            player->RemoveAurasDueToSpell(mapping.InnerSpellId);
    }
}

void OnEquipmentChanged(Player* player)
{
    Sync(player);
}
}

class spell_polearm_staff_outer_aura : public AuraScript
{
    PrepareAuraScript(spell_polearm_staff_outer_aura);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr)
            PolearmStaffInnerAuras::Sync(player);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr)
            if (uint32 innerSpellId = PolearmStaffInnerAuras::GetInnerSpellForOuter(GetSpellInfo()->Id))
                player->RemoveAurasDueToSpell(innerSpellId);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_polearm_staff_outer_aura::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(spell_polearm_staff_outer_aura::HandleRemove, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_polearm_staff_outer_aura_loader : public SpellScriptLoader
{
public:
    spell_polearm_staff_outer_aura_loader() : SpellScriptLoader("spell_polearm_staff_outer_aura") { }

    AuraScript* GetAuraScript() const override
    {
        return new spell_polearm_staff_outer_aura();
    }
};

class polearm_staff_inner_aura_player : public PlayerScript
{
public:
    polearm_staff_inner_aura_player() : PlayerScript("polearm_staff_inner_aura_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        PolearmStaffInnerAuras::Sync(player);
    }
};

void AddSC_custom_polearm_staff_inner_auras()
{
    new spell_polearm_staff_outer_aura_loader();
    new polearm_staff_inner_aura_player();
}
