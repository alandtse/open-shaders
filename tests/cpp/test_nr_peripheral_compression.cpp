#include "Features/Upscaling/NeuralRendering/PeripheralCompression.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
	NR::PeripheralCompressionPlan Plan(uint32_t percent, uint32_t width = 2048, uint32_t height = 2048, uint32_t ordinaryWidth = 2048, uint32_t ordinaryHeight = 2048)
	{
		return NR::BuildPeripheralCompressionPlan(width, height, ordinaryWidth, ordinaryHeight, percent, true, 8, 64);
	}

	void Near(NR::PeripheralMapping::Point actual, NR::PeripheralMapping::Point expected)
	{
		for (unsigned axis = 0; axis < 2; ++axis)
			REQUIRE(actual[axis] == Catch::Approx(expected[axis]).margin(2e-6));
	}
}

TEST_CASE("Peripheral sampling is opt-in and rejects unsupported or invalid geometry", "[nr][peripheral]")
{
	using enum NR::PeripheralCompressionReason;
	REQUIRE(Plan(0).reason == Disabled);
	REQUIRE(Plan(101).reason == InvalidGeometry);
	REQUIRE(Plan(20, 0).reason == InvalidGeometry);
	REQUIRE(Plan(20, 2000, 2048, 2048, 2048).reason == InvalidGeometry);
	REQUIRE(NR::BuildPeripheralCompressionPlan(2048, 2048, 2048, 2048, 20, false, 8, 64).reason == UnsupportedScope);
	REQUIRE(NR::BuildPeripheralCompressionPlan(2048, 2048, 2048, 2048, 20, true, 0, 64).reason == InvalidGeometry);
	REQUIRE(Plan(20, 64, 64, 64, 64).reason == InsufficientSavings);
	REQUIRE(Plan(100, 16384, 64, 4096, 64).reason == FootprintLimit);
	REQUIRE_FALSE(Plan(20, 64, 64, 64, 64).Active());
}

TEST_CASE("Peripheral compaction retains the entire eye and decreases the model grid", "[nr][peripheral]")
{
	for (uint32_t percent : { 1u, 20u, 50u, 100u }) {
		const auto plan = Plan(percent);
		REQUIRE(plan.Active());
		REQUIRE(plan.width < 2048);
		REQUIRE(plan.height < 2048);
		REQUIRE(plan.width % 8 == 0);
		REQUIRE(plan.height % 8 == 0);
		Near(plan.mapping.Forward({ 0.0f, 0.0f }), { 0.0f, 0.0f });
		Near(plan.mapping.Forward({ 1.0f, 1.0f }), { 1.0f, 1.0f });
		Near(plan.mapping.Forward({ 0.0f, 1.0f }), { 0.0f, 1.0f });
		Near(plan.mapping.Forward({ 1.0f, 0.0f }), { 1.0f, 0.0f });
		Near(plan.mapping.Forward({ 0.5f, 0.5f }), { 0.5f, 0.5f });
		for (unsigned y = 0; y <= 30; ++y)
			for (unsigned x = 0; x <= 30; ++x) {
				const NR::PeripheralMapping::Point source{ float(x) / 30, float(y) / 30 };
				const auto mapped = plan.mapping.Forward(source);
				REQUIRE(mapped[0] >= 0.0f);
				REQUIRE(mapped[1] >= 0.0f);
				REQUIRE(mapped[0] <= 1.0f);
				REQUIRE(mapped[1] <= 1.0f);
				Near(plan.mapping.Inverse(mapped), source);
			}
	}
	REQUIRE(Plan(100).width == 1024);
	REQUIRE(Plan(100).height == 1024);
}

TEST_CASE("The protected centre retains ordinary model density independently of uniform scale", "[nr][peripheral]")
{
	for (uint32_t ordinary : { 2048u, 1536u, 1024u, 512u }) {
		const auto plan = Plan(50, 2048, 2048, ordinary, ordinary);
		REQUIRE(plan.Active());
		const auto left = plan.mapping.Forward({ 0.4f, 0.5f });
		const auto right = plan.mapping.Forward({ 0.6f, 0.5f });
		REQUIRE((right[0] - left[0]) * plan.width >= 0.2f * ordinary - 1e-3f);
	}
}

