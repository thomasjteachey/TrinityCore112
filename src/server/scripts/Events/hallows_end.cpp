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

#include "ScriptMgr.h"
#include "Containers.h"
#include "CreatureAIImpl.h"
#include "GameObject.h"
#include "GameTime.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PassiveAI.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>

enum HallowEndCandysSpells
{
    SPELL_HALLOWS_END_CANDY_ORANGE_GIANT          = 24924, // Effect 1: Apply Aura: Mod Size, Value: 30%
    SPELL_HALLOWS_END_CANDY_SKELETON              = 24925, // Effect 1: Apply Aura: Change Model (Skeleton). Effect 2: Apply Aura: Underwater Breathing
    SPELL_HALLOWS_END_CANDY_PIRATE                = 24926, // Effect 1: Apply Aura: Increase Swim Speed, Value: 50%
    SPELL_HALLOWS_END_CANDY_GHOST                 = 24927, // Effect 1: Apply Aura: Levitate / Hover. Effect 2: Apply Aura: Slow Fall, Effect 3: Apply Aura: Water Walking
    SPELL_HALLOWS_END_CANDY_FEMALE_DEFIAS_PIRATE  = 44742, // Effect 1: Apply Aura: Change Model (Defias Pirate, Female). Effect 2: Increase Swim Speed, Value: 50%
    SPELL_HALLOWS_END_CANDY_MALE_DEFIAS_PIRATE    = 44743  // Effect 1: Apply Aura: Change Model (Defias Pirate, Male).   Effect 2: Increase Swim Speed, Value: 50%
};

std::array<uint32, 4> const CandysSpells =
{
    SPELL_HALLOWS_END_CANDY_ORANGE_GIANT,
    SPELL_HALLOWS_END_CANDY_SKELETON,
    SPELL_HALLOWS_END_CANDY_PIRATE,
    SPELL_HALLOWS_END_CANDY_GHOST
};

// 24930 - Hallow's End Candy
class spell_hallow_end_candy : public SpellScript
{
    PrepareSpellScript(spell_hallow_end_candy);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo(CandysSpells);
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        GetCaster()->CastSpell(GetCaster(), Trinity::Containers::SelectRandomContainerElement(CandysSpells), true);
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_hallow_end_candy::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// 24926 - Hallow's End Candy
class spell_hallow_end_candy_pirate : public AuraScript
{
    PrepareAuraScript(spell_hallow_end_candy_pirate);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo(
        {
            SPELL_HALLOWS_END_CANDY_FEMALE_DEFIAS_PIRATE,
            SPELL_HALLOWS_END_CANDY_MALE_DEFIAS_PIRATE
        });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        uint32 spell = GetTarget()->GetNativeGender() == GENDER_FEMALE ? SPELL_HALLOWS_END_CANDY_FEMALE_DEFIAS_PIRATE : SPELL_HALLOWS_END_CANDY_MALE_DEFIAS_PIRATE;
        GetTarget()->CastSpell(GetTarget(), spell, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        uint32 spell = GetTarget()->GetNativeGender() == GENDER_FEMALE ? SPELL_HALLOWS_END_CANDY_FEMALE_DEFIAS_PIRATE : SPELL_HALLOWS_END_CANDY_MALE_DEFIAS_PIRATE;
        GetTarget()->RemoveAurasDueToSpell(spell);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_hallow_end_candy_pirate::HandleApply, EFFECT_0, SPELL_AURA_MOD_INCREASE_SWIM_SPEED, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_hallow_end_candy_pirate::HandleRemove, EFFECT_0, SPELL_AURA_MOD_INCREASE_SWIM_SPEED, AURA_EFFECT_HANDLE_REAL);
    }
};

enum TrickSpells
{
    SPELL_PIRATE_COSTUME_MALE           = 24708,
    SPELL_PIRATE_COSTUME_FEMALE         = 24709,
    SPELL_NINJA_COSTUME_MALE            = 24710,
    SPELL_NINJA_COSTUME_FEMALE          = 24711,
    SPELL_LEPER_GNOME_COSTUME_MALE      = 24712,
    SPELL_LEPER_GNOME_COSTUME_FEMALE    = 24713,
    SPELL_SKELETON_COSTUME              = 24723,
    SPELL_GHOST_COSTUME_MALE            = 24735,
    SPELL_GHOST_COSTUME_FEMALE          = 24736,
    SPELL_TRICK_BUFF                    = 24753,
};

