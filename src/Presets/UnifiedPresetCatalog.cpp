#include "UnifiedPresetCatalog.h"
#include "PCH.h"

#include "BaselineManager.h"
#include "CSEditor/FormEditSources.h"
#include "CSEditor/SceneManager/SceneManager.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "CSEditor/SceneManager/SceneSettingsUI.h"
#include "CSEditor/WeatherUtils.h"
#include "Feature.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "PostProcessingMode.h"
#include "Presets/PresetCompatibility.h"
#include "Presets/PresetState.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <span>
#include <stb_image.h>
#include <vector>

using json = nlohmann::json;

namespace
{
	/// Where older builds kept the active pack state, read once if SettingsUser.json has none yet.
	constexpr const char* kLegacyActiveStateFileName = "_active.json";
	/// Matches State::Save, so a state write leaves the file formatted as the user config always is.
	constexpr int kUserSettingsIndent = 1;
	constexpr const char* kBaselinePackIdsKey = "baselinePackIds";
	constexpr const char* kLegacyMetaFileName = "preset.json";
	constexpr size_t kMaxDescriptionLength = 400;
	constexpr std::string_view kTruncationSuffix = "...";
	constexpr size_t kMaxScreenshots = 12;
	constexpr int kRgbaChannels = 4;

	using Util::ToLower;

	constexpr const char* kPipelineKey = "pipeline";
	constexpr const char* kPipelineRevisionKey = "pipelineRevision";

	/** @brief The Effects 11 preset id a pack switches to, read off the form of its id. */

	std::string GetPayloadPresetId(const std::string& packId)
	{
		const auto* pack = UnifiedPresetCatalog::GetSingleton().FindPack(packId);
		return pack && pack->provider ? pack->provider->GetPresetId(packId) : std::string{};
	}

	std::filesystem::path ResolveRelative(const std::filesystem::path& root, const std::string& rel)
	{
		if (rel.empty())
			return {};
		auto path = root / Util::PathHelpers::FromUtf8(rel);
		return std::filesystem::exists(path) ? path : std::filesystem::path{};
	}

	/** @brief The first candidate that exists under root, or an empty path. */
	std::filesystem::path FindFirstExisting(const std::filesystem::path& root, std::span<const char* const> candidates)
	{
		for (const char* name : candidates) {
			if (auto path = ResolveRelative(root, name); !path.empty())
				return path;
		}
		return {};
	}

