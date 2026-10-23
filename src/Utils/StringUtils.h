// Self-contained string/path helpers.
//
// Header-only and dependency-free (no PCH, RE, or SKSE) so the pure logic can be
// unit-tested directly in tests/cpp. Heavier string formatting lives in Format.h.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Util
{
	/** @brief Checks for one nonempty path component without separators, a drive prefix, or dot navigation. */
	inline bool IsSinglePathComponent(std::string_view name)
	{
		return !name.empty() && name.find_first_of("/\\:") == std::string_view::npos && name != "." && name != "..";
	}

	/** @brief Joins a relative path to a base, rejecting roots and parent navigation. */
	inline std::filesystem::path JoinRelativePath(const std::filesystem::path& base, const std::filesystem::path& path)
	{
		if (path.is_absolute() || path.has_root_path())
			throw std::invalid_argument("Path must be relative");
		for (const auto& part : path)
			if (part == "..")
				throw std::invalid_argument("Parent path components are not allowed");
		return base / path;
	}

	/** @brief Appends trimmed, nonempty lines after removing hash comments. */
	inline void ReadConfigLines(std::istream& stream, std::vector<std::string>& lines)
	{
		std::string line;
		while (std::getline(stream, line)) {
			line = line.substr(0, line.find('#'));
			const auto begin = line.find_first_not_of(" \t\r\n");
			if (begin != std::string::npos)
				lines.push_back(line.substr(begin, line.find_last_not_of(" \t\r\n") - begin + 1));
		}
	}

	/** Encodes a filesystem path as UTF-8 without narrowing Unicode filenames. */
	inline std::string PathToUtf8(const std::filesystem::path& path)
	{
		const auto text = path.u8string();
		return { reinterpret_cast<const char*>(text.data()), text.size() };
	}

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

	/** @brief Case-insensitive equality for two strings. */
	inline bool IEquals(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
			return std::tolower(static_cast<unsigned char>(ca)) == std::tolower(static_cast<unsigned char>(cb));
		});
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

	/** @brief Uppercases the ASCII letters in @p a_str, returning a new string. */
	inline std::string ToUpperAscii(std::string_view a_str)
	{
		std::string result;
		result.reserve(a_str.size());
		for (char c : a_str)
			result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		return result;
	}

	/** @brief Case-insensitive (ASCII) test that @p a_text begins with @p a_prefix. */
	inline bool IStartsWith(std::string_view a_text, std::string_view a_prefix)
	{
		return a_text.size() >= a_prefix.size() && IEquals(a_text.substr(0, a_prefix.size()), a_prefix);
	}

	/** @brief Case-insensitive (ASCII) test that @p a_text ends with @p a_suffix. */
	inline bool IEndsWith(std::string_view a_text, std::string_view a_suffix)
	{
		return a_text.size() >= a_suffix.size() && IEquals(a_text.substr(a_text.size() - a_suffix.size()), a_suffix);
	}

	/** @brief Case-insensitive (ASCII) test that @p a_text contains @p a_needle; an empty needle is always found. */
	inline bool IContains(std::string_view a_text, std::string_view a_needle)
	{
		return std::search(a_text.begin(), a_text.end(), a_needle.begin(), a_needle.end(), [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		}) != a_text.end() ||
		       a_needle.empty();
	}

	/** @brief The part of @p a_text without leading and trailing ASCII whitespace (space, tab, CR, LF, FF, VT). */
	inline std::string_view TrimAscii(std::string_view a_text)
	{
		constexpr std::string_view whitespace = " \t\r\n\f\v";
		const auto first = a_text.find_first_not_of(whitespace);
		if (first == std::string_view::npos)
			return {};
		return a_text.substr(first, a_text.find_last_not_of(whitespace) - first + 1);
	}
}
