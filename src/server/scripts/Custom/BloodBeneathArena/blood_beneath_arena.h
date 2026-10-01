/*
 * Blood Beneath the Arena - a level-60 5-man in the cave under the Gurubashi Arena (map 1685).
 *
 * Flow: free Var'jun -> he walks to the cleansing pillar -> the Bloodbound Gong starts the
 * Sand, Ice and Forest trials (two trash waves, then a champion with a support drummer) ->
 * Var'jun calls out Zalvaxa (interrupt her ritual to wake her) -> the twins Thraxia and
 * Malizzia -> Vraka'ti offers a treasure (taking it fails the quest and expels the party) or
 * is fought, and unmasks at 25% into Anok'Suten. Blightblood hands out a boon for every
 * cleared wave set, champion, Zalvaxa and the twins.
 *
 * Only stock spells are used for abilities (tuned with base-point overrides); the eight boons
 * are custom auras 92100-92107. Data rows: tools/blood_beneath_arena/.
 */

#ifndef BLOOD_BENEATH_ARENA_H
#define BLOOD_BENEATH_ARENA_H

#include "CreatureAIImpl.h"
#include "Duration.h"
#include "Position.h"

class Creature;
class WorldObject;

#define BBAScriptName "instance_blood_beneath_arena"

uint32 const BBA_MAP_ID = 1685;
uint32 const BBA_QUEST  = 60100;

enum BBAEncounters
{
    DATA_SAND_TRIAL             = 0,
    DATA_ICE_TRIAL              = 1,
    DATA_FOREST_TRIAL           = 2,
    DATA_ZALVAXA                = 3,
    DATA_TWINS                  = 4,
    DATA_FINALE                 = 5,
    BBA_ENCOUNTER_COUNT
};

enum BBAData
{
    // SetData / GetData
    DATA_STAGE                  = 100,  // BBAStage
    DATA_BOONS_EARNED           = 101,  // SetData(x, n) adds n
    DATA_BOONS_GRANTED          = 102,
    DATA_GRANT_BOON             = 103,  // SetData only: hand the next boon to every player
    DATA_START_TRIAL            = 104,  // SetData only: the gong was rung
    DATA_OFFERING_TAKEN         = 105,  // SetData(x, low guid of the taker)

    // ObjectData (GetCreature / GetGameObject / GetGuidData)
    DATA_VARJUN                 = 200,
    DATA_BLIGHTBLOOD            = 201,
    DATA_ZALVAXA_NPC            = 202,
    DATA_THRAXIA                = 203,
    DATA_MALIZZIA               = 204,
    DATA_VRAKATI                = 205,
    DATA_ANOKSUTEN              = 206,
    DATA_GONG                   = 207,
    DATA_OFFERING               = 208
};

enum BBAStage
{
    STAGE_CAGED                 = 0,    // Var'jun in his cage
    STAGE_FREED                 = 1,    // cage open, waiting for "lead the way"
    STAGE_ESCORT                = 2,    // walking to the pillar
    STAGE_RITUAL                = 3,    // at the pillar, gong armed
    STAGE_RITUAL_DONE           = 4,    // all three trials beaten
    STAGE_ZALVAXA               = 5,    // Zalvaxa called out
    STAGE_UPPER                 = 6,    // Zalvaxa dead, twins open
    STAGE_THRONE                = 7,    // twins dead, Vraka'ti open
    STAGE_COMPLETE              = 8,    // Anok'Suten dead
    STAGE_BETRAYED              = 9     // the offering was taken
};

enum BBACreatures
{
    NPC_VARJUN                  = 922000,
    NPC_BLIGHTBLOOD             = 922001,
    NPC_CAPTIVE                 = 922004,
    NPC_TORTURED_CAPTIVE        = 922005,

    NPC_ZULKASH                 = 922010,
    NPC_RAKAJIN                 = 922011,
    NPC_SAND_BERSERKER          = 922012,
    NPC_SAND_HEXER              = 922013,
    NPC_SAND_MEDIC              = 922014,
    NPC_ROKTHUL                 = 922020,
    NPC_YEKRA                   = 922021,
    NPC_ICE_BERSERKER           = 922022,
    NPC_ICE_HEXER               = 922023,
    NPC_ICE_MEDIC               = 922024,
    NPC_MOKRA                   = 922030,
    NPC_KEZANI                  = 922031,
    NPC_FOREST_BERSERKER        = 922032,
    NPC_FOREST_HEXER            = 922033,
    NPC_FOREST_MEDIC            = 922034,

