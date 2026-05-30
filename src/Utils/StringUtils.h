// Self-contained string/path helpers.
//
// Header-only and dependency-free (no PCH, RE, or SKSE) so the pure logic can be
// unit-tested directly in tests/cpp. Heavier string formatting lives in Format.h.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Util
{
	/**
	 * @brief Lowercases the ASCII letters in @p a_str, returning a new string.
	 *
	 * The C++ standard library has no whole-string lowercase, so this wraps the
	 * canonical std::tolower idiom. Locale-independent for ASCII; suitable for
	 * case-insensitive matching of asset paths.
	 *
	 * @param a_str Input text.
	 * @return A lowercased copy of @p a_str.
	 */
	inline std::string ToLowerAscii(std::string_view a_str)
	{
		std::string result;
		result.reserve(a_str.size());
		for (char c : a_str)
			result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		return result;
	}

	/**
	 * @brief Extracts the lowercased filename stem from a path, requiring a given extension.
	 *
	 * Uses std::filesystem to split the path (matching the extension+stem idiom used
	 * elsewhere in the codebase, e.g. TerrainShadows / FeatureIssues), verifies the
	 * extension case-insensitively, then lowercases the stem. Used to match asset
	 * filenames (e.g. a `.dds` texture or `.ini` config) against keyed lookup tables.
	 *
	 * @param a_path Path to a file, absolute or relative. Both `\` and `/` separators
	 *               are accepted on every platform.
	 * @param a_extension Required extension including the leading dot, e.g. ".dds" or ".ini".
	 * @return The lowercased stem, or std::nullopt when the extension doesn't match or
	 *         the stem is empty. Note std::filesystem treats a leading-dot-only name
	 *         (e.g. ".dds") as a stem with no extension, so those are rejected here too.
	 */
	inline std::optional<std::string> GetLowercaseStem(std::string_view a_path, std::string_view a_extension)
	{
		// Normalize `\` to `/` before parsing: game asset paths use backslashes, but
		// std::filesystem only treats them as separators on Windows. This keeps the
		// helper separator-agnostic on every platform (matching the hand-rolled code
		// it replaced) so the unit tests aren't Windows-only.
		std::string normalized(a_path);
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		const std::filesystem::path path = normalized;

		const std::string extension = path.extension().string();
		if (extension.size() != a_extension.size() ||
			!std::equal(extension.begin(), extension.end(), a_extension.begin(),
				[](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); }))
			return std::nullopt;

		const std::string stem = path.stem().string();
		if (stem.empty())
			return std::nullopt;

		return ToLowerAscii(stem);
	}
}
