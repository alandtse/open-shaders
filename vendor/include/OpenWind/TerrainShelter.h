#pragma once

#include "OpenWind/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace OpenWind::ObstacleShelter { struct Frame; }

namespace OpenWind::TerrainShelter
{
	struct TerrainHeightmap;
	inline constexpr uint32_t kTerrainSize = 65;
	inline constexpr uint32_t kTerrainSampleCount = kTerrainSize * kTerrainSize;
	inline constexpr uint32_t kVolumeWidth = 32;
	inline constexpr uint32_t kVolumeDepth = 16;
	inline constexpr uint32_t kVolumeSampleCount = kVolumeWidth * kVolumeWidth * kVolumeDepth;
	inline constexpr uint32_t kVolumePlanes = kVolumeDepth + 1;
	inline constexpr uint32_t kCascadeCount = 3;
	inline constexpr float kCellSize = 256.0f;
	inline constexpr float kMiddleCellSize = 1024.0f;
	inline constexpr float kMiddleRadius = (kVolumeWidth - 1) * kMiddleCellSize * 0.5f;
	inline constexpr std::array<float, kVolumeDepth> kHeightLevels{
		0.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f, 768.0f, 1024.0f,
		1536.0f, 2048.0f, 3072.0f, 4096.0f, 6144.0f, 8192.0f, 12288.0f, 16384.0f
	};
	inline constexpr float kSampleFadeSeconds = 0.5f;

	/** Terrain samples and discovery times shared by CPU and GPU shelter evaluation. */
	struct Heightfield
	{
		uint64_t revision{};
		uint32_t worldspaceId{};
		uint32_t valid{};
		float2 origin{};
		float cellSize = kCellSize;
		float centerHeight{};
		std::array<float2, kTerrainSampleCount> samples{};
	};

	/** Five float4 rows forming the shelter compute parameter prefix. */
	struct Parameters
	{
		float4 terrain{};
		float4 volumeOrigin{};
		float4 volumeStep{ kCellSize, kCellSize, 1.0f, 0.0f };
		float4 directions{};
		float4 response{};
	};

	/** One immutable terrain revision and the current wind-dependent exposure parameters. */
	struct Frame
	{
		Heightfield heightfield{};
		Parameters parameters{};
		Heightfield farHeightfield{};
		Parameters farParameters{};
		Heightfield middleHeightfield{};
		Parameters middleParameters{};
	};

	static_assert(sizeof(Parameters) == 80);
	static_assert(offsetof(Heightfield, samples) == 32);
	static_assert(std::is_standard_layout_v<Frame>);
	static_assert(std::is_trivially_copyable_v<Frame>);

	/** Returns height, discovery confidence, and known-sample weight at an undeformed XY position. */
	[[nodiscard]] float3 SampleGround(const Heightfield& a_field, const Parameters& a_parameters,
		float a_x, float a_y) noexcept;

	/** Evaluates one exposure lattice node using the shared directional terrain horizon. */
	[[nodiscard]] float EvaluateNode(const Heightfield& a_field, const Parameters& a_parameters,
		uint32_t a_x, uint32_t a_y, uint32_t a_z) noexcept;
	/** Trilinearly samples the exposure lattice with a smooth fade to open air at its boundary. */
	[[nodiscard]] float SampleExposure(const Heightfield& a_field, const Parameters& a_parameters,
		const float3& a_position) noexcept;
	/** Samples combined, terrain, and obstacle exposure using matching ground-relative cascades. */
	[[nodiscard]] float3 SampleExposureComponents(const Heightfield* a_near, const Parameters& a_nearParameters,
		const Heightfield* a_far, const Parameters& a_farParameters, const ObstacleShelter::Frame* a_obstacles,
		const float4& a_obstacleResponse, const float3& a_position, const TerrainHeightmap* a_world = nullptr,
		const Heightfield* a_middle = nullptr, const Parameters* a_middleParameters = nullptr) noexcept;
}
