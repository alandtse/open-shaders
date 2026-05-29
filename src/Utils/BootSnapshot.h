#pragma once

#include "Utils/RestartSettings.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

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
			// offsetof is conditionally-supported on non-standard-layout types, but MSVC
			// computes correct offsets for scalar fields -- so std::string members are fine.
			// Only polymorphic types are rejected (a vtable would shift every offset).
			static_assert(!std::is_polymorphic_v<SettingsT>, "BootSnapshot does not support polymorphic Settings (member offsets are not stable).");
			static_assert(std::is_copy_assignable_v<SettingsT>, "BootSnapshot requires copy-assignable Settings.");
			static_assert(std::is_default_constructible_v<SettingsT>,
				"BootSnapshot requires default-constructible Settings (bootCopy_ default-inits and detail::MemberOffset constructs a temporary).");
		}

		void Latch(const SettingsT& live) noexcept(std::is_nothrow_copy_assignable_v<SettingsT>)
		{
			if constexpr (std::is_trivially_copyable_v<SettingsT>) {
				// Trivially-copyable fast path: memcpy preserves padding bytes
				// verbatim. Matters when a registered restart field's *type*
				// contains padding -- HasPendingChange's memcmp would otherwise
				// see false-positive diffs from uninitialized padding bytes.
				std::memcpy(&bootCopy_, &live, sizeof(SettingsT));
			} else {
				// Copy-assign for Settings with non-trivial members (e.g. the
				// std::string formula fields in ShadowCasterManager::Settings).
				// Registered restart fields must still be trivially comparable
				// for HasPendingChange's per-field memcmp to be meaningful;
				// std::string in the outer struct is fine as long as it isn't
				// registered (it isn't -- formulas are runtime-tunable).
				bootCopy_ = live;
			}
			latched_ = true;
		}

		void LatchIfNeeded(const SettingsT& live) noexcept(noexcept(std::declval<BootSnapshot&>().Latch(live)))
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
