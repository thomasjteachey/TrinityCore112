/*
 * Blood Beneath the Arena - Zalvaxa the Wicked, the twins Thraxia and Malizzia,
 * Vraka'ti the Blood Gorger and Anok'Suten.
 */

#include "blood_beneath_arena.h"
#include "Creature.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "InstanceScript.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <list>
#include <map>

namespace
{
enum BBABossSpells
{
    // named abilities (custom rows, bba_ability_spells.py)
    SPELL_BLOOD_FRENZY          = 92111,
    SPELL_BONE_SPEAR            = 92126,
    SPELL_GRAVE_ROT             = 92127,
    SPELL_VOODOO_HEX            = 92128,
    SPELL_CORPSE_EXPLOSION      = 92129,
    SPELL_BLOOD_FED             = 92130,    // stacks to 10, +10% damage each
    SPELL_ENDLESS_DEAD          = 92131,    // Zalvaxa's ritual channel (visual)
    SPELL_GARROTE               = 92132,
    SPELL_CRIMSON_STEP          = 92133,
    SPELL_BLOOD_REUNION         = 92135,    // 2 sec, refreshed while the twins stand together
    SPELL_SHADOWBURN            = 92136,
    SPELL_BLADE_FLURRY          = 92151,
    SPELL_BLOOD_BOLT            = 92138,
    SPELL_SACRIFICIAL_MARK      = 92139,
    SPELL_SACRIFICIAL_ERUPTION  = 92140,
    SPELL_BLOOD_CHAINS          = 92141,
    SPELL_BLOOD_DRAIN           = 92142,
    SPELL_BLOOD_NOVA            = 92143,
    SPELL_WEB_COCOON            = 92144,    // stun + DoT; cast by the victim on itself
    SPELL_SHADOW_WEB            = 92145,
    SPELL_PIERCING_LEGS         = 92146,
    SPELL_BLOOD_WEB             = 92147,
    SPELL_BLOOD_HARVEST         = 92148,
    SPELL_LAST_THREAD           = 92149,
    // stock
    SPELL_SHADOW_BOLT_HEAVY     = 15232,
    SPELL_SHADOW_WORD_PAIN      = 15654,
    SPELL_DRAIN_LIFE            = 17238,
    SPELL_FEAR                  = 12096,
    SPELL_SINISTER_STRIKE       = 15581,
    SPELL_GOUGE                 = 12540,
    SPELL_BLIND                 = 21060,
    SPELL_EVISCERATE            = 15691,
    SPELL_VANISH                = 24699,
    SPELL_CURSE_OF_AGONY        = 18266,
    SPELL_IMMOLATE              = 15570,
    SPELL_RAIN_OF_FIRE          = 4629,
};

enum BBABossActions
{
    ACTION_SACRIFICE            = 20,   // Zalvaxa -> Risen: walk to her
    ACTION_FED                  = 21,   // Risen -> Zalvaxa: a Blood-Fed stack
    ACTION_TWIN_REVIVE          = 22,
    ACTION_ANOK_EMERGE          = 23,
    ACTION_VRAKATI_RESTORE      = 24
};

enum BBABossTexts
{
    // Zalvaxa
    SAY_ZAL_SUMMONING           = 0,
    SAY_ZAL_WAVE                = 1,
    SAY_ZAL_BONE_SPEAR          = 2,
    SAY_ZAL_GRAVE_ROT           = 3,
    SAY_ZAL_HEX                 = 4,
    SAY_ZAL_INTERRUPTED         = 5,
    SAY_ZAL_TELEPORT            = 6,
    SAY_ZAL_LOW                 = 7,
    SAY_ZAL_DEATH               = 8,
    SAY_ZAL_REPLY               = 9,
    // Thraxia / Malizzia share the layout
    SAY_TWIN_ABILITY_0          = 0,
    SAY_TWIN_ABILITY_1          = 1,
    SAY_TWIN_ABILITY_2          = 2,
    SAY_TWIN_ABILITY_3          = 3,
    SAY_TWIN_FELL               = 4,
    SAY_TWIN_DEATH              = 5,
    // Vraka'ti
    SAY_VRA_FIRST               = 0,
    SAY_VRA_COMBAT              = 1,
    SAY_VRA_MARK                = 2,
    SAY_VRA_DRAIN               = 3,
    SAY_VRA_ADDS                = 4,
    SAY_VRA_REVEAL_1            = 5,
    SAY_VRA_REVEAL_2            = 6,
    SAY_VRA_REVEAL_3            = 7,
    SAY_VRA_BETRAYED            = 8,
    // Anok'Suten
    SAY_ANOK_UNMASK             = 0,
    SAY_ANOK_COCOON             = 1,
    SAY_ANOK_WEB                = 2,
    SAY_ANOK_SWARM              = 3,
    SAY_ANOK_LEGS               = 4,
    SAY_ANOK_BLOOD_WEB          = 5,
    SAY_ANOK_ENRAGE             = 6,
    SAY_ANOK_DEATH              = 7,
    // Var'jun
    SAY_VARJUN_AFTER_ZALVAXA    = 11
};

uint32 const TEXT_VRAKATI_OFFER = 60106;
uint32 const ACTION_GOSSIP_FIGHT = GOSSIP_ACTION_INFO_DEF + 10;

// player-or-pet spells that count as "interrupting" Zalvaxa's ritual
bool IsInterruptLike(SpellInfo const* spell)
{
    return spell->HasEffect(SPELL_EFFECT_INTERRUPT_CAST) || spell->HasAura(SPELL_AURA_MOD_SILENCE)
        || spell->HasAura(SPELL_AURA_MOD_STUN) || spell->HasAura(SPELL_AURA_MOD_PACIFY_SILENCE);
}

bool CastByPlayerSide(WorldObject* caster)
{
    if (!caster)
        return false;
    if (caster->GetTypeId() == TYPEID_PLAYER)
        return true;
    if (Unit* unit = caster->ToUnit())
        return unit->GetCharmerOrOwnerPlayerOrPlayerItself() != nullptr;
    return false;
}

// Talk the first time an ability is used, then a third of the time
struct TalkLimiter
{
    uint32 used = 0;
    bool Should(uint8 id)
    {
        uint32 const bit = 1u << id;
        if (!(used & bit))
        {
            used |= bit;
            return true;
        }
        return roll_chance_i(33);
    }
};
}

