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

#ifndef TRINITY_PLAYERBOT_PVP_CLASS_ACTIONS_H
#define TRINITY_PLAYERBOT_PVP_CLASS_ACTIONS_H

#include "PlayerbotPvpCore.h"

#include <chrono>
#include <string>

class Player;
class Unit;
class SpellInfo;

namespace playerbot
{
class PvpClassActions
{
public:
    static bool AreRehgarMovementDiagnosticsEnabled();

    static bool Execute(Player* player, PvpClassSpellContext const& context);
    // Seams for the PvE manager: player MotionMasters can sit in a pending/
    // uninitialized state (see PrepareMotionMasterForExplicitBotMovement), so
    // external modules must issue follow/point movement through these instead
    // of calling MotionMaster directly.
    static bool PrepareForExplicitMovement(Player* player);
    static bool IssueFollowMovement(Player* player, Unit* target, float desiredDistance);
    static bool IsWarlockCurseTargetCooldownActive(Player const* player, Unit const* target, uint32 spellId);
    static void RegisterWarlockCurseTargetCooldown(Player const* player, Unit const* target, uint32 spellId, std::chrono::seconds cooldown);
    static bool IsCasterSpellCooldownActive(Player const* player, uint32 spellId);

    // True when this cast would deliver literally nothing because the target is
    // immune to all of it. Lives here rather than in either caller because both
    // the PvP decision engine and the PvE manager's direct-cast floors need the
    // same answer, and two copies of a rule like this drift.
    static bool IsCastWastedOnTargetImmunity(Unit const* caster, Unit const* target, SpellInfo const* spellInfo);

    // Get a bot off its mount COMPLETELY - flag, model, auras and speed.
    // Unit::Dismount alone does not: see the definition.
    static void ForceDismount(Player* player);

    // Dismount because the bot is now close enough to fight this target. Shared
    // by the PvP and PvE engagement paths so both stop riding at the same moment.
    static void DismountToFight(Player* player, Unit const* victim);
    static void RegisterCasterSpellCooldown(Player const* player, uint32 spellId, std::chrono::seconds cooldown);
    static void RegisterCasterSpellCooldown(Player const* player, uint32 spellId, std::chrono::milliseconds cooldown);
    static std::string GetLastExecutionStatus(Player const* player);
    static std::string GetLastMovementDebugStatus(Player const* player);
    static bool HasRecentTargetRelativeMovementOrder(Player const* player, Unit const* target, uint32 maxAgeMs = 1500);
    static bool IsBattlegroundObjectInteractionInProgress(Player const* player);
    static bool IsPetSpellAction(Player const* player, PvpClassSpellContext const& context);
    static bool TryIssueShadowWraithFleeMovement(Player* player, Unit* threat);
    // Seam for the PvE manager: send the bot's pet at its victim. A pet that
    // is not attacking cannot growl, cannot hold threat and contributes
    // nothing, and the PvE tick had no way to command one at all.
    static void CommandPetAttack(Player* player, Unit* target);

    // Health (food) or mana (drink) per five seconds that the free eat/drink
    // fallback should restore to a bot of this level.
    //
    // A bot with something edible in its bags eats the real item and gets that
    // item's own level-appropriate aura. A bot with nothing eats for free
    // instead of standing there, and the two spells that pays for - 29073 "Eat"
    // and 22734 "Drink" - carry flat level-65 amounts, 530 health and 700 mana
    // per five seconds. Player::RegenerateHealth pays out two fifths of that
    // every two-second tick, so on a low level bot whose whole pool is a few
    // hundred points the first tick is the whole bar. That is what players were
    // seeing in the open world: a wounded bot sits down and is instantly full.
    //
    // Shared rather than duplicated because the PvE manager and the PvP cast
    // dispatcher both reach for this fallback, and a ladder kept in two places
    // is a ladder that drifts.
    static int32 FreeRefreshmentAmount(Player const* player, bool drink);
};
}

#endif
