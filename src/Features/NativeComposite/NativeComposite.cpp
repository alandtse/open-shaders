#include "NativeComposite.h"
#include "Utils/DevBenchUx.h"
#include "State.h"
#include "Globals.h"
#include "Features/LinearLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeCompositeFeature::Settings, Enabled, AddLighting, LightingStrength, DirectSpatialReconstruction, PersistLab, ShadowLayerEnabled, ShadowContribution, ShadowBlend, ShadowRadiusPixels, ShadowReceiverTolerance, ShadowPreview, ShadowAfterLighting, ShadowSpatialReconstruction)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NativeComposite::Policy, diffuse, specular, visual)

namespace
{
	const char* LightingStatus(uint32_t a_state)
	{
		switch (a_state) {
		case 1: return "Waiting for current-frame lighting";
		case 2: return "Raster pass-through: color space unavailable";
		case 3: return "Additive lighting submitted";
		case 4: return "Raster pass-through: lighting pipeline or format unavailable";
		case 5: return "Independent DX12 shadow layer submitted";
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
	if (ImGui::Checkbox("Spatially reconstruct missing native direct pixels", &settings.DirectSpatialReconstruction)) ++validityRevision;
	ImGui::TextDisabled("Full-resolution diffuse/specular fill; raw RT signals remain untouched.");
	ImGui::SeparatorText("Independent RT shadows");
	bool shadowChanged = false;
	if (ImGui::Checkbox("Enable independent shadow layer", &settings.ShadowLayerEnabled)) {
		shadowChanged = true;
		if (settings.ShadowLayerEnabled)
			globals::features::dx12NativeScene.RequestShadowProducerForComposite();
	}
	shadowChanged |= ImGui::SliderFloat("Shadow contribution", &settings.ShadowContribution, 0.0f, 4.0f, "%.2fx");
	shadowChanged |= ImGui::SliderFloat("Shadow blend", &settings.ShadowBlend, 0.0f, 1.0f, "%.2f");
	shadowChanged |= ImGui::SliderFloat("Shadow softness (pixels)", &settings.ShadowRadiusPixels, 0.0f, 32.0f, "%.1f");
	shadowChanged |= ImGui::SliderFloat("Shadow receiver tolerance", &settings.ShadowReceiverTolerance, 0.1f, 64.0f, "%.2f");
	shadowChanged |= ImGui::Checkbox("Preview independent shadow mask", &settings.ShadowPreview);
	shadowChanged |= ImGui::Checkbox("Spatially reconstruct missing shadow pixels", &settings.ShadowSpatialReconstruction);
	shadowChanged |= ImGui::Checkbox("Apply shadows to final combined lighting", &settings.ShadowAfterLighting);
	ImGui::TextDisabled("Off: shadow raster first. On: shadow raster + native direct.");
	ImGui::TextDisabled("Boost >1 deepens partial shadows. Native Lighting may be disabled.");
	if (shadowChanged) ++validityRevision;
	if (ImGui::CollapsingHeader("Validity Lab")) {
		ImGui::Checkbox("Persist experimental compositor policies", &settings.PersistLab);
		const char* policies[]{ "Strict", "Effective", "Force finite contributions" };
		int diffuse = static_cast<int>(policy.diffuse), specular = static_cast<int>(policy.specular), visual = static_cast<int>(policy.visual);
		bool changed = ImGui::Combo("Diffuse acceptance", &diffuse, policies, 3);
		changed |= ImGui::Combo("Specular acceptance", &specular, policies, 3);
		const char* views[]{ "Normal", "Original validity", "Effective validity", "Forced acceptance", "Rejection reasons", "Primary hits", "Depth mismatch", "Shadow mask", "Direct diffuse", "Direct specular", "Strict vs effective" };
		changed |= ImGui::Combo("Validity visualization", &visual, views, static_cast<int>(std::size(views)));
		if (changed) { policy.diffuse = static_cast<uint32_t>(diffuse); policy.specular = static_cast<uint32_t>(specular); policy.visual = static_cast<uint32_t>(visual); ++validityRevision; }
		ImGui::TextDisabled("Validity: green evaluated, cyan culled, magenta unsupported, black unavailable.");
		ImGui::TextDisabled("Reasons: red camera, dark red miss, orange depth, magenta material, purple evaluation,");
		ImGui::TextDisabled("blue no lights, yellow visibility, white non-finite, green success.");
		ImGui::TextDisabled("Comparison: cyan effective only, orange strict only, black equal.");
		ImGui::TextDisabled("Mask: magenta absent, yellow invalid, cyan rejected, orange not rejected; red/green visibility.");
		ImGui::TextDisabled("GPU completed: %llu  Accepted: %llu  Modified: %llu  Debug: %llu",
			static_cast<unsigned long long>(pass.CompletedComposites()), static_cast<unsigned long long>(pass.AcceptedPixels()),
			static_cast<unsigned long long>(pass.ModifiedPixels()), static_cast<unsigned long long>(pass.VisualizationPixels()));
		ImGui::TextDisabled("Direct spatially reconstructed pixels (GPU-completed): %llu", static_cast<unsigned long long>(pass.ReconstructedPixels()));
		ImGui::TextDisabled("Displayed-pixel consumption is not measured.");
	}
	enabled = settings.Enabled;
	ImGui::TextDisabled("%s", pass.Status().c_str());
	ImGui::TextDisabled("%s", LightingStatus(lightingStatus.load()));
	if (settings.ShadowLayerEnabled) {
		ImGui::TextWrapped("Independent shadow admission: %s", pass.ShadowStatus().c_str());
		const auto sceneDiagnostics = globals::features::dx12NativeScene.GetDiagnostics();
		const auto producerStatus = sceneDiagnostics.value("shadowStatus", std::string{});
		if (!producerStatus.empty())
			ImGui::TextWrapped("Shadow producer: %s", producerStatus.c_str());
		const auto mask = sceneDiagnostics.value("shadow", json::object());
		if (mask.is_object()) {
			ImGui::TextDisabled("Raw shadow dispatches: %llu | opaque recoveries: %llu",
				static_cast<unsigned long long>(mask.value("submissions", uint64_t{ 0 })),
				static_cast<unsigned long long>(sceneDiagnostics.value("shadowOpaqueRetryRecovered", uint64_t{ 0 })));
		}
	}
	ImGui::TextDisabled("Lighting submissions: %llu | shadow-layer submissions: %llu",
		static_cast<unsigned long long>(lightingSubmissions.load()), static_cast<unsigned long long>(shadowSubmissions.load()));
}

void NativeCompositeFeature::LoadSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	settings = a_json;
	policy = settings.PersistLab && a_json.contains("ValidityLab") ? a_json.at("ValidityLab").get<NativeComposite::Policy>() : NativeComposite::Policy{};
	policy.diffuse = std::min(policy.diffuse, 2u);
	policy.specular = std::min(policy.specular, 2u);
	policy.visual = std::min(policy.visual, 10u);
	++validityRevision;
	settings.LightingStrength = std::isfinite(settings.LightingStrength) ? std::clamp(settings.LightingStrength, 0.0f, 2.0f) : 1.0f;
	const auto safe = [](float v, float fallback, float low, float high) { return std::isfinite(v) ? std::clamp(v, low, high) : fallback; };
	settings.ShadowContribution = safe(settings.ShadowContribution, Settings{}.ShadowContribution, 0.0f, 4.0f);
	settings.ShadowBlend = safe(settings.ShadowBlend, Settings{}.ShadowBlend, 0.0f, 1.0f);
	settings.ShadowRadiusPixels = safe(settings.ShadowRadiusPixels, Settings{}.ShadowRadiusPixels, 0.0f, 32.0f);
	settings.ShadowReceiverTolerance = safe(settings.ShadowReceiverTolerance, Settings{}.ShadowReceiverTolerance, 0.1f, 64.0f);
	enabled = settings.Enabled;
	if (settings.ShadowLayerEnabled)
		globals::features::dx12NativeScene.RequestShadowProducerForComposite();
}

void NativeCompositeFeature::SaveSettings(json& a_json)
{
	std::scoped_lock lock(settingsMutex);
	a_json = settings;
	if (settings.PersistLab) a_json["ValidityLab"] = policy;
}

void NativeCompositeFeature::RestoreDefaultSettings()
{
	std::scoped_lock lock(settingsMutex);
	settings = {};
	policy = {};
	++validityRevision;
	enabled = settings.Enabled;
	if (settings.ShadowLayerEnabled)
		globals::features::dx12NativeScene.RequestShadowProducerForComposite();
}

void NativeCompositeFeature::ConfigureShadowLayer(const json& args)
{
	std::scoped_lock lock(settingsMutex);
	const auto tune = [&](const char* name, float& target, float low, float high) {
		if (!args.contains(name)) return;
		const float value = args.at(name).get<float>();
		if (std::isfinite(value)) target = std::clamp(value, low, high);
	};
	tune("contribution", settings.ShadowContribution, 0.0f, 4.0f);
	// The old DX12 Native Scene command used 'strength'.
	tune("strength", settings.ShadowContribution, 0.0f, 4.0f);
	tune("blend", settings.ShadowBlend, 0.0f, 1.0f);
	tune("radiusPixels", settings.ShadowRadiusPixels, 0.0f, 32.0f);
	tune("receiverTolerance", settings.ShadowReceiverTolerance, 0.1f, 64.0f);
	if (args.contains("enabled")) settings.ShadowLayerEnabled = args.at("enabled").get<bool>();
	else if (args.contains("strength") || args.contains("contribution")) settings.ShadowLayerEnabled = true;
	if (args.contains("preview")) settings.ShadowPreview = args.at("preview").get<bool>();
	if (args.contains("spatialReconstruction")) settings.ShadowSpatialReconstruction = args.at("spatialReconstruction").get<bool>();
	if (args.contains("afterLighting")) settings.ShadowAfterLighting = args.at("afterLighting").get<bool>();
	if (settings.ShadowLayerEnabled)
		globals::features::dx12NativeScene.RequestShadowProducerForComposite();
	++validityRevision;
}

void NativeCompositeFeature::RegisterUxActions()
{
	FEATURE_COMMAND("configure", "Set Native Composite enabled, addLighting, lightingStrength and directSpatialReconstruction.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativeCompositeFeature*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		if (args.contains("enabled")) feature.settings.Enabled = args.at("enabled").get<bool>();
		if (args.contains("addLighting")) feature.settings.AddLighting = args.at("addLighting").get<bool>();
		if (args.contains("directSpatialReconstruction")) { feature.settings.DirectSpatialReconstruction = args.at("directSpatialReconstruction").get<bool>(); ++feature.validityRevision; }
		if (args.contains("lightingStrength")) {
			const float value = args.at("lightingStrength").get<float>();
			feature.settings.LightingStrength = std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 1.0f;
		}
		feature.enabled = feature.settings.Enabled;
	});
	FEATURE_COMMAND("shadowLayer", "Configure independent shadow composition, unrelated to Native Lighting: enabled, contribution (0..4), blend (0..1), radiusPixels, receiverTolerance, preview, afterLighting, spatialReconstruction.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativeCompositeFeature*>(self);
		feature.ConfigureShadowLayer(args);
	});
	FEATURE_COMMAND("validityLab", "Set runtime diffusePolicy and specularPolicy (0 strict, 1 effective, 2 finite), visualMode (0 normal, 1 original, 2 effective, 3 forced, 4 reasons, 5 hits, 6 depth, 7 mask, 8 diffuse, 9 specular, 10 comparison), and persistLab.", [](Feature* self, const json& args) {
		auto& feature = *static_cast<NativeCompositeFeature*>(self);
		std::scoped_lock lock(feature.settingsMutex);
		if (args.contains("diffusePolicy")) feature.policy.diffuse = std::min(args.at("diffusePolicy").get<uint32_t>(), 2u);
		if (args.contains("persistLab")) feature.settings.PersistLab = args.at("persistLab").get<bool>();
		if (args.contains("specularPolicy")) feature.policy.specular = std::min(args.at("specularPolicy").get<uint32_t>(), 2u);
		if (args.contains("visualMode")) feature.policy.visual = std::min(args.at("visualMode").get<uint32_t>(), 10u);
		++feature.validityRevision;
	});
	FEATURE_QUERY("validityLabStatus", "Read compositor acceptance, visualization, persistence choice and policy revision.", ([](const Feature* self, const json&) -> json {
		auto& feature = *const_cast<NativeCompositeFeature*>(static_cast<const NativeCompositeFeature*>(self));
		std::scoped_lock lock(feature.settingsMutex);
		return { { "settings", feature.policy }, { "persistLab", feature.settings.PersistLab }, { "revision", feature.validityRevision } };
	}));
}

