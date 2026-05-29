#include "ScriptMgr.h"
#include "Battleground.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "MotionMaster.h"
#include "Player.h"
#include "SharedDefines.h"

namespace
{
enum LPlusJumpPadEntries
{
    GO_LPLUS_TRAMPOLINE_1 = 300003,
    GO_LPLUS_TRAMPOLINE_2 = 300004,
    GO_LPLUS_TRAMPOLINE_3 = 300005,
    GO_LPLUS_TRAMPOLINE_4 = 300006
};

struct JumpTarget
{
    uint32 MapId;
    float X;
    float Y;
    float Z;
    float O;
};

bool GetJumpTarget(uint32 entry, JumpTarget& target)
{
    switch (entry)
    {
        case GO_LPLUS_TRAMPOLINE_1:
            target = { 1230, 1356.110352f, -796.668884f, -85.145966f, 3.086610f };
            return true;
        case GO_LPLUS_TRAMPOLINE_2:
            target = { 1230, 1356.436646f, -750.763672f, -85.144066f, 3.157297f };
            return true;
        case GO_LPLUS_TRAMPOLINE_3:
            target = { 1230, 1405.581299f, -750.040527f, -85.144279f, 6.271400f };
            return true;
        case GO_LPLUS_TRAMPOLINE_4:
            target = { 1230, 1406.165527f, -795.367432f, -85.146095f, 0.058874f };
            return true;
        default:
            return false;
    }
}
}

class go_lplus_jump_pad : public GameObjectScript
{
public:
    go_lplus_jump_pad() : GameObjectScript("go_lplus_jump_pad") { }

    struct go_lplus_jump_padAI : public GameObjectAI
    {
        explicit go_lplus_jump_padAI(GameObject* go) : GameObjectAI(go) { }

        bool OnGossipHello(Player* player) override
        {
            if (!player || !player->IsAlive())
                return true;

            Battleground* bg = player->GetBattleground();
            if (!bg || bg->GetTypeID(true) != BATTLEGROUND_BRT || bg->GetStatus() != STATUS_IN_PROGRESS)
                return true;

            JumpTarget target;
            if (!GetJumpTarget(me->GetEntry(), target))
                return true;

            if (player->GetMapId() != target.MapId)
                return true;

            player->GetMotionMaster()->MoveJump(target.X, target.Y, target.Z, target.O, 10.0f, 12.0f, EVENT_JUMP, true);
            return true;
        }
    };

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new go_lplus_jump_padAI(go);
    }
};

void AddSC_go_lplus_jump_pad()
{
    new go_lplus_jump_pad();
}
