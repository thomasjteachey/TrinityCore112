/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Four Horsemen - Mograine, Zeliek, Blaumeux and Korth'azz.
 * Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0);
 * Naxxramas 40 scripts by Sogladev.
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

#include "ScriptMgr.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "VanillaRaids/VanillaRaids.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"

enum FourHorsemenSpells40
{
    SPELL_BERSERK                    = 26662,
    // Marks
    SPELL_MARK_OF_KORTHAZZ           = 28832,
    SPELL_MARK_OF_BLAUMEUX           = 28833,
    SPELL_MARK_OF_MOGRAINE           = 28834,
    SPELL_MARK_OF_ZELIEK             = 28835,
    // Korth'azz
    SPELL_KORTHAZZ_METEOR            = 28884,
    // Blaumeux
    SPELL_BLAUMEUX_VOID_ZONE         = 28863,
    // Zeliek
    SPELL_ZELIEK_HOLY_WRATH          = 28883,
    // Mograine
    SPELL_MOGRAINE_UNHOLY_SHADOW     = 28882,

    // All four shield wall at 50% and again at 20%, for 20 seconds.
    SPELL_SHIELDWALL                 = 29061,
    SPELL_SUMMON_PLAYER              = 25104
};

enum FourHorsemenEvents40
{
    EVENT_MARK_CAST                  = 1,
    EVENT_PRIMARY_SPELL              = 2,
    EVENT_BERSERK                    = 4,
    EVENT_HEALTH_CHECK               = 5
};

enum FourHorsemenIds40
{
    HORSEMAN_ZELIEK                  = 0,
    HORSEMAN_BLAUMEUX                = 1,
    HORSEMAN_MOGRAINE                = 2,
    HORSEMAN_KORTHAZZ                = 3
};

enum FourHorsemenSpirits40
{
    SPELL_SUMMON_SPIRIT_ZELIEK       = 28934,
    SPELL_SUMMON_SPIRIT_BLAUMEUX     = 28931,
    SPELL_SUMMON_SPIRIT_MOGRAINE     = 28928,
    SPELL_SUMMON_SPIRIT_KORTHAZZ     = 28932,

    NPC_SPIRIT_ZELIEK                = 16777,
    NPC_SPIRIT_BLAUMEUX              = 16776,
    NPC_SPIRIT_MOGRAINE              = 16775,
    NPC_SPIRIT_KORTHAZZ              = 16778
};

enum FourHorsemenSays40
{
    SAY_AGGRO                        = 0,
    SAY_TAUNT                        = 1,
    SAY_SPECIAL                      = 2,
    SAY_SLAY                         = 3,
    SAY_DEATH                        = 4
};

uint32 const TABLE_SPELL_MARK_40[4] =
{
    SPELL_MARK_OF_ZELIEK, SPELL_MARK_OF_BLAUMEUX, SPELL_MARK_OF_MOGRAINE, SPELL_MARK_OF_KORTHAZZ
};

uint32 const TABLE_SPELL_SUMMON_SPIRIT_40[4] =
{
    SPELL_SUMMON_SPIRIT_ZELIEK, SPELL_SUMMON_SPIRIT_BLAUMEUX, SPELL_SUMMON_SPIRIT_MOGRAINE, SPELL_SUMMON_SPIRIT_KORTHAZZ
};

