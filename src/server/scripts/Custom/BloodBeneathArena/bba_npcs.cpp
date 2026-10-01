/*
 * Blood Beneath the Arena - Var'jun (escort + trial controller), Blightblood (boons), the
 * tribal trials (trash waves, champions, support drummers), shared adds, the ground-effect
 * trigger, and the two interactive objects (Bloodbound Gong, Vraka'ti's offering).
 */

#include "blood_beneath_arena.h"
#include "Creature.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GossipDef.h"
#include "InstanceScript.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "Random.h"
#include <algorithm>
#include <list>

namespace
{
enum BBACommonSpells
{
    SPELL_CLEANSING_CHANNEL     = 12508,    // Water Channeling (visual)
    SPELL_ENRAGE                = 8599,     // trash berserkers at 30%
    // named abilities (custom rows, bba_ability_spells.py)
    SPELL_RITUAL_RHYTHM         = 92110,    // drummer -> champion, 8 sec
    SPELL_BLOOD_FRENZY          = 92111,
    SPELL_EVOCATION             = 92112,    // Desperate Evocation, cannot be kicked
    SPELL_SAND_SLASH            = 92113,
    SPELL_BURROWING_AMBUSH      = 92114,
    SPELL_SANDSTORM             = 92115,
    SPELL_BLINDING_SAND         = 92116,
    SPELL_SAND_TRAP             = 92117,
    SPELL_ICEBLOOD_STRIKE       = 92118,
    SPELL_FROZEN_GROUND         = 92120,
    SPELL_ICE_SPEAR             = 92121,
    SPELL_THORNED_STRIKE        = 92122,
    SPELL_GRASPING_VINES        = 92123,
    SPELL_VENOMOUS_BREATH       = 92124,
    SPELL_SPORE_BURST           = 92125,
    // stock
    SPELL_HEALING_WAVE          = 15982,
    SPELL_HEALING_TOUCH         = 23381,
    SPELL_MEDIC_HEAL            = 12491,
    SPELL_SHADOW_BOLT_SMALL     = 9613,
    SPELL_FROSTBOLT             = 15497,
    SPELL_CURSE_OF_AGONY        = 18266,
    SPELL_SHADOW_WORD_PAIN      = 15654,
    SPELL_CLEAVE                = 15496,
    SPELL_FROST_NOVA            = 15531,
    SPELL_ENTANGLING_ROOTS      = 22127,
    SPELL_DAZED                 = 1604
};

enum BBAGossipTexts
{
    TEXT_VARJUN_CAGED           = 60100,
    TEXT_VARJUN_STORY           = 60101,
    TEXT_BLIGHT_HELLO           = 60102,
    TEXT_BLIGHT_CHAINS          = 60103,
    TEXT_VARJUN_RITUAL          = 60107,
    TEXT_VARJUN_RITUAL_DONE     = 60108,
    TEXT_VARJUN_FREED           = 60109,
    TEXT_BLIGHT_BOON            = 60110,
    TEXT_VARJUN_UPPER           = 60111,
    TEXT_VARJUN_END             = 60112
};

enum VarjunTexts
{
    SAY_VARJUN_FREED            = 0,
    SAY_VARJUN_QUEST            = 1,
    SAY_VARJUN_AFTER_PATROLS    = 2,
    SAY_VARJUN_MEET_BLIGHT      = 3,
    SAY_VARJUN_REPLY_BLIGHT     = 4,
    SAY_VARJUN_BEFORE_RITUAL    = 5,
    SAY_VARJUN_GONG             = 6,
    SAY_VARJUN_ZALVAXA_TAUNT    = 10,
    SAY_VARJUN_EXIT             = 12
};

enum BlightTexts
{
    SAY_BLIGHT_FIRST            = 0,
    SAY_BLIGHT_GREET            = 1,
    SAY_BLIGHT_REPLY            = 2,
    SAY_BLIGHT_EXPLAIN          = 3,
    SAY_BLIGHT_FINAL            = 11
};

enum TrialTexts
{
    SAY_CHAMPION_PULL           = 0,
    SAY_CHAMPION_FRENZY         = 1,
    SAY_CHAMPION_DEATH          = 2,
    SAY_SUPPORT_DRUMS           = 0,
    SAY_SUPPORT_HEAL            = 1,
    SAY_SUPPORT_EVOCATION       = 2
};

enum BBAGossipActions
{
    ACTION_GOSSIP_OPEN_CAGE     = GOSSIP_ACTION_INFO_DEF + 1,
    ACTION_GOSSIP_STORY         = GOSSIP_ACTION_INFO_DEF + 2,
    ACTION_GOSSIP_LEAD          = GOSSIP_ACTION_INFO_DEF + 3,
    ACTION_GOSSIP_CALL_ZALVAXA  = GOSSIP_ACTION_INFO_DEF + 4,
    ACTION_GOSSIP_BOON          = GOSSIP_ACTION_INFO_DEF + 5,
    ACTION_GOSSIP_CHAINS        = GOSSIP_ACTION_INFO_DEF + 6
};

enum BBAPoints
{
    POINT_BLIGHT                = 1,
    POINT_PILLAR                = 2
};

// npc_bba_zone SetData keys
enum BBAZoneData
{
    ZONE_SPELL                  = 1,
    ZONE_BP0                    = 2,    // value + 1, so 0 means "leave the DBC value"
    ZONE_BP1                    = 3,    // value + 1, so 0 means "leave the DBC value"
    ZONE_DELAY                  = 4,
    ZONE_DAZE                   = 5,    // pulse Dazed on players within this many yards
    ACTION_ZONE_GO              = 100
};

void NotifyTrialWipe(Creature* me)
{
    if (!me->IsSummon())
        return;
    if (Unit* summoner = me->ToTempSummon()->GetSummonerUnit())
        if (Creature* varjun = summoner->ToCreature())
            if (varjun->GetEntry() == NPC_VARJUN && varjun->IsAIEnabled())
                varjun->AI()->DoAction(ACTION_TRIAL_WIPE);
}
}

