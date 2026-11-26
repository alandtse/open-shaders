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
		return { mode != SCMNativeMode::Disabled, mode == SCMNativeMode::ShadowOffload || mode == SCMNativeMode::FullShadow };
	}

	enum class SCMNativeCasterMode
	{
		Disabled,
		Observe,
		Suppress,
		SuppressAll
	};

	/** @brief Selects the native extension for the synchronous SCM caster walk. */
	constexpr SCMNativeCasterMode SelectSCMNativeCasterMode(SCMNativePolicy policy, bool nativeOwned, bool allLights = false)
	{
		if (!policy.observeGeometry)
			return SCMNativeCasterMode::Disabled;
		if (!policy.observeLights || !nativeOwned)
			return SCMNativeCasterMode::Observe;
		return allLights ? SCMNativeCasterMode::SuppressAll : SCMNativeCasterMode::Suppress;
	}
}
