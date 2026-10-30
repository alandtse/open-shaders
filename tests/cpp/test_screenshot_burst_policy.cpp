#ifdef DEVBENCH_BRIDGE_ENABLED
#	include "Features/ScreenshotBurstPolicy.h"
#	include <array>
#	include <catch2/catch_test_macros.hpp>

namespace
{
	using json = nlohmann::json;

	json Descriptor()
	{
		return { { "regions", json::array({ { { "x", 1 }, { "y", 2 }, { "width", 3 }, { "height", 2 } },
								  { { "x", 4 }, { "y", 1 }, { "width", 3 }, { "height", 3 } } }) } };
	}
}

TEST_CASE("Burst plan packs regions into a native atlas", "[screenshot-burst]")
{
	using namespace ScreenshotBurst;
	const auto plan = Parse(Descriptor(), 150, 2);
	CHECK(plan.payloadBytes == 18000);
	CHECK(plan.width == 3);
	CHECK(plan.height == 5);
}

TEST_CASE("Burst atlas copies keep the native orientation under every flip", "[screenshot-burst]")
{
	using namespace ScreenshotBurst;
	const auto plan = Parse(Descriptor(), 150, 2);
	for (bool flipX : { false, true }) {
		for (bool flipY : { false, true }) {
			CAPTURE(flipX, flipY);
			std::array<uint32_t, 80> source{};
			std::array<uint32_t, 15> atlas{};
			for (uint32_t y = 0; y < 8; ++y)
				for (uint32_t x = 0; x < 10; ++x)
					source[(flipY ? 7 - y : y) * 10 + (flipX ? 9 - x : x)] = y * 10 + x;
			for (const auto& copy : plan.Copies(10, 8, flipX, flipY))
				for (uint32_t y = 0; y < copy.height; ++y)
					for (uint32_t x = 0; x < copy.width; ++x)
						atlas[(copy.destinationY + y) * 3 + x] = source[(copy.y + y) * 10 + copy.x + x];
			uint32_t top = 0;
			for (const auto& r : plan.regions) {
				for (uint32_t y = 0; y < r.height; ++y)
					for (uint32_t x = 0; x < r.width; ++x)
						CHECK(atlas[(flipY ? 4 - top - y : top + y) * 3 + (flipX ? 2 - x : x)] == (r.y + y) * 10 + r.x + x);
				top += r.height;
			}
		}
	}
}

TEST_CASE("Burst parsing rejects descriptors outside its budgets", "[screenshot-burst]")
{
	using namespace ScreenshotBurst;
	const auto descriptor = Descriptor();
	const auto plan = Parse(descriptor, 150, 2);
	CHECK_THROWS(plan.Copies(6, 8, false, false));
	CHECK_THROWS(Parse(descriptor, MaximumFrames + 1, 2));
	CHECK_THROWS(Parse(descriptor, 0, 2));
	for (auto invalid : { json(-1), json(1.5), json(UINT64_MAX), json(0) }) {
		auto value = descriptor;
		value["regions"][0]["width"] = invalid;
		CHECK_THROWS(Parse(value, 150, 2));
	}

	auto value = descriptor;
	value["maximumBytes"] = plan.payloadBytes - 1;
	CHECK_THROWS(Parse(value, 150, 2));
	value["maximumBytes"] = plan.payloadBytes;
	CHECK(Parse(value, 150, 2).payloadBytes == plan.payloadBytes);
	value["maximumBytes"] = MaximumBytes + 1;
	CHECK_THROWS(Parse(value, 150, 2));

	value = descriptor;
	value["regions"][1]["width"] = 2;
	CHECK_THROWS(Parse(value, 150, 2));
	value = descriptor;
	value["ignored"] = true;
	CHECK_THROWS(Parse(value, 150, 2));
	value = { { "regions", json::array({ { { "x", 0 }, { "y", 0 }, { "width", 16384 }, { "height", 16384 } } }) } };
	CHECK_THROWS(Parse(value, 1, 2));
}

TEST_CASE("Burst continuity requires consecutive acquisitions", "[screenshot-burst]")
{
	using namespace ScreenshotBurst;
	Continuity continuity;
	CHECK_FALSE(continuity.Receipt(3, false).at("complete").get<bool>());
	CHECK((continuity.Observe(100, 200) && continuity.Observe(101, 201) && continuity.Observe(102, 202)));
	CHECK(continuity.Receipt(3, true).at("complete").get<bool>());
	CHECK_FALSE(continuity.Receipt(3, false).at("complete").get<bool>());
	for (auto next : { 102ull, 104ull, 1ull }) {
		auto gap = continuity;
		CHECK_FALSE(gap.Observe(next, 203));
		CHECK_FALSE(gap.Receipt(4, true).at("complete").get<bool>());
	}
	CHECK_FALSE(continuity.Observe(103, 204));
	continuity.Fail("later_failure");
	CHECK(continuity.failure == "nonconsecutive_acquisition");

	Continuity monotonic;
	CHECK((monotonic.Observe(0, std::nullopt) && monotonic.Observe(1, std::nullopt)));
	CHECK(monotonic.Receipt(2, true).at("complete").get<bool>());
	CHECK_FALSE(monotonic.Observe(2, 5));
}
#endif
