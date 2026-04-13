#include "ScreenSpaceGI.h"

#include <DirectXTex.h>
#include <cmath>

#include "Deferred.h"
#include "FoveatedCommon.h"
#include "State.h"
#include "Upscaling.h"
#include "Util.h"
#include "Utils/D3D.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	EnableExperimentalSpecularGI,
	EnableVanillaSSAO,
	AOInteriorsOnly,
	ILInteriorsOnly,
	NumSlices,
	NumSteps,
	EnableAdaptiveSampling,
	ResolutionMode,
	VRCullDistance,
	CenterFullResMaskScale,
	FoveatedPresetMode,
	MinScreenRadius,
	AORadius,
	GIRadius,
	Thickness,
	DepthFadeRange,
	GISaturation,
	GIDistanceCompensation,
	AOPower,
	GIStrength,
	EnableTemporalDenoiser,
	EnableBlur,
	DepthDisocclusion,
	NormalDisocclusion,
	MaxAccumFrames,
	BlurRadius,
	DistanceNormalisation)

namespace
{
	constexpr float kVRCullDistanceMin = 0.0f;
	constexpr float kVRCullDistanceMax = 20480.0f;
	constexpr int kResolutionModeMin = 0;
	constexpr int kResolutionModeMax = 2;
	constexpr int kFoveatedPresetModeOff = 0;
	constexpr int kFoveatedPresetModeStrict = 1;
	constexpr int kFoveatedPresetModeFoveated = 2;

	float ClampVRCullDistance(float a_distance)
	{
		return std::clamp(a_distance, kVRCullDistanceMin, kVRCullDistanceMax);
	}

	int ClampResolutionMode(int a_resolutionMode)
	{
		return std::clamp(a_resolutionMode, kResolutionModeMin, kResolutionModeMax);
	}

	int ClampFoveatedPresetMode(int a_mode)
	{
		return std::clamp(a_mode, kFoveatedPresetModeOff, kFoveatedPresetModeFoveated);
	}

	int ResolveRuntimeFoveatedPresetMode(const ScreenSpaceGI::Settings& a_settings)
	{
		if (!REL::Module::IsVR())
			return kFoveatedPresetModeOff;
		return ClampFoveatedPresetMode(a_settings.FoveatedPresetMode);
	}

	uint32_t QuantizeCenterOffset(float a_value)
	{
		return static_cast<uint32_t>(std::lround((a_value + 1.0f) * 10000.0f));
	}

	bool IsRuntimeFoveatedPresetActive(const ScreenSpaceGI::Settings& a_settings)
	{
		return ResolveRuntimeFoveatedPresetMode(a_settings) != kFoveatedPresetModeOff;
	}
	float GetFoveatedPresetCenterScale(int a_mode)
	{
		if (a_mode == kFoveatedPresetModeStrict)
			return FoveatedCommon::kCenterAreaMin;
		if (a_mode == kFoveatedPresetModeFoveated)
			return FoveatedCommon::kCenterAreaMin;
		return 0.0f;
	}

	float ClampCenterMaskScale(float a_scale)
	{
		if (a_scale <= 0.0f)
			return 0.0f;
		return FoveatedCommon::ClampCenterArea(a_scale);
	}

	bool IsCenterAreaLinkedToUpscaling()
	{
		return REL::Module::IsVR() && globals::features::upscaling.settings.linkFoveatedCenterAreaWithSSGI;
	}

	float GetLinkedUpscalingCenterMaskScale()
	{
		return ClampCenterMaskScale(globals::features::upscaling.settings.foveatedCenterArea);
	}

	float ResolveFoveatedCenterMaskScale(const ScreenSpaceGI::Settings& a_settings)
	{
		if (!REL::Module::IsVR())
			return 0.0f;

		const bool foveatedPresetActive = IsRuntimeFoveatedPresetActive(a_settings);
		if (!foveatedPresetActive)
			return ClampCenterMaskScale(a_settings.CenterFullResMaskScale);

		if (IsCenterAreaLinkedToUpscaling())
			return GetLinkedUpscalingCenterMaskScale();

		return ClampCenterMaskScale(a_settings.CenterFullResMaskScale);
	}

	std::array<float2, 2> GetSharedUpscalingMaskOffsetsForSsgi()
	{
		// Mask placement is always owned by the upscaling feature so both systems stay aligned.
		// The link toggle only controls whether the center-area size is shared.
		auto centerOffsets = globals::features::upscaling.GetResolvedFoveatedMaskCenterOffsets();
		if (!REL::Module::IsVR())
			centerOffsets[1] = { 0.0f, 0.0f };
		return centerOffsets;
	}

	void SyncResolvedCenterMaskScale(ScreenSpaceGI::Settings& a_settings)
	{
		a_settings.CenterFullResMaskScale = ResolveFoveatedCenterMaskScale(a_settings);
	}

	void ResetVRSpecificSettings(ScreenSpaceGI::Settings& a_settings)
	{
		const ScreenSpaceGI::Settings defaults{};
		a_settings.VRCullDistance = defaults.VRCullDistance;
		a_settings.CenterFullResMaskScale = defaults.CenterFullResMaskScale;
		a_settings.FoveatedPresetMode = defaults.FoveatedPresetMode;
	}

	void StripVRSpecificSettings(json& o_json)
	{
		o_json.erase("VRCullDistance");
		o_json.erase("CenterFullResMaskScale");
		o_json.erase("FoveatedPresetMode");
	}

	void ApplyPlatformSettingOverrides(ScreenSpaceGI::Settings& a_settings)
	{
		a_settings.FoveatedPresetMode = ResolveRuntimeFoveatedPresetMode(a_settings);
		a_settings.ResolutionMode = ClampResolutionMode(a_settings.ResolutionMode);
		a_settings.VRCullDistance = ClampVRCullDistance(a_settings.VRCullDistance);
		if (!REL::Module::IsVR()) {
			a_settings.CenterFullResMaskScale = 0.0f;
		}
		if (a_settings.FoveatedPresetMode != kFoveatedPresetModeOff) {
			// Foveated presets run through the quarter-res base path; "Foveated" mode later suppresses periphery AO.
			a_settings.ResolutionMode = 2;
			a_settings.CenterFullResMaskScale = ClampCenterMaskScale(a_settings.CenterFullResMaskScale);
			if (a_settings.CenterFullResMaskScale <= 0.0f)
				a_settings.CenterFullResMaskScale = GetFoveatedPresetCenterScale(a_settings.FoveatedPresetMode);
			// Foveated presets are AO-only by design; IL must stay off while active.
			a_settings.EnableGI = false;
			if (a_settings.FoveatedPresetMode == kFoveatedPresetModeStrict) {
				// Strict mode hard-disables denoiser passes for stability/perf consistency.
				a_settings.EnableTemporalDenoiser = false;
				a_settings.EnableBlur = false;
			}
		} else {
			a_settings.CenterFullResMaskScale = 0.0f;  // no manual foveation path; foveation is preset-toggle only
		}
	}

