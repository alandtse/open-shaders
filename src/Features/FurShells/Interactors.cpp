#include "Interactors.h"

#include "Globals.h"
#include "Utils/ActorUtils.h"
#include "Utils/SkinnedMesh.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string_view>

namespace FurInteractors
{
	namespace
	{
		constexpr float MaxDentRadius = 4.0f;
		constexpr float MinDentPress = 0.15f;
		constexpr float MinDentDepth = 0.04f;
		constexpr float DentRecoverBase = 0.4f;
		constexpr float DentRecoverPerDepth = 2.0f;
		constexpr float MaxFrameSeconds = 0.1f;
		constexpr size_t MaxPressSamples = 384;
		constexpr uint32_t MemoryIdleFrames = 240;
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
		std::vector<int> boneSides(skin->numMatrices, -1);
		for (uint32_t bone = 0; bone < skin->numMatrices; ++bone) {
			a_memory.bones.emplace_back(skin->bones[bone]);
			const std::string_view name = skin->bones[bone] && skin->bones[bone]->name.c_str() ? skin->bones[bone]->name.c_str() : "";
			boneSides[bone] = Util::BodyCapsules::HandBoneSide(name);
		}

		const size_t stride = std::max<size_t>(1, vertices.size() / MaxPressSamples);
		for (size_t index = 0; index < vertices.size(); index += stride) {
			const Util::SkinnedVertex& vertex = vertices[index];
			const auto dominant = Util::DominantBone(vertex);
			if (!dominant || !a_memory.bones[*dominant])
				continue;
			const uint16_t bone = *dominant;
			a_memory.samples.push_back({ bone, boneSides[bone], skin->skinData->GetBoneDataSkinToBone(bone) * vertex.position });
		}
	}

	Memory::MeshMemory& Memory::Prepare(const RE::BSGeometry* a_geometry, const void* a_skin)
	{
		MeshMemory& mesh = memory[a_geometry];
		mesh.usedFrame = frame;
		if (mesh.skin != a_skin) {
			mesh.skin = a_skin;
			mesh.dents.clear();
			mesh.measuredFrame = UINT32_MAX;
			BuildPressSamples(a_geometry, mesh);
		}
		return mesh;
	}

	size_t Memory::Nearest(const RE::BSGeometry* a_geometry, std::span<const Capsule> a_candidates, std::span<Capsule> a_out)
	{
		ZoneScopedN("FurShells::Nearest");
		const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
		if (!skin) {
			const size_t keep = std::min(a_out.size(), a_candidates.size());
			std::copy_n(a_candidates.begin(), keep, a_out.begin());
			return keep;
		}

		MeshMemory& mesh = Prepare(a_geometry, skin);
		const size_t keep = std::min(a_out.size(), a_candidates.size());
		if (a_candidates.size() <= a_out.size() || mesh.samples.empty()) {
			std::copy_n(a_candidates.begin(), keep, a_out.begin());
			return keep;
		}

		std::vector<float3> positions;
		positions.reserve(mesh.samples.size());
		for (const PressSample& sample : mesh.samples)
			positions.push_back(Util::ToFloat3(mesh.bones[sample.bone]->world * sample.local));

		std::vector<std::pair<float, size_t>> gaps;
		gaps.reserve(a_candidates.size());
		for (size_t index = 0; index < a_candidates.size(); ++index) {
			const Capsule& capsule = a_candidates[index];
			float nearest = FLT_MAX;
			for (size_t sample = 0; sample < positions.size(); ++sample) {
				if (capsule.side >= 0 && mesh.samples[sample].side == capsule.side)
					continue;
				nearest = std::min(nearest, (positions[sample] - capsule.ClosestPointOnAxis(positions[sample])).LengthSquared());
			}
			gaps.emplace_back(std::sqrt(nearest) - capsule.radius, index);
		}
		std::ranges::partial_sort(gaps, gaps.begin() + static_cast<std::ptrdiff_t>(keep));
		for (size_t index = 0; index < keep; ++index)
			a_out[index] = a_candidates[gaps[index].second];
		return keep;
	}

	size_t Memory::Remember(const RE::BSGeometry* a_geometry, float a_furLength, size_t a_count, std::array<Capsule, MaxCapsulesPerPass>& a_capsules)
	{
		ZoneScopedN("FurShells::Remember");
		const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
		if (!skin || a_furLength <= 0.0f)
			return a_count;

		MeshMemory& mesh = Prepare(a_geometry, skin);

		if (mesh.measuredFrame != frame) {
			mesh.measuredFrame = frame;
			for (size_t index = 0; index < a_count && !mesh.samples.empty(); ++index) {
				const Capsule& capsule = a_capsules[index];
				if (!capsule.appendage || capsule.radius > MaxDentRadius)
					continue;

				float nearest = FLT_MAX;
				const PressSample* nearestSample = nullptr;
				float3 pressPoint;
				for (const PressSample& sample : mesh.samples) {
					if (capsule.side >= 0 && sample.side == capsule.side)
						continue;
					const float3 position = Util::ToFloat3(mesh.bones[sample.bone]->world * sample.local);
					const float3 closest = capsule.ClosestPointOnAxis(position);
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
			ghost.pointA = ghost.pointB = Util::ToFloat3(position);
			ghost.radius = dent.radius;
			ghost.strength = std::clamp(dent.depth, 0.0f, 1.0f);
			ghost.remembered = true;
			a_capsules[count++] = ghost;
		}
		return count;
	}
}
