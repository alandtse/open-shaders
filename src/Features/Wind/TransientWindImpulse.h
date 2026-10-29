#pragma once

#include <cstddef>
#include <type_traits>

namespace WindField
{
	inline constexpr std::size_t kTransientImpulseCapacity = 96;

	enum class TransientWindSourceType : uint32_t
	{
		DirectionalWave,
		RadialWave,
		DirectionalCone,
		OrientedFlow,
		DirectionalOvalWave,
		AttachedWedge
	};

	struct TransientWindSource
	{
		float3 origin{};
		float wavefrontDistance{};
		float3 direction{};
		float strength{};
		float maxDistance{};
		float waveHalfWidth{};
		float propagationSpeed{};
		float coneSpreadTangent{};
		float decayTime{};
		float collisionRadius{};
		float sideSpreadTangent{};
		float sideStrength{};
		TransientWindSourceType type{};
		float verticalScale{ 1.0f };
		float falloffWidth{};
		float orientationRoll{};
	};
	static_assert(sizeof(TransientWindSource) == 80);
	static_assert(offsetof(TransientWindSource, orientationRoll) == 76);
	static_assert(std::is_standard_layout_v<TransientWindSource>);

}
