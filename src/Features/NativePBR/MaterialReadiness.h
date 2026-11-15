#pragma once

#include "NativePBRCompiler.h"

namespace NativePBR
{
	inline constexpr uint32_t kUnresolvedInstanceIssues = UnknownOpacity | UnknownNormalMode |
		UnknownUvAnimation | UnmatchedSceneInstance;
	inline constexpr uint32_t kUnusableRecipeSourceIssues = MissingRequiredTextures | InvalidSnapshot | InvalidTexturePath;
	inline constexpr uint32_t kUnsupportedRecipeFeatures = UnsupportedLandscape | UnsupportedPbrFeatures |
		UnsupportedGlint | UnsupportedDisplacement | UnsupportedFeatureTextures | UnsupportedProjection;

	/** @brief Distinguishes material-source failures, unsupported shading and unobserved instance state. */
	struct RecipeReadiness
	{
		uint32_t sourceIssues = 0, unsupportedFeatures = 0, unresolvedInstance = 0;
		bool IsOpaqueFeatureCompatible() const { return sourceIssues == 0 && unsupportedFeatures == 0; }
	};

	/** @brief Classifies effective recipe capabilities without granting render eligibility. */
	inline RecipeReadiness ClassifyRecipeReadiness(uint32_t a_effectiveIssues)
	{
		return { a_effectiveIssues & kUnusableRecipeSourceIssues,
			a_effectiveIssues & kUnsupportedRecipeFeatures, a_effectiveIssues & kUnresolvedInstanceIssues };
	}

	inline RecipeReadiness ClassifyRecipeReadiness(const Recipe& a_recipe)
	{
		return ClassifyRecipeReadiness(EffectiveNativeIssues(a_recipe));
	}

	/** @brief Uses original vertex-shader UVs from a verified same-frame opaque sidecar. */
	inline RecipeReadiness ResolveCapturedOpaqueUv(RecipeReadiness a_readiness, bool a_currentFrameOpaqueCapture)
	{
		if (a_currentFrameOpaqueCapture)
			a_readiness.unresolvedInstance &= ~UnknownUvAnimation;
		return a_readiness;
	}

	/** @brief Excludes projected-only parameters and feature textures unused by the opaque shader permutation. */
	inline RecipeReadiness ResolveCapturedOpaqueFeatures(RecipeReadiness a_readiness, const Recipe& a_recipe,
		bool a_currentFrameOpaqueCapture)
	{
		if (!a_currentFrameOpaqueCapture)
			return a_readiness;
		const auto flags = a_recipe.parameters.pbrFlags;
		constexpr uint32_t kSubsurface = uint32_t(PBRFlags::Subsurface);
		constexpr uint32_t kTwoLayer = uint32_t(PBRFlags::TwoLayer);
		constexpr uint32_t kFuzz = uint32_t(PBRFlags::Fuzz);
		constexpr uint32_t kHair = uint32_t(PBRFlags::HairMarschner);
		a_readiness.unsupportedFeatures &= ~UnsupportedProjection;
		if (!a_recipe.parameters.glint.enabled || (flags & (kTwoLayer | kHair | kFuzz)) != 0)
			a_readiness.unsupportedFeatures &= ~UnsupportedGlint;
		const bool usesFeatures0 = (flags & kSubsurface) != 0 && (flags & (kTwoLayer | kHair)) == 0 &&
			!a_recipe.textures[static_cast<size_t>(TextureRole::Features0)].canonicalPath.empty();
		const bool usesFeatures1 = (flags & (kTwoLayer | kFuzz)) != 0 &&
			!a_recipe.textures[static_cast<size_t>(TextureRole::Features1)].canonicalPath.empty();
		if (!usesFeatures0 && !usesFeatures1)
			a_readiness.unsupportedFeatures &= ~UnsupportedFeatureTextures;
		return a_readiness;
	}

	/** @brief Counts overlapping causes without confusing source or shading readiness with texture residency. */
	struct MaterialReadinessCounts
	{
		size_t recipes = 0, sourceLimited = 0, featureLimited = 0, unresolvedInstances = 0, opaqueFeatureCompatible = 0;

		void Observe(const Recipe& a_recipe)
		{
			const auto readiness = ClassifyRecipeReadiness(a_recipe);
			++recipes;
			sourceLimited += readiness.sourceIssues != 0;
			featureLimited += readiness.unsupportedFeatures != 0;
			unresolvedInstances += readiness.unresolvedInstance != 0;
			opaqueFeatureCompatible += readiness.IsOpaqueFeatureCompatible();
		}
	};

	/** @brief Captures only engine-observable opacity and normal mode, retaining unknown UV motion. */
	inline void ResolveObservedInstanceProperties(InstanceProperties& a_properties, std::optional<float> a_materialAlpha)
	{
		constexpr uint64_t kVertexAlpha = 1ull << 3;
		constexpr uint64_t kModelSpaceNormal = 1ull << 12;
		a_properties.normalMode = (a_properties.shaderFlags & kModelSpaceNormal) != 0 ?
			InstanceProperties::NormalMode::ModelSpace : InstanceProperties::NormalMode::TangentSpace;
		if (a_properties.alphaProperty || a_properties.alphaFlags != 0 || (a_properties.shaderFlags & kVertexAlpha) != 0 ||
			(a_materialAlpha && std::isfinite(*a_materialAlpha) && *a_materialAlpha < 1.0f))
			a_properties.opacity = InstanceProperties::Opacity::NonOpaque;
		else if (a_properties.opacity == InstanceProperties::Opacity::Unknown &&
			a_materialAlpha && std::isfinite(*a_materialAlpha) && *a_materialAlpha == 1.0f)
			a_properties.opacity = InstanceProperties::Opacity::Opaque;
	}
}
