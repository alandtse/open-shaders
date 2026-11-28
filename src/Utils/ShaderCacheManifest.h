#pragma once

// Sidecar content-digest manifest for the shader disk cache (Data/ShaderCache/
// Manifest.json), free of game/SKSE dependencies so it's independently
// testable. Maps a cache blob's path (relative to Data/ShaderCache) to the
// XXH3-128 content digest of the inputs that produced it. A missing or
// unparseable manifest is always treated as "no entries" -- callers fall
// back to whatever staleness check they had before this manifest existed,
// never to "trust the blob".

#include "Utils/ContentHash.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

namespace Util::ShaderCacheManifest
{
	/// A blob newer than the manifest may have replaced the recorded bytecode.
	inline bool IsSavedBeforeManifest(const std::filesystem::path& blob, std::filesystem::file_time_type manifestTime)
	{
		std::error_code ec;
		const auto blobTime = std::filesystem::last_write_time(blob, ec);
		return !ec && blobTime <= manifestTime;
	}

	class Manifest
	{
	public:
		/// Load from disk, or start empty if the file is missing/unparseable.
		void Load(const std::filesystem::path& manifestPath)
		{
			std::lock_guard lock(mutex);
			path = manifestPath;
			entries.clear();
			contentKeys.clear();
			bytecodeHashes.clear();
			std::error_code ec;
			legacyCutoff = std::filesystem::last_write_time(manifestPath, ec);
			std::ifstream ifs(manifestPath, std::ios::binary);
			if (!ifs.is_open())
				return;
			nlohmann::json j;
			try {
				ifs >> j;
			} catch (...) {
				return;  // corrupt/partial file -> treat as absent, not an error
			}
			if (!j.is_object() || !j.contains("entries") || !j["entries"].is_object())
				return;
			if (const auto cutoff = j.find("legacyCutoffTicks"); cutoff != j.end()) {
				using FileTime = std::filesystem::file_time_type;
				legacyCutoff = cutoff->is_number_integer() ?
				                   std::min(legacyCutoff, FileTime{ FileTime::duration{ cutoff->get<int64_t>() } }) :
				                   FileTime::min();
			}
			const auto loadHashes = [&](const char* field, auto& hashes) {
				if (!j.contains(field) || !j[field].is_object())
					return;
				for (const auto& [key, value] : j[field].items()) {
					if (value.is_string())
						hashes[key] = value.get<std::string>();
				}
			};
			loadHashes("entries", entries);
			loadHashes("contentKeys", contentKeys);
			loadHashes("bytecodeHashes", bytecodeHashes);
		}

		/// Validate recorded bytecode, falling back to the original manifest timestamp for legacy entries.
		bool MatchesRecordedBlob(const std::string& relativePath, const void* data, size_t size) const
		{
			std::lock_guard lock(mutex);
			if (!entries.contains(relativePath) && !contentKeys.contains(relativePath))
				return false;
			if (const auto it = bytecodeHashes.find(relativePath); it != bytecodeHashes.end())
				return ContentHash::HashBytes(data, size).ToHex() == it->second;
			return IsSavedBeforeManifest(path.parent_path() / relativePath, legacyCutoff);
		}

		/// True when no digest is on record, so no cached blob can be validated against its source.
		bool Empty() const
		{
			std::lock_guard lock(mutex);
			return entries.empty();
		}

		/// Digest last recorded for this relative blob path, if any.
		std::optional<std::string> Get(const std::string& relativePath) const
		{
			std::lock_guard lock(mutex);
			const auto it = entries.find(relativePath);
			if (it == entries.end())
				return std::nullopt;
			return it->second;
		}

		/// Record/overwrite a digest. Does not itself write to disk; call Save()
		/// (directly, or via a debounced caller) to persist.
		void Set(const std::string& relativePath, const std::string& digestHex, std::string bytecodeHash = {})
		{
			std::lock_guard lock(mutex);
			entries[relativePath] = digestHex;
			if (bytecodeHash.empty())
				bytecodeHashes.erase(relativePath);
			else
				bytecodeHashes[relativePath] = std::move(bytecodeHash);
			dirty = true;
		}

		/// Content key of the code this blob was compiled from, if one was recorded.
		std::optional<std::string> GetContentKey(const std::string& relativePath) const
		{
			std::lock_guard lock(mutex);
			const auto it = contentKeys.find(relativePath);
			if (it == contentKeys.end())
				return std::nullopt;
			return it->second;
		}

		/// Record/overwrite a blob's content key. Persist with Save().
		void SetContentKey(const std::string& relativePath, const std::string& keyHex)
		{
			std::lock_guard lock(mutex);
			contentKeys[relativePath] = keyHex;
			dirty = true;
		}

		/// Removes every entry for which shouldRemove(relativePath) returns true.
		/// The predicate carries any filesystem side effect (deleting the blob
		/// this entry described) — this class stays free of Data/ShaderCache path
		/// knowledge, matching its game/SKSE-independent design. Marks dirty if
		/// anything was removed; caller still owns calling Save(). Returns the
		/// number of entries removed.
		size_t PruneIf(const std::function<bool(const std::string&)>& shouldRemove)
		{
			std::lock_guard lock(mutex);
			size_t removed = 0;
			for (auto it = entries.begin(); it != entries.end();) {
				if (shouldRemove(it->first)) {
					contentKeys.erase(it->first);
					bytecodeHashes.erase(it->first);
					it = entries.erase(it);
					++removed;
					dirty = true;
				} else {
					++it;
				}
			}
			return removed;
		}

		/// Atomic write (temp file + rename, same directory so same volume): a
		/// crash mid-write leaves the previous manifest intact rather than a
		/// half-written one, which JSON parsing would just reject anyway, but
		/// this avoids even that transient failure window. No-op if nothing
		/// changed since the last Save()/Load().
		bool Save()
		{
			std::lock_guard lock(mutex);
			if (!dirty || path.empty())
				return true;
			nlohmann::json j;
			j["schemaVersion"] = 1;
			j["entries"] = entries;
			j["legacyCutoffTicks"] = legacyCutoff.time_since_epoch().count();
			if (!contentKeys.empty())
				j["contentKeys"] = contentKeys;
			if (!bytecodeHashes.empty())
				j["bytecodeHashes"] = bytecodeHashes;
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			const auto tempPath = path.parent_path() / std::format("{}.{}.tmp", path.filename().string(), std::random_device{}());
			{
				std::ofstream ofs(tempPath, std::ios::binary | std::ios::trunc);
				if (!ofs.is_open())
					return false;
				ofs << j.dump();
				if (!ofs)
					return false;
			}
			std::filesystem::rename(tempPath, path, ec);
			if (ec) {
				std::filesystem::remove(tempPath, ec);
				return false;
			}
			dirty = false;
			return true;
		}

	private:
		mutable std::mutex mutex;
		std::filesystem::path path;
		std::filesystem::file_time_type legacyCutoff = std::filesystem::file_time_type::min();
		std::unordered_map<std::string, std::string> entries;
		std::unordered_map<std::string, std::string> contentKeys;
		std::unordered_map<std::string, std::string> bytecodeHashes;
		bool dirty = false;
	};
}
