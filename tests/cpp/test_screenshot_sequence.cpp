#include "Features/Screenshot/SequenceContract.h"
#include "Features/Screenshot/SequencePolicy.h"
#include "Features/Screenshot/Service.h"
#include "Features/Screenshot/Storage.h"
#include "Utils/FileDigest.h"
#include "screenshot_burst_test.h"
#include "service_retry_test.h"
#include <Windows.h>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <limits>

namespace
{
	using json = nlohmann::json;
	using namespace OS::Capture;

	json Plan(bool vr = true)
	{
		return {
			{ "frameCount", 120 },
			{ "capture", { { "source", { { "kind", vr ? "hmd_submission" : "desktop_mirror" }, { "fallback", "reject" } } },
							 { "outputs", json::array({ { { "view", vr ? "side_by_side" : "source_native" },
											  { "encoding", { { "format", "bmp" }, { "colourContract", "sdr_srgb" } } } } }) },
							 { "destination", { { "directory", "C:/Captures" }, { "overwrite", "never" } } } } }
		};
	}

	json Command()
	{
		return { { "contractMajor", 1 }, { "clientId", "test" }, { "commandId", "start" }, { "action", "sequence_start" } };
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / ("os-sequence-test-" + ServiceFoundation::NewId());
		TemporaryDirectory() { std::filesystem::create_directory(path); }
		~TemporaryDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};
}

TEST_CASE("Sequence plans preserve the explicit runtime source", "[screenshot-sequence]")
{
	CHECK(ParsePlan(Plan(), true).source == "hmd_submission");
	CHECK(ParsePlan(Plan(false), false).source == "desktop_mirror");
	CHECK_THROWS(ParsePlan(Plan(), false));
	CHECK_THROWS(ParsePlan(Plan(false), true));
	auto plan = Plan();
	plan["capture"]["outputs"][0]["encoding"]["format"] = "png";
	CHECK(ParsePlan(plan, true).png);
	plan["capture"]["outputs"][0]["view"] = "left_eye";
	CHECK_THROWS(ParsePlan(plan, true));
}

TEST_CASE("DevBench discovery advertises the implemented sequence bounds", "[screenshot-sequence]")
{
	const auto schema = json::parse(SequenceToolDescription).at("inputSchema");
	const auto& sequence = schema.at("properties").at("sequence").at("properties");
	CHECK(sequence.at("frameCount").at("maximum") == MaximumFrames);
	CHECK(sequence.at("schedule").at("properties").at("intervalFrames").at("maximum") == MaximumSpanFrames);
	CHECK(schema.at("additionalProperties") == false);
	CHECK(sequence.at("capture").at("additionalProperties") == false);
	auto defaults = Plan();
	defaults.erase("frameCount");
	CHECK(sequence.at("frameCount").at("default") == ParsePlan(defaults, true).frameCount);
}

