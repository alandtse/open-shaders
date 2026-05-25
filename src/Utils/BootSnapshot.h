#pragma once

#include "Utils/RestartSettings.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace Util::Settings
{
	namespace detail
	{
		template <typename SettingsT, typename T>
		size_t MemberOffset(T SettingsT::* member) noexcept
		{
			static_assert(std::is_default_constructible_v<SettingsT>);
			SettingsT tmp{};
			const auto* base = reinterpret_cast<const std::byte*>(&tmp);
			const auto* field = reinterpret_cast<const std::byte*>(&(tmp.*member));
			return static_cast<size_t>(field - base);
		}
	}

	template <typename SettingsT>
	class BootSnapshot
	{
	public:
		template <size_t N>
		explicit constexpr BootSnapshot(const RestartTable<SettingsT, N>& table) noexcept :
			table_(table.data()), tableSize_(N)
		{
			static_assert(std::is_standard_layout_v<SettingsT>, "BootSnapshot requires standard-layout Settings for offsetof-based tables.");
			static_assert(std::is_trivially_copyable_v<SettingsT>, "BootSnapshot requires trivially-copyable Settings.");
		}

		void Latch(const SettingsT& live) noexcept
		{
			// Byte-wise copy so padding bytes are reproduced verbatim. Assignment
			// of a trivially-copyable struct copies the object representation
			// (which the C++ standard guarantees for trivially-copyable types),
			// but memcpy makes that intent explicit and removes any compiler
			// latitude that might leave padding indeterminate — `HasPendingChange`
			// uses memcmp on field slices, so any padding-byte drift would
			// surface as a false-positive diff.
			std::memcpy(&bootCopy_, &live, sizeof(SettingsT));
			latched_ = true;
		}

		void LatchIfNeeded(const SettingsT& live) noexcept
		{
			if (!latched_) {
				Latch(live);
			}
		}

		bool IsLatched() const noexcept { return latched_; }

		std::span<const RestartFieldInfo> Fields() const noexcept
		{
			return { table_, tableSize_ };
		}

		const void* RawBoot(std::string_view jsonKey) const noexcept
		{
			if (!latched_) {
				return nullptr;
			}
			const auto* field = FindRestartField(Fields(), jsonKey);
			if (!field) {
				return nullptr;
			}
			return reinterpret_cast<const std::byte*>(&bootCopy_) + field->offset;
		}

		template <typename T>
		const T& Boot(T SettingsT::* member) const noexcept
		{
			static const T kZero{};
			if (!latched_) {
				return kZero;
			}
			const size_t offset = detail::MemberOffset(member);
			return *reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(&bootCopy_) + offset);
		}

		template <typename T>
		bool HasPendingChange(const SettingsT& live, T SettingsT::* member) const noexcept
		{
			if (!latched_) {
				return false;
			}
			const size_t offset = detail::MemberOffset(member);
			return std::memcmp(reinterpret_cast<const std::byte*>(&bootCopy_) + offset,
					   reinterpret_cast<const std::byte*>(&live) + offset,
					   sizeof(T)) != 0;
		}

		bool HasPendingChange(const SettingsT& live, const RestartFieldInfo& field) const noexcept
		{
			if (!latched_ || !field.jsonKey) {
				return false;
			}
			return std::memcmp(reinterpret_cast<const std::byte*>(&bootCopy_) + field.offset,
					   reinterpret_cast<const std::byte*>(&live) + field.offset,
					   field.size) != 0;
		}

		template <typename T>
		const RestartFieldInfo* FindField(T SettingsT::* member) const noexcept
		{
			const size_t offset = detail::MemberOffset(member);
			const size_t size = sizeof(T);
			for (const auto& field : Fields()) {
				if (field.offset == offset && field.size == size) {
					return &field;
				}
			}
			return nullptr;
		}

	private:
		SettingsT bootCopy_{};
		const RestartFieldInfo* table_ = nullptr;
		size_t tableSize_ = 0;
		bool latched_ = false;
	};
}