	float2 GetHardenedSsgiFrameDim(float2 a_renderTexSize)
	{
		float2 frameDim = Util::ConvertToDynamic(a_renderTexSize);
		frameDim = { floor(frameDim.x), floor(frameDim.y) };

		auto* depthSRV = Util::GetCurrentSceneDepthSRV();
		uint32_t depthWidth = 0;
		uint32_t depthHeight = 0;
		if (!Util::TryGetDepthSrvDimensions(depthSRV, depthWidth, depthHeight))
			return { std::max(1.0f, frameDim.x), std::max(1.0f, frameDim.y) };

		float scaleX = frameDim.x / a_renderTexSize.x;  // runtime ratio fallback
		float scaleY = frameDim.y / static_cast<float>(depthHeight);
		scaleY = std::clamp(scaleY, 0.25f, 2.0f);

		if (!REL::Module::IsVR()) {
			scaleX = frameDim.x / static_cast<float>(depthWidth);
		} else {
			const float perEyeFrameWidth = frameDim.x * 0.5f;
			const float combinedX = (perEyeFrameWidth * 2.0f) / static_cast<float>(depthWidth);
			const float perEyeX = perEyeFrameWidth / static_cast<float>(depthWidth);

			const int viewportWidthPerEye = static_cast<int>(std::floor(a_renderTexSize.x * 0.5f));
			switch (Util::DetectVRDepthLayout(depthWidth, viewportWidthPerEye)) {
			case Util::VRDepthLayout::CombinedStereo:
				scaleX = combinedX;
				break;
			case Util::VRDepthLayout::PerEye:
				scaleX = perEyeX;
				break;
			case Util::VRDepthLayout::Unknown:
			default:
				scaleX = std::abs(combinedX - scaleX) <= std::abs(perEyeX - scaleX) ? combinedX : perEyeX;
				break;
			}
		}

		scaleX = std::clamp(scaleX, 0.25f, 2.0f);

		float2 hardenedFrameDim = {
			floor(a_renderTexSize.x * scaleX),
			floor(a_renderTexSize.y * scaleY)
		};
		hardenedFrameDim.x = std::max(1.0f, hardenedFrameDim.x);
		hardenedFrameDim.y = std::max(1.0f, hardenedFrameDim.y);
		return hardenedFrameDim;
	}
}

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::RestoreDefaultSettings()
{
	settings = {};
	ApplyPlatformSettingOverrides(settings);
	recompileFlag = true;
}

