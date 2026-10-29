#pragma once

#include "OpenWind/Types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace OpenWind::WindField
{
	inline constexpr std::size_t kTransientImpulseCapacity = 96;
	inline constexpr float kTransientImpulseMaximumDecayTime = 5.0f;
	inline constexpr float kWakeWidestFraction = 0.35f;
	inline constexpr float kWakeTailFraction = 1.0f - kWakeWidestFraction;

	enum class TransientWindSourceType : uint32_t
	{
		DirectionalWave,
		RadialWave,
		DirectionalCone,
		OrientedFlow,
		DirectionalOvalWave,
		AttachedWedge
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
		float orientationRoll{};
	};
	static_assert(sizeof(TransientWindSource) == 80);
	static_assert(std::is_standard_layout_v<TransientWindSource>);
	static_assert(offsetof(TransientWindSource, orientationRoll) == 76);

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

	/** @brief Creates a rounded, banked wake trailing the supplied travel direction. */
	[[nodiscard]] TransientWindSource MakeAttachedWedge(const float3& a_origin,
		const float3& a_forward, const float3& a_right, float a_strength, float a_length,
		float a_halfWidth, float a_downwardReach, float a_edgeSoftness,
		float a_outwardFlow, float a_downwash) noexcept;

	namespace detail
	{
		inline void GetWedgeAxes(const float3& a_forward, float a_roll, float3& a_right, float3& a_up) noexcept
		{
			constexpr float kMinimumDivisor = 1e-4f;
			const float horizontalLength = std::hypot(a_forward.x, a_forward.y);
			const float3 referenceRight = horizontalLength > kMinimumDivisor ?
			                                 float3{ a_forward.y, -a_forward.x, 0.0f } / horizontalLength :
			                                 float3{ 1.0f, 0.0f, 0.0f };
			const float3 referenceUp = referenceRight.Cross(a_forward);
			a_right = referenceRight * std::cos(a_roll) + referenceUp * std::sin(a_roll);
			a_up = a_right.Cross(a_forward);
		}
	}

	/** @brief Returns a wake section's center and elliptical half-axes at a normalized downstream distance. */
	inline void GetAttachedWedgeSection(const TransientWindSource& a_source, float a_along,
		float3& a_center, float3& a_right, float3& a_up) noexcept
	{
		const float directionLength = a_source.direction.Length();
		const float3 forward = std::isfinite(directionLength) && directionLength > 1e-3f ?
		                           a_source.direction / directionLength : float3{};
		float3 right, up;
		detail::GetWedgeAxes(forward, a_source.orientationRoll, right, up);
		const float along = std::clamp(a_along, 0.0f, 1.0f);
		const float longitudinal = (along - kWakeWidestFraction) / kWakeTailFraction;
		const float sectionScale = std::sqrt(std::max(1.0f - longitudinal * longitudinal, 0.0f));
		a_center = a_source.origin - forward * (along * a_source.maxDistance) -
		           up * (along * (a_source.verticalScale - a_source.waveHalfWidth));
		a_right = right * (a_source.collisionRadius * sectionScale);
		a_up = up * (a_source.waveHalfWidth * sectionScale);
	}
}
