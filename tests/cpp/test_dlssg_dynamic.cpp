#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "Features/Upscaling/DLSSGDynamic.h"

TEST_CASE("Dynamic MFG is selected only when enabled, supported and requested", "[upscaling][dlssg]")
{
	CHECK(DLSSGDynamic::Select(true, true, true, 0.0f).dynamic);
	CHECK_FALSE(DLSSGDynamic::Select(false, true, true, 0.0f).dynamic);
	CHECK_FALSE(DLSSGDynamic::Select(true, false, true, 0.0f).dynamic);
	CHECK_FALSE(DLSSGDynamic::Select(true, true, false, 0.0f).dynamic);
}

TEST_CASE("Dynamic MFG passes a valid target and falls back to the refresh rate otherwise", "[upscaling][dlssg]")
{
	CHECK(DLSSGDynamic::Select(true, true, true, 120.0f).targetFrameRate == 120.0f);
	CHECK(DLSSGDynamic::Select(true, true, true, 0.0f).targetFrameRate == 0.0f);
	CHECK(DLSSGDynamic::Select(true, true, true, -30.0f).targetFrameRate == 0.0f);
	CHECK(DLSSGDynamic::Select(true, true, true, std::numeric_limits<float>::quiet_NaN()).targetFrameRate == 0.0f);
	CHECK(DLSSGDynamic::Select(true, true, true, std::numeric_limits<float>::infinity()).targetFrameRate == 0.0f);
}

TEST_CASE("Fixed mode ignores the target frame rate", "[upscaling][dlssg]")
{
	CHECK(DLSSGDynamic::Select(true, true, false, 120.0f).targetFrameRate == 0.0f);
}
