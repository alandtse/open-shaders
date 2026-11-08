#include "DX12Resources.h"
#include "Utils/FileDigest.h"
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectCELL.h>
#include <BS_thread_pool.hpp>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <vector>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12Resources::Settings, Enabled)

struct DX12Resources::Job
{
    std::atomic<bool> done{ false };
    std::string path;
    std::string error;
    json report;
    double elapsedMs = 0;
};

namespace
{
    constexpr uint64_t kMaxReportBytes = 1024 * 1024;

    /** @brief Atomic, content-addressed discovery report; no engine pointers or GPU work. */
    bool WriteDiscoveryReport(const json& report, std::string& path, std::string& error)
    {
        const std::string bytes = report.dump(2);
        if (bytes.empty() || bytes.size() > kMaxReportBytes) {
            error = "DX12 Resources report size invalid";
            return false;
        }
        const std::span raw(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        const auto sha = Util::FileDigest::Sha256BytesHex(raw);
        if (!sha) { error = "Could not hash DX12 Resources report"; return false; }
        const std::filesystem::path folder =
            "Data/SKSE/Plugins/CommunityShaders/DX12-cache/reports";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec || std::filesystem::is_symlink(std::filesystem::symlink_status(folder, ec)) || ec) {
            error = "DX12-cache report directory unavailable";
            return false;
        }
        const auto final = folder / (*sha + ".json");
        const auto temp = folder / (*sha + ".tmp." + std::to_string(GetCurrentProcessId()) +
            "." + std::to_string(GetTickCount64()));
        const HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = "Could not create DX12-cache temporary report";
            return false;
        }
        DWORD written = 0;
        const bool wrote = WriteFile(handle, bytes.data(), DWORD(bytes.size()),
            &written, nullptr) && written == bytes.size() && FlushFileBuffers(handle);
        CloseHandle(handle);
        if (!wrote) {
            DeleteFileW(temp.c_str());
            error = "Could not write DX12-cache report";
            return false;
        }
        const auto digest = Util::FileDigest::Sha256FileHex(temp);
        if (!digest || *digest != *sha ||
            !MoveFileExW(temp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            error = "DX12-cache report verification or publication failed";
            return false;
        }
        path = final.string();
        return true;
    }
}

void DX12Resources::SubmitManualRequest(const DX12ResourceService::CellRequest& request)
{
    // This lookup is run from the user's manual UI action, NEVER on a background worker.
    if (!dataLoaded.load(std::memory_order_acquire)) {
        status = "Skyrim data files are not loaded";
        return;
    }
    auto* handler = RE::TESDataHandler::GetSingleton();
    const auto* cell = handler ?
        handler->LookupForm<RE::TESObjectCELL>(request.localId, request.plugin) : nullptr;
    if (!cell) {
        status = "CELL record not found for the selected plugin and local FormID";
        return;
    }
    if (!cell->IsInteriorCell()) {
        status = "The resolved CELL is not a closed interior";
        return;
    }
    DX12ResourceService::CellDiscovery source;
    source.requested = request;
    source.resolvedFormId = cell->GetFormID();
    source.cellName = cell->GetName() ? cell->GetName() : "";
    source.isInterior = true;
    source.isAttached = cell->IsAttached();
    if (const auto* file = cell->GetFile(0))
        source.formSource = file->GetFilename();
    auto next = std::make_shared<Job>();
    reportSaved = false;
    status = "Preparing source-discovery report (full geometry extraction not yet ported)";
    try {
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([source = std::move(source), output = next] {
            const auto started = std::chrono::steady_clock::now();
            try {
                output->report = DX12ResourceService::BuildDiscoveryReport(source);
                WriteDiscoveryReport(output->report, output->path, output->error);
            } catch (const std::exception& ex) { output->error = ex.what(); }
            output->elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            output->done.store(true, std::memory_order_release);
        });
        job = std::move(next);
    } catch (const std::exception& ex) { status = ex.what(); }
}

void DX12Resources::PollJob()
{
    if (!job || !job->done.load(std::memory_order_acquire)) return;
    latestReport = job->report;
    reportSaved = job->error.empty() && !job->path.empty();
    reportPath = job->path;
    status = reportSaved ?
        "Source record saved. Geometry/light extraction parity is NOT ready." :
        "Preparation discovery failed: " + job->error;
    lastElapsedMs = job->elapsedMs;
    job.reset();
}

void DX12Resources::DrawSettings()
{
    std::scoped_lock lock(statusMutex);
    PollJob();
    ImGui::Checkbox("Enable manual DX12 Resources preparation", &settings.Enabled);
    ImGui::TextDisabled("Independent from DX12 Native Scene and legacy True PBR.");
    ImGui::BeginDisabled(!settings.Enabled);
    ImGui::InputTextWithHint("Source plugin", "Skyrim.esm", pluginText.data(), pluginText.size());
    ImGui::InputTextWithHint("Local CELL FormID (hex)", "000001", localIdText.data(),
        localIdText.size(), ImGuiInputTextFlags_CharsHexadecimal);
    const auto request = DX12ResourceService::ParseCellRequest(
        pluginText.data(), localIdText.data());
    if (request)
        ImGui::Text("CELL identity: %s", request->Key().c_str());
    else
        ImGui::TextDisabled("Enter an exact source plugin and local CELL FormID.");
    ImGui::TextDisabled("Only closed interiors. No save prediction or automatic processing.");
    ImGui::BeginDisabled(!request || !dataLoaded.load(std::memory_order_acquire) || bool(job));
    if (ImGui::Button("Prepare Selected Cell (source discovery)") && request)
        SubmitManualRequest(*request);
    ImGui::EndDisabled();
    if (job) ImGui::TextUnformatted("Source-discovery worker running...");
    ImGui::TextWrapped("%s", status.c_str());
    if (reportSaved) {
        ImGui::Text("Report: %s", reportPath.c_str());
        ImGui::Text("Elapsed: %.2f ms", lastElapsedMs);
        ImGui::TextDisabled("Geometry, materials and lights are NOT marked ready.");
    }
    ImGui::EndDisabled();
}

void DX12Resources::LoadSettings(json& input)
{
    std::scoped_lock lock(statusMutex);
    settings = input.get<Settings>();
}
void DX12Resources::SaveSettings(json& output)
{
    std::scoped_lock lock(statusMutex);
    output = settings;
}
void DX12Resources::RestoreDefaultSettings()
{
    std::scoped_lock lock(statusMutex);
    settings = {};
}
json DX12Resources::GetDiagnostics()
{
    std::scoped_lock lock(statusMutex);
    PollJob();
    return { { "enabled", settings.Enabled },
        { "dataLoaded", dataLoaded.load(std::memory_order_acquire) },
        { "stage", "discovery-only" }, { "prepared", false },
        { "workerBusy", bool(job) }, { "status", status },
        { "reportSaved", reportSaved }, { "reportPath", reportPath },
        { "lastElapsedMs", lastElapsedMs }, { "report", latestReport } };
}