// ---------------------------------------------------------------------------------------
// Zalvaxa the Wicked. Dormant on her dais until Var'jun calls her out; then she channels the
// endless dead and cannot be hurt. The first interrupt, silence or stun from a player wakes
// her: she teleports to the middle of the chamber and the real fight starts.
// ---------------------------------------------------------------------------------------
struct boss_bba_zalvaxa : public BossAI
{
    enum Events
    {
        EVENT_PRE_SUMMON = 1, EVENT_PRE_CHANNEL, EVENT_AWAKEN_MOVE,
        EVENT_BONE_SPEAR, EVENT_GRAVE_ROT, EVENT_HEX, EVENT_RAISE, EVENT_DRAIN, EVENT_FEAR
    };

    boss_bba_zalvaxa(Creature* creature) : BossAI(creature, DATA_ZALVAXA) { }

    void Reset() override
    {
        _Reset();
        _called = false;
        _awake = false;
        _sacrificed = false;
        _desperate = false;
        _stacks = 0;
        _rot.clear();
        me->SetObjectScale(me->GetCreatureTemplate()->scale);
        me->SetReactState(REACT_PASSIVE);
        me->SetImmuneToPC(true);
    }

    void DoAction(int32 action) override
    {
        if (action == ACTION_ZALVAXA_CALLED && !_called && instance->GetBossState(DATA_ZALVAXA) != DONE)
        {
            _called = true;
            Talk(SAY_ZAL_REPLY);
            me->SetImmuneToPC(false);
            DoZoneInCombat();
        }
        else if (action == ACTION_FED)
        {
            ++_stacks;
            DoCastSelf(SPELL_BLOOD_FED, true);
            me->SetObjectScale(me->GetCreatureTemplate()->scale * (1.0f + 0.05f * std::min<uint32>(_stacks, 10)));
        }
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        if (!_awake)
        {
            Talk(SAY_ZAL_SUMMONING);
            events.ScheduleEvent(EVENT_PRE_SUMMON, 1s);
            events.ScheduleEvent(EVENT_PRE_CHANNEL, 3s);
        }
    }

    void SpellHit(WorldObject* caster, SpellInfo const* spellInfo) override
    {
        if (_called && !_awake && CastByPlayerSide(caster) && IsInterruptLike(spellInfo))
            Awaken();
    }

    void Awaken()
    {
        _awake = true;
        events.Reset();
        me->InterruptNonMeleeSpells(false);
        me->RemoveAurasDueToSpell(SPELL_ENDLESS_DEAD);
        // she has to be stunnable to be woken; once awake she is not
        for (Mechanics m : { MECHANIC_STUN, MECHANIC_KNOCKOUT, MECHANIC_ROOT })
            me->ApplySpellImmune(0, IMMUNITY_MECHANIC, m, true);
        Talk(SAY_ZAL_INTERRUPTED);
        events.ScheduleEvent(EVENT_AWAKEN_MOVE, 2s);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*type*/, SpellInfo const* /*spellInfo*/) override
    {
        if (!_awake)
        {
            damage = 0;
            return;
        }
        if (!_sacrificed && me->HealthBelowPctDamaged(45, damage))
        {
            _sacrificed = true;
            Talk(SAY_ZAL_LOW);
            for (ObjectGuid guid : summons)
                if (Creature* risen = ObjectAccessor::GetCreature(*me, guid))
                    if (risen->IsAlive() && risen->IsAIEnabled())
                        risen->AI()->DoAction(ACTION_SACRIFICE);
        }
        if (!_desperate && me->HealthBelowPctDamaged(15, damage))
            _desperate = true;
    }

    void JustSummoned(Creature* summon) override
    {
        BossAI::JustSummoned(summon);
        if (summon->IsAIEnabled())
            summon->AI()->DoZoneInCombat();
    }

    void JustDied(Unit* /*killer*/) override
    {
        Talk(SAY_ZAL_DEATH);
        _JustDied();
        if (Creature* varjun = instance->GetCreature(DATA_VARJUN))
            if (varjun->IsAIEnabled())
                varjun->AI()->Talk(SAY_VARJUN_AFTER_ZALVAXA);
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        // a wipe puts her back to sleep; Var'jun can call her out again
        if (instance->GetData(DATA_STAGE) == STAGE_ZALVAXA)
            instance->SetData(DATA_STAGE, STAGE_RITUAL_DONE);
        BossAI::EnterEvadeMode(why);
        _DespawnAtEvade(5s);
    }

