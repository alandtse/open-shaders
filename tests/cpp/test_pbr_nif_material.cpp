#include "Materials/PBRNifMaterial.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("PBR NIF coat features take precedence over hair and thin-surface", "[native_pbr][nif]")
{
	const auto material = PBRRecords::InterpretNifSurface({
		.multiLayerParallax = true,
		.backLighting = true,
		.effectLighting = true,
		.anisotropicLighting = true,
		.rimLighting = true,
		.softLighting = true,
		.fitSlope = true
	});
	const auto expected = uint32_t(PBRFlags::TwoLayer) | uint32_t(PBRFlags::CoatNormal) |
		uint32_t(PBRFlags::ColoredCoat) | uint32_t(PBRFlags::InterlayerParallax);
	REQUIRE(material.pbrFlags == expected);
	REQUIRE(material.thinSurfaceFlags == 0);
	REQUIRE_FALSE(material.applyThinSurfaceFlags);
	REQUIRE_FALSE(material.enableGlint);
}

TEST_CASE("PBR NIF hair takes precedence over other surface lobes", "[native_pbr][nif]")
{
	const auto material = PBRRecords::InterpretNifSurface({
		.backLighting = true,
		.effectLighting = true,
		.anisotropicLighting = true,
		.rimLighting = true,
		.softLighting = true,
		.fitSlope = true
	});
	REQUIRE(material.pbrFlags == uint32_t(PBRFlags::HairMarschner));
	REQUIRE(material.thinSurfaceFlags == 0);
	REQUIRE_FALSE(material.applyThinSurfaceFlags);
	REQUIRE_FALSE(material.enableGlint);
}

TEST_CASE("PBR NIF thin-surface flags combine with subsurface and fuzz", "[native_pbr][nif]")
{
	const auto material = PBRRecords::InterpretNifSurface({
		.effectLighting = true,
		.anisotropicLighting = true,
		.rimLighting = true,
		.softLighting = true,
		.fitSlope = true
	});
	REQUIRE(material.applyThinSurfaceFlags);
	REQUIRE(material.thinSurfaceFlags == (uint32_t(PBRFlags::ThinSurface) | uint32_t(PBRFlags::Foliage)));
	REQUIRE(material.pbrFlags == (uint32_t(PBRFlags::Subsurface) | uint32_t(PBRFlags::Fuzz)));
	REQUIRE_FALSE(material.enableGlint);
}

TEST_CASE("PBR NIF slope glint requires no soft lighting", "[native_pbr][nif]")
{
	const auto glint = PBRRecords::InterpretNifSurface({ .fitSlope = true });
	REQUIRE(glint.applyThinSurfaceFlags);
	REQUIRE(glint.enableGlint);
	REQUIRE(glint.pbrFlags == 0);
	REQUIRE(glint.thinSurfaceFlags == 0);

	const auto plain = PBRRecords::InterpretNifSurface({});
	REQUIRE(plain.applyThinSurfaceFlags);
	REQUIRE_FALSE(plain.enableGlint);
}
