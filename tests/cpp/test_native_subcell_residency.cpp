#include <catch2/catch_test_macros.hpp>
#include "Features/DX12NativeScene/SubcellResidency.h"

using namespace DX12Native::SubcellResidency;

TEST_CASE("Native subcell residency is revision and generation scoped", "[dx12-native-scene][residency]")
{
    Ledger ledger;
    ledger.ResetWorld(10);
    ledger.Recenter({0, 0});
    REQUIRE(ledger.StartPrepare({0, 0}, 10));
    auto generation = ledger.Get({0, 0}).generation;
    CHECK_FALSE(ledger.PublishDisk({0, 0}, 9, generation, "digest"));
    CHECK(ledger.PublishDisk({0, 0}, 10, generation, "digest"));
    CHECK(ledger.PublishGpu({0, 0}, 10, generation, 128));
    CHECK(ledger.MarkBLAS({0, 0}, 10, generation));
    CHECK(ledger.MarkTLAS({0, 0}, 10, generation));
    CHECK(ledger.TrackedGPUBytes() == 128);
    CHECK_FALSE(ledger.RetireGPU({0, 0}));
    ledger.Recenter({2, 0});
    CHECK(ledger.RetireGPU({0, 0}));
    CHECK(ledger.Get({0, 0}).state == State::GPUEvicted);
    CHECK(ledger.Get({0, 0}).digest == "digest");
    CHECK(ledger.TrackedGPUBytes() == 0);
    ledger.Invalidate({0, 0});
    CHECK_FALSE(ledger.PublishGpu({0, 0}, 10, generation, 64));
    ledger.ResetWorld(11);
    CHECK(ledger.TrackedTiles() == 0);
}

TEST_CASE("Subcell disk cache persists while active membership slides", "[dx12-native-scene][residency]")
{
    Ledger ledger;
    ledger.ResetWorld(1);
    ledger.Recenter({0, 0});
    for(auto p : DX12Native::SpatialSubcells::Active3x3({0,0})) {
        REQUIRE(ledger.StartPrepare(p, 1));
        REQUIRE(ledger.PublishDisk(p, 1, ledger.Get(p).generation, "cached"));
    }
    CHECK(ledger.ActiveReady() == 9);
    ledger.Recenter({1, 0});
    CHECK(ledger.ActiveReady() == 6);
    CHECK(ledger.Get({-1, 0}).digest == "cached");
    CHECK_FALSE(ledger.Get({-1, 0}).active);
}
