#pragma once

#include <xxhash.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

/// Fast non-cryptographic XXH3-128 hashing for shader cache keys.
namespace Util::ContentHash
{
	struct Hash128
	{
		uint64_t high = 0;
		uint64_t low = 0;

		bool operator==(const Hash128&) const = default;

		/** @brief 32 lowercase hex digits, high word first. */
		std::string ToHex() const
		{
			return std::format("{:016x}{:016x}", high, low);
		}
	};

	inline Hash128 HashBytes(const void* a_data, size_t a_size)
	{
		const XXH128_hash_t hash = XXH3_128bits(a_data, a_size);
		return { hash.high64, hash.low64 };
	}

	inline Hash128 HashString(std::string_view a_text)
	{
		return HashBytes(a_text.data(), a_text.size());
	}

	/// Order-sensitive combination of two hashes.
	inline Hash128 CombineHashes(const Hash128& a_first, const Hash128& a_second)
	{
		const std::array<uint64_t, 4> buffer{ a_first.high, a_first.low, a_second.high, a_second.low };
		return HashBytes(buffer.data(), buffer.size() * sizeof(uint64_t));
	}

	/// Content hash of a file's bytes, normalizing CRLF -> LF first so a line-
	/// ending difference alone (CI checkout vs. a user's Windows install) never
	/// changes the digest for meaning-identical content. nullopt on any IO
	/// failure -- callers must treat that as "hash unknown", never "unchanged".
	inline std::optional<Hash128> HashFile(const std::filesystem::path& path)
	{
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs.is_open())
			return std::nullopt;
		std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
		if (ifs.bad())
			return std::nullopt;
		std::string normalized;
		normalized.reserve(raw.size());
		for (size_t i = 0; i < raw.size(); ++i) {
			if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
				continue;
			normalized.push_back(raw[i]);
		}
		return HashString(normalized);
	}
}
