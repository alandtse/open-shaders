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
		if (!scope || !centerX || !centerY || *centerX <= std::numeric_limits<int64_t>::min() + kGridRadius ||
			*centerX >= std::numeric_limits<int64_t>::max() - kGridRadius ||
			*centerY <= std::numeric_limits<int64_t>::min() + kGridRadius ||
			*centerY >= std::numeric_limits<int64_t>::max() - kGridRadius)
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
}