	std::filesystem::path ResolvePackDirectory(const UnifiedPresetCatalog::PackInfo& pack)
	{
		const auto manifest = Util::FileHelpers::ResolveExistingFile(pack.source == UnifiedPresetCatalog::SourceKind::UnifiedPack ?
																		 UnifiedPresetCatalog::GetPackManifestPath(pack.rootPath) :
																		 pack.rootPath / "enbseries.ini");
		if (!manifest.empty())
			return manifest.parent_path();
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(pack.rootPath, error), end; !error && it != end; it.increment(error)) {
			if (!it->is_regular_file(error))
				continue;
			auto resolved = Util::FileHelpers::ResolveExistingFile(it->path());
			if (resolved.empty())
				continue;
			for (const auto& component : it->path().lexically_relative(pack.rootPath)) {
				(void)component;
				resolved = resolved.parent_path();
			}
			if (Util::IEquals(Util::PathHelpers::ToUtf8(resolved.filename()), Util::PathHelpers::ToUtf8(pack.rootPath.filename())))
				return resolved;
		}
		return {};
	}

	/** @brief The manifest's declared preset type, matched case-insensitively; null when absent or unrecognized. */
	std::optional<UnifiedPresetCatalog::PresetType> ReadPresetType(const json& manifest, const std::string& packId)
	{
		using PresetType = UnifiedPresetCatalog::PresetType;
		const auto it = manifest.find(UnifiedPresetCatalog::kPresetTypeKey);
		if (it == manifest.end())
			return std::nullopt;
		if (it->is_string()) {
			for (auto type : { PresetType::CS, PresetType::E11, PresetType::Baseline })
				if (Util::IEquals(it->get_ref<const std::string&>(), UnifiedPresetCatalog::GetPresetTypeName(type)))
					return type;
		}
		logger::warn("[Presets] Pack '{}' has unknown type '{}'; grouping by detected payloads", packId, it->dump());
		return std::nullopt;
	}

	/** @brief Whether an id from persisted state is a plain pack folder name, never a path or an orphan id. */
	bool IsPackFolderId(std::string_view id)
	{
		return PresetState::IsPackId(id);
	}

	/** @brief A pack folder, the Legacy Effects 11 install, or an orphan Effects 11 preset. */

	bool IsActivePackId(std::string_view id)
	{
		if (IsPackFolderId(id))
			return true;
		for (auto* provider : UnifiedPresetCatalog::GetProviders())
			for (const auto& entry : provider->ScanLibrary())
				if (entry.id == id)
					return true;
		return false;
	}

	/** @brief Feature short names set by a pack's Baseline folder, sorted and de-duplicated. */
	std::vector<std::string> ListBaselineFeatures(const std::string& packId)
	{
		std::vector<std::string> features;
		for (const auto& packPath : Util::PathHelpers::GetUnifiedPackPaths(packId)) {
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(packPath / UnifiedPresetCatalog::kBaselineSubdir, ec)) {
				std::error_code typeEc;
				if (!entry.is_regular_file(typeEc))
					continue;
				auto name = BaselineManager::ParseBaselineFeatureName(entry.path());
				if (!name.empty())
					features.push_back(std::move(name));
			}
		}
		std::ranges::sort(features);
		features.erase(std::unique(features.begin(), features.end()), features.end());
		return features;
	}

	bool IsImageExtension(const std::filesystem::path& path)
	{
		static constexpr std::array<std::string_view, 6> kImageExtensions = { ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".dds" };
		return std::ranges::contains(kImageExtensions, ToLower(Util::PathHelpers::ToUtf8(path.extension())));
	}

	std::string InferDescriptionFromReadme(const std::filesystem::path& packRoot)
	{
		static constexpr const char* kReadmeNames[] = {
			"ReadMe.txt", "README.txt", "readme.txt", "ReadMe.md", "README.md", "readme.md"
		};
		for (const char* name : kReadmeNames) {
			const auto path = packRoot / name;
			std::ifstream in(path);
			if (!in)
				continue;
			std::string line;
			std::string result;
			while (std::getline(in, line)) {
				// Trim trailing CR from Windows text files.
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty()) {
					if (!result.empty())
						break;
					continue;
				}
				if (!result.empty())
					result += ' ';
				result += line;
				if (result.size() > kMaxDescriptionLength)
					break;
			}
			if (!result.empty()) {
				if (result.size() > kMaxDescriptionLength) {
					result.resize(kMaxDescriptionLength - kTruncationSuffix.size());
					result += kTruncationSuffix;
				}
				return result;
			}
		}
		return {};
	}

	/** @brief Fill unset logo, cover and screenshots from conventional file and folder names in the pack root. */
	void InferMissingArtwork(UnifiedPresetCatalog::PackInfo& pack)
	{
		static constexpr const char* kLogoCandidates[] = {
			"logo.png", "logo.jpg", "logo.jpeg", "logo.webp", "logo.dds",
			"preview.png", "preview.jpg", "thumbnail.png", "thumbnail.jpg", "icon.png"
		};
		static constexpr const char* kCoverCandidates[] = {
			"cover.png", "cover.jpg", "cover.jpeg", "cover.webp", "cover.dds",
			"banner.png", "banner.jpg", "header.png", "header.jpg"
		};
		static constexpr const char* kShotDirs[] = {
			"gallery", "screenshots", "Screenshots", "images", "Images", "previews", "Previews"
		};

		if (pack.logoPath.empty())
			pack.logoPath = FindFirstExisting(pack.rootPath, kLogoCandidates);
		if (pack.coverPath.empty())
			pack.coverPath = FindFirstExisting(pack.rootPath, kCoverCandidates);

		if (!pack.screenshotPaths.empty())
			return;

		std::error_code ec;
		for (const char* dirName : kShotDirs) {
			const auto dir = pack.rootPath / dirName;
			if (!std::filesystem::is_directory(dir, ec))
				continue;
			for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
				if (ec || !file.is_regular_file(ec) || !IsImageExtension(file.path()))
					continue;
				pack.screenshotPaths.push_back(file.path());
			}
		}

		// Loose images at pack root (skip logo/cover already chosen).
		for (const auto& file : std::filesystem::directory_iterator(pack.rootPath, ec)) {
			if (ec || !file.is_regular_file(ec) || !IsImageExtension(file.path()))
				continue;
			const auto path = file.path();
			if ((!pack.logoPath.empty() && path == pack.logoPath) ||
				(!pack.coverPath.empty() && path == pack.coverPath))
				continue;
			pack.screenshotPaths.push_back(path);
		}

		std::sort(pack.screenshotPaths.begin(), pack.screenshotPaths.end());
		if (pack.screenshotPaths.size() > kMaxScreenshots)
			pack.screenshotPaths.resize(kMaxScreenshots);
	}
}

UnifiedPresetCatalog& UnifiedPresetCatalog::GetSingleton()
{
	static UnifiedPresetCatalog instance;
	return instance;
}

std::filesystem::path UnifiedPresetCatalog::GetPackManifestPath(const std::filesystem::path& packRoot)
{
	auto manifestPath = packRoot / packRoot.filename();
	manifestPath += ".json";
	std::error_code ec;
	if (!std::filesystem::exists(manifestPath, ec) && std::filesystem::exists(packRoot / kLegacyMetaFileName, ec))
		return packRoot / kLegacyMetaFileName;
	return manifestPath;
}

const char* UnifiedPresetCatalog::GetPresetTypeName(PresetType type)
{
	switch (type) {
	case PresetType::E11:
		return "E11";
	case PresetType::Baseline:
		return "Baseline";
	default:
		return "CS";
	}
}

json UnifiedPresetCatalog::ReadPackManifest(const std::filesystem::path& packRoot, std::string* outError)
{
	const auto manifestPath = GetPackManifestPath(packRoot);
	std::string error;
	if (auto manifest = Util::FileHelpers::ReadJsonFile(manifestPath, "preset pack manifest", PresetState::kMaxDocumentBytes, &error)) {
		if (manifest->is_object())
			return std::move(*manifest);
		error = T("menu.presets.manifest_not_object", "not a JSON object");
	}
	if (outError && !error.empty())
		*outError = I18n::GetSingleton()->Format("menu.presets.invalid_manifest",
			{ { "file", Util::PathHelpers::ToUtf8(manifestPath.filename()) }, { "error", error } }, "Invalid {file}: {error}");
	return json::object();
}