// 24750 - Trick
class spell_hallow_end_trick : public SpellScript
{
    PrepareSpellScript(spell_hallow_end_trick);

    bool Validate(SpellInfo const* /*spell*/) override
    {
        return ValidateSpellInfo(
        {
            SPELL_PIRATE_COSTUME_MALE,
            SPELL_PIRATE_COSTUME_FEMALE,
            SPELL_NINJA_COSTUME_MALE,
            SPELL_NINJA_COSTUME_FEMALE,
            SPELL_LEPER_GNOME_COSTUME_MALE,
            SPELL_LEPER_GNOME_COSTUME_FEMALE,
            SPELL_SKELETON_COSTUME,
            SPELL_GHOST_COSTUME_MALE,
            SPELL_GHOST_COSTUME_FEMALE,
            SPELL_TRICK_BUFF
        });
    }

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (Player* target = GetHitPlayer())
        {
            uint8 gender = target->GetNativeGender();
            uint32 spellId = SPELL_TRICK_BUFF;
            switch (urand(0, 5))
            {
                case 1:
                    spellId = gender == GENDER_FEMALE ? SPELL_LEPER_GNOME_COSTUME_FEMALE : SPELL_LEPER_GNOME_COSTUME_MALE;
                    break;
                case 2:
                    spellId = gender == GENDER_FEMALE ? SPELL_PIRATE_COSTUME_FEMALE : SPELL_PIRATE_COSTUME_MALE;
                    break;
                case 3:
                    spellId = gender == GENDER_FEMALE ? SPELL_GHOST_COSTUME_FEMALE : SPELL_GHOST_COSTUME_MALE;
                    break;
                case 4:
                    spellId = gender == GENDER_FEMALE ? SPELL_NINJA_COSTUME_FEMALE : SPELL_NINJA_COSTUME_MALE;
                    break;
                case 5:
                    spellId = SPELL_SKELETON_COSTUME;
                    break;
                default:
                    break;
            }

            caster->CastSpell(target, spellId, true);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_hallow_end_trick::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

enum TrickOrTreatSpells
{
    SPELL_TRICK                 = 24714,
    SPELL_TREAT                 = 24715,
    SPELL_TRICKED_OR_TREATED    = 24755,
    SPELL_TRICKY_TREAT_SPEED    = 42919,
    SPELL_TRICKY_TREAT_TRIGGER  = 42965,
    SPELL_UPSET_TUMMY           = 42966
};

// 24751 - Trick or Treat
class spell_hallow_end_trick_or_treat : public SpellScript
{
    PrepareSpellScript(spell_hallow_end_trick_or_treat);

    bool Validate(SpellInfo const* /*spell*/) override
    {
        return ValidateSpellInfo({ SPELL_TRICK, SPELL_TREAT, SPELL_TRICKED_OR_TREATED });
    }

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (Player* target = GetHitPlayer())
        {
            caster->CastSpell(target, roll_chance_i(50) ? SPELL_TRICK : SPELL_TREAT, true);
            caster->CastSpell(target, SPELL_TRICKED_OR_TREATED, true);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_hallow_end_trick_or_treat::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// 44436 - Tricky Treat
class spell_hallow_end_tricky_treat : public SpellScript
{
    PrepareSpellScript(spell_hallow_end_tricky_treat);

    bool Validate(SpellInfo const* /*spell*/) override
    {
        return ValidateSpellInfo(
        {
            SPELL_TRICKY_TREAT_SPEED,
            SPELL_TRICKY_TREAT_TRIGGER,
            SPELL_UPSET_TUMMY
        });
    }

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (caster->HasAura(SPELL_TRICKY_TREAT_TRIGGER) && caster->GetAuraCount(SPELL_TRICKY_TREAT_SPEED) > 3 && roll_chance_i(33))
            caster->CastSpell(caster, SPELL_UPSET_TUMMY, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_hallow_end_tricky_treat::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

enum HallowendData
{
    SPELL_HALLOWED_WAND_PIRATE             = 24717,
    SPELL_HALLOWED_WAND_NINJA              = 24718,
    SPELL_HALLOWED_WAND_LEPER_GNOME        = 24719,
    SPELL_HALLOWED_WAND_RANDOM             = 24720,
    SPELL_HALLOWED_WAND_SKELETON           = 24724,
    SPELL_HALLOWED_WAND_WISP               = 24733,
    SPELL_HALLOWED_WAND_GHOST              = 24737,
    SPELL_HALLOWED_WAND_BAT                = 24741
};

// 24717, 24718, 24719, 24720, 24724, 24733, 24737, 24741
class spell_hallow_end_wand : public SpellScript
{
    PrepareSpellScript(spell_hallow_end_wand);

    bool Validate(SpellInfo const* /*spellEntry*/) override
    {
        return ValidateSpellInfo(
        {
            SPELL_PIRATE_COSTUME_MALE,
            SPELL_PIRATE_COSTUME_FEMALE,
            SPELL_NINJA_COSTUME_MALE,
            SPELL_NINJA_COSTUME_FEMALE,
            SPELL_LEPER_GNOME_COSTUME_MALE,
            SPELL_LEPER_GNOME_COSTUME_FEMALE,
            SPELL_GHOST_COSTUME_MALE,
            SPELL_GHOST_COSTUME_FEMALE
        });
    }

    void HandleScriptEffect()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();

        uint32 spellId = 0;
        uint8 gender = target->GetNativeGender();

        switch (GetSpellInfo()->Id)
        {
            case SPELL_HALLOWED_WAND_LEPER_GNOME:
                spellId = gender ? SPELL_LEPER_GNOME_COSTUME_FEMALE : SPELL_LEPER_GNOME_COSTUME_MALE;
                break;
            case SPELL_HALLOWED_WAND_PIRATE:
                spellId = gender ? SPELL_PIRATE_COSTUME_FEMALE : SPELL_PIRATE_COSTUME_MALE;
                break;
            case SPELL_HALLOWED_WAND_GHOST:
                spellId = gender ? SPELL_GHOST_COSTUME_FEMALE : SPELL_GHOST_COSTUME_MALE;
                break;
            case SPELL_HALLOWED_WAND_NINJA:
                spellId = gender ? SPELL_NINJA_COSTUME_FEMALE : SPELL_NINJA_COSTUME_MALE;
                break;
            case SPELL_HALLOWED_WAND_RANDOM:
                spellId = RAND(SPELL_HALLOWED_WAND_PIRATE, SPELL_HALLOWED_WAND_NINJA, SPELL_HALLOWED_WAND_LEPER_GNOME, SPELL_HALLOWED_WAND_SKELETON, SPELL_HALLOWED_WAND_WISP, SPELL_HALLOWED_WAND_GHOST, SPELL_HALLOWED_WAND_BAT);
                break;
            default:
                return;
        }
        caster->CastSpell(target, spellId, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_hallow_end_wand::HandleScriptEffect);
    }
};

/*
 * The Headless Horseman's town attacks.
 *
 * Stock TrinityCore never scripted these, so "Let the Fires Come!" and "Stop the
 * Fires!" could be taken and never finished, and Fire Brigade Practice had no
 * fires to throw at. How it works here:
 *
 * - The Orphan Matron in each town calls the Shade of the Horseman when someone
 *   accepts "Let the Fires Come!", and on every quarter hour while a person is
 *   standing near her.
 * - The Shade circles the town and lights fire markers (23686) in waves. A fire
 *   grows while it burns and, at full size, spreads to a nearby marker. How many
 *   may burn at once grows with the number of people in town (bots don't count).
 * - People douse fires with Water Buckets from the town's barrels. A bucket that
 *   lands next to another person is caught instead, so it can be passed along.
 * - Once every wave is lit and every fire is out, the Shade lands and fights.
 *   Killing it completes both quests for everyone nearby and leaves a Large
 *   Jack-o'-Lantern (Smash the Pumpkin). If the town is still burning when time
 *   runs out, the Shade gloats and leaves.
 *
 * The quest offers are gated in `conditions`: "Let the Fires Come!" only while no
 * Shade is in town, "Stop the Fires!" only during an attack, both after Fire
 * Brigade Practice.
 */
namespace HallowsEndFires
{
    enum Creatures
    {
        NPC_SHADE_OF_THE_HORSEMAN   = 23543,
        NPC_FIRE_NODE               = 23686, // town spawns carry their own ScriptName; the template stays Scarlet Monastery's bunny
        NPC_PRACTICE_FIRE           = 23537
    };

    enum Quests
    {
        QUEST_LET_THE_FIRES_COME_A  = 12135,
        QUEST_LET_THE_FIRES_COME_H  = 12139,
        QUEST_STOP_THE_FIRES_A      = 11131,
        QUEST_STOP_THE_FIRES_H      = 11219
    };

    enum Spells
    {
        SPELL_FIRE                  = 42074, // the burning visual
        SPELL_START_FIRE            = 42132, // missile from the Shade (or a fire) onto a marker
        SPELL_WATER_SPLASH          = 42348,
        SPELL_GIVE_WATER_BUCKET     = 42349,
        SPELL_CONFLAGRATION         = 42380,
        SPELL_CONFLAGRATION_SOUND   = 48149,
        SPELL_HORSEMANS_CLEAVE      = 42587
    };

    enum GameObjects
    {
        GO_LARGE_JACK_O_LANTERN     = 186887
    };

    enum Texts
    {
        TALK_CONFLAGRATION          = 0,
        TALK_PREPARE                = 1,
        TALK_START                  = 2,
        TALK_MORE_FIRES             = 3,
        TALK_FAILED                 = 4,
        TALK_DEFEATED               = 5,
        TALK_DEATH                  = 6
    };

    enum Misc
    {
        ACTION_IGNITE               = 1,
        ACTION_DOUSE                = 2,
        ACTION_EXTINGUISH           = 3,
        DATA_HEAT                   = 1,
        DATA_TOWN                   = 2,
        POINT_LAND                  = 1
    };

    enum ShadeEvents
    {
        EVENT_PREPARE = 1,
        EVENT_WAVE,
        EVENT_SPREAD,
        EVENT_CHECK,
        EVENT_TIME_UP,
        EVENT_ENGAGE,
        EVENT_CONFLAGRATION,
        EVENT_CLEAVE,
        EVENT_GIVE_UP
    };

    uint8 constexpr MAX_HEAT        = 3;
    uint8 constexpr WAVE_COUNT      = 4;
    float constexpr TOWN_RADIUS     = 80.0f;  // every town's markers sit within ~71 yards of its matron
    float constexpr SPREAD_RANGE    = 20.0f;
    float constexpr CREDIT_RANGE    = 100.0f;
    float constexpr MATRON_SENSE    = 60.0f;  // a quarter-hour attack needs someone this close to the matron
    uint32 constexpr QUARTER_HOUR   = 15 * MINUTE;
    uint32 constexpr REST_AFTER_ATTACK = 90;  // seconds before the same town can be attacked again

    struct TownAttack
    {
        uint32 MapId;
        uint32 PathId;      // waypoint_data, flown on repeat while the town burns
        Position Spawn;
        Position Land;      // where the Shade comes down to fight, near the matron
    };

    std::array<TownAttack, 6> const TownAttacks =
    { {
        { 0,   235431, { -9494.40f,     48.53f,  70.50f, 0.50f }, { -9445.10f,     63.27f,  58.16f } }, // Goldshire
        { 0,   235432, { -5558.34f,   -499.46f, 414.12f, 2.08f }, { -5616.30f,   -481.89f, 398.99f } }, // Kharanos
        { 530, 235433, { -4163.58f, -12460.30f,  63.02f, 4.31f }, { -4198.10f, -12509.13f,  46.60f } }, // Azure Watch
        { 1,   235434, {   373.20f,  -4723.40f,  31.20f, 3.20f }, {   360.90f,  -4735.50f,  11.77f } }, // Razor Hill
        { 0,   235435, {  2195.20f,    264.00f,  55.62f, 0.15f }, {  2229.40f,    263.10f,  36.13f } }, // Brill
        { 530, 235436, {  9547.91f,  -6809.90f,  27.96f, 3.40f }, {  9532.90f,  -6833.80f,  18.50f } }  // Falconwing Square
    } };

    // The town whose landing spot is closest to this matron, or -1.
    int32 FindTown(WorldObject const* matron)
    {
        int32 best = -1;
        float bestDist = 100.0f;
        for (std::size_t i = 0; i < TownAttacks.size(); ++i)
        {
            if (TownAttacks[i].MapId != matron->GetMapId())
                continue;

            float dist = matron->GetExactDist2d(TownAttacks[i].Land);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = int32(i);
            }
        }
        return best;
    }

    // A person at the keyboard: playerbots drive both virtual and transient sessions.
    bool IsRealPlayer(Player const* player)
    {
        WorldSession const* session = player->GetSession();
        return session && !session->IsVirtualSession() && !session->IsTransientPlayerSession();
    }

    uint32 CountRealPlayers(WorldObject const* center, float range)
    {
        std::list<Player*> players;
        center->GetPlayerListInGrid(players, range);
        return uint32(std::count_if(players.begin(), players.end(), [](Player const* player) { return IsRealPlayer(player); }));
    }
}

using namespace HallowsEndFires;

// 23686 - Headless Horseman Flame Bunny, town spawns only (per-spawn ScriptName)
struct npc_hallows_end_fire_node : public NullCreatureAI
{
    npc_hallows_end_fire_node(Creature* creature) : NullCreatureAI(creature), _heat(0), _growTimer(0)
    {
        // Splashing a marker must never put the thrower in combat with it.
        me->SetIsCombatDisallowed(true);
    }

    void JustAppeared() override
    {
        me->SetDisableGravity(true);
        Extinguish();
    }

    uint32 GetData(uint32 type) const override
    {
        return type == DATA_HEAT ? _heat : 0;
    }

    void DoAction(int32 action) override
    {
        switch (action)
        {
            case ACTION_IGNITE:
                if (_heat)
                    return;
                _heat = 1;
                DoCastSelf(SPELL_FIRE, true);
                _growTimer = 10 * IN_MILLISECONDS;
                UpdateSize();
                break;
            case ACTION_DOUSE:
                if (!_heat)
                    return;
                if (_heat <= 2)
                    Extinguish();
                else
                {
                    _heat -= 2;
                    _growTimer = 10 * IN_MILLISECONDS;
                    UpdateSize();
                }
                break;
            case ACTION_EXTINGUISH:
                Extinguish();
                break;
            default:
                break;
        }
    }

    void SpellHit(WorldObject* caster, SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id == SPELL_WATER_SPLASH && caster->GetTypeId() == TYPEID_PLAYER)
            DoAction(ACTION_DOUSE);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        damage = 0;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!_heat || _heat >= MAX_HEAT)
            return;

        if (_growTimer <= diff)
        {
            ++_heat;
            _growTimer = 10 * IN_MILLISECONDS;
            UpdateSize();
        }
        else
            _growTimer -= diff;
    }

private:
    void Extinguish()
    {
        _heat = 0;
        me->RemoveAurasDueToSpell(SPELL_FIRE);
        me->SetObjectScale(1.0f);
    }

    void UpdateSize()
    {
        me->SetObjectScale(0.5f + 0.5f * _heat);
    }

    uint8 _heat;
    uint32 _growTimer;
};

// 23537 - Headless Horseman - Fire (DND): the Fire Brigade Practice blazes
struct npc_hallows_end_practice_fire : public NullCreatureAI
{
    npc_hallows_end_practice_fire(Creature* creature) : NullCreatureAI(creature), _relightTimer(IN_MILLISECONDS)
    {
        me->SetIsCombatDisallowed(true);
    }

    void JustAppeared() override
    {
        _relightTimer = IN_MILLISECONDS;
    }

    void SpellHit(WorldObject* caster, SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id != SPELL_WATER_SPLASH || !me->HasAura(SPELL_FIRE))
            return;

        Player* player = caster->ToPlayer();
        if (!player)
            return;

        me->RemoveAurasDueToSpell(SPELL_FIRE);
        _relightTimer = 6 * IN_MILLISECONDS;
        player->KilledMonsterCredit(NPC_PRACTICE_FIRE, me->GetGUID());
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        damage = 0;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!_relightTimer)
            return;

        if (_relightTimer <= diff)
        {
            _relightTimer = 0;
            DoCastSelf(SPELL_FIRE, true);
        }
        else
            _relightTimer -= diff;
    }

private:
    uint32 _relightTimer;
};

