#include "DetailMaps.h"

#include <DirectXTex.h>

#include "Globals.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/MathUtils.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.detail_maps."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DetailMaps::Settings,
	Enabled,
	Tiling,
	AlbedoStrength,
	NormalStrength,
	RoughnessStrength,
	FadeStart,
	FadeEnd)

namespace
{
	constexpr uint32_t DetailFlag = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::DetailMaps);
	constexpr uint32_t ReflectionsFlag = static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);
	constexpr uint32_t TextureSlot = 23;
	constexpr size_t MaxEntries = 8192;
	constexpr int32_t NoMatch = -1;
	constexpr int32_t Excluded = -2;
	constexpr uint32_t NoSet = std::numeric_limits<uint32_t>::max();

	bool IsFirstPersonPass()
	{
		auto* accumulator = *globals::game::currentAccumulator.get();
		return accumulator && accumulator->GetRuntimeData().renderMode == RE::BSShaderAccumulator::RENDER_MODE::kFirstPersonView;
	}

	bool IsDetailDescriptor(uint32_t a_descriptor, bool a_allowForward)
	{
		using Flags = SIE::ShaderCache::LightingShaderFlags;
		using enum SIE::ShaderCache::LightingShaderTechniques;
		constexpr uint32_t excluded = static_cast<uint32_t>(Flags::ModelSpaceNormals) | static_cast<uint32_t>(Flags::WorldMap) | static_cast<uint32_t>(Flags::LandscapeSeams) |
		                              static_cast<uint32_t>(Flags::FurShells) | static_cast<uint32_t>(Flags::FurShellsDepth) | DetailFlag;
		if ((a_descriptor & excluded) != 0 || (!a_allowForward && (a_descriptor & static_cast<uint32_t>(Flags::Deferred)) == 0))
			return false;

		if ((a_descriptor & static_cast<uint32_t>(Flags::TruePbr)) != 0 && ((a_descriptor >> 24) & 0x3F) != 0)
			return false;

		switch (static_cast<SIE::ShaderCache::LightingShaderTechniques>((a_descriptor >> 24) & 0x3F)) {
		case None:
		case Envmap:
		case Glowmap:
		case Parallax:
		case ParallaxOcc:
		case MultilayerParallax:
		case TreeAnim:
		case MultiIndexSparkle:
			return true;
		default:
			return false;
		}
	}

	std::string ToLower(std::string_view a_text)
	{
		std::string text(a_text);
		std::ranges::transform(text, text.begin(), [](char a_char) {
			return a_char == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(a_char)));
		});
		return text;
	}

	std::string NormalizePath(const char* a_path)
	{
		std::string path = ToLower(a_path);
		if (path.starts_with("data\\"))
			path.erase(0, 5);
		if (!path.starts_with("textures\\"))
			path.insert(0, "textures\\");
		if (path.starts_with("textures\\pbr\\"))
			path.erase(9, 4);
		return path;
	}

	bool WildcardMatch(std::string_view a_text, std::string_view a_pattern)
	{
		while (!a_pattern.empty()) {
			if (a_pattern.starts_with("**")) {
				a_pattern.remove_prefix(2);
				if (a_pattern.empty())
					return true;
				if (a_pattern.front() == '\\' && WildcardMatch(a_text, a_pattern.substr(1)))
					return true;
				for (size_t index = 0; index <= a_text.size(); ++index)
					if (WildcardMatch(a_text.substr(index), a_pattern))
						return true;
				return false;
			}
			if (a_pattern.front() == '*') {
				a_pattern.remove_prefix(1);
				for (size_t index = 0; index <= a_text.size(); ++index) {
					if (WildcardMatch(a_text.substr(index), a_pattern))
						return true;
					if (index < a_text.size() && a_text[index] == '\\')
						break;
				}
				return false;
			}
			if (a_text.empty() || (a_pattern.front() == '?' ? a_text.front() == '\\' : a_pattern.front() != a_text.front()))
				return false;
			a_pattern.remove_prefix(1);
			a_text.remove_prefix(1);
		}
		return a_text.empty();
	}

	bool MatchesAny(std::string_view a_path, const std::vector<std::string>& a_patterns)
	{
		return std::ranges::any_of(a_patterns, [&](const std::string& a_pattern) { return WildcardMatch(a_path, a_pattern); });
	}

	std::vector<std::string> ReadPatterns(const json& a_object, const char* a_key)
	{
		std::vector<std::string> patterns;
		const auto it = a_object.find(a_key);
		if (it == a_object.end())
			return patterns;
		if (it->is_string()) {
			patterns.push_back(ToLower(it->get<std::string>()));
		} else if (it->is_array()) {
			for (const auto& value : *it)
				if (value.is_string() && !value.get<std::string>().empty())
					patterns.push_back(ToLower(value.get<std::string>()));
		}
		return patterns;
	}

	float ReadNumber(const json& a_object, const char* a_key, float a_default, float a_min, float a_max)
	{
		const auto it = a_object.find(a_key);
		const float value = it != a_object.end() && it->is_number() ? it->get<float>() : a_default;
		return Util::ClampFinite(value, a_min, a_max, a_default);
	}

	std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& a_folder, std::string_view a_extension)
	{
		std::vector<std::filesystem::path> files;
		std::error_code error;
		std::filesystem::directory_iterator it(a_folder, error);
		for (const std::filesystem::directory_iterator end; !error && it != end; it.increment(error)) {
			std::error_code fileError;
			if (it->is_regular_file(fileError) && ToLower(it->path().extension().string()) == a_extension)
				files.push_back(it->path());
		}
		std::ranges::sort(files);
		return files;
	}
}

