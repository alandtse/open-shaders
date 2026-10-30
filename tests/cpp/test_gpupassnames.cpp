#include "GpuPassNames.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

using namespace GpuPassDetail;

TEST_CASE("Pass names are selected by index", "[gpupass]")
{
	constexpr auto names = MakePassNames("Feature::A", "Feature::B", "Feature::C");
	STATIC_REQUIRE(names.names.size() == 3);
	CHECK((std::string_view(names.names[names.Clamp(0)]) == "Feature::A"));
	CHECK((std::string_view(names.names[names.Clamp(1)]) == "Feature::B"));
	CHECK((std::string_view(names.names[names.Clamp(2)]) == "Feature::C"));
}

TEST_CASE("An out-of-range pass index clamps to the last name", "[gpupass]")
{
	constexpr auto names = MakePassNames("Feature::A", "Feature::B", "Feature::C");
	CHECK(names.Clamp(3) == 2);
	CHECK(names.Clamp(static_cast<std::size_t>(-1)) == 2);
	CHECK((std::string_view(names.names[names.Clamp(99)]) == "Feature::C"));
}

TEST_CASE("A single-name table always selects that name", "[gpupass]")
{
	constexpr auto names = MakePassNames("Feature::Only");
	CHECK(names.Clamp(0) == 0);
	CHECK(names.Clamp(5) == 0);
}