std::vector<PresetProvider*> UnifiedPresetCatalog::GetProviders()
{
	std::vector<PresetProvider*> providers;
	for (auto* feature : Feature::GetFeatureList())
		if (auto* provider = feature->GetPresetProvider())
			providers.push_back(provider);
	return providers;
}

PresetProvider* UnifiedPresetCatalog::GetPayloadProvider(PresetType type)
{
	for (auto* provider : GetProviders())
		if (!provider->GetBackendId().empty() && Util::IEquals(provider->GetManifestType(), GetPresetTypeName(type)))
			return provider;
	return nullptr;
}

std::optional<std::string> UnifiedPresetCatalog::GetActivePayloadPresetId(const PresetProvider& provider) const
{
	const auto* pack = FindPack(activePackId);
	return pack && pack->provider == &provider ? std::optional(provider.GetPresetId(activePackId)) : std::nullopt;
}

std::vector<std::filesystem::path> UnifiedPresetCatalog::GetActivePackRoots() const
{
	return activePackId.empty() || activePackId.contains(':') ? std::vector<std::filesystem::path>{} :
	                                                            Util::PathHelpers::GetUnifiedPackPaths(activePackId);
}

std::filesystem::path UnifiedPresetCatalog::GetActivePackRoot() const
{
	// Orphan ids carry a "source:" prefix, which no pack folder name can contain.
	if (activePackId.empty() || activePackId.contains(':'))
		return {};
	return Util::PathHelpers::GetUnifiedPackPath(activePackId);
}

std::filesystem::path UnifiedPresetCatalog::ResolveActivePackPath(const std::filesystem::path& path) const
{
	std::error_code ec;
	if (path.empty() || path.is_absolute() || std::filesystem::exists(path, ec))
		return path;
	if (const auto packPath = PresetState::FindFirstExistingPayload(GetActivePackRoots(), path); !packPath.empty())
		return packPath;
	return path;
}

void UnifiedPresetCatalog::LoadActiveState()
{
	std::optional<json> state;
	if (const auto settings = Util::FileHelpers::ReadJsonObject(Util::PathHelpers::GetSettingsUserPath(), "user config")) {
		if (const auto section = settings->find(kUserSettingsKey); section != settings->end() && section->is_object())
			state = *section;
	}
	if (!state)
		state = Util::FileHelpers::ReadJsonObject(Util::PathHelpers::GetUnifiedPresetsRealPath() / kLegacyActiveStateFileName, "active pack state");
	LoadActiveState(json{ { kUserSettingsKey, state.value_or(json::object()) } });
}

void UnifiedPresetCatalog::LoadActiveState(const json& settings)
{
	activePackId.clear();
	baselinePackIds.clear();
	activePipeline.reset();
	pipelineRevision = 0;
	sceneControlOverrides = json::object();
	const auto section = settings.find(kUserSettingsKey);
	preservedState = PresetState::Normalize(section != settings.end() ? *section : json::object(), IsActivePackId);
	++revision;
	if (const auto overrides = preservedState.find(kSceneControlKey); overrides != preservedState.end() && overrides->is_object()) {
		for (const auto& [packId, choices] : overrides->items())
			if (IsActivePackId(packId) && choices.is_object())
				sceneControlOverrides[packId] = choices;
	}
	activePackId = preservedState.at("activePackId").get<std::string>();
	baselinePackIds = preservedState.at(kBaselinePackIdsKey).get<std::vector<std::string>>();
	if (const auto pipeline = preservedState.find(kPipelineKey); pipeline != preservedState.end() && pipeline->is_string())
		activePipeline = magic_enum::enum_cast<PostProcessingMode::Mode>(pipeline->get<std::string>());
	if (const auto storedRevision = preservedState.find(kPipelineRevisionKey);
		storedRevision != preservedState.end() && storedRevision->is_number_unsigned() &&
		storedRevision->get<uint64_t>() <= std::numeric_limits<uint32_t>::max())
		pipelineRevision = storedRevision->get<uint32_t>();
}

json UnifiedPresetCatalog::SerializeActiveState() const
{
	json state = preservedState;
	state["activePackId"] = activePackId;
	state[kBaselinePackIdsKey] = baselinePackIds;
	state.erase(kPipelineKey);
	state.erase(kPipelineRevisionKey);
	state.erase(kSceneControlKey);
	if (activePipeline) {
		state[kPipelineKey] = magic_enum::enum_name(*activePipeline);
		state[kPipelineRevisionKey] = pipelineRevision;
	}
	if (!sceneControlOverrides.empty())
		state[kSceneControlKey] = sceneControlOverrides;
	return state;
}

