#include "Features/Upscaling/NeuralRendering/Guide.h"
#include "Features/Upscaling/NeuralRendering/Lifecycle.h"
#include "Features/Upscaling/NeuralRendering/Parameters.h"
#include "Features/Upscaling/NeuralRendering/PendingRequest.h"
#include "Features/Upscaling/NeuralRendering/Tuning.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace
{
	// REL::Version cannot be included in this target; the gate only needs major()/minor().
	struct FakeVersion
	{
		uint16_t majorValue = 0;
		uint16_t minorValue = 0;
		[[nodiscard]] constexpr uint16_t major() const { return majorValue; }
		[[nodiscard]] constexpr uint16_t minor() const { return minorValue; }
	};

	using Version = std::optional<FakeVersion>;

	/**
	 * @brief Records which typed Set overload each write helper reaches.
	 *        The tag a key is stored under is what Feature 18 reads back, and it cannot be
	 *        observed without NGX, so this pins it against the tested 310.8 contract.
	 */
	struct RecordingParameter : NVSDK_NGX_Parameter
	{
		enum class Tag : uint8_t
		{
			kUnsignedLongLong,
			kFloat,
			kDouble,
			kUnsigned,
			kInt,
			kD3D11Resource,
			kD3D12Resource,
			kVoidPointer
		};

		struct Write
		{
			std::string key;
			Tag tag;
			int asInt = 0;
			bool operator==(const Write&) const = default;
		};

		std::vector<Write> writes;

		void Set(const char* a_name, unsigned long long) override { Record(a_name, Tag::kUnsignedLongLong); }
		void Set(const char* a_name, float) override { Record(a_name, Tag::kFloat); }
		void Set(const char* a_name, double) override { Record(a_name, Tag::kDouble); }
		void Set(const char* a_name, unsigned int) override { Record(a_name, Tag::kUnsigned); }
		void Set(const char* a_name, int a_value) override { Record(a_name, Tag::kInt, a_value); }
		void Set(const char* a_name, ID3D11Resource*) override { Record(a_name, Tag::kD3D11Resource); }
		void Set(const char* a_name, ID3D12Resource*) override { Record(a_name, Tag::kD3D12Resource); }
		void Set(const char* a_name, void*) override { Record(a_name, Tag::kVoidPointer); }

		NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_Success; }
		NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_Success; }
		NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_Success; }
		NVSDK_NGX_Result Get(const char*, unsigned int*) const override { return NVSDK_NGX_Result_Success; }
		NVSDK_NGX_Result Get(const char*, int*) const override { return NVSDK_NGX_Result_Success; }
		NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_Success; }
		NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_Success; }
		NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_Success; }

		void Reset() override {}

	private:
		void Record(const char* a_name, Tag a_tag, int a_value = 0) { writes.push_back({ a_name ? a_name : "", a_tag, a_value }); }
	};
}

TEST_CASE("Runtime version gate accepts any build of the observed line", "[nr][nr-version]")
{
	REQUIRE(NR::IsSupportedRuntimeVersion(Version{ FakeVersion{ 310, 8 } }));
}

TEST_CASE("Runtime version gate rejects other lines and an unreadable version", "[nr][nr-version]")
{
	REQUIRE_FALSE(NR::IsSupportedRuntimeVersion(Version{ FakeVersion{ 310, 7 } }));
	REQUIRE_FALSE(NR::IsSupportedRuntimeVersion(Version{ FakeVersion{ 310, 9 } }));
	REQUIRE_FALSE(NR::IsSupportedRuntimeVersion(Version{ FakeVersion{ 311, 8 } }));
	REQUIRE_FALSE(NR::IsSupportedRuntimeVersion(Version{ FakeVersion{ 309, 8 } }));
	REQUIRE_FALSE(NR::IsSupportedRuntimeVersion(Version{ FakeVersion{ 0, 0 } }));
	REQUIRE_FALSE(NR::IsSupportedRuntimeVersion(Version{}));
}

