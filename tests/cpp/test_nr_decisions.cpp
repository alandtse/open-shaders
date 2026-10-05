// Unit tests for the extracted NR decisions: the hook's per-frame action, the
// failure outcome, history-reset reasons, the runtime/version gates, the shared
// eye width and the output check. These exercise the production helpers only.

#include "Features/Upscaling/NeuralRendering/Diagnostics.h"
#include "Features/Upscaling/NeuralRendering/Lifecycle.h"
#include "Features/Upscaling/NeuralRendering/Runtime.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace
{
	/** @brief The accepted runtime version, as the version-info API returns it. */
	struct FakeVersion
	{
		uint32_t majorVersion = 310, minorVersion = 8;
		uint32_t major() const { return majorVersion; }
		uint32_t minor() const { return minorVersion; }
		std::string string(std::string_view separator) const { return std::format("{}{}{}.0", majorVersion, separator, minorVersion); }
	};

	/** @brief A frame that is enabled, rendering a world, with an initialized runtime. */
	NR::FrameInputs RunningFrame(uint32_t frame)
	{
		NR::FrameInputs inputs;
		inputs.enabled = true;
		inputs.worldRendered = true;
		inputs.ready = true;
		inputs.lastFrame = frame - 1;
		inputs.frameCount = frame;
		return inputs;
	}
}

TEST_CASE("DecideFrame frees pass resources when off and skips until a world is drawn", "[nr]")
{
	const auto disabled = NR::DecideFrame({ .enabled = false });
	REQUIRE(disabled == NR::FrameAction::ReleasePassResources);

	// Disabled wins whatever the runtime state is: it must not initialize, run or latch.
	NR::FrameInputs latched;
	latched.failed = latched.ready = true;
	latched.retryRequested = true;
	REQUIRE(NR::DecideFrame(latched) == NR::FrameAction::ReleasePassResources);

	auto waitingForWorld = RunningFrame(7);
	waitingForWorld.worldRendered = false;
	REQUIRE(NR::DecideFrame(waitingForWorld) == NR::FrameAction::SkipNoWorld);
}

TEST_CASE("DecideFrame skips a second hook call for one frameCount", "[nr]")
{
	auto duplicate = RunningFrame(42);
	duplicate.lastFrame = 42;
	REQUIRE(NR::DecideFrame(duplicate) == NR::FrameAction::SkipDuplicate);

	// The next frame of the same run is not a duplicate.
	REQUIRE(NR::DecideFrame(RunningFrame(43)) == NR::FrameAction::Run);
}

TEST_CASE("DecideFrame latches a failure until a retry is requested", "[nr]")
{
	auto failed = RunningFrame(10);
	failed.failed = true;
	REQUIRE(NR::DecideFrame(failed) == NR::FrameAction::SkipLatched);

	failed.retryRequested = true;
	REQUIRE(NR::DecideFrame(failed) == NR::FrameAction::RebuildThenRun);
}

TEST_CASE("DecideFrame rebuilds only a latched runtime and never caps retries", "[nr]")
{
	// A retry with no latched failure must not tear the runtime down.
	auto healthy = RunningFrame(11);
	healthy.retryRequested = true;
	REQUIRE(NR::DecideFrame(healthy) == NR::FrameAction::Run);

	// Retry stays available for as many failed cycles as the user asks for.
	for (uint32_t attempt = 0; attempt < 8; ++attempt) {
		auto failed = RunningFrame(attempt + 20);
		failed.failed = true;
		REQUIRE(NR::DecideFrame(failed) == NR::FrameAction::SkipLatched);
		failed.retryRequested = true;
		REQUIRE(NR::DecideFrame(failed) == NR::FrameAction::RebuildThenRun);
	}
}

TEST_CASE("DecideFrame initializes on the first world frame and runs after", "[nr]")
{
	auto cold = RunningFrame(1);
	cold.ready = false;
	cold.lastFrame = UINT32_MAX;
	REQUIRE(NR::DecideFrame(cold) == NR::FrameAction::InitializeThenRun);
}

