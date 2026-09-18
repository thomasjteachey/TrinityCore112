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

#include "PetAI.h"
#include "AIException.h"
#include "Chat.h"
#include "Creature.h"
#include "Errors.h"
#include "Group.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Util.h"
#include "WorldSession.h"

namespace
{
    bool HasCreatureSpells(Creature const* creature)
    {
        for (uint32 spellId : creature->m_spells)
            if (spellId)
                return true;

        return false;
    }

    bool IsPlayerOwnedSpellGuardian(Creature const* creature)
    {
        if (!creature->IsGuardian() || !HasCreatureSpells(creature))
            return false;

        Unit* owner = reinterpret_cast<Guardian const*>(creature)->GetOwner();
        return owner && owner->GetTypeId() == TYPEID_PLAYER;
    }

    bool ShouldUseCreatureSpellAutocast(Creature const* creature)
    {
        return IsPlayerOwnedSpellGuardian(creature) && !creature->IsPet();
    }

    bool IsHealingSpell(SpellInfo const* spellInfo)
    {
        return std::any_of(spellInfo->GetEffects().begin(), spellInfo->GetEffects().end(), [](SpellEffectInfo const& effect)
        {
            return effect.IsEffect(SPELL_EFFECT_HEAL);
        });
    }

    // ---------------------------------------------------------------------
    // ".gm diagnostics on pet"
    //
    // A running commentary on why a pet engaged, disengaged or refused to
    // engage, delivered to the pet's own owner and to nobody else. It exists
    // because the pet re-validates its target on EVERY tick while the player
    // who ordered the attack validated it once, so a pet can walk home from a
    // target its owner is still happily hitting, with nothing on screen to say
    // why.
    //
    // Two rules keep it honest and cheap:
    //
    //  - Every line is a STATE CHANGE, never a per-tick sample. PetAI::UpdateAI
    //    runs for every pet on the realm every world tick; a line per tick would
    //    be unreadable and would drown the chat frame in seconds.
    //  - Nothing is computed before the listener is known. PetDiagListener() is
    //    three pointer hops and a mask test, and it comes FIRST - the aura walks
    //    and the string building below only ever run for a pet somebody asked
    //    about. (An earlier diagnostic in this codebase got this wrong and paid
    //    for two VMAP raycasts per chase tick on every player-owned unit whether
    //    or not anyone was listening.)
    Player* PetDiagListener(Creature const* me)
    {
        Unit* owner = me->GetCharmerOrOwner();
        if (!owner)
            return nullptr;

        Player* player = owner->ToPlayer();
        if (!player)
            return nullptr;

        WorldSession* session = player->GetSession();
        if (!session || !session->IsGmDiagnosticEnabled(GmDiagnosticCategory::Pet))
            return nullptr;

        return player;
    }

    char const* ControlAuraTypeName(AuraType type)
    {
        switch (type)
        {
            case SPELL_AURA_MOD_CONFUSE: return "confuse";
            case SPELL_AURA_MOD_FEAR:    return "fear";
            case SPELL_AURA_MOD_STUN:    return "stun";
            case SPELL_AURA_MOD_ROOT:    return "root";
            case SPELL_AURA_TRANSFORM:   return "transform";
            default:                     return "control";
        }
    }

    // Mirrors Unit::HasBreakableByDamageCrowdControlAura - the same five aura
    // types, the same "ignore my own channel" rule - but hands back the aura
    // that answered yes instead of a bare bool.
    //
    // Naming the spell is the entire point. That function's idea of "crowd
    // control" is much wider than a player's: any ROOT, STUN, FEAR, CONFUSE or
    // TRANSFORM whose spell carries AURA_INTERRUPT_FLAG_TAKE_DAMAGE counts. A
    // Frost Nova, an Entangling Roots, a Freezing Trap or a scripted knockdown
    // all qualify, and none of them look like crowd control to the person
    // watching their pet turn around and walk away.
    AuraEffect const* FindBreakableControlAura(Unit const* victim, Unit* excludeCasterChannel, AuraType& typeOut)
    {
        uint32 excludeAura = 0;
        if (Spell* channeled = excludeCasterChannel ? excludeCasterChannel->GetCurrentSpell(CURRENT_CHANNELED_SPELL) : nullptr)
            excludeAura = channeled->GetSpellInfo()->Id;

        AuraType const controlTypes[] =
        {
            SPELL_AURA_MOD_CONFUSE,
            SPELL_AURA_MOD_FEAR,
            SPELL_AURA_MOD_STUN,
            SPELL_AURA_MOD_ROOT,
            SPELL_AURA_TRANSFORM
        };

        for (AuraType type : controlTypes)
            for (AuraEffect const* effect : victim->GetAuraEffectsByType(type))
                if ((!excludeAura || excludeAura != effect->GetSpellInfo()->Id) &&
                    (effect->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE))
                {
                    typeOut = type;
                    return effect;
                }

        return nullptr;
    }

