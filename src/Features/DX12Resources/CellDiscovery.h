#pragma once

#include "CellRequest.h"
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
                { "formSource", cell.formSource }
            } },
            { "interior", cell.isInterior },
            { "runtimeCellAttached", cell.isAttached },
            { "geometryReady", false },
            { "materialsReady", false },
            { "lightsReady", false },
            { "preprocessorParity", false },
            { "renderable", false },
            { "nextStage", "Port first-stage offline Mutagen NIF/REFR/light extraction into native DX12 Resources" },
            { "discoveryComplete", cell.isInterior && cell.resolvedFormId != 0 }
        };
    }

    inline bool IsRenderableCacheReport(const nlohmann::json& report)
    {
        // Discovery reports must never be mistaken for a prepared cell.
        return report.is_object() && report.value("schemaVersion", 0) == 1 &&
            report.value("kind", std::string{}) == "prepared-interior" &&
            report.value("geometryReady", false) &&
            report.value("materialsReady", false) &&
            report.value("lightsReady", false) &&
            report.value("preprocessorParity", false) &&
            report.value("renderable", false);
    }
}
