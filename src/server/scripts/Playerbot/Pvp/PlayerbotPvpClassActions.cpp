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

#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "Player.h"
#include "Protocol/Opcodes.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Unit.h"
#include "WorldSession.h"

#include <chrono>

namespace
{
char const* GetTargetModeLabel(playerbot::PvpClassSpellContext::TargetMode mode);

void NotifyDuelDecision(Player* player, playerbot::PvpClassSpellContext const& context, bool casted, std::string const& failureReason)
{
    if (!player || !player->duel)
        return;

    Player* opponent = player->duel->Opponent;
    if (opponent)
    {
        std::string message = "Decision: ";
        message += context.actionName ? context.actionName : "none";
        message += " | spell=" + std::to_string(context.spellId);
        message += " | target=";
        message += GetTargetModeLabel(context.targetMode);
        message += " | success=";
        message += casted ? "yes" : "no";
        message += " | reason=";
        message += context.reason ? context.reason : "none";

        player->Whisper(message, LANG_UNIVERSAL, opponent);
    }

    TC_LOG_DEBUG("playerbots.pvp.class",
        "[PvP duel] {} decision={} spell={} target={} success={} reason={}",
        player->GetName(), context.actionName ? context.actionName : "none", context.spellId,
        GetTargetModeLabel(context.targetMode), casted ? "yes" : "no", context.reason ? context.reason : "none");
}

void FinalizeVirtualNearTeleport(Player* player)
{
    if (!player || !player->IsBeingTeleportedNear())
        return;

    uint32 const oldZone = player->GetZoneId();
    WorldLocation const& dest = player->GetTeleportDest();

    player->SetSemaphoreTeleportNear(false);
    player->UpdatePosition(dest, true);
    player->SetFallInformation(0, player->GetPositionZ());

    uint32 newZone = 0;
    uint32 newArea = 0;
    player->GetZoneAndAreaId(newZone, newArea);
    player->UpdateZone(newZone, newArea);

    if (oldZone != newZone)
    {
        if (player->pvpInfo.IsHostile)
            player->CastSpell(player, 2479, true);
        else if (player->IsPvP() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
            player->UpdatePvP(false, false);
    }

    player->ResummonPetTemporaryUnSummonedIfAny();
    player->ProcessDelayedOperations();
}

char const* GetTargetModeLabel(playerbot::PvpClassSpellContext::TargetMode mode)
{
    switch (mode)
    {
        case playerbot::PvpClassSpellContext::TargetMode::Enemy: return "enemy";
        case playerbot::PvpClassSpellContext::TargetMode::Self: return "self";
        case playerbot::PvpClassSpellContext::TargetMode::Ally: return "ally";
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default: return "none";
    }
}

Unit* ResolveTarget(Player* player, playerbot::PvpClassSpellContext const& context)
{
    if (!player)
        return nullptr;

    switch (context.targetMode)
    {
        case playerbot::PvpClassSpellContext::TargetMode::Self:
            return player;
        case playerbot::PvpClassSpellContext::TargetMode::Ally:
        case playerbot::PvpClassSpellContext::TargetMode::Enemy:
            if (!context.targetGuid.IsEmpty())
                return ObjectAccessor::GetUnit(*player, context.targetGuid);
            return (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy) ? player->GetVictim() : nullptr;
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default:
            return nullptr;
    }
}

bool CastDirectSpell(Player* player, playerbot::PvpClassSpellContext const& context, std::string& failureReason)
{
    failureReason.clear();

    if (!player || !context.spellId || !player->HasSpell(context.spellId))
    {
        failureReason = "missing_spell";
        return false;
    }

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(context.spellId);
    if (!spellInfo)
    {
        failureReason = "spell_info_missing";
        return false;
    }

    if (player->GetSpellHistory()->HasCooldown(context.spellId) ||
        player->GetSpellHistory()->HasGlobalCooldown(spellInfo) ||
        player->IsNonMeleeSpellCast(false, false, true))
    {
        failureReason = "cooldown_or_casting";
        return false;
    }

    Unit* target = ResolveTarget(player, context);

    if (!target || !target->IsAlive())
    {
        failureReason = "target_invalid_or_dead";
        return false;
    }
    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self && target != player)
    {
        failureReason = "self_target_mismatch";
        return false;
    }

    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
    {
        if (!player->IsValidAttackTarget(target, spellInfo))
        {
            failureReason = "invalid_enemy_target";
            return false;
        }

        // Keep explicit enemy selection/victim linkage for virtual sessions so
        // cast checks and AI follow-up consistently reference the same hostile.
        player->SetSelection(target->GetGUID());
        if (player->GetVictim() != target)
            player->Attack(target, false);

        // Virtual sessions can visually "turn" while server-side facing checks
        // still fail for the immediate cast tick. SetInFront updates orientation
        // instantly, so facing-sensitive spells pass UNIT_NOT_INFRONT checks.
        player->SetFacingToObject(target);
        player->SetInFront(target);
    }
    else if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
    {
        if (!player->IsValidAssistTarget(target, spellInfo))
        {
            failureReason = "invalid_ally_target";
            return false;
        }
    }
    else if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::None)
    {
        failureReason = "target_mode_none";
        return false;
    }

