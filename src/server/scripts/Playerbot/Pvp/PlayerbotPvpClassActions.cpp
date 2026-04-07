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
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Unit.h"
#include "WorldSession.h"

namespace
{
char const* GetTargetModeLabel(playerbot::PvpClassSpellContext::TargetMode mode);

void NotifyDuelDecision(Player* player, playerbot::PvpClassSpellContext const& context, bool casted)
{
    if (!player || !player->duel || player->duel->State != DUEL_STATE_IN_PROGRESS || !player->duel->Opponent)
        return;

    Player* opponent = player->duel->Opponent;
    if (!opponent || !opponent->GetSession())
        return;

    static std::unordered_map<uint64, uint32> s_LastDecisionWhisperByBot;
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32& lastWhisperMs = s_LastDecisionWhisperByBot[player->GetGUID().GetRawValue()];
    if ((nowMs - lastWhisperMs) < 700)
        return;

    std::ostringstream message;
    message << "[PvP duel] " << player->GetName() << " decision="
        << (context.actionName ? context.actionName : "none")
        << " spell=" << context.spellId
        << " target=" << GetTargetModeLabel(context.targetMode)
        << " success=" << (casted ? "yes" : "no")
        << " reason=" << (context.reason ? context.reason : "none");

    player->Whisper(message.str(), LANG_UNIVERSAL, opponent);
    lastWhisperMs = nowMs;
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

bool CastDirectSpell(Player* player, playerbot::PvpClassSpellContext const& context)
{
    if (!player || !context.spellId || !player->HasSpell(context.spellId))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(context.spellId);
    if (!spellInfo)
        return false;

    if (player->GetSpellHistory()->HasCooldown(context.spellId) ||
        player->GetSpellHistory()->HasGlobalCooldown(spellInfo) ||
        player->IsNonMeleeSpellCast(false, false, true))
        return false;

    Unit* target = ResolveTarget(player, context);

    if (!target || !target->IsAlive())
        return false;
    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self && target != player)
        return false;

    if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
    {
        if (!player->IsValidAttackTarget(target, spellInfo))
            return false;
    }
    else if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally)
    {
        if (!player->IsValidAssistTarget(target, spellInfo))
            return false;
    }
    else if (context.targetMode == playerbot::PvpClassSpellContext::TargetMode::None)
        return false;

    if (!player->IsWithinLOSInMap(target))
        return false;

    float const maxRange = spellInfo->GetMaxRange(false);
    if (maxRange > 0.0f && !player->IsWithinDistInMap(target, maxRange))
        return false;

    float const minRange = spellInfo->GetMinRange(false);
    if (minRange > 0.0f && player->IsWithinDistInMap(target, minRange))
        return false;

    if (spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
        if (player->GetPower(Powers(spellInfo->PowerType)) < int32(spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask())))
            return false;

    // Blink (1953) is a leap-forward spell with a destination target
    // (TARGET_DEST_CASTER_FRONT_LEAP). For virtual bot sessions, casting only
    // on a unit target can leave relocation unresolved; provide an explicit
    // front destination to mirror client cast payload semantics.
    if (context.spellId == 1953 && context.targetMode == playerbot::PvpClassSpellContext::TargetMode::Self)
    {
        Position const dest = player->GetFirstCollisionPosition(20.0f, player->GetOrientation());
        player->CastSpell(CastSpellTargetArg(dest), context.spellId);
    }
    else
        player->CastSpell(target, context.spellId, false);

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
        }
    }

    return true;
}
}

namespace playerbot
{
bool PvpClassActions::Execute(Player* player, PvpClassSpellContext const& context)
{
    if (!player || !context.classSpellsEnabled || !context.shouldExecute)
        return false;

    bool const casted = CastDirectSpell(player, context);
    NotifyDuelDecision(player, context, casted);
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