    void SummonRisen(Position const& near, uint8 count)
    {
        for (uint8 i = 0; i < count; ++i)
        {
            Position pos = near;
            me->MovePositionToFirstCollision(pos, frand(2.0f, 6.0f), frand(0.0f, float(2 * M_PI)));
            me->SummonCreature(NPC_RISEN_BLOODBOUND, pos, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 10s);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        // Grave Rot: a skeleton rises where each rot runs out
        for (auto itr = _rot.begin(); itr != _rot.end();)
        {
            if (itr->second <= diff)
            {
                if (Unit* target = ObjectAccessor::GetUnit(*me, itr->first))
                    SummonRisen(target->GetPosition(), 1);
                itr = _rot.erase(itr);
            }
            else
            {
                itr->second -= diff;
                ++itr;
            }
        }

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_PRE_SUMMON:
                    SummonRisen(BBARisenSpawns[urand(0, 5)], 2);
                    if (roll_chance_i(25))
                        Talk(SAY_ZAL_WAVE);
                    events.Repeat(8s);
                    break;
                case EVENT_PRE_CHANNEL:
                    // the ritual visual; what wakes her is the player's interrupt landing (SpellHit)
                    if (!me->HasAura(SPELL_ENDLESS_DEAD))
                        DoCastSelf(SPELL_ENDLESS_DEAD);
                    events.Repeat(1s);
                    break;
                case EVENT_AWAKEN_MOVE:
                    Talk(SAY_ZAL_TELEPORT);
                    me->NearTeleportTo(BBAZalvaxaMiddlePos);
                    me->SetReactState(REACT_AGGRESSIVE);
                    events.ScheduleEvent(EVENT_BONE_SPEAR, 5s);
                    events.ScheduleEvent(EVENT_GRAVE_ROT, 9s);
                    events.ScheduleEvent(EVENT_HEX, 15s);
                    events.ScheduleEvent(EVENT_RAISE, 12s);
                    events.ScheduleEvent(EVENT_DRAIN, 20s);
                    events.ScheduleEvent(EVENT_FEAR, 26s);
                    break;
                case EVENT_BONE_SPEAR:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                    {
                        if (_talk.Should(SAY_ZAL_BONE_SPEAR))
                            Talk(SAY_ZAL_BONE_SPEAR);
                        DoCast(target, SPELL_BONE_SPEAR);
                    }
                    events.Repeat(8s);
                    break;
                case EVENT_GRAVE_ROT:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                    {
                        if (_talk.Should(SAY_ZAL_GRAVE_ROT))
                            Talk(SAY_ZAL_GRAVE_ROT);
                        DoCast(target, SPELL_GRAVE_ROT, true);
                        _rot[target->GetGUID()] = 18000;
                    }
                    events.Repeat(18s);
                    break;
                case EVENT_HEX:
                    if (_talk.Should(SAY_ZAL_HEX))
                        Talk(SAY_ZAL_HEX);
                    DoCastSelf(SPELL_VOODOO_HEX);
                    events.Repeat(20s);
                    break;
                case EVENT_RAISE:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                        SummonRisen(target->GetPosition(), 3);
                    if (roll_chance_i(33))
                        Talk(SAY_ZAL_WAVE);
                    events.Repeat(_desperate ? 9s : 15s);
                    break;
                case EVENT_DRAIN:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 20.0f, true))
                        DoCast(target, SPELL_DRAIN_LIFE, CastSpellExtraArgs().AddSpellBP0(_desperate ? 250 : 170));
                    events.Repeat(24s);
                    break;
                case EVENT_FEAR:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 1, 20.0f, true))
                        DoCast(target, SPELL_FEAR);
                    events.Repeat(30s);
                    break;
                default:
                    break;
            }
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        if (_awake)
            DoMeleeAttackIfReady();
    }

private:
    bool _called = false;
    bool _awake = false;
    bool _sacrificed = false;
    bool _desperate = false;
    uint32 _stacks = 0;
    std::map<ObjectGuid, uint32> _rot;
    TalkLimiter _talk;
};

// Risen Bloodbound: weak, endless. A quarter of them burst when they die; after 45% they
// stop fighting and crawl to Zalvaxa to feed her.
struct npc_bba_risen : public ScriptedAI
{
    npc_bba_risen(Creature* creature) : ScriptedAI(creature) { }

    void IsSummonedBy(WorldObject* summoner) override
    {
        _summoner = summoner->GetGUID();
        DoZoneInCombat();
    }

    void DoAction(int32 action) override
    {
        if (action != ACTION_SACRIFICE || _crawling)
            return;
        Creature* zalvaxa = ObjectAccessor::GetCreature(*me, _summoner);
        if (!zalvaxa)
            return;
        _crawling = true;
        me->AttackStop();
        me->SetReactState(REACT_PASSIVE);
        me->SetWalk(true);
        me->GetMotionMaster()->Clear();
        me->GetMotionMaster()->MoveFollow(zalvaxa, 0.0f, 0.0f);
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (!_crawling && roll_chance_i(25))
            BBASpawnZone(me, me->GetPosition(), SPELL_CORPSE_EXPLOSION, -1, -1, 1500, 4s, 0);
    }

    void UpdateAI(uint32 /*diff*/) override
    {
        if (_crawling)
        {
            Creature* zalvaxa = ObjectAccessor::GetCreature(*me, _summoner);
            if (!zalvaxa || !zalvaxa->IsAlive())
            {
                me->DespawnOrUnsummon();
                return;
            }
            if (me->IsWithinDist(zalvaxa, 3.0f))
            {
                if (zalvaxa->IsAIEnabled())
                    zalvaxa->AI()->DoAction(ACTION_FED);
                me->DespawnOrUnsummon();
            }
            return;
        }

        if (!UpdateVictim())
            return;
        DoMeleeAttackIfReady();
    }

private:
    ObjectGuid _summoner;
    bool _crawling = false;
};