bool UnifiedPresetCatalog::SaveActiveState() const
{
	// Only this section is written now; the rest of the user config waits for Save Settings.
	const auto path = Util::PathHelpers::GetSettingsUserPath();
	std::error_code ec;
	auto document = std::filesystem::exists(path, ec) ? Util::FileHelpers::ReadJsonObject(path, "user config") :
	                                                    std::optional<json>(json::object());
	if (!document) {
		logger::warn("[Presets] Active pack state not saved: {} could not be read", Util::PathHelpers::ToUtf8(path));
		return false;
	}
	try {
		(*document)[kUserSettingsKey] = SerializeActiveState();
		if (!Util::FileHelpers::WriteJsonAtomically(path, *document, kUserSettingsIndent, "user config")) {
			logger::warn("[Presets] Failed to save active pack state to {}", Util::PathHelpers::ToUtf8(path));
			return false;
		}
	} catch (const std::exception& e) {
		logger::warn("[Presets] Failed to save active pack state: {}", e.what());
		return false;
	}
	// Left behind, the old file would come back as the state the next time the user config goes missing.
	std::filesystem::remove(Util::PathHelpers::GetUnifiedPresetsRealPath() / kLegacyActiveStateFileName, ec);
	++revision;
	return true;
}

void UnifiedPresetCatalog::SetActivePackId(const std::string& id)
{
	activePackId = id;
	SaveActiveState();
}

bool UnifiedPresetCatalog::UsePackPipeline(PresetProvider& provider, bool active)
{
	// Only a pipeline that took effect is recorded, so the next boot runs what this session does.
	if (!provider.SetPipelineActive(active))
		return false;
	activePipeline = PostProcessingMode::Get();
	++pipelineRevision;
	return true;
}

UnifiedPresetCatalog::PackInfo* UnifiedPresetCatalog::FindPack(const std::string& id)
{
	return const_cast<PackInfo*>(std::as_const(*this).FindPack(id));
}

const UnifiedPresetCatalog::PackInfo* UnifiedPresetCatalog::FindPack(const std::string& id) const
{
	for (const auto& pack : packs) {
		if (pack.id == id)
			return &pack;
	}
	return nullptr;
}

void UnifiedPresetCatalog::DiscoverUnifiedPacks()
{
	for (const auto& packRoot : Util::PathHelpers::ListCommunityShaderEntries(Util::PathHelpers::kUnifiedPresetsSubdir, true)) {
		const auto packId = Util::PathHelpers::ToUtf8(packRoot.filename());
		const auto roots = Util::PathHelpers::GetUnifiedPackPaths(packId);

		std::string manifestError;
		const auto meta = ReadPackManifest(packRoot, &manifestError);
		PackInfo pack{ PackManifest::Read(meta, packId) };
		pack.id = packId;
		pack.source = SourceKind::UnifiedPack;
		pack.rootPath = packRoot;
		pack.valid = manifestError.empty();
		pack.invalidReason = std::move(manifestError);
		pack.type = ReadPresetType(meta, packId);
		if (const auto disableAtBoot = meta.find(kDisableAtBootKey); disableAtBoot != meta.end() && disableAtBoot->is_object()) {
			for (auto it = disableAtBoot->begin(); it != disableAtBoot->end(); ++it) {
				if (it.value().is_boolean())
					pack.disableAtBoot[it.key()] = it.value().get<bool>();
			}
		}

		pack.logoPath = PresetState::FindFirstExistingPayload(roots, Util::PathHelpers::FromUtf8(pack.logo));
		pack.coverPath = PresetState::FindFirstExistingPayload(roots, Util::PathHelpers::FromUtf8(pack.cover));
		for (const auto& shot : pack.screenshots) {
			if (auto path = PresetState::FindFirstExistingPayload(roots, Util::PathHelpers::FromUtf8(shot)); !path.empty())
				pack.screenshotPaths.push_back(std::move(path));
		}
		pack.compat = PresetCompatibility::Evaluate(pack.csVersion, pack.requiredFeatures, pack.requiredPlugins);

		for (auto* provider : GetProviders()) {
			for (const auto& source : roots) {
				if (auto root = provider->ResolvePayloadRoot(source, meta); !root.empty()) {
					pack.provider = provider;
					pack.effects11Root = std::move(root);
					pack.hasEffects11 = true;
					break;
				}
			}
			if (pack.provider)
				break;
		}
		pack.hasCSPresets = std::ranges::any_of(roots, SceneSettingsManager::HasScenePayload);
		pack.hasFormEdits = std::ranges::any_of(roots, FormEditSources::HasPackFormFiles);
		pack.baselineFeatures = ListBaselineFeatures(packId);
		pack.hasBaseline = !pack.baselineFeatures.empty() || !pack.disableAtBoot.empty() ||
		                   (pack.type && *pack.type == PresetType::Baseline);

		if (pack.description.empty())
			pack.description = InferDescriptionFromReadme(packRoot);
		InferMissingArtwork(pack);

		if (!pack.hasEffects11 && !pack.hasCSPresets && !pack.hasFormEdits && !pack.hasBaseline) {
			pack.valid = false;
			if (pack.invalidReason.empty())
				pack.invalidReason = T("menu.presets.missing_payload", "No Effects11, CS Presets, Forms or Base Settings payload found");
		}

		packs.push_back(std::move(pack));
	}
}

void UnifiedPresetCatalog::DiscoverProviderPresets()
{
	for (auto* provider : GetProviders()) {
		for (const auto& entry : provider->ScanLibrary()) {
			if (FindPack(entry.id))
				continue;
			PackInfo pack;
			pack.id = entry.id;
			pack.name = entry.name;
			pack.rootPath = entry.rootPath;
			pack.effects11Root = entry.rootPath;
			pack.hasEffects11 = true;
			pack.valid = entry.valid;
			pack.invalidReason = entry.invalidReason;
			pack.provider = provider;
			pack.source = entry.legacy ? SourceKind::Effects11Legacy : SourceKind::Effects11Orphan;
			packs.push_back(std::move(pack));
		}
	}
}

