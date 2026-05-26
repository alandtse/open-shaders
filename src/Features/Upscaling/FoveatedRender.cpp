#include "FoveatedRender.h"

#include "../../Globals.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "FoveatedRender/Core.h"

#include <algorithm>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoveatedRender::Settings,
	enabled,
	dlssMode,
	stretchMode,
	sharpenMode,
	enableMVDilation,
	enableReactiveMask,
	enableTransparencyMask);

// ============================================================================
// Lifecycle
// ============================================================================

void FoveatedRender::PostPostLoad()
{
	// Opt into PR-1's stereo extension so the controller tracks a separate
	// right-eye UV (HMD nose-side overlap symmetry).
	subrectController.SetStereoEnabled(true);

	// Seed sensible foveal presets. Empty-case only — user edits persist.
	subrectController.SeedDefaultPresets({
		{ .name = "Full Eye", .uv = { 0.0f, 0.0f, 1.0f, 1.0f } },
		{ .name = "Center 75%", .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
		{ .name = "Center 50%", .uv = { 0.25f, 0.25f, 0.5f, 0.5f } },
	});
}

void FoveatedRender::ClearShaderCache()
{
	FoveatedRenderImpl::Core::ClearShaderCache();
}

// ============================================================================
// Settings I/O — driven from Upscaling::Save/LoadSettings under a nested key
// ============================================================================

void FoveatedRender::SaveSettings(json& o_json)
{
	o_json = settings;
	subrectController.SaveSettings(o_json);
}

void FoveatedRender::LoadSettings(const json& o_json)
{
	settings = o_json;
	subrectController.LoadSettings(const_cast<json&>(o_json));
	ClampSettings();
}

void FoveatedRender::RestoreDefaultSettings()
{
	settings = {};
	ClampSettings();
}

void FoveatedRender::ClampSettings()
{
	settings.enabled = std::min(settings.enabled, 1u);
	settings.dlssMode = std::min(settings.dlssMode, 1u);
	settings.stretchMode = std::min(settings.stretchMode, 2u);
	settings.sharpenMode = std::min(settings.sharpenMode, 1u);
	settings.enableMVDilation = std::min(settings.enableMVDilation, 1u);
	settings.enableReactiveMask = std::min(settings.enableReactiveMask, 1u);
	settings.enableTransparencyMask = std::min(settings.enableTransparencyMask, 1u);
	// Preset clamping reads from Upscaling::Settings now.
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	sharedPreset = std::min(sharedPreset, 5u);
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// Activation + accessors
// ============================================================================

bool FoveatedRender::IsActive() const
{
	return enabledAtBoot && IsRuntimeSupported();
}

bool FoveatedRender::IsRuntimeSupported() const
{
	return globals::game::isVR && globals::features::upscaling.streamline.featureDLSS;
}

void FoveatedRender::LatchQualityMode()
{
	qualityModeAtBoot = std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActiveQualityMode() const
{
	return std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActivePresetDLSS() const
{
	return std::min(globals::features::upscaling.settings.presetDLSS, 5u);
}

float FoveatedRender::GetActiveSharpnessDLSS() const
{
	return std::clamp(globals::features::upscaling.settings.sharpnessDLSS, 0.0f, 1.0f);
}

float FoveatedRender::GetRenderScaleForQuality(uint qualityMode)
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
		return 3.0f;
	}
}

bool FoveatedRender::IsPresetCompatibleWithMode(uint presetIndex) const
{
	// Preset indices: 0=Default, 1=J, 2=K, 3=L, 4=M, 5=F
	// Faster mode: J(1) and K(2) are incompatible.
	if (GetDlssMode() == DlssMode::kFaster) {
		return presetIndex != 1 && presetIndex != 2;
	}
	return true;
}

void FoveatedRender::ClampPresetToMode()
{
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// UI — FoveatedRender-specific knobs only. Quality / sharpness / preset /
// Streamline log level live on Upscaling's panel and apply to both DLSS paths.
// Called from Upscaling::DrawSettings inside a TreeNode.
// ============================================================================

void FoveatedRender::DrawSettings()
{
	static const char* toggleModes[] = { "Off", "On" };
	static const char* dlssModes[] = { "Default", "Faster" };
	static const char* stretchModes[] = { "Bilinear", "Point (VRS-like)", "Gaussian Blur" };
	static const char* sharpenModes[] = { "RCAS", "None" };

	ClampSettings();

	ImGui::TextWrapped(
		"Foveated subrect DLSS: only the user-selected region gets full DLSS upscaling, "
		"the periphery is cheaply stretched. Significant DLSS cost reduction at the cost "
		"of peripheral sharpness. VR + DLSS only.");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Subrect DLSS only upscales the region you select; the periphery is cheaply stretched.\n"
			"Composes with VRS, Screenshot, and lossless recording via the shared Subrect module.\n"
			"Quality / Sharpness / DLSS Preset / Streamline log level are shared with the standard DLSS path above.");
	}

	const bool runtimeSupported = IsRuntimeSupported();
	if (!runtimeSupported) {
		settings.enabled = 0;
	}

	ImGui::Separator();
	if (!runtimeSupported)
		ImGui::BeginDisabled();
	ImGui::SliderInt("Enable", reinterpret_cast<int*>(&settings.enabled), 0, 1, toggleModes[settings.enabled]);
	if (!runtimeSupported)
		ImGui::EndDisabled();

	if ((settings.enabled != 0) != enabledAtBoot) {
		Util::Text::RestartNeeded("Pending restart: FoveatedRender will %s on next launch.",
			settings.enabled ? "enable" : "disable");
	}

	if (enabledAtBoot) {
		ImGui::TextDisabled("Active: upscaling is forced to DLSS while enabled.");
	}

	if (!globals::game::isVR) {
		Util::Text::Warning("VR only. Non-VR / FSR support pending future contributors.");
	}
	if (globals::game::isVR && !globals::features::upscaling.streamline.featureDLSS) {
		Util::Text::Warning("DLSS runtime not available. Enable is blocked.");
	}

	// ── VR-only knobs ──
	if (globals::game::isVR) {
		ImGui::Separator();
		ImGui::Text("VR DLSS Mode");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Default: per-eye isolation, highest quality, 2 extra resource sets.\n"
				"Faster: SBS viewport trick, no extra resources, J/K presets unavailable.");
		}

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

		ImGui::Separator();
		ImGui::Text("Background Stretch");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How the low-res periphery is upscaled to fill the full eye.\nOnly affects the area outside the DLSS subrect.");
		}
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

		ImGui::Separator();
		ImGui::Text("Post-DLSS Sharpening");
		ImGui::SliderInt("Sharpen Mode", reinterpret_cast<int*>(&settings.sharpenMode), 0, 1, sharpenModes[settings.sharpenMode]);
		if (GetSharpenMode() == SharpenMode::kRCAS) {
			ImGui::TextDisabled("AMD FidelityFX RCAS (contrast-adaptive sharpening).");
		} else {
			ImGui::TextDisabled("No post-DLSS sharpening.");
		}

		ImGui::Separator();
		ImGui::Text("Encode Textures");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Optional input preprocessing for DLSS.\nGenerally not needed — disable all three for lower overhead.");
		}
		ImGui::SliderInt("MV Dilation", reinterpret_cast<int*>(&settings.enableMVDilation), 0, 1, toggleModes[settings.enableMVDilation]);
		ImGui::SliderInt("Reactive Mask", reinterpret_cast<int*>(&settings.enableReactiveMask), 0, 1, toggleModes[settings.enableReactiveMask]);
		ImGui::SliderInt("Transparency Mask", reinterpret_cast<int*>(&settings.enableTransparencyMask), 0, 1, toggleModes[settings.enableTransparencyMask]);

		ImGui::Separator();
		ImGui::Text("Subrect Region");
		ImGui::TextWrapped(
			"Drag in the preview below to select the region that gets full DLSS upscaling. "
			"The rest is cheaply stretched — saves significant DLSS cost.");
		Util::Text::Warning("If you also use VRS or Screenshot, set them to the same subrect preset for consistent results.");

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
