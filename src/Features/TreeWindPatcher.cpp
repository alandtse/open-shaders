#include "TreeWindPatcher.h"

#include "Utils/FileSystem.h"
#include "Utils/Format.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace TreeWindPatcher
{
	namespace
	{
		constexpr std::size_t kMaximumPatchFileSize = 1024 * 1024;
		constexpr std::size_t kMaximumRulesPerFile = 4096;
		constexpr std::size_t kMaximumModelPathLength = 260;
		constexpr float kMinimumSensitivity = 0.0f;
		constexpr float kMaximumSensitivity = 4.0f;
		constexpr std::string_view kPatchFileName = "NatureOfTheWildLands.json";
		constexpr std::string_view kBackupFileName = "NatureOfTheWildLands_backup.json";
		const RE::BSFixedString kRuleIdName = "OS_TreeWindRule";
		const RE::BSFixedString kBendSensitivityName = "OS_TreeBendSensitivity";
		const RE::BSFixedString kLeafAmbientSensitivityName = "OS_TreeLeafAmbientSensitivity";
		const RE::BSFixedString kTreeBoundsName = "OS_TreeWindBounds";
		constexpr std::size_t kTreeBoundsValueCount = 2;
		constexpr float kMinimumBoundExtent = 1e-3f;

		struct ModelAabb
		{
			RE::NiPoint3 minimum{
				(std::numeric_limits<float>::max)(),
				(std::numeric_limits<float>::max)(),
				(std::numeric_limits<float>::max)()
			};
			RE::NiPoint3 maximum{
				(std::numeric_limits<float>::lowest)(),
				(std::numeric_limits<float>::lowest)(),
				(std::numeric_limits<float>::lowest)()
			};
			bool valid = false;
		};

		struct LoadedRule
		{
			float bend = 1.0f;
			float leafAmbient = 1.0f;
		};

		struct RuntimeRule
		{
			std::string mesh;
			std::atomic<float> bend{ 1.0f };
			std::atomic<float> leafAmbient{ 1.0f };
			std::atomic<float> persistedBend{ 1.0f };
			std::atomic<float> persistedLeafAmbient{ 1.0f };
		};

		std::vector<std::unique_ptr<RuntimeRule>> runtimeRules;
		std::unordered_map<std::string, std::uint32_t> ruleIds;
		std::mutex saveMutex;

		float ClampSensitivity(float a_value)
		{
			return std::isfinite(a_value) ? std::clamp(a_value, kMinimumSensitivity, kMaximumSensitivity) : 1.0f;
		}

		bool ValuesDiffer(float a_lhs, float a_rhs)
		{
			return std::abs(a_lhs - a_rhs) > 0.0001f;
		}

		std::string NormalizeModelPath(std::string a_path)
		{
			a_path = Util::FixFilePath(a_path);
			while (a_path.starts_with("./"))
				a_path.erase(0, 2);
			if (const auto meshesPosition = a_path.find("meshes/"); meshesPosition != std::string::npos)
				a_path.erase(0, meshesPosition);
			else
				a_path.insert(0, "meshes/");
			return a_path;
		}

		bool IsSafeModelPath(const std::string& a_path)
		{
			return a_path.size() <= kMaximumModelPathLength && a_path.starts_with("meshes/") && a_path.ends_with(".nif") &&
			       a_path.find("../") == std::string::npos && a_path.find(':') == std::string::npos;
		}

		std::optional<float> ReadSensitivity(
			const nlohmann::json& a_entry,
			std::string_view a_key,
			const std::filesystem::path& a_filePath,
			std::size_t a_ruleIndex)
		{
			const auto valueIt = a_entry.find(a_key);
			if (valueIt == a_entry.end())
				return std::nullopt;
			if (!valueIt->is_number()) {
				logger::warn("[TreeWindPatcher] Ignoring non-numeric '{}' in {} rule {}", a_key, a_filePath.string(), a_ruleIndex);
				return std::nullopt;
			}

			const double value = valueIt->get<double>();
			if (!std::isfinite(value)) {
				logger::warn("[TreeWindPatcher] Ignoring non-finite '{}' in {} rule {}", a_key, a_filePath.string(), a_ruleIndex);
				return std::nullopt;
			}

			return std::clamp(static_cast<float>(value), kMinimumSensitivity, kMaximumSensitivity);
		}

		bool LoadPatchFile(const std::filesystem::path& a_filePath, std::unordered_map<std::string, LoadedRule>& a_rules)
		{
			std::error_code error;
			const auto fileSize = std::filesystem::file_size(a_filePath, error);
			if (error || fileSize > kMaximumPatchFileSize) {
				logger::warn("[TreeWindPatcher] Skipping unreadable or oversized patch file: {}", a_filePath.string());
				return false;
			}

			std::ifstream stream(a_filePath, std::ios::binary);
			if (!stream) {
				logger::warn("[TreeWindPatcher] Failed to open patch file: {}", a_filePath.string());
				return false;
			}

			try {
				const auto root = nlohmann::json::parse(stream);
				if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("trees") || !root["trees"].is_array()) {
					logger::warn("[TreeWindPatcher] Invalid version or trees array in {}", a_filePath.string());
					return false;
				}

				const auto& treeEntries = root["trees"];
				if (treeEntries.size() > kMaximumRulesPerFile) {
					logger::warn("[TreeWindPatcher] Too many rules in {}; maximum is {}", a_filePath.string(), kMaximumRulesPerFile);
					return false;
				}

				for (std::size_t ruleIndex = 0; ruleIndex < treeEntries.size(); ++ruleIndex) {
					const auto& entry = treeEntries[ruleIndex];
					if (!entry.is_object() || !entry.contains("mesh") || !entry["mesh"].is_string()) {
						logger::warn("[TreeWindPatcher] Ignoring malformed rule {} in {}", ruleIndex, a_filePath.string());
						continue;
					}

					const auto modelPath = NormalizeModelPath(entry["mesh"].get<std::string>());
					if (!IsSafeModelPath(modelPath)) {
						logger::warn("[TreeWindPatcher] Ignoring unsafe model path in {} rule {}", a_filePath.string(), ruleIndex);
						continue;
					}

					const auto bendSensitivity = ReadSensitivity(entry, "bendSensitivity", a_filePath, ruleIndex);
					const auto leafAmbientSensitivity = ReadSensitivity(entry, "leafAmbientSensitivity", a_filePath, ruleIndex);
					if (!bendSensitivity && !leafAmbientSensitivity) {
						logger::warn("[TreeWindPatcher] Rule {} in {} has no valid sensitivity values", ruleIndex, a_filePath.string());
						continue;
					}

					auto& rule = a_rules[modelPath];
					if (bendSensitivity)
						rule.bend = *bendSensitivity;
					if (leafAmbientSensitivity)
						rule.leafAmbient = *leafAmbientSensitivity;
				}
				return true;
			} catch (const nlohmann::json::exception& exception) {
				logger::error("[TreeWindPatcher] Failed to parse {}: {}", a_filePath.string(), exception.what());
				return false;
			}
		}

		void EnsureEditablePatch(const std::filesystem::path& a_patchPath, const std::filesystem::path& a_backupPath)
		{
			std::error_code error;
			const bool patchExists = std::filesystem::is_regular_file(a_patchPath, error);
			error.clear();
			const bool backupExists = std::filesystem::is_regular_file(a_backupPath, error);
			if (patchExists && !backupExists) {
				std::filesystem::copy_file(a_patchPath, a_backupPath, std::filesystem::copy_options::none, error);
				if (error)
					logger::warn("[TreeWindPatcher] Failed to create startup backup {}: {}", a_backupPath.string(), error.message());
				else
					logger::info("[TreeWindPatcher] Created startup backup {}", a_backupPath.string());
			} else if (!patchExists && backupExists) {
				std::filesystem::copy_file(a_backupPath, a_patchPath, std::filesystem::copy_options::none, error);
				if (error)
					logger::warn("[TreeWindPatcher] Failed to restore missing patch {}: {}", a_patchPath.string(), error.message());
				else
					logger::info("[TreeWindPatcher] Restored missing patch from {}", a_backupPath.string());
			}
		}

		void LoadRules()
		{
			runtimeRules.clear();
			ruleIds.clear();
			const auto patchDirectory = Util::PathHelpers::GetTreeWindPatchesPath();
			std::error_code error;
			if (!std::filesystem::is_directory(patchDirectory, error))
				return;

			const auto patchPath = patchDirectory / kPatchFileName;
			const auto backupPath = patchDirectory / kBackupFileName;
			EnsureEditablePatch(patchPath, backupPath);
			if (!std::filesystem::is_regular_file(patchPath, error))
				return;

			std::unordered_map<std::string, LoadedRule> mergedRules;
			LoadPatchFile(patchPath, mergedRules);

			std::vector<std::string> modelPaths;
			modelPaths.reserve(mergedRules.size());
			for (const auto& [modelPath, rule] : mergedRules)
				modelPaths.push_back(modelPath);
			std::ranges::sort(modelPaths);

			runtimeRules.reserve(modelPaths.size());
			ruleIds.reserve(modelPaths.size());
			for (const auto& modelPath : modelPaths) {
				const auto& merged = mergedRules.at(modelPath);
				auto rule = std::make_unique<RuntimeRule>();
				rule->mesh = modelPath;
				rule->bend.store(merged.bend, std::memory_order_relaxed);
				rule->leafAmbient.store(merged.leafAmbient, std::memory_order_relaxed);
				rule->persistedBend.store(merged.bend, std::memory_order_relaxed);
				rule->persistedLeafAmbient.store(merged.leafAmbient, std::memory_order_relaxed);
				const auto id = static_cast<std::uint32_t>(runtimeRules.size() + 1);
				ruleIds.emplace(rule->mesh, id);
				runtimeRules.push_back(std::move(rule));
			}

			if (!runtimeRules.empty())
				logger::info("[TreeWindPatcher] Loaded {} model rules from {}", runtimeRules.size(), patchPath.string());
		}

		void IncludePoint(ModelAabb& a_bounds, const RE::NiPoint3& a_point)
		{
			if (!std::isfinite(a_point.x) || !std::isfinite(a_point.y) || !std::isfinite(a_point.z))
				return;

			a_bounds.minimum.x = std::min(a_bounds.minimum.x, a_point.x);
			a_bounds.minimum.y = std::min(a_bounds.minimum.y, a_point.y);
			a_bounds.minimum.z = std::min(a_bounds.minimum.z, a_point.z);
			a_bounds.maximum.x = std::max(a_bounds.maximum.x, a_point.x);
			a_bounds.maximum.y = std::max(a_bounds.maximum.y, a_point.y);
			a_bounds.maximum.z = std::max(a_bounds.maximum.z, a_point.z);
			a_bounds.valid = true;
		}

		void IncludeTransformedBox(ModelAabb& a_bounds, const RE::NiPoint3& a_center,
			const RE::NiPoint3& a_extents, const RE::NiTransform& a_transform)
		{
			for (int x = -1; x <= 1; x += 2) {
				for (int y = -1; y <= 1; y += 2) {
					for (int z = -1; z <= 1; z += 2) {
						IncludePoint(a_bounds, a_transform * RE::NiPoint3{
																 a_center.x + a_extents.x * static_cast<float>(x),
																 a_center.y + a_extents.y * static_cast<float>(y),
																 a_center.z + a_extents.z * static_cast<float>(z) });
					}
				}
			}
		}

		std::optional<RE::NiTransform> GetTransformToAncestor(
			const RE::NiAVObject* a_object, const RE::NiAVObject* a_ancestor)
		{
			RE::NiTransform transform;
			const auto* current = a_object;
			while (current && current != a_ancestor) {
				transform = current->local * transform;
				current = current->parent;
			}
			return current == a_ancestor ? std::optional{ transform } : std::nullopt;
		}

		const RE::BSBound* FindAuthoredBound(
			const RE::NiAVObject* a_object, const RE::NiAVObject*& a_owner)
		{
			static REL::Relocation<const RE::NiRTTI*> boundRTTI{ RE::BSBound::Ni_RTTI };
			for (const auto* current = a_object; current; current = current->parent) {
				for (std::uint16_t index = 0; index < current->GetExtraDataSize(); ++index) {
					const auto* extraData = current->GetExtraDataAt(index);
					if (extraData && extraData->GetRTTI() == boundRTTI.get()) {
						a_owner = current;
						return static_cast<const RE::BSBound*>(extraData);
					}
				}
			}
			return nullptr;
		}

		bool IncludeGeometryVertices(
			ModelAabb& a_bounds, RE::BSGeometry& a_geometry, const RE::NiTransform& a_geometryToLeaf)
		{
			auto* rendererData = a_geometry.GetGeometryRuntimeData().rendererData;
			auto* triShape = a_geometry.AsTriShape();
			if (!rendererData || !triShape || !rendererData->rawVertexData ||
				!rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_VERTEX))
				return false;

			const std::uint32_t vertexSize = rendererData->vertexDesc.GetSize();
			const std::uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
			if (vertexSize < sizeof(RE::NiPoint3) || vertexCount == 0)
				return false;

			bool foundVertex = false;
#if defined(_MSC_VER)
			__try
#endif
			{
				for (std::uint32_t index = 0; index < vertexCount; ++index) {
					RE::NiPoint3 position;
					std::memcpy(&position,
						rendererData->rawVertexData + static_cast<std::size_t>(vertexSize) * index,
						sizeof(position));
					IncludePoint(a_bounds, a_geometryToLeaf * position);
					foundVertex = true;
				}
			}
#if defined(_MSC_VER)
			__except (1) {
				return false;
			}
#endif
			return foundVertex;
		}

		ModelAabb CalculateTreeBounds(RE::BSLeafAnimNode& a_leafParent)
		{
			ModelAabb bounds;
			bool usedNonVertexFallback = false;
			RE::BSVisit::TraverseScenegraphObjects(&a_leafParent, [&](RE::NiAVObject* a_object) {
				if (auto* geometry = a_object->AsGeometry()) {
					const auto geometryToLeaf = GetTransformToAncestor(geometry, &a_leafParent);
					const auto& modelBound = geometry->GetModelData().modelBound;
					const bool hasVertexBounds =
						geometryToLeaf && IncludeGeometryVertices(bounds, *geometry, *geometryToLeaf);
					if (!hasVertexBounds)
						usedNonVertexFallback = true;
					if (geometryToLeaf && !hasVertexBounds && std::isfinite(modelBound.radius) && modelBound.radius > 0.0f) {
						const auto center = *geometryToLeaf * modelBound.center;
						const float radius = std::abs(geometryToLeaf->scale) * modelBound.radius;
						IncludeTransformedBox(bounds, center, { radius, radius, radius }, RE::NiTransform{});
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (bounds.valid && !usedNonVertexFallback)
				return bounds;

			const RE::NiAVObject* boundOwner = nullptr;
			if (const auto* authoredBound = FindAuthoredBound(&a_leafParent, boundOwner)) {
				if (const auto leafToOwner = GetTransformToAncestor(&a_leafParent, boundOwner)) {
					ModelAabb authoredBounds;
					IncludeTransformedBox(
						authoredBounds, authoredBound->center, authoredBound->extents, leafToOwner->Invert());
					if (authoredBounds.valid)
						return authoredBounds;
				}
			}
			return bounds;
		}

		void SetTreeBoundsExtraData(RE::BSGeometry& a_geometry, const ModelAabb& a_leafBounds,
			const RE::NiTransform& a_geometryToLeaf)
		{
			ModelAabb geometryBounds;
			const RE::NiPoint3 center{
				(a_leafBounds.minimum.x + a_leafBounds.maximum.x) * 0.5f,
				(a_leafBounds.minimum.y + a_leafBounds.maximum.y) * 0.5f,
				(a_leafBounds.minimum.z + a_leafBounds.maximum.z) * 0.5f
			};
			const RE::NiPoint3 extents{
				(a_leafBounds.maximum.x - a_leafBounds.minimum.x) * 0.5f,
				(a_leafBounds.maximum.y - a_leafBounds.minimum.y) * 0.5f,
				(a_leafBounds.maximum.z - a_leafBounds.minimum.z) * 0.5f
			};
			IncludeTransformedBox(geometryBounds, center, extents, a_geometryToLeaf.Invert());
			if (!geometryBounds.valid)
				return;

			const float height = geometryBounds.maximum.z - geometryBounds.minimum.z;
			if (height <= kMinimumBoundExtent)
				return;

			const std::vector values{
				geometryBounds.minimum.z,
				height
			};
			if (auto* existing = a_geometry.GetExtraData(kTreeBoundsName)) {
				static REL::Relocation<const RE::NiRTTI*> floatsExtraDataRTTI{ RE::NiFloatsExtraData::Ni_RTTI };
				if (existing->GetRTTI() == floatsExtraDataRTTI.get()) {
					auto* boundsData = static_cast<RE::NiFloatsExtraData*>(existing);
					if (boundsData->size == kTreeBoundsValueCount && boundsData->value)
						std::ranges::copy(values, boundsData->value);
				} else {
					logger::warn("[TreeWindPatcher] Extra data '{}' exists with an incompatible type", kTreeBoundsName.c_str());
				}
				return;
			}

			if (auto* extraData = RE::NiFloatsExtraData::Create(kTreeBoundsName, values))
				a_geometry.AddExtraData(extraData);
		}

		void SetIntegerExtraData(RE::NiObjectNET& a_object, const RE::BSFixedString& a_name, std::int32_t a_value)
		{
			if (auto* existing = a_object.GetExtraData(a_name)) {
				static REL::Relocation<const RE::NiRTTI*> integerExtraDataRTTI{ RE::NiIntegerExtraData::Ni_RTTI };
				if (existing->GetRTTI() == integerExtraDataRTTI.get())
					static_cast<RE::NiIntegerExtraData*>(existing)->value = a_value;
				else
					logger::warn("[TreeWindPatcher] Extra data '{}' exists with an incompatible type", a_name.c_str());
				return;
			}

			if (auto* extraData = RE::NiIntegerExtraData::Create(a_name, a_value))
				a_object.AddExtraData(extraData);
		}

		void ApplyModelData(const char* a_modelName, RE::NiNode* a_root)
		{
			if (!a_modelName || !a_root)
				return;

			const auto modelPath = NormalizeModelPath(a_modelName);
			const auto ruleIt = ruleIds.find(modelPath);
			const bool hasRule = ruleIt != ruleIds.end();
			if (!hasRule && modelPath.find("/trees/") == std::string::npos)
				return;

			std::size_t patchedParentCount = 0;
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				if (auto* leafParent = netimmerse_cast<RE::BSLeafAnimNode*>(a_object)) {
					if (hasRule)
						SetIntegerExtraData(*leafParent, kRuleIdName, static_cast<std::int32_t>(ruleIt->second));

					const auto bounds = CalculateTreeBounds(*leafParent);
					if (bounds.valid) {
						RE::BSVisit::TraverseScenegraphObjects(leafParent, [&](RE::NiAVObject* a_child) {
							if (auto* geometry = a_child->AsGeometry()) {
								if (const auto geometryToLeaf = GetTransformToAncestor(geometry, leafParent))
									SetTreeBoundsExtraData(*geometry, bounds, *geometryToLeaf);
							}
							return RE::BSVisit::BSVisitControl::kContinue;
						});
					}
					++patchedParentCount;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});

			if (hasRule && patchedParentCount == 0)
				logger::debug("[TreeWindPatcher] Matched {} but found no BSLeafAnimNode", a_modelName);
			else if (hasRule)
				logger::debug("[TreeWindPatcher] Applied rule {} to {} leaf parents", ruleIt->second, patchedParentCount);
		}

		void ReadFloatExtraData(const RE::BSLeafAnimNode& a_leafParent, const RE::BSFixedString& a_name, float& a_value)
		{
			static REL::Relocation<const RE::NiRTTI*> floatExtraDataRTTI{ RE::NiFloatExtraData::Ni_RTTI };
			if (const auto* data = a_leafParent.GetExtraData(a_name); data && data->GetRTTI() == floatExtraDataRTTI.get()) {
				const float value = static_cast<const RE::NiFloatExtraData*>(data)->value;
				if (std::isfinite(value))
					a_value = std::clamp(value, kMinimumSensitivity, kMaximumSensitivity);
			}
		}

		void ReadTreeBoundsExtraData(const RE::BSGeometry& a_geometry, Sensitivities& a_values)
		{
			static REL::Relocation<const RE::NiRTTI*> floatsExtraDataRTTI{ RE::NiFloatsExtraData::Ni_RTTI };
			const auto* data = a_geometry.GetExtraData(kTreeBoundsName);
			if (!data || data->GetRTTI() != floatsExtraDataRTTI.get())
				return;

			const auto* boundsData = static_cast<const RE::NiFloatsExtraData*>(data);
			if (boundsData->size != kTreeBoundsValueCount || !boundsData->value)
				return;

			const float minimumZ = boundsData->value[0];
			const float height = boundsData->value[1];
			if (!std::isfinite(minimumZ) || !std::isfinite(height) || height <= kMinimumBoundExtent)
				return;

			a_values.boundMinimumZ = minimumZ;
			a_values.boundHeight = height;
			a_values.hasBounds = true;
		}

		struct TESProcessorPostCreate
		{
			static void thunk(
				RE::TESModelDB::TESProcessor* a_this,
				const RE::BSModelDB::DBTraits::ArgsType& a_args,
				const char* a_modelName,
				RE::NiPointer<RE::NiNode>& a_root,
				std::uint32_t& a_typeOut)
			{
				func(a_this, a_args, a_modelName, a_root, a_typeOut);
				ApplyModelData(a_modelName, a_root.get());
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void LoadAndInstall()
	{
		LoadRules();
		stl::write_vfunc<0x1, TESProcessorPostCreate>(RE::VTABLE_TESModelDB____TESProcessor[0]);
		logger::info("[TreeWindPatcher] Installed model creation hook");
	}

	Sensitivities GetSensitivities(const RE::BSGeometry* a_geometry)
	{
		Sensitivities sensitivities;
		if (!a_geometry)
			return sensitivities;
		ReadTreeBoundsExtraData(*a_geometry, sensitivities);

		const auto* leafParent = netimmerse_cast<RE::BSLeafAnimNode*>(a_geometry->parent);
		if (!leafParent)
			return sensitivities;

		static REL::Relocation<const RE::NiRTTI*> integerExtraDataRTTI{ RE::NiIntegerExtraData::Ni_RTTI };
		if (const auto* data = leafParent->GetExtraData(kRuleIdName);
			data && data->GetRTTI() == integerExtraDataRTTI.get()) {
			const auto id = static_cast<const RE::NiIntegerExtraData*>(data)->value;
			if (id > 0 && static_cast<std::size_t>(id) <= runtimeRules.size()) {
				const auto& rule = *runtimeRules[static_cast<std::size_t>(id) - 1];
				sensitivities.bend = rule.bend.load(std::memory_order_relaxed);
				sensitivities.leafAmbient = rule.leafAmbient.load(std::memory_order_relaxed);
				return sensitivities;
			}
		}

		ReadFloatExtraData(*leafParent, kBendSensitivityName, sensitivities.bend);
		ReadFloatExtraData(*leafParent, kLeafAmbientSensitivityName, sensitivities.leafAmbient);
		return sensitivities;
	}

	std::size_t GetRuleCount()
	{
		return runtimeRules.size();
	}

	RuleSnapshot GetRule(std::size_t a_index)
	{
		if (a_index >= runtimeRules.size())
			return {};

		const auto& rule = *runtimeRules[a_index];
		const float bend = rule.bend.load(std::memory_order_relaxed);
		const float leafAmbient = rule.leafAmbient.load(std::memory_order_relaxed);
		return {
			static_cast<std::uint32_t>(a_index + 1),
			rule.mesh,
			bend,
			leafAmbient,
			ValuesDiffer(bend, rule.persistedBend.load(std::memory_order_relaxed)) ||
				ValuesDiffer(leafAmbient, rule.persistedLeafAmbient.load(std::memory_order_relaxed))
		};
	}

	bool SetRule(std::size_t a_index, float a_bend, float a_leafAmbient)
	{
		if (a_index >= runtimeRules.size())
			return false;
		a_bend = ClampSensitivity(a_bend);
		a_leafAmbient = ClampSensitivity(a_leafAmbient);
		runtimeRules[a_index]->bend.store(a_bend, std::memory_order_relaxed);
		runtimeRules[a_index]->leafAmbient.store(a_leafAmbient, std::memory_order_relaxed);
		return true;
	}

	bool SetRule(std::string_view a_mesh, float a_bend, float a_leafAmbient)
	{
		const auto normalized = NormalizeModelPath(std::string(a_mesh));
		const auto ruleIt = ruleIds.find(normalized);
		return ruleIt != ruleIds.end() && SetRule(static_cast<std::size_t>(ruleIt->second) - 1, a_bend, a_leafAmbient);
	}

	void RevertUnsavedChanges()
	{
		for (auto& rule : runtimeRules) {
			rule->bend.store(rule->persistedBend.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->leafAmbient.store(rule->persistedLeafAmbient.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
	}

	SaveResult SaveRules()
	{
		std::scoped_lock lock(saveMutex);
		SaveResult result;
		const auto outputPath = Util::PathHelpers::GetTreeWindPatchesPath() / kPatchFileName;
		result.path = outputPath.string();

		nlohmann::json treeEntries = nlohmann::json::array();
		for (const auto& rule : runtimeRules) {
			const float bend = rule->bend.load(std::memory_order_relaxed);
			const float leafAmbient = rule->leafAmbient.load(std::memory_order_relaxed);
			treeEntries.push_back({
				{ "mesh", rule->mesh },
				{ "bendSensitivity", bend },
				{ "leafAmbientSensitivity", leafAmbient },
			});
		}

		const nlohmann::json root{
			{ "$schema", "TreeWindPatches.schema.json" },
			{ "version", 1 },
			{ "trees", std::move(treeEntries) },
		};

		std::error_code error;
		std::filesystem::create_directories(outputPath.parent_path(), error);
		if (error) {
			result.error = std::format("Failed to create patch directory: {}", error.message());
			return result;
		}

		std::ofstream stream(outputPath, std::ios::binary | std::ios::trunc);
		if (!stream) {
			result.error = "Failed to open the tree wind patch for writing";
			return result;
		}
		stream << root.dump(4) << '\n';
		stream.close();
		if (!stream) {
			result.error = "Failed while writing the tree wind patch";
			return result;
		}

		for (auto& rule : runtimeRules) {
			rule->persistedBend.store(rule->bend.load(std::memory_order_relaxed), std::memory_order_relaxed);
			rule->persistedLeafAmbient.store(rule->leafAmbient.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
		result.success = true;
		result.savedRuleCount = root["trees"].size();
		logger::info("[TreeWindPatcher] Saved {} model rules to {}", result.savedRuleCount, result.path);
		return result;
	}

	SaveResult RestoreBackup()
	{
		std::scoped_lock lock(saveMutex);
		SaveResult result;
		const auto patchDirectory = Util::PathHelpers::GetTreeWindPatchesPath();
		const auto outputPath = patchDirectory / kPatchFileName;
		const auto backupPath = patchDirectory / kBackupFileName;
		result.path = outputPath.string();

		std::error_code error;
		if (!std::filesystem::is_regular_file(backupPath, error)) {
			result.error = "Tree wind backup file is missing";
			return result;
		}

		std::unordered_map<std::string, LoadedRule> backupRules;
		if (!LoadPatchFile(backupPath, backupRules)) {
			result.error = "Tree wind backup is invalid";
			return result;
		}
		std::filesystem::copy_file(backupPath, outputPath, std::filesystem::copy_options::overwrite_existing, error);
		if (error) {
			result.error = std::format("Failed to restore backup: {}", error.message());
			return result;
		}

		for (auto& rule : runtimeRules) {
			const auto backupIt = backupRules.find(rule->mesh);
			const LoadedRule restored = backupIt != backupRules.end() ? backupIt->second : LoadedRule{};
			rule->bend.store(restored.bend, std::memory_order_relaxed);
			rule->leafAmbient.store(restored.leafAmbient, std::memory_order_relaxed);
			rule->persistedBend.store(restored.bend, std::memory_order_relaxed);
			rule->persistedLeafAmbient.store(restored.leafAmbient, std::memory_order_relaxed);
		}

		result.success = true;
		result.savedRuleCount = backupRules.size();
		logger::info("[TreeWindPatcher] Restored {} model rules from {}", result.savedRuleCount, backupPath.string());
		return result;
	}

	std::size_t GetUnsavedRuleCount()
	{
		return static_cast<std::size_t>(std::ranges::count_if(runtimeRules, [](const auto& a_rule) {
			return ValuesDiffer(a_rule->bend.load(std::memory_order_relaxed), a_rule->persistedBend.load(std::memory_order_relaxed)) ||
			       ValuesDiffer(a_rule->leafAmbient.load(std::memory_order_relaxed), a_rule->persistedLeafAmbient.load(std::memory_order_relaxed));
		}));
	}
}
