#pragma once

// Fast, non-cryptographic content hashing (XXH3-128) for shader-cache keys.
// No adversarial threat model applies here -- the disk-cache loader already
// trusts anything sitting at the expected path unconditionally (see
// ShaderCache.cpp's use of D3DCOMPILE_SKIP_VALIDATION) -- so speed wins over
// collision-resistance against a deliberate attacker.

#include <xxhash.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace Util::ContentHash
{
	struct Hash128
	{
		uint64_t high = 0;
		uint64_t low = 0;

		bool operator==(const Hash128&) const = default;

		std::string ToHex() const
		{
			return std::format("{:016x}{:016x}", high, low);
		}
	};

	inline Hash128 HashBytes(const void* data, size_t size)
	{
		const XXH128_hash_t h = XXH3_128bits(data, size);
		return Hash128{ h.high64, h.low64 };
	}

	inline Hash128 HashString(std::string_view s)
	{
		return HashBytes(s.data(), s.size());
	}

	/// Combine two hashes into one, order-sensitive. For folding a dependency
	/// tree Merkle-style: CombineHashes(selfHash, childHash) per child, in a
	/// stable (e.g. sorted-by-path) order so the result doesn't depend on
	/// filesystem iteration order.
	inline Hash128 CombineHashes(const Hash128& a, const Hash128& b)
	{
		const std::array<uint64_t, 4> buf{ a.high, a.low, b.high, b.low };
		return HashBytes(buf.data(), buf.size() * sizeof(uint64_t));
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
