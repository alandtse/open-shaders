#include "Features/Upscaling/NeuralRendering/MaterialStrengthPolicy.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Provider material failures do not disable reduced-model selection", "[nr]")
{
	using namespace NR::MaterialStrength;
	for (const bool rejected : { false, true }) {
		for (const bool degraded : { false, true }) {
			REQUIRE(IsAvailable(true, rejected, degraded, false));
			REQUIRE_FALSE(IsAvailable(true, rejected, degraded, true));
		}
	}
	REQUIRE(IsAvailable(false, false, false, false));
	REQUIRE(IsAvailable(false, false, false, true));
	REQUIRE_FALSE(IsAvailable(false, true, false, false));
	REQUIRE_FALSE(IsAvailable(false, false, true, false));
	REQUIRE_FALSE(IsAvailable(false, true, true, false));
}

TEST_CASE("Identity material protection is cleared once while its provider binding remains resident", "[nr]")
{
	using namespace NR::MaterialStrength;
	const Values identity;
	REQUIRE_FALSE(HasEdits(identity));
	REQUIRE(PlanAlphaUpdate(identity, false) == AlphaUpdate::kClear);
	REQUIRE(PlanAlphaUpdate(identity, true) == AlphaUpdate::kReuse);
	for (uint32_t category = 0; category < kCount; ++category) {
		auto edited = identity;
		edited.strength[category] = 0.5f;
		REQUIRE(HasEdits(edited));
		REQUIRE(PlanAlphaUpdate(edited, false) == AlphaUpdate::kDraw);
		REQUIRE(PlanAlphaUpdate(edited, true) == AlphaUpdate::kDraw);
	}
}

TEST_CASE("Material selection ignores edge softness for an identity mask", "[nr]")
{
	using namespace NR::MaterialStrength;
	for (uint32_t radius = 0; radius <= kMaxEdgeSoftness; ++radius) {
		auto identity = Values{};
		identity.edgeSoftness = radius;
		REQUIRE_FALSE(HasEdits(identity));
		REQUIRE(PlanAlphaUpdate(identity, true) == AlphaUpdate::kReuse);
	}
}
