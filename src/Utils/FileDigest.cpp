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
	template <class ReadChunk>
	static std::optional<std::string> Sha256Hex(ReadChunk&& readChunk)
	{
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
		while (true) {
			const auto read = readChunk(buffer.data(), buffer.size());
			if (!read)
				return std::nullopt;
			if (*read == 0)
				break;
			if (!BCRYPT_SUCCESS(BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(*read), 0)))
				return std::nullopt;
		}

		std::array<uint8_t, 32> digest{};
		if (!BCRYPT_SUCCESS(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0)))
			return std::nullopt;

		std::string hex;
		hex.reserve(digest.size() * 2);
		for (const auto byte : digest)
			hex += std::format("{:02X}", byte);
		return hex;
	}

	std::optional<std::string> Sha256FileHex(const std::filesystem::path& path)
	{
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs.is_open())
			return std::nullopt;
		return Sha256Hex([&ifs](char* buffer, size_t size) -> std::optional<size_t> {
			ifs.read(buffer, static_cast<std::streamsize>(size));
			if (ifs.bad() || (ifs.fail() && !ifs.eof()))
				return std::nullopt;
			return static_cast<size_t>(ifs.gcount());
		});
	}

	std::optional<std::string> Sha256HandleHex(void* handle)
	{
		LARGE_INTEGER beginning{};
		if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN))
			return std::nullopt;
		return Sha256Hex([handle](char* buffer, size_t size) -> std::optional<size_t> {
			DWORD count = 0;
			if (!ReadFile(handle, buffer, static_cast<DWORD>(size), &count, nullptr))
				return std::nullopt;
			return count;
		});
	}

}