    // Why WorldObject::IsValidAttackTarget said no, walked in the same order it
    // walks so the answer is the check the pet actually tripped over.
    //
    // The friendliness answer at the bottom is the interesting one on this
    // realm. Everything is one faction here, so an enemy player or playerbot is
    // only attackable because a pseudo-faction rule higher up in that function
    // says so - and those rules read live state (the bot's FFA byte, sanctuary,
    // War Mode). The moment one of them goes quiet mid-fight the target reads
    // as friendly, and the pet, unlike its owner, notices immediately.
    char const* DescribeInvalidAttackTarget(Creature const* me, Unit const* victim)
    {
        if (!victim->IsAlive())
            return "target is dead";

        if (victim->HasUnitState(UNIT_STATE_UNATTACKABLE))
            return "target is in an unattackable state (evading, or mid-teleport)";

        if (victim->GetTypeId() == TYPEID_PLAYER && victim->ToPlayer()->IsGameMaster())
            return "target is a game master";

        if (me->GetPhaseMask() != victim->GetPhaseMask())
            return "phase mismatch - pet and target are no longer in the same phase";

        if (!me->CanSeeOrDetect(victim))
            return "pet can no longer see or detect the target (stealth, invisibility or visibility rules)";

        if (victim->HasUnitFlag(UNIT_FLAG_UNINTERACTIBLE))
            return "target is flagged uninteractible";

        if (victim->HasUnitFlag(UNIT_FLAG_ON_TAXI))
            return "target boarded a taxi";

        if (victim->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_ATTACKABLE_1 | UNIT_FLAG_NON_ATTACKABLE_2))
            return "target carries a non-attackable unit flag";

        if (victim->IsImmuneToNPC())
            return "target is immune to NPCs, and a pet is an NPC (its owner is not)";

        if (me->IsInSanctuary())
            return "pet is standing in a sanctuary";

        if (victim->IsInSanctuary())
            return "target is standing in a sanctuary";

        if (Player const* victimPlayer = victim->ToPlayer())
            if (!victimPlayer->IsPvP() && !victimPlayer->IsFFAPvP())
                return "target player is no longer PvP or FFA flagged";

        if (me->IsFriendlyTo(victim) || victim->IsFriendlyTo(me))
            return "target now reads as friendly to the pet";

        return "IsValidAttackTarget refused for a reason not broken out here";
    }
}

int32 PetAI::Permissible(Creature const* creature)
{
    if (creature->HasUnitTypeMask(UNIT_MASK_CONTROLABLE_GUARDIAN))
    {
        if (Unit const* owner = reinterpret_cast<Guardian const*>(creature)->GetOwner())
            if (owner->GetTypeId() == TYPEID_PLAYER)
                return PERMIT_BASE_PROACTIVE;

        return PERMIT_BASE_REACTIVE;
    }

    if (IsPlayerOwnedSpellGuardian(creature))
        return PERMIT_BASE_PROACTIVE;

    return PERMIT_BASE_NO;
}

PetAI::PetAI(Creature* creature) : CreatureAI(creature), _tracker(TIME_INTERVAL_LOOK), _lastCrowdControlledVictim(ObjectGuid::Empty), _queuedSpellTarget(ObjectGuid::Empty), _queuedSpellId(0),
    _diagnosticVictim(ObjectGuid::Empty), _diagnosticRefusedTarget(ObjectGuid::Empty), _diagnosticRefusalReason(nullptr),
    _diagnosticMotionType(MAX_MOTION_TYPE)
{
    if (!me->GetCharmInfo())
        throw InvalidAIException("Creature doesn't have a valid charm info");

    UpdateAllies();
}

void PetAI::QueueSpell(uint32 spellId, SpellCastTargets const& targets)
{
    _queuedSpellId = spellId;
    _queuedSpellTargets = targets;
    _queuedSpellTarget = targets.GetUnitTargetGUID();

    if (Unit* target = _queuedSpellTargets.GetUnitTarget())
        _AttackStart(target);
}

void PetAI::ClearQueuedSpell()
{
    _queuedSpellTargets = SpellCastTargets();
    _queuedSpellTarget.Clear();
    _queuedSpellId = 0;
}

void PetAI::ProcessSpellQueue()
{
    if (!_queuedSpellId || me->HasUnitState(UNIT_STATE_CASTING))
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(_queuedSpellId);
    if (!spellInfo)
    {
        ClearQueuedSpell();
        return;
    }

    Unit* target = _queuedSpellTargets.GetUnitTarget();
    if (!target && !_queuedSpellTarget.IsEmpty())
        target = ObjectAccessor::GetUnit(*me, _queuedSpellTarget);

    if (!target || !target->IsAlive())
    {
        ClearQueuedSpell();
        return;
    }

    _queuedSpellTargets.SetUnitTarget(target);

    Spell* spell = new Spell(me, spellInfo, TRIGGERED_NONE);
    spell->m_targets = _queuedSpellTargets;

    SpellCastResult result = spell->CheckPetCast(target);

    if (result == SPELL_CAST_OK)
    {
        spell->prepare(spell->m_targets);
        ClearQueuedSpell();
        return;
    }

    float desiredRange = spellInfo->GetMinRange(false);

    spell->finish(false);
    delete spell;

    if (result == SPELL_FAILED_OUT_OF_RANGE)
    {
        _AttackStart(target);
        me->GetMotionMaster()->MoveChase(target, desiredRange);
        return;
    }

    if (result == SPELL_FAILED_TOO_CLOSE)
    {
        me->GetMotionMaster()->MoveChase(target, desiredRange);
        return;
    }

    ClearQueuedSpell();
}

