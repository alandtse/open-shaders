#pragma once

#include "GpuMaterialResources.h"
#include "PreparedSceneMaterials.h"

#include <cmath>

namespace NativePBR
{
	/** @brief Maps a completed TLAS generation to immutable, resident opaque NativePBR recipes. */
	struct RayMaterialBindings
	{
		std::shared_ptr<const PreparedSceneMaterials> prepared;
		std::shared_ptr<const GpuMaterialResources> materials;
		std::vector<GpuDrawBinding> instances;
		uint32_t eligible = 0, unsupported = 0, unavailable = 0;
		bool forcedAdmission = false;
		bool parallaxRequired = true;
	};

	/** @brief Selects only opaque meshes whose complete native shading inputs are known. */
	inline bool RayOpaqueEligible(const DX12Native::Instance& a_instance, const MaterialLookup& a_material,
		bool a_forceAdmission = false)
	{
		if (a_forceAdmission)
			return a_material && a_material.recipe && a_material.recipe->parameters.kind == MaterialKind::Mesh &&
				a_instance.material.opaqueCandidate && a_instance.material.attributes &&
				(a_instance.material.attributes->stride == 36 || a_instance.material.attributes->stride == 60);
		if (!a_material || a_material.instanceIssues || a_material.instanceOverride ||
			!a_material.recipe || a_material.recipe->parameters.kind != MaterialKind::Mesh ||
			!a_instance.material.opaqueCandidate || a_instance.material.alphaTest ||
			!a_instance.material.attributes || !a_instance.material.attributes->hasTangentFrame ||
			a_instance.material.attributes->stride != 60 ||
			!std::isfinite(a_material.recipe->parameters.materialAlpha) ||
			a_material.recipe->parameters.materialAlpha != 1.0f ||
			a_material.instanceProperties.opacity == InstanceProperties::Opacity::NonOpaque ||
			a_material.instanceProperties.normalMode == InstanceProperties::NormalMode::ModelSpace ||
			a_material.instanceProperties.uvAnimation == InstanceProperties::UvAnimation::Animated)
			return false;
		constexpr auto excluded = uint32_t(DX12Native::OfflineMaterialFlags::AlphaCoverage) |
			uint32_t(DX12Native::OfflineMaterialFlags::AnimatedMaterial) |
			uint32_t(DX12Native::OfflineMaterialFlags::Unknown);
		if ((uint32_t(a_instance.material.offlineMaterialFlags) & excluded) != 0 ||
			!std::isfinite(a_instance.material.alpha) || a_instance.material.alpha != 1.0f)
			return false;
		if (a_instance.material.offlineDescription) {
			const auto& description = *a_instance.material.offlineDescription;
			const auto flags = description.find("shaderFlags1");
			if (flags != description.end() && flags->is_number_integer() &&
				(flags->get<uint32_t>() & (1u << 12)) != 0)
				return false;
		}
		auto readiness = a_material.Readiness();
		if (a_material.instanceProperties.opacity == InstanceProperties::Opacity::Unknown)
			readiness.unresolvedInstance &= ~UnknownOpacity;
		if (a_material.instanceProperties.normalMode == InstanceProperties::NormalMode::Unknown)
			readiness.unresolvedInstance &= ~UnknownNormalMode;
		if (a_material.instanceProperties.uvAnimation == InstanceProperties::UvAnimation::Unknown)
			readiness.unresolvedInstance &= ~UnknownUvAnimation;
		return readiness.sourceIssues == 0 && readiness.unsupportedFeatures == 0 &&
			readiness.unresolvedInstance == 0;
	}

	/** @brief Emits compact per-instance lookups while retaining scene and texture generations. */
	inline std::shared_ptr<const RayMaterialBindings> PrepareRayMaterialBindings(
		std::shared_ptr<const PreparedSceneMaterials> a_prepared,
		std::shared_ptr<const GpuMaterialResources> a_materials, bool a_forceAdmission = false,
		bool a_parallaxRequired = true)
	{
		if (!a_prepared || !a_prepared->geometry || !a_prepared->materialPublication ||
			!a_materials || !a_materials->generation ||
			a_prepared->materialPublication->recipes != a_materials->generation ||
			a_prepared->instances.size() != a_prepared->geometry->instances.size())
			return {};
		auto result = std::make_shared<RayMaterialBindings>();
		result->prepared = std::move(a_prepared);
		result->materials = std::move(a_materials);
		result->forcedAdmission = a_forceAdmission;
		result->parallaxRequired = a_parallaxRequired;
		result->instances.resize(result->prepared->instances.size(), PackGpuDrawBinding(0, 0, UINT32_MAX, 0));
		for (size_t index = 0; index < result->instances.size(); ++index) {
			const auto& instance = result->prepared->geometry->instances[index];
			const auto& identity = instance.preparedIdentity;
			if (instance.material.opaqueCandidate && !identity.sourceDigest.empty() &&
				!identity.referenceKey.empty() && !identity.shapeName.empty())
				result->instances[index].flags |= kGpuDrawPreparedOpaque;
			if (instance.material.projectedSnowCandidate)
				result->instances[index].flags |= kGpuDrawProjectedSnowCandidate;
		}
		for (const auto& binding : result->prepared->instances) {
			if (binding.instanceIndex >= result->instances.size() ||
				binding.geometryIdentity != result->prepared->geometry->instances[binding.instanceIndex].identity ||
				!RayOpaqueEligible(result->prepared->geometry->instances[binding.instanceIndex], binding.material, a_forceAdmission)) {
				++result->unsupported;
				continue;
			}
			const auto& recipe = *binding.material.recipe;
			const auto gpu = result->materials->Find(recipe.identity, recipe.revision);
			if (!gpu || (!a_forceAdmission &&
				(a_parallaxRequired ? gpu->availability : ClassifyGpuMaterialAvailability(gpu->residentRoles,
					RayRequiredTextureRoles(gpu->requestedRoles, false))) != GpuMaterialAvailability::Resident) ||
				gpu->index >= result->materials->entries.size()) {
				++result->unavailable;
				continue;
			}
			// Match the legacy scene compiler's shaderFlags2 vertex-color interpretation.
			// Other material flags remain unsupported until their per-hit inputs are established.
			uint32_t flags = result->instances[binding.instanceIndex].flags;
			const auto& description = result->prepared->geometry->instances[binding.instanceIndex].material.offlineDescription;
			if (description) {
				const auto shaderFlags = description->find("shaderFlags2");
				if (shaderFlags != description->end() && shaderFlags->is_number_integer() &&
					(shaderFlags->get<uint32_t>() & (1u << 5)) != 0)
					flags |= kGpuDrawUsesVertexColor;
			}
			result->instances[binding.instanceIndex] = PackGpuDrawBinding(recipe.identity, recipe.revision, gpu->index, flags);
			++result->eligible;
		}
		return result;
	}
}
