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
	/** @brief Readiness is separate from the additive lighting-validity policy. */
	VALIDITY_INLINE bool NativeFirstReady(uint stage, uint effective, uint reason, bool a_lightingAccepted)
	{
		const bool geometry = (reason & 0x08000000u) != 0u;
		const bool material = (reason & 0x04000000u) != 0u;
		if (stage == 1u)
			return geometry;
		if (stage == 2u)
			return geometry && material;
		return stage == 3u && geometry && material && effective == 1u &&
			(reason & 0x20000000u) != 0u && a_lightingAccepted;
	}
#ifdef __cplusplus
}
#endif
#undef VALIDITY_INLINE
#endif
