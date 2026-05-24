#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace Util::Settings
{
	// Type-erased field descriptor for restart-gated settings.
	//
	// `jsonKey` must match the NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE field name so
	// MCP/RemoteControl can refer to it without per-feature glue.
	struct RestartFieldInfo
	{
		const char* jsonKey = nullptr;
		const char* label = nullptr;
		size_t offset = 0;
		size_t size = 0;
	};

	template <typename SettingsT, size_t N>
	using RestartTable = std::array<RestartFieldInfo, N>;

	inline constexpr const RestartFieldInfo* FindRestartField(std::span<const RestartFieldInfo> fields, std::string_view jsonKey) noexcept
	{
		for (const auto& field : fields) {
			if (field.jsonKey && jsonKey == field.jsonKey) {
				return &field;
			}
		}
		return nullptr;
	}
}

// Convenience macro for building a RestartFieldInfo entry without duplicating
// the member name string. Requires SettingsT to be standard-layout.
#define UTIL_RESTART_FIELD(SettingsT, member, userLabel) \
	Util::Settings::RestartFieldInfo{ #member, userLabel, offsetof(SettingsT, member), sizeof(decltype(SettingsT::member)) }