void DetailMaps::ClampSettings()
{
	settings.Tiling = Util::ClampFinite(settings.Tiling, 0.1f, 8.0f, 1.0f);
	settings.AlbedoStrength = Util::ClampFinite(settings.AlbedoStrength, 0.0f, 4.0f, 1.0f);
	settings.NormalStrength = Util::ClampFinite(settings.NormalStrength, 0.0f, 4.0f, 1.0f);
	settings.RoughnessStrength = Util::ClampFinite(settings.RoughnessStrength, 0.0f, 4.0f, 1.0f);
	settings.FadeStart = Util::ClampFinite(settings.FadeStart, 0.0f, 8000.0f, 400.0f);
	settings.FadeEnd = Util::ClampFinite(settings.FadeEnd, settings.FadeStart + 1.0f, 8001.0f, std::max(settings.FadeStart + 1.0f, 1200.0f));
}

void DetailMaps::RestoreDefaultSettings()
{
	settings = {};
}

void DetailMaps::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampSettings();
}

void DetailMaps::SaveSettings(json& o_json)
{
	o_json = settings;
}

void DetailMaps::DrawSettings()
{
	if (loaded && !HasContent()) {
		Util::Text::WrappedWarning("%s", T(TKEY("pack_missing"), "No detail sets or rules found. Install Detailed Maps."));
		if (ImGui::Button(T(TKEY("get_pack"), "Get Detailed Maps")))
			ShellExecuteA(nullptr, "open", "https://www.nexusmods.com/skyrimspecialedition/mods/197436", nullptr, nullptr, SW_SHOWNORMAL);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("get_pack_tooltip"), "Opens the Nexus page. Install the pack so its Rules and Sets land in Data\\Shaders\\DetailMaps."));
	}

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enabled_tooltip"), "Adds detail to textures matched by the rules. The detail shaders compile at startup while this is on."));

	ImGui::SliderFloat(T(TKEY("tiling"), "Tiling"), &settings.Tiling, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("tiling_tooltip"), "How often the detail repeats across a texture. Higher is finer."));

	ImGui::SliderFloat(T(TKEY("albedo_strength"), "Color Strength"), &settings.AlbedoStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("normal_strength"), "Bump Strength"), &settings.NormalStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("roughness_strength"), "Roughness Strength"), &settings.RoughnessStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

	ImGui::SliderFloat(T(TKEY("fade_start"), "Fade Start"), &settings.FadeStart, 0.0f, 4000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("fade_end"), "Fade End"), &settings.FadeEnd, 0.0f, 4000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("fade_tooltip"), "Detail fades out between these distances in game units and is skipped past the end."));
	ClampSettings();

	if (ImGui::Button(T(TKEY("reload"), "Reload Rules and Sets")))
		Reload();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("reload_tooltip"), "Rereads Data\\Shaders\\DetailMaps\\Rules\\*.json and Data\\Shaders\\DetailMaps\\Sets\\*.dds."));

	ImGui::Text("%s: %zu", T(TKEY("set_count"), "Detail sets"), sets.size());
	ImGui::Text("%s: %zu", T(TKEY("rule_count"), "Rules"), rules.size());
	ImGui::Text("%s: %u", T(TKEY("pass_count"), "Detail draws last frame"), lastPassCount);
	for (const auto& error : ruleErrors)
		ImGui::TextColored(Util::Colors::GetError(), "%s", error.c_str());
}

