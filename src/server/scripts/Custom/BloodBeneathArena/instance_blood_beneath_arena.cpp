/*
 * Blood Beneath the Arena - instance controller (map 1685).
 *
 * Owns the run's progress (stage, boons, the betrayal), gates the later encounters until the
 * earlier ones are beaten, re-applies earned boons when a player enters and strips them when
 * a player leaves.
 */

#include "blood_beneath_arena.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <sstream>
#include <vector>

namespace
{
ObjectData const creatureData[] =
{
    { NPC_VARJUN,      DATA_VARJUN      },
    { NPC_BLIGHTBLOOD, DATA_BLIGHTBLOOD },
    { NPC_ZALVAXA,     DATA_ZALVAXA_NPC },
    { NPC_THRAXIA,     DATA_THRAXIA     },
    { NPC_MALIZZIA,    DATA_MALIZZIA    },
    { NPC_VRAKATI,     DATA_VRAKATI     },
    { NPC_ANOKSUTEN,   DATA_ANOKSUTEN   },
    { 0,               0                }
};

ObjectData const gameobjectData[] =
{
    { GO_BLOODBOUND_GONG,  DATA_GONG     },
    { GO_VRAKATI_OFFERING, DATA_OFFERING },
    { 0,                   0             }
};

enum BlightbloodTexts
{
    SAY_BLIGHT_AFTER_ZALVAXA    = 9,
    SAY_BLIGHT_AFTER_TWINS      = 10,
    SAY_BLIGHT_FINAL_EXIT       = 11,
    SAY_BLIGHT_BOON_FIRST       = 12    // 12..19, one per boon
};

uint32 const BETRAYAL_GOLD = 50 * 10000;
uint32 const BETRAYAL_EXPEL_DELAY = 4000;
}

class instance_blood_beneath_arena : public InstanceMapScript
{
public:
    instance_blood_beneath_arena() : InstanceMapScript(BBAScriptName, BBA_MAP_ID) { }

    struct instance_blood_beneath_arena_InstanceMapScript : public InstanceScript
    {
        instance_blood_beneath_arena_InstanceMapScript(InstanceMap* map) : InstanceScript(map)
        {
            SetHeaders("BBA");
            SetBossNumber(BBA_ENCOUNTER_COUNT);
            LoadObjectData(creatureData, gameobjectData);
            _stage = STAGE_CAGED;
            _boonsEarned = 0;
            _boonsGranted = 0;
            _expelTimer = 0;
        }

        void OnGameObjectCreate(GameObject* go) override
        {
            InstanceScript::OnGameObjectCreate(go);

            switch (go->GetEntry())
            {
                case GO_APOTHECARY_CAGE:
                    // players must not open the cages themselves; Var'jun's opens by gossip,
                    // the captives' open when the run is over
                    go->SetFlag(GO_FLAG_NOT_SELECTABLE);
                    _cages.push_back(go->GetGUID());
                    if (go->GetExactDist2d(BBAVarjunCagePos.GetPositionX(), BBAVarjunCagePos.GetPositionY()) < 3.0f)
                    {
                        _varjunCage = go->GetGUID();
                        if (_stage >= STAGE_FREED)
                            go->SetGoState(GO_STATE_ACTIVE);
                    }
                    if (_stage == STAGE_COMPLETE)
                        go->SetGoState(GO_STATE_ACTIVE);
                    break;
                case GO_VRAKATI_OFFERING:
                    if (_stage == STAGE_BETRAYED || GetBossState(DATA_FINALE) == DONE)
                        go->SetFlag(GO_FLAG_NOT_SELECTABLE);
                    break;
                default:
                    break;
            }
        }

        void OnPlayerEnter(Player* player) override
        {
            for (uint32 i = 0; i < _boonsGranted && i < BBA_BOON_COUNT; ++i)
                if (!player->HasAura(BBABoonSpells[i]))
                    player->CastSpell(player, BBABoonSpells[i], true);
        }

