#pragma once

#include "Utils/ContentHash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/// Persistent store of compiled shader blobs, addressed by Util::CompileDedupe::MakeKey (a hash of the
/// preprocessed source plus everything else that changes the bytecode).
/// Lives in Data/ShaderCache/ContentStore, which cache invalidation leaves in place.
namespace Util::ShaderContentStore
{
	/// Sharded on-disk blob store addressed by a CompileDedupe key; safe for concurrent use by compile threads.
	class Store
	{
	public:
		/// @param a_maxBytes Size cap enforced by Put once a sixteenth of it has been written since the last trim; 0 disables.
		explicit Store(std::filesystem::path a_root, uint64_t a_maxBytes = 0) :
			root(std::move(a_root)), maxBytes(a_maxBytes) {}

		/// On-disk location of the blob for a key.
		std::filesystem::path PathFor(const ContentHash::Hash128& a_key) const
		{
			const auto hex = a_key.ToHex();
			return root / hex.substr(0, 2) / (hex + ".bin");
		}

		/// Returns the stored blob, or an empty vector on a miss or unreadable entry.
		/// A hit refreshes the entry's write time so Trim evicts by last use.
		std::vector<char> Get(const ContentHash::Hash128& a_key) const
		{
			const auto path = PathFor(a_key);
			std::ifstream ifs(path, std::ios::binary);
			if (!ifs.is_open())
				return {};
			std::vector<char> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
			if (ifs.bad())
				return {};
			ifs.close();
			std::error_code ec;
			std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
			return bytes;
		}

		/// Writes via a temp file and rename so a reader never sees a partial blob.
		bool Put(const ContentHash::Hash128& a_key, const void* a_data, size_t a_size) const
		{
			if (!a_data || a_size == 0)
				return false;
			const auto path = PathFor(a_key);
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec)
				return false;
			// Concurrent compile threads can finish identical shaders, so each write needs its own temp name.
			static std::atomic<uint64_t> tempCounter{ 0 };
			auto tmp = path;
			tmp += std::format(".{}.tmp", tempCounter.fetch_add(1, std::memory_order_relaxed));
			{
				std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
				if (!ofs.is_open())
					return false;
				ofs.write(static_cast<const char*>(a_data), static_cast<std::streamsize>(a_size));
				if (!ofs.good())
					return false;
			}
			std::filesystem::rename(tmp, path, ec);
			if (ec) {
				std::filesystem::remove(tmp, ec);
				return false;
			}
			if (maxBytes && (bytesSinceTrim += a_size) >= maxBytes / 16 && trimMutex.try_lock()) {
				std::scoped_lock lock(std::adopt_lock, trimMutex);
				bytesSinceTrim = 0;
				Trim(maxBytes);
			}
			return true;
		}

		/// Deletes every stored blob; later Put calls recreate the directories.
		void Clear() const
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		/// Evicts least-recently-used entries until the store is at most a_maxBytes.
		/// Returns the number of entries removed.
		size_t Trim(uint64_t a_maxBytes) const
		{
			struct Entry
			{
				std::filesystem::path path;
				uint64_t size;
				std::filesystem::file_time_type time;
			};
			std::vector<Entry> entries;
			uint64_t total = 0;
			std::error_code ec;
			for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
				if (!it->is_regular_file(ec) || it->path().extension() != ".bin")
					continue;
				const auto size = it->file_size(ec);
				const auto time = it->last_write_time(ec);
				if (ec)
					continue;
				entries.push_back({ it->path(), size, time });
				total += size;
			}
			if (total <= a_maxBytes)
				return 0;
			std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
			size_t removed = 0;
			for (const auto& e : entries) {
				if (total <= a_maxBytes)
					break;
				std::filesystem::remove(e.path, ec);
				if (!ec) {
					total -= e.size;
					++removed;
				}
			}
			return removed;
		}

	private:
		std::filesystem::path root;
		uint64_t maxBytes;
		mutable std::atomic<uint64_t> bytesSinceTrim{ 0 };
		mutable std::mutex trimMutex;
	};
}