void PetAI::UpdateAI(uint32 diff)
{
    if (!me->IsAlive() || !me->GetCharmInfo())
        return;

    Unit* owner = me->GetCharmerOrOwner();

    ReportDiagnosticVictimChange();

    ProcessSpellQueue();

    if (_updateAlliesTimer <= diff)
        // UpdateAllies self set update timer
        UpdateAllies();
    else
        _updateAlliesTimer -= diff;

    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
    {
        Unit* victim = me->EnsureVictim();
        // is only necessary to stop casting, the pet must not exit combat
        if (!me->GetCurrentSpell(CURRENT_CHANNELED_SPELL) && // ignore channeled spells (Pin, Seduction)
            victim->HasBreakableByDamageCrowdControlAura(me))
        {
            ObjectGuid victimGuid = victim->GetGUID();
            if (_lastCrowdControlledVictim != victimGuid)
            {
                _lastCrowdControlledVictim = victimGuid;

                // Name the spell, not just the verdict. "Its target isn't crowd
                // controlled" is the commonest thing an owner says about this
                // branch, and they are usually right by their own definition and
                // wrong by HasBreakableByDamageCrowdControlAura's.
                if (Player* listener = PetDiagListener(me))
                {
                    ChatHandler handler(listener->GetSession());
                    AuraType controlType = SPELL_AURA_NONE;
                    if (AuraEffect const* control = FindBreakableControlAura(victim, me, controlType))
                    {
                        SpellInfo const* controlSpell = control->GetSpellInfo();
                        char const* spellName = controlSpell->SpellName[handler.GetSessionDbcLocale()];
                        if (!spellName)
                            spellName = controlSpell->SpellName[LOCALE_enUS];

                        handler.PSendSysMessage(
                            "[PetDiag] Breaking off %s: \"%s\" (spell %u) is on it - a %s that breaks on damage, %d ms left.",
                            victim->GetName().c_str(), spellName ? spellName : "<unnamed>", controlSpell->Id,
                            ControlAuraTypeName(controlType), control->GetBase()->GetDuration());
                    }
                    else
                        handler.PSendSysMessage(
                            "[PetDiag] Breaking off %s: it has a damage-breakable control aura (it expired before it could be named).",
                            victim->GetName().c_str());
                }

                me->InterruptNonMeleeSpells(false);
                StopAttack("its target picked up a damage-breakable control aura");
                return;
            }
        }
        else if (!_lastCrowdControlledVictim.IsEmpty() && _lastCrowdControlledVictim == victim->GetGUID())
            _lastCrowdControlledVictim.Clear();

        char const* stopReason = nullptr;
        if (NeedToStop(&stopReason))
        {
            TC_LOG_TRACE("scripts.ai.petai", "PetAI::UpdateAI: AI stopped attacking {}", me->GetGUID().ToString());
            StopAttack(stopReason);
            return;
        }

        // Check before attacking to prevent pets from leaving stay position
        if (me->GetCharmInfo()->HasCommandState(COMMAND_STAY))
        {
            if (me->GetCharmInfo()->IsCommandAttack() || (me->GetCharmInfo()->IsAtStay() && me->IsWithinMeleeRange(me->GetVictim())))
                DoMeleeAttackIfReady();
        }
        else
            DoMeleeAttackIfReady();
    }
    else
    {
        if (!_lastCrowdControlledVictim.IsEmpty())
            _lastCrowdControlledVictim.Clear();
        if (me->HasReactState(REACT_AGGRESSIVE) || me->GetCharmInfo()->IsAtStay())
        {
            // Every update we need to check targets only in certain cases
            // Aggressive - Allow auto select if owner or pet don't have a target
            // Stay - Only pick from pet or owner targets / attackers so targets won't run by
            //   while chasing our owner. Don't do auto select.
            // All other cases (ie: defensive) - Targets are assigned by DamageTaken(), OwnerAttackedBy(), OwnerAttacked(), etc.
            Unit* nextTarget = SelectNextTarget(me->HasReactState(REACT_AGGRESSIVE));

            if (nextTarget)
                AttackStart(nextTarget);
            else
                HandleReturnMovement("it has no target and found nothing worth picking up");
        }
        else
            HandleReturnMovement("it has no target, and a non-aggressive pet does not go looking for one");
    }

    // Autocast (cast only in combat or persistent spells in any state)
    if (!me->HasUnitState(UNIT_STATE_CASTING))
    {
        TargetSpellList targetSpellStore;

        bool const useCreatureSpells = ShouldUseCreatureSpellAutocast(me);
        uint8 const spellCount = useCreatureSpells ? MAX_CREATURE_SPELLS : me->GetPetAutoSpellSize();

        for (uint8 i = 0; i < spellCount; ++i)
        {
            uint32 spellID = useCreatureSpells ? me->m_spells[i] : me->GetPetAutoSpellOnPos(i);
            if (!spellID)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
            if (!spellInfo || spellInfo->IsPassive() || (useCreatureSpells && !spellInfo->IsAutocastable()))
                continue;

            if (me->GetSpellHistory()->HasGlobalCooldown(spellInfo))
                continue;

            // check spell cooldown
            if (!me->GetSpellHistory()->IsReady(spellInfo))
                continue;

            if (spellInfo->IsPositive())
            {
                if (spellInfo->CanBeUsedInCombat())
                {
                    // Check if we're in combat or commanded to attack
                    if (!useCreatureSpells && !me->IsInCombat() && !me->GetCharmInfo()->IsCommandAttack())
                        continue;
                }

                Spell* spell = new Spell(me, spellInfo, useCreatureSpells ? TRIGGERED_IGNORE_POWER_AND_REAGENT_COST_NO_TRIGGER : TRIGGERED_NONE);
                bool spellUsed = false;

                // Some spells can target enemy or friendly (DK Ghoul's Leap)
                // Check for enemy first (pet then owner)
                Unit* target = me->getAttackerForHelper();
                if (!target && owner)
                    target = owner->getAttackerForHelper();

                if (target)
                {
                    if (CanAttack(target) && spell->CanAutoCast(target))
                    {
                        targetSpellStore.push_back(std::make_pair(target, spell));
                        spellUsed = true;
                    }
                }

                if (spellInfo->HasEffect(SPELL_EFFECT_JUMP_DEST))
                {
                    if (!spellUsed)
                        delete spell;
                    continue; // Pets must only jump to target
                }

                // No enemy, check friendly
                if (!spellUsed)
                {
                    for (ObjectGuid target : _allySet)
                    {
                        Unit* ally = ObjectAccessor::GetUnit(*me, target);

                        //only buff targets that are in combat, unless the spell can only be cast while out of combat
                        if (!ally)
                            continue;

                        if (useCreatureSpells && IsHealingSpell(spellInfo) && ally->IsFullHealth())
                            continue;

                        if (spell->CanAutoCast(ally))
                        {
                            targetSpellStore.push_back(std::make_pair(ally, spell));
                            spellUsed = true;
                            break;
                        }
                    }
                }

                // No valid targets at all
                if (!spellUsed)
                    delete spell;
            }
            else if (me->GetVictim() && CanAttack(me->GetVictim()) && spellInfo->CanBeUsedInCombat())
            {
                Spell* spell = new Spell(me, spellInfo, useCreatureSpells ? TRIGGERED_IGNORE_POWER_AND_REAGENT_COST_NO_TRIGGER : TRIGGERED_NONE);
                if (spell->CanAutoCast(me->GetVictim()))
                    targetSpellStore.push_back(std::make_pair(me->GetVictim(), spell));
                else
                    delete spell;
            }
        }

        // found units to cast on to
        if (!targetSpellStore.empty())
        {
            TargetSpellList::iterator it = targetSpellStore.begin();
            std::advance(it, urand(0, targetSpellStore.size() - 1));

            Spell* spell  = (*it).second;
            Unit*  target = (*it).first;

            targetSpellStore.erase(it);

            SpellCastTargets targets;
            targets.SetUnitTarget(target);

            spell->prepare(targets);
        }

        // deleted cached Spell objects
        for (std::pair<Unit*, Spell*> const& unitspellpair : targetSpellStore)
            delete unitspellpair.second;
    }

    // Update speed as needed to prevent dropping too far behind and despawning
    me->UpdateSpeed(MOVE_RUN);
    me->UpdateSpeed(MOVE_WALK);
    me->UpdateSpeed(MOVE_FLIGHT);

}

