#include "Utils/HdrPngMetadata.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace Util::HdrPng
{
	float PqToNits(float pq)
	{
		// SMPTE ST 2084 EOTF constants.
		constexpr float m1 = 0.1593017578125f;
		constexpr float m2 = 78.84375f;
		constexpr float c1 = 0.8359375f;
		constexpr float c2 = 18.8515625f;
		constexpr float c3 = 18.6875f;

		if (pq <= 0.0f) {
			return 0.0f;
		}
		if (pq > 1.0f) {
			pq = 1.0f;
		}
		const float e = std::pow(pq, 1.0f / m2);
		const float num = e - c1 > 0.0f ? e - c1 : 0.0f;
		const float den = c2 - c3 * e;
		if (den <= 0.0f) {
			return 10000.0f;
		}
		return 10000.0f * std::pow(num / den, 1.0f / m1);
	}

	ContentLightLevel ComputeContentLightLevel(const uint16_t* rgb, size_t pixelCount)
	{
		ContentLightLevel cll;
		if (rgb == nullptr || pixelCount == 0) {
			return cll;
		}

		// 1-nit histogram over [0,10000] for a hot-pixel-robust percentile, plus a
		// running mean. maxRGB per CTA-861.3 (the brightest component, not luminance).
		// Heap-allocated (~80 KB) to keep it off the worker-thread stack.
		constexpr int kBins = 10001;
		std::vector<uint64_t> hist(kBins, 0);
		double sumNits = 0.0;

		for (size_t i = 0; i < pixelCount; ++i) {
			const uint16_t* px = rgb + i * 3;
			uint16_t mx = px[0];
			if (px[1] > mx) {
				mx = px[1];
			}
			if (px[2] > mx) {
				mx = px[2];
			}
			const float nits = PqToNits(static_cast<float>(mx) / 65535.0f);
			sumNits += nits;
			int bin = static_cast<int>(nits + 0.5f);
			if (bin < 0) {
				bin = 0;
			} else if (bin >= kBins) {
				bin = kBins - 1;
			}
			++hist[static_cast<size_t>(bin)];
		}

		// 99.5th percentile -> MaxCLL.
		const uint64_t threshold = static_cast<uint64_t>(
			std::ceil(static_cast<double>(pixelCount) * 0.995));
		uint64_t cumulative = 0;
		int percentileBin = 0;
		for (int b = 0; b < kBins; ++b) {
			cumulative += hist[static_cast<size_t>(b)];
			if (cumulative >= threshold) {
				percentileBin = b;
				break;
			}
		}

		const double meanNits = sumNits / static_cast<double>(pixelCount);
		// cLLi units are 0.0001 cd/m^2.
		cll.maxCLL = static_cast<uint32_t>(static_cast<double>(percentileBin) * 10000.0);
		cll.maxFALL = static_cast<uint32_t>(meanNits * 10000.0 + 0.5);
		return cll;
	}

	uint32_t Crc32(const uint8_t* data, size_t len)
	{
		uint32_t crc = 0xFFFFFFFFu;
		for (size_t i = 0; i < len; ++i) {
			crc ^= data[i];
			for (int k = 0; k < 8; ++k) {
				crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
			}
		}
		return crc ^ 0xFFFFFFFFu;
	}

	namespace
	{
		void PushBE16(std::vector<uint8_t>& out, uint16_t v)
		{
			out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
			out.push_back(static_cast<uint8_t>(v & 0xFF));
		}

		void PushBE32(std::vector<uint8_t>& out, uint32_t v)
		{
			out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
			out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
			out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
			out.push_back(static_cast<uint8_t>(v & 0xFF));
		}

		uint32_t ReadBE32(const uint8_t* p)
		{
			return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
			       (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
		}
	}

	std::vector<uint8_t> BuildClliChunk(ContentLightLevel cll)
	{
		std::vector<uint8_t> chunk;
		chunk.reserve(4 + 4 + 8 + 4);
		PushBE32(chunk, 8);  // payload length
		// Type + payload; the CRC covers both (not the length).
		const size_t crcStart = chunk.size();
		const char type[4] = { 'c', 'L', 'L', 'i' };
		chunk.insert(chunk.end(), type, type + 4);
		PushBE32(chunk, cll.maxCLL);
		PushBE32(chunk, cll.maxFALL);
		const uint32_t crc = Crc32(chunk.data() + crcStart, chunk.size() - crcStart);
		PushBE32(chunk, crc);
		return chunk;
	}

	std::vector<uint8_t> BuildMdcvChunk()
	{
		// Fixed BT.2020 primaries / D65 white (x,y / 0.00002) and a 1000-nit mastering
		// display (max=1000 cd/m^2, min=0.0001 cd/m^2, both / 0.0001).
		std::vector<uint8_t> chunk;
		chunk.reserve(4 + 4 + 24 + 4);
		PushBE32(chunk, 24);  // payload length
		const size_t crcStart = chunk.size();
		const char type[4] = { 'm', 'D', 'C', 'v' };
		chunk.insert(chunk.end(), type, type + 4);
		PushBE16(chunk, 35400);      // red x
		PushBE16(chunk, 14600);      // red y
		PushBE16(chunk, 8500);       // green x
		PushBE16(chunk, 39850);      // green y
		PushBE16(chunk, 6550);       // blue x
		PushBE16(chunk, 2300);       // blue y
		PushBE16(chunk, 15635);      // white x (D65)
		PushBE16(chunk, 16450);      // white y (D65)
		PushBE32(chunk, 10000000u);  // max luminance: 1000 cd/m^2
		PushBE32(chunk, 1u);         // min luminance: 0.0001 cd/m^2
		const uint32_t crc = Crc32(chunk.data() + crcStart, chunk.size() - crcStart);
		PushBE32(chunk, crc);
		return chunk;
	}

	bool InsertChunkBeforeIdat(std::vector<uint8_t>& png, const std::vector<uint8_t>& chunkBytes)
	{
		static constexpr uint8_t kSig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
		if (png.size() < 8 || std::memcmp(png.data(), kSig, 8) != 0) {
			return false;
		}

		size_t pos = 8;
		while (pos + 8 <= png.size()) {
			const uint32_t dataLen = ReadBE32(png.data() + pos);
			const uint8_t* type = png.data() + pos + 4;
			if (std::memcmp(type, "IDAT", 4) == 0) {
				png.insert(png.begin() + static_cast<std::ptrdiff_t>(pos),
					chunkBytes.begin(), chunkBytes.end());
				return true;
			}
			// Advance past length(4) + type(4) + data + crc(4); guard overflow.
			const size_t advance = static_cast<size_t>(dataLen) + 12;
			if (advance < 12 || pos + advance < pos) {
				return false;
			}
			pos += advance;
		}
		return false;
	}
}