// 23543 - Shade of the Horseman
struct npc_hallows_end_shade_of_the_horseman : public ScriptedAI
{
    npc_hallows_end_shade_of_the_horseman(Creature* creature) : ScriptedAI(creature), _town(-1), _waves(0), _fighting(false) { }

    void InitializeAI() override
    {
        me->SetReactState(REACT_PASSIVE);
        me->SetCanFly(true);
        me->SetDisableGravity(true);
        me->SetUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);
    }

    void MoveInLineOfSight(Unit* /*who*/) override { }

    // The matron hands over the town right after summoning; that starts the attack.
    void SetData(uint32 type, uint32 value) override
    {
        if (type != DATA_TOWN || _town >= 0 || value >= TownAttacks.size())
            return;

        _town = int32(value);
        TownAttack const& town = TownAttacks[_town];

        std::list<Creature*> nodes;
        me->GetCreatureListWithEntryInGrid(nodes, NPC_FIRE_NODE, 150.0f);
        for (Creature* node : nodes)
            if (node->GetExactDist2d(town.Land) <= TOWN_RADIUS)
                _nodes.push_back(node->GetGUID());

        me->GetMotionMaster()->MovePath(town.PathId, true);

        _events.ScheduleEvent(EVENT_PREPARE, 3s);
        _events.ScheduleEvent(EVENT_WAVE, 20s);
        _events.ScheduleEvent(EVENT_GIVE_UP, 12min); // backstop: never haunt a town forever
    }

    void MovementInform(uint32 type, uint32 id) override
    {
        if (type != POINT_MOTION_TYPE || id != POINT_LAND)
            return;

        me->SetCanFly(false);
        me->SetDisableGravity(false);
        me->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);
        me->SetReactState(REACT_AGGRESSIVE);
        _events.ScheduleEvent(EVENT_ENGAGE, 2s);
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        _fighting = true;
        _events.CancelEvent(EVENT_ENGAGE);
        _events.ScheduleEvent(EVENT_CONFLAGRATION, 6s);
        _events.ScheduleEvent(EVENT_CLEAVE, 7s);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        // Everyone ran or died: the Shade leaves rather than flying home.
        if (_fighting)
            me->DespawnOrUnsummon(2s);
    }

    void JustDied(Unit* /*killer*/) override
    {
        Talk(TALK_DEATH);
        _events.Reset();
        ExtinguishAll();

        if (_town >= 0)
        {
            Position const& land = TownAttacks[_town].Land;
            me->SummonGameObject(GO_LARGE_JACK_O_LANTERN, land, QuaternionData::fromEulerAnglesZYX(me->GetOrientation(), 0.0f, 0.0f), 3min, GO_SUMMON_TIMED_DESPAWN);
        }

        std::list<Player*> players;
        me->GetPlayerListInGrid(players, CREDIT_RANGE, false);
        for (Player* player : players)
            for (uint32 questId : { QUEST_LET_THE_FIRES_COME_A, QUEST_LET_THE_FIRES_COME_H, QUEST_STOP_THE_FIRES_A, QUEST_STOP_THE_FIRES_H })
                player->AreaExploredOrEventHappens(questId);
    }

    void UpdateAI(uint32 diff) override
    {
        _events.Update(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_PREPARE:
                    Talk(TALK_PREPARE);
                    break;
                case EVENT_WAVE:
                    LightWave();
                    if (_waves == 1)
                    {
                        _events.ScheduleEvent(EVENT_SPREAD, 15s);
                        _events.ScheduleEvent(EVENT_CHECK, 3s);
                        _events.ScheduleEvent(EVENT_TIME_UP, 5min);
                    }
                    if (_waves < WAVE_COUNT)
                        _events.Repeat(30s);
                    break;
                case EVENT_SPREAD:
                    Spread();
                    _events.Repeat(15s);
                    break;
                case EVENT_CHECK:
                    if (_waves >= WAVE_COUNT && !CountBurning())
                    {
                        Defeated();
                        return;
                    }
                    _events.Repeat(3s);
                    break;
                case EVENT_TIME_UP:
                    Failed();
                    return;
                case EVENT_ENGAGE:
                    if (Player* target = me->SelectNearestPlayer(40.0f))
                        AttackStart(target);
                    else
                        _events.Repeat(5s);
                    break;
                case EVENT_CONFLAGRATION:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                    {
                        Talk(TALK_CONFLAGRATION, target);
                        DoCast(target, SPELL_CONFLAGRATION);
                        target->CastSpell(target, SPELL_CONFLAGRATION_SOUND, true);
                    }
                    _events.Repeat(12s);
                    break;
                case EVENT_CLEAVE:
                    DoCastVictim(SPELL_HORSEMANS_CLEAVE, true);
                    _events.Repeat(8s);
                    break;
                case EVENT_GIVE_UP:
                    if (_fighting)
                    {
                        _events.Repeat(1min);
                        break;
                    }
                    ExtinguishAll();
                    me->DespawnOrUnsummon();
                    return;
                default:
                    break;
            }
        }

        if (!_fighting || !UpdateVictim())
            return;

        DoMeleeAttackIfReady();
    }

