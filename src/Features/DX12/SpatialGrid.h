#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace DX12
{
	inline constexpr std::array<uint32_t, 7> kSubcellSizes{ 4096, 6144, 8192, 10240, 12288, 14336, 16384 };
	inline constexpr int kGridRadius = 1;
	inline constexpr size_t kGridCapacity = (2 * kGridRadius + 1) * (2 * kGridRadius + 1);
	inline constexpr size_t kPredictiveCapacity = 16;  // 5x5 outer ring: twelve sides, four corners.

	/** @brief Identifies one runtime subcell, independent of any on-disk cache. */
	struct SubcellKey
	{
		uint32_t scope = 0;
		int64_t x = 0, y = 0;
		bool operator==(const SubcellKey&) const = default;
	};

	/** @brief Returns the containing tile even for negative game coordinates. */
	inline std::optional<int64_t> Coordinate(double position, uint32_t size)
	{
		if (!std::isfinite(position) || !std::ranges::contains(kSubcellSizes, size))
			return std::nullopt;
		const double result = std::floor(position / double(size));
		if (result < double(std::numeric_limits<int64_t>::min()) ||
			result >= double(std::numeric_limits<int64_t>::max()))
			return std::nullopt;
		return int64_t(result);
	}

	/** @brief Calculates the nine live tiles around the player, closest first. */
	inline std::optional<std::array<SubcellKey, kGridCapacity>> Neighborhood(uint32_t scope, uint32_t size, double x, double y)
	{
		const auto centerX = Coordinate(x, size), centerY = Coordinate(y, size);
		if (!scope || !centerX || !centerY || *centerX <= std::numeric_limits<int64_t>::min() + 2 ||
			*centerX >= std::numeric_limits<int64_t>::max() - 2 ||
			*centerY <= std::numeric_limits<int64_t>::min() + 2 ||
			*centerY >= std::numeric_limits<int64_t>::max() - 2)
			return std::nullopt;
		std::array<SubcellKey, kGridCapacity> tiles{};
		size_t index = 0;
		for (int radius = 0; radius <= kGridRadius; ++radius)
			for (int dy = -radius; dy <= radius; ++dy)
				for (int dx = -radius; dx <= radius; ++dx)
					if (std::max(std::abs(dx), std::abs(dy)) == radius)
						tiles[index++] = { scope, *centerX + dx, *centerY + dy };
		return tiles;
	}

	/** @brief Companion-only 5x5 outer ring. First twelve are sides, final four are corners. */
	inline std::optional<std::array<SubcellKey, kPredictiveCapacity>> PredictiveNeighborhood(
		uint32_t scope, uint32_t size, double x, double y)
	{
		const auto center = Neighborhood(scope, size, x, y);
		if (!center)
			return std::nullopt;
		const auto origin = center->front();
		std::array<SubcellKey, kPredictiveCapacity> tiles{};
		size_t side = 0, corner = 12;
		for (int64_t dy = -2; dy <= 2; ++dy)
			for (int64_t dx = -2; dx <= 2; ++dx) {
				if (std::max(std::abs(dx), std::abs(dy)) != 2)
					continue;
				const bool outerCorner = std::abs(dx) == 2 && std::abs(dy) == 2;
				tiles[outerCorner ? corner++ : side++] = { scope, origin.x + dx, origin.y + dy };
			}
		return tiles;
	}
}
