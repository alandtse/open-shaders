#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace DX12Native
{
	/** @brief Retains bounded immutable pages until reused or cleared. */
	template <class Page>
	class MaterialPageCache
	{
	public:
		/** @brief Transfers a matching page to a new owner. */
		std::optional<Page> Take(const std::string& a_key)
		{
			const auto found = entries.find(a_key);
			if (found == entries.end()) return std::nullopt;
			bytes -= found->second.bytes;
			auto page = std::move(found->second.page);
			entries.erase(found);
			return page;
		}

		/** @brief Retains a page only if it fits the supplied byte limit. */
		bool Retain(std::string a_key, Page a_page, uint64_t a_bytes, uint64_t a_limit)
		{
			if (!a_bytes || a_bytes > a_limit || bytes > a_limit - a_bytes || entries.contains(a_key)) return false;
			entries.emplace(std::move(a_key), Entry{ std::move(a_page), a_bytes });
			bytes += a_bytes;
			return true;
		}

		/** @brief Releases cache ownership; external page owners remain valid. */
		void Clear() { entries.clear(); bytes = 0; }
		uint64_t Bytes() const { return bytes; }

	private:
		struct Entry { Page page; uint64_t bytes; };
		std::unordered_map<std::string, Entry> entries;
		uint64_t bytes = 0;
	};
}