// ---------------------------------------------------------------------------------------
// The twins. Thraxia takes half damage from magic, Malizzia half from physical. Together
// within 15 yards they heal and hit harder. The first to fall lies in a pool of blood for
// 30 seconds: kill the other in that window or she rises again at 30%.
// ---------------------------------------------------------------------------------------
struct boss_bba_twin : public ScriptedAI
{
    enum Events
    {
        EVENT_PROXIMITY = 1, EVENT_REVIVE,
        EVENT_SINISTER, EVENT_GOUGE, EVENT_BLIND, EVENT_FLURRY, EVENT_EVISCERATE, EVENT_STEP_STRIKE,
        EVENT_BOLT, EVENT_AGONY, EVENT_FEAR, EVENT_DRAIN, EVENT_IMMOLATE, EVENT_RAIN, EVENT_SHADOWBURN, EVENT_SHADOW_STEP
    };

    boss_bba_twin(Creature* creature) : ScriptedAI(creature), _instance(creature->GetInstanceScript())
    {
        _thraxia = creature->GetEntry() == NPC_THRAXIA;
    }

    Creature* Sibling() const
    {
        return _instance->GetCreature(_thraxia ? DATA_MALIZZIA : DATA_THRAXIA);
    }

    void Reset() override
    {
        _events.Reset();
        _fallen = false;
        _vanished = false;
        me->SetStandState(UNIT_STAND_STATE_STAND);
        me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
        me->SetReactState(REACT_AGGRESSIVE);
        if (_instance->GetBossState(DATA_TWINS) == IN_PROGRESS)
            _instance->SetBossState(DATA_TWINS, NOT_STARTED);
        Lock(_instance->GetData(DATA_STAGE) < STAGE_UPPER);
    }

    void Lock(bool locked)
    {
        me->SetImmuneToPC(locked);
        if (locked)
            me->SetUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
        else
            me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
    }

    void DoAction(int32 action) override
    {
        switch (action)
        {
            case ACTION_UNLOCK:
                Lock(false);
                break;
            case ACTION_TWIN_FELL:
                // the sister is down: frenzy and start the clock on her return
                if (!_thraxia)
                    Talk(SAY_TWIN_FELL);
                DoCastSelf(SPELL_BLOOD_FRENZY, true);
                _events.ScheduleEvent(EVENT_REVIVE, 30s);
                break;
            case ACTION_TWIN_REVIVE:
                if (!_fallen)
                    break;
                _fallen = false;
                me->SetStandState(UNIT_STAND_STATE_STAND);
                me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
                me->SetReactState(REACT_AGGRESSIVE);
                me->SetHealth(me->CountPctFromMaxHealth(30));
                DoZoneInCombat();
                ScheduleAbilities();
                break;
            case ACTION_TWIN_DIED:
                // the survivor died while this one lay in the pool: it never gets up
                if (_fallen)
                {
                    _fallen = false;
                    me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
                    me->KillSelf();
                }
                break;
            default:
                break;
        }
    }

    void JustEngagedWith(Unit* who) override
    {
        _instance->SetBossState(DATA_TWINS, IN_PROGRESS);
        if (Creature* sibling = Sibling())
            if (sibling->IsAlive() && !sibling->IsInCombat() && sibling->IsAIEnabled())
                sibling->AI()->AttackStart(who);
        DoZoneInCombat();
        ScheduleAbilities();
        if (_thraxia)
        {
            Talk(SAY_TWIN_ABILITY_0);
            DoCastVictim(SPELL_GARROTE, true);
        }
    }

    void ScheduleAbilities()
    {
        _events.Reset();
        _events.ScheduleEvent(EVENT_PROXIMITY, 1s);
        if (_thraxia)
        {
            _events.ScheduleEvent(EVENT_SINISTER, 5s);
            _events.ScheduleEvent(EVENT_GOUGE, 12s);
            _events.ScheduleEvent(EVENT_BLIND, 20s);
            _events.ScheduleEvent(EVENT_FLURRY, 28s);
            _events.ScheduleEvent(EVENT_EVISCERATE, 18s);
        }
        else
        {
            _events.ScheduleEvent(EVENT_BOLT, 2s);
            _events.ScheduleEvent(EVENT_AGONY, 10s);
            _events.ScheduleEvent(EVENT_FEAR, 18s);
            _events.ScheduleEvent(EVENT_DRAIN, 26s);
            _events.ScheduleEvent(EVENT_IMMOLATE, 7s);
            _events.ScheduleEvent(EVENT_RAIN, 24s);
            _events.ScheduleEvent(EVENT_SHADOWBURN, 6s);
            _events.ScheduleEvent(EVENT_SHADOW_STEP, 25s);
        }
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*type*/, SpellInfo const* spellInfo) override
    {
        if (_fallen)
        {
            damage = 0;
            return;
        }

        bool const magic = spellInfo && !(spellInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL);
        if (_thraxia == magic)
            damage /= 2;

        if (_thraxia && !_vanished && me->HealthBelowPctDamaged(60, damage))
        {
            _vanished = true;
            DoCastSelf(SPELL_VANISH, true);
            _events.ScheduleEvent(EVENT_STEP_STRIKE, 3s);
        }

        if (damage < me->GetHealth())
            return;

        Creature* sibling = Sibling();
        boss_bba_twin* sibAI = sibling && sibling->IsAlive() && sibling->IsAIEnabled() ? dynamic_cast<boss_bba_twin*>(sibling->AI()) : nullptr;
        if (sibAI && !sibAI->_fallen)
        {
            damage = me->GetHealth() > 1 ? me->GetHealth() - 1 : 0;
            Fall(sibling);
        }
    }

    void Fall(Creature* sibling)
    {
        _fallen = true;
        _events.Reset();
        me->InterruptNonMeleeSpells(false);
        me->RemoveAurasDueToSpell(SPELL_VANISH);
        me->AttackStop();
        me->SetReactState(REACT_PASSIVE);
        me->GetMotionMaster()->Clear();
        me->StopMoving();
        me->SetUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
        me->SetStandState(UNIT_STAND_STATE_DEAD);
        if (_thraxia)
            Talk(SAY_TWIN_FELL);
        if (sibling->IsAIEnabled())
            sibling->AI()->DoAction(ACTION_TWIN_FELL);
    }

