#include "DX12Resources.h"
#include "CompanionProcess.h"
#include "PreparedCellValidation.h"
#include "Utils/FileDigest.h"
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESFile.h>
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
#include <algorithm>
#include <string_view>
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12Resources::Settings, Enabled)

struct DX12Resources::ScanJob
{
    std::atomic<bool> done{ false };
    std::optional<DX12ResourceService::SaveLocationHint> hint;
    std::string error;
};

struct DX12Resources::CatalogJob
{
    std::atomic<bool> done{ false };
    std::string plugin;
    std::vector<DX12ResourceService::PluginInteriorEntry> entries;
    std::string error;
};

struct DX12Resources::PreparationJob
{
    std::atomic<bool> done{ false };
    std::string path;
    std::string error;
    std::string logPath;
    json report;
    double elapsedMs = 0;
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
        if (!file) continue;
        const std::string_view filename = file->GetFilename();
        if (filename.empty()) continue;
        candidates.push_back({ { std::string(filename), cell->GetLocalFormID() },
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
                    if (const auto* file = cell->GetFile(0)) {
                        const std::string_view filename = file->GetFilename();
                        if (!filename.empty()) {
                            detectedCell = DX12ResourceService::CellRequest{
                                std::string(filename), cell->GetLocalFormID() };
                            detectionLocation = cell->GetName() ? cell->GetName() : "";
                            detectionStatus = "Current interior resolved directly from Skyrim";
                            return;
                        }
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


void DX12Resources::RefreshLoadedPlugins()
{
    loadedPlugins.clear();
    pluginListReady = true;
    pluginScanActive = false;
    pluginScanJob.reset();
    pluginScanCursor = 0;
    pluginInteriors.clear();
    chosenInterior.reset();
    selectedPlugin.clear();
    pluginScanStatus = "Choose a loaded plugin to enumerate its interiors";
    if (!dataLoaded.load(std::memory_order_acquire)) return;
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) return;

    const auto add = [&](const RE::TESFile* file) {
        if (!file) return;
        const std::string_view name = file->GetFilename();
        if (name.empty() || name.size() > 120) return;
        loadedPlugins.emplace_back(name);
    };
    const auto* regular = handler->GetLoadedMods();
    if (regular)
        for (uint32_t i = 0; i < handler->GetLoadedModCount(); ++i) add(regular[i]);
    const auto* light = handler->GetLoadedLightMods();
    if (light)
        for (uint32_t i = 0; i < handler->GetLoadedLightModCount(); ++i) add(light[i]);
    std::ranges::sort(loadedPlugins, [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char l, unsigned char r) { return std::tolower(l) < std::tolower(r); });
    });
    const auto unique = std::unique(loadedPlugins.begin(), loadedPlugins.end(),
        [](const auto& a, const auto& b) {
            return DX12ResourceService::SamePluginName(a, b);
        });
    loadedPlugins.erase(unique, loadedPlugins.end());
}

void DX12Resources::StartPluginCellScan(std::string plugin)
{
    selectedPlugin = std::move(plugin);
    pluginInteriors.clear();
    chosenInterior.reset();
    pluginScanCursor = 0;
    pluginScanJob.reset(); // prior worker owns its inputs and exits independently
    pluginScanActive = false;
    if (selectedPlugin.empty()) {
        pluginScanStatus = "Choose a loaded plugin";
        return;
    }
    if (!dataLoaded.load(std::memory_order_acquire)) {
        pluginScanStatus = "Skyrim's plugin load order is not ready";
        return;
    }
    const auto known = std::ranges::find_if(loadedPlugins, [&](const std::string& name) {
        return DX12ResourceService::SamePluginName(name, selectedPlugin);
    });
    if (known == loadedPlugins.end()) {
        pluginScanStatus = "Selected plugin is no longer in the loaded plugin list";
        return;
    }
    const auto data = DX12ResourceService::SkyrimDataDirectory();
    const auto pluginsFile = DX12ResourceService::SkyrimPluginsFile();
    if (pluginsFile.empty() || !std::filesystem::is_regular_file(pluginsFile)) {
        pluginScanStatus = "Skyrim Plugins.txt not found; cannot enumerate on-disk CELLs";
        return;
    }
    const auto output = data / "SKSE/Plugins/CommunityShaders/DX12-cache/reports" /
        std::format("interiors-{}-{}-{}.json", GetCurrentProcessId(), GetTickCount64(), pluginScanSequence++);
    const auto log = data / "SKSE/Plugins/CommunityShaders/DX12-cache/logs" /
        (output.stem().wstring() + L".log");
    auto next = std::make_shared<CatalogJob>();
    next->plugin = selectedPlugin;
    try {
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([next, data, pluginsFile, output, log] {
            try {
                const auto result = DX12ResourceService::RunCompanion(
                    { L"--list-cells", data.wstring(), std::filesystem::path(next->plugin).wstring(),
                        pluginsFile.wstring(), output.wstring() }, log);
                if (!result.error.empty()) {
                    next->error = result.error + ": " + result.log;
                } else {
                    std::error_code ec;
                    if (std::filesystem::file_size(output, ec) > 32 * 1024 * 1024 || ec)
                        throw std::runtime_error("Companion interior catalog size invalid");
                    std::ifstream input(output, std::ios::binary);
                    if (!input) throw std::runtime_error("Companion interior catalog was not created");
                    const json report = json::parse(input);
                    if (!report.is_object() ||
                        report.at("kind") != "dx12-resources-plugin-interiors" ||
                        report.at("schemaVersion") != 1 ||
                        !DX12ResourceService::SamePluginName(
                            report.at("selectedPlugin").get<std::string>(), next->plugin))
                        throw std::runtime_error("Companion interior catalog contract mismatch");
                    const auto& records = report.at("interiors");
                    if (!records.is_array() || records.size() > 200000 ||
                        report.at("interiorCount").get<size_t>() != records.size())
                        throw std::runtime_error("Companion interior catalog record count invalid");
                    for (const auto& record : records) {
                        const auto owner = record.at("plugin").get<std::string>();
                        const auto id = record.at("localId").get<uint32_t>();
                        const auto parsed = DX12ResourceService::ParseCellRequest(owner, std::format("{:X}", id));
                        if (!parsed) throw std::runtime_error("Companion returned invalid CELL identity");
                        const auto relation = record.at("relation").get<std::string>();
                        if (relation != "defined" && relation != "override")
                            throw std::runtime_error("Companion returned invalid CELL relation");
                        DX12ResourceService::PluginInteriorEntry entry;
                        entry.identity = *parsed;
                        entry.editorId = record.at("editorId").get<std::string>();
                        entry.displayName = record.at("displayName").get<std::string>();
                        entry.relation = relation == "defined" ?
                            DX12ResourceService::PluginCellRelation::Defined :
                            DX12ResourceService::PluginCellRelation::Override;
                        next->entries.push_back(std::move(entry));
                    }
                    DX12ResourceService::SortInteriorCatalog(next->entries);
                }
            } catch (const std::exception& error) { next->error = error.what(); }
            next->done.store(true, std::memory_order_release);
        });
        pluginScanJob = std::move(next);
        pluginScanActive = true;
        pluginScanStatus = "Enumerating on-disk CELLs in selected plugin...";
    } catch (const std::exception& error) {
        pluginScanStatus = "Could not start CELL scan: " + std::string(error.what());
    }
}

void DX12Resources::AdvancePluginCellScan()
{
    if (!pluginScanJob || !pluginScanJob->done.load(std::memory_order_acquire)) return;
    const auto completed = std::move(pluginScanJob);
    pluginScanActive = false;
    if (!DX12ResourceService::SamePluginName(completed->plugin, selectedPlugin))
        return; // superseded by a newer plugin selection
    if (!completed->error.empty()) {
        pluginInteriors.clear();
        chosenInterior.reset();
        pluginScanStatus = "Plugin CELL enumeration failed: " + completed->error;
        return;
    }
    pluginInteriors = std::move(completed->entries);
    pluginScanCursor = static_cast<uint32_t>(pluginInteriors.size());
    pluginScanStatus = std::format("Indexed {} interior CELLs from {} (including overrides)",
        pluginInteriors.size(), selectedPlugin);
}

void DX12Resources::StartCellPreparation(const DX12ResourceService::CellRequest& request)
{
    if (preparationJob) {
        preparationStatus = "A CELL preparation job is already running";
        return;
    }
    if (!DX12ResourceService::ParseCellRequest(request.plugin, std::format("{:X}", request.localId))) {
        preparationStatus = "Selected CELL identity is invalid";
        return;
    }
    const auto data = DX12ResourceService::SkyrimDataDirectory();
    const auto pluginsFile = DX12ResourceService::SkyrimPluginsFile();
    if (pluginsFile.empty() || !std::filesystem::is_regular_file(pluginsFile)) {
        preparationStatus = "Skyrim Plugins.txt is unavailable";
        return;
    }

    // The producer deliberately refuses output under Data. Stage in the game
    // root, validate all referenced hashes, then publish inside DX12-cache.
    const auto workspace = data.parent_path() / "DX12ResourcesWork" / "cells" / request.plugin /
        std::format("{:06X}-{}-{}", request.localId, GetTickCount64(), preparationSequence++);
    const auto logPath = data / "SKSE/Plugins/CommunityShaders/DX12-cache/logs" /
        std::format("prepare-{}-{}-{}.log", request.localId, GetCurrentProcessId(), GetTickCount64());
    auto next = std::make_shared<PreparationJob>();
    preparationStatus = "Preparing selected CELL with independent DX12ResourcesPrep...";
    preparationReport = json::object();
    preparationPath.clear();
    try {
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([next, request, data, pluginsFile, workspace, logPath] {
            const auto started = std::chrono::steady_clock::now();
            next->logPath = logPath.string();
            try {
                const auto identity = std::format("{}:{:06X}", request.plugin, request.localId);
                const auto process = DX12ResourceService::RunCompanion({
                    data.wstring(), std::filesystem::path(identity).wstring(),
                    workspace.wstring(), pluginsFile.wstring(), L"1"
                }, logPath);
                if (!process.error.empty())
                    throw std::runtime_error(process.error + ": " + process.log);
                next->report = DX12ResourceService::ValidatePreparedCell(workspace, request);
                const auto published = DX12ResourceService::PublishPreparedCell(
                    workspace, data, request);
                next->path = published.string();
            } catch (const std::exception& error) { next->error = error.what(); }
            next->elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            next->done.store(true, std::memory_order_release);
        });
        preparationJob = std::move(next);
    } catch (const std::exception& error) {
        preparationStatus = "Could not start CELL preparation: " + std::string(error.what());
    }
}

void DX12Resources::PollCellPreparation()
{
    if (!preparationJob || !preparationJob->done.load(std::memory_order_acquire)) return;
    const auto completed = std::move(preparationJob);
    preparationLogPath = completed->logPath;
    preparationElapsedMs = completed->elapsedMs;
    if (!completed->error.empty()) {
        preparationStatus = "CELL preparation failed: " + completed->error;
        return;
    }
    preparationReport = std::move(completed->report);
    preparationPath = completed->path;
    const int missing = preparationReport.value("unresolvedAssets", 0);
    preparationStatus = missing == 0 ?
        "CELL preparation validated and cached (runtime coverage still unverified)" :
        std::format("CELL prepared and cached with {} unresolved source assets; NOT render-ready", missing);
}

void DX12Resources::DrawSettings()
{
    std::scoped_lock lock(statusMutex);
    PollJob();
    PollCellPreparation();
    auto* ui = RE::UI::GetSingleton();
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
    if (!pluginListReady && dataLoaded.load(std::memory_order_acquire))
        RefreshLoadedPlugins();
    AdvancePluginCellScan();
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

    ImGui::SeparatorText("Browse interior cells by plugin");
    ImGui::TextDisabled("Interiors are read from the selected plugin file, including unloaded CELLs.");
    if (ImGui::Button("Refresh loaded plugins") && !pluginScanActive)
        RefreshLoadedPlugins();
    ImGui::Text("Loaded plugins: %zu", loadedPlugins.size());
    ImGui::InputTextWithHint("Filter plugins", "Unslaad, Glenmoril...",
        pluginFilter.data(), pluginFilter.size());
    if (ImGui::BeginCombo("Plugin", selectedPlugin.empty() ?
        "(select loaded plugin)" : selectedPlugin.c_str())) {
        const std::string_view filter(pluginFilter.data());
        for (const auto& plugin : loadedPlugins) {
            if (!DX12ResourceService::ContainsAsciiInsensitive(plugin, filter)) continue;
            const bool selected = DX12ResourceService::SamePluginName(selectedPlugin, plugin);
            if (ImGui::Selectable(plugin.c_str(), selected)) StartPluginCellScan(plugin);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (!selectedPlugin.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Rescan selected plugin"))
            StartPluginCellScan(selectedPlugin);
    }
    ImGui::TextWrapped("%s", pluginScanStatus.c_str());
    ImGui::InputTextWithHint("Filter interiors", "Name, EditorID or FormID",
        interiorFilter.data(), interiorFilter.size());
    const std::string_view cellQuery(interiorFilter.data());
    std::vector<size_t> matchingCells;
    matchingCells.reserve(pluginInteriors.size());
    for (size_t i = 0; i < pluginInteriors.size(); ++i)
        if (DX12ResourceService::MatchesInteriorSearch(pluginInteriors[i], cellQuery))
            matchingCells.push_back(i);
    ImGui::Text("Matching interiors: %zu / %zu", matchingCells.size(), pluginInteriors.size());
    if (ImGui::BeginChild("DX12ResourcesInteriorList", ImVec2(0, 220),
        ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(int(matchingCells.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& cell = pluginInteriors[matchingCells[size_t(row)]];
                const bool selected = chosenInterior &&
                    chosenInterior->plugin == cell.identity.plugin &&
                    chosenInterior->localId == cell.identity.localId;
                const auto label = std::format("{}  |  {}  |  {}  [{}]",
                    cell.displayName.empty() ? "(unnamed)" : cell.displayName,
                    cell.editorId.empty() ? "(no EditorID)" : cell.editorId,
                    cell.identity.Key(),
                    cell.relation == DX12ResourceService::PluginCellRelation::Defined ?
                        "defined" : "override");
                ImGui::PushID(int(matchingCells[size_t(row)]));
                if (ImGui::Selectable(label.c_str(), selected))
                    chosenInterior = cell.identity;
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    if (chosenInterior)
        ImGui::Text("Selected interior: %s", chosenInterior->Key().c_str());
    else
        ImGui::TextDisabled("Select any listed interior, even if it is not loaded or in the latest save.");
    // A known selected interior may run source discovery regardless of automatic
    // save/current-cell detection. Only exact CELL validity and job ownership gate it.
    const bool canPrepareChosen = DX12ResourceService::CanPrepareKnownInterior(
        settings.Enabled, dataLoaded.load(std::memory_order_acquire),
        bool(job) || bool(preparationJob), chosenInterior);
    ImGui::BeginDisabled(!canPrepareChosen);
    if (ImGui::Button("Process Selected Cell") && canPrepareChosen)
        StartCellPreparation(*chosenInterior);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Quality 1; off-thread Mutagen/Nifly extraction; no atlas.");
    if (preparationJob) ImGui::TextUnformatted("DX12ResourcesPrep is processing the selected CELL...");
    ImGui::TextWrapped("%s", preparationStatus.c_str());
    if (!preparationPath.empty()) {
        ImGui::TextWrapped("Validated cache: %s", preparationPath.c_str());
        ImGui::Text("Elapsed: %.1f ms", preparationElapsedMs);
        ImGui::Text("Compiled shapes: %d | Placed instances: %d | Unresolved sources: %d",
            preparationReport.value("compiledShapes", 0),
            preparationReport.value("placedShapeInstances", 0),
            preparationReport.value("unresolvedAssets", 0));
    }
    if (!preparationLogPath.empty())
        ImGui::TextWrapped("Preparation log: %s", preparationLogPath.c_str());
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
        { "pluginBrowser", {
            { "loadedPluginCount", loadedPlugins.size() },
            { "selectedPlugin", selectedPlugin },
            { "scanActive", pluginScanActive },
            { "scanProgress", pluginScanCursor },
            { "interiorsFound", pluginInteriors.size() },
            { "status", pluginScanStatus },
            { "selectedCell", chosenInterior ? chosenInterior->Key() : std::string{} }
        } },
        { "dataLoaded", dataLoaded.load(std::memory_order_acquire) },
        { "stage", "discovery-only" }, { "prepared", false },
        { "workerBusy", bool(job) || bool(preparationJob) }, { "status", status },
        { "companionPreparation", {
            { "running", bool(preparationJob) }, { "status", preparationStatus },
            { "output", preparationPath }, { "log", preparationLogPath },
            { "elapsedMs", preparationElapsedMs }, { "report", preparationReport }
        } },
        { "reportSaved", reportSaved }, { "reportPath", reportPath },
        { "lastElapsedMs", lastElapsedMs }, { "report", latestReport } };
}
