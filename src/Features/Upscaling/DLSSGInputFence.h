#pragma once

#include <cstdint>

/**
 * @brief Tracks the value DLSS-G's input fence must reach before the tagged inputs of the
 *        last present may be overwritten. The fence is not owned; it dies with DLSS-G's resources.
 */
template <class Fence>
class DLSSGInputFenceTracker
{
public:
	/** @brief Records the last present's fence value, replacing the old target so a fence recreated at the same address cannot keep a stale one. */
	void Track(Fence* a_fence, std::uint64_t a_value)
	{
		fence = a_fence;
		target = fence ? a_value : 0;
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
