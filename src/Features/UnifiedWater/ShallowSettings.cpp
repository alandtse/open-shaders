#include "Features/UnifiedWater.h"

#include "I18n/I18n.h"
#include "Util.h"
#include "Utils/DebugSection.h"

#define I18N_KEY_PREFIX "feature.unified_water."

using namespace UnifiedWaterSurface;

namespace UnifiedWaterSurface
{
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(UnifiedWater::ShallowBufferData, ShallowFallbackStrength, DeepConnectionProbeReachUnits, DeepContextDepthUnits, ShoreContactMinFadePixels, ShoreDepthBlendRangeUnits, ShallowSurfaceDepthRangeUnits, ShallowFallbackMaxDistance, DeepContextTransitionUnits)
}

UnifiedWater::ShallowSettings UnifiedWater::GetSelectedShallowSettings() const
{
	std::scoped_lock lock(shallowSettingsMutex);
	return publishedShallowSettings;
}

void UnifiedWater::SetShallowSettings(ShallowSettings value)
{
	value = UnifiedWaterSurface::Sanitize(value);
	std::scoped_lock selectedLock(selectedSettingsMutex);
	shallowSettings = value;
	debugSettings.UseOpenShadersDepthBehaviour = value.UseOpenShadersDepthBehaviour;
	std::scoped_lock lock(shallowSettingsMutex);
	publishedShallowSettings = value;
}

bool UnifiedWater::IsShallowSurfaceReady() const
{
	if (!loaded || !IsWaterDataReady() || !flowmap || !gFlowMapSourceTex || !gFlowMapSize)
		return false;
	if (flowmap->GetWidth() <= 0 || flowmap->GetHeight() <= 0 || *gFlowMapSize != flowmap->GetWidth() ||
		!std::isfinite(flowmap->GetInverseWidth()) || flowmap->GetInverseWidth() <= 0.0f ||
		!std::isfinite(flowmap->GetInverseHeight()) || flowmap->GetInverseHeight() <= 0.0f)
		return false;
	RE::NiPointer<RE::NiSourceTexture> texture;
	return flowmap->TryGetFlowmap(texture) && gFlowMapSourceTex->get() == texture.get();
}

UnifiedWaterSurface::CommonBufferData UnifiedWater::GetShallowSettings() const
{
	const auto selected = GetSelectedShallowSettings();
	return UnifiedWaterSurface::ForRuntime(selected, selected.ShallowFallbackStrength > 0.0f && IsShallowSurfaceReady());
}

void UnifiedWater::LoadSettings(json& value)
{
	ShallowSettings selected;
	if (value.is_object()) {
		static_cast<ShallowBufferData&>(selected) = value.get<ShallowBufferData>();
		selected.UseOpenShadersDepthBehaviour = value.value("UseOpenShadersDepthBehaviour", false);
	}
	SetShallowSettings(selected);
}

void UnifiedWater::SaveSettings(json& value)
{
	std::scoped_lock lock(selectedSettingsMutex);
	value = shallowSettings;
	value["UseOpenShadersDepthBehaviour"] = debugSettings.UseOpenShadersDepthBehaviour;
}

void UnifiedWater::RestoreDefaultSettings()
{
	SetShallowSettings({});
}

json UnifiedWater::GetDiagnostics()
{
	const auto selected = GetSelectedShallowSettings();
	const bool ready = IsShallowSurfaceReady();
	return {
		{ "shallowSurfaceReady", ready },
		{ "legacyDepthBehaviour", selected.UseOpenShadersDepthBehaviour },
		{ "selectedShallowStrength", selected.ShallowFallbackStrength },
		{ "effectiveShallowStrength", UnifiedWaterSurface::ForRuntime(selected, ready).ShallowFallbackStrength }
	};
}

