#pragma once

#include "Utils/MathUtils.h"

#include <algorithm>
#include <cstdint>

namespace FurShellLimits
{
	inline constexpr Util::Range LengthRange{ 0.05f, 6.0f };
	inline constexpr Util::Range BodyLengthRange{ 0.05f, 3.0f };
	inline constexpr Util::Range DroopRange{ 0.0f, 2.0f };
	inline constexpr Util::Range RootThresholdRange{ 0.0f, 1.0f };
	inline constexpr Util::Range TipThresholdRange{ 0.0f, 1.0f };
	inline constexpr Util::Range RootDarkeningRange{ 0.0f, 1.0f };
	inline constexpr Util::Range ShellColorRange{ 0.0f, 1.0f };
	inline constexpr Util::Range FadeStartRange{ 0.0f, 8000.0f };
	inline constexpr Util::Range FadeEndRange{ 0.0f, 8000.0f };
	inline constexpr Util::Range MinFurPixelsRange{ 0.0f, 16.0f };
	inline constexpr Util::Range WindStrengthRange{ 0.0f, 2.0f };
	inline constexpr Util::Range WetStrengthRange{ 0.0f, 1.0f };
	inline constexpr Util::Range TouchStrengthRange{ 0.0f, 2.0f };
	/** @brief Lowest length the global slider offers; loaded files and overrides may go down to LengthRange. */
	inline constexpr float kMinGlobalSliderLength = 0.1f;

	/** @brief Clamps the values shared by the global settings and a per-texture override to the ranges the renderer accepts. */
	template <class T>
	void ClampShellValues(T& a_values, uint32_t a_maxShells)
	{
		a_values.ShellCount = std::clamp(a_values.ShellCount, 1u, a_maxShells);
		const T defaults{};
		a_values.Length = Util::ClampFinite(a_values.Length, LengthRange.minimum, LengthRange.maximum, defaults.Length);
		a_values.Droop = Util::ClampFinite(a_values.Droop, DroopRange.minimum, DroopRange.maximum, defaults.Droop);
		a_values.RootThreshold = Util::ClampFinite(a_values.RootThreshold, RootThresholdRange.minimum, RootThresholdRange.maximum, defaults.RootThreshold);
		a_values.TipThreshold = Util::ClampFinite(a_values.TipThreshold, TipThresholdRange.minimum, TipThresholdRange.maximum, defaults.TipThreshold);
		a_values.RootDarkening = Util::ClampFinite(a_values.RootDarkening, RootDarkeningRange.minimum, RootDarkeningRange.maximum, defaults.RootDarkening);
		a_values.ShellColor = Util::ClampFinite(a_values.ShellColor, ShellColorRange.minimum, ShellColorRange.maximum, defaults.ShellColor);
		a_values.FadeStart = Util::ClampFinite(a_values.FadeStart, FadeStartRange.minimum, FadeStartRange.maximum, defaults.FadeStart);
		a_values.FadeEnd = Util::ClampFinite(a_values.FadeEnd, FadeEndRange.minimum, FadeEndRange.maximum, defaults.FadeEnd);
	}

	/** @brief ClampShellValues plus the values only the global settings have. */
	template <class T>
	void ClampSettings(T& a_settings, uint32_t a_maxShells)
	{
		ClampShellValues(a_settings, a_maxShells);
		const T defaults{};
		a_settings.BodyLength = Util::ClampFinite(a_settings.BodyLength, BodyLengthRange.minimum, BodyLengthRange.maximum, defaults.BodyLength);
		a_settings.MinFurPixels = Util::ClampFinite(a_settings.MinFurPixels, MinFurPixelsRange.minimum, MinFurPixelsRange.maximum, defaults.MinFurPixels);
		a_settings.WindStrength = Util::ClampFinite(a_settings.WindStrength, WindStrengthRange.minimum, WindStrengthRange.maximum, defaults.WindStrength);
		a_settings.WetStrength = Util::ClampFinite(a_settings.WetStrength, WetStrengthRange.minimum, WetStrengthRange.maximum, defaults.WetStrength);
		a_settings.TouchStrength = Util::ClampFinite(a_settings.TouchStrength, TouchStrengthRange.minimum, TouchStrengthRange.maximum, defaults.TouchStrength);
	}
}
