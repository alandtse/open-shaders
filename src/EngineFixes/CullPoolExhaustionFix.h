#pragma once

/**
 * @brief Drops culling appends once the BSCullingProcess free pool is nearly exhausted.
 *
 * BSCullingProcess::AppendVirtual writes through PopFreeQueueEntry's result with no null check,
 * so an exhausted 8192-entry pool is a guaranteed null write. Hooked on both the base and the
 * parabolic culling vtables, which override the slot separately.
 */
struct CullPoolExhaustionFix : EngineFix
{
	std::string GetName() override { return "Cull Pool Exhaustion Fix"; }
	const char* GetEngineFixesName() const override { return "CullingProcessAppendVirtualPoolGuard"; }

	void Install() override;

	/** @brief Appends dropped since the last reset; the shadow scheduler drains it per frame. */
	static inline std::atomic<std::uint32_t> dropCount{ 0 };

	template <class Vtable>
	struct AppendVirtualGuard
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
