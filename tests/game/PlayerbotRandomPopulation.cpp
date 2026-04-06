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

TEST_CASE("Playerbot random population startup bootstrap wiring remains enabled", "[playerbot][population]")
{
    std::string const source = ReadFile("src/server/scripts/Playerbot/playerbot_loader.cpp");
    CHECK(source.find("OnStartupBootstrap") != std::string::npos);
}

TEST_CASE("Playerbot random population rebalance continues to issue login attempts", "[playerbot][population]")
{
    std::string const source = ReadFile("src/server/scripts/Playerbot/Pvp/PlayerbotRandomBotParticipation.cpp");
    CHECK(source.find("state.loginAttempts++") != std::string::npos);
    CHECK(source.find("TryLoginBotCharacter") != std::string::npos);
}


TEST_CASE("Playerbot random population login orchestration support stays enabled", "[playerbot][population]")
{
    std::string const source = ReadFile("src/server/scripts/Playerbot/Pvp/PlayerbotRandomBotParticipation.cpp");
    CHECK(source.find("bool SupportsLoginOrchestration()") != std::string::npos);
    CHECK(source.find("return true;") != std::string::npos);
}

TEST_CASE("Playerbot random population keeps real-player safety gating", "[playerbot][population]")
{
    std::string const source = ReadFile("src/server/scripts/Playerbot/Pvp/PlayerbotRandomBotParticipation.cpp");
    CHECK(source.find("IsManagedRandomBot") != std::string::npos);
    CHECK(source.find("state.skippedSafetyRealPlayers++") != std::string::npos);
}

TEST_CASE("Playerbot battleground queue target remains Warsong", "[playerbot][population]")
{
    std::string const source = ReadFile("src/server/scripts/Playerbot/Pvp/PlayerbotPvpLifecycleActions.cpp");
    CHECK(source.find("ManagedRandomBotQueueTarget") != std::string::npos);
    CHECK(source.find("BATTLEGROUND_WS") != std::string::npos);
}
