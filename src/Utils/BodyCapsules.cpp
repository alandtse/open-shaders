#include "BodyCapsules.h"

#include "Globals.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/Game.h"
#include "Utils/HiggsInterface.h"
#include "Utils/SkinnedMesh.h"

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace Util::BodyCapsules
{
	namespace
	{
		constexpr size_t MaxActors = 8;
		constexpr size_t MaxCapsulesPerActor = 6;
		constexpr float MinActorCapsuleRadius = 3.0f;
		constexpr float MinRadiusPerDistance = 0.002f;
		constexpr float MinFitRadius = 0.4f;
		constexpr float MaxFitRadius = 3.0f;
		constexpr float MaxPalmFitRadius = 2.0f;
		constexpr float FitPercentile = 0.85f;
		constexpr size_t MinFitVertices = 6;
		constexpr uint32_t FitCheckInterval = 120;
		constexpr float FingertipLengthScale = 0.9f;
		constexpr float TipPercentile = 0.97f;
		constexpr float MaxTipExtent = 6.0f;
		constexpr size_t IndexFinger = 1;
		constexpr size_t PinkyFinger = 4;
		constexpr float MaxMovePerFrame = 8.0f;
		constexpr uint64_t AppendageKeyBase = 0x100000000ull;
		constexpr size_t RightHandKeyOffset = 64;
		constexpr size_t HeldKeyOffset = 128;
		constexpr size_t MaxPartsPerHeld = 4;

		uint64_t MakeKey(RE::FormID a_owner, size_t a_index)
		{
			return (static_cast<uint64_t>(a_owner) << 8) ^ static_cast<uint64_t>(a_index);
		}

		/** @brief Skeleton node name of a hand (index 0) or of one finger joint (index 1 to 15). */
		std::string HandNodeName(bool a_left, size_t a_index)
		{
			const char side = a_left ? 'L' : 'R';
			if (a_index == 0)
				return std::format("NPC {} Hand [{}Hnd]", side, side);
			const size_t finger = (a_index - 1) / 3;
			const size_t joint = (a_index - 1) % 3;
			return std::format("NPC {} Finger{}{} [{}F{}{}]", side, finger, joint, side, finger, joint);
		}
	}

	bool Tracker::GetHandPositions(bool a_left, float3& a_hand, float3& a_fingerJoint) const
	{
		const auto& nodes = handNodes[a_left ? 0 : 1].nodes;
		const auto& hand = nodes[0];
		const auto& finger = nodes[1 + 2 * JointsPerFinger + 2];
		if (!hand || !finger || !hand->parent || !finger->parent)
			return false;
		a_hand = ToFloat3(hand->world.translate);
		a_fingerJoint = ToFloat3(finger->world.translate);
		return true;
	}

	Tracker::HandReport Tracker::GetHandReport(bool a_left) const
	{
		HandReport report;
		const auto& nodes = handNodes[a_left ? 0 : 1].nodes;
		const auto& radii = handRadii[a_left ? 0 : 1];
		for (size_t joint = 0; joint < report.position.size(); ++joint) {
			const auto& node = nodes[1 + joint];
			report.found[joint] = node && node->parent;
			if (report.found[joint])
				report.position[joint] = ToFloat3(node->world.translate);
			report.radius[joint] = radii.finger[joint];
		}
		report.tipExtent = radii.tipExtent;
		report.palmRadius = radii.palm;
		return report;
	}

	float Tracker::GetAverageFingerRadius() const
	{
		float sum = 0.0f;
		for (const auto& hand : handRadii)
			for (const float radius : hand.finger)
				sum += radius;
		return sum / static_cast<float>(handRadii.size() * handRadii[0].finger.size());
	}

	float Tracker::GetAveragePalmRadius() const
	{
		return (handRadii[0].palm + handRadii[1].palm) * 0.5f;
	}

	void Tracker::FitRadii(RE::Actor* a_player)
	{
		// Matched by name because armor can be skinned to a different copy of the skeleton.
		std::unordered_map<std::string, size_t> slotOfName;
		for (size_t side = 0; side < handNodes.size(); ++side) {
			for (size_t index = 0; index < HandNodeCount; ++index)
				slotOfName[HandNodeName(side == 0, index)] = side * HandNodeCount + index;
		}

		const auto segmentEnd = [&](size_t a_side, size_t a_index) -> std::optional<float3> {
			const auto& nodes = handNodes[a_side].nodes;
			const bool distal = a_index != 0 && (a_index - 1) % JointsPerFinger == JointsPerFinger - 1;
			const RE::NiAVObject* source = nodes[a_index].get();
			if (a_index == 0)
				source = nodes[1 + IndexFinger * JointsPerFinger].get();  // the palm runs to the index knuckle
			else if (!distal)
				source = nodes[a_index + 1].get();
			if (!source)
				return std::nullopt;
			const float3 translate = ToFloat3(source->local.translate);
			return distal ? translate * FingertipLengthScale : translate;
		};

		std::array<float, 2 * HandNodeCount> outermost{};
		std::array<float, 2 * HandNodeCount> tipExtents{};
		size_t meshes = 0;

		std::vector<Util::SkinnedVertex> vertices;
		Util::ForEachActorGeometry(a_player, [&](RE::BSGeometry* a_geometry) {
			if (!Util::ReadSkinnedVertices(a_geometry, vertices))
				return;
			const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();

			std::vector<std::optional<size_t>> handSlotOfBone(skin->numMatrices);
			for (uint32_t bone = 0; bone < skin->numMatrices; ++bone) {
				const RE::NiAVObject* boneNode = skin->bones[bone];
				if (!boneNode || !boneNode->name.c_str())
					continue;
				if (const auto found = slotOfName.find(boneNode->name.c_str()); found != slotOfName.end())
					handSlotOfBone[bone] = found->second;
			}

			std::array<std::vector<float>, 2 * HandNodeCount> distances;
			std::array<std::vector<float>, 2 * HandNodeCount> alongAxis;
			bool touched = false;
			for (const Util::SkinnedVertex& vertex : vertices) {
				const auto dominant = Util::DominantBone(vertex);
				if (!dominant || !handSlotOfBone[*dominant])
					continue;
				const uint16_t skinBone = *dominant;

				const size_t handSlot = *handSlotOfBone[skinBone];
				const auto end = segmentEnd(handSlot / HandNodeCount, handSlot % HandNodeCount);
				if (!end)
					continue;

				const RE::NiTransform skinToBone = skin->skinData->GetBoneDataSkinToBone(skinBone);
				const float3 point = ToFloat3(skinToBone * vertex.position);
				const bool distal = handSlot % HandNodeCount != 0 && (handSlot % HandNodeCount - 1) % JointsPerFinger == JointsPerFinger - 1;
				const float along = point.Dot(*end) / std::max(end->LengthSquared(), 1e-4f);
				const float t = distal ? std::max(along, 0.0f) : std::clamp(along, 0.0f, 1.0f);
				distances[handSlot].push_back((point - *end * t).Length());
				if (distal)
					alongAxis[handSlot].push_back(along * end->Length());
				touched = true;
			}

			if (!touched)
				return;
			++meshes;
			for (size_t slot = 0; slot < distances.size(); ++slot) {
				auto& list = distances[slot];
				if (list.size() < MinFitVertices)
					continue;
				const size_t rank = std::min(list.size() - 1, static_cast<size_t>(FitPercentile * static_cast<float>(list.size())));
				std::ranges::nth_element(list, list.begin() + static_cast<std::ptrdiff_t>(rank));
				outermost[slot] = std::max(outermost[slot], list[rank]);
			}
			for (size_t slot = 0; slot < alongAxis.size(); ++slot) {
				auto& list = alongAxis[slot];
				if (list.size() < MinFitVertices)
					continue;
				const size_t rank = std::min(list.size() - 1, static_cast<size_t>(TipPercentile * static_cast<float>(list.size())));
				std::ranges::nth_element(list, list.begin() + static_cast<std::ptrdiff_t>(rank));
				tipExtents[slot] = std::max(tipExtents[slot], list[rank]);
			}
		});

		fitMeshes = meshes;
		for (size_t side = 0; side < handRadii.size(); ++side) {
			HandRadii radii;
			for (size_t finger = 0; finger < FingerCount; ++finger) {
				for (size_t joint = 0; joint < JointsPerFinger; ++joint) {
					const float fitted = outermost[side * HandNodeCount + 1 + finger * JointsPerFinger + joint];
					if (fitted > 0.0f)
						radii.finger[finger * JointsPerFinger + joint] = std::clamp(fitted, MinFitRadius, MaxFitRadius);
				}
				radii.tipExtent[finger] = std::min(tipExtents[side * HandNodeCount + 1 + finger * JointsPerFinger + JointsPerFinger - 1], MaxTipExtent);
			}
			if (const float palm = outermost[side * HandNodeCount]; palm > 0.0f)
				radii.palm = std::clamp(palm, MinFitRadius, MaxPalmFitRadius);
			handRadii[side] = radii;
		}
		logger::info("[Fur Shells] Fitted hand capsule radii from {} meshes: fingers {:.2f}, palm {:.2f}", meshes, GetAverageFingerRadius(), GetAveragePalmRadius());
	}

	void Tracker::Push(Capsule a_capsule, size_t a_index)
	{
		const float3 center = (a_capsule.pointA + a_capsule.pointB) * 0.5f;
		const uint64_t key = MakeKey(a_capsule.owner, a_index) + (a_capsule.appendage ? AppendageKeyBase : 0);
		a_capsule.move = float3{};
		if (const auto previous = previousCenters.find(key); previous != previousCenters.end()) {
			float3 move = center - previous->second;
			const float length = move.Length();
			if (length > MaxMovePerFrame)
				move *= MaxMovePerFrame / length;
			a_capsule.move = move;
		}
		currentCenters[key] = center;
		capsules.push_back(a_capsule);
	}

	void Tracker::AddActorCapsules(RE::Actor* a_actor, const RE::NiPoint3& a_camera)
	{
		auto* root = a_actor->Get3D(false);
		if (!root)
			return;

		const float distance = a_camera.GetDistance(a_actor->GetPosition());
		std::vector<Util::ShapeCollisionCapsule> shapes;
		RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			Util::ShapeCollisionCapsule capsule;
			if (Util::GetShapeCollisionCapsule(a_object, capsule) && capsule.radius >= MinActorCapsuleRadius &&
				capsule.radius >= distance * MinRadiusPerDistance)
				shapes.push_back(capsule);
			return RE::BSVisit::BSVisitControl::kContinue;
		});

		std::ranges::sort(shapes, [](const auto& a, const auto& b) { return a.radius > b.radius; });
		if (shapes.size() > MaxCapsulesPerActor)
			shapes.resize(MaxCapsulesPerActor);

		for (size_t index = 0; index < shapes.size(); ++index) {
			Capsule capsule;
			capsule.pointA = ToFloat3(shapes[index].pointA);
			capsule.pointB = ToFloat3(shapes[index].pointB);
			capsule.radius = shapes[index].radius;
			capsule.owner = a_actor->GetFormID();
			Push(capsule, index);
		}
	}

	void Tracker::RefreshHandNodes(RE::Actor* a_player)
	{
		// VRIK animates the third-person skeleton from the controllers; without it only the first-person arms follow them.
		const bool vrik = GetModuleHandleA("vrik") != nullptr;
		RE::NiAVObject* root = a_player->Get3D(!vrik);
		if (root == handNodeRoot)
			return;

		handNodeRoot = root;
		handNodesFound = 0;
		RE::NiNode* rootNode = root ? root->AsNode() : nullptr;
		for (size_t side = 0; side < handNodes.size(); ++side) {
			for (size_t index = 0; index < HandNodeCount; ++index) {
				const std::string name = HandNodeName(side == 0, index);
				handNodes[side].nodes[index] = rootNode ? RE::NiPointer<RE::NiAVObject>(rootNode->GetObjectByName(RE::BSFixedString(name.c_str()))) : nullptr;
				if (handNodes[side].nodes[index])
					++handNodesFound;
				else if (root)
					logger::debug("[Fur Shells] Hand node not found: {}", name);
			}
		}
		if (root)
			logger::info("[Fur Shells] Found {} of {} hand and finger nodes in the {} skeleton", handNodesFound, handNodes.size() * HandNodeCount, vrik ? "third-person" : "first-person");
	}

	void Tracker::AddHandCapsules(RE::Actor* a_player, bool a_left)
	{
		const auto& nodes = handNodes[a_left ? 0 : 1].nodes;
		const auto position = [&](size_t a_index, float3& a_out) {
			const auto& node = nodes[a_index];
			if (!node || !node->parent)
				return false;
			a_out = ToFloat3(node->world.translate);
			return true;
		};

		const RE::FormID owner = a_player->GetFormID();
		size_t index = a_left ? 0 : RightHandKeyOffset;
		const auto add = [&](const float3& a_a, const float3& a_b, float a_radius) {
			Capsule capsule;
			capsule.pointA = a_a;
			capsule.pointB = a_b;
			capsule.radius = a_radius;
			capsule.owner = owner;
			capsule.appendage = true;
			capsule.side = a_left ? 0 : 1;
			Push(capsule, index++);
		};

		std::array<float3, FingerCount> knuckles{};
		std::array<bool, FingerCount> hasKnuckle{};
		for (size_t finger = 0; finger < FingerCount; ++finger) {
			float3 joints[JointsPerFinger];
			bool complete = true;
			for (size_t joint = 0; joint < JointsPerFinger; ++joint)
				complete = position(1 + finger * JointsPerFinger + joint, joints[joint]) && complete;
			if (!complete)
				continue;
			const auto& radii = handRadii[a_left ? 0 : 1].finger;
			add(joints[0], joints[1], radii[finger * JointsPerFinger]);
			add(joints[1], joints[2], radii[finger * JointsPerFinger + 1]);
			const float tipRadius = radii[finger * JointsPerFinger + 2];
			const float3 distalAxis = joints[2] - joints[1];
			const float tipExtent = handRadii[a_left ? 0 : 1].tipExtent[finger];
			const float3 tip = tipExtent > 0.0f ? joints[2] + distalAxis * (std::max(tipExtent - tipRadius, 0.0f) / std::max(distalAxis.Length(), 1e-4f)) : joints[2] + distalAxis * FingertipLengthScale;
			add(joints[2], tip, tipRadius);
			knuckles[finger] = joints[0];
			hasKnuckle[finger] = true;
		}

		float3 hand;
		if (hasKnuckle[IndexFinger] && hasKnuckle[PinkyFinger]) {
			add(knuckles[IndexFinger], knuckles[PinkyFinger], handRadii[a_left ? 0 : 1].finger[IndexFinger * JointsPerFinger]);
			if (position(0, hand))
				add(hand, (knuckles[IndexFinger] + knuckles[PinkyFinger]) * 0.5f, handRadii[a_left ? 0 : 1].palm);
		}
	}

	void Tracker::AddHeldCapsules(RE::Actor* a_player)
	{
		auto* higgs = Util::Higgs::GetInterface();
		for (auto& side : heldReports)
			side.fill({});
		if (!higgs)
			return;

		for (int side = 0; side < 2; ++side) {
			const bool left = side == 0;
			const auto add = [&](RE::NiObject* a_object, size_t a_slot) {
				auto& report = heldReports[side][a_slot];
				report.available = true;
				auto* body = a_object ? a_object->AsBhkRigidBody() : nullptr;
				report.body = body != nullptr;
				if (auto* rigid = body ? skyrim_cast<RE::hkpRigidBody*>(body->referencedObject.get()) : nullptr) {
					report.inWorld = rigid->world != nullptr;
					if (const auto* shape = rigid->collidable.GetShape()) {
						report.shapeType = static_cast<int>(shape->type);
						if (const auto* list = skyrim_cast<const RE::hkpListShape*>(shape))
							report.children = static_cast<int>(list->childInfo.size());
					}
				}
				std::array<Util::ShapeCollisionCapsule, MaxPartsPerHeld> parts;
				const size_t count = Util::GetRigidBodyCapsules(body, parts);
				report.capsule = count > 0;
				report.parts = static_cast<int>(count);
				for (size_t part = 0; part < count; ++part) {
					Capsule capsule;
					capsule.pointA = ToFloat3(parts[part].pointA);
					capsule.pointB = ToFloat3(parts[part].pointB);
					capsule.radius = parts[part].radius;
					capsule.owner = a_player->GetFormID();
					capsule.appendage = true;
					capsule.side = side;
					Push(capsule, HeldKeyOffset + (a_slot * MaxPartsPerHeld + part) * 2 + static_cast<size_t>(side));
				}
			};
			const auto* equipped = a_player->GetEquippedObject(left);
			const auto* armor = equipped ? equipped->As<RE::TESObjectARMO>() : nullptr;
			const bool holdsWeapon = equipped && (equipped->IsWeapon() || (armor && armor->IsShield()));
			const auto* state = a_player->AsActorState();
			auto& weaponReport = heldReports[side][0];
			weaponReport.drawn = state && state->IsWeaponDrawn();
			weaponReport.collisionDisabled = higgs->IsWeaponCollisionDisabled(left);
			if (holdsWeapon && weaponReport.drawn && !weaponReport.collisionDisabled)
				add(higgs->GetWeaponRigidBody(left), 0);
			if (higgs->IsHoldingObject(left))
				add(higgs->GetGrabbedRigidBody(left), 1);
			if (handNodesFound == 0)
				add(higgs->GetHandRigidBody(left), 2);
		}
	}

	void Tracker::Update(float a_maxDistance)
	{
		const uint32_t frame = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		if (!updateLatch.TryAdvance(frame))
			return;
		ZoneScopedN("BodyCapsules::Update");
		if (frame != lastUpdateFrame + 1)
			previousCenters.clear();
		lastUpdateFrame = frame;
		capsules.clear();
		currentCenters.clear();

		const RE::NiPoint3 camera = Util::GetEyePosition(0);
		const float maxSquared = a_maxDistance * a_maxDistance;

		struct Candidate
		{
			RE::Actor* actor;
			float squaredDistance;
		};
		std::vector<Candidate> candidates;
		Util::ForEachLoadedActor([&](RE::Actor* a_actor) {
			if (!a_actor->Is3DLoaded())
				return;
			const float squaredDistance = camera.GetSquaredDistance(a_actor->GetPosition());
			if (squaredDistance <= maxSquared)
				candidates.push_back({ a_actor, squaredDistance });
		});
		std::ranges::sort(candidates, [](const auto& a, const auto& b) { return a.squaredDistance < b.squaredDistance; });
		if (candidates.size() > MaxActors)
			candidates.resize(MaxActors);

		for (const auto& candidate : candidates)
			AddActorCapsules(candidate.actor, camera);

		if (auto* player = globals::game::player; globals::game::isVR && player && player->Is3DLoaded()) {
			RefreshHandNodes(player);
			if (handNodesFound > 0 && (fitSignature == 0 || ++updatesSinceFitCheck >= FitCheckInterval)) {
				updatesSinceFitCheck = 0;
				uint64_t signature = 1;
				Util::ForEachActorGeometry(player, [&](RE::BSGeometry* a_geometry) {
					if (const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get())
						signature ^= reinterpret_cast<uint64_t>(skin) * 0x9E3779B97F4A7C15ull;
				});
				if (signature != fitSignature) {
					fitSignature = signature;
					FitRadii(player);
				}
			}
			AddHandCapsules(player, true);
			AddHandCapsules(player, false);
			AddHeldCapsules(player);
		}

		previousCenters.swap(currentCenters);
	}

	size_t Tracker::Select(const RE::NiPoint3& a_center, float a_radius, float a_reach, RE::FormID a_wearer,
		std::span<Capsule> a_out, float& a_nearestGap) const
	{
		struct Scored
		{
			const Capsule* capsule = nullptr;
			float distance = 0.0f;
		};
		std::array<Scored, MaxSelectedCapsules> best{};
		const size_t limit = std::min(a_out.size(), best.size());
		size_t count = 0;

		const float3 center = ToFloat3(a_center);
		for (const Capsule& capsule : capsules) {
			if (a_wearer != 0 && capsule.owner == a_wearer && !capsule.appendage)
				continue;
			const float distance = (center - capsule.ClosestPointOnAxis(center)).Length() - capsule.radius;
			a_nearestGap = std::min(a_nearestGap, distance - a_radius);
			if (distance > a_radius + a_reach)
				continue;

			if (count < limit) {
				best[count++] = { &capsule, distance };
			} else {
				auto farthest = std::ranges::max_element(std::span(best.data(), limit), [](const auto& a, const auto& b) { return a.distance < b.distance; });
				if (distance < farthest->distance)
					*farthest = { &capsule, distance };
			}
		}

		for (size_t index = 0; index < count; ++index)
			a_out[index] = *best[index].capsule;
		return count;
	}

	void Tracker::Clear()
	{
		capsules.clear();
		previousCenters.clear();
		currentCenters.clear();
		updateLatch.Reset();
	}

	Tracker& GetTracker()
	{
		static Tracker tracker;
		return tracker;
	}
}
