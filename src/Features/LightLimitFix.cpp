#include "LightLimitFix.h"
#include "Features/InverseSquareLighting/Common.h"
#include "Features/LightLimitFix/SettingsSanitize.h"
#include "Features/LightLimitFix/ShadowCasterMath.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "InverseSquareLighting.h"
#include "LinearLighting.h"
#include "Utils/UI.h"

#include "Deferred.h"
#include "Menu/ThemeManager.h"
#include "Shadercache.h"
#include "State.h"
#include "Util.h"
#include "Utils/ExternalEmittance.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace
{
	constexpr float kParticleLightsSaturationMin = 1.0f;
	constexpr float kParticleLightsSaturationMax = 2.0f;
	constexpr float kParticleBrightnessMin = 0.0f;
	constexpr float kParticleBrightnessMax = 10.0f;
	constexpr float kParticleRadiusMin = 0.0f;
	constexpr float kParticleRadiusMax = 10.0f;
	constexpr float kBillboardBrightnessMin = 0.0f;
	constexpr float kBillboardBrightnessMax = 10.0f;
	constexpr float kBillboardRadiusMin = 0.0f;
	constexpr float kBillboardRadiusMax = 10.0f;
	constexpr float kParticleClusterThresholdMin = 8.0f;
	constexpr float kParticleClusterThresholdMax = 128.0f;
	constexpr int kMaxParticlesPerEmitterMin = 32;
	constexpr int kMaxParticlesPerEmitterMax = 2048;
	constexpr float kMaxParticleDistanceMin = 0.0f;
	constexpr float kMaxParticleDistanceMax = 20000.0f;
	constexpr float kJsonPlacedLightIntensityMin = 0.0f;
	constexpr float kJsonPlacedLightIntensityMax = 8.0f;

	float ClampFiniteOrDefault(float a_value, float a_min, float a_max, float a_default)
	{
		if (!std::isfinite(a_value))
			return a_default;
		return std::clamp(a_value, a_min, a_max);
	}

	void SanitizeSettings(LightLimitFix::Settings& a_settings)
	{
		a_settings.ParticleLightsSaturation =
			ClampFiniteOrDefault(a_settings.ParticleLightsSaturation, kParticleLightsSaturationMin, kParticleLightsSaturationMax, 1.0f);
		a_settings.ParticleBrightness =
			ClampFiniteOrDefault(a_settings.ParticleBrightness, kParticleBrightnessMin, kParticleBrightnessMax, 1.0f);
		a_settings.ParticleRadius =
			ClampFiniteOrDefault(a_settings.ParticleRadius, kParticleRadiusMin, kParticleRadiusMax, 1.0f);
		a_settings.BillboardBrightness =
			ClampFiniteOrDefault(a_settings.BillboardBrightness, kBillboardBrightnessMin, kBillboardBrightnessMax, 1.0f);
		a_settings.BillboardRadius =
			ClampFiniteOrDefault(a_settings.BillboardRadius, kBillboardRadiusMin, kBillboardRadiusMax, 1.0f);
		a_settings.ParticleClusterThreshold =
			ClampFiniteOrDefault(a_settings.ParticleClusterThreshold, kParticleClusterThresholdMin, kParticleClusterThresholdMax, 32.0f);
		a_settings.MaxParticlesPerEmitter = std::clamp(a_settings.MaxParticlesPerEmitter, kMaxParticlesPerEmitterMin, kMaxParticlesPerEmitterMax);
		a_settings.MaxParticleDistance =
			ClampFiniteOrDefault(a_settings.MaxParticleDistance, kMaxParticleDistanceMin, kMaxParticleDistanceMax, 6000.0f);
		a_settings.JsonPlacedLightIntensity =
			ClampFiniteOrDefault(a_settings.JsonPlacedLightIntensity, kJsonPlacedLightIntensityMin, kJsonPlacedLightIntensityMax, 1.0f);
	}

	void ClearStrictLightData(LightLimitFix::StrictLightDataCB& a_data, bool a_resetRoomIndex) noexcept
	{
		a_data.NumStrictLights = 0;
		a_data.ShadowBitMask = 0;
		if (a_resetRoomIndex)
			a_data.RoomIndex = -1;
	}
}

// Debug visualisation state (EnableLightsVisualisation / LightsVisualisationMode)
// is intentionally NOT serialized -- it lives as instance members on the
// LightLimitFix class so it resets per session and can't accidentally end up in
// a shipped JSON config that would force every load to compile the heavier
// LLFDEBUG shader permutation.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LightLimitFix::Settings,
	EnableContactShadows,
	ContactShadowMaxSteps,
	ContactShadowMaxDistance,
	ContactShadowStride,
	ContactShadowThickness,
	ContactShadowDepthFade,
	ContactShadowMinIntensity,
	ShowShadowOverlay,
	ShadowSettings,
	EnableParticleContactShadows,
	EnableParticleLights,
	EnableParticleLightsCulling,
	EnableParticleLightsDetection,
	EnableParticleLightsOptimization,
	ParticleLightsSaturation,
	ParticleBrightness,
	ParticleRadius,
	BillboardBrightness,
	BillboardRadius,
	ParticleClusterThreshold,
	MaxParticlesPerEmitter,
	MaxParticleDistance,
	JsonPlacedLightIntensity,
	JsonPlacedLightsInteriorsOnly,
	JsonPlacedLightsPortalStrictOnly)