    NPC_CUTTHROAT               = 922040,
    NPC_HEXXER                  = 922041,
    NPC_ORACLE                  = 922042,
    NPC_HEADHUNTER              = 922043,
    NPC_WITCH                   = 922044,
    NPC_CATLORD                 = 922045,
    NPC_SHADOWCASTER            = 922046,

    NPC_ZALVAXA                 = 922050,
    NPC_RISEN_BLOODBOUND        = 922051,
    NPC_THRAXIA                 = 922060,
    NPC_MALIZZIA                = 922061,
    NPC_VRAKATI                 = 922070,
    NPC_ANOKSUTEN               = 922071,
    NPC_ZEALOT                  = 922072,
    NPC_SPIDERLING              = 922073,
    NPC_WEB_COCOON              = 922074,
    NPC_BBA_TRIGGER             = 922080
};

enum BBAGameObjects
{
    GO_APOTHECARY_CAGE          = 188362,
    GO_BLOODBOUND_GONG          = 900300,
    GO_VRAKATI_OFFERING         = 900302
};

enum BBABoons
{
    SPELL_BOON_DUNE             = 92100,
    SPELL_BOON_BURIED_FANG      = 92101,
    SPELL_BOON_FROZEN_BREATH    = 92102,
    SPELL_BOON_FROSTBOUND       = 92103,
    SPELL_BOON_LIVING_ROOT      = 92104,
    SPELL_BOON_THORNED_SOUL     = 92105,
    SPELL_BOON_DEFIANCE         = 92106,
    SPELL_BOON_UNBOUND          = 92107,
    BBA_BOON_COUNT              = 8
};

uint32 const BBABoonSpells[BBA_BOON_COUNT] =
{
    SPELL_BOON_DUNE, SPELL_BOON_BURIED_FANG, SPELL_BOON_FROZEN_BREATH, SPELL_BOON_FROSTBOUND,
    SPELL_BOON_LIVING_ROOT, SPELL_BOON_THORNED_SOUL, SPELL_BOON_DEFIANCE, SPELL_BOON_UNBOUND
};

// Actions sent between the scripts (DoAction)
enum BBAActions
{
    ACTION_START_TRIAL          = 1,    // -> Var'jun
    ACTION_TRIAL_WIPE           = 2,    // trial mob -> Var'jun
    ACTION_ZALVAXA_CALLED       = 3,    // Var'jun -> Zalvaxa
    ACTION_UNLOCK               = 4,    // instance -> twins / Vraka'ti
    ACTION_SUPPORT_HEAL_MODE    = 5,    // champion -> support
    ACTION_FINALE_DONE          = 6,    // instance -> Var'jun / Blightblood
    ACTION_TWIN_FELL            = 7,    // twin -> twin
    ACTION_TWIN_DIED            = 8,    // twin -> twin
    ACTION_BETRAYED             = 9,    // instance -> Vraka'ti
    ACTION_RESYNC               = 10    // .bba stage -> Var'jun: re-place for the current stage
};

// Fixed points (the mock-up the customer built on map 0; map 1685 shares the coordinates)
Position const BBAVarjunCagePos     = { -13232.52f, 201.61f, -67.00f, 3.55f };
Position const BBABlightMeetPos     = { -13252.80f, 262.70f, -79.50f, 3.65f };
Position const BBAPillarPos         = { -13224.50f, 268.60f, -93.00f, 1.60f };
Position const BBAExitPos           = { -13239.00f, 199.50f, -67.00f, 0.60f };
Position const BBABlightExitPos     = { -13236.00f, 210.50f, -67.00f, 4.70f };
Position const BBAZalvaxaMiddlePos  = { -13160.00f, 297.50f, -79.00f, 2.00f };
Position const BBAVrakatiPos        = { -13218.90f, 285.30f, -58.20f, 0.22f };
Position const BBAEntrancePos       = { -13277.40f, 127.372f, 26.1418f, 4.25f };   // map 0

