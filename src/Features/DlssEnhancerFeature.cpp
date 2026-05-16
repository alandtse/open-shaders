#include "DlssEnhancerFeature.h"

#include "DlssEnhancer/Core.h"
#include "Globals.h"
#include "Upscaling.h"
#include "Utils/UI.h"

#include <algorithm>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DlssEnhancerFeature::Settings,
	enabled,
	qualityMode,
	streamlineLogLevel,
	sharpnessDLSS,
	presetDLSS,
	dlssMode,
	stretchMode,
	sharpenMode,
	enableMVDilation,
	enableReactiveMask,
	enableTransparencyMask);

bool DlssEnhancerFeature::IsActive() const
{
	return enabledAtBoot && IsRuntimeSupported();
}

bool DlssEnhancerFeature::IsRuntimeSupported() const
{
	return globals::game::isVR && globals::features::upscaling.streamline.featureDLSS;
}

float DlssEnhancerFeature::GetRenderScaleForQuality(uint qualityMode)
{
	switch (qualityMode) {
	case 1:
		return 1.5f;  // Quality
	case 2:
		return 1.7f;  // Balanced
	case 3:
		return 2.0f;  // Performance
	case 4:
		return 3.0f;  // Ultra Performance
	default:
		return 3.0f;  // fallback to safest (smallest render)
	}
}

bool DlssEnhancerFeature::IsPresetCompatibleWithMode(uint presetIndex) const
{
	// Preset indices: 0=Default, 1=J, 2=K, 3=L, 4=M, 5=F
	// Faster mode: J(1) and K(2) are incompatible
	if (GetDlssMode() == DlssMode::kFaster) {
		return presetIndex != 1 && presetIndex != 2;
	}
	return true;
}

void DlssEnhancerFeature::ClampPresetToMode()
{
	if (!IsPresetCompatibleWithMode(settings.presetDLSS)) {
		settings.presetDLSS = 3;  // Fall back to L
	}
}