TEST_CASE("Sequence bounds reject invalid or silently lossy requests", "[screenshot-sequence]")
{
	for (const auto value : { json(-1), json(0), json(1.5), json(10'001), json(UINT64_MAX) }) {
		auto plan = Plan();
		plan["frameCount"] = value;
		CHECK_THROWS(ParsePlan(plan, true));
	}
	auto plan = Plan();
	plan["schedule"] = { { "intervalFrames", 216'000 }, { "startDelayFrames", 1 } };
	plan["frameCount"] = 2;
	CHECK_THROWS(ParsePlan(plan, true));
	plan["schedule"]["startDelayFrames"] = 0;
	CHECK_NOTHROW(ParsePlan(plan, true));
	for (const auto& [path, value] : std::vector<std::pair<json::json_pointer, json>>{
			 { json::json_pointer("/capture/source/fallback"), "mirror" },
			 { json::json_pointer("/capture/outputs/0/resize"), { { "width", 100 } } },
			 { json::json_pointer("/capture/outputs/0/encoding/format"), "jpeg" },
			 { json::json_pointer("/capture/outputs/0/encoding/colourContract"), "hdr" },
			 { json::json_pointer("/capture/destination/overwrite"), "replace" },
			 { json::json_pointer("/capture/destination/directory"), std::string("C:/capture\0hidden", 17) },
			 { json::json_pointer("/packaging/frameManifest"), false },
			 { json::json_pointer("/packaging/previewVideo/requested"), true },
			 { json::json_pointer("/useSettings"), true },
			 { json::json_pointer("/backpressure/policy"), "wait" },
			 { json::json_pointer("/schedule/pausePolicy"), "capture" },
			 { json::json_pointer("/schedule/intervalFrames"), -1 },
			 { json::json_pointer("/typo"), 1 } }) {
		auto invalid = Plan();
		invalid[path] = value;
		INFO(path.to_string());
		CHECK_THROWS(ParsePlan(invalid, true));
	}
}

TEST_CASE("Late frame observations retain missing cadence slots", "[screenshot-sequence]")
{
	SequenceSchedule schedule;
	schedule.plan.frameCount = 4;
	schedule.plan.intervalFrames = 2;
	schedule.nextFrame = 100;
	CHECK_FALSE(schedule.Due(99));
	REQUIRE(schedule.Due(100));
	schedule.Advance();
	std::vector<uint64_t> missed;
	while (schedule.Due(106) && schedule.nextFrame < 106) {
		missed.push_back(schedule.nextFrame);
		schedule.Advance();
	}
	CHECK(missed == std::vector<uint64_t>{ 102, 104 });
	CHECK(schedule.nextOrdinal == 4);
	CHECK(schedule.nextFrame == 106);
	schedule.Advance();
	CHECK(schedule.Finished());
	CHECK_FALSE(schedule.Due(1000));
}

TEST_CASE("Stereo pairing rejects duplicate eyes and different engine frames", "[screenshot-sequence]")
{
	CHECK(CanAcceptEye(0, 0, 42, 42));
	CHECK(CanAcceptEye(1, 1, 42, 42));
	CHECK(CanAcceptEye(0, 2, 42, 42));
	CHECK_FALSE(CanAcceptEye(0, 1, 42, 42));
	CHECK_FALSE(CanAcceptEye(1, 1, 43, 42));
	CHECK_FALSE(CanAcceptEye(2, 0, 42, 42));
}

TEST_CASE("Native submission bounds preserve orientation and reject invalid dimensions", "[screenshot-sequence]")
{
	const auto left = ResolveBounds(400, 200, { 0, 0, .5f, 1 });
	const auto right = ResolveBounds(400, 200, { 1, 1, .5f, 0 });
	REQUIRE(left);
	REQUIRE(right);
	CHECK(left->x == 0);
	CHECK(left->width == 200);
	CHECK(right->x == 200);
	CHECK(right->width == 200);
	CHECK(right->flipX);
	CHECK(right->flipY);
	CHECK_FALSE(ResolveBounds(400, 200, { 0, 0, 0, 1 }));
	CHECK_FALSE(ResolveBounds(400, 200, { -.1f, 0, 1, 1 }));
	CHECK_FALSE(ResolveBounds(400, 200, { 0, 0, std::numeric_limits<float>::quiet_NaN(), 1 }));
	CHECK_FALSE(ResolveBounds(UINT32_MAX, 200, { 0, 0, 1, 1 }));
	CHECK_FALSE(ResolveBounds(0, 200, { 0, 0, 1, 1 }));
}

TEST_CASE("Readback conversion respects row pitch, bounds orientation and channel order", "[screenshot-sequence]")
{
	const std::vector<uint8_t> source{ 3, 2, 1, 4, 7, 6, 5, 8, 99, 99, 99, 99,
		11, 10, 9, 12, 15, 14, 13, 16 };
	const PixelBounds bounds{ 0, 0, 2, 2, true, true };
	CHECK(CopyPixels(source, 12, bounds, true) == std::vector<uint8_t>{
													  13, 14, 15, 255, 9, 10, 11, 255, 5, 6, 7, 255, 1, 2, 3, 255 });
	CHECK_THROWS(CopyPixels(source, 4, bounds, true));
	CHECK_THROWS(CopyPixels(std::span(source).first(12), 12, bounds, true));
}

TEST_CASE("Command retries cannot start a second capture", "[screenshot-sequence]")
{
	ServiceFoundation service({ "openshaders.screenshot", 1, 0, 1 });
	auto command = Command();
	int calls = 0;
	auto handler = [&](const json& args) {
		++calls;
		auto result = service.MakeEnvelope(args, true);
		result["result"] = { { "requestId", "sequence-1" }, { "state", "recording" } };
		return result;
	};
	CHECK(service.Dispatch(command, handler)["ok"] == true);
	const auto retry = service.Dispatch(command, handler, [](std::string_view) { return json{ { "state", "completed" } }; });
	CHECK(retry["result"]["state"] == "completed");
	CHECK(retry["result"]["idempotentReplay"] == true);
	CHECK(calls == 1);
	CHECK(service.Dispatch(command, handler, [](std::string_view) { return json(nullptr); })["error"]["code"] == "receipt_expired");
	command["sequence"] = Plan();
	CHECK(service.Dispatch(command, handler)["error"]["code"] == "idempotency_conflict");
	CHECK(calls == 1);
	command["contractMajor"] = 1.0;
	CHECK(service.Dispatch(command, handler)["error"]["code"] == "unsupported_contract_version");
}

TEST_CASE("Invalid capture plans return non-retryable validation errors", "[screenshot-sequence]")
{
	ServiceFoundation service({ "openshaders.screenshot", 1, 0, 1 });
	const auto result = service.Dispatch(Command(), [](const json&) -> json {
		throw std::invalid_argument("unsupported capture");
	});
	CHECK(result["ok"] == false);
	CHECK(result["error"]["code"] == "invalid_request");
	CHECK(result["error"]["retryable"] == false);
}

TEST_CASE("Sequence storage publishes verified files without replacing existing evidence", "[screenshot-sequence]")
{
	using namespace OS::CaptureStorage;
	TemporaryDirectory directory;
	const auto lease = DirectoryLease::CreateExclusive(directory.path, "test-1");
	CHECK_THROWS(DirectoryLease::CreateExclusive(directory.path, "test-1"));
	CHECK_THROWS(DirectoryLease::CreateExclusive(directory.path, "../escape"));
	CHECK_THROWS(lease->VerifyDirectChild(directory.path / "outside.bmp"));
	const auto path = lease->Path() / "frame.bmp";
	const auto temporary = lease->Path() / "frame.writing";
	lease->VerifyDirectChild(path);
	const std::string content = "abc";
	const auto artifact = CommittedFile::WriteAtomically(temporary, path, content.data(), content.size(), false);
	CHECK(artifact.bytes == 3);
	CHECK(artifact.sha256 == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
	CHECK_FALSE(std::filesystem::exists(temporary));
	CHECK(Util::FileDigest::Sha256FileHex(path) == artifact.sha256);
	CHECK(CommittedFile::Open(path).Describe().sha256 == artifact.sha256);
	CHECK_THROWS(CommittedFile::WriteAtomically(temporary, path, "bad", 3, false));
	CHECK_FALSE(std::filesystem::exists(temporary));
	CHECK(Util::FileDigest::Sha256FileHex(path) == artifact.sha256);
	std::error_code renameError;
	std::filesystem::rename(lease->Path(), directory.path / "renamed", renameError);
	CHECK(renameError);
	CHECK_NOTHROW(lease->Verify());
}

TEST_CASE("Digest failures stay unavailable and empty files hash correctly", "[screenshot-sequence]")
{
	TemporaryDirectory directory;
	CHECK_FALSE(Util::FileDigest::Sha256FileHex(directory.path / "missing"));
	CHECK_FALSE(Util::FileDigest::Sha256HandleHex(INVALID_HANDLE_VALUE));
	const auto path = directory.path / "empty";
	std::ofstream(path, std::ios::binary).close();
	CHECK(Util::FileDigest::Sha256FileHex(path) == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
}

TEST_CASE("Burst budgets, native atlas orientation and continuity", "[screenshot-sequence][screenshot-burst]")
{
	REQUIRE_NOTHROW(RunScreenshotBurstTests());
	auto request = Plan();
	request["burst"] = { { "regions", json::array({ { { "x", 0 }, { "y", 0 }, { "width", 384 }, { "height", 256 } } }) } };
	CHECK(ParsePlan(request, true).burst.payloadBytes == 120ull * 384 * 256 * 8);
	auto implicitLength = request;
	implicitLength.erase("frameCount");
	CHECK_THROWS(ParsePlan(implicitLength, true));
	implicitLength.erase("burst");
	CHECK(ParsePlan(implicitLength, true).frameCount == 30);
	request["schedule"]["intervalFrames"] = 2;
	CHECK_THROWS(ParsePlan(request, true));
}

TEST_CASE("Golden reference requests retain host identity and native output", "[screenshot-sequence]")
{
	TemporaryDirectory folder;
	const auto path = folder.path / L"r\u00E9f\u56FE.png";
	const auto text = Util::PathToUtf8(path);
	CHECK(std::filesystem::u8path(text) == path);
	const json request = { { "outputPath", text }, { "requestId", "checkpoint#1#4" } };
	for (bool vr : { false, true }) {
		const auto command = ReferenceCommand(request, vr);
		const auto plan = ParsePlan(command.at("sequence"), vr);
		CHECK(plan.frameCount == 1);
		CHECK(plan.png);
		CHECK(command.at("reference").at("requestId") == "checkpoint#1#4");
		CHECK(command.at("reference").at("outputPath") == text);
	}
	for (const auto invalid : { "relative.png", "C:/capture.bmp", "" }) {
		auto args = request;
		args["outputPath"] = invalid;
		CHECK_THROWS(ReferenceCommand(args, true));
	}
	auto args = request;
	args["requestId"] = std::string(129, 'a');
	CHECK_THROWS(ReferenceCommand(args, true));
	args = request;
	args["outputPath"] = text + std::string(1, '\0');
	CHECK_THROWS(ReferenceCommand(args, true));
	const auto lease = OS::CaptureStorage::DirectoryLease::CreateExclusive(folder.path, "reference");
	CHECK_NOTHROW(lease->VerifyDestinationChild(path));
	CHECK_THROWS(lease->VerifyDirectChild(path));
	CHECK_THROWS(lease->VerifyDestinationChild(folder.path.parent_path() / "escaped.png"));
}

TEST_CASE("Retryable dispatch failures release command reservations", "[screenshot-sequence]")
{
	ServiceFoundation service({ "openshaders.screenshot", 1, 0, 1 });
	CheckRetryableCommands(service, [](bool ok, const char* message) {
		INFO(message);
		REQUIRE(ok);
	});
}
