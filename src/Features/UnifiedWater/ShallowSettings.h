#pragma once

#include "Utils/MathUtils.h"

#include <cstddef>
#include <type_traits>

namespace UnifiedWaterSurface
{
	/** @brief Bounded shallow-surface controls packed into two shader registers. */
	struct alignas(16) CommonBufferData
	{
		float ShallowFallbackStrength = 1.0f;
		float DeepConnectionProbeReachUnits = 768.0f;
		float DeepContextDepthUnits = 192.0f;
		float ShoreContactMinFadePixels = 2.0f;
		float ShoreDepthBlendRangeUnits = 10.0f;
		float ShallowSurfaceDepthRangeUnits = 256.0f;
		float ShallowFallbackMaxDistance = 26000.0f;
		float DeepContextTransitionUnits = 128.0f;

		bool operator==(const CommonBufferData&) const = default;
	};

	/** @brief Selected controls, including the diagnostic legacy blend override. */
	struct Settings : CommonBufferData
	{
		bool UseOpenShadersDepthBehaviour = false;
		bool operator==(const Settings&) const = default;
	};

	inline constexpr CommonBufferData kMinimum{ 0.0f, 0.0f, 32.0f, 0.0f, 0.0f, 16.0f, 0.0f, 4.0f };
	inline constexpr CommonBufferData kMaximum{ 1.0f, 768.0f, 384.0f, 4.0f, 20.0f, 256.0f, 65536.0f, 128.0f };

	static_assert(sizeof(CommonBufferData) == 32 && alignof(CommonBufferData) == 16);
	static_assert(std::is_trivially_copyable_v<CommonBufferData>);
	static_assert(offsetof(CommonBufferData, ShallowFallbackStrength) == 0);
	static_assert(offsetof(CommonBufferData, DeepConnectionProbeReachUnits) == 4);
	static_assert(offsetof(CommonBufferData, DeepContextDepthUnits) == 8);
	static_assert(offsetof(CommonBufferData, ShoreContactMinFadePixels) == 12);
	static_assert(offsetof(CommonBufferData, ShoreDepthBlendRangeUnits) == 16);
	static_assert(offsetof(CommonBufferData, ShallowSurfaceDepthRangeUnits) == 20);
	static_assert(offsetof(CommonBufferData, ShallowFallbackMaxDistance) == 24);
	static_assert(offsetof(CommonBufferData, DeepContextTransitionUnits) == 28);

	/** @brief Normalizes persisted and live controls before publication. */
	[[nodiscard]] inline Settings Sanitize(Settings value)
	{
		const Settings defaults{};
		value.ShallowFallbackStrength = Util::ClampFinite(value.ShallowFallbackStrength, kMinimum.ShallowFallbackStrength, kMaximum.ShallowFallbackStrength, defaults.ShallowFallbackStrength);
		value.DeepConnectionProbeReachUnits = Util::ClampFinite(value.DeepConnectionProbeReachUnits, kMinimum.DeepConnectionProbeReachUnits, kMaximum.DeepConnectionProbeReachUnits, defaults.DeepConnectionProbeReachUnits);
		value.DeepContextDepthUnits = Util::ClampFinite(value.DeepContextDepthUnits, kMinimum.DeepContextDepthUnits, kMaximum.DeepContextDepthUnits, defaults.DeepContextDepthUnits);
		value.ShoreContactMinFadePixels = Util::ClampFinite(value.ShoreContactMinFadePixels, kMinimum.ShoreContactMinFadePixels, kMaximum.ShoreContactMinFadePixels, defaults.ShoreContactMinFadePixels);
		value.ShoreDepthBlendRangeUnits = Util::ClampFinite(value.ShoreDepthBlendRangeUnits, kMinimum.ShoreDepthBlendRangeUnits, kMaximum.ShoreDepthBlendRangeUnits, defaults.ShoreDepthBlendRangeUnits);
		value.ShallowSurfaceDepthRangeUnits = Util::ClampFinite(value.ShallowSurfaceDepthRangeUnits, kMinimum.ShallowSurfaceDepthRangeUnits, kMaximum.ShallowSurfaceDepthRangeUnits, defaults.ShallowSurfaceDepthRangeUnits);
		value.ShallowFallbackMaxDistance = Util::ClampFinite(value.ShallowFallbackMaxDistance, kMinimum.ShallowFallbackMaxDistance, kMaximum.ShallowFallbackMaxDistance, defaults.ShallowFallbackMaxDistance);
		value.DeepContextTransitionUnits = Util::ClampFinite(value.DeepContextTransitionUnits, kMinimum.DeepContextTransitionUnits, kMaximum.DeepContextTransitionUnits, defaults.DeepContextTransitionUnits);
		return value;
	}

	/** @brief Suppresses unavailable rendering without changing the selected controls. */
	[[nodiscard]] inline CommonBufferData ForRuntime(Settings selected, bool ready)
	{
		CommonBufferData result = Sanitize(selected);
		if (!ready || selected.UseOpenShadersDepthBehaviour)
			result.ShallowFallbackStrength = 0.0f;
		return result;
	}
}
