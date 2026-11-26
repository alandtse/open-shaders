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

	/** @brief Raster comparison permits geometry preparation but never native shadow ownership. */
	constexpr SCMNativePolicy SelectSCMNativePolicy(SCMNativeMode mode)
	{
		switch (mode) {
		case SCMNativeMode::RasterComparison:
			return { true, false, false };
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
		if (!active || vr)
			return SCMNativeMode::Disabled;
		if (!eligibleCell)
			return SCMNativeMode::RasterComparison;
		return fullRT ? SCMNativeMode::FullRT :
		       offloading ? SCMNativeMode::ShadowOffload : SCMNativeMode::RasterComparison;
	}

	/** @brief Full RT owns every supported point light, independent of native scene coverage. */
	constexpr bool NativeStaticAuthority(SCMNativePolicy policy, bool supportedPointLight, bool matched)
	{
		return supportedPointLight && (policy.fullStaticAuthority || (policy.observeLights && matched));
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