void PetAI::KilledUnit(Unit* victim)
{
    // Called from Unit::Kill() in case where pet or owner kills something
    // if owner killed this victim, pet may still be attacking something else
    if (me->GetVictim() && me->GetVictim() != victim)
        return;

    // Clear target just in case. May help problem where health / focus / mana
    // regen gets stuck. Also resets attack command.
    // Can't use StopAttack() because that activates movement handlers and ignores
    // next target selection
    me->AttackStop();
    me->InterruptNonMeleeSpells(false);

    // A kill is an ordinary end to a fight, not a target snatched away, so the
    // trace should not go on to report it as one.
    _diagnosticVictim.Clear();

    // Before returning to owner, see if there are more things to attack
    if (Unit* nextTarget = SelectNextTarget(false))
        AttackStart(nextTarget);
    else
        HandleReturnMovement("its target is dead and there is nothing else nearby to pick up"); // Return
}

void PetAI::AttackStart(Unit* target)
{
    // Overrides Unit::AttackStart to prevent pet from switching off its assigned target
    if (!target || target == me)
        return;

    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
        return;

    _AttackStart(target);
}

void PetAI::_AttackStart(Unit* target)
{
    // Check all pet states to decide if we can attack this target
    char const* refusalReason = nullptr;
    if (!CanAttack(target, &refusalReason))
    {
        // DamageTaken routes every single hit through here, so a refusal is
        // only worth reporting when the target or the reason changes -
        // otherwise a pet being beaten on would repeat one line per swing.
        ObjectGuid const targetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
        if (_diagnosticRefusedTarget != targetGuid || _diagnosticRefusalReason != refusalReason)
        {
            _diagnosticRefusedTarget = targetGuid;
            _diagnosticRefusalReason = refusalReason;

            if (Player* listener = PetDiagListener(me))
                ChatHandler(listener->GetSession()).PSendSysMessage("[PetDiag] Will not engage %s: %s.",
                    target ? target->GetName().c_str() : "<nothing>", refusalReason ? refusalReason : "no reason recorded");
        }
        return;
    }

    _diagnosticRefusedTarget.Clear();
    _diagnosticRefusalReason = nullptr;

    if (target->HasBreakableByDamageCrowdControlAura(me) && me->GetCharmInfo()->IsCommandAttack())
        _lastCrowdControlledVictim = target->GetGUID();

    // Only chase if not commanded to stay or if stay but commanded to attack
    DoAttack(target, (!me->GetCharmInfo()->HasCommandState(COMMAND_STAY) || me->GetCharmInfo()->IsCommandAttack()));
}

