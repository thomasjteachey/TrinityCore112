#ifndef TRINITY_ABCOMMANDSCRIPT_H
#define TRINITY_ABCOMMANDSCRIPT_H

#include "ChatCommand.h"
#include "ScriptMgr.h"

class AutoBalanceCommandScript : public CommandScript
{
public:
    AutoBalanceCommandScript();

    Trinity::ChatCommands::ChatCommandTable GetCommands() const override;

private:
    static bool HandleSetOffsetCommand(ChatHandler* handler, int32 offset);
    static bool HandleGetOffsetCommand(ChatHandler* handler);
    static bool HandleMapStatsCommand(ChatHandler* handler);
};

#endif // TRINITY_ABCOMMANDSCRIPT_H
