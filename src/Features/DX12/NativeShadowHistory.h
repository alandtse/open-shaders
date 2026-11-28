#pragma once

#include "../../../package/Shaders/Common/NativeShadowRT.hlsli"

#include <algorithm>
#include <cstdint>

namespace DX12Native
{
	struct ShadowHistoryStamp
	{
		uint32_t frame = 0, width = 0, height = 0, quality = 0, scope = 0, cell = 0;
		uint64_t epoch = 0;
		uint32_t samples = 0;
	};

	/** @brief Only consecutive, compatible RT frames may share screen-space samples. */
	constexpr uint32_t ShadowHistoryCount(const ShadowHistoryStamp& previous, const ShadowHistoryStamp& current)
	{
		if (current.quality == kShadowQualityOffloading || !previous.samples ||
			uint32_t(current.frame - previous.frame) != 1u ||
			previous.width != current.width || previous.height != current.height ||
			previous.quality != current.quality || previous.scope != current.scope ||
			previous.cell != current.cell || previous.epoch != current.epoch)
			return 0;
		return std::min(previous.samples, ShadowTemporalFrames(current.quality) - 1u);
	}
}
