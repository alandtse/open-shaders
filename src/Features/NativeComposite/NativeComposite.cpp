#include "NativeComposite.h"
#include "Utils/DevBenchUx.h"
#include "State.h"
#include "Globals.h"
#include "Features/LinearLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeCompositeFeature::Settings, Enabled, AddLighting, LightingStrength)

namespace
{
	const char* LightingStatus(uint32_t a_state)
	{
		switch (a_state) {
		case 1: return "Waiting for current-frame lighting";
		case 2: return "Raster pass-through: color space unavailable";
		case 3: return "Additive lighting submitted";
		case 4: return "Raster pass-through: lighting pipeline or format unavailable";
		default: return "Additive lighting disabled";
		}
	}
}

void NativeCompositeFeature::DrawSettings()
{
	std::scoped_lock lock(settingsMutex);
	ImGui::Checkbox(T("feature.nativecomposite.enabled", "Enable Native Composite"), &settings.Enabled);
	ImGui::Checkbox(T("feature.nativecomposite.add_lighting", "Add native direct lighting"), &settings.AddLighting);
	ImGui::SliderFloat(T("feature.nativecomposite.lighting_strength", "Lighting strength"), &settings.LightingStrength, 0.0f, 2.0f);
	enabled = settings.Enabled;
	ImGui::TextDisabled("%s", pass.Status().c_str());
	ImGui::TextDisabled("%s", LightingStatus(lightingStatus.load()));
	ImGui::TextDisabled("Lighting submissions: %llu", static_cast<unsigned long long>(lightingSubmissions.load()));
}

void NativeCompositeFeature::LoadSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	settings = a_json;
	settings.LightingStrength = std::isfinite(settings.LightingStrength) ? std::clamp(settings.LightingStrength, 0.0f, 2.0f) : 1.0f;
	enabled = settings.Enabled;
}

void NativeCompositeFeature::SaveSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	a_json = settings;
}

void NativeCompositeFeature::RestoreDefaultSettings()
{
	std::scoped_lock lock(settingsMutex);
	settings = {};
	enabled = settings.Enabled;
}

void NativeCompositeFeature::RegisterUxActions()
{
	FEATURE_COMMAND("configure", "Set Native Composite enabled, addLighting and lightingStrength.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativeCompositeFeature*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		if (args.contains("enabled")) feature.settings.Enabled = args.at("enabled").get<bool>();
		if (args.contains("addLighting")) feature.settings.AddLighting = args.at("addLighting").get<bool>();
		if (args.contains("lightingStrength")) {
			const float value = args.at("lightingStrength").get<float>();
			feature.settings.LightingStrength = std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 1.0f;
		}
		feature.enabled = feature.settings.Enabled;
	});
}

json NativeCompositeFeature::GetDiagnostics()
{
	const auto state = lightingStatus.load();
	return { { "enabled", enabled.load() }, { "captures", captures.load() }, { "lightingSubmissions", lightingSubmissions.load() },
		{ "lightingStatus", LightingStatus(state) } };
}

std::shared_ptr<const NativeComposite::Frame> NativeCompositeFeature::Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
	ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
	uint32_t a_frame, uint32_t a_width, uint32_t a_height, NativeComposite::Transfer a_transfer)
{
	if (!loaded || !enabled)
		return {};
	Settings selected;
	{ std::scoped_lock lock(settingsMutex); selected = settings; }
	std::shared_ptr<const NativeLightingFeature::Contribution> lighting;
	const bool unsupportedGamut = a_transfer == NativeComposite::Transfer::Unknown ||
		(a_transfer == NativeComposite::Transfer::Linear && globals::features::linearLighting.settings.enableACEScg);
	lightingStatus = !selected.AddLighting || selected.LightingStrength <= 0 ? 0 : unsupportedGamut ? 2 : 1;
	if (selected.AddLighting && selected.LightingStrength > 0 && globals::game::graphicsState && !unsupportedGamut)
		lighting = globals::features::nativeLighting.GetContribution(globals::game::graphicsState->GetFrameCount());
	auto frame = pass.Capture(a_device11, a_context11, a_device12, a_queue, a_source, a_frame, a_width, a_height, a_transfer, lighting, selected.LightingStrength);
	if (frame) ++captures;
	if (frame && frame->lightingSubmitted) { ++lightingSubmissions; lightingStatus = 3; }
	else if (lighting) lightingStatus = 4;
	return frame;
}
