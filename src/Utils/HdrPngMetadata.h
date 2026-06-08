#pragma once

// Additive HDR-PNG metadata: compute and inject a cLLi (Content Light Level
// Information) chunk into a PNG produced by stb_image_write_hdr_png. The vendored
// stb encoder writes cICP/sBIT/iCCP/cHRM but not cLLi; this restores the MaxCLL/
// MaxFALL static metadata the previous (sk_hdr_png) encoder produced, without
// touching the vendored header.
//
// All functions are pure (no DirectX/SKSE deps) so they are unit-tested in
// tests/cpp/test_hdr_png_metadata.cpp. Behavior is pinned to the PNG/CTA-861
// spec + known references there to prevent drift.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Util::HdrPng
{
	// SMPTE ST 2084 (PQ) EOTF: perceptual-quantizer code value in [0,1] -> absolute
	// luminance in cd/m^2 (nits), 0..10000.
	float PqToNits(float pq);

	// CTA-861.3 content light levels, in cLLi units (0.0001 cd/m^2).
	struct ContentLightLevel
	{
		uint32_t maxCLL = 0;   // peak per-pixel max(R,G,B), 99.5th percentile
		uint32_t maxFALL = 0;  // frame-average of per-pixel max(R,G,B)
	};

	// rgb: tightly packed 16-bit PQ samples, 3 channels/pixel (the buffer handed to
	// the stb writer). pixelCount = width*height. Decodes each channel via PqToNits,
	// takes per-pixel max(R,G,B); MaxCLL = 99.5th percentile (robust to hot pixels),
	// MaxFALL = mean.
	ContentLightLevel ComputeContentLightLevel(const uint16_t* rgb, size_t pixelCount);

	// PNG CRC-32 (ISO 3309) over the given bytes (chunk type + data), as required by
	// the PNG chunk CRC field.
	uint32_t Crc32(const uint8_t* data, size_t len);

	// Build a complete cLLi chunk: length(4 BE) + "cLLi" + payload(8 BE) + CRC(4 BE).
	std::vector<uint8_t> BuildClliChunk(ContentLightLevel cll);

	// Build a complete mDCv (Mastering Display Color Volume, SMPTE ST 2086) chunk for
	// a fixed BT.2020 / D65 / 1000-nit mastering display. Per the PNG spec the 24-byte
	// payload is: 6x u16 primaries (R,G,B; x then y; /0.00002), 2x u16 white point,
	// then u32 max + u32 min luminance (/0.0001 cd/m^2). All big-endian.
	std::vector<uint8_t> BuildMdcvChunk();

	// Insert chunkBytes immediately before the first IDAT chunk of png (in place).
	// Returns false (png unchanged) if png is not a well-formed PNG with an IDAT.
	bool InsertChunkBeforeIdat(std::vector<uint8_t>& png, const std::vector<uint8_t>& chunkBytes);
}
