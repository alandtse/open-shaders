#pragma once

#include "OpenWind/Types.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace OpenWind::ObstacleShelter
{
	inline constexpr uint32_t kWidth = 32;
	inline constexpr uint32_t kCascadeCount = 3;
	inline constexpr uint32_t kDirectionCount = 2;
	inline constexpr uint32_t kFieldCount = kCascadeCount * kDirectionCount;
	inline constexpr std::array<float, kCascadeCount - 1> kCascadeCellSizes{ 256.0f, 1024.0f };
	inline constexpr uint32_t kHeightCount = 6;
	inline constexpr uint32_t kColumnCount = kWidth * kWidth;
	inline constexpr uint32_t kSampleCount = kColumnCount * kHeightCount;
	inline constexpr std::array<float, kHeightCount> kHeights{ 16.0f, 64.0f, 192.0f, 512.0f, 1280.0f, 3072.0f };
	inline constexpr float kBlendSeconds = 0.5f;
	inline constexpr float kStaleHoldSeconds = 12.0f;
	inline constexpr float kStaleFadeSeconds = 2.0f;
	inline constexpr float kBoundaryCells = 2.0f;

	/** One direction's static-collision cache; sample indices are height-major, then Y, then X. */
	struct Grid
	{
		uint64_t revision{};
		uint32_t worldspaceId{};
		uint32_t valid{};
		float2 origin{};
		float cellSize{};
		float range{};
		float2 direction{};
		/** Ground height and discovery time; a negative discovery time denotes unknown ground. */
		std::array<float2, kColumnCount> ground{};
		/** Initial exposure, target exposure, blend start, and last success; negative last success denotes unknown. */
		std::array<float4, kSampleCount> samples{};
	};

	/** Each near-to-far cascade stores current then transition; response is time, strength, direction blend, enabled. */
	struct Frame
	{
		std::array<Grid, kFieldCount> fields{};
		float4 response{};
	};
	static_assert(std::is_standard_layout_v<Frame>);
	static_assert(std::is_trivially_copyable_v<Frame>);

	/** Returns static-geometry exposure using only immutable cached ray results. */
	[[nodiscard]] float SampleExposure(const Frame& a_frame, const float3& a_position) noexcept;
	/** Samples with current response values, optionally rejecting grids from a different worldspace. */
	[[nodiscard]] float SampleExposure(const Frame& a_frame, const float4& a_response,
		const float3& a_position, uint32_t a_worldspaceId = 0) noexcept;
}
