#pragma once

#include <Windows.h>
#include <array>
#include <cstddef>

namespace LightReadSafety
{
	/** Check the engine's light-read span without querying the surrounding allocation. */
	template <std::size_t Size>
	bool IsReadable(const void* pointer) noexcept
	{
		static_assert(Size > 0);
		std::array<std::byte, Size> snapshot;
		SIZE_T copied = 0;
		return ::ReadProcessMemory(::GetCurrentProcess(), pointer, snapshot.data(), snapshot.size(), &copied) && copied == Size;
	}
}
