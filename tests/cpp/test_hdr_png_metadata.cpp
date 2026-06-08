// Unit tests for Util::HdrPng (additive HDR-PNG cLLi metadata).
//
// Behavior is pinned to the spec + known references (not to the prior sk_hdr_png
// code, which had spec deviations) so the encoder migration can't silently drift:
//   - PQ EOTF -> nits at SMPTE ST 2084 reference points
//   - PNG CRC-32 against the canonical IEND constant
//   - cLLi chunk byte layout (length/type/BE payload/CRC)
//   - MaxCLL = 99.5th-percentile max(R,G,B); MaxFALL = mean
//   - IDAT-splice round-trips a parseable PNG

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Utils/HdrPngMetadata.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace Util::HdrPng;

namespace
{
	// PQ code value (0..1) for a target luminance, via the inverse EOTF (OETF).
	float NitsToPq(float nits)
	{
		constexpr float m1 = 0.1593017578125f, m2 = 78.84375f;
		constexpr float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
		const float y = nits / 10000.0f;
		const float ym1 = std::pow(y, m1);
		return std::pow((c1 + c2 * ym1) / (1.0f + c3 * ym1), m2);
	}
	uint16_t PqUnorm(float nits) { return static_cast<uint16_t>(NitsToPq(nits) * 65535.0f + 0.5f); }
}

TEST_CASE("PqToNits matches ST 2084 reference points", "[hdrpng]")
{
	REQUIRE(PqToNits(0.0f) == 0.0f);
	REQUIRE(PqToNits(1.0f) >= 9990.0f);  // peak ~10000 nits
	REQUIRE(PqToNits(-0.5f) == 0.0f);    // clamps below 0
	// Round-trip a few luminances through the OETF then back.
	for (float nits : { 1.0f, 100.0f, 203.0f, 1000.0f, 4000.0f }) {
		const float back = PqToNits(NitsToPq(nits));
		REQUIRE(back == Catch::Approx(nits).margin(nits * 0.02f + 0.5f));
	}
	// Monotonic.
	REQUIRE(PqToNits(0.25f) < PqToNits(0.5f));
	REQUIRE(PqToNits(0.5f) < PqToNits(0.75f));
}

TEST_CASE("Crc32 matches the canonical PNG IEND constant", "[hdrpng]")
{
	const uint8_t iend[4] = { 'I', 'E', 'N', 'D' };
	REQUIRE(Crc32(iend, 4) == 0xAE426082u);
}

TEST_CASE("ComputeContentLightLevel handles uniform images", "[hdrpng]")
{
	// All white (PQ 1.0 -> 10000 nits). cLLi units are 0.0001 cd/m^2.
	std::vector<uint16_t> white(3 * 16, 0xFFFF);
	auto cll = ComputeContentLightLevel(white.data(), 16);
	REQUIRE(cll.maxCLL == 100000000u);  // 10000 nits
	REQUIRE(cll.maxFALL >= 99000000u);

	// All black.
	std::vector<uint16_t> black(3 * 16, 0);
	auto z = ComputeContentLightLevel(black.data(), 16);
	REQUIRE(z.maxCLL == 0u);
	REQUIRE(z.maxFALL == 0u);

	// Empty / null guards.
	REQUIRE(ComputeContentLightLevel(nullptr, 0).maxCLL == 0u);
}

TEST_CASE("ComputeContentLightLevel uses max(R,G,B) and the 99.5th percentile", "[hdrpng]")
{
	// max(R,G,B): green-only pixel at 1000 nits reads as 1000-nit content.
	const uint16_t g1000 = PqUnorm(1000.0f);
	std::vector<uint16_t> green(3 * 4, 0);
	for (int i = 0; i < 4; ++i) {
		green[i * 3 + 1] = g1000;  // G channel only
	}
	auto cg = ComputeContentLightLevel(green.data(), 4);
	REQUIRE(cg.maxCLL == Catch::Approx(10000000.0).margin(200000.0));  // ~1000 nits

	// 990 dim + 10 bright of 1000: 99.5th percentile (threshold 995) falls in the
	// bright set, so a few hot pixels DO raise MaxCLL.
	const uint16_t dim = PqUnorm(100.0f), bright = PqUnorm(4000.0f);
	std::vector<uint16_t> mix;
	mix.reserve(3 * 1000);
	for (int i = 0; i < 990; ++i) {
		mix.push_back(dim);
		mix.push_back(dim);
		mix.push_back(dim);
	}
	for (int i = 0; i < 10; ++i) {
		mix.push_back(bright);
		mix.push_back(bright);
		mix.push_back(bright);
	}
	auto cm = ComputeContentLightLevel(mix.data(), 1000);
	REQUIRE(cm.maxCLL > 1500000000u / 100u);                            // well above the 100-nit dim floor (1,000,000)
	REQUIRE(cm.maxCLL == Catch::Approx(40000000.0).margin(2000000.0));  // ~4000 nits
}