private:
    // How many fires may burn at once: a lone defender faces four, each extra person two more.
    uint32 BurningCap() const
    {
        uint32 people = _town >= 0 ? CountRealPlayers(me, TOWN_RADIUS + 40.0f) : 0;
        return std::min<uint32>(uint32(_nodes.size()), 2 + 2 * std::max<uint32>(people, 1));
    }

    uint32 CountBurning() const
    {
        uint32 burning = 0;
        for (ObjectGuid const& guid : _nodes)
            if (Creature* node = ObjectAccessor::GetCreature(*me, guid))
                if (node->AI()->GetData(DATA_HEAT))
                    ++burning;
        return burning;
    }

    void Ignite(WorldObject* source, Creature* node)
    {
        source->CastSpell(node, SPELL_START_FIRE, true); // the missile; the marker lights either way
        node->AI()->DoAction(ACTION_IGNITE);
    }

    void LightWave()
    {
        ++_waves;
        if (_waves == 2)
            Talk(TALK_START);
        else if (_waves == 3)
            Talk(TALK_MORE_FIRES);

        uint32 cap = BurningCap();
        uint32 burning = CountBurning();
        if (burning >= cap)
            return;

        std::vector<Creature*> unlit;
        for (ObjectGuid const& guid : _nodes)
            if (Creature* node = ObjectAccessor::GetCreature(*me, guid))
                if (!node->AI()->GetData(DATA_HEAT))
                    unlit.push_back(node);

        uint32 people = CountRealPlayers(me, TOWN_RADIUS + 40.0f);
        uint32 count = std::min<uint32>(cap - burning, 1 + people / 2);
        Trinity::Containers::RandomResize(unlit, count);
        for (Creature* node : unlit)
            Ignite(me, node);
    }

    // A fire at full size jumps to the nearest dark marker, while the cap allows.
    void Spread()
    {
        uint32 cap = BurningCap();
        uint32 burning = CountBurning();

        std::vector<Creature*> nodes;
        for (ObjectGuid const& guid : _nodes)
            if (Creature* node = ObjectAccessor::GetCreature(*me, guid))
                nodes.push_back(node);

        for (Creature* source : nodes)
        {
            if (burning >= cap)
                return;

            if (source->AI()->GetData(DATA_HEAT) < MAX_HEAT)
                continue;

            Creature* target = nullptr;
            for (Creature* node : nodes)
                if (!node->AI()->GetData(DATA_HEAT) && source->GetExactDist(node) <= SPREAD_RANGE)
                    if (!target || source->GetExactDist(node) < source->GetExactDist(target))
                        target = node;

            if (target)
            {
                Ignite(source, target);
                ++burning;
            }
        }
    }

    void ExtinguishAll()
    {
        for (ObjectGuid const& guid : _nodes)
            if (Creature* node = ObjectAccessor::GetCreature(*me, guid))
                node->AI()->DoAction(ACTION_EXTINGUISH);
    }

    void Defeated()
    {
        _events.Reset();
        Talk(TALK_DEFEATED);
        me->GetMotionMaster()->Clear();
        me->GetMotionMaster()->MovePoint(POINT_LAND, TownAttacks[_town].Land, false);
        _events.ScheduleEvent(EVENT_GIVE_UP, 3min); // nobody comes to fight it
    }

    void Failed()
    {
        _events.Reset();
        Talk(TALK_FAILED);
        ExtinguishAll();
        me->DespawnOrUnsummon(3s);
    }

    EventMap _events;
    GuidVector _nodes;
    int32 _town;
    uint8 _waves;
    bool _fighting;
};