void UnifiedPresetCatalog::Discover()
{
	ReleaseAllArtwork();
	packs.clear();
	Util::FileHelpers::EnsureDirectoryExists(Util::PathHelpers::GetUnifiedPresetsRealPath());
	LoadActiveState();

	// Before the scan, so the packs it creates are listed by it.
	for (auto* provider : GetProviders())
		provider->PrepareDiscovery();
	DiscoverUnifiedPacks();
	DiscoverProviderPresets();

	// With nothing saved, the Legacy install still runs by default; show it as the active pack without persisting.
	if (activePackId.empty() && !activePipeline) {
		for (auto* provider : GetProviders()) {
			const auto id = provider->GetDefaultActivePackId();
			if (!id.empty() && FindPack(id)) {
				activePackId = id;
				break;
			}
		}
	}

	std::sort(packs.begin(), packs.end(), [](const PackInfo& a, const PackInfo& b) {
		return ToLower(a.name) < ToLower(b.name);
	});

	logger::info("[Presets] Discovered {} pack(s); active='{}'", packs.size(), activePackId.empty() ? "(none)" : activePackId);
}

std::vector<size_t> UnifiedPresetCatalog::Query(std::optional<PresetType> typeFilter, const std::string& search) const
{
	std::vector<size_t> indices;
	indices.reserve(packs.size());

	for (size_t i = 0; i < packs.size(); ++i) {
		const auto& pack = packs[i];
		if (typeFilter && !pack.IsType(*typeFilter))
			continue;

		if (!search.empty()) {
			const bool match = ContainsStringIgnoreCase(pack.name, search) || ContainsStringIgnoreCase(pack.author, search) ||
			                   ContainsStringIgnoreCase(pack.description, search) ||
			                   std::ranges::any_of(pack.tags, [&](const auto& tag) { return ContainsStringIgnoreCase(tag, search); });
			if (!match)
				continue;
		}

		indices.push_back(i);
	}
	return indices;
}

bool UnifiedPresetCatalog::LoadTextureSRV(const std::filesystem::path& path, winrt::com_ptr<ID3D11ShaderResourceView>& outSRV)
{
	outSRV = nullptr;
	if (path.empty() || !std::filesystem::exists(path) || !globals::d3d::device)
		return false;

	// Read via filesystem::path so MO2 / Unicode paths work; stbi_load(char*) uses fopen and can fail.
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		logger::warn("[Presets] Failed to open artwork '{}'", Util::PathHelpers::ToUtf8(path));
		return false;
	}
	const auto fileSize = file.tellg();
	if (fileSize <= 0) {
		logger::warn("[Presets] Empty artwork '{}'", Util::PathHelpers::ToUtf8(path));
		return false;
	}
	file.seekg(0, std::ios::beg);
	std::vector<unsigned char> bytes(static_cast<size_t>(fileSize));
	if (!file.read(reinterpret_cast<char*>(bytes.data()), fileSize)) {
		logger::warn("[Presets] Failed to read artwork '{}'", Util::PathHelpers::ToUtf8(path));
		return false;
	}

	ID3D11ShaderResourceView* srv = nullptr;
	ImVec2 size{};
	const auto ext = ToLower(Util::PathHelpers::ToUtf8(path.extension()));
	bool ok = false;
	if (ext == ".dds") {
		ok = Util::LoadDDSTextureFromFile(globals::d3d::device, Util::PathHelpers::ToUtf8(path).c_str(), &srv, size);
	} else {
		int width = 0, height = 0;
		unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, nullptr, kRgbaChannels);
		if (!pixels) {
			logger::warn("[Presets] stbi failed for '{}': {}", Util::PathHelpers::ToUtf8(path), stbi_failure_reason() ? stbi_failure_reason() : "unknown");
			return false;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = static_cast<UINT>(width);
		desc.Height = static_cast<UINT>(height);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = pixels;
		init.SysMemPitch = static_cast<UINT>(width * kRgbaChannels);

		winrt::com_ptr<ID3D11Texture2D> texture;
		const HRESULT hrTex = globals::d3d::device->CreateTexture2D(&desc, &init, texture.put());
		stbi_image_free(pixels);
		if (FAILED(hrTex) || !texture) {
			logger::warn("[Presets] CreateTexture2D failed for '{}' ({:x})", Util::PathHelpers::ToUtf8(path), static_cast<unsigned>(hrTex));
			return false;
		}
		Util::SetResourceName(texture.get(), "UnifiedPresetCatalog::Artwork");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		const HRESULT hrSrv = globals::d3d::device->CreateShaderResourceView(texture.get(), &srvDesc, &srv);
		if (FAILED(hrSrv) || !srv) {
			logger::warn("[Presets] CreateSRV failed for '{}' ({:x})", Util::PathHelpers::ToUtf8(path), static_cast<unsigned>(hrSrv));
			return false;
		}
		Util::SetResourceName(srv, "UnifiedPresetCatalog::Artwork SRV");
		size = ImVec2(static_cast<float>(width), static_cast<float>(height));
		ok = true;
	}
	if (!ok || !srv)
		return false;

	outSRV.attach(srv);
	return true;
}

