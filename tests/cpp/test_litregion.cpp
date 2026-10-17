// Unit tests for the spot lit-region distance
// (src/Features/LightLimitFix/LitRegion.h).

#include "Features/LightLimitFix/LitRegion.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using Catch::Approx;
using LightLimitFixLitRegion::DistanceToFrustum;

namespace
{
	// Row-major identity: the clip box is the world-space box x,y in [-1,1], z in [0,1].
	void Identity(float m[16])
	{
		for (int i = 0; i < 16; ++i)
			m[i] = 0.0f;
		m[0] = m[5] = m[10] = m[15] = 1.0f;
	}
}

TEST_CASE("DistanceToFrustum is zero inside the clip box", "[llf]")
{
	float m[16];
	Identity(m);
	const float inside[3] = { 0.0f, 0.5f, 0.5f };
	const float onFace[3] = { 1.0f, 0.0f, 0.0f };
	REQUIRE(DistanceToFrustum(m, inside) == Approx(0.0f));
	REQUIRE(DistanceToFrustum(m, onFace) == Approx(0.0f));
}

TEST_CASE("DistanceToFrustum measures the distance past each face", "[llf]")
{
	float m[16];
	Identity(m);
	const float pastX[3] = { 4.0f, 0.0f, 0.5f };
	const float pastNegY[3] = { 0.0f, -3.0f, 0.5f };
	const float pastNear[3] = { 0.0f, 0.0f, -2.0f };
	const float pastFar[3] = { 0.0f, 0.0f, 6.0f };
	REQUIRE(DistanceToFrustum(m, pastX) == Approx(3.0f));
	REQUIRE(DistanceToFrustum(m, pastNegY) == Approx(2.0f));
	REQUIRE(DistanceToFrustum(m, pastNear) == Approx(2.0f));
	REQUIRE(DistanceToFrustum(m, pastFar) == Approx(5.0f));
}

TEST_CASE("DistanceToFrustum follows a translated and scaled box", "[llf]")
{
	float m[16];
	Identity(m);
	m[0] = m[5] = 0.5f;  // x,y world half-extent 2
	m[10] = 0.25f;       // z world extent 4
	m[12] = -1.0f;       // x world range [0, 4]
	const float inside[3] = { 2.0f, 0.0f, 1.0f };
	const float outside[3] = { 6.0f, 0.0f, 1.0f };
	REQUIRE(DistanceToFrustum(m, inside) == Approx(0.0f).margin(1e-4f));
	REQUIRE(DistanceToFrustum(m, outside) == Approx(2.0f).margin(1e-4f));
}

TEST_CASE("DistanceToFrustum handles a perspective projection", "[llf]")
{
	// Row-vector perspective looking down +z: w = z, x/y clip = x/y (90 degree fov),
	// z clip = z - 1 so the near plane is at z = 1 (clip z in [0,w] gives z in [1,inf)).
	float m[16] = {};
	m[0] = 1.0f;
	m[5] = 1.0f;
	m[10] = 1.0f;
	m[14] = -1.0f;
	m[11] = 1.0f;
	const float inside[3] = { 0.0f, 0.0f, 10.0f };
	const float behindNear[3] = { 0.0f, 0.0f, -4.0f };
	REQUIRE(DistanceToFrustum(m, inside) == Approx(0.0f));
	REQUIRE(DistanceToFrustum(m, behindNear) > 4.0f);
}

TEST_CASE("DistanceToFrustum rejects unusable input", "[llf]")
{
	float m[16];
	Identity(m);
	const float p[3] = { 0.0f, 0.0f, 0.5f };
	m[0] = std::numeric_limits<float>::quiet_NaN();
	REQUIRE(DistanceToFrustum(m, p) < 0.0f);

	float zero[16] = {};
	REQUIRE(DistanceToFrustum(zero, p) < 0.0f);
}
