#pragma once

#include <charconv>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace DX12ResourceService
{
    /** @brief Manual, unambiguous cell request. No form pointers cross a worker boundary. */
    struct CellRequest
    {
        std::string plugin;
        uint32_t localId = 0;
        // Exterior uses a persistent WRLD FormKey, independent of the
        // temporary exterior CELL under the player's feet.
        bool worldspace = false;
        std::string CacheFamily() const { return worldspace ? "worldspaces" : "cells"; }
        std::string ScopedKey() const { return (worldspace ? "WRLD:" : "CELL:") + Key(); }
        std::string Key() const { return std::format("{:06X}:{}", localId, plugin); }
    };

    inline std::optional<CellRequest> ParseCellRequest(
        std::string_view plugin, std::string_view localHex)
    {
        if (plugin.empty() || plugin.size() > 120 ||
            localHex.empty() || localHex.size() > 10) return std::nullopt;
        // Plugin name is an identity, never a filesystem path.
        for (const unsigned char c : plugin) {
            if (c < 32 || c > 126 || c == '/' || c == '\\' || c == ':' ||
                c == '<' || c == '>' || c == '|' || c == '*' || c == '?' || c == '"')
                return std::nullopt;
        }
        std::string suffix(plugin.substr(plugin.size() > 4 ? plugin.size() - 4 : 0));
        for (char& c : suffix) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (suffix != ".esm" && suffix != ".esp" && suffix != ".esl") return std::nullopt;
        if (localHex.size() >= 2 && localHex.substr(0, 2) == "0x")
            localHex.remove_prefix(2);
        uint32_t id = 0;
        const auto [end, error] = std::from_chars(
            localHex.data(), localHex.data() + localHex.size(), id, 16);
        if (error != std::errc{} || end != localHex.data() + localHex.size() ||
            !id || id > 0xFFFFFF) return std::nullopt;
        return CellRequest{ std::string(plugin), id };
    }
}
