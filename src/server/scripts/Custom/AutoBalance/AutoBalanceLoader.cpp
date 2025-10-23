#include "AutoBalance.h"

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
}

void AddAutoBalanceScripts()
{
    new DefaultAutoBalanceModule();
}
