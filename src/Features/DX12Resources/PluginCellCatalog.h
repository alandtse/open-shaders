#pragma once

#include "CellRequest.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DX12ResourceService
{
    /** @brief Compare plugin names and UI queries with ASCII case folding.
     * Skyrim plugin filenames are not arbitrary filesystem paths in this API.
     */
    inline bool SamePluginName(std::string_view a, std::string_view b)
    {
        if (a.empty() || a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }

    inline bool ContainsAsciiInsensitive(std::string_view text, std::string_view query)
    {
        if (query.empty()) return true;
        if (query.size() > text.size()) return false;
        for (size_t start = 0; start <= text.size() - query.size(); ++start) {
            bool matched = true;
            for (size_t i = 0; i < query.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(text[start + i])) !=
                    std::tolower(static_cast<unsigned char>(query[i]))) {
                    matched = false;
                    break;
                }
            if (matched) return true;
        }
        return false;
    }

    enum class PluginCellRelation : uint8_t { Absent, Defined, Override };

    /** @brief A plugin can define a CELL or override a CELL owned by another plugin.
     * An override must keep the original owner/local FormID for LookupForm().
     */
    inline PluginCellRelation ClassifyPluginCell(std::string_view selected,
        std::string_view owner, std::span<const std::string_view> contributors)
    {
        if (SamePluginName(selected, owner)) return PluginCellRelation::Defined;
        for (const auto contributor : contributors)
            if (SamePluginName(selected, contributor)) return PluginCellRelation::Override;
        return PluginCellRelation::Absent;
    }

    struct PluginInteriorEntry
    {
        CellRequest identity;  // Original owning plugin and local FormID, never an override's.
        std::string editorId;
        std::string displayName;
        PluginCellRelation relation = PluginCellRelation::Absent;
    };

    inline bool MatchesInteriorSearch(const PluginInteriorEntry& entry, std::string_view query)
    {
        return ContainsAsciiInsensitive(entry.displayName, query) ||
            ContainsAsciiInsensitive(entry.editorId, query) ||
            ContainsAsciiInsensitive(entry.identity.Key(), query);
    }

    /** @brief An explicitly chosen interior does not require latest-save
     * detection or a currently attached cell. The engine validates its
     * identity again before any asynchronous source-discovery job.
     */
    inline bool CanPrepareKnownInterior(bool enabled, bool dataLoaded, bool jobActive,
        const std::optional<CellRequest>& selected)
    {
        return enabled && dataLoaded && !jobActive &&
            selected.has_value() && !selected->plugin.empty() && selected->localId != 0;
    }

    inline void SortInteriorCatalog(std::vector<PluginInteriorEntry>& cells)
    {
        std::ranges::sort(cells, [](const auto& a, const auto& b) {
            if (a.displayName != b.displayName) return a.displayName < b.displayName;
            if (a.editorId != b.editorId) return a.editorId < b.editorId;
            return a.identity.Key() < b.identity.Key();
        });
    }
}