TEST_CASE("Only an absent runtime DLL reports a warning", "[nr][nr-lifecycle]")
{
	REQUIRE(NR::FailureSeverityOf(NR::FailureKind::kNone) == NR::FailureSeverity::kNone);
	REQUIRE(NR::FailureSeverityOf(NR::FailureKind::kDllMissing) == NR::FailureSeverity::kWarn);
	for (auto kind : { NR::FailureKind::kVersionRejected, NR::FailureKind::kLoadFailed,
			 NR::FailureKind::kExportMissing, NR::FailureKind::kUnsupportedAdapter,
			 NR::FailureKind::kInitFailed, NR::FailureKind::kParameterApiUnavailable,
			 NR::FailureKind::kSehFault, NR::FailureKind::kDeviceRemoved, NR::FailureKind::kDrainTimeout,
			 NR::FailureKind::kWaitFailed })
		REQUIRE(NR::FailureSeverityOf(kind) == NR::FailureSeverity::kError);
}

TEST_CASE("The version gate refuses a wrong-version DLL before any device work", "[nr][nr-version]")
{
	// The probe is the only thing the request consults before creating a D3D12 device, so these
	// three outcomes are what keep a device from ever existing for an unsupported DLL.
	REQUIRE(NR::DecideProbe(false, false) == NR::ProbeDecision::kDllMissing);
	REQUIRE(NR::DecideProbe(true, false) == NR::ProbeDecision::kVersionRejected);
	REQUIRE(NR::DecideProbe(true, true) == NR::ProbeDecision::kProceed);

	// A probe whose version could not be read is refused, not treated as an absent file.
	const auto unreadable = NR::DecideProbe(true, NR::IsSupportedRuntimeVersion(Version{}));
	REQUIRE(unreadable == NR::ProbeDecision::kVersionRejected);
}

TEST_CASE("A request is one attempt and only starts from an idle layer", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	REQUIRE(life.CanAttempt());

	life.MarkInitialized();
	REQUIRE_FALSE(life.CanAttempt());

	life.MarkFailed(NR::FailureKind::kInitFailed);
	REQUIRE_FALSE(life.CanAttempt());

	// Idle again after a shutdown, with its budget untouched. Each retry buys exactly the attempt
	// it arms, including the retry that spends the whole budget.
	life.Reset();
	REQUIRE(life.CanAttempt());
	for (uint32_t retry = 0; retry < NR::Lifecycle::kMaxRetries; ++retry) {
		life.MarkFailed(NR::FailureKind::kLoadFailed);
		REQUIRE(life.RequestRetry());
		// Each retry buys exactly the attempt it arms, the retry that spends the whole budget too.
		REQUIRE(life.CanAttempt());
		REQUIRE(life.State() == NR::RuntimeState::kNotLoaded);
		REQUIRE(life.RetryCount() == retry + 1);
	}

	// The budget is spent, so a further retry is refused and must leave the latch truthful rather
	// than clear it into an idle layer that then refuses every attempt.
	life.MarkFailed(NR::FailureKind::kLoadFailed);
	REQUIRE_FALSE(life.RequestRetry());
	REQUIRE(life.State() == NR::RuntimeState::kFailed);
	REQUIRE(life.Failure() == NR::FailureKind::kLoadFailed);
	REQUIRE(life.RetryCount() == NR::Lifecycle::kMaxRetries);
	REQUIRE_FALSE(life.CanAttempt());
}

TEST_CASE("A spent budget still allows the last retry on the drain-first path", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	for (uint32_t retry = 0; retry < NR::Lifecycle::kMaxRetries - 1; ++retry) {
		life.MarkFailed(NR::FailureKind::kInitFailed);
		REQUIRE(life.RequestRetry());
		REQUIRE(life.CanAttempt());
	}
	REQUIRE(life.RetryCount() == NR::Lifecycle::kMaxRetries - 1);

	// The last retry arms a drain-first attempt, which runs only once the drain's own shutdown has
	// reset the layer: the reset must not revoke the arm the shutdown was spending budget for.
	life.MarkFailed(NR::FailureKind::kDrainTimeout);
	REQUIRE(life.DecideRetry() == NR::RetryDecision::kDrainFirst);
	REQUIRE(life.RequestRetry());
	REQUIRE(life.RetryCount() == NR::Lifecycle::kMaxRetries);
	life.Reset();
	REQUIRE(life.CanAttempt());

	// That attempt failed too, so the next retry is refused with the latch and its reason intact.
	life.MarkFailed(NR::FailureKind::kInitFailed);
	REQUIRE_FALSE(life.RequestRetry());
	REQUIRE(life.State() == NR::RuntimeState::kFailed);
	REQUIRE(life.Failure() == NR::FailureKind::kInitFailed);
	REQUIRE(life.RetryCount() == NR::Lifecycle::kMaxRetries);
	REQUIRE_FALSE(life.CanAttempt());
}