void DetailMaps::LoadSets()
{
	sets.clear();
	auto* device = globals::d3d::device;
	const auto folder = Util::PathHelpers::GetShadersPath() / "DetailMaps" / "Sets";
	for (const auto& path : ListFiles(folder, ".dds")) {
		DirectX::ScratchImage image;
		if (FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image))) {
			ruleErrors.push_back(std::format("{}: {}", path.filename().string(), T(TKEY("set_load_failed"), "could not be loaded")));
			continue;
		}

		const auto& metadata = image.GetMetadata();
		if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.IsCubemap()) {
			ruleErrors.push_back(std::format("{}: {}", path.filename().string(), T(TKEY("set_not_2d"), "is not a plain 2D texture")));
			continue;
		}

		Set set{ ToLower(path.stem().string()), nullptr };
		if (FAILED(DirectX::CreateShaderResourceView(device, image.GetImages(), image.GetImageCount(), metadata, set.view.put()))) {
			ruleErrors.push_back(std::format("{}: {}", path.filename().string(), T(TKEY("set_create_failed"), "could not be created on the GPU")));
			continue;
		}

		winrt::com_ptr<ID3D11Resource> resource;
		set.view->GetResource(resource.put());
		Util::SetResourceName(resource.get(), "DetailMaps::%s", set.name.c_str());
		Util::SetResourceName(set.view.get(), "DetailMaps::%s SRV", set.name.c_str());
		sets.push_back(std::move(set));
	}
	logger::info("[Detail Maps] Loaded {} detail sets", sets.size());
}

void DetailMaps::LoadRules()
{
	rules.clear();
	excludes.clear();

	const auto folder = Util::PathHelpers::GetShadersPath() / "DetailMaps" / "Rules";
	const auto files = ListFiles(folder, ".json");
	for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
		const auto fileName = files[fileIndex].filename().string();
		json data;
		try {
			std::ifstream stream(files[fileIndex]);
			data = json::parse(stream, nullptr, true, true);
		} catch (const std::exception& e) {
			ruleErrors.push_back(std::format("{}: {}", fileName, e.what()));
			continue;
		}
		if (!data.is_object()) {
			ruleErrors.push_back(std::format("{}: {}", fileName, T(TKEY("rules_not_object"), "expected a JSON object")));
			continue;
		}

		for (auto& pattern : ReadPatterns(data, "exclude"))
			excludes.push_back(std::move(pattern));

		const auto list = data.find("rules");
		if (list == data.end() || !list->is_array())
			continue;

		for (const auto& item : *list) {
			if (!item.is_object())
				continue;

			Rule rule;
			rule.file = fileIndex;
			rule.match = ReadPatterns(item, "match");
			rule.exclude = ReadPatterns(item, "exclude");
			const auto setName = item.contains("set") && item["set"].is_string() ? ToLower(item["set"].get<std::string>()) : std::string();
			const auto set = std::ranges::find(sets, setName, &Set::name);
			if (rule.match.empty() || (set == sets.end() && setName != "none")) {
				ruleErrors.push_back(std::format("{}: {} \"{}\"", fileName, T(TKEY("rule_skipped"), "skipped a rule without patterns or with an unknown set"), setName));
				continue;
			}

			rule.set = set == sets.end() ? NoSet : static_cast<uint32_t>(std::distance(sets.begin(), set));
			rule.scale = ReadNumber(item, "scale", 4.0f, 0.01f, 256.0f);
			rule.albedo = ReadNumber(item, "albedo", 1.0f, 0.0f, 4.0f);
			rule.normal = ReadNumber(item, "normal", 1.0f, 0.0f, 4.0f);
			rule.roughness = ReadNumber(item, "roughness", 1.0f, 0.0f, 4.0f);
			rule.transpose = item.contains("transpose") && item["transpose"].is_boolean() && item["transpose"].get<bool>();
			rule.priority = static_cast<int>(ReadNumber(item, "priority", 0.0f, -1000.0f, 1000.0f));
			rules.push_back(std::move(rule));
		}
	}

	std::ranges::stable_sort(rules, [](const Rule& a_left, const Rule& a_right) {
		return a_left.priority != a_right.priority ? a_left.priority > a_right.priority : a_left.file > a_right.file;
	});
	logger::info("[Detail Maps] Loaded {} rules and {} exclusions from {} files", rules.size(), excludes.size(), files.size());
}

