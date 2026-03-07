#include "Common/SharedData.hlsli"

namespace InverseSquareLighting
{
	static const float SCALE = 0.8f;
	static const float METRES_TO_UNITS = 70.f;
	static const float METRES_TO_UNITS_SQ = METRES_TO_UNITS * METRES_TO_UNITS;
	static const float SCALED_UNITS_SQ = SCALE * METRES_TO_UNITS_SQ;

	float GetAttenuation(float distance, LightLimitFix::Light light)
	{
		float isEnabled = 1.0f - float((light.lightFlags & LightLimitFix::LightFlags::Disabled) != 0);
		float isInvSq = float((light.lightFlags & LightLimitFix::LightFlags::InverseSquare) != 0);

		float safeDistanceSq = max(distance * distance, EPSILON_DIVISION);
		float safeSizeBias = max(light.sizeBias, 0.0f);
		float invSq = SCALED_UNITS_SQ * rcp(max(safeDistanceSq + safeSizeBias, EPSILON_DIVISION));
		float safeRadius = max(light.radius, 1e-3f);
		float safeFadeZone = clamp(light.fadeZone, 0.0f, 1e3f);
		float t = saturate((safeRadius - distance) * safeFadeZone);
		float fastSmoothstep = t * t * (3.0f - 2.0f * t);
		invSq *= fastSmoothstep;

		float safeInvRadius = max(light.invRadius, 0.0f);
		float intensityFactor = saturate(distance * safeInvRadius);
		float reg = 1.0f - intensityFactor * intensityFactor;

		float attenuation = lerp(reg, invSq, isInvSq) * isEnabled;
		return min(attenuation, 1.0e6f);
	}
}
