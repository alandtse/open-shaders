// Unit tests for Util::ShaderDescriptor. The per-draw descriptor upload clears every bit
// that took part in the shader lookup; a shader that reads one of those bits at runtime
// must keep it, or its runtime branch is silently skipped.

#include "Utils/ShaderDescriptor.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using Util::ShaderDescriptor::ForEachSpecialization;
using Util::ShaderDescriptor::UploadedPixelDescriptor;

TEST_CASE("Shader specialization enumeration produces every combination once", "[ShaderDescriptor]")
{
	std::vector<std::uint32_t> descriptors;
	ForEachSpecialization(0x1200, 0x10000200, [&](auto descriptor) { descriptors.push_back(descriptor); });
	const std::vector<std::uint32_t> expected{ 0x10001200, 0x10001000, 0x1200, 0x1000 };
	CHECK(descriptors == expected);

	descriptors.clear();
	ForEachSpecialization(0x1200, 0, [&](auto descriptor) { descriptors.push_back(descriptor); });
	CHECK(descriptors == std::vector<std::uint32_t>{ 0x1200 });
}

TEST_CASE("UploadedPixelDescriptor clears lookup bits and keeps the rest", "[ShaderDescriptor]")
{
	CHECK(UploadedPixelDescriptor(0x15e, 0x5a, 0) == 0x104);
	CHECK(UploadedPixelDescriptor(0xffffffff, 0, 0) == 0xffffffff);
	CHECK(UploadedPixelDescriptor(0, 0xffffffff, 0) == 0);
}

TEST_CASE("UploadedPixelDescriptor keeps runtime bits that are part of the lookup", "[ShaderDescriptor]")
{
	// A wading water draw: 0x15e requested, 0x5a looked up, 0x40 read at runtime.
	CHECK(UploadedPixelDescriptor(0x15e, 0x5a, 0x40) == 0x144);
	CHECK((UploadedPixelDescriptor(0x15e, 0x5a, 0x40) & 0x40) != 0);
	CHECK((UploadedPixelDescriptor(0x15e, 0x5a, 0) & 0x40) == 0);
}

TEST_CASE("UploadedPixelDescriptor never sets bits the game did not request", "[ShaderDescriptor]")
{
	CHECK(UploadedPixelDescriptor(0x104, 0x5a, 0x40) == 0x104);
	CHECK(UploadedPixelDescriptor(0, 0x5a, 0x40) == 0);
}
