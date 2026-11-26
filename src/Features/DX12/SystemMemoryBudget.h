#pragma once

#include <Windows.h>
#include <cstdint>

namespace DX12
{
	inline bool HasAvailableSystemMemory(uint64_t bytes)
	{
		MEMORYSTATUSEX memory{};
		memory.dwLength = sizeof(memory);
		return GlobalMemoryStatusEx(&memory) && bytes <= memory.ullAvailPhys && bytes <= memory.ullAvailPageFile;
	}
}
