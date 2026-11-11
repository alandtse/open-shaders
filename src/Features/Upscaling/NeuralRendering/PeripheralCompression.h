#pragma once

#include "PeripheralMapping.h"

namespace NR
{
	inline constexpr std::uint32_t kMaximumPeripheralCompressionPercent = 100;
	inline constexpr float kPeripheralMaximumFootprint = 32.0f;
	inline constexpr float kPeripheralEdgeSlope = 0.25f;
	inline constexpr std::uint32_t kPeripheralMaximumDimension = 16384;

	/** @brief Why this frame uses compressed or ordinary sampling. */
	enum class PeripheralCompressionReason : std::uint32_t
	{
		Disabled,
		Active,
		UnsupportedScope,
		InvalidGeometry,
		InsufficientSavings,
		FootprintLimit,
		ShaderUnavailable,
		ResourceUnavailable
	};

	/** @brief Frozen full-eye sampling map and one shared per-eye allocation size. */
	struct PeripheralCompressionPlan
	{
		PeripheralMapping mapping{};
		std::uint32_t width = 0, height = 0;
		PeripheralCompressionReason reason = PeripheralCompressionReason::Disabled;
		/** @brief True only after the sampling footprint was admitted. */
		[[nodiscard]] bool Active() const noexcept { return reason == PeripheralCompressionReason::Active; }
	};

	/** @brief Admits compression as one transaction; rejection retains the caller's ordinary model size. */
	[[nodiscard]] inline PeripheralCompressionPlan BuildPeripheralCompressionPlan(std::uint32_t width, std::uint32_t height,
		std::uint32_t ordinaryWidth, std::uint32_t ordinaryHeight, std::uint32_t percent, bool supported, std::uint32_t alignment, std::uint32_t minimumExtent) noexcept
	{
		PeripheralCompressionPlan plan;
		using enum PeripheralCompressionReason;
		const auto reject = [&](PeripheralCompressionReason reason) { return PeripheralCompressionPlan{ .reason = reason }; };
		if (percent == 0)
			return plan;
		if (!supported)
			return reject(UnsupportedScope);
		if (percent > kMaximumPeripheralCompressionPercent || !width || !height || !ordinaryWidth || !ordinaryHeight ||
			ordinaryWidth > width || ordinaryHeight > height || width > kPeripheralMaximumDimension || height > kPeripheralMaximumDimension ||
			!alignment || alignment > kPeripheralMaximumDimension || !minimumExtent || minimumExtent > kPeripheralMaximumDimension)
			return reject(InvalidGeometry);
		auto& mapping = plan.mapping;
		mapping.protectedFraction = 1.0f - static_cast<float>(percent) * 0.01f;
		mapping.boundaryScale = (1.0f + mapping.protectedFraction) * 0.5f;
		mapping.edgeSlope = kPeripheralEdgeSlope;
		if (ordinaryWidth < minimumExtent || ordinaryHeight < minimumExtent)
			return reject(InsufficientSavings);
		const std::array source{ width, height };
		double minimumScale = 0.0;
		for (unsigned axis = 0; axis < 2; ++axis)
			minimumScale = std::max(minimumScale, mapping.boundaryScale *
													  (1.0 + (1.0 - mapping.edgeSlope) * source[axis] / source[1u - axis]) /
													  (mapping.edgeSlope * kPeripheralMaximumFootprint));
		const auto alignUp = [alignment](std::uint32_t pixels) { return ((pixels + alignment - 1u) / alignment) * alignment; };
		// If either ordinary axis rounds below its density bound, fallback must not enlarge the other axis.
		double fallbackScale = std::min(minimumScale, 1.0);
		for (const auto full : source) {
			const auto density = alignUp(static_cast<std::uint32_t>(std::ceil(full * std::min(minimumScale, 1.0))));
			fallbackScale = std::max(fallbackScale, (density - alignment * 0.5) / full);
		}
		const auto reduce = [&](std::uint32_t full, std::uint32_t ordinary) {
			const auto densityFloor = static_cast<std::uint32_t>(std::ceil(full * std::min(fallbackScale, 1.0)));
			const auto compressed = static_cast<std::uint32_t>(std::ceil(ordinary * mapping.boundaryScale));
			const auto pixels = std::max(compressed, densityFloor);
			return std::min(ordinary, std::max(minimumExtent, alignUp(pixels)));
		};
		plan.width = reduce(width, ordinaryWidth);
		plan.height = reduce(height, ordinaryHeight);
		const std::array model{ plan.width, plan.height };
		for (unsigned axis = 0; axis < 2; ++axis) {
			const float step = mapping.boundaryScale / model[axis] +
			                   (1.0f - mapping.edgeSlope) * mapping.boundaryScale / model[1u - axis];
			const float footprint = source[axis] * step / mapping.edgeSlope;
			if (!std::isfinite(footprint) || footprint > kPeripheralMaximumFootprint)
				return reject(FootprintLimit);
		}
		if (static_cast<std::uint64_t>(plan.width) * plan.height >= static_cast<std::uint64_t>(ordinaryWidth) * ordinaryHeight)
			return reject(InsufficientSavings);
		plan.reason = Active;
		return plan;
	}

	/** @brief Stable devbench names for admission and fallback. */
	[[nodiscard]] constexpr const char* PeripheralCompressionReasonName(PeripheralCompressionReason reason) noexcept
	{
		switch (reason) {
		case PeripheralCompressionReason::Active:
			return "active";
		case PeripheralCompressionReason::UnsupportedScope:
			return "unsupported_scope";
		case PeripheralCompressionReason::InvalidGeometry:
			return "invalid_geometry";
		case PeripheralCompressionReason::InsufficientSavings:
			return "insufficient_savings";
		case PeripheralCompressionReason::FootprintLimit:
			return "footprint_limit";
		case PeripheralCompressionReason::ShaderUnavailable:
			return "shader_unavailable";
		case PeripheralCompressionReason::ResourceUnavailable:
			return "resource_unavailable";
		default:
			return "disabled";
		}
	}
}
