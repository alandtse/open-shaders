#ifndef WATER_FINITE_HLSLI
#define WATER_FINITE_HLSLI

namespace WaterFinite
{
	// FXC fast-math can discard isfinite checks on computed values; classify the stored exponent instead.
	bool IsFinite(float value)
	{
		return (asuint(value) & 0x7f800000u) != 0x7f800000u;
	}

	bool2 IsFinite(float2 value)
	{
		return (asuint(value) & 0x7f800000u) != 0x7f800000u;
	}

	bool3 IsFinite(float3 value)
	{
		return (asuint(value) & 0x7f800000u) != 0x7f800000u;
	}

	bool4 IsFinite(float4 value)
	{
		return (asuint(value) & 0x7f800000u) != 0x7f800000u;
	}
}

#endif
