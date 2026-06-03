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
	peripheryBlurRadius,
	debugVisualize,
	peripheryAAMode,
	peripheryTemporalAlpha,
	subrectBlendMode,
	subrectFeatherWidth,
	subrectDitherStrength);

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
	// happens.
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
	settings.peripheryAAMode = std::min(settings.peripheryAAMode, 1u);
	settings.subrectBlendMode = std::min(settings.subrectBlendMode, 2u);
	settings.peripheryBlurRadius = std::clamp(settings.peripheryBlurRadius, 0.5f, 4.0f);
	settings.peripheryTemporalAlpha = std::clamp(settings.peripheryTemporalAlpha, 0.05f, 0.5f);
	settings.subrectFeatherWidth = std::clamp(settings.subrectFeatherWidth, 2.0f, 128.0f);
	settings.subrectDitherStrength = std::clamp(settings.subrectDitherStrength, 0.0f, 2.0f);
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
	// Activity requires DLSS to be the *selected* upscaler, not merely available.
	// IsRuntimeSupported() (availability) only gates the Enable checkbox; if we
	// keyed activity off it too, foveation — and its SSR consumer — would run
	// while the user has TAA/FSR/None selected. It also fails closed under
	// RenderDoc, whose DX12-interop swapchain disables DLSS: GetUpscaleMethod()
	// then returns a non-DLSS method, so the route stands down instead of
	// dereferencing uninitialised DLSS resources.
	return enabledAtBoot && IsRuntimeSupported() &&
	       globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS;
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
		if (globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS)
			Util::Text::WrappedInfo("Active: foveated subrect DLSS is running.");
		else
			Util::Text::Warning("Standing by: only runs while the Upscaling Method is DLSS. Inactive right now.");
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
	static const char* stretchModes[] = { "Bilinear", "Point", "Gaussian Blur" };

	ClampSettings();

	Util::Text::WrappedInfo("Quality, Sharpness, and DLSS Preset are on the main Upscaling panel — changes there apply to foveated rendering too.");

	// ── VR-only knobs ──
	if (globals::game::isVR) {
		ImGui::Separator();
		ImGui::Text("VR DLSS Mode");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Default — highest quality. Each eye gets its own isolated copy of color/depth/motion\n"
				"vectors so DLSS can't sample across the stereo midline. 5 copies per eye per frame.\n"
				"All DLSS presets supported. Best for screenshots or when Faster shows edge artifacts.\n"
				"\n"
				"Faster — lower overhead. DLSS reads directly from the frame buffer using a viewport\n"
				"offset instead of isolating each eye. 1 snapshot + 2 mask clears per frame.\n"
				"DLSS may sample 1-2 pixels from the neighboring eye near the stereo center — usually\n"
				"invisible in motion. Presets J and K are incompatible and auto-clamp to L.");
		}

		static const char* dlssModes[] = { "Default", "Faster" };
		uint prevMode = settings.dlssMode;
		ImGui::SliderInt("DLSS Mode", reinterpret_cast<int*>(&settings.dlssMode), 0, 1, dlssModes[std::min(settings.dlssMode, 1u)]);
		if (settings.dlssMode != prevMode) {
			const uint prevPreset = globals::features::upscaling.settings.presetDLSS;
			ClampPresetToMode();
			if (globals::features::upscaling.settings.presetDLSS != prevPreset) {
				logger::info("[FOVEATED] DLSS preset clamped from {} to {} after mode switch (J/K incompatible with Faster)",
					prevPreset, globals::features::upscaling.settings.presetDLSS);
			}
		}
		switch (GetDlssMode()) {
		case DlssMode::kDefault:
			ImGui::TextWrapped("Per-eye isolation: 5 copies per frame, 2 DLSS evaluates. All presets.");
			break;
		case DlssMode::kFaster:
			ImGui::TextWrapped("Viewport offset: 1 snapshot, 2 mask clears, 2 DLSS evaluates. Presets J/K unavailable.");
			break;
		default:
			break;
		}

		ImGui::Separator();
		ImGui::Text("Periphery Rendering");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"The area outside your selected subrect is filled cheaply rather than running DLSS.\n"
				"These settings control how that cheap fill looks and whether it flickers.\n"
				"\n"
				"Stretch method: how pixels outside the subrect are reconstructed from the lower-res\n"
				"render buffer. Does not affect the DLSS subrect region at all.\n"
				"\n"
				"Periphery AA: reduces temporal flicker in the stretched area using motion-compensated\n"
				"history blending. Independent of the DLSS subrect.\n"
				"\n"
				"Edge Blend: controls how the DLSS subrect edge meets the stretched periphery.\n"
				"Hard Copy leaves a sharp seam; Feather/Dither soften it. Only affects the boundary.");
		}

		ImGui::SliderInt("Stretch", reinterpret_cast<int*>(&settings.stretchMode), 0, 2, stretchModes[settings.stretchMode]);
		switch (GetStretchMode()) {
		case StretchMode::kBilinear:
			ImGui::TextWrapped("Bilinear: smooth upscale of the render buffer. Looks soft but clean.");
			break;
		case StretchMode::kPoint:
			ImGui::TextWrapped("Point: cheapest, visibly pixelated. Good for benchmarking foveated savings.");
			break;
		case StretchMode::kGaussianBlur:
			ImGui::TextWrapped("Gaussian: blurs the periphery further into soft focus. Good default for foveated use.");
			ImGui::SliderFloat("Blur Radius", &settings.peripheryBlurRadius, 0.5f, 4.0f, "%.1f px");
			break;
		}

		{
			static const char* peripheryAAModes[] = { "None", "Temporal Smooth" };
			ImGui::SliderInt("Periphery AA", reinterpret_cast<int*>(&settings.peripheryAAMode), 0, 1, peripheryAAModes[settings.peripheryAAMode]);
		}
		if (GetPeripheryAAMode() == PeripheryAAMode::kTemporalSmooth) {
			ImGui::TextWrapped("Blends the stretched periphery with motion-reprojected history to reduce flicker.");
			ImGui::SliderFloat("Smoothing", &settings.peripheryTemporalAlpha, 0.05f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Lower = more temporal history (smoother but may ghost). Higher = more responsive.");
			}
		}

		{
			static const char* blendModes[] = { "Hard Copy", "Feather", "Dither" };
			ImGui::SliderInt("Edge Blend", reinterpret_cast<int*>(&settings.subrectBlendMode), 0, 2, blendModes[std::min(settings.subrectBlendMode, 2u)]);
		}
		switch (GetSubrectBlendMode()) {
		case SubrectBlendMode::kHardCopy:
			ImGui::TextWrapped("Sharp seam at the subrect boundary. Lowest cost.");
			break;
		case SubrectBlendMode::kFeather:
			ImGui::TextWrapped("Smoothstep fade over N pixels at the boundary. Hides the seam.");
			ImGui::SliderFloat("Feather Width", &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			break;
		case SubrectBlendMode::kDither:
			ImGui::TextWrapped("Noise-dithered fade — more natural-looking than feather at large subrects.");
			ImGui::SliderFloat("Band Width", &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			ImGui::SliderFloat("Noise Amount", &settings.subrectDitherStrength, 0.0f, 2.0f, "%.2f");
			break;
		}

		ImGui::Separator();
		ImGui::Text("Subrect Region");
		ImGui::TextWrapped(
			"Drag in the preview below to select the region that gets full DLSS upscaling. "
			"The rest is cheaply stretched — saves significant DLSS cost.");
		Util::Text::WrappedInfo("Screenshot has its own subrect; align them only if you want pixel-matched captures.");

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