struct boss_four_horsemen_40 : public VanillaRaidsBossAI
{
    boss_four_horsemen_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_HORSEMEN)
    {
        switch (me->GetEntry())
        {
            case NPC_SIR_ZELIEK_40:
                _horsemanId = HORSEMAN_ZELIEK;
                break;
            case NPC_LADY_BLAUMEUX_40:
                _horsemanId = HORSEMAN_BLAUMEUX;
                break;
            case NPC_HIGHLORD_MOGRAINE_40:
                _horsemanId = HORSEMAN_MOGRAINE;
                break;
            case NPC_THANE_KORTHAZZ_40:
                _horsemanId = HORSEMAN_KORTHAZZ;
                break;
            default:
                _horsemanId = HORSEMAN_MOGRAINE;
                break;
        }
    }

    bool IsInRoom()
    {
        if (me->GetExactDist(2535.1f, -2968.7f, 241.3f) > 100.0f)
        {
            EnterEvadeMode(EVADE_REASON_BOUNDARY);
            return false;
        }

        return true;
    }

    void Reset() override
    {
        BossAI::Reset();
        me->NearTeleportTo(me->GetHomePosition());
        events.Reset();
        events.RescheduleEvent(EVENT_MARK_CAST, 20s);
        events.RescheduleEvent(EVENT_BERSERK, 600s);
        summons.DespawnAll(); // the spirits
        events.RescheduleEvent(EVENT_PRIMARY_SPELL, 10s, 15s);
        _doneFirstShieldWall = false;
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        Talk(SAY_SLAY);
    }

    void SpellHitTarget(WorldObject* target, SpellInfo const* spellInfo) override
    {
        if (spellInfo && spellInfo->Id == TABLE_SPELL_MARK_40[_horsemanId])
            if (Unit* unitTarget = target->ToUnit())
                ModifyThreatByPercent(unitTarget, -50);
    }

    void JustDied(Unit* killer) override
    {
        BossAI::JustDied(killer);
        Talk(SAY_DEATH);

        // The last one down clears the spirits and drops the chest; the first
        // three leave a spirit behind instead.
        if (instance && instance->GetBossState(BOSS_HORSEMEN) == DONE)
        {
            Map::PlayerList const& players = me->GetMap()->GetPlayers();
            if (players.isEmpty())
                return;

            for (uint32 spiritEntry : { NPC_SPIRIT_ZELIEK, NPC_SPIRIT_BLAUMEUX, NPC_SPIRIT_MOGRAINE, NPC_SPIRIT_KORTHAZZ })
                if (Creature* spirit = GetClosestCreatureWithEntry(me, spiritEntry, 200.0f))
                    spirit->DespawnOrUnsummon();

            if (Player* player = players.begin()->GetSource())
                if (GameObject* chest = player->SummonGameObject(GO_HORSEMEN_CHEST_40,
                        Position(2514.8f, -2944.9f, 245.55f, 5.51f), QuaternionData(), 0s))
                    chest->SetLootRecipient(me);
        }
        else
            DoCastSelf(TABLE_SPELL_SUMMON_SPIRIT_40[_horsemanId], CastSpellExtraArgs(true));
    }

    void JustSummoned(Creature* summon) override
    {
        summons.Summon(summon);
        summons.DoZoneInCombat();
        // Same bit AzerothCore calls UNIT_FLAG_DISABLE_MOVE: the spirits stand still.
        summon->SetUnitFlag(UNIT_FLAG_REMOVE_CLIENT_CONTROL);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_AGGRO);
        events.ScheduleEvent(EVENT_HEALTH_CHECK, 1s);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!IsInRoom())
            return;

        if (!UpdateVictim())
            return;

        if (Unit* victim = me->GetVictim())
            if (!me->IsWithinDistInMap(victim, VISIBILITY_DISTANCE_NORMAL))
                me->CastSpell(victim, SPELL_SUMMON_PLAYER, CastSpellExtraArgs(true));

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_MARK_CAST:
                DoCastSelf(TABLE_SPELL_MARK_40[_horsemanId]);
                events.Repeat(12s);
                return;
            case EVENT_BERSERK:
                Talk(SAY_SPECIAL);
                DoCastSelf(SPELL_BERSERK, CastSpellExtraArgs(true));
                return;
            case EVENT_PRIMARY_SPELL:
                switch (_horsemanId)
                {
                    case HORSEMAN_ZELIEK:
                        me->CastSpell(me->GetVictim(), SPELL_ZELIEK_HOLY_WRATH, CastSpellExtraArgs(TRIGGERED_NONE)
                            .AddSpellBP0(VanillaRaids::NerfFourHorsemen() ? 222 : 443)
                            .AddSpellMod(SPELLVALUE_MAX_TARGETS, 50)); // 30 yards
                        break;
                    case HORSEMAN_BLAUMEUX:
                        if (!VanillaRaids::NerfFourHorsemen())
                            me->CastSpell(me->GetVictim(), SPELL_BLAUMEUX_VOID_ZONE, CastSpellExtraArgs(TRIGGERED_NONE));
                        break;
                    case HORSEMAN_MOGRAINE:
                        // Same spell data as vanilla - shadow damage, not fire.
                        me->CastSpell(me->GetVictim(), SPELL_MOGRAINE_UNHOLY_SHADOW, CastSpellExtraArgs(TRIGGERED_NONE));
                        break;
                    case HORSEMAN_KORTHAZZ:
                    default:
                        me->CastSpell(me->GetVictim(), SPELL_KORTHAZZ_METEOR,
                            CastSpellExtraArgs(TRIGGERED_NONE).AddSpellBP0(12824));
                        break;
                }
                events.Repeat(15s);
                return;
            case EVENT_HEALTH_CHECK:
                if (!_doneFirstShieldWall && me->GetHealthPct() <= 50.0f)
                {
                    DoCastSelf(SPELL_SHIELDWALL, CastSpellExtraArgs(true));
                    _doneFirstShieldWall = true;
                    events.Repeat(1s);
                    break;
                }

                if (_doneFirstShieldWall && me->GetHealthPct() <= 20.0f)
                {
                    if (!me->HasAura(SPELL_SHIELDWALL)) // do not refresh the first one
                        DoCastSelf(SPELL_SHIELDWALL, CastSpellExtraArgs(true));
                    break;
                }

                events.Repeat(1s);
                return;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    uint8 _horsemanId = HORSEMAN_MOGRAINE;
    bool _doneFirstShieldWall = false;
};

void AddSC_boss_four_horsemen_40()
{
    RegisterNaxxramasCreatureAI(boss_four_horsemen_40);
}