// Summons a ground-effect trigger at pos, casting spellId (with base points) after delayMs.
// Used by every boss for zones, traps and delayed explosions.
Creature* BBASpawnZone(WorldObject* summoner, Position const& pos, uint32 spellId, int32 bp0, int32 bp1, uint32 delayMs, Milliseconds lifetime, uint32 dazeRange)
{
    Creature* trigger = summoner->SummonCreature(NPC_BBA_TRIGGER, pos, TEMPSUMMON_TIMED_DESPAWN, lifetime);
    if (!trigger || !trigger->IsAIEnabled())
        return trigger;
    trigger->AI()->SetData(ZONE_SPELL, spellId);
    trigger->AI()->SetData(ZONE_BP0, bp0 >= 0 ? uint32(bp0 + 1) : 0);
    trigger->AI()->SetData(ZONE_BP1, bp1 >= 0 ? uint32(bp1 + 1) : 0);
    trigger->AI()->SetData(ZONE_DELAY, delayMs);
    trigger->AI()->SetData(ZONE_DAZE, dazeRange);
    trigger->AI()->DoAction(ACTION_ZONE_GO);
    return trigger;
}

// ---------------------------------------------------------------------------------------
// Var'jun: quest giver, escort, and the controller of the three tribal trials.
// ---------------------------------------------------------------------------------------
struct npc_bba_varjun : public ScriptedAI
{
    npc_bba_varjun(Creature* creature) : ScriptedAI(creature), _instance(creature->GetInstanceScript()), _summons(me) { }

    enum Events
    {
        EVENT_MOVE_PILLAR       = 1,
        EVENT_ESCORT_TIMEOUT,
        EVENT_START_CHANNEL,
        EVENT_NEXT_WAVE,
        EVENT_CHAMPION,
        EVENT_CALL_ZALVAXA,
        EVENT_GO_TO_EXIT,
        EVENT_SAY_BASE          = 100,  // + Var'jun text group
        EVENT_BLIGHT_SAY_BASE   = 200   // + Blightblood text group
    };

    void JustAppeared() override
    {
        ScriptedAI::JustAppeared();
        Resync();
    }

    // place Var'jun where the run's stage says he should be (spawn, reload, .bba stage)
    void Resync()
    {
        uint32 const stage = _instance->GetData(DATA_STAGE);
        if (stage >= STAGE_FREED)
            if (GameObject* cage = me->FindNearestGameObject(GO_APOTHECARY_CAGE, 4.0f))
                cage->SetGoState(GO_STATE_ACTIVE);
        if (stage >= STAGE_RITUAL && stage < STAGE_COMPLETE)
        {
            me->GetMotionMaster()->Clear();
            me->NearTeleportTo(BBAPillarPos);
            _events.ScheduleEvent(EVENT_START_CHANNEL, 2s);
        }
        else if (stage >= STAGE_COMPLETE)
            me->NearTeleportTo(BBAExitPos);
    }

    void Reset() override
    {
        _summons.DespawnAll();
        _trial = -1;
    }

