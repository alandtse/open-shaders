#include "Utils/RevisionCache.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using Util::RevisionCache;

TEST_CASE("The first read builds the value, whatever the revision", "[RevisionCache]")
{
	for (std::uint64_t revision : { std::uint64_t{ 0 }, std::uint64_t{ 1 }, UINT64_MAX }) {
		RevisionCache<int> cache;
		int builds = 0;
		CHECK(cache.Get(revision, [&] { return ++builds * 10; }) == 10);
		CHECK(builds == 1);
	}
}

TEST_CASE("The same revision reuses the value", "[RevisionCache]")
{
	RevisionCache<int> cache;
	int builds = 0;
	const auto make = [&] { return ++builds; };
	cache.Get(7, make);
	CHECK(cache.Get(7, make) == 1);
	CHECK(cache.Get(7, make) == 1);
	CHECK(builds == 1);
}

TEST_CASE("A new revision rebuilds once", "[RevisionCache]")
{
	RevisionCache<int> cache;
	int builds = 0;
	const auto make = [&] { return ++builds; };
	cache.Get(1, make);
	CHECK(cache.Get(2, make) == 2);
	CHECK(cache.Get(2, make) == 2);
	CHECK(builds == 2);
}

TEST_CASE("Going back to an older revision rebuilds", "[RevisionCache]")
{
	RevisionCache<int> cache;
	int builds = 0;
	const auto make = [&] { return ++builds; };
	cache.Get(5, make);
	cache.Get(6, make);
	CHECK(cache.Get(5, make) == 3);
}

TEST_CASE("The returned reference holds the cached value", "[RevisionCache]")
{
	RevisionCache<int> cache;
	const int& first = cache.Get(1, [] { return 42; });
	const int& second = cache.Get(1, [] { return 0; });
	CHECK(&first == &second);
	CHECK(second == 42);
}
