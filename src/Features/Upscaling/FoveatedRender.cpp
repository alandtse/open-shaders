#include "FoveatedRender.h"

#include "../../Globals.h"
#include "../../Utils/Subrect.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "FoveatedRender/Core.h"

#include <algorithm>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoveatedRender::Settings,
	enabled,
	dlssMode,
	stretchMode,
	debugVisualize);

// ============================================================================
// Lifecycle
// ============================================================================

void FoveatedRender::PostPostLoad()
{
	// Opt into PR-1's stereo extension so the controller tracks a separate
	// right-eye UV (HMD nose-side overlap symmetry).
	subrectController.SetStereoEnabled(true);

	// Seed sensible foveal presets. Empty-case only — user edits persist.
	// "Center N%" presets are symmetric per eye (no rightUV → auto-mirror, which
	// for centered UVs produces an identical right-eye UV). "Nasal Convergence"
	// is asymmetric: left eye biased toward its right edge, right eye biased
	// toward its left edge — both targeting the nose-side region where HMD
	// binocular fusion is strongest, so DLSS reconstruction lands in the actual
	// stereo overlap zone rather than diverging left/right fields.
	subrectController.SeedDefaultPresets({
		{ .name = "Full Eye", .uv = { 0.0f, 0.0f, 1.0f, 1.0f } },
		{ .name = "Center 75%", .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
		{ .name = "Center 50%", .uv = { 0.25f, 0.25f, 0.5f, 0.5f } },
		{ .name = "Nasal Convergence 50%",
			.uv = { 0.5f, 0.25f, 0.5f, 0.5f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.25f, 0.5f, 0.5f } },
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
	// Util::Subrect::Controller::LoadSettings takes `const json&` (Subrect.h:68)
	// so no const_cast is needed — keeping it would imply mutation that never
	// happens. (Copilot + CodeRabbit on PR #44.)
	subrectController.LoadSettings(o_json);
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
	settings.debugVisualize = std::min(settings.debugVisualize, 1u);
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
	return Upscaling::GetQualityModeRatio(qualityMode);
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

void FoveatedRender::DrawEnable()
{
	ClampSettings();

	ImGui::TextWrapped(
		"Foveated subrect DLSS: only the user-selected region gets full DLSS upscaling, "
		"the periphery is cheaply stretched. Significant DLSS cost reduction at the cost "
		"of peripheral sharpness. VR + DLSS only.");

	const bool runtimeSupported = IsRuntimeSupported();
	if (!runtimeSupported) {
		settings.enabled = 0;
	}

	if (!runtimeSupported)
		ImGui::BeginDisabled();
	bool enabledBool = settings.enabled != 0;
	if (ImGui::Checkbox("Enable Foveated DLSS", &enabledBool))
		settings.enabled = enabledBool ? 1u : 0u;
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
}

void FoveatedRender::DrawSettings()
{
	static const char* dlssModes[] = { "Default", "Faster" };
	static const char* stretchModes[] = { "Bilinear", "Point", "Gaussian Blur" };

	ClampSettings();

	ImGui::TextDisabled("Quality / Sharpness / DLSS Preset / Streamline log level are shared with the standard DLSS path above.");

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
			ImGui::Text(
				"How the cheap periphery is reconstructed to fill the area outside the DLSS subrect.\n"
				"This is the cost-saving step — DLSS only runs on the subrect, the rest is filled by\n"
				"this cheaper pass. Only affects pixels outside your selected region.");
		}
		ImGui::SliderInt("Stretch Mode", reinterpret_cast<int*>(&settings.stretchMode), 0, 2, stretchModes[settings.stretchMode]);
		switch (GetStretchMode()) {
		case StretchMode::kBilinear:
			ImGui::TextDisabled("Bilinear: clean linear upscale. Looks like a soft DLSS-Performance result.");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Use when: you want the periphery to look like a sensible low-quality reconstruction,\n"
					"close to how DLSS-Performance would look. Default-ish choice.\n"
					"\n"
					"Visual artifact: typical bilinear softness — fine geometry in the periphery looks\n"
					"slightly out of focus but not visibly stretched.");
			}
			break;
		case StretchMode::kPoint:
			ImGui::TextDisabled("Point (nearest-neighbor): cheapest. Visibly pixelated periphery.");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Use when: you want the smallest possible cost in the periphery and don't mind\n"
					"obvious pixelation outside your gaze region. Useful for benchmarking the upper\n"
					"bound of foveated savings.\n"
					"\n"
					"Visual artifact: chunky pixel blocks in the periphery, very visible if you look\n"
					"away from the subrect center.");
			}
			break;
		case StretchMode::kGaussianBlur:
			ImGui::TextDisabled("Gaussian blur: softens periphery further. Hides upscale artifacts behind blur.");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Use when: you want the periphery to fall away into soft focus — closer to how\n"
					"natural human peripheral vision feels. Good default for actual foveated use.\n"
					"\n"
					"Visual artifact: noticeable blur in the periphery. If your subrect is large this\n"
					"is barely visible; if small, the blur is the dominant visual signal.");
			}
			break;
		}

		ImGui::Separator();
		ImGui::Text("Subrect Region");
		ImGui::TextWrapped(
			"Drag in the preview below to select the region that gets full DLSS upscaling. "
			"The rest is cheaply stretched — saves significant DLSS cost.");
		ImGui::TextDisabled("Screenshot has its own subrect; align them only if you want pixel-matched captures.");

		bool debugBool = settings.debugVisualize != 0;
		if (ImGui::Checkbox("Visualize regions", &debugBool))
			settings.debugVisualize = debugBool ? 1u : 0u;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Diagnostic: tint the cheap-stretched periphery red so the DLSS-reconstructed\n"
				"subrect (un-tinted) pops visually in-game. Lets you confirm at a glance where\n"
				"DLSS is actually running vs where the cheap stretch is filling. No perf impact;\n"
				"runtime toggle, no restart needed.");
		}

		// Preview off kVR_FRAMEBUFFER (the final composed SBS image the headset
		// sees) rather than kMAIN. kMAIN is mid-pipeline and carries non-1
		// alpha where Skyrim composited UI plates, so even with the opaque
		// blend callback you see the menu mask outline instead of the rendered
		// world. ScreenshotFeature picks the same RT for the same reason
		// (ScreenshotFeature.cpp:243). Foveated is VR-only so kVR_FRAMEBUFFER
		// is always populated when we get here.
		auto renderer = globals::game::renderer;
		if (renderer) {
			auto& fb = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kVR_FRAMEBUFFER];
			auto* tex = static_cast<ID3D11Texture2D*>(fb.texture);
			subrectController.DrawEditor(fb.SRV, tex, 0.5f, 0.0f, Util::Subrect::OpaquePreviewBlendCallback);
		} else {
			subrectController.DrawEditor(nullptr, nullptr, 0.5f);
		}
	}
}
