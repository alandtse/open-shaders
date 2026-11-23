#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace Util
{
	/** @brief True when a render extent is the tiny size the engine uses for a minimized window (8x8). */
	[[nodiscard]] constexpr bool IsMinimizedExtent(std::uint32_t a_width, std::uint32_t a_height)
	{
		constexpr std::uint32_t kMinimizedExtent = 16;
		return a_width < kMinimizedExtent || a_height < kMinimizedExtent;
	}

	/** @brief Mip count for a texture of the given size, capped at a_maxMips; D3D11 allows at most floor(log2(largest side)) + 1. */
	[[nodiscard]] constexpr std::uint32_t MipChainLength(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_maxMips)
	{
		return std::min(a_maxMips, static_cast<std::uint32_t>(std::bit_width(std::max(a_width, a_height))));
	}
}
