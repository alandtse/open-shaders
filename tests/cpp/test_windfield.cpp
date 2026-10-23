#include "Features/Wind/WindField.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;

TEST_CASE("Wind field matches the shared CPU/GPU conformance samples", "[wind-field]")
{
	constexpr float parityTolerance = 5e-4f;
	const WindField::WindTuning tuning;

#define WIND_FIELD_PARITY_SAMPLE(                                                                                                \
	positionX, positionY, positionZ, travelDistance, directionX, directionY, directionZ, speed, expectedGust, expectedVelocityX, \
	expectedVelocityY, expectedVelocityZ)                                                                                        \
	{                                                                                                                            \
		const float3 position(positionX, positionY, positionZ);                                                                  \
		const float3 direction(directionX, directionY, directionZ);                                                              \
		const WindField::Field field = WindField::CreateField(direction, speed, travelDistance);                                 \
		const WindField::WindSample sample = WindField::SampleField(position, field, tuning);                                    \
		REQUIRE(sample.ambientGust == Approx(expectedGust).margin(parityTolerance));                                             \
		REQUIRE(sample.velocity.x == Approx(expectedVelocityX).margin(parityTolerance));                                         \
		REQUIRE(sample.velocity.y == Approx(expectedVelocityY).margin(parityTolerance));                                         \
		REQUIRE(sample.velocity.z == Approx(expectedVelocityZ).margin(parityTolerance));                                         \
	}
#include "Tests/WindFieldParitySamples.hlsli"
#undef WIND_FIELD_PARITY_SAMPLE
}

TEST_CASE("Wind sampling is deterministic and stateless", "[wind-field]")
{
	const float3 position(1024.0f, -2048.0f, 32.0f);
	const float3 direction(0.25f, 0.75f, 0.0f);
	const WindField::WindTuning tuning;
	const WindField::Field field = WindField::CreateField(direction, 0.8f, 12902.4f);
	const WindField::WindSample first = WindField::SampleField(position, field, tuning);
	const WindField::WindSample second = WindField::SampleField(position, field, tuning);

	REQUIRE(first.ambientGust == second.ambientGust);
	REQUIRE(first.velocity.x == second.velocity.x);
	REQUIRE(first.velocity.y == second.velocity.y);
	REQUIRE(first.velocity.z == second.velocity.z);
}

TEST_CASE("Wind field transport tuning does not change local air velocity", "[wind-field]")
{
	const float3 position(1536.0f, -640.0f, 24.0f);
	const float3 direction(1.0f, 0.0f, 0.0f);
	WindField::WindTuning baselineTuning;
	WindField::WindTuning fastTransportTuning = baselineTuning;
	fastTransportTuning.gustAdvectionBaseSpeed *= 2.0f;
	fastTransportTuning.gustAdvectionMultiplier = 8.0f;

	const auto field = WindField::CreateField(direction, 0.75f, 4096.0f);
	const auto baseline = WindField::SampleField(position, field, baselineTuning);
	const auto fastTransport = WindField::SampleField(position, field, fastTransportTuning);

	REQUIRE(fastTransport.ambientGust == baseline.ambientGust);
	REQUIRE(fastTransport.velocity.x == baseline.velocity.x);
	REQUIRE(fastTransport.velocity.y == baseline.velocity.y);
	REQUIRE(fastTransport.velocity.z == baseline.velocity.z);
}

TEST_CASE("Zero gust amplitude preserves mean air velocity", "[wind-field]")
{
	WindField::WindTuning tuning;
	tuning.gustAmplitude = 0.0f;
	const float3 direction(0.0f, 1.0f, 0.0f);

	const auto firstField = WindField::CreateField(direction, 0.8f);
	const auto secondField = WindField::CreateField(direction, 0.8f, 8192.0f);
	const auto first = WindField::SampleField(float3(0.0f, 0.0f, 0.0f), firstField, tuning);
	const auto second = WindField::SampleField(float3(4096.0f, -2048.0f, 0.0f), secondField, tuning);

	REQUIRE(first.velocity.x == Approx(0.0f));
	REQUIRE(first.velocity.y == Approx(0.8f));
	REQUIRE(second.velocity.x == Approx(0.0f));
	REQUIRE(second.velocity.y == Approx(0.8f));
}