void LightLimitFix::DrawSettings()
{
	auto shaderCache = globals::shaderCache;

	ShadowCasterManager::DrawSettings(settings.ShadowSettings);

	if (ImGui::TreeNodeEx(T("feature.light_limit_fix.statistics", "Statistics"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::format("Clustered Light Count : {}", lightCount).c_str());
		ImGui::Text(std::format("Particle Lights Count : {}", currentParticleLights.size()).c_str());
		ImGui::TreePop();
	}

	// ---- Active Shadow Casters --------------------------------------
	// One cohesive section: overlay toggle, then ALL the stats grouped
	// together (summary + scheduler stats + budget verdict), then the
	// table below. Same layout as the overlay so testers see the same
	// thing in both views with the stats above the (potentially long)
	// table -- no scrolling required to find the headline numbers.
	ImGui::SeparatorText("Shadow Limit Fix -- Active Casters");

	ImGui::Checkbox("Show Shadow Overlay", &settings.ShowShadowOverlay);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Pop out an always-visible overlay window with the shadow caster table.\n"
			"Without this, the overlay only appears when a light is suppressed\n"
			"or a visualisation mode is active. Enable to access the table's\n"
			"debug controls (cycle button, solo, Shift+hover pulse) any time.");
	}

	ShadowCasterManager::DrawShadowSummary(lightCount, MAX_LIGHTS, shadowUnshadowedLightCount);
	ShadowCasterManager::DrawShadowSchedulerStats();
	ImGui::Separator();
	ShadowCasterManager::DrawShadowLightTable(true, false);

	///////////////////////////////
	ImGui::SeparatorText("Contact Shadows");

	ImGui::Checkbox("Enable Contact Shadows", &settings.EnableContactShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("All point lights (strict and clustered, except simple lights) cast short screen-space shadows. Performance impact.");
	}

	if (settings.EnableContactShadows && ImGui::TreeNode("Contact Shadow Tuning")) {
		// SliderScalar with ImGuiDataType_U32 instead of `SliderInt + (int*)cast`:
		// the cast violates strict aliasing (UB) and would also misinterpret any
		// transient negative value inside ImGui before clamp. SliderScalar
		// reads/writes the uint storage directly with explicit min/max bounds.
		constexpr uint32_t kMinSteps = 1, kMaxSteps = 16;
		ImGui::SliderScalar("Max Steps", ImGuiDataType_U32, &settings.ContactShadowMaxSteps,
			&kMinSteps, &kMaxSteps, "%u", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Raymarch steps at zero depth. Higher = longer / more accurate contact shadows, linearly more cost.\nVR users should consider 2 to halve per-eye cost.");
		}

		// AlwaysClamp on every float slider too: without it, Ctrl+Click text entry can
		// land arbitrary out-of-range values in settings before GetCommonBufferData's
		// boundary clamp catches them at the GPU side.
		ImGui::SliderFloat("Max Distance", &settings.ContactShadowMaxDistance, 64.0f, 4096.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("View-space depth at which contact shadows fade to zero steps. Avoids paying for shadows on distant surfaces where they don't read.");
		}

		ImGui::SliderFloat("Stride", &settings.ContactShadowStride, 0.5f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Per-step march length in view-space units at near depth (auto-scales linearly past ~100 units so far surfaces don't undersample). Larger = longer screen-space reach with coarser detail.");
		}

		ImGui::SliderFloat("Thickness", &settings.ContactShadowThickness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Depth-delta multiplier for shadow onset. Larger = darker contact at occluder edges.");
		}

		ImGui::SliderFloat("Depth Fade", &settings.ContactShadowDepthFade, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Depth-delta multiplier for shadow falloff. Larger = shadows truncate sooner behind thick occluders.");
		}

		ImGui::SliderFloat("Min Light Intensity", &settings.ContactShadowMinIntensity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Skip contact shadows for CLUSTERED lights whose normalized distance falloff "
				"`1 - (lightDist/radius)^2` at the pixel is below this threshold. "
				"Strict lights are always raymarched regardless of this threshold. "
				"Higher = larger perf win, may drop subtle shadows from weak lights at their reach edge.");
		}

		ImGui::TreePop();
	}

	ImGui::BeginDisabled(!settings.EnableContactShadows);
	ImGui::Checkbox("Enable Particle Contact Shadows", &settings.EnableParticleContactShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Also cast contact shadows from particle lights. Larger performance impact in fire/magic-heavy scenes.");
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText("Particle Lights");

	ImGui::TextWrapped(
		"Turns configured particle effects (candles, braziers, torches, magic) into dynamic lights. "
		"Requires a particle-light config pack shipping Data\\ParticleLights\\*.ini (e.g. Embers HD, "
		"Lanterns of Skyrim); with no pack installed this section has no effect.");
	ImGui::TextWrapped(
		"Particle lights are additive emitters and do NOT cast shadow-map shadows, so they never appear "
		"in the shadow caster table above. Turn on \"Enable Particle Contact Shadows\" in the Contact "
		"Shadows section for short screen-space contact shadows.");
	ImGui::Spacing();

	ImGui::Checkbox("Enable Particle Lights", &settings.EnableParticleLights);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Master toggle for the particle-light feature.");
	}

	if (ImGui::TreeNode("Performance##particles")) {
		ImGui::Checkbox("Enable Culling", &settings.EnableParticleLightsCulling);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Significantly improves performance by not rendering empty textures. Only disable if you are encountering issues.");
		}

		ImGui::Checkbox("Enable Detection", &settings.EnableParticleLightsDetection);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Adds particle lights to the player light level so that NPCs detect them for stealth and gameplay.");
		}

		ImGui::Checkbox("Enable Optimization", &settings.EnableParticleLightsOptimization);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Merges vertices which are close enough to each other to improve performance.");
		}

		ImGui::SliderFloat("Cluster Threshold", &settings.ParticleClusterThreshold, kParticleClusterThresholdMin, kParticleClusterThresholdMax, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Distance+radius similarity threshold for merging particles into one light.\n"
				"Higher = more merging, better performance, blurrier lights.\n"
				"Lower = less merging, more precise, more expensive.");
		}

		ImGui::SliderInt("Max Particles per Emitter", &settings.MaxParticlesPerEmitter, kMaxParticlesPerEmitterMin, kMaxParticlesPerEmitterMax);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Maximum number of particles sampled per emitter per frame.\n"
				"Higher = closer to the real particle system but more CPU work.\n"
				"Lower = faster, especially for very dense effects.");
		}

		ImGui::SliderFloat("Max Particle Distance", &settings.MaxParticleDistance, 1000.0f, kMaxParticleDistanceMax, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Particle lights beyond this distance from the camera are skipped entirely.\n"
				"Lower = better performance, but distant effects won't contribute light.\n"
				"Higher = more distant particle lighting, but more cost.");
		}

		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Appearance##particles")) {
		ImGui::SliderFloat("Saturation", &settings.ParticleLightsSaturation, kParticleLightsSaturationMin, kParticleLightsSaturationMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Color saturation of particle/billboard lights. 1.0 = source color; higher = more vivid.");
		}
		ImGui::SliderFloat("Particle Brightness", &settings.ParticleBrightness, kParticleBrightnessMin, kParticleBrightnessMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Intensity multiplier for particle-system emitters (fire, sparks, magic).");
		}
		ImGui::SliderFloat("Particle Radius", &settings.ParticleRadius, kParticleRadiusMin, kParticleRadiusMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Radius multiplier for particle-system emitters. Larger = light reaches further.");
		}
		ImGui::SliderFloat("Billboard Brightness", &settings.BillboardBrightness, kBillboardBrightnessMin, kBillboardBrightnessMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Intensity multiplier for billboard (single-quad) emitters such as candle flames.");
		}
		ImGui::SliderFloat("Billboard Radius", &settings.BillboardRadius, kBillboardRadiusMin, kBillboardRadiusMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Radius multiplier for billboard emitters. Larger = light reaches further.");
		}

		ImGui::TreePop();
	}

	ImGui::SeparatorText("Placed Lights (JSON)");

	ImGui::TextWrapped(
		"Scales the intensity of runtime lights attached from Light records by Light Placer-style mods. "
		"Separate from particle lights; requires Inverse Square Lighting for the runtime metadata.");
	ImGui::Spacing();

	{
		const bool jsonPlacedLightsSupported = globals::features::inverseSquareLighting.loaded;
		ImGui::BeginDisabled(!jsonPlacedLightsSupported);
		ImGui::SliderFloat("Intensity Scale", &settings.JsonPlacedLightIntensity, kJsonPlacedLightIntensityMin, kJsonPlacedLightIntensityMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Scales intensity for attached runtime lights generated from Light records.\n"
				"Primarily targets Light Placer-style JSON lights.\n"
				"Requires Inverse Square Lighting runtime metadata.");
		}

		ImGui::Checkbox("Interiors Only", &settings.JsonPlacedLightsInteriorsOnly);
		ImGui::Checkbox("Portal Strict Only", &settings.JsonPlacedLightsPortalStrictOnly);
		ImGui::EndDisabled();

		if (!jsonPlacedLightsSupported)
			ImGui::TextDisabled("Requires Inverse Square Lighting to identify JSON-placed runtime lights.");
	}

	///////////////////////////////
	ImGui::SeparatorText(T("feature.light_limit_fix.debug", "Debug"));

	if (ImGui::TreeNode(T("feature.light_limit_fix.light_limit_vis", "Light Limit Visualization"))) {
		ImGui::Checkbox(T("feature.light_limit_fix.enable_lights_vis", "Enable Lights Visualisation"), &EnableLightsVisualisation);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.enable_lights_vis_tooltip", "Enables visualization of the light limit\n"));
		}

		{
			static const char* comboOptions[] = {
				"Light Limit",
				"Strict Lights Count",
				"Clustered Lights Count",
				"Shadow Mask",
				"Shadow Light Count",
				"Point Light Shadow Factor",
				"Unshadowed Point Lights",
				"Shadow Caster Density",
				"Shadow Slot Index Color",
				"Light Type Visualization",
			};
			// Round-trip through int instead of `(int*)&uint` to avoid strict-aliasing UB
			// (ImGui has no ComboScalar). Clamp on the way in defends against any stale
			// persisted value that might still exist from older builds.
			int visMode = std::clamp(static_cast<int>(LightsVisualisationMode),
				0, IM_ARRAYSIZE(comboOptions) - 1);
			ImGui::Combo(T("feature.light_limit_fix.lights_vis_mode", "Lights Visualisation Mode"), &visMode, comboOptions, IM_ARRAYSIZE(comboOptions));
			LightsVisualisationMode = static_cast<uint>(visMode);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"%s",
					T("feature.light_limit_fix.lights_vis_mode_tooltip",
						"Light Limit: Red when the strict light limit is reached (>=7 portal-strict lights).\n"
						"\n"
						"Strict Lights Count: Heatmap of portal-strict lights per pixel (blue=0, red=15).\n"
						"\n"
						"Clustered Lights Count: Heatmap of dynamic lights in each screen tile (blue=0, red=128)."));
				ShadowCasterManager::DrawVisualisationTooltipShadowModes();
			}
		}

		currentEnableLightsVisualisation = EnableLightsVisualisation;
		if (previousEnableLightsVisualisation != currentEnableLightsVisualisation) {
			globals::state->SetDefines(EnableLightsVisualisation ? "LLFDEBUG" : "");
			shaderCache->Clear(RE::BSShader::Type::Lighting);
			previousEnableLightsVisualisation = currentEnableLightsVisualisation;
		}

		ImGui::TreePop();
	}
}

