// CPU-only tests for startup instance routing and exact Feature 18 extents.

#include "Features/Upscaling/NeuralRendering/EyeMode.h"
#include "Features/Upscaling/NeuralRendering/PrewarmDimensions.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Prewarm uses the active runtime eye count and mode-specific size", "[nr][prewarm]")
{
	NR::Tuning tuning;
	REQUIRE(NR::PrewarmEyeCount(false) == 1);
	REQUIRE(NR::PrewarmEyeCount(true) == 2);
	REQUIRE(NR::PrewarmEyeExtent(tuning, true, 1920, 1080) == std::pair<uint32_t, uint32_t>{ 1920, 1080 });

	tuning.skipFrameReuse = true;
	tuning.modelScale = 0.5f;
	REQUIRE(NR::GetEyeMode(tuning) == NR::EyeMode::kAlternate);
	REQUIRE(NR::PrewarmEyeExtent(tuning, true, 1920, 1080) == std::pair<uint32_t, uint32_t>{ 1920, 1080 });
	REQUIRE(NR::PrewarmEyeExtent(tuning, false, 1920, 1080) == NR::ModelExtent(0.5f, 1920, 1080));

	tuning.sbsEvaluate = true;
	REQUIRE(NR::GetEyeMode(tuning) == NR::EyeMode::kSideBySide);
	REQUIRE(NR::PrewarmEyeExtent(tuning, true, 1920, 1080) == NR::ModelExtent(0.5f, 1920, 1080));
}

TEST_CASE("SBS prewarm dimensions match PlanSbs for full frames and crops", "[nr][prewarm]")
{
	const Util::Subrect::PixelRegion fullEye{ 0, 0, 1920, 1080 };
	const std::array<Util::Subrect::PixelRegion, 2> full{ fullEye, fullEye };
	REQUIRE(NR::SideBySideExtent(full, 1920, 1080) == std::pair<uint32_t, uint32_t>{ 3872, 1080 });

	const std::array<Util::Subrect::PixelRegion, 2> crops{
		Util::Subrect::PixelRegion{ 10, 20, 640, 480 },
		Util::Subrect::PixelRegion{ 0, 0, 800, 600 },
	};
	REQUIRE(NR::SideBySideExtent(crops, 1920, 1080) == std::pair<uint32_t, uint32_t>{ 1472, 600 });
}