    bool OnGossipHello(Player* player) override
    {
        uint32 const stage = _instance->GetData(DATA_STAGE);
        InitGossipMenuFor(player, TEXT_VARJUN_CAGED);
        if (me->IsQuestGiver())
            player->PrepareQuestMenu(me->GetGUID());

        uint32 text = TEXT_VARJUN_UPPER;
        switch (stage)
        {
            case STAGE_CAGED:
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Open the cage.", GOSSIP_SENDER_MAIN, ACTION_GOSSIP_OPEN_CAGE);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Who are you?", GOSSIP_SENDER_MAIN, ACTION_GOSSIP_STORY);
                text = TEXT_VARJUN_CAGED;
                break;
            case STAGE_FREED:
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "The way is clear. Lead on, Var'jun.", GOSSIP_SENDER_MAIN, ACTION_GOSSIP_LEAD);
                text = TEXT_VARJUN_FREED;
                break;
            case STAGE_ESCORT:
                text = TEXT_VARJUN_FREED;
                break;
            case STAGE_RITUAL:
                text = TEXT_VARJUN_RITUAL;
                break;
            case STAGE_RITUAL_DONE:
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Call out Zalvaxa.", GOSSIP_SENDER_MAIN, ACTION_GOSSIP_CALL_ZALVAXA);
                text = TEXT_VARJUN_RITUAL_DONE;
                break;
            case STAGE_COMPLETE:
            case STAGE_BETRAYED:
                text = TEXT_VARJUN_END;
                break;
            default:
                break;
        }
        SendGossipMenuFor(player, text, me->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
        ClearGossipMenuFor(player);
        uint32 const stage = _instance->GetData(DATA_STAGE);

        switch (action)
        {
            case ACTION_GOSSIP_STORY:
                SendGossipMenuFor(player, TEXT_VARJUN_STORY, me->GetGUID());
                return true;
            case ACTION_GOSSIP_OPEN_CAGE:
                CloseGossipMenuFor(player);
                if (stage != STAGE_CAGED)
                    break;
                if (GameObject* cage = me->FindNearestGameObject(GO_APOTHECARY_CAGE, 4.0f))
                    cage->SetGoState(GO_STATE_ACTIVE);
                _instance->SetData(DATA_STAGE, STAGE_FREED);
                Talk(SAY_VARJUN_FREED);
                _events.ScheduleEvent(uint32(EVENT_SAY_BASE) + SAY_VARJUN_QUEST, 5s);
                break;
            case ACTION_GOSSIP_LEAD:
                CloseGossipMenuFor(player);
                if (stage != STAGE_FREED)
                    break;
                _instance->SetData(DATA_STAGE, STAGE_ESCORT);
                Talk(SAY_VARJUN_AFTER_PATROLS);
                me->SetWalk(false);
                me->GetMotionMaster()->MovePoint(POINT_BLIGHT, BBABlightMeetPos);
                _events.ScheduleEvent(EVENT_ESCORT_TIMEOUT, 40s);
                break;
            case ACTION_GOSSIP_CALL_ZALVAXA:
                CloseGossipMenuFor(player);
                if (stage != STAGE_RITUAL_DONE)
                    break;
                _instance->SetData(DATA_STAGE, STAGE_ZALVAXA);
                me->InterruptNonMeleeSpells(false);
                Talk(SAY_VARJUN_ZALVAXA_TAUNT);
                _events.ScheduleEvent(EVENT_CALL_ZALVAXA, 4s);
                break;
            default:
                CloseGossipMenuFor(player);
                break;
        }
        return true;
    }

    void MovementInform(uint32 type, uint32 id) override
    {
        if (type != POINT_MOTION_TYPE)
            return;

        if (id == POINT_BLIGHT)
        {
            _events.CancelEvent(EVENT_ESCORT_TIMEOUT);
            if (Creature* blight = _instance->GetCreature(DATA_BLIGHTBLOOD))
                me->SetFacingToObject(blight);
            Talk(SAY_VARJUN_MEET_BLIGHT);
            _events.ScheduleEvent(uint32(EVENT_BLIGHT_SAY_BASE) + SAY_BLIGHT_REPLY, 4s);
            _events.ScheduleEvent(uint32(EVENT_BLIGHT_SAY_BASE) + SAY_BLIGHT_GREET, 8s);
            _events.ScheduleEvent(uint32(EVENT_SAY_BASE) + SAY_VARJUN_REPLY_BLIGHT, 12s);
            _events.ScheduleEvent(uint32(EVENT_BLIGHT_SAY_BASE) + SAY_BLIGHT_EXPLAIN, 16s);
            _events.ScheduleEvent(EVENT_MOVE_PILLAR, 22s);
        }
        else if (id == POINT_PILLAR)
            ArriveAtPillar();
    }

    void ArriveAtPillar()
    {
        _events.CancelEvent(EVENT_ESCORT_TIMEOUT);
        if (_instance->GetData(DATA_STAGE) == STAGE_ESCORT)
            _instance->SetData(DATA_STAGE, STAGE_RITUAL);
        Talk(SAY_VARJUN_BEFORE_RITUAL);
        _events.ScheduleEvent(uint32(EVENT_SAY_BASE) + SAY_VARJUN_GONG, 7s);
        _events.ScheduleEvent(EVENT_START_CHANNEL, 9s);
    }

    void DoAction(int32 action) override
    {
        switch (action)
        {
            case ACTION_START_TRIAL:
                StartTrial();
                break;
            case ACTION_TRIAL_WIPE:
                if (_trial < 0)
                    break;
                _events.CancelEvent(EVENT_NEXT_WAVE);
                _events.CancelEvent(EVENT_CHAMPION);
                _summons.DespawnAll();
                if (_instance->GetBossState(BBATrials[_trial].BossData) != DONE)
                    _instance->SetBossState(BBATrials[_trial].BossData, NOT_STARTED);
                _trial = -1;
                break;
            case ACTION_FINALE_DONE:
                _events.ScheduleEvent(EVENT_GO_TO_EXIT, 6s);
                break;
            case ACTION_RESYNC:
                Resync();
                break;
            default:
                break;
        }
    }

    void StartTrial()
    {
        if (_trial >= 0 || _instance->GetData(DATA_STAGE) != STAGE_RITUAL)
            return;
        for (int8 i = 0; i < 3; ++i)
        {
            if (_instance->GetBossState(BBATrials[i].BossData) == DONE)
                continue;
            _trial = i;
            _wave = 1;
            _instance->SetBossState(BBATrials[i].BossData, SPECIAL);
            SpawnWave();
            return;
        }
    }

    void SpawnWave()
    {
        BBATrial const& t = BBATrials[_trial];
        _alive = 0;
        uint32 const wave1[3] = { t.Berserker, t.Hexer, t.Medic };
        uint32 const wave2[4] = { t.Berserker, t.Berserker, t.Hexer, t.Medic };
        uint32 const* entries = _wave == 1 ? wave1 : wave2;
        uint8 const count = _wave == 1 ? 3 : 4;
        for (uint8 i = 0; i < count; ++i)
            if (me->SummonCreature(entries[i], t.WavePos[i], TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60s))
                ++_alive;
    }

    void JustSummoned(Creature* summon) override
    {
        _summons.Summon(summon);
        if (summon->IsAIEnabled())
            summon->AI()->DoZoneInCombat();
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        _summons.Despawn(summon);
    }

    void SummonedCreatureDies(Creature* summon, Unit* /*killer*/) override
    {
        if (_trial < 0)
            return;
        BBATrial const& t = BBATrials[_trial];

        if (summon->GetEntry() == t.Champion)
        {
            _instance->SetData(DATA_BOONS_EARNED, 1);
            if (Creature* support = me->FindNearestCreature(t.Support, 200.0f))
                support->DespawnOrUnsummon(3s);
            _events.ScheduleEvent(uint32(EVENT_SAY_BASE) + t.VarjunDoneText, 4s);
            _trial = -1;
            return;
        }
        if (summon->GetEntry() == t.Support)
            return;

        if (_alive && --_alive == 0)
        {
            if (_wave == 1)
            {
                _wave = 2;
                _events.ScheduleEvent(EVENT_NEXT_WAVE, 6s);
            }
            else if (_wave == 2)
            {
                _wave = 3;
                _instance->SetData(DATA_BOONS_EARNED, 1);
                _events.ScheduleEvent(EVENT_CHAMPION, 8s);
            }
        }
    }

    void UpdateAI(uint32 diff) override
    {
        _events.Update(diff);
        while (uint32 eventId = _events.ExecuteEvent())
            HandleEvent(eventId);
    }

    void HandleEvent(uint32 eventId)
    {
        if (eventId >= EVENT_BLIGHT_SAY_BASE)
        {
            if (Creature* blight = _instance->GetCreature(DATA_BLIGHTBLOOD))
                if (blight->IsAIEnabled())
                    blight->AI()->Talk(uint8(eventId - EVENT_BLIGHT_SAY_BASE));
            return;
        }
        if (eventId >= EVENT_SAY_BASE)
        {
            Talk(uint8(eventId - EVENT_SAY_BASE));
            return;
        }

        switch (eventId)
        {
            case EVENT_MOVE_PILLAR:
                me->GetMotionMaster()->MovePoint(POINT_PILLAR, BBAPillarPos);
                _events.ScheduleEvent(EVENT_ESCORT_TIMEOUT, 40s);
                break;
            case EVENT_ESCORT_TIMEOUT:
                // pathing failed somewhere in the cave: finish the walk by teleport
                me->GetMotionMaster()->Clear();
                me->NearTeleportTo(BBAPillarPos);
                ArriveAtPillar();
                break;
            case EVENT_START_CHANNEL:
                me->SetFacingTo(BBAPillarPos.GetOrientation());
                if (_instance->GetData(DATA_STAGE) < STAGE_ZALVAXA)
                    DoCastSelf(SPELL_CLEANSING_CHANNEL);
                break;
            case EVENT_NEXT_WAVE:
                if (_trial >= 0)
                    SpawnWave();
                break;
            case EVENT_CHAMPION:
                if (_trial >= 0)
                {
                    BBATrial const& t = BBATrials[_trial];
                    me->SummonCreature(t.Support, t.SupportPos, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 60s);
                    me->SummonCreature(t.Champion, t.ChampionPos, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 300s);
                }
                break;
            case EVENT_CALL_ZALVAXA:
                if (Creature* zalvaxa = _instance->GetCreature(DATA_ZALVAXA_NPC))
                    if (zalvaxa->IsAIEnabled())
                        zalvaxa->AI()->DoAction(ACTION_ZALVAXA_CALLED);
                break;
            case EVENT_GO_TO_EXIT:
                me->InterruptNonMeleeSpells(false);
                me->NearTeleportTo(BBAExitPos);
                Talk(SAY_VARJUN_EXIT);
                break;
            default:
                break;
        }
    }

