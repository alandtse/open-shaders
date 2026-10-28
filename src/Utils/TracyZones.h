#pragma once
#include "Utils/Macros.h"
#include <Tracy/Tracy.hpp>
#include <atomic>
#include <cstdint>

namespace TracyZones
{
	/** @brief Runtime-switchable families of high-volume per-draw Tracy zones. */
	enum class Family : uint8_t
	{
		StateDraw,
		RenderPassHook,
		Count,
	};

	/** @brief Emission switch for the per-draw families; off by default so a normal capture stays small. */
	inline std::atomic<bool> perDrawZones{ false };

	/** @brief Per-family call counters accumulated since the last frame plot. */
	inline std::atomic<uint32_t> perFamilyCalls[static_cast<uint32_t>(Family::Count)]{};

	/** @brief Returns whether the per-draw zone families currently emit. */
	inline bool PerDrawZonesEnabled()
	{
		return perDrawZones.load(std::memory_order_relaxed);
	}

	/** @brief Turns emission of the per-draw zone families on or off. */
	inline void SetPerDrawZones(bool a_enabled)
	{
		perDrawZones.store(a_enabled, std::memory_order_relaxed);
	}

	/** @brief Records one call of a zone family, emitted or not. No-op without Tracy. */
	inline void Count(Family a_family)
	{
#ifdef TRACY_ENABLE
		perFamilyCalls[static_cast<uint32_t>(a_family)].fetch_add(1, std::memory_order_relaxed);
#else
		(void)a_family;
#endif
	}

	/** @brief Reads a family's call count since the last frame plot. */
	inline uint32_t Snapshot(Family a_family)
	{
#ifdef TRACY_ENABLE
		return perFamilyCalls[static_cast<uint32_t>(a_family)].load(std::memory_order_relaxed);
#else
		(void)a_family;
		return 0;
#endif
	}

	/** @brief Publishes per-frame call counts as Tracy plots and resets them. No-op without Tracy. */
	inline void PlotAndResetCounters()
	{
#ifdef TRACY_ENABLE
		TracyPlot("zonecalls.State::Draw", (int64_t)perFamilyCalls[static_cast<uint32_t>(Family::StateDraw)].exchange(0, std::memory_order_relaxed));
		TracyPlot("zonecalls.OnRenderPassBegin", (int64_t)perFamilyCalls[static_cast<uint32_t>(Family::RenderPassHook)].exchange(0, std::memory_order_relaxed));
#endif
	}
}

/** @brief Opens a per-draw Tracy zone that emits only while TracyZones::PerDrawZonesEnabled() holds. */
#define CS_DRAW_ZONE(name) ZoneNamedN(CS_DETAIL_CONCAT(cs_draw_zone_, __LINE__), name, TracyZones::PerDrawZonesEnabled())
