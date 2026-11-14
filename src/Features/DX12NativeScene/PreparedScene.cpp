#include "PreparedScene.h"
#include "CellPackedFile.h"
#include "VideoMemoryBudget.h"

#include "Utils/FileDigest.h"
#include <BS_thread_pool.hpp>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kMaximumInstances = 8192;
		constexpr uint64_t kMaximumManifestBytes = 16ull * 1024 * 1024;

		bool IsDigest(const std::string& a_value)
		{
			return a_value.size() == 64 && std::ranges::all_of(a_value, [](unsigned char c) { return std::isxdigit(c) != 0; });
		}

		winrt::com_ptr<ID3D12Resource> UploadBuffer(const GraphicsBackend& a_backend, std::span<const uint8_t> a_payload,
			uint64_t a_limit, uint64_t& a_allocation, const wchar_t* a_name)
		{
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_payload.size() + 4);
			const auto allocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
			if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > a_limit)
				throw std::runtime_error("Prepared upload budget exceeded");
			a_allocation = allocation.SizeInBytes;
			winrt::com_ptr<ID3D12Resource> buffer;
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.put())));
			buffer->SetName(a_name);
			void* mapped = nullptr;
			const D3D12_RANGE noRead{ 0, 0 };
			DX::ThrowIfFailed(buffer->Map(0, &noRead, &mapped));
			std::memcpy(mapped, a_payload.data(), a_payload.size());
			std::memset(static_cast<uint8_t*>(mapped) + a_payload.size(), 0, 4);
			buffer->Unmap(0, nullptr);
			return buffer;
		}

		std::shared_ptr<const VertexAttributes> UploadShading(const GraphicsBackend& a_backend, std::span<const uint8_t> a_bytes,
			uint32_t a_vertexCount, uint64_t a_limit)
		{
			std::array<uint32_t, 4> header{};
			std::memcpy(header.data(), a_bytes.data(), sizeof(header));
			constexpr uint32_t kLegacyShadingStride = 36, kTangentShadingStride = 60;
			const auto kShadingStride = header[1] == 2 ? kTangentShadingStride : kLegacyShadingStride;
			if (header[0] != 0x41475352 || (header[1] != 1 && header[1] != 2) || header[2] != a_vertexCount || header[3] != kShadingStride ||
				a_bytes.size() != 16 + uint64_t(a_vertexCount) * kShadingStride)
				throw std::runtime_error("Invalid RSGA shading header");
			for (uint64_t offset = 16; offset < a_bytes.size(); offset += 4) {
				float value;
				std::memcpy(&value, a_bytes.data() + offset, 4);
				if (!std::isfinite(value))
					throw std::runtime_error("Non-finite prepared shading value");
			}
			for (uint64_t offset = 16; offset < a_bytes.size(); offset += kShadingStride) {
				float basis[15]{};
				std::memcpy(basis, a_bytes.data() + offset, kShadingStride);
				if (basis[0] == 0 && basis[1] == 0 && basis[2] == 0)
					throw std::runtime_error("Zero prepared shading normal");
				if (header[1] == 2) {
					const auto determinant = (basis[1] * basis[11] - basis[2] * basis[10]) * basis[12] +
					                         (basis[2] * basis[9] - basis[0] * basis[11]) * basis[13] +
					                         (basis[0] * basis[10] - basis[1] * basis[9]) * basis[14];
					if (!std::isfinite(determinant) || determinant == 0)
						throw std::runtime_error("Invalid prepared tangent basis");
				}
			}
			auto shading = std::make_shared<VertexAttributes>();
			shading->vertexCount = a_vertexCount;
			shading->stride = kShadingStride;
			shading->hasTangentFrame = header[1] == 2;
			shading->buffer = UploadBuffer(a_backend, a_bytes.subspan(16), a_limit,
				shading->allocationBytes, L"DX12NativeScene::PreparedShading");
			return shading;
		}

		std::shared_ptr<const Geometry> Upload(const GraphicsBackend& a_backend, std::span<const uint8_t> a_bytes, uint64_t a_limit)
		{
			std::array<uint32_t, 4> header{};
			std::memcpy(header.data(), a_bytes.data(), sizeof(header));
			const auto [magic, version, vertices, indices] = header;
			const uint64_t vertexBytes = uint64_t(vertices) * 12, indexBytes = uint64_t(indices) * 4;
			if (magic != 0x4d475352 || version != 1 || !vertices || !indices || indices % 3 || 16 + vertexBytes + indexBytes != a_bytes.size())
				throw std::runtime_error("Invalid prepared RSGM header");
			for (uint64_t offset = 16; offset < 16 + vertexBytes; offset += 4) {
				float value;
				std::memcpy(&value, a_bytes.data() + offset, 4);
				if (!std::isfinite(value))
					throw std::runtime_error("Non-finite prepared vertex");
			}
			for (uint64_t offset = 16 + vertexBytes; offset < a_bytes.size(); offset += 4) {
				uint32_t index;
				std::memcpy(&index, a_bytes.data() + offset, 4);
				if (index >= vertices)
					throw std::runtime_error("Prepared index exceeds vertex range");
			}
			auto mesh = std::make_shared<Mesh>();
			mesh->stride = 12;
			mesh->vertexCount = vertices;
			auto geometry = std::make_shared<Geometry>();
			geometry->prepared = true;
			geometry->source = std::move(mesh);
			geometry->indexOffset = vertexBytes;
			geometry->validationOffset = vertexBytes + indexBytes;
			geometry->buffer = UploadBuffer(a_backend, a_bytes.subspan(16), a_limit,
				geometry->allocationBytes, L"DX12NativeScene::PreparedGeometry");
			return geometry;
		}

		// DX12 device + verified content hash: weak pointers never pin GPU
		// resources beyond the owning, fenced scene.
		struct SharedPreparedBuffers
		{
			std::mutex mutex;
			std::unordered_map<std::string, std::weak_ptr<const Geometry>> geometry;
			std::unordered_map<std::string, std::weak_ptr<const VertexAttributes>> shading;
		};
		SharedPreparedBuffers& PreparedBufferCache()
		{
			static SharedPreparedBuffers cache;
			return cache;
		}

		// Budget is a scene-wide ceiling: zero selects DXGI automatic mode.
		// Geometry uploads proceed in this owned worker, retaining completed
		// buffers across short-lived DXGI pressure instead of restarting the
		// CELL for every deferred allocation.
		class PreparedUploadAllowance
		{
		public:
			PreparedUploadAllowance(const GraphicsBackend& backend, uint64_t manual, uint64_t& used) : backend(backend), manual(manual), used(used) {}

			uint64_t Required(uint64_t payloadBytes) const
			{
				const auto desc = CD3DX12_RESOURCE_DESC::Buffer(payloadBytes + 4);
				const auto info = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
				if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes)
					throw std::runtime_error("Invalid prepared geometry allocation");
				return info.SizeInBytes;
			}

			uint64_t Available(uint64_t required, bool wait)
			{
				// Reserve room for the production BLAS/TLAS and materials.
				// The DXGI policy itself already retains a device-wide reserve.
				const uint64_t hardCeiling = manual ? manual : UINT64_MAX;
				const uint64_t hardGeometry = hardCeiling == UINT64_MAX ?
				                                  hardCeiling :
				                                  hardCeiling - hardCeiling / 8;
				if (required > hardGeometry || used > hardGeometry - required)
					throw std::runtime_error(
						std::format("Prepared geometry exceeds manual scene budget (used {} MiB, next {} MiB, limit {} MiB)",
							used / VideoMemoryBudget::kMiB, required / VideoMemoryBudget::kMiB,
							hardGeometry / VideoMemoryBudget::kMiB));

				for (unsigned attempt = 0;; ++attempt) {
					const auto dxgi = memory.Query(backend.d3d12Device.get(), used, manual);
					const uint64_t allowable = dxgi.effectiveBytes - dxgi.effectiveBytes / 8;
					if (used <= allowable && required <= allowable - used)
						return allowable - used;
					if (!wait)
						return 0;
					// Lack of an available local-budget query is not a
					// transient condition; do not repeatedly sleep on fallback.
					if (!dxgi.dxgiAvailable || attempt >= 120)
						throw std::runtime_error(
							std::format("Prepared geometry waiting for DXGI headroom (used {} MiB, next {} MiB, allowance {} MiB)",
								used / VideoMemoryBudget::kMiB, required / VideoMemoryBudget::kMiB,
								allowable / VideoMemoryBudget::kMiB));
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
				}
			}

		private:
			const GraphicsBackend& backend;
			VideoMemoryBudget memory;
			uint64_t manual, &used;
		};

		std::shared_ptr<const PreparedCell> Load(const GraphicsBackend& a_backend, const std::filesystem::path& a_directory,
			const std::string& a_cell, uint64_t a_epoch, uint64_t a_budget, uint64_t a_materialBudget,
			const std::function<void(uint64_t, uint64_t)>& progress)
		{
			const auto manifestPath = a_directory / "cell-layout.json";
			if (std::filesystem::file_size(manifestPath) > kMaximumManifestBytes)
				throw std::runtime_error("Prepared manifest exceeds limit");
			std::ifstream file(manifestPath);
			const auto manifest = json::parse(file);
			if (manifest.at("schemaVersion").get<uint32_t>() != 2 || manifest.at("rendererSchemaVersion").get<uint32_t>() != 2 || _stricmp(manifest.at("cell").get_ref<const std::string&>().c_str(), a_cell.c_str()) != 0)
				throw std::runtime_error("Prepared cell identity/schema mismatch");
			const auto& rows = manifest.at("instances");
			if (!rows.is_array() || rows.size() > kMaximumInstances ||
				(rows.empty() && (!manifest.contains("subcell") || manifest.at("subcell").is_null())))
				throw std::runtime_error("Prepared instance count invalid");
			auto result = std::make_shared<PreparedCell>();
			PreparedUploadAllowance uploads(a_backend, a_budget, result->bytes);
			result->digest = manifest.at("layoutDigest").get<std::string>();
			if (!IsDigest(result->digest))
				throw std::runtime_error("Prepared layout digest invalid");
			auto snapshot = std::make_shared<Snapshot>();
			snapshot->generation = snapshot->sweep = a_epoch;
			snapshot->scope.scene = a_epoch;
			snapshot->scope.batch = a_epoch;
			snapshot->truncated = true;
			snapshot->offline = true;
			std::shared_ptr<const json> rooms;
			if (manifest.value("roomSchemaVersion", 0u) == 1) {
				const auto& definitions = manifest.at("rooms");
				if (!definitions.is_array() || definitions.size() > 128)
					throw std::runtime_error("Prepared room count invalid");
				rooms = std::make_shared<const json>(definitions);
				for (const auto& room : definitions) result->roomDefinitions.push_back(room);
			}
			// Keep the prepared material manifest in automatic mode as well.
			// The binding worker will independently recheck DXGI headroom.
			const auto initialMaterials = VideoMemoryBudget{}.Query(
																 a_backend.d3d12Device.get(), 0, a_materialBudget)
			                                  .effectiveBytes;
			if (initialMaterials) {
				auto source = std::make_shared<MaterialSource>(
					a_directory, a_cell, result->digest, initialMaterials, rooms);
				source->textureReferences = std::make_shared<const json>(manifest.at("textureReferences"));
				snapshot->materialSource = std::move(source);
			}
			std::unordered_map<std::string, std::shared_ptr<const Geometry>> geometries;
			std::unordered_map<std::string, std::shared_ptr<const VertexAttributes>> shadingCache;
			std::unordered_map<std::string, std::string> shadingFailures;
			std::unordered_map<std::string, std::shared_ptr<const json>> materials;
			const auto shadingSchema = manifest.value("shadingSchemaVersion", 0u);
			const bool shadingSchemaSupported = shadingSchema == 1 || shadingSchema == 2;
			const auto geometryPack = DX12Packed::Read(a_directory, manifest.at("geometryPack"),
				"cell-geometry.rsgp", DX12Packed::kGeometryMagic);
			const auto shadingPack = DX12Packed::Read(a_directory, manifest.at("shadingPack"),
				"cell-shading.rssp", DX12Packed::kShadingMagic);
			for (const auto& row : rows) {
				const auto hash = row.at("geometrySha256").get<std::string>();
				if (!IsDigest(hash) || row.at("geometryFile").get<std::string>() != "geometry/" + hash + ".rsgm")
					throw std::runtime_error("Prepared geometry path invalid");
				auto found = geometries.find(hash);
				if (found == geometries.end()) {
					const auto cacheKey = std::format("{:p}:{}",
						static_cast<const void*>(a_backend.d3d12Device.get()), hash);
					auto& shared = PreparedBufferCache();
					std::shared_ptr<const Geometry> geometry;
					{
						std::scoped_lock lock(shared.mutex);
						if (auto it = shared.geometry.find(cacheKey); it != shared.geometry.end())
							geometry = it->second.lock();
					}
					if (geometry)
						++result->sharedGeometryHits;
					else {
						const auto bytes = geometryPack.Get(hash);
						if (bytes.size() < 16)
							throw std::runtime_error("Prepared RSGM geometry header truncated");
						const auto allocation = uploads.Required(bytes.size() - 16);
						const auto allowance = uploads.Available(allocation, true);
						geometry = Upload(a_backend, bytes, allowance);
						std::scoped_lock lock(shared.mutex);
						shared.geometry[cacheKey] = geometry;
						if (shared.geometry.size() > 4096)
							std::erase_if(shared.geometry, [](const auto& item) { return item.second.expired(); });
					}
					result->bytes += geometry->allocationBytes;
					result->geometries.emplace(geometry->source.get(), geometry);
					found = geometries.emplace(hash, std::move(geometry)).first;
					if (progress)
						progress(geometries.size(), result->bytes);
				}
				Instance instance;
				instance.identity = snapshot->instances.size() + 1;
				if (row.contains("referenceKey") && row.at("referenceKey").is_string() &&
					row.contains("model") && row.at("model").is_string() &&
					row.contains("shapeName") && row.at("shapeName").is_string() &&
					row.contains("sourceSha256") && row.at("sourceSha256").is_string() &&
					row.contains("blockIndex") && row.at("blockIndex").is_number_unsigned()) {
					auto reference = row.at("referenceKey").get<std::string>();
					auto model = row.at("model").get<std::string>();
					auto shape = row.at("shapeName").get<std::string>();
					auto digest = row.at("sourceSha256").get<std::string>();
					const auto block = row.at("blockIndex").get<uint64_t>();
					if (!reference.empty() && reference.size() <= 256 && !model.empty() && model.size() <= 1024 &&
						!shape.empty() && shape.size() <= 1024 && IsDigest(digest) && block < UINT32_MAX)
						instance.preparedIdentity = { std::move(reference), std::move(model), std::move(shape), std::move(digest), uint32_t(block) };
				}
				instance.mesh = found->second->source;
				instance.indexCount = row.at("indexCount").get<uint32_t>();
				if (uint64_t(instance.indexCount) * 4 != found->second->validationOffset - found->second->indexOffset)
					throw std::runtime_error("Prepared instance range mismatch");
				const auto matrix = row.at("shapeToCell").get<std::vector<float>>();
				if (matrix.size() != 12 || !std::ranges::all_of(matrix, [](float v) { return std::isfinite(v); }))
					throw std::runtime_error("Prepared instance transform invalid");
				const double determinant = double(matrix[0]) * (double(matrix[5]) * matrix[10] - double(matrix[6]) * matrix[9]) -
				                           double(matrix[1]) * (double(matrix[4]) * matrix[10] - double(matrix[6]) * matrix[8]) +
				                           double(matrix[2]) * (double(matrix[4]) * matrix[9] - double(matrix[5]) * matrix[8]);
				if (!std::isfinite(determinant) || determinant == 0)
					throw std::runtime_error("Prepared instance transform is singular");
				std::memcpy(instance.world.m, matrix.data(), sizeof(instance.world));
				const auto bounds = row.at("cellBounds").get<std::vector<float>>();
				if (bounds.size() != 6 || !std::ranges::all_of(bounds, [](float v) { return std::isfinite(v); }))
					throw std::runtime_error("Prepared instance bounds invalid");
				for (size_t axis = 0; axis < 3; ++axis)
					if (bounds[axis] > bounds[axis + 3])
						throw std::runtime_error("Prepared instance bounds reversed");
				instance.bound = { (bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f,
					std::max({ bounds[3] - bounds[0], bounds[4] - bounds[1], bounds[5] - bounds[2] }) * 0.5f };
				instance.material.opaqueCandidate = row.at("opaqueOccluderCandidate").get<bool>();
				if (rooms && row.contains("roomReferenceKey")) {
					if (row.at("roomReferenceKey").is_null())
						instance.material.roomIndex = 0;
					else if (row.at("roomReferenceKey").is_string()) {
						for (uint32_t room = 0; room < rooms->size(); ++room)
							if (rooms->at(room).at("referenceKey") == row.at("roomReferenceKey"))
								instance.material.roomIndex = room + 1;
					}
				}
				const auto status = row.at("geometryStatus").get<std::string>();
				if (status != "compiled") {
					instance.material.opaqueCandidate = false;
					if (status == "preview-only:alpha-test-or-blend")
						instance.material.offlineMaterialFlags = OfflineMaterialFlags::AlphaCoverage;
					else if (status == "preview-only:vertex-or-palette-alpha")
						instance.material.offlineMaterialFlags = OfflineMaterialFlags::VertexAlpha;
					else if (status == "preview-only:controlled-material")
						instance.material.offlineMaterialFlags = OfflineMaterialFlags::AnimatedMaterial;
					else
						instance.material.offlineMaterialFlags = OfflineMaterialFlags::Unknown;
				}
				instance.material.twoSided = row.at("twoSided").get<bool>();
				instance.material.shadingStatus = row.value("shadingStatus", std::string("absent:legacy-package"));
				if (instance.material.shadingStatus == "compiled" && !shadingSchemaSupported)
					instance.material.shadingStatus = "unsupported:shading-schema";
				if (instance.material.shadingStatus == "compiled") {
					const auto shadingHash = row.contains("shadingSha256") && row.at("shadingSha256").is_string() ? row.at("shadingSha256").get<std::string>() : "";
					const auto shadingFile = row.contains("shadingFile") && row.at("shadingFile").is_string() ? row.at("shadingFile").get<std::string>() : "";
					if (IsDigest(shadingHash) && shadingFile == "shading/" + shadingHash + ".rsga") {
						if (!shadingCache.contains(shadingHash) && !shadingFailures.contains(shadingHash)) {
							try {
								const auto key = std::format("{:p}:{}:{}",
									static_cast<const void*>(a_backend.d3d12Device.get()),
									shadingHash, instance.mesh->vertexCount);
								auto& shared = PreparedBufferCache();
								std::shared_ptr<const VertexAttributes> shading;
								{
									std::scoped_lock lock(shared.mutex);
									if (auto it = shared.shading.find(key); it != shared.shading.end())
										shading = it->second.lock();
								}
								if (shading)
									++result->sharedShadingHits;
								else {
									const auto bytes = shadingPack.Get(shadingHash);
									if (bytes.size() < 16)
										throw std::runtime_error("Prepared RSGA shading header truncated");
									const auto required = uploads.Required(bytes.size() - 16);
									const auto allowance = uploads.Available(required, false);
									if (!allowance)
										throw std::runtime_error("deferred:prepared-shading-headroom");
									shading = UploadShading(a_backend, bytes, instance.mesh->vertexCount, allowance);
									std::scoped_lock lock(shared.mutex);
									shared.shading[key] = shading;
									if (shared.shading.size() > 4096)
										std::erase_if(shared.shading, [](const auto& item) { return item.second.expired(); });
								}
								result->bytes += shading->allocationBytes;
								shadingCache.emplace(shadingHash, std::move(shading));
							} catch (const std::exception& error) {
								shadingFailures.emplace(shadingHash, error.what());
							}
						}
						if (const auto attributes = shadingCache.find(shadingHash); attributes != shadingCache.end() && attributes->second->vertexCount == instance.mesh->vertexCount) {
							instance.material.attributes = attributes->second;
							++result->shadedInstances;
							if (attributes->second->hasTangentFrame)
								++result->tangentInstances;
						} else
							instance.material.shadingStatus = shadingFailures.contains(shadingHash) ? "invalid:" + shadingFailures.at(shadingHash) : "invalid:vertex-count";
					} else
						instance.material.shadingStatus = "invalid:shading-path";
				}
				if (row.contains("material") && row.at("material").is_object()) {
					const auto materialKey = row.at("material").dump();
					auto [materialEntry, inserted] = materials.try_emplace(materialKey, nullptr);
					if (inserted)
						materialEntry->second = std::make_shared<const json>(row.at("material"));
					instance.material.offlineDescription = materialEntry->second;
				}
				if (!instance.material.opaqueCandidate)
					++result->maskedInstances;
				snapshot->instances.push_back(std::move(instance));
			}
			for (const auto& light : manifest.at("lights")) {
				if (result->lights.size() >= 256)
					throw std::runtime_error("Prepared light count exceeds limit");
				if (light.at("classification").get<std::string>() != "placed-light-baseline")
					continue;
				const auto position = light.at("position").get<std::vector<float>>();
				if (position.size() != 3 || !std::ranges::all_of(position, [](float v) { return std::isfinite(v); }))
					continue;
				result->lights.push_back({ position[0], position[1], position[2] });
				result->lightDefinitions.push_back(light);
				PreparedPointLight native{};
				native.position = { position[0], position[1], position[2] };
				// Temporary inverse-square baseline: advanced source flags and
				// reference overrides are not modeled yet, but do not suppress the
				// point-light approximation when its fundamental data is valid.
				std::string eligibility = "invalid-baseline";
				try {
					const auto& data = light.at("baseline");
					const auto rgb = data.at("color").get<std::vector<int>>();
					const auto flags = data.value("flags", std::string{});
					const float radius = data.at("radius").get<float>();
					const float fade = data.value("fadeValue", 1.0f);
					const bool disabled = flags.find("Disabled") != std::string::npos;
					const bool approximate = light.value("hasReferenceOverrides", false) ||
					                         flags.find("PortalStrict") != std::string::npos ||
					                         flags.find("Spot") != std::string::npos ||
					                         flags.find("Particle") != std::string::npos ||
					                         flags.find("Flicker") != std::string::npos || flags.find("Pulse") != std::string::npos;
					if (disabled)
						eligibility = "disabled";
					else if (rgb.size() != 3 || !std::isfinite(radius) || !std::isfinite(fade) ||
							 radius <= 0 || fade <= 0 || radius > 60000.0f ||
							 !std::ranges::all_of(rgb, [](int c) { return c >= 0 && c <= 255; }))
						eligibility = "invalid-baseline";
					else {
						const auto srgb = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
						const float scale = fade * radius * radius;
						native.radiance = { srgb(float(rgb[0]) / 255.0f) * scale,
							srgb(float(rgb[1]) / 255.0f) * scale, srgb(float(rgb[2]) / 255.0f) * scale };
						native.radius = radius;
						native.sourceRadius = std::max(0.5f, radius * 0.025f);
						native.supported = true;
						eligibility = approximate ? "approximated" : "supported";
					}
				} catch (const json::exception&) {
					eligibility = "invalid-baseline";
				}
				result->lightEligibility.push_back(std::move(eligibility));
				result->nativeLights.push_back(native);
			}
			result->snapshot = std::move(snapshot);
			return result;
		}
	}

	struct PreparedScene::State
	{
		std::atomic<bool> busy{ false };
		uint64_t completedRevision = 0;
		std::atomic<uint64_t> uploadedMeshes{ 0 }, uploadedBytes{ 0 };
		bool retryable = false;
		uint32_t budgetRetries = 0;
		std::chrono::steady_clock::time_point retryAfter{};
		std::shared_ptr<const PreparedCell> cell;
		std::string failure;
	};

	bool PreparedScene::Requested() const
	{
		if (!requested)
			return false;
		if (!state || state->busy.load(std::memory_order_acquire) ||
			state->completedRevision != revision || !state->retryable)
			return true;
		return std::chrono::steady_clock::now() < state->retryAfter;
	}

	void PreparedScene::Reset()
	{
		++revision;
		requested = false;
	}

	void PreparedScene::Request(const GraphicsBackend& a_backend, std::filesystem::path a_directory, std::string a_cell, uint64_t a_epoch, uint64_t a_budget, uint64_t a_materialBudget)
	{
		if (!state)
			state = std::make_shared<State>();
		if (Requested() || state->busy.exchange(true, std::memory_order_acq_rel))
			return;
		state->uploadedMeshes.store(0, std::memory_order_relaxed);
		state->uploadedBytes.store(0, std::memory_order_relaxed);
		try {
			static BS::thread_pool<> pool{ 1 };
			pool.detach_task([owned = state, backend = a_backend, directory = std::move(a_directory), cell = std::move(a_cell),
								 epoch = a_epoch, budget = a_budget, materialBudget = a_materialBudget, request = revision] {
				std::shared_ptr<const PreparedCell> loaded;
				std::string failure;
				try {
					loaded = Load(backend, directory, cell, epoch, budget, materialBudget,
						[owned](uint64_t meshes, uint64_t bytes) {
							owned->uploadedMeshes.store(meshes, std::memory_order_release);
							owned->uploadedBytes.store(bytes, std::memory_order_release);
						});
				} catch (const std::exception& error) {
					failure = error.what();
				}
				const bool transient = failure.starts_with("Prepared geometry waiting for DXGI headroom");
				if (transient)
					++owned->budgetRetries;
				else
					owned->budgetRetries = 0;
				// Persistent shortage is not solved by endless reloads.
				owned->retryable = transient && owned->budgetRetries <= 3;
				owned->retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(3);
				owned->cell = std::move(loaded);
				owned->failure = std::move(failure);
				owned->completedRevision = request;
				owned->busy.store(false, std::memory_order_release);
			});
			requested = true;
		} catch (const std::exception& error) {
			state->failure = error.what();
			state->completedRevision = revision;
			state->busy.store(false, std::memory_order_release);
			requested = true;
		}
	}

	std::shared_ptr<const PreparedCell> PreparedScene::GetCell() const
	{
		return state && !state->busy.load(std::memory_order_acquire) && state->completedRevision == revision ? state->cell : nullptr;
	}

	json PreparedScene::GetDiagnostics() const
	{
		const bool busy = state && state->busy.load(std::memory_order_acquire);
		const auto cell = GetCell();
		uint32_t supportedLights = 0, approximatedLights = 0, invalidLights = 0, disabledLights = 0;
		if (cell)
			for (const auto& eligibility : cell->lightEligibility) {
				if (eligibility == "supported" || eligibility == "approximated")
					++supportedLights;
				if (eligibility == "approximated")
					++approximatedLights;
				if (eligibility == "invalid-baseline")
					++invalidLights;
				if (eligibility == "disabled")
					++disabledLights;
			}
		return { { "loading", busy }, { "loaded", bool(cell) }, { "requested", requested },
			{ "budgetRetryPending", state && !busy && state->completedRevision == revision && state->retryable },
			{ "budgetRetryCount", state ? state->budgetRetries : 0 },
			{ "uploadProgressMeshes", state ? state->uploadedMeshes.load(std::memory_order_acquire) : 0 },
			{ "uploadProgressBytes", state ? state->uploadedBytes.load(std::memory_order_acquire) : 0 },
			{ "lightCount", cell ? cell->lights.size() : 0 },
			{ "supportedLightCount", supportedLights },
			{ "approximatedLightCount", approximatedLights }, { "invalidLightCount", invalidLights }, { "disabledLightCount", disabledLights },
			{ "lightEligibility", cell ? cell->lightEligibility : std::vector<std::string>{} },
			{ "unsupportedLightCount", cell ? cell->nativeLights.size() - supportedLights : 0 },
			{ "failure", state && !busy && state->completedRevision == revision ? state->failure : "" },
			{ "geometryBytes", cell ? cell->bytes : 0 }, { "meshes", cell ? cell->geometries.size() : 0 },
			{ "sharedGeometryHits", cell ? cell->sharedGeometryHits : 0u },
			{ "sharedShadingHits", cell ? cell->sharedShadingHits : 0u },
			{ "instances", cell ? cell->snapshot->instances.size() : 0 }, { "maskedInstances", cell ? cell->maskedInstances : 0 },
			{ "shadedInstances", cell ? cell->shadedInstances : 0 },
			{ "tangentInstances", cell ? cell->tangentInstances : 0 },
			{ "layoutDigest", cell ? cell->digest : "" }, { "runtimeCoverageValid", false }, { "pluginStateValidated", false } };
	}
}
