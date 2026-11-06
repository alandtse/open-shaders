#include "MaterialResources.h"

#include <algorithm>
#include <cctype>
#include <span>
#include <cmath>
#include <cstring>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kReady = 1, kPbr = 2, kModelNormal = 4, kVertexColor = 8, kAlphaTest = 16, kVertexAlpha = 32, kEmission = 64, kSpecular = 128, kOpacityReady = 256;
		constexpr uint32_t kPbrSourceFlag = 1u << 23;

		std::string Normalize(std::string a_path)
		{
			std::ranges::transform(a_path, a_path.begin(), [](unsigned char c) { return c == '/' ? '\\' : char(std::tolower(c)); });
			if (!a_path.empty() && !a_path.starts_with("textures\\")) a_path = "textures\\" + a_path;
			return a_path;
		}

		std::array<uint32_t, 2> Find(const MaterialResources& a_resources, const json& a_material, size_t a_slot)
		{
			const auto& paths = a_material.at("textures");
			if (!paths.is_array() || a_slot >= paths.size() || !paths[a_slot].is_string()) return { kMissingTexture, 0 };
			const auto found = a_resources.locations.find(Normalize(paths[a_slot].get<std::string>()));
			return found == a_resources.locations.end() ? std::array<uint32_t, 2>{ kMissingTexture, 0 } : found->second;
		}

		float4 Color(const json& a_material, const char* a_key, float a_multiplier)
		{
			const auto values = a_material.at(a_key).get<std::vector<float>>();
			if (values.size() != 3 || !std::ranges::all_of(values, [](float value) { return std::isfinite(value); })) throw std::runtime_error("Invalid material color");
			return { values[0], values[1], values[2], a_multiplier };
		}

		MaterialRecord Compile(const MaterialResources& a_resources, const Material& a_source)
		{
			MaterialRecord result;
			if (!a_source.offlineDescription) return result;
			const auto& source = *a_source.offlineDescription;
			const auto flags1 = source.at("shaderFlags1").get<uint32_t>(), flags2 = source.at("shaderFlags2").get<uint32_t>();
			const auto diffuse = Find(a_resources, source, 0), normal = Find(a_resources, source, 1);
			const bool pbr = (flags2 & kPbrSourceFlag) != 0;
			const auto packed = pbr ? Find(a_resources, source, 5) : std::array<uint32_t, 2>{ kMissingTexture, 0 };
			const auto emission = Find(a_resources, source, 2);
			result.colorNormal = { diffuse[0], diffuse[1], normal[0], normal[1] };
			result.packedEmission = { packed[0], packed[1], emission[0], emission[1] };
			const auto uv = source.at("uvOffsetScale").get<std::vector<float>>();
			if (uv.size() != 4 || !std::ranges::all_of(uv, [](float value) { return std::isfinite(value); })) throw std::runtime_error("Invalid material UV transform");
			result.uv = { uv[2], uv[3], uv[0], uv[1] };
			result.emission = Color(source, "emissiveColor", source.at("emissiveMultiple").get<float>());
			result.specular = Color(source, "specularColor", source.at("specularStrength").get<float>());
			const auto alphaFlags = source.at("alphaFlags").is_null() ? 0u : source.at("alphaFlags").get<uint32_t>();
			const auto threshold = source.at("alphaThreshold").is_null() ? 0u : source.at("alphaThreshold").get<uint32_t>();
			result.scalar = { source.at("alpha").get<float>(), float(threshold) / 255.0f, source.at("glossiness").get<float>(), result.specular.w };
			const auto clamp = source.at("textureClampMode").get<uint32_t>();
			const std::array<float, 8> scalars{ result.emission.w, result.specular.w, result.scalar.x, result.scalar.y, result.scalar.z, result.scalar.w, uv[0], uv[1] };
			if (clamp > 3 || threshold > 255 || !std::ranges::all_of(scalars, [](float value) { return std::isfinite(value); })) throw std::runtime_error("Invalid material parameters");
			const bool modelNormal = (flags1 & (1u << 12)) != 0;
			const bool basis = a_source.attributes && (modelNormal || a_source.attributes->hasTangentFrame);
			const bool ready = diffuse[0] != kMissingTexture && normal[0] != kMissingTexture && basis && (!pbr || packed[0] != kMissingTexture);
			uint32_t flags = ready ? kReady : 0;
			if (pbr) flags |= kPbr;
			if (modelNormal) flags |= kModelNormal;
			if (flags2 & (1u << 5)) flags |= kVertexColor;
			if (flags1 & (1u << 3)) flags |= kVertexAlpha;
			if (alphaFlags & (1u << 9)) flags |= kAlphaTest;
			if (diffuse[0] != kMissingTexture && a_source.attributes) flags |= kOpacityReady;
			if (pbr || (flags1 & (1u << 22))) flags |= kEmission;
			if (flags1 & 1u) flags |= kSpecular;
			uint32_t unsupported = 0;
			if (source.at("shaderType") != "Default") unsupported |= 1;
			if (alphaFlags & 1u) unsupported |= 2;
			if (a_source.offlineMaterialFlags == OfflineMaterialFlags::AnimatedMaterial || a_source.offlineMaterialFlags == OfflineMaterialFlags::Unknown) unsupported |= 4;
			if (pbr && (flags2 & ((1u << 12) | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27)))) unsupported |= 8;
			result.flags = { flags, alphaFlags, clamp, unsupported };
			return result;
		}

		template <class T>
		winrt::com_ptr<ID3D12Resource> UploadTable(const GraphicsBackend& a_backend, const std::vector<T>& a_values, uint64_t& a_bytes, const wchar_t* a_name)
		{
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_values.size() * sizeof(T));
			const auto allocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
			if (allocation == UINT64_MAX || allocation > 16ull * 1024 * 1024) throw std::runtime_error("Material table budget exceeded");
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			winrt::com_ptr<ID3D12Resource> buffer;
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.put())));
			buffer->SetName(a_name);
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			DX::ThrowIfFailed(buffer->Map(0, &noRead, &mapped));
			std::memcpy(mapped, a_values.data(), a_values.size() * sizeof(T));
			buffer->Unmap(0, nullptr);
			a_bytes += allocation;
			return buffer;
		}
	}

	std::shared_ptr<const MaterialResources> MaterialResources::Load(const GraphicsBackend& a_backend, const TopLevel& a_scene)
	{
		static_assert(sizeof(MaterialRecord) == 128 && sizeof(SurfaceRecord) == 32);
		auto result = std::make_shared<MaterialResources>();
		if (a_scene.materialSource) {
			try { LoadMaterialTextures(a_backend, *a_scene.materialSource, *result); }
			catch (const MaterialUploadError&) { throw; }
			catch (const std::exception& error) {
				result->failure = error.what();
				result->pools.clear(); result->locations.clear(); result->bytes = result->textures = 0;
			}
		}
		std::vector<MaterialRecord> materials;
		std::vector<SurfaceRecord> surfaces;
		std::unordered_map<std::string, uint32_t> unique;
		for (uint32_t index = 0; index < a_scene.instances.size(); ++index) {
			const auto& instance = a_scene.instances[index];
			const auto attributes = instance.material.attributes;
			const auto key = instance.material.offlineDescription ? instance.material.offlineDescription->dump() + ":" + std::to_string(attributes ? attributes->stride : 0) + ":" + std::to_string(uint32_t(instance.material.offlineMaterialFlags)) : "unavailable";
			const auto [found, inserted] = unique.try_emplace(key, uint32_t(materials.size()));
			if (inserted) {
				MaterialRecord record;
				try { record = Compile(*result, instance.material); }
				catch (const std::exception&) { record.flags[3] = 16; }
				if ((record.flags[0] & kReady) && record.flags[3] == 0) ++result->readyMaterials; else ++result->rejectedMaterials;
				materials.push_back(record);
			}
			const auto& geometry = *a_scene.bottoms[index]->geometry;
			if (geometry.indexOffset > UINT32_MAX || !instance.mesh || instance.indexCount % 3) throw std::runtime_error("Invalid material geometry binding");
			const SurfaceRecord surface{
				{ uint32_t(geometry.indexOffset), instance.firstIndex, instance.mesh->vertexCount, attributes ? attributes->stride : 0 },
				{ found->second, instance.indexCount / 3, instance.material.opaqueCandidate ? 1u : 0u, instance.material.roomIndex }
			};
			surfaces.push_back(surface);
		}
		if (materials.empty() || surfaces.empty()) throw std::runtime_error("Empty material tables");
		result->materials = UploadTable(a_backend, materials, result->tableBytes, L"DX12NativeScene::MaterialTable");
		result->surfaces = UploadTable(a_backend, surfaces, result->tableBytes, L"DX12NativeScene::SurfaceTable");
		struct RoomVolume { std::array<float4, 3> inverse; float4 halfExtents; };
		static_assert(sizeof(RoomVolume) == 64);
		std::vector<RoomVolume> volumes;
		if (a_scene.materialSource && a_scene.materialSource->rooms) {
			result->roomsKnown = true;
			for (const auto& room : *a_scene.materialSource->rooms) {
				const auto inverse = room.at("cellToRoom").get<std::vector<float>>();
				const auto extents = room.at("halfExtents").get<std::vector<float>>();
				if (inverse.size() != 12 || extents.size() != 3 ||
					!std::ranges::all_of(inverse, [](float v) { return std::isfinite(v); }) ||
					!std::ranges::all_of(extents, [](float v) { return std::isfinite(v) && v > 0; })) throw std::runtime_error("Invalid prepared room volume");
				RoomVolume volume{};
				std::memcpy(volume.inverse.data(), inverse.data(), 12 * sizeof(float));
				volume.halfExtents = { extents[0], extents[1], extents[2], 0 };
				volumes.push_back(volume);
			}
		}
		result->roomCount = uint32_t(volumes.size());
		if (volumes.empty()) volumes.push_back({});
		result->roomVolumes = UploadTable(a_backend, volumes, result->tableBytes, L"DX12NativeScene::RoomVolumes");
		return result;
	}
}
