#pragma once

#include "Feature.h"
#include "CellRequest.h"
#include "AutoCellResolver.h"
#include "PluginCellCatalog.h"
#include <optional>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <array>
#include <memory>
#include <string>
#include <vector>

/** @brief Standalone interior preparation service with automatic CELL identification. */
struct DX12Resources final : Feature
{
    struct Settings { bool Enabled = true; } settings;

    std::string GetName() override { return "DX12 Resources"; }
    std::string GetShortName() override { return "DX12Resources"; }
    std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
    bool IsCore() const override { return true; }
    bool IsInMenu() const override { return true; }
    bool SupportsVR() override { return false; }
    void DrawSettings() override;
    void LoadSettings(json& input) override;
    void SaveSettings(json& output) override;
    void RestoreDefaultSettings() override;
    void DataLoaded() override { dataLoaded.store(true, std::memory_order_release); }
    json GetDiagnostics() override;
    std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
    {
        return { "Automatic interior identification and companion-based CELL processing.",
                 { "Latest-save or current interior detection", "Plugin-scoped interior selection",
                   "Independent on-disk CELL preparation job",
                   "Versioned DX12-cache reports" } };
    }

private:
    struct ScanJob;
    struct CatalogJob;
    struct PreparationJob;
    void BeginAutomaticDetection(bool mainMenu);
    void PollAutomaticDetection();
    void RefreshLoadedPlugins();
    void StartPluginCellScan(std::string plugin);
    void AdvancePluginCellScan();
    void StartCellPreparation(const DX12ResourceService::CellRequest& request);
    void PollCellPreparation();
    std::atomic<bool> dataLoaded{ false };
    std::mutex statusMutex;
    std::shared_ptr<ScanJob> scanJob;
    std::shared_ptr<CatalogJob> pluginScanJob;
    std::shared_ptr<PreparationJob> preparationJob;
    std::optional<DX12ResourceService::CellRequest> detectedCell;
    bool detectionRequested = false;
    std::optional<bool> lastDetectionMainMenu;
    std::string detectionSource = "Not detected";
    std::string detectionLocation;
    std::string detectionSave;
    std::string detectionStatus = "Waiting for Skyrim data";

    // One on-demand browser; engine CELL metadata is copied incrementally on
    // the UI thread and never accessed by a detached worker.
    std::vector<std::string> loadedPlugins;
    std::string selectedPlugin;
    std::vector<DX12ResourceService::PluginInteriorEntry> pluginInteriors;
    std::optional<DX12ResourceService::CellRequest> chosenInterior;
    std::string pluginScanStatus = "Choose a loaded plugin to enumerate its interiors";
    std::array<char, 96> pluginFilter{};
    std::array<char, 96> interiorFilter{};
    std::uint32_t pluginScanCursor = 0; // Number of records delivered by the companion.
    std::uint32_t pluginScanSequence = 0;
    std::uint32_t preparationSequence = 0;
    std::string preparationStatus = "No cell preparation started";
    std::string preparationCell;
    std::string preparationPath;
    std::string preparationLogPath;
    double preparationElapsedMs = 0;
    json preparationReport = json::object();
    bool pluginListReady = false;
    bool pluginScanActive = false;

};
