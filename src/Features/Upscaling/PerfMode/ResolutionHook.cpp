#include "../PerfMode.h"

#include <algorithm>
#include <cmath>

#include "../../../State.h"
#include "../../Upscaling.h"

// Quality mode -> render-scale resolution is supplied by the FFX SDK helper
// (same one Upscaling.cpp uses), avoiding a duplicate scale table here.
#include <FidelityFX/host/ffx_fsr3.h>

void PerfMode::InstallRenderTargetSizeHook()
{
	if (!globals::game::isVR)
		return;

	if (hookActive)
		return;

	// Eager capture — get real HMD resolution BEFORE installing hook
	auto* openvr = RE::BSOpenVR::GetSingleton();
	if (!openvr || !openvr->vrSystem) {
		logger::error("[PerfMode] BSOpenVR or vrSystem not available — hook NOT installed");
		return;
	}

	uint32_t w = 0, h = 0;
	openvr->vrSystem->GetRecommendedRenderTargetSize(&w, &h);
	if (w == 0 || h == 0) {
		logger::error("[PerfMode] GetRecommendedRenderTargetSize returned {}x{} — hook NOT installed", w, h);
		return;
	}

	displayEyeWidth = w;
	displayEyeHeight = h;

	// BSShaderRenderTargets::Create runs after SKSE feature settings load, so
	// upscaling.settings.qualityMode here reflects the user-saved value. We
	// snapshot the corresponding scale at install time and never re-read it —
	// the engine's RT allocations happen once, so a later UI quality change
	// can't shrink/grow RTs anyway. (Requires a game restart, same as DLSS.)
	//
	// Validate before division: a bad/corrupt JSON could put qualityMode
	// outside FFX's range, returning 0/inf/NaN; that would propagate to bogus
	// renderEye dimensions and silently mis-size every engine RT. Fail closed
	// — leave hookActive=false so the rest of PerfMode is dormant and DLSS
	// runs on dev's standard path.
	auto& upscaling = globals::features::upscaling;
	const uint32_t qualityModeRaw = upscaling.settings.qualityMode;
	uint32_t qualityMode = std::clamp<uint32_t>(qualityModeRaw, 0, 4);  // FfxFsr3QualityMode range
	float scale = ffxFsr3GetUpscaleRatioFromQualityMode(static_cast<FfxFsr3QualityMode>(qualityMode));
	if (!std::isfinite(scale) || scale <= 0.0f) {
		logger::error("[PerfMode] FFX returned invalid upscale ratio {} for qualityMode {} (raw {}); hook NOT installed", scale, qualityMode, qualityModeRaw);
		return;
	}

	// Explicit render scale overrides the preset-derived ratio. The preset itself
	// becomes inert; the upscaler mode is re-derived as the nearest preset ratio
	// so NGX's extent-vs-mode validation matches the RT allocation. Reserving
	// native allocations wins over both: the live scale then owns the render res.
	const float explicitScaleRaw = upscaling.settings.vrRenderScale;
	bool explicitScale = explicitScaleRaw > 0.0f;
	if (upscaling.settings.vrRenderScaleReserveNative) {
		scale = 1.0f;
		explicitScale = false;
	} else if (explicitScale) {
		const float clamped = std::clamp(explicitScaleRaw, Upscaling::kVRRenderScaleMin, Upscaling::kVRRenderScaleMax);
		if (clamped != explicitScaleRaw)
			logger::warn("[PerfMode] vrRenderScale {} out of range, clamped to {}", explicitScaleRaw, clamped);
		scale = 1.0f / clamped;
		qualityMode = Upscaling::NearestQualityModeForRatio(scale);
	}

	// Even dimensions: odd widths truncate the engine's half-res buffers
	// (underwater mask, volumetrics) and misalign SBS stereo math by a pixel.
	renderEyeWidth = std::max<uint32_t>(2, (uint32_t)(w / scale)) & ~1u;
	renderEyeHeight = std::max<uint32_t>(2, (uint32_t)(h / scale)) & ~1u;

	// DLSS validates render extents against the latched mode's NGX-supported
	// range; an explicit scale can land between preset ratios, so clamp the
	// allocation itself (never the dispatch extent: the RTs must match).
	if (explicitScale && upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS)
		upscaling.streamline.ClampToDLSSRenderRange(qualityMode, displayEyeWidth, displayEyeHeight, renderEyeWidth, renderEyeHeight);

	latchedQualityMode = qualityMode;
	explicitScaleLatched = explicitScale;
	logger::info("[PerfMode] Latched display {}x{} -> render {}x{} per eye (quality mode {}, {})",
		displayEyeWidth, displayEyeHeight, renderEyeWidth, renderEyeHeight, qualityMode,
		upscaling.settings.vrRenderScaleReserveNative ? "native reserved" :
		explicitScale                                 ? "explicit scale" :
		                                                "preset-derived");

	// Restart-required settings snapshot is latched by the render-target
	// creation hook, but keep this robust to call-order changes.
	globals::features::upscaling.bootSnapshot.LatchIfNeeded(globals::features::upscaling.settings);

	stl::write_vfunc<0x12, GetRenderTargetSize_Hook>(RE::VTABLE_BSOpenVR[0]);

	// Per-frame detours that used to live in Hooks.cpp. Both addresses are
	// already detoured by core/other features; stl::detour_thunk chains
	// (each new install wraps the prior thunk via its static func ptr).
	if (!setDirtyStatesHookInstalled) {
		stl::detour_thunk<BSGraphics_SetDirtyStates_Hook>(REL::RelocationID(75580, 77386));
		setDirtyStatesHookInstalled = true;
	}
	if (!updateViewPortHookInstalled) {
		stl::detour_thunk<BSGraphics_Renderer_UpdateViewPort_Hook>(REL::RelocationID(75455, 77240));
		updateViewPortHookInstalled = true;
	}

	hookActive = true;
}

void PerfMode::GetRenderTargetSize_Hook::thunk(RE::BSOpenVR* a_this, uint32_t* a_width, uint32_t* a_height)
{
	// Call original to get real HMD resolution
	func(a_this, a_width, a_height);

	auto& perfMode = globals::features::upscaling.perfMode;

	*a_width = perfMode.renderEyeWidth;
	*a_height = perfMode.renderEyeHeight;
}
