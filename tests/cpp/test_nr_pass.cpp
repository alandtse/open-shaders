#include "Features/Upscaling/NeuralRendering/ColorContract.h"
#include "Features/Upscaling/NeuralRendering/FramePlan.h"
#include "Features/Upscaling/NeuralRendering/Lifecycle.h"
#include "Features/Upscaling/NeuralRendering/Parameters.h"
#include "Features/Upscaling/NeuralRendering/ProxyContract.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace
{
	using Catch::Approx;
	using NR::ExposureSource;
	using NR::FrameAction;
	using NR::FrameInputs;
	using NR::ProxyContract;

	/** @brief Frame inputs that would run, so each case only has to break one of them. */
	FrameInputs Runnable()
	{
		FrameInputs inputs;
		inputs.enabled = true;
		inputs.worldRendered = true;
		inputs.adapterSupported = true;
		return inputs;
	}
}

TEST_CASE("The frame decision skips before it would run anything", "[nr][nr-frame]")
{
	REQUIRE(NR::DecideFrameAction(Runnable()) == FrameAction::kRun);

	SECTION("the feature toggle wins over every other input")
	{
		auto inputs = Runnable();
		inputs.enabled = false;
		inputs.worldRendered = false;
		inputs.adapterSupported = false;
		inputs.failed = true;
		inputs.retryRequested = true;
		REQUIRE(NR::DecideFrameAction(inputs) == FrameAction::kSkipDisabled);
	}

	SECTION("a frame with no world never runs, however it is otherwise configured")
	{
		auto inputs = Runnable();
		inputs.worldRendered = false;
		inputs.retryRequested = true;
		REQUIRE(NR::DecideFrameAction(inputs) == FrameAction::kSkipNoWorld);
	}

	SECTION("a non-NVIDIA adapter never runs")
	{
		auto inputs = Runnable();
		inputs.adapterSupported = false;
		inputs.retryRequested = true;
		REQUIRE(NR::DecideFrameAction(inputs) == FrameAction::kSkipUnsupported);
	}

	SECTION("a queued retry is not swallowed by the latch it clears")
	{
		auto inputs = Runnable();
		inputs.failed = true;
		REQUIRE(NR::DecideFrameAction(inputs) == FrameAction::kSkipLatched);
		inputs.retryRequested = true;
		REQUIRE(NR::DecideFrameAction(inputs) == FrameAction::kRetry);
	}
}

TEST_CASE("A requested scene exposure falls back to the local estimate", "[nr][nr-exposure]")
{
	REQUIRE(NR::ResolveExposureSource(ExposureSource::kScene, true) == ExposureSource::kScene);
	REQUIRE(NR::ResolveExposureSource(ExposureSource::kScene, false) == ExposureSource::kLocal);
	// The other arms are explicit choices, so an absent scene exposure does not override them.
	REQUIRE(NR::ResolveExposureSource(ExposureSource::kLocal, false) == ExposureSource::kLocal);
	REQUIRE(NR::ResolveExposureSource(ExposureSource::kScalar, false) == ExposureSource::kScalar);
	REQUIRE(NR::ResolveExposureSource(ExposureSource::kScalar, true) == ExposureSource::kScalar);
}

TEST_CASE("A new history starts on a reset, a first frame or a frame gap", "[nr][nr-history]")
{
	SECTION("a consecutive frame continues the history")
	{
		REQUIRE_FALSE(NR::NeedsHistoryReset(false, 999, 1000));
	}

	SECTION("an explicit reset always wins")
	{
		REQUIRE(NR::NeedsHistoryReset(true, 999, 1000));
	}

	SECTION("the first frame has no history to continue")
	{
		REQUIRE(NR::NeedsHistoryReset(false, UINT32_MAX, 1));
	}

	SECTION("a frame the pass skipped breaks the history")
	{
		// The pass did not apply on frame 1000, so frame 1001 cannot reproject into its output.
		REQUIRE(NR::NeedsHistoryReset(false, 999, 1001));
		REQUIRE(NR::NeedsHistoryReset(false, 999, 999));
	}
}

