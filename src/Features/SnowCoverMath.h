#pragma once

#include <algorithm>
#include <cmath>

namespace SnowCoverMath
{
	constexpr float MonthsPerYear = 12.0f;
	constexpr float MaxWeatherAccumulation = 2.0f;

	/** @brief Interpolates the cyclic snow line between the summer and winter peaks. */
	inline float SeasonalAltitude(float month, unsigned summerMonth, unsigned winterMonth, float summerHeight, float winterHeight)
	{
		if (summerMonth == winterMonth)
			return -0.5f * (summerHeight + winterHeight);
		const float phase = std::fmod(month - static_cast<float>(summerMonth) + MonthsPerYear, MonthsPerYear);
		const float winterPhase = std::fmod(static_cast<float>(winterMonth) - static_cast<float>(summerMonth) + MonthsPerYear, MonthsPerYear);
		const float weight = phase <= winterPhase ? phase / winterPhase : (MonthsPerYear - phase) / (MonthsPerYear - winterPhase);
		return -std::lerp(summerHeight, winterHeight, weight);
	}

	/** @brief Advances weather snow by elapsed game hours, with rain melting twice as quickly. */
	inline float Accumulate(float snow, float hours, bool snowing, bool raining, float snowSpeed, float meltSpeed)
	{
		if (snowing)
			snow += hours * snowSpeed;
		else if (raining)
			snow -= 2.0f * hours * meltSpeed;
		else if (snow > 0.0f)
			snow = std::max(snow - hours * meltSpeed, 0.0f);
		else
			snow = std::min(snow + hours * meltSpeed, 0.0f);
		return std::clamp(snow, -MaxWeatherAccumulation, MaxWeatherAccumulation);
	}
}
