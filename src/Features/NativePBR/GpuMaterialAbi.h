#pragma once

#include "NativePBRCompiler.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace NativePBR
{
	inline constexpr uint32_t kGpuMissingTexture = UINT32_MAX;
	inline constexpr uint32_t kGpuMaterialRecordVersion = 1;
	inline constexpr uint32_t kGpuTexturePoolCapacity = 256;
	inline constexpr uint32_t kGpuTextureDescriptorCapacity = 2 * kGpuTexturePoolCapacity;
	inline constexpr uint32_t kGpuRequiredTextureRoles = 0x07u;
	enum class GpuMaterialAvailability : uint8_t
	{
		Resident,
		PartiallyResident,
		Unavailable
	};
	/** @brief Resource coverage is independent of recipe feature-support diagnostics. */
	inline GpuMaterialAvailability ClassifyGpuMaterialAvailability(uint32_t a_resident, uint32_t a_requested)
	{
		return (a_resident & (a_requested | kGpuRequiredTextureRoles)) == (a_requested | kGpuRequiredTextureRoles) ?
			GpuMaterialAvailability::Resident : a_resident ?
			GpuMaterialAvailability::PartiallyResident : GpuMaterialAvailability::Unavailable;
	}
	inline constexpr uint32_t kGpuDrawUsesVertexColor = 1u;
	inline constexpr uint32_t kGpuDrawProjected = 2u;
	/** @brief A companion-prepared opaque instance may only use an authoritative NativePBR material. */
	inline constexpr uint32_t kGpuDrawPreparedOpaque = 4u;
	inline constexpr uint32_t kGpuDrawProjectedSnowCandidate = 1u << 3;
	inline constexpr uint32_t kGpuSnowCoverageShift = 16u;
	inline constexpr uint32_t kGpuSnowCoverageMask = 0xffu << kGpuSnowCoverageShift;
	/** @brief Adds frame-local native snow coverage without changing the 24-byte draw ABI. */
	inline uint32_t WithNativeSnowCoverage(uint32_t a_flags, float a_coverage)
	{
		const auto coverage = std::isfinite(a_coverage) ? std::clamp(a_coverage, 0.0f, 1.0f) : 0.0f;
		return (a_flags & ~kGpuSnowCoverageMask) | (uint32_t(coverage * 255.0f + 0.5f) << kGpuSnowCoverageShift);
	}
	/** @brief Strong native terrain evidence wins; nearby terrain is accepted with reduced strength. */
	inline float NativeSnowCoverage(float a_local, bool a_weatherSnowing)
	{
		return std::max(std::isfinite(a_local) ? std::clamp(a_local, 0.0f, 1.0f) : 0.0f,
			a_weatherSnowing ? 0.55f : 0.0f);
	}
	/** @brief Controls.y bit 8 switches the packed map from PBR RMAOS to legacy environment/complex channels. */
	inline constexpr uint32_t kGpuLegacyComplexMask = 1u << 8;
	inline constexpr uint32_t kRayMaterialOptionParallax = 1u << 0;
	inline constexpr uint32_t kRayMaterialOptionComplex = 1u << 1;
	inline constexpr uint32_t RayRequiredTextureRoles(uint32_t a_requested, bool a_parallaxEnabled)
	{
		return a_parallaxEnabled ? a_requested : a_requested & ~(1u << static_cast<uint32_t>(TextureRole::Displacement));
	}
	using GpuTextureLocation = std::array<uint32_t, 2>;
	using GpuTextureLocations = std::array<GpuTextureLocation, static_cast<size_t>(TextureRole::Count)>;
	/** @brief Converts one physical DDS page to the matching linear or sRGB SRV index. */
	inline uint32_t GpuTextureDescriptorIndex(uint32_t a_pool, TextureInterpretation a_interpretation)
	{
		return a_pool < kGpuTexturePoolCapacity ?
			a_pool + (a_interpretation == TextureInterpretation::ColorSrgb ? kGpuTexturePoolCapacity : 0) :
			kGpuMissingTexture;
	}
	/** @brief Requires only the texture roles sampled by the current opaque SM6.5 evaluator. */
	inline uint32_t GpuRequestedTextureRoles(const Recipe& a_recipe)
	{
		uint32_t result = kGpuRequiredTextureRoles;
		for (size_t role = 0; role <= static_cast<size_t>(TextureRole::Emission); ++role)
			if (!a_recipe.textures[role].canonicalPath.empty())
				result |= 1u << role;
		if (SupportsOpaqueClearcoat(a_recipe) &&
			!a_recipe.textures[static_cast<size_t>(TextureRole::Features0)].canonicalPath.empty())
			result |= 1u << static_cast<size_t>(TextureRole::Features0);
		if (SupportsOpaqueParallax(a_recipe) &&
			!a_recipe.textures[static_cast<size_t>(TextureRole::Displacement)].canonicalPath.empty())
			result |= 1u << static_cast<size_t>(TextureRole::Displacement);
		return result;
	}

	/** @brief Geometry-independent SM6 material record, indexed by stable material identity. */
	struct GpuMaterialRecord
	{
		std::array<std::array<uint32_t, 4>, 4> textures{};
		std::array<std::array<float, 4>, 2> uv{};
		std::array<float, 4> base{};
		std::array<float, 4> coat{};
		std::array<float, 4> surface{};
		std::array<float, 4> projected{};
		std::array<float, 4> coatColor{};
		std::array<float, 4> subsurfaceColor{};
		std::array<float, 4> fuzzColor{};
		std::array<float, 4> thinSurface{};
		std::array<float, 4> glint{};
		std::array<float, 4> projectedGlint{};
		std::array<uint32_t, 4> properties{};
		std::array<uint32_t, 4> identity{};
		std::array<uint32_t, 4> controls{};
	};
	static_assert(sizeof(GpuMaterialRecord) == 304);
	static_assert(offsetof(GpuMaterialRecord, uv) == 64);
	static_assert(offsetof(GpuMaterialRecord, base) == 96);
	static_assert(offsetof(GpuMaterialRecord, coat) == 112);
	static_assert(offsetof(GpuMaterialRecord, surface) == 128);
	static_assert(offsetof(GpuMaterialRecord, projected) == 144);
	static_assert(offsetof(GpuMaterialRecord, coatColor) == 160);
	static_assert(offsetof(GpuMaterialRecord, subsurfaceColor) == 176);
	static_assert(offsetof(GpuMaterialRecord, fuzzColor) == 192);
	static_assert(offsetof(GpuMaterialRecord, thinSurface) == 208);
	static_assert(offsetof(GpuMaterialRecord, glint) == 224);
	static_assert(offsetof(GpuMaterialRecord, projectedGlint) == 240);
	static_assert(offsetof(GpuMaterialRecord, properties) == 256);
	static_assert(offsetof(GpuMaterialRecord, identity) == 272);
	static_assert(offsetof(GpuMaterialRecord, controls) == 288);

	struct GpuDrawBinding
	{
		std::array<uint32_t, 2> identity{};
		std::array<uint32_t, 2> revision{};
		uint32_t materialIndex = UINT32_MAX;
		uint32_t flags = 0;
	};
	static_assert(sizeof(GpuDrawBinding) == 24);
	static_assert(offsetof(GpuDrawBinding, identity) == 0);
	static_assert(offsetof(GpuDrawBinding, revision) == 8);
	static_assert(offsetof(GpuDrawBinding, materialIndex) == 16);
	static_assert(offsetof(GpuDrawBinding, flags) == 20);

	inline GpuDrawBinding PackGpuDrawBinding(uint64_t a_identity, uint64_t a_revision,
		uint32_t a_materialIndex, uint32_t a_flags)
	{
		return { { uint32_t(a_identity), uint32_t(a_identity >> 32) },
			{ uint32_t(a_revision), uint32_t(a_revision >> 32) }, a_materialIndex, a_flags };
	}

	/** @brief Packs all observed material features without treating unsupported lobes as residency failures. */
	inline GpuMaterialRecord PackGpuMaterialRecord(const Recipe& a_recipe, const GpuTextureLocations& a_locations)
	{
		GpuMaterialRecord result;
		uint32_t residentRoles = 0;
		for (size_t role = 0; role < a_locations.size(); ++role) {
			const auto& location = a_locations[role];
			auto& pair = result.textures[role / 2];
			pair[(role % 2) * 2] = location[0];
			pair[(role % 2) * 2 + 1] = location[1];
			if (location[0] != kGpuMissingTexture)
				residentRoles |= 1u << role;
		}
		result.textures[3][2] = kGpuMissingTexture;
		const auto& source = a_recipe.parameters;
		result.uv = source.uvOffsetScale;
		result.base = { source.materialAlpha, source.roughnessScale, source.displacementScale, source.specularLevel };
		result.coat = { source.coatStrength, source.coatRoughness, source.coatSpecularLevel, source.subsurfaceOpacity };
		result.surface = { source.fuzzWeight, source.subsurfaceBalance, source.fuzzRoughness, source.projectedRoughness };
		result.projected = { source.projectedColor[0], source.projectedColor[1], source.projectedColor[2], source.projectedSpecularLevel };
		result.coatColor = { source.coatColor[0], source.coatColor[1], source.coatColor[2], 0 };
		result.subsurfaceColor = { source.subsurfaceColor[0], source.subsurfaceColor[1], source.subsurfaceColor[2], 0 };
		result.fuzzColor = { source.fuzzColor[0], source.fuzzColor[1], source.fuzzColor[2], 0 };
		result.thinSurface = { source.thinSurfaceParameters[0], source.thinSurfaceParameters[1], source.thinSurfaceParameters[2], 0 };
		result.glint = { source.glint.screenScale, source.glint.microfacetDensity, source.glint.roughness, source.glint.randomness };
		result.projectedGlint = { source.projectedGlint.screenScale, source.projectedGlint.microfacetDensity, source.projectedGlint.roughness, source.projectedGlint.randomness };
		result.properties = { source.pbrFlags, source.nifThinSurfaceFlags, source.clampMode, a_recipe.issues };
		result.identity = { uint32_t(a_recipe.identity), uint32_t(a_recipe.identity >> 32),
			uint32_t(a_recipe.revision), uint32_t(a_recipe.revision >> 32) };
		result.controls = { residentRoles,
			(source.glint.enabled ? 1u : 0u) | (source.projectedGlint.enabled ? 2u : 0u) |
			(source.fuzzRoughnessTexture ? 4u : 0u) |
			(source.legacyComplexMask && ActiveOpaquePbrFlags(a_recipe) == 0u ? kGpuLegacyComplexMask : 0u),
			uint32_t(source.kind), ActiveOpaquePbrFlags(a_recipe) };
		return result;
	}
}
