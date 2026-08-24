#include "Utils/WindField.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE("Wind field matches the shared CPU/GPU conformance samples", "[wind-field]")
{
	constexpr float parityTolerance = 5e-4f;
	const WindField::WindTuning tuning;
	const std::array<WindField::AmbientGust, WindField::kAmbientGustCapacity> gusts{};

#define WIND_FIELD_PARITY_SAMPLE(                                                                                  \
	positionX, positionY, positionZ, directionX, directionY, directionZ, speed, expectedGust, expectedVelocityX,   \
	expectedVelocityY, expectedVelocityZ)                                                                          \
	{                                                                                                              \
		const float3 position(positionX, positionY, positionZ);                                                    \
		const float3 direction(directionX, directionY, directionZ);                                                \
		const WindField::WindSample sample = WindField::SampleWind(position, direction, speed, tuning, gusts, 0u); \
		REQUIRE(sample.ambientGust == Approx(expectedGust).margin(parityTolerance));                               \
		REQUIRE(sample.velocity.x == Approx(expectedVelocityX).margin(parityTolerance));                           \
		REQUIRE(sample.velocity.y == Approx(expectedVelocityY).margin(parityTolerance));                           \
		REQUIRE(sample.velocity.z == Approx(expectedVelocityZ).margin(parityTolerance));                           \
	}
#include "Tests/WindFieldParitySamples.hlsli"
#undef WIND_FIELD_PARITY_SAMPLE
}

TEST_CASE("Wind sampling is deterministic and stateless", "[wind-field]")
{
	const float3 position(1024.0f, -2048.0f, 32.0f);
	const float3 direction(0.25f, 0.75f, 0.0f);
	const WindField::WindTuning tuning;
	const std::array<WindField::AmbientGust, WindField::kAmbientGustCapacity> gusts{};
	const WindField::WindSample first = WindField::SampleWind(position, direction, 0.8f, tuning, gusts, 0u);
	const WindField::WindSample second = WindField::SampleWind(position, direction, 0.8f, tuning, gusts, 0u);

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
	fastTransportTuning.gustSpeedMin *= 2.0f;
	fastTransportTuning.gustSpeedMax *= 2.0f;
	fastTransportTuning.gustAdvectionMultiplier = 8.0f;
	const std::array<WindField::AmbientGust, WindField::kAmbientGustCapacity> gusts{};

	const auto baseline = WindField::SampleWind(position, direction, 0.75f, baselineTuning, gusts, 0u);
	const auto fastTransport = WindField::SampleWind(position, direction, 0.75f, fastTransportTuning, gusts, 0u);

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
	const std::array<WindField::AmbientGust, WindField::kAmbientGustCapacity> gusts{};

	const auto first = WindField::SampleWind(float3(0.0f, 0.0f, 0.0f), direction, 0.8f, tuning, gusts, 0u);
	const auto second = WindField::SampleWind(float3(4096.0f, -2048.0f, 0.0f), direction, 0.8f, tuning, gusts, 0u);

	REQUIRE(first.velocity.x == Approx(0.0f));
	REQUIRE(first.velocity.y == Approx(0.8f));
	REQUIRE(second.velocity.x == Approx(0.0f));
	REQUIRE(second.velocity.y == Approx(0.8f));
}
