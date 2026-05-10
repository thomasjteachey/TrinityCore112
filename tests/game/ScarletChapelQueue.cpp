#include "../tc_catch2.h"

#include <fstream>
#include <string>

namespace
{
std::string ReadFile(std::string const& path)
{
    std::ifstream in(path);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

TEST_CASE("Scarlet Chapel forced queue side is preserved during active battleground refill", "[scarlet][queue]")
{
    std::string const queueSource = ReadFile("src/server/game/Battlegrounds/BattlegroundQueue.cpp");
    std::string const queueHeader = ReadFile("src/server/game/Battlegrounds/BattlegroundQueue.h");
    std::string const npcSource = ReadFile("src/server/scripts/Custom/npc_scarlet_chapel_queue.cpp");

    CHECK(queueHeader.find("IsForcedTeam") != std::string::npos);
    CHECK(queueSource.find("ginfo->IsForcedTeam = true") != std::string::npos);
    CHECK(queueSource.find("if ((*Ali_itr)->IsForcedTeam)") != std::string::npos);
    CHECK(queueSource.find("if ((*Horde_itr)->IsForcedTeam)") != std::string::npos);
    CHECK(npcSource.find("ACTION_QUEUE_ALLIANCE") != std::string::npos);
    CHECK(npcSource.find("ACTION_QUEUE_HORDE") != std::string::npos);
}
