#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

namespace Util::Settings
{
	/** @brief The setting's display scale, 1 when unset or invalid. */
	template <class Setting>
	double GetCatalogNumericDisplayScale(const Setting& setting)
	{
		return std::isfinite(setting.displayScale) && setting.displayScale > 0.0 ?
		           setting.displayScale :
		           1.0;
	}

	/** @brief Maps a stored value into the control's display space; false when it has no finite image. */
	template <class Setting>
	bool ConvertCatalogNumericStoredToDisplay(const Setting& setting, double storedValue, double& displayValue)
	{
		using EditorSemantic = decltype(setting.editorSemantic);
		using NumericTransform = decltype(setting.numericTransform);
		if (!std::isfinite(storedValue))
			return false;
		if (setting.editorSemantic == EditorSemantic::Generic) {
			displayValue = storedValue;
			return true;
		}
		if (setting.editorSemantic != EditorSemantic::Numeric)
			return false;

		double transformedValue = storedValue;
		switch (setting.numericTransform) {
		case NumericTransform::Identity:
			break;
		case NumericTransform::Log2:
			if (storedValue <= 0.0)
				return false;
			transformedValue = std::log2(storedValue);
			break;
		default:
			return false;
		}

		displayValue = transformedValue * GetCatalogNumericDisplayScale(setting);
		return std::isfinite(displayValue);
	}

	/** @brief Inverse of ConvertCatalogNumericStoredToDisplay. */
	template <class Setting>
	bool ConvertCatalogNumericDisplayToStored(const Setting& setting, double displayValue, double& storedValue)
	{
		using EditorSemantic = decltype(setting.editorSemantic);
		using NumericTransform = decltype(setting.numericTransform);
		if (!std::isfinite(displayValue))
			return false;
		if (setting.editorSemantic == EditorSemantic::Generic) {
			storedValue = displayValue;
			return true;
		}
		if (setting.editorSemantic != EditorSemantic::Numeric)
			return false;

		const double transformedValue = displayValue / GetCatalogNumericDisplayScale(setting);
		switch (setting.numericTransform) {
		case NumericTransform::Identity:
			storedValue = transformedValue;
			break;
		case NumericTransform::Log2:
			storedValue = std::exp2(transformedValue);
			break;
		default:
			return false;
		}
		return std::isfinite(storedValue);
	}

	/** @brief Converts persisted catalog bounds once into the control's finite display range. */
	template <class Setting>
	bool GetCatalogNumericDisplayBounds(const Setting& setting, double& minimum, double& maximum)
	{
		return setting.hasNumericBounds &&
		       ConvertCatalogNumericStoredToDisplay(setting, setting.minimumValue, minimum) &&
		       ConvertCatalogNumericStoredToDisplay(setting, setting.maximumValue, maximum) && minimum < maximum;
	}

	/** @brief Clamps a value into its control's range, since applying bypasses the ImGui bound.
	 *  @return Whether the value was changed. */
	template <class Setting>
	bool ClampCatalogNumericValue(const Setting& setting, nlohmann::json& value)
	{
		using EditorSemantic = decltype(setting.editorSemantic);
		if (!setting.hasNumericBounds || !value.is_number() ||
			setting.editorSemantic != EditorSemantic::Numeric)
			return false;

		const double minimum = setting.minimumValue;
		const double maximum = setting.maximumValue;
		if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum)
			return false;

		const double storedValue = value.get<double>();
		if (!std::isfinite(storedValue) || (storedValue >= minimum && storedValue <= maximum))
			return false;

		if (!value.is_number_integer()) {
			value = std::clamp(storedValue, minimum, maximum);
			return true;
		}
		// An integer control cannot land on a fractional bound, so its range is the whole numbers inside.
		const double lowest = std::ceil(minimum);
		const double highest = std::floor(maximum);
		if (lowest > highest)
			return false;
		const double clamped = std::clamp(storedValue, lowest, highest);
		const double limit = std::ldexp(1.0, std::numeric_limits<std::int64_t>::digits);
		if (clamped < -limit || clamped >= limit)
			return false;
		value = static_cast<std::int64_t>(clamped);
		return true;
	}
}
