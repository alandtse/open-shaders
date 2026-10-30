#pragma once

#include "Utils/ContentHash.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Util::StandaloneShaderCacheKey
{
	using Defines = std::vector<std::pair<std::string, std::optional<std::string>>>;

	/** @brief Returns the normalized source path relative to the Shaders directory. */
	inline std::filesystem::path SourcePath(const std::filesystem::path& a_path)
	{
		const auto normalized = a_path.lexically_normal();
		std::filesystem::path relative;
		bool inShaders = false;
		for (const auto& part : normalized) {
			if (inShaders)
				relative /= part;
			else if (part == L"Shaders")
				inShaders = true;
		}
		return inShaders ? relative : normalized;
	}

	/** @brief Returns the shader-relative source name with portable separators. */
	inline std::string SourceName(const std::filesystem::path& a_path)
	{
		return SourcePath(a_path).generic_string();
	}

	/** @brief Sorts macros and retains the final definition of each name. */
	inline Defines CanonicalDefines(const Defines& a_defines)
	{
		std::map<std::string, std::optional<std::string>> definitions;
		for (const auto& [name, value] : a_defines)
			definitions[name] = value;
		return { definitions.begin(), definitions.end() };
	}

	/** @brief Identifies portable bytecode by source, entry point, profile, flags and canonical macros. */
	inline ContentHash::Hash128 MakeKey(std::string_view a_source, std::string_view a_entryPoint,
		std::string_view a_profile, uint32_t a_flags, const Defines& a_defines)
	{
		std::string key = "standalone-v1";
		const auto append = [&key](std::string_view value) {
			key.push_back('\0');
			key.append(value);
		};
		append(a_source);
		append(a_entryPoint);
		append(a_profile);
		append(std::to_string(a_flags));
		for (const auto& [name, value] : a_defines) {
			append(name);
			append(value ? "1" : "0");
			if (value)
				append(*value);
		}
		return ContentHash::HashString(key);
	}
}
