#pragma once

namespace DX12
{
	inline constexpr char kOffloadingProfile[] = "ShadowOffloading";
	inline constexpr char kRayTracedProfile[] = "RayTracedStaticShadows";

	/** @brief Legacy flat settings belong only to the formerly selected native profile. */
	template <class Json>
	Json ReadModeProfile(const Json& input, bool rayTraced, bool legacyRayTraced)
	{
		const auto* key = rayTraced ? kRayTracedProfile : kOffloadingProfile;
		if (input.contains(key) && input.at(key).is_object())
			return input.at(key);
		if (!input.contains(kOffloadingProfile) && !input.contains(kRayTracedProfile) && rayTraced == legacyRayTraced)
			return input;
		return Json::object();
	}
}
