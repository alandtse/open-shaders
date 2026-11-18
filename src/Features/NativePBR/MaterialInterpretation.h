#pragma once

#include "Materials/PBRRecordData.h"
#include "Materials/PBRFlags.h"
#include "NativePBRRegistry.h"

#include <algorithm>
#include <optional>
#include <span>

namespace NativePBR
{
	/** @brief Converts the shared TruePBR-compatible JSON glint schema into owned values. */
	inline Glint InterpretGlint(const PBRRecords::GlintParameters& a_source)
	{
		return { a_source.enabled, a_source.screenSpaceScale, a_source.logMicrofacetDensity,
			a_source.microfacetRoughness, a_source.densityRandomization };
	}

	/** @brief Decodes NIF PBRThinSurface extra-data without mutating a Skyrim material. */
	inline bool InterpretNifThinSurface(std::span<const float> a_values, MaterialData& a_material)
	{
		if (a_values.size() != 4 || std::ranges::any_of(a_values, [](float value) { return !std::isfinite(value); }))
			return false;
		a_material.subsurfaceBalance = std::clamp(a_values[0], 0.0f, 1.0f);
		a_material.thinSurfaceParameters = { std::clamp(a_values[1], 0.0f, 1.0f),
			std::clamp(a_values[2], 0.0f, 1.0f), std::clamp(a_values[3], 0.0f, 3.0f) };
		return true;
	}

	/** @brief Decodes NIF PBRFuzz extra-data without mutating a Skyrim material. */
	inline bool InterpretNifFuzz(std::span<const float> a_values, MaterialData& a_material)
	{
		if (a_values.size() != 2 || std::ranges::any_of(a_values, [](float value) { return !std::isfinite(value); }) ||
			(a_values[1] != 0.0f && a_values[1] != 1.0f))
			return false;
		a_material.fuzzRoughness = a_values[0] < 0.0f ? -1.0f : std::min(a_values[0], 1.0f);
		a_material.fuzzRoughnessTexture = a_values[1] == 1.0f;
		return true;
	}

	/** @brief Applies compatible texture-set JSON fields to a value copy, retaining NIF precedence. */
	inline MaterialData InterpretTextureSet(const MaterialData& a_base, const PBRRecords::TextureSetData& a_config,
		std::optional<std::array<float, 4>> a_nifThinSurface = {}, std::optional<std::array<float, 2>> a_nifFuzz = {})
	{
		MaterialData result = a_base;
		result.roughnessScale = a_config.roughnessScale;
		result.displacementScale = a_config.displacementScale;
		result.specularLevel = a_config.specularLevel;
		result.subsurfaceBalance = std::clamp(a_config.subsurfaceBalance, 0.0f, 1.0f);
		constexpr auto kFlags = uint32_t(PBRFlags::ThinSurface) | uint32_t(PBRFlags::Foliage);
		result.pbrFlags = (result.pbrFlags & ~kFlags) | (result.nifThinSurfaceFlags & kFlags);
		if (a_config.thinSurface)
			result.pbrFlags |= uint32_t(PBRFlags::ThinSurface);
		if (a_config.foliage)
			result.pbrFlags |= uint32_t(PBRFlags::Foliage);
		result.thinSurfaceParameters = { std::clamp(a_config.thinSurfaceSpecularWeight, 0.0f, 1.0f),
			std::clamp(a_config.thinSurfaceBackSpecularWeight, 0.0f, 1.0f),
			std::clamp(a_config.thinSurfaceBackRoughnessScale, 0.0f, 3.0f) };
		if ((result.pbrFlags & uint32_t(PBRFlags::TwoLayer)) != 0) {
			result.coatColor = { a_config.coatColor.red, a_config.coatColor.green, a_config.coatColor.blue };
			result.coatStrength = a_config.coatStrength;
			result.coatRoughness = a_config.coatRoughness;
			result.coatSpecularLevel = a_config.coatSpecularLevel;
		} else {
			if ((result.pbrFlags & uint32_t(PBRFlags::Subsurface)) != 0) {
				result.subsurfaceColor = { a_config.subsurfaceColor.red, a_config.subsurfaceColor.green, a_config.subsurfaceColor.blue };
				result.subsurfaceOpacity = a_config.subsurfaceOpacity;
			}
			if ((result.pbrFlags & uint32_t(PBRFlags::Fuzz)) != 0) {
				result.fuzzColor = { a_config.fuzzColor.red, a_config.fuzzColor.green, a_config.fuzzColor.blue };
				result.fuzzWeight = a_config.fuzzWeight;
				result.fuzzRoughness = a_config.fuzzRoughness;
				result.fuzzRoughnessTexture = a_config.fuzzRoughnessTexture;
			}
		}
		result.glint = InterpretGlint(a_config.glintParameters);
		if (a_nifThinSurface)
			InterpretNifThinSurface(*a_nifThinSurface, result);
		if (a_nifFuzz)
			InterpretNifFuzz(*a_nifFuzz, result);
		return result;
	}

	/** @brief Stores reference-local projected material data separate from a shared base material. */
	struct InstanceMaterialOverride
	{
		std::string editorId;
		std::array<float, 3> baseColorScale{ 1.0f, 1.0f, 1.0f };
		float roughness = 1.0f, specularLevel = 0.04f;
		Glint glint;
		bool operator==(const InstanceMaterialOverride&) const = default;
	};

	/** @brief Copies MATO JSON into immutable-ready, engine-pointer-free instance values. */
	inline InstanceMaterialOverride InterpretMaterialObject(std::string a_editorId, const PBRRecords::MaterialObjectData& a_config)
	{
		return { std::move(a_editorId), a_config.baseColorScale, a_config.roughness,
			a_config.specularLevel, InterpretGlint(a_config.glintParameters) };
	}
}
