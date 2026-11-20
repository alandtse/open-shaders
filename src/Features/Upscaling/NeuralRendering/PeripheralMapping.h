#pragma once

// Analytic curve adapted from Yuri Grib (BeliyG3), optimizer-fps-dlss5, MIT.
// See SKSE/Plugins/CommunityShaders/Licenses/NeuralRendering/PeripheralCompression.txt.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace NR
{
	/** @brief Invertible sampling map shared by preparation, motion and reconstruction. */
	struct PeripheralMapping
	{
		using Point = std::array<float, 2>;
		float protectedFraction = 1.0f, boundaryScale = 1.0f, edgeSlope = 1.0f;
		std::uint32_t padding = 0;
		bool operator==(const PeripheralMapping&) const = default;

		/** @brief Distance from the centre relative to the full eye rectangle. */
		[[nodiscard]] float Distance(Point delta) const noexcept
		{
			return 2.0f * std::max(std::abs(delta[0]), std::abs(delta[1]));
		}

		/** @brief Compressed radius, retaining a unit derivative at the protected centre. */
		[[nodiscard]] float ForwardRadius(float radius) const noexcept
		{
			if (radius <= protectedFraction)
				return radius;
			if (radius >= 1.0f)
				return boundaryScale + (radius - 1.0f) * edgeSlope;
			const float phase = (radius - protectedFraction) / (1.0f - protectedFraction);
			return protectedFraction + (boundaryScale - protectedFraction) * (2.0f * phase / (1.0f + phase));
		}

		/** @brief Inverse radial scale, including motion endpoints outside the eye rectangle. */
		[[nodiscard]] float InverseScale(float radius) const noexcept
		{
			if (radius <= protectedFraction)
				return 1.0f;
			if (radius >= boundaryScale)
				return (1.0f + (radius - boundaryScale) / edgeSlope) / radius;
			const float phase = (radius - protectedFraction) / (boundaryScale - protectedFraction);
			return (protectedFraction + (1.0f - protectedFraction) * phase / (2.0f - phase)) / radius;
		}

		/** @brief Maps source-eye UVs to the model's UVs. */
		[[nodiscard]] Point Forward(Point source) const noexcept
		{
			const Point delta{ source[0] - 0.5f, source[1] - 0.5f };
			const float radius = Distance(delta);
			const float scale = radius > protectedFraction ? ForwardRadius(radius) / radius : 1.0f;
			return { 0.5f + delta[0] * scale / boundaryScale, 0.5f + delta[1] * scale / boundaryScale };
		}

		/** @brief Maps model UVs back to source-eye UVs. */
		[[nodiscard]] Point Inverse(Point target) const noexcept
		{
			const Point delta{ (target[0] - 0.5f) * boundaryScale, (target[1] - 0.5f) * boundaryScale };
			const float scale = InverseScale(Distance(delta));
			return { 0.5f + delta[0] * scale, 0.5f + delta[1] * scale };
		}

		/** @brief Model bounds of the source rectangle, including extrema along the centre axes. */
		[[nodiscard]] std::pair<Point, Point> TargetBounds(Point lower, Point upper) const noexcept
		{
			Point minimum{ 1.0f, 1.0f }, maximum{};
			for (float x : { lower[0], upper[0], std::clamp(0.5f, lower[0], upper[0]) })
				for (float y : { lower[1], upper[1], std::clamp(0.5f, lower[1], upper[1]) }) {
					const auto mapped = Forward({ x, y });
					for (unsigned axis = 0; axis < 2; ++axis) {
						minimum[axis] = std::min(minimum[axis], mapped[axis]);
						maximum[axis] = std::max(maximum[axis], mapped[axis]);
					}
				}
			return { minimum, maximum };
		}

		/** @brief Conservative source bounds of one model footprint, including axis crossings. */
		[[nodiscard]] std::pair<Point, Point> SourceBounds(Point lower, Point upper) const noexcept
		{
			Point low{}, high{}, nearest{}, farthest{};
			for (unsigned axis = 0; axis < 2; ++axis) {
				low[axis] = (lower[axis] - 0.5f) * boundaryScale;
				high[axis] = (upper[axis] - 0.5f) * boundaryScale;
				nearest[axis] = low[axis] <= 0.0f && high[axis] >= 0.0f ? 0.0f : std::min(std::abs(low[axis]), std::abs(high[axis]));
				farthest[axis] = std::max(std::abs(low[axis]), std::abs(high[axis]));
			}
			const float nearScale = InverseScale(Distance(nearest)), farScale = InverseScale(Distance(farthest));
			for (unsigned axis = 0; axis < 2; ++axis) {
				lower[axis] = 0.5f + std::min(low[axis] * nearScale, low[axis] * farScale);
				upper[axis] = 0.5f + std::max(high[axis] * nearScale, high[axis] * farScale);
			}
			return { lower, upper };
		}
	};
	static_assert(sizeof(PeripheralMapping) == 16);
}