        void OnPlayerLeave(Player* player) override
        {
            for (uint32 spell : BBABoonSpells)
                player->RemoveAurasDueToSpell(spell);
        }

        uint32 GetData(uint32 type) const override
        {
            switch (type)
            {
                case DATA_STAGE:         return _stage;
                case DATA_BOONS_EARNED:  return _boonsEarned;
                case DATA_BOONS_GRANTED: return _boonsGranted;
                default:
                    return 0;
            }
        }

        void SetData(uint32 type, uint32 data) override
        {
            switch (type)
            {
                case DATA_STAGE:
                    _stage = data;
                    SaveToDB();
                    break;
                case DATA_BOONS_EARNED:
                    _boonsEarned = std::min<uint32>(_boonsEarned + data, BBA_BOON_COUNT);
                    SaveToDB();
                    break;
                case DATA_GRANT_BOON:
                    GrantNextBoon();
                    break;
                case DATA_START_TRIAL:
                    if (Creature* varjun = GetCreature(DATA_VARJUN))
                        if (varjun->IsAIEnabled())
                            varjun->AI()->DoAction(ACTION_START_TRIAL);
                    break;
                case DATA_OFFERING_TAKEN:
                    Betray(data);
                    break;
                default:
                    break;
            }
        }

        bool SetBossState(uint32 type, EncounterState state) override
        {
            if (!InstanceScript::SetBossState(type, state))
                return false;

            if (state != DONE)
                return true;

            switch (type)
            {
                case DATA_SAND_TRIAL:
                case DATA_ICE_TRIAL:
                case DATA_FOREST_TRIAL:
                    if (GetBossState(DATA_SAND_TRIAL) == DONE && GetBossState(DATA_ICE_TRIAL) == DONE && GetBossState(DATA_FOREST_TRIAL) == DONE && _stage < STAGE_RITUAL_DONE)
                        SetData(DATA_STAGE, STAGE_RITUAL_DONE);
                    break;
                case DATA_ZALVAXA:
                    SetData(DATA_BOONS_EARNED, 1);
                    SetData(DATA_STAGE, STAGE_UPPER);
                    for (uint32 twin : { DATA_THRAXIA, DATA_MALIZZIA })
                        if (Creature* c = GetCreature(twin))
                            if (c->IsAIEnabled())
                                c->AI()->DoAction(ACTION_UNLOCK);
                    if (Creature* blight = GetCreature(DATA_BLIGHTBLOOD))
                        if (blight->IsAIEnabled())
                            blight->AI()->Talk(SAY_BLIGHT_AFTER_ZALVAXA);
                    break;
                case DATA_TWINS:
                    SetData(DATA_BOONS_EARNED, 1);
                    SetData(DATA_STAGE, STAGE_THRONE);
                    if (Creature* c = GetCreature(DATA_VRAKATI))
                        if (c->IsAIEnabled())
                            c->AI()->DoAction(ACTION_UNLOCK);
                    if (Creature* blight = GetCreature(DATA_BLIGHTBLOOD))
                        if (blight->IsAIEnabled())
                            blight->AI()->Talk(SAY_BLIGHT_AFTER_TWINS);
                    break;
                case DATA_FINALE:
                    SetData(DATA_STAGE, STAGE_COMPLETE);
                    for (ObjectGuid guid : _cages)
                        if (GameObject* cage = instance->GetGameObject(guid))
                            cage->SetGoState(GO_STATE_ACTIVE);
                    if (GameObject* offering = GetGameObject(DATA_OFFERING))
                        offering->SetFlag(GO_FLAG_NOT_SELECTABLE);
                    for (uint32 npc : { DATA_VARJUN, DATA_BLIGHTBLOOD })
                        if (Creature* c = GetCreature(npc))
                            if (c->IsAIEnabled())
                                c->AI()->DoAction(ACTION_FINALE_DONE);
                    break;
                default:
                    break;
            }
            return true;
        }

