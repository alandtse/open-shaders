#pragma once

#include "AccelerationScene.h"
#include "LogicalTextureRegistry.h"
#include <array>
#include <stdexcept>

namespace DX12Native
{
	/** @brief Latches a failed submitted upload so automatic retries cannot accumulate abandoned GPU work. */
	struct MaterialUploadError : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};
	inline constexpr uint32_t kMaximumTexturePools = 256;
	inline constexpr uint32_t kMissingTexture = UINT32_MAX;
	/** @brief Matches the immutable 128-byte ray-material shader record. */
	struct MaterialRecord
	{
		std::array<uint32_t, 4> colorNormal{ kMissingTexture, 0, kMissingTexture, 0 };
		std::array<uint32_t, 4> packedEmission{ kMissingTexture, 0, kMissingTexture, 0 };
		float4 uv{ 1, 1, 0, 0 }, emission{}, specular{}, scalar{};
		std::array<uint32_t, 4> flags{};
		float4 reserved{};
	};
	/** @brief Matches the 32-byte instance-to-shading binding consumed at triangle hits. */
	struct SurfaceRecord
	{
		std::array<uint32_t, 4> geometry{};
		std::array<uint32_t, 4> material{};
	};
	/** @brief Retains one compatible array page with original mip data and a linear view. */
	struct MaterialTexturePool
	{
		winrt::com_ptr<ID3D12Resource> resource;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t mips = 0, slices = 0;
		uint64_t bytes = 0;
	};
	/** @brief Owns completed native texture uploads and shared material/instance tables. */
	struct MaterialResources
	{
		std::vector<MaterialTexturePool> pools;
		std::unordered_map<std::string, uint64_t> locations;
		std::unordered_map<uint64_t, std::array<uint32_t, 2>> logicalLocations;
		std::vector<std::shared_ptr<const LogicalTextureRegistry::Ticket>> textureTickets;
		winrt::com_ptr<ID3D12Resource> materials, surfaces;
		winrt::com_ptr<ID3D12Resource> roomVolumes;
		uint32_t roomCount = 0;
		bool roomsKnown = false;
		uint64_t bytes = 0, tableBytes = 0, peakStagingBytes = 0, uploadedBytes = 0, uploadWaitMicroseconds = 0;
		uint32_t readyMaterials = 0, rejectedMaterials = 0, textures = 0, uploadedPages = 0, reusedPages = 0;
		std::string failure;
		/** @brief Loads one owned package on the binding worker; never accesses engine objects. */
		static std::shared_ptr<const MaterialResources> Load(const GraphicsBackend& a_backend, const TopLevel& a_scene);
	};

	/** @brief Validates and uploads original DDS mip chains into bounded compatible array pages. */
	void LoadMaterialTextures(const GraphicsBackend& a_backend, const MaterialSource& a_source, MaterialResources& a_result);
	/** @brief Releases a completed or device-removed failed upload without waiting. */
	void RetireMaterialUploads();
	/** @brief Returns retained cache bytes without waiting for the loading worker. */
	std::optional<uint64_t> RetainedMaterialPageBytes();
}
