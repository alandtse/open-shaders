#include "Features/NativePBR/SurfaceFrameContract.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("NativePBR ABI-v3 transport preserves the ABI-v2 sidecar inputs", "[NativePBR][surface]")
{
	using namespace NativePBR;
	CHECK(kSurfaceSourceAbiVersion == 2);
	CHECK(kSurfaceFrameAbiVersion == 3);
	CHECK(kSurfaceFrameAttachmentCount == 8);
	CHECK(kSurfaceFrameBaseAttachments == 6);
	CHECK(kSurfaceTransportAttachmentCount == 9);
	CHECK(kSurfaceTransportBaseAttachments == 7);
	CHECK(kSurfaceFrameSlotCount == 2);
	CHECK(kSurfaceFrameFormats[0] == DXGI_FORMAT_R32G32_UINT);
	CHECK(kSurfaceFrameFormats[1] == DXGI_FORMAT_R32G32B32A32_FLOAT);
	CHECK(kSurfaceFrameFormats[2] == DXGI_FORMAT_R32G32B32A32_FLOAT);
	CHECK(kSurfaceFrameFormats[3] == DXGI_FORMAT_R16G16B16A16_FLOAT);
	CHECK(kSurfaceFrameFormats[4] == DXGI_FORMAT_R16G16B16A16_FLOAT);
	CHECK(kSurfaceFrameFormats[5] == DXGI_FORMAT_R16G16B16A16_FLOAT);
	CHECK(kSurfaceFrameFormats[6] == DXGI_FORMAT_R32G32B32A32_FLOAT);
	CHECK(kSurfaceFrameFormats[7] == DXGI_FORMAT_R32G32B32A32_FLOAT);
	CHECK(kSurfaceTransportFormats[0] == DXGI_FORMAT_R8G8B8A8_UINT);
	CHECK(kSurfaceTransportFormats[1] == DXGI_FORMAT_R8G8B8A8_UINT);
	CHECK(kSurfaceTransportFormats[2] == kSurfaceFrameFormats[1]);
	for (size_t i = 3; i < kSurfaceTransportAttachmentCount; ++i)
		CHECK(kSurfaceTransportFormats[i] == kSurfaceFrameFormats[i - 1]);
	CHECK(sizeof(GpuMaterialRecord) == 304);
	CHECK(sizeof(GpuDrawBinding) == 24);
	CHECK(kGpuTextureDescriptorCapacity == 512);
}

TEST_CASE("NativePBR surface draw ownership requires sequential immutable identities", "[NativePBR][surface]")
{
	using namespace NativePBR;
	std::vector<SidecarDraw> draws{
		{ .ordinal = 1, .materialIdentity = 17, .materialRevision = 2 },
		{ .ordinal = 2, .materialIdentity = 19, .materialRevision = 4, .projectedUv = true }
	};
	CHECK(ValidSurfaceDrawTable(draws));
	draws[1].ordinal = 1;
	CHECK_FALSE(ValidSurfaceDrawTable(draws));
	draws[1].ordinal = 2;
	draws[1].materialIdentity = 0;
	CHECK_FALSE(ValidSurfaceDrawTable(draws));
	draws[1].materialIdentity = 19;
	draws[1].materialRevision = 0;
	CHECK_FALSE(ValidSurfaceDrawTable(draws));
	CHECK_FALSE(ValidSurfaceDrawTable(std::span<const SidecarDraw>{}));
	std::vector<SidecarDraw> tooMany(kMaximumFrameDraws + 1);
	for (size_t index = 0; index < tooMany.size(); ++index) {
		tooMany[index].ordinal = uint32_t(index + 1);
		tooMany[index].materialIdentity = 17;
		tooMany[index].materialRevision = 2;
	}
	CHECK_FALSE(ValidSurfaceDrawTable(tooMany));
}

TEST_CASE("NativePBR surface publication defaults to unowned and unsignaled", "[NativePBR][surface]")
{
	NativePBR::SurfaceFrame frame;
	CHECK(frame.abiVersion == NativePBR::kSurfaceFrameAbiVersion);
	CHECK_FALSE(frame.nativeOwnedDraws);
	CHECK_FALSE(frame.projectedAvailable);
	CHECK_FALSE(frame.ProducerComplete());
	for (const auto& texture : frame.textures)
		CHECK_FALSE(texture);
}
