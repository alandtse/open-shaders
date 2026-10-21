#include "Utils/MathUtils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <format>
#include <limits>
#include <string_view>

using Catch::Approx;

TEST_CASE("ClampFinite clamps in-range, low, and high inputs", "[utils][math]")
{
	REQUIRE(Util::ClampFinite(0.5f, 0.0f, 1.0f, 0.25f) == Approx(0.5f));
	REQUIRE(Util::ClampFinite(-1.0f, 0.0f, 1.0f, 0.25f) == Approx(0.0f));
	REQUIRE(Util::ClampFinite(2.0f, 0.0f, 1.0f, 0.25f) == Approx(1.0f));
	REQUIRE(Util::ClampFinite(64.0f, 64.0f, 4096.0f, 128.0f) == Approx(64.0f));
	REQUIRE(Util::ClampFinite(4096.0f, 64.0f, 4096.0f, 128.0f) == Approx(4096.0f));
}

TEST_CASE("ClampFinite falls back to default on non-finite input", "[utils][math]")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	REQUIRE(Util::ClampFinite(nan, 0.0f, 1.0f, 0.5f) == Approx(0.5f));
	REQUIRE(Util::ClampFinite(inf, 0.0f, 1.0f, 0.5f) == Approx(0.5f));
	REQUIRE(Util::ClampFinite(-inf, 0.0f, 1.0f, 0.5f) == Approx(0.5f));
}

TEST_CASE("ClampFiniteOrDefault behaves identically to ClampFinite", "[utils][math]")
{
	REQUIRE(Util::ClampFiniteOrDefault(0.75f, 0.0f, 1.0f, 0.1f) == Approx(0.75f));
	const float nan = std::numeric_limits<float>::quiet_NaN();
	REQUIRE(Util::ClampFiniteOrDefault(nan, 0.0f, 1.0f, 0.1f) == Approx(0.1f));
}

TEST_CASE("QuantizeFloat rounds to nearest multiple of step", "[utils][math]")
{
	REQUIRE(Util::QuantizeFloat(1.23f, 0.5f) == Approx(1.0f));
	REQUIRE(Util::QuantizeFloat(1.26f, 0.5f) == Approx(1.5f));
	REQUIRE(Util::QuantizeFloat(10.0f, 2.5f) == Approx(10.0f));
}

TEST_CASE("HashCombine produces deterministic non-zero combinations", "[utils][math]")
{
	const auto h1 = Util::HashCombine(0, 42);
	const auto h2 = Util::HashCombine(h1, 100);
	REQUIRE(h1 != 0);
	REQUIRE(h2 != h1);
	REQUIRE(Util::HashCombine(0, 42) == h1);
}

TEST_CASE("Smoothstep ramps between its edges and tolerates equal edges", "[utils][math]")
{
	REQUIRE(Util::Smoothstep(0.0f, 10.0f, -5.0f) == Approx(0.0f));
	REQUIRE(Util::Smoothstep(0.0f, 10.0f, 5.0f) == Approx(0.5f));
	REQUIRE(Util::Smoothstep(0.0f, 10.0f, 50.0f) == Approx(1.0f));
	REQUIRE(Util::Smoothstep(2.0f, 2.0f, 1.0f) == Approx(0.0f));
	REQUIRE(Util::Smoothstep(2.0f, 2.0f, 3.0f) == Approx(1.0f));
	REQUIRE(std::isfinite(Util::Smoothstep(0.0f, 0.0f, 0.0f)));
}

TEST_CASE("ExpBlend approaches 1 and treats a non-positive time constant as a jump", "[utils][math]")
{
	REQUIRE(Util::ExpBlend(0.0f, 0.1f) == Approx(0.0f));
	REQUIRE(Util::ExpBlend(0.1f, 0.1f) == Approx(1.0f - std::exp(-1.0f)));
	REQUIRE(Util::ExpBlend(10.0f, 0.1f) == Approx(1.0f));
	REQUIRE(Util::ExpBlend(0.0f, 0.0f) == 1.0f);
	REQUIRE(Util::ExpBlend(0.1, 0.1) == Approx(1.0 - std::exp(-1.0)));
}

TEST_CASE("SafeNormalize returns the zero vector instead of NaN", "[utils][math]")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	REQUIRE(Util::SafeNormalize({ 3.0f, 0.0f, 4.0f }).Length() == Approx(1.0f));
	REQUIRE(Util::SafeNormalize({}).Length() == 0.0f);
	REQUIRE(Util::SafeNormalize({ 1e-5f, 0.0f, 0.0f }).Length() == 0.0f);
	REQUIRE(Util::SafeNormalize({ 0.5f, 0.0f, 0.0f }, 1.0f).Length() == 0.0f);
	REQUIRE(Util::SafeNormalize({ nan, 0.0f, 0.0f }).Length() == 0.0f);
	REQUIRE_FALSE(Util::IsFinite({ 0.0f, nan, 0.0f }));
	REQUIRE(Util::IsFinite({ 1.0f, 2.0f, 3.0f }));
}

TEST_CASE("Fnv1a matches the published test vectors and chains", "[utils][math]")
{
	REQUIRE(Util::Fnv1a32("") == 0x811C9DC5u);
	REQUIRE(Util::Fnv1a32("a") == 0xE40C292Cu);
	REQUIRE(Util::Fnv1a32("foobar") == 0xBF9CF968u);
	REQUIRE(Util::Fnv1a64("") == 0xCBF29CE484222325ull);
	REQUIRE(Util::Fnv1a64("a") == 0xAF63DC4C8601EC8Cull);
	REQUIRE(Util::Fnv1a64("foobar") == 0x85944171F73967E8ull);
	REQUIRE(Util::Fnv1a32("bar", Util::Fnv1a32("foo")) == Util::Fnv1a32("foobar"));
	REQUIRE(Util::Fnv1a64("bar", Util::Fnv1a64("foo")) == Util::Fnv1a64("foobar"));
}

TEST_CASE("Fnv1a pins the cache file name and load-order hash inputs", "[utils][math][hash]")
{
	const auto suffix = [](std::string_view a_defines) {
		return std::format("_{:08X}", Util::Fnv1a32(a_defines));
	};
	REQUIRE(suffix("LIGHTING VC DEFERRED SKINNED") == "_F62EFEAB");
	REQUIRE(suffix("FUR_SHELLS FUR_SHELLS_DEPTH") == "_09127D93");
	REQUIRE(suffix("A") == "_C40BF6CC");
	REQUIRE(suffix("DEFINE_WITH_UNDERSCORES 1 2 3") == "_701DB32D");

	std::uint64_t loadOrder = Util::kFnv1a64Basis;
	for (const std::string_view name : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "Dragonborn.esm", "Plugin.esp", "2.19.0" })
		loadOrder = Util::Fnv1a64(name, loadOrder);
	REQUIRE(loadOrder == 0x9AC194451BE5CFBBull);

	std::uint32_t waterNormals = Util::kFnv1a32Basis;
	for (const std::string_view name : { "textures/water/a.dds", "textures/water/b.dds", "", "textures/water/c.dds" })
		waterNormals = Util::Fnv1a32(name, waterNormals);
	REQUIRE(waterNormals == 0x265DC651u);
}
