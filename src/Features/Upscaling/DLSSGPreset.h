#pragma once

#include <cstdint>

namespace DLSSGPreset
{
	/** @brief DLSS-G model preset, stored as the value of the driver profile's DLSS-FG preset key. */
	enum class Preset : uint32_t
	{
		kDefault = 0,
		kA = 1,
		kB = 2,
		kC = 3,
	};

	inline constexpr uint32_t kCount = 4;  ///< Number of Preset values, Default included.

	/** @brief What to do to the driver profile's DLSS-FG preset key after the selected preset changes. */
	enum class ProfileAction
	{
		kNone,
		kSet,
		kDelete,
	};

	/**
	 * @brief Plans the driver profile write for a change from a_previous to a_current. Default deletes
	 * the key only when it replaces an explicit preset, so an untouched Default leaves an NVIDIA App
	 * override in place.
	 */
	inline ProfileAction PlanProfileAction(uint32_t a_previous, uint32_t a_current)
	{
		if (a_current == a_previous)
			return ProfileAction::kNone;
		return a_current == static_cast<uint32_t>(Preset::kDefault) ? ProfileAction::kDelete : ProfileAction::kSet;
	}
}
