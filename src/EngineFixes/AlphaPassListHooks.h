#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AlphaPassList
{
	inline constexpr std::uint8_t LinkedGroup = 1;

	struct NativeSite
	{
		std::uintptr_t rva;
		std::array<std::uint8_t, 18> prefix;
		std::size_t size;
	};

	inline constexpr std::array NativeSites{
		NativeSite{ 0x1347150, { 0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF }, 18 },
		NativeSite{ 0x134A6F0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 }, 16 },
		NativeSite{ 0x1347320, { 0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 }, 10 },
		NativeSite{ 0x1347E60, { 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE }, 16 }
	};

	struct Hook
	{
		void** original;
		void* replacement;
	};

	enum class InstallStatus
	{
		inactive,
		unsupported,
		modified,
		failed,
		installed
	};

	struct InstallResult
	{
		InstallStatus status;
		long error{};
	};

	/** Verify every native site before attaching the append and retirement hooks atomically. */
	template <class Operations>
	InstallResult InstallHooks(bool a_isVR, bool a_supportedVersion, std::uintptr_t a_base,
		std::span<const Hook, NativeSites.size()> a_hooks, const Operations& a_operations)
	{
		if (!a_isVR)
			return { InstallStatus::inactive };
		if (!a_supportedVersion)
			return { InstallStatus::unsupported };
		for (const auto& site : NativeSites) {
			std::array<std::uint8_t, 18> actual{};
			if (!a_operations.Read(a_base + site.rva, std::span(actual).first(site.size)) ||
				!std::equal(site.prefix.begin(), site.prefix.begin() + site.size, actual.begin()))
				return { InstallStatus::modified };
		}
		for (std::size_t i = 0; i < NativeSites.size(); ++i)
			*a_hooks[i].original = reinterpret_cast<void*>(a_base + NativeSites[i].rva);

		auto result = a_operations.Begin();
		// A busy transaction belongs to another installer and must not be aborted here.
		if (result == a_operations.InvalidOperation())
			return { InstallStatus::failed, result };
		if (result == 0)
			result = a_operations.UpdateThread();
		if (result == 0) {
			for (const auto& hook : a_hooks) {
				result = a_operations.Attach(hook.original, hook.replacement);
				if (result != 0)
					break;
			}
		}
		if (result != 0) {
			const auto aborted = a_operations.Abort();
			return { InstallStatus::failed, aborted == 0 ? result : aborted };
		}
		result = a_operations.Commit();
		return { result == 0 ? InstallStatus::installed : InstallStatus::failed, result };
	}

	/** Invalidate cached membership before and after native queue retirement. */
	class RetirementScope
	{
		std::atomic_uint64_t& generation;

	public:
		explicit RetirementScope(std::atomic_uint64_t& a_generation) noexcept : generation(a_generation)
		{
			generation.fetch_add(1, std::memory_order_acq_rel);
		}
		~RetirementScope() { generation.fetch_add(1, std::memory_order_acq_rel); }
		RetirementScope(const RetirementScope&) = delete;
		RetirementScope& operator=(const RetirementScope&) = delete;
	};
}
