#include "SkinnedMesh.h"

#include <DirectXPackedVector.h>

namespace Util
{
	bool ReadSkinnedVertices(RE::BSGeometry* a_geometry, std::vector<SkinnedVertex>& a_vertices)
	{
		a_vertices.clear();
		const auto* skin = a_geometry ? a_geometry->GetGeometryRuntimeData().skinInstance.get() : nullptr;
		const auto* partitionSet = skin ? skin->skinPartition.get() : nullptr;
		if (!partitionSet || !skin->skinData || !skin->bones)
			return false;

		for (uint32_t partitionIndex = 0; partitionIndex < partitionSet->numPartitions; ++partitionIndex) {
			const auto& part = partitionSet->partitions[partitionIndex];
			RE::BSGraphics::VertexDesc desc = part.vertexDesc;
			if (!part.bones || !part.buffData || !part.buffData->rawVertexData || !desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
				continue;

			const uint32_t stride = desc.GetSize();
			const uint32_t skinOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
			a_vertices.reserve(a_vertices.size() + part.vertices);
			for (uint16_t vertex = 0; vertex < part.vertices; ++vertex) {
				const uint8_t* data = part.buffData->rawVertexData + static_cast<size_t>(vertex) * stride;
				const auto* position = reinterpret_cast<const float*>(data);
				const auto* weights = reinterpret_cast<const uint16_t*>(data + skinOffset);
				const uint8_t* indices = data + skinOffset + 4 * sizeof(uint16_t);

				SkinnedVertex out;
				out.position = { position[0], position[1], position[2] };
				for (size_t slot = 0; slot < out.bone.size(); ++slot) {
					const uint8_t partitionBone = indices[slot];
					if (partitionBone >= part.numBones || part.bones[partitionBone] >= skin->numMatrices)
						continue;
					out.bone[slot] = part.bones[partitionBone];
					out.weight[slot] = DirectX::PackedVector::XMConvertHalfToFloat(weights[slot]);
				}
				a_vertices.push_back(out);
			}
		}
		return !a_vertices.empty();
	}
}