void DlssEnhancerFeature::PostPostLoad()
{
	// Opt into PR-1's stereo extension so the controller tracks a separate
	// right-eye UV (HMD nose-side overlap symmetry).
	subrectController.SetStereoEnabled(true);

	// Seed sensible foveal presets. Empty-case only — user edits/deletions persist.
	subrectController.SeedDefaultPresets({
		{ "Full Eye", { 0.0f, 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },
		{ "Center 75%", { 0.125f, 0.125f, 0.75f, 0.75f }, { 0.125f, 0.125f, 0.75f, 0.75f } },
		{ "Center 50%", { 0.25f, 0.25f, 0.5f, 0.5f }, { 0.25f, 0.25f, 0.5f, 0.5f } },
	});
}

void DlssEnhancerFeature::DrawSettings()
{
	static const char* toggleModes[] = { "Off", "On" };
	static const char* qualityModes[] = { "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance" };
	static const char* logLevels[] = { "Off", "Default", "Verbose" };
	static const char* presets[] = { "Default", "Preset J", "Preset K", "Preset L", "Preset M", "Preset F" };
	static const char* dlssModes[] = { "Default", "Faster" };
	static const char* stretchModes[] = { "Bilinear", "Point (VRS-like)", "Gaussian Blur" };
	static const char* sharpenModes[] = { "RCAS", "None" };

	ClampSettings();

	// ── Feature description ──
	ImGui::TextWrapped(
		"VR DLSS enhancement: independent DLSS control and subrect cropping "
		"to reduce DLSS cost. Great for low-end GPUs.");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Subrect DLSS only upscales the region you select; the periphery is cheaply stretched.\n"
			"Composes with VRS, Screenshot, and upcoming lossless recording via the shared Subrect module.");

	// Reminder: save settings
	ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
	ImGui::TextWrapped("Remember to save settings via the button at the top-right corner.");
	ImGui::PopStyleColor();

	// Resolution advice
	ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
	ImGui::TextWrapped("For maximum benefit, set your HMD resolution high. If using Virtual Desktop, use at least 'Godlike' (6K) resolution. Ultra Performance quality mode is recommended.");
	ImGui::PopStyleColor();

	const bool runtimeSupported = IsRuntimeSupported();
	if (!runtimeSupported) {
		settings.enabled = 0;
	}

	// ── Master Enable ──
	ImGui::Separator();
	if (!runtimeSupported) {
		ImGui::BeginDisabled();
	}
	ImGui::SliderInt("Enable", reinterpret_cast<int*>(&settings.enabled), 0, 1, toggleModes[settings.enabled]);
	if (!runtimeSupported) {
		ImGui::EndDisabled();
	}

	if ((settings.enabled != 0) != enabledAtBoot) {
		ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
		ImGui::TextWrapped(">>> RESTART REQUIRED for this change to take effect. <<<");
		ImGui::PopStyleColor();
	}

	if (enabledAtBoot) {
		ImGui::TextDisabled("Active: upscaling is forced to DLSS while enabled.");
	}

	if (!globals::game::isVR) {
		ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
		ImGui::Text("VR only. Non-VR / FSR support pending future contributors.");
		ImGui::PopStyleColor();
	}

	if (globals::game::isVR && !globals::features::upscaling.streamline.featureDLSS) {
		ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
		ImGui::Text("DLSS runtime not available. Enable is blocked.");
		ImGui::PopStyleColor();
	}

	// ── Upscale Settings ──
	ImGui::Separator();
	ImGui::Text("Upscale Settings");

	ImGui::SliderInt("Quality Mode", reinterpret_cast<int*>(&settings.qualityMode), 1, 4, qualityModes[settings.qualityMode]);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Lower = higher render resolution (better quality, higher cost).\nUltra Performance recommended for maximum VRAM savings.");

	ImGui::SliderFloat("Sharpness", &settings.sharpnessDLSS, 0.0f, 1.0f, "%.1f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Post-DLSS RCAS sharpening intensity. 0 = off, 1 = maximum.");

	// DLSS Model Preset
	{
		const char* currentPresetLabel = presets[settings.presetDLSS];
		if (ImGui::BeginCombo("DLSS Model Preset", currentPresetLabel)) {
			for (uint i = 0; i <= 5; ++i) {
				if (!IsPresetCompatibleWithMode(i))
					continue;
				bool selected = (settings.presetDLSS == i);
				if (ImGui::Selectable(presets[i], selected)) {
					settings.presetDLSS = i;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("NVIDIA model variant. Default is fine for most cases.");
	}

	ImGui::SliderInt("Streamline Logging", reinterpret_cast<int*>(&settings.streamlineLogLevel), 0, 2, logLevels[settings.streamlineLogLevel]);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Debug logging from Streamline SDK. Off for normal use.");

	// ── Post-DLSS Sharpening ──
	ImGui::Separator();
	ImGui::Text("Post-DLSS Sharpening");
	ImGui::SliderInt("Sharpen Mode", reinterpret_cast<int*>(&settings.sharpenMode), 0, 1, sharpenModes[settings.sharpenMode]);
	if (GetSharpenMode() == SharpenMode::kRCAS) {
		ImGui::TextDisabled("AMD FidelityFX RCAS (contrast-adaptive sharpening).");
	} else {
		ImGui::TextDisabled("No post-DLSS sharpening.");
	}

	if (globals::game::isVR) {
		// ── VR DLSS Mode ──
		ImGui::Separator();
		ImGui::Text("VR DLSS Mode");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Default: per-eye isolation, highest quality, 2 extra resource sets.\n"
				"Faster: SBS viewport trick, no extra resources, J/K presets unavailable.");

		uint prevMode = settings.dlssMode;
		ImGui::SliderInt("DLSS Mode", reinterpret_cast<int*>(&settings.dlssMode), 0, 1, dlssModes[settings.dlssMode]);

		if (settings.dlssMode != prevMode) {
			ClampPresetToMode();
		}

		switch (GetDlssMode()) {
		case DlssMode::kDefault:
			ImGui::TextDisabled("Per-eye isolation: 2 resource sets, 2 DLSS evaluates.");
			break;
		case DlssMode::kFaster:
			ImGui::TextDisabled("SBS viewport: 0 extra resources, 2 evaluates. J/K unavailable.");
			break;
		}

		// ── Background Stretch ──
		ImGui::Separator();
		ImGui::Text("Background Stretch");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How the low-res periphery is upscaled to fill the full eye.\nOnly affects the area outside the DLSS subrect.");
		ImGui::SliderInt("Stretch Mode", reinterpret_cast<int*>(&settings.stretchMode), 0, 2, stretchModes[settings.stretchMode]);
		switch (GetStretchMode()) {
		case StretchMode::kBilinear:
			ImGui::TextDisabled("Standard bilinear (clean upscale).");
			break;
		case StretchMode::kPoint:
			ImGui::TextDisabled("Nearest-neighbor: cheapest, VRS-like broadcast.");
			break;
		case StretchMode::kGaussianBlur:
			ImGui::TextDisabled("Gaussian blur: softens periphery for natural look.");
			break;
		}

		// ── Encode Textures ──
		ImGui::Separator();
		ImGui::Text("Encode Textures");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Optional input preprocessing for DLSS.\nGenerally not needed — disable all three for lower overhead.");
		ImGui::SliderInt("MV Dilation", reinterpret_cast<int*>(&settings.enableMVDilation), 0, 1, toggleModes[settings.enableMVDilation]);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("5x5 depth-aware motion vector dilation. Off = use raw game MVecs.");
		ImGui::SliderInt("Reactive Mask", reinterpret_cast<int*>(&settings.enableReactiveMask), 0, 1, toggleModes[settings.enableReactiveMask]);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("TAA-weighted reactive hint for DLSS.");
		ImGui::SliderInt("Transparency Mask", reinterpret_cast<int*>(&settings.enableTransparencyMask), 0, 1, toggleModes[settings.enableTransparencyMask]);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("NormalsWaterMask.z passthrough for transparency hint.");

		// ── Subrect Region ──
		ImGui::Separator();
		ImGui::Text("Subrect Region");
		ImGui::TextWrapped(
			"Drag in the preview below to select the region that gets full DLSS upscaling. "
			"The rest is cheaply stretched — saves significant DLSS cost.");
		ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
		ImGui::TextWrapped("If you also use VRS or Screenshot, set them to the same subrect preset for consistent results.");
		ImGui::PopStyleColor();

		auto renderer = globals::game::renderer;
		if (renderer) {
			auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
			auto* tex = static_cast<ID3D11Texture2D*>(main.texture);
			subrectController.DrawEditor(main.SRV, tex, 0.5f);
		} else {
			subrectController.DrawEditor(nullptr, nullptr, 0.5f);
		}
	}
}

void DlssEnhancerFeature::SaveSettings(json& o_json)
{
	o_json = settings;
	subrectController.SaveSettings(o_json);
}

void DlssEnhancerFeature::LoadSettings(json& o_json)
{
	settings = o_json;
	subrectController.LoadSettings(o_json);
	RestoreDefaultSettings();
}

void DlssEnhancerFeature::ClampSettings()
{
	settings.enabled = std::min(settings.enabled, 1u);
	settings.qualityMode = std::clamp(settings.qualityMode, 1u, 4u);
	settings.streamlineLogLevel = std::min(settings.streamlineLogLevel, 2u);
	settings.presetDLSS = std::min(settings.presetDLSS, 5u);
	settings.dlssMode = std::min(settings.dlssMode, 1u);
	settings.stretchMode = std::min(settings.stretchMode, 2u);
	settings.sharpenMode = std::min(settings.sharpenMode, 1u);
	settings.enableMVDilation = std::min(settings.enableMVDilation, 1u);
	settings.enableReactiveMask = std::min(settings.enableReactiveMask, 1u);
	settings.enableTransparencyMask = std::min(settings.enableTransparencyMask, 1u);
	settings.sharpnessDLSS = std::clamp(settings.sharpnessDLSS, 0.0f, 1.0f);

	ClampPresetToMode();
}

void DlssEnhancerFeature::RestoreDefaultSettings()
{
	ClampSettings();
}

void DlssEnhancerFeature::ClearShaderCache()
{
	DlssEnhancer::Core::ClearShaderCache();
}