void PetAI::OwnerAttackedBy(Unit* attacker)
{
    // Called when owner takes damage. This function helps keep pets from running off
    //  simply due to owner gaining aggro.

    if (!attacker || !me->IsAlive())
        return;

    // Passive pets don't do anything
    if (me->HasReactState(REACT_PASSIVE))
        return;

    // Prevent pet from disengaging from current target
    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
        return;

    // Continue to evaluate and attack if necessary
    AttackStart(attacker);
}

void PetAI::OwnerAttacked(Unit* target)
{
    // Called when owner attacks something. Allows defensive pets to know
    //  that they need to assist

    // Target might be NULL if called from spell with invalid cast targets
    if (!target || !me->IsAlive())
        return;

    // Passive pets don't do anything
    if (me->HasReactState(REACT_PASSIVE))
        return;

    // Prevent pet from disengaging from current target
    if (me->GetVictim() && me->EnsureVictim()->IsAlive())
        return;

    // Continue to evaluate and attack if necessary
    AttackStart(target);
}

Unit* PetAI::SelectNextTarget(bool allowAutoSelect) const
{
    // Provides next target selection after current target death.
    // This function should only be called internally by the AI
    // Targets are not evaluated here for being valid targets, that is done in _CanAttack()
    // The parameter: allowAutoSelect lets us disable aggressive pet auto targeting for certain situations

    // Passive pets don't do next target selection
    if (me->HasReactState(REACT_PASSIVE))
        return nullptr;

    // Check pet attackers first so we don't drag a bunch of targets to the owner
    if (Unit* myAttacker = me->getAttackerForHelper())
        if (!myAttacker->HasBreakableByDamageCrowdControlAura())
            return myAttacker;

    // Not sure why we wouldn't have an owner but just in case...
    if (!me->GetCharmerOrOwner())
        return nullptr;

    // Check owner attackers
    if (Unit* ownerAttacker = me->GetCharmerOrOwner()->getAttackerForHelper())
        if (!ownerAttacker->HasBreakableByDamageCrowdControlAura())
            return ownerAttacker;

    // Check owner victim
    // 3.0.2 - Pets now start attacking their owners victim in defensive mode as soon as the hunter does
    if (Unit* ownerVictim = me->GetCharmerOrOwner()->GetVictim())
            return ownerVictim;

    // Neither pet or owner had a target and aggressive pets can pick any target
    // To prevent aggressive pets from chain selecting targets and running off, we
    //  only select a random target if certain conditions are met.
    if (me->HasReactState(REACT_AGGRESSIVE) && allowAutoSelect)
    {
        if (!me->GetCharmInfo()->IsReturning() || me->GetCharmInfo()->IsFollowing() || me->GetCharmInfo()->IsAtStay())
            if (Unit* nearTarget = me->SelectNearestHostileUnitInAggroRange(true, true))
                return nearTarget;
    }

    // Default - no valid targets
    return nullptr;
}

void PetAI::HandleReturnMovement(char const* diagnosticReason)
{
    // Handles moving the pet back to stay or owner

    // Prevent activating movement when under control of spells
    // such as "Eyes of the Beast"
    if (me->IsCharmed())
        return;

    if (!me->GetCharmInfo())
    {
        TC_LOG_WARN("scripts.ai.petai", "me->GetCharmInfo() is NULL in PetAI::HandleReturnMovement(). Debug info: {}", GetDebugInfo());
        return;
    }

    if (me->GetCharmInfo()->HasCommandState(COMMAND_STAY))
    {
        if (!me->GetCharmInfo()->IsAtStay() && !me->GetCharmInfo()->IsReturning())
        {
            // Return to previous position where stay was clicked
            float x, y, z;

            me->GetCharmInfo()->GetStayPosition(x, y, z);

            // Inside the guard on purpose: the guard is what makes this a
            // transition. HandleReturnMovement itself is called on every tick
            // the pet has no target.
            if (Player* listener = PetDiagListener(me))
                ChatHandler(listener->GetSession()).PSendSysMessage(
                    "[PetDiag] Walking back to its stay spot because %s.", diagnosticReason);

            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsReturning(true);

            if (me->HasUnitState(UNIT_STATE_CHASE))
                me->GetMotionMaster()->Remove(CHASE_MOTION_TYPE);

            me->GetMotionMaster()->MovePoint(me->GetGUID().GetCounter(), x, y, z, true);
        }
    }
    else // COMMAND_FOLLOW
    {
        if (!me->GetCharmInfo()->IsFollowing() && !me->GetCharmInfo()->IsReturning())
        {
            if (Player* listener = PetDiagListener(me))
                ChatHandler(listener->GetSession()).PSendSysMessage(
                    "[PetDiag] Running back to you because %s.", diagnosticReason);

            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsReturning(true);
            me->GetCharmInfo()->SetIsCommandAttack(false);
            me->GetCharmInfo()->SetIsCommandFollow(true);
            me->GetCharmInfo()->SetIsAtStay(false);
            me->GetCharmInfo()->SetIsFollowing(false);

            if (me->HasUnitState(UNIT_STATE_CHASE))
                me->GetMotionMaster()->Remove(CHASE_MOTION_TYPE);

            me->GetMotionMaster()->MoveFollow(me->GetCharmerOrOwner(), PET_FOLLOW_DIST, me->GetFollowAngle());
        }
    }
    me->RemoveUnitFlag(UNIT_FLAG_PET_IN_COMBAT); // on player pets, this flag indicates that we're actively going after a target - we're returning, so remove it
}

