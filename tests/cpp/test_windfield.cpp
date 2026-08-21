#include "Utils/WindField.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
		const WindField::WindSample sample = WindField::SampleWind(position, travelDistance, direction, speed, tuning);          \
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
	const WindField::WindSample first = WindField::SampleWind(position, 33.6f, direction, 0.8f, tuning);
	const WindField::WindSample second = WindField::SampleWind(position, 33.6f, direction, 0.8f, tuning);

	REQUIRE(first.ambientGust == second.ambientGust);
	REQUIRE(first.velocity.x == second.velocity.x);
	REQUIRE(first.velocity.y == second.velocity.y);
	REQUIRE(first.velocity.z == second.velocity.z);
}