void UnifiedWater::DrawSettings()
{
	std::scoped_lock lock(selectedSettingsMutex);
	if (debugSettings.UseOpenShadersDepthBehaviour)
		ImGui::TextWrapped("%s", T(TKEY("legacy_depth_notice"), "Shallow-water tuning is inactive while Use Legacy {brand} Depth Behaviour is enabled in Debug. Your values are preserved."));
	ImGui::BeginDisabled(debugSettings.UseOpenShadersDepthBehaviour);
	ImGui::SliderFloat(T(TKEY("shallow_shoredepthblendrangeunits"), "Edge Fade Depth"), &shallowSettings.ShoreDepthBlendRangeUnits,
		kMinShoreDepthBlendRangeUnits, kMaxShoreDepthBlendRangeUnits, "%.1f units", ImGuiSliderFlags_AlwaysClamp);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shallow_edge_fade_tooltip"), "Depth over which the shallow-water surface becomes visible near the shore. Higher values soften the transition; zero removes this depth-based fade."));
	ImGui::SliderFloat(T(TKEY("shallow_shallowfallbackmaxdistance"), "Shallow Fallback Max Distance"), &shallowSettings.ShallowFallbackMaxDistance,
		kMinShallowFallbackMaxDistance, kMaxShallowFallbackMaxDistance, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shallow_distance_tooltip"), "How far away the shallow-water visibility adjustment applies. Lower values save work; zero disables it everywhere."));
	ImGui::SliderFloat(T(TKEY("shallow_strength"), "Shallow Fallback Strength"), &shallowSettings.ShallowFallbackStrength,
		kMinShallowFallbackStrength, kMaxShallowFallbackStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shallow_strength_tooltip"), "Makes very shallow, clear water easier to see against the riverbed. Higher values strengthen the surface; zero removes this adjustment."));
	ImGui::EndDisabled();
	SetShallowSettings({ shallowSettings, debugSettings.UseOpenShadersDepthBehaviour });
}

void UnifiedWater::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	{
		std::scoped_lock lock(selectedSettingsMutex);
		ImGui::Checkbox(T(TKEY("use_open_shaders_depth_behaviour"), "Use Legacy {brand} Depth Behaviour"), &debugSettings.UseOpenShadersDepthBehaviour);
		if (auto tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("legacy_depth_tooltip"), "Uses the existing {brand} water blending without the shallow-water visibility adjustment. Your shore and depth settings are preserved."));
		ImGui::BeginDisabled(debugSettings.UseOpenShadersDepthBehaviour);
		ImGui::SliderFloat(T(TKEY("shallow_deepconnectionprobereachunits"), "Connection Search Reach"), &shallowSettings.DeepConnectionProbeReachUnits,
			kMinDeepConnectionProbeReachUnits, kMaxDeepConnectionProbeReachUnits, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shallow_connection_tooltip"), "Search distance toward deeper terrain at half and full reach. Increase for broad shallow banks; zero disables connected-depth protection."));
		ImGui::SliderFloat(T(TKEY("shallow_deepcontextdepthunits"), "Deep Context Depth"), &shallowSettings.DeepContextDepthUnits,
			kMinDeepContextDepthUnits, kMaxDeepContextDepthUnits, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shallow_deep_depth_tooltip"), "Depth normal to the water plane where connected medium or deep channels get full protection from restoration."));
		ImGui::SliderFloat(T(TKEY("shallow_deepcontexttransitionunits"), "Deep Context Transition"), &shallowSettings.DeepContextTransitionUnits,
			kMinDeepContextTransitionUnits, kMaxDeepContextTransitionUnits, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shallow_deep_transition_tooltip"), "Depth fade below Deep Context Depth for connected-water protection. Higher values soften the transition."));
		ImGui::SliderFloat(T(TKEY("shallow_shallowsurfacedepthrangeunits"), "Shallow Surface Depth"), &shallowSettings.ShallowSurfaceDepthRangeUnits,
			kMinShallowSurfaceDepthRangeUnits, kMaxShallowSurfaceDepthRangeUnits, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
		if (auto tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shallow_surface_depth_tooltip"), "Depth normal to the water plane where the shallow cue disappears. Lower it if the cue reaches medium-depth water; raise it if shallow streams still lose their surface."));
		ImGui::SliderFloat(T(TKEY("shallow_shorecontactminfadepixels"), "Minimum Edge Fade Width"), &shallowSettings.ShoreContactMinFadePixels,
			kMinShoreContactMinFadePixels, kMaxShoreContactMinFadePixels, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
		if (auto tooltip = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shallow_edge_width_tooltip"), "Minimum screen-space width of the terrain-contact fade. Prevents subpixel seams without extra samples. Zero uses only Edge Fade Depth."));
		ImGui::EndDisabled();
		SetShallowSettings({ shallowSettings, debugSettings.UseOpenShadersDepthBehaviour });
	}

	if (Util::Button(T(TKEY("regenerate_flowmap"), "Regenerate Flowmap")) && flowmap) {
		if (flowmap->RegenerateAndLoadFlowmap())
			SetFlowmapTex();
	}

	if (Util::Button(T(TKEY("regenerate_caches"), "Regenerate Caches")) && waterCache)
		waterCache->RegenerateCaches();
}
