#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DX12ResourceService
{
    /** @brief Save metadata is only a location hint, NOT a resolved CELL FormID. */
    struct SaveLocationHint
    {
        std::string fileName;
        std::string editorId;
        std::string locationName;
        std::string sourceDirectory;
    };

    inline bool SameCellHint(std::string_view a, std::string_view b)
    {
        if (a.empty() || b.empty() || a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }

    /** @brief Read the bounded, uncompressed Skyrim SE ESS header only.
     * Saves may use ZLIB/LZ4, but the identifying header is never compressed.
     */
    inline std::optional<std::string> ReadEssLocation(std::span<const uint8_t> bytes)
    {
        constexpr std::string_view magic = "TESV_SAVEGAME";
        if (bytes.size() < 35 ||
            std::string_view(reinterpret_cast<const char*>(bytes.data()), magic.size()) != magic)
            return std::nullopt;
        size_t position = magic.size();
        const auto get32 = [&]() -> std::optional<uint32_t> {
            if (position > bytes.size() || 4 > bytes.size() - position) return {};
            const uint32_t n = uint32_t(bytes[position]) |
                (uint32_t(bytes[position + 1]) << 8) |
                (uint32_t(bytes[position + 2]) << 16) |
                (uint32_t(bytes[position + 3]) << 24);
            position += 4;
            return n;
        };
        const auto wordString = [&]() -> std::optional<std::string> {
            if (position > bytes.size() || 2 > bytes.size() - position) return {};
            const uint16_t n = uint16_t(bytes[position]) | uint16_t(bytes[position + 1]) << 8;
            position += 2;
            if (!n || n > 512 || position > bytes.size() || n > bytes.size() - position)
                return {};
            std::string result(reinterpret_cast<const char*>(bytes.data() + position), n);
            position += n;
            if (result.find('\0') != std::string::npos) return {};
            return result;
        };
        const auto size = get32();
        const auto version = get32();
        const auto saveNumber = get32();
        if (!size || !version || !saveNumber || *size < 16 || *size > 8192 ||
            *version < 12 || *version > 20) return {};
        if (!wordString() || !get32()) return {}; // player name and level
        return wordString();                      // player location
    }

    /** @brief Skyrim SE's structured filename often records the cell EditorID.
     * Treat it as a hint only, and validate it against loaded CELL records.
     */
    inline std::string SaveFileCellEditorId(std::string_view stem)
    {
        std::vector<std::string_view> parts;
        while (!stem.empty()) {
            const auto end = stem.find('_');
            parts.push_back(stem.substr(0, end));
            if (end == std::string_view::npos) break;
            stem.remove_prefix(end + 1);
        }
        if (parts.size() < 8 || parts[4].empty() || parts[4].size() > 120)
            return {};
        const auto editor = parts[4];
        if (!std::ranges::all_of(editor, [](unsigned char c) {
            return std::isalnum(c) || c == '_';
        })) return {};
        return std::string(editor);
    }

    /** @brief Bound file traversal, with no symlink following or save modification. */
    inline std::optional<SaveLocationHint> LatestSaveLocation(
        const std::vector<std::filesystem::path>& saveFolders)
    {
        struct Entry { std::filesystem::path file, root; std::filesystem::file_time_type written; };
        std::vector<Entry> found;
        for (const auto& root : saveFolders) {
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec) || ec) continue;
            std::filesystem::recursive_directory_iterator it(
                root, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (size_t inspected = 0; it != end && !ec && inspected < 20000; it.increment(ec), ++inspected) {
                if (it.depth() > 1) { it.disable_recursion_pending(); continue; }
                const auto& p = it->path();
                if (it->is_symlink(ec) || ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
                if (!SameCellHint(p.extension().string(), ".ess")) continue;
                const auto len = it->file_size(ec);
                if (ec || len < 64 || len > (2ull << 30)) { ec.clear(); continue; }
                const auto written = it->last_write_time(ec);
                if (ec) { ec.clear(); continue; }
                found.push_back({ p, root, written });
            }
        }
        std::ranges::sort(found, [](const Entry& x, const Entry& y) {
            return x.written > y.written;
        });
        for (size_t i = 0; i < std::min(size_t(16), found.size()); ++i) {
            std::ifstream file(found[i].file, std::ios::binary);
            if (!file) continue;
            std::vector<uint8_t> bytes(8192);
            file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
            bytes.resize(size_t(file.gcount()));
            const auto location = ReadEssLocation(bytes);
            if (!location) continue;
            return SaveLocationHint{
                found[i].file.filename().string(),
                SaveFileCellEditorId(found[i].file.stem().string()),
                *location, found[i].root.string()
            };
        }
        return {};
    }
}