TEST_CASE("The exposure grid stays inside its partial buffer", "[nr][nr-exposure]")
{
	SECTION("an empty extent has no grid")
	{
		REQUIRE(NR::ComputeExposureGrid(0, 1080).Empty());
		REQUIRE(NR::ComputeExposureGrid(1920, 0).Empty());
	}

	SECTION("a small source gets at least one group")
	{
		const auto grid = NR::ComputeExposureGrid(16, 16);
		REQUIRE(grid.columns == 1);
		REQUIRE(grid.rows == 1);
		REQUIRE(grid.Count() == 1);
	}

	SECTION("the grid covers the source and stops at the cap")
	{
		// A 4K stereo source is the widest case the pass can see; the grid must still cover it.
		const auto grid = NR::ComputeExposureGrid(7680, 4320);
		REQUIRE(grid.columns == NR::kMaxExposureAxis);
		REQUIRE(grid.rows == NR::kMaxExposureAxis);
		REQUIRE(grid.Count() == NR::kMaxExposureGroups);
		// Every group must own at least one texel, or the reduction would read nothing.
		REQUIRE(grid.columns <= 7680);
		REQUIRE(grid.rows <= 4320);
	}

	SECTION("a tile per group keeps the dispatch proportional to the extent")
	{
		const auto grid = NR::ComputeExposureGrid(1280, 720);
		REQUIRE(grid.columns == 20);
		REQUIRE(grid.rows == 12);
		REQUIRE(grid.Count() <= NR::kMaxExposureGroups);
	}
}

TEST_CASE("The per-eye render width matches the encoder's own expression", "[nr][nr-geometry]")
{
	// floor(x / eyes) and floor(floor(x) / eyes) agree for every x, which is what lets the NR call
	// site and the shared encoder share one expression.
	for (uint32_t raw = 0; raw < 4096; ++raw) {
		const auto asFloat = static_cast<float>(raw);
		REQUIRE(NR::EyeRenderWidth(asFloat, 1) == raw);
		REQUIRE(NR::EyeRenderWidth(asFloat, 2) == raw / 2);
	}
	REQUIRE(NR::EyeRenderWidth(4032.5f, 2) == 2016);
	REQUIRE(NR::EyeRenderWidth(4032.0f, 0) == 0);
}

TEST_CASE("The menu's status line follows what the user can act on", "[nr][nr-status]")
{
	using NR::DisplayState;
	using NR::FailureKind;
	using NR::RuntimeState;

	const auto classify = [](bool a_enabled, bool a_adapter, RuntimeState a_layer, FailureKind a_failure, bool a_passFailed) {
		return NR::ClassifyDisplayState(a_enabled, a_adapter, a_layer, a_failure, a_passFailed);
	};

	SECTION("the toggle wins over everything the pass knows")
	{
		REQUIRE(classify(false, false, RuntimeState::kFailed, FailureKind::kDllMissing, true) == DisplayState::kOff);
	}

	SECTION("an adapter that can have no device is reported before any latch")
	{
		REQUIRE(classify(true, false, RuntimeState::kFailed, FailureKind::kInitFailed, true) == DisplayState::kNoGpu);
	}

	SECTION("an idle layer is starting and a live one is active")
	{
		REQUIRE(classify(true, true, RuntimeState::kNotLoaded, FailureKind::kNone, false) == DisplayState::kStarting);
		REQUIRE(classify(true, true, RuntimeState::kInitialized, FailureKind::kNone, false) == DisplayState::kActive);
	}

	SECTION("the two explainable DLL failures are told apart from the rest")
	{
		REQUIRE(classify(true, true, RuntimeState::kFailed, FailureKind::kDllMissing, false) == DisplayState::kDllMissing);
		REQUIRE(classify(true, true, RuntimeState::kFailed, FailureKind::kVersionRejected, false) == DisplayState::kVersionRejected);
		REQUIRE(classify(true, true, RuntimeState::kFailed, FailureKind::kInitFailed, false) == DisplayState::kFailed);
		REQUIRE(classify(true, true, RuntimeState::kFailed, FailureKind::kSehFault, false) == DisplayState::kFailed);
	}

	SECTION("a pass-level latch outranks a runtime that is still live")
	{
		REQUIRE(classify(true, true, RuntimeState::kInitialized, FailureKind::kNone, true) == DisplayState::kFailed);
	}
}

TEST_CASE("The proxy contract clamps a persisted selector", "[nr][nr-proxy]")
{
	REQUIRE(NR::ClampProxyContract(0) == ProxyContract::kHdr);
	REQUIRE(NR::ClampProxyContract(1) == ProxyContract::kSdr);
	REQUIRE(NR::ClampProxyContract(2) == ProxyContract::kHdr);
	REQUIRE(NR::ClampProxyContract(0xFFFFFFFFu) == ProxyContract::kHdr);

	REQUIRE(NR::ProxyIsHdr(ProxyContract::kHdr));
	REQUIRE_FALSE(NR::ProxyIsHdr(ProxyContract::kSdr));

	SECTION("each pairing drives its own creation flags")
	{
		REQUIRE(NR::ProxyFlags(ProxyContract::kHdr) != NR::ProxyFlags(ProxyContract::kSdr));
		REQUIRE((NR::ProxyFlags(ProxyContract::kHdr) & NR::kHdrProxyFlags) == NR::kHdrProxyFlags);
		REQUIRE((NR::ProxyFlags(ProxyContract::kSdr) & NR::kSdrProxyFlags) == NR::kSdrProxyFlags);
		// The SDR pairing is the HDR one without IsHDR, which is the whole difference between them.
		REQUIRE(NR::kSdrProxyFlags == (NR::kHdrProxyFlags & ~NVSDK_NGX_DLSS_Feature_Flags_IsHDR));
	}
}

