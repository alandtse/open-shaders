#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FoveatedCommon
{
	constexpr float kCenterAreaMin = 0.45f;
	constexpr float kCenterAreaMax = 1.0f;
	constexpr float kCenterFeather = 0.05f;
	constexpr float kEllipseInscribedRectScale = 0.70710678f;
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
		return std::clamp(value, kCenterAreaMin, kCenterAreaMax);
	}

	inline int AlignDownToThreadGroup(int value)
	{
		return value & ~(kThreadGroupSize - 1);
	}

	inline int AlignUpToThreadGroup(int value)
	{
		return (value + (kThreadGroupSize - 1)) & ~(kThreadGroupSize - 1);
	}

	inline DispatchBounds BuildCenteredDispatchBounds(uint32_t eyeMinX, uint32_t eyeMaxX, uint32_t frameHeight, float centerScale, float centerOffsetX = 0.0f, float centerOffsetY = 0.0f)
	{
		DispatchBounds bounds{};

		const int eyeMinXInt = static_cast<int>(eyeMinX);
		const int eyeMaxXInt = static_cast<int>(eyeMaxX);
		const int frameHeightInt = static_cast<int>(frameHeight);
		if (eyeMaxXInt <= eyeMinXInt || frameHeightInt <= 0)
			return bounds;

		centerScale = ClampCenterArea(centerScale);

		const float eyeWidth = static_cast<float>(eyeMaxX - eyeMinX);
		const float frameHeightF = static_cast<float>(frameHeight);
		const float centerXNormalized = std::clamp(0.5f + centerOffsetX, 0.0f, 1.0f);
		const float centerYNormalized = std::clamp(0.5f + centerOffsetY, 0.0f, 1.0f);
		const float centerX = static_cast<float>(eyeMinX) + eyeWidth * centerXNormalized;
		const float centerY = frameHeightF * centerYNormalized;
		const float extentX = centerScale * eyeWidth * 0.5f + kCenterFeather * eyeWidth;
		const float extentY = centerScale * frameHeightF * 0.5f + kCenterFeather * frameHeightF;

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

	inline DispatchBounds BuildCenteredInscribedEllipseRect(uint32_t width, uint32_t height, float centerScale, float centerOffsetX = 0.0f, float centerOffsetY = 0.0f)
	{
		DispatchBounds bounds{};
		if (width == 0 || height == 0)
			return bounds;

		const float inscribedHalfScale = ClampCenterArea(centerScale) * kEllipseInscribedRectScale * 0.5f;
		const float widthF = static_cast<float>(width);
		const float heightF = static_cast<float>(height);
		const float centerXNormalized = std::clamp(0.5f + centerOffsetX, 0.0f, 1.0f);
		const float centerYNormalized = std::clamp(0.5f + centerOffsetY, 0.0f, 1.0f);

		bounds.minX = std::clamp(static_cast<int>(std::ceil((centerXNormalized - inscribedHalfScale) * widthF - 0.5f)), 0, static_cast<int>(width));
		bounds.maxX = std::clamp(static_cast<int>(std::floor((centerXNormalized + inscribedHalfScale) * widthF - 0.5f)) + 1, 0, static_cast<int>(width));
		bounds.minY = std::clamp(static_cast<int>(std::ceil((centerYNormalized - inscribedHalfScale) * heightF - 0.5f)), 0, static_cast<int>(height));
		bounds.maxY = std::clamp(static_cast<int>(std::floor((centerYNormalized + inscribedHalfScale) * heightF - 0.5f)) + 1, 0, static_cast<int>(height));
		return bounds;
	}
}
