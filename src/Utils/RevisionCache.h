#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace Util
{
	/** @brief Holds a value built from settings and rebuilds it only when their revision counter changes. */
	template <class T>
	class RevisionCache
	{
	public:
		/** @brief Returns the cached value, calling a_make() first when nothing is cached or a_revision differs from the cached one. */
		template <class Make>
		const T& Get(std::uint64_t a_revision, Make&& a_make)
		{
			if (revision != a_revision) {
				value = std::forward<Make>(a_make)();
				revision = a_revision;
			}
			return value;
		}

	private:
		T value{};
		std::optional<std::uint64_t> revision;
	};
}
