#pragma once

#include <cstdint>
#include <type_traits>

namespace OpenWind
{
	inline constexpr uint32_t kAPIRevision = 1;
	inline constexpr uint32_t kMaximumBatchSize = 16384;

	/** Skyrim world-space vector; wind values retain the existing normalized response scale. */
	struct Vector3
	{
		float x{}, y{}, z{};
	};

	/** Separate full-3D contributions from one immutable field snapshot. */
	struct WindSample
	{
		Vector3 baseVelocity{};
		Vector3 gustVelocity{};
		Vector3 fineVelocity{};
		Vector3 transientVelocity{};
		Vector3 transientVelocityExcludingHavokImpulses{};
		Vector3 finalVelocity{};
		Vector3 windExcludingHavokImpulses{};
		float ambientGust{};
		float transientIntensity{};
		/** Terrain-only diagnostic exposure, from zero sheltered to one exposed. */
		float terrainExposure = 1.0f;
		/** Static-object diagnostic exposure, from zero sheltered to one exposed. */
		float obstacleExposure = 1.0f;
		/** Ambient components already include this combined exposure; transients do not. */
		float windExposure = 1.0f;
		uint64_t frameId{};
	};
	static_assert(sizeof(WindSample) == 112);
	static_assert(std::is_trivially_copyable_v<WindSample>);

	/** Thread-safe batch queries never advance the wind; one batch observes one published update. */
	struct API
	{
		uint32_t structSize = sizeof(API);
		uint32_t revision = kAPIRevision;
		uint32_t sampleSize = sizeof(WindSample);
		uint32_t maximumBatchSize = kMaximumBatchSize;
		/** True after the renderer has initialized and published a field snapshot. */
		bool (*IsReady)() noexcept {};
		/** Writes count samples; false leaves output unchanged. Positions must be finite Skyrim world coordinates. */
		bool (*SampleWind)(const Vector3* positions, WindSample* samples, uint32_t count) noexcept {};
	};

	/** Returns the current interface regardless of revision; callers check sizes and required callbacks. */
	using RequestAPI = const API* (*)(uint32_t revision) noexcept;
}
