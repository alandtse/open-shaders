#include "Utils/LastUpload.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using Util::LastUpload;

namespace
{
	struct Block
	{
		float values[4];
		std::uint32_t count;
	};
}

TEST_CASE("A fresh cache asks for the first upload, even of all-zero data", "[LastUpload]")
{
	LastUpload<Block> last;
	CHECK(last.Differs(Block{}));
}

TEST_CASE("Recorded bytes are not uploaded again", "[LastUpload]")
{
	LastUpload<Block> last;
	const Block data{ { 1.0f, 2.0f, 3.0f, 4.0f }, 5 };
	last.Record(data);
	CHECK_FALSE(last.Differs(data));
}

TEST_CASE("Any changed field asks for an upload", "[LastUpload]")
{
	LastUpload<Block> last;
	const Block data{ { 1.0f, 2.0f, 3.0f, 4.0f }, 5 };
	last.Record(data);

	Block changedValue = data;
	changedValue.values[3] = 4.5f;
	CHECK(last.Differs(changedValue));

	Block changedCount = data;
	changedCount.count = 6;
	CHECK(last.Differs(changedCount));
}

TEST_CASE("Data that differs but was never recorded keeps asking", "[LastUpload]")
{
	LastUpload<Block> last;
	const Block first{ { 1.0f, 0.0f, 0.0f, 0.0f }, 1 };
	const Block second{ { 2.0f, 0.0f, 0.0f, 0.0f }, 1 };
	last.Record(first);
	CHECK(last.Differs(second));
	CHECK(last.Differs(second));
	last.Record(second);
	CHECK_FALSE(last.Differs(second));
	CHECK(last.Differs(first));
}

TEST_CASE("Reset forces the next upload of the same data", "[LastUpload]")
{
	LastUpload<Block> last;
	const Block data{ { 1.0f, 2.0f, 3.0f, 4.0f }, 5 };
	last.Record(data);
	last.Reset();
	CHECK(last.Differs(data));
	last.Record(data);
	CHECK_FALSE(last.Differs(data));
}
