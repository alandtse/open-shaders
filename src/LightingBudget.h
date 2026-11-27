#pragma once

#include "ShaderCache.h"

/**
 * Ownership of the Lighting shader's scarce shared resources: descriptor bits and constant buffer slots.
 * Claim a bit or slot here first; the static_asserts fail the build on overlap.
 * Design: docs/development/lighting-extension-points.md.
 */
namespace LightingBudget
{
	using Flags = SIE::ShaderCache::LightingShaderFlags;

	constexpr uint32_t Bits(uint32_t a_first, uint32_t a_last)
	{
		return static_cast<uint32_t>((0xFFFFFFFFull >> (31 - a_last)) & (0xFFFFFFFFull << a_first));
	}

	constexpr uint32_t kEngineMask = Bits(0, 2) | Bits(9, 23);
	constexpr uint32_t kUpstreamMask = Bits(3, 5);
	/** Technique index (`0x3F & (descriptor >> 24)`). Bit 29 is technique bit 5 and is decoded unmasked, so it is never a feature flag. */
	constexpr uint32_t kTechniqueMask = Bits(24, 29);
	constexpr uint32_t kForkMask = Bits(30, 31);
	/** Detail Maps' compiled variant; the one bit between the upstream flags and the still-free range. */
	constexpr uint32_t kDetailMapsMask = static_cast<uint32_t>(Flags::DetailMaps);
	/** Bits still available to a feature that needs a distinct compiled variant. */
	constexpr uint32_t kFreeMask = Bits(7, 8);

	static_assert((kEngineMask & kUpstreamMask) == 0);
	static_assert((kEngineMask & kTechniqueMask) == 0);
	static_assert((kEngineMask & kForkMask) == 0);
	static_assert((kEngineMask & kDetailMapsMask) == 0);
	static_assert((kEngineMask & kFreeMask) == 0);
	static_assert((kUpstreamMask & kTechniqueMask) == 0);
	static_assert((kUpstreamMask & kForkMask) == 0);
	static_assert((kUpstreamMask & kDetailMapsMask) == 0);
	static_assert((kUpstreamMask & kFreeMask) == 0);
	static_assert((kTechniqueMask & kForkMask) == 0);
	static_assert((kTechniqueMask & kDetailMapsMask) == 0);
	static_assert((kTechniqueMask & kFreeMask) == 0);
	static_assert((kForkMask & kDetailMapsMask) == 0);
	static_assert((kForkMask & kFreeMask) == 0);
	static_assert((kDetailMapsMask & kFreeMask) == 0);
	static_assert((kEngineMask | kUpstreamMask | kTechniqueMask | kForkMask | kDetailMapsMask | kFreeMask) == 0xFFFFFFFFu);

	constexpr uint32_t Mask(Flags a_flag)
	{
		return static_cast<uint32_t>(a_flag);
	}

	static_assert(Mask(Flags::DetailMaps) == kDetailMapsMask);
	static_assert((Mask(Flags::TruePbr) | Mask(Flags::Deferred) | Mask(Flags::LandscapeSeams)) == kUpstreamMask);
	static_assert((Mask(Flags::FurShells) | Mask(Flags::FurShellsDepth)) == kForkMask);
	static_assert((Mask(Flags::VC) | Mask(Flags::Skinned) | Mask(Flags::ModelSpaceNormals)) == Bits(0, 2));
	static_assert((Mask(Flags::Specular) | Mask(Flags::AdditionalAlphaMask)) <= Bits(9, 23));
	static_assert(static_cast<uint32_t>(SIE::ShaderCache::LightingShaderTechniques::MTLandLODBlend) < 32u, "technique bit 5 must stay clear");

	/** Pixel-shader constant buffer slots of the Lighting shader and the feature that owns each. */
	namespace PS
	{
		constexpr uint32_t kFree = 9;
		constexpr uint32_t kFreeVROnly = 11;
	}

	/** Vertex-shader constant buffer slots: b9 and b10 are the bone buffers (`Common/Skinned.hlsli`), so no slot is free in both stages. */
	namespace VS
	{
		constexpr uint32_t kFree = 11;
	}
}