void ScreenSpaceGI::DrawSettings()
{
	ApplyPlatformSettingOverrides(settings);
	SyncResolvedCenterMaskScale(settings);
	static bool showAdvanced;
	const bool isVR = REL::Module::IsVR();
	const bool linkedCenterArea = IsCenterAreaLinkedToUpscaling();
	const bool foveatedPresetActive = IsRuntimeFoveatedPresetActive(settings);

	if (!ShadersOK())
		ImGui::TextColored({ 1, 0, 0, 1 }, "Compute shaders failed to compile!");

	///////////////////////////////
	ImGui::SeparatorText("Toggles");

	if (ImGui::BeginTable("Toggles", 3, ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableSetupColumn("ToggleCol1", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("ToggleCol2", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("ToggleCol3", ImGuiTableColumnFlags_WidthStretch, 1.0f);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::Checkbox("Enabled", &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enable Screen Space Global Illumination. When disabled, all other settings are ignored.");
		}

		ImGui::TableNextColumn();
		{
			auto ilToggleGuard = Util::DisableGuard(!settings.Enabled || foveatedPresetActive);
			recompileFlag |= ImGui::Checkbox("Indirect Lighting (IL)", &settings.EnableGI);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Indirect Lighting is forced off while a foveated preset is active.");
		}
		ImGui::TableNextColumn();
		{
			auto ssaoToggleGuard = Util::DisableGuard(!settings.Enabled);
			ImGui::Checkbox("Vanilla SSAO", &settings.EnableVanillaSSAO);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enable Skyrim's built-in SSAO. Usually disabled when using SSGI to avoid double-darkening.");
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		{
			auto advancedGuard = Util::DisableGuard(!settings.Enabled);
			ImGui::Checkbox("Advanced Options", &showAdvanced);
		}

		ImGui::TableNextColumn();
		{
			auto aoInteriorsGuard = Util::DisableGuard(!settings.Enabled);
			ImGui::Checkbox("AO Interiors Only", &settings.AOInteriorsOnly);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Run AO only in interiors to improve exterior performance.");
		}

		ImGui::TableNextColumn();
		{
			auto ilInteriorsGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI || foveatedPresetActive);
			ImGui::Checkbox("IL Interiors Only", &settings.ILInteriorsOnly);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Run indirect lighting only in interiors to improve exterior performance.");
		}

		if (showAdvanced) {
			auto hqSpecGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI || foveatedPresetActive);
			recompileFlag |= ImGui::Checkbox("(Experimental) HQ Specular IL", &settings.EnableExperimentalSpecularGI);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("An experimental specular GI that is more accurate but requires more samples. Won't be blurred.");
		}

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Presets");

	{
		auto presetsAndQualityGuard = Util::DisableGuard(!settings.Enabled);
		auto drawPresetButton = [](const char* a_label, const ImVec2& a_size, const ImVec4& a_normal, const ImVec4& a_hovered, const ImVec4& a_active) {
			Util::StyledButtonWrapper style(a_normal, a_hovered, a_active);
			return ImGui::Button(a_label, a_size);
		};
		auto drawFoveatedToggleButton = [&](const char* a_label, bool a_active, const ImVec2& a_size) {
			const ImVec4 normal = a_active ? ImVec4(0.88f, 0.74f, 0.22f, 1.0f) : ImVec4(0.62f, 0.49f, 0.12f, 1.0f);
			const ImVec4 hovered = a_active ? ImVec4(0.95f, 0.82f, 0.30f, 1.0f) : ImVec4(0.74f, 0.58f, 0.16f, 1.0f);
			const ImVec4 pressed = a_active ? ImVec4(0.98f, 0.88f, 0.38f, 1.0f) : ImVec4(0.82f, 0.66f, 0.20f, 1.0f);
			return drawPresetButton(a_label, a_size, normal, hovered, pressed);
		};

		if (ImGui::BeginTable("Presets", 4, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("PresetAO", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("PresetStrictFoveated", ImGuiTableColumnFlags_WidthStretch, 1.35f);
			ImGui::TableSetupColumn("PresetFoveated", ImGuiTableColumnFlags_WidthStretch, 1.35f);
			ImGui::TableSetupColumn("PresetReference", ImGuiTableColumnFlags_WidthStretch, 1.0f);

			ImGui::TableNextColumn();
			if (ImGui::Button("AO only", { -1, 0 })) {
				settings.NumSlices = 3;
				settings.NumSteps = 6;
				settings.ResolutionMode = 0;
				settings.CenterFullResMaskScale = 0.0f;
				settings.FoveatedPresetMode = kFoveatedPresetModeOff;
				settings.VRCullDistance = 1500.0f;
				settings.AOPower = 1.8f;
				settings.EnableBlur = false;
				settings.EnableTemporalDenoiser = false;
				settings.EnableGI = false;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Full Res, no GI, no SSGI foveation. Upscaling foveation remains independent and can stay enabled.");

			ImGui::TableNextColumn();
			const bool strictActive = settings.FoveatedPresetMode == kFoveatedPresetModeStrict;
			{
				auto foveatedGuard = Util::DisableGuard(!isVR);
				if (drawFoveatedToggleButton("Foveated/QRes", strictActive, { -1, 0 })) {
					if (strictActive) {
						settings.FoveatedPresetMode = kFoveatedPresetModeOff;
						settings.CenterFullResMaskScale = 0.0f;
					} else {
						settings.NumSlices = 3;
						settings.NumSteps = 6;
						settings.ResolutionMode = 2;
						settings.FoveatedPresetMode = kFoveatedPresetModeStrict;
						settings.CenterFullResMaskScale = linkedCenterArea ?
						                                     GetLinkedUpscalingCenterMaskScale() :
						                                     (settings.CenterFullResMaskScale > 0.0f ?
						                                          ClampCenterMaskScale(settings.CenterFullResMaskScale) :
						                                          GetFoveatedPresetCenterScale(settings.FoveatedPresetMode));
						settings.VRCullDistance = 1500.0f;
						settings.AOPower = 1.8f;
						settings.EnableBlur = false;
						settings.EnableTemporalDenoiser = false;
						settings.EnableGI = false;
					}
					recompileFlag = true;
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				if (!isVR)
					ImGui::Text("VR only.");
				ImGui::Text("Quarter-res AO outside with Full Res AO in the center. Denoisers are disabled while active.");
			}

			ImGui::TableNextColumn();
			const bool foveatedActive = settings.FoveatedPresetMode == kFoveatedPresetModeFoveated;
			{
				auto foveatedGuard = Util::DisableGuard(!isVR);
				if (drawFoveatedToggleButton("Foveated/Only", foveatedActive, { -1, 0 })) {
					if (foveatedActive) {
						settings.FoveatedPresetMode = kFoveatedPresetModeOff;
						settings.CenterFullResMaskScale = 0.0f;
					} else {
						settings.NumSlices = 3;
						settings.NumSteps = 6;
						settings.ResolutionMode = 2;
						settings.FoveatedPresetMode = kFoveatedPresetModeFoveated;
						settings.CenterFullResMaskScale = linkedCenterArea ?
						                                     GetLinkedUpscalingCenterMaskScale() :
						                                     (settings.CenterFullResMaskScale > 0.0f ?
						                                          ClampCenterMaskScale(settings.CenterFullResMaskScale) :
						                                          GetFoveatedPresetCenterScale(settings.FoveatedPresetMode));
						settings.VRCullDistance = 1500.0f;
						settings.AOPower = 1.8f;
						settings.EnableGI = false;
					}
					recompileFlag = true;
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				if (!isVR)
					ImGui::Text("VR only.");
				ImGui::Text("Full Res AO in center only; AO is disabled outside center. Use the center-area slider to tune coverage.");
			}

			ImGui::TableNextColumn();
			if (ImGui::Button("Reference", { -1, 0 })) {
				settings.NumSlices = 8;
				settings.NumSteps = 10;
				settings.ResolutionMode = 0;
				settings.FoveatedPresetMode = kFoveatedPresetModeOff;
				settings.CenterFullResMaskScale = 0.0f;
				settings.EnableBlur = true;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("High-quality baseline: Full Res with GI and blur enabled, 8 slices and 10 steps.");

			ImGui::EndTable();
		}
		if (!isVR) {
			ImGui::TextDisabled("Foveated/QRes and Foveated/Only presets are VR only.");
		}

		ImGui::SeparatorText("Quality/Performance");

		if (isVR) {
			ImGui::SliderFloat("AO/IL Cull Distance", &settings.VRCullDistance, kVRCullDistanceMin, kVRCullDistanceMax, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("0 disables. Lower values improve performance but reduce distant AO/IL.");
			}
			settings.VRCullDistance = ClampVRCullDistance(settings.VRCullDistance);
		}

		if (showAdvanced) {
			ImGui::SliderInt("Slices", (int*)&settings.NumSlices, 1, 10);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How many directions do the samples take.\n"
					"Controls noise.");

			ImGui::SliderInt("Steps Per Slice", (int*)&settings.NumSteps, 1, 20);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How many samples does it take in one direction.\n"
					"Controls accuracy of lighting, and noise when effect radius is large.");
		}

		recompileFlag |= ImGui::Checkbox("Adaptive Sampling", &settings.EnableAdaptiveSampling);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Reduces AO sample count in far distance and low-variance regions to improve performance.");
		}

		const int previousResolutionMode = settings.ResolutionMode;
		settings.ResolutionMode = ClampResolutionMode(settings.ResolutionMode);
		settings.FoveatedPresetMode = ResolveRuntimeFoveatedPresetMode(settings);
		const bool foveatedPresetActiveInPerfSection = settings.FoveatedPresetMode != kFoveatedPresetModeOff;

		bool clickedFullRes = false;
		bool clickedHalfRes = false;
		bool clickedQuarterRes = false;
		{
			auto resolutionGuard = Util::DisableGuard(foveatedPresetActiveInPerfSection);
			clickedFullRes = ImGui::RadioButton("Full Res", &settings.ResolutionMode, 0);
		}
		constexpr float kPresetTableTotalWeight = 1.0f + 1.35f + 1.35f + 1.0f;
		constexpr float kSecondPresetColumnStartRatio = 1.0f / kPresetTableTotalWeight;
		const float groupStartX = ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x * kSecondPresetColumnStartRatio);
		ImGui::SameLine(groupStartX);

		ImGui::BeginGroup();
		{
			auto resolutionGuard = Util::DisableGuard(foveatedPresetActiveInPerfSection);
			clickedHalfRes = ImGui::RadioButton("Half Res", &settings.ResolutionMode, 1);
		}
		ImGui::SameLine(0.0f, 14.0f);
		{
			auto resolutionGuard = Util::DisableGuard(foveatedPresetActiveInPerfSection);
			clickedQuarterRes = ImGui::RadioButton("Quarter Res", &settings.ResolutionMode, 2);
		}
		ImGui::EndGroup();

		settings.ResolutionMode = ClampResolutionMode(settings.ResolutionMode);
		if (clickedFullRes || clickedHalfRes || clickedQuarterRes) {
			settings.FoveatedPresetMode = kFoveatedPresetModeOff;
			settings.CenterFullResMaskScale = 0.0f;  // Pure Full/Half/Quarter.
		}
		recompileFlag |= (settings.ResolutionMode != previousResolutionMode);
		if (foveatedPresetActiveInPerfSection) {
			settings.ResolutionMode = 2;
			float centerArea = ResolveFoveatedCenterMaskScale(settings);
			ImGui::SliderFloat("Foveated Area", &centerArea, FoveatedCommon::kCenterAreaMin, FoveatedCommon::kCenterAreaMax, "%.2f");
			centerArea = ClampCenterMaskScale(centerArea);
			if (linkedCenterArea)
				globals::features::upscaling.settings.foveatedCenterArea = centerArea;
			settings.CenterFullResMaskScale = centerArea;
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Controls how much of the screen keeps Full Res AO when a foveated preset is active.");
				if (linkedCenterArea)
					ImGui::Text("Linked with Upscaling center area (shared value).");
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual");

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);

		ImGui::SliderFloat("AO Power", &settings.AOPower, 0.f, 6.f, "%.2f");

		{
			auto ilGuard = Util::DisableGuard(!settings.EnableGI);
			ImGui::SliderFloat("IL Source Brightness", &settings.GIStrength, 0.f, 6.f, "%.2f");
		}

		ImGui::Separator();

		ImGui::SliderFloat("AO radius", &settings.AORadius, 10.f, 1024.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				"A smaller radius produces tighter AO.",
				Util::Units::FormatDistance(settings.AORadius)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		{
			auto ilRadiusGuard = Util::DisableGuard(!settings.EnableGI);

			ImGui::SliderFloat("IL radius", &settings.GIRadius, 10.f, 1024.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"A larger radius produces wider IL.",
					Util::Units::FormatDistance(settings.GIRadius)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}

		if (showAdvanced) {
			ImGui::SliderFloat("Min Screen Radius", &settings.MinScreenRadius, 0.f, 0.05f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"The minimum screen-space effect radius as proportion of display width, to prevent far field AO being too small.");
		}

		ImGui::SliderFloat2("Depth Fade Range", &settings.DepthFadeRange.x, 1e4, 5e4, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				"Distance range where depth-based effects fade out.",
				"Near: " + Util::Units::FormatDistance(settings.DepthFadeRange.x),
				"Far: " + Util::Units::FormatDistance(settings.DepthFadeRange.y)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		if (showAdvanced) {
			ImGui::Separator();

			ImGui::SliderFloat("Thickness", &settings.Thickness, 0.f, 128.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"How thick the occluders are. Only affects AO.",
					Util::Units::FormatDistance(settings.Thickness)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual - IL");

	{
		auto visualILGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI);

		if (showAdvanced) {
			ImGui::SliderFloat("IL Distance Compensation", &settings.GIDistanceCompensation, -5.0f, 5.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brighten/Dimming further radiance samples.");

			ImGui::Separator();
		}

		Util::PercentageSlider("IL Saturation", &settings.GISaturation);
	}

	///////////////////////////////
	ImGui::SeparatorText("Denoising");

	{
		const bool strictFoveatedActive = settings.FoveatedPresetMode == kFoveatedPresetModeStrict;
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled || strictFoveatedActive);

		if (ImGui::BeginTable("denoisers", 2)) {
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::Checkbox("Temporal Denoiser", &settings.EnableTemporalDenoiser);

			ImGui::TableNextColumn();
			ImGui::Checkbox("Blur", &settings.EnableBlur);

			ImGui::EndTable();
		}
		if (strictFoveatedActive) {
			ImGui::TextDisabled("Foveated/QRes is active: denoising is disabled.");
		}

		if (showAdvanced) {
			ImGui::Separator();

			{
				auto temporalGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser);
				ImGui::SliderInt("Max Frame Accumulation", (int*)&settings.MaxAccumFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("How many past frames to accumulate results with. Higher values are less noisy but potentially cause ghosting.");
			}

			ImGui::Separator();

			{
				auto disocclusionGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser && !settings.EnableGI);

				Util::PercentageSlider("Movement Disocclusion", &settings.DepthDisocclusion, 0.f, 20.f);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"If a pixel has moved too far from the last frame, its radiance will not be carried to this frame.\n"
						"Lower values are stricter.");

				ImGui::Separator();
			}

			{
				auto blurGuard = Util::DisableGuard(!settings.EnableBlur);
				ImGui::SliderFloat("Blur Radius", &settings.BlurRadius, 0.f, 30.f, "%.1f px");

				ImGui::SliderFloat("Geometry Weight", &settings.DistanceNormalisation, 0.f, 5.f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"Higher value makes the blur more sensitive to differences in geometry.");
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texNoise, debugRescale)
		BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
		BUFFER_VIEWER_NODE(texPrevGeo, debugRescale)
		BUFFER_VIEWER_NODE(texRadiance, debugRescale)
		BUFFER_VIEWER_NODE(texAo[0], debugRescale)
		BUFFER_VIEWER_NODE(texAo[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[1], debugRescale)

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::LoadSettings(json& o_json)
{
	settings = o_json;
	// Backward compatibility: older configs used a single InteriorsOnly toggle.
	if (o_json.contains("InteriorsOnly") &&
	    !o_json.contains("AOInteriorsOnly") &&
	    !o_json.contains("ILInteriorsOnly")) {
		const bool legacyInteriorsOnly = o_json.value("InteriorsOnly", settings.AOInteriorsOnly);
		settings.AOInteriorsOnly = legacyInteriorsOnly;
		settings.ILInteriorsOnly = legacyInteriorsOnly;
	}
	if (!REL::Module::IsVR()) {
		ResetVRSpecificSettings(settings);
	}
	ApplyPlatformSettingOverrides(settings);

	recompileFlag = true;
}

void ScreenSpaceGI::SaveSettings(json& o_json)
{
	ApplyPlatformSettingOverrides(settings);
	o_json = settings;
	if (!REL::Module::IsVR()) {
		StripVRSpecificSettings(o_json);
	}
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		ssgiCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSGICB>());
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

		{
			texRadiance = eastl::make_unique<Texture2D>(texDesc);
			texRadiance->CreateSRV(srvDesc);
			texRadiance->CreateUAV(uavDesc);  // Create default UAV for mip 0

			// Create individual UAVs for each mip level for prefiltering
			for (uint i = 0; i < 5; ++i) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = DXGI_FORMAT_R11G11B10_FLOAT,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texRadiance->resource.get(), &mipUavDesc, uavRadiance[i].put()));
			}

			// Create temporary texture for prefiltering (single mip level, used as SRV input)
			D3D11_TEXTURE2D_DESC tempTexDesc = texDesc;
			tempTexDesc.MipLevels = 1;
			tempTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

			D3D11_SHADER_RESOURCE_VIEW_DESC tempSrvDesc = {
				.Format = DXGI_FORMAT_R11G11B10_FLOAT,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MostDetailedMip = 0,
					.MipLevels = 1 }
			};

			texRadianceTemp = eastl::make_unique<Texture2D>(tempTexDesc);
			texRadianceTemp->CreateSRV(tempSrvDesc);
		}

		texDesc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;

		{
			texWorkingDepth = eastl::make_unique<Texture2D>(texDesc);
			texWorkingDepth->CreateSRV(srvDesc);
			for (int i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth[i].put()));
			}
		}

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		{
			texIlY[0] = eastl::make_unique<Texture2D>(texDesc);
			texIlY[0]->CreateSRV(srvDesc);
			texIlY[0]->CreateUAV(uavDesc);

			texIlY[1] = eastl::make_unique<Texture2D>(texDesc);
			texIlY[1]->CreateSRV(srvDesc);
			texIlY[1]->CreateUAV(uavDesc);

			texGiSpecular[0] = eastl::make_unique<Texture2D>(texDesc);
			texGiSpecular[0]->CreateSRV(srvDesc);
			texGiSpecular[0]->CreateUAV(uavDesc);

			texGiSpecular[1] = eastl::make_unique<Texture2D>(texDesc);
			texGiSpecular[1]->CreateSRV(srvDesc);
			texGiSpecular[1]->CreateUAV(uavDesc);
		}
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		{
			texIlCoCg[0] = eastl::make_unique<Texture2D>(texDesc);
			texIlCoCg[0]->CreateSRV(srvDesc);
			texIlCoCg[0]->CreateUAV(uavDesc);

			texIlCoCg[1] = eastl::make_unique<Texture2D>(texDesc);
			texIlCoCg[1]->CreateSRV(srvDesc);
			texIlCoCg[1]->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8_UNORM;
		{
			texAo[0] = eastl::make_unique<Texture2D>(texDesc);
			texAo[0]->CreateSRV(srvDesc);
			texAo[0]->CreateUAV(uavDesc);

			texAo[1] = eastl::make_unique<Texture2D>(texDesc);
			texAo[1]->CreateSRV(srvDesc);
			texAo[1]->CreateUAV(uavDesc);

			texAccumFrames[0] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[0]->CreateSRV(srvDesc);
			texAccumFrames[0]->CreateUAV(uavDesc);

			texAccumFrames[1] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[1]->CreateSRV(srvDesc);
			texAccumFrames[1]->CreateUAV(uavDesc);

			texCenterAo = eastl::make_unique<Texture2D>(texDesc);
			texCenterAo->CreateSRV(srvDesc);
			texCenterAo->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		{
			texCenterIlY = eastl::make_unique<Texture2D>(texDesc);
			texCenterIlY->CreateSRV(srvDesc);
			texCenterIlY->CreateUAV(uavDesc);

			texCenterGiSpecular = eastl::make_unique<Texture2D>(texDesc);
			texCenterGiSpecular->CreateSRV(srvDesc);
			texCenterGiSpecular->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		{
			texCenterIlCoCg = eastl::make_unique<Texture2D>(texDesc);
			texCenterIlCoCg->CreateSRV(srvDesc);
			texCenterIlCoCg->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		{
			texPrevGeo = eastl::make_unique<Texture2D>(texDesc);
			texPrevGeo->CreateSRV(srvDesc);
			texPrevGeo->CreateUAV(uavDesc);
		}
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\ScreenSpaceGI\\fast_2uges.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(srvDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
	}

	CompileComputeShaders();
}

void ScreenSpaceGI::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&prefilterDepthsCompute,
		&prefilterRadianceCompute,
		&radianceDisoccCompute,
		&radianceDisoccAOOnlyCompute,
		&giCompute,
		&giAOOnlyCompute,
		&centerGIMaskedCompute,
		&centerGIMaskedAOOnlyCompute,
		&blurCompute,
		&upsampleCompute,
		&upsampleAOOnlyCompute,
		&centerBlendCompute,
		&centerBlendAOOnlyCompute
	};

	for (auto shader : shaderPtrs)
		*shader = nullptr;

	CompileComputeShaders();
}

void ScreenSpaceGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		bool includeResolutionDefines = true;
		bool includeTemporalDefines = true;
		bool includeGIDefines = true;
		bool includeAdaptiveSamplingDefines = false;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", { { "LINEAR_FILTER", "" } } },
			{ &prefilterRadianceCompute, "prefilterRadiance.cs.hlsl", {} },
			{ &radianceDisoccCompute, "radianceDisocc.cs.hlsl", {} },
			{ &radianceDisoccAOOnlyCompute, "radianceDisocc.cs.hlsl", {}, true, true, false },
			{ &giCompute, "gi.cs.hlsl", {}, true, true, true, true },
			{ &giAOOnlyCompute, "gi.cs.hlsl", {}, true, true, false, true },
			{ &centerGIMaskedCompute, "gi.cs.hlsl", { { "CENTER_FULL_PASS", "" } }, false, false, true, true },
			{ &centerGIMaskedAOOnlyCompute, "gi.cs.hlsl", { { "CENTER_FULL_PASS", "" } }, false, false, false, true },
			{ &blurCompute, "blur.cs.hlsl", {} },
			{ &upsampleCompute, "upsample.cs.hlsl", {} },
			{ &upsampleAOOnlyCompute, "upsample.cs.hlsl", {}, true, false, false },
			{ &centerBlendCompute, "centerBlend.cs.hlsl", {}, false, false },
			{ &centerBlendAOOnlyCompute, "centerBlend.cs.hlsl", {}, false, false, false },
		};
	for (auto& info : shaderInfos) {
		if (REL::Module::IsVR())
			info.defines.push_back({ "VR", "" });
		if (info.includeResolutionDefines) {
			if (settings.ResolutionMode == 1)
				info.defines.push_back({ "HALF_RES", "" });
			if (settings.ResolutionMode == 2)
				info.defines.push_back({ "QUARTER_RES", "" });
		}
		if (info.includeTemporalDefines && settings.EnableTemporalDenoiser)
			info.defines.push_back({ "TEMPORAL_DENOISER", "" });
		if (info.includeGIDefines && settings.EnableGI)
			info.defines.push_back({ "GI", "" });
		if (info.includeGIDefines && settings.EnableExperimentalSpecularGI)
			info.defines.push_back({ "GI_SPECULAR", "" });
		if (info.includeAdaptiveSamplingDefines && settings.EnableAdaptiveSampling)
			info.defines.push_back({ "ADAPTIVE_SAMPLING", "" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceGI::ShadersOK()
{
	const bool baseShadersOK = texNoise &&
	                           prefilterDepthsCompute &&
	                           prefilterRadianceCompute &&
	                           radianceDisoccCompute &&
	                           radianceDisoccAOOnlyCompute &&
	                           giCompute &&
	                           giAOOnlyCompute &&
	                           blurCompute &&
	                           upsampleCompute &&
	                           upsampleAOOnlyCompute;

	const bool centerShadersOK = texCenterAo &&
	                             texCenterIlY &&
	                             texCenterIlCoCg &&
	                             texCenterGiSpecular &&
	                             centerGIMaskedCompute &&
	                             centerGIMaskedAOOnlyCompute &&
	                             centerBlendCompute &&
	                             centerBlendAOOnlyCompute;
	const float centerScale = ResolveFoveatedCenterMaskScale(settings);
	const bool centerMaskActive = centerScale > 0.0f;

	// Keep legacy SSGI path fully functional when center mask is off.
	if (ClampResolutionMode(settings.ResolutionMode) == 0 || !centerMaskActive)
		return baseShadersOK;

	return baseShadersOK && centerShadersOK;
}

void ScreenSpaceGI::UpdateSB()
{
	float2 res = { (float)texRadiance->desc.Width, (float)texRadiance->desc.Height };
	float2 dynres = GetHardenedSsgiFrameDim(res);
	const bool isVR = REL::Module::IsVR();
	const float centerMaskScale = ResolveFoveatedCenterMaskScale(settings);

	static float4x4 prevInvView[2] = {};

	SSGICB& data = ssgiCBData;
	data = {};
	{
		const bool useUnjitteredCamera = isVR;

		for (int eyeIndex = 0; eyeIndex < (1 + isVR); ++eyeIndex) {
			const auto eye = Util::GetCameraData(eyeIndex);
			float proj11 = eye.projMat(0, 0);
			float proj22 = eye.projMat(1, 1);
			float4x4 currentInvView = eye.viewMat.Invert();

			if (useUnjitteredCamera) {
				const auto& projUnjittered = globals::game::frameBufferCached.GetCameraProjUnjittered(eyeIndex);
				proj11 = projUnjittered._11;
				proj22 = projUnjittered._22;
				currentInvView = globals::game::frameBufferCached.GetCameraViewInverse(eyeIndex);
			}

			data.PrevInvViewMat[eyeIndex] = prevInvView[eyeIndex];
			data.NDCToViewMul[eyeIndex] = { 2.0f / proj11, -2.0f / proj22 };
			data.NDCToViewAdd[eyeIndex] = { -1.0f / proj11, 1.0f / proj22 };
			if (isVR)
				data.NDCToViewMul[eyeIndex].x *= 2;

			prevInvView[eyeIndex] = currentInvView;
		}

		data.TexDim = res;
		data.RcpTexDim = float2(1.0f) / res;
		data.FrameDim = dynres;
		data.RcpFrameDim = float2(1.0f) / dynres;
		data.FrameIndex = globals::state->frameCount;

		data.NumSlices = settings.NumSlices;
		data.NumSteps = settings.NumSteps;
		data.MinScreenRadius = settings.MinScreenRadius * dynres.x;

		data.EffectRadius = std::max(settings.AORadius, settings.GIRadius);
		const float safeEffectRadius = std::max(data.EffectRadius, 1e-3f);
		data.EffectRadius = safeEffectRadius;
		data.AORadius = settings.AORadius / safeEffectRadius;
		data.GIRadius = settings.GIRadius / safeEffectRadius;
		data.Thickness = settings.Thickness;
		const float depthFadeStart = std::min(settings.DepthFadeRange.x, settings.DepthFadeRange.y);
		const float depthFadeEnd = std::max(settings.DepthFadeRange.x, settings.DepthFadeRange.y);
		data.DepthFadeRange = { depthFadeStart, depthFadeEnd };
		const float depthFadeSpan = std::max(depthFadeEnd - depthFadeStart, 1.0f);
		data.DepthFadeScaleConst = 1.0f / depthFadeSpan;

		data.GISaturation = settings.GISaturation;
		data.GIDistanceCompensation = settings.GIDistanceCompensation;
		data.GICompensationMaxDist = settings.AORadius;

		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;

		data.DepthDisocclusion = settings.DepthDisocclusion;
		data.NormalDisocclusion = settings.NormalDisocclusion;
		data.MaxAccumFrames = settings.MaxAccumFrames;
		data.BlurRadius = settings.BlurRadius;
		data.DistanceNormalisation = settings.DistanceNormalisation;
		data.VRCullDistance = isVR ? ClampVRCullDistance(settings.VRCullDistance) : 0.0f;
		data.CenterFullResMaskScale = centerMaskScale;
		data.CenterFullResMaskFeather = FoveatedCommon::kCenterFeather;
		auto centerOffsets = GetSharedUpscalingMaskOffsetsForSsgi();
		data.CenterFullResMaskOffsets = { centerOffsets[0].x, centerOffsets[0].y, centerOffsets[1].x, centerOffsets[1].y };
		data.CenterDispatchOffsetX = 0.0f;
		data.CenterDispatchOffsetY = 0.0f;
		data.CenterDispatchSizeX = dynres.x;
		data.CenterDispatchSizeY = dynres.y;
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI()
{
	ApplyPlatformSettingOverrides(settings);
	SyncResolvedCenterMaskScale(settings);

	auto context = globals::d3d::context;
	if (!context)
		return;
	const bool isVR = REL::Module::IsVR();
	const int resolutionMode = ClampResolutionMode(settings.ResolutionMode);
	const int foveatedPresetMode = ResolveRuntimeFoveatedPresetMode(settings);
	const float centerScale = ResolveFoveatedCenterMaskScale(settings);
	const bool foveatedCenterOnlyMode = isVR && foveatedPresetMode == kFoveatedPresetModeFoveated;

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISSAOBlurH, imageSpaceManager);

	// Toggle vanilla SSAO
	static bool* enableSSAO = reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(BSImagespaceShaderISSAOBlurH.get()) + 0x50LL);
	*enableSSAO = settings.EnableVanillaSSAO;

	const bool isInterior = Util::IsInterior();
	const bool allowAOSpace = !settings.AOInteriorsOnly || isInterior;
	const bool allowILSpace = !settings.ILInteriorsOnly || isInterior;
	const bool runILPath = settings.EnableGI && allowILSpace;
	const bool temporalEnabled = settings.EnableTemporalDenoiser;
	const bool runRadianceDisoccPass = !foveatedCenterOnlyMode && (runILPath || temporalEnabled);
	const bool runPrefilterRadiancePass = runILPath;
	const bool blurEnabled = !foveatedCenterOnlyMode && settings.EnableBlur && runILPath;
	ID3D11ComputeShader* activeRadianceDisoccCompute = nullptr;
	ID3D11ComputeShader* activeGICompute = nullptr;
	ID3D11ComputeShader* activeCenterGICompute = nullptr;
	ID3D11ComputeShader* activeCenterBlendCompute = nullptr;
	ID3D11ComputeShader* activeUpsampleCompute = nullptr;
	auto refreshActiveShaders = [&]() {
		activeRadianceDisoccCompute = runILPath ? radianceDisoccCompute.get() : radianceDisoccAOOnlyCompute.get();
		activeGICompute = runILPath ? giCompute.get() : giAOOnlyCompute.get();
		activeCenterGICompute = runILPath ? centerGIMaskedCompute.get() : centerGIMaskedAOOnlyCompute.get();
		activeCenterBlendCompute = runILPath ? centerBlendCompute.get() : centerBlendAOOnlyCompute.get();
		activeUpsampleCompute = runILPath ? upsampleCompute.get() : upsampleAOOnlyCompute.get();
	};
	refreshActiveShaders();
	static uint lastFrameAoTexIdx = 0;
	static uint lastFrameGITexIdx = 0;
	static uint lastFrameAccumTexIdx = 0;

	auto resetHistoryState = [&](const char* a_reason) {
		lastFrameAoTexIdx = 0;
		lastFrameGITexIdx = 0;
		lastFrameAccumTexIdx = 0;
		outputAoIdx = 0;
		outputIlIdx = 0;

		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		auto clearUavIfValid = [&](auto& a_texture) {
			if (a_texture && a_texture->uav)
				context->ClearUnorderedAccessViewFloat(a_texture->uav.get(), clr);
		};
		clearUavIfValid(texAccumFrames[0]);
		clearUavIfValid(texAccumFrames[1]);
		clearUavIfValid(texAo[0]);
		clearUavIfValid(texAo[1]);
		clearUavIfValid(texIlY[0]);
		clearUavIfValid(texIlY[1]);
		clearUavIfValid(texIlCoCg[0]);
		clearUavIfValid(texIlCoCg[1]);
		clearUavIfValid(texGiSpecular[0]);
		clearUavIfValid(texGiSpecular[1]);
		clearUavIfValid(texPrevGeo);
		logger::debug("SSGI history reset ({})", a_reason);
	};

	auto clearOutputsAndReturn = [&]() {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		auto clearOutputIfValid = [&](auto& a_textureArray, uint a_index) {
			if (a_index < 2 && a_textureArray[a_index] && a_textureArray[a_index]->uav)
				context->ClearUnorderedAccessViewFloat(a_textureArray[a_index]->uav.get(), clr);
		};
		clearOutputIfValid(texAo, outputAoIdx);
		clearOutputIfValid(texIlY, outputIlIdx);
		clearOutputIfValid(texIlCoCg, outputIlIdx);
		clearOutputIfValid(texGiSpecular, outputAoIdx);
	};

	if (!(settings.Enabled && (allowAOSpace || allowILSpace))) {
		clearOutputsAndReturn();
		return;
	}

	static uint64_t lastModeSignature = 0;
	static bool hasModeSignature = false;
	const uint modeCenterScaleMilli = static_cast<uint>(std::round(centerScale * 1000.0f));
	uint64_t modeSignature = 1469598103934665603ull;
	auto hashCombine = [&](uint64_t a_value) {
		modeSignature ^= a_value + 0x9e3779b97f4a7c15ull + (modeSignature << 6) + (modeSignature >> 2);
	};
	hashCombine(static_cast<uint64_t>(resolutionMode));
	hashCombine(static_cast<uint64_t>(foveatedPresetMode));
	hashCombine(static_cast<uint64_t>(modeCenterScaleMilli));
	if (modeCenterScaleMilli > 0) {
		const auto modeCenterOffsets = GetSharedUpscalingMaskOffsetsForSsgi();
		hashCombine(static_cast<uint64_t>(QuantizeCenterOffset(modeCenterOffsets[0].x)));
		hashCombine(static_cast<uint64_t>(QuantizeCenterOffset(modeCenterOffsets[0].y)));
		if (isVR) {
			hashCombine(static_cast<uint64_t>(QuantizeCenterOffset(modeCenterOffsets[1].x)));
			hashCombine(static_cast<uint64_t>(QuantizeCenterOffset(modeCenterOffsets[1].y)));
		}
	}
	hashCombine(static_cast<uint64_t>(runILPath));
	hashCombine(static_cast<uint64_t>(temporalEnabled));
	hashCombine(static_cast<uint64_t>(blurEnabled));
	hashCombine(static_cast<uint64_t>(settings.EnableExperimentalSpecularGI));
	hashCombine(static_cast<uint64_t>(allowAOSpace));
	hashCombine(static_cast<uint64_t>(allowILSpace));
	if (!hasModeSignature || modeSignature != lastModeSignature) {
		resetHistoryState("runtime mode switch");
		lastModeSignature = modeSignature;
		hasModeSignature = true;
	}

	if (recompileFlag) {
		ClearShaderCache();
		refreshActiveShaders();
		resetHistoryState("shader recompile");
		clearOutputsAndReturn();
		return;
	}

	if (!ShadersOK()) {
		logger::warn("SSGI shader set incomplete for current runtime mode; skipping frame.");
		clearOutputsAndReturn();
		return;
	}

	const bool centerShadersReady = texCenterAo &&
	                                texCenterIlY &&
	                                texCenterIlCoCg &&
	                                texCenterGiSpecular &&
	                                centerGIMaskedCompute &&
	                                centerGIMaskedAOOnlyCompute &&
	                                centerBlendCompute &&
	                                centerBlendAOOnlyCompute;
	const bool centerMaskEnabled = centerShadersReady &&
	                               (resolutionMode != 0) &&
	                               (centerScale > 0.0f);
	const bool centerBlendNeeded = centerMaskEnabled && (centerScale < 0.99f);
	const bool centerDirectWrite = centerMaskEnabled && !centerBlendNeeded;

	auto requireActiveShader = [&](bool a_needed, ID3D11ComputeShader* a_shader, const char* a_name) {
		if (!a_needed)
			return true;
		if (a_shader)
			return true;
		logger::warn("SSGI runtime shader missing ({}); skipping frame.", a_name);
		clearOutputsAndReturn();
		return false;
	};
	if (!requireActiveShader(runRadianceDisoccPass, activeRadianceDisoccCompute, "radianceDisocc(active)") ||
	    !requireActiveShader(!foveatedCenterOnlyMode, activeGICompute, "gi(active)") ||
	    !requireActiveShader(blurEnabled, blurCompute.get(), "blur") ||
	    !requireActiveShader((resolutionMode != 0) && !centerDirectWrite && !foveatedCenterOnlyMode, activeUpsampleCompute, "upsample(active)") ||
	    !requireActiveShader(centerMaskEnabled, activeCenterGICompute, "centerGI(active)") ||
	    !requireActiveShader(centerBlendNeeded, activeCenterBlendCompute, "centerBlend(active)")) {
		return;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSGI");

	uint inputAoTexIdx = lastFrameAoTexIdx;
	uint inputGITexIdx = lastFrameGITexIdx;

	UpdateSB();

	//////////////////////////////////////////////////////

	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = globals::deferred;

	float2 size = {
		(float)texRadiance->desc.Width,
		(float)texRadiance->desc.Height
	};
	size = GetHardenedSsgiFrameDim(size);
	auto resolution = std::array{ (uint)size.x, (uint)size.y };
	auto resChoices = std::array{
		resolution, std::array{ resolution[0] >> 1, resolution[1] >> 1 }, std::array{ resolution[0] >> 2, resolution[1] >> 2 }
	};
	auto internalRes = resChoices[resolutionMode];
	using DispatchRect = CenterDispatchRect;
	auto centerOffsets = GetSharedUpscalingMaskOffsetsForSsgi();

	auto buildCenterDispatchRect = [&](uint a_eyeIndex) -> DispatchRect {
		DispatchRect rect{};
		const uint frameWidth = resolution[0];
		const uint frameHeight = resolution[1];
		if (frameWidth == 0 || frameHeight == 0)
			return rect;

		uint eyeMinX = 0;
		uint eyeMaxX = frameWidth;
		if (isVR) {
			const uint midX = frameWidth >> 1;
			if (a_eyeIndex == 0) {
				eyeMinX = 0;
				eyeMaxX = midX;
			} else {
				eyeMinX = midX;
				eyeMaxX = frameWidth;
			}
		}

		const uint eyeWidth = (eyeMaxX > eyeMinX) ? (eyeMaxX - eyeMinX) : 0;
		if (eyeWidth == 0)
			return rect;

		const float2 centerOffset = centerOffsets[a_eyeIndex];
		const auto bounds = FoveatedCommon::BuildCenteredDispatchBounds(eyeMinX, eyeMaxX, frameHeight, centerScale, centerOffset.x, centerOffset.y);
		const int minX = bounds.minX;
		const int maxX = bounds.maxX;
		const int minY = bounds.minY;
		const int maxY = bounds.maxY;

		if (maxX <= minX || maxY <= minY)
			return rect;

		rect.x = static_cast<uint>(minX);
		rect.y = static_cast<uint>(minY);
		rect.width = static_cast<uint>(maxX - minX);
		rect.height = static_cast<uint>(maxY - minY);
		return rect;
	};

	auto& cache = centerRectCache;
	const float centerScaleDelta = cache.scale - centerScale;
	const bool centerCacheDirty =
		cache.frameWidth != resolution[0] ||
		cache.frameHeight != resolution[1] ||
		cache.isVR != isVR ||
		(centerScaleDelta < 0.0f ? -centerScaleDelta : centerScaleDelta) > 1e-6f ||
		std::abs(cache.centerOffsets[0].x - centerOffsets[0].x) > 1e-6f ||
		std::abs(cache.centerOffsets[0].y - centerOffsets[0].y) > 1e-6f ||
		(isVR && (std::abs(cache.centerOffsets[1].x - centerOffsets[1].x) > 1e-6f ||
		          std::abs(cache.centerOffsets[1].y - centerOffsets[1].y) > 1e-6f));
	if (centerCacheDirty) {
		cache.frameWidth = resolution[0];
		cache.frameHeight = resolution[1];
		cache.isVR = isVR;
		cache.scale = centerScale;
		cache.centerOffsets = centerOffsets;
		cache.rects[0] = buildCenterDispatchRect(0);
		cache.rects[1] = isVR ? buildCenterDispatchRect(1) : DispatchRect{};
	}

	auto forEachCenterRect = [&](auto&& a_fn) {
		a_fn(cache.rects[0]);
		if (isVR)
			a_fn(cache.rects[1]);
	};

	auto dispatchCenterShader = [&](ID3D11ComputeShader* a_shader) {
		forEachCenterRect([&](const DispatchRect& rect) {
			if (rect.width == 0 || rect.height == 0)
				return;

			ssgiCBData.CenterDispatchOffsetX = static_cast<float>(rect.x);
			ssgiCBData.CenterDispatchOffsetY = static_cast<float>(rect.y);
			ssgiCBData.CenterDispatchSizeX = static_cast<float>(rect.width);
			ssgiCBData.CenterDispatchSizeY = static_cast<float>(rect.height);
			ssgiCB->Update(ssgiCBData);
			ID3D11Buffer* centerCb = ssgiCB->CB();
			context->CSSetConstantBuffers(1, 1, &centerCb);

			context->CSSetShader(a_shader, nullptr, 0);
			context->Dispatch((rect.width + 7u) >> 3, (rect.height + 7u) >> 3, 1);
		});
	};

	auto copyTextureRects = [&](ID3D11Resource* a_dst, ID3D11Resource* a_src) {
		forEachCenterRect([&](const DispatchRect& rect) {
			if (rect.width == 0 || rect.height == 0)
				return;
			D3D11_BOX srcBox{
				rect.x,
				rect.y,
				0u,
				rect.x + rect.width,
				rect.y + rect.height,
				1u
			};
			context->CopySubresourceRegion(a_dst, 0, rect.x, rect.y, 0, a_src, 0, &srcBox);
		});
	};

	std::array<ID3D11ShaderResourceView*, 11> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 6> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssgiCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	//////////////////////////////////////////////////////

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// prefilter depths
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Depths");

		srvs.at(0) = Util::GetCurrentSceneDepthSRV();
		for (int i = 0; i < 5; ++i)
			uavs.at(i) = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);
	}

	// fetch radiance and disocclusion (optional in AO-only + no temporal mode)
	if (runRadianceDisoccPass) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Radiance Disocc");

		resetViews();
		srvs.at(0) = runILPath ? rts[deferred->forwardRenderTargets[0]].SRV : nullptr;
		srvs.at(1) = texWorkingDepth->srv.get();
		srvs.at(2) = rts[NORMALROUGHNESS].SRV;
		if (temporalEnabled) {
			srvs.at(3) = texPrevGeo->srv.get();
			srvs.at(4) = rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
			srvs.at(5) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
			srvs.at(6) = texAo[inputAoTexIdx]->srv.get();
			if (runILPath) {
				srvs.at(7) = texIlY[inputGITexIdx]->srv.get();
				srvs.at(8) = texIlCoCg[inputGITexIdx]->srv.get();
				srvs.at(9) = texGiSpecular[inputAoTexIdx]->srv.get();
			}
		}
		srvs.at(10) = nullptr;

		// AO-only temporal mode does not need radiance or IL history traffic.
		uavs.at(0) = runILPath ? texRadiance->uav.get() : nullptr;
		if (temporalEnabled) {
			uavs.at(1) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
			uavs.at(2) = texAo[!inputAoTexIdx]->uav.get();
			if (runILPath) {
				uavs.at(3) = texIlY[!inputGITexIdx]->uav.get();
				uavs.at(4) = texIlCoCg[!inputGITexIdx]->uav.get();
				uavs.at(5) = texGiSpecular[!inputAoTexIdx]->uav.get();
			}
		}

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(activeRadianceDisoccCompute, nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		// Prefilter radiance texture only when GI is enabled.
		if (runPrefilterRadiancePass) {
			TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Radiance");

			// First copy mip 0 from radiance to temporary texture to avoid read/write conflict
			context->CopySubresourceRegion(
				texRadianceTemp->resource.get(), 0, 0, 0, 0,
				texRadiance->resource.get(), 0, nullptr);

			resetViews();
			srvs.at(0) = texRadianceTemp->srv.get();  // Use temporary texture as input
			uavs.at(0) = uavRadiance[0].get();        // Mip 0
			uavs.at(1) = uavRadiance[1].get();        // Mip 1
			uavs.at(2) = uavRadiance[2].get();        // Mip 2
			uavs.at(3) = uavRadiance[3].get();        // Mip 3
			uavs.at(4) = uavRadiance[4].get();        // Mip 4

			context->CSSetShaderResources(0, 1, srvs.data());
			context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
			context->CSSetShader(prefilterRadianceCompute.get(), nullptr, 0);
			context->Dispatch((internalRes[0] + 15u) >> 4, (internalRes[1] + 15u) >> 4, 1);
		}

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		if (temporalEnabled)
			lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// GI
	if (!foveatedCenterOnlyMode) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - GI");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = runILPath ? texRadiance->srv.get() : nullptr;
		srvs.at(3) = texNoise->srv.get();
		if (temporalEnabled) {
			srvs.at(4) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
			srvs.at(5) = texAo[inputAoTexIdx]->srv.get();
			srvs.at(6) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(7) = texIlCoCg[inputGITexIdx]->srv.get();
			srvs.at(8) = texGiSpecular[inputAoTexIdx]->srv.get();
		}

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
		uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();
		uavs.at(4) = texPrevGeo->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(activeGICompute, nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAoTexIdx = inputAoTexIdx;
	}

	// blur
	if (blurEnabled) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Diffuse Blur");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = temporalEnabled ? texAccumFrames[lastFrameAccumTexIdx]->srv.get() : nullptr;
		srvs.at(3) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(4) = texIlCoCg[inputGITexIdx]->srv.get();

		uavs.at(0) = temporalEnabled ? texAccumFrames[!lastFrameAccumTexIdx]->uav.get() : nullptr;
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(blurCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		if (temporalEnabled)
			lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// upsample
	if (resolutionMode != 0 && !centerDirectWrite && !foveatedCenterOnlyMode) {
		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = texAo[inputAoTexIdx]->srv.get();
		if (runILPath) {
			srvs.at(2) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(3) = texIlCoCg[inputGITexIdx]->srv.get();
			srvs.at(4) = texGiSpecular[inputAoTexIdx]->srv.get();
		}

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		if (runILPath) {
			uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
			uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
			uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();
		}

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(activeUpsampleCompute, nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
	}

	// full-res center refinement (half/quarter modes), then smooth blend into current output
	if (centerMaskEnabled) {
		if (foveatedCenterOnlyMode) {
			FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
			const uint clearAoIdx = centerBlendNeeded ? inputAoTexIdx : !inputAoTexIdx;
			if (clearAoIdx < 2 && texAo[clearAoIdx] && texAo[clearAoIdx]->uav)
				context->ClearUnorderedAccessViewFloat(texAo[clearAoIdx]->uav.get(), clr);
		}

		{
			TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Center FullRes GI");

			resetViews();
			srvs.at(0) = texWorkingDepth->srv.get();
			srvs.at(1) = rts[NORMALROUGHNESS].SRV;
			srvs.at(2) = runILPath ? texRadiance->srv.get() : nullptr;
			srvs.at(3) = texNoise->srv.get();
			srvs.at(9) = runILPath ? rts[deferred->forwardRenderTargets[0]].SRV : nullptr;

			uavs.at(0) = centerBlendNeeded ? texCenterAo->uav.get() : texAo[!inputAoTexIdx]->uav.get();
			uavs.at(1) = centerBlendNeeded ? texCenterIlY->uav.get() : texIlY[!inputGITexIdx]->uav.get();
			uavs.at(2) = centerBlendNeeded ? texCenterIlCoCg->uav.get() : texIlCoCg[!inputGITexIdx]->uav.get();
			const bool writeCenterSpecular = runILPath && settings.EnableExperimentalSpecularGI;
			uavs.at(3) = writeCenterSpecular ?
			                 (centerBlendNeeded ? texCenterGiSpecular->uav.get() : texGiSpecular[!inputAoTexIdx]->uav.get()) :
			                 nullptr;
			uavs.at(4) = texPrevGeo->uav.get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			dispatchCenterShader(activeCenterGICompute);
		}

		if (centerBlendNeeded) {
			TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Center Blend");

			resetViews();

			srvs.at(0) = texAo[inputAoTexIdx]->srv.get();
			srvs.at(4) = texCenterAo->srv.get();
			uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
			if (runILPath) {
				srvs.at(1) = texIlY[inputGITexIdx]->srv.get();
				srvs.at(2) = texIlCoCg[inputGITexIdx]->srv.get();
				srvs.at(3) = texGiSpecular[inputAoTexIdx]->srv.get();
				srvs.at(5) = texCenterIlY->srv.get();
				srvs.at(6) = texCenterIlCoCg->srv.get();
				srvs.at(7) = texCenterGiSpecular->srv.get();
				uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
				uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
				uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();
			}

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			dispatchCenterShader(activeCenterBlendCompute);

			// Blend pass runs center-rect only; copy just those rects back into the full-frame source.
			resetViews();
			copyTextureRects(texAo[inputAoTexIdx]->resource.get(), texAo[!inputAoTexIdx]->resource.get());
			if (runILPath) {
				copyTextureRects(texIlY[inputGITexIdx]->resource.get(), texIlY[!inputGITexIdx]->resource.get());
				copyTextureRects(texIlCoCg[inputGITexIdx]->resource.get(), texIlCoCg[!inputGITexIdx]->resource.get());
				copyTextureRects(texGiSpecular[inputAoTexIdx]->resource.get(), texGiSpecular[!inputAoTexIdx]->resource.get());
			}
		} else {
			// Center pass wrote directly into the destination full-res buffers.
			inputAoTexIdx = !inputAoTexIdx;
			inputGITexIdx = !inputGITexIdx;
		}
	}

	outputAoIdx = inputAoTexIdx;
	outputIlIdx = inputGITexIdx;

	// Apply split interior gating after the pipeline: AO and IL can now be gated independently.
	if (!allowAOSpace) {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texAo[outputAoIdx]->uav.get(), clr);
	}
	if (!allowILSpace && settings.EnableGI) {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texIlY[outputIlIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texIlCoCg[outputIlIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texGiSpecular[outputAoIdx]->uav.get(), clr);
	}

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
}
