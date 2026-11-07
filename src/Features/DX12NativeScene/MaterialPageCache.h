#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DX12Native
{
	/** @brief Retains bounded immutable pages until reused or cleared. */
	template <class Page>
	class MaterialPageCache
	{
	public:
		struct Match { std::string key; Page page; std::vector<std::string> slices; };
		/** @brief Transfers a matching page to a new owner. */
		std::optional<Page> Take(const std::string& a_key)
		{
			auto match = TakePage(a_key);
			if (!match) return std::nullopt;
			return std::move(match->page);
		}

		/** @brief Transfers the immutable page containing a texture identity. */
		std::optional<Match> TakeContaining(const std::string& a_slice)
		{
			const auto found = slicePages.find(a_slice);
			if (found == slicePages.end()) return std::nullopt;
			return TakePage(found->second);
		}

		/** @brief Retains a page only if it fits the supplied byte limit. */
		bool Retain(std::string a_key, Page a_page, uint64_t a_bytes, uint64_t a_limit, std::vector<std::string> a_slices = {})
		{
			if (!a_bytes || a_bytes > a_limit || bytes > a_limit - a_bytes || entries.contains(a_key)) return false;
			const auto entry = entries.emplace(std::move(a_key), Entry{ std::move(a_page), a_bytes, std::move(a_slices) }).first;
			for (const auto& slice : entry->second.slices) slicePages.try_emplace(slice, entry->first);
			bytes += a_bytes;
			return true;
		}

		/** @brief Releases cache ownership; external page owners remain valid. */
		void Clear() { slicePages.clear(); entries.clear(); bytes = 0; }
		uint64_t Bytes() const { return bytes; }

	private:
		struct Entry { Page page; uint64_t bytes; std::vector<std::string> slices; };
		std::optional<Match> TakePage(const std::string& a_key)
		{
			const auto found = entries.find(a_key);
			if (found == entries.end()) return std::nullopt;
			for (const auto& slice : found->second.slices) {
				const auto index = slicePages.find(slice);
				if (index != slicePages.end() && index->second == found->first) slicePages.erase(index);
			}
			bytes -= found->second.bytes;
			Match result{ found->first, std::move(found->second.page), std::move(found->second.slices) };
			entries.erase(found);
			return result;
		}
		std::unordered_map<std::string, Entry> entries;
		std::unordered_map<std::string, std::string> slicePages;
		uint64_t bytes = 0;
	};
}
