// Unit tests for Util::DevBenchNotices. A notice rides on every devbench reply, so a clean
// renderer must produce none and an unchanged one must not be re-sent.

#include "Utils/DevBenchNotices.h"

#include <catch2/catch_test_macros.hpp>

using namespace Util::DevBenchNotices;

namespace
{
	bool Has(const std::vector<Notice>& a_notices, const char* a_key)
	{
		for (const auto& notice : a_notices) {
			if (notice.key == a_key)
				return true;
		}
		return false;
	}
}

TEST_CASE("A healthy renderer posts nothing", "[DevBenchNotices]")
{
	REQUIRE(Evaluate({}).empty());
}

TEST_CASE("Each condition maps to its own notice", "[DevBenchNotices]")
{
	Conditions conditions;
	conditions.shadersCompiling = true;
	conditions.failedShaders = 3;
	conditions.cacheAwaitingDecision = true;
	conditions.rejectedFeatures = { "Old Feature" };
	conditions.restartPendingFeatures = { "Reverse Z" };

	const auto notices = Evaluate(conditions);
	REQUIRE(notices.size() == 5);
	REQUIRE(Has(notices, kKeyRestart));
	REQUIRE(Has(notices, kKeyCompiling));
	REQUIRE(Has(notices, kKeyFailed));
	REQUIRE(Has(notices, kKeyCache));
	REQUIRE(Has(notices, kKeyFeatures));
}

TEST_CASE("Compile failures are critical and listed first", "[DevBenchNotices]")
{
	Conditions conditions;
	conditions.shadersCompiling = true;
	conditions.failedShaders = 1;

	const auto notices = Evaluate(conditions);
	REQUIRE(notices.front().key == kKeyFailed);
	REQUIRE(notices.front().severity == Severity::Critical);
	REQUIRE(std::string(SeverityName(notices.front().severity)) == "critical");
}

TEST_CASE("The rejected feature list is capped", "[DevBenchNotices]")
{
	Conditions conditions;
	for (int i = 0; i < 8; ++i)
		conditions.rejectedFeatures.push_back("F" + std::to_string(i));

	const auto notices = Evaluate(conditions);
	REQUIRE(notices.size() == 1);
	REQUIRE(notices[0].text.find("F4") != std::string::npos);
	REQUIRE(notices[0].text.find("F5") == std::string::npos);
	REQUIRE(notices[0].text.find("+3 more") != std::string::npos);
	REQUIRE(notices[0].text.size() < 500);
}

TEST_CASE("Every notice fits devbench's text limit", "[DevBenchNotices]")
{
	Conditions conditions;
	conditions.shadersCompiling = true;
	conditions.failedShaders = 1;
	conditions.cacheAwaitingDecision = true;
	conditions.rejectedFeatures = { "A", "B", "C", "D", "E", "F" };

	for (const auto& notice : Evaluate(conditions))
		REQUIRE(notice.text.size() <= 500);
}

TEST_CASE("The reconciler posts once, then only on change, and clears resolved notices", "[DevBenchNotices]")
{
	Reconciler reconciler;
	Conditions conditions;
	conditions.shadersCompiling = true;

	auto delta = reconciler.Update(Evaluate(conditions));
	REQUIRE(delta.post.size() == 1);
	REQUIRE(delta.clear.empty());

	delta = reconciler.Update(Evaluate(conditions));
	REQUIRE(delta.post.empty());
	REQUIRE(delta.clear.empty());

	conditions.failedShaders = 2;
	delta = reconciler.Update(Evaluate(conditions));
	REQUIRE(delta.post.size() == 1);
	REQUIRE(delta.post[0].key == kKeyFailed);

	conditions = {};
	delta = reconciler.Update(Evaluate(conditions));
	REQUIRE(delta.post.empty());
	REQUIRE(delta.clear.size() == 2);
}