void PetAI::DoAttack(Unit* target, bool chase)
{
    // Handles attack with or without chase and also resets flags
    // for next update / creature kill

    if (me->Attack(target, true))
    {
        me->SetUnitFlag(UNIT_FLAG_PET_IN_COMBAT); // on player pets, this flag indicates we're actively going after a target - that's what we're doing, so set it
        // Play sound to let the player know the pet is attacking something it picked on its own
        if (me->HasReactState(REACT_AGGRESSIVE) && !me->GetCharmInfo()->IsCommandAttack())
            me->SendPetAIReaction(me->GetGUID());

        if (chase)
        {
            bool oldCmdAttack = me->GetCharmInfo()->IsCommandAttack(); // This needs to be reset after other flags are cleared
            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsCommandAttack(oldCmdAttack); // For passive pets commanded to attack so they will use spells

            if (me->HasUnitState(UNIT_STATE_FOLLOW))
                me->GetMotionMaster()->Remove(FOLLOW_MOTION_TYPE);

            // Pets with ranged attacks should not care about the chase angle at all.
            float chaseDistance = me->GetPetChaseDistance();
            float angle = chaseDistance == 0.f ? float(M_PI) : 0.f;
            float tolerance = chaseDistance == 0.f ? float(M_PI_4) : float(M_PI * 2);
            me->GetMotionMaster()->MoveChase(target, ChaseRange(0.f, chaseDistance));
        }
        else // (Stay && ((Aggressive || Defensive) && In Melee Range)))
        {
            ClearCharmInfoFlags();
            me->GetCharmInfo()->SetIsAtStay(true);

            if (me->HasUnitState(UNIT_STATE_FOLLOW))
                me->GetMotionMaster()->Remove(FOLLOW_MOTION_TYPE);

            me->GetMotionMaster()->MoveIdle();
        }
    }
}

void PetAI::MovementInform(uint32 type, uint32 id)
{
    // Receives notification when pet reaches stay or follow owner
    switch (type)
    {
        case POINT_MOTION_TYPE:
        {
            // Pet is returning to where stay was clicked. data should be
            // pet's GUIDLow since we set that as the waypoint ID
            if (id == me->GetGUID().GetCounter() && me->GetCharmInfo()->IsReturning())
            {
                if (Player* listener = PetDiagListener(me))
                    ChatHandler(listener->GetSession()).SendSysMessage("[PetDiag] Reached its stay spot; it is idle and will pick up targets in melee reach only.");

                ClearCharmInfoFlags();
                me->GetCharmInfo()->SetIsAtStay(true);
                me->GetMotionMaster()->MoveIdle();
            }
            break;
        }
        case FOLLOW_MOTION_TYPE:
        {
            // If data is owner's GUIDLow then we've reached follow point,
            // otherwise we're probably chasing a creature
            if (me->GetCharmerOrOwner() && me->GetCharmInfo() && id == me->GetCharmerOrOwner()->GetGUID().GetCounter() && me->GetCharmInfo()->IsReturning())
            {
                // Worth watching for: until this arrives the pet still counts as
                // "returning", and a returning pet told to follow refuses every
                // target. A return that never reports arrival is a pet that has
                // gone deaf, not a pet that is being fussy.
                if (Player* listener = PetDiagListener(me))
                    ChatHandler(listener->GetSession()).SendSysMessage("[PetDiag] Got back to you; it is following again and free to take targets.");

                ClearCharmInfoFlags();
                me->GetCharmInfo()->SetIsFollowing(true);
            }
            break;
        }
        default:
            break;
    }
}