LightLimitFix::PerFrame LightLimitFix::GetCommonBufferData()
{
	// Defensive sanitization before the values hit the constant buffer. The
	// sliders enforce ImGuiSliderFlags_AlwaysClamp at the UI, but Settings
	// can be mutated through other paths (JSON persistence, mod overrides,
	// remote-control / MCP server, or just an internal logic bug) -- a few
	// of these fields will produce divisions, infinite loops, or visual
	// corruption if they arrive non-finite or out-of-range, so we re-validate
	// at the shader boundary rather than trusting upstream callers.
	//
	// std::clamp passes NaN through unchanged (every NaN comparison is false),
	// so reject non-finite values explicitly first; fall back to the lower
	// bound on NaN/inf to produce degraded but stable behavior.
	auto sanitizeFloat = [](float v, float lo, float hi) {
		return LightLimitFixSanitize::SanitizeFloat(v, lo, hi);
	};

	PerFrame perFrame{};
	perFrame.EnableContactShadows = settings.EnableContactShadows;
	perFrame.ContactShadowMaxSteps = std::clamp<uint32_t>(settings.ContactShadowMaxSteps, 1u, 16u);
	perFrame.ContactShadowMaxDistance = sanitizeFloat(settings.ContactShadowMaxDistance, 64.0f, 4096.0f);
	perFrame.ContactShadowStride = sanitizeFloat(settings.ContactShadowStride, 0.5f, 8.0f);
	perFrame.ContactShadowThickness = sanitizeFloat(settings.ContactShadowThickness, 0.0f, 1.0f);
	perFrame.ContactShadowDepthFade = sanitizeFloat(settings.ContactShadowDepthFade, 0.0f, 1.0f);
	perFrame.ContactShadowMinIntensity = sanitizeFloat(settings.ContactShadowMinIntensity, 0.0f, 1.0f);
	perFrame.ShadowMapSlots = ShadowCasterManager::GetInstalledSlotCount();
	perFrame.EnableParticleContactShadows = settings.EnableContactShadows && settings.EnableParticleContactShadows;
	std::copy(clusterSize, clusterSize + 3, perFrame.ClusterSize);
	perFrame.EnableLightsVisualisation = EnableLightsVisualisation;
	perFrame.LightsVisualisationMode = LightsVisualisationMode;
	return perFrame;
}

void LightLimitFix::SetupResources()
{
	auto screenSize = globals::state->screenSize;
	if (globals::game::isVR)
		screenSize.x *= .5;
	clusterSize[0] = ((uint)screenSize.x + 63) / 64;
	clusterSize[1] = ((uint)screenSize.y + 63) / 64;
	clusterSize[2] = 32;
	uint clusterCount = clusterSize[0] * clusterSize[1] * clusterSize[2];

	{
		std::vector<std::pair<const char*, const char*>> clusterDefines;
		if (globals::game::isVR)
			clusterDefines = { { "VR", "" } };
		clusterBuildingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", clusterDefines, "cs_5_0");
		clusterCullingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", clusterDefines, "cs_5_0");

		lightBuildingCB = new ConstantBuffer(ConstantBufferDesc<LightBuildingCB>());
		lightCullingCB = new ConstantBuffer(ConstantBufferDesc<LightCullingCB>());
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DEFAULT;
		sbDesc.CPUAccessFlags = 0;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.Flags = 0;

		std::uint32_t numElements = clusterCount;

		sbDesc.StructureByteStride = sizeof(ClusterAABB);
		sbDesc.ByteWidth = sizeof(ClusterAABB) * numElements;
		clusters = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::Clusters");
		srvDesc.Buffer.NumElements = numElements;
		clusters->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		clusters->CreateUAV(uavDesc);

		numElements = 1;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		lightIndexCounter = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightIndexCounter");
		srvDesc.Buffer.NumElements = numElements;
		lightIndexCounter->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightIndexCounter->CreateUAV(uavDesc);

		numElements = clusterCount * CLUSTER_MAX_LIGHTS;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		lightIndexList = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightIndexList");
		srvDesc.Buffer.NumElements = numElements;
		lightIndexList->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightIndexList->CreateUAV(uavDesc);

		numElements = clusterCount;
		sbDesc.StructureByteStride = sizeof(LightGrid);
		sbDesc.ByteWidth = sizeof(LightGrid) * numElements;
		lightGrid = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightGrid");
		srvDesc.Buffer.NumElements = numElements;
		lightGrid->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightGrid->CreateUAV(uavDesc);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(LightData);
		sbDesc.ByteWidth = sizeof(LightData) * MAX_LIGHTS;
		lights = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::Lights");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_LIGHTS;
		lights->CreateSRV(srvDesc);
	}

	{
		strictLightDataCB = new ConstantBuffer(ConstantBufferDesc<StrictLightDataCB>());
	}
}

