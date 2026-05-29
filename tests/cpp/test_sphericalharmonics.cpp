// Unit tests for the spherical-harmonics math library (src/Utils/SphericalHarmonics.h).
//
// Pure DirectXMath/SimpleMath; no game/D3D/ImGui dependency. The float2/3/4
// aliases come from the force-included test_prelude.h.

#include "Utils/SphericalHarmonics.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using Catch::Approx;
using SphericalHarmonics::SH2;

namespace
{
	// Band-0 / band-1 real-SH normalization constants the library hard-codes.
	constexpr float K0 = 0.28209479177387814347f;  // 1 / (2*sqrt(pi))
	constexpr float K1 = 0.48860251190291992159f;  // sqrt(3) / (2*sqrt(pi))
}

TEST_CASE("Evaluate emits the band-0 constant and axis-aligned band-1 terms", "[sh]")
{
	// c0 is direction-independent.
	REQUIRE(SphericalHarmonics::Evaluate(float3(0, 0, 1)).c0 == Approx(K0));
	REQUIRE(SphericalHarmonics::Evaluate(float3(1, 0, 0)).c0 == Approx(K0));

	// +Z: only the M=0 (c1[1]) term is non-zero, == +K1.
	const SH2 z = SphericalHarmonics::Evaluate(float3(0, 0, 1));
	REQUIRE(z.c1[0] == Approx(0.0f));
	REQUIRE(z.c1[1] == Approx(K1));
	REQUIRE(z.c1[2] == Approx(0.0f));

	// +X drives c1[2] to -K1; +Y drives c1[0] to -K1.
	REQUIRE(SphericalHarmonics::Evaluate(float3(1, 0, 0)).c1[2] == Approx(-K1));
	REQUIRE(SphericalHarmonics::Evaluate(float3(0, 1, 0)).c1[0] == Approx(-K1));
}

TEST_CASE("Dot is symmetric and matches the closed form", "[sh]")
{
	const SH2 a(1.0f, 2.0f, 3.0f, 4.0f);
	const SH2 b(0.5f, -1.0f, 2.0f, 0.0f);
	REQUIRE(SphericalHarmonics::Dot(a, b) == Approx(4.5f));
	REQUIRE(SphericalHarmonics::Dot(a, b) == Approx(SphericalHarmonics::Dot(b, a)));
}

TEST_CASE("Self-dot of a unit-direction basis equals 1/pi", "[sh]")
{
	// K0^2 + K1^2 == 1/pi for any unit direction (one band-1 term active here).
	const SH2 z = SphericalHarmonics::Evaluate(float3(0, 0, 1));
	REQUIRE(SphericalHarmonics::Dot(z, z) == Approx(1.0f / std::numbers::pi_v<float>));
}

TEST_CASE("Add is componentwise and commutative", "[sh]")
{
	const SH2 a(1.0f, 2.0f, 3.0f, 4.0f);
	const SH2 b(10.0f, 20.0f, 30.0f, 40.0f);
	const SH2 s = SphericalHarmonics::Add(a, b);
	REQUIRE(s.c0 == Approx(11.0f));
	REQUIRE(s.c1[0] == Approx(22.0f));
	REQUIRE(s.c1[1] == Approx(33.0f));
	REQUIRE(s.c1[2] == Approx(44.0f));

	const SH2 s2 = SphericalHarmonics::Add(b, a);
	REQUIRE(s.c0 == Approx(s2.c0));
	REQUIRE(s.c1[2] == Approx(s2.c1[2]));
}

TEST_CASE("Scale is linear; scaling by zero zeroes all coefficients", "[sh]")
{
	const SH2 a(1.0f, -2.0f, 3.0f, -4.0f);
	const SH2 d = SphericalHarmonics::Scale(a, 2.0f);
	REQUIRE(d.c0 == Approx(2.0f));
	REQUIRE(d.c1[0] == Approx(-4.0f));
	REQUIRE(d.c1[1] == Approx(6.0f));
	REQUIRE(d.c1[2] == Approx(-8.0f));

	const SH2 z = SphericalHarmonics::Scale(a, 0.0f);
	REQUIRE(z.c0 == Approx(0.0f));
	REQUIRE(z.c1[0] == Approx(0.0f));
	REQUIRE(z.c1[1] == Approx(0.0f));
	REQUIRE(z.c1[2] == Approx(0.0f));
}

TEST_CASE("Unproject of a pure band-0 SH returns c0 * K0 in every direction", "[sh]")
{
	// A DC-only SH (band-1 zero) projects to the same value regardless of dir.
	const SH2 dc(3.0f, 0.0f, 0.0f, 0.0f);
	REQUIRE(SphericalHarmonics::Unproject(float3(0, 0, 1), dc) == Approx(3.0f * K0));
	REQUIRE(SphericalHarmonics::Unproject(float3(1, 0, 0), dc) == Approx(3.0f * K0));
	REQUIRE(SphericalHarmonics::Unproject(float3(0, -1, 0), dc) == Approx(3.0f * K0));
}
