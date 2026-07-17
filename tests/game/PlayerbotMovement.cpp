#include "../tc_catch2.h"

#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadFile(std::string const& path)
{
    std::ifstream in(path);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t CountOccurrences(std::string const& text, std::string const& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++count;
    return count;
}
}

TEST_CASE("Socketless playerbot spline movement keeps MovementInfo synchronized", "[playerbot][movement]")
{
    std::string const source = ReadFile("src/server/game/Entities/Unit/Unit.cpp");
    CHECK(source.find("unit->m_movementInfo.pos.Relocate(x, y, z, orientation);") != std::string::npos);
    CHECK(source.find("unit->m_movementInfo.time = GameTime::GetGameTimeMS();") != std::string::npos);
    CHECK(source.find("SyncSocketlessServerDrivenMovementInfo(this, x, y, z, orientation);") != std::string::npos);
}

TEST_CASE("Socketless playerbot facing preserves active translation", "[playerbot][movement]")
{
    std::string const source = ReadFile("src/server/game/Entities/Unit/Unit.cpp");
    CHECK(source.find("bool HasActiveTranslationalSpline(Unit const* unit)") != std::string::npos);
    CHECK(source.find("unit->movespline->splineflags.parabolic") != std::string::npos);
    CHECK(CountOccurrences(source, "IsSocketlessServerDrivenPlayer(this) && HasActiveTranslationalSpline(this)") >= 2);
}

TEST_CASE("Socketless pure vertical knock-up uses a closed client-timed spline", "[playerbot][movement]")
{
    std::string const source = ReadFile("src/server/game/Movement/MotionMaster.cpp");
    CHECK(source.find("pureVerticalServerKnockup") != std::string::npos);
    CHECK(source.find("Movement::PointsArray const path = { startPoint, arcPoint, startPoint };") != std::string::npos);
    CHECK(source.find("2.f * VerticalKnockupSplineRadius / airTime") != std::string::npos);
}

TEST_CASE("Socketless playerbot equivalent point orders are centrally preserved", "[playerbot][movement]")
{
    std::string const motionMaster = ReadFile("src/server/game/Movement/MotionMaster.cpp");
    std::string const pointGenerator = ReadFile("src/server/game/Movement/MovementGenerators/PointMovementGenerator.h");
    CHECK(motionMaster.find("ShouldPreserveEquivalentPoint") != std::string::npos);
    CHECK(motionMaster.find("PB MotionMaster preserved equivalent Point") != std::string::npos);
    CHECK(pointGenerator.find("GetDestinationX() const") != std::string::npos);
    CHECK(pointGenerator.find("GetFinalOrientation() const") != std::string::npos);
}
