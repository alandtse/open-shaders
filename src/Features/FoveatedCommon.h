#pragma once

#include <algorithm>
#include <cmath>

// Shared CPU-side helpers for VR foveated shader detail; GPU mirror in
// package/Shaders/Common/FoveatedMask.hlsli, with GetShaderMode's 0/1/2 encoding as the contract.
// Only the SSR-consumed pieces exist; compute-dispatch helpers are omitted until a CS effect needs them.
namespace FoveatedCommon
{
	constexpr float kCenterScaleMin = 0.25f;
	constexpr float kCenterScaleMax = 1.0f;
	constexpr float kCenterFeather = 0.05f;
	constexpr float kCenterHorizontalScaleMin = 1.0f;
	constexpr float kCenterHorizontalScaleMax = 2.0f;
	constexpr float kFullCoverageThreshold = 0.999f;

	enum class DetailMode
	{
		Off,
		Feathered,
		HardCutoff
	};

	constexpr DetailMode GetDetailMode(bool a_enabled, bool a_hardCutoff)
	{
		if (!a_enabled)
			return DetailMode::Off;
		return a_hardCutoff ? DetailMode::HardCutoff : DetailMode::Feathered;
	}

	// 0 = off, 1 = feathered, 2 = hard cutoff. Must match the
	// FOVEATED_SHADER_DETAIL_MODE_* constants in FoveatedShaderDetail.hlsli.
	constexpr float GetShaderMode(DetailMode a_mode)
	{
		switch (a_mode) {
		case DetailMode::Feathered:
			return 1.0f;
		case DetailMode::HardCutoff:
			return 2.0f;
		case DetailMode::Off:
		default:
			return 0.0f;
		}
	}

	inline float ClampCenterScale(float a_value)
	{
		if (!std::isfinite(a_value))
			return kCenterScaleMax;
		return std::clamp(a_value, kCenterScaleMin, kCenterScaleMax);
	}

	// A near-full center means foveation does nothing useful — callers skip the
	// per-pixel mask in that case to avoid paying its cost for no saving.
	inline bool IsActiveCoverage(float a_centerScale)
	{
		return ClampCenterScale(a_centerScale) < kFullCoverageThreshold;
	}

	inline float ClampCenterHorizontalScale(float a_value)
	{
		if (!std::isfinite(a_value))
			return 1.0f;
		return std::clamp(a_value, kCenterHorizontalScaleMin, kCenterHorizontalScaleMax);
	}
}
