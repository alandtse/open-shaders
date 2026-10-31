#pragma once

#include <algorithm>

namespace Util
{
	/** @brief The item a dropdown shows for a stored value: the value itself, or the nearest item when it lies outside the list. */
	template <class Stored>
	[[nodiscard]] int ComboShownIndex(Stored stored, int itemCount)
	{
		return std::clamp(static_cast<int>(stored), 0, (std::max)(itemCount - 1, 0));
	}

	/**
	 * @brief Stores the dropdown's pick when the user changed it, and reports whether it did.
	 *
	 * An unchanged pick must not be written back: a scene-setting reset changes the stored value
	 * itself while the dropdown still shows the old item, and writing that item back would undo it.
	 */
	template <class Stored>
	bool CommitComboIndex(Stored& stored, int shownIndex, int pickedIndex)
	{
		if (pickedIndex == shownIndex)
			return false;
		stored = static_cast<Stored>(pickedIndex);
		return true;
	}
}
