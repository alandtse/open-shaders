#pragma once

namespace DX12
{
	enum class SCMNativeMode
	{
		Disabled,
		RasterComparison,
		ShadowOffload,
		FullShadow
	};

	struct SCMNativePolicy
	{
		bool observeGeometry = false;
		bool observeLights = false;
	};

	/** @brief Raster comparison permits geometry preparation but never native shadow ownership. */
	constexpr SCMNativePolicy SelectSCMNativePolicy(bool active, bool shadowOffload, bool vr)
	{
		return active && !vr ? SCMNativePolicy{ true, shadowOffload } : SCMNativePolicy{};
	}

	constexpr SCMNativePolicy SelectSCMNativePolicy(SCMNativeMode mode)
	{
		return SelectSCMNativePolicy(mode == SCMNativeMode::ShadowOffload || mode == SCMNativeMode::RasterComparison,
			mode == SCMNativeMode::ShadowOffload, false);
	}

	enum class SCMNativeCasterMode
	{
		Disabled,
		Observe,
		Suppress
	};

	/** @brief Selects the native extension for the synchronous SCM caster walk. */
	constexpr SCMNativeCasterMode SelectSCMNativeCasterMode(SCMNativePolicy policy, bool nativeOwned)
	{
		if (!policy.observeGeometry)
			return SCMNativeCasterMode::Disabled;
		if (!policy.observeLights || !nativeOwned)
			return SCMNativeCasterMode::Observe;
		return SCMNativeCasterMode::Suppress;
	}
}
