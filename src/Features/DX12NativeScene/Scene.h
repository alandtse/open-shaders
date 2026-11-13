#pragma once

#include <unordered_set>

#include "GraphicsBackend.h"
#include <bit>
#include <d3d11.h>
#include <d3d12.h>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <winrt/base.h>

namespace DX12Native
{
	// Shared native-only light ABI: prepared-first LLF, direct RayQuery,
	// and independent RayQuery shadow tracing must agree on capacity.
	inline constexpr uint32_t kMaximumNativePointLights = 64;
	inline constexpr uint32_t kNativeLightRecordTexels = 8;
	inline constexpr uint32_t kNativeShadowLightTextureWidth =
		kMaximumNativePointLights * kNativeLightRecordTexels;
	struct SceneScope
	{
		uint64_t scene = 0, light = 0, batch = 0;
		uint32_t frame = 0, slot = 0, policy = 0;
		bool operator==(const SceneScope&) const = default;
	};
	inline constexpr uint8_t kOpaqueInstanceMask = 1;
	inline constexpr uint8_t kMaterialPendingInstanceMask = 2;
	/** @brief Owns the prepared material package identity and its independent residency limit. */
	struct MaterialSource
	{
		std::filesystem::path directory;
		std::string cell, layoutDigest;
		uint64_t budget = 0;
		std::shared_ptr<const json> rooms;
		std::shared_ptr<const json> textureReferences;  // Existing Data/BSA path references from the packed CELL.
		uint64_t uploadBatchBytes = 16ull * 1024 * 1024;
		bool importLoosePbr = false;                            // Worker-owned loose DDS supplement, explicitly opt-in.
		bool importArchivePbr = false;                          // Optional archive fallback when the loose file is absent.
		bool usePreparedTextureManifest = true;                 // Legacy atlas compatibility. Runtime-source mode may omit it.
		std::unordered_set<std::string> attemptedRuntimePaths;  // Worker-owned stream history.
	};
	/** @brief Preserves offline material work classes without rediscovering them during traversal. */
	enum class OfflineMaterialFlags : uint32_t
	{
		None = 0,
		AlphaCoverage = 1,
		VertexAlpha = 2,
		AnimatedMaterial = 4,
		Unknown = 0x80000000u
	};
	/** @brief Budgets engine observations and owned GPU geometry independently. */
	struct SceneLimits
	{
		uint32_t maxInstances = 8192;
		uint64_t bufferBytes = 128ull * 1024 * 1024;
	};

	/** @brief Owns copied GPU bytes without retaining engine geometry. */
	struct Mesh
	{
		winrt::com_ptr<ID3D11Buffer> vertices, indices;
		uint32_t stride = 0;
		uint32_t vertexCount = 0;
		uint64_t descriptor = 0;
		uint64_t bytes = 0;
	};

	/** @brief Owns compiled normal/UV/color data independently of shared intersection geometry. */
	struct VertexAttributes
	{
		winrt::com_ptr<ID3D12Resource> buffer;
		uint32_t vertexCount = 0, stride = 0;
		bool hasTangentFrame = false;
		uint64_t allocationBytes = 0;
		std::string contentDigest;
	};

	/** @brief Captures material coverage without a live Skyrim material pointer. */
	struct Material
	{
		winrt::com_ptr<ID3D11ShaderResourceView> alphaTexture;
		float4 uvTransform{ 1, 1, 0, 0 };
		float alphaCutoff = 0;
		float alpha = 1;
		uint16_t alphaFlags = 0;
		int32_t textureClampMode = 0;
		bool twoSided = false;
		bool alphaTest = false;
		bool opaqueCandidate = true;
		OfflineMaterialFlags offlineMaterialFlags = OfflineMaterialFlags::None;
		std::shared_ptr<const VertexAttributes> attributes;
		std::shared_ptr<const json> offlineDescription;
		std::string shadingStatus;
		uint32_t roomIndex = UINT32_MAX;
	};

	/** @brief Retains prepared source provenance without asserting a live-engine match. */
	struct PreparedIdentity
	{
		std::string referenceKey, model, shapeName, sourceDigest;
		uint32_t blockIndex = UINT32_MAX;
	};

	/** @brief Records a rigid instance separately from retained mesh bytes. */
	struct Instance
	{
		uint64_t identity = 0;
		PreparedIdentity preparedIdentity;
		std::shared_ptr<const Mesh> mesh;
		Material material;
		DirectX::XMFLOAT3X4 world{};
		float4 bound{};
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		uint64_t observationFrame = 0;
	};

	/** @brief Publishes completed GPU copies; acquisition coverage is distinct from traversal coverage. */
	struct Snapshot
	{
		uint64_t generation = 0;
		uint64_t sweep = 0;
		SceneScope scope;
		uint64_t firstFrame = 0, lastFrame = 0;
		std::vector<Instance> instances;
		std::unordered_map<std::string, uint32_t> omissions;
		uint32_t visitedNodes = 0;
		bool truncated = false;
		bool offline = false;
		std::shared_ptr<const MaterialSource> materialSource;
	};

}
