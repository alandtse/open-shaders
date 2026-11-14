#pragma once

#include <algorithm>
#include <cstdint>

namespace DX12Native
{
	enum class MaterialUploadStatus
	{
		Pending,
		Ready,
		Failed
	};
	inline constexpr uint32_t kMaterialUploadTimeoutMs = 5000;

	/** @brief Interprets a fence result and bounded elapsed time without waiting. */
	inline MaterialUploadStatus EvaluateMaterialUpload(uint64_t a_completed, uint64_t a_elapsedMs, uint64_t a_timeoutMs)
	{
		if (a_completed == UINT64_MAX)
			return MaterialUploadStatus::Failed;
		if (a_completed >= 1)
			return MaterialUploadStatus::Ready;
		return a_elapsedMs >= a_timeoutMs ? MaterialUploadStatus::Failed : MaterialUploadStatus::Pending;
	}

	/** @brief Checks whether another staging allocation fits without overflowing the batch limit. */
	inline bool MaterialUploadFitsBatch(uint64_t a_current, uint64_t a_next, uint64_t a_limit)
	{
		return a_current <= a_limit && a_next <= a_limit - a_current;
	}

	/** @brief Tracks one bounded wait for a previously submitted upload slot. */
	class MaterialUploadRetry
	{
	public:
		void Start(uint64_t a_nowMs)
		{
			started = a_nowMs;
			waiting = true;
		}
		void Reset() { waiting = false; }
		bool Waiting() const { return waiting; }
		MaterialUploadStatus Poll(bool a_ready, uint64_t a_nowMs)
		{
			if (!waiting)
				return MaterialUploadStatus::Ready;
			const auto elapsed = a_nowMs >= started ? a_nowMs - started : 0;
			const auto status = EvaluateMaterialUpload(a_ready ? 1 : 0, elapsed, kMaterialUploadTimeoutMs);
			if (status != MaterialUploadStatus::Pending)
				waiting = false;
			return status;
		}

	private:
		uint64_t started = 0;
		bool waiting = false;
	};

	/** @brief Bounds the prepared-material staging limit selected in settings. */
	inline uint32_t ClampMaterialBatchMB(uint32_t a_value) { return std::clamp(a_value, 1u, 64u); }
}
