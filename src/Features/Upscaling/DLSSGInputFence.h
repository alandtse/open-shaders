#pragma once

#include <algorithm>
#include <cstdint>

/**
 * @brief Tracks the value DLSS-G's input fence must reach before the tagged inputs of the
 *        last present may be overwritten. The fence is not owned; it dies with DLSS-G's resources.
 */
template <class Fence>
class DLSSGInputFenceTracker
{
public:
	/** @brief Records a post-present fence value; a different fence (DLSS-G recreated) discards the old target. */
	void Track(Fence* a_fence, std::uint64_t a_value)
	{
		if (a_fence != fence) {
			fence = a_fence;
			target = 0;
		}
		if (fence)
			target = std::max(target, a_value);
	}

	/** @brief Forgets the fence, for when DLSS-G frees the resources that own it. */
	void Reset()
	{
		fence = nullptr;
		target = 0;
	}

	/** @brief The fence to wait on, or null when no present has been tracked. */
	Fence* GetFence() const { return target ? fence : nullptr; }

	/** @brief The value the fence reaches once DLSS-G has consumed the last presented inputs. */
	std::uint64_t GetTarget() const { return target; }

	/** @brief True when DLSS-G is still reading the last presented inputs, given the fence's completed value. */
	bool IsBehind(std::uint64_t a_completed) const { return GetFence() && a_completed < target; }

private:
	Fence* fence = nullptr;
	std::uint64_t target = 0;
};
