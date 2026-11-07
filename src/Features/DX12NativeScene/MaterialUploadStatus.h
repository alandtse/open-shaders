#pragma once

#include <cstdint>

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
}
