#pragma once

#include "CellRequest.h"
#include "PluginCellCatalog.h"
#include "Utils/FileDigest.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <format>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace DX12ResourceService
{
    using PrepJson = nlohmann::json;

    inline PrepJson ReadPreparationJson(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0 || size > 64ull * 1024 * 1024)
            throw std::runtime_error("Missing or oversized preparation manifest: " + path.filename().string());
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read preparation manifest");
        return PrepJson::parse(input);
    }

    inline void VerifyPreparedBlob(const std::filesystem::path& root,
        const std::string& relative, const std::string& sha, std::string_view folder,
        std::string_view extension, std::unordered_set<std::string>& checked)
    {
        if (sha.size() != 64 ||
            !std::all_of(sha.begin(), sha.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }) ||
            relative != std::string(folder) + "/" + sha + std::string(extension))
            throw std::runtime_error("Invalid prepared resource reference");
        if (!checked.insert(relative).second) return;
        const auto file = root / std::filesystem::path(relative);
        std::error_code ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(file, ec)) ||
            ec || !std::filesystem::is_regular_file(file, ec) || ec)
            throw std::runtime_error("Missing or unsafe prepared resource: " + relative);
        auto digest = Util::FileDigest::Sha256FileHex(file);
        if (!digest) throw std::runtime_error("Could not hash prepared resource: " + relative);
        std::transform(digest->begin(), digest->end(), digest->begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        if (*digest != sha)
            throw std::runtime_error("Prepared resource SHA-256 mismatch: " + relative);
    }

    /** @brief Validate Stage A producer data, not runtime renderability. */
    inline PrepJson ValidatePreparedCell(const std::filesystem::path& output,
        const CellRequest& request)
    {
        const auto prep = ReadPreparationJson(output / "cell-preparation.json");
        const auto layout = ReadPreparationJson(output / "cell-layout.json");
        const auto meshes = ReadPreparationJson(output / "cell-meshes.json");
        const auto records = ReadPreparationJson(output / "cell-records.json");
        const auto textures = ReadPreparationJson(output / "cell-textures.json");
        (void)ReadPreparationJson(output / "cell-index.json");

        if (prep.at("schemaVersion") != 1 ||
            prep.at("formatContract") != "dx12-resources-cell" ||
            prep.at("formatVersion") != 1 ||
            !SamePluginName(prep.at("cell").get<std::string>(), request.Key()) ||
            !SamePluginName(layout.at("cell").get<std::string>(), request.Key()) ||
            !SamePluginName(records.at("cellKey").get<std::string>(), request.Key()) ||
            !SamePluginName(textures.at("cell").get<std::string>(), request.Key()))
            throw std::runtime_error("Prepared CELL identity or format contract mismatch");

        const auto digest = prep.at("layoutDigest").get<std::string>();
        if (digest.empty() || layout.at("layoutDigest") != digest ||
            textures.at("layoutDigest") != digest)
            throw std::runtime_error("Prepared layout digest inconsistency");
        if (prep.at("inputDigest") != layout.at("inputDigest") ||
            prep.at("inputDigest") != records.at("inputDigest"))
            throw std::runtime_error("Prepared input fingerprint inconsistency");

        std::unordered_set<std::string> checked;
        for (const auto& asset : meshes.at("assets")) {
            for (const auto& shape : asset.at("shapes")) {
                if (!shape.at("geometryFile").is_null()) {
                    VerifyPreparedBlob(output, shape.at("geometryFile").get<std::string>(),
                        shape.at("geometrySha256").get<std::string>(), "geometry", ".rsgm", checked);
                } else if (shape.at("status") == "compiled") {
                    throw std::runtime_error("Compiled shape is missing geometry");
                }
                if (!shape.at("shadingFile").is_null()) {
                    VerifyPreparedBlob(output, shape.at("shadingFile").get<std::string>(),
                        shape.at("shadingSha256").get<std::string>(), "shading", ".rsga", checked);
                } else if (shape.at("shadingStatus") == "compiled") {
                    throw std::runtime_error("Compiled shading attributes are missing");
                }
            }
        }
        for (const auto& instance : layout.at("instances")) {
            VerifyPreparedBlob(output, instance.at("geometryFile").get<std::string>(),
                instance.at("geometrySha256").get<std::string>(), "geometry", ".rsgm", checked);
            if (!instance.at("shadingFile").is_null())
                VerifyPreparedBlob(output, instance.at("shadingFile").get<std::string>(),
                    instance.at("shadingSha256").get<std::string>(), "shading", ".rsga", checked);
        }
        for (const auto& asset : prep.at("assets")) {
            if (asset.at("cachedSource").is_null()) continue; // explicitly unresolved
            VerifyPreparedBlob(output, asset.at("cachedSource").get<std::string>(),
                asset.at("sha256").get<std::string>(), "source-cache", ".nif", checked);
        }
        for (const auto& texture : textures.at("textures")) {
            if (!texture.at("file").is_null())
                VerifyPreparedBlob(output, texture.at("file").get<std::string>(),
                    texture.at("sha256").get<std::string>(), "textures", ".dds", checked);
        }
        if (prep.at("runtimeCoverageValid") != false ||
            prep.at("accelerationBuilt") != false)
            throw std::runtime_error("Stage A coverage claims do not match the agreed partial format");
        return prep;
    }

    inline std::filesystem::path PublishPreparedCell(const std::filesystem::path& staging,
        const std::filesystem::path& data, const CellRequest& request)
    {
        // Original plugin is a validated filename, never an arbitrary path.
        const auto parsed = ParseCellRequest(request.plugin,
            std::format("{:06X}", request.localId));
        if (!parsed) throw std::runtime_error("Invalid CELL identity for cache publication");
        const auto final = data / "SKSE/Plugins/CommunityShaders/DX12-cache/cells" /
            request.plugin / std::format("{:06X}", request.localId);
        std::error_code ec;
        std::filesystem::create_directories(final.parent_path(), ec);
        if (ec || std::filesystem::is_symlink(
                std::filesystem::symlink_status(final.parent_path(), ec)) || ec)
            throw std::runtime_error("DX12-cache publication directory unavailable");
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(final, ec)))
            throw std::runtime_error("Refusing to replace a symlinked prepared CELL");
        ec.clear();

        const auto backup = final.parent_path() /
            (final.filename().wstring() + L".previous." + std::to_wstring(GetTickCount64()));
        bool hadPrevious = std::filesystem::exists(final, ec);
        if (ec) throw std::runtime_error("Cannot inspect previous prepared CELL");
        if (hadPrevious) {
            std::filesystem::rename(final, backup, ec);
            if (ec) throw std::runtime_error("Cannot preserve previous prepared CELL");
        }
        std::filesystem::rename(staging, final, ec);
        if (ec) {
            if (hadPrevious) {
                std::error_code rollback;
                std::filesystem::rename(backup, final, rollback);
            }
            throw std::runtime_error("Could not publish prepared CELL; previous cache retained");
        }
        if (hadPrevious) {
            std::error_code ignored;
            std::filesystem::remove_all(backup, ignored);
        }
        return final;
    }
}
