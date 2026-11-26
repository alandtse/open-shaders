#pragma once

#include <cstdint>
#include <limits>

namespace DX12Native
{
	// An untracked submission has no ordinary completion marker. Device removal
	// permits retirement, but must never be interpreted as a successful GPU build.
	inline constexpr uint64_t kUntrackedFenceValue = std::numeric_limits<uint64_t>::max();

	constexpr bool FenceValueRetired(bool hasFence, uint64_t completed, uint64_t target)
	{
		return target == 0 ||
		       (hasFence && (completed == kUntrackedFenceValue ||
								(target != kUntrackedFenceValue && completed >= target)));
	}
}
