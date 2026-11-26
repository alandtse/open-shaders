#include "ScenePrepWorker.h"

#include "Globals.h"
#include "RE/B/BGSMovableStatic.h"
#include "RE/B/BSGeometry.h"
#include "RE/B/BSMeshLODTriShape.h"
#include "RE/B/BSModelDB.h"
#include "RE/B/BSShaderProperty.h"
#include "RE/B/BSTriShape.h"
#include "RE/E/ExtraEnableStateParent.h"
#include "RE/G/GridCellArray.h"
#include "RE/N/NiNode.h"
#include "RE/N/NiTimeController.h"
#include "RE/T/TES.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESModel.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectLAND.h"
#include "RE/T/TESObjectLIGH.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/T/TESObjectSTAT.h"
#include "RE/T/TESWorldSpace.h"
#include "ScenePrepCoverage.h"
#include "ScenePrepEligibility.h"
#include "ScenePrepEngineBridge.h"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace DX12
{
	namespace
	{
		constexpr size_t kMaxCandidateRefs = 150'000;
		constexpr size_t kMaxTileShapes = 384;
		constexpr size_t kMaxNifSources = 4096;
		constexpr size_t kMaxMissingShapes = 4096;
		constexpr size_t kMaxRejectionReasons = 12;
		constexpr uint64_t kMaxTileGeometryBytes = 48ull * 1024 * 1024;
		constexpr uint64_t kMaxDecodedCacheBytes = 48ull * 1024 * 1024;
		constexpr uint32_t kMaxSceneGraphDepth = 32;
		constexpr uint32_t kTerrainOrdinalBase = 0x10000u;

		void RecordRejection(
			std::vector<std::pair<std::string, uint32_t>>& a_reasons, uint32_t& a_failedCount,
			std::string_view a_reason, uint32_t a_count = 1)
		{
			a_failedCount += a_count;
			if (!a_count)
				return;
			const std::string key(a_reason.substr(0, 80));
			for (auto& [existing, total] : a_reasons) {
				if (existing == key) {
					total += a_count;
					return;
				}
			}
			if (a_reasons.size() < kMaxRejectionReasons) {
				a_reasons.emplace_back(key, a_count);
				return;
			}
			for (auto& [existing, total] : a_reasons) {
				if (existing == "other") {
					total += a_count;
					return;
				}
			}
			a_reasons.back().second += a_count;
		}

		void AddMissingShapeEntry(
			std::vector<MissingNifShapeDescriptor>& a_missing,
			std::vector<std::pair<std::string, uint32_t>>& a_reasons, uint32_t& a_failedCount,
			std::string_view a_plugin, uint32_t a_localID,
			std::string_view a_nifPath, int32_t a_blockIndex, std::string_view a_reason)
		{
			if (a_nifPath.empty() || a_nifPath.size() > 260)
				return;
			if (a_missing.size() >= kMaxMissingShapes) {
				RecordRejection(a_reasons, a_failedCount, "missing shape manifest capacity");
				return;
			}
			MissingNifShapeDescriptor entry;
			entry.referencePlugin = std::string(a_plugin.substr(0, 128));
			entry.referenceLocal = a_localID;
			entry.nifPath = std::string(a_nifPath);
			entry.blockIndex = a_blockIndex;
			entry.reason = std::string(a_reason.substr(0, 80));
			a_missing.push_back(std::move(entry));
		}

		void AddMissingShape(
			ScenePrepRequest& a_request, std::string_view a_plugin, uint32_t a_localID,
			std::string_view a_nifPath, int32_t a_blockIndex, std::string_view a_reason)
		{
			AddMissingShapeEntry(
				a_request.missingShapes, a_request.rejectionReasons, a_request.failedShapes,
				a_plugin, a_localID, a_nifPath, a_blockIndex, a_reason);
		}

		void AddNifSource(
			ScenePrepRequest& a_request, std::string_view a_plugin, uint32_t a_localID,
			std::string_view a_nifPath)
		{
			if (a_nifPath.empty() || a_nifPath.size() > 260 || a_plugin.empty())
				return;
			if (a_request.nifSources.size() >= kMaxNifSources) {
				RecordRejection(a_request.rejectionReasons, a_request.failedShapes, "NIF source manifest capacity");
				return;
			}
			NifSourceDescriptor entry;
			entry.referencePlugin = std::string(a_plugin.substr(0, 128));
			entry.referenceLocal = a_localID;
			entry.nifPath = std::string(a_nifPath);
			a_request.nifSources.push_back(std::move(entry));
		}

		bool IsSupportedRigidGeometryType(RE::BSGeometry::Type a_type) noexcept
		{
			switch (a_type) {
			case RE::BSGeometry::Type::kTriShape:
			case RE::BSGeometry::Type::kMeshLODTriShape:
			case RE::BSGeometry::Type::kLODMultiIndexTriShape:
			case RE::BSGeometry::Type::kMultiIndexTriShape:
			case RE::BSGeometry::Type::kSubIndexTriShape:
			case RE::BSGeometry::Type::kSubIndexLandTriShape:
				return true;
			default:
				return false;
			}
		}

		struct ExtractedShapeDesc
		{
			const RE::BSGraphics::TriShape* rendererData = nullptr;
			const uint8_t* rawVertices = nullptr;
			const uint16_t* rawIndices = nullptr;
			uint32_t vertexCount = 0;
			uint32_t firstIndex = 0;
			uint32_t indexCount = 0;
			uint32_t stride = 0;
		};

		enum class GeometryInspectResult : uint8_t
		{
			Accepted,
			SkippedNonShadow,
			Rejected
		};

		GeometryInspectResult InspectGeometry(
			RE::BSGeometry& a_geom, ExtractedShapeDesc& a_out, std::string_view& a_outReason)
		{
			auto* tri = a_geom.AsTriShape();
			if (!tri || a_geom.AsDynamicTriShape() || !IsSupportedRigidGeometryType(a_geom.GetType().get())) {
				a_outReason = "unsupported animated or nontriangle shape";
				return GeometryInspectResult::Rejected;
			}
			const auto& runtime = a_geom.GetGeometryRuntimeData();
			if (runtime.skinInstance) {
				a_outReason = "unsupported animated or nontriangle shape";
				return GeometryInspectResult::Rejected;
			}
			const auto* property = runtime.shaderProperty.get();
			if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get() ||
				property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction,
					RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction)) {
				return GeometryInspectResult::SkippedNonShadow;
			}
			if (runtime.alphaProperty) {
				a_outReason = "alpha-dependent shape (textures required)";
				return GeometryInspectResult::Rejected;
			}
			const auto* rendererData = runtime.rendererData;
			if (!rendererData || !rendererData->rawVertexData || !rendererData->rawIndexData) {
				a_outReason = "invalid or unsupported shape data";
				return GeometryInspectResult::Rejected;
			}
			const uint64_t rawDesc = std::bit_cast<uint64_t>(runtime.vertexDesc);
			const uint32_t stride = VertexStrideBytes(rawDesc);
			const auto counts = tri->GetTrishapeRuntimeData();
			if (!HasVertexPositionStream(rawDesc) || stride < 16 || stride > 256 ||
				!counts.vertexCount || !counts.triangleCount) {
				a_outReason = "invalid or unsupported shape data";
				return GeometryInspectResult::Rejected;
			}
			uint32_t firstIndex = 0;
			uint32_t indexCount = uint32_t(counts.triangleCount) * 3u;
			if (a_geom.GetType() == RE::BSGeometry::Type::kMeshLODTriShape) {
				const auto& lodCounts = static_cast<const RE::BSMeshLODTriShape&>(a_geom)
				                            .GetMeshLODTriShapeRuntimeData()
				                            .lodTriangleCounts;
				const auto slice = ResolveMeshLODSlice(a_geom.GetLODMode(), lodCounts, counts.triangleCount);
				if (!slice) {
					a_outReason = "invalid or unsupported shape data";
					return GeometryInspectResult::Rejected;
				}
				firstIndex = slice->first;
				indexCount = slice->second;
			}
			a_out.rendererData = rendererData;
			a_out.rawVertices = rendererData->rawVertexData;
			a_out.rawIndices = rendererData->rawIndexData;
			a_out.vertexCount = counts.vertexCount;
			a_out.firstIndex = firstIndex;
			a_out.indexCount = indexCount;
			a_out.stride = stride;
			return GeometryInspectResult::Accepted;
		}

		void CaptureAttachedSubtree(
			RE::NiAVObject* a_obj, uint32_t a_ownerFormID, std::string_view a_plugin, uint32_t a_localID,
			std::string_view a_nifPath, uint32_t& a_shapeOrdinal, uint32_t a_depth, ScenePrepRequest& a_request)
		{
			if (!a_obj || a_obj->GetAppCulled())
				return;
			if (IsSceneGraphDepthExceeded(a_depth, kMaxSceneGraphDepth)) {
				RecordRejection(a_request.rejectionReasons, a_request.failedShapes, "scene graph depth limit exceeded");
				AddMissingShape(a_request, a_plugin, a_localID, a_nifPath, -1, "scene graph depth limit exceeded");
				return;
			}
			if (a_obj->GetControllers() != nullptr) {
				RecordRejection(a_request.rejectionReasons, a_request.failedShapes, "unsupported animated or nontriangle shape");
				AddMissingShape(a_request, a_plugin, a_localID, a_nifPath, -1, "unsupported animated or nontriangle shape");
				return;
			}
			if (auto* node = a_obj->AsNode()) {
				for (const auto& child : node->GetChildren())
					CaptureAttachedSubtree(child.get(), a_ownerFormID, a_plugin, a_localID, a_nifPath, a_shapeOrdinal, a_depth + 1, a_request);
				return;
			}
			auto* geom = a_obj->AsGeometry();
			if (!geom)
				return;

			const uint32_t ordinal = a_shapeOrdinal++;
			ExtractedShapeDesc extracted{};
			std::string_view rejectReason;
			const auto status = InspectGeometry(*geom, extracted, rejectReason);
			if (status == GeometryInspectResult::SkippedNonShadow)
				return;
			if (status == GeometryInspectResult::Rejected) {
				RecordRejection(a_request.rejectionReasons, a_request.failedShapes, rejectReason);
				AddMissingShape(a_request, a_plugin, a_localID, a_nifPath, int32_t(ordinal), rejectReason);
				return;
			}
			const auto rows = TransformToRows3x4(FromNiTransform(geom->world));
			if (!IsInvertibleTransform3x4(rows)) {
				RecordRejection(a_request.rejectionReasons, a_request.failedShapes, "invalid NIF transform");
				AddMissingShape(a_request, a_plugin, a_localID, a_nifPath, int32_t(ordinal), "invalid NIF transform");
				return;
			}
			CapturedAttachedShape captured;
			captured.identity = MakeShapeIdentity(a_ownerFormID, ordinal);
			captured.rendererData = extracted.rendererData;
			captured.rawVertices = extracted.rawVertices;
			captured.rawIndices = extracted.rawIndices;
			captured.vertexCount = extracted.vertexCount;
			captured.firstIndex = extracted.firstIndex;
			captured.indexCount = extracted.indexCount;
			captured.stride = extracted.stride;
			captured.transform = rows;
			a_request.attachedShapes.push_back(captured);
			// Retain the BSGeometry on the worker read path; drained back on the game thread.
			a_request.keepAlive.emplace_back(geom);
		}

		struct CachedDecodedMesh
		{
			std::vector<float3> positions;
			std::vector<uint32_t> indices;
			uint64_t bytes = 0;
		};

		struct RawMeshSliceKey
		{
			const RE::BSGraphics::TriShape* rendererData = nullptr;
			uint32_t firstIndex = 0;
			uint32_t indexCount = 0;

			bool operator==(const RawMeshSliceKey&) const = default;
		};

		struct RawMeshSliceKeyHash
		{
			size_t operator()(const RawMeshSliceKey& a_key) const noexcept
			{
				size_t h = std::hash<const void*>{}(a_key.rendererData);
				h ^= std::hash<uint32_t>{}(a_key.firstIndex) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(a_key.indexCount) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				return h;
			}
		};

		struct ExtractedModelShape
		{
			uint32_t ordinal = 0;
			std::shared_ptr<const CachedDecodedMesh> mesh;
			AffineTransform toModel{};
		};

		struct CachedModelExtraction
		{
			std::vector<ExtractedModelShape> shapes;
			std::vector<std::tuple<int32_t, std::string>> missing;
			std::vector<std::pair<std::string, uint32_t>> rejections;
			uint64_t bytes = 0;
		};

		void WalkDemandModelSubtree(
			RE::NiAVObject* a_obj, const AffineTransform& a_parentTransform, uint32_t& a_shapeOrdinal,
			uint32_t a_depth, CachedModelExtraction& a_out)
		{
			if (!a_obj || a_obj->GetAppCulled())
				return;
			if (IsSceneGraphDepthExceeded(a_depth, kMaxSceneGraphDepth)) {
				uint32_t ignored = 0;
				RecordRejection(a_out.rejections, ignored, "scene graph depth limit exceeded");
				a_out.missing.emplace_back(-1, "scene graph depth limit exceeded");
				return;
			}
			if (a_obj->GetControllers() != nullptr) {
				uint32_t ignored = 0;
				RecordRejection(a_out.rejections, ignored, "unsupported animated or nontriangle shape");
				a_out.missing.emplace_back(-1, "unsupported animated or nontriangle shape");
				return;
			}
			const auto currentTransform = ComposeAffine(a_parentTransform, FromNiTransform(a_obj->local));
			if (auto* node = a_obj->AsNode()) {
				for (const auto& child : node->GetChildren())
					WalkDemandModelSubtree(child.get(), currentTransform, a_shapeOrdinal, a_depth + 1, a_out);
				return;
			}
			auto* geom = a_obj->AsGeometry();
			if (!geom)
				return;

			const uint32_t ordinal = a_shapeOrdinal++;
			ExtractedShapeDesc extracted{};
			std::string_view rejectReason;
			const auto status = InspectGeometry(*geom, extracted, rejectReason);
			if (status == GeometryInspectResult::SkippedNonShadow)
				return;
			if (status == GeometryInspectResult::Rejected) {
				uint32_t ignored = 0;
				RecordRejection(a_out.rejections, ignored, rejectReason);
				a_out.missing.emplace_back(int32_t(ordinal), std::string(rejectReason));
				return;
			}
			const auto rows = TransformToRows3x4(currentTransform);
			if (!IsInvertibleTransform3x4(rows)) {
				uint32_t ignored = 0;
				RecordRejection(a_out.rejections, ignored, "invalid NIF transform");
				a_out.missing.emplace_back(int32_t(ordinal), "invalid NIF transform");
				return;
			}
			auto mesh = std::make_shared<CachedDecodedMesh>();
			if (!DecodeRawMeshBuffers(extracted.rawVertices, extracted.vertexCount, extracted.stride,
					extracted.rawIndices, extracted.firstIndex, extracted.indexCount,
					mesh->positions, mesh->indices)) {
				uint32_t ignored = 0;
				RecordRejection(a_out.rejections, ignored, "invalid or unsupported shape data");
				a_out.missing.emplace_back(int32_t(ordinal), "invalid or unsupported shape data");
				return;
			}
			mesh->bytes = uint64_t(mesh->positions.size()) * sizeof(float3) +
			              uint64_t(mesh->indices.size()) * sizeof(uint32_t);
			a_out.bytes += 128ull + mesh->bytes;
			a_out.shapes.push_back({ ordinal, std::move(mesh), currentTransform });
		}
	}

	std::optional<ScenePrepRequest> CaptureScenePrepRequest(
		SubcellKey a_key, uint32_t a_size, const RE::TESObjectCELL* a_cell,
		const RE::TESWorldSpace* a_world, bool a_predictive, bool a_corner)
	{
		if (!a_key.scope || !std::ranges::contains(kSubcellSizes, a_size) || !a_cell)
			return std::nullopt;
		if (!a_world && (!a_cell->IsInteriorCell() || a_cell->GetFormID() != a_key.scope))
			return std::nullopt;
		if (a_world && (!globals::game::tes || a_world->GetFormID() != a_key.scope))
			return std::nullopt;

		ScenePrepRequest request;
		request.key = a_key;
		request.size = a_size;
		request.worldspace = a_world != nullptr;
		request.predictive = a_predictive;
		request.corner = a_corner;

		ankerl::unordered_dense::set<uint32_t> visitedRefs;
		size_t candidateCount = 0;

		const auto visitRef = [&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult {
			if (!a_ref || a_ref->IsDeleted() || a_ref->IsDisabled() || a_ref->IsDynamicForm())
				return RE::BSContainer::ForEachResult::kContinue;
			const uint32_t refID = a_ref->GetFormID();
			if (!refID || !visitedRefs.insert(refID).second)
				return RE::BSContainer::ForEachResult::kContinue;

			const auto pos = a_ref->GetPosition();
			if (request.worldspace && !InCandidateHaloXY(pos.x, pos.y, a_key.x, a_key.y, a_size))
				return RE::BSContainer::ForEachResult::kContinue;

			if (++candidateCount > kMaxCandidateRefs) {
				request.candidateOverflow = true;
				return RE::BSContainer::ForEachResult::kStop;
			}

			const auto* base = a_ref->GetBaseObject();
			if (!base) {
				RecordRejection(request.rejectionReasons, request.failedShapes, "unresolved base record");
				return RE::BSContainer::ForEachResult::kContinue;
			}

			if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
				RecordRejection(request.rejectionReasons, request.failedShapes, "invalid placement");
				return RE::BSContainer::ForEachResult::kContinue;
			}

			if (const auto* light = base->As<RE::TESObjectLIGH>()) {
				if (InsideSubcellXY(pos.x, pos.y, a_key.x, a_key.y, a_size)) {
					if (request.lights.size() < kMaxPreparedLights) {
						PreparedLight prepared;
						prepared.referenceID = refID;
						prepared.position = float3{ pos.x, pos.y, pos.z };
						prepared.color = float3{
							float(light->data.color.red) / 255.0f,
							float(light->data.color.green) / 255.0f,
							float(light->data.color.blue) / 255.0f
						};
						prepared.radius = float(light->data.radius);
						prepared.fade = light->fade;
						prepared.emitterRadius = 0.0f;
						prepared.falloffExponent = light->data.fallofExponent;
						prepared.flags = light->data.flags.underlying();
						if (std::isfinite(prepared.radius) && std::isfinite(prepared.fade) &&
							std::isfinite(prepared.falloffExponent))
							request.lights.push_back(prepared);
					} else {
						request.lightCapacityExceeded = true;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			const auto* refFile = a_ref->GetFile(0);
			const std::string_view plugin = refFile ? refFile->GetFilename() : std::string_view{};
			const uint32_t localID = a_ref->GetLocalFormID();
			const auto* modeled = base->As<RE::TESModel>();
			const std::optional<std::string> nifPath =
				(modeled && modeled->GetModel() && modeled->GetModel()[0] != '\0') ?
					NormalizeNifPath(modeled->GetModel()) :
					std::nullopt;
			const std::string_view nifView = nifPath ? std::string_view(*nifPath) : std::string_view{};

			const bool supportedStatic = base->Is(
				RE::FormType::Static, RE::FormType::StaticCollection,
				RE::FormType::Furniture, RE::FormType::MovableStatic);
			if (!supportedStatic) {
				if (!nifView.empty() && (!request.worldspace || InsideSubcellXY(pos.x, pos.y, a_key.x, a_key.y, a_size))) {
					RecordRejection(request.rejectionReasons, request.failedShapes, "unsupported modeled record");
					AddNifSource(request, plugin, localID, nifView);
					AddMissingShape(request, plugin, localID, nifView, -1, "unsupported modeled record");
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			if (const auto* stat = base->As<RE::TESObjectSTAT>(); stat && stat->IsSkyObject())
				return RE::BSContainer::ForEachResult::kContinue;

			if (!nifView.empty())
				AddNifSource(request, plugin, localID, nifView);

			if (IsRuntimeDependentPlacement(
					base->Is(RE::FormType::MovableStatic), a_ref->HasVMAD(),
					a_ref->extraList.GetByType<RE::ExtraEnableStateParent>() != nullptr,
					a_ref->IsInitiallyDisabled())) {
				RecordRejection(request.rejectionReasons, request.failedShapes, "runtime-dependent placement");
				AddMissingShape(request, plugin, localID, nifView, -1, "runtime-dependent placement");
				return RE::BSContainer::ForEachResult::kContinue;
			}

			if (auto* root = a_ref->Get3D()) {
				uint32_t ordinal = 0;
				CaptureAttachedSubtree(root, refID, plugin, localID, nifView, ordinal, 0, request);
				return RE::BSContainer::ForEachResult::kContinue;
			}

			if (!nifPath) {
				RecordRejection(request.rejectionReasons, request.failedShapes, "missing mesh path");
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto placement = PlacementFromEulerXYZ(
				float3{ pos.x, pos.y, pos.z },
				float3{ a_ref->GetAngleX(), a_ref->GetAngleY(), a_ref->GetAngleZ() },
				a_ref->GetScale());
			if (!placement) {
				RecordRejection(request.rejectionReasons, request.failedShapes, "invalid placement");
				AddMissingShape(request, plugin, localID, nifView, -1, "invalid placement");
				return RE::BSContainer::ForEachResult::kContinue;
			}
			request.unattachedPlacements.push_back({ refID,
				localID,
				std::string(plugin),
				*nifPath,
				*placement });
			return RE::BSContainer::ForEachResult::kContinue;
		};

		if (!request.worldspace) {
			request.enumerationComplete = a_cell->IsAttached();
			a_cell->ForEachReference(visitRef);
		} else {
			request.enumerationComplete = false;
			const auto coreWindow = SubcellExteriorCellWindow(a_key.x, a_key.y, a_size, 0);
			const auto haloWindow = SubcellExteriorCellWindow(a_key.x, a_key.y, a_size, 1);

			const int64_t coreWidth = int64_t(coreWindow.maxCellX) - coreWindow.minCellX + 1;
			const int64_t coreHeight = int64_t(coreWindow.maxCellY) - coreWindow.minCellY + 1;
			std::vector<bool> coreAttached(
				coreWidth > 0 && coreHeight > 0 && coreWidth <= 32 && coreHeight <= 32 ?
					size_t(coreWidth * coreHeight) :
					0u,
				false);

			if (const auto* gridCells = globals::game::tes->gridCells) {
				for (uint32_t gx = 0; gx < gridCells->length && !request.candidateOverflow; ++gx) {
					for (uint32_t gy = 0; gy < gridCells->length && !request.candidateOverflow; ++gy) {
						auto* extCell = gridCells->GetCell(gx, gy);
						if (!extCell || !extCell->IsAttached() || !extCell->IsExteriorCell())
							continue;
						const auto* coords = extCell->GetCoordinates();
						if (!coords)
							continue;
						const int32_t cx = coords->cellX;
						const int32_t cy = coords->cellY;
						const bool inCore = cx >= coreWindow.minCellX && cx <= coreWindow.maxCellX &&
						                    cy >= coreWindow.minCellY && cy <= coreWindow.maxCellY;
						if (inCore && !coreAttached.empty()) {
							const size_t idx = size_t(cx - coreWindow.minCellX) * size_t(coreHeight) +
							                   size_t(cy - coreWindow.minCellY);
							coreAttached[idx] = true;
						}
						if (cx < haloWindow.minCellX || cx > haloWindow.maxCellX ||
							cy < haloWindow.minCellY || cy > haloWindow.maxCellY)
							continue;

						extCell->ForEachReference(visitRef);

						if (inCore && !request.candidateOverflow) {
							if (const auto* land = extCell->GetRuntimeData().cellLand; land && land->loadedData) {
								const uint32_t landOwner = land->GetFormID() ? land->GetFormID() : extCell->GetFormID();
								uint32_t landOrdinal = kTerrainOrdinalBase;
								for (uint32_t q = 0; q < 4; ++q) {
									if (auto* meshNode = land->loadedData->mesh[q])
										CaptureAttachedSubtree(meshNode, landOwner, {}, 0, {}, landOrdinal, 0, request);
									else if (auto* geom = land->loadedData->geom[q].get())
										CaptureAttachedSubtree(geom, landOwner, {}, 0, {}, landOrdinal, 0, request);
								}
							}
						}
					}
				}
			}

			request.hasUnattachedCells = coreAttached.empty() || !std::ranges::all_of(coreAttached, std::identity{});
			if (!request.candidateOverflow && a_world->persistentCell)
				a_world->persistentCell->ForEachReference(visitRef);
		}

		if (request.lightCapacityExceeded && request.rejectionReasons.size() < kMaxRejectionReasons)
			request.rejectionReasons.emplace_back("tile light capacity", 1u);

		return request;
	}

	ScenePrepWorker::ScenePrepWorker() :
		worker([this](std::stop_token a_stop) {
			try {
				Run(a_stop);
			} catch (const std::exception& error) {
				std::scoped_lock lock(mutex);
				running = false;
				if (!errorLogged) {
					errorLogged = true;
					logger::error("[DX12] Scene preparation worker failed: {}", error.what());
				}
			} catch (...) {
				std::scoped_lock lock(mutex);
				running = false;
				if (!errorLogged) {
					errorLogged = true;
					logger::error("[DX12] Scene preparation worker failed with unknown exception");
				}
			}
		})
	{}

	ScenePrepWorker::~ScenePrepWorker()
	{
		worker.request_stop();
		wake.notify_all();
		if (worker.joinable())
			worker.join();
		DrainRetiredObjects();
	}

	bool ScenePrepWorker::Idle() const
	{
		std::scoped_lock lock(mutex);
		return !running && !queued && !completed;
	}

	bool ScenePrepWorker::Enqueue(ScenePrepRequest a_request)
	{
		std::scoped_lock lock(mutex);
		if (running || queued || completed)
			return false;
		queued = std::move(a_request);
		wake.notify_one();
		return true;
	}

	std::optional<ScenePrepResult> ScenePrepWorker::Take()
	{
		std::scoped_lock lock(mutex);
		return std::exchange(completed, std::nullopt);
	}

	void ScenePrepWorker::Cancel()
	{
		generation.fetch_add(1, std::memory_order_acq_rel);
		std::scoped_lock lock(mutex);
		if (queued)
			RetireObjectsLocked(queued->keepAlive);
		queued.reset();
		completed.reset();
		errorLogged = false;
		wake.notify_all();
	}

	void ScenePrepWorker::DrainRetiredObjects()
	{
		std::vector<RE::NiPointer<RE::NiObject>> toRelease;
		{
			std::scoped_lock lock(mutex);
			toRelease.swap(retiredObjects);
		}
	}

	void ScenePrepWorker::RetireObjectsLocked(std::vector<RE::NiPointer<RE::NiObject>>& a_objects)
	{
		if (a_objects.empty())
			return;
		retiredObjects.insert(
			retiredObjects.end(),
			std::make_move_iterator(a_objects.begin()),
			std::make_move_iterator(a_objects.end()));
		a_objects.clear();
	}

	void ScenePrepWorker::Run(std::stop_token a_stop)
	{
		uint64_t observedGeneration = generation.load(std::memory_order_acquire);
		while (!a_stop.stop_requested()) {
			std::optional<ScenePrepRequest> task;
			uint64_t epoch = 0;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, a_stop, [&] {
					return queued.has_value() || generation.load(std::memory_order_acquire) != observedGeneration;
				});
				if (a_stop.stop_requested())
					break;
				if (generation.load(std::memory_order_acquire) != observedGeneration) {
					observedGeneration = generation.load(std::memory_order_acquire);
					continue;
				}
				task = std::move(*queued);
				queued.reset();
				running = true;
				epoch = generation.load(std::memory_order_acquire);
			}

			const auto started = std::chrono::steady_clock::now();
			std::vector<RE::NiPointer<RE::NiObject>> localRetired;
			ScenePrepResult result;
			try {
				result = Prepare(std::move(*task), epoch, a_stop, localRetired);
			} catch (const std::exception& error) {
				result.publication.key = task ? task->key : SubcellKey{};
				result.failure = std::format("Scene preparation error: {}", std::string_view(error.what()).substr(0, 240));
				if (task)
					for (auto& obj : task->keepAlive)
						localRetired.push_back(std::move(obj));
			} catch (...) {
				result.publication.key = task ? task->key : SubcellKey{};
				result.failure = "Unknown scene preparation failure";
				if (task)
					for (auto& obj : task->keepAlive)
						localRetired.push_back(std::move(obj));
			}
			result.preparationMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - started)
			                           .count();
			{
				std::scoped_lock lock(mutex);
				RetireObjectsLocked(localRetired);
				running = false;
				if (!a_stop.stop_requested() && epoch == generation.load(std::memory_order_acquire))
					completed = std::move(result);
			}
		}
	}

	ScenePrepResult ScenePrepWorker::Prepare(
		ScenePrepRequest a_request, uint64_t a_epoch, std::stop_token a_stop,
		std::vector<RE::NiPointer<RE::NiObject>>& a_outRetired)
	{
		a_outRetired = std::move(a_request.keepAlive);

		ScenePrepResult result;
		result.predictive = a_request.predictive;
		result.corner = a_request.corner;
		auto& pub = result.publication;
		pub.key = a_request.key;
		pub.lights = std::move(a_request.lights);
		pub.nifSources = std::move(a_request.nifSources);
		pub.missingShapes = std::move(a_request.missingShapes);
		pub.rejectionReasons = std::move(a_request.rejectionReasons);
		pub.failedShapes = a_request.failedShapes;
		pub.hasUnattachedCells = a_request.hasUnattachedCells;

		const auto canceled = [&] {
			return a_stop.stop_requested() || generation.load(std::memory_order_acquire) != a_epoch;
		};

		if (a_request.candidateOverflow) {
			result.failure = "Candidate reference limit exceeded";
			return result;
		}

		std::unordered_set<uint64_t> emittedIdentities;
		const auto emitShape = [&](
								   uint64_t a_identity, const std::vector<float3>& a_positions,
								   const std::vector<uint32_t>& a_indices, const std::array<float, 12>& a_transform,
								   std::string_view a_plugin, uint32_t a_localID, std::string_view a_nifPath,
								   int32_t a_blockIndex) {
			const auto overlaps = OverlapsSubcellXY(a_positions, a_transform, a_request.key.x, a_request.key.y, a_request.size);
			if (!overlaps) {
				RecordRejection(pub.rejectionReasons, pub.failedShapes, "invalid placed bounds");
				AddMissingShapeEntry(
					pub.missingShapes, pub.rejectionReasons, pub.failedShapes,
					a_plugin, a_localID, a_nifPath, a_blockIndex, "invalid placed bounds");
				return;
			}
			if (!*overlaps)
				return;

			++pub.eligibleShapes;
			const uint64_t bytes = uint64_t(a_positions.size()) * sizeof(float3) +
			                       uint64_t(a_indices.size()) * sizeof(uint32_t);
			if (pub.shapes.size() >= kMaxTileShapes || bytes > kMaxTileGeometryBytes - pub.geometryBytes) {
				RecordRejection(pub.rejectionReasons, pub.failedShapes, "tile shape or byte budget exceeded");
				return;
			}
			if (!a_identity || !emittedIdentities.insert(a_identity).second) {
				RecordRejection(pub.rejectionReasons, pub.failedShapes, "duplicate shape identity");
				return;
			}

			auto shape = std::make_shared<Shape>();
			shape->identity = a_identity;
			shape->positions = a_positions;
			shape->indices = a_indices;
			shape->transform = a_transform;
			pub.geometryBytes += bytes;
			pub.shapes.push_back(std::move(shape));
		};

		std::unordered_map<RawMeshSliceKey, std::shared_ptr<const CachedDecodedMesh>, RawMeshSliceKeyHash> rawCache;
		uint64_t rawCacheBytes = 0;

		for (const auto& attached : a_request.attachedShapes) {
			if (canceled()) {
				result.failure = "Preparation canceled by scene change";
				return result;
			}
			const RawMeshSliceKey sliceKey{ attached.rendererData, attached.firstIndex, attached.indexCount };
			std::shared_ptr<const CachedDecodedMesh> decoded;
			if (const auto it = rawCache.find(sliceKey); it != rawCache.end()) {
				decoded = it->second;
			} else {
				auto fresh = std::make_shared<CachedDecodedMesh>();
				if (!DecodeRawMeshBuffers(
						attached.rawVertices, attached.vertexCount, attached.stride,
						attached.rawIndices, attached.firstIndex, attached.indexCount,
						fresh->positions, fresh->indices)) {
					RecordRejection(pub.rejectionReasons, pub.failedShapes, "invalid or unsupported shape data");
					continue;
				}
				fresh->bytes = uint64_t(fresh->positions.size()) * sizeof(float3) +
				               uint64_t(fresh->indices.size()) * sizeof(uint32_t);
				decoded = fresh;
				if (fresh->bytes <= kMaxDecodedCacheBytes - rawCacheBytes) {
					rawCacheBytes += fresh->bytes;
					rawCache.emplace(sliceKey, decoded);
				}
			}
			emitShape(attached.identity, decoded->positions, decoded->indices, attached.transform, {}, 0, {}, -1);
		}
		rawCache.clear();

		std::unordered_map<std::string, std::shared_ptr<const CachedModelExtraction>> modelCache;
		uint64_t modelCacheBytes = 0;

		for (const auto& placement : a_request.unattachedPlacements) {
			if (canceled()) {
				result.failure = "Preparation canceled by scene change";
				return result;
			}
			std::shared_ptr<const CachedModelExtraction> extraction;
			if (const auto it = modelCache.find(placement.nifPath); it != modelCache.end()) {
				extraction = it->second;
			} else {
				auto fresh = std::make_shared<CachedModelExtraction>();
				RE::BSModelDB::DBTraits::ArgsType args{};
				args.LODmult = 0;
				args.texLoadLevel = 0;
				args.unk8 = false;
				args.unk9 = false;
				args.unkA = false;
				args.postProcess = false;
				RE::NiPointer<RE::NiNode> modelRoot;
				const auto err = RE::BSModelDB::Demand(placement.nifPath.c_str(), modelRoot, args);
				if (modelRoot)
					a_outRetired.emplace_back(modelRoot.get());
				if (err != RE::BSResource::ErrorCode::kNone || !modelRoot) {
					uint32_t ignored = 0;
					RecordRejection(fresh->rejections, ignored, "NIF unavailable or unsupported");
					fresh->missing.emplace_back(-1, "NIF unavailable or unsupported");
				} else {
					uint32_t ordinal = 0;
					WalkDemandModelSubtree(modelRoot.get(), AffineTransform{}, ordinal, 0, *fresh);
					if (fresh->shapes.empty() && fresh->missing.empty()) {
						uint32_t ignored = 0;
						RecordRejection(fresh->rejections, ignored, "no supported opaque rigid triangles");
						fresh->missing.emplace_back(-1, "no supported opaque rigid triangles");
					}
				}
				extraction = fresh;
				fresh->bytes += 256ull + placement.nifPath.size() + fresh->missing.size() * 96ull;
				if (fresh->bytes <= kMaxDecodedCacheBytes - modelCacheBytes) {
					modelCacheBytes += fresh->bytes;
					modelCache.emplace(placement.nifPath, extraction);
				}
			}

			for (const auto& [reason, count] : extraction->rejections)
				RecordRejection(pub.rejectionReasons, pub.failedShapes, reason, count);
			for (const auto& [blockIdx, reason] : extraction->missing) {
				if (pub.missingShapes.size() >= kMaxMissingShapes) {
					RecordRejection(pub.rejectionReasons, pub.failedShapes, "missing shape manifest capacity");
					break;
				}
				MissingNifShapeDescriptor entry;
				entry.referencePlugin = placement.refPlugin.substr(0, 128);
				entry.referenceLocal = placement.refLocal;
				entry.nifPath = placement.nifPath;
				entry.blockIndex = blockIdx;
				entry.reason = reason.substr(0, 80);
				pub.missingShapes.push_back(std::move(entry));
			}

			for (const auto& modelShape : extraction->shapes) {
				const auto worldAffine = ComposeAffine(placement.transform, modelShape.toModel);
				const auto rows = TransformToRows3x4(worldAffine);
				if (!IsInvertibleTransform3x4(rows)) {
					RecordRejection(pub.rejectionReasons, pub.failedShapes, "invalid placed bounds");
					continue;
				}
				emitShape(
					MakeShapeIdentity(placement.refFormID, modelShape.ordinal),
					modelShape.mesh->positions, modelShape.mesh->indices, rows,
					placement.refPlugin, placement.refLocal, placement.nifPath, int32_t(modelShape.ordinal));
			}
		}

		pub.validated = true;
		pub.enumerationComplete = a_request.enumerationComplete && !a_request.lightCapacityExceeded && !a_request.candidateOverflow;
		if (!pub.enumerationComplete || pub.failedShapes || pub.eligibleShapes != pub.shapes.size()) {
			result.failure = std::format(
				"Partial coverage: {} eligible shapes, {} extracted, {} unsupported; complete enumeration: {}",
				pub.eligibleShapes, pub.shapes.size(), pub.failedShapes,
				pub.enumerationComplete ? "yes" : "no");
		}
		return result;
	}
}
