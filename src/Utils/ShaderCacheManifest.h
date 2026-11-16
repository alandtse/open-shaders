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
#include <utility>

namespace Util::ShaderCacheManifest
{
	struct Entry
	{
		std::string digest;
		std::string contentKey;
		std::string bytecodeHash;

		bool operator==(const Entry&) const = default;

		/// Checks that the bytes are the ones recorded with this digest and content key.
		bool MatchesBytecode(const void* data, size_t size) const
		{
			return !bytecodeHash.empty() && ContentHash::HashBytes(data, size).ToHex() == bytecodeHash;
		}
	};

	class Manifest
	{
	public:
		/// Load from disk, or start empty if the file is missing/unparseable.
		void Load(const std::filesystem::path& manifestPath)
		{
			std::lock_guard lock(mutex);
			path = manifestPath;
			entries.clear();
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
					entries[key].digest = value.get<std::string>();
			}
			const auto loadField = [&](const char* field, std::string Entry::* member) {
				const auto object = j.find(field);
				if (object == j.end() || !object->is_object())
					return;
				for (const auto& [key, value] : object->items()) {
					if (auto it = entries.find(key); it != entries.end() && value.is_string())
						it->second.*member = value.get<std::string>();
				}
			};
			loadField("contentKeys", &Entry::contentKey);
			loadField("bytecodeHashes", &Entry::bytecodeHash);
		}

		/// True when no digest is on record, so no cached blob can be validated against its source.
		bool Empty() const
		{
			std::lock_guard lock(mutex);
			return entries.empty();
		}

		/// Metadata last recorded for this relative blob path, if any.
		std::optional<Entry> Get(const std::string& relativePath) const
		{
			std::lock_guard lock(mutex);
			const auto it = entries.find(relativePath);
			if (it == entries.end())
				return std::nullopt;
			return it->second;
		}

		/// Record/overwrite the blob metadata together. Does not itself write to disk; call Save()
		/// (directly, or via a debounced caller) to persist.
		void Set(const std::string& relativePath, Entry entry)
		{
			std::lock_guard lock(mutex);
			entries.insert_or_assign(relativePath, std::move(entry));
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
			auto& digests = j["entries"] = nlohmann::json::object();
			auto& keys = j["contentKeys"] = nlohmann::json::object();
			auto& hashes = j["bytecodeHashes"] = nlohmann::json::object();
			for (const auto& [key, entry] : entries) {
				digests[key] = entry.digest;
				if (!entry.contentKey.empty())
					keys[key] = entry.contentKey;
				if (!entry.bytecodeHash.empty())
					hashes[key] = entry.bytecodeHash;
			}
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
		std::unordered_map<std::string, Entry> entries;
		bool dirty = false;
	};
}
