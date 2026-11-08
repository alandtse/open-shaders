#pragma once

#include "Feature.h"
#include "CellRequest.h"
#include "CellDiscovery.h"
#include <atomic>
#include <mutex>
#include <array>
#include <memory>
#include <string>

/** @brief Independent manual interior preparation service. It owns no RT shading. */
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
        return { "Manual, standalone DX12 interior resource preparation and validation.",
                 { "Manual plugin/local CELL selection", "Independent preparation job",
                   "Versioned DX12-cache reports" } };
    }

private:
    struct Job;
    void SubmitManualRequest(const DX12ResourceService::CellRequest& request);
    void PollJob();
    std::atomic<bool> dataLoaded{ false };
    std::mutex statusMutex;
    std::shared_ptr<Job> job;
    json latestReport = json::object();
    std::string status = "No CELL selected";
    std::string reportPath;
    bool reportSaved = false;
    double lastElapsedMs = 0;
    std::array<char, 128> pluginText{};
    std::array<char, 16> localIdText{};
};
