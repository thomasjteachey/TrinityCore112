#include "AutoBalance.h"
#include "AutoBalance/AutoBalanceCreature.h"
#include "ScriptMgr.h"

namespace
{
    class DefaultAutoBalanceModule final : public ABModuleScript
    {
    public:
        DefaultAutoBalanceModule()
            : ABModuleScript("DefaultAutoBalanceModule")
        {
        }

        void OnMessage(AutoBalance::Message const& /*message*/) override { }
    };

    class AutoBalanceCreatureUpdateScript final : public AllCreatureScript
    {
    public:
        AutoBalanceCreatureUpdateScript()
            : AllCreatureScript("AutoBalanceCreatureUpdateScript")
        {
        }

        void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
        {
            AutoBalance::ScaleCreature(creature);
        }
    };
}

extern void AddAutoBalanceCommandScripts();

void AddAutoBalanceScripts()
{
    new DefaultAutoBalanceModule();
    new AutoBalanceCreatureUpdateScript();
    AddAutoBalanceCommandScripts();
}