json NativeCompositeFeature::GetDiagnostics()
{
	std::scoped_lock lock(settingsMutex);
	const auto state = lightingStatus.load();
	const auto sceneDiagnostics = globals::features::dx12NativeScene.GetDiagnostics();
	return { { "enabled", enabled.load() }, { "captures", captures.load() }, { "lightingSubmissions", lightingSubmissions.load() }, { "shadowSubmissions", shadowSubmissions.load() },
		{ "completedComposites", pass.CompletedComposites() }, { "acceptedPixels", pass.AcceptedPixels() },
		{ "modifiedPixels", pass.ModifiedPixels() }, { "visualizationPixels", pass.VisualizationPixels() },
		{ "effectiveRecoveredPixels", pass.EffectiveRecoveredPixels() }, { "finiteOnlyPixels", pass.FiniteOnlyPixels() },
		{ "counterReadFailures", pass.CounterReadFailures() }, { "directReconstructedPixels", pass.ReconstructedPixels() }, { "visuallyConsumedPixels", nullptr },
		{ "lightingStatus", LightingStatus(state) },
		{ "shadowAdmissionStatus", pass.ShadowStatus() },
		{ "shadowProducerStatus", sceneDiagnostics.value("shadowStatus", std::string{}) },
		{ "shadowProducerStage", sceneDiagnostics.value("shadowStage", std::string{}) },
		{ "shadowProducer", sceneDiagnostics.value("shadow", json::object()) },
		{ "shadowOpaqueRetryAttempts", sceneDiagnostics.value("shadowOpaqueRetryAttempts", uint64_t{ 0 }) },
		{ "shadowOpaqueRetryRecovered", sceneDiagnostics.value("shadowOpaqueRetryRecovered", uint64_t{ 0 }) },
		{ "shadowLayerEnabled", settings.ShadowLayerEnabled },
		{ "shadowContribution", settings.ShadowContribution },
		{ "shadowAfterLighting", settings.ShadowAfterLighting }, { "shadowSpatialReconstruction", settings.ShadowSpatialReconstruction } };
}

