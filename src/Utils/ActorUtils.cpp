#include "ActorUtils.h"

#include "Globals.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace
{
	RE::NiPoint3 TransformHavokPoint(const RE::hkTransform& transform, const RE::hkVector4& point)
	{
		alignas(16) float localPoint[4];
		_mm_store_ps(localPoint, point.quad);

		__m128 worldPoint = transform.translation.quad;
		worldPoint = _mm_add_ps(worldPoint, _mm_mul_ps(transform.rotation.col0.quad, _mm_set1_ps(localPoint[0])));
		worldPoint = _mm_add_ps(worldPoint, _mm_mul_ps(transform.rotation.col1.quad, _mm_set1_ps(localPoint[1])));
		worldPoint = _mm_add_ps(worldPoint, _mm_mul_ps(transform.rotation.col2.quad, _mm_set1_ps(localPoint[2])));

		alignas(16) float transformedPoint[4];
		_mm_store_ps(transformedPoint, worldPoint);
		return RE::NiPoint3(transformedPoint[0], transformedPoint[1], transformedPoint[2]) *
		       RE::bhkWorld::GetWorldScaleInverse();
	}

	bool IsCapsuleFinite(const Util::ShapeCollisionCapsule& capsule)
	{
		return std::isfinite(capsule.pointA.x) && std::isfinite(capsule.pointA.y) && std::isfinite(capsule.pointA.z) &&
		       std::isfinite(capsule.pointB.x) && std::isfinite(capsule.pointB.y) && std::isfinite(capsule.pointB.z) &&
		       std::isfinite(capsule.radius) && capsule.radius > 0.0f;
	}

	/** @brief Ray hit filter that drops the player's own collision so a first-person ray is not self-occluded. */
	class OccluderRayCollector : public RE::hkpClosestRayHitCollector
	{
	public:
		void AddRayHit(const RE::hkpCdBody& a_body, const RE::hkpShapeRayCastCollectorOutput& a_hitInfo) override
		{
			const RE::hkpCdBody* body = std::addressof(a_body);
			for (const auto* parent = body->parent; parent; parent = parent->parent)
				body = parent;
			if (!body)
				return;
			if (RE::TESHavokUtilities::FindCollidableRef(*static_cast<const RE::hkpCollidable*>(body)) == globals::game::player)
				return;
			RE::hkpClosestRayHitCollector::AddRayHit(a_body, a_hitInfo);
		}
	};

	/** @brief True when the authored local box is usable: finite and open on every axis. */
	bool HasAuthoredBox(const RE::NiPoint3& a_min, const RE::NiPoint3& a_max)
	{
		const auto finite = [](const RE::NiPoint3& a_point) {
			return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
		};
		return finite(a_min) && finite(a_max) &&
		       a_max.x > a_min.x && a_max.y > a_min.y && a_max.z > a_min.z;
	}

	/** @brief Most skeleton nodes one bounds query visits, so a malformed hierarchy cannot stall the frame. */
	constexpr size_t kMaxSkeletonNodes = 512;

	/**
	 * @brief Grows a_box over the skeleton's joint positions that lie inside a_reach.
	 *        Camera nodes ride well outside the body, and any node further than the body's own reach
	 *        from its rest-pose box is an attachment rather than a limb, so both are ignored.
	 */
	void IncludeSkeletonJoints(RE::NiAVObject* a_root, const Util::PointBox& a_reach, Util::PointBox& a_box)
	{
		size_t visited = 0;
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			auto* node = a_object ? a_object->AsNode() : nullptr;
			if (!node)
				return RE::BSVisit::BSVisitControl::kContinue;
			if (++visited > kMaxSkeletonNodes)
				return RE::BSVisit::BSVisitControl::kStop;
			const char* name = node->name.c_str();
			if (name && std::string_view(name).starts_with("Camera"))
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto& translate = node->world.translate;
			const float3 joint{ translate.x, translate.y, translate.z };
			if (joint.x >= a_reach.min.x && joint.x <= a_reach.max.x && joint.y >= a_reach.min.y && joint.y <= a_reach.max.y &&
				joint.z >= a_reach.min.z && joint.z <= a_reach.max.z)
				a_box.Include(joint);
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}
}

