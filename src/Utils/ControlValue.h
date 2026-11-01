#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace Util
{
	/** @brief The item a dropdown shows for a stored value: the value itself, or the nearest item when it lies outside the list. */
	template <class Stored>
	[[nodiscard]] int ComboShownIndex(Stored stored, int itemCount)
	{
		return std::clamp(static_cast<int>(stored), 0, (std::max)(itemCount - 1, 0));
	}

	/** @brief The item a dropdown shows for a stored value that is one of @p values, or @p fallbackIndex when it is none. */
	template <class Stored, class Value, std::size_t Count>
	[[nodiscard]] int ComboValueIndex(const Stored& stored, const std::array<Value, Count>& values, int fallbackIndex)
	{
		for (std::size_t i = 0; i < Count; ++i) {
			if (values[i] == stored)
				return static_cast<int>(i);
		}
		return fallbackIndex;
	}

	/**
	 * @brief Stores what a control shows through a copy, but only when the user changed it.
	 *
	 * An unchanged copy must not be written back: a scene-setting reset changes the stored value itself
	 * while the control still shows the old one, and writing that back would undo the reset. It also
	 * keeps a converted float from drifting by rounding on every frame.
	 *
	 * @param toStored Converts the shown value to the stored one; a static_cast by default.
	 * @return True when the stored value was written.
	 */
	template <class Stored, class Shown, class ToStored>
	bool CommitDisplayedValue(Stored& stored, const Shown& shown, const Shown& displayed, ToStored toStored)
	{
		if (displayed == shown)
			return false;
		stored = toStored(displayed);
		return true;
	}

	template <class Stored, class Shown>
	bool CommitDisplayedValue(Stored& stored, const Shown& shown, const Shown& displayed)
	{
		return CommitDisplayedValue(stored, shown, displayed, [](const Shown& value) { return static_cast<Stored>(value); });
	}
}