private:
    InstanceScript* _instance;
    SummonList _summons;
    EventMap _events;
    int8 _trial = -1;
    uint8 _wave = 0;
    uint8 _alive = 0;
};

// ---------------------------------------------------------------------------------------
// Blightblood: the unclaimed sacrifice. Hands out the boons the party has earned.
// ---------------------------------------------------------------------------------------
struct npc_bba_blightblood : public ScriptedAI
{
    npc_bba_blightblood(Creature* creature) : ScriptedAI(creature), _instance(creature->GetInstanceScript()) { }

    void JustAppeared() override
    {
        ScriptedAI::JustAppeared();
        if (_instance->GetData(DATA_STAGE) >= STAGE_COMPLETE && _instance->GetData(DATA_STAGE) != STAGE_BETRAYED)
            me->NearTeleportTo(BBABlightExitPos);
    }

    bool OnGossipHello(Player* player) override
    {
        if (!_greeted)
        {
            _greeted = true;
            Talk(SAY_BLIGHT_FIRST, player);
        }
        InitGossipMenuFor(player, TEXT_BLIGHT_HELLO);
        bool const boonReady = _instance->GetData(DATA_BOONS_GRANTED) < _instance->GetData(DATA_BOONS_EARNED);
        if (boonReady)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Take back what the Bloodbound stole.", GOSSIP_SENDER_MAIN, ACTION_GOSSIP_BOON);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Why were you in chains?", GOSSIP_SENDER_MAIN, ACTION_GOSSIP_CHAINS);
        SendGossipMenuFor(player, boonReady ? TEXT_BLIGHT_BOON : TEXT_BLIGHT_HELLO, me->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
        ClearGossipMenuFor(player);
        if (action == ACTION_GOSSIP_CHAINS)
        {
            SendGossipMenuFor(player, TEXT_BLIGHT_CHAINS, me->GetGUID());
            return true;
        }
        CloseGossipMenuFor(player);
        if (action == ACTION_GOSSIP_BOON)
            _instance->SetData(DATA_GRANT_BOON, 0);
        return true;
    }

