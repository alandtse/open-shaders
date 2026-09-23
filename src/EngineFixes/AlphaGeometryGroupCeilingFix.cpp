#include "AlphaGeometryGroupCeilingFix.h"

#include "Globals.h"

namespace
{
	// Slot counts passed to the array's static constructor; VR was built with twice the slots.
	constexpr std::uint32_t kCapacityFlat = 512;
	constexpr std::uint32_t kCapacityVR = 1024;

	// The engine claims slots with LOCK XADD, so another worker can claim one between the check
	// and the increment: the reserve must exceed the max concurrent threads.
	constexpr std::uint32_t kReserve = 64;

	std::uint32_t* s_count = nullptr;
	std::uint32_t s_limit = 0;

	/// Decodes the slot counter from ClearAlphaGeometryGroups (100856/107646), which has no id of
	/// its own: an 11-byte `mov dword [rip+disp32], 0; ret` whose operand is the counter.
	std::uint32_t* DecodeCounter()
	{
		constexpr std::uint8_t kMovDwordImmOpcode = 0xC7;
		constexpr std::uint8_t kRipRelativeModRM = 0x05;
		constexpr std::uint8_t kRetOpcode = 0xC3;
		constexpr std::size_t kMovDwordImmSize = 10;

		const auto clearFn = REL::RelocationID(100856, 107646).address();
		if (!clearFn)
			return nullptr;

		const auto* code = reinterpret_cast<const std::uint8_t*>(clearFn);
		std::int32_t displacement = 0;
		std::uint32_t immediate = 1;
		std::memcpy(&displacement, code + 2, sizeof(displacement));
		std::memcpy(&immediate, code + 6, sizeof(immediate));

		// A patched sentinel would mean the counter no longer means what the ceiling assumes.
		if (code[0] != kMovDwordImmOpcode || code[1] != kRipRelativeModRM || immediate != 0u ||
			code[kMovDwordImmSize] != kRetOpcode)
			return nullptr;

		// Dereferenced every frame, so the decode must land in the module's own data.
		const auto counter = clearFn + kMovDwordImmSize + displacement;
		const auto data = REL::Module::get().segment(REL::Segment::data);
		if (counter < data.address() || counter >= data.address() + data.size())
			return nullptr;
		return reinterpret_cast<std::uint32_t*>(counter);
	}
}

void AlphaGeometryGroupCeilingFix::Install()
{
	s_count = DecodeCounter();
	if (!s_count) {
		logger::error("[EngineFixes] Alpha GeometryGroup ceiling not installed: ClearAlphaGeometryGroups did not decode to a counter in .data");
		return;
	}
	s_limit = (globals::game::isVR ? kCapacityVR : kCapacityFlat) - kReserve;
	stl::detour_thunk<BSBatchRenderer_StartGroupingAlphas>(REL::RelocationID(100874, 107670));
}

void* AlphaGeometryGroupCeilingFix::BSBatchRenderer_StartGroupingAlphas::thunk(RE::BSBatchRenderer* a_this, void* a_bound, RE::NiCamera* a_camera, bool a_sortByClosestPoint)
{
	if (s_count && a_camera) {
		const std::uint32_t live = *s_count;
		// CAS so a concurrent worker cannot lose a higher peak.
		std::uint32_t seen = peak.load(std::memory_order_relaxed);
		while (live > seen && !peak.compare_exchange_weak(seen, live, std::memory_order_relaxed)) {
		}
		if (live >= s_limit) {
			const std::uint64_t n = drops.fetch_add(1, std::memory_order_relaxed) + 1;
			if (n == 1u || (n % 10000u) == 0u)
				logger::warn("[EngineFixes] Alpha GeometryGroup ceiling reached ({} live, {} refused)", live, n);
			return nullptr;
		}
	}
	return func(a_this, a_bound, a_camera, a_sortByClosestPoint);
}