    void JustDied(Unit* /*killer*/) override
    {
        Talk(SAY_TWIN_DEATH);
        Creature* sibling = Sibling();
        if (sibling && sibling->IsAlive())
        {
            if (sibling->IsAIEnabled())
                sibling->AI()->DoAction(ACTION_TWIN_DIED);
            if (sibling->IsAlive())
                return;     // she is still on her feet: the encounter goes on
        }
        _instance->SetBossState(DATA_TWINS, DONE);
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        // a fallen twin never evades on her own; the survivor resets both
        if (_fallen || _evading)
            return;
        _evading = true;
        if (Creature* sibling = Sibling())
            if (sibling->IsAlive() && sibling->IsAIEnabled())
                if (boss_bba_twin* sibAI = dynamic_cast<boss_bba_twin*>(sibling->AI()))
                    if (!sibAI->_evading)
                    {
                        sibAI->_fallen = false;
                        sibling->AI()->EnterEvadeMode(why);
                    }
        ScriptedAI::EnterEvadeMode(why);
        _evading = false;
    }

    void UpdateAI(uint32 diff) override
    {
        if (_fallen)
            return;
        if (!UpdateVictim())
            return;

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_PROXIMITY:
                {
                    Creature* sibling = Sibling();
                    boss_bba_twin* sibAI = sibling && sibling->IsAIEnabled() ? dynamic_cast<boss_bba_twin*>(sibling->AI()) : nullptr;
                    // together within 15 yards: Blood Reunion (+25% damage, 1% health a second)
                    if (sibling && sibling->IsAlive() && sibAI && !sibAI->_fallen && me->IsWithinDist(sibling, 15.0f))
                        DoCastSelf(SPELL_BLOOD_REUNION, true);
                    _events.Repeat(1s);
                    break;
                }
                case EVENT_REVIVE:
                    if (Creature* sibling = Sibling())
                        if (sibling->IsAIEnabled())
                            sibling->AI()->DoAction(ACTION_TWIN_REVIVE);
                    me->RemoveAurasDueToSpell(SPELL_BLOOD_FRENZY);
                    break;
                // Thraxia
                case EVENT_SINISTER:
                    DoCastVictim(SPELL_SINISTER_STRIKE, CastSpellExtraArgs(true).AddSpellBP0(300));
                    _events.Repeat(5s);
                    break;
                case EVENT_GOUGE:
                    if (_talk.Should(SAY_TWIN_ABILITY_1))
                        Talk(SAY_TWIN_ABILITY_1);
                    DoCastVictim(SPELL_GOUGE, true);
                    _events.Repeat(20s);
                    break;
                case EVENT_BLIND:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 1, 10.0f, true))
                    {
                        if (_talk.Should(SAY_TWIN_ABILITY_2))
                            Talk(SAY_TWIN_ABILITY_2);
                        DoCast(target, SPELL_BLIND, true);
                    }
                    _events.Repeat(28s);
                    break;
                case EVENT_FLURRY:
                    if (_talk.Should(SAY_TWIN_ABILITY_3))
                        Talk(SAY_TWIN_ABILITY_3);
                    DoCastSelf(SPELL_BLADE_FLURRY, true);
                    _events.Repeat(36s);
                    break;
                case EVENT_EVISCERATE:
                    DoCastVictim(SPELL_EVISCERATE, CastSpellExtraArgs(true).AddSpellBP0(1200));
                    _events.Repeat(18s);
                    break;
                case EVENT_STEP_STRIKE:
                    me->RemoveAurasDueToSpell(SPELL_VANISH);
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 1, 50.0f, true))
                    {
                        Talk(SAY_TWIN_ABILITY_2);
                        me->NearTeleportTo(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), target->GetOrientation());
                        DoCast(target, SPELL_CRIMSON_STEP, true);
                    }
                    break;
                // Malizzia
                case EVENT_BOLT:
                    if (_talk.Should(SAY_TWIN_ABILITY_0))
                        Talk(SAY_TWIN_ABILITY_0);
                    DoCastVictim(SPELL_SHADOW_BOLT_HEAVY, CastSpellExtraArgs().AddSpellBP0(440));
                    _events.Repeat(4s, 5s);
                    break;
                case EVENT_AGONY:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                    {
                        if (_talk.Should(SAY_TWIN_ABILITY_1))
                            Talk(SAY_TWIN_ABILITY_1);
                        DoCast(target, SPELL_CURSE_OF_AGONY, CastSpellExtraArgs(true).AddSpellBP0(90));
                    }
                    _events.Repeat(20s);
                    break;
                case EVENT_FEAR:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 1, 20.0f, true))
                    {
                        if (_talk.Should(SAY_TWIN_ABILITY_2))
                            Talk(SAY_TWIN_ABILITY_2);
                        DoCast(target, SPELL_FEAR);
                    }
                    _events.Repeat(28s);
                    break;
                case EVENT_DRAIN:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 20.0f, true))
                    {
                        if (_talk.Should(SAY_TWIN_ABILITY_3))
                            Talk(SAY_TWIN_ABILITY_3);
                        DoCast(target, SPELL_DRAIN_LIFE, CastSpellExtraArgs().AddSpellBP0(150));
                    }
                    _events.Repeat(22s);
                    break;
                case EVENT_IMMOLATE:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                        DoCast(target, SPELL_IMMOLATE, CastSpellExtraArgs(true).AddSpellBP0(40).AddSpellBP1(380));
                    _events.Repeat(16s);
                    break;
                case EVENT_RAIN:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                        me->CastSpell(target->GetPosition(), SPELL_RAIN_OF_FIRE, CastSpellExtraArgs().AddSpellBP0(180));
                    _events.Repeat(24s);
                    break;
                case EVENT_SHADOWBURN:
                {
                    // the execute: a player below 35% takes a heavy instant bolt
                    std::list<Player*> players;
                    me->GetPlayerListInGrid(players, 30.0f);
                    for (Player* player : players)
                        if (player->IsAlive() && !player->IsGameMaster() && player->GetHealthPct() < 35.0f)
                        {
                            DoCast(player, SPELL_SHADOWBURN, true);
                            _events.Repeat(30s);
                            return;
                        }
                    _events.Repeat(3s);
                    break;
                }
                case EVENT_SHADOW_STEP:
                    if (Unit* target = SelectTarget(SelectTargetMethod::MaxDistance, 0, 40.0f, true))
                        me->NearTeleportTo(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), target->GetOrientation());
                    _events.Repeat(25s);
                    break;
                default:
                    break;
            }
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        DoMeleeAttackIfReady();
    }

    bool _fallen = false;
    bool _evading = false;

