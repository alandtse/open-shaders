#pragma once

#include "Utils/FrameLatch.h"

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

namespace Util::BodyCapsules
{
	/** @brief Most capsules Select can return for one query. */
	inline constexpr size_t MaxSelectedCapsules = 64;

	/** @brief Finger and palm capsule radii used until a fit to the player's hand meshes succeeds. */
	inline constexpr float DefaultFingerRadius = 1.2f;
	inline constexpr float DefaultPalmRadius = 1.6f;

	/** @brief A body part that can touch a mesh, in absolute world space. */
	struct Capsule
	{
		float3 pointA;
		float3 pointB;
		float radius = 0.0f;
		float3 move;             ///< How far the capsule centre moved since the previous Update, in world units per frame and capped at 8 units.
		RE::FormID owner = 0;    ///< The actor whose body the capsule belongs to.
		bool appendage = false;  ///< A hand, a finger or an item held in a hand, which also touches the owner's own gear.
		int side = -1;           ///< For an appendage, 0 for the left hand and 1 for the right, which for a held item is the hand holding it.
		float strength = 1.0f;   ///< How strongly the capsule acts, 1 for a live body part and lower for a contact that is fading.
	};

	/**
	 * @brief Collects the collision capsules of nearby actors and the player's VR hands and fingers.
	 * @details Actor capsules come from the same Havok collision objects Grass Collision reads. Finger capsules come from
	 *          the player skeleton, so they work without HIGGS. When HIGGS is installed in VR, the weapon equipped in each hand and the object it holds
	 *          are added too, as appendages of that hand, and its hand body stands in for the fingers until the skeleton nodes are found. Update rebuilds the list at most once per rendered frame,
	 *          so every consumer can call it; read the list on the thread that calls Update, since nothing locks it.
	 */
	class Tracker
	{
	public:
		/**
		 * @brief Rebuilds the capsule list for the actors within a_maxDistance of the camera and the player's VR hands.
		 * @details Does nothing when the list was already rebuilt this frame, so the first caller of a frame sets the range.
		 */
		void Update(float a_maxDistance);
		/** @brief Drops every capsule and the movement history; the next Update rebuilds them. */
		void Clear();

		/**
		 * @brief Picks the capsules nearest a mesh, skipping its wearer's own body capsules but keeping the wearer's hands and fingers.
		 * @param a_center World bound centre of the mesh.
		 * @param a_radius World bound radius of the mesh.
		 * @param a_reach How far past the mesh a capsule still matters.
		 * @param a_wearer Form ID of the actor wearing the mesh, or 0 when unknown.
		 * @param a_out Receives the chosen capsules, at most MaxSelectedCapsules of them.
		 * @param a_nearestGap Receives the smallest distance from the mesh bound to any eligible capsule, which is negative when one touches.
		 * @return How many entries of a_out were filled.
		 */
		[[nodiscard]] size_t Select(const RE::NiPoint3& a_center, float a_radius, float a_reach, RE::FormID a_wearer,
			std::span<Capsule> a_out, float& a_nearestGap) const;

		[[nodiscard]] size_t GetCount() const { return capsules.size(); }

		/** @brief How many of the player's hand and finger skeleton nodes were found, out of 2 * HandNodeCount. */
		[[nodiscard]] size_t GetHandNodeCount() const { return handNodesFound; }

		/** @brief World position of a hand node and of its middle fingertip joint, for checking that injected controller poses move the hands; false while the nodes are not found. */
		[[nodiscard]] bool GetHandPositions(bool a_left, float3& a_hand, float3& a_fingerJoint) const;

		/** @brief One hand's skeleton and fit, for diagnostics: per finger joint a world position (valid when found) and radius, per finger the fitted tip extent. */
		struct HandReport
		{
			static constexpr size_t Fingers = 5;
			static constexpr size_t Joints = 3;
			std::array<float3, Fingers * Joints> position;
			std::array<bool, Fingers * Joints> found{};
			std::array<float, Fingers * Joints> radius{};
			std::array<float, Fingers> tipExtent{};
			float palmRadius = 0.0f;
		};
		[[nodiscard]] HandReport GetHandReport(bool a_left) const;

		/** @brief What HIGGS gave for one hand's weapon (slot 0), grabbed object (slot 1) or hand body (slot 2) last frame, for diagnostics. */
		struct HeldReport
		{
			bool available = false;  ///< HIGGS answered the interface fetch.
			bool body = false;       ///< HIGGS returned a rigid body.
			bool inWorld = false;    ///< The body is in a physics world.
			int shapeType = -1;      ///< hkpShapeType of the body's shape, -1 when there is none.
			bool capsule = false;    ///< The body was reduced to capsules and added.
			int children = 0;        ///< Child shapes of a list shape, 0 for any other shape.
			int parts = 0;           ///< Capsules the body became.
		};
		[[nodiscard]] HeldReport GetHeldReport(bool a_left, size_t a_slot) const { return heldReports[a_left ? 0 : 1][a_slot]; }

		/** @brief How many skinned meshes the capsule radii were fitted from, 0 while the defaults are in use. */
		[[nodiscard]] size_t GetFitMeshCount() const { return fitMeshes; }

		/** @brief Mean fitted finger radius over both hands. */
		[[nodiscard]] float GetAverageFingerRadius() const;

		/** @brief Mean fitted palm radius over both hands. */
		[[nodiscard]] float GetAveragePalmRadius() const;

	private:
		static constexpr size_t FingerCount = 5;
		static constexpr size_t JointsPerFinger = 3;
		static constexpr size_t HandNodeCount = 1 + FingerCount * JointsPerFinger;

		struct HandNodes
		{
			std::array<RE::NiPointer<RE::NiAVObject>, HandNodeCount> nodes;
		};

		void AddActorCapsules(RE::Actor* a_actor, const RE::NiPoint3& a_camera);
		void AddHandCapsules(RE::Actor* a_player, bool a_left);
		void Push(Capsule a_capsule, size_t a_index);
		void RefreshHandNodes(RE::Actor* a_player);
		void AddHeldCapsules(RE::Actor* a_player);

		/**
		 * @brief Fits each finger and palm capsule radius to the skinned vertices of the player's hand and armor meshes.
		 * @details Reads the CPU copies of the vertices and skin weights. Each vertex goes to its dominant bone, and a bone's radius
		 *          is a high percentile of its vertices' distances to the bone segment, taking the outermost mesh over the hand.
		 */
		void FitRadii(RE::Actor* a_player);

		std::vector<Capsule> capsules;
		std::unordered_map<uint64_t, float3> previousCenters;
		std::unordered_map<uint64_t, float3> currentCenters;
		std::array<HandNodes, 2> handNodes;
		const RE::NiAVObject* handNodeRoot = nullptr;
		size_t handNodesFound = 0;

		struct HandRadii
		{
			std::array<float, FingerCount * JointsPerFinger> finger;
			std::array<float, FingerCount> tipExtent{};  ///< Mesh distance from each fingertip joint to the finger's end, 0 until fitted.
			float palm = DefaultPalmRadius;

			HandRadii() { finger.fill(DefaultFingerRadius); }
		};
		std::array<HandRadii, 2> handRadii;
		size_t fitMeshes = 0;
		uint64_t fitSignature = 0;
		uint32_t updatesSinceFitCheck = 0;
		std::array<std::array<HeldReport, 3>, 2> heldReports{};
		FrameLatch updateLatch;
		uint32_t lastUpdateFrame = 0;
	};

	/** @brief The shared tracker, which every consumer of body capsules reads. */
	[[nodiscard]] Tracker& GetTracker();
}