bool UnifiedPresetCatalog::EnsureArtwork(PackInfo& pack)
{
	if (pack.artworkLoaded)
		return true;

	LoadTextureSRV(pack.logoPath, pack.logoSRV);
	LoadTextureSRV(pack.coverPath, pack.coverSRV);
	// List thumbs need something: fall back to cover when logo is missing/unloadable.
	if (!pack.logoSRV && pack.coverSRV)
		pack.logoSRV = pack.coverSRV;

	pack.screenshotSRVs.clear();
	pack.screenshotSRVs.reserve(pack.screenshotPaths.size());
	for (const auto& shot : pack.screenshotPaths) {
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		if (LoadTextureSRV(shot, srv))
			pack.screenshotSRVs.push_back(std::move(srv));
	}
	pack.artworkLoaded = true;
	return true;
}

void UnifiedPresetCatalog::ReleaseAllArtwork()
{
	for (auto& pack : packs) {
		pack.logoSRV = nullptr;
		pack.coverSRV = nullptr;
		pack.screenshotSRVs.clear();
		pack.artworkLoaded = false;
	}
}

bool UnifiedPresetCatalog::ApplyChange(const std::function<bool()>& change, const std::function<bool()>& rollback)
try {
	if (SceneSettingsManager::GetSingleton()->HasPendingContextEdit()) {
		SceneSettingsUI::WarnPendingContextEdit();
		return false;
	}
	if (!FormEditSources::CanChangePacks())
		return false;
	const json previousState{ { kUserSettingsKey, SerializeActiveState() } };
	json previousSettings;
	globals::state->SaveToJson(previousSettings);
	const auto previousMode = PostProcessingMode::Get();
	std::vector<std::pair<PresetProvider*, std::string>> previousPayloads;
	for (auto* provider : GetProviders())
		if (const auto selected = provider->GetActivePresetId())
			previousPayloads.emplace_back(provider, *selected);
	FormEditSources::ReloadHistoryGuard history;
	try {
		if (change()) {
			BaselineManager::GetSingleton()->Refresh();
			if (globals::state->ReapplyPresetLayers()) {
				SceneSettingsManager::GetSingleton()->ReloadOverwrites();
				if (FormEditSources::ReloadActivePack() && SaveActiveState()) {
					history.Commit();
					globals::state->CheckSettingsChanged();
					return true;
				}
			}
		}
	} catch (const std::exception& error) {
		logger::error("[Presets] Selection change failed: {}", error.what());
	} catch (...) {
		logger::error("[Presets] Selection change failed after an unknown exception");
	}
	const auto restore = [](const char* layer, auto&& action) {
		try {
			if (!action())
				logger::error("[Presets] Could not restore {}", layer);
		} catch (const std::exception& error) {
			logger::error("[Presets] Could not restore {}: {}", layer, error.what());
		} catch (...) {
			logger::error("[Presets] Could not restore {} after an unknown exception", layer);
		}
	};
	if (rollback)
		restore("preset files", rollback);
	restore("selection", [&] { LoadActiveState(previousState); return true; });
	for (const auto& [provider, selected] : previousPayloads)
		restore("payload", [&] { return provider->Apply(selected, false); });
	restore("baseline settings", [&] { BaselineManager::GetSingleton()->Refresh(); return globals::state->ReapplyPresetLayers(); });
	restore("pipeline", [&] { return PostProcessingMode::Set(previousMode); });
	restore("live settings", [&] { globals::state->LoadFromJson(previousSettings); return true; });
	restore("scene settings", [] { SceneSettingsManager::GetSingleton()->ReloadOverwrites(); return true; });
	restore("form sources", [] { return FormEditSources::ReloadActivePack(); });
	globals::state->CheckSettingsChanged();
	return false;
} catch (const std::exception& error) {
	logger::error("[Presets] Could not prepare selection change: {}", error.what());
	return false;
} catch (...) {
	logger::error("[Presets] Could not prepare selection change after an unknown exception");
	return false;
}

bool UnifiedPresetCatalog::ApplyPack(const std::string& id, bool saveEffects11Current)
{
	auto* pack = FindPack(id);
	if (!pack || !pack->valid)
		return false;
	const bool applied = ApplyChange([&] {
		bool payloadApplied = false;
		if (pack->hasEffects11) {
			if (!pack->provider || !pack->provider->Apply(pack->provider->GetPresetId(pack->id), saveEffects11Current))
				return false;
			payloadApplied = true;
		}
		const bool hasActivePayload = payloadApplied || pack->hasCSPresets || pack->hasFormEdits;
		if (hasActivePayload) {
			activePackId = id;
			const bool providerOwnsPipeline = payloadApplied && (!pack->type ||
																	Util::IEquals(pack->provider->GetManifestType(), GetPresetTypeName(*pack->type)));
			if (!(providerOwnsPipeline && UsePackPipeline(*pack->provider)))
				for (auto* provider : GetProviders())
					if (provider->IsDefaultPipelineProvider() && UsePackPipeline(*provider))
						break;
		}
		if (pack->hasBaseline)
			EnableBaseline(*pack);
		return hasActivePayload || pack->hasBaseline;
	});
	if (applied && pack->hasBaseline)
		if (const auto edited = globals::state->FindEditsUnderPack(pack->id); !edited.empty())
			QueueBaselineOverlap(pack->id, edited);
	return applied;
}

