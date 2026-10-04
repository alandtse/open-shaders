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
			for (const auto& [key, value] : j["entries"].items()) {
				if (value.is_string())
					entries[key] = value.get<std::string>();
			}
			if (j.contains("contentKeys") && j["contentKeys"].is_object()) {
				for (const auto& [key, value] : j["contentKeys"].items()) {
					if (value.is_string())
						contentKeys[key] = value.get<std::string>();
				}
			}
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
		void Set(const std::string& relativePath, const std::string& digestHex)
		{
			std::lock_guard lock(mutex);
			entries[relativePath] = digestHex;
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
			if (!contentKeys.empty())
				j["contentKeys"] = contentKeys;
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
		std::unordered_map<std::string, std::string> entries;
		std::unordered_map<std::string, std::string> contentKeys;
		bool dirty = false;
	};
}
