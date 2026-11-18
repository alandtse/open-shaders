#pragma once

#include "AccelerationScene.h"
#include "LiveGpuTextureEligibility.h"
#include "LiveLooseTexturePlan.h"
#include "LogicalTextureRegistry.h"
#include "MaterialUploadStatus.h"
#include "Features/NativePBR/VerifiedBridge.h"
#include "VerifiedTextureSources.h"
#include <array>
#include <stdexcept>

namespace DX12Native
{
	/** @brief Latches a failed submitted upload so automatic retries cannot accumulate abandoned GPU work. */
	struct MaterialUploadError : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};
	/** @brief Defers a new load while an earlier submitted upload still owns staging. */
	struct MaterialUploadBusy : MaterialUploadError
	{
		using MaterialUploadError::MaterialUploadError;
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
		winrt::com_ptr<ID3D12Fence> readyFence;
	};
	/** @brief Polls a submitted material upload without waiting on the CPU. */
	struct MaterialUploadCompletion
	{
		virtual ~MaterialUploadCompletion() = default;
		virtual MaterialUploadStatus Poll() = 0;
	};
	/** @brief Owns material tables and texture pages, including any pending final upload. */
	struct MaterialResources
	{
		std::vector<MaterialTexturePool> pools;
		std::unordered_map<std::string, uint64_t> locations;
		VerifiedTextureSources verifiedTextureSources;
		std::unordered_map<uint64_t, std::array<uint32_t, 2>> logicalLocations;
		std::vector<std::shared_ptr<const LogicalTextureRegistry::Ticket>> textureTickets;
		std::shared_ptr<MaterialUploadCompletion> pendingUpload;
		winrt::com_ptr<ID3D12Resource> materials, surfaces;
		winrt::com_ptr<ID3D12Resource> roomVolumes;
		uint32_t roomCount = 0;
		bool roomsKnown = false;
		uint64_t bytes = 0, tableBytes = 0, peakStagingBytes = 0, uploadedBytes = 0, uploadWaitMicroseconds = 0;
		uint64_t asyncBatchStagingBytes = 0;
		uint32_t readyMaterials = 0, rejectedMaterials = 0, textures = 0, uploadedPages = 0, reusedPages = 0, reusedTextures = 0, rejectedTextureSources = 0, asyncBatchPages = 0;
		uint32_t diskCacheHits = 0, diskCacheWrites = 0;
		uint32_t livePbrMatched = 0, livePbrApplied = 0, livePbrEmissiveApplied = 0, livePbrAmbiguous = 0, livePbrIncompatible = 0, livePbrMissing = 0;
		uint32_t livePbrSourcesMissing = 0, livePbrSourcesAmbiguous = 0, livePbrSourcesIncompatible = 0;
		uint32_t livePbrUnsupported = 0, livePbrUnsafeOpacity = 0, livePbrMissingBasis = 0;
		uint32_t livePbrMissingBindings = 0, livePbrOfflineFallback = 0;
		uint32_t looseDdsPlanned = 0, looseDdsImported = 0, looseDdsRejected = 0, looseDdsDeferred = 0;
		uint32_t looseDdsReadyRecipes = 0, looseDdsFeatureLimitedRecipes = 0, looseDdsSkippedRecipes = 0, looseDdsPrepared = 0, looseDdsInvalid = 0;
		uint32_t looseDdsPreviouslyAttempted = 0;
		std::vector<std::string> looseDdsAttemptedPaths;
		uint64_t looseDdsBytes = 0, archiveDdsBytes = 0;
		uint32_t archiveDdsPlanned = 0, archiveDdsImported = 0, archiveDdsRejected = 0;
		LiveGpuTextureRoutes liveGpuRoutes;
		NativePBR::RecipeIssueTally liveRecipeIssues;
		std::string failure, firstTextureSourceFailure;
		/** @brief Loads one owned package on the binding worker; never accesses engine objects. */
		static std::shared_ptr<const MaterialResources> Load(const GraphicsBackend& a_backend, const TopLevel& a_scene,
			const NativePBR::LiveBindingSource* a_livePbr = nullptr,
			const MaterialResources* a_previous = nullptr);
		/** @brief Loads independent DDS residency from immutable recipes without geometry or TLAS. */
		static std::shared_ptr<MaterialResources> LoadResidency(const GraphicsBackend& a_backend, const MaterialSource& a_source,
			const NativePBR::LiveMatchResult* a_matches, const MaterialResources* a_previous = nullptr,
			bool a_extendedRoles = false);
	};

	/** @brief Validates and uploads original DDS mip chains into bounded compatible array pages. */
	void LoadMaterialTextures(const GraphicsBackend& a_backend, const MaterialSource& a_source, MaterialResources& a_result,
		const NativePBR::LiveMatchResult* a_matches = nullptr, bool a_extendedRoles = false);
	/** @brief Releases a completed or device-removed failed upload without waiting. */
	void RetireMaterialUploads();
	/** @brief Checks the single upload slot without waiting. */
	bool MaterialUploadsReady();
	/** @brief Returns retained cache bytes without waiting for the loading worker. */
	std::optional<uint64_t> RetainedMaterialPageBytes();
}