TEST_CASE("BuildClliChunk emits the spec byte layout", "[hdrpng]")
{
	ContentLightLevel cll;
	cll.maxCLL = 10000000u;  // 0x00989680
	cll.maxFALL = 2500000u;  // 0x002625A0
	auto chunk = BuildClliChunk(cll);

	REQUIRE(chunk.size() == 4 + 4 + 8 + 4);  // len + type + payload + crc
	// Length = 8 (payload), big-endian.
	REQUIRE(chunk[0] == 0x00);
	REQUIRE(chunk[1] == 0x00);
	REQUIRE(chunk[2] == 0x00);
	REQUIRE(chunk[3] == 0x08);
	// Type.
	REQUIRE(chunk[4] == 'c');
	REQUIRE(chunk[5] == 'L');
	REQUIRE(chunk[6] == 'L');
	REQUIRE(chunk[7] == 'i');
	// Payload, big-endian.
	const uint8_t expect[8] = { 0x00, 0x98, 0x96, 0x80, 0x00, 0x26, 0x25, 0xA0 };
	for (int i = 0; i < 8; ++i) {
		REQUIRE(chunk[8 + i] == expect[i]);
	}
	// CRC covers type + payload (not length).
	const uint32_t crc = Crc32(chunk.data() + 4, 12);
	const uint32_t stored = (uint32_t(chunk[16]) << 24) | (uint32_t(chunk[17]) << 16) |
	                        (uint32_t(chunk[18]) << 8) | uint32_t(chunk[19]);
	REQUIRE(stored == crc);
}

TEST_CASE("BuildMdcvChunk emits the 24-byte spec layout (BT.2020/D65/1000-nit)", "[hdrpng]")
{
	auto chunk = BuildMdcvChunk();
	REQUIRE(chunk.size() == 4 + 4 + 24 + 4);  // len + type + 24B payload + crc

	REQUIRE(chunk[0] == 0x00);
	REQUIRE(chunk[1] == 0x00);
	REQUIRE(chunk[2] == 0x00);
	REQUIRE(chunk[3] == 0x18);  // length 24
	REQUIRE(chunk[4] == 'm');
	REQUIRE(chunk[5] == 'D');
	REQUIRE(chunk[6] == 'C');
	REQUIRE(chunk[7] == 'v');

	// Payload: u16 primaries R,G,B (x,y) + u16 white, then u32 max/min luminance, BE.
	const uint8_t expect[24] = {
		0x8A,
		0x48,  // red x   35400
		0x39,
		0x08,  // red y   14600
		0x21,
		0x34,  // green x  8500
		0x9B,
		0xAA,  // green y 39850
		0x19,
		0x96,  // blue x   6550
		0x08,
		0xFC,  // blue y   2300
		0x3D,
		0x13,  // white x 15635 (D65)
		0x40,
		0x42,  // white y 16450 (D65)
		0x00,
		0x98,
		0x96,
		0x80,  // max 10000000 (1000 cd/m^2)
		0x00,
		0x00,
		0x00,
		0x01,  // min 1        (0.0001 cd/m^2)
	};
	for (int i = 0; i < 24; ++i) {
		REQUIRE(chunk[8 + i] == expect[i]);
	}
	// CRC covers type + payload.
	const uint32_t crc = Crc32(chunk.data() + 4, 28);
	const uint32_t stored = (uint32_t(chunk[32]) << 24) | (uint32_t(chunk[33]) << 16) |
	                        (uint32_t(chunk[34]) << 8) | uint32_t(chunk[35]);
	REQUIRE(stored == crc);
}

TEST_CASE("InsertChunkBeforeIdat splices before the first IDAT", "[hdrpng]")
{
	auto be32 = [](std::vector<uint8_t>& v, uint32_t x) {
		v.push_back(uint8_t(x >> 24));
		v.push_back(uint8_t(x >> 16));
		v.push_back(uint8_t(x >> 8));
		v.push_back(uint8_t(x));
	};
	auto chunk = [&](std::vector<uint8_t>& v, const char* type, const std::vector<uint8_t>& data) {
		be32(v, static_cast<uint32_t>(data.size()));
		v.insert(v.end(), type, type + 4);
		v.insert(v.end(), data.begin(), data.end());
		be32(v, 0);  // dummy CRC for the harness PNG
	};

	std::vector<uint8_t> png = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	chunk(png, "IHDR", std::vector<uint8_t>(13, 0));
	const size_t idatStart = png.size();
	chunk(png, "IDAT", std::vector<uint8_t>(4, 0xAB));
	chunk(png, "IEND", {});

	const auto cl = BuildClliChunk(ContentLightLevel{ 123u, 45u });
	const size_t before = png.size();
	REQUIRE(InsertChunkBeforeIdat(png, cl));
	REQUIRE(png.size() == before + cl.size());
	// The inserted chunk now occupies the old IDAT offset.
	REQUIRE(std::memcmp(png.data() + idatStart + 4, "cLLi", 4) == 0);
	// The original IDAT follows, shifted by the chunk size.
	REQUIRE(std::memcmp(png.data() + idatStart + cl.size() + 4, "IDAT", 4) == 0);

	// Non-PNG input is rejected and left unchanged.
	std::vector<uint8_t> notPng = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	const auto copy = notPng;
	REQUIRE_FALSE(InsertChunkBeforeIdat(notPng, cl));
	REQUIRE(notPng == copy);
}
