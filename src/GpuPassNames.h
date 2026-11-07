#pragma once

#include <array>
#include <cstddef>

namespace GpuPassDetail
{
	/** @brief A fixed set of literal pass names, selected by index. */
	template <std::size_t N>
	struct PassNames
	{
		std::array<const char*, N> names;

		/** @brief Keeps an out-of-range index on the last name instead of reading past the table. */
		constexpr std::size_t Clamp(std::size_t index) const { return index < N ? index : N - 1; }
	};

	/** @brief Builds the name table at compile time. */
	template <class... Names>
	constexpr PassNames<sizeof...(Names)> MakePassNames(Names... names)
	{
		static_assert(sizeof...(Names) > 0);
		return { { names... } };
	}
}