std::shared_ptr<const NativeComposite::Frame> NativeCompositeFeature::Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
	ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
	uint32_t a_frame, uint32_t a_width, uint32_t a_height, NativeComposite::Transfer a_transfer)
{
	if (!loaded || !enabled)
		return {};
	Settings selected;
	NativeComposite::Policy selectedPolicy;
	uint64_t revision;
	{ std::scoped_lock lock(settingsMutex); selected = settings; selectedPolicy = policy; revision = validityRevision; }
	std::shared_ptr<const NativeLightingFeature::Contribution> lighting;
	const bool unsupportedGamut = a_transfer == NativeComposite::Transfer::Unknown ||
		(a_transfer == NativeComposite::Transfer::Linear && globals::features::linearLighting.settings.enableACEScg);
	const auto candidate = globals::game::graphicsState ? globals::features::nativeLighting.GetContribution(globals::game::graphicsState->GetFrameCount()) : nullptr;
	if (candidate && candidate->geometryOnly && selectedPolicy.visual == 0) selectedPolicy.visual = 5;
	lightingStatus = (!selected.AddLighting || selected.LightingStrength <= 0) && selectedPolicy.visual == 0 ? 0 : unsupportedGamut && selectedPolicy.visual == 0 ? 2 : 1;
	if ((selected.AddLighting && selected.LightingStrength > 0 && !unsupportedGamut) || selectedPolicy.visual != 0)
		lighting = candidate;
	const NativeComposite::ShadowSettings shadowSettings{
		selected.ShadowLayerEnabled ? selected.ShadowContribution : 0.0f,
		selected.ShadowBlend, selected.ShadowRadiusPixels, selected.ShadowReceiverTolerance,
		selected.ShadowLayerEnabled && selected.ShadowPreview, selected.ShadowAfterLighting, selected.ShadowSpatialReconstruction, revision
	};
	const bool wantShadow = shadowSettings.preview || shadowSettings.contribution * shadowSettings.blend > 0.0f;
	const auto shadow = wantShadow && !unsupportedGamut ? globals::features::dx12NativeScene.GetShadowMask(a_frame) : nullptr;
	const auto raster = shadow ? globals::features::dx12NativeScene.GetRasterFrame(a_frame) : nullptr;
	auto frame = pass.Capture(a_device11, a_context11, a_device12, a_queue, a_source, a_frame, a_width, a_height,
		a_transfer, lighting, shadow, raster, shadowSettings, selected.LightingStrength, selected.DirectSpatialReconstruction, selectedPolicy, revision);
	if (frame) ++captures;
	if (frame && frame->lightingSubmitted) { ++lightingSubmissions; lightingStatus = 3; }
	else if (lighting) lightingStatus = 4;
	if (frame && frame->shadowSubmitted) {
		++shadowSubmissions;
		if (!frame->lightingSubmitted) lightingStatus = 5;
	}
	return frame;
}
