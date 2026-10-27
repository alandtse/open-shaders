#include "Features/Screenshot/SequencePolicy.h"
#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("half-float colour maps to 8 bits by clamping and scaling", "[screenshot-half]")
{
	using namespace OS::Capture;
	const auto& table = HalfToByteTable();
	REQUIRE(table[0x0000] == 0);
	REQUIRE(table[0x3C00] == 255);  // 1.0
	REQUIRE(table[0x3800] == 128);  // 0.5
	REQUIRE(table[0xBC00] == 0);    // -1.0
	REQUIRE(table[0x7BFF] == 255);  // largest finite half
	REQUIRE(table[0x7E00] == 0);    // NaN
	REQUIRE(table[0x7C00] == 255);  // +infinity
	for (size_t code = 1; code < 0x3C00; ++code)
		REQUIRE(table[code] >= table[code - 1]);
}

TEST_CASE("half-float pixels are converted per channel, flipped and made opaque", "[screenshot-half]")
{
	using namespace OS::Capture;
	const uint16_t source[8] = { 0x3C00, 0x3800, 0x0000, 0x3C00, 0x0000, 0x3C00, 0x3800, 0x3C00 };  // two texels
	std::array<uint8_t, 16> bytes{};
	std::memcpy(bytes.data(), source, sizeof(source));
	const PixelBounds plain{ 0, 0, 2, 1, false, false };
	const auto out = CopyPixels(bytes, 16, plain, false, SourceDepth::Half);
	REQUIRE(out.size() == 8);
	REQUIRE(out[0] == 255);
	REQUIRE(out[1] == 128);
	REQUIRE(out[2] == 0);
	REQUIRE(out[3] == 255);
	REQUIRE(out[4] == 0);
	REQUIRE(out[5] == 255);
	REQUIRE(out[6] == 128);
	REQUIRE(out[7] == 255);
	const PixelBounds flipped{ 0, 0, 2, 1, true, false };
	REQUIRE(CopyPixels(bytes, 16, flipped, false, SourceDepth::Half)[0] == 0);
	REQUIRE_THROWS(CopyPixels(std::span<const uint8_t>(bytes).first(8), 8, plain, false, SourceDepth::Half));
}

TEST_CASE("ten-bit sRGB pixels scale to 8 bits per channel and are made opaque", "[screenshot-half]")
{
	using namespace OS::Capture;
	const uint32_t texels[2] = { (3u << 30) | (1023u << 20) | (512u << 10) | 0u, (0u << 30) | (0u << 20) | (1023u << 10) | 1023u };
	std::array<uint8_t, 8> bytes{};
	std::memcpy(bytes.data(), texels, sizeof(texels));
	const PixelBounds bounds{ 0, 0, 2, 1, false, false };
	const auto out = CopyPixels(bytes, 8, bounds, false, SourceDepth::Ten);
	REQUIRE(out.size() == 8);
	REQUIRE(out[0] == 0);
	REQUIRE(out[1] == 128);  // 512 / 1023 * 255 = 127.6
	REQUIRE(out[2] == 255);
	REQUIRE(out[3] == 255);
	REQUIRE(out[4] == 255);
	REQUIRE(out[5] == 255);
	REQUIRE(out[6] == 0);
	REQUIRE(out[7] == 255);
	REQUIRE_THROWS(CopyPixels(std::span<const uint8_t>(bytes).first(4), 4, bounds, false, SourceDepth::Ten));
}

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Features/Screenshot/BurstAdapter.h"
#	include <Windows.h>
#	include <catch2/catch_test_macros.hpp>

namespace
{
	using json = nlohmann::json;
	using namespace OS::Capture::Burst;

	Request Valid()
	{
		Request request;
		request.requestId = "burst-1";
		request.prefix = "shot";
		request.outputDir = "C:/Captures/run";
		request.count = 3;
		request.everyNFrames = 2;
		return request;
	}

	json EngineFrame(uint32_t ordinal, uint64_t engineFrame, uint64_t micros, const char* state, json artifact = nullptr)
	{
		json frame = { { "ordinal", ordinal }, { "engineFrame", engineFrame }, { "monotonicTimestampUs", micros }, { "state", state } };
		if (!artifact.is_null())
			frame["artifact"] = std::move(artifact);
		return frame;
	}
}

TEST_CASE("burst requests are validated before the engine sees them", "[screenshot-burst]")
{
	REQUIRE(Validate(Valid()).empty());
	auto request = Valid();
	request.requestId.clear();
	REQUIRE_FALSE(Validate(request).empty());
	request = Valid();
	request.prefix = "a/b";
	REQUIRE_FALSE(Validate(request).empty());
	request = Valid();
	request.outputDir = "relative/dir";
	REQUIRE_FALSE(Validate(request).empty());
	request = Valid();
	request.count = 0;
	REQUIRE_FALSE(Validate(request).empty());
	request.count = MaximumCount + 1;
	REQUIRE_FALSE(Validate(request).empty());
	request = Valid();
	request.everyNFrames = 0;
	REQUIRE_FALSE(Validate(request).empty());
}