    void DoAction(int32 action) override
    {
        if (action == ACTION_FINALE_DONE)
            _events.ScheduleEvent(1, 8s);
    }

    void UpdateAI(uint32 diff) override
    {
        _events.Update(diff);
        if (_events.ExecuteEvent() == 1)
        {
            me->NearTeleportTo(BBABlightExitPos);
            Talk(SAY_BLIGHT_FINAL);
        }
    }

private:
    InstanceScript* _instance;
    EventMap _events;
    bool _greeted = false;
};

// ---------------------------------------------------------------------------------------
// Trash: wave mobs, patrols, Vraka'ti's zealots and Anok'Suten's spiderlings.
// ---------------------------------------------------------------------------------------
struct npc_bba_trash : public ScriptedAI
{
    enum Role { ROLE_BERSERKER, ROLE_HEXER, ROLE_MEDIC, ROLE_SWARM };
    enum Events { EVENT_PRIMARY = 1, EVENT_SECONDARY };

    npc_bba_trash(Creature* creature) : ScriptedAI(creature)
    {
        switch (me->GetEntry())
        {
            case NPC_SAND_HEXER: case NPC_ICE_HEXER: case NPC_FOREST_HEXER:
            case NPC_HEXXER: case NPC_WITCH: case NPC_SHADOWCASTER:
                _role = ROLE_HEXER; break;
            case NPC_SAND_MEDIC: case NPC_ICE_MEDIC: case NPC_FOREST_MEDIC: case NPC_ORACLE:
                _role = ROLE_MEDIC; break;
            case NPC_SPIDERLING:
                _role = ROLE_SWARM; break;
            default:
                _role = ROLE_BERSERKER; break;
        }
    }

    void Reset() override
    {
        _events.Reset();
        _enraged = false;
    }

    void IsSummonedBy(WorldObject* /*summoner*/) override
    {
        DoZoneInCombat();
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        switch (_role)
        {
            case ROLE_BERSERKER:
                _events.ScheduleEvent(EVENT_PRIMARY, 6s, 9s);
                break;
            case ROLE_HEXER:
                _events.ScheduleEvent(EVENT_PRIMARY, 1s, 3s);
                _events.ScheduleEvent(EVENT_SECONDARY, 8s, 12s);
                break;
            case ROLE_MEDIC:
                _events.ScheduleEvent(EVENT_PRIMARY, 5s, 7s);
                _events.ScheduleEvent(EVENT_SECONDARY, 3s, 6s);
                break;
            default:
                break;
        }
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        NotifyTrialWipe(me);
        ScriptedAI::EnterEvadeMode(why);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*type*/, SpellInfo const* /*spellInfo*/) override
    {
        if (_role == ROLE_BERSERKER && !_enraged && me->HealthBelowPctDamaged(30, damage))
        {
            _enraged = true;
            DoCastSelf(SPELL_ENRAGE, true);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (_role)
            {
                case ROLE_BERSERKER:
                    DoCastVictim(SPELL_CLEAVE, CastSpellExtraArgs().AddSpellBP0(250));
                    _events.Repeat(8s, 11s);
                    break;
                case ROLE_HEXER:
                    if (eventId == EVENT_PRIMARY)
                    {
                        bool const ice = me->GetEntry() == NPC_ICE_HEXER;
                        DoCastVictim(ice ? SPELL_FROSTBOLT : SPELL_SHADOW_BOLT_SMALL, CastSpellExtraArgs().AddSpellBP0(ice ? 280 : 300));
                        _events.Repeat(4s, 6s);
                    }
                    else
                    {
                        if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                            DoCast(target, SPELL_CURSE_OF_AGONY, CastSpellExtraArgs().AddSpellBP0(60));
                        _events.Repeat(15s, 20s);
                    }
                    break;
                case ROLE_MEDIC:
                    if (eventId == EVENT_PRIMARY)
                    {
                        if (Unit* hurt = DoSelectLowestHpFriendly(40.0f, 1500))
                            DoCast(hurt, SPELL_MEDIC_HEAL, CastSpellExtraArgs().AddSpellBP0(1200));
                        _events.Repeat(7s, 9s);
                    }
                    else
                    {
                        if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 30.0f, true))
                            DoCast(target, SPELL_SHADOW_WORD_PAIN, CastSpellExtraArgs().AddSpellBP0(60));
                        _events.Repeat(12s, 16s);
                    }
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
    EventMap _events;
    Role _role;
    bool _enraged = false;
};

// ---------------------------------------------------------------------------------------
// Tribal champions (Zul'kash, Rok'thul, Mok'ra). The support drummer buffs them until 30%,
// then heals; Blood Frenzy at 30% raises their damage for the rest of the fight.
// ---------------------------------------------------------------------------------------
struct npc_bba_champion : public BossAI
{
    enum Events
    {
        EVENT_STRIKE = 1, EVENT_AMBUSH, EVENT_SANDSTORM, EVENT_SANDSTORM_PULSE, EVENT_BLIND, EVENT_SAND_TRAP,
        EVENT_FROSTBOLT, EVENT_NOVA, EVENT_FROZEN_GROUND, EVENT_ICE_SPEAR,
        EVENT_ROOTS, EVENT_VINES, EVENT_BREATH, EVENT_SPORES
    };

