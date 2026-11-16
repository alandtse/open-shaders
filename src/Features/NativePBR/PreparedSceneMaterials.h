#pragma once

#include "Features/DX12NativeScene/ShadowBindings.h"
#include "MaterialService.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace NativePBR
{
	inline constexpr uint32_t kPreparedSceneMaterialsVersion = 1;

	/** @brief Associates one immutable scene instance with its observed material publication. */
	struct PreparedSceneMaterialBinding
	{
		uint32_t instanceIndex = 0;
		uint64_t geometryIdentity = 0;
		MaterialLookup material;
	};

	/** @brief Retains native scene geometry, resident DDS pages and per-instance material identities without shading. */
	struct PreparedSceneMaterials
	{
		uint32_t version = kPreparedSceneMaterialsVersion;
		uint32_t cellID = 0;
		uint64_t sceneRevision = 0, materialSerial = 0, instanceEpoch = 0;
		std::shared_ptr<const DX12Native::TopLevel> geometry;
		std::shared_ptr<const DX12Native::ShadowBindings> sceneResources;
		std::shared_ptr<const MaterialPublication> materialPublication;
		std::vector<PreparedSceneMaterialBinding> instances;
		uint32_t available = 0, missing = 0, ambiguous = 0;
	};

	/** @brief Resolves scene instances to native recipes without rewriting geometry or evaluating material values. */
	inline std::vector<PreparedSceneMaterialBinding> BindPreparedSceneMaterials(
		const std::vector<DX12Native::Instance>& a_instances, const MaterialPublication& a_publication)
	{
		std::unordered_map<std::string, uint32_t> occurrences;
		for (const auto& instance : a_instances) {
			const auto& identity = instance.preparedIdentity;
			if (!identity.referenceKey.empty() && !identity.shapeName.empty())
				++occurrences[MaterialPublication::Key(identity.referenceKey, identity.shapeName)];
		}
		std::vector<PreparedSceneMaterialBinding> result;
		result.reserve(a_instances.size());
		for (uint32_t index = 0; index < a_instances.size(); ++index) {
			const auto& instance = a_instances[index];
			const auto& identity = instance.preparedIdentity;
			MaterialLookup material;
			if (identity.referenceKey.empty() || identity.shapeName.empty())
				material.status = MaterialLookupStatus::MissingInstance;
			else if (occurrences[MaterialPublication::Key(identity.referenceKey, identity.shapeName)] != 1)
				material.status = MaterialLookupStatus::AmbiguousInstance;
			else
				material = a_publication.FindInstance(identity.referenceKey, identity.shapeName);
			result.push_back({ index, instance.identity, std::move(material) });
		}
		return result;
	}

	/** @brief Publishes one scene/material generation only when all scene-owned resources agree. */
	inline std::shared_ptr<const PreparedSceneMaterials> PrepareSceneMaterials(
		std::shared_ptr<const DX12Native::TopLevel> a_geometry,
		std::shared_ptr<const DX12Native::ShadowBindings> a_resources,
		std::shared_ptr<const MaterialPublication> a_materials,
		uint32_t a_cellID, uint64_t a_sceneRevision)
	{
		if (!a_geometry || !a_resources || a_resources->scene != a_geometry ||
			!a_resources->materials || !a_materials || !a_materials->recipes)
			return {};
		auto result = std::make_shared<PreparedSceneMaterials>();
		result->cellID = a_cellID;
		result->sceneRevision = a_sceneRevision;
		result->materialSerial = a_materials->serial;
		result->instanceEpoch = a_materials->instanceEpoch;
		result->geometry = std::move(a_geometry);
		result->sceneResources = std::move(a_resources);
		result->materialPublication = std::move(a_materials);
		result->instances = BindPreparedSceneMaterials(result->geometry->instances, *result->materialPublication);
		for (const auto& binding : result->instances) {
			if (binding.material)
				++result->available;
			else if (binding.material.status == MaterialLookupStatus::AmbiguousInstance)
				++result->ambiguous;
			else
				++result->missing;
		}
		return result;
	}
}
