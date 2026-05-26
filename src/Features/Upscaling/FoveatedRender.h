#pragma once

// ============================================================================
// FoveatedRender — VR DLSS enhancement mode of Upscaling
// ============================================================================
//
// Foveated subrect-DLSS path: only the user-selected region gets full DLSS
// upscaling; the periphery is cheaply stretched via SubrectStretchCS. Halves
// (or more) the DLSS workload. Composes with VRS, Screenshot, and the lossless
// recording feature through the shared Util::Subrect module — use the same
// preset for consistent results across them.
//
// Architecturally a mode inside Upscaling (mirroring DLSSperf): a static-
// inline member, not a peer Feature. Settings that overlap with Upscaling's
// (quality mode, sharpness, DLSS preset, Streamline log level) read directly
// from `globals::features::upscaling.settings` rather than being duplicated.
// VR + DLSS only at present; non-VR / FSR extension is left to future work.
//
// ============================================================================

#include "../../Utils/Subrect.h"

struct FoveatedRender
{
public:
	// DLSS execution mode for VR
	enum class DlssMode : uint
	{
		kDefault = 0,  // Per-eye isolation: 2 extra resource sets, 2 evaluates. Supports F/J/K/L/M.
		kFaster = 1,   // SBS viewport: tell SL to read subrect from SBS directly, no extra resources, 2 evaluates. J/K incompatible, only L/M/F.
	};

	// Stretch algorithm for DRS → full-eye background (used by SubrectStretchCS shader)
	enum class StretchMode : uint
	{
		kBilinear = 0,      // Default bilinear sampling (clean upscale)
		kPoint = 1,         // Nearest-neighbor / point (cheapest, VRS-like broadcast)
		kGaussianBlur = 2,  // 3x3 Gaussian blur (soft periphery)
	};

	// Sharpening algorithm selection (extensible)
	enum class SharpenMode : uint
	{
		kRCAS = 0,  // AMD FidelityFX RCAS (current default)
		kNone = 1,  // No post-DLSS sharpening
	};

	// FoveatedRender-specific settings. Quality mode / sharpness / DLSS preset /
	// Streamline log level live on Upscaling::Settings and are read through
	// the accessors below — do not duplicate them here.
	struct Settings
	{
		uint enabled = 1;
		uint dlssMode = (uint)DlssMode::kDefault;
		uint stretchMode = (uint)StretchMode::kGaussianBlur;
		uint sharpenMode = (uint)SharpenMode::kRCAS;
		uint enableMVDilation = 0;
		uint enableReactiveMask = 0;
		uint enableTransparencyMask = 0;
	};

	Settings settings;
	Util::Subrect::Controller subrectController;

	// Called from Upscaling::DrawSettings under a TreeNode.
	void DrawSettings();
	// Called from Upscaling::SaveSettings / LoadSettings to round-trip JSON.
	void SaveSettings(json& o_json);
	void LoadSettings(const json& o_json);
	void RestoreDefaultSettings();
	void ClearShaderCache();
	// Called from Upscaling::PostPostLoad to seed subrect presets.
	void PostPostLoad();

	bool IsRuntimeSupported() const;
	bool IsActive() const;
	bool IsLoaded() const { return enabledAtBoot; }

	// Main enable: latched at boot, change requires restart
	void LatchEnabled() { enabledAtBoot = (settings.enabled != 0); }

	// Quality mode reads through Upscaling::Settings — latch the boot value so
	// downstream RT allocations stay coherent if the user moves the slider.
	void LatchQualityMode();
	uint GetQualityModeAtBoot() const { return qualityModeAtBoot; }

	/// Render-to-display scale denominator for a given quality mode.
	/// Quality=1.5, Balanced=1.7, Performance=2.0, UltraPerformance=3.0.
	static float GetRenderScaleForQuality(uint qualityMode);

	DlssMode GetDlssMode() const { return (DlssMode)std::min(settings.dlssMode, 1u); }
	StretchMode GetStretchMode() const { return (StretchMode)std::min(settings.stretchMode, 2u); }
	SharpenMode GetSharpenMode() const { return (SharpenMode)std::min(settings.sharpenMode, 1u); }

	// Active getters: clamp + route shared fields through Upscaling::Settings.
	uint GetActiveQualityMode() const;
	uint GetActivePresetDLSS() const;
	float GetActiveSharpnessDLSS() const;

	bool IsEncodeMVDilation() const { return settings.enableMVDilation != 0; }
	bool IsEncodeReactiveMask() const { return settings.enableReactiveMask != 0; }
	bool IsEncodeTransparencyMask() const { return settings.enableTransparencyMask != 0; }
	bool IsAnyEncodeEnabled() const { return IsEncodeMVDilation() || IsEncodeReactiveMask() || IsEncodeTransparencyMask(); }

private:
	bool enabledAtBoot = false;  // latched from settings.enabled at boot
	uint qualityModeAtBoot = 4;  // latched from Upscaling::Settings::qualityMode at boot

	bool IsPresetCompatibleWithMode(uint presetIndex) const;
	void ClampPresetToMode();
	void ClampSettings();
};
