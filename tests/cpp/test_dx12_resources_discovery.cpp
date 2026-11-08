#include <catch2/catch_test_macros.hpp>
#include "Features/DX12Resources/CellDiscovery.h"

TEST_CASE("DX12 Resources marks main-menu CELL discovery as non-renderable", "[dx12_resources]")
{
    DX12ResourceService::CellDiscovery discovered;
    discovered.requested = { "Skyrim.esm", 0x123 };
    discovered.resolvedFormId = 0x00000123;
    discovered.cellName = "TestInterior";
    discovered.isInterior = true;
    const auto report = DX12ResourceService::BuildDiscoveryReport(discovered);
    REQUIRE(report.at("identity").at("localFormId") == 0x123);
    REQUIRE(report.at("discoveryComplete") == true);
    REQUIRE(report.at("geometryReady") == false);
    REQUIRE_FALSE(DX12ResourceService::IsRenderableCacheReport(report));
    auto spoofed = report;
    spoofed["renderable"] = true;
    REQUIRE_FALSE(DX12ResourceService::IsRenderableCacheReport(spoofed));
}
