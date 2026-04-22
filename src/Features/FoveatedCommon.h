#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FoveatedCommon
{
	constexpr float kCenterAreaMin = 0.30f;
	constexpr float kCenterAreaMax = 1.0f;
	constexpr float kCenterFeather = 0.05f;
	constexpr float kCenterHorizontalScaleMin = 1.0f;
	constexpr float kCenterHorizontalScaleMax = 2.0f;
	constexpr float kMaskShapePower = 4.0f;
	constexpr int kThreadGroupSize = 8;

	struct DispatchBounds
	{
		int minX = 0;
		int minY = 0;
		int maxX = 0;
		int maxY = 0;
	};

	inline float ClampCenterArea(float value)
	{
		if (!std::isfinite(value))
			return kCenterAreaMax;
		return std::clamp(value, kCenterAreaMin, kCenterAreaMax);
	}

	inline float ClampCenterHorizontalScale(float value)
	{
		if (!std::isfinite(value))
			return 1.0f;
		return std::clamp(value, kCenterHorizontalScaleMin, kCenterHorizontalScaleMax);
	}

	inline int AlignDownToThreadGroup(int value)
	{
		return value & ~(kThreadGroupSize - 1);
	}

	inline int AlignUpToThreadGroup(int value)
	{
		return (value + (kThreadGroupSize - 1)) & ~(kThreadGroupSize - 1);
	}

	inline DispatchBounds BuildCenteredDispatchBounds(uint32_t eyeMinX, uint32_t eyeMaxX, uint32_t frameHeight, float centerScale, float centerOffsetX = 0.0f, float centerOffsetY = 0.0f, float centerFeather = kCenterFeather, float centerHorizontalScale = 1.0f)
	{
		DispatchBounds bounds{};

		const int eyeMinXInt = static_cast<int>(eyeMinX);
		const int eyeMaxXInt = static_cast<int>(eyeMaxX);
		const int frameHeightInt = static_cast<int>(frameHeight);
		if (eyeMaxXInt <= eyeMinXInt || frameHeightInt <= 0)
			return bounds;

		centerScale = ClampCenterArea(centerScale);
		centerHorizontalScale = ClampCenterHorizontalScale(centerHorizontalScale);
		centerFeather = std::isfinite(centerFeather) ? std::max(0.0f, centerFeather) : kCenterFeather;

		const float eyeWidth = static_cast<float>(eyeMaxX - eyeMinX);
		const float frameHeightF = static_cast<float>(frameHeight);
		const float centerXNormalized = std::clamp(0.5f + centerOffsetX, 0.0f, 1.0f);
		const float centerYNormalized = std::clamp(0.5f + centerOffsetY, 0.0f, 1.0f);
		const float centerX = static_cast<float>(eyeMinX) + eyeWidth * centerXNormalized;
		const float centerY = frameHeightF * centerYNormalized;
		// Match shader-space feather expansion:
		// normalizedFeather = centerFeather / min(radiusX, radiusY),
		// mask boundary = radius * (1 + normalizedFeather).
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

	inline float GetInscribedSuperellipseRectScale(float shapePower)
	{
		const float clampedPower = std::max(shapePower, 1.0f);
		return std::pow(0.5f, 1.0f / clampedPower);
	}

	inline DispatchBounds BuildCenteredInscribedMaskRect(uint32_t width, uint32_t height, float centerScale, float centerOffsetX = 0.0f, float centerOffsetY = 0.0f, float centerHorizontalScale = 1.0f)
	{
		DispatchBounds bounds{};
		if (width == 0 || height == 0)
			return bounds;

		const float clampedCenterScale = ClampCenterArea(centerScale);
		const float clampedCenterHorizontalScale = ClampCenterHorizontalScale(centerHorizontalScale);
		const float inscribedScale = GetInscribedSuperellipseRectScale(kMaskShapePower);
		const float inscribedHalfScaleX = clampedCenterScale * clampedCenterHorizontalScale * inscribedScale * 0.5f;
		const float inscribedHalfScaleY = clampedCenterScale * inscribedScale * 0.5f;
		const float widthF = static_cast<float>(width);
		const float heightF = static_cast<float>(height);
		const float centerXNormalized = std::clamp(0.5f + centerOffsetX, 0.0f, 1.0f);
		const float centerYNormalized = std::clamp(0.5f + centerOffsetY, 0.0f, 1.0f);

		bounds.minX = std::clamp(static_cast<int>(std::ceil((centerXNormalized - inscribedHalfScaleX) * widthF - 0.5f)), 0, static_cast<int>(width));
		bounds.maxX = std::clamp(static_cast<int>(std::floor((centerXNormalized + inscribedHalfScaleX) * widthF - 0.5f)) + 1, 0, static_cast<int>(width));
		bounds.minY = std::clamp(static_cast<int>(std::ceil((centerYNormalized - inscribedHalfScaleY) * heightF - 0.5f)), 0, static_cast<int>(height));
		bounds.maxY = std::clamp(static_cast<int>(std::floor((centerYNormalized + inscribedHalfScaleY) * heightF - 0.5f)) + 1, 0, static_cast<int>(height));
		return bounds;
	}
}
