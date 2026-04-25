#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace Util
{
	std::optional<REL::Version> GetDllVersion(const std::wstring& dllPath);

	// Returns logical processor count, clamped to at least 1.
	uint32_t GetLogicalCoreCount();

	// Returns the logical processor count on the highest-efficiency cores.
	// On non-hybrid CPUs this falls back to hardware_concurrency().
	uint32_t GetPerformanceCoreCount();
}  // namespace Util