TEST_CASE("DecideFrame suspends without freeing pass resources while dialogue-only gating holds", "[nr]")
{
	auto suspended = RunningFrame(50);
	suspended.suspended = true;
	REQUIRE(NR::DecideFrame(suspended) == NR::FrameAction::Suspend);

	// Suspension outranks the world check: the pass stays initialized instead of looking not yet started.
	suspended.worldRendered = false;
	REQUIRE(NR::DecideFrame(suspended) == NR::FrameAction::Suspend);

	// Switching the feature off still frees the pass resources, suspension or not.
	suspended.enabled = false;
	REQUIRE(NR::DecideFrame(suspended) == NR::FrameAction::ReleasePassResources);
}

TEST_CASE("ResumesFromSuspend flags exactly the first frame after a suspension", "[nr]")
{
	bool wasSuspended = false;
	REQUIRE_FALSE(NR::ResumesFromSuspend(false, wasSuspended));
	REQUIRE_FALSE(NR::ResumesFromSuspend(true, wasSuspended));
	REQUIRE_FALSE(NR::ResumesFromSuspend(true, wasSuspended));
	REQUIRE(NR::ResumesFromSuspend(false, wasSuspended));
	REQUIRE_FALSE(NR::ResumesFromSuspend(false, wasSuspended));
}

TEST_CASE("OnFailure tears down only for a removed device", "[nr]")
{
	REQUIRE(NR::OnFailure(true) == NR::FailureAction::TeardownThenLatch);
	REQUIRE(NR::OnFailure(false) == NR::FailureAction::Latch);
}

TEST_CASE("FrameResetReasons covers the request, the first frame and a frame gap", "[nr]")
{
	using NR::Diagnostics::FirstFrame;
	using NR::Diagnostics::FrameGap;
	using NR::Diagnostics::Requested;

	REQUIRE(NR::Diagnostics::FrameResetReasons(false, UINT32_MAX, 100) == FirstFrame);
	REQUIRE(NR::Diagnostics::FrameResetReasons(false, 100, 101) == 0);
	REQUIRE(NR::Diagnostics::FrameResetReasons(false, 100, 102) == FrameGap);
	REQUIRE(NR::Diagnostics::FrameResetReasons(false, 100, 99) == FrameGap);
	REQUIRE(NR::Diagnostics::FrameResetReasons(true, 100, 101) == Requested);
	REQUIRE(NR::Diagnostics::FrameResetReasons(true, UINT32_MAX, 100) == (Requested | FirstFrame));
}

TEST_CASE("Only a 310.8 runtime is accepted", "[nr]")
{
	REQUIRE(NR::UnsupportedRuntimeReason(std::optional<FakeVersion>{ FakeVersion{ 310, 8 } }, "C:\\game").empty());
	REQUIRE(NR::UnsupportedRuntimeReason(std::optional<FakeVersion>{ FakeVersion{ 310, 8 } }, "").empty());

	for (const auto& rejected : { FakeVersion{ 310, 7 }, FakeVersion{ 310, 9 }, FakeVersion{ 311, 8 }, FakeVersion{ 309, 8 } }) {
		const auto reason = NR::UnsupportedRuntimeReason(std::optional<FakeVersion>{ rejected }, "C:\\game");
		REQUIRE_FALSE(reason.empty());
		REQUIRE(reason == std::format("unsupported runtime version {} (needs 310.8)", rejected.string(".")));
	}

	REQUIRE(NR::UnsupportedRuntimeReason(std::optional<FakeVersion>{}, "C:\\game") ==
			"nvngx_dlssnr.dll in C:\\game has no version information");
}

