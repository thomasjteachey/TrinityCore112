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

#ifndef TRINITY_PETAI_H
#define TRINITY_PETAI_H

#include "CreatureAI.h"
#include "MovementDefines.h"
#include "ObjectGuid.h"
#include "Timer.h"
#include "Spell.h"

class Creature;
class Spell;

typedef std::vector<std::pair<Unit*, Spell*>> TargetSpellList;

class TC_GAME_API PetAI : public CreatureAI
{
    public:
        static int32 Permissible(Creature const* creature);

        explicit PetAI(Creature* creature);

        void UpdateAI(uint32) override;
        void KilledUnit(Unit* /*victim*/) override;
        // only start attacking if not attacking something else already
        void AttackStart(Unit* target) override;
        // always start attacking if possible
        void _AttackStart(Unit* target);
        void MovementInform(uint32 type, uint32 id) override;
        void OwnerAttackedBy(Unit* attacker) override;
        void OwnerAttacked(Unit* target) override;
        void DamageTaken(Unit* attacker, uint32& /*damage*/, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo = nullptr*/) override { AttackStart(attacker); }
        void ReceiveEmote(Player* player, uint32 textEmote) override;
        void JustEnteredCombat(Unit* who) override { EngagementStart(who); }
        void JustExitedCombat() override { EngagementOver(); }
        void OnCharmed(bool isNew) override;
        void QueueSpell(uint32 spellId, SpellCastTargets const& targets);

        // The following aren't used by the PetAI but need to be defined to override
        // default CreatureAI functions which interfere with the PetAI

        void MoveInLineOfSight(Unit* /*who*/) override { } // CreatureAI interferes with returning pets
        void MoveInLineOfSight_Safe(Unit* /*who*/) { } // CreatureAI interferes with returning pets
        void JustAppeared() override { } // we will control following manually
        void EnterEvadeMode(EvadeReason /*why*/) override { } // For fleeing, pets don't use this type of Evade mechanic
        // The reason is only ever read by the ".gm diagnostics pet" trace, which
        // is why it has a default: outside callers have nothing useful to say.
        void HandleReturnMovement(char const* diagnosticReason = "an outside caller asked for it");

    private:
        void ClearQueuedSpell();
        void ProcessSpellQueue();
        // Out-parameter rather than a separate "why did it say yes/no" helper,
        // so the explanation can never drift from the decision that produced it.
        // Both are only written when the caller asks for them.
        bool NeedToStop(char const** reason = nullptr);
        void StopAttack(char const* diagnosticReason);
        void UpdateAllies();
        Unit* SelectNextTarget(bool allowAutoSelect) const;
        void DoAttack(Unit* target, bool chase);
        bool CanAttack(Unit* target, char const** refusalReason = nullptr);
        // Quick access to set all flags to FALSE
        void ClearCharmInfoFlags();

        // ".gm diagnostics pet" bookkeeping. Every line the trace emits is a
        // state change, not a per-tick sample, so it needs to remember what it
        // last said. See the notes at the top of PetAI.cpp.
        void ReportDiagnosticVictimChange();
        void ReportDiagnosticMovementChange();

        TimeTracker _tracker;
        GuidSet _allySet;
        uint32 _updateAlliesTimer;
        ObjectGuid _lastCrowdControlledVictim;
        ObjectGuid _queuedSpellTarget;
        SpellCastTargets _queuedSpellTargets;
        uint32 _queuedSpellId;
        ObjectGuid _diagnosticVictim;
        ObjectGuid _diagnosticRefusedTarget;
        // Compared by pointer, not by strcmp: every reason is a literal from
        // PetAI.cpp, so identity is exactly the "same reason as last time" test
        // the refusal throttle wants.
        char const* _diagnosticRefusalReason;
        MovementGeneratorType _diagnosticMotionType;
};

#endif