void UnifiedPresetCatalog::DisableActivePack()
{
	if (activePackId.empty())
		return;
	ApplyChange([&] {
		if (const auto* pack = FindPack(activePackId); pack && pack->provider)
			UsePackPipeline(*pack->provider, false);
		activePackId.clear();
		return true;
	});
}

bool UnifiedPresetCatalog::DisablePack(const std::string& id)
{
	if (activePackId != id && !IsBaselineEnabled(id))
		return true;
	return ApplyChange([&] { return DeactivatePack(id); });
}

bool UnifiedPresetCatalog::DeactivatePack(const std::string& id)
{
	if (activePackId == id) {
		if (const auto* pack = FindPack(id); pack && pack->provider)
			if (!UsePackPipeline(*pack->provider, false))
				return false;
		activePackId.clear();
	}
	std::erase(baselinePackIds, id);
	return true;
}

bool UnifiedPresetCatalog::CanDeletePack(const std::string& id) const
{
	const auto* pack = FindPack(id);
	if (!pack || pack->source == SourceKind::Effects11Legacy || pack->rootPath.empty())
		return false;
	const auto folder = Util::PathHelpers::ToUtf8(pack->rootPath.filename());
	const auto library = pack->source == SourceKind::UnifiedPack ? Util::PathHelpers::kUnifiedPresetsSubdir : Util::PathHelpers::kEffects11PresetsSubdir;
	return Util::IsSinglePathComponent(folder) &&
	       std::ranges::any_of(Util::PathHelpers::GetCommunityShaderScanRoots(library), [&](const auto& root) {
			   return root.lexically_normal() == pack->rootPath.parent_path().lexically_normal();
		   });
}

bool UnifiedPresetCatalog::DeletePack(const std::string& id)
{
	if (!CanDeletePack(id))
		return false;
	const auto* pack = FindPack(id);
	const auto path = ResolvePackDirectory(*pack);
	if (path.empty() || !Util::IEquals(Util::PathHelpers::ToUtf8(path.filename()), Util::PathHelpers::ToUtf8(pack->rootPath.filename())) ||
		!Util::IEquals(Util::PathHelpers::ToUtf8(path.parent_path().filename()), "Presets"))
		return false;
	const auto deletedRoot = path.parent_path() / "_deleted";
	const auto destination = deletedRoot / std::format("{}-{}", Util::PathHelpers::ToUtf8(path.filename()), ::GetTickCount64());
	bool moved = false;
	const bool deleted = ApplyChange([&] {
		if (!DeactivatePack(id))
			return false;
		std::error_code error;
		std::filesystem::create_directories(deletedRoot, error);
		if (!error)
			std::filesystem::rename(path, destination, error);
		if (error) {
			logger::error("[Presets] Could not delete '{}': {}", path.string(), error.message());
			return false;
		}
		moved = true;
		return true; }, [&] {
		if (!moved)
			return true;
		std::error_code error;
		std::filesystem::rename(destination, path, error);
		return !error; });
	if (!deleted)
		return false;
	Discover();
	return true;
}

bool UnifiedPresetCatalog::IsBaselineEnabled(const std::string& id) const
{
	return std::ranges::find(baselinePackIds, id) != baselinePackIds.end();
}

void UnifiedPresetCatalog::ReapplyIfInUse(const std::string& id)
{
	if (activePackId == id || IsBaselineEnabled(id))
		ApplyChange([] { return true; });
}

void UnifiedPresetCatalog::EnableBaseline(const PackInfo& pack)
{
	// Re-enabling moves the pack to the end so it wins conflicts against the other Baseline packs.
	std::erase(baselinePackIds, pack.id);
	baselinePackIds.push_back(pack.id);
}

void UnifiedPresetCatalog::LoadSceneControl() const
{
	const auto packRoot = GetActivePackRoot();
	const auto manifest = packRoot.empty() ? json::object() : ReadPackManifest(packRoot);
	auto sceneControl = manifest.value(kSceneControlKey, json::object());
	if (!sceneControl.is_object())
		sceneControl = json::object();
	// The pack's choices are its defaults; the user's own win.
	if (const auto overrides = sceneControlOverrides.find(activePackId); overrides != sceneControlOverrides.end())
		sceneControl.update(*overrides);
	for (auto* provider : GetProviders())
		provider->ApplySceneControl(sceneControl);
}

bool UnifiedPresetCatalog::SetSceneControl(const std::string& featureShortName, bool handedOff)
try {
	auto previousOverrides = sceneControlOverrides;
	std::vector<std::pair<PresetProvider*, bool>> previousChoices;
	for (auto* provider : GetProviders())
		if (provider->HandlesFeature(featureShortName))
			previousChoices.emplace_back(provider, provider->IsFeatureHandedOff(featureShortName));
	if (previousChoices.empty())
		return false;
	try {
		for (const auto& choice : previousChoices)
			choice.first->SetFeatureHandedOff(featureShortName, handedOff);
		if (activePackId.empty())
			return true;
		sceneControlOverrides[activePackId][featureShortName] = handedOff;
		if (SaveActiveState())
			return true;
	} catch (const std::exception& error) {
		logger::error("[Presets] Could not update scene control for {}: {}", featureShortName, error.what());
	} catch (...) {
		logger::error("[Presets] Could not update scene control for {}", featureShortName);
	}
	sceneControlOverrides = std::move(previousOverrides);
	for (const auto& [provider, previous] : previousChoices) {
		try {
			provider->SetFeatureHandedOff(featureShortName, previous);
		} catch (...) {
			logger::error("[Presets] Could not restore scene control for {}", featureShortName);
		}
	}
	return false;
} catch (const std::exception& error) {
	logger::error("[Presets] Could not prepare scene control update for {}: {}", featureShortName, error.what());
	return false;
} catch (...) {
	logger::error("[Presets] Could not prepare scene control update for {}", featureShortName);
	return false;
}

