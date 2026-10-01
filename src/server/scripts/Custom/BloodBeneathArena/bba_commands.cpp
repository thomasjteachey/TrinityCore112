/*
 * Blood Beneath the Arena - GM test commands (use inside map 1685).
 *
 *   .bba status         stage, encounter states, boons
 *   .bba stage <n>      jump the run FORWARD to a stage (marks the earlier encounters done
 *                       and credits their boons); 3 = at the pillar, gong armed
 *                       4 = trials done, 6 = Zalvaxa done (twins open), 7 = twins done (throne)
 *   .bba trial          start the next trial now, as if the gong were rung
 *   .bba boons          hand out every earned boon now
 *
 * Going backwards is not supported: reset the instance (.instance unbind) instead.
 */

#include "blood_beneath_arena.h"
#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <algorithm>

using namespace Trinity::ChatCommands;

namespace
{
char const* StageName(uint32 stage)
{
    switch (stage)
    {
        case STAGE_CAGED:       return "Var'jun caged";
        case STAGE_FREED:       return "Var'jun freed";
        case STAGE_ESCORT:      return "escort to the pillar";
        case STAGE_RITUAL:      return "ritual (gong armed)";
        case STAGE_RITUAL_DONE: return "trials done, Zalvaxa not called";
        case STAGE_ZALVAXA:     return "Zalvaxa called";
        case STAGE_UPPER:       return "Zalvaxa dead, twins open";
        case STAGE_THRONE:      return "twins dead, Vraka'ti open";
        case STAGE_COMPLETE:    return "complete";
        case STAGE_BETRAYED:    return "offering taken (betrayed)";
        default:                return "?";
    }
}

char const* StateName(EncounterState state)
{
    switch (state)
    {
        case NOT_STARTED: return "not started";
        case IN_PROGRESS: return "in progress";
        case FAIL:        return "failed";
        case DONE:        return "done";
        case SPECIAL:     return "special";
        default:          return "?";
    }
}

InstanceScript* RunOf(ChatHandler* handler)
{
    Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    if (!player || player->GetMapId() != BBA_MAP_ID || !player->GetInstanceScript())
    {
        handler->SendSysMessage("You must be inside Blood Beneath the Arena (map 1685).");
        handler->SetSentErrorMessage(true);
        return nullptr;
    }
    return player->GetInstanceScript();
}
}

class bba_commandscript : public CommandScript
{
public:
    bba_commandscript() : CommandScript("bba_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable bbaCommandTable =
        {
            { "status", HandleStatus, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "stage",  HandleStage,  rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "trial",  HandleTrial,  rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "boons",  HandleBoons,  rbac::RBAC_PERM_COMMAND_GM, Console::No }
        };
        static ChatCommandTable rootTable =
        {
            { "bba", bbaCommandTable }
        };
        return rootTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        InstanceScript* run = RunOf(handler);
        if (!run)
            return false;
        uint32 const stage = run->GetData(DATA_STAGE);
        handler->PSendSysMessage("Stage %u: %s", stage, StageName(stage));
        static char const* const names[BBA_ENCOUNTER_COUNT] = { "Sand trial", "Ice trial", "Forest trial", "Zalvaxa", "Twins", "Vraka'ti / Anok'Suten" };
        for (uint32 i = 0; i < BBA_ENCOUNTER_COUNT; ++i)
            handler->PSendSysMessage("  %s: %s", names[i], StateName(run->GetBossState(i)));
        handler->PSendSysMessage("Boons: %u earned, %u handed out", run->GetData(DATA_BOONS_EARNED), run->GetData(DATA_BOONS_GRANTED));
        return true;
    }

    static bool HandleStage(ChatHandler* handler, uint32 target)
    {
        InstanceScript* run = RunOf(handler);
        if (!run)
            return false;
        uint32 const stage = run->GetData(DATA_STAGE);
        if (target <= stage || target > STAGE_THRONE || target == STAGE_ESCORT || target == STAGE_ZALVAXA)
        {
            handler->PSendSysMessage("Can only jump forward to 1, 3, 4, 6 or 7 (current stage %u).", stage);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // boons the skipped steps would have earned: 2 per trial, then Zalvaxa and the twins
        // credit themselves through their DONE states
        uint32 const wantBoons = target >= STAGE_RITUAL_DONE ? 6 : 0;
        uint32 const have = run->GetData(DATA_BOONS_EARNED);
        if (wantBoons > have)
            run->SetData(DATA_BOONS_EARNED, wantBoons - have);

        run->SetData(DATA_STAGE, std::min<uint32>(target, STAGE_RITUAL_DONE));
        if (target >= STAGE_RITUAL_DONE)
            for (uint32 trial : { DATA_SAND_TRIAL, DATA_ICE_TRIAL, DATA_FOREST_TRIAL })
                run->SetBossState(trial, DONE);
        if (target >= STAGE_UPPER)
            run->SetBossState(DATA_ZALVAXA, DONE);     // -> stage UPPER, twins unlocked
        if (target >= STAGE_THRONE)
            run->SetBossState(DATA_TWINS, DONE);       // -> stage THRONE, Vraka'ti unlocked

        if (Creature* varjun = run->GetCreature(DATA_VARJUN))
        {
            if (target >= STAGE_RITUAL && varjun->IsAIEnabled())
                varjun->AI()->DoAction(ACTION_RESYNC);
        }
        if (target >= STAGE_UPPER)
            if (Creature* zalvaxa = run->GetCreature(DATA_ZALVAXA_NPC))
                if (zalvaxa->IsAlive())
                    zalvaxa->DespawnOrUnsummon();
        if (target >= STAGE_THRONE)
            for (uint32 twin : { DATA_THRAXIA, DATA_MALIZZIA })
                if (Creature* c = run->GetCreature(twin))
                    if (c->IsAlive())
                        c->DespawnOrUnsummon();

        handler->PSendSysMessage("Run moved to stage %u: %s", run->GetData(DATA_STAGE), StageName(run->GetData(DATA_STAGE)));
        return true;
    }

    static bool HandleTrial(ChatHandler* handler)
    {
        InstanceScript* run = RunOf(handler);
        if (!run)
            return false;
        if (run->GetData(DATA_STAGE) != STAGE_RITUAL)
        {
            handler->SendSysMessage("Trials can only start while the ritual runs (stage 3).");
            handler->SetSentErrorMessage(true);
            return false;
        }
        run->SetData(DATA_START_TRIAL, 0);
        handler->SendSysMessage("Next trial started.");
        return true;
    }

    static bool HandleBoons(ChatHandler* handler)
    {
        InstanceScript* run = RunOf(handler);
        if (!run)
            return false;
        while (run->GetData(DATA_BOONS_GRANTED) < run->GetData(DATA_BOONS_EARNED))
            run->SetData(DATA_GRANT_BOON, 0);
        handler->PSendSysMessage("%u boons handed out.", run->GetData(DATA_BOONS_GRANTED));
        return true;
    }
};

void AddSC_bba_commands()
{
    new bba_commandscript();
}