TEST_CASE("Rectangular compaction admits each supported eye size without crossing eye ownership", "[nr][peripheral]")
{
	for (const auto dimensions : { std::array{ 1921u, 1081u }, std::array{ 2048u, 2200u }, std::array{ 2304u, 2560u } }) {
		const auto plan = Plan(45, dimensions[0], dimensions[1], dimensions[0], dimensions[1]);
		REQUIRE(plan.Active());
		for (unsigned axis = 0; axis < 2; ++axis) {
			NR::PeripheralMapping::Point edge{ 0.37f, 0.61f };
			edge[axis] = 0.0f;
			Near(plan.mapping.Inverse(plan.mapping.Forward(edge)), edge);
			edge[axis] = 1.0f;
			Near(plan.mapping.Inverse(plan.mapping.Forward(edge)), edge);
		}
	}
}

TEST_CASE("Offset source rectangles keep every reconstruction sample within their conservative model bounds", "[nr][peripheral]")
{
	const std::array rectangles{
		std::array{ 0.0f, 0.0f, 1.0f, 1.0f },
		std::array{ 0.12f, 0.2f, 0.73f, 0.91f },
		std::array{ 0.45f, 0.05f, 0.95f, 0.4f },
		std::array{ 0.02f, 0.71f, 0.32f, 0.99f },
		std::array{ 0.45f, 0.45f, 0.55f, 0.55f }
	};
	for (uint32_t percent : { 1u, 20u, 50u, 100u }) {
		const auto mapping = Plan(percent).mapping;
		for (const auto& rect : rectangles) {
			const auto [lower, upper] = mapping.TargetBounds({ rect[0], rect[1] }, { rect[2], rect[3] });
			for (unsigned y = 0; y <= 40; ++y)
				for (unsigned x = 0; x <= 40; ++x) {
					const NR::PeripheralMapping::Point point{ std::lerp(rect[0], rect[2], float(x) / 40), std::lerp(rect[1], rect[3], float(y) / 40) };
					const auto mapped = mapping.Forward(point);
					for (unsigned axis = 0; axis < 2; ++axis) {
						REQUIRE(mapped[axis] >= lower[axis] - 1e-6f);
						REQUIRE(mapped[axis] <= upper[axis] + 1e-6f);
					}
				}
		}
	}
}

TEST_CASE("Admitted area filters cover the inverse footprint and keep bounded work", "[nr][peripheral]")
{
	for (uint32_t ordinary : { 2048u, 1024u, 512u })
		for (uint32_t percent : { 1u, 20u, 50u, 100u }) {
			const auto plan = Plan(percent, 2048, 2048, ordinary, ordinary);
			if (!plan.Active()) {
				REQUIRE(plan.reason == NR::PeripheralCompressionReason::InsufficientSavings);
				continue;
			}
			for (unsigned y = 0; y < plan.height; y += 17)
				for (unsigned x = 0; x < plan.width; x += 19) {
					const NR::PeripheralMapping::Point first{ float(x) / plan.width, float(y) / plan.height };
					const NR::PeripheralMapping::Point end{ float(x + 1) / plan.width, float(y + 1) / plan.height };
					const auto [lower, upper] = plan.mapping.SourceBounds(first, end);
					for (unsigned axis = 0; axis < 2; ++axis)
						REQUIRE((upper[axis] - lower[axis]) * 2048 <= NR::kPeripheralMaximumFootprint + 0.01f);
					for (float fy : { 0.0f, 0.5f, 1.0f })
						for (float fx : { 0.0f, 0.5f, 1.0f }) {
							const auto sample = plan.mapping.Inverse({ std::lerp(first[0], end[0], fx), std::lerp(first[1], end[1], fy) });
							for (unsigned axis = 0; axis < 2; ++axis) {
								REQUIRE(sample[axis] >= lower[axis] - 1e-6f);
								REQUIRE(sample[axis] <= upper[axis] + 1e-6f);
							}
						}
				}
		}
}

TEST_CASE("Motion endpoints retain off-screen displacements instead of clipping to the visible mask", "[nr][peripheral]")
{
	const auto map = Plan(80).mapping;
	for (const auto point : { NR::PeripheralMapping::Point{ -0.15f, 0.45f }, NR::PeripheralMapping::Point{ 0.72f, 1.12f } }) {
		const auto endpoint = map.Forward(point);
		Near(map.Inverse(endpoint), point);
	}
}