TEST_CASE("Fine wind stays bounded without changing the broad gust field", "[wind-field]")
{
	WindField::WindTuning tuning;
	const auto field = WindField::CreateField(float3(0.6f, 0.8f, 0.0f), 0.1f, 4096.0f);
	constexpr float maximumAngle = 4.0f * 3.14159265f / 180.0f;
	for (float strength : { 0.25f, 1.0f, 4.0f }) {
		tuning.microTurbulenceStrength = strength;
		for (float speed : { 0.0001f, 0.01f, 0.1f, 0.5f, 2.0f }) {
			const float3 base = field.direction * speed;
			WindField::FineState state;
			for (int step = 0; step < 96; ++step) {
				const float3 position(-1600.0f + step * 37.0f, 330.0f - step * 19.0f, 0.0f);
				const auto broad = WindField::SampleField(position, field, tuning);
				WindField::AdvanceFineState(state, base, 1.0f / 60.0f, tuning);
				const auto fine = WindField::SampleFineVelocity(position, base, float3{}, state, tuning);
				const auto local = base + fine.slowVelocity + fine.detailVelocity;
				REQUIRE(std::isfinite(local.x));
				REQUIRE(std::isfinite(local.y));
				REQUIRE(local.x * field.direction.x + local.y * field.direction.y >= speed * 0.19999f);
				const float angle = std::atan2(base.x * local.y - base.y * local.x, base.x * local.x + base.y * local.y);
				REQUIRE(std::abs(angle) <= maximumAngle + 1e-6f);
				REQUIRE(local.z == 0.0f);
				auto changedDetail = tuning;
				changedDetail.turbulentGustSeed ^= 0xABCDEF01u;
				const auto otherFine = WindField::SampleFineVelocity(position, base, float3{}, state, changedDetail);
				REQUIRE(otherFine.slowVelocity.x == fine.slowVelocity.x);
				REQUIRE(otherFine.slowVelocity.y == fine.slowVelocity.y);
				const auto broadAfter = WindField::SampleField(position, field, tuning);
				REQUIRE(broadAfter.ambientGust == broad.ambientGust);
				REQUIRE(broadAfter.velocity.x == broad.velocity.x);
				REQUIRE(broadAfter.velocity.y == broad.velocity.y);
			}
		}
	}
	REQUIRE(field.travelDistance == 4096.0f);
	REQUIRE(field.direction.x == Approx(0.6f));
	REQUIRE(field.direction.y == Approx(0.8f));
}

TEST_CASE("Fine wind transport integrates speed and heading without resetting phase", "[wind-field]")
{
	WindField::WindTuning tuning;
	WindField::FineState state;
	const float3 base(0.1f, 0.0f, 0.0f);
	WindField::AdvanceFineState(state, base, 1.0f, tuning);
	const float firstX = state.travel.x + state.travel.z;
	REQUIRE(firstX == Approx(179.2f));
	REQUIRE(state.travel.y + state.travel.w == 0.0f);
	tuning.fineFrequency = 2.0f;
	WindField::AdvanceFineState(state, float3(0.0f, 0.5f, 0.0f), 0.25f, tuning);
	REQUIRE(state.travel.x + state.travel.z == firstX);
	REQUIRE(state.travel.y + state.travel.w == Approx(192.0f));
	const auto beforePause = state;
	WindField::AdvanceFineState(state, base, 0.0f, tuning);
	WindField::AdvanceFineState(state, float3{}, 10.0f, tuning);
	tuning.fineFrequency = 0.0f;
	WindField::AdvanceFineState(state, base, 10.0f, tuning);
	REQUIRE(state.travel.x == beforePause.travel.x);
	REQUIRE(state.travel.y == beforePause.travel.y);
	REQUIRE(state.travel.z == beforePause.travel.z);
	REQUIRE(state.travel.w == beforePause.travel.w);

	tuning.fineFrequency = 1.0f;
	const float3 position(123.0f, -456.0f, 0.0f);
	const auto first = WindField::SampleFineVelocity(position, base, float3{}, state, tuning);
	const auto oldState = state;
	WindField::AdvanceFineState(state, base, 0.5f, tuning);
	const float3 translation(
		state.travel.x + state.travel.z - oldState.travel.x - oldState.travel.z,
		state.travel.y + state.travel.w - oldState.travel.y - oldState.travel.w, 0.0f);
	const auto transported = WindField::SampleFineVelocity(position + translation, base, float3{}, state, tuning);
	REQUIRE(transported.slowVelocity.x == Approx(first.slowVelocity.x).margin(1e-6f));
	REQUIRE(transported.slowVelocity.y == Approx(first.slowVelocity.y).margin(1e-6f));
	REQUIRE(transported.detailVelocity.x == Approx(first.detailVelocity.x).margin(1e-6f));
	REQUIRE(transported.detailVelocity.y == Approx(first.detailVelocity.y).margin(1e-6f));
}

