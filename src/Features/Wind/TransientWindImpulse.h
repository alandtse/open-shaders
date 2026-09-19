#pragma once

#include <cstddef>
#include <span>
#include <type_traits>

namespace WindField
{
	inline constexpr std::size_t kTransientImpulseCapacity = 96;
	inline constexpr float kTransientImpulseMaximumDecayTime = 5.0f;

	enum class TransientWindSourceType : uint32_t
	{
		DirectionalWave,
		RadialWave,
		DirectionalCone,
		OrientedFlow,
		DirectionalOvalWave
	};

	struct TransientWindSource
	{
		float3 origin{};
		float wavefrontDistance{};
		float3 direction{};
		float strength{};
		float maxDistance{};
		float waveHalfWidth{};
		float propagationSpeed{};
		float coneSpreadTangent{};
		float decayTime{};
		float collisionRadius{};
		float sideSpreadTangent{};
		float sideStrength{};
		TransientWindSourceType type{};
		float verticalScale{ 1.0f };
		float falloffWidth{};
		float padding{};
	};
	static_assert(sizeof(TransientWindSource) == 80);
	static_assert(std::is_standard_layout_v<TransientWindSource>);

	struct TransientImpulseSample
	{
		float3 velocity{};
		float intensity{};
	};

	/** @brief A zero a_impulse.direction produces a radial wave instead of a directional one. */
	[[nodiscard]] TransientImpulseSample SampleTransientImpulse(
		const float3& a_worldPosition, const TransientWindSource& a_impulse) noexcept;

	[[nodiscard]] TransientImpulseSample SampleTransientImpulses(
		const float3& a_worldPosition, std::span<const TransientWindSource> a_impulses) noexcept;

	[[nodiscard]] TransientWindSource MakeDirectionalWave(const float3& a_origin,
		const float3& a_direction, float a_strength, float a_maxDistance, float a_waveHalfWidth,
		float a_propagationSpeed, float a_coneCosine, float a_decayTime) noexcept;

	[[nodiscard]] TransientWindSource MakeRadialWave(const float3& a_origin,
		const float3& a_fallbackDirection, float a_strength, float a_maxDistance,
		float a_waveHalfWidth, float a_propagationSpeed, float a_decayTime) noexcept;

	/** @brief Creates a filled directional cone suitable for a frame-updated attached source. */
	[[nodiscard]] TransientWindSource MakeDirectionalCone(const float3& a_origin,
		const float3& a_direction, float a_strength, float a_maxDistance,
		float a_coneCosine) noexcept;

	/** @brief Creates an oriented bow-wave and wake volume around a moving source. */
	[[nodiscard]] TransientWindSource MakeOrientedFlow(const float3& a_origin,
		const float3& a_direction, float a_strength, float a_radius, float a_bowLength,
		float a_wakeLength) noexcept;
}
