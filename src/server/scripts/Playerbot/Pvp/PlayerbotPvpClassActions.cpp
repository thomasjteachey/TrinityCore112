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

#include "PlayerbotPvpClassActions.h"

#include "Player.h"
#include "Unit.h"

#include <array>

namespace
{
uint32 FindKnownSpell(Player const* player, std::initializer_list<uint32> spellIds)
{
    if (!player)
        return 0;

    for (uint32 spellId : spellIds)
        if (player->HasSpell(spellId))
            return spellId;

    return 0;
}

uint32 ResolveSpellId(Player const* player, playerbot::PvpClassSpellActionType actionType)
{
    switch (actionType)
    {
        case playerbot::PvpClassSpellActionType::Charge:
            return FindKnownSpell(player, { 11578, 11577, 100 });
        case playerbot::PvpClassSpellActionType::BattleStance:
            return FindKnownSpell(player, { 2457 });
        case playerbot::PvpClassSpellActionType::BattleShout:
            return FindKnownSpell(player, { 47436, 47435, 6673 });
        case playerbot::PvpClassSpellActionType::MortalStrike:
            return FindKnownSpell(player, { 47486, 47485, 12294 });
        case playerbot::PvpClassSpellActionType::Execute:
            return FindKnownSpell(player, { 47471, 25236, 20662, 20661, 5308 });
        case playerbot::PvpClassSpellActionType::Overpower:
            return FindKnownSpell(player, { 7384 });
        case playerbot::PvpClassSpellActionType::Hamstring:
            return FindKnownSpell(player, { 1715 });
        case playerbot::PvpClassSpellActionType::HeroicStrike:
            return FindKnownSpell(player, { 47450, 47449, 78 });
        case playerbot::PvpClassSpellActionType::None:
        default:
            break;
    }

    return 0;
}

bool CastResolvedSpell(Player* player, playerbot::PvpClassSpellActionType actionType)
{
    if (!player || actionType == playerbot::PvpClassSpellActionType::None)
        return false;

    if (player->IsNonMeleeSpellCast(false))
        return false;

    uint32 const spellId = ResolveSpellId(player, actionType);
    if (!spellId)
        return false;

    Unit* target = player;
    if (actionType != playerbot::PvpClassSpellActionType::BattleStance &&
        actionType != playerbot::PvpClassSpellActionType::BattleShout)
    {
        target = player->GetVictim();
        if (!target || !target->IsAlive())
            return false;
    }

    player->CastSpell(target, spellId, false);
    return true;
}
}

namespace playerbot
{
bool PvpClassActions::Execute(Player* player, PvpClassSpellContext const& context)
{
    if (!player || !context.classSpellsEnabled || !context.shouldExecute)
        return false;

    return CastResolvedSpell(player, context.actionType);
}
}
