// Unit tests for ShadowCasterManager::FormulaHelper
// (src/Features/LightLimitFix/FormulaHelper.{h,cpp}).
//
// Compiled in the opt-in cpp_tests_slow target only: it pulls in the very large
// exprtk header, which is too slow for the default fast cpp_tests suite.

#include "Features/LightLimitFix/FormulaHelper.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using Catch::Approx;
using ShadowCasterManager::FormulaHelper;
namespace scm = ShadowCasterManager;

TEST_CASE("Parse + Calculate evaluates an arithmetic expression", "[formula]")
{
	FormulaHelper f;
	REQUIRE(f.Parse("2 + 3 * 4"));
	REQUIRE(f.Calculate() == Approx(14.0));
}

TEST_CASE("Calculate before a successful Parse returns 0", "[formula]")
{
	FormulaHelper f;
	REQUIRE(f.Calculate() == Approx(0.0));
}

TEST_CASE("Formula reads bound parameters and re-evaluates live", "[formula]")
{
	FormulaHelper::SetParam(scm::kFormulaParam_LightIntensity, 2.0);
	FormulaHelper::SetParam(scm::kFormulaParam_LightDistance, 100.0);
	FormulaHelper f;
	REQUIRE(f.Parse("lightintensity * 10 + lightdistance"));
	REQUIRE(f.Calculate() == Approx(120.0));

	// Calculate re-reads the live param each call.
	FormulaHelper::SetParam(scm::kFormulaParam_LightIntensity, 3.0);
	REQUIRE(f.Calculate() == Approx(130.0));
}

TEST_CASE("Parse rejects malformed syntax and unknown variables", "[formula]")
{
	FormulaHelper syntax;
	REQUIRE_FALSE(syntax.Parse("2 +"));  // incomplete expression
	REQUIRE(syntax.Calculate() == Approx(0.0));  // failed parse leaves it unset

	FormulaHelper unknown;
	REQUIRE_FALSE(unknown.Parse("notavariable * 2"));  // unregistered symbol
}

TEST_CASE("Parse is single-shot; a second Parse on the same helper is rejected", "[formula]")
{
	FormulaHelper f;
	REQUIRE(f.Parse("1"));
	REQUIRE_FALSE(f.Parse("2"));  // already parsed -> use Reparse instead
	REQUIRE(f.Calculate() == Approx(1.0));
}

TEST_CASE("Validate reports success/failure with an error message", "[formula]")
{
	std::string err = "preset";
	REQUIRE(FormulaHelper::Validate("lightradius + 1", err));

	std::string err2;
	REQUIRE_FALSE(FormulaHelper::Validate("1 + + 2", err2));
	REQUIRE_FALSE(err2.empty());
}

TEST_CASE("Reparse keeps the old formula when the new one is invalid", "[formula]")
{
	FormulaHelper f;
	REQUIRE(f.Parse("5"));
	REQUIRE(f.Calculate() == Approx(5.0));

	REQUIRE_FALSE(f.Reparse("bad +"));  // invalid -> old formula stays active
	REQUIRE(f.Calculate() == Approx(5.0));

	REQUIRE(f.Reparse("7"));  // valid -> replaces
	REQUIRE(f.Calculate() == Approx(7.0));
}
