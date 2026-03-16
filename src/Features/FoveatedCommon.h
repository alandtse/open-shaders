#pragma once

#include <algorithm>

namespace FoveatedCommon
{
	constexpr float kCenterAreaMin = 0.25f;
	constexpr float kCenterAreaMax = 1.0f;
	constexpr float kCenterFeather = 0.05f;
	constexpr int kThreadGroupSize = 8;

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
}
