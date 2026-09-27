#pragma once

// Fast, non-cryptographic content hashing (XXH3-128) for shader-cache keys.
// No adversarial threat model applies here -- the disk-cache loader already
// trusts anything sitting at the expected path unconditionally (see
// ShaderCache.cpp's use of D3DCOMPILE_SKIP_VALIDATION) -- so speed wins over
// collision-resistance against a deliberate attacker.
// Sha256FileHex is the deliberate exception: a cryptographic digest for pinning a
// third-party runtime DLL to a validated build, never a cache key.

#include <Windows.h>
#include <bcrypt.h>
#include <xxhash.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

	/// SHA-256 of a file's raw bytes -- no line-ending normalization -- as uppercase
	/// hex, for identifying a build of a third-party binary. nullopt on any IO or CNG
	/// failure: callers must treat that as "digest unknown", never "validated".
	inline std::optional<std::string> Sha256FileHex(const std::filesystem::path& path)
	{
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs.is_open())
			return std::nullopt;

		struct AlgorithmDeleter
		{
			void operator()(BCRYPT_ALG_HANDLE handle) const { BCryptCloseAlgorithmProvider(handle, 0); }
		};
		struct HashDeleter
		{
			void operator()(BCRYPT_HASH_HANDLE handle) const { BCryptDestroyHash(handle); }
		};
		using Algorithm = std::unique_ptr<std::remove_pointer_t<BCRYPT_ALG_HANDLE>, AlgorithmDeleter>;
		using Hash = std::unique_ptr<std::remove_pointer_t<BCRYPT_HASH_HANDLE>, HashDeleter>;

		BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
			return std::nullopt;
		Algorithm algorithm(rawAlgorithm);
		DWORD objectSize = 0, objectWritten = 0;
		if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &objectWritten, 0)))
			return std::nullopt;
		std::vector<uint8_t> object(objectSize);
		BCRYPT_HASH_HANDLE rawHash = nullptr;
		if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.get(), &rawHash, object.data(), objectSize, nullptr, 0, 0)))
			return std::nullopt;
		Hash hash(rawHash);

		// 165 MB dlssnr builds are the norm here, so the 64 KB chunks stay off the heap.
		std::array<char, 64 * 1024> buffer{};
		while (ifs) {
			ifs.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const auto read = ifs.gcount();
			if (read > 0 && !BCRYPT_SUCCESS(BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0)))
				return std::nullopt;
		}
		if (ifs.bad())
			return std::nullopt;

		std::array<uint8_t, 32> digest{};
		if (!BCRYPT_SUCCESS(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0)))
			return std::nullopt;

		std::string hex;
		hex.reserve(digest.size() * 2);
		for (const auto byte : digest)
			hex += std::format("{:02X}", byte);
		return hex;
	}
}