private:
    InstanceScript* _instance;
    EventMap _events;
    bool _thraxia = false;
    bool _vanished = false;
    TalkLimiter _talk;
};

// ---------------------------------------------------------------------------------------
// Vraka'ti the Blood Gorger. Offers the party a treasure; the fight starts by gossip. At 25%
// she stops taking damage, unmasks and hands the fight to Anok'Suten.
// ---------------------------------------------------------------------------------------
struct boss_bba_vrakati : public ScriptedAI
{
    enum Events
    {
        EVENT_BLOOD_BOLT = 1, EVENT_PAIN, EVENT_MARK, EVENT_MARK_BURST, EVENT_CHAINS, EVENT_DRAIN, EVENT_FAITHFUL, EVENT_NOVA,
        EVENT_REVEAL_2, EVENT_REVEAL_3, EVENT_REVEAL_4, EVENT_LEAVE
    };

    boss_bba_vrakati(Creature* creature) : ScriptedAI(creature), _instance(creature->GetInstanceScript()), _summons(me) { }

    void Reset() override
    {
        _events.Reset();
        _summons.DespawnAll();
        _revealed = false;
        me->SetVisible(true);
        me->SetReactState(REACT_AGGRESSIVE);
        if (_instance->GetBossState(DATA_FINALE) == IN_PROGRESS)
            _instance->SetBossState(DATA_FINALE, NOT_STARTED);
        Peaceful(_instance->GetData(DATA_STAGE) >= STAGE_THRONE);
    }

    // unlocked = selectable and talkable (a hostile faction refuses gossip, so she parleys as
    // friendly), but not attackable until the party chooses to fight
    void Peaceful(bool unlocked)
    {
        me->SetImmuneToPC(true);
        me->SetFaction(unlocked ? FACTION_FRIENDLY : me->GetCreatureTemplate()->faction);
        if (unlocked)
        {
            me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
            me->SetNpcFlag(UNIT_NPC_FLAG_GOSSIP);
        }
        else
        {
            me->SetUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
            me->RemoveNpcFlag(UNIT_NPC_FLAG_GOSSIP);
        }
    }

    void DoAction(int32 action) override
    {
        switch (action)
        {
            case ACTION_UNLOCK:
                Peaceful(true);
                break;
            case ACTION_BETRAYED:
                Peaceful(false);
                Talk(SAY_VRA_BETRAYED);
                _events.ScheduleEvent(EVENT_LEAVE, 6s);
                break;
            case ACTION_VRAKATI_RESTORE:
                // Anok'Suten wiped the party: the disguise is back on the throne
                _revealed = false;
                me->SetVisible(true);
                EnterEvadeMode(EVADE_REASON_OTHER);
                break;
            default:
                break;
        }
    }

    bool OnGossipHello(Player* player) override
    {
        if (me->HasUnitFlag(UNIT_FLAG_UNINTERACTIBLE))
            return true;
        if (!_greeted)
        {
            _greeted = true;
            Talk(SAY_VRA_FIRST, player);
        }
        InitGossipMenuFor(player, TEXT_VRAKATI_OFFER);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "We came for you, not your treasure.", GOSSIP_SENDER_MAIN, ACTION_GOSSIP_FIGHT);
        SendGossipMenuFor(player, TEXT_VRAKATI_OFFER, me->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
        CloseGossipMenuFor(player);
        if (action != ACTION_GOSSIP_FIGHT || _instance->GetData(DATA_STAGE) != STAGE_THRONE)
            return true;

        me->RemoveNpcFlag(UNIT_NPC_FLAG_GOSSIP);
        me->SetFaction(me->GetCreatureTemplate()->faction);
        me->SetImmuneToPC(false);
        if (GameObject* offering = _instance->GetGameObject(DATA_OFFERING))
            offering->SetFlag(GO_FLAG_NOT_SELECTABLE);
        AttackStart(player);
        DoZoneInCombat();
        return true;
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        _instance->SetBossState(DATA_FINALE, IN_PROGRESS);
        Talk(SAY_VRA_COMBAT);
        _events.ScheduleEvent(EVENT_BLOOD_BOLT, 3s);
        _events.ScheduleEvent(EVENT_PAIN, 8s);
        _events.ScheduleEvent(EVENT_MARK, 12s);
        _events.ScheduleEvent(EVENT_CHAINS, 18s);
        _events.ScheduleEvent(EVENT_DRAIN, 24s);
        _events.ScheduleEvent(EVENT_FAITHFUL, 30s);
        _events.ScheduleEvent(EVENT_NOVA, 36s);
    }

