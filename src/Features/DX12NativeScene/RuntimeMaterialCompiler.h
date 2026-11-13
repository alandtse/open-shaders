#pragma once

#include "MaterialResources.h"
#include "MaterialTextureLookup.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace DX12Native
{
	enum class RuntimeMaterialStatus : uint8_t
	{
		Ready,
		UnsupportedRecipe,
		UnsafeOpacity,
		MissingBasis,
		MissingTextures
	};

	struct RuntimeMaterialCompile
	{
		RuntimeMaterialStatus status = RuntimeMaterialStatus::UnsupportedRecipe;
		MaterialRecord record{};
		bool emissionApplied = false;
	};

	/** @brief Looks up the live recipe's resident texture rather than an atlas material slot. */
	inline std::array<uint32_t, 2> FindRuntimeTexture(
		const MaterialResources& a_resources, const NativePBR::Recipe& a_recipe, NativePBR::TextureRole a_role)
	{
		const auto& path = a_recipe.textures[static_cast<size_t>(a_role)].canonicalPath;
		if (path.empty())
			return { kMissingTexture, 0 };
		const auto found = a_resources.locations.find(MaterialTextureLookupKey(path));
		if (found == a_resources.locations.end())
			return { kMissingTexture, 0 };
		const auto location = a_resources.logicalLocations.find(found->second);
		return location == a_resources.logicalLocations.end() ?
		           std::array<uint32_t, 2>{ kMissingTexture, 0 } :
		           location->second;
	}

	/** @brief Compiles native PBR shading directly from captured True PBR metadata.
     *
     * The prepared scene contributes only triangle attributes, never offline material
     * paths, roughness, emission, alpha or sampler state. Explicitly unsupported
     * features, unsafe opacity and absent resident resources fail closed so the
     * caller can retain its legacy/offline material fallback.
     */
	inline RuntimeMaterialCompile CompileRuntimeMaterial(
		const MaterialResources& a_resources, const Material& a_geometry, const NativePBR::VerifiedLiveMatch& a_live)
	{
		RuntimeMaterialCompile output;
		if (!a_live.recipe || !NativePBR::NativeMeshRecipeSupported(*a_live.recipe))
			return output;
		const auto& recipe = *a_live.recipe;
		const auto& params = recipe.parameters;
		const auto& hint = a_live.properties;
		constexpr uint64_t kVertexAlphaFlag = 1ull << 3;
		constexpr uint64_t kModelSpaceNormalFlag = 1ull << 12;
		constexpr uint64_t kVertexColorFlag = 1ull << 37;
		constexpr uint32_t kReady = 1, kPbr = 2, kModelNormal = 4, kVertexColor = 8,
						   kEmission = 64, kSpecular = 128, kOpacityReady = 256;
		if (!a_geometry.opaqueCandidate || a_geometry.offlineMaterialFlags != OfflineMaterialFlags::None ||
			hint.alphaProperty || hint.skinned || hint.alphaFlags != 0 ||
			(hint.shaderFlags & kVertexAlphaFlag) != 0 ||
			!std::isfinite(params.materialAlpha) || std::abs(params.materialAlpha - 1.0f) > 0.0001f) {
			output.status = RuntimeMaterialStatus::UnsafeOpacity;
			return output;
		}
		if (params.roughnessScale < 0 || params.roughnessScale > 8 ||
			params.specularLevel < 0 || params.specularLevel > 8 ||
			(hint.emissionKnown && !NativePBR::ValidLiveEmission(hint))) {
			return output;
		}
		const bool modelSpaceNormals = (hint.shaderFlags & kModelSpaceNormalFlag) != 0;
		if (!a_geometry.attributes || (!modelSpaceNormals && !a_geometry.attributes->hasTangentFrame)) {
			output.status = RuntimeMaterialStatus::MissingBasis;
			return output;
		}

		const auto color = FindRuntimeTexture(a_resources, recipe, NativePBR::TextureRole::BaseColor);
		const auto normal = FindRuntimeTexture(a_resources, recipe, NativePBR::TextureRole::Normal);
		const auto packed = FindRuntimeTexture(a_resources, recipe, NativePBR::TextureRole::Rmaos);
		const auto emission = FindRuntimeTexture(a_resources, recipe, NativePBR::TextureRole::Emission);
		if (color[0] == kMissingTexture || normal[0] == kMissingTexture || packed[0] == kMissingTexture ||
			(!recipe.textures[static_cast<size_t>(NativePBR::TextureRole::Emission)].canonicalPath.empty() &&
				emission[0] == kMissingTexture)) {
			output.status = RuntimeMaterialStatus::MissingTextures;
			return output;
		}
		auto& record = output.record;
		record.colorNormal = { color[0], color[1], normal[0], normal[1] };
		record.packedEmission = { packed[0], packed[1], emission[0], emission[1] };
		const auto& uv = params.uvOffsetScale[0];
		record.uv = { uv[2], uv[3], uv[0], uv[1] };
		record.specular = { 1.0f, 1.0f, 1.0f, params.specularLevel };
		record.scalar = { params.materialAlpha, 0.0f, params.specularLevel, params.roughnessScale };
		uint32_t flags = kReady | kPbr | kSpecular | kOpacityReady;
		if (modelSpaceNormals)
			flags |= kModelNormal;
		if ((hint.shaderFlags & kVertexColorFlag) != 0)
			flags |= kVertexColor;
		if (NativePBR::ValidLiveEmission(hint)) {
			record.emission = { hint.emissiveColor[0], hint.emissiveColor[1], hint.emissiveColor[2],
				hint.emissiveMultiplier };
			flags |= kEmission;
			output.emissionApplied = true;
		}
		record.flags = { flags, 0u, params.clampMode, 0u };
		output.status = RuntimeMaterialStatus::Ready;
		return output;
	}
}
