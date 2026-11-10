#pragma once

#include "CellRequest.h"
#include "PreprocessorParity.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

namespace DX12ResourceService
{
    /** @brief A safe, value-only copy from Skyrim's CELL form lookup. */
    struct CellDiscovery
    {
        CellRequest requested;
        uint32_t resolvedFormId = 0;
        std::string cellName;
        std::string formSource;
        std::string selectedPluginSha256;
    uint64_t selectedPluginBytes = 0;
    bool isInterior = false;
        bool isAttached = false;
    };

    /** @brief Discovery is NOT the legacy NIF/REFR preprocessing output. */
    inline nlohmann::json BuildDiscoveryReport(const CellDiscovery& cell)
    {
        return {
            { "schemaVersion", 1 },
            { "kind", "cell-source-discovery" },
            { "identity", {
                { "plugin", cell.requested.plugin },
                { "localFormId", cell.requested.localId },
                { "resolvedFormId", cell.resolvedFormId },
                { "cellName", cell.cellName },
                { "formSource", cell.formSource },
                { "selectedPluginSha256", cell.selectedPluginSha256 },
                { "selectedPluginBytes", cell.selectedPluginBytes },
                { "selectedPluginVerified", cell.selectedPluginSha256.size() == 64 }
            } },
            { "interior", cell.isInterior },
            { "runtimeCellAttached", cell.isAttached },
            { "geometryReady", false },
            { "materialsReady", false },
            { "lightsReady", false },
            { "preprocessorParity", false },
            { "preprocessorParityGates", ParityStatus{}.ToJson() },
            { "renderable", false },
            { "nextStage", "Port first-stage offline Mutagen NIF/REFR/light extraction into native DX12 Resources" },
            { "discoveryComplete", cell.isInterior && cell.resolvedFormId != 0 }
        };
    }

    inline bool IsDiscoveryOnlyReport(const nlohmann::json& report)
    {
        return report.is_object() && report.value("schemaVersion", 0) == 1 &&
            report.value("kind", std::string{}) == "cell-source-discovery" &&
            !report.value("geometryReady", true) && !report.value("materialsReady", true) &&
            !report.value("lightsReady", true) && !report.value("renderable", true);
    }
}