    void JustSummoned(Creature* summon) override
    {
        _summons.Summon(summon);
        if (summon->GetEntry() == NPC_ZEALOT && summon->IsAIEnabled())
            summon->AI()->DoZoneInCombat();
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*type*/, SpellInfo const* /*spellInfo*/) override
    {
        if (_revealed)
        {
            damage = 0;
            return;
        }
        if (!me->HealthBelowPctDamaged(25, damage))
            return;

        damage = 0;
        _revealed = true;
        _events.Reset();
        me->InterruptNonMeleeSpells(false);
        me->AttackStop();
        me->SetReactState(REACT_PASSIVE);
        me->SetImmuneToPC(true);
        me->GetMotionMaster()->Clear();
        me->StopMoving();
        Talk(SAY_VRA_REVEAL_1);
        _events.ScheduleEvent(EVENT_REVEAL_2, 4s);
        _events.ScheduleEvent(EVENT_REVEAL_3, 8s);
        _events.ScheduleEvent(EVENT_REVEAL_4, 11s);
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        if (_revealed)
            return;     // she is offstage while Anok'Suten fights; ACTION_VRAKATI_RESTORE brings her back
        ScriptedAI::EnterEvadeMode(why);
    }

    void UpdateAI(uint32 diff) override
    {
        _events.Update(diff);

        // the reveal and the betrayal exit run without a victim
        if (_revealed || !me->IsInCombat())
        {
            while (uint32 eventId = _events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_REVEAL_2:
                        Talk(SAY_VRA_REVEAL_2);
                        break;
                    case EVENT_REVEAL_3:
                        Talk(SAY_VRA_REVEAL_3);
                        break;
                    case EVENT_REVEAL_4:
                        me->SummonCreature(NPC_ANOKSUTEN, me->GetPosition(), TEMPSUMMON_MANUAL_DESPAWN);
                        me->SetVisible(false);
                        break;
                    case EVENT_LEAVE:
                        me->DespawnOrUnsummon();
                        break;
                    default:
                        break;
                }
            }
            return;
        }

        if (!UpdateVictim())
            return;
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_BLOOD_BOLT:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                        DoCast(target, SPELL_BLOOD_BOLT);
                    _events.Repeat(5s, 7s);
                    break;
                case EVENT_PAIN:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                        DoCast(target, SPELL_SHADOW_WORD_PAIN, CastSpellExtraArgs(true).AddSpellBP0(100));
                    _events.Repeat(18s);
                    break;
                case EVENT_MARK:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                    {
                        _marked = target->GetGUID();
                        Talk(SAY_VRA_MARK, target);
                        DoCast(target, SPELL_SACRIFICIAL_MARK, true);
                        _events.ScheduleEvent(EVENT_MARK_BURST, 5s);
                    }
                    _events.Repeat(20s);
                    break;
                case EVENT_MARK_BURST:
                    if (Unit* target = ObjectAccessor::GetUnit(*me, _marked))
                        if (target->IsAlive())
                            BBASpawnZone(me, target->GetPosition(), SPELL_SACRIFICIAL_ERUPTION, -1, -1, 0, 3s, 0);
                    break;
                case EVENT_CHAINS:
                    for (uint8 i = 0; i < 2; ++i)
                        if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                            DoCast(target, SPELL_BLOOD_CHAINS, true);
                    _events.Repeat(24s);
                    break;
                case EVENT_DRAIN:
                {
                    Unit* target = ObjectAccessor::GetUnit(*me, _marked);
                    if (!target || !target->IsAlive() || !me->IsWithinDist(target, 20.0f))
                        target = SelectTarget(SelectTargetMethod::Random, 0, 20.0f, true);
                    if (target)
                    {
                        Talk(SAY_VRA_DRAIN);
                        DoCast(target, SPELL_BLOOD_DRAIN);
                    }
                    _events.Repeat(26s);
                    break;
                }
                case EVENT_FAITHFUL:
                    Talk(SAY_VRA_ADDS);
                    for (int8 side : { -1, 1 })
                    {
                        Position pos = me->GetPosition();
                        me->MovePositionToFirstCollision(pos, 5.0f, float(side) * float(M_PI) / 2.0f);
                        me->SummonCreature(NPC_ZEALOT, pos, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 10s);
                    }
                    _events.Repeat(35s);
                    break;
                case EVENT_NOVA:
                    DoCastSelf(SPELL_BLOOD_NOVA);
                    _events.Repeat(30s);
                    break;
                default:
                    break;
            }
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        DoMeleeAttackIfReady();
    }

private:
    InstanceScript* _instance;
    SummonList _summons;
    EventMap _events;
    ObjectGuid _marked;
    bool _revealed = false;
    bool _greeted = false;
};

// ---------------------------------------------------------------------------------------
// Anok'Suten, Mistress of the Blood Web: the true villain.
// ---------------------------------------------------------------------------------------
struct boss_bba_anoksuten : public BossAI
{
    enum Events { EVENT_COCOON = 1, EVENT_SHADOW_WEB, EVENT_SWARM, EVENT_LEGS, EVENT_BLOOD_WEB };

    boss_bba_anoksuten(Creature* creature) : BossAI(creature, DATA_FINALE) { }

    void Reset() override
    {
        // BossAI::_Reset would mark the finale NOT_STARTED; Vraka'ti owns that
        events.Reset();
        summons.DespawnAll();
        _harvested = false;
        _enraged = false;
    }

