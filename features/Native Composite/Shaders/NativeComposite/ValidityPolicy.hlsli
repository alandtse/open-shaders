#ifndef NATIVE_COMPOSITE_VALIDITY_POLICY_HLSLI
#define NATIVE_COMPOSITE_VALIDITY_POLICY_HLSLI
#ifdef __cplusplus
#	include <cstdint>
namespace NativeCompositeValidity
{
	using uint = uint32_t;
#	define VALIDITY_INLINE inline
#else
#	define VALIDITY_INLINE
#endif
	VALIDITY_INLINE bool AcceptContribution(uint policy, uint original, uint effective, uint reason)
	{
		if (policy == 0)
			return original == 1;
		if (policy == 1)
			return effective == 1;
		return policy == 2 && (reason & 0x20000000u) != 0;
	}
#ifdef __cplusplus
}
#endif
#undef VALIDITY_INLINE
#endif
