#include <catch2/catch_test_macros.hpp>
#include "Features/NativePBR/MaterialSidecarContract.h"

#include <limits>

TEST_CASE("NativePBR sidecar encodes a full 64-bit registry identity and frame-local draw ordinal", "[NativePBR]")
{
	constexpr uint64_t material = 0xFEDCBA9876543210ull;
	const std::array<NativePBR::SidecarDraw, 3> draws{ { { 1, 0x0000000076543210ull, 1 }, { 2, material, 20 }, { 3, 9, 1 } } };
	REQUIRE(NativePBR::ResolveSidecarMaterial(2, draws) == material);
	REQUIRE(NativePBR::ResolveSidecarMaterial(1, draws) == 0x76543210u);
	REQUIRE(NativePBR::ResolveSidecarMaterial(0, draws) == 0);
	REQUIRE(NativePBR::ResolveSidecarMaterial(4, draws) == 0);
	const std::array<NativePBR::SidecarDraw, 1> mismatched{ { { 31, material, 1 } } };
	REQUIRE(NativePBR::ResolveSidecarMaterial(1, mismatched) == 0);
}

TEST_CASE("NativePBR projected draw keeps its capture-only classification", "[NativePBR]")
{
	const NativePBR::SidecarDraw ordinary{ 1, 42, 1, "ref", "mesh", false };
	const NativePBR::SidecarDraw projected{ 2, 43, 1, "ref", "mesh", true };
	const std::array draws{ ordinary, projected };
	REQUIRE_FALSE(ordinary.projectedUv);
	REQUIRE(projected.projectedUv);
	REQUIRE(NativePBR::ResolveSidecarMaterial(2, draws) == 43);
}

TEST_CASE("NativePBR input-only replay reuses the original draw ordinal", "[NativePBR]")
{
	using namespace NativePBR;
	const std::array<SidecarDraw, 2> draws{ { { 1, 42, 10, "ref", "mesh", false },
		{ 2, 43, 11, "ref", "mesh", false } } };
	REQUIRE(MatchesInputOnlyReplay(3, draws, 43, 11));
	REQUIRE_FALSE(MatchesInputOnlyReplay(2, draws, 43, 11));
	REQUIRE_FALSE(MatchesInputOnlyReplay(3, draws, 42, 11));
	REQUIRE_FALSE(MatchesInputOnlyReplay(3, draws, 43, 10));
	REQUIRE_FALSE(MatchesInputOnlyReplay(1, draws, 43, 11));
	REQUIRE_FALSE(MatchesInputOnlyReplay(3, std::span<const SidecarDraw>{}, 43, 11));
}

TEST_CASE("NativePBR input-only identity survives R16G16 UNORM packing", "[NativePBR]")
{
	using namespace NativePBR;
	const float opaque = static_cast<float>(kInputOnlyOpaqueSignature) / kInputOnlyIdentityScale;
	const float projected = static_cast<float>(kInputOnlyProjectedSignature) / kInputOnlyIdentityScale;
	REQUIRE(DecodeInputOnlyOrdinal(1.0f / kInputOnlyIdentityScale, opaque, false) == 1);
	REQUIRE(DecodeInputOnlyOrdinal(static_cast<float>(kMaximumFrameDraws) / kInputOnlyIdentityScale, projected, true) == kMaximumFrameDraws);
	REQUIRE(DecodeInputOnlyOrdinal(0, opaque, false) == 0);
	REQUIRE(DecodeInputOnlyOrdinal(static_cast<float>(kMaximumFrameDraws + 1) / kInputOnlyIdentityScale, opaque, false) == 0);
	REQUIRE(DecodeInputOnlyOrdinal(1.0f / kInputOnlyIdentityScale, projected, false) == 0);
	REQUIRE(DecodeInputOnlyOrdinal(-0.5f, opaque, false) == 0);
	REQUIRE(DecodeInputOnlyOrdinal(0.5f, std::numeric_limits<float>::quiet_NaN(), false) == 0);
}

TEST_CASE("NativePBR coverage rejects different final depth and invalid depth", "[NativePBR]")
{
	REQUIRE(NativePBR::SidecarDepthMatches(0.25f, 0.25f, false));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.25f, 0.251f, false));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.25f, 0.251f, true));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(-1.0f, 0.0f, true));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.0f, 2.0f, true));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(std::numeric_limits<float>::quiet_NaN(), 0.5f, false));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.5f, std::numeric_limits<float>::infinity(), false));
}

TEST_CASE("NativePBR sidecar adds two projected outputs without using deferred MRTs", "[NativePBR]")
{
	REQUIRE(NativePBR::kSidecarSurfaceAbiVersion == 2u);
	REQUIRE(NativePBR::kBaseSidecarTargetCount == 6u);
	REQUIRE(NativePBR::kSidecarBytesPerPixel.size() == 8u);
	uint32_t bytes = 0;
	for (size_t i = 0; i < NativePBR::kBaseSidecarTargetCount; ++i)
		bytes += NativePBR::kSidecarBytesPerPixel[i];
	REQUIRE(bytes == 64u);
	REQUIRE(bytes + NativePBR::kSidecarBytesPerPixel[6] + NativePBR::kSidecarBytesPerPixel[7] == 96u);
	REQUIRE(static_cast<size_t>(NativePBR::SidecarTarget::IdentityAndColor) == 0);
	REQUIRE(static_cast<size_t>(NativePBR::SidecarTarget::Basis2) == 5);
	REQUIRE(static_cast<size_t>(NativePBR::SidecarTarget::ProjectedDirection) == 7);
}

TEST_CASE("NativePBR ABI v2 packs ordinal and vertex color into two uints", "[NativePBR]")
{
	using namespace NativePBR;
	constexpr SidecarIdentityAndColor identity{ 7, PackSidecarRgba8(0x12, 0x34, 0x56, 0x78) };
	STATIC_REQUIRE(sizeof(SidecarIdentityAndColor) == 8u);
	STATIC_REQUIRE(offsetof(SidecarIdentityAndColor, rgba8) == 4u);
	REQUIRE(identity.ordinal == 7u);
	REQUIRE(identity.rgba8 == 0x78563412u);
	REQUIRE(PackSidecarRgba8(255, 128, 0, 255) == 0xFF0080FFu);
	const SidecarIdentityAndColor uncovered{};
	REQUIRE(uncovered.ordinal == 0u);
	REQUIRE(uncovered.rgba8 == 0u);
}
