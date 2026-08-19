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
    // SetInFront / SetFacingTo / SetFacingToObject all route bots through the
    // shared turn helper, which keeps an active translational spline untouched.
    CHECK(CountOccurrences(source, "TurnSocketlessServerDrivenPlayer(this, ") >= 3);
    CHECK(source.find("bool const visibleTurn = !HasActiveTranslationalSpline(unit) &&") != std::string::npos);
}

TEST_CASE("Socketless playerbot turn-in-place is published to observers", "[playerbot][movement]")
{
    std::string const source = ReadFile("src/server/game/Entities/Unit/Unit.cpp");
    // A stopped bot announces a facing change like a client would (MSG_MOVE_SET_FACING
    // from the live position), never while a spline is still playing.
    std::size_t const broadcastPos = source.find("void BroadcastSocketlessServerDrivenFacing(Unit* unit)");
    REQUIRE(broadcastPos != std::string::npos);
    std::string const body = source.substr(broadcastPos, 2000);
    CHECK(body.find("if (unit->movespline && !unit->movespline->Finalized())") != std::string::npos);
    CHECK(body.find("WorldPacket data(MSG_MOVE_SET_FACING, 64);") != std::string::npos);
    CHECK(body.find("movementInfo.pos.Relocate(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetOrientation());") != std::string::npos);
    CHECK(body.find("MOVEMENTFLAG_SPLINE_ENABLED") != std::string::npos);
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

TEST_CASE("Socketless playerbot state toggles broadcast spline-family opcodes", "[playerbot][movement]")
{
    std::string const source = ReadFile("src/server/game/Entities/Player/Player.cpp");
    CHECK(source.find("bool IsServerDrivenBotSession(Player const* player)") != std::string::npos);
    // Every player-style movement-state toggle must have a bot branch that
    // publishes the GUID-only spline opcode instead of MSG_MOVE_* + MovementInfo.
    CHECK(source.find("SMSG_SPLINE_MOVE_GRAVITY_DISABLE : SMSG_SPLINE_MOVE_GRAVITY_ENABLE") != std::string::npos);
    CHECK(source.find("SMSG_SPLINE_MOVE_SET_FLYING : SMSG_SPLINE_MOVE_UNSET_FLYING") != std::string::npos);
    CHECK(source.find("SMSG_SPLINE_MOVE_SET_HOVER : SMSG_SPLINE_MOVE_UNSET_HOVER") != std::string::npos);
    CHECK(source.find("SMSG_SPLINE_MOVE_START_SWIM : SMSG_SPLINE_MOVE_STOP_SWIM") != std::string::npos);
    CHECK(source.find("SMSG_SPLINE_MOVE_WATER_WALK : SMSG_SPLINE_MOVE_LAND_WALK") != std::string::npos);
    CHECK(source.find("SMSG_SPLINE_MOVE_FEATHER_FALL : SMSG_SPLINE_MOVE_NORMAL_FALL") != std::string::npos);
    // The bot swim relay must no longer use the MovementInfo-carrying opcodes.
    CHECK(source.find("MSG_MOVE_START_SWIM : MSG_MOVE_STOP_SWIM") == std::string::npos);
    CHECK(CountOccurrences(source, "IsServerDrivenBotSession(this)") >= 6);
}

TEST_CASE("Socketless playerbot movers never broadcast MSG_MOVE_HEARTBEAT", "[playerbot][movement]")
{
    std::string const unitSource = ReadFile("src/server/game/Entities/Unit/Unit.cpp");
    std::size_t const guardPos = unitSource.find("void Unit::SendMovementFlagUpdate(bool self /* = false */)");
    REQUIRE(guardPos != std::string::npos);
    std::string const functionBody = unitSource.substr(guardPos, unitSource.find("BuildHeartBeatMsg", guardPos) - guardPos);
    CHECK(functionBody.find("IsSocketlessServerDrivenPlayer(this)") != std::string::npos);

    std::string const lifecycleSource = ReadFile("src/server/scripts/Playerbot/Pvp/PlayerbotPvpLifecycleActions.cpp");
    CHECK(lifecycleSource.find("player->SendMovementFlagUpdate();") == std::string::npos);
}
