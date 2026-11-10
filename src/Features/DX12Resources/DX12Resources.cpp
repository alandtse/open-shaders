#include "DX12Resources.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Globals.h"
#include "CompanionProcess.h"
#include "PreparedCellValidation.h"
#include "Features/DX12NativeScene/SpatialSubcells.h"
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
#include <vector>
#include <algorithm>
#include <string_view>
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12Resources::Settings, Enabled, AutoPrepareOnCellEntry)

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
    DX12ResourceService::CellRequest request;
    std::optional<DX12ResourceService::PreparedSubcellAddress> tile;
    std::string path;
    std::string error;
    std::string logPath;
    json report;
    double elapsedMs = 0;
};


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

void DX12Resources::StartCellPreparation(const DX12ResourceService::CellRequest& request,
    std::optional<DX12ResourceService::PreparedSubcellAddress> tile, bool predictive)
{
    if (preparationJob) {
        preparationStatus = "A CELL preparation job is already running";
        return;
    }
    if (!DX12ResourceService::ParseCellRequest(request.plugin, std::format("{:X}", request.localId)) ||
        (tile && !tile->Valid())) {
        preparationStatus = "Selected CELL identity is invalid";
        return;
    }
    const auto data = DX12ResourceService::SkyrimDataDirectory();
    const auto pluginsFile = DX12ResourceService::SkyrimPluginsFile();
    if (pluginsFile.empty() || !std::filesystem::is_regular_file(pluginsFile)) {
        preparationStatus = "Skyrim Plugins.txt is unavailable";
        return;
    }

    // Work is isolated beneath DX12-cache/work, never beneath the published cells/.
    // The companion only permits this explicitly scoped write location.
    const auto workspace = data / "SKSE/Plugins/CommunityShaders/DX12-cache/work/cells" / request.plugin /
        std::format("{}-{:06X}-{}-{}", request.worldspace ? "WRLD" : "CELL",
            request.localId, GetTickCount64(), preparationSequence++);
    const auto logPath = data / "SKSE/Plugins/CommunityShaders/DX12-cache/logs" /
        std::format("prepare-{}-{}-{}.log", request.localId, GetCurrentProcessId(), GetTickCount64());
    auto next = std::make_shared<PreparationJob>();
    next->request = request;
    next->tile = tile;
    preparationStatus = tile ? "Preparing a native DX12 subcell..." :
        "Processing CELL with DX12ResourcesPrep...";
    preparationReport = json::object();
    preparationPath.clear();
    preparationLogPath.clear();
    preparationElapsedMs = 0;
    preparationCell = request.Key();
    try {
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([next, request, tile, predictive, data, pluginsFile, workspace, logPath] {
            const auto started = std::chrono::steady_clock::now();
            next->logPath = logPath.string();
            try {
                const auto identity = std::format("{}:{:06X}", request.plugin, request.localId);
                std::vector<std::wstring> args{
                    data.wstring(), std::filesystem::path(identity).wstring(),
                    workspace.wstring(), pluginsFile.wstring(), L"1"
                };
                if (tile) {
                    args.insert(args.begin(), request.worldspace ?
                        L"--worldspace-subcell" : L"--subcell");
                    args.push_back(std::to_wstring(tile->size));
                    args.push_back(std::to_wstring(tile->x));
                    args.push_back(std::to_wstring(tile->y));
                }
                const auto process = DX12ResourceService::RunCompanion(args, logPath,
                    tile ? (predictive ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS) :
                        NORMAL_PRIORITY_CLASS);
                if (!process.error.empty())
                    throw std::runtime_error(process.error + ": " + process.log);
                next->report = DX12ResourceService::ValidatePreparedCell(workspace, request, tile);
                const auto published = DX12ResourceService::PublishPreparedCell(
                    workspace, data, request, tile);
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
        if (completed->tile) {
            const auto identity = std::format("{}:{}:{}:{}",
                completed->request.ScopedKey(), completed->tile->size,
                completed->tile->x, completed->tile->y);
            auto& retry = failedStreamingTiles[identity];
            retry.failures = std::min(retry.failures + 1u, 5u);
            const auto delayMs = 1000ull << std::min(retry.failures - 1u, 5u);
            retry.nextAttemptAtMs = GetTickCount64() + delayMs;
        }
        return;
    }
    preparationReport = std::move(completed->report);
    preparationPath = completed->path;
    if (!completed->tile)
        globals::features::dx12NativeScene.NotifyPreparedCellPublished(
            completed->request.plugin, completed->request.localId);
    const int missing = preparationReport.value("unresolvedAssets", 0);
    preparationStatus = missing == 0 ?
        (completed->tile ? "DX12 subcell cached, GPU residency not yet active" :
            "CELL preparation validated and cached (runtime coverage still unverified)") :
        std::format("CELL prepared with {} unresolved assets; coverage incomplete", missing);
}

void DX12Resources::AdvanceAutomaticInterior(const DX12ResourceService::CellRequest& request)
{
    std::scoped_lock lock(statusMutex);
    // The companion job is independent of menu visibility; dispatch and
    // publication polling happen on the native scene's normal frame path.
    PollCellPreparation();
    if (!settings.Enabled || !settings.AutoPrepareOnCellEntry ||
        !dataLoaded.load(std::memory_order_acquire)) return;
    const auto identity = request.ScopedKey();
    if (identity.empty() || lastAutomaticInterior == identity) return;
    lastAutomaticInterior = identity;
    if (preparationJob) {
        // Another request is running; retry the newly entered interior after
        // that job completes rather than enqueue a concurrent full-cell build.
        lastAutomaticInterior.clear();
        return;
    }
    const auto cache = DX12ResourceService::SkyrimDataDirectory() /
        "SKSE/Plugins/CommunityShaders/DX12-cache" / request.CacheFamily() /
        request.plugin / std::format("{:06X}", request.localId);
    std::error_code ec;
    const bool available = std::filesystem::is_regular_file(cache / "cell-layout.json", ec) &&
        !ec && std::filesystem::is_regular_file(cache / "cell-geometry.rsgp", ec) &&
        !ec && std::filesystem::is_regular_file(cache / "cell-shading.rssp", ec) && !ec;
    if (available) { ++automaticCacheHits; return; }
    ++automaticLaunches;
    StartCellPreparation(request);
}


void DX12Resources::AdvanceStreamingScene(const DX12ResourceService::CellRequest& request,
    uint32_t size, double x, double y, double velocityX, double velocityY)
{
    std::scoped_lock lock(statusMutex);
    PollCellPreparation();
    if (!settings.Enabled || !settings.AutoPrepareOnCellEntry ||
        !dataLoaded.load(std::memory_order_acquire) ||
        !DX12Native::SpatialSubcells::ValidSize(size) ||
        !std::isfinite(x) || !std::isfinite(y)) return;
    const auto scope = request.ScopedKey() + ":" + std::to_string(size);
    if (streamingScope != scope) {
        streamingScope = scope;
        failedStreamingTiles.clear();
        lastStreamingPollMs = 0;
    }
    const auto now = GetTickCount64();
    if (lastStreamingPollMs && now - lastStreamingPollMs < 350) return;
    lastStreamingPollMs = now;
    streamingSize = size;
    const auto center = DX12Native::SpatialSubcells::FromPosition(x, y, size);
    if (std::abs(center.x) > 1'000'000 || std::abs(center.y) > 1'000'000) {
        streamingStatus = "Subcell coordinates exceed supported preparation region";
        return;
    }
    const auto root = DX12ResourceService::SkyrimDataDirectory() /
        "SKSE/Plugins/CommunityShaders/DX12-cache" / request.CacheFamily() /
        request.plugin / std::format("{:06X}", request.localId) / "subcells" /
        std::to_string(size);
    auto isCached = [&](DX12Native::SpatialSubcells::Coordinate tile) {
        const auto dir = root / std::format("{}_{}", tile.x, tile.y);
        std::error_code ec;
        return std::filesystem::is_regular_file(dir / "cell-layout.json", ec) && !ec &&
            std::filesystem::is_regular_file(dir / "cell-geometry.rsgp", ec) && !ec &&
            std::filesystem::is_regular_file(dir / "cell-shading.rssp", ec) && !ec;
    };
    const auto order = DX12Native::SpatialSubcells::OrderedWork(center, velocityX, velocityY);
    streamingActive = streamingSides = streamingCorners = 0;
    for (auto c : order.active) if (isCached(c)) ++streamingActive;
    for (auto c : order.predictive.sides) if (isCached(c)) ++streamingSides;
    for (auto c : order.predictive.corners) if (isCached(c)) ++streamingCorners;
    streamingStatus = std::format("3x3 {}/9; sides {}/12; corners {}/4",
        streamingActive, streamingSides, streamingCorners);
    if (preparationJob) return;

    const auto pending = DX12Native::SpatialSubcells::PendingWork(center, velocityX, velocityY, isCached);
    for (const auto tile : pending) {
        const auto identity = std::format("{}:{}:{}:{}", request.ScopedKey(), size, tile.x, tile.y);
        if (auto retry = failedStreamingTiles.find(identity); retry != failedStreamingTiles.end()) {
            if (retry->second.failures >= 5 || now < retry->second.nextAttemptAtMs)
                continue;
        }
        ++streamingLaunches;
        StartCellPreparation(request,
            DX12ResourceService::PreparedSubcellAddress{ size, tile.x, tile.y },
            streamingActive == 9);
        return;
    }
    if (!pending.empty()) streamingStatus +=
        " | waiting for bounded retry or a refreshed scene scope";
}

void DX12Resources::DrawSettings()
{
    std::scoped_lock lock(statusMutex);
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
    ImGui::Checkbox("Automatically prepare missing native scenes", &settings.AutoPrepareOnCellEntry);
    ImGui::TextDisabled("Companion runs off-thread; a matching cache is reused without restarting preparation.");
    ImGui::TextWrapped("Subcell scheduler: %s", streamingStatus.c_str());
    ImGui::TextDisabled("Active subcell preparation runs below normal CPU priority.");
    ImGui::TextDisabled("Predictive 5x5 prewarm uses idle priority.");
    ImGui::TextDisabled("GPU residency and native TLAS have separate controls.");
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
    const bool canProcessDetected = DX12ResourceService::CanPrepareKnownInterior(
        settings.Enabled, dataLoaded.load(std::memory_order_acquire),
        bool(preparationJob), detectedCell);
    ImGui::BeginDisabled(!canProcessDetected);
    if (ImGui::Button(mainMenu ? "Process Detected Interior" : "Process Current Cell") &&
        canProcessDetected)
        StartCellPreparation(*detectedCell);
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
    // Explicit CELL selection and automatic detection both share the same
    // companion preparation, validation and publication path.
    const bool canPrepareChosen = DX12ResourceService::CanPrepareKnownInterior(
        settings.Enabled, dataLoaded.load(std::memory_order_acquire),
        bool(preparationJob), chosenInterior);
    ImGui::BeginDisabled(!canPrepareChosen);
    if (ImGui::Button("Process Selected Cell") && canPrepareChosen)
        StartCellPreparation(*chosenInterior);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Quality 1; off-thread Mutagen/Nifly extraction; no atlas.");
    if (preparationJob) ImGui::TextUnformatted("DX12ResourcesPrep is processing the requested CELL...");
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
    PollCellPreparation();
    return { { "enabled", settings.Enabled },
        { "autoPrepareOnCellEntry", settings.AutoPrepareOnCellEntry },
        { "automaticLaunches", automaticLaunches }, { "automaticCacheHits", automaticCacheHits },
        { "lastAutomaticInterior", lastAutomaticInterior },
        { "subcellStreaming", { { "scope", streamingScope }, { "status", streamingStatus },
            { "size", streamingSize }, { "activePrepared", streamingActive },
            { "sidesPrepared", streamingSides }, { "cornersPrepared", streamingCorners },
            { "launches", streamingLaunches }, { "failedTiles", failedStreamingTiles.size() },
            { "retryExhausted", std::ranges::count_if(failedStreamingTiles, [](const auto& item) {
                return item.second.failures >= 5;
            }) } } },
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
        { "stage", "cell-processing" }, { "prepared", !preparationPath.empty() },
        { "workerBusy", bool(preparationJob) }, { "status", preparationStatus },
        { "companionPreparation", {
            { "running", bool(preparationJob) }, { "cell", preparationCell },
            { "status", preparationStatus },
            { "output", preparationPath }, { "log", preparationLogPath },
            { "elapsedMs", preparationElapsedMs }, { "report", preparationReport }
        } } };
}