    npc_bba_champion(Creature* creature) : BossAI(creature, BBAGetTrialByChampion(creature->GetEntry()) ? BBAGetTrialByChampion(creature->GetEntry())->BossData : DATA_SAND_TRIAL)
    {
        _trial = BBAGetTrialByChampion(creature->GetEntry());
    }

    void Reset() override
    {
        _Reset();
        _frenzy = false;
        _pulses = 0;
        me->SetObjectScale(me->GetCreatureTemplate()->scale);
    }

    void IsSummonedBy(WorldObject* /*summoner*/) override
    {
        DoZoneInCombat();
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_CHAMPION_PULL);
        switch (me->GetEntry())
        {
            case NPC_ZULKASH:
                events.ScheduleEvent(EVENT_STRIKE, 9s);
                events.ScheduleEvent(EVENT_AMBUSH, 22s);
                events.ScheduleEvent(EVENT_SANDSTORM, 26s);
                events.ScheduleEvent(EVENT_BLIND, 30s);
                events.ScheduleEvent(EVENT_SAND_TRAP, 18s);
                break;
            case NPC_ROKTHUL:
                events.ScheduleEvent(EVENT_STRIKE, 9s);
                events.ScheduleEvent(EVENT_FROSTBOLT, 6s);
                events.ScheduleEvent(EVENT_NOVA, 24s);
                events.ScheduleEvent(EVENT_FROZEN_GROUND, 20s);
                events.ScheduleEvent(EVENT_ICE_SPEAR, 28s);
                break;
            case NPC_MOKRA:
                events.ScheduleEvent(EVENT_STRIKE, 9s);
                events.ScheduleEvent(EVENT_ROOTS, 22s);
                events.ScheduleEvent(EVENT_VINES, 24s);
                events.ScheduleEvent(EVENT_BREATH, 18s);
                events.ScheduleEvent(EVENT_SPORES, 30s);
                break;
            default:
                break;
        }
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*type*/, SpellInfo const* /*spellInfo*/) override
    {
        if (!_frenzy && me->HealthBelowPctDamaged(30, damage))
        {
            _frenzy = true;
            Talk(SAY_CHAMPION_FRENZY);
            DoCastSelf(SPELL_BLOOD_FRENZY, true);
            me->SetObjectScale(me->GetCreatureTemplate()->scale * 1.15f);
            if (_trial)
                if (Creature* support = me->FindNearestCreature(_trial->Support, 200.0f))
                    if (support->IsAIEnabled())
                        support->AI()->DoAction(ACTION_SUPPORT_HEAL_MODE);
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        _JustDied();
        Talk(SAY_CHAMPION_DEATH);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        NotifyTrialWipe(me);
    }

    Unit* RandomTarget(bool notTank)
    {
        return SelectTarget(SelectTargetMethod::Random, notTank ? 1 : 0, 50.0f, true);
    }

    void ExecuteEvent(uint32 eventId) override
    {
        switch (eventId)
        {
            case EVENT_STRIKE:
                if (me->GetEntry() == NPC_ROKTHUL)
                    DoCastVictim(SPELL_ICEBLOOD_STRIKE, true);
                else
                    DoCastVictim(me->GetEntry() == NPC_MOKRA ? SPELL_THORNED_STRIKE : SPELL_SAND_SLASH, true);
                events.Repeat(9s);
                break;
            case EVENT_AMBUSH:
                if (Unit* target = RandomTarget(true))
                {
                    me->NearTeleportTo(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), target->GetOrientation());
                    DoCast(target, SPELL_BURROWING_AMBUSH, true);
                }
                events.Repeat(22s);
                break;
            case EVENT_SANDSTORM:
                _pulses = 4;
                events.ScheduleEvent(EVENT_SANDSTORM_PULSE, 0s);
                events.Repeat(26s);
                break;
            case EVENT_SANDSTORM_PULSE:
                DoCastSelf(SPELL_SANDSTORM, true);
                if (--_pulses)
                    events.Repeat(1s);
                break;
            case EVENT_BLIND:
                if (Unit* target = RandomTarget(true))
                    DoCast(target, SPELL_BLINDING_SAND, true);
                events.Repeat(30s);
                break;
            case EVENT_SAND_TRAP:
                if (Unit* target = RandomTarget(false))
                    BBASpawnZone(me, target->GetPosition(), SPELL_SAND_TRAP, -1, -1, 1500, 4s, 0);
                events.Repeat(18s);
                break;
            case EVENT_FROSTBOLT:
                if (Unit* target = RandomTarget(false))
                    DoCast(target, SPELL_FROSTBOLT, CastSpellExtraArgs().AddSpellBP0(320));
                events.Repeat(6s);
                break;
            case EVENT_NOVA:
                DoCastSelf(SPELL_FROST_NOVA, CastSpellExtraArgs(true).AddSpellBP0(220));
                events.Repeat(24s);
                break;
            case EVENT_FROZEN_GROUND:
                if (Unit* target = RandomTarget(false))
                    BBASpawnZone(me, target->GetPosition(), SPELL_FROZEN_GROUND, -1, -1, 1500, 13s, 0);
                events.Repeat(20s);
                break;
            case EVENT_ICE_SPEAR:
                if (Unit* target = SelectTarget(SelectTargetMethod::MaxDistance, 0, 50.0f, true))
                    DoCast(target, SPELL_ICE_SPEAR);
                events.Repeat(28s);
                break;
            case EVENT_ROOTS:
                if (Unit* target = RandomTarget(true))
                    DoCast(target, SPELL_ENTANGLING_ROOTS, CastSpellExtraArgs().AddSpellBP1(260 / 3));
                events.Repeat(22s);
                break;
            case EVENT_VINES:
                if (Unit* target = RandomTarget(false))
                    BBASpawnZone(me, target->GetPosition(), SPELL_GRASPING_VINES, -1, -1, 2000, 13s, 0);
                events.Repeat(24s);
                break;
            case EVENT_BREATH:
                DoCastVictim(SPELL_VENOMOUS_BREATH, true);
                events.Repeat(18s);
                break;
            case EVENT_SPORES:
                if (Unit* target = RandomTarget(false))
                    BBASpawnZone(me, target->GetPosition(), SPELL_SPORE_BURST, -1, -1, 1500, 4s, 0);
                events.Repeat(30s);
                break;
            default:
                break;
        }
    }

private:
    BBATrial const* _trial = nullptr;
    bool _frenzy = false;
    uint8 _pulses = 0;
};

