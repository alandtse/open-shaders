#include "WinApi.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <thread>
#include <vector>

namespace Util
{
	uint32_t GetLogicalCoreCount()
	{
		static const uint32_t cached = std::max(1u, std::thread::hardware_concurrency());
		return cached;
	}

	std::optional<REL::Version> GetDllVersion(const std::wstring& dllPath)
	{
		DWORD handle = 0;
		DWORD size = GetFileVersionInfoSize(dllPath.c_str(), &handle);
		if (size == 0) {
			return std::nullopt;
		}

		std::vector<BYTE> buffer(size);
		if (!GetFileVersionInfo(dllPath.c_str(), handle, size, buffer.data())) {
			return std::nullopt;
		}

		VS_FIXEDFILEINFO* fileInfo = nullptr;
		UINT fileInfoSize = 0;
		if (!VerQueryValue(buffer.data(), L"\\", reinterpret_cast<void**>(&fileInfo), &fileInfoSize)) {
			return std::nullopt;
		}

		if (fileInfoSize == sizeof(VS_FIXEDFILEINFO)) {
			return REL::Version(HIWORD(fileInfo->dwFileVersionMS), LOWORD(fileInfo->dwFileVersionMS), HIWORD(fileInfo->dwFileVersionLS), LOWORD(fileInfo->dwFileVersionLS));
		}

		return std::nullopt;
	}

	uint32_t GetPerformanceCoreCount()
	{
		static const uint32_t cached = []() -> uint32_t {
			const uint32_t fallback = GetLogicalCoreCount();

			DWORD size = 0;
			GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
				return fallback;
			}

			std::vector<uint8_t> buffer(size);
			auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
			if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &size)) {
				return fallback;
			}

			BYTE maxEfficiencyClass = 0;
			for (DWORD offset = 0; offset < size;) {
				auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
				maxEfficiencyClass = std::max(maxEfficiencyClass, entry->Processor.EfficiencyClass);
				offset += entry->Size;
			}

			uint32_t count = 0;
			for (DWORD offset = 0; offset < size;) {
				auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
				if (entry->Processor.EfficiencyClass == maxEfficiencyClass) {
					for (WORD group = 0; group < entry->Processor.GroupCount; ++group) {
						count += static_cast<uint32_t>(std::popcount(entry->Processor.GroupMask[group].Mask));
					}
				}
				offset += entry->Size;
			}

			return count > 0 ? count : fallback;
		}();

		return cached;
	}
}  // namespace Util