void DetailMaps::Reload()
{
	ruleErrors.clear();
	LoadSets();
	LoadRules();
	entries.clear();
	lastDiffuse = nullptr;
	lastSet = nullptr;
	lastRule = NoMatch;
}

int32_t DetailMaps::MatchRule(const char* a_path) const
{
	if (!a_path || !*a_path)
		return NoMatch;

	const auto path = NormalizePath(a_path);
	if (MatchesAny(path, excludes))
		return Excluded;

	for (size_t index = 0; index < rules.size(); ++index) {
		const auto& rule = rules[index];
		if (MatchesAny(path, rule.match) && !MatchesAny(path, rule.exclude))
			return rule.set == NoSet ? Excluded : static_cast<int32_t>(index);
	}
	return NoMatch;
}

int32_t DetailMaps::FindRule(RE::BSRenderPass* a_pass)
{
	auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
	auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
	if (!diffuse)
		return NoMatch;
	RE::BSTextureSet* set = material->textureSet.get();
	if (diffuse == lastDiffuse && set == lastSet)
		return lastRule;

	auto it = entries.find(diffuse);
	if (it == entries.end() || it->second.name != diffuse->name || (it->second.fallbackSet && it->second.fallbackSet != set)) {
		int32_t rule = MatchRule(diffuse->name.c_str());
		const RE::BSTextureSet* fallbackSet = nullptr;
		if (rule == NoMatch && set) {
			rule = MatchRule(set->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse));
			fallbackSet = set;
		}
		if (entries.size() >= MaxEntries)
			entries.clear();
		it = entries.insert_or_assign(diffuse, Entry{ diffuse->name, std::max(rule, NoMatch), fallbackSet }).first;
	}

	lastDiffuse = diffuse;
	lastSet = set;
	lastRule = it->second.rule;
	return lastRule;
}

bool DetailMaps::ShouldPrecompile(uint32_t a_descriptor) const
{
	return loaded && settings.Enabled && HasContent() && IsDetailDescriptor(a_descriptor, false);
}

bool DetailMaps::IsEligible(uint32_t a_descriptor) const
{
	return loaded && settings.Enabled && HasContent() && IsDetailDescriptor(a_descriptor, IsFirstPersonPass());
}

void DetailMaps::GenerateShaderPermutations(RE::BSShader* a_shader)
{
	if (a_shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return;

	constexpr uint32_t truePbrFlag = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr);
	auto* state = globals::state;
	auto* shaderCache = globals::shaderCache;
	for (const auto& entry : a_shader->pixelShaders) {
		auto vertexDescriptor = entry->id;
		auto pixelDescriptor = entry->id;
		state->ModifyShaderLookup(*a_shader, vertexDescriptor, pixelDescriptor, true);
		if (ShouldPrecompile(pixelDescriptor))
			std::ignore = shaderCache->GetPixelShader(*a_shader, pixelDescriptor | DetailFlag, true);
		if (globals::features::truePBR.loaded && ShouldPrecompile(pixelDescriptor | truePbrFlag))
			std::ignore = shaderCache->GetPixelShader(*a_shader, pixelDescriptor | truePbrFlag | DetailFlag, true);
	}
}