// ---------------------------------------------------------------------------------------
// Support drummers (Raka'jin, Yekra, Kez'ani).
// ---------------------------------------------------------------------------------------
struct npc_bba_support : public ScriptedAI
{
    enum Events { EVENT_RHYTHM = 1, EVENT_BOLT, EVENT_HEAL, EVENT_EVOCATION_END };

    npc_bba_support(Creature* creature) : ScriptedAI(creature)
    {
        _trial = BBAGetTrialBySupport(creature->GetEntry());
    }

    void Reset() override
    {
        _events.Reset();
        _healMode = false;
        _evocating = false;
        me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
    }

    void IsSummonedBy(WorldObject* /*summoner*/) override
    {
        me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
        DoZoneInCombat();
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        Talk(SAY_SUPPORT_DRUMS);
        _events.ScheduleEvent(EVENT_RHYTHM, 4s);
        _events.ScheduleEvent(EVENT_BOLT, 2s);
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        NotifyTrialWipe(me);
        ScriptedAI::EnterEvadeMode(why);
    }

    Creature* Champion() const
    {
        return _trial ? me->FindNearestCreature(_trial->Champion, 200.0f) : nullptr;
    }

    bool Controlled() const
    {
        return me->HasUnitState(UNIT_STATE_CONTROLLED) || me->HasAuraType(SPELL_AURA_MOD_SILENCE) || me->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE);
    }

    void DoAction(int32 action) override
    {
        if (action != ACTION_SUPPORT_HEAL_MODE || _healMode)
            return;
        _healMode = true;
        Talk(SAY_SUPPORT_HEAL);
        _events.CancelEvent(EVENT_RHYTHM);
        _events.ScheduleEvent(EVENT_HEAL, 2s);
    }

    void OnSpellCast(SpellInfo const* spell) override
    {
        // every completed heal costs a fifth of the drummer's mana; Evocation is the burn window
        if (spell->Id == SPELL_HEALING_WAVE || spell->Id == SPELL_HEALING_TOUCH)
        {
            int32 const cost = int32(me->GetMaxPower(POWER_MANA) / 5);
            me->SetPower(POWER_MANA, std::max<int32>(0, int32(me->GetPower(POWER_MANA)) - cost));
        }
    }

    void SetEvocationImmunity(bool apply)
    {
        me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_INTERRUPT_CAST, apply);
        for (Mechanics m : { MECHANIC_SILENCE, MECHANIC_STUN, MECHANIC_FEAR, MECHANIC_POLYMORPH, MECHANIC_KNOCKOUT, MECHANIC_HORROR, MECHANIC_SAPPED, MECHANIC_DISORIENTED, MECHANIC_SLEEP })
            me->ApplySpellImmune(0, IMMUNITY_MECHANIC, m, apply);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        _events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_RHYTHM:
                    if (Controlled())
                    {
                        _events.Repeat(2s);
                        break;
                    }
                    if (Creature* champion = Champion())
                    {
                        if (roll_chance_i(33))
                            Talk(SAY_SUPPORT_DRUMS);
                        DoCast(champion, SPELL_RITUAL_RHYTHM, true);
                    }
                    _events.Repeat(12s);
                    break;
                case EVENT_BOLT:
                    if (!_healMode)
                        DoCastVictim(SPELL_SHADOW_BOLT_SMALL, CastSpellExtraArgs().AddSpellBP0(200));
                    _events.Repeat(6s, 8s);
                    break;
                case EVENT_HEAL:
                {
                    Creature* champion = Champion();
                    if (!champion || !champion->IsAlive())
                        break;
                    if (_evocating)
                    {
                        _events.Repeat(1s);
                        break;
                    }
                    if (me->GetPower(POWER_MANA) < me->GetMaxPower(POWER_MANA) / 5)
                    {
                        _evocating = true;
                        Talk(SAY_SUPPORT_EVOCATION);
                        SetEvocationImmunity(true);
                        DoCastSelf(SPELL_EVOCATION, CastSpellExtraArgs(true).AddSpellBP0(int32(me->GetMaxPower(POWER_MANA) * 5 / 100)));
                        _events.ScheduleEvent(EVENT_EVOCATION_END, 8500ms);
                        _events.Repeat(9s);
                        break;
                    }
                    if (champion->GetHealthPct() < 100.0f)
                    {
                        uint32 const spell = _trial && _trial->BossData == DATA_FOREST_TRIAL ? SPELL_HEALING_TOUCH : SPELL_HEALING_WAVE;
                        DoCast(champion, spell, CastSpellExtraArgs().AddSpellBP0(int32(champion->CountPctFromMaxHealth(3))));
                    }
                    _events.Repeat(7s);
                    break;
                }
                case EVENT_EVOCATION_END:
                    _evocating = false;
                    SetEvocationImmunity(false);
                    break;
                default:
                    break;
            }
            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        if (!_evocating)
            DoMeleeAttackIfReady();
    }