namespace Util
{
	// Actor query pattern adapted from po3 under MIT.
	// https://github.com/powerof3/PapyrusExtenderSSE/blob/7a73b47bc87331bec4e16f5f42f2dbc98b66c3a7/include/Papyrus/Functions/Faction.h#L24C7-L46
	void ForEachLoadedActor(const std::function<void(RE::Actor*)>& a_callback)
	{
		if (!a_callback)
			return;

		auto* player = globals::game::player;
		if (player)
			a_callback(player);

		if (const auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (const auto& actorHandle : processLists->highActorHandles) {
				if (const auto actor = actorHandle.get(); actor && actor.get() != player)
					a_callback(actor.get());
			}
		}
	}

	BoundPoints GetActorBoundPoints(RE::Actor& a_actor, bool a_includeSkeleton, float a_jointMargin)
	{
		BoundPoints restPose;
		auto* root = a_actor.Get3D(false);
		if (!root)
			return restPose;
		PointBox reach;
		const auto authoredMin = a_actor.GetBoundMin();
		const auto authoredMax = a_actor.GetBoundMax();
		if (HasAuthoredBox(authoredMin, authoredMax)) {
			for (const float x : { authoredMin.x, authoredMax.x })
				for (const float y : { authoredMin.y, authoredMax.y })
					for (const float z : { authoredMin.z, authoredMax.z }) {
						const auto world = root->world * RE::NiPoint3{ x, y, z };
						const float3 corner{ world.x, world.y, world.z };
						restPose.Add(corner);
						reach.Include(corner);
					}
		} else if (root->worldBound.radius > 0.0f) {
			const auto& bound = root->worldBound;
			const float3 center{ bound.center.x, bound.center.y, bound.center.z };
			const float3 extent{ bound.radius, bound.radius, bound.radius };
			restPose.AddBox(center - extent, center + extent);
			reach.Include(center - extent);
			reach.Include(center + extent);
		} else {
			return restPose;
		}
		if (!a_includeSkeleton)
			return restPose;
		// A corpse or knocked-down actor keeps an upright root while its bones lie flat, so the
		// rest-pose box would span a volume the body is not in.
		const bool poseFollowsRoot = !a_actor.IsDead() && !a_actor.IsInRagdollState();
		BoundPoints result;
		if (poseFollowsRoot)
			result = restPose;
		const float3 span = reach.max - reach.min;
		reach.Expand(std::max({ span.x, span.y, span.z }));
		PointBox joints;
		IncludeSkeletonJoints(root, reach, joints);
		if (joints.Valid()) {
			joints.Expand(a_jointMargin);
			result.AddBox(joints.min, joints.max);
		} else if (!poseFollowsRoot) {
			result = restPose;
		}
		return result;
	}