void UnifiedPresetCatalog::QueueBaselineOverlap(const std::string& packId, const std::vector<Feature*>& features)
{
	if (std::ranges::find(overlapPackIds, packId) == overlapPackIds.end())
		overlapPackIds.push_back(packId);
	for (auto* feature : features)
		if (auto name = feature->GetDisplayName(); std::ranges::find(overlapFeatureNames, name) == overlapFeatureNames.end())
			overlapFeatureNames.push_back(std::move(name));

	std::string packNames;
	for (const auto& id : overlapPackIds) {
		const auto* pack = FindPack(id);
		packNames += (packNames.empty() ? "" : ", ") + (pack && !pack->name.empty() ? pack->name : id);
	}
	std::string names;
	for (const auto& name : overlapFeatureNames)
		names += (names.empty() ? "" : ", ") + name;
	const auto count = overlapFeatureNames.size();

	overlapPrompt.title = T("menu.presets.overlap_title", "Base Settings Preset Over Your Changes");
	overlapPrompt.message = std::vformat(T("menu.presets.overlap_message",
											 "{} sets {} features you have changed: {}.\n\n"
											 "Your changes are kept and still win. Use the preset's values to drop them for the settings it sets."),
		std::make_format_args(packNames, count, names));
	overlapPrompt.confirmLabel = T("menu.presets.overlap_use_preset", "Use Preset Values");
	overlapPrompt.cancelLabel = T("menu.presets.overlap_keep", "Keep My Changes");
	overlapPrompt.focus = Util::ConfirmationPopup::Focus::Cancel;
	overlapPrompt.Request();
}

void UnifiedPresetCatalog::DrawBaselineOverlapPrompt()
{
	if (overlapPrompt.Draw())
		for (const auto& packId : overlapPackIds)
			globals::state->ClearEditsUnderPack(packId);
	if (!overlapPrompt.IsOpen()) {
		overlapPackIds.clear();
		overlapFeatureNames.clear();
	}
}

bool UnifiedPresetCatalog::RemoveBaseline(const std::string& id)
{
	if (!IsBaselineEnabled(id))
		return false;
	return ApplyChange([&] { std::erase(baselinePackIds, id); return true; });
}

bool UnifiedPresetCatalog::MoveBaseline(const std::string& id, bool raise)
{
	const auto found = std::ranges::find(baselinePackIds, id);
	if (found == baselinePackIds.end())
		return false;
	const auto index = static_cast<size_t>(found - baselinePackIds.begin());
	if (raise ? index + 1 >= baselinePackIds.size() : index == 0)
		return false;
	const size_t target = raise ? index + 1 : index - 1;
	return ApplyChange([&] { std::swap(baselinePackIds[index], baselinePackIds[target]); return true; });
}

bool UnifiedPresetCatalog::EnsureBaselineManifest(const std::filesystem::path& packRoot, const std::string& displayName)
{
	const auto manifestPath = GetPackManifestPath(packRoot);
	std::error_code ec;
	if (std::filesystem::exists(manifestPath, ec))
		return true;

	json manifest{ { PackManifest::kNameKey, displayName }, { kPresetTypeKey, GetPresetTypeName(PresetType::Baseline) } };
	PackManifest{ .version = PackManifest::kDefaultVersion, .csVersion = PresetCompatibility::CurrentCsVersionString() }.WriteDetails(manifest);
	return Util::FileHelpers::WriteJsonAtomically(manifestPath, manifest, 4, "preset pack manifest");
}

bool UnifiedPresetCatalog::OpenPresetsFolder() const
{
	const auto path = Util::PathHelpers::GetUnifiedPresetsRealPath();
	Util::FileHelpers::EnsureDirectoryExists(path);
	return Util::FileHelpers::OpenWithShell(path);
}

bool UnifiedPresetCatalog::OpenPackFolder(const std::string& id) const
{
	const auto* pack = FindPack(id);
	if (!pack)
		return false;

	// Explorer is outside MO2's VFS, so resolve the pack's own file rather than the plugin's mod root.
	const auto directory = ResolvePackDirectory(*pack);
	if (directory.empty())
		return pack->source == SourceKind::Effects11Legacy && Util::FileHelpers::OpenWithShell(pack->rootPath);
	constexpr std::wstring_view extendedPrefix = LR"(\\?\)";
	constexpr std::wstring_view uncPrefix = LR"(\\?\UNC\)";
	auto path = directory.native();
	if (path.starts_with(uncPrefix))
		path.replace(0, uncPrefix.size(), LR"(\\)");
	else if (path.starts_with(extendedPrefix))
		path.erase(0, extendedPrefix.size());
	return Util::FileHelpers::OpenWithShell(path);
}