private:
    BBATrial const* _trial = nullptr;
    EventMap _events;
    bool _healMode = false;
    bool _evocating = false;
};

// ---------------------------------------------------------------------------------------
// Ground-effect trigger: casts one spell at its own position after a delay; optionally
// pulses Dazed on players standing in it (Anok'Suten's Shadow Web).
// ---------------------------------------------------------------------------------------
struct npc_bba_zone : public ScriptedAI
{
    enum Events { EVENT_FIRE = 1, EVENT_DAZE };

    npc_bba_zone(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override
    {
        me->SetReactState(REACT_PASSIVE);
    }

    void SetData(uint32 id, uint32 value) override
    {
        switch (id)
        {
            case ZONE_SPELL: _spell = value; break;
            case ZONE_BP0:   _bp0 = int32(value) - 1; _hasBp0 = value != 0; break;
            case ZONE_BP1:   _bp1 = int32(value) - 1; _hasBp1 = value != 0; break;
            case ZONE_DELAY: _delay = value; break;
            case ZONE_DAZE:  _daze = value; break;
            default: break;
        }
    }

    void DoAction(int32 action) override
    {
        if (action != ACTION_ZONE_GO)
            return;
        _events.ScheduleEvent(EVENT_FIRE, Milliseconds(_delay));
        if (_daze)
            _events.ScheduleEvent(EVENT_DAZE, Milliseconds(_delay) + 1s);
    }

    void UpdateAI(uint32 diff) override
    {
        _events.Update(diff);
        while (uint32 eventId = _events.ExecuteEvent())
        {
            if (eventId == EVENT_FIRE && _spell)
            {
                CastSpellExtraArgs args(true);
                if (_hasBp0)
                    args.AddSpellBP0(_bp0);
                if (_hasBp1)
                    args.AddSpellBP1(_bp1);
                me->CastSpell(me->GetPosition(), _spell, args);
            }
            else if (eventId == EVENT_DAZE)
            {
                std::list<Player*> players;
                me->GetPlayerListInGrid(players, float(_daze));
                for (Player* player : players)
                    if (player->IsAlive() && !player->IsGameMaster())
                        me->CastSpell(player, SPELL_DAZED, true);
                _events.Repeat(1s);
            }
        }
    }

private:
    EventMap _events;
    uint32 _spell = 0;
    int32 _bp0 = 0;
    bool _hasBp0 = false;
    int32 _bp1 = 0;
    bool _hasBp1 = false;
    uint32 _delay = 0;
    uint32 _daze = 0;
};

// ---------------------------------------------------------------------------------------
// Bloodbound Gong: rings in the next tribal trial while the ritual is running.
// ---------------------------------------------------------------------------------------
struct go_bba_gong : public GameObjectAI
{
    go_bba_gong(GameObject* go) : GameObjectAI(go) { }

    bool OnGossipHello(Player* /*player*/) override
    {
        InstanceScript* instance = me->GetInstanceScript();
        if (!instance || instance->GetData(DATA_STAGE) != STAGE_RITUAL)
            return true;
        instance->SetData(DATA_START_TRIAL, 0);
        return false;
    }
};

// ---------------------------------------------------------------------------------------
// Vraka'ti's offering: the betrayal branch (50g, quest failed, party expelled).
// ---------------------------------------------------------------------------------------
struct go_bba_offering : public GameObjectAI
{
    go_bba_offering(GameObject* go) : GameObjectAI(go) { }

    bool OnGossipHello(Player* player) override
    {
        if (InstanceScript* instance = me->GetInstanceScript())
            instance->SetData(DATA_OFFERING_TAKEN, player->GetGUID().GetCounter());
        return true;
    }
};

void AddSC_bba_npcs()
{
    RegisterBloodBeneathArenaCreatureAI(npc_bba_varjun);
    RegisterBloodBeneathArenaCreatureAI(npc_bba_blightblood);
    RegisterBloodBeneathArenaCreatureAI(npc_bba_trash);
    RegisterBloodBeneathArenaCreatureAI(npc_bba_champion);
    RegisterBloodBeneathArenaCreatureAI(npc_bba_support);
    RegisterBloodBeneathArenaCreatureAI(npc_bba_zone);
    RegisterBloodBeneathArenaGameObjectAI(go_bba_gong);
    RegisterBloodBeneathArenaGameObjectAI(go_bba_offering);
}
