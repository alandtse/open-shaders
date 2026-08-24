#pragma once

#include <cstddef>
#include <type_traits>

namespace WindField
{
	inline constexpr std::size_t kTransientImpulseCapacity = 4;
	inline constexpr float kTransientImpulseMaximumDecayTime = 5.0f;

	/** @brief A directional pressure wave traveling through the shared 3D wind field. */
	struct TransientImpulse
	{
		float3 origin{};
		float wavefrontDistance{};
		float3 direction{};
		float strength{};
		float maxDistance{};
		float waveHalfWidth{};
		float propagationSpeed{};
		float coneCosine{};
		float decayTime{};
		float3 padding{};
	};
	static_assert(sizeof(TransientImpulse) == 64);
	static_assert(std::is_standard_layout_v<TransientImpulse>);

	/** @brief The XYZ velocity and normalized visualization intensity contributed by one impulse. */
	struct TransientImpulseSample
	{
		float3 velocity{};
		float intensity{};
	};

	/**
	 * @brief Samples one moving directional pressure wave at an absolute world position.
	 * @param a_worldPosition Absolute position in Skyrim's Z-up world space.
	 * @param a_impulse Current wavefront state and rank-derived propagation parameters.
	 * @return Additive XYZ velocity and normalized local wave intensity.
	 */
	[[nodiscard]] TransientImpulseSample SampleTransientImpulse(
		const float3& a_worldPosition, const TransientImpulse& a_impulse) noexcept;
}
