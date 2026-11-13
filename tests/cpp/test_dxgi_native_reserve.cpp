#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Global native DXGI headroom reserve applies to all consumers", "[dx12-native-scene][dxgi]")
{
	using DX12Native::VideoMemoryBudget;
	const auto original = VideoMemoryBudget::HeadroomPercent();
	struct Restore
	{
		uint32_t saved;
		~Restore() { DX12Native::VideoMemoryBudget::SetHeadroomPercent(saved); }
	} restore{ original };
	constexpr uint64_t mib = VideoMemoryBudget::kMiB;

	VideoMemoryBudget::SetHeadroomPercent(10);
	const auto first = VideoMemoryBudget::Evaluate(8192 * mib, 4096 * mib, 512 * mib);
	REQUIRE(first.dxgiAvailable);
	CHECK(first.localBudget == 8192 * mib);
	CHECK(first.localUsage == 4096 * mib);
	CHECK(first.reserve > 819 * mib);
	CHECK(first.reserve < 820 * mib);
	CHECK(first.effectiveBytes == 512 * mib + 4096 * mib - first.reserve);

	VideoMemoryBudget::SetHeadroomPercent(50);
	CHECK(VideoMemoryBudget::HeadroomPercent() == 50);
	const auto restricted = VideoMemoryBudget::Evaluate(8192 * mib, 4096 * mib, 512 * mib);
	CHECK(restricted.effectiveBytes == 512 * mib);
	VideoMemoryBudget::SetHeadroomPercent(0);
	CHECK(VideoMemoryBudget::HeadroomPercent() == 1);
	VideoMemoryBudget::SetHeadroomPercent(99);
	CHECK(VideoMemoryBudget::HeadroomPercent() == 50);
}
