#include "DX12Resources.h"
#include "Utils/FileDigest.h"
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/U/UI.h>
#include <RE/M/MainMenu.h>
#include <shlobj.h>
#pragma comment(lib, "Shell32.lib")
#include <BS_thread_pool.hpp>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <vector>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12Resources::Settings, Enabled)

struct DX12Resources::ScanJob
{
    std::atomic<bool> done{ false };
    std::optional<DX12ResourceService::SaveLocationHint> hint;
    std::string error;
};

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

    /** @brief Hash one selected plugin without resolving all overrides/NIFs. */
    bool HashSelectedPlugin(const std::string& plugin, std::string& sha, uint64_t& bytes)
    {
        const auto path = std::filesystem::path("Data") / plugin;
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{ handle };
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            !GetFileSizeEx(handle, &size) || size.QuadPart <= 0) return false;
        auto digest = Util::FileDigest::Sha256HandleHex(handle);
        if (!digest) return false;
        sha = std::move(*digest);
        bytes = uint64_t(size.QuadPart);
        return true;
    }

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

void DX12Resources::SubmitDetectedRequest(const DX12ResourceService::CellRequest& request)
{
    // Only the main/UI thread resolves live RE forms. No engine pointer enters the worker.
    if (!dataLoaded.load(std::memory_order_acquire)) {
        status = "Skyrim data files are not loaded";
        return;
    }
    auto* handler = RE::TESDataHandler::GetSingleton();
    const auto* cell = handler ?
        handler->LookupForm<RE::TESObjectCELL>(request.localId, request.plugin) : nullptr;
    if (!cell) {
        status = "Detected CELL record no longer resolves in the active load order";
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
        pool.detach_task([source = std::move(source), output = next]() mutable {
            const auto started = std::chrono::steady_clock::now();
            try {
                HashSelectedPlugin(source.requested.plugin,
                    source.selectedPluginSha256, source.selectedPluginBytes);
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

void DX12Resources::PollAutomaticDetection()
{
    if (!scanJob || !scanJob->done.load(std::memory_order_acquire)) return;
    const auto completed = std::move(scanJob);
    detectedCell.reset();
    detectionSource = "Latest save";
    if (!completed->hint) {
        detectionStatus = completed->error.empty() ?
            "No valid Skyrim SE save found in the standard Saves folders." : completed->error;
        return;
    }
    detectionSave = completed->hint->fileName;
    detectionLocation = completed->hint->locationName;
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        detectionStatus = "Skyrim's CELL form registry is unavailable";
        return;
    }
    std::vector<DX12ResourceService::InteriorCellCandidate> candidates;
    // Engine form access stays on the main/UI thread, never the save-file worker.
    for (const auto* cell : handler->GetFormArray<RE::TESObjectCELL>()) {
        if (!cell || !cell->IsInteriorCell()) continue;
        const char* editor = cell->GetFormEditorID();
        const char* display = cell->GetName();
        if (!DX12ResourceService::SameCellHint(
                editor ? std::string_view(editor) : std::string_view{}, completed->hint->editorId) &&
            !DX12ResourceService::SameCellHint(
                display ? std::string_view(display) : std::string_view{}, completed->hint->locationName))
            continue;
        const auto* file = cell->GetFile(0);
        if (!file || !file->GetFilename()) continue;
        candidates.push_back({ { file->GetFilename(), cell->GetLocalFormID() },
            editor ? editor : "", display ? display : "" });
    }
    const auto result = DX12ResourceService::ResolveSaveCell(*completed->hint, candidates);
    detectedCell = result.cell;
    detectionStatus = result.cell ?
        "Interior resolved: " + result.explanation :
        "No interior selected: " + result.explanation;
}

void DX12Resources::BeginAutomaticDetection(bool mainMenu)
{
    if (!dataLoaded.load(std::memory_order_acquire)) {
        detectionStatus = "Waiting for Skyrim to load plugin CELL records";
        return;
    }
    if (!mainMenu) {
        detectedCell.reset();
        detectionSource = "Current game cell";
        detectionSave.clear();
        detectionLocation.clear();
        if (const auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (const auto* cell = player->GetParentCell()) {
                if (cell->IsInteriorCell()) {
                    if (const auto* file = cell->GetFile(0); file && file->GetFilename()) {
                        detectedCell = DX12ResourceService::CellRequest{
                            file->GetFilename(), cell->GetLocalFormID() };
                        detectionLocation = cell->GetName() ? cell->GetName() : "";
                        detectionStatus = "Current interior resolved directly from Skyrim";
                        return;
                    }
                }
            }
        }
        detectionStatus = "No current closed interior; only interior cells are supported";
        return;
    }
    if (detectionRequested) return;
    detectionRequested = true;
    detectedCell.reset();
    detectionSource = "Latest save";
    detectionStatus = "Scanning Skyrim save headers...";
    std::vector<std::filesystem::path> folders;
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)) &&
        documents) {
        const std::filesystem::path root(documents);
        folders.push_back(root / "My Games" / "Skyrim Special Edition" / "Saves");
        folders.push_back(root / "My Games" / "Skyrim Special Edition GOG" / "Saves");
        CoTaskMemFree(documents);
    }
    if (folders.empty()) {
        detectionStatus = "Windows Documents folder could not be resolved";
        return;
    }
    auto next = std::make_shared<ScanJob>();
    try {
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([next, folders = std::move(folders)] {
            try { next->hint = DX12ResourceService::LatestSaveLocation(folders); }
            catch (const std::exception& ex) { next->error = ex.what(); }
            next->done.store(true, std::memory_order_release);
        });
        scanJob = std::move(next);
    } catch (const std::exception& ex) { detectionStatus = ex.what(); }
}

void DX12Resources::DrawSettings()
{
    std::scoped_lock lock(statusMutex);
    PollJob();
    const auto* ui = RE::UI::GetSingleton();
    const bool mainMenu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
    if (!lastDetectionMainMenu || *lastDetectionMainMenu != mainMenu) {
        // Return to menu must not reuse an in-game CELL; leaving menu must
        // discard speculative save hints from an old detached scan.
        lastDetectionMainMenu = mainMenu;
        detectionRequested = false;
        scanJob.reset(); // worker owns its input/output independently
        detectedCell.reset();
        detectionSave.clear();
        detectionLocation.clear();
        detectionSource = "Not detected";
        detectionStatus = "Detecting active interior...";
    }
    PollAutomaticDetection();
    ImGui::Checkbox("Enable DX12 Resources preparation", &settings.Enabled);
    ImGui::TextDisabled("Current interior or latest-save location; no manual IDs required.");
    ImGui::BeginDisabled(!settings.Enabled);
    ImGui::BeginDisabled(bool(scanJob));
    if (ImGui::Button("Refresh detected interior")) {
        detectionRequested = false;
        detectedCell.reset();
    }
    ImGui::EndDisabled();
    if (!scanJob) BeginAutomaticDetection(mainMenu);
    ImGui::SeparatorText("Detected interior");
    ImGui::Text("Source: %s", detectionSource.c_str());
    if (!detectionSave.empty()) ImGui::TextWrapped("Save: %s", detectionSave.c_str());
    if (!detectionLocation.empty()) ImGui::TextWrapped("Location: %s", detectionLocation.c_str());
    if (detectedCell) ImGui::Text("Resolved CELL: %s", detectedCell->Key().c_str());
    ImGui::TextWrapped("%s", detectionStatus.c_str());
    ImGui::TextDisabled("Only closed interiors. Ambiguous names are not selected.");
    ImGui::BeginDisabled(!detectedCell || bool(job));
    if (ImGui::Button("Prepare Detected Interior (source discovery)") && detectedCell)
        SubmitDetectedRequest(*detectedCell);
    ImGui::EndDisabled();
    if (job) ImGui::TextUnformatted("Source-discovery worker running...");
    ImGui::TextWrapped("%s", status.c_str());
    if (reportSaved) {
        ImGui::TextWrapped("Report: %s", reportPath.c_str());
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
        { "detection", { { "source", detectionSource },
            { "status", detectionStatus },
            { "location", detectionLocation }, { "save", detectionSave },
            { "resolved", detectedCell.has_value() },
            { "cell", detectedCell ? detectedCell->Key() : std::string{} } } },
        { "dataLoaded", dataLoaded.load(std::memory_order_acquire) },
        { "stage", "discovery-only" }, { "prepared", false },
        { "workerBusy", bool(job) }, { "status", status },
        { "reportSaved", reportSaved }, { "reportPath", reportPath },
        { "lastElapsedMs", lastElapsedMs }, { "report", latestReport } };
}