TEST_CASE("A retry drains again when the last shutdown kept the runtime alive", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	REQUIRE(life.DecideRetry() == NR::RetryDecision::kAttempt);

	for (auto kind : { NR::FailureKind::kDrainTimeout, NR::FailureKind::kWaitFailed }) {
		NR::Lifecycle retained;
		retained.MarkFailed(kind);
		REQUIRE(NR::IsRetainedFailure(kind));
		REQUIRE(retained.DecideRetry() == NR::RetryDecision::kDrainFirst);
		REQUIRE(retained.RequestRetry());
		REQUIRE(retained.RetryCount() == 1);
	}

	// A failure that released nothing to drain re-arms instead.
	life.MarkFailed(NR::FailureKind::kVersionRejected);
	REQUIRE_FALSE(NR::IsRetainedFailure(NR::FailureKind::kVersionRejected));
	REQUIRE(life.DecideRetry() == NR::RetryDecision::kRearm);
	REQUIRE(life.RequestRetry());

	life.MarkFailed(NR::FailureKind::kSehFault);
	REQUIRE(life.DecideRetry() == NR::RetryDecision::kRefused);

	life.MarkInitialized();
	REQUIRE(life.DecideRetry() == NR::RetryDecision::kRefused);

	// Retrying retained resources spends the budget too, so a stuck drain cannot be retried
	// forever; the layer can still be shut down explicitly.
	NR::Lifecycle spent;
	for (uint32_t retry = 0; retry < NR::Lifecycle::kMaxRetries; ++retry) {
		spent.MarkFailed(NR::FailureKind::kInitFailed);
		REQUIRE(spent.RequestRetry());
	}
	spent.MarkFailed(NR::FailureKind::kDrainTimeout);
	REQUIRE(spent.DecideRetry() == NR::RetryDecision::kRefused);
}

TEST_CASE("A removed device is retried without draining again", "[nr][nr-lifecycle]")
{
	// A removed device fails every later call the same way, so a drain-first retry could never
	// retire it: the retry must re-arm into a fresh attempt, and nothing may be reported retained.
	NR::Lifecycle life;
	life.MarkFailed(NR::FailureKind::kDeviceRemoved);
	REQUIRE_FALSE(NR::IsRetainedFailure(NR::FailureKind::kDeviceRemoved));
	REQUIRE(life.DecideRetry() == NR::RetryDecision::kRearm);
	REQUIRE(life.RequestRetry());
	REQUIRE(life.RetryCount() == 1);
	REQUIRE(life.State() == NR::RuntimeState::kNotLoaded);
	REQUIRE(life.CanAttempt());
}

TEST_CASE("A lifecycle permits an attempt only from idle", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	REQUIRE(life.State() == NR::RuntimeState::kNotLoaded);
	REQUIRE(life.CanAttempt());

	life.MarkInitialized();
	REQUIRE(life.State() == NR::RuntimeState::kInitialized);
	REQUIRE_FALSE(life.CanAttempt());

	life.MarkFailed(NR::FailureKind::kInitFailed);
	REQUIRE(life.State() == NR::RuntimeState::kFailed);
	REQUIRE_FALSE(life.CanAttempt());
}

TEST_CASE("A lifecycle re-arms only through an explicit retry", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	life.MarkFailed(NR::FailureKind::kVersionRejected);
	REQUIRE(life.RetryCount() == 0);

	REQUIRE(life.RequestRetry());
	REQUIRE(life.State() == NR::RuntimeState::kNotLoaded);
	REQUIRE(life.CanAttempt());
	REQUIRE(life.Failure() == NR::FailureKind::kNone);
	REQUIRE(life.RetryCount() == 1);

	// Not latched right now, so there is nothing to re-arm and the budget must not move.
	REQUIRE_FALSE(life.RequestRetry());
	REQUIRE(life.RetryCount() == 1);
}

