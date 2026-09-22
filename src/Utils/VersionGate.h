#pragma once

#include <optional>

namespace Util
{
	/**
	 * @brief True when a version was read and is below the minimum; an unreadable version never blocks.
	 * @tparam Version Any lexicographically ordered version type (e.g. REL::Version).
	 */
	template <class Version>
	[[nodiscard]] constexpr bool IsBelowMinimum(const std::optional<Version>& a_found, const Version& a_minimum)
	{
		return a_found && *a_found < a_minimum;
	}
}
