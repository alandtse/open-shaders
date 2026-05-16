#pragma once

// ============================================================================
// DlssEnhancerFeature — VR DLSS enhancement feature (settings, GUI, persistence)
// ============================================================================
//
// Currently VR + DLSS only. Non-VR / FSR users will see this disabled.
// Future contributors: extend IsRuntimeSupported() and the Streamline path
// for flat-screen or FSR support.
//
//  Key advantage: Subrect DLSS — only the user-selected region gets full DLSS;
//  periphery is cheaply stretched. Halves (or more) the DLSS workload. Works
//  with VRS, Screenshot, and the upcoming lossless recording feature via the
//  shared Subrect module — use the same preset for best results.
//
// ============================================================================

#include "Feature.h"
#include "FeatureCategories.h"
#include "Utils/Subrect.h"

struct DlssEnhancerFeature : Feature
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

	struct Settings
	{
		uint enabled = 1;
		uint qualityMode = 4;  // Ultra Performance (maximum VRAM saving)
		uint streamlineLogLevel = 0;
		float sharpnessDLSS = 1.0f;
		uint presetDLSS = 0;
		uint dlssMode = (uint)DlssMode::kDefault;
		uint stretchMode = (uint)StretchMode::kGaussianBlur;
		uint sharpenMode = (uint)SharpenMode::kRCAS;
		uint enableMVDilation = 0;
		uint enableReactiveMask = 0;
		uint enableTransparencyMask = 0;
	};

	Settings settings;
	Util::Subrect::Controller subrectController;

	std::string GetName() override { return "DLSS Enhancer"; }
	// Matches features/DLSS Enhancer/Shaders/Features/DLSSENHANCER.ini —
	// FindFeatureByShortName + version-issue lookup are filename-keyed.
	std::string GetShortName() override { return "DLSSENHANCER"; }
	bool SupportsVR() override { return true; }
	bool IsCore() const override { return false; }
	std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"VR DLSS enhancer path with independent control surface.",
			{ "Own DLSS quality/preset/sharpness settings",
				"2 DLSS modes: Default / Faster",
				"Visual subrect cropping via drag editor",
				"Direct settings source for Streamline" }
		};
	}

	void DrawSettings() override;
	void SaveSettings(json& o_json) override;
	void LoadSettings(json& o_json) override;
	void RestoreDefaultSettings() override;
	void PostPostLoad() override;
	void ClearShaderCache() override;

	bool IsRuntimeSupported() const;
	bool IsActive() const;
	bool IsLoaded() const { return enabledAtBoot; }

	// Main enable: latched at boot, change requires restart
	void LatchEnabled() { enabledAtBoot = (settings.enabled != 0); }

	// Quality mode: latched at boot for resolution calculation
	void LatchQualityMode() { qualityModeAtBoot = std::clamp(settings.qualityMode, 1u, 4u); }
	uint GetQualityModeAtBoot() const { return qualityModeAtBoot; }

	/// Render-to-display scale denominator for a given quality mode.
	/// Quality=1.5, Balanced=1.7, Performance=2.0, UltraPerformance=3.0.
	static float GetRenderScaleForQuality(uint qualityMode);

	DlssMode GetDlssMode() const { return (DlssMode)std::min(settings.dlssMode, 1u); }
	StretchMode GetStretchMode() const { return (StretchMode)std::min(settings.stretchMode, 2u); }
	SharpenMode GetSharpenMode() const { return (SharpenMode)std::min(settings.sharpenMode, 1u); }

	// Active getters (snapshot of the current settings, for downstream consumers)
	uint GetActiveQualityMode() const { return std::clamp(settings.qualityMode, 1u, 4u); }
	uint GetActivePresetDLSS() const { return std::min(settings.presetDLSS, 5u); }
	float GetActiveSharpnessDLSS() const { return std::clamp(settings.sharpnessDLSS, 0.0f, 1.0f); }

	bool IsEncodeMVDilation() const { return settings.enableMVDilation != 0; }
	bool IsEncodeReactiveMask() const { return settings.enableReactiveMask != 0; }
	bool IsEncodeTransparencyMask() const { return settings.enableTransparencyMask != 0; }
	bool IsAnyEncodeEnabled() const { return IsEncodeMVDilation() || IsEncodeReactiveMask() || IsEncodeTransparencyMask(); }

private:
	bool enabledAtBoot = false;  // latched from settings.enabled at boot
	uint qualityModeAtBoot = 4;  // latched from settings.qualityMode at boot (default: UltraPerf)

	// Returns true if the current preset is compatible with the active DlssMode
	bool IsPresetCompatibleWithMode(uint presetIndex) const;
	// Clamp preset to a compatible value for the active mode
	void ClampPresetToMode();
	// Clamp all settings to valid ranges
	void ClampSettings();
};
