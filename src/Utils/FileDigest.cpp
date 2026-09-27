#include "FileDigest.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <memory>
#include <vector>

namespace Util::FileDigest
{
	std::optional<std::string> Sha256FileHex(const std::filesystem::path& path)
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

		// 64 KB chunks, so hashing a large file never reads it into memory.
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
