#include "TransientWindImpulse.h"

#include <algorithm>
#include <cmath>

namespace WindField
{
	namespace
	{
		constexpr float kMinimumDivisor = 1e-4f;

		float Smoothstep(float a_minimum, float a_maximum, float a_value) noexcept
		{
			const float range = std::max(a_maximum - a_minimum, kMinimumDivisor);
			const float amount = std::clamp((a_value - a_minimum) / range, 0.0f, 1.0f);
			return amount * amount * (3.0f - 2.0f * amount);
		}

		float3 NormalizeDirection(const float3& a_direction) noexcept
		{
			const float lengthSquared = a_direction.x * a_direction.x + a_direction.y * a_direction.y +
			                            a_direction.z * a_direction.z;
			return lengthSquared > 1e-6f ? a_direction / std::sqrt(lengthSquared) : float3{};
		}
	}

	TransientImpulseSample SampleTransientImpulse(
		const float3& a_worldPosition, const TransientImpulse& a_impulse) noexcept
	{
		if (!std::isfinite(a_impulse.strength) || a_impulse.strength <= 0.0f ||
			!std::isfinite(a_impulse.maxDistance) || a_impulse.maxDistance <= 0.0f) {
			return {};
		}

		const float3 direction = NormalizeDirection(a_impulse.direction);
		const float3 offset = a_worldPosition - a_impulse.origin;
		const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
		const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
		if (distance > a_impulse.maxDistance) {
			return {};
		}
		const float alignment = distanceSquared > 1e-6f ?
		                            (offset.x * direction.x + offset.y * direction.y + offset.z * direction.z) / distance :
		                            1.0f;
		const float angularFalloff = Smoothstep(std::clamp(a_impulse.coneCosine, -1.0f, 1.0f), 1.0f, alignment);
		const float waveHalfWidth = std::max(std::abs(a_impulse.waveHalfWidth), kMinimumDivisor);
		const float waveDistance = std::max(a_impulse.wavefrontDistance, 0.0f);
		const float distanceFromWavefront = distance - waveDistance;
		const float decayTime = std::clamp(
			std::isfinite(a_impulse.decayTime) ? a_impulse.decayTime : 0.0f, 0.0f,
			kTransientImpulseMaximumDecayTime);
		const float propagationSpeed =
			std::isfinite(a_impulse.propagationSpeed) ? std::max(a_impulse.propagationSpeed, 0.0f) : 0.0f;
		const float trailingWidth = waveHalfWidth + propagationSpeed * decayTime;
		const float localWaveWidth = distanceFromWavefront < 0.0f ? trailingWidth : waveHalfWidth;
		const float waveFalloff =
			1.0f - Smoothstep(0.0f, localWaveWidth, std::abs(distanceFromWavefront));
		const float distanceFalloff = 1.0f - Smoothstep(0.0f, a_impulse.maxDistance, distance);
		const float intensity = std::clamp(angularFalloff * waveFalloff * distanceFalloff, 0.0f, 1.0f);
		return { direction * (a_impulse.strength * intensity), intensity };
	}
}