        void Update(uint32 diff) override
        {
            if (!_expelTimer)
                return;
            if (_expelTimer > diff)
            {
                _expelTimer -= diff;
                return;
            }
            _expelTimer = 0;

            Map::PlayerList const& players = instance->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                if (Player* player = itr->GetSource())
                    if (!player->IsGameMaster())
                        player->TeleportTo(0, BBAEntrancePos.GetPositionX(), BBAEntrancePos.GetPositionY(), BBAEntrancePos.GetPositionZ(), BBAEntrancePos.GetOrientation());
        }

        void WriteSaveDataMore(std::ostringstream& data) override
        {
            data << _stage << ' ' << _boonsEarned << ' ' << _boonsGranted;
        }

        void ReadSaveDataMore(std::istringstream& data) override
        {
            data >> _stage >> _boonsEarned >> _boonsGranted;
            if (_stage > STAGE_BETRAYED)
                _stage = STAGE_CAGED;
            // a reload mid-escort resumes at the pillar; a reload mid-Zalvaxa lets Var'jun call her again
            if (_stage == STAGE_ESCORT)
                _stage = STAGE_RITUAL;
            if (_stage == STAGE_ZALVAXA)
                _stage = STAGE_RITUAL_DONE;
            _boonsEarned = std::min<uint32>(_boonsEarned, BBA_BOON_COUNT);
            _boonsGranted = std::min(_boonsGranted, _boonsEarned);
        }

    private:
        void GrantNextBoon()
        {
            if (_boonsGranted >= _boonsEarned || _boonsGranted >= BBA_BOON_COUNT)
                return;

            uint32 const spell = BBABoonSpells[_boonsGranted];
            Map::PlayerList const& players = instance->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                if (Player* player = itr->GetSource())
                    player->CastSpell(player, spell, true);

            if (Creature* blight = GetCreature(DATA_BLIGHTBLOOD))
                if (blight->IsAIEnabled())
                    blight->AI()->Talk(SAY_BLIGHT_BOON_FIRST + _boonsGranted);

            ++_boonsGranted;
            SaveToDB();
        }

        // The offering beside the throne: one opener only, 50g, the quest fails for everyone
        // inside, Vraka'ti sends the party away.
        void Betray(uint32 takerLowGuid)
        {
            if (_stage >= STAGE_COMPLETE || GetBossState(DATA_FINALE) != NOT_STARTED || GetBossState(DATA_TWINS) != DONE)
                return;

            SetData(DATA_STAGE, STAGE_BETRAYED);

            Map::PlayerList const& players = instance->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            {
                Player* player = itr->GetSource();
                if (!player)
                    continue;
                if (player->GetGUID().GetCounter() == takerLowGuid)
                    player->ModifyMoney(BETRAYAL_GOLD);
                if (player->GetQuestStatus(BBA_QUEST) == QUEST_STATUS_INCOMPLETE)
                    player->FailQuest(BBA_QUEST);
            }

            if (Creature* vrakati = GetCreature(DATA_VRAKATI))
                if (vrakati->IsAIEnabled())
                    vrakati->AI()->DoAction(ACTION_BETRAYED);
            if (GameObject* offering = GetGameObject(DATA_OFFERING))
                offering->SetFlag(GO_FLAG_NOT_SELECTABLE);

            _expelTimer = BETRAYAL_EXPEL_DELAY;
        }

        uint32 _stage;
        uint32 _boonsEarned;
        uint32 _boonsGranted;
        uint32 _expelTimer;
        ObjectGuid _varjunCage;
        std::vector<ObjectGuid> _cages;
    };

    InstanceScript* GetInstanceScript(InstanceMap* map) const override
    {
        return new instance_blood_beneath_arena_InstanceMapScript(map);
    }
};

void AddSC_instance_blood_beneath_arena()
{
    new instance_blood_beneath_arena();
}
