#include "CullPoolExhaustionFix.h"

namespace
{
	// RE::BSCullingProcess::Data free-object-pool layout, Ghidra-verified byte-identical across
	// SE/AE/VR: PtrMultiProdCons<Data,8192,0>'s free/start/end. PopFreeQueueEntry's CAS loop
	// guarantees tail >= head (no underflow).
	constexpr std::uintptr_t kFreePoolOffset = 0x20150;
	constexpr std::uintptr_t kPoolHeadOffset = 0x10000;
	constexpr std::uintptr_t kPoolTailOffset = 0x10008;

	// Absorbs concurrent worker pops between the check and the append.
	constexpr std::uint32_t kFreeEntryMargin = 16;

	struct BaseVtable;
	struct ParabolicVtable;

	std::atomic<bool> s_loggedDrop{ false };

	bool NearExhaustion(const RE::BSCullingProcess* a_this)
	{
		const auto* pool = reinterpret_cast<const std::uint8_t*>(a_this) + kFreePoolOffset;
		const auto head = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolHeadOffset)->load(std::memory_order_relaxed);
		const auto tail = reinterpret_cast<const std::atomic<std::uint32_t>*>(pool + kPoolTailOffset)->load(std::memory_order_relaxed);
		return tail - head < kFreeEntryMargin;
	}
}

void CullPoolExhaustionFix::Install()
{
	stl::write_vfunc<0x18, AppendVirtualGuard<BaseVtable>>(RE::VTABLE_BSCullingProcess[0]);
	stl::write_vfunc<0x18, AppendVirtualGuard<ParabolicVtable>>(RE::VTABLE_BSParabolicCullingProcess[0]);
}

template <class Vtable>
void CullPoolExhaustionFix::AppendVirtualGuard<Vtable>::thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
{
	if (NearExhaustion(a_this)) {
		dropCount.fetch_add(1, std::memory_order_relaxed);
		if (!s_loggedDrop.exchange(true, std::memory_order_relaxed))
			logger::warn("[EngineFixes] Culling pool near exhaustion; dropping appends");
		return;
	}
	func(a_this, a_visible, a_alphaGroupIndex);
}
