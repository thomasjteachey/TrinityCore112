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
#include "SpellHistory.h"
#include "Unit.h"

namespace
{
bool CastDirectSpell(Player* player, uint32 spellId, bool selfCast)
{
    if (!player || !spellId || !player->HasSpell(spellId))
        return false;

    if (player->GetSpellHistory()->HasCooldown(spellId) || player->IsNonMeleeSpellCast(false))
        return false;

    Unit* target = selfCast ? static_cast<Unit*>(player) : player->GetVictim();
    if (!target || !target->IsAlive())
        return false;

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

    return CastDirectSpell(player, context.spellId, context.selfCast);
}
}