	bool IsActorVisibleFromEye(RE::Actor& a_actor, const RE::NiPoint3& a_eyePosition)
	{
		auto* cell = a_actor.GetParentCell();
		auto* world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world)
			return false;
		const float scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pickData{};
		pickData.rayInput.from = a_eyePosition * scale;
		pickData.rayInput.enableShapeCollectionFilter = false;
		pickData.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
		OccluderRayCollector collector;
		pickData.closestRayHitCollector = &collector;
		for (const auto location : { RE::ACTOR_LOS_LOCATION::kEye, RE::ACTOR_LOS_LOCATION::kHead,
				 RE::ACTOR_LOS_LOCATION::kTorso, RE::ACTOR_LOS_LOCATION::kFeet }) {
			collector.Reset();
			pickData.rayOutput.Reset();
			pickData.rayInput.to = a_actor.CalculateLOSLocation(location) * scale;
			if (!world->PickObject(pickData))
				return true;
			const auto* collidable = pickData.rayOutput.rootCollidable;
			if (!collidable || RE::TESHavokUtilities::FindCollidableRef(*collidable) == std::addressof(a_actor))
				return true;
		}
		return false;
	}

	void ForEachGeometry(RE::NiAVObject* a_root, const std::function<void(RE::BSGeometry*)>& a_callback)
	{
		if (!a_root || !a_callback)
			return;

		RE::BSVisit::TraverseScenegraphGeometries(a_root, [&a_callback](RE::BSGeometry* a_geometry) {
			a_callback(a_geometry);
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	void ForEachActorGeometry(RE::Actor* a_actor, const std::function<void(RE::BSGeometry*)>& a_callback)
	{
		if (!a_actor || !a_actor->Is3DLoaded() || !a_callback)
			return;

		auto* thirdPersonRoot = a_actor->Get3D(false);
		auto* firstPersonRoot = a_actor->Get3D(true);
		ForEachGeometry(thirdPersonRoot, a_callback);
		if (firstPersonRoot != thirdPersonRoot)
			ForEachGeometry(firstPersonRoot, a_callback);
	}

	void ForEachHeldWeaponGeometry(RE::Actor* a_actor, const std::function<void(RE::BSGeometry*)>& a_callback)
	{
		if (!a_actor || !a_callback)
			return;

		const std::uint32_t bipedCount = a_actor == globals::game::player ? 2u : 1u;
		for (std::uint32_t bipedIndex = 0; bipedIndex < bipedCount; ++bipedIndex) {
			const auto& biped = a_actor->GetBiped(bipedIndex != 0);
			if (!biped)
				continue;

			for (std::uint32_t slot = RE::BIPED_OBJECTS::kOneHandSword;
				slot <= RE::BIPED_OBJECTS::kCrossbow; ++slot) {
				const auto& object = biped->objects[slot];
				if (object.item && object.item->IsWeapon() && object.partClone)
					ForEachGeometry(object.partClone.get(), a_callback);
			}
		}
	}

	bool GetShapeCollisionCapsule(RE::bhkNiCollisionObject* collisionObj, ShapeCollisionCapsule& capsule)
	{
		if (!collisionObj)
			return false;

		RE::bhkRigidBody* bhkRigid = collisionObj->body.get() ? collisionObj->body.get()->AsBhkRigidBody() : nullptr;
		RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
		const auto* shape = hkpRigid ? hkpRigid->collidable.GetShape() : nullptr;
		if (bhkRigid && hkpRigid && shape && !skyrim_cast<const RE::hkpListShape*>(shape)) {  // Ignore hkpListShape, unsupported
			if (shape->type == RE::hkpShapeType::kCapsule) {
				const auto* capsuleShape = static_cast<const RE::hkpCapsuleShape*>(shape);
				RE::hkTransform transform;
				bhkRigid->GetTransform(transform);

				capsule.pointA = TransformHavokPoint(transform, capsuleShape->vertexA);
				capsule.pointB = TransformHavokPoint(transform, capsuleShape->vertexB);
				capsule.radius = capsuleShape->radius * RE::bhkWorld::GetWorldScaleInverse();
				return IsCapsuleFinite(capsule);
			}

			RE::hkVector4 massCenter;
			bhkRigid->GetCenterOfMassWorld(massCenter);
			float massTrans[4];
			// Use unaligned store to avoid UB from potential stack misalignment
			_mm_storeu_ps(massTrans, massCenter.quad);
			capsule.pointA = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();
			capsule.pointB = capsule.pointA;
			return Util::ExtractShapeBound(shape, capsule.radius) && IsCapsuleFinite(capsule);
		}
		return false;
	}

	bool GetShapeBound(RE::bhkNiCollisionObject* collisionObj, RE::NiPoint3& centerPos, float& radius)
	{
		ShapeCollisionCapsule capsule;
		if (!GetShapeCollisionCapsule(collisionObj, capsule))
			return false;

		centerPos = (capsule.pointA + capsule.pointB) * 0.5f;
		radius = capsule.radius + capsule.pointA.GetDistance(capsule.pointB) * 0.5f;
		return true;
	}

	bool ExtractShapeBound(const RE::hkpShape* shape, float& radius)
	{
		using ShapeType = RE::hkpShapeType;
		if (!shape)
			return false;

		// Helpers to avoid repeating projection math and ensure offset-invariant half-extents
		auto project = [shape](float x, float y, float z) {
			return shape->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
		};
		auto symmetricHalfExtents = [&project](float& hx, float& hy, float& hz) {
			float x_pos = project(1.0f, 0.0f, 0.0f);
			float x_neg = project(-1.0f, 0.0f, 0.0f);
			float y_pos = project(0.0f, 1.0f, 0.0f);
			float y_neg = project(0.0f, -1.0f, 0.0f);
			float z_pos = project(0.0f, 0.0f, 1.0f);
			float z_neg = project(0.0f, 0.0f, -1.0f);
			hx = 0.5f * (x_pos - x_neg);
			hy = 0.5f * (y_pos - y_neg);
			hz = 0.5f * (z_pos - z_neg);
		};
		auto halfDiagonal = [](float hx, float hy, float hz) {
			return sqrtf(hx * hx + hy * hy + hz * hz);
		};
		if (shape->type == ShapeType::kCapsule) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			// For capsules, use the maximum half-extent (typically hz for vertical orientation)
			// as the farthest point lies along the capsule's main axis, not at the diagonal
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else if (shape->type == ShapeType::kSphere) {
			// For spheres, any axis should yield the same half-extent; use symmetric X
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = hx;
			return true;
		} else if (shape->type == ShapeType::kBox) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = halfDiagonal(hx, hy, hz);
			return true;
		} else if (shape->type == ShapeType::kCylinder) {
			// Use symmetric half-extents; cylinder radius is max of X/Y half-extents
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			float hr = std::max(hx, hy);
			radius = sqrtf(hr * hr + hz * hz);
			return true;
		} else if (shape->type == ShapeType::kConvexVertices || shape->type == ShapeType::kTriangle) {
			// Offset-invariant estimate: take symmetric half-extents per axis and use the max
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else {
			// Fallback: mirror the convex/triangle approach for consistency
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		}
	}

	bool IsDragon(const RE::Actor& a_actor, const RE::BGSKeyword* a_dragonKeyword)
	{
		const auto* race = a_actor.GetRace();
		if (!race)
			return false;
		if (a_dragonKeyword)
			return race->HasKeyword(a_dragonKeyword);
		if (race->HasKeywordString("ActorTypeDragon"))
			return true;

		constexpr std::string_view dragonGraph = "dragonbehavior.hkx";
		for (const auto& behaviorGraph : race->behaviorGraphs) {
			const char* model = behaviorGraph.GetModel();
			if (!model)
				continue;
			const std::string_view path(model);
			const auto match = std::search(path.begin(), path.end(), dragonGraph.begin(), dragonGraph.end(),
				[](unsigned char a_left, unsigned char a_right) {
					return std::tolower(a_left) == std::tolower(a_right);
				});
			if (match != path.end())
				return true;
		}
		return false;
	}

	float3 GetVisualOrigin(RE::Actor& a_actor) noexcept
	{
		if (auto* root = a_actor.Get3D(false)) {
			const auto& origin = root->world.translate;
			return { origin.x, origin.y, origin.z };
		}

		auto origin = a_actor.GetPosition();
		origin.z += (a_actor.GetBoundMax().z - a_actor.GetBoundMin().z) * 0.5f;
		return { origin.x, origin.y, origin.z };
	}

	float3 GetMagicOrigin(RE::Actor& a_actor) noexcept
	{
		if (auto* caster = a_actor.GetMagicCaster(RE::MagicSystem::CastingSource::kOther)) {
			if (auto* magicNode = caster->GetMagicNode()) {
				const auto& origin = magicNode->world.translate;
				return { origin.x, origin.y, origin.z };
			}
		}
		auto origin = a_actor.GetPosition();
		origin.z += (a_actor.GetBoundMax().z - a_actor.GetBoundMin().z) * 0.7f;
		return { origin.x, origin.y, origin.z };
	}

	float3 GetAimDirection(RE::Actor& a_actor) noexcept
	{
		float aimAngle = a_actor.GetAimAngle();
		float aimHeading = a_actor.GetAimHeading();
		if (!std::isfinite(aimAngle))
			aimAngle = a_actor.GetAngleX();
		if (!std::isfinite(aimHeading))
			aimHeading = a_actor.GetAngleZ();
		const float horizontalScale = std::cos(aimAngle);
		return {
			horizontalScale * std::sin(aimHeading),
			horizontalScale * std::cos(aimHeading),
			-std::sin(aimAngle)
		};
	}
}
