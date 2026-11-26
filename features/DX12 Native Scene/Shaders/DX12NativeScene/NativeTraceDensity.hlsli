#ifndef NATIVE_TRACE_DENSITY_HLSLI
#define NATIVE_TRACE_DENSITY_HLSLI

#ifdef __cplusplus
#	include <cstdint>
namespace DX12Native
{
	using uint = uint32_t;
#	define NATIVE_DENSITY_INLINE inline
#else
#	define NATIVE_DENSITY_INLINE
#endif

	NATIVE_DENSITY_INLINE uint NativeDensityHash(uint value)
	{
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		return value ^ (value >> 16u);
	}


#ifdef __cplusplus
}
#endif
#undef NATIVE_DENSITY_INLINE
#endif
