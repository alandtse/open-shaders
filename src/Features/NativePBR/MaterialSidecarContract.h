#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <span>
#include <string>

namespace NativePBR
{
	/** Distinct from the eight deferred MRTs; none of their channels are repurposed. */
	enum class SidecarTarget : size_t
	{
		IdentityAndColor,
		UVAndDdx,
		DdyDepthFacing,
		Basis0,
		Basis1,
		Basis2,
		ProjectedWorldPosition,
		ProjectedDirection,
		Count
	};

	inline constexpr size_t kBaseSidecarTargetCount = static_cast<size_t>(SidecarTarget::ProjectedWorldPosition);
	inline constexpr uint32_t kSidecarSurfaceAbiVersion = 2;
	inline constexpr std::array<uint32_t, static_cast<size_t>(SidecarTarget::Count)> kSidecarBytesPerPixel{ 8, 16, 16, 8, 8, 8, 16, 16 };

	/** R32G32_UINT: frame-local ordinal, then RGBA8 with R in the least-significant byte. */
	struct SidecarIdentityAndColor
	{
		uint32_t ordinal = 0;
		uint32_t rgba8 = 0;
	};
	static_assert(sizeof(SidecarIdentityAndColor) == 8);
	static_assert(offsetof(SidecarIdentityAndColor, rgba8) == 4);

	inline constexpr uint32_t PackSidecarRgba8(uint8_t a_r, uint8_t a_g, uint8_t a_b, uint8_t a_a)
	{
		return uint32_t(a_r) | (uint32_t(a_g) << 8) | (uint32_t(a_b) << 16) | (uint32_t(a_a) << 24);
	}
	inline constexpr uint32_t kMaximumFrameDraws = 1024;
	inline constexpr uint32_t kInputOnlyIdentityScale = 65535;
	inline constexpr uint32_t kInputOnlyOpaqueSignature = 0xA55A;
	inline constexpr uint32_t kInputOnlyProjectedSignature = 0x5AA5;

	/** R16G16_UNORM Masks2 holds the ordinal and format signature for an owned opaque draw. */
	inline uint32_t DecodeInputOnlyOrdinal(float a_normalizedOrdinal, float a_normalizedSignature, bool a_projected)
	{
		if (!std::isfinite(a_normalizedOrdinal) || !std::isfinite(a_normalizedSignature) ||
			a_normalizedOrdinal < 0 || a_normalizedOrdinal > 1 || a_normalizedSignature < 0 || a_normalizedSignature > 1)
			return 0;
		const auto signature = static_cast<uint32_t>(std::lround(a_normalizedSignature * kInputOnlyIdentityScale));
		const auto ordinal = static_cast<uint32_t>(std::lround(a_normalizedOrdinal * kInputOnlyIdentityScale));
		return signature == (a_projected ? kInputOnlyProjectedSignature : kInputOnlyOpaqueSignature) &&
			ordinal <= kMaximumFrameDraws ? ordinal : 0;
	}

	/** Stored alongside the frame's fenced GPU textures; no borrowed engine pointers. */
	struct SidecarDraw
	{
		uint32_t ordinal = 0;
		uint64_t materialIdentity = 0;
		uint64_t materialRevision = 0;
		std::string referenceKey, shapeName;
		bool projectedUv = false;
	};

	inline bool MatchesInputOnlyReplay(uint32_t a_nextOrdinal, std::span<const SidecarDraw> a_draws,
		uint64_t a_materialIdentity, uint64_t a_materialRevision)
	{
		if (a_nextOrdinal <= 1 || a_draws.empty())
			return false;
		const auto& draw = a_draws.back();
		return draw.ordinal == a_nextOrdinal - 1 && draw.materialIdentity == a_materialIdentity &&
			draw.materialRevision == a_materialRevision;
	}

	/** Zero is uncovered. Nonzero ordinals address an exact frame-local 64-bit material ID. */
	inline uint64_t ResolveSidecarMaterial(uint32_t a_ordinal, std::span<const SidecarDraw> a_draws)
	{
		if (!a_ordinal || a_ordinal > a_draws.size())
			return 0;
		const auto& draw = a_draws[a_ordinal - 1];
		return draw.ordinal == a_ordinal ? draw.materialIdentity : 0;
	}

	/** Full-precision UV gradients; geometric TBN rows use finite FP16 storage. */
	struct SidecarSurface
	{
		std::array<float, 2> uv{}, ddx{}, ddy{};
		std::array<std::array<float, 3>, 3> basis{};
		float rasterDepth = 0;
		bool frontFacing = true;
	};

	inline bool SidecarDepthMatches(float a_writtenDepth, float a_finalDepth, bool a_depth24)
	{
		if (!std::isfinite(a_writtenDepth) || !std::isfinite(a_finalDepth) ||
			a_writtenDepth < 0 || a_writtenDepth > 1 || a_finalDepth < 0 || a_finalDepth > 1)
			return false;
		// D24 UNORM can quantize the original raster depth by half a representable unit.
		// D32 FLOAT must match the original interpolated SV_Position.z exactly.
		return a_depth24 ? std::abs(a_writtenDepth - a_finalDepth) <= 0.5f / 16777215.0f :
			a_writtenDepth == a_finalDepth;
	}
}