Position const BBARisenSpawns[6] =
{
    { -13164.8f, 295.6f, -80.9f, 3.66f }, { -13162.5f, 298.9f, -79.4f, 3.39f },
    { -13160.0f, 295.0f, -79.1f, 3.61f }, { -13157.4f, 298.4f, -77.2f, 3.68f },
    { -13158.2f, 301.7f, -76.8f, 3.90f }, { -13154.8f, 296.2f, -76.4f, 3.70f }
};

struct BBATrial
{
    uint32 BossData;
    uint32 Champion;
    uint32 Support;
    uint32 Berserker;
    uint32 Hexer;
    uint32 Medic;
    Position ChampionPos;
    Position SupportPos;
    Position WavePos[4];
    uint8 VarjunDoneText;   // Var'jun's line when the trial is beaten
};

BBATrial const BBATrials[3] =
{
    { DATA_SAND_TRIAL, NPC_ZULKASH, NPC_RAKAJIN, NPC_SAND_BERSERKER, NPC_SAND_HEXER, NPC_SAND_MEDIC,
      { -13157.9f, 265.3f, -78.6f, 3.18f }, { -13157.4f, 276.3f, -78.6f, 3.11f },
      { { -13166.1f, 272.9f, -84.3f, 3.12f }, { -13165.8f, 268.2f, -83.9f, 3.04f },
        { -13163.0f, 263.0f, -82.0f, 3.10f }, { -13163.0f, 277.0f, -82.0f, 3.10f } }, 7 },
    { DATA_ICE_TRIAL, NPC_ROKTHUL, NPC_YEKRA, NPC_ICE_BERSERKER, NPC_ICE_HEXER, NPC_ICE_MEDIC,
      { -13227.8f, 315.2f, -78.5f, 5.36f }, { -13236.9f, 307.9f, -78.5f, 5.36f },
      { { -13227.4f, 305.3f, -84.2f, 5.36f }, { -13231.1f, 305.6f, -83.0f, 5.36f },
        { -13227.4f, 308.9f, -82.8f, 5.35f }, { -13233.0f, 309.0f, -81.0f, 5.35f } }, 8 },
    { DATA_FOREST_TRIAL, NPC_MOKRA, NPC_KEZANI, NPC_FOREST_BERSERKER, NPC_FOREST_HEXER, NPC_FOREST_MEDIC,
      { -13177.9f, 311.6f, -78.5f, 4.12f }, { -13183.4f, 309.7f, -82.7f, 4.24f },
      { { -13187.6f, 308.0f, -84.5f, 4.26f }, { -13188.3f, 311.9f, -83.0f, 4.10f },
        { -13188.9f, 318.0f, -78.6f, 4.18f }, { -13182.0f, 315.0f, -80.0f, 4.20f } }, 9 }
};

inline BBATrial const* BBAGetTrialByChampion(uint32 entry)
{
    for (BBATrial const& t : BBATrials)
        if (t.Champion == entry)
            return &t;
    return nullptr;
}

inline BBATrial const* BBAGetTrialBySupport(uint32 entry)
{
    for (BBATrial const& t : BBATrials)
        if (t.Support == entry)
            return &t;
    return nullptr;
}

// Summons the invisible ground-effect trigger at pos; it casts spellId (bp0, and bp1 when
// >= 0) after delayMs, lives for lifetime, and pulses Dazed within dazeRange yards if non-zero.
Creature* BBASpawnZone(WorldObject* summoner, Position const& pos, uint32 spellId, int32 bp0, int32 bp1, uint32 delayMs, Milliseconds lifetime, uint32 dazeRange);

template <class AI, class T>
inline AI* GetBloodBeneathArenaAI(T* obj)
{
    return GetInstanceAI<AI>(obj, BBAScriptName);
}

#define RegisterBloodBeneathArenaCreatureAI(ai_name) RegisterCreatureAIWithFactory(ai_name, GetBloodBeneathArenaAI)
#define RegisterBloodBeneathArenaGameObjectAI(ai_name) RegisterGameObjectAIWithFactory(ai_name, GetBloodBeneathArenaAI)

#endif