void LightLimitFix::Reset()
{
	std::lock_guard<std::mutex> queueLock{ particleLightsQueueMutex };

	for (auto& particleLight : currentParticleLights) {
		if (!particleLight.node)
			continue;

		if (!particleLight.billboard) {
			if (const auto particleSystem = static_cast<RE::NiParticleSystem*>(particleLight.node)) {
				if (auto particleData = particleSystem->GetParticlesRuntimeData().particleData.get())
					particleData->DecRefCount();
			}
		}
		particleLight.node->DecRefCount();
	}
	currentParticleLights.clear();
	std::swap(currentParticleLights, queuedParticleLights);
	// References are keyed by transient pass geometry pointers; rebuild every frame to avoid stale entries.
	particleLightsReferences.clear();
	jsonPlacedLightCache.clear();
}

void LightLimitFix::OnSceneTransitionReset(bool opening)
{
	// LoadingMenu open: drop the shadow-caster session caches before the engine tears down the old
	// cell. Dispatched on the render thread (Feature::DrainSceneTransitions), so it serializes with
	// the settings-menu table iteration that reads the same caches instead of racing it.
	if (opening)
		ShadowCasterManager::ResetSession();
}

void LightLimitFix::LoadSettings(json& o_json)
{
	settings = o_json;
	SanitizeSettings(settings);
	// iShadowMapResolution:Display is owned by Skyrim's INI, not our JSON.
	ShadowCasterManager::LoadINISettings();

	// Raise saved values below the current floor so older configs migrate.
	if (settings.ShadowSettings.MaxRedrawPerFrame < ShadowCasterManager::Settings::kMinMaxRedrawPerFrame)
		settings.ShadowSettings.MaxRedrawPerFrame = ShadowCasterManager::Settings::kMinMaxRedrawPerFrame;
}

void LightLimitFix::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
	ShadowCasterManager::SaveINISettings();
}

void LightLimitFix::RestoreDefaultSettings()
{
	settings = {};
	SanitizeSettings(settings);
}

RE::NiNode* GetParentRoomNode(RE::NiAVObject* object)
{
	if (object == nullptr)
		return nullptr;

	static const auto* roomRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundRoom }.get();
	static const auto* portalRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSPortalSharedNode }.get();

	const auto* rtti = object->GetRTTI();
	if (rtti == roomRtti || rtti == portalRtti)
		return static_cast<RE::NiNode*>(object);

	return GetParentRoomNode(object->parent);
}

void LightLimitFix::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* a_pass)
{
	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	ClearStrictLightData(strictLightDataTemp, true);

	if (!a_pass || !a_pass->geometry)
		return;

	if (!roomNodes.empty()) {
		if (RE::NiNode* roomNode = GetParentRoomNode(a_pass->geometry)) {
			if (auto it = roomNodes.find(roomNode); it != roomNodes.cend())
				strictLightDataTemp.RoomIndex = it->second;
		}
	}
}

void LightLimitFix::BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->sceneLights) {
		ClearStrictLightData(strictLightDataTemp, false);
		return;
	}

	auto smState = globals::game::smState;
	if (!smState) {
		ClearStrictLightData(strictLightDataTemp, false);
		return;
	}

	auto& isl = globals::features::inverseSquareLighting;

	auto accumulator = *globals::game::currentAccumulator.get();
	if (!accumulator) {
		ClearStrictLightData(strictLightDataTemp, false);
		return;
	}

	bool inWorld = accumulator->GetRuntimeData().activeShadowSceneNode == smState->shadowSceneNode[0];
	const bool isInterior = Util::IsInterior();

	constexpr uint32_t kStrictLightCapacity = 15;
	const uint32_t availableSceneLights = a_pass->numLights > 0 ? (a_pass->numLights - 1) : 0;
	const uint32_t requestedStrictLights = inWorld ? 0u : availableSceneLights;
	const uint32_t strictLightCount = std::min(requestedStrictLights, kStrictLightCapacity);
	const uint32_t strictShadowLightCount = std::min(static_cast<uint32_t>(a_pass->numShadowLights), availableSceneLights);
	RefreshJsonPlacedLightCacheFrame();

	ClearStrictLightData(strictLightDataTemp, false);

	uint32_t outIndex = 0;
#if defined(_MSC_VER)
	__try
#endif
	{
		for (uint32_t i = 0; i < strictLightCount; i++) {
			auto bsLight = a_pass->sceneLights[i + 1];
			if (!bsLight)
				continue;
			auto niLight = bsLight->light.get();
			if (!niLight)
				continue;

			auto& runtimeData = niLight->GetLightRuntimeData();

			LightData light{};
			light.color = { runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
			light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

			if (isl.loaded) {
				isl.ProcessLight(light, bsLight, niLight);
			} else {
				light.radius = runtimeData.radius.x;
				light.fade = runtimeData.fade;
			}

			light.fade *= bsLight->lodDimmer;
			const bool isPortalStrict = !IsGlobalLight(bsLight);
			ApplyJsonPlacedLightIntensityScale(light, bsLight, niLight, isPortalStrict, isInterior);

			SetLightPosition(light, niLight->world.translate, inWorld);

			if (i < strictShadowLightCount && bsLight->IsShadowLight()) {
				auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
				// Use SCM's stable container-slot index instead of reading the
				// live `shadowmapDescriptors[0].shadowmapIndex`. The descriptor
				// field can be corrupted mid-frame by ReturnShadowmaps() (called
				// via Hook_DisableColorMask) after ScheduleShadowCasters fixed
				// it but before this strict-light setup runs -- a stale-but-in
				// -range index would still pass an upper-bound check yet point
				// strict-light shader sampling at the wrong kSHADOWMAPS slice.
				// GetShadowSlot reads from the SCM's own pool (s_lights, set in
				// ScheduleShadowCasters and never touched by ReturnShadowmaps),
				// so it stays consistent with CopyShadowLightData and
				// UpdateLights, which also key off it. Returns -1 for the sun
				// or inactive lights; both cases skip setting the Shadow flag.
				const int32_t slot = ShadowCasterManager::GetShadowSlot(shadowLight);
				if (slot >= 0 && static_cast<uint32_t>(slot) < ShadowCasterManager::GetInstalledSlotCount()) {
					light.shadowMapIndex = static_cast<uint32_t>(slot);
					light.lightFlags.set(LightFlags::Shadow);
				}
			}

			strictLightDataTemp.StrictLights[outIndex++] = light;
		}
		strictLightDataTemp.NumStrictLights = outIndex;

		// Don't build strictLightDataTemp.ShadowBitMask: no shader reads it (the
		// IsLightIgnored bit-mask branch was replaced by per-light shadowMapIndex
		// sampling, set inline above). The field stays for cbuffer ABI stability
		// and is zero-initialised by ClearStrictLightData.
	}
#if defined(_MSC_VER)
	__except (1) {
		ClearStrictLightData(strictLightDataTemp, false);
	}
#endif
}

