#pragma once
#include <cstdint>

namespace WaterFlowmapLayout
{
	constexpr std::uint32_t TexelsPerCell = 64;
	constexpr std::uint32_t MipCount = 6;
	constexpr std::int32_t MaxCells = 16384 / TexelsPerCell;

	/** @brief Checks cell bounds against the 64-texel D3D11 flowmap capacity. */
	constexpr bool ValidCells(std::int32_t width, std::int32_t height) noexcept
	{
		return width > 0 && height > 0 && width <= MaxCells && height <= MaxCells;
	}

	/** @brief Accepts the full flowmap or one of its supported mip reductions. */
	constexpr bool MatchesTexture(std::uint32_t width, std::uint32_t height,
		std::int32_t cellsX, std::int32_t cellsY) noexcept
	{
		if (!ValidCells(cellsX, cellsY))
			return false;
		for (std::uint32_t mip = 0; mip < MipCount; ++mip) {
			if (width == (static_cast<std::uint32_t>(cellsX) * TexelsPerCell >> mip) &&
				height == (static_cast<std::uint32_t>(cellsY) * TexelsPerCell >> mip))
				return true;
		}
		return false;
	}
}
