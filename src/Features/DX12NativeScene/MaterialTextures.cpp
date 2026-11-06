#include "MaterialResources.h"

#include "Utils/FileDigest.h"
#include <DirectXTex.h>
#include <fstream>
#include <map>
#include <tuple>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace DX12Native
{
	namespace
	{
		constexpr uint64_t kMaximumDdsBytes = 128ull * 1024 * 1024;
		constexpr uint32_t kPageSlices = 8, kUploadTimeoutMS = 5000;
		struct Texture
		{
			std::string hash;
			std::vector<std::string> paths;
			DirectX::ScratchImage image;
		};

		std::vector<uint8_t> ReadTexture(const std::filesystem::path& a_directory, const std::string& a_hash)
		{
			if (a_hash.size() != 64 || !std::ranges::all_of(a_hash, [](unsigned char c) { return std::isxdigit(c) != 0; }))
				throw std::runtime_error("Invalid material texture digest");
			const auto path = a_directory / "textures" / (a_hash + ".dds");
			const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Material texture unavailable");
			const SKSE::stl::scope_exit close([&]() noexcept { CloseHandle(file); });
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size) || size.QuadPart < 128 || uint64_t(size.QuadPart) > kMaximumDdsBytes)
				throw std::runtime_error("Material DDS byte limit exceeded");
			const auto hash = Util::FileDigest::Sha256HandleHex(file);
			if (!hash || _stricmp(hash->c_str(), a_hash.c_str()) != 0) throw std::runtime_error("Material DDS digest mismatch");
			if (!SetFilePointerEx(file, {}, nullptr, FILE_BEGIN)) throw std::runtime_error("Material DDS seek failed");
			std::vector<uint8_t> bytes(size_t(size.QuadPart));
			DWORD read = 0;
			if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size()) throw std::runtime_error("Material DDS read failed");
			return bytes;
		}

		struct Upload
		{
			winrt::com_ptr<ID3D12Resource> target, staging;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12Fence> fence;
			winrt::handle event;
			bool submitted = false;
			Upload() = default;
			Upload(Upload&&) = default;
			~Upload()
			{
				if (submitted && fence && fence->GetCompletedValue() < 1) (void)new Upload(std::move(*this));
			}
		};

		void ValidateMetadata(const DirectX::TexMetadata& a_meta, uint64_t a_budget)
		{
			if (a_meta.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || a_meta.arraySize != 1 || a_meta.IsCubemap() || !a_meta.width || !a_meta.height ||
				a_meta.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_meta.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !a_meta.mipLevels || a_meta.mipLevels > 15)
				throw std::runtime_error("Unsupported material texture shape");
			uint64_t bytes = 0;
			for (size_t mip = 0; mip < a_meta.mipLevels; ++mip) {
				size_t rowPitch = 0, slicePitch = 0;
				DX::ThrowIfFailed(DirectX::ComputePitch(a_meta.format, std::max(size_t(1), a_meta.width >> mip), std::max(size_t(1), a_meta.height >> mip), rowPitch, slicePitch));
				if (slicePitch > kMaximumDdsBytes || bytes > kMaximumDdsBytes - slicePitch) throw std::runtime_error("Decoded material DDS exceeds limit");
				bytes += slicePitch;
			}
			if (bytes > a_budget) throw std::runtime_error("Decoded material DDS exceeds budget");
		}

		void UploadPage(const GraphicsBackend& a_backend, const std::vector<Texture*>& a_textures, MaterialResources& a_result, uint64_t a_budget)
		{
			const auto& meta = a_textures.front()->image.GetMetadata();
			const auto format = DirectX::MakeLinear(meta.format);
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DirectX::MakeTypeless(format), meta.width, uint32_t(meta.height), uint16_t(a_textures.size()), uint16_t(meta.mipLevels));
			const auto allocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
			const auto subresources = uint32_t(a_textures.size() * meta.mipLevels);
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
			std::vector<uint32_t> rows(subresources);
			std::vector<uint64_t> rowBytes(subresources);
			uint64_t uploadBytes = 0;
			a_backend.d3d12Device->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(), rowBytes.data(), &uploadBytes);
			const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBytes);
			const auto stagingAllocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &stagingDesc).SizeInBytes;
			if (allocation == UINT64_MAX || stagingAllocation == UINT64_MAX || allocation > a_budget || stagingAllocation > a_budget - allocation ||
				a_result.bytes > a_budget - allocation - stagingAllocation) throw std::runtime_error("Material texture/staging budget exceeded");
			Upload upload;
			const auto deviceHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			const auto stagingHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&deviceHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(upload.target.put())));
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&stagingHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload.staging.put())));
			upload.target->SetName(L"DX12NativeScene::MaterialTexturePool");
			upload.staging->SetName(L"DX12NativeScene::MaterialTextureStaging");
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			DX::ThrowIfFailed(upload.staging->Map(0, &noRead, &mapped));
			for (uint32_t subresource = 0; subresource < subresources; ++subresource) {
				const auto* image = a_textures[subresource / meta.mipLevels]->image.GetImage(subresource % meta.mipLevels, 0, 0);
				if (!image || image->rowPitch < rowBytes[subresource] || image->slicePitch < image->rowPitch * rows[subresource]) {
					upload.staging->Unmap(0, nullptr);
					throw std::runtime_error("Material DDS subresource layout mismatch");
				}
				for (uint32_t row = 0; row < rows[subresource]; ++row)
					std::memcpy(static_cast<uint8_t*>(mapped) + footprints[subresource].Offset + uint64_t(row) * footprints[subresource].Footprint.RowPitch,
						image->pixels + uint64_t(row) * image->rowPitch, size_t(rowBytes[subresource]));
			}
			upload.staging->Unmap(0, nullptr);
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(upload.allocator.put())));
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload.allocator.get(), nullptr, IID_PPV_ARGS(upload.commands.put())));
			upload.commands->SetName(L"DX12NativeScene::MaterialTextureUpload");
			for (uint32_t subresource = 0; subresource < subresources; ++subresource) {
				const auto destination = CD3DX12_TEXTURE_COPY_LOCATION(upload.target.get(), subresource);
				const auto source = CD3DX12_TEXTURE_COPY_LOCATION(upload.staging.get(), footprints[subresource]);
				upload.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			}
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(upload.target.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			upload.commands->ResourceBarrier(1, &barrier);
			DX::ThrowIfFailed(upload.commands->Close());
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(upload.fence.put())));
			upload.event.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
			if (!upload.event) throw std::runtime_error("Material upload event unavailable");
			DX::ThrowIfFailed(upload.fence->SetEventOnCompletion(1, upload.event.get()));
			ID3D12CommandList* lists[]{ upload.commands.get() };
			a_backend.commandQueue->ExecuteCommandLists(1, lists);
			upload.submitted = true;
			if (FAILED(a_backend.commandQueue->Signal(upload.fence.get(), 1))) throw MaterialUploadError("Material upload queue signal failed");
			if (WaitForSingleObject(upload.event.get(), kUploadTimeoutMS) != WAIT_OBJECT_0 || upload.fence->GetCompletedValue() == UINT64_MAX)
				throw MaterialUploadError("Background material upload did not complete");
			const auto pool = uint32_t(a_result.pools.size());
			for (uint32_t slice = 0; slice < a_textures.size(); ++slice)
				for (const auto& path : a_textures[slice]->paths) a_result.locations.emplace(path, std::array<uint32_t, 2>{ pool, slice });
			a_result.pools.push_back({ upload.target, format, uint32_t(meta.mipLevels), uint32_t(a_textures.size()) });
			a_result.bytes += allocation;
			a_result.textures += uint32_t(a_textures.size());
		}
	}

	void LoadMaterialTextures(const GraphicsBackend& a_backend, const MaterialSource& a_source, MaterialResources& a_result)
	{
		const auto path = a_source.directory / "cell-textures.json";
		if (std::filesystem::file_size(path) > 16ull * 1024 * 1024) throw std::runtime_error("Texture manifest exceeds limit");
		std::ifstream stream(path);
		const auto manifest = json::parse(stream);
		if (manifest.at("schemaVersion") != 1 || _stricmp(manifest.at("cell").get_ref<const std::string&>().c_str(), a_source.cell.c_str()) != 0 || manifest.at("layoutDigest") != a_source.layoutDigest)
			throw std::runtime_error("Texture manifest identity mismatch");
		const auto& entries = manifest.at("textures");
		if (!entries.is_array() || entries.size() > 4096) throw std::runtime_error("Texture source count exceeds limit");
		std::map<std::string, std::unique_ptr<Texture>> textures;
		uint64_t sourceBytes = 0;
		for (const auto& entry : entries) {
			if (entry.value("status", std::string{}) != "cached") continue;
			const auto hash = entry.at("sha256").get<std::string>();
			if (entry.at("file") != "textures/" + hash + ".dds") throw std::runtime_error("Texture cache path mismatch");
			auto& texture = textures[hash];
			if (!texture) {
				texture = std::make_unique<Texture>();
				texture->hash = hash;
				const auto bytes = ReadTexture(a_source.directory, hash);
				if (sourceBytes > a_source.budget || bytes.size() > a_source.budget - sourceBytes) throw std::runtime_error("DDS source budget exceeded");
				sourceBytes += bytes.size();
				DirectX::TexMetadata metadata{};
				DX::ThrowIfFailed(DirectX::GetMetadataFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_NONE, metadata));
				ValidateMetadata(metadata, a_source.budget);
				DX::ThrowIfFailed(DirectX::LoadFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, nullptr, texture->image));
				const auto& meta = texture->image.GetMetadata();
				ValidateMetadata(meta, a_source.budget);
				D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ DirectX::MakeLinear(meta.format) };
				DX::ThrowIfFailed(a_backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
				constexpr auto required = D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_TEXTURE2D;
				if ((support.Support1 & required) != required) throw std::runtime_error("Material texture sampling unsupported");
			}
			texture->paths.push_back(entry.at("path").get<std::string>());
		}
		using Key = std::tuple<size_t, size_t, size_t, DXGI_FORMAT>;
		std::map<Key, std::vector<Texture*>> groups;
		for (const auto& [hash, texture] : textures) {
			const auto& meta = texture->image.GetMetadata();
			groups[{ meta.width, meta.height, meta.mipLevels, DirectX::MakeLinear(meta.format) }].push_back(texture.get());
		}
		for (const auto& [key, group] : groups)
			for (size_t first = 0; first < group.size(); first += kPageSlices) {
				if (a_result.pools.size() >= kMaximumTexturePools) throw std::runtime_error("Texture pool count exceeded");
				const std::vector<Texture*> page(group.begin() + first, group.begin() + std::min(first + kPageSlices, group.size()));
				UploadPage(a_backend, page, a_result, a_source.budget);
			}
	}
}