bool PetAI::CanAttack(Unit* target, char const** refusalReason)
{
    // Evaluates wether a pet can attack a specific target based on CommandState, ReactState and other flags
    // IMPORTANT: The order in which things are checked is important, be careful if you add or remove checks

    // Written straight into the out-parameter at each refusal rather than
    // reconstructed afterwards by a parallel helper - a second copy of this
    // ladder would start lying the first time somebody reordered this one.
    auto refuse = [refusalReason](char const* why)
    {
        if (refusalReason)
            *refusalReason = why;
        return false;
    };

    // Hmmm...
    if (!target)
        return refuse("there is no target");

    if (!target->IsAlive())
    {
        // if target is invalid, pet should evade automaticly
        // Clear target to prevent getting stuck on dead targets
        //me->AttackStop();
        //me->InterruptNonMeleeSpells(false);
        return refuse("the target is dead");
    }

    if (!me->GetCharmInfo())
    {
        TC_LOG_WARN("scripts.ai.petai", "me->GetCharmInfo() is NULL in PetAI::CanAttack(). Debug info: {}", GetDebugInfo());
        return refuse("the pet has no charm info");
    }

    // Passive - passive pets can attack if told to
    if (me->HasReactState(REACT_PASSIVE))
    {
        if (!me->GetCharmInfo()->IsCommandAttack())
            return refuse("it is passive and was not told to attack");
        return true;
    }

    // CC - mobs under crowd control can be attacked if owner commanded
    if (target->HasBreakableByDamageCrowdControlAura())
    {
        if (!me->GetCharmInfo()->IsCommandAttack())
            return refuse("the target has a damage-breakable control aura and you did not order the attack");
        return true;
    }

    // Returning - pets ignore attacks only if owner clicked follow
    if (me->GetCharmInfo()->IsReturning())
    {
        if (me->GetCharmInfo()->IsCommandFollow())
            return refuse("it is on its way back to you and you told it to follow");
        return true;
    }

    // Stay - can attack if target is within range or commanded to
    if (me->GetCharmInfo()->HasCommandState(COMMAND_STAY))
    {
        if (!me->IsWithinMeleeRange(target) && !me->GetCharmInfo()->IsCommandAttack())
            return refuse("it was told to stay and the target is out of melee reach of its stay spot");
        return true;
    }

    //  Pets attacking something (or chasing) should only switch targets if owner tells them to
    if (me->GetVictim() && me->GetVictim() != target)
    {
        // Check if our owner selected this target and clicked "attack"
        Unit* ownerTarget = nullptr;
        if (Player* owner = me->GetCharmerOrOwner()->ToPlayer())
            ownerTarget = owner->GetSelectedUnit();
        else
            ownerTarget = me->GetCharmerOrOwner()->GetVictim();

        if (ownerTarget && me->GetCharmInfo()->IsCommandAttack())
        {
            if (target->GetGUID() != ownerTarget->GetGUID())
                return refuse("it is already on the target you ordered, and this is a different one");
            return true;
        }
    }

    // Follow
    if (me->GetCharmInfo()->HasCommandState(COMMAND_FOLLOW))
    {
        if (me->GetCharmInfo()->IsReturning())
            return refuse("it is following you and still on its way back");
        return true;
    }

    // default, though we shouldn't ever get here
    return refuse("no rule allowed the attack (command state fell through)");
}

void PetAI::ReceiveEmote(Player* player, uint32 emote)
{
    if (me->GetOwnerGUID() != player->GetGUID())
        return;

    switch (emote)
    {
        case TEXT_EMOTE_COWER:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(/*EMOTE_ONESHOT_ROAR*/EMOTE_ONESHOT_OMNICAST_GHOUL);
            break;
        case TEXT_EMOTE_ANGRY:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(/*EMOTE_ONESHOT_COWER*/EMOTE_STATE_STUN);
            break;
        case TEXT_EMOTE_GLARE:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(EMOTE_STATE_STUN);
            break;
        case TEXT_EMOTE_SOOTHE:
            if (me->IsPet() && me->ToPet()->IsPetGhoul())
                me->HandleEmoteCommand(EMOTE_ONESHOT_OMNICAST_GHOUL);
            break;
    }
}

bool PetAI::NeedToStop(char const** reason)
{
    // This is needed for charmed creatures, as once their target was reset other effects can trigger threat
    if (me->IsCharmed() && me->GetVictim() == me->GetCharmer())
    {
        if (reason)
            *reason = "it is charmed and its target is whoever is charming it";
        return true;
    }

    // dont allow pets to follow targets far away from owner
    if (Unit* owner = me->GetCharmerOrOwner())
        if (owner->GetExactDist(me) >= (owner->GetVisibilityRange() - 10.0f))
        {
            if (reason)
                *reason = "it hit the leash - it got further from you than the visibility range allows";
            return true;
        }

    // Re-asked every single tick, which the owner's own attack is not. Anything
    // that flips here mid-fight sends the pet home while its owner carries on
    // swinging, and that asymmetry is what most "why did my pet run back?"
    // reports turn out to be.
    if (!me->IsValidAttackTarget(me->GetVictim()))
    {
        if (reason)
            *reason = DescribeInvalidAttackTarget(me, me->GetVictim());
        return true;
    }

    return false;
}

void PetAI::StopAttack(char const* diagnosticReason)
{
    if (!me->IsAlive())
    {
        me->GetMotionMaster()->Clear();
        me->GetMotionMaster()->MoveIdle();
        me->CombatStop();
        return;
    }

    // Claimed here so ReportDiagnosticVictimChange does not also announce the
    // target loss next tick as if something outside the AI had caused it.
    _diagnosticVictim.Clear();

    me->AttackStop();
    me->InterruptNonMeleeSpells(false);
    me->GetCharmInfo()->SetIsCommandAttack(false);
    ClearCharmInfoFlags();
    HandleReturnMovement(diagnosticReason ? diagnosticReason : "the pet AI stopped the attack");
}