TEST_CASE("Tuning sanitizes the selectors and the skin structure sentinel", "[nr][nr-tuning]")
{
	NR::Tuning tuning;
	REQUIRE(tuning.ProxyContractValue() == ProxyContract::kHdr);
	REQUIRE(tuning.ExposureSourceValue() == ExposureSource::kScene);

	SECTION("a value past the enum falls back to the default arm")
	{
		tuning.proxyContract = 7;
		tuning.exposureSource = 9;
		tuning.Sanitize();
		REQUIRE(tuning.ProxyContractValue() == ProxyContract::kHdr);
		REQUIRE(tuning.ExposureSourceValue() == ExposureSource::kScene);
	}

	SECTION("a negative skin structure is always the Auto sentinel")
	{
		// A value inside (-1, 0) is not a strength the runtime accepts, so it must not survive.
		tuning.skinStructureStrength = -0.5f;
		tuning.Sanitize();
		REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);

		tuning.skinStructureStrength = -1.0f;
		tuning.Sanitize();
		REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);

		tuning.skinStructureStrength = 1.5f;
		tuning.Sanitize();
		REQUIRE(tuning.skinStructureStrength == 1.5f);
	}

	SECTION("the strength bounds still hold")
	{
		tuning.intensity = std::numeric_limits<float>::quiet_NaN();
		tuning.localToneStrength = 9.0f;
		tuning.localStructureStrength = -4.0f;
		tuning.style = 5;
		tuning.Sanitize();
		REQUIRE(tuning.intensity == NR::Tuning::kDefaultStrength);
		REQUIRE(tuning.localToneStrength == NR::Tuning::kMaxStrength);
		REQUIRE(tuning.localStructureStrength == NR::Tuning::kMinStrength);
		REQUIRE(tuning.style == NR::Tuning::kMaxStyle);
	}
}

TEST_CASE("A zero tone residual is an exact passthrough", "[nr][nr-color]")
{
	using namespace NR::Color;

	// The shipped contract has to be a passthrough at tone 0 in both domains, so the composite
	// cannot shift a frame the model left alone.
	REQUIRE(CompositeGain(0.0f, kMaxToneStops, true) == 1.0f);
	REQUIRE(CompositeGain(0.0f, kMaxToneStops, false) == 1.0f);

	SECTION("the tone bound is symmetric and clamps")
	{
		REQUIRE(ClampTone(0.0f, kMaxToneStops) == 0.0f);
		REQUIRE(ClampTone(5.0f, kMaxToneStops) == kMaxToneStops);
		REQUIRE(ClampTone(-5.0f, kMaxToneStops) == -kMaxToneStops);
		REQUIRE(CompositeGain(50.0f, kMaxToneStops, true) == CompositeGain(kMaxToneStops, kMaxToneStops, true));
		REQUIRE(CompositeGain(-50.0f, kMaxToneStops, true) == CompositeGain(-kMaxToneStops, kMaxToneStops, true));
	}

	SECTION("the legacy domain only re-encodes the gain")
	{
		// 1/1.6 is exactly representable, so the exponent cannot perturb the passthrough.
		REQUIRE(1.0f / kLegacySceneGamma == 0.625f);
		// The re-encoding compresses the gain toward 1 in both directions, never past it.
		const float linearUp = CompositeGain(1.0f, kMaxToneStops, true);
		const float legacyUp = CompositeGain(1.0f, kMaxToneStops, false);
		REQUIRE(legacyUp == Approx(std::pow(linearUp, 1.0f / kLegacySceneGamma)));
		REQUIRE(legacyUp < linearUp);
		REQUIRE(legacyUp > 1.0f);

		const float linearDown = CompositeGain(-1.0f, kMaxToneStops, true);
		const float legacyDown = CompositeGain(-1.0f, kMaxToneStops, false);
		REQUIRE(legacyDown > linearDown);
		REQUIRE(legacyDown < 1.0f);
	}

	SECTION("the gain is monotonic in the tone")
	{
		float previous = CompositeGain(-kMaxToneStops, kMaxToneStops, true);
		for (float tone = -kMaxToneStops + 0.1f; tone <= kMaxToneStops; tone += 0.1f) {
			const float gain = CompositeGain(tone, kMaxToneStops, true);
			REQUIRE(gain > previous);
			previous = gain;
		}
	}
}

