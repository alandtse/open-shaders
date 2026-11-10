#pragma once

#include "CellRequest.h"
#include "../DX12NativeScene/CellPackedFile.h"
#include "PluginCellCatalog.h"
#include "Utils/FileDigest.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
#include <cstdint>
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
    struct PreparedSubcellAddress
    {
        uint32_t size = 4096;
        int64_t x = 0, y = 0;
        bool Valid() const
        {
            return size >= 2048 && size <= 16384 && size % 1024 == 0 &&
                x >= -1'000'000 && x <= 1'000'000 && y >= -1'000'000 && y <= 1'000'000;
        }
        std::string Name() const { return std::format("{}_{}", x, y); }
    };

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
        const CellRequest& request, std::optional<PreparedSubcellAddress> subcell = std::nullopt)
    {
        if (subcell && !subcell->Valid())
            throw std::runtime_error("Invalid subcell validation request");
        const auto prep = ReadPreparationJson(output / "cell-preparation.json");
        const auto layout = ReadPreparationJson(output / "cell-layout.json");
        const auto meshes = ReadPreparationJson(output / "cell-meshes.json");
        const auto records = ReadPreparationJson(output / "cell-records.json");
        const auto geometryPack = DX12Packed::Read(output, layout.at("geometryPack"),
            "cell-geometry.rsgp", DX12Packed::kGeometryMagic);
        const auto shadingPack = DX12Packed::Read(output, layout.at("shadingPack"),
            "cell-shading.rssp", DX12Packed::kShadingMagic);
        (void)ReadPreparationJson(output / "cell-index.json");

        if (prep.at("schemaVersion") != 1 ||
            prep.at("formatContract") != "dx12-resources-cell" ||
            prep.at("formatVersion") != 2 ||
            !SamePluginName(prep.at("cell").get<std::string>(), request.Key()) ||
            !SamePluginName(layout.at("cell").get<std::string>(), request.Key()) ||
            !SamePluginName(records.at("cellKey").get<std::string>(), request.Key()) ||
            layout.at("schemaVersion") != 2 || layout.at("rendererSchemaVersion") != 2)
            throw std::runtime_error("Prepared CELL identity or format contract mismatch");

        const auto checkSubcell = [&](const PrepJson& manifest) {
            if (!subcell) return !manifest.contains("subcell") || manifest.at("subcell").is_null();
            if (!manifest.contains("subcell") || !manifest.at("subcell").is_object()) return false;
            const auto& tile = manifest.at("subcell");
            return tile.value("size", 0u) == subcell->size &&
                tile.value("x", INT64_MIN) == subcell->x &&
                tile.value("y", INT64_MIN) == subcell->y;
        };
        const checkScope = [&](const PrepJson& manifest) {
            return manifest.value("scopeType", std::string("cell")) ==
                (request.worldspace ? "worldspace" : "cell");
        };
        if (!checkScope(prep) || !checkScope(layout) ||
            !checkSubcell(prep) || !checkSubcell(layout))
            throw std::runtime_error("Prepared CELL subcell scope mismatch");
        const auto digest = prep.at("layoutDigest").get<std::string>();
        if (digest.empty() || layout.at("layoutDigest") != digest)
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
            const auto geometryHash = instance.at("geometrySha256").get<std::string>();
            VerifyPreparedBlob(output, instance.at("geometryFile").get<std::string>(),
                geometryHash, "geometry", ".rsgm", checked);
            (void)geometryPack.Get(geometryHash);
            if (!instance.at("shadingFile").is_null()) {
                const auto shadingHash = instance.at("shadingSha256").get<std::string>();
                VerifyPreparedBlob(output, instance.at("shadingFile").get<std::string>(),
                    shadingHash, "shading", ".rsga", checked);
                (void)shadingPack.Get(shadingHash);
            }
        }
        for (const auto& asset : prep.at("assets")) {
            if (asset.at("cachedSource").is_null()) continue; // explicitly unresolved
            VerifyPreparedBlob(output, asset.at("cachedSource").get<std::string>(),
                asset.at("sha256").get<std::string>(), "source-cache", ".nif", checked);
        }
        const auto& textureReferences = layout.at("textureReferences");
        if (!textureReferences.is_array() || textureReferences.size() > 4096)
            throw std::runtime_error("Packed CELL texture reference count invalid");
        for (const auto& reference : textureReferences) {
            const auto path = reference.at("path").get<std::string>();
            if (reference.at("status") != "referenced" || path.size() > 1024 ||
                !path.starts_with("textures/") || !path.ends_with(".dds") ||
                path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
                path.find(':') != std::string::npos || path.find("//") != std::string::npos)
                throw std::runtime_error("Invalid prepared external DDS reference");
        }
        if (prep.at("runtimeCoverageValid") != false ||
            prep.at("accelerationBuilt") != false)
            throw std::runtime_error("Stage A coverage claims do not match the agreed partial format");
        return prep;
    }

    /** @brief Keep the published CELL to three files; retain only compact JSON diagnostics. */
    inline std::filesystem::path PublishPreparedCell(const std::filesystem::path& staging,
        const std::filesystem::path& data, const CellRequest& request,
        std::optional<PreparedSubcellAddress> subcell = std::nullopt)
    {
        if (subcell && !subcell->Valid()) throw std::runtime_error("Invalid prepared subcell destination");
        const auto parsed = ParseCellRequest(request.plugin,
            std::format("{:06X}", request.localId));
        if (!parsed) throw std::runtime_error("Invalid CELL identity for cache publication");

        auto final = data / "SKSE/Plugins/CommunityShaders/DX12-cache" /
            request.CacheFamily() / request.plugin / std::format("{:06X}", request.localId);
        if (subcell)
            final /= std::filesystem::path("subcells") / std::to_string(subcell->size) / subcell->Name();
        std::error_code ec;
        std::filesystem::create_directories(final.parent_path(), ec);
        if (ec || std::filesystem::is_symlink(
            std::filesystem::symlink_status(final.parent_path(), ec)) || ec)
            throw std::runtime_error("DX12-cache publication directory unavailable");
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(final, ec)))
            throw std::runtime_error("Refusing to replace an unsafe prepared CELL");
        ec.clear();

        // Keep compact diagnostic JSON beneath DX12-cache/diagnostics; never in published cells/.
        const auto stamp = std::to_wstring(GetTickCount64());
        const auto diagnostics = data / "SKSE/Plugins/CommunityShaders/DX12-cache/diagnostics" /
            request.CacheFamily() / request.plugin /
            (std::format(L"{:06X}-", request.localId) + stamp);
        std::filesystem::create_directories(diagnostics);
        for (const char* report : { "cell-preparation.json", "cell-records.json",
                 "cell-index.json", "cell-meshes.json" }) {
            std::filesystem::copy_file(staging / report, diagnostics / report);
        }

        const auto incoming = final.parent_path() / (final.filename().wstring() + L".incoming." + stamp);
        const auto backup = final.parent_path() / (final.filename().wstring() + L".previous." + stamp);
        if (!std::filesystem::create_directory(incoming))
            throw std::runtime_error("Prepared CELL publication staging already exists");
        bool hadPrevious = false;
        try {
            for (const char* resource : { "cell-layout.json", "cell-geometry.rsgp", "cell-shading.rssp" })
                std::filesystem::copy_file(staging / resource, incoming / resource);

            // Verify the actual published bytes before making them visible to the DX12 loader.
            const auto manifest = ReadPreparationJson(incoming / "cell-layout.json");
            if (subcell) {
                const auto& tile = manifest.at("subcell");
                if (tile.at("size").get<uint32_t>() != subcell->size ||
                    tile.at("x").get<int64_t>() != subcell->x ||
                    tile.at("y").get<int64_t>() != subcell->y)
                    throw std::runtime_error("Published subcell has incorrect coordinates");
            }
            (void)DX12Packed::Read(incoming, manifest.at("geometryPack"),
                "cell-geometry.rsgp", DX12Packed::kGeometryMagic);
            (void)DX12Packed::Read(incoming, manifest.at("shadingPack"),
                "cell-shading.rssp", DX12Packed::kShadingMagic);

            hadPrevious = std::filesystem::exists(final, ec);
            if (ec) throw std::runtime_error("Cannot inspect previous prepared CELL");
            if (hadPrevious) {
                std::filesystem::rename(final, backup, ec);
                if (ec) throw std::runtime_error("Cannot preserve previous prepared CELL");
            }
            std::filesystem::rename(incoming, final, ec);
            if (ec) {
                if (hadPrevious) {
                    std::error_code rollback;
                    std::filesystem::rename(backup, final, rollback);
                }
                throw std::runtime_error("Could not publish prepared CELL; previous cache retained");
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(incoming, ignored);
            throw;
        }
        if (hadPrevious) {
            std::error_code ignored;
            std::filesystem::remove_all(backup, ignored);
        }
        // All intermediate NIF, source-cache, per-shape blobs and compiler files are staging-only.
        std::filesystem::remove_all(staging, ec);
        return final;
    }
}
