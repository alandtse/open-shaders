#pragma once

#include <algorithm>
#include <cmath>

// Pure inverse-square-lighting radius / attenuation math, extracted from
// InverseSquareLighting so it can be unit-tested without the game/RE runtime.
// All inputs and outputs are plain floats -- no engine types -- so this header
// compiles standalone into the cpp_tests binary. InverseSquareLighting's static
// methods delegate here.
namespace ISLMath
{
	inline constexpr float DefaultCutoff = 0.05f;
	inline constexpr float DefaultShadowCasterCutoff = 0.022f;

	inline constexpr float Scale = 0.8f;
	inline constexpr float MetresToUnits = 70.f;
	inline constexpr float MetresToUnitsSq = MetresToUnits * MetresToUnits;
	inline constexpr float ScaledUnitsSq = Scale * MetresToUnitsSq;
	inline constexpr float FadeZoneBase = 4.5f * Scale * MetresToUnits;

	inline float SmoothStep(float edge0, float edge1, float x)
	{
		const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// Radius at which the inverse-square falloff reaches `cutoff`. A cutoffOverride
	// of exactly 1.0 means "use the default"; anything else overrides it. NaN
	// results (degenerate inputs) clamp to 1.0 so a light never gets a bad radius.
	inline float CalculateRadius(float intensity, bool shadowCaster, float cutoffOverride, float size)
	{
		float cutoff = shadowCaster ? DefaultShadowCasterCutoff : DefaultCutoff;
		cutoff = cutoffOverride == 1.f ? cutoff : cutoffOverride;
		const float radius = std::sqrt(ScaledUnitsSq * ((2 * intensity - cutoff * size * size) / (2 * cutoff)));
		return std::isnan(radius) ? 1.f : radius;
	}

	inline float GetAttenuation(float distance, float radius, float size)
	{
		const float attenuation = ScaledUnitsSq / (distance * distance + ScaledUnitsSq * size * size / 2);
		const float fadeZone = std::clamp(FadeZoneBase / radius, 0.0f, 1.0f);
		const float fade = SmoothStep(0, radius * fadeZone, radius - distance);
		return attenuation * fade;
	}
}
