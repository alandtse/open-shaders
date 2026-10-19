#pragma once

#include "RE/Skyrim.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Util
{
	/** @brief Skin bone slot value for an unused or invalid influence. */
	inline constexpr uint16_t InvalidSkinBone = 0xFFFF;

	/** @brief Skin weight a vertex's strongest bone needs for DominantBone to name it. */
	inline constexpr float MinDominantBoneWeight = 0.5f;

	/** @brief One vertex of a skinned mesh as the engine keeps it on the CPU: bind-pose position and up to four bone influences. */
	struct SkinnedVertex
	{
		RE::NiPoint3 position;                                                                               ///< Mesh-space bind position.
		std::array<uint16_t, 4> bone{ InvalidSkinBone, InvalidSkinBone, InvalidSkinBone, InvalidSkinBone };  ///< Index into the skin instance's bone list.
		std::array<float, 4> weight{};
	};

	/**
	 * @brief Reads the CPU copy of a skinned mesh's vertices.
	 * @details Skin partitions keep partition-local bone indices; they are resolved to skin-instance bone indices here, so a
	 *          bone is `skin->bones[vertex.bone[slot]]` and its bind transform is
	 *          `skin->skinData->GetBoneDataSkinToBone(vertex.bone[slot])`. Partitions without CPU vertex data, positions or
	 *          skinning are skipped.
	 * @param a_geometry Skinned geometry.
	 * @param a_vertices Receives the vertices, replacing its contents.
	 * @return False when the geometry has no usable CPU vertex data.
	 */
	[[nodiscard]] bool ReadSkinnedVertices(RE::BSGeometry* a_geometry, std::vector<SkinnedVertex>& a_vertices);

	/** @brief The skin bone that carries most of a vertex, or nothing when it has no valid bone or none weighs at least MinDominantBoneWeight. */
	[[nodiscard]] inline std::optional<uint16_t> DominantBone(const SkinnedVertex& a_vertex)
	{
		size_t dominant = 0;
		for (size_t slot = 1; slot < a_vertex.weight.size(); ++slot) {
			if (a_vertex.weight[slot] > a_vertex.weight[dominant])
				dominant = slot;
		}
		if (a_vertex.weight[dominant] < MinDominantBoneWeight || a_vertex.bone[dominant] == InvalidSkinBone)
			return std::nullopt;
		return a_vertex.bone[dominant];
	}
}
