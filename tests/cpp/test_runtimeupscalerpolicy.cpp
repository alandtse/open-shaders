// Unit tests for RuntimeUpscalerPolicy, which decides when FSR needs a D3D12 device and when the probe releases it.

#include "Features/Upscaling/RuntimeUpscalerPolicy.h"

#include <catch2/catch_test_macros.hpp>

using RuntimeUpscalerPolicy::ReleaseProbeDevice;
using RuntimeUpscalerPolicy::UseRuntimeProvider;

TEST_CASE("AMD uses the runtime provider with or without an upgrade", "[RuntimeUpscalerPolicy]")
{
	STATIC_CHECK(UseRuntimeProvider(true, true, false));
	STATIC_CHECK(UseRuntimeProvider(true, true, true));
}

TEST_CASE("Other vendors use the runtime provider only for a selected upgrade", "[RuntimeUpscalerPolicy]")
{
	STATIC_CHECK_FALSE(UseRuntimeProvider(true, false, false));
	STATIC_CHECK(UseRuntimeProvider(true, false, true));
}

TEST_CASE("A missing provider is never used", "[RuntimeUpscalerPolicy]")
{
	STATIC_CHECK_FALSE(UseRuntimeProvider(false, true, true));
	STATIC_CHECK_FALSE(UseRuntimeProvider(false, false, true));
}

TEST_CASE("The probe releases only an unused device it created", "[RuntimeUpscalerPolicy]")
{
	STATIC_CHECK(ReleaseProbeDevice(true, false, false));
	STATIC_CHECK_FALSE(ReleaseProbeDevice(false, false, false));
	STATIC_CHECK_FALSE(ReleaseProbeDevice(true, true, false));
	STATIC_CHECK_FALSE(ReleaseProbeDevice(true, false, true));
}