TEST_CASE("ClassifyRuntime names the gate that refuses a runtime", "[nr]")
{
	using NR::RuntimeAvailability;

	const auto accepted = std::optional<FakeVersion>{ FakeVersion{ 310, 8 } };
	const auto validated = NR::kValidatedRuntimeSha256[0];

	// No file is refused before anything can be read from it, whatever digest was passed.
	const auto missing = NR::ClassifyRuntime(false, std::optional<FakeVersion>{}, validated, "C:\\game");
	REQUIRE(missing.state == RuntimeAvailability::State::kMissing);
	REQUIRE(missing.version.empty());
	REQUIRE(missing.reason == "nvngx_dlssnr.dll not found in C:\\game");

	// A version the pass does not accept is refused by version, even for a pinned build.
	const auto versioned = NR::ClassifyRuntime(true, std::optional<FakeVersion>{ FakeVersion{ 310, 9 } }, validated, "C:\\game");
	REQUIRE(versioned.state == RuntimeAvailability::State::kUnsupportedVersion);
	REQUIRE(versioned.version == "310.9.0");
	REQUIRE(versioned.reason == "unsupported runtime version 310.9.0 (needs 310.8)");

	// An unpinned build of the accepted version is refused by digest, and an unreadable
	// digest is refused too rather than being treated as unchanged.
	for (const std::string refused : { std::string{}, std::string(64, '0') }) {
		const auto build = NR::ClassifyRuntime(true, accepted, refused, "C:\\game");
		REQUIRE(build.state == RuntimeAvailability::State::kUnvalidatedBuild);
		REQUIRE(build.version == "310.8.0");
		REQUIRE_FALSE(build.Ready());
	}

	// The pinned build of the accepted version is the only combination that is ready.
	const auto ready = NR::ClassifyRuntime(true, accepted, validated, "C:\\game");
	REQUIRE(ready.state == RuntimeAvailability::State::kReady);
	REQUIRE(ready.Ready());
	REQUIRE(ready.version == "310.8.0");
	REQUIRE(ready.reason.empty());
}

TEST_CASE("Only a validated runtime loads unless developer mode forces one", "[nr]")
{
	using NR::RuntimeAvailability;
	const auto verdict = [](RuntimeAvailability::State state) {
		RuntimeAvailability availability;
		availability.state = state;
		return availability;
	};

	REQUIRE(verdict(RuntimeAvailability::State::kReady).AllowsLoad(false));
	REQUIRE(verdict(RuntimeAvailability::State::kReady).AllowsLoad(true));

	// A build the pass refuses is a developer's to force, and nobody else's.
	for (const auto refused : { RuntimeAvailability::State::kUnsupportedVersion, RuntimeAvailability::State::kUnvalidatedBuild }) {
		REQUIRE_FALSE(verdict(refused).AllowsLoad(false));
		REQUIRE(verdict(refused).AllowsLoad(true));
	}

	// There is nothing to load without the file, whatever developer mode says.
	REQUIRE_FALSE(verdict(RuntimeAvailability::State::kMissing).AllowsLoad(false));
	REQUIRE_FALSE(verdict(RuntimeAvailability::State::kMissing).AllowsLoad(true));
}

TEST_CASE("Only the validated runtime builds are accepted", "[nr]")
{
	REQUIRE(NR::kValidatedRuntimeSha256.size() == 2);
	for (const auto validated : NR::kValidatedRuntimeSha256) {
		REQUIRE(NR::IsValidatedRuntimeHash(validated));

		// Hex carries no case, so a lowercase digest names the same build.
		std::string lowered(validated);
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		REQUIRE(NR::IsValidatedRuntimeHash(lowered));
	}

	// A different 310.8.x build, an empty digest, and one short or long by a single
	// character are all refused rather than prefix-matched.
	REQUIRE_FALSE(NR::IsValidatedRuntimeHash("0000000000000000000000000000000000000000000000000000000000000000"));
	REQUIRE_FALSE(NR::IsValidatedRuntimeHash(""));
	REQUIRE_FALSE(NR::IsValidatedRuntimeHash("8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC20"));
	REQUIRE_FALSE(NR::IsValidatedRuntimeHash("8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC2060"));
}

