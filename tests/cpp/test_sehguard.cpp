#include "Utils/SehGuard.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
	constexpr DWORD kCxxExceptionCode = 0xE06D7363;
}

TEST_CASE("Seh guard completes a normal call", "[sehguard]")
{
	bool ran = false;
	const bool completed = Util::SehGuarded([&] { ran = true; });

	REQUIRE(completed);
	REQUIRE(ran);
}

TEST_CASE("Seh guard catches an access violation and reports its code", "[sehguard]")
{
	DWORD exceptionCode = 0;
	volatile int* nullWrite = nullptr;
	const bool completed = Util::SehGuarded([&] { *nullWrite = 1; }, &exceptionCode);

	REQUIRE_FALSE(completed);
	REQUIRE(exceptionCode == EXCEPTION_ACCESS_VIOLATION);
}

TEST_CASE("Seh guard catches a thrown C++ exception and reports its code", "[sehguard]")
{
	DWORD exceptionCode = 0;
	const bool completed = Util::SehGuarded([&] { throw 1; }, &exceptionCode);

	REQUIRE_FALSE(completed);
	REQUIRE(exceptionCode == kCxxExceptionCode);
}
