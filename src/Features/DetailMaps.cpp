#include "DetailMaps.h"
#include <DirectXTex.h>

#include "Globals.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
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
	constexpr uint32_t PerDrawSlot = 8;
	constexpr uint32_t FirstPersonRenderMode = 0x16;
	constexpr size_t MaxEntries = 8192;
	constexpr int32_t NoMatch = -1;
	constexpr int32_t Excluded = -2;
	constexpr uint32_t NoSet = std::numeric_limits<uint32_t>::max();
	constexpr std::string_view SetsPath = "Data\\Shaders\\DetailMaps\\Sets";
	constexpr std::string_view RulesPath = "Data\\Shaders\\DetailMaps\\Rules";

	bool IsFirstPersonPass()
	{
		auto* accumulator = *globals::game::currentAccumulator.get();
		return accumulator && accumulator->GetRuntimeData().renderMode == FirstPersonRenderMode;
	}

	bool IsDetailDescriptor(uint32_t a_descriptor, bool a_allowForward)
	{
		using Flags = SIE::ShaderCache::LightingShaderFlags;
		using enum SIE::ShaderCache::LightingShaderTechniques;
		constexpr uint32_t excluded = static_cast<uint32_t>(Flags::ModelSpaceNormals) | static_cast<uint32_t>(Flags::WorldMap) | static_cast<uint32_t>(Flags::LandscapeSeams) |
		                              static_cast<uint32_t>(Flags::FurShells) | static_cast<uint32_t>(Flags::FurShellsDepth) | static_cast<uint32_t>(Flags::NoSnowCover) |
		                              static_cast<uint32_t>(Flags::NoWetness) | static_cast<uint32_t>(Flags::Reflections) | DetailFlag;
		if ((a_descriptor & excluded) != 0 || (!a_allowForward && (a_descriptor & static_cast<uint32_t>(Flags::Deferred)) == 0))
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
		return std::isfinite(value) ? std::clamp(value, a_min, a_max) : a_default;
	}

	std::vector<std::filesystem::path> ListFiles(std::string_view a_folder, std::string_view a_extension)
	{
		std::vector<std::filesystem::path> files;
		std::error_code error;
		std::filesystem::directory_iterator it(std::filesystem::path(a_folder), error);
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
	settings.Tiling = std::clamp(std::isfinite(settings.Tiling) ? settings.Tiling : 1.0f, 0.1f, 8.0f);
	settings.AlbedoStrength = std::clamp(std::isfinite(settings.AlbedoStrength) ? settings.AlbedoStrength : 1.0f, 0.0f, 4.0f);
	settings.NormalStrength = std::clamp(std::isfinite(settings.NormalStrength) ? settings.NormalStrength : 1.0f, 0.0f, 4.0f);
	settings.RoughnessStrength = std::clamp(std::isfinite(settings.RoughnessStrength) ? settings.RoughnessStrength : 1.0f, 0.0f, 4.0f);
	settings.FadeStart = std::clamp(std::isfinite(settings.FadeStart) ? settings.FadeStart : 400.0f, 0.0f, 8000.0f);
	settings.FadeEnd = std::clamp(std::isfinite(settings.FadeEnd) ? settings.FadeEnd : 1200.0f, settings.FadeStart + 1.0f, 8001.0f);
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
	for (const auto& path : ListFiles(SetsPath, ".dds")) {
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

	const auto files = ListFiles(RulesPath, ".json");
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
	lastRule = NoMatch;
	bindingValid = false;
	boundView = nullptr;
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
	if (diffuse == lastDiffuse)
		return lastRule;

	auto it = entries.find(diffuse);
	if (it == entries.end() || it->second.name != diffuse->name) {
		int32_t rule = MatchRule(diffuse->name.c_str());
		if (rule == NoMatch && material->textureSet)
			rule = MatchRule(material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse));
		if (entries.size() >= MaxEntries)
			entries.clear();
		it = entries.insert_or_assign(diffuse, Entry{ diffuse->name, std::max(rule, NoMatch) }).first;
	}

	lastDiffuse = diffuse;
	lastRule = it->second.rule;
	return lastRule;
}

bool DetailMaps::ShouldPrecompile(uint32_t a_descriptor) const
{
	return loaded && settings.Enabled && IsDetailDescriptor(a_descriptor, false);
}

bool DetailMaps::IsEligible(uint32_t a_descriptor) const
{
	return loaded && settings.Enabled && perDrawCB && !sets.empty() && !rules.empty() && IsDetailDescriptor(a_descriptor, IsFirstPersonPass());
}

bool DetailMaps::BindPass(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return false;
	if ((globals::state->permutationData.ExtraShaderDescriptor & ReflectionsFlag) != 0)
		return false;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	if (a_pass->shaderProperty->flags.any(kDecal, kDynamicDecal))
		return false;

	auto* geometry = a_pass->geometry;
	const float distance = IsFirstPersonPass() ? 0.0f : std::max(0.0f, geometry->worldBound.center.GetDistance(Util::GetEyePosition()) - geometry->worldBound.radius);
	if (distance >= settings.FadeEnd)
		return false;

	const int32_t ruleIndex = FindRule(a_pass);
	if (ruleIndex < 0 || static_cast<size_t>(ruleIndex) >= rules.size())
		return false;

	const auto& rule = rules[static_cast<size_t>(ruleIndex)];
	const PerDraw perDraw{
		rule.scale * settings.Tiling,
		rule.albedo * settings.AlbedoStrength,
		rule.normal * settings.NormalStrength,
		rule.roughness * settings.RoughnessStrength,
		settings.FadeEnd,
		1.0f / std::max(settings.FadeEnd - settings.FadeStart, 1.0f),
		rule.transpose ? 1.0f : 0.0f,
		0.0f
	};

	auto* context = globals::d3d::context;
	if (!bindingValid || std::memcmp(&perDraw, &lastPerDraw, sizeof(PerDraw)) != 0) {
		perDrawCB->Update(perDraw);
		lastPerDraw = perDraw;
	}

	auto* view = sets[rule.set].view.get();
	if (!bindingValid || view != boundView) {
		context->PSSetShaderResources(TextureSlot, 1, &view);
		boundView = view;
	}

	if (!bindingValid) {
		ID3D11Buffer* buffer = perDrawCB->CB();
		context->PSSetConstantBuffers(PerDrawSlot, 1, &buffer);
	}

	bindingValid = true;
	++passCount;
	return true;
}

void DetailMaps::Reset()
{
	lastPassCount = passCount;
	passCount = 0;
	bindingValid = false;
	lastDiffuse = nullptr;
	if (!entries.empty() && globals::state->IsMainOrLoadingMenuOpen())
		entries.clear();
}

void DetailMaps::SetupResources()
{
	perDrawCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PerDraw>(), "DetailMaps::PerDraw");
	Reload();
}

#undef I18N_KEY_PREFIX
