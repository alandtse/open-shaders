#pragma once

#include <cstdint>
#include <algorithm>

namespace DX12Native
{
	enum class MaterialUploadStatus { Pending, Ready, Failed };

	/** @brief Interprets a fence result and bounded elapsed time without waiting. */
	inline MaterialUploadStatus EvaluateMaterialUpload(uint64_t a_completed, uint64_t a_elapsedMs, uint64_t a_timeoutMs)
	{
		if (a_completed == UINT64_MAX) return MaterialUploadStatus::Failed;
		if (a_completed >= 1) return MaterialUploadStatus::Ready;
		return a_elapsedMs >= a_timeoutMs ? MaterialUploadStatus::Failed : MaterialUploadStatus::Pending;
	}

	/** @brief Checks whether another staging allocation fits without overflowing the batch limit. */
	inline bool MaterialUploadFitsBatch(uint64_t a_current, uint64_t a_next, uint64_t a_limit)
	{
		return a_current <= a_limit && a_next <= a_limit - a_current;
	}

	/** @brief Bounds the prepared-material staging limit selected in settings. */
	inline uint32_t ClampMaterialBatchMB(uint32_t a_value) { return std::clamp(a_value, 1u, 64u); }
}
