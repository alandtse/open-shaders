#include <catch2/catch_test_macros.hpp>

#include "Features/Upscaling/DLSSGPreset.h"

namespace
{
	constexpr auto Value(DLSSGPreset::Preset a_preset)
	{
		return static_cast<uint32_t>(a_preset);
	}
}

TEST_CASE("DLSS-G presets match the driver's DLSS-FG preset values", "[upscaling][dlssg]")
{
	CHECK(Value(DLSSGPreset::Preset::kDefault) == 0);
	CHECK(Value(DLSSGPreset::Preset::kA) == 1);
	CHECK(Value(DLSSGPreset::Preset::kB) == 2);
	CHECK(Value(DLSSGPreset::Preset::kC) == 3);
	CHECK(DLSSGPreset::kCount == 4);
}

TEST_CASE("An unchanged DLSS-G preset leaves the driver profile alone", "[upscaling][dlssg]")
{
	for (uint32_t preset = 0; preset < DLSSGPreset::kCount; ++preset)
		CHECK(DLSSGPreset::PlanProfileAction(preset, preset) == DLSSGPreset::ProfileAction::kNone);
}

TEST_CASE("Selecting an explicit DLSS-G preset writes it to the driver profile", "[upscaling][dlssg]")
{
	using enum DLSSGPreset::Preset;
	CHECK(DLSSGPreset::PlanProfileAction(Value(kDefault), Value(kC)) == DLSSGPreset::ProfileAction::kSet);
	CHECK(DLSSGPreset::PlanProfileAction(Value(kA), Value(kC)) == DLSSGPreset::ProfileAction::kSet);
	CHECK(DLSSGPreset::PlanProfileAction(Value(kC), Value(kB)) == DLSSGPreset::ProfileAction::kSet);
}

TEST_CASE("Returning to the default DLSS-G preset removes the driver profile key", "[upscaling][dlssg]")
{
	using enum DLSSGPreset::Preset;
	CHECK(DLSSGPreset::PlanProfileAction(Value(kA), Value(kDefault)) == DLSSGPreset::ProfileAction::kDelete);
	CHECK(DLSSGPreset::PlanProfileAction(Value(kC), Value(kDefault)) == DLSSGPreset::ProfileAction::kDelete);
}