void LightLimitFix::BSLightingShader_SetupGeometry_After(RE::BSRenderPass*)
{
	auto shaderCache = globals::shaderCache;
	auto context = globals::d3d::context;
	auto smState = globals::game::smState;

	if (!shaderCache->IsEnabled())
		return;

	if (!smState || !strictLightDataCB)
		return;

	auto accumulator = *globals::game::currentAccumulator.get();
	if (!accumulator)
		return;

	auto shadowSceneNode = smState->shadowSceneNode[0];

	const auto isEmpty = strictLightDataTemp.NumStrictLights == 0;
	const bool isWorld = accumulator->GetRuntimeData().activeShadowSceneNode == shadowSceneNode;
	const auto roomIndex = strictLightDataTemp.RoomIndex;

	if (!isEmpty || (isEmpty && !wasEmpty) || isWorld != wasWorld || previousRoomIndex != roomIndex) {
		strictLightDataCB->Update(strictLightDataTemp);
		wasEmpty = isEmpty;
		wasWorld = isWorld;
		previousRoomIndex = roomIndex;
	}

	if (frameChecker.IsNewFrame()) {
		ID3D11Buffer* buffer = { strictLightDataCB->CB() };
		context->PSSetConstantBuffers(3, 1, &buffer);
	}
}

void LightLimitFix::SetLightPosition(LightLimitFix::LightData& a_light, RE::NiPoint3 a_initialPosition, bool a_cached)
{
	for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
		RE::NiPoint3 eyePosition;

		if (a_cached)
			eyePosition = eyePositionCached[eyeIndex];
		else
			eyePosition = Util::GetEyePosition(eyeIndex);

		auto worldPos = a_initialPosition - eyePosition;
		a_light.positionWS[eyeIndex].data.x = worldPos.x;
		a_light.positionWS[eyeIndex].data.y = worldPos.y;
		a_light.positionWS[eyeIndex].data.z = worldPos.z;
	}
}

void LightLimitFix::RefreshJsonPlacedLightCacheFrame()
{
	if (jsonPlacedLightCacheFrameChecker.IsNewFrame())
		jsonPlacedLightCache.clear();
}

bool LightLimitFix::IsJsonPlacedLight(RE::BSLight* a_bsLight, RE::NiLight* a_niLight)
{
	if (!a_bsLight || !a_niLight || !a_bsLight->pointLight)
		return false;
	if (!globals::features::inverseSquareLighting.loaded)
		return false;
	if (const auto it = jsonPlacedLightCache.find(a_niLight); it != jsonPlacedLightCache.end())
		return it->second;

	bool isJsonPlacedLight = false;
	if (const auto ownerRef = a_niLight->GetUserData()) {
		if (const auto ownerBase = ownerRef->GetObjectReference(); ownerBase && ownerBase->GetFormType() != RE::FormType::Light) {
			const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(a_niLight);
			if (runtimeData && runtimeData->lighFormId != 0) {
				const auto lighForm = RE::TESForm::LookupByID(runtimeData->lighFormId);
				isJsonPlacedLight = lighForm && lighForm->GetFormType() == RE::FormType::Light;
			}
		}
	}

	jsonPlacedLightCache.insert_or_assign(a_niLight, isJsonPlacedLight);
	return isJsonPlacedLight;
}

void LightLimitFix::ApplyJsonPlacedLightIntensityScale(
	LightData& a_light,
	RE::BSLight* a_bsLight,
	RE::NiLight* a_niLight,
	bool a_isPortalStrict,
	bool a_isInterior)
{
	if (std::abs(settings.JsonPlacedLightIntensity - 1.0f) <= 1e-4f)
		return;
	if (settings.JsonPlacedLightsInteriorsOnly && !a_isInterior)
		return;
	if (settings.JsonPlacedLightsPortalStrictOnly && !a_isPortalStrict)
		return;
	if (!IsJsonPlacedLight(a_bsLight, a_niLight))
		return;

	a_light.fade *= settings.JsonPlacedLightIntensity;
}

void LightLimitFix::Prepass()
{
	auto context = globals::d3d::context;

	auto state = globals::state;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "LightLimitFix Prepass");
	state->BeginPerfEvent("LightLimitFix Prepass");
	ShadowCasterManager::Update(settings.ShadowSettings, globals::game::smState->shadowSceneNode[0], nullptr);
	UpdateLights();

	ID3D11ShaderResourceView* views[3]{};
	views[0] = lights->srv.get();
	views[1] = lightIndexList->srv.get();
	views[2] = lightGrid->srv.get();
	context->PSSetShaderResources(35, ARRAYSIZE(views), views);

	state->EndPerfEvent();
}

bool LightLimitFix::IsValidLight(RE::BSLight* a_light)
{
	return a_light && a_light->light && a_light->light.get() && !a_light->light->GetFlags().any(RE::NiAVObject::Flag::kHidden);
}

bool LightLimitFix::IsGlobalLight(RE::BSLight* a_light)
{
	return a_light && !(a_light->portalStrict || !a_light->portalGraph);
}

void LightLimitFix::PostPostLoad()
{
	particleLights.GetConfigs();
	Hooks::Install();
	ShadowCasterManager::Init(settings.ShadowSettings);
	ShadowCasterManager::Install(settings.ShadowSettings);
}

void LightLimitFix::DataLoaded()
{
	if (auto gameSettings = globals::game::gameSettingCollection) {
		if (auto iMagicLightMaxCount = gameSettings->GetSetting("iMagicLightMaxCount")) {
			iMagicLightMaxCount->data.i = MAXINT32;
			logger::info("[LLF] Unlocked magic light limit");
		}
	}
}

