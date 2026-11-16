#include "Features/NativePBR/MaterialRouting.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("NativePBR renderer ownership is independent of DX12 initialization", "[native_pbr]")
{
	REQUIRE(NativePBR::SelectBackend(true, true, false) == NativePBR::Backend::NativeDX12);
	REQUIRE(NativePBR::SelectBackend(false, true, false) == NativePBR::Backend::LegacyDX11);
	REQUIRE(NativePBR::SelectBackend(true, false, false) == NativePBR::Backend::LegacyDX11);
	REQUIRE(NativePBR::SelectBackend(true, true, true) == NativePBR::Backend::LegacyDX11);
	NativePBR::RoutingInputs input;
	input.backend = NativePBR::Backend::NativeDX12;
	input.nativeFeatureEnabled = true;
	input.featureLoaded = true;
	input.scope = NativePBR::MaterialScope::OpaqueStaticMesh;
	const auto unavailable = NativePBR::ResolveRouting(input);
	REQUIRE(unavailable.owner == NativePBR::RenderingOwner::NativeUnavailable);

	input.sceneOwnsNative = true;
	const auto active = NativePBR::ResolveRouting(input);
	REQUIRE(active.owner == NativePBR::RenderingOwner::Native);

	input.materialInputsAvailable = false;
	input.publishedNativeMaterialGeneration = 0;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::Native);
	input.materialInputsAvailable = true;
	input.publishedNativeMaterialGeneration = 7;
	REQUIRE(NativePBR::ResolveRouting(input).publishedNativeMaterialGeneration == 7);

	input.legacyComparisonRequested = true;
	const auto comparison = NativePBR::ResolveRouting(input);
	REQUIRE(comparison.owner == NativePBR::RenderingOwner::Native);
	REQUIRE(comparison.legacyComparisonRequested);

	input.legacyComparisonRequested = false;
	input.scope = NativePBR::MaterialScope::ProjectedStaticMesh;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::Native);
	input.scope = NativePBR::MaterialScope::Unmigrated;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::NativeUnavailable);
	input.sceneOwnsNative = false;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::NativeUnavailable);
	input.sceneOwnsNative = true;
	input.scope = NativePBR::MaterialScope::OpaqueStaticMesh;
	input.nativeFeatureEnabled = false;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::NativeUnavailable);
	input.backend = NativePBR::Backend::LegacyDX11;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::Legacy);
	REQUIRE(NativePBR::ResolveRouting(input).backend == NativePBR::Backend::LegacyDX11);
}
