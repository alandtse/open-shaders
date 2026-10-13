#pragma once

#include "EyeMode.h"
#include "ModelScale.h"
#include "Utils/Subrect.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace NR
{
	inline constexpr uint32_t kSideBySideGutter = 32;

	/** @brief Eye count used by the render route and its startup instance preparation. */
	[[nodiscard]] inline uint32_t PrewarmEyeCount(bool vr) { return vr ? 2u : 1u; }

	/** @brief Model size shared by per-eye evaluation, including Alternate mode's full-size requirement. */
	[[nodiscard]] inline std::pair<uint32_t, uint32_t> PrewarmEyeExtent(const Tuning& tuning, bool vr, uint32_t width, uint32_t height)
	{
		const auto scale = vr && GetEyeMode(tuning) == EyeMode::kAlternate ? Tuning::kMaxModelScale : tuning.modelScale;
		const auto reduced = ModelExtent(scale, width, height);
		return reduced.first ? reduced : std::pair{ width, height };
	}

	/** @brief SBS Feature 18 extent from the same eye crops used by PlanSbs; empty crops use the model extent. */
	[[nodiscard]] inline std::pair<uint32_t, uint32_t> SideBySideExtent(
		const std::array<Util::Subrect::PixelRegion, 2>& crops, uint32_t modelWidth, uint32_t modelHeight)
	{
		const auto leftWidth = crops[0].w ? crops[0].w : modelWidth;
		const auto leftHeight = crops[0].h ? crops[0].h : modelHeight;
		const auto rightWidth = crops[1].w ? crops[1].w : modelWidth;
		const auto rightHeight = crops[1].h ? crops[1].h : modelHeight;
		return { leftWidth + kSideBySideGutter + rightWidth, std::max(leftHeight, rightHeight) };
	}
}
