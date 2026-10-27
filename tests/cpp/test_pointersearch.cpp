// Unit tests for Util::FindPointer, which lets the light list search answer from a vector compare instead of one entry at a time.

#include "Utils/PointerSearch.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace
{
	std::vector<const void*> MakeList(std::uint32_t a_size)
	{
		std::vector<const void*> list(a_size);
		for (std::uint32_t i = 0; i < a_size; ++i)
			list[i] = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x1000) + i * 0x140);
		return list;
	}
}

TEST_CASE("An empty list never contains the pointer", "[PointerSearch]")
{
	const void* const* none = nullptr;
	CHECK(Util::FindPointer(none, 0, reinterpret_cast<const void*>(0x1000)) == 0);
	CHECK(Util::FindPointerScalar(none, 0, reinterpret_cast<const void*>(0x1000)) == 0);
}

TEST_CASE("Every position is found at its index for every list length", "[PointerSearch]")
{
	for (std::uint32_t size = 1; size <= 37; ++size) {
		const auto list = MakeList(size);
		for (std::uint32_t index = 0; index < size; ++index) {
			CHECK(Util::FindPointerScalar(list.data(), size, list[index]) == index);
			CHECK(Util::FindPointer(list.data(), size, list[index]) == index);
		}
	}
}

TEST_CASE("An absent pointer returns the list size", "[PointerSearch]")
{
	const void* absent = reinterpret_cast<const void*>(0xDEAD0);
	for (std::uint32_t size = 0; size <= 37; ++size) {
		const auto list = MakeList(size);
		CHECK(Util::FindPointerScalar(list.data(), size, absent) == size);
		CHECK(Util::FindPointer(list.data(), size, absent) == size);
	}
}

TEST_CASE("A pointer listed twice reports the first position", "[PointerSearch]")
{
	auto list = MakeList(11);
	list[9] = list[2];
	CHECK(Util::FindPointer(list.data(), 11, list[2]) == 2);
	CHECK(Util::FindPointerScalar(list.data(), 11, list[2]) == 2);
}

TEST_CASE("The AVX2 search agrees with the scalar search", "[PointerSearch]")
{
	if (!Util::HasAvx2())
		SKIP("AVX2 is not available on this machine");

	const auto list = MakeList(257);
	for (std::uint32_t index = 0; index < 257; ++index)
		CHECK(Util::FindPointerAvx2(list.data(), 257, list[index]) == Util::FindPointerScalar(list.data(), 257, list[index]));
	CHECK(Util::FindPointerAvx2(list.data(), 257, reinterpret_cast<const void*>(0xDEAD0)) == 257);
}
