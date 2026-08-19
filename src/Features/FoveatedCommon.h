#pragma once

#include <algorithm>
#include <cmath>

// Shared CPU-side helpers for VR foveated shader detail; GPU mirror in FoveatedMask.hlsli, with
// GetShaderMode's 0/1/2 encoding as the contract. Only SSR-consumed pieces exist (compute helpers omitted).
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

	struct DispatchBounds
	{
		int minX = 0;
		int minY = 0;
		int maxX = 0;
		int maxY = 0;
	};

	constexpr int kThreadGroupSize = 8;

	inline int AlignDownToThreadGroup(int value)
	{
		return value & ~(kThreadGroupSize - 1);
	}

	inline int AlignUpToThreadGroup(int value)
	{
		return (value + (kThreadGroupSize - 1)) & ~(kThreadGroupSize - 1);
	}

	inline DispatchBounds BuildCenteredDispatchBounds(
		uint32_t eyeMinX,
		uint32_t eyeMaxX,
		uint32_t frameHeight,
		float centerScale,
		float centerOffsetX = 0.0f,
		float centerOffsetY = 0.0f,
		float centerFeather = kCenterFeather,
		float centerHorizontalScale = 1.0f)
	{
		DispatchBounds bounds{};

		const int eyeMinXInt = static_cast<int>(eyeMinX);
		const int eyeMaxXInt = static_cast<int>(eyeMaxX);
		const int frameHeightInt = static_cast<int>(frameHeight);
		if (eyeMaxXInt <= eyeMinXInt || frameHeightInt <= 0)
			return bounds;

		centerScale = ClampCenterScale(centerScale);
		centerHorizontalScale = ClampCenterHorizontalScale(centerHorizontalScale);
		centerFeather = std::isfinite(centerFeather) ? std::max(0.0f, centerFeather) : kCenterFeather;
		centerOffsetX = std::isfinite(centerOffsetX) ? centerOffsetX : 0.0f;
		centerOffsetY = std::isfinite(centerOffsetY) ? centerOffsetY : 0.0f;

		const float eyeWidth = static_cast<float>(eyeMaxX - eyeMinX);
		const float frameHeightF = static_cast<float>(frameHeight);
		const float centerXNormalized = std::clamp(0.5f + centerOffsetX, 0.0f, 1.0f);
		const float centerYNormalized = std::clamp(0.5f + centerOffsetY, 0.0f, 1.0f);
		const float centerX = static_cast<float>(eyeMinX) + eyeWidth * centerXNormalized;
		const float centerY = frameHeightF * centerYNormalized;

		const float radiusX = centerScale * centerHorizontalScale * 0.5f;
		const float radiusY = centerScale * 0.5f;
		const float baseRadius = std::max(std::min(radiusX, radiusY), 1e-4f);
		const float normalizedFeather = centerFeather / baseRadius;
		const float extentX = radiusX * (1.0f + normalizedFeather) * eyeWidth;
		const float extentY = radiusY * (1.0f + normalizedFeather) * frameHeightF;

		int minX = static_cast<int>(centerX - extentX);
		int maxX = static_cast<int>(centerX + extentX + 0.9999f);
		int minY = static_cast<int>(centerY - extentY);
		int maxY = static_cast<int>(centerY + extentY + 0.9999f);

		minX = std::max(minX, eyeMinXInt);
		maxX = std::min(maxX, eyeMaxXInt);
		minY = std::max(minY, 0);
		maxY = std::min(maxY, frameHeightInt);

		minX = std::max(AlignDownToThreadGroup(minX - eyeMinXInt) + eyeMinXInt, eyeMinXInt);
		maxX = std::min(AlignUpToThreadGroup(maxX - eyeMinXInt) + eyeMinXInt, eyeMaxXInt);
		minY = std::max(AlignDownToThreadGroup(minY), 0);
		maxY = std::min(AlignUpToThreadGroup(maxY), frameHeightInt);

		if (maxX <= minX || maxY <= minY)
			return DispatchBounds{};

		bounds.minX = minX;
		bounds.minY = minY;
		bounds.maxX = maxX;
		bounds.maxY = maxY;
		return bounds;
	}
}