void PetAI::UpdateAllies()
{
    _updateAlliesTimer = 10 * IN_MILLISECONDS; // update friendly targets every 10 seconds, lesser checks increase performance

    Unit* owner = me->GetCharmerOrOwner();
    if (!owner)
        return;

    Group* group = nullptr;
    if (Player* player = owner->ToPlayer())
        group = player->GetGroup();

    // only pet and owner/not in group->ok
    if (_allySet.size() == 2 && !group)
        return;

    // owner is in group; group members filled in already (no raid -> subgroupcount = whole count)
    if (group && !group->isRaidGroup() && _allySet.size() == (group->GetMembersCount() + 2))
        return;

    _allySet.clear();
    _allySet.insert(me->GetGUID());
    if (group) // add group
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* Target = itr->GetSource();
            if (!Target || !Target->IsInMap(owner) || !group->SameSubGroup(owner->ToPlayer(), Target))
                continue;

            if (Target->GetGUID() == owner->GetGUID())
                continue;

            _allySet.insert(Target->GetGUID());
        }
    }
    else // remove group
        _allySet.insert(owner->GetGUID());
}

void PetAI::OnCharmed(bool isNew)
{
    if (!me->isPossessedByPlayer() && me->IsCharmed())
        me->GetMotionMaster()->MoveFollow(me->GetCharmer(), PET_FOLLOW_DIST, me->GetFollowAngle());

    CreatureAI::OnCharmed(isNew);
}

void PetAI::ClearCharmInfoFlags()
{
    CharmInfo* ci = me->GetCharmInfo();
    if (ci)
    {
        ci->SetIsAtStay(false);
        ci->SetIsCommandAttack(false);
        ci->SetIsCommandFollow(false);
        ci->SetIsFollowing(false);
        ci->SetIsReturning(false);
    }
}

void PetAI::ReportDiagnosticVictimChange()
{
    ReportDiagnosticMovementChange();

    // Runs before anything else in UpdateAI and costs one GUID compare on the
    // overwhelmingly common tick where the target has not changed.
    Unit* victim = me->GetVictim();
    ObjectGuid const victimGuid = victim ? victim->GetGUID() : ObjectGuid::Empty;
    if (victimGuid == _diagnosticVictim)
        return;

    ObjectGuid const previousGuid = _diagnosticVictim;
    _diagnosticVictim = victimGuid;

    Player* listener = PetDiagListener(me);
    if (!listener)
        return;

    ChatHandler handler(listener->GetSession());

    if (victim)
    {
        CharmInfo* charmInfo = me->GetCharmInfo();
        handler.PSendSysMessage("[PetDiag] Engaging %s, %.1f yd away (%s).", victim->GetName().c_str(), me->GetExactDist(victim),
            charmInfo && charmInfo->IsCommandAttack() ? "you ordered it" : "it chose this itself");
        return;
    }

    if (previousGuid.IsEmpty())
        return;

    // StopAttack and KilledUnit both clear _diagnosticVictim themselves, so
    // getting here means the target was taken away by something OUTSIDE the pet
    // AI - the mob evaded or reset, combat was dropped, a charm or a script
    // called AttackStop. From the owner's chair that looks exactly like the pet
    // deciding to leave, which is why it gets its own line.
    Unit* previous = ObjectAccessor::GetUnit(*me, previousGuid);
    if (!previous)
    {
        handler.SendSysMessage("[PetDiag] Its target is no longer visible to the pet (died, despawned, phased or went out of range).");
        return;
    }

    handler.PSendSysMessage("[PetDiag] Its target %s was cleared by something outside the pet AI - alive=%u, in combat=%u, %.1f yd away. Likely an evade, a reset, or combat being dropped.",
        previous->GetName().c_str(), uint32(previous->IsAlive()), uint32(previous->IsInCombat()), me->GetExactDist(previous));
}

void PetAI::ReportDiagnosticMovementChange()
{
    // The blind spot every other line here shares: a pet can be walked home
    // without PetAI deciding anything at all, if something else puts a follow on
    // top of its motion stack while the fight is still on. That looks identical
    // from the owner's chair and leaves no trace anywhere else, so the motion
    // type is watched directly. One enum compare per tick.
    MovementGeneratorType const motionType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (motionType == _diagnosticMotionType)
        return;

    MovementGeneratorType const previousType = _diagnosticMotionType;
    _diagnosticMotionType = motionType;

    // Only worth a line while there is a fight to be pulled out of. Out of
    // combat the pet is supposed to be following, and saying so every time it
    // idles and re-follows is noise.
    if (!me->GetVictim() || motionType != FOLLOW_MOTION_TYPE || previousType != CHASE_MOTION_TYPE)
        return;

    if (Player* listener = PetDiagListener(me))
        ChatHandler(listener->GetSession()).SendSysMessage(
            "[PetDiag] It swapped from chasing to following you while it still has a target - something outside the pet AI moved it, the pet AI did not give up.");
}