    void IsSummonedBy(WorldObject* /*summoner*/) override
    {
        Talk(SAY_ANOK_UNMASK);
        DoZoneInCombat();
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        events.ScheduleEvent(EVENT_COCOON, 8s);
        events.ScheduleEvent(EVENT_SHADOW_WEB, 15s);
        events.ScheduleEvent(EVENT_SWARM, 18s);
        events.ScheduleEvent(EVENT_LEGS, 9s);
        events.ScheduleEvent(EVENT_BLOOD_WEB, 28s);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*type*/, SpellInfo const* /*spellInfo*/) override
    {
        if (!_harvested && me->HealthBelowPctDamaged(50, damage))
        {
            // Blood Harvest: an interruptible self-heal
            _harvested = true;
            DoCastSelf(SPELL_BLOOD_HARVEST, CastSpellExtraArgs().AddSpellBP0(int32(me->CountPctFromMaxHealth(8))));
        }
        if (!_enraged && me->HealthBelowPctDamaged(10, damage))
        {
            _enraged = true;
            Talk(SAY_ANOK_ENRAGE);
            DoCastSelf(SPELL_LAST_THREAD, true);
        }
    }

    void ReleaseCocoon(ObjectGuid cocoon)
    {
        auto itr = _cocoons.find(cocoon);
        if (itr == _cocoons.end())
            return;
        if (Player* player = ObjectAccessor::GetPlayer(*me, itr->second))
            player->RemoveAurasDueToSpell(SPELL_WEB_COCOON);
        _cocoons.erase(itr);
    }

    void SummonedCreatureDies(Creature* summon, Unit* killer) override
    {
        BossAI::SummonedCreatureDies(summon, killer);
        ReleaseCocoon(summon->GetGUID());
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        BossAI::SummonedCreatureDespawn(summon);
        ReleaseCocoon(summon->GetGUID());
    }

    void ReleaseAll()
    {
        while (!_cocoons.empty())
            ReleaseCocoon(_cocoons.begin()->first);
    }

    void JustDied(Unit* /*killer*/) override
    {
        Talk(SAY_ANOK_DEATH);
        ReleaseAll();
        _JustDied();
        if (Creature* vrakati = instance->GetCreature(DATA_VRAKATI))
            vrakati->DespawnOrUnsummon();
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        ReleaseAll();
        summons.DespawnAll();
        if (Creature* vrakati = instance->GetCreature(DATA_VRAKATI))
            if (vrakati->IsAIEnabled())
                vrakati->AI()->DoAction(ACTION_VRAKATI_RESTORE);
        me->DespawnOrUnsummon();
    }

    void ExecuteEvent(uint32 eventId) override
    {
        switch (eventId)
        {
            case EVENT_COCOON:
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 1, 40.0f, true))
                {
                    Talk(SAY_ANOK_COCOON);
                    target->CastSpell(target, SPELL_WEB_COCOON, true);
                    if (Creature* cocoon = me->SummonCreature(NPC_WEB_COCOON, target->GetPosition(), TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 20s))
                        _cocoons[cocoon->GetGUID()] = target->GetGUID();
                }
                events.Repeat(20s);
                break;
            case EVENT_SHADOW_WEB:
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                {
                    Talk(SAY_ANOK_WEB);
                    BBASpawnZone(me, target->GetPosition(), SPELL_SHADOW_WEB, -1, -1, 1500, 12s, 6);
                }
                events.Repeat(18s);
                break;
            case EVENT_SWARM:
                Talk(SAY_ANOK_SWARM);
                for (uint8 i = 0; i < 6; ++i)
                {
                    Position pos = me->GetPosition();
                    me->MovePositionToFirstCollision(pos, 6.0f, float(i) * float(M_PI) / 3.0f);
                    if (Creature* spider = me->SummonCreature(NPC_SPIDERLING, pos, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 10s))
                        if (spider->IsAIEnabled())
                            spider->AI()->DoZoneInCombat();
                }
                events.Repeat(24s);
                break;
            case EVENT_LEGS:
                if (roll_chance_i(20))
                    Talk(SAY_ANOK_LEGS);
                DoCastVictim(SPELL_PIERCING_LEGS, true);
                events.Repeat(12s);
                break;
            case EVENT_BLOOD_WEB:
                if (Unit* target = SelectTarget(SelectTargetMethod::Random, 1, 20.0f, true))
                {
                    Talk(SAY_ANOK_BLOOD_WEB);
                    DoCast(target, SPELL_BLOOD_WEB);
                }
                events.Repeat(32s);
                break;
            default:
                break;
        }
    }

private:
    std::map<ObjectGuid, ObjectGuid> _cocoons;  // cocoon -> wrapped player
    bool _harvested = false;
    bool _enraged = false;
};

// Blood Web Cocoon: holds its player; kill it to free them.
struct npc_bba_cocoon : public ScriptedAI
{
    npc_bba_cocoon(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override
    {
        me->SetReactState(REACT_PASSIVE);
        me->SetControlled(true, UNIT_STATE_ROOT);
    }

    void UpdateAI(uint32 /*diff*/) override { }
};

void AddSC_bba_bosses()
{
    RegisterBloodBeneathArenaCreatureAI(boss_bba_zalvaxa);
    RegisterBloodBeneathArenaCreatureAI(npc_bba_risen);
    RegisterBloodBeneathArenaCreatureAI(boss_bba_twin);
    RegisterBloodBeneathArenaCreatureAI(boss_bba_vrakati);
    RegisterBloodBeneathArenaCreatureAI(boss_bba_anoksuten);
    RegisterBloodBeneathArenaCreatureAI(npc_bba_cocoon);
}

void AddSC_instance_blood_beneath_arena();
void AddSC_bba_npcs();
void AddSC_bba_commands();

void AddSC_blood_beneath_arena()
{
    AddSC_instance_blood_beneath_arena();
    AddSC_bba_npcs();
    AddSC_bba_bosses();
    AddSC_bba_commands();
}