void LightLimitFix::ClearShaderCache()
{
	if (clusterBuildingCS) {
		clusterBuildingCS->Release();
		clusterBuildingCS = nullptr;
	}
	if (clusterCullingCS) {
		clusterCullingCS->Release();
		clusterCullingCS = nullptr;
	}
	std::vector<std::pair<const char*, const char*>> clusterDefines;
	if (globals::game::isVR)
		clusterDefines = { { "VR", "" } };
	clusterBuildingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", clusterDefines, "cs_5_0");
	clusterCullingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", clusterDefines, "cs_5_0");
}

void LightLimitFix::UpdateLights()
{
	ZoneScopedN("LLF::UpdateLights");

	auto context = globals::d3d::context;
	if (!context || !lights || !lights->resource) {
		// Drop last frame's particle lights so AddParticleLightLuminance (gameplay
		// thread) can't keep feeding stale lights into NPC detection on this early-out.
		std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
		cachedParticleLights.clear();
		return;
	}

	auto smState = globals::game::smState;
	auto& isl = globals::features::inverseSquareLighting;
	auto clearAndUpdate = [&]() {
		lightCount = 0;
		// Drop last frame's particle lights too: AddParticleLightLuminance reads
		// cachedParticleLights on the gameplay thread, so a bare early-return here
		// would leave stale lights contributing to NPC light-level detection.
		{
			std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
			cachedParticleLights.clear();
		}
		UpdateStructure();
	};

	if (!smState) {
		clearAndUpdate();
		return;
	}

	auto shadowSceneNode = smState->shadowSceneNode[0];
	if (!shadowSceneNode) {
		clearAndUpdate();
		return;
	}

	// Cache data since cameraData can become invalid in first-person
	for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
		auto eyePosition = globals::game::frameBufferCached.GetCameraPosAdjust(eyeIndex);
		eyePositionCached[eyeIndex] = { eyePosition.x, eyePosition.y, eyePosition.z };
	}

	eastl::vector<LightData> lightsData{};
	lightsData.reserve(MAX_LIGHTS);
	const bool isInterior = Util::IsInterior();
	RefreshJsonPlacedLightCacheFrame();

	// Process point lights

	roomNodes.clear();

	auto addRoom = [&](RE::NiNode* node, LightData& light) {
		if (!node)
			return;

		constexpr std::size_t kMaxRoomFlags = 128;
		uint8_t roomIndex = 0;
		if (auto it = roomNodes.find(node); it == roomNodes.cend()) {
			if (roomNodes.size() >= kMaxRoomFlags)
				return;
			roomIndex = static_cast<uint8_t>(roomNodes.size());
			roomNodes.insert_or_assign(node, roomIndex);
		} else {
			roomIndex = it->second;
		}
		light.roomFlags.SetBit(roomIndex, 1);
	};

	// Hover-pulse helper: if the table has a hovered row matching this light's
	// pointer, replace the cluster colour with a magenta pulse so the user can
	// see which light a row corresponds to in 3D. Pulse cycles ~once per second
	// using ImGui::GetTime() for a stable visual signal.
	auto applyDebugOverrides = [](LightData& light, const void* lightPtr) {
		auto hoverKey = ShadowCasterManager::GetHoveredLight();
		if (hoverKey != 0 && reinterpret_cast<uintptr_t>(lightPtr) == hoverKey) {
			float t = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.2831853f);
			light.color = { 1.0f, 0.0f, 1.0f };  // magenta
			light.fade = 4.0f + t * 4.0f;        // pulsed intensity
		}
	};

	auto addLight = [&](const RE::NiPointer<RE::BSLight>& e) {
		if (auto bsLight = e.get()) {
			if (auto niLight = bsLight->light.get()) {
				// IsSuppressed includes solo (every key except the soloed one is
				// implicitly suppressed). This filters every non-shadow cluster
				// light through the user's debug overrides.
				if (ShadowCasterManager::IsSuppressed(reinterpret_cast<uintptr_t>(bsLight)))
					return;
				if (IsValidLight(bsLight)) {
					auto& runtimeData = niLight->GetLightRuntimeData();

					LightData light{};
					light.color = { runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
					light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

					if (isl.loaded) {
						isl.ProcessLight(light, bsLight, niLight);
					} else {
						light.radius = runtimeData.radius.x;
						light.fade = runtimeData.fade;
					}

					light.fade *= bsLight->lodDimmer;
					const bool isPortalStrict = !IsGlobalLight(bsLight);

					if (isPortalStrict) {
						for (const auto& roomPtr : bsLight->rooms) {
							if (roomPtr)
								addRoom(static_cast<RE::NiNode*>(roomPtr), light);
						}
						for (const auto& portalPtr : bsLight->portals) {
							if (portalPtr && portalPtr->portalSharedNode)
								addRoom(static_cast<RE::NiNode*>(portalPtr->portalSharedNode.get()), light);
						}
						light.lightFlags.set(LightFlags::PortalStrict);
					}
					ApplyJsonPlacedLightIntensityScale(light, bsLight, niLight, isPortalStrict, isInterior);

					SetLightPosition(light, niLight->world.translate);

					applyDebugOverrides(light, bsLight);

					if ((light.color.x + light.color.y + light.color.z) * light.fade > 1e-4 && light.radius > 1e-4) {
						lightsData.push_back(light);
					}
				}
			}
		}
	};

	auto addShadowLight = [&](RE::BSShadowLight* shadowLight, bool castsShadow, uint32_t shadowSlot = 0) {
		if (IsValidLight(shadowLight)) {
			if (auto niLight = shadowLight->light.get()) {
				auto& runtimeData = niLight->GetLightRuntimeData();

				LightData light{};
				light.color = { runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
				light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

				if (isl.loaded) {
					isl.ProcessLight(light, shadowLight, niLight);
				} else {
					light.radius = runtimeData.radius.x;
					// light.color *= runtimeData.fade;
					light.fade = runtimeData.fade;
				}

				light.fade *= shadowLight->lodDimmer;

				if (!IsGlobalLight(shadowLight)) {
					// List of BSMultiBoundRooms affected by a light
					for (const auto& roomPtr : shadowLight->rooms) {
						addRoom(roomPtr, light);
					}
					// List of BSPortals affected by a light
					for (const auto& portalPtr : shadowLight->portals) {
						addRoom(portalPtr->portalSharedNode.get(), light);
					}
					light.lightFlags.set(LightFlags::PortalStrict);
				}

				if (castsShadow) {
					// Use the caller-provided stable slot index from s_lights
					// rather than shadowmapDescriptors[0].shadowmapIndex, which
					// can drift relative to our scheduler-assigned slot when
					// ReturnShadowmaps fires between scheduling and lighting.
					light.shadowMapIndex = shadowSlot;
					light.lightFlags.set(LightFlags::Shadow);
				}

				SetLightPosition(light, niLight->world.translate);

				applyDebugOverrides(light, shadowLight);

				if ((light.color.x + light.color.y + light.color.z) * light.fade > 1e-4 && light.radius > 1e-4) {
					lightsData.push_back(light);
				}
			}
		}
	};

	// Single pass over shadowLightsAccum:
	//   - Builds shadowLightPtrs so activeLights below skips lights already added here.
	//   - Calls addShadowLight for each logical light.
	// EnableLight calls both GameEnableLight (→ activeLights) and
	// GameSetShadowCasterSlot (→ shadowLightsAccum) for redrawn lights, so without
	// the skip below each redrawn shadow light would be added twice.
	//
	// Static reuses the bucket array across frames -- a local set would
	// destroy + recreate its buckets every frame, defeating the reserve().
	// Dense layout avoids the per-insert node allocation a std::unordered_set
	// would incur. Upper bound is the configured kSHADOWMAPS slot count;
	// shadowLightsAccum is sized to hold at most that many distinct point/spot
	// lights (sun occupies one logical entry but no kSHADOWMAPS slice, hence
	// the belt-and-braces +1).
	static ankerl::unordered_dense::set<RE::BSLight*> shadowLightPtrs;
	shadowLightPtrs.clear();
	shadowLightPtrs.reserve(ShadowCasterManager::GetInstalledSlotCount() + 1);
	ShadowCasterManager::ForEachShadowLight(shadowSceneNode->GetRuntimeData().shadowLightsAccum,
		[&](RE::BSShadowLight* light) {
			shadowLightPtrs.insert(light);
			// GetShadowSlot returns the kSHADOWMAPS texture slot:
			//   -1 : sun (no kSHADOWMAPS slice — sun shadows live in kSHADOWMAPS_ESRAM
			//        and are sampled via the directional cascade path, not the cluster
			//        loop). Skip cluster injection entirely. The sun stays in
			//        shadowLightPtrs so the activeLights loop below doesn't re-add it.
			//   >=0: kSHADOWMAPS slice index (0..ShadowMapSlots-1) post-reclaim.
			int32_t stableSlot = ShadowCasterManager::GetShadowSlot(light);
			if (stableSlot < 0)
				return;
			bool castsShadow = static_cast<uint32_t>(stableSlot) < ShadowCasterManager::GetInstalledSlotCount();
			addShadowLight(light, castsShadow, castsShadow ? static_cast<uint32_t>(stableSlot) : 0u);
		});

	for (auto& e : shadowSceneNode->GetRuntimeData().activeLights) {
		if (auto bsLight = e.get(); bsLight && shadowLightPtrs.count(bsLight))
			continue;  // shadow light: already added above with correct Shadow flag
		addLight(e);
	}

	// Converted shadow lights (shadow lights demoted to normal-light overflow handling
	// via SCM's ConvertExcessToNormal) live in the engine's activeShadowLights list
	// (offset 0x148) — verified via Ghidra against ShadowSceneNode AE 1.6.1170. They
	// are NOT migrated to activeLights (0x130) when our Hook_IsShadowLight reports
	// false, because the engine's AddLight just searches the existing wrappers and
	// activates the matching one in-place rather than moving entries between lists.
	//
	// Iterate SCM's s_normalConvert directly rather than scanning activeShadowLights:
	// only lights actually in s_normalConvert are intended to render as non-shadow.
	// activeShadowLights also contains BSShadowLights that are merely active shadow
	// casters this frame (already handled via shadowLightsAccum above), and could in
	// principle contain disabled-but-not-yet-removed entries. Iterating the convert
	// list is both tighter (no false positives) and cheaper.
	//
	// Without this, ConvertExcessToNormal lights have no entry in the cluster
	// lightsData[] and never render — the user-visible "converted lights are
	// invisible" symptom.
	ShadowCasterManager::ForEachConvertedLight([&](RE::BSShadowLight* light) {
		auto* asBs = static_cast<RE::BSLight*>(light);
		if (shadowLightPtrs.count(asBs))
			return;  // simultaneously a shadow caster this frame; already added
		// Honour the user's suppression toggle in the shadow caster table:
		// converted lights share the same lightKey suppression set as shadow
		// lights, so suppressing one in the table hides it whether it's
		// rendering as a shadow caster or demoted to non-shadow.
		if (ShadowCasterManager::IsSuppressed(reinterpret_cast<uintptr_t>(light)))
			return;
		// Engine zeroes lodDimmer when its shadow-distance LOD cull fires
		// (BSShadowParabolicLight_UpdateCamera test 2, gated on the lodFade
		// flag -- not a visibility test, see ShadowCasterManager.cpp's
		// Ghidra-verified comment). Without restoration, addLight()'s
		// `light.fade *= lodDimmer` would zero the contribution and the
		// (color*fade > 1e-4) filter would drop the light entirely.
		//
		// Restore only when fully zeroed. Any smooth fade value the engine
		// set (between 0 and 1) is preserved -- those represent the engine's
		// own gradual distance attenuation, which is correct to honour for
		// cluster lighting. Overriding unconditionally was producing
		// distant always-full-bright converted lights that ignored the
		// engine's intended fade-with-distance.
		if (light->lodDimmer == 0.0f)
			light->lodDimmer = 1.0f;
		addLight(RE::NiPointer<RE::BSLight>(asBs));
	});

	ProcessQueuedParticleLights(lightsData);

	lightCount = std::min((uint)lightsData.size(), MAX_LIGHTS);

	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(lights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(LightData) * lightCount;
	if (bytes > 0)
		memcpy_s(mapped.pData, bytes, lightsData.data(), bytes);
	context->Unmap(lights->resource.get(), 0);

	UpdateStructure();

	// Single-shot consumption: clear the hover key after the cluster has read it.
	// The table re-sets it every frame the cursor is hovering a row with Shift
	// held, so the pulse continues smoothly while hovering. As soon as the menu
	// closes (or the cursor leaves the table, or Shift is released), the table
	// stops re-setting the key and the pulse vanishes on the next frame.
	ShadowCasterManager::SetHoveredLight(0);
}

void LightLimitFix::UpdateStructure()
{
	auto context = globals::d3d::context;

	lightsNear = *globals::game::cameraNear;
	lightsFar = *globals::game::cameraFar;

	auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	if (globals::game::isVR)
		renderSize.x *= .5;
	clusterSize[0] = ((uint)renderSize.x + 63) / 64;
	clusterSize[1] = ((uint)renderSize.y + 63) / 64;
	clusterSize[2] = 32;

	{
		TracyD3D11Zone(globals::state->tracyCtx, "LightLimitFix Cluster Build");
		LightBuildingCB updateData{};
		updateData.LightsNear = lightsNear;
		updateData.LightsFar = lightsFar;
		std::copy(clusterSize, clusterSize + 3, updateData.ClusterSize);

		lightBuildingCB->Update(updateData);

		ID3D11Buffer* buffer = lightBuildingCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11UnorderedAccessView* clusters_uav = clusters->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &clusters_uav, nullptr);

		context->CSSetShader(clusterBuildingCS, nullptr, 0);
		globals::profiler->BeginPass("LightLimitFix::ClusterBuild");
		context->Dispatch(clusterSize[0], clusterSize[1], clusterSize[2]);
		globals::profiler->EndPass();

		ID3D11UnorderedAccessView* null_uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
	}

	{
		TracyD3D11Zone(globals::state->tracyCtx, "LightLimitFix Cluster Cull");
		LightCullingCB updateData{};
		updateData.LightCount = lightCount;
		std::copy(clusterSize, clusterSize + 3, updateData.ClusterSize);

		lightCullingCB->Update(updateData);

		UINT counterReset[4] = { 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(lightIndexCounter->uav.get(), counterReset);

		ID3D11Buffer* buffer = lightCullingCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11ShaderResourceView* srvs[] = { clusters->srv.get(), lights->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { lightIndexCounter->uav.get(), lightIndexList->uav.get(), lightGrid->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(clusterCullingCS, nullptr, 0);
		globals::profiler->BeginPass("LightLimitFix::ClusterCull");
		context->Dispatch((clusterSize[0] + 15) / 16, (clusterSize[1] + 15) / 16, (clusterSize[2] + 3) / 4);
		globals::profiler->EndPass();
	}

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* null_buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &null_buffer);

	ID3D11ShaderResourceView* null_srvs[2] = { nullptr };
	context->CSSetShaderResources(0, 2, null_srvs);

	ID3D11UnorderedAccessView* null_uavs[3] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 3, null_uavs, nullptr);
}

namespace
{
	// SEH-guarded read of sceneLights[0]->light. The pointer-value plausibility
	// check can't distinguish a live BSLight from a stale-but-canonical one, so
	// reading dirLight->light can itself AV (#92). Treat any fault as "no NiLight"
	// so the caller skips the engine's null-deref path instead of crashing in the
	// guard. Kept in its own function (no C++ unwinding objects) per MSVC's __try
	// restriction. Matches the __except(1) AV-guard pattern used elsewhere here.
	RE::NiLight* SafeReadDirectionalNiLight(RE::BSLight* dirLight)
	{
#if defined(_MSC_VER)
		__try {
			return dirLight->light.get();
		} __except (1) {
			return nullptr;
		}
#else
		return dirLight->light.get();
#endif
	}
}

void LightLimitFix::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	// Engine derefs sceneLights[0]->light with no null check -> CTD on a null/
	// stale directional slot (#92). Skip the engine call when it isn't safe;
	// clamping numLights (sibling guard) can't help -- slot 0 is always read.
	bool directionalSlotSafe = true;
	if (Pass) {
		using ShadowCasterManager::IsPlausibleShadowLightPtr;
		RE::BSLight* dirLight = (Pass->numLights > 0 && Pass->sceneLights) ? Pass->sceneLights[0] : nullptr;
		// A stale-but-canonical dirLight passes the pointer-value check yet still AVs on
		// dirLight->light, so capture the NiLight under SEH and reuse it below (no second deref).
		RE::NiLight* niLight = IsPlausibleShadowLightPtr(reinterpret_cast<std::uintptr_t>(dirLight)) ? SafeReadDirectionalNiLight(dirLight) : nullptr;
		if (Pass->numLights == 0 || !IsPlausibleShadowLightPtr(reinterpret_cast<std::uintptr_t>(niLight))) {
			directionalSlotSafe = false;
			static int logged = 0;
			if (logged++ < 10) {
				logger::warn(
					"[LLF] BSLightingShader_SetupGeometry: directional sceneLights[0] unsafe "
					"(numLights={} BSLight=0x{:x} NiLight=0x{:x}); skipping engine SetupGeometry "
					"to avoid GeometrySetupConstantDirectionalLight null-deref (#92)",
					Pass->numLights,
					reinterpret_cast<std::uintptr_t>(dirLight),
					reinterpret_cast<std::uintptr_t>(niLight));
			}
		}
	}

	// Run before/after even on skip so the strict-light CB is reset, not stale.
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	if (directionalSlotSafe)
		func(This, Pass, RenderFlags);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
}

void LightLimitFix::Hooks::BSEffectShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	// Defensive pre-call guard: BSEffectShader::SetupGeometry iterates
	// Pass->sceneLights[i] and dereferences bsLight->light->fade
	// (BSLight+0x48 -> NiLight+0x134) with NO null check. Stale entries are
	// possible because Pass->sceneLights[] is a raw BSLight** (not
	// NiPointer<>): the engine's pass cache can outlive individual lights
	// or capture them after their NiLight has been cleared. Crashes seen in
	// the wild include garbage data (BSLight memory recycled as a string
	// buffer) and outright NULL NiLight (engine half-destroyed the BSLight
	// but it's still ref-counted alive in some list).
	//
	// Walk the array and clamp numLights to the count of entries that the
	// engine can safely dereference. Validation:
	//   - BSLight* is canonical, 8-byte aligned, non-null
	//   - bsLight->light pointer is canonical, 8-byte aligned, non-null
	// Entries failing either check stop the loop; the engine's own loop
	// bails on the first bad entry too, so clamping matches its contract.
	if (Pass && Pass->sceneLights && Pass->numLights > 0) {
		using ShadowCasterManager::IsPlausibleShadowLightPtr;
		std::uint8_t validCount = 0;
		for (std::uint8_t i = 0; i < Pass->numLights; ++i) {
			RE::BSLight* bsLight = Pass->sceneLights[i];
			if (!IsPlausibleShadowLightPtr(reinterpret_cast<std::uintptr_t>(bsLight))) {
				static int loggedBsLight = 0;
				if (loggedBsLight++ < 10) {
					logger::warn(
						"[LLF] BSEffectShader_SetupGeometry: bad BSLight* at "
						"sceneLights[{}]=0x{:x} numLights={}; clamping to {}",
						i, reinterpret_cast<std::uintptr_t>(bsLight), Pass->numLights, validCount);
				}
				break;
			}
			RE::NiLight* niLight = bsLight->light.get();
			if (!IsPlausibleShadowLightPtr(reinterpret_cast<std::uintptr_t>(niLight))) {
				// Catches both NULL (engine cleared the NiPointer) and
				// garbage (BSLight memory recycled). NULL is the more common
				// observed failure -- the engine's loop has no null check
				// before reading [+0x134].
				static int loggedNiLight = 0;
				if (loggedNiLight++ < 10) {
					logger::warn(
						"[LLF] BSEffectShader_SetupGeometry: bad NiLight at "
						"sceneLights[{}] (BSLight=0x{:x} NiLight=0x{:x}); clamping to {}",
						i,
						reinterpret_cast<std::uintptr_t>(bsLight),
						reinterpret_cast<std::uintptr_t>(niLight),
						validCount);
				}
				break;
			}
			++validCount;
		}
		if (validCount < Pass->numLights)
			Pass->numLights = validCount;
	}

	func(This, Pass, RenderFlags);
	ExternalEmittance::UpdatePermutation(Pass);
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
}

void LightLimitFix::Hooks::BSWaterShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
}