TEST_CASE("a burst starts a whole-frame PNG sequence on the matching source", "[screenshot-burst]")
{
	const auto flat = SequenceStartCommand(Valid(), false);
	REQUIRE(flat.at("action") == "sequence_start");
	REQUIRE(flat.at("commandId") == "burst-1");
	const auto& sequence = flat.at("sequence");
	REQUIRE(sequence.at("frameCount") == 3);
	REQUIRE(sequence.at("schedule").at("intervalFrames") == 2);
	REQUIRE(sequence.at("schedule").at("basis") == "game_frames");
	REQUIRE(sequence.at("capture").at("source").at("kind") == "desktop_mirror");
	REQUIRE(sequence.at("capture").at("outputs")[0].at("encoding").at("format") == "png");
	REQUIRE_FALSE(sequence.contains("burst"));

	const auto vr = SequenceStartCommand(Valid(), true);
	REQUIRE(vr.at("sequence").at("capture").at("source").at("kind") == "hmd_submission");
	REQUIRE(vr.at("sequence").at("capture").at("outputs")[0].at("view") == "side_by_side");
}

TEST_CASE("the engine manifest becomes numbered frames, drops and a burst manifest", "[screenshot-burst]")
{
	const json artifact = { { "path", "C:/Captures/run/CS_sequence_x/frame_000001.png" }, { "width", 2560 }, { "height", 1440 } };
	json third = artifact;
	third["path"] = "C:/Captures/run/CS_sequence_x/frame_000003.png";
	const json engine = { { "frames", json::array({ EngineFrame(1, 100, 1'000'000, "written", artifact),
										  EngineFrame(2, 102, 1'033'000, "dropped"),
										  EngineFrame(3, 104, 1'070'000, "written", third),
										  EngineFrame(4, 106, 1'100'000, "failed") }) } };
	const auto converted = Convert(engine, Valid());

	REQUIRE(converted.counts.captured == 2);
	REQUIRE(converted.counts.dropped == 1);
	REQUIRE(converted.counts.failed == 1);
	REQUIRE(converted.counts.width == 2560);
	REQUIRE(converted.counts.height == 1440);

	REQUIRE(converted.files.size() == 2);
	REQUIRE(converted.files[0].target.filename() == "shot_0000.png");
	REQUIRE(converted.files[1].target.filename() == "shot_0002.png");
	REQUIRE(converted.files[1].source.filename() == "frame_000003.png");

	const auto& manifest = converted.manifest;
	REQUIRE(manifest.at("requestId") == "burst-1");
	REQUIRE(manifest.at("everyNFrames") == 2);
	REQUIRE(manifest.at("format") == "png");
	REQUIRE(manifest.at("frames").size() == 2);
	REQUIRE(manifest.at("frames")[1].at("file") == "shot_0002.png");
	REQUIRE(manifest.at("frames")[0].at("gameFrame") == 100);
	REQUIRE(manifest.at("frames")[0].at("wallMs").get<double>() == 0.0);
	REQUIRE(manifest.at("frames")[1].at("wallMs").get<double>() == 70.0);
	REQUIRE(manifest.at("dropped").size() == 1);
	REQUIRE(manifest.at("dropped")[0].at("index") == 1);
}

TEST_CASE("a manifest with no usable frames reports no captures", "[screenshot-burst]")
{
	const auto converted = Convert(json{ { "error", "storage" } }, Valid());
	REQUIRE(converted.counts.captured == 0);
	REQUIRE(converted.files.empty());
	REQUIRE(converted.manifest.at("frames").empty());
}

TEST_CASE("the burst manifest reports uiExcluded only when every plane was composed without UI", "[screenshot-burst]")
{
	const auto frame = [](bool a_excluded) {
		json artifact = { { "path", "C:/Captures/run/CS_sequence_x/frame_000001.png" }, { "width", 4 }, { "height", 2 },
			{ "planes", json::array({ { { "uiExcluded", a_excluded } } }) } };
		return json{ { "frames", json::array({ EngineFrame(1, 10, 0, "written", artifact) }) } };
	};
	REQUIRE(Convert(frame(true), Valid()).manifest.at("uiExcluded") == true);
	REQUIRE(Convert(frame(false), Valid()).manifest.at("uiExcluded") == false);
	REQUIRE(Convert(json{ { "frames", json::array() } }, Valid()).manifest.at("uiExcluded") == false);
}

#endif
