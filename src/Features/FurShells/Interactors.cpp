#include "Interactors.h"

#include "Globals.h"
#include "Utils/SkinnedMesh.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string_view>

namespace FurInteractors
{
	namespace
	{
		constexpr float MinDominantWeight = 0.5f;
		constexpr float MaxDentRadius = 4.0f;
		constexpr float MinDentPress = 0.15f;
		constexpr float MinDentDepth = 0.04f;
		constexpr float DentRecoverBase = 0.4f;
		constexpr float DentRecoverPerDepth = 2.0f;
		constexpr float MaxFrameSeconds = 0.1f;
		constexpr size_t MaxPressSamples = 384;
		constexpr uint32_t MemoryIdleFrames = 240;
		float3 ToFloat3(const RE::NiPoint3& a_point)
		{
			return { a_point.x, a_point.y, a_point.z };
		}
	}

	void Memory::Advance()
	{
		const float deltaTime = globals::game::deltaTime ? *globals::game::deltaTime : 0.0f;
		frameSeconds = std::clamp(deltaTime, 0.0f, MaxFrameSeconds);
		DecayDents();
		++frame;
	}

	void Memory::Clear()
	{
		memory.clear();
	}

	size_t Memory::GetDentCount() const
	{
		size_t count = 0;
		for (const auto& [geometry, mesh] : memory)
			count += mesh.dents.size();
		return count;
	}

	void Memory::DecayDents()
	{
		for (auto it = memory.begin(); it != memory.end();) {
			auto& dents = it->second.dents;
			for (Dent& dent : dents) {
				if (dent.refreshedFrame != frame)
					dent.depth *= std::exp(-frameSeconds / std::max(dent.recoverSeconds, 0.05f));
			}
			std::erase_if(dents, [](const Dent& a_dent) { return a_dent.depth < MinDentDepth; });
			if (dents.empty() && frame - it->second.usedFrame > MemoryIdleFrames)
				it = memory.erase(it);
			else
				++it;
		}
	}

	void Memory::BuildPressSamples(const RE::BSGeometry* a_geometry, MeshMemory& a_memory) const
	{
		a_memory.samples.clear();
		a_memory.bones.clear();
		std::vector<Util::SkinnedVertex> vertices;
		if (!Util::ReadSkinnedVertices(const_cast<RE::BSGeometry*>(a_geometry), vertices))
			return;

		const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
		a_memory.handSides = 0;
		for (uint32_t bone = 0; bone < skin->numMatrices; ++bone) {
			a_memory.bones.emplace_back(skin->bones[bone]);
			const std::string_view name = skin->bones[bone] && skin->bones[bone]->name.c_str() ? skin->bones[bone]->name.c_str() : "";
			if (name.find("Hand [") != std::string_view::npos || name.find("Finger") != std::string_view::npos)
				a_memory.handSides |= name.starts_with("NPC L ") ? 1u : name.starts_with("NPC R ") ? 2u :
				                                                                                     0u;
		}

		const size_t stride = std::max<size_t>(1, vertices.size() / MaxPressSamples);
		for (size_t index = 0; index < vertices.size(); index += stride) {
			const Util::SkinnedVertex& vertex = vertices[index];
			const size_t dominant = static_cast<size_t>(std::ranges::max_element(vertex.weight) - vertex.weight.begin());
			const uint16_t bone = vertex.bone[dominant];
			if (vertex.weight[dominant] < MinDominantWeight || bone == Util::InvalidSkinBone || !a_memory.bones[bone])
				continue;
			a_memory.samples.push_back({ bone, skin->skinData->GetBoneDataSkinToBone(bone) * vertex.position });
		}
	}

	size_t Memory::Remember(const RE::BSGeometry* a_geometry, float a_furLength, size_t a_count, std::array<Capsule, MaxCapsulesPerPass>& a_capsules)
	{
		const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
		if (!skin || a_furLength <= 0.0f)
			return a_count;

		MeshMemory& mesh = memory[a_geometry];
		mesh.usedFrame = frame;
		if (mesh.skin != skin) {
			mesh.skin = skin;
			mesh.dents.clear();
			mesh.measuredFrame = UINT32_MAX;
			BuildPressSamples(a_geometry, mesh);
		}

		if (mesh.measuredFrame != frame) {
			mesh.measuredFrame = frame;
			for (size_t index = 0; index < a_count && !mesh.samples.empty(); ++index) {
				const Capsule& capsule = a_capsules[index];
				if (!capsule.appendage || capsule.radius > MaxDentRadius || (capsule.side >= 0 && (mesh.handSides >> capsule.side & 1u)))
					continue;

				const float3 axis = capsule.pointB - capsule.pointA;
				const float axisLengthSquared = std::max(axis.LengthSquared(), 1e-4f);
				float nearest = FLT_MAX;
				const PressSample* nearestSample = nullptr;
				float3 pressPoint;
				for (const PressSample& sample : mesh.samples) {
					const float3 position = ToFloat3(mesh.bones[sample.bone]->world * sample.local);
					const float t = std::clamp((position - capsule.pointA).Dot(axis) / axisLengthSquared, 0.0f, 1.0f);
					const float3 closest = capsule.pointA + axis * t;
					const float gap = (position - closest).Length() - capsule.radius;
					if (gap < nearest) {
						nearest = gap;
						nearestSample = &sample;
						pressPoint = closest;
					}
				}

				const float press = std::clamp((a_furLength - nearest) / a_furLength, 0.0f, 1.0f);
				if (!nearestSample || press < MinDentPress)
					continue;

				const RE::NiAVObject* bone = mesh.bones[nearestSample->bone].get();
				const RE::NiPoint3 local = bone->world.Invert() * RE::NiPoint3{ pressPoint.x, pressPoint.y, pressPoint.z };
				const float recover = DentRecoverBase + DentRecoverPerDepth * press;
				Dent* dent = nullptr;
				for (Dent& existing : mesh.dents) {
					if (existing.bone.get() == bone && existing.local.GetDistance(local) < std::max(capsule.radius, 0.5f * existing.radius))
						dent = &existing;
				}
				if (!dent && mesh.dents.size() < MaxDentsPerMesh)
					dent = &mesh.dents.emplace_back();
				if (!dent) {
					dent = &*std::ranges::min_element(mesh.dents, [](const Dent& a, const Dent& b) { return a.depth < b.depth; });
					if (dent->depth >= press)
						continue;
					*dent = {};
				}
				dent->bone.reset(const_cast<RE::NiAVObject*>(bone));
				dent->local = local;
				dent->radius = capsule.radius;
				dent->depth = press;
				dent->recoverSeconds = std::max(dent->recoverSeconds, recover);
				dent->refreshedFrame = frame;
			}
		}

		size_t count = a_count;
		for (const Dent& dent : mesh.dents) {
			if (dent.refreshedFrame == frame || count >= MaxCapsulesPerPass)
				continue;
			const RE::NiPoint3 position = dent.bone->world * dent.local;
			Capsule ghost;
			ghost.pointA = ghost.pointB = ToFloat3(position);
			ghost.radius = dent.radius;
			ghost.strength = std::clamp(dent.depth, 0.0f, 1.0f);
			a_capsules[count++] = ghost;
		}
		return count;
	}
}