TEST_CASE("A lifecycle stops retrying once its budget is spent", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	for (uint32_t attempt = 0; attempt < NR::Lifecycle::kMaxRetries; ++attempt) {
		REQUIRE(life.CanAttempt());
		life.MarkFailed(NR::FailureKind::kLoadFailed);
		REQUIRE(life.RequestRetry());
	}
	REQUIRE(life.RetryCount() == NR::Lifecycle::kMaxRetries);

	life.MarkFailed(NR::FailureKind::kLoadFailed);
	REQUIRE_FALSE(life.RequestRetry());
	REQUIRE(life.State() == NR::RuntimeState::kFailed);

	// A shutdown releases resources but not budget, so even an idle layer refuses a plain attempt.
	life.Reset();
	REQUIRE_FALSE(life.CanAttempt());
}

TEST_CASE("A structured exception latches a lifecycle permanently", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	life.MarkFailed(NR::FailureKind::kSehFault);
	REQUIRE_FALSE(life.RequestRetry());
	REQUIRE(life.State() == NR::RuntimeState::kFailed);
	REQUIRE(life.Failure() == NR::FailureKind::kSehFault);
}

TEST_CASE("A completed shutdown returns a lifecycle to idle", "[nr][nr-lifecycle]")
{
	NR::Lifecycle life;
	life.MarkInitialized();
	life.Reset();
	REQUIRE(life.State() == NR::RuntimeState::kNotLoaded);
	REQUIRE(life.CanAttempt());
	REQUIRE(life.RetryCount() == 0);
}

TEST_CASE("A posted request replaces one that has not been taken", "[nr][nr-request]")
{
	NR::PendingRequest pending;
	REQUIRE_FALSE(pending.Any());
	REQUIRE(pending.Take() == NR::RequestKind::kNone);

	// The render thread takes only one request per frame, so the newest wins: an init posted
	// before a shutdown must never run, or one frame could initialise and drain the layer at once.
	pending.Post(NR::RequestKind::kInitialize);
	pending.Post(NR::RequestKind::kShutdown);
	REQUIRE(pending.Any());
	REQUIRE(pending.Take() == NR::RequestKind::kShutdown);

	// A take leaves the slot empty, so the frames between requests see nothing pending.
	REQUIRE_FALSE(pending.Any());
	REQUIRE(pending.Take() == NR::RequestKind::kNone);

	// A status read reports the queued request without consuming it, and the label is what the
	// devbench pendingRequest field prints.
	pending.Post(NR::RequestKind::kRetry);
	REQUIRE(pending.Peek() == NR::RequestKind::kRetry);
	REQUIRE(pending.Peek() == NR::RequestKind::kRetry);
	REQUIRE(std::string(NR::RequestKindName(pending.Peek())) == "Retry");
	REQUIRE(pending.Take() == NR::RequestKind::kRetry);
	REQUIRE(std::string(NR::RequestKindName(NR::RequestKind::kNone)) == "None");
	REQUIRE(std::string(NR::RequestKindName(NR::RequestKind::kInitialize)) == "Initialize");
	REQUIRE(std::string(NR::RequestKindName(NR::RequestKind::kShutdown)) == "Shutdown");

	// Same replacement rule when repeats sit behind the newest request.
	pending.Post(NR::RequestKind::kRetry);
	pending.Post(NR::RequestKind::kRetry);
	pending.Post(NR::RequestKind::kInitialize);
	REQUIRE(pending.Take() == NR::RequestKind::kInitialize);
	REQUIRE_FALSE(pending.Any());
}