// 24519 - Costumed Orphan Matron, 23973 - Masked Orphan Matron
struct npc_hallows_end_orphan_matron : public ScriptedAI
{
    npc_hallows_end_orphan_matron(Creature* creature) : ScriptedAI(creature), _town(-1), _checkTimer(5 * IN_MILLISECONDS), _lastQuarter(0), _restUntil(0), _pending(false) { }

    void JustAppeared() override
    {
        _town = FindTown(me);
    }

    void OnQuestAccept(Player* /*player*/, Quest const* quest) override
    {
        if (quest->GetQuestId() == QUEST_LET_THE_FIRES_COME_A || quest->GetQuestId() == QUEST_LET_THE_FIRES_COME_H)
        {
            _pending = true;
            TryStartAttack();
        }
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        if (summon->GetGUID() == _shadeGuid)
        {
            _shadeGuid.Clear();
            _restUntil = GameTime::GetGameTime() + REST_AFTER_ATTACK;
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (_checkTimer > diff)
        {
            _checkTimer -= diff;
            return;
        }
        _checkTimer = 5 * IN_MILLISECONDS;

        // The Horseman also comes on every quarter hour, when somebody is here to see it.
        time_t now = GameTime::GetGameTime();
        uint32 quarter = uint32(now / QUARTER_HOUR);
        if (quarter != _lastQuarter && now % QUARTER_HOUR < MINUTE)
        {
            _lastQuarter = quarter;
            if (CountRealPlayers(me, MATRON_SENSE))
                _pending = true;
        }

        if (_pending)
            TryStartAttack();
    }

private:
    void TryStartAttack()
    {
        if (_town < 0)
        {
            _pending = false;
            return;
        }

        // One attack at a time; an accept during an attack just joins it.
        if (!_shadeGuid.IsEmpty() && ObjectAccessor::GetCreature(*me, _shadeGuid))
        {
            _pending = false;
            return;
        }

        if (GameTime::GetGameTime() < _restUntil)
            return; // still pending; the next check starts it

        _pending = false;
        if (Creature* shade = me->SummonCreature(NPC_SHADE_OF_THE_HORSEMAN, TownAttacks[_town].Spawn, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 15s))
        {
            _shadeGuid = shade->GetGUID();
            shade->AI()->SetData(DATA_TOWN, uint32(_town));
        }
    }

    ObjectGuid _shadeGuid;
    int32 _town;
    uint32 _checkTimer;
    uint32 _lastQuarter;
    time_t _restUntil;
    bool _pending;
};

// 42339 - Bucket Lands
class spell_hallows_end_bucket_lands : public SpellScript
{
    PrepareSpellScript(spell_hallows_end_bucket_lands);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_GIVE_WATER_BUCKET, SPELL_WATER_SPLASH });
    }

    // Whatever is closest to where the bucket lands takes it: a person catches it, a fire gets doused.
    void FilterTargets(std::list<WorldObject*>& targets)
    {
        WorldLocation const* dest = GetExplTargetDest();
        targets.remove(GetCaster());
        if (targets.empty() || !dest)
            return;

        WorldObject* nearest = targets.front();
        for (WorldObject* target : targets)
            if (target->GetExactDist(dest) < nearest->GetExactDist(dest))
                nearest = target;

        targets.clear();
        targets.push_back(nearest);
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (Player* catcher = GetHitPlayer())
            caster->CastSpell(catcher, SPELL_GIVE_WATER_BUCKET, true);
        else if (Unit* fire = GetHitUnit())
            caster->CastSpell(fire->GetPosition(), SPELL_WATER_SPLASH, true);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_hallows_end_bucket_lands::FilterTargets, EFFECT_0, TARGET_UNIT_DEST_AREA_ENTRY);
        OnEffectHitTarget += SpellEffectFn(spell_hallows_end_bucket_lands::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddSC_event_hallows_end()
{
    RegisterSpellScript(spell_hallow_end_candy);
    RegisterSpellScript(spell_hallow_end_candy_pirate);
    RegisterSpellScript(spell_hallow_end_trick);
    RegisterSpellScript(spell_hallow_end_trick_or_treat);
    RegisterSpellScript(spell_hallow_end_tricky_treat);
    RegisterSpellScript(spell_hallow_end_wand);
    RegisterSpellScript(spell_hallows_end_bucket_lands);
    RegisterCreatureAI(npc_hallows_end_fire_node);
    RegisterCreatureAI(npc_hallows_end_practice_fire);
    RegisterCreatureAI(npc_hallows_end_shade_of_the_horseman);
    RegisterCreatureAI(npc_hallows_end_orphan_matron);
}
