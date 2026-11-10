#include <catch2/catch_test_macros.hpp>
#include "Features/DX12NativeScene/SpatialSubcells.h"

#include <algorithm>
#include <set>
#include <utility>

using namespace DX12Native::SpatialSubcells;

TEST_CASE("Native subcell sizes and negative XY coordinates are stable", "[dx12-native-scene][subcells]")
{
    STATIC_REQUIRE(ValidSize(2048));
    STATIC_REQUIRE(ValidSize(4096));
    STATIC_REQUIRE(ValidSize(16384));
    STATIC_REQUIRE_FALSE(ValidSize(1024));
    STATIC_REQUIRE_FALSE(ValidSize(2560));
    STATIC_REQUIRE_FALSE(ValidSize(17408));

    CHECK((FromPosition(0.0, 0.0, 4096) == Coordinate{ 0, 0 }));
    CHECK((FromPosition(4095.99, -0.01, 4096) == Coordinate{ 0, -1 }));
    CHECK((FromPosition(-4096.0, -4096.01, 4096) == Coordinate{ -1, -2 }));
    CHECK((FromPosition(4096.0, 8192.0, 4096) == Coordinate{ 1, 2 }));
}

TEST_CASE("Recentered active 3x3 reuses overlapping subcells", "[dx12-native-scene][subcells]")
{
    REQUIRE(ReusedActiveCount({0, 0}, {0, 0}) == 9);
    REQUIRE(ReusedActiveCount({0, 0}, {1, 0}) == 6);
    REQUIRE(ReusedActiveCount({0, 0}, {0, -1}) == 6);
    REQUIRE(ReusedActiveCount({0, 0}, {1, -1}) == 4);
    REQUIRE(ReusedActiveCount({0, 0}, {3, 0}) == 0);
}

TEST_CASE("3x3 preparation is followed by twelve sides and four corners", "[dx12-native-scene][subcells]")
{
    const Coordinate center{0, 0};
    const auto order = OrderedWork(center, 1.0, 0.0);
    CHECK((order.active.front() == center));
    std::set<std::pair<int64_t, int64_t>> known;
    for (auto point : order.active) CHECK(known.emplace(point.x, point.y).second);
    for (auto point : order.predictive.sides) CHECK(known.emplace(point.x, point.y).second);
    for (auto point : order.predictive.corners) CHECK(known.emplace(point.x, point.y).second);
    REQUIRE(known.size() == 25);
    CHECK(order.predictive.sides[0].x == 2); // Predict toward the player's motion.

    std::set<std::pair<int64_t, int64_t>> complete;
    const auto ready = [&](Coordinate point) {
        return complete.contains({point.x, point.y});
    };
    auto pending = PendingWork(center, 1.0, 0.0, ready);
    REQUIRE(pending.size() == 9);
    for (auto point : order.active) complete.emplace(point.x, point.y);
    pending = PendingWork(center, 1.0, 0.0, ready);
    REQUIRE(pending.size() == 12);
    for (auto point : order.predictive.sides) complete.emplace(point.x, point.y);
    pending = PendingWork(center, 1.0, 0.0, ready);
    REQUIRE(pending.size() == 4);
    for (auto point : order.predictive.corners) complete.emplace(point.x, point.y);
    CHECK(PendingWork(center, 1.0, 0.0, ready).empty());
}
