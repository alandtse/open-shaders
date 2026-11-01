#pragma once

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <type_traits>

namespace Util
{
	/** @brief The item a dropdown shows for a stored value: the value itself, or the nearest item when it lies outside the list. */
	template <class Stored>
	[[nodiscard]] int ComboShownIndex(Stored stored, int itemCount)
	{
		return std::clamp(static_cast<int>(stored), 0, (std::max)(itemCount - 1, 0));
	}

	/**
	 * @brief Whether @p labelCount labels fit a dropdown over a setting of type @p Stored.
	 *
	 * An enum setting needs one label per enumerator, with enumerator i holding value i, because the
	 * dropdown stores the picked index. Any other type has no list to check. Compile time only.
	 */
	template <class Stored>
	[[nodiscard]] constexpr bool ComboLabelsMatch(std::size_t labelCount)
	{
		if constexpr (std::is_enum_v<Stored>) {
			const auto& values = magic_enum::enum_values<Stored>();
			if (labelCount != values.size())
				return false;
			for (std::size_t i = 0; i < values.size(); ++i) {
				if (static_cast<long long>(magic_enum::enum_integer(values[i])) != static_cast<long long>(i))
					return false;
			}
		}
		return true;
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
