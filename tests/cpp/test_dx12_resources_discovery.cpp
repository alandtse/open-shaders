#include "Features/DX12Resources/CellDiscovery.h"
#include "Features/DX12Resources/PreprocessorParity.h"
#include <catch2/catch_test_macros.hpp>

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
	REQUIRE_FALSE(report.at("identity").at("selectedPluginVerified").get<bool>());
	REQUIRE(report.at("geometryReady") == false);
	REQUIRE(DX12ResourceService::IsDiscoveryOnlyReport(report));
	REQUIRE_FALSE(report.at("preprocessorParityGates").at("parityComplete").get<bool>());
	auto spoofed = report;
	spoofed["renderable"] = true;
	REQUIRE_FALSE(DX12ResourceService::IsDiscoveryOnlyReport(spoofed));
}

TEST_CASE("DX12 resource parity requires completed source and full mesh coverage", "[dx12_resources]")
{
	DX12ResourceService::ParityStatus status;
	REQUIRE_FALSE(status.Complete());
	status.winningRecordsResolved = status.referenceSweepComplete =
		status.nifProvenanceVerified = status.compiledStaticGeometry =
			status.composedTransforms = status.compiledShadingAttributes =
				status.collectedStaticLights = status.outputValidated = true;
	REQUIRE_FALSE(status.Complete());
	status.placedReferences = 3;
	status.compiledShapes = 1;
	status.compiledTriangles = 2;
	REQUIRE(status.Complete());
	status.outputValidated = false;
	REQUIRE_FALSE(status.Complete());
}
