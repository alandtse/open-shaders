#pragma once

#include <bit>
#include <cstdint>

namespace PointLightFlags
{
	enum class Flags : std::uint32_t
	{
		PortalStrict = (1u << 0),
		Shadow = (1u << 1),
		Simple = (1u << 2),

		Initialised = (1u << 8),
		Disabled = (1u << 9),
		InverseSquare = (1u << 10),
		Linear = (1u << 11),
		Particle = (1u << 12),
		Spot = (1u << 13),
		OmniDirectional = (1u << 14),
	};

	constexpr std::uint32_t ToMask(Flags a_flag) noexcept
	{
		return static_cast<std::uint32_t>(a_flag);
	}

	inline std::uint32_t GetRuntimeLightFlags(RE::NiLight* a_niLight) noexcept
	{
		if (!a_niLight)
			return 0;

		auto& runtimeData = a_niLight->GetLightRuntimeData();
		const std::uint32_t flags = std::bit_cast<std::uint32_t>(runtimeData.ambient.red);
		return (flags & ToMask(Flags::Initialised)) != 0 ? flags : 0;
	}

	inline std::uint32_t GetPointLightTypeFlags(RE::BSLight* a_bsLight)
	{
		if (!a_bsLight || !a_bsLight->pointLight)
			return 0;

		auto* shadowLight = skyrim_cast<RE::BSShadowLight*>(a_bsLight);
		return shadowLight && shadowLight->GetIsFrustumLight() ? ToMask(Flags::Spot) : ToMask(Flags::OmniDirectional);
	}

	inline std::uint32_t GetVanillaPointLightFlags(RE::BSLight* a_bsLight, RE::NiLight* a_niLight)
	{
		constexpr std::uint32_t typeMask = ToMask(Flags::Spot) | ToMask(Flags::OmniDirectional);
		std::uint32_t flags = GetRuntimeLightFlags(a_niLight) & (ToMask(Flags::Linear) | typeMask);
		if ((flags & typeMask) == 0)
			flags |= GetPointLightTypeFlags(a_bsLight);
		return flags;
	}

	inline void SetPointLightTypeFlags(stl::enumeration<Flags>& a_flags, RE::BSLight* a_bsLight)
	{
		if (!a_bsLight || !a_bsLight->pointLight)
			return;
		if (a_flags.any(Flags::Spot, Flags::OmniDirectional))
			return;

		if ((GetPointLightTypeFlags(a_bsLight) & ToMask(Flags::Spot)) != 0) {
			a_flags.set(Flags::Spot);
			a_flags.reset(Flags::OmniDirectional);
		} else {
			a_flags.reset(Flags::Spot);
			a_flags.set(Flags::OmniDirectional);
		}
	}
}