    if (!player->IsWithinLOSInMap(target))
    {
        failureReason = "no_los";
        return false;
    }

    float const maxRange = spellInfo->GetMaxRange(false);
    if (maxRange > 0.0f && !player->IsWithinDistInMap(target, maxRange))
    {
        failureReason = "out_of_range";
        return false;
    }

    float const minRange = spellInfo->GetMinRange(false);
    if (minRange > 0.0f && player->IsWithinDistInMap(target, minRange))
    {
        failureReason = "too_close";
        return false;
    }

    if (spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
        if (player->GetPower(Powers(spellInfo->PowerType)) < int32(spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask())))
        {
            failureReason = "insufficient_power";
            return false;
        }

    // Cast-time spells like Frostbolt fail while moving. Since playerbots do
    // not have client-side stop-cast behavior, explicitly stop movement before
    // attempting non-instant casts.
    if (spellInfo->CalcCastTime() > 0)
        player->StopMoving();

    // Blink (1953) is a leap-forward spell with a destination target
    // (TARGET_DEST_CASTER_FRONT_LEAP). For virtual bot sessions, casting only
    // on a unit target can leave relocation unresolved; provide an explicit
    // front destination to mirror client cast payload semantics.
    SpellCastResult castResult = SPELL_FAILED_ERROR;
    if (context.spellId == 1953 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
    {
        Position const dest = player->GetFirstCollisionPosition(20.0f, player->GetOrientation());
        castResult = player->CastSpell(CastSpellTargetArg(dest), context.spellId);
    }
    else
        castResult = player->CastSpell(target, context.spellId, false);

    if (castResult != SPELL_CAST_OK)
        return false;

    // Hunter PvP trap setup: when Feign Death succeeds against a nearby melee
    // threat, pause movement, clear explicit target selection for visual parity,
    // then cast Freezing Trap exactly 500ms later before resuming chase.
    if (context.spellId == 5384)
    {
        Unit* pressureTarget = nullptr;
        if (!context.targetGuid.IsEmpty())
            pressureTarget = ObjectAccessor::GetUnit(*player, context.targetGuid);
        if (!pressureTarget)
            pressureTarget = player->GetVictim();

        bool const closeMeleePressure = pressureTarget && pressureTarget->IsAlive() && player->IsWithinDistInMap(pressureTarget, 5.0f);
        if (closeMeleePressure && player->HasSpell(1499))
        {
            ObjectGuid const hunterGuid = player->GetGUID();
            ObjectGuid const pressureTargetGuid = pressureTarget->GetGUID();

            player->StopMoving();
            player->SetSelection(ObjectGuid::Empty);

            player->m_Events.AddEventAtOffset([hunterGuid, pressureTargetGuid]()
            {
                Player* hunter = ObjectAccessor::FindConnectedPlayer(hunterGuid);
                if (!hunter || !hunter->IsInWorld() || !hunter->IsAlive())
                    return;

                SpellInfo const* freezingTrapInfo = sSpellMgr->GetSpellInfo(1499);
                if (freezingTrapInfo &&
                    !hunter->GetSpellHistory()->HasCooldown(1499) &&
                    !hunter->GetSpellHistory()->HasGlobalCooldown(freezingTrapInfo) &&
                    !hunter->IsNonMeleeSpellCast(false, false, true))
                {
                    SpellCastResult const trapCastResult = hunter->CastSpell(hunter, 1499, false);
                    if (trapCastResult != SPELL_CAST_OK)
                        TC_LOG_DEBUG("playerbots.pvp.class", "Hunter trap follow-up failed after feign death delay: guid={}, result={}.", hunter->GetGUID().ToString(), uint32(trapCastResult));
                }

                if (Unit* resumedTarget = ObjectAccessor::GetUnit(*hunter, pressureTargetGuid))
                    if (resumedTarget->IsAlive())
                        hunter->Attack(resumedTarget, false);
            }, std::chrono::milliseconds(500));
        }
    }

    bool hasTeleportEffect = false;
    for (uint8 effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
    {
        switch (spellInfo->GetEffect(SpellEffIndex(effectIndex)).Effect)
        {
            case SPELL_EFFECT_TELEPORT_UNITS:
            case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
            case SPELL_EFFECT_LEAP:
            case SPELL_EFFECT_JUMP:
            case SPELL_EFFECT_JUMP_DEST:
            case SPELL_EFFECT_LEAP_BACK:
            case SPELL_EFFECT_CHARGE:
            case SPELL_EFFECT_CHARGE_DEST:
                hasTeleportEffect = true;
                break;
            default:
                break;
        }

        if (hasTeleportEffect)
            break;
    }

    // Bot players do not own a real game client to naturally ACK near teleports
    // (for example Blink). If a teleport is still pending after cast, synthesize
    // the teleport ACK immediately so other combat actions (like Charge) resolve
    // against the post-Blink location.
    if (hasTeleportEffect && player->IsBeingTeleportedNear())
    {
        WorldSession* session = player->GetSession();
        if (session && session->IsVirtualSession())
        {
            TC_LOG_DEBUG("playerbots.pvp.class",
                "Playerbot PvP teleport ACK synthesized: guid={} spell={} map={} x={} y={} z={}.",
                player->GetGUID().ToString(), context.spellId, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            WorldPacket teleportAck(MSG_MOVE_TELEPORT_ACK, 20);
            teleportAck << player->GetPackGUID();
            teleportAck << uint32(0);
            teleportAck << uint32(0);
            session->HandleMoveTeleportAck(teleportAck);

            if (player->IsBeingTeleportedNear())
                FinalizeVirtualNearTeleport(player);
        }
    }

    // Avoid immediate reapplication loops after quick dispels by imposing
    // short tactical cooldowns on selected PvP debuffs.
    if (context.spellId == 112826)
        player->GetSpellHistory()->AddCooldown(context.spellId, 0, std::chrono::seconds(15));
    if (context.spellId == 3034 || context.spellId == 1714)
        player->GetSpellHistory()->AddCooldown(context.spellId, 0, std::chrono::seconds(12));

    return true;
}

bool UseDirectItem(Player* player, playerbot::PvpClassSpellContext const& context, std::string& failureReason)
{
    failureReason.clear();
    if (!player || !context.itemEntry)
    {
        failureReason = "missing_item_entry";
        return false;
    }

    Item* item = player->GetItemByEntry(context.itemEntry);
    if (!item)
    {
        failureReason = "item_missing";
        return false;
    }

    if (player->CanUseItem(item) != EQUIP_ERR_OK)
    {
        failureReason = "item_unusable";
        return false;
    }

    SpellCastTargets targets;
    targets.SetUnitTarget(player);
    player->CastItemUseSpell(item, targets, 1, 0);
    return true;
}
}

namespace playerbot
{
bool PvpClassActions::Execute(Player* player, PvpClassSpellContext const& context)
{
    if (!player || !context.classSpellsEnabled || !context.shouldExecute)
        return false;

    std::string failureReason;
    bool casted = false;
    if (context.itemEntry)
        casted = UseDirectItem(player, context, failureReason);
    else
        casted = CastDirectSpell(player, context, failureReason);
    NotifyDuelDecision(player, context, casted, failureReason);
    TC_LOG_DEBUG("playerbots.pvp.class",
        "Playerbot PvP class execution: action={} spell={} target_mode={} target_guid={} success={} reason={}.",
        context.actionName ? context.actionName : "none",
        context.spellId,
        GetTargetModeLabel(context.targetMode),
        context.targetGuid.ToString(),
        casted,
        context.reason ? context.reason : "none");
    return casted;
}
}