TEST_CASE("Fine wind remains continuous across transport packing and noise boundaries", "[wind-field]")
{
	const WindField::WindTuning tuning;
	const float3 position(123.0f, -456.0f, 0.0f);
	const float3 base(0.1f, 0.0f, 0.0f);
	WindField::FineState state{ float4(0.0f, 0.0f, 767.99f, 0.0f) };
	for (float spatialScale : { 0.3f, 1.0f, 2.35f }) {
		const auto first = WindField::SampleFineVelocity(position, base, float3{}, state, tuning, spatialScale, 0.7f);
		auto advanced = state;
		WindField::AdvanceFineState(advanced, base, 0.001f, tuning);
		REQUIRE(advanced.travel.x == 768.0f);
		const auto next = WindField::SampleFineVelocity(position, base, float3{}, advanced, tuning, spatialScale, 0.7f);
		REQUIRE(std::hypot(next.slowVelocity.x - first.slowVelocity.x, next.slowVelocity.y - first.slowVelocity.y) < 0.001f);
		REQUIRE(std::hypot(next.detailVelocity.x - first.detailVelocity.x, next.detailVelocity.y - first.detailVelocity.y) < 0.001f);
	}
	state = {};
	const auto initial = WindField::SampleFineVelocity(position, base, float3{}, state, tuning);
	state.travel.x = 768.0f * 256.0f;
	state.travel.y = -768.0f * 256.0f;
	const auto periodic = WindField::SampleFineVelocity(position, base, float3{}, state, tuning);
	REQUIRE(periodic.slowVelocity.x == Approx(initial.slowVelocity.x).margin(1e-6f));
	REQUIRE(periodic.slowVelocity.y == Approx(initial.slowVelocity.y).margin(1e-6f));
	REQUIRE(periodic.detailVelocity.x == Approx(initial.detailVelocity.x).margin(1e-6f));
	REQUIRE(periodic.detailVelocity.y == Approx(initial.detailVelocity.y).margin(1e-6f));
}

TEST_CASE("Fine wind controls separate background activity from positive gust coupling", "[wind-field]")
{
	WindField::WindTuning tuning;
	WindField::FineState state;
	const float3 position(123.0f, -456.0f, 0.0f);
	const float3 base(0.1f, 0.0f, 0.0f);
	const auto calm = WindField::SampleFineVelocity(position, float3{}, float3{}, state, tuning);
	const auto cancelled = WindField::SampleFineVelocity(position, base, -base, state, tuning);
	REQUIRE(calm.slowVelocity.Length() == 0.0f);
	REQUIRE(calm.detailVelocity.Length() == 0.0f);
	REQUIRE(cancelled.slowVelocity.Length() == 0.0f);
	REQUIRE(cancelled.detailVelocity.Length() == 0.0f);

	tuning.fineWindCoupling = 0.0f;
	tuning.fineGustCoupling = 0.0f;
	const auto background = WindField::SampleFineVelocity(position, base, float3{}, state, tuning);
	REQUIRE(background.slowVelocity.Length() > 0.0f);
	REQUIRE(background.detailVelocity.Length() > 0.0f);
	tuning.fineBaselineAmplitude = 0.0f;
	const auto zeroControls = WindField::SampleFineVelocity(position, base, float3{}, state, tuning);
	REQUIRE(zeroControls.slowVelocity.Length() == 0.0f);
	REQUIRE(zeroControls.detailVelocity.Length() == 0.0f);

	tuning.fineGustCoupling = 0.75f;
	const auto noGust = WindField::SampleFineVelocity(position, base, float3{}, state, tuning);
	const auto lull = WindField::SampleFineVelocity(position, base, -base * 0.5f, state, tuning);
	const auto gust = WindField::SampleFineVelocity(position, base, base, state, tuning);
	REQUIRE(noGust.slowVelocity.Length() == 0.0f);
	REQUIRE(noGust.detailVelocity.Length() == 0.0f);
	REQUIRE(lull.slowVelocity.Length() == 0.0f);
	REQUIRE(lull.detailVelocity.Length() == 0.0f);
	REQUIRE(gust.slowVelocity.Length() > 0.0f);
	REQUIRE(gust.detailVelocity.Length() > 0.0f);
	tuning.microTurbulenceStrength = 0.0f;
	const auto disabled = WindField::SampleFineVelocity(position, base, base, state, tuning);
	REQUIRE(disabled.slowVelocity.Length() == 0.0f);
	REQUIRE(disabled.detailVelocity.Length() == 0.0f);
}
