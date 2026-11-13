#pragma once

#include "Utils/FileDigest.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
	/** @brief Stores content-addressed DDS bytes with bounded, atomic publication. */
	class MaterialDiskCache
	{
	public:
		enum class StoreResult
		{
			Stored,
			Existing,
			Unavailable
		};
		explicit MaterialDiskCache(std::filesystem::path a_directory,
			uint64_t a_limit = 128ull * 1024 * 1024, std::string a_extension = ".dds") :
			directory(std::move(a_directory)), limit(a_limit), extension(std::move(a_extension))
		{
			if (extension != ".dds" && extension != ".rgc")
				throw std::invalid_argument("Unsupported native cache extension");
		}

		/** @brief Background-only bounded listing; each item must be read and hash-verified. */
		std::vector<std::string> ListDigests(size_t a_limit = 256)
		{
			std::scoped_lock lock(mutex);
			std::vector<std::string> digests;
			if (!a_limit)
				return digests;
			std::error_code error;
			const auto status = std::filesystem::symlink_status(directory, error);
			if (error || !std::filesystem::is_directory(status) ||
				std::filesystem::is_symlink(status))
				return digests;
			for (std::filesystem::directory_iterator it(directory, error), end;
				!error && it != end && digests.size() < a_limit; it.increment(error)) {
				const auto name = it->path().filename().string();
				if (name.size() != 64 + extension.size() ||
					name.substr(64) != extension || !ValidDigest(name.substr(0, 64)))
					continue;
				if (it->is_symlink(error) || error || !it->is_regular_file(error) || error) {
					error.clear();
					continue;
				}
				digests.push_back(name.substr(0, 64));
			}
			std::ranges::sort(digests);
			return digests;
		}

		/** @brief Reads only bytes whose SHA-256 matches their content-addressed name. */
		std::optional<std::vector<uint8_t>> Read(const std::string& a_digest, uint64_t a_maxBytes)
		{
			std::scoped_lock lock(mutex);
			if (!ValidDigest(a_digest))
				return std::nullopt;
			auto bytes = ReadVerified(directory / (a_digest + extension), a_digest, a_maxBytes);
			if (bytes)
				Remember(a_digest);
			else
				known.erase(a_digest);
			return bytes;
		}

		/** @brief Indicates a previously verified entry; reads still verify its content. */
		bool Known(const std::string& a_digest)
		{
			std::scoped_lock lock(mutex);
			return known.contains(a_digest);
		}

		/** @brief Validates temporary content before publishing it under its digest. */
		StoreResult Store(const std::string& a_digest, std::span<const uint8_t> a_bytes)
		{
			std::scoped_lock lock(mutex);
			if (!ValidDigest(a_digest) || a_bytes.empty() || a_bytes.size() > limit)
				return StoreResult::Unavailable;
			const auto contentHash = Util::FileDigest::Sha256BytesHex(a_bytes);
			if (!contentHash || _stricmp(contentHash->c_str(), a_digest.c_str()) != 0)
				return StoreResult::Unavailable;
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			if (error || std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error)) || error)
				return StoreResult::Unavailable;
			const auto lockPath = directory / ".lock";
			const auto lockFile = CreateFileW(lockPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
				FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (lockFile == INVALID_HANDLE_VALUE)
				return StoreResult::Unavailable;
			struct CloseLock
			{
				HANDLE file;
				~CloseLock() { CloseHandle(file); }
			} closeLock{ lockFile };
			BY_HANDLE_FILE_INFORMATION lockInfo{};
			if (!GetFileInformationByHandle(lockFile, &lockInfo) || (lockInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
				return StoreResult::Unavailable;
			const auto final = directory / (a_digest + extension);
			if (ReadVerified(final, a_digest, limit)) {
				Remember(a_digest);
				return StoreResult::Existing;
			}
			known.erase(a_digest);
			if (!MakeRoom(a_bytes.size(), final))
				return StoreResult::Unavailable;
			const auto temp = directory / (a_digest + ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(++serial));
			const auto file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return StoreResult::Unavailable;
			DWORD written = 0;
			const bool success = a_bytes.size() <= MAXDWORD && WriteFile(file, a_bytes.data(), DWORD(a_bytes.size()), &written, nullptr) && written == a_bytes.size();
			CloseHandle(file);
			const auto hash = success ? Util::FileDigest::Sha256FileHex(temp) : std::nullopt;
			const bool valid = hash && _stricmp(hash->c_str(), a_digest.c_str()) == 0;
			const bool published = valid && MoveFileExW(temp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			if (!published)
				DeleteFileW(temp.c_str());
			if (published)
				Remember(a_digest);
			return published ? StoreResult::Stored : StoreResult::Unavailable;
		}

		/** @brief Reads a regular file and verifies its digest through the opened handle. */
		static std::optional<std::vector<uint8_t>> ReadVerified(const std::filesystem::path& a_path, const std::string& a_digest, uint64_t a_maxBytes)
		{
			if (!ValidDigest(a_digest))
				return std::nullopt;
			const auto file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return std::nullopt;
			struct Close
			{
				HANDLE file;
				~Close() { CloseHandle(file); }
			} close{ file };
			BY_HANDLE_FILE_INFORMATION info{};
			LARGE_INTEGER size{};
			if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
				!GetFileSizeEx(file, &size) || size.QuadPart < 128 || uint64_t(size.QuadPart) > a_maxBytes || uint64_t(size.QuadPart) > MAXDWORD)
				return std::nullopt;
			const auto hash = Util::FileDigest::Sha256HandleHex(file);
			if (!hash || _stricmp(hash->c_str(), a_digest.c_str()) != 0 || !SetFilePointerEx(file, {}, nullptr, FILE_BEGIN))
				return std::nullopt;
			std::vector<uint8_t> bytes(size_t(size.QuadPart));
			DWORD read = 0;
			if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size())
				return std::nullopt;
			return bytes;
		}

	private:
		void Remember(const std::string& a_digest)
		{
			if (known.size() >= 4096 && !known.contains(a_digest))
				known.clear();
			known.insert(a_digest);
		}
		static bool ValidDigest(const std::string& a_digest)
		{
			return a_digest.size() == 64 && std::ranges::all_of(a_digest, [](unsigned char c) { return std::isxdigit(c) != 0; });
		}
		bool MakeRoom(uint64_t a_needed, const std::filesystem::path& a_final)
		{
			struct Entry
			{
				std::filesystem::path path;
				uint64_t bytes;
				std::filesystem::file_time_type time;
				bool temporary;
			};
			std::vector<Entry> entries;
			uint64_t total = 0;
			std::error_code error;
			for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
				const auto& item = *it;
				const auto name = item.path().filename().string();
				const bool regularName = name.size() == 64 + extension.size() && name.substr(64) == extension;
				const bool tempName = name.size() > 69 && name.substr(64, 5) == ".tmp." &&
				                      std::ranges::all_of(name.substr(69), [](unsigned char c) { return std::isdigit(c) != 0 || c == '.'; });
				if ((!regularName && !tempName) || !ValidDigest(name.substr(0, 64)) || item.is_symlink(error) || error || !item.is_regular_file(error) || error)
					continue;
				const auto bytes = item.file_size(error);
				if (error || bytes > UINT64_MAX - total)
					return false;
				total += bytes;
				const auto time = item.last_write_time(error);
				if (error)
					return false;
				if (entries.size() >= 8192)
					return false;
				entries.push_back({ item.path(), bytes, time, tempName });
			}
			if (error)
				return false;
			std::ranges::sort(entries, {}, &Entry::time);
			for (const auto& entry : entries) {
				if (entry.temporary) {
					if (std::filesystem::file_time_type::clock::now() - entry.time > std::chrono::hours(24) &&
						std::filesystem::remove(entry.path, error) && !error)
						total -= entry.bytes;
					error.clear();
					continue;
				}
				if (entry.path == a_final || total > limit || a_needed > limit - total) {
					if (!std::filesystem::remove(entry.path, error) || error)
						return false;
					known.erase(entry.path.filename().string().substr(0, 64));
					total -= entry.bytes;
				}
			}
			return total <= limit && a_needed <= limit - total;
		}
		std::filesystem::path directory;
		uint64_t limit, serial = 0;
		std::string extension;
		std::unordered_set<std::string> known;
		std::mutex mutex;
	};
}