TEST_CASE("IsUnderDriverStore is case-insensitive and anchored to the prefix", "[nr]")
{
	const std::wstring_view systemDirectory = L"C:\\Windows\\System32";

	REQUIRE(NR::IsUnderDriverStore(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nvngx.dll", systemDirectory));
	REQUIRE(NR::IsUnderDriverStore(L"c:\\windows\\system32\\driverstore\\f\\nvngx.dll", systemDirectory));
	REQUIRE(NR::IsUnderDriverStore(L"C:\\WINDOWS\\SYSTEM32\\DriverStore\\", systemDirectory));

	// A lookalike folder, a module outside it, the prefix without its trailing separator
	// and anything shorter than the prefix all fail.
	REQUIRE_FALSE(NR::IsUnderDriverStore(L"C:\\Windows\\System32\\DriverStoreX\\nvngx.dll", systemDirectory));
	REQUIRE_FALSE(NR::IsUnderDriverStore(L"C:\\Windows\\System32\\nvngx.dll", systemDirectory));
	REQUIRE_FALSE(NR::IsUnderDriverStore(L"C:\\Windows\\System32\\DriverStore", systemDirectory));
	REQUIRE_FALSE(NR::IsUnderDriverStore(L"C:\\Windows\\System32", systemDirectory));
	REQUIRE_FALSE(NR::IsUnderDriverStore(L"", systemDirectory));
}

TEST_CASE("EyeRenderWidth splits the stereo size as Upscale does", "[nr]")
{
	REQUIRE(NR::EyeRenderWidth(3840, 2) == 1920);
	REQUIRE(NR::EyeRenderWidth(2560, 2) == 1280);
	REQUIRE(NR::EyeRenderWidth(3840, 1) == 3840);
	REQUIRE(NR::EyeRenderWidth(0, 2) == 0);

	// The width Upscale() derives as (uint32_t)(renderSize.x / numEyes): truncating the
	// stereo width before the split has to agree for every size this pass can see.
	for (uint32_t renderWidth : { 1u, 1920u, 1921u, 2560u, 3840u, 3841u, 5120u, 7680u, 7681u })
		for (uint32_t eyes : { 1u, 2u })
			REQUIRE(NR::EyeRenderWidth(renderWidth, eyes) == static_cast<uint32_t>(static_cast<float>(renderWidth) / static_cast<float>(eyes)));
}

TEST_CASE("IsSupportedOutput accepts the whitelisted formats and shapes", "[nr]")
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = 3840;
	desc.Height = 2160;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	REQUIRE(NR::IsSupportedOutput(desc, 1920, 2160, 2));
	REQUIRE(NR::IsSupportedOutput(desc, 3840, 2160, 1));

	for (const auto format : { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R11G11B10_FLOAT }) {
		desc.Format = format;
		REQUIRE(NR::IsSupportedOutput(desc, 1920, 2160, 2));
	}

	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 1920, 2160, 2));
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	desc.ArraySize = 2;
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 1920, 2160, 2));
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 2;
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 1920, 2160, 2));
	desc.SampleDesc.Count = 1;

	// The per-eye width has to fit the texture, and a zero extent is never usable.
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 1921, 2160, 2));
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 1920, 2161, 2));
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 0, 2160, 2));
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 1920, 0, 2));

	desc.Width = 0;
	REQUIRE_FALSE(NR::IsSupportedOutput(desc, 1920, 2160, 2));
}

TEST_CASE("Tuning::Sanitize bounds every strength knob", "[nr]")
{
	NR::Tuning tuning;
	tuning.style = 7;
	tuning.intensity = 5.0f;
	tuning.localToneStrength = -3.0f;
	tuning.localStructureStrength = std::numeric_limits<float>::quiet_NaN();
	tuning.skinStructureStrength = 9.0f;
	tuning.Sanitize();
	REQUIRE(tuning.style == NR::Tuning::kMaxStyle);
	REQUIRE(tuning.intensity == NR::Tuning::kMaxStrength);
	REQUIRE(tuning.localToneStrength == NR::Tuning::kMinStrength);
	REQUIRE(tuning.localStructureStrength == NR::Tuning::kDefaultStrength);
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kMaxStrength);

	tuning.intensity = std::numeric_limits<float>::infinity();
	tuning.localToneStrength = -std::numeric_limits<float>::infinity();
	tuning.skinStructureStrength = std::numeric_limits<float>::quiet_NaN();
	tuning.Sanitize();
	REQUIRE(tuning.intensity == NR::Tuning::kDefaultStrength);
	REQUIRE(tuning.localToneStrength == NR::Tuning::kDefaultStrength);
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);

	// "Auto" must survive sanitizing; a value below it clamps up to it.
	tuning.skinStructureStrength = NR::Tuning::kAutomaticSkinStructure;
	tuning.Sanitize();
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);
	tuning.skinStructureStrength = -5.0f;
	tuning.Sanitize();
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);

	// In-range values are left alone.
	tuning.intensity = 1.5f;
	tuning.localToneStrength = 0.25f;
	tuning.style = 1;
	tuning.Sanitize();
	REQUIRE(tuning.intensity == Catch::Approx(1.5f));
	REQUIRE(tuning.localToneStrength == Catch::Approx(0.25f));
	REQUIRE(tuning.style == 1);
}