TEST_CASE("Parameter writes use the tag the tested runtime was driven with", "[nr][nr-parameters]")
{
	RecordingParameter parameters;
	NR::SetInt(&parameters, "DLSSNR.Width", 64u);
	NR::SetInt(&parameters, "DLSSNR.Style", 2u);
	NR::SetFloat(&parameters, "DLSSNR.Scale", 1.0f);
	NR::SetPointer(&parameters, "DLSSNR.Color", nullptr);
	REQUIRE(parameters.writes == std::vector<RecordingParameter::Write>{
									 { "DLSSNR.Width", RecordingParameter::Tag::kInt, 64 },
									 { "DLSSNR.Style", RecordingParameter::Tag::kInt, 2 },
									 { "DLSSNR.Scale", RecordingParameter::Tag::kFloat },
									 { "DLSSNR.Color", RecordingParameter::Tag::kVoidPointer },
								 });
}

TEST_CASE("An integer parameter is stored as the int tag, bit pattern intact", "[nr][nr-parameters]")
{
	RecordingParameter parameters;
	NR::SetInt(&parameters, "DLSSNR.Width", 0xFFFFFFFFu);
	REQUIRE(parameters.writes.size() == 1);
	REQUIRE(parameters.writes[0].tag == RecordingParameter::Tag::kInt);
	REQUIRE(parameters.writes[0].asInt == -1);
}

TEST_CASE("Guide regions outside the extent are clamped", "[nr][nr-guide]")
{
	SECTION("a region inside the extent is unchanged")
	{
		const auto clamped = NR::ClampGuideRegion(64, 64, NR::GuideRegion{ 4, 6, 30, 20 });
		REQUIRE(clamped.has_value());
		REQUIRE(clamped->baseX == 4);
		REQUIRE(clamped->baseY == 6);
		REQUIRE(clamped->width == 30);
		REQUIRE(clamped->height == 20);
	}

	SECTION("a region reaching past the extent is truncated")
	{
		const auto clamped = NR::ClampGuideRegion(64, 64, NR::GuideRegion{ 60, 62, 100, 100 });
		REQUIRE(clamped.has_value());
		REQUIRE(clamped->width == 4);
		REQUIRE(clamped->height == 2);
	}

	SECTION("a region starting at the extent covers no texel")
	{
		REQUIRE_FALSE(NR::ClampGuideRegion(64, 64, NR::GuideRegion{ 64, 0, 64, 64 }).has_value());
		REQUIRE_FALSE(NR::ClampGuideRegion(64, 64, NR::GuideRegion{ 0, 64, 64, 64 }).has_value());
	}

	SECTION("a zero-sized region covers no texel")
	{
		REQUIRE_FALSE(NR::ClampGuideRegion(64, 64, NR::GuideRegion{ 0, 0, 0, 64 }).has_value());
		REQUIRE_FALSE(NR::ClampGuideRegion(64, 64, NR::GuideRegion{ 0, 0, 64, 0 }).has_value());
	}
}

TEST_CASE("Tuning clamps non-finite and out-of-range strengths", "[nr][nr-tuning]")
{
	NR::Tuning tuning;
	REQUIRE(tuning.style == 0);
	REQUIRE(tuning.intensity == NR::Tuning::kDefaultStrength);
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);

	tuning.style = 9;
	tuning.intensity = 5.0f;
	tuning.localToneStrength = -3.0f;
	tuning.localStructureStrength = std::numeric_limits<float>::quiet_NaN();
	tuning.skinStructureStrength = std::numeric_limits<float>::infinity();
	tuning.Sanitize();

	REQUIRE(tuning.style == NR::Tuning::kMaxStyle);
	REQUIRE(tuning.intensity == NR::Tuning::kMaxStrength);
	REQUIRE(tuning.localToneStrength == NR::Tuning::kMinStrength);
	REQUIRE(tuning.localStructureStrength == NR::Tuning::kDefaultStrength);
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);
}

TEST_CASE("Tuning keeps the automatic skin-structure sentinel", "[nr][nr-tuning]")
{
	NR::Tuning tuning;
	tuning.skinStructureStrength = 1.5f;
	tuning.Sanitize();
	REQUIRE(tuning.skinStructureStrength == 1.5f);

	tuning.skinStructureStrength = 10.0f;
	tuning.Sanitize();
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kMaxStrength);

	tuning.skinStructureStrength = -8.0f;
	tuning.Sanitize();
	REQUIRE(tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure);
}
