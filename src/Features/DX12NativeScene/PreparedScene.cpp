#include "PreparedScene.h"

#include "Utils/FileDigest.h"
#include <BS_thread_pool.hpp>
#include <cctype>
#include <cstring>
#include <span>
#include <fstream>
#include <cmath>

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

		std::vector<uint8_t> ReadMesh(const std::filesystem::path& a_path, const std::string& a_digest, uint64_t a_limit)
		{
			const auto file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Prepared mesh cannot be opened");
			const SKSE::stl::scope_exit close([&]() noexcept { CloseHandle(file); });
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size) || size.QuadPart < 16 || uint64_t(size.QuadPart) > a_limit || uint64_t(size.QuadPart) > UINT32_MAX)
				throw std::runtime_error("Prepared mesh exceeds byte budget");
			const auto digest = Util::FileDigest::Sha256HandleHex(file);
			if (!digest || _stricmp(digest->c_str(), a_digest.c_str()) != 0) throw std::runtime_error("Prepared mesh hash mismatch");
			if (!SetFilePointerEx(file, {}, nullptr, FILE_BEGIN)) throw std::runtime_error("Prepared mesh seek failed");
			std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
				throw std::runtime_error("Prepared mesh read failed");
			return bytes;
		}

		winrt::com_ptr<ID3D12Resource> UploadBuffer(const GraphicsBackend& a_backend, std::span<const uint8_t> a_payload,
			uint64_t a_limit, uint64_t& a_allocation, const wchar_t* a_name)
		{
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_payload.size() + 4);
			const auto allocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
			if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > a_limit) throw std::runtime_error("Prepared upload budget exceeded");
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

		std::shared_ptr<const VertexAttributes> UploadShading(const GraphicsBackend& a_backend, const std::vector<uint8_t>& a_bytes,
			uint32_t a_vertexCount, uint64_t a_limit)
		{
			std::array<uint32_t, 4> header{};
			std::memcpy(header.data(), a_bytes.data(), sizeof(header));
			constexpr uint32_t kLegacyShadingStride = 36, kTangentShadingStride = 60;
			const auto kShadingStride = header[1] == 2 ? kTangentShadingStride : kLegacyShadingStride;
			if (header[0] != 0x41475352 || (header[1] != 1 && header[1] != 2) || header[2] != a_vertexCount || header[3] != kShadingStride ||
				a_bytes.size() != 16 + uint64_t(a_vertexCount) * kShadingStride) throw std::runtime_error("Invalid RSGA shading header");
			for (uint64_t offset = 16; offset < a_bytes.size(); offset += 4) {
				float value;
				std::memcpy(&value, a_bytes.data() + offset, 4);
				if (!std::isfinite(value)) throw std::runtime_error("Non-finite prepared shading value");
			}
			for (uint64_t offset = 16; offset < a_bytes.size(); offset += kShadingStride) {
				float basis[15]{};
				std::memcpy(basis, a_bytes.data() + offset, kShadingStride);
				if (basis[0] == 0 && basis[1] == 0 && basis[2] == 0) throw std::runtime_error("Zero prepared shading normal");
				if (header[1] == 2) {
					const auto determinant = (basis[1] * basis[11] - basis[2] * basis[10]) * basis[12] +
						(basis[2] * basis[9] - basis[0] * basis[11]) * basis[13] +
						(basis[0] * basis[10] - basis[1] * basis[9]) * basis[14];
					if (!std::isfinite(determinant) || determinant == 0) throw std::runtime_error("Invalid prepared tangent basis");
				}
			}
			auto shading = std::make_shared<VertexAttributes>();
			shading->vertexCount = a_vertexCount;
			shading->stride = kShadingStride;
			shading->hasTangentFrame = header[1] == 2;
			shading->buffer = UploadBuffer(a_backend, std::span(a_bytes).subspan(16), a_limit,
				shading->allocationBytes, L"DX12NativeScene::PreparedShading");
			return shading;
		}

		std::shared_ptr<const Geometry> Upload(const GraphicsBackend& a_backend, const std::vector<uint8_t>& a_bytes, uint64_t a_limit)
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
				if (!std::isfinite(value)) throw std::runtime_error("Non-finite prepared vertex");
			}
			for (uint64_t offset = 16 + vertexBytes; offset < a_bytes.size(); offset += 4) {
				uint32_t index;
				std::memcpy(&index, a_bytes.data() + offset, 4);
				if (index >= vertices) throw std::runtime_error("Prepared index exceeds vertex range");
			}
			auto mesh = std::make_shared<Mesh>();
			mesh->stride = 12;
			mesh->vertexCount = vertices;
			auto geometry = std::make_shared<Geometry>();
			geometry->prepared = true;
			geometry->source = std::move(mesh);
			geometry->indexOffset = vertexBytes;
			geometry->validationOffset = vertexBytes + indexBytes;
			geometry->buffer = UploadBuffer(a_backend, std::span(a_bytes).subspan(16), a_limit,
				geometry->allocationBytes, L"DX12NativeScene::PreparedGeometry");
			return geometry;
		}

		std::shared_ptr<const PreparedCell> Load(const GraphicsBackend& a_backend, const std::filesystem::path& a_directory,
			const std::string& a_cell, uint64_t a_epoch, uint64_t a_budget, uint64_t a_materialBudget)
		{
			const auto manifestPath = a_directory / "cell-layout.json";
			if (std::filesystem::file_size(manifestPath) > kMaximumManifestBytes) throw std::runtime_error("Prepared manifest exceeds limit");
			std::ifstream file(manifestPath);
			const auto manifest = json::parse(file);
			if (manifest.at("schemaVersion").get<uint32_t>() != 1 || manifest.at("rendererSchemaVersion").get<uint32_t>() != 1 || _stricmp(manifest.at("cell").get_ref<const std::string&>().c_str(), a_cell.c_str()) != 0)
				throw std::runtime_error("Prepared cell identity/schema mismatch");
			const auto& rows = manifest.at("instances");
			if (!rows.is_array() || rows.empty() || rows.size() > kMaximumInstances) throw std::runtime_error("Prepared instance count invalid");
			auto result = std::make_shared<PreparedCell>();
			result->digest = manifest.at("layoutDigest").get<std::string>();
			if (!IsDigest(result->digest)) throw std::runtime_error("Prepared layout digest invalid");
			auto snapshot = std::make_shared<Snapshot>();
			snapshot->generation = snapshot->sweep = a_epoch;
			snapshot->scope.scene = a_epoch;
			snapshot->scope.batch = a_epoch;
			snapshot->truncated = true;
			snapshot->offline = true;
			std::shared_ptr<const json> rooms;
			if (manifest.value("roomSchemaVersion", 0u) == 1) {
				const auto& definitions = manifest.at("rooms");
				if (!definitions.is_array() || definitions.size() > 128) throw std::runtime_error("Prepared room count invalid");
				rooms = std::make_shared<const json>(definitions);
				for (const auto& room : definitions) result->roomDefinitions.push_back(room);
			}
			if (a_materialBudget) snapshot->materialSource = std::make_shared<const MaterialSource>(a_directory, a_cell, result->digest, a_materialBudget, rooms);
			std::unordered_map<std::string, std::shared_ptr<const Geometry>> geometries;
			std::unordered_map<std::string, std::shared_ptr<const VertexAttributes>> shadingCache;
			std::unordered_map<std::string, std::string> shadingFailures;
			std::unordered_map<std::string, std::shared_ptr<const json>> materials;
			const auto shadingSchema = manifest.value("shadingSchemaVersion", 0u);
			const bool shadingSchemaSupported = shadingSchema == 1 || shadingSchema == 2;
			for (const auto& row : rows) {
				const auto hash = row.at("geometrySha256").get<std::string>();
				if (!IsDigest(hash) || row.at("geometryFile").get<std::string>() != "geometry/" + hash + ".rsgm")
					throw std::runtime_error("Prepared geometry path invalid");
				auto found = geometries.find(hash);
				if (found == geometries.end()) {
					if (result->bytes >= a_budget) throw std::runtime_error("Prepared package budget exhausted");
					const auto bytes = ReadMesh(a_directory / "geometry" / (hash + ".rsgm"), hash, a_budget - result->bytes);
					auto geometry = Upload(a_backend, bytes, a_budget - result->bytes);
					result->bytes += geometry->allocationBytes;
					result->geometries.emplace(geometry->source.get(), geometry);
					found = geometries.emplace(hash, std::move(geometry)).first;
				}
				Instance instance;
				instance.identity = snapshot->instances.size() + 1;
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
				if (!std::isfinite(determinant) || determinant == 0) throw std::runtime_error("Prepared instance transform is singular");
				std::memcpy(instance.world.m, matrix.data(), sizeof(instance.world));
				const auto bounds = row.at("cellBounds").get<std::vector<float>>();
				if (bounds.size() != 6 || !std::ranges::all_of(bounds, [](float v) { return std::isfinite(v); }))
					throw std::runtime_error("Prepared instance bounds invalid");
				for (size_t axis = 0; axis < 3; ++axis)
					if (bounds[axis] > bounds[axis + 3]) throw std::runtime_error("Prepared instance bounds reversed");
				instance.bound = { (bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f,
					std::max({ bounds[3] - bounds[0], bounds[4] - bounds[1], bounds[5] - bounds[2] }) * 0.5f };
				instance.material.opaqueCandidate = row.at("opaqueOccluderCandidate").get<bool>();
				if (rooms && row.contains("roomReferenceKey")) {
					if (row.at("roomReferenceKey").is_null()) instance.material.roomIndex = 0;
					else if (row.at("roomReferenceKey").is_string()) {
						for (uint32_t room = 0; room < rooms->size(); ++room)
							if (rooms->at(room).at("referenceKey") == row.at("roomReferenceKey")) instance.material.roomIndex = room + 1;
					}
				}
				const auto status = row.at("geometryStatus").get<std::string>();
				if (status != "compiled") {
					instance.material.opaqueCandidate = false;
					if (status == "preview-only:alpha-test-or-blend") instance.material.offlineMaterialFlags = OfflineMaterialFlags::AlphaCoverage;
					else if (status == "preview-only:vertex-or-palette-alpha") instance.material.offlineMaterialFlags = OfflineMaterialFlags::VertexAlpha;
					else if (status == "preview-only:controlled-material") instance.material.offlineMaterialFlags = OfflineMaterialFlags::AnimatedMaterial;
					else instance.material.offlineMaterialFlags = OfflineMaterialFlags::Unknown;
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
								const auto bytes = ReadMesh(a_directory / "shading" / (shadingHash + ".rsga"), shadingHash, a_budget - result->bytes);
								auto shading = UploadShading(a_backend, bytes, instance.mesh->vertexCount, a_budget - result->bytes);
								result->bytes += shading->allocationBytes;
								shadingCache.emplace(shadingHash, std::move(shading));
							} catch (const std::exception& error) { shadingFailures.emplace(shadingHash, error.what()); }
						}
						if (const auto attributes = shadingCache.find(shadingHash); attributes != shadingCache.end() && attributes->second->vertexCount == instance.mesh->vertexCount) {
							instance.material.attributes = attributes->second;
							++result->shadedInstances;
							if (attributes->second->hasTangentFrame) ++result->tangentInstances;
						} else instance.material.shadingStatus = shadingFailures.contains(shadingHash) ? "invalid:" + shadingFailures.at(shadingHash) : "invalid:vertex-count";
					} else instance.material.shadingStatus = "invalid:shading-path";
				}
				if (row.contains("material") && row.at("material").is_object()) {
					const auto materialKey = row.at("material").dump();
					auto [materialEntry, inserted] = materials.try_emplace(materialKey, nullptr);
					if (inserted) materialEntry->second = std::make_shared<const json>(row.at("material"));
					instance.material.offlineDescription = materialEntry->second;
				}
				if (!instance.material.opaqueCandidate) ++result->maskedInstances;
				snapshot->instances.push_back(std::move(instance));
			}
			for (const auto& light : manifest.at("lights")) {
				if (result->lights.size() >= 256) throw std::runtime_error("Prepared light count exceeds limit");
				if (light.at("classification").get<std::string>() != "placed-light-baseline") continue;
				const auto position = light.at("position").get<std::vector<float>>();
				if (position.size() != 3 || !std::ranges::all_of(position, [](float v) { return std::isfinite(v); })) continue;
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
					if (disabled) eligibility = "disabled";
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
				} catch (const json::exception&) { eligibility = "invalid-baseline"; }
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
		std::shared_ptr<const PreparedCell> cell;
		std::string failure;
	};

	void PreparedScene::Reset()
	{
		++revision;
		requested = false;
	}

	void PreparedScene::Request(const GraphicsBackend& a_backend, std::filesystem::path a_directory, std::string a_cell, uint64_t a_epoch, uint64_t a_budget, uint64_t a_materialBudget)
	{
		if (!state) state = std::make_shared<State>();
		if (requested || state->busy.exchange(true, std::memory_order_acq_rel)) return;
		try {
			static BS::thread_pool<> pool{ 1 };
			pool.detach_task([owned = state, backend = a_backend, directory = std::move(a_directory), cell = std::move(a_cell),
				epoch = a_epoch, budget = a_budget, materialBudget = a_materialBudget, request = revision] {
				std::shared_ptr<const PreparedCell> loaded;
				std::string failure;
				try { loaded = Load(backend, directory, cell, epoch, budget, materialBudget); }
				catch (const std::exception& error) { failure = error.what(); }
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
				if (eligibility == "supported" || eligibility == "approximated") ++supportedLights;
				if (eligibility == "approximated") ++approximatedLights;
				if (eligibility == "invalid-baseline") ++invalidLights;
				if (eligibility == "disabled") ++disabledLights;
			}
		return { { "loading", busy }, { "loaded", bool(cell) }, { "requested", requested },
			{ "lightCount", cell ? cell->lights.size() : 0 },
			{ "supportedLightCount", supportedLights },
			{ "approximatedLightCount", approximatedLights }, { "invalidLightCount", invalidLights }, { "disabledLightCount", disabledLights },
			{ "lightEligibility", cell ? cell->lightEligibility : std::vector<std::string>{} },
			{ "unsupportedLightCount", cell ? cell->nativeLights.size() - supportedLights : 0 },
			{ "failure", state && !busy && state->completedRevision == revision ? state->failure : "" },
			{ "geometryBytes", cell ? cell->bytes : 0 }, { "meshes", cell ? cell->geometries.size() : 0 },
			{ "instances", cell ? cell->snapshot->instances.size() : 0 }, { "maskedInstances", cell ? cell->maskedInstances : 0 },
			{ "shadedInstances", cell ? cell->shadedInstances : 0 },
			{ "tangentInstances", cell ? cell->tangentInstances : 0 },
			{ "layoutDigest", cell ? cell->digest : "" }, { "runtimeCoverageValid", false }, { "pluginStateValidated", false } };
	}
}
