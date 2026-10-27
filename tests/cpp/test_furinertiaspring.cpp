// Unit tests for FurInertia::Spring (the lag of fur tips behind a moving mesh).

#include "Features/FurShells/InertiaSpring.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

namespace
{
	constexpr float Frame = 1.0f / 90.0f;
	constexpr float MaxOffset = 2.0f;

	const FurInertia::Response Stylized = FurInertia::GetResponse(FurInertia::GetTuning(FurInertia::Preset::Stylized, {}), FurInertia::ReferenceLength, FurInertia::ReferenceDroop);

	FurInertia::Spring Settled(float3& a_position)
	{
		a_position = float3{};
		return FurInertia::Spring::At(a_position);
	}

	void Advance(FurInertia::Spring& a_spring, float3& a_position, const float3& a_velocity, float a_seconds, const FurInertia::Response& a_response = Stylized)
	{
		for (float t = 0.0f; t < a_seconds; t += Frame) {
			a_position += a_velocity * Frame;
			a_spring.Step(a_position, Frame, MaxOffset, a_response);
		}
	}
}

TEST_CASE("Inertia spring stays at rest for a still mesh", "[furinertia]")
{
	float3 position;
	auto spring = Settled(position);
	Advance(spring, position, {}, 1.0f);
	REQUIRE(spring.offset.Length() == 0.0f);
}

TEST_CASE("Inertia spring does not lean the fur at a steady speed", "[furinertia]")
{
	float3 position;
	auto spring = Settled(position);
	Advance(spring, position, { 300.0f, 0.0f, 0.0f }, 3.0f);
	REQUIRE(spring.offset.Length() < 0.05f);
}

TEST_CASE("Inertia spring swings the tips forward on a stop and settles without overshoot", "[furinertia]")
{
	float3 position;
	auto spring = Settled(position);
	Advance(spring, position, { 300.0f, 0.0f, 0.0f }, 2.0f);

	float peak = 0.0f;
	float lowest = 0.0f;
	for (float t = 0.0f; t < 1.0f; t += Frame) {
		spring.Step(position, Frame, MaxOffset, Stylized);
		peak = std::max(peak, spring.offset.x);
		lowest = std::min(lowest, spring.offset.x);
	}
	REQUIRE(peak > 0.1f);
	REQUIRE(lowest > -0.01f * peak);
	REQUIRE(std::abs(spring.offset.x) < 0.01f * peak);
}

TEST_CASE("Inertia spring never exceeds the allowed offset", "[furinertia]")
{
	float3 position;
	auto spring = Settled(position);
	Advance(spring, position, { 250.0f, 0.0f, 0.0f }, 0.5f);
	for (int i = 0; i < 200; ++i) {
		position.x += (i % 2 == 0 ? 120.0f : -120.0f);
		spring.Step(position, Frame, MaxOffset, Stylized);
		REQUIRE(spring.offset.Length() <= MaxOffset * 1.0001f);
	}
}

TEST_CASE("Inertia spring resets on a teleport", "[furinertia]")
{
	float3 position;
	auto spring = Settled(position);
	Advance(spring, position, { 300.0f, 0.0f, 0.0f }, 1.0f);
	Advance(spring, position, {}, 0.05f);
	REQUIRE(spring.offset.Length() > 0.0f);

	position.x += 5000.0f;
	spring.Step(position, Frame, MaxOffset, Stylized);
	REQUIRE(spring.offset.Length() == 0.0f);
	REQUIRE(spring.velocity.Length() == 0.0f);
}

TEST_CASE("Inertia spring stays finite on a long frame and ignores a zero frame", "[furinertia]")
{
	float3 position;
	auto spring = Settled(position);
	for (int i = 0; i < 40; ++i) {
		position.x += (i % 2 == 0 ? 6.0f : -6.0f);
		spring.Step(position, 0.2f, MaxOffset, Stylized);
		REQUIRE(std::isfinite(spring.offset.Length()));
		REQUIRE(spring.offset.Length() <= MaxOffset * 1.0001f);
	}

	const auto before = spring.offset;
	position.x += 1.0f;
	spring.Step(position, 0.0f, MaxOffset, Stylized);
	REQUIRE(spring.offset.x == before.x);
}

TEST_CASE("Inertia spring lags by the gain over omega squared at a steady acceleration", "[furinertia]")
{
	float3 position;
	auto spring = Settled(position);
	float speed = 0.0f;
	for (float t = 0.0f; t < 2.0f; t += Frame) {
		speed += 100.0f * Frame;
		position.x += speed * Frame;
		spring.Step(position, Frame, 100.0f, Stylized);
	}
	REQUIRE_THAT(spring.offset.x, Catch::Matchers::WithinRel(-Stylized.gain * 100.0f / (Stylized.omega * Stylized.omega), 0.05f));
}

TEST_CASE("Inertia spring is stable at a realistic hair frequency and barely moves short fur", "[furinertia]")
{
	const auto realistic = FurInertia::GetResponse(FurInertia::GetTuning(FurInertia::Preset::Realistic, {}), 1.8f, 0.15f);
	float3 position;
	auto spring = Settled(position);
	float speed = 0.0f;
	for (float t = 0.0f; t < 1.0f; t += Frame) {
		speed += 100.0f * Frame;
		position.x += speed * Frame;
		spring.Step(position, Frame, MaxOffset, realistic);
		REQUIRE(std::isfinite(spring.offset.Length()));
	}
	REQUIRE_THAT(spring.offset.x, Catch::Matchers::WithinRel(-realistic.gain * 100.0f / (realistic.omega * realistic.omega), 0.05f));
	REQUIRE(spring.offset.Length() < 0.01f);

	position.x += 5000.0f * Frame;
	spring.Step(position, Frame, MaxOffset, realistic);
	REQUIRE(std::isfinite(spring.offset.Length()));
	REQUIRE(spring.offset.Length() <= MaxOffset);
}

