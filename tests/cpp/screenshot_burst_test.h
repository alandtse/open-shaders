#pragma once
#include "Features/ScreenshotBurstPolicy.h"
#include <array>
#include <stdexcept>

inline void RunScreenshotBurstTests()
{
	using namespace ScreenshotBurst;
	using json = nlohmann::json;
	auto check = [](bool condition) { if (!condition) throw std::runtime_error("screenshot burst regression"); };
	auto rejects = [&check](auto&& action) {
		bool rejected = false;
		try {
			action();
		} catch (const std::exception&) {
			rejected = true;
		}
		check(rejected);
	};
	json descriptor = { { "regions", json::array({ { { "x", 1 }, { "y", 2 }, { "width", 3 }, { "height", 2 } },
										 { { "x", 4 }, { "y", 1 }, { "width", 3 }, { "height", 3 } } }) } };
	const auto plan = Parse(descriptor, 150, 2);
	check(plan.payloadBytes == 18000 && plan.width == 3 && plan.height == 5);
	for (bool flipX : { false, true }) {
		for (bool flipY : { false, true }) {
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
						check(atlas[(flipY ? 4 - top - y : top + y) * 3 + (flipX ? 2 - x : x)] == (r.y + y) * 10 + r.x + x);
				top += r.height;
			}
		}
	}
	rejects([&] { plan.Copies(6, 8, false, false); });
	rejects([&] { Parse(descriptor, MaximumFrames + 1, 2); });
	rejects([&] { Parse(descriptor, 0, 2); });
	for (auto invalid : { json(-1), json(1.5), json(UINT64_MAX), json(0) }) {
		auto value = descriptor;
		value["regions"][0]["width"] = invalid;
		rejects([&] { Parse(value, 150, 2); });
	}
	auto value = descriptor;
	value["maximumBytes"] = plan.payloadBytes - 1;
	rejects([&] { Parse(value, 150, 2); });
	value["maximumBytes"] = plan.payloadBytes;
	check(Parse(value, 150, 2).payloadBytes == plan.payloadBytes);
	value["maximumBytes"] = MaximumBytes + 1;
	rejects([&] { Parse(value, 150, 2); });
	value = descriptor;
	value["regions"][1]["width"] = 2;
	rejects([&] { Parse(value, 150, 2); });
	value = descriptor;
	value["ignored"] = true;
	rejects([&] { Parse(value, 150, 2); });
	value = { { "regions", json::array({ { { "x", 0 }, { "y", 0 }, { "width", 16384 }, { "height", 16384 } } }) } };
	rejects([&] { Parse(value, 1, 2); });
	Continuity continuity;
	check(!continuity.Receipt(3, false).at("complete").get<bool>());
	check(continuity.Observe(100, 200) && continuity.Observe(101, 201) && continuity.Observe(102, 202));
	check(continuity.Receipt(3, true).at("complete").get<bool>());
	check(!continuity.Receipt(3, false).at("complete").get<bool>());
	for (auto next : { 102ull, 104ull, 1ull }) {
		auto gap = continuity;
		check(!gap.Observe(next, 203));
		check(!gap.Receipt(4, true).at("complete").get<bool>());
	}
	check(!continuity.Observe(103, 204));
	continuity.Fail("later_failure");
	check(continuity.failure == "nonconsecutive_acquisition");
	Continuity mono;
	check(mono.Observe(0, std::nullopt) && mono.Observe(1, std::nullopt));
	check(mono.Receipt(2, true).at("complete").get<bool>());
	check(!mono.Observe(2, 5));
}
