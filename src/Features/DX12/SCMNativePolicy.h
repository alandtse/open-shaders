#pragma once

namespace DX12
{
	enum class SCMNativeMode
	{
		Disabled,
		RasterComparison,
		ShadowOffload,
		FullRT
	};

	struct SCMNativePolicy
	{
		bool observeGeometry = false;
		bool observeLights = false;
		bool fullStaticAuthority = false;
	};

	/** @brief Raster comparison and disabled cells preserve the baseline SCM policy. */
	constexpr SCMNativePolicy SelectSCMNativePolicy(SCMNativeMode mode)
	{
		switch (mode) {
		case SCMNativeMode::ShadowOffload:
			return { true, true, false };
		case SCMNativeMode::FullRT:
			return { false, false, true };
		default:
			return {};
		}
	}

	/** @brief Resolve session and cell eligibility once before shadow scheduling. */
	constexpr SCMNativeMode SelectSCMNativeMode(bool active, bool vr, bool offloading, bool fullRT, bool eligibleCell)
	{
		if (!active || vr || !eligibleCell || (!offloading && !fullRT))
			return SCMNativeMode::Disabled;
		return fullRT ? SCMNativeMode::FullRT : SCMNativeMode::ShadowOffload;
	}

	/**
	 * @brief Native tracing owns a supported point light's static casters once it is matched: to a traced
	 * light in shadow offload, or to a visibility slice that lighting read in Full RT.
	 */
	constexpr bool NativeStaticAuthority(SCMNativePolicy policy, bool supportedPointLight, bool matched)
	{
		return supportedPointLight && matched && (policy.fullStaticAuthority || policy.observeLights);
	}

	/** @brief Full RT admits intrinsic deformation, actor attachments or actual per-shape motion. */
	constexpr bool FullRTDynamicCaster(bool skinned, bool deforming, bool actorAttached, bool transformMoving)
	{
		return skinned || deforming || actorAttached || transformMoving;
	}

	enum class SCMNativeCasterMode
	{
		Disabled,
		Observe,
		Suppress,
		SuppressAll
	};

	/** @brief Selects the native extension for the synchronous SCM caster walk. */
	constexpr SCMNativeCasterMode SelectSCMNativeCasterMode(SCMNativePolicy policy, bool nativeOwned)
	{
		if (policy.fullStaticAuthority)
			return nativeOwned ? SCMNativeCasterMode::SuppressAll : SCMNativeCasterMode::Disabled;
		if (!policy.observeGeometry)
			return SCMNativeCasterMode::Disabled;
		return policy.observeLights && nativeOwned ? SCMNativeCasterMode::Suppress : SCMNativeCasterMode::Observe;
	}
}