uint32_t DetailMaps::GetLightingVariantFlag() const
{
	return DetailFlag;
}

std::string_view DetailMaps::GetLightingVariantDefine() const
{
	return "DETAIL_MAPS";
}

void DetailMaps::BeginPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass)
{
	if (swapped)
		EndPass();

	auto* state = globals::state;
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty || !IsEligible(state->modifiedPixelDescriptor))
		return;
	if (!globals::shaderCache->IsEnabled() || (state->permutationData.ExtraShaderDescriptor & ReflectionsFlag) != 0)
		return;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	if (a_pass->shaderProperty->flags.any(kDecal, kDynamicDecal))
		return;

	auto* geometry = a_pass->geometry;
	const float distance = IsFirstPersonPass() ? 0.0f : std::max(0.0f, geometry->worldBound.center.GetDistance(Util::GetEyePosition(0)) - geometry->worldBound.radius);
	if (distance >= settings.FadeEnd)
		return;

	const int32_t ruleIndex = FindRule(a_pass);
	if (ruleIndex < 0 || static_cast<size_t>(ruleIndex) >= rules.size())
		return;

	auto* pixelShader = globals::shaderCache->GetPixelShader(*a_shader, state->modifiedPixelDescriptor | DetailFlag);
	if (!pixelShader)
		return;

	const auto& rule = rules[static_cast<size_t>(ruleIndex)];
	auto& perDraw = state->permutationData.PerDrawFeatureData;
	perDraw[0] = float4(rule.scale * settings.Tiling, rule.albedo * settings.AlbedoStrength, rule.normal * settings.NormalStrength, rule.roughness * settings.RoughnessStrength);
	perDraw[1] = float4(settings.FadeEnd, 1.0f / std::max(settings.FadeEnd - settings.FadeStart, 1.0f), rule.transpose ? 1.0f : 0.0f, 0.0f);

	auto* context = globals::d3d::context;
	context->PSGetShader(savedPixelShader.put(), nullptr, nullptr);
	context->PSSetShader(Util::AsReal(pixelShader->shader), nullptr, 0);

	ID3D11ShaderResourceView* view = sets[rule.set].view.get();
	context->PSSetShaderResources(TextureSlot, 1, &view);

	swapped = true;
	++passCount;
}

void DetailMaps::EndPass()
{
	if (!swapped)
		return;
	swapped = false;

	auto* context = globals::d3d::context;
	context->PSSetShader(savedPixelShader.get(), nullptr, 0);
	savedPixelShader = nullptr;

	ID3D11ShaderResourceView* view = nullptr;
	context->PSSetShaderResources(TextureSlot, 1, &view);
}

void DetailMaps::Prepass()
{
	lastPassCount = passCount;
	passCount = 0;
}

void DetailMaps::Reset()
{
	EndPass();
	lastDiffuse = nullptr;
	lastSet = nullptr;
	lastRule = NoMatch;
	if (!entries.empty() && globals::state->IsMainOrLoadingMenuOpen())
		entries.clear();
}

void DetailMaps::SetupResources()
{
	Reload();
}

struct DetailMaps::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::detailMaps.BeginPass(a_shader, a_pass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::detailMaps.EndPass();
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
};

void DetailMaps::PostPostLoad()
{
	// Keep DetailMaps ahead of FurShells in Feature::GetFeatureList(): the last-installed 0x6 hook wins the pixel
	// shader for a pass, and fur passes are only excluded from this swap by FurShells overriding it.
	stl::write_vfunc<0x6, Hooks::BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, Hooks::BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[Detail Maps] Installed hooks");
}

#undef I18N_KEY_PREFIX
