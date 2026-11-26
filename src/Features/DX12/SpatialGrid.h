#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace DX12
{
	inline constexpr uint32_t kMinSubcellSize = 4096;
	inline constexpr uint32_t kMaxSubcellSize = 16384;
	inline constexpr uint32_t kSubcellSizeStep = 2048;
	inline constexpr auto kSubcellSizes = [] {
		std::array<uint32_t, (kMaxSubcellSize - kMinSubcellSize) / kSubcellSizeStep + 1> sizes{};
		for (size_t i = 0; i < sizes.size(); ++i)
			sizes[i] = kMinSubcellSize + static_cast<uint32_t>(i) * kSubcellSizeStep;
		return sizes;
	}();
	inline constexpr int kGridRadius = 1;
	inline constexpr int kGridDiameter = 2 * kGridRadius + 1;
	inline constexpr size_t kGridCapacity = kGridDiameter * kGridDiameter;
	inline constexpr int kPredictiveRadius = kGridRadius + 1;
	inline constexpr int kPredictiveDiameter = 2 * kPredictiveRadius + 1;
	inline constexpr size_t kPredictiveCapacity = kPredictiveDiameter * kPredictiveDiameter - kGridCapacity;
	inline constexpr size_t kPredictiveCornerCount = 4;
	inline constexpr size_t kPredictiveSideCount = kPredictiveCapacity - kPredictiveCornerCount;

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
		if (!scope || !centerX || !centerY || *centerX <= std::numeric_limits<int64_t>::min() + kPredictiveRadius ||
			*centerX >= std::numeric_limits<int64_t>::max() - kPredictiveRadius ||
			*centerY <= std::numeric_limits<int64_t>::min() + kPredictiveRadius ||
			*centerY >= std::numeric_limits<int64_t>::max() - kPredictiveRadius)
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

	/** @brief Predictive 5x5 outer ring. First twelve are sides, final four are corners. */
	inline std::optional<std::array<SubcellKey, kPredictiveCapacity>> PredictiveNeighborhood(
		uint32_t scope, uint32_t size, double x, double y)
	{
		const auto center = Neighborhood(scope, size, x, y);
		if (!center)
			return std::nullopt;
		const auto origin = center->front();
		std::array<SubcellKey, kPredictiveCapacity> tiles{};
		size_t side = 0, corner = kPredictiveSideCount;
		for (int64_t dy = -kPredictiveRadius; dy <= kPredictiveRadius; ++dy)
			for (int64_t dx = -kPredictiveRadius; dx <= kPredictiveRadius; ++dx) {
				if (std::max(std::abs(dx), std::abs(dy)) != kPredictiveRadius)
					continue;
				const bool outerCorner = std::abs(dx) == kPredictiveRadius && std::abs(dy) == kPredictiveRadius;
				tiles[outerCorner ? corner++ : side++] = { scope, origin.x + dx, origin.y + dy };
			}
		return tiles;
	}
}