TEST_CASE("The display proxy is bounded and invertible", "[nr][nr-color]")
{
	using namespace NR::Color;

	SECTION("the proxy of a bright linear color stays below 1")
	{
		const auto proxy = MakeDisplayProxy(float3{ 40.0f, 40.0f, 40.0f });
		REQUIRE(proxy.x < 1.0f);
		REQUIRE(proxy.y < 1.0f);
		REQUIRE(proxy.z < 1.0f);
		REQUIRE(proxy.x > 0.9f);
	}

	SECTION("the proxy of a black color is black")
	{
		const auto proxy = MakeDisplayProxy(float3{ 0.0f, 0.0f, 0.0f });
		REQUIRE(proxy.x == 0.0f);
		REQUIRE(proxy.y == 0.0f);
		REQUIRE(proxy.z == 0.0f);
	}

	SECTION("negative channels cannot reach the model")
	{
		const auto proxy = MakeDisplayProxy(float3{ -2.0f, -2.0f, -2.0f });
		REQUIRE(proxy.x == 0.0f);
	}

	SECTION("decoding undoes the encoding")
	{
		for (float value = 0.0f; value <= 1.0f; value += 0.05f) {
			const auto decoded = ProxySrgbToLinear(ProxyLinearToSrgb(float3{ value, value, value }));
			REQUIRE(decoded.x == Approx(value).margin(1e-3f));
		}
	}

	SECTION("the encoder is monotonic")
	{
		float previous = -1.0f;
		for (float value = 0.0f; value <= 4.0f; value += 0.1f) {
			const float encoded = NeutwoEncode(float3{ value, value, value }).x;
			REQUIRE(encoded >= previous);
			previous = encoded;
		}
	}
}

TEST_CASE("The linearization follows the frame's Linear Lighting state", "[nr][nr-color]")
{
	using namespace NR::Color;

	SECTION("legacy kMAIN is gamma decoded")
	{
		const auto linear = ToLinearNR(float3{ 0.5f, 0.25f, 0.0f }, false);
		REQUIRE(linear.x == Approx(std::pow(0.5f, kLegacySceneGamma)));
		REQUIRE(linear.y == Approx(std::pow(0.25f, kLegacySceneGamma)));
		REQUIRE(linear.z == 0.0f);
		// The decode takes the positive part, so a negative channel cannot produce a NaN.
		REQUIRE(ToLinearNR(float3{ -0.5f, 0.0f, 0.0f }, false).x == 0.0f);
	}

	SECTION("scene-linear kMAIN is already linear")
	{
		const auto linear = ToLinearNR(float3{ 0.5f, 0.25f, 2.0f }, true);
		REQUIRE(linear.x == 0.5f);
		REQUIRE(linear.y == 0.25f);
		REQUIRE(linear.z == 2.0f);
	}
}

TEST_CASE("The exposure arms map luminance to a bounded exposure", "[nr][nr-exposure]")
{
	using namespace NR::Color;

	SECTION("a mid-grey scene exposes to 1")
	{
		REQUIRE(ExposureFromAdapted(kMiddleGrey, kExposureMinLum, kExposureMaxLum) == Approx(1.0f));
	}

	SECTION("the adapted luminance is clamped into range")
	{
		REQUIRE(ExposureFromAdapted(0.0f, kExposureMinLum, kExposureMaxLum) == Approx(kMiddleGrey / kExposureMinLum));
		REQUIRE(ExposureFromAdapted(1000.0f, kExposureMinLum, kExposureMaxLum) == Approx(kMiddleGrey / kExposureMaxLum));
		// Brighter scene, lower exposure.
		REQUIRE(ExposureFromAdapted(1.0f, kExposureMinLum, kExposureMaxLum) < ExposureFromAdapted(0.1f, kExposureMinLum, kExposureMaxLum));
	}

	SECTION("a reset snaps and a continued frame blends")
	{
		REQUIRE(AdaptExposure(0.1f, 2.0f, 0.016f, true, kExposureTau) == 2.0f);
		// No elapsed time means no movement at all.
		REQUIRE(AdaptExposure(0.1f, 2.0f, 0.0f, false, kExposureTau) == 0.1f);
		const float blended = AdaptExposure(0.1f, 2.0f, 0.1f, false, kExposureTau);
		REQUIRE(blended > 0.1f);
		REQUIRE(blended < 2.0f);
	}

	SECTION("the log luminance is floored, so a black frame cannot drive it to -inf")
	{
		REQUIRE(ExposureLogLuminance(float3{ 0.0f, 0.0f, 0.0f }, false) == Approx(std::log2(kExposureLumaFloor)));
		REQUIRE(std::isfinite(ExposureLogLuminance(float3{ 0.0f, 0.0f, 0.0f }, true)));
	}

	SECTION("an unreadable previous value cannot poison the adaptation")
	{
		REQUIRE(AdaptExposure(std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.1f, false, kExposureTau) == 2.0f);
	}
}
