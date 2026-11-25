#pragma once

namespace DX12
{
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
		return policy.observeLights && nativeOwned ? SCMNativeCasterMode::Suppress : SCMNativeCasterMode::Observe;
	}
}