TEST_CASE("Inertia tuning is normalised so the reference fur uses the preset's base values", "[furinertia]")
{
	REQUIRE_THAT(Stylized.omega, Catch::Matchers::WithinRel(24.0f, 0.001f));
	REQUIRE(Stylized.gain == 2.0f);
}

TEST_CASE("Inertia tuning softens longer and droopier fur", "[furinertia]")
{
	const auto stylized = FurInertia::GetTuning(FurInertia::Preset::Stylized, {});
	const auto body = FurInertia::GetResponse(stylized, 1.8f, 0.45f);
	const auto head = FurInertia::GetResponse(stylized, 1.2f, 0.2f);
	REQUIRE(body.omega < Stylized.omega);
	REQUIRE_THAT(body.omega, Catch::Matchers::WithinRel(17.45f, 0.01f));
	REQUIRE_THAT(head.omega, Catch::Matchers::WithinRel(Stylized.omega, 0.03f));
}

TEST_CASE("Realistic tuning follows a real hair and ignores droop", "[furinertia]")
{
	const auto realistic = FurInertia::GetTuning(FurInertia::Preset::Realistic, {});
	const auto longFur = FurInertia::GetResponse(realistic, 1.8f, 0.0f);
	REQUIRE_THAT(longFur.omega / 6.2831853f, Catch::Matchers::WithinRel(28.4f, 0.01f));
	REQUIRE(longFur.omega == FurInertia::GetResponse(realistic, 1.8f, 0.5f).omega);
	REQUIRE(longFur.gain == 1.0f);
}

TEST_CASE("Custom tuning passes through and the frequency is clamped", "[furinertia]")
{
	const FurInertia::Tuning custom{ 10.0f, 1.5f, 0.0f, 0.0f };
	REQUIRE(FurInertia::GetTuning(FurInertia::Preset::Custom, custom).frequencyHz == 10.0f);
	const auto veryLong = FurInertia::GetResponse(FurInertia::GetTuning(FurInertia::Preset::Realistic, {}), 500.0f, 0.15f);
	REQUIRE(veryLong.omega >= FurInertia::MinFrequencyHz * 6.2831853f * 0.999f);
}

TEST_CASE("Inertia uses the fur length in world units and bounds a bad scale", "[furinertia]")
{
	REQUIRE(FurInertia::GetWorldLength(1.2f, 1.0f) == 1.2f);
	REQUIRE_THAT(FurInertia::GetWorldLength(1.2f, 1.5f), Catch::Matchers::WithinRel(1.8f, 0.001f));
	REQUIRE(FurInertia::GetWorldLength(1.2f, 0.0f) > 0.0f);
	REQUIRE(FurInertia::GetWorldLength(1.2f, 1.0e6f) <= 12.0001f);
}

TEST_CASE("Inertia tuning from a settings file is brought into range", "[furinertia]")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float infinity = std::numeric_limits<float>::infinity();
	const auto tuned = FurInertia::Sanitize({ nan, 99.0f, -5.0f, 99.0f, 0.0f });
	REQUIRE(tuned.frequencyHz == FurInertia::Tuning{}.frequencyHz);
	REQUIRE(tuned.gain == FurInertia::MaxGain);
	REQUIRE(tuned.lengthExponent == 0.0f);
	REQUIRE(tuned.droopExponent == FurInertia::MaxDroopExponent);
	REQUIRE(tuned.maxBend == FurInertia::MinBendLimit);

	const auto infinite = FurInertia::Sanitize({ infinity, nan, infinity, nan, infinity });
	const FurInertia::Tuning stylized{};
	REQUIRE(infinite.frequencyHz == stylized.frequencyHz);
	REQUIRE(infinite.gain == stylized.gain);
	REQUIRE(infinite.lengthExponent == stylized.lengthExponent);
	REQUIRE(infinite.droopExponent == stylized.droopExponent);
	REQUIRE(infinite.maxBend == stylized.maxBend);

	const auto high = FurInertia::Sanitize({ 1000.0f, 0.0f, 0.5f, 0.5f, 99.0f });
	REQUIRE(high.frequencyHz == FurInertia::MaxTuningFrequencyHz);
	REQUIRE(high.gain == FurInertia::MinGain);
	REQUIRE(high.maxBend == FurInertia::MaxBendLimit);
}

TEST_CASE("Every preset is a tuning the settings accept, so testers can share it as Custom", "[furinertia]")
{
	for (const auto preset : { FurInertia::Preset::Stylized, FurInertia::Preset::Realistic }) {
		const auto tuning = FurInertia::GetTuning(preset, {});
		const auto sanitized = FurInertia::Sanitize(tuning);
		REQUIRE(sanitized.frequencyHz == tuning.frequencyHz);
		REQUIRE(sanitized.gain == tuning.gain);
		REQUIRE(sanitized.lengthExponent == tuning.lengthExponent);
		REQUIRE(sanitized.droopExponent == tuning.droopExponent);
		REQUIRE(sanitized.maxBend == tuning.maxBend);

		const auto asCustom = FurInertia::GetTuning(FurInertia::Preset::Custom, tuning);
		REQUIRE(FurInertia::GetResponse(asCustom, 1.8f, 0.45f).omega == FurInertia::GetResponse(tuning, 1.8f, 0.45f).omega);
		REQUIRE(FurInertia::GetResponse(asCustom, 1.8f, 0.45f).gain == tuning.gain);
	}
}
